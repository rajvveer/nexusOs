/* Host test of the native AHCI driver (arch/x86_64/ahci.c) against a simulated AHCI 1.3 HBA: BIOS/OS handoff
   (with the BIOS busy for a second), HBA reset, staggered spin-up with per-port link timing (a slow link that a
   fixed 20 ms wait missed, a device needing COMRESET, empty ports), link power management switched off, command
   list / FIS / PRDT processing for IDENTIFY, READ/WRITE DMA EXT and FLUSH CACHE EXT on 512 B and 4 KiB-sector
   disks, ATAPI ports ignored, task-file errors and lost commands (COMRESET recovery and retry), 32-bit HBAs
   (DMA limit), interrupts and polling, and a SATA controller left in RAID mode. */
#define AHCI_TEST_MMIO 1
#define NEXUS_HOST_TEST 1
#include "../arch/x86_64/native.h"
#include "../kernel/string.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>

#define PORTS 4
#define DISK_BYTES (4u << 20)
static uint64_t clock_ns;
uint64_t native_clock_ns(void) { return clock_ns += 20000; }
static struct port_sim {
    bool present, atapi, sector4k; uint32_t link_ms;                 /* device and how long its OOB takes */
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, tfd, sig, sctl, serr, ci; uint64_t link_at; bool linking, needs_comreset;
    uint8_t* disk; bool fail_next, fail_quiet_next, drop_next, inflight; unsigned commands, comresets, identifies, flushes;
} ports_sim[PORTS];
static struct {
    uint32_t cap, ghc, is, pi, cap2, bohc; uint64_t hr_until, bios_release_at; bool bios_busy, hr_stuck;
    bool irq_pending; unsigned spinups;
} h;
static uint8_t hba_space[0x1100];
static uint64_t now_ns(void) { return clock_ns; }

static void link_start(struct port_sim* p) {
    if (!p->present || p->needs_comreset) return;
    p->linking = true; p->link_at = now_ns() + p->link_ms * 1000000ULL;
}
static uint32_t ssts(struct port_sim* p) {
    if (!p->present) return 0;
    if (p->needs_comreset) return 1;
    if (p->linking && now_ns() >= p->link_at) { p->linking = false; p->tfd = 0x50; p->sig = p->atapi ? 0xEB140101u : 0x00000101u; }
    return p->linking ? 0 : 0x123;                     /* no COMINIT yet: DET 0; then DET 3, SPD 2, IPM active */
}
/* Execute slot 0: header at CLB, H2D FIS + PRDT at CTBA. */
static void execute(unsigned n) {
    struct port_sim* p = &ports_sim[n];
    uint32_t* header = (uint32_t*)(uintptr_t)(p->clb | (uint64_t)p->clbu << 32);
    assert((header[0] & 31) == 5 && !(p->clb & 1023));
    unsigned prdtl = header[0] >> 16; bool write = (header[0] >> 6) & 1;
    uint8_t* table = (uint8_t*)(uintptr_t)(header[2] | (uint64_t)header[3] << 32); assert(!((uintptr_t)table & 127));
    assert(table[0] == 0x27 && (table[1] & 0x80));
    uint8_t command = table[2]; p->commands++;
    if (p->drop_next) { p->drop_next = false; p->inflight = false; return; }       /* lost: CI stays set */
    if (p->fail_quiet_next) { p->fail_quiet_next = false; p->tfd = 0x51; p->is |= 1; p->ci = 0; h.is |= 1u << n; if (p->ie) h.irq_pending = true; return; }   /* ERR in TFD, no TFES */
    if (p->fail_next) { p->fail_next = false; p->tfd = 0x51 | 0x04 << 8; p->is |= 1u << 30; p->ci = 0; h.is |= 1u << n; if (p->ie) h.irq_pending = true; return; }
    uint64_t lba = table[4] | (uint64_t)table[5] << 8 | (uint64_t)table[6] << 16 | (uint64_t)table[8] << 24 | (uint64_t)table[9] << 32 | (uint64_t)table[10] << 40;
    uint32_t count = table[12] | (uint32_t)table[13] << 8; if (!count) count = 65536;
    uint32_t sector = p->sector4k ? 4096 : 512, bytes = 0;
    static uint8_t identify[512];
    uint8_t* source = NULL;
    if (command == 0xEC) {
        memset(identify, 0, 512); uint16_t* w = (uint16_t*)identify; p->identifies++;
        const char* model = "SIMULATED SATA SSD                      ";   /* 40 characters, space padded */
        for (unsigned i = 0; i < 20; i++) w[27 + i] = (uint16_t)(model[2 * i] << 8 | model[2 * i + 1]);
        w[83] = 0x4000 | 1 << 10 | 1 << 12 | 1 << 13; uint64_t sectors = DISK_BYTES / sector;
        w[100] = (uint16_t)sectors; w[101] = (uint16_t)(sectors >> 16);
        if (p->sector4k) { w[106] = 0x4000 | 1 << 12; w[117] = 2048; w[118] = 0; }
        source = identify; bytes = 512;
    } else if (command == 0x25 || command == 0x35) {
        assert((lba + count) * sector <= DISK_BYTES && (table[7] & 0x40));
        source = p->disk + lba * sector; bytes = count * sector;
    } else if (command == 0xEA) p->flushes++;
    else assert(!"unexpected command");
    uint32_t done = 0;
    for (unsigned i = 0; i < prdtl; i++) {
        uint32_t* prd = (uint32_t*)(table + 0x80 + i * 16); uint8_t* buffer = (uint8_t*)(uintptr_t)(prd[0] | (uint64_t)prd[1] << 32);
        uint32_t n = (prd[3] & 0x3FFFFF) + 1; assert(!(prd[0] & 1) && n <= 4u << 20);
        if (write) memcpy(source + done, buffer, n); else memcpy(buffer, source + done, n);
        done += n;
    }
    assert(done == bytes && (command != 0x35 || write));
    p->tfd = 0x50; p->is |= 1; p->ci = 0; h.is |= 1u << n; if (p->ie) h.irq_pending = true;
}
static void run_pending(void) { for (unsigned n = 0; n < PORTS; n++) if (ports_sim[n].inflight) { ports_sim[n].inflight = false; execute(n); } }

static uint32_t rd(volatile uint8_t* base, unsigned offset) {
    unsigned at = (unsigned)((const uint8_t*)base - hba_space) + offset;
    if (at < 0x100) switch (at) {
        case 0x00: return h.cap;
        case 0x04: if ((h.ghc & 1) && !h.hr_stuck && now_ns() >= h.hr_until) h.ghc &= ~1u; return h.ghc;
        case 0x08: return h.is;
        case 0x0C: return h.pi;
        case 0x10: return 0x00010301;
        case 0x24: return h.cap2;
        case 0x28: if (h.bios_busy && (h.bohc & 2) && now_ns() >= h.bios_release_at) { h.bohc &= ~0x11u; } return h.bohc;
        default: return 0;
    }
    unsigned n = (at - 0x100) / 0x80, r = (at - 0x100) % 0x80; assert(n < PORTS);
    struct port_sim* p = &ports_sim[n];
    switch (r) {
    case 0x00: return p->clb; case 0x04: return p->clbu; case 0x08: return p->fb; case 0x0C: return p->fbu;
    case 0x10: return p->is; case 0x14: return p->ie;
    case 0x18: return p->cmd | (p->cmd & 1 ? 0x8000u : 0) | (p->cmd & 0x10 ? 0x4000u : 0);   /* CR follows ST, FR follows FRE */
    case 0x20: return p->tfd; case 0x24: return p->sig; case 0x28: return ssts(p); case 0x2C: return p->sctl;
    case 0x30: return p->serr; case 0x38: return p->ci;
    default: return 0;
    }
}
static void wr(volatile uint8_t* base, unsigned offset, uint32_t value) {
    unsigned at = (unsigned)((uint8_t*)base - hba_space) + offset;
    if (at < 0x100) {
        switch (at) {
        case 0x04:
            if (value & 1) { h.ghc = value; h.hr_until = now_ns() + 5000000ULL;          /* HBA reset: ports lose state */
                for (unsigned n = 0; n < PORTS; n++) { struct port_sim* p = &ports_sim[n]; p->cmd = 0; p->is = p->ie = p->ci = 0; p->sctl = 0; p->tfd = 0x7F; p->linking = false; } }
            else h.ghc = (h.ghc & 1) | (value & ~1u);
            break;
        case 0x08: h.is &= ~value; break;
        case 0x28: h.bohc = value & 0x1F; if (value & 2) { if (h.bios_busy) { h.bohc |= 0x10 | 1; h.bios_release_at = now_ns() + 1000000000ULL; } else h.bohc &= ~1u; } break;
        default: break;
        }
        return;
    }
    unsigned n = (at - 0x100) / 0x80, r = (at - 0x100) % 0x80; assert(n < PORTS);
    struct port_sim* p = &ports_sim[n];
    switch (r) {
    case 0x00: p->clb = value; break; case 0x04: p->clbu = value; break; case 0x08: p->fb = value; break; case 0x0C: p->fbu = value; break;
    case 0x10: p->is &= ~value; break;
    case 0x14: p->ie = value; break;
    case 0x18: {
        uint32_t old = p->cmd; p->cmd = value & ~0xC000u;
        if ((value & 2) && !(old & 2)) { h.spinups++; assert((p->sctl & 0xF00) == 0x300); link_start(p); }   /* spin-up after IPM was set */
        if ((value & 1) && !(old & 1)) assert(p->cmd & 0x10);                        /* ST only with FRE */
        break;
    }
    case 0x2C: {
        uint32_t old = p->sctl; p->sctl = value;
        if ((old & 15) == 1 && (value & 15) == 0) { p->comresets++; p->needs_comreset = false; p->tfd = 0x80; link_start(p); }
        if ((value & 15) == 1) p->linking = false;
        break;
    }
    case 0x30: p->serr &= ~value; break;
    case 0x38: assert(value == 1 && (p->cmd & 1) && !(p->tfd & 0x88)); p->ci |= value; p->inflight = true; break;
    default: break;
    }
}

/* ---- kernel / block-layer stubs ---- */
#include "../arch/x86_64/ahci.c"
int pm_register_storage(const struct pm_ops* ops) { (void)ops; return 0; }   /* power.h: no system sleep in the simulator */
static char log_text[1 << 16]; static unsigned log_used;
void vga_print(const char* text) { size_t n = strlen(text); if (log_used + n < sizeof(log_text)) { memcpy(log_text + log_used, text, n); log_used += (unsigned)n; log_text[log_used] = 0; } fputs(text, stdout); }
static bool logged(const char* text) { return strstr(log_text, text) != NULL; }
void uint_to_str(uint32_t value, char* buffer) { sprintf(buffer, "%u", value); }
uint64_t dma32_alloc(uint64_t bytes) { uint8_t* block = calloc(1, bytes + 4096); assert(block); return ((uint64_t)(uintptr_t)block + 4095) & ~4095ULL; }
void arena_free(uint64_t address, uint64_t bytes) { (void)address; (void)bytes; }
static pci_device_t devices[2]; static int device_total;
pci_device_t* pci_get_devices(void) { return devices; }
int pci_device_count(void) { return device_total; }
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) { (void)bus; (void)dev; (void)func; assert(offset == PCI_COMMAND); return 0; }
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) { (void)bus; (void)dev; (void)func; assert(offset == PCI_COMMAND && (value & 6) == 6); }
volatile void* block_pci_bar(pci_device_t* pci, unsigned index, uint64_t bytes) { (void)pci; assert(index == 5 && bytes >= 0x100 + PORTS * 0x80); return hba_space; }
static bool use_irq; static void (*irq_handler)(void*); static void* irq_context; static unsigned irqs_delivered;
unsigned block_pci_irq(pci_device_t* pci, void (*handler)(void* context), void* context) { (void)pci; irq_handler = handler; irq_context = context; return use_irq ? 0x141 : 0; }
void block_wait(unsigned line) {
    run_pending();
    if (line && h.irq_pending) { h.irq_pending = false; irqs_delivered++; irq_handler(irq_context); }
    else if (!line) h.irq_pending = false;
}
uint64_t block_ticks(void) { return clock_ns; }
static uint64_t dma_limit_seen; static bool direct = true; static uint8_t* bounce;
bool block_dma_direct(const void* buffer, uint64_t bytes, uint64_t limit) { (void)buffer; (void)bytes; dma_limit_seen = limit; return direct; }
uint8_t* block_bounce(uint64_t* bytes) { *bytes = 1 << 20; return bounce; }
void block_name_number(char* name, const char* prefix, unsigned number, const char* suffix) { sprintf(name, "%s%u%s", prefix, number, suffix ? suffix : ""); }
static struct block_device* registered[8]; static unsigned registered_count;
int block_register(struct block_device* dev) { registered[registered_count++] = dev; return 0; }

/* ---- scenarios ---- */
static void world(uint32_t cap, uint32_t pi, bool bios_busy) {
    for (unsigned i = 0; i < port_total; i++) ports[i] = NULL;
    port_total = hba_total = 0; registered_count = 0; log_used = 0; log_text[0] = 0; irqs_delivered = 0;
    memset(&h, 0, sizeof(h)); h.cap = cap; h.pi = pi; h.cap2 = 1; h.bios_busy = bios_busy; h.bohc = 1;   /* BIOS owns it */
    for (unsigned n = 0; n < PORTS; n++) { uint8_t* disk = ports_sim[n].disk; memset(&ports_sim[n], 0, sizeof(ports_sim[n])); ports_sim[n].disk = disk; ports_sim[n].tfd = 0x7F; }
    devices[0] = (pci_device_t){.vendor_id = 0x1022, .device_id = 0x43F6, .class_code = 1, .subclass = 6, .prog_if = 1}; device_total = 1;
}
static uint8_t* buffer(size_t bytes) { uint8_t* b = malloc(bytes + 4096); assert(b); return (uint8_t*)(((uintptr_t)b + 4095) & ~4095ULL); }
static void roundtrip(struct block_device* dev, struct port_sim* p, uint64_t lba, uint32_t count, uint8_t seed) {
    size_t bytes = (size_t)count * dev->block_size; uint8_t* b = buffer(bytes);
    for (size_t i = 0; i < bytes; i++) b[i] = (uint8_t)(seed + i * 31);
    assert(dev->ops->write(dev, lba, count, b) == 0 && !memcmp(p->disk + lba * dev->block_size, b, bytes));
    memset(b, 0, bytes);
    assert(dev->ops->read(dev, lba, count, b) == 0 && !memcmp(p->disk + lba * dev->block_size, b, bytes));
}

int main(void) {
    for (unsigned n = 0; n < PORTS; n++) ports_sim[n].disk = calloc(1, DISK_BYTES);
    bounce = buffer(1 << 20);
    const uint32_t cap64 = 1u << 31 | 1u << 27 | 31u << 8 | (PORTS - 1);              /* S64A, SSS, 32 slots, 4 ports */

    /* 1. AMD 600-series HBA: port 0 SSD (5 ms link), port 1 HDD whose COMINIT comes after 60 ms, port 2 needs a COMRESET,
          port 3 empty; BIOS busy for a second during the handoff; interrupts. */
    world(cap64, 0xF, true); use_irq = true;
    ports_sim[0].present = true; ports_sim[0].link_ms = 5;
    ports_sim[1].present = true; ports_sim[1].link_ms = 60; ports_sim[1].sector4k = true;   /* a fixed 20 ms wait missed it */
    ports_sim[2].present = true; ports_sim[2].needs_comreset = true; ports_sim[2].link_ms = 10;
    uint64_t t0 = clock_ns;
    assert(ahci_probe_all() && registered_count == 3 && h.spinups == 4);
    assert(clock_ns - t0 >= 1000000000ULL && logged("AHCI BIOS/OS handoff complete") && !(h.bohc & 1) && (h.bohc & 2));
    assert(ports_sim[2].comresets == 1 && ports_sim[0].comresets == 0 && ports_sim[1].comresets == 0);
    struct block_device *ssd = registered[0], *hdd = registered[1], *third = registered[2];
    assert(ssd->block_size == 512 && hdd->block_size == 4096 && hdd->blocks == DISK_BYTES / 4096 && !strcmp(ssd->model, "SIMULATED SATA SSD"));
    for (unsigned n = 0; n < 3; n++) assert((ports_sim[n].sctl & 0xF0F) == 0x300 && ports_sim[n].ie);
    roundtrip(ssd, &ports_sim[0], 10, 1, 1); roundtrip(ssd, &ports_sim[0], 100, 4096, 2);   /* 2 MiB: one command */
    roundtrip(hdd, &ports_sim[1], 3, 300, 3); roundtrip(third, &ports_sim[2], 5, 9, 4);
    assert(ssd->ops->flush(ssd) == 0 && ports_sim[0].flushes == 1 && irqs_delivered > 6 && dma_limit_seen == ~0ULL);
    puts("handoff, slow link, COMRESET port, 4K sectors, data path passed");

    /* 2. Task-file error: COMRESET recovery, retried; a lost command: 30 s timeout, recovery, retried. */
    unsigned before = ports_sim[0].comresets; ports_sim[0].fail_next = true;
    roundtrip(ssd, &ports_sim[0], 7, 8, 5);
    assert(ports_sim[0].comresets == before + 1 && ssd->stats.resets == 1 && ssd->stats.errors == 1 && logged("command error cmd=0x00000035"));
    t0 = clock_ns; ports_sim[0].drop_next = true;
    roundtrip(ssd, &ports_sim[0], 9, 8, 6);
    assert(clock_ns - t0 >= 30000000000ULL && ports_sim[0].comresets == before + 2 && logged("command timeout cmd=0x00000035") && logged("port reset, link up"));
    before = ports_sim[0].comresets; ports_sim[0].fail_quiet_next = true; roundtrip(ssd, &ports_sim[0], 11, 8, 8);
    assert(ports_sim[0].comresets == before + 1);                     /* ERR status without TFES is an error too */
    puts("task-file error and timeout recovery passed");

    /* 3. 32-bit HBA, polled, no BIOS handoff capability, an ATAPI drive, all links up at once (no debounce wait). */
    world(1u << 27 | 31u << 8 | (PORTS - 1), 0x3, false); use_irq = false; h.cap2 = 0;
    ports_sim[0].present = true; ports_sim[0].link_ms = 1; ports_sim[1].present = true; ports_sim[1].atapi = true; ports_sim[1].link_ms = 1;
    t0 = clock_ns;
    assert(ahci_probe_all() && registered_count == 1 && logged("ATAPI device ignored") && clock_ns - t0 < 100000000ULL);
    roundtrip(registered[0], &ports_sim[0], 1, 16, 7);
    assert(dma_limit_seen == 0x100000000ULL && irqs_delivered == 0);
    puts("32-bit HBA, polling, ATAPI, fast links passed");

    /* 4. HBA reset that never completes; a controller in RAID mode. */
    world(cap64, 0x1, false); h.hr_stuck = true; ports_sim[0].present = true;
    assert(!ahci_probe_all() && logged("AHCI HBA reset timeout") && registered_count == 0);
    world(cap64, 0x1, false); devices[0].subclass = 4; devices[0].prog_if = 0;
    assert(!ahci_probe_all() && logged("storage controller 1022:43f6 is in RAID mode"));
    puts("reset timeout and RAID mode passed");
    puts("AHCI SIMULATOR TESTS PASSED");
    return 0;
}
