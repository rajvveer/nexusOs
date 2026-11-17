/* Host test of the native NVMe driver (arch/x86_64/nvme.c) against a simulated NVMe 1.4 controller: register
   state machine (CC.EN/CSTS.RDY, CAP.TO, doorbell stride, CC.SHN/CSTS.SHST), admin queue (Identify controller /
   namespace / active list, Set Features queues / APST / power state, Create I/O CQ/SQ), I/O queue with a spec
   PRP walker (PRP1 offset, PRP2 data or list, chained list pages - every data pointer after PRP1 must be page
   aligned), MSI-X style interrupts (block_wait delivers them) and polling, and fault injection: lost commands
   (timeout, controller reset, retry), controller fatal status, error completions, a controller that falls off the
   bus (all-ones registers), one that never becomes ready, metadata namespace formats, MDTS splitting, bounce
   buffers, shutdown notification with and without an answer. */
#define NVME_TEST_MMIO 1
#define NEXUS_HOST_TEST 1
#include "../arch/x86_64/native.h"
#include "../kernel/string.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>

#define DISK_BLOCKS 16384u
static struct sim {
    uint64_t cap; uint32_t cc, csts, aqa, intms, ready_delay, never_ready, shst_delay; uint64_t asq, acq; bool gone, no_shst;
    struct { uint64_t base; uint32_t size, head, tail, cqid; bool live; } sq[2];
    struct { uint64_t base; uint32_t size, tail, head; uint8_t phase; bool live, ien; } cq[2];
    uint8_t* disk; uint32_t block_size, lbaf_metadata; uint8_t mdts, apsta, vwc; uint32_t rtd3e;
    /* fault injection (next I/O command) */
    bool drop_next, cfs_next; uint16_t status_next;
    /* observations */
    unsigned enables, disables, apst_sets, power_sets, apst_table_nonzero, io_commands, flushes, max_prp_lists, shn_writes;
    uint32_t apst_cdw11, power_cdw11, queues_cdw11; bool irq_pending;
} s;
static uint64_t clock_ns;
uint64_t native_clock_ns(void) { return clock_ns += 20000; }                          /* 20 us per look at the clock */

/* ---- controller ---- */
/* Completions are posted when the host next waits (block_wait): device latency, so the interrupt path runs. */
static struct { unsigned cqid, sqid; uint16_t cid, status; uint32_t result; } inflight[64]; static unsigned inflight_count;
static void post(unsigned cqid, uint16_t cid, unsigned sqid, uint16_t status, uint32_t result) {
    assert(inflight_count < 64);
    inflight[inflight_count].cqid = cqid; inflight[inflight_count].sqid = sqid; inflight[inflight_count].cid = cid;
    inflight[inflight_count].status = status; inflight[inflight_count++].result = result;
}
static void complete(unsigned cqid, uint16_t cid, unsigned sqid, uint16_t status, uint32_t result) {
    if (!s.cq[cqid].live) return;                                                      /* queue deleted by a reset */
    uint32_t* e = (uint32_t*)(uintptr_t)(s.cq[cqid].base + (uint64_t)s.cq[cqid].tail * 16);
    e[0] = result; e[1] = 0; e[2] = s.sq[sqid].head | sqid << 16;
    e[3] = cid | (uint32_t)s.cq[cqid].phase << 16 | (uint32_t)status << 17;
    if (++s.cq[cqid].tail == s.cq[cqid].size) { s.cq[cqid].tail = 0; s.cq[cqid].phase ^= 1; }
    assert(s.cq[cqid].tail != s.cq[cqid].head);                                       /* host must keep up */
    if (s.cq[cqid].ien) s.irq_pending = true;
}
static uint64_t prp_at(uint64_t address) { assert(!(address & 7)); return *(uint64_t*)(uintptr_t)address; }
/* Gather the data pages of a transfer per the PRP rules; returns the number of list pages used. */
static unsigned prp_walk(const uint32_t* c, uint32_t bytes, uint8_t* host, bool to_device, uint64_t lba) {
    uint64_t prp1 = c[6] | (uint64_t)c[7] << 32, prp2 = c[8] | (uint64_t)c[9] << 32;
    assert(!(prp1 & 3));
    uint32_t first = 4096 - (uint32_t)(prp1 & 4095); if (first > bytes) first = bytes;
    uint8_t* disk = s.disk + lba * s.block_size;
    #define MOVE(address, n, at) do { if (to_device) memcpy(disk + (at), (void*)(uintptr_t)(address), n); else memcpy((void*)(uintptr_t)(address), disk + (at), n); (void)host; } while (0)
    MOVE(prp1, first, 0);
    uint32_t done = first; unsigned lists = 0;
    if (done == bytes) { assert(!prp2 || bytes <= first); return 0; }
    if (bytes - done <= 4096) { assert(!(prp2 & 4095)); MOVE(prp2, bytes - done, done); return 0; }
    uint64_t list = prp2; assert(!(list & 7)); lists = 1;
    for (unsigned slot = (unsigned)((list & 4095) / 8); done < bytes; slot++) {
        uint64_t entry = prp_at((list & ~4095ULL) + slot * 8);
        if (slot == 511 && bytes - done > 4096) { list = entry; assert(!(list & 4095)); slot = (unsigned)-1; lists++; continue; }
        assert(entry && !(entry & 4095));                                               /* data pages are page aligned */
        uint32_t n = bytes - done < 4096 ? bytes - done : 4096; MOVE(entry, n, done); done += n;
    }
    #undef MOVE
    return lists;
}
static void identify(uint32_t cns, uint32_t nsid, uint8_t* out) {
    memset(out, 0, 4096);
    if (cns == 1) {
        memcpy(out + 24, "Simulated NVMe SSD                      ", 40); out[77] = s.mdts; out[525] = s.vwc; out[265] = s.apsta;
        memcpy(out + 88, &s.rtd3e, 4); uint32_t nn = 2; memcpy(out + 516, &nn, 4);
    } else if (cns == 0) {
        if (nsid != 1) return;                                                          /* inactive: zero NSZE */
        uint64_t blocks = DISK_BLOCKS * 512ULL / s.block_size; memcpy(out, &blocks, 8); memcpy(out + 8, &blocks, 8);
        out[26] = 0; uint32_t lbaf = s.lbaf_metadata | (uint32_t)(s.block_size == 4096 ? 12 : 9) << 16; memcpy(out + 128, &lbaf, 4);
    } else if (cns == 2) { uint32_t one = 1; memcpy(out, &one, 4); }
}
static void run_queue(unsigned sqid) {
    while (s.sq[sqid].live && s.sq[sqid].head != s.sq[sqid].tail) {
        uint32_t* c = (uint32_t*)(uintptr_t)(s.sq[sqid].base + (uint64_t)s.sq[sqid].head * 64);
        if (++s.sq[sqid].head == s.sq[sqid].size) s.sq[sqid].head = 0;
        uint8_t opcode = (uint8_t)c[0]; uint16_t cid = (uint16_t)(c[0] >> 16), status = 0; uint32_t result = 0;
        unsigned cqid = sqid ? s.sq[sqid].cqid : 0;
        if (sqid) {
            s.io_commands++;
            if (s.drop_next) { s.drop_next = false; continue; }                       /* lost: no completion */
            if (s.cfs_next) { s.cfs_next = false; s.csts |= 2; continue; }            /* controller fatal status */
            if (s.status_next) { status = s.status_next; s.status_next = 0; post(cqid, cid, sqid, status, 0); continue; }
            assert(c[1] == 1);
            if (opcode == 0x00) s.flushes++;
            else {
                assert(opcode == 0x01 || opcode == 0x02);
                uint64_t lba = c[10] | (uint64_t)c[11] << 32; uint32_t blocks = (c[12] & 0xFFFF) + 1, bytes = blocks * s.block_size;
                assert(lba + blocks <= DISK_BLOCKS * 512ULL / s.block_size);
                assert(!s.mdts || bytes <= (4096u << s.mdts));
                unsigned lists = prp_walk(c, bytes, NULL, opcode == 0x01, lba);
                if (lists > s.max_prp_lists) s.max_prp_lists = lists;
            }
            post(cqid, cid, sqid, 0, 0); continue;
        }
        switch (opcode) {
        case 0x06: identify(c[10] & 0xFF, c[1], (uint8_t*)(uintptr_t)(c[6] | (uint64_t)c[7] << 32)); break;
        case 0x09: {
            uint32_t fid = c[10] & 0xFF;
            if (fid == 0x07) { s.queues_cdw11 = c[11]; result = 0; }
            else if (fid == 0x0C) {
                s.apst_sets++; s.apst_cdw11 = c[11];
                const uint8_t* table = (const uint8_t*)(uintptr_t)(c[6] | (uint64_t)c[7] << 32); assert(table);
                for (unsigned i = 0; i < 256; i++) if (table[i]) { s.apst_table_nonzero++; break; }
            } else if (fid == 0x02) { s.power_sets++; s.power_cdw11 = c[11]; }
            else status = 0x0002;                                                       /* invalid field */
            break;
        }
        case 0x05: {                                                                    /* Create I/O CQ */
            unsigned qid = c[10] & 0xFFFF; assert(qid == 1 && (c[11] & 1));
            s.cq[1].base = c[6] | (uint64_t)c[7] << 32; s.cq[1].size = (c[10] >> 16) + 1; s.cq[1].tail = s.cq[1].head = 0;
            s.cq[1].phase = 1; s.cq[1].live = true; s.cq[1].ien = (c[11] & 2) != 0; assert(!(s.cq[1].base & 4095) && (c[11] >> 16) == 0);
            break;
        }
        case 0x01: {                                                                    /* Create I/O SQ */
            unsigned qid = c[10] & 0xFFFF; assert(qid == 1 && s.cq[1].live && (c[11] >> 16) == 1 && (c[11] & 1));
            s.sq[1].base = c[6] | (uint64_t)c[7] << 32; s.sq[1].size = (c[10] >> 16) + 1; s.sq[1].head = s.sq[1].tail = 0;
            s.sq[1].cqid = 1; s.sq[1].live = true; assert(!(s.sq[1].base & 4095));
            break;
        }
        default: status = 0x0001;                                                       /* invalid opcode */
        }
        post(cqid, cid, sqid, status, result);
    }
}
static void deliver(void) {
    for (unsigned i = 0; i < inflight_count; i++) complete(inflight[i].cqid, inflight[i].cid, inflight[i].sqid, inflight[i].status, inflight[i].result);
    inflight_count = 0;
}
static void controller_disable(void) {
    inflight_count = 0; s.csts &= ~3u; memset(s.sq, 0, sizeof(s.sq)); memset(s.cq, 0, sizeof(s.cq)); s.disables++;
}
static uint32_t rd(volatile uint8_t* base, unsigned offset) {
    (void)base;
    if (s.gone) return ~0u;
    switch (offset) {
    case 0x00: return (uint32_t)s.cap;
    case 0x04: return (uint32_t)(s.cap >> 32);
    case 0x08: return 0x00010400;
    case 0x0C: return s.intms;
    case 0x14: return s.cc;
    case 0x1C:
        if ((s.cc & 1) && !(s.csts & 1) && !s.never_ready && s.ready_delay-- == 0) s.csts |= 1;
        if ((s.cc >> 14) & 3) { if (!s.no_shst && s.shst_delay-- == 0) s.csts = (s.csts & ~0xCu) | 0x8u; }
        return s.csts;
    default: return 0;
    }
}
static void wr(volatile uint8_t* base, unsigned offset, uint32_t value) {
    (void)base;
    if (s.gone) return;
    uint32_t stride = 4u << ((s.cap >> 32) & 15);
    if (offset >= 0x1000) {
        unsigned index = (offset - 0x1000) / stride, qid = index / 2; assert((offset - 0x1000) % stride == 0 && qid < 2);
        if (index & 1) { assert(s.cq[qid].live && value < s.cq[qid].size); s.cq[qid].head = value; return; }
        assert(s.sq[qid].live && value < s.sq[qid].size); s.sq[qid].tail = value; run_queue(qid); return;
    }
    switch (offset) {
    case 0x0C: s.intms |= value; break;
    case 0x10: s.intms &= ~value; break;
    case 0x14: {
        uint32_t old = s.cc; s.cc = value;
        if ((value >> 14) & 3) s.shn_writes++;
        if ((old & 1) && !(value & 1)) controller_disable();
        if (!(old & 1) && (value & 1)) {
            assert(((value >> 16) & 15) == 6 && ((value >> 20) & 15) == 4 && ((value >> 7) & 15) == 0);   /* SQES 64, CQES 16, 4 KiB pages */
            assert(s.aqa && s.asq && s.acq && !(s.asq & 4095) && !(s.acq & 4095));
            s.sq[0].base = s.asq; s.sq[0].size = (s.aqa & 0xFFF) + 1; s.sq[0].head = s.sq[0].tail = s.sq[0].cqid = 0; s.sq[0].live = true;
            s.cq[0].base = s.acq; s.cq[0].size = ((s.aqa >> 16) & 0xFFF) + 1; s.cq[0].tail = s.cq[0].head = 0; s.cq[0].phase = 1;
            s.cq[0].live = true; s.cq[0].ien = true; s.enables++; s.ready_delay = 3;
        }
        break;
    }
    case 0x24: s.aqa = value; break;
    case 0x28: s.asq = (s.asq & ~0xFFFFFFFFULL) | value; break;
    case 0x2C: s.asq = (s.asq & 0xFFFFFFFFULL) | (uint64_t)value << 32; break;
    case 0x30: s.acq = (s.acq & ~0xFFFFFFFFULL) | value; break;
    case 0x34: s.acq = (s.acq & 0xFFFFFFFFULL) | (uint64_t)value << 32; break;
    default: break;
    }
}

/* ---- kernel / block-layer stubs ---- */
#include "../arch/x86_64/nvme.c"
int pm_register_storage(const struct pm_ops* ops) { (void)ops; return 0; }   /* power.h: no system sleep in the simulator */
static char log_text[1 << 16]; static unsigned log_used;
void vga_print(const char* text) { size_t n = strlen(text); if (log_used + n < sizeof(log_text)) { memcpy(log_text + log_used, text, n); log_used += (unsigned)n; log_text[log_used] = 0; } fputs(text, stdout); }
static bool logged(const char* text) { return strstr(log_text, text) != NULL; }
void uint_to_str(uint32_t value, char* buffer) { sprintf(buffer, "%u", value); }
uint64_t arena_alloc(uint64_t bytes) { uint8_t* block = calloc(1, bytes + 4096); assert(block); return ((uint64_t)(uintptr_t)block + 4095) & ~4095ULL; }
void arena_free(uint64_t address, uint64_t bytes) { (void)address; (void)bytes; }
static const char* option;
bool boot_option(const char* word) { return option && !strcmp(word, option); }
static pci_device_t device = {.vendor_id = 0x2646, .device_id = 0x5025, .class_code = 1, .subclass = 8, .prog_if = 2};
pci_device_t* pci_get_devices(void) { return &device; }
int pci_device_count(void) { return 1; }
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) { (void)bus; (void)dev; (void)func; assert(offset == PCI_COMMAND); return 0; }
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) { (void)bus; (void)dev; (void)func; assert(offset == PCI_COMMAND && (value & 6) == 6); }
static uint8_t bar[0x2000];
volatile void* block_pci_bar(pci_device_t* pci, unsigned index, uint64_t bytes) { (void)pci; assert(index == 0 && bytes >= 0x1000); return bar; }
static bool use_irq; static void (*irq_handler)(void*); static void* irq_context; static unsigned msix_calls, detaches, irqs_delivered;
unsigned block_pci_irq_msix(pci_device_t* pci, void (*handler)(void* context), void* context) {
    (void)pci; msix_calls++; irq_handler = handler; irq_context = context; return use_irq ? 0x140 : 0;
}
void block_irq_detach(pci_device_t* pci, unsigned irq, void* context) { (void)pci; (void)irq; assert(context == irq_context); detaches++; }
void block_wait(unsigned line) {
    deliver();
    if (line && s.irq_pending) { s.irq_pending = false; irqs_delivered++; irq_handler(irq_context); }
    else if (!line) s.irq_pending = false;
}
uint64_t block_ticks(void) { return clock_ns; }
static bool direct = true; static uint8_t* bounce;
bool block_dma_direct(const void* buffer, uint64_t bytes, uint64_t limit) { (void)buffer; (void)bytes; (void)limit; return direct; }
uint8_t* block_bounce(uint64_t* bytes) { *bytes = 256 * 1024; return bounce; }
void block_name_number(char* name, const char* prefix, unsigned number, const char* suffix) { sprintf(name, "%s%u%s", prefix, number, suffix ? suffix : ""); }
static struct block_device* registered[8]; static unsigned registered_count;
int block_register(struct block_device* dev) { registered[registered_count++] = dev; return 0; }

/* ---- scenarios ---- */
static void reset_world(uint32_t block_size, uint8_t mdts, uint8_t apsta) {
    for (unsigned i = 0; i < controller_total; i++) controllers[i] = NULL;
    controller_total = 0; registered_count = 0; log_used = 0; log_text[0] = 0; option = NULL; direct = true;
    uint8_t* disk = s.disk; memset(&s, 0, sizeof(s)); s.disk = disk;
    s.cap = 0x3FF | 1ULL << 16 | 20ULL << 24 | 1ULL << 37 | 0ULL << 32;                /* 1024 entries, CQR, TO 10 s, NVM, DSTRD 0 */
    s.block_size = block_size; s.mdts = mdts; s.apsta = apsta; s.vwc = 1; s.rtd3e = 2000000;
    for (uint32_t i = 0; i < DISK_BLOCKS * 512; i++) s.disk[i] = (uint8_t)(i * 7 + (i >> 9));
    irqs_delivered = msix_calls = detaches = 0;
}
static struct block_device* probe(void) {
    bool found = nvme_probe_all();
    if (!found) return NULL;
    assert(registered_count == 1 && msix_calls == 1);
    return registered[0];
}
static uint8_t* buffer(size_t bytes, size_t offset) { uint8_t* b = malloc(bytes + offset + 4096); assert(b); return (uint8_t*)(((uintptr_t)b + 4095) & ~4095ULL) + offset; }
static void check_read(struct block_device* dev, uint64_t lba, uint32_t count, size_t offset) {
    uint8_t* b = buffer((size_t)count * dev->block_size, offset);
    assert(dev->ops->read(dev, lba, count, b) == 0);
    assert(!memcmp(b, s.disk + lba * dev->block_size, (size_t)count * dev->block_size));
}
static void check_write(struct block_device* dev, uint64_t lba, uint32_t count, size_t offset, uint8_t seed) {
    size_t bytes = (size_t)count * dev->block_size; uint8_t* b = buffer(bytes, offset);
    for (size_t i = 0; i < bytes; i++) b[i] = (uint8_t)(seed + i * 13);
    assert(dev->ops->write(dev, lba, count, b) == 0);
    assert(!memcmp(b, s.disk + lba * dev->block_size, bytes));
}

int main(void) {
    s.disk = malloc(DISK_BLOCKS * 512); bounce = buffer(256 * 1024, 0);

    /* 1. Probe with interrupts, APST-capable drive: queues, APST off + PS0, data path incl. PRP lists. */
    reset_world(512, 0, 1); use_irq = true;
    struct block_device* dev = probe();
    assert(dev && dev->block_size == 512 && dev->blocks == DISK_BLOCKS && dev->max_blocks == 4096 && !strcmp(dev->name, "nvme0n1"));
    assert(s.enables == 1 && s.queues_cdw11 == 0 && s.apst_sets == 1 && s.apst_cdw11 == 0 && !s.apst_table_nonzero && s.power_sets == 1 && s.power_cdw11 == 0);
    assert(logged("\"Simulated NVMe SSD\"") && logged("APST-off"));
    check_read(dev, 0, 1, 0); check_read(dev, 3, 8, 0); check_read(dev, 100, 9, 512);       /* PRP1 + PRP2 (offset) */
    check_read(dev, 1000, 4096, 0);                                                         /* 2 MiB: one full list page */
    assert(s.max_prp_lists == 1);
    check_read(dev, 5000, 4096, 1024);                         /* unaligned 2 MiB: 512 entries, still one list page */
    assert(s.max_prp_lists == 1);                              /* (MDTS cap 2 MiB: the driver's list chaining is never needed) */
    check_write(dev, 7, 3, 512, 5); check_write(dev, 9000, 4096, 2048, 9);
    assert(dev->ops->flush(dev) == 0 && s.flushes == 1 && irqs_delivered > 10);
    puts("probe, APST off, PRP data path passed");

    /* 2. A lost command: timeout, controller reset (APST re-applied), the command is retried. */
    unsigned enables = s.enables; s.drop_next = true; uint64_t t0 = clock_ns;
    check_read(dev, 42, 16, 0);
    assert(s.enables == enables + 1 && s.disables >= 1 && s.apst_sets == 2 && dev->stats.resets == 1 && dev->stats.errors == 1);
    assert(clock_ns - t0 >= 30000000000ULL && logged("command timeout opcode=0x00000002") && logged("controller reset, queues recreated"));
    /* 3. Controller fatal status: detected within milliseconds, reset, retried. */
    t0 = clock_ns; s.cfs_next = true; check_write(dev, 77, 2, 0, 3);
    assert(clock_ns - t0 < 1000000000ULL && logged("controller fatal status opcode=0x00000001") && dev->stats.resets == 2);
    /* 4. Error completions: retried once without a reset, unless Do Not Retry is set. */
    unsigned io = s.io_commands; s.status_next = 0x0281;                                    /* media: unrecovered read error */
    uint8_t* b = buffer(4096, 0);
    assert(dev->ops->read(dev, 10, 8, b) == 0 && s.io_commands == io + 2 && dev->stats.resets == 2 && logged("command error opcode=0x00000002 status=0x00000281"));
    io = s.io_commands; s.status_next = 0x4281;                                             /* ... with DNR */
    assert(dev->ops->read(dev, 10, 8, b) < 0 && s.io_commands == io + 1 && dev->stats.resets == 2);
    /* 5. Bounce buffers (non-DMA-able caller memory) split at the bounce size. */
    direct = false; io = s.io_commands; check_read(dev, 200, 1024, 4); check_write(dev, 300, 600, 0, 7); direct = true;
    assert(s.io_commands - io == 2 + 2);                                                    /* 256 KiB bounce: 512 + 512, 512 + 88 */
    puts("timeouts, fatal status, error completions, bounce buffers passed");

    /* 6. Shutdown notification: SHN written once, SHST awaited; a second call does nothing. */
    s.shst_delay = 5; nvme_shutdown_all(); nvme_shutdown_all();
    assert(s.shn_writes == 1 && ((s.cc >> 14) & 3) == 1 && (s.csts & 0xC) == 8 && logged("shutdown complete"));
    puts("shutdown notification passed");

    /* 7. Polled mode, 4 KiB sectors, MDTS 5 (128 KiB) splitting, no APST support, then a controller that is gone. */
    reset_world(4096, 5, 0); use_irq = false;
    dev = probe();
    assert(dev && dev->block_size == 4096 && dev->max_blocks == 32 && s.apst_sets == 0 && s.power_sets == 0 && logged("no-APST"));
    io = s.io_commands; check_read(dev, 10, 100, 0); assert(s.io_commands - io == 4 && irqs_delivered == 0);
    t0 = clock_ns; s.gone = true;
    assert(dev->ops->read(dev, 0, 1, b) < 0 && clock_ns - t0 < 1000000000ULL && logged("controller gone from the bus"));
    s.gone = false; s.no_shst = true; nvme_shutdown_all();
    assert(logged("shutdown notification timed out"));
    puts("polled mode, MDTS, missing APST, surprise removal, shutdown timeout passed");

    /* 8. nvme.apst=keep leaves the drive's power settings alone. */
    reset_world(512, 0, 1); option = "nvme.apst=keep"; dev = probe();
    assert(dev && s.apst_sets == 0 && s.power_sets == 0 && logged("APST-firmware"));
    /* 9. A controller that never becomes ready: probe fails cleanly (interrupt detached, controller disabled). */
    reset_world(512, 0, 1); s.never_ready = 1; use_irq = true;
    assert(!nvme_probe_all() && detaches == 1 && !(s.cc & 1) && logged("controller did not become ready") && registered_count == 0);
    /* 10. Namespace with metadata (LBAF MS != 0): not used. */
    reset_world(512, 0, 0); s.lbaf_metadata = 8;
    nvme_probe_all(); assert(registered_count == 0 && logged("namespace format unsupported") && logged("no usable namespaces"));
    puts("apst=keep, never-ready controller, metadata namespace passed");
    puts("NVME SIMULATOR TESTS PASSED");
    return 0;
}
