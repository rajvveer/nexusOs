/* NVMe 1.x driver: controller reset/enable, admin queue, Identify Controller/Namespace list,
 * one I/O queue pair, Read/Write/Flush with PRP1/PRP2 and chained PRP lists, doorbell stride,
 * timeouts with controller reset. Completion: MSI-X, else MSI, else PCI INTx (block_pci_irq_msix;
 * the handler reaps the completion queues) with polling as the fallback. Requests are synchronous
 * under the big kernel lock, so one outstanding command per queue.
 * Bare-metal hardening: Autonomous Power State Transitions switched off and power state 0 selected
 * (after every reset; nvme.apst=keep leaves the firmware's setting) - several consumer SSDs drop off
 * the bus from deep APST states; controller fatal status / timeouts reset the controller and retry
 * once; a controller that reads all-ones (gone from the bus) fails fast; nvme_shutdown_all() sends
 * the normal shutdown notification (CC.SHN) before power-off. tests/test_nvme_sim.py simulates all of it. */
#include "native.h"
#include "power.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "../../kernel/pci.h"
#include "boot_info.h"

#define REG_CAP 0x00
#define REG_VS 0x08
#define REG_INTMS 0x0C
#define REG_INTMC 0x10
#define REG_CC 0x14
#define REG_CSTS 0x1C
#define REG_AQA 0x24
#define REG_ASQ 0x28
#define REG_ACQ 0x30
#define CSTS_RDY 0x1u
#define CSTS_CFS 0x2u
#define CSTS_SHST_DONE 0x8u
#define CC_SHN_NORMAL (1u << 14)
#define FEATURE_POWER 0x02
#define FEATURE_APST 0x0C
#define ADMIN_ENTRIES 32
#define IO_ENTRIES 64
#define PRP_PAGES 2
#define MAX_TRANSFER (2u * 1024 * 1024)
#define MAX_CONTROLLERS 4
#define MAX_NAMESPACES 4
#define ADMIN_TIMEOUT_MS 5000
#define IO_TIMEOUT_MS 30000
#define FLUSH_TIMEOUT_MS 60000
#define PAGE_BYTES 4096u

struct nvme_queue {
    uint8_t* sq; uint8_t* cq; uint16_t entries, id, sq_tail, cq_head, cid; uint8_t phase;
    unsigned sq_doorbell, cq_doorbell;   /* register offsets */
    volatile bool done; volatile uint16_t status; volatile uint32_t result; uint16_t wait_cid;
};
struct nvme_ctrl;
struct nvme_ns { struct block_device dev; struct nvme_ctrl* ctrl; uint32_t nsid; };
struct nvme_ctrl {
    volatile uint8_t* regs; unsigned index, irq; uint64_t cap; uint32_t stride, max_transfer, timeout_ms, rtd3e_us;
    bool vwc, apsta, apst_off, shut_down;
    struct nvme_queue admin, io;
    uint64_t* prp[PRP_PAGES];
    uint8_t* identify;          /* 4 KiB page for Identify data */
    struct nvme_ns* ns[MAX_NAMESPACES]; unsigned namespaces;
    struct block_stats* irq_stats;
};
static struct nvme_ctrl* controllers[MAX_CONTROLLERS];
static unsigned controller_total;

#ifndef NVME_TEST_MMIO   /* tests/nvme_sim_test.c supplies rd/wr over a simulated controller */
static inline uint32_t rd(volatile uint8_t* base, unsigned offset) { return *(volatile uint32_t*)(base + offset); }
static inline void wr(volatile uint8_t* base, unsigned offset, uint32_t value) { *(volatile uint32_t*)(base + offset) = value; }
#endif
static void wr64(volatile uint8_t* base, unsigned offset, uint64_t value) { wr(base, offset, (uint32_t)value); wr(base, offset + 4, (uint32_t)(value >> 32)); }
static void print_hex32(uint32_t value) { char text[11] = "0x"; for (int i = 0; i < 8; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (28 - 4 * i)) & 15]; text[10] = 0; vga_print(text); }
static void message(const struct nvme_ctrl* c, const char* text) { char d[12]; uint_to_str(c->index, d); vga_print("[NVMe] nvme"); vga_print(d); vga_print(": "); vga_print(text); }

static void queue_reset(struct nvme_ctrl* c, struct nvme_queue* q) {
    memset(q->sq, 0, (uint64_t)q->entries * 64); memset(q->cq, 0, (uint64_t)q->entries * 16);
    q->sq_tail = q->cq_head = 0; q->phase = 1; q->done = false;
    q->sq_doorbell = 0x1000 + (2u * q->id) * c->stride;
    q->cq_doorbell = 0x1000 + (2u * q->id + 1) * c->stride;
}
/* Consume every new completion entry (phase tag), remember the awaited one, ring the head doorbell.
 * Runs from the IRQ handler and from the waiting loop; both hold the kernel lock. */
static void reap(struct nvme_ctrl* c, struct nvme_queue* q) {
    bool advanced = false;
    for (;;) {
        volatile uint32_t* entry = (volatile uint32_t*)(q->cq + (uint64_t)q->cq_head * 16);
        uint32_t dw3 = entry[3];
        if (((dw3 >> 16) & 1) != q->phase) break;
        if ((uint16_t)dw3 == q->wait_cid && !q->done) { q->result = entry[0]; q->status = (uint16_t)(dw3 >> 17); q->done = true; }
        if (++q->cq_head == q->entries) { q->cq_head = 0; q->phase ^= 1; }
        advanced = true;
    }
    if (advanced) wr(c->regs, q->cq_doorbell, q->cq_head);
}
static void nvme_irq(void* context) {
    struct nvme_ctrl* c = context;
    if (rd(c->regs, REG_CSTS) == ~0u) return;
    bool pending = false;
    for (unsigned i = 0; i < 2; i++) {
        struct nvme_queue* q = i ? &c->io : &c->admin;
        if (!q->sq) continue;
        volatile uint32_t* entry = (volatile uint32_t*)(q->cq + (uint64_t)q->cq_head * 16);
        if (((entry[3] >> 16) & 1) == q->phase) pending = true;
        reap(c, q);
    }
    if (pending && c->irq_stats) c->irq_stats->irqs++;
}
/* 0 ok, -1 command error status, -2 timeout or controller fatal status (reset required). */
static int execute(struct nvme_ctrl* c, struct nvme_queue* q, uint32_t* command, unsigned timeout_ms) {
    if (++q->cid == 0xFFFF) q->cid = 1;
    command[0] = (command[0] & 0xFFFFu) | (uint32_t)q->cid << 16;
    q->wait_cid = q->cid; q->done = false;
    memcpy(q->sq + (uint64_t)q->sq_tail * 64, command, 64);
    if (++q->sq_tail == q->entries) q->sq_tail = 0;
    __asm__ volatile("mfence" ::: "memory");
    wr(c->regs, q->sq_doorbell, q->sq_tail);
    uint64_t start = native_clock_ns(), deadline = start + timeout_ms * 1000000ULL, check = start + 1000000;
    for (;;) {
        reap(c, q);   /* completion detection reads only the CQ in memory: no register traffic while polling */
        if (q->done) break;
        uint64_t now = native_clock_ns();
        if (now >= check || now > deadline) {
            uint32_t status = rd(c->regs, REG_CSTS); check = now + 1000000;
            if ((status & CSTS_CFS) || now > deadline) {
                volatile uint32_t* entry = (volatile uint32_t*)(q->cq + (uint64_t)q->cq_head * 16);
                message(c, status == ~0u ? "controller gone from the bus (registers read all-ones) opcode=" :
                           (status & CSTS_CFS) ? "controller fatal status opcode=" : "command timeout opcode="); print_hex32(command[0] & 0xFF);
                vga_print(" queue="); print_hex32(q->id); vga_print(" cid="); print_hex32(q->cid); vga_print(" sq_tail="); print_hex32(q->sq_tail);
                vga_print(" cq_head="); print_hex32(q->cq_head); vga_print(" phase="); print_hex32(q->phase); vga_print(" cqe3="); print_hex32(entry[3]);
                vga_print(" csts="); print_hex32(status); vga_print(" intms="); print_hex32(rd(c->regs, REG_INTMS)); vga_print("\n");
                return -2;
            }
        }
        block_wait(c->irq);
    }
    if (q->status & 0x7FF) {
        message(c, "command error opcode="); print_hex32(command[0] & 0xFF); vga_print(" status="); print_hex32(q->status); vga_print("\n");
        return -1;
    }
    return 0;
}
static int admin(struct nvme_ctrl* c, uint8_t opcode, uint32_t nsid, uint64_t prp1, uint32_t cdw10, uint32_t cdw11) {
    uint32_t command[16] = {0};
    command[0] = opcode; command[1] = nsid; command[6] = (uint32_t)prp1; command[7] = (uint32_t)(prp1 >> 32);
    command[10] = cdw10; command[11] = cdw11;
    return execute(c, &c->admin, command, ADMIN_TIMEOUT_MS);
}

static bool wait_ready(struct nvme_ctrl* c, bool ready) {
    uint64_t deadline = native_clock_ns() + c->timeout_ms * 1000000ULL;
    for (;;) {
        uint32_t status = rd(c->regs, REG_CSTS);
        if (status == ~0u) return false;
        if (ready && (status & CSTS_CFS)) return false;
        if (!!(status & CSTS_RDY) == ready) return true;
        if (native_clock_ns() > deadline) return false;
        __asm__ volatile("pause");
    }
}
/* Disable, program the admin queue, enable, then (re)create the I/O queue pair. */
static bool controller_start(struct nvme_ctrl* c) {
    wr(c->regs, REG_CC, rd(c->regs, REG_CC) & ~1u);
    if (!wait_ready(c, false)) { message(c, "controller did not disable\n"); return false; }
    queue_reset(c, &c->admin); queue_reset(c, &c->io);
    wr(c->regs, REG_AQA, (uint32_t)(ADMIN_ENTRIES - 1) << 16 | (ADMIN_ENTRIES - 1));
    wr64(c->regs, REG_ASQ, virt_to_phys(c->admin.sq)); wr64(c->regs, REG_ACQ, virt_to_phys(c->admin.cq));
    wr(c->regs, REG_INTMC, 1);
    /* IOCQES 16 B, IOSQES 64 B, round-robin arbitration, 4 KiB pages, NVM command set, enable. */
    wr(c->regs, REG_CC, (4u << 20) | (6u << 16) | 1u);
    if (!wait_ready(c, true)) { message(c, "controller did not become ready\n"); return false; }
    if (admin(c, 0x09, 0, 0, 0x07, 0) < 0) return false;   /* Set Features: one I/O SQ/CQ pair */
    /* Create I/O CQ 1 (physically contiguous, interrupts on vector 0) and I/O SQ 1 bound to it. */
    if (admin(c, 0x05, 0, virt_to_phys(c->io.cq), (uint32_t)(IO_ENTRIES - 1) << 16 | 1, 1u | 2u) < 0) return false;
    if (admin(c, 0x01, 0, virt_to_phys(c->io.sq), (uint32_t)(IO_ENTRIES - 1) << 16 | 1, 1u << 16 | 1u) < 0) return false;
    return true;
}
/* APST off and power state 0 (Set Features 0Ch with a zeroed 256-byte table, then 02h), unless nvme.apst=keep. */
static void power_policy(struct nvme_ctrl* c) {
    if (!c->apsta || boot_option("nvme.apst=keep")) return;
    memset(c->identify, 0, PAGE_BYTES);
    c->apst_off = admin(c, 0x09, 0, virt_to_phys(c->identify), FEATURE_APST, 0) == 0 && admin(c, 0x09, 0, 0, FEATURE_POWER, 0) == 0;
}
static bool controller_reset(struct nvme_ctrl* c) {
    for (unsigned i = 0; i < c->namespaces; i++) c->ns[i]->dev.stats.resets++;
    bool ok = controller_start(c);
    if (ok) power_policy(c);
    message(c, ok ? "controller reset, queues recreated\n" : "controller reset failed\n");
    return ok;
}

/* PRP1 = first (possibly unaligned) page, PRP2 = second page or a PRP list; a full list page
 * chains to the next through its last entry. */
static bool build_prps(struct nvme_ctrl* c, uint64_t address, uint32_t bytes, uint64_t* prp1, uint64_t* prp2) {
    *prp1 = address; *prp2 = 0;
    uint32_t first = PAGE_BYTES - (uint32_t)(address & (PAGE_BYTES - 1));
    if (bytes <= first) return true;
    uint64_t next = address + first; uint32_t pages = (bytes - first + PAGE_BYTES - 1) / PAGE_BYTES;
    if (pages == 1) { *prp2 = next; return true; }
    unsigned list = 0, slot = 0; uint64_t* entries = c->prp[0];
    *prp2 = virt_to_phys(entries);
    for (uint32_t i = 0; i < pages; i++) {
        if (slot == 511 && i + 1 < pages) {
            if (++list == PRP_PAGES) return false;
            entries[511] = virt_to_phys(c->prp[list]); entries = c->prp[list]; slot = 0;
        }
        entries[slot++] = next + (uint64_t)i * PAGE_BYTES;
    }
    return true;
}
static int io_command(struct nvme_ns* ns, uint8_t opcode, uint64_t lba, uint32_t count, uint64_t address, uint32_t bytes, unsigned timeout_ms) {
    struct nvme_ctrl* c = ns->ctrl;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        uint32_t command[16] = {0};
        uint64_t prp1 = 0, prp2 = 0;
        if (bytes && !build_prps(c, address, bytes, &prp1, &prp2)) return -1;
        command[0] = opcode; command[1] = ns->nsid;
        command[6] = (uint32_t)prp1; command[7] = (uint32_t)(prp1 >> 32); command[8] = (uint32_t)prp2; command[9] = (uint32_t)(prp2 >> 32);
        if (bytes) { command[10] = (uint32_t)lba; command[11] = (uint32_t)(lba >> 32); command[12] = count - 1; }
        uint64_t start = block_ticks();
        int result = execute(c, &c->io, command, timeout_ms);
        ns->dev.stats.busy_ticks += block_ticks() - start;
        if (!result) return 0;
        ns->dev.stats.errors++;
        if (result == -1 && (c->io.status & 0x4000)) return -1;     /* Do Not Retry */
        if (result == -2 && !controller_reset(c)) return -1;
    }
    return -1;
}
static int nvme_transfer(struct block_device* dev, uint64_t lba, uint32_t count, uint8_t* data, bool write) {
    struct nvme_ns* ns = dev->driver;
    uint32_t bs = dev->block_size;
    uint64_t bounce_bytes; uint8_t* bounce = block_bounce(&bounce_bytes);
    bool direct = block_dma_direct(data, (uint64_t)count * bs, ~0ULL);
    uint32_t limit = dev->max_blocks;
    if (!direct && bounce_bytes / bs < limit) limit = (uint32_t)(bounce_bytes / bs);
    while (count) {
        uint32_t n = count < limit ? count : limit, bytes = n * bs;
        uint8_t* target = direct ? data : bounce;
        if (write && !direct) memcpy(bounce, data, bytes);
        if (io_command(ns, write ? 0x01 : 0x02, lba, n, virt_to_phys(target), bytes, IO_TIMEOUT_MS) < 0) return -1;
        if (!write && !direct) memcpy(data, bounce, bytes);
        if (write) { dev->stats.write_cmds++; dev->stats.write_sectors += bytes / 512; }
        else { dev->stats.read_cmds++; dev->stats.read_sectors += bytes / 512; }
        lba += n; count -= n; data += bytes;
    }
    return 0;
}
static int nvme_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buffer) { return nvme_transfer(dev, lba, count, buffer, false); }
static int nvme_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buffer) { return nvme_transfer(dev, lba, count, (uint8_t*)buffer, true); }
static int nvme_flush(struct block_device* dev) {
    struct nvme_ns* ns = dev->driver;
    if (!ns->ctrl->vwc) return 0; /* no volatile write cache: completed writes are durable */
    dev->stats.flushes++;
    return io_command(ns, 0x00, 0, 0, 0, 0, FLUSH_TIMEOUT_MS);
}
static const struct block_ops nvme_ops = {nvme_read, nvme_write, nvme_flush};

static void copy_string(char* out, const uint8_t* in, unsigned length) {
    memcpy(out, in, length); out[length] = 0;
    for (unsigned i = length; i && (out[i - 1] == ' ' || !out[i - 1]); i--) out[i - 1] = 0;
}
static char controller_serial[21];   /* Identify Controller SN, for the namespaces added next */
static void add_namespace(struct nvme_ctrl* c, uint32_t nsid, const char* model) {
    if (c->namespaces == MAX_NAMESPACES) return;
    if (admin(c, 0x06, nsid, virt_to_phys(c->identify), 0, 0) < 0) return;  /* Identify Namespace */
    const uint8_t* id = c->identify;
    uint64_t blocks; memcpy(&blocks, id, 8);
    uint8_t format = id[26] & 15; uint32_t lbaf; memcpy(&lbaf, id + 128 + 4 * format, 4);
    uint32_t block_size = 1u << ((lbaf >> 16) & 0xFF);
    if (!blocks) return;
    if ((lbaf & 0xFFFF) || (block_size != 512 && block_size != 4096)) { message(c, "namespace format unsupported\n"); return; }
    uint64_t page = arena_alloc(sizeof(struct nvme_ns));
    if (!page) return;
    struct nvme_ns* ns = (struct nvme_ns*)(uintptr_t)page;
    ns->ctrl = c; ns->nsid = nsid;
    char name[12]; block_name_number(name, "nvme", c->index, "n"); block_name_number(ns->dev.name, name, nsid, NULL);
    strncpy(ns->dev.model, model, sizeof(ns->dev.model) - 1);
    strncpy(ns->dev.serial, controller_serial, sizeof(ns->dev.serial) - 1);
    ns->dev.kind = "NVMe"; ns->dev.block_size = block_size; ns->dev.blocks = blocks;
    ns->dev.max_blocks = c->max_transfer / block_size; ns->dev.ops = &nvme_ops; ns->dev.driver = ns;
    c->ns[c->namespaces++] = ns;
}
/* System sleep (power.h): flush, then a normal shutdown notification (CC.SHN, CSTS.SHST complete) so the device
 * commits its state before power goes; after the power loss the queues are created again (MSI-X is back first). */
static int nvme_pm_suspend(void* context, unsigned state) {
    struct nvme_ctrl* c = context; (void)state;
    if (c->vwc && c->namespaces) nvme_flush(&c->ns[0]->dev);
    wr(c->regs, REG_CC, (rd(c->regs, REG_CC) & ~(3u << 14)) | (1u << 14));
    uint64_t deadline = native_clock_ns() + c->timeout_ms * 1000000ULL;
    while (((rd(c->regs, REG_CSTS) >> 2) & 3) != 2 && native_clock_ns() < deadline) __asm__ volatile("pause");
    c->shut_down = true;                                 /* nvme_shutdown_all: already notified */
    return 0;
}
static void nvme_pm_resume(void* context, unsigned state) {
    struct nvme_ctrl* c = context; (void)state;
    c->shut_down = false;
    if (!controller_start(c)) { message(c, "controller did not come back after resume\n"); return; }
    power_policy(c);                                     /* APST off / PS0 again: the features reset with the power */
}
static void controller_probe(pci_device_t* pci) {
    if (controller_total == MAX_CONTROLLERS) return;
    uint16_t command = pci_read16(pci->bus, pci->device, pci->function, PCI_COMMAND);
    pci_write32(pci->bus, pci->device, pci->function, PCI_COMMAND, (uint32_t)((command | 6u) & ~0x400u));
    volatile uint8_t* regs = block_pci_bar(pci, 0, 0x2000);
    if (!regs) { vga_print("[64] NVMe BAR0 missing or cannot be mapped (I/O, above 4 GiB or overlapping RAM)\n"); return; }
    uint64_t cap = (uint64_t)rd(regs, REG_CAP) | (uint64_t)rd(regs, REG_CAP + 4) << 32;
    uint32_t stride = 4u << ((cap >> 32) & 15);
    if (((cap >> 48) & 15) != 0 || !((cap >> 37) & 1)) { vga_print("[64] NVMe controller needs >4 KiB pages or lacks the NVM command set\n"); return; }
    if (0x1000 + 4 * stride > 0x2000 && !block_pci_bar(pci, 0, 0x1000 + 4 * stride)) return;
    unsigned entries = (unsigned)(cap & 0xFFFF) + 1;
    if (entries < IO_ENTRIES) { vga_print("[64] NVMe queues too small\n"); return; }
    /* Pages: controller, admin SQ/CQ, I/O SQ/CQ, PRP lists, Identify. */
    uint64_t memory = arena_alloc((6 + PRP_PAGES) * (uint64_t)PAGE_BYTES);
    if (!memory) { vga_print("[64] NVMe no memory\n"); return; }
    struct nvme_ctrl* c = (struct nvme_ctrl*)(uintptr_t)memory;
    c->regs = regs; c->index = controller_total; c->cap = cap; c->stride = stride;
    c->timeout_ms = (unsigned)((cap >> 24) & 0xFF) * 500; if (c->timeout_ms < 500) c->timeout_ms = 500;
    c->admin.sq = (uint8_t*)(uintptr_t)(memory + PAGE_BYTES); c->admin.cq = (uint8_t*)(uintptr_t)(memory + 2 * PAGE_BYTES);
    c->io.sq = (uint8_t*)(uintptr_t)(memory + 3 * PAGE_BYTES); c->io.cq = (uint8_t*)(uintptr_t)(memory + 4 * PAGE_BYTES);
    c->identify = (uint8_t*)(uintptr_t)(memory + 5 * PAGE_BYTES);
    for (unsigned i = 0; i < PRP_PAGES; i++) c->prp[i] = (uint64_t*)(uintptr_t)(memory + (6 + i) * (uint64_t)PAGE_BYTES);
    c->admin.entries = ADMIN_ENTRIES; c->admin.id = 0; c->io.entries = IO_ENTRIES; c->io.id = 1;
    /* Before any queue exists: an MSI-X vector is only delivered for queues created after it was enabled. */
    c->irq = block_pci_irq_msix(pci, nvme_irq, c);
    if (!controller_start(c)) goto release;
    if (admin(c, 0x06, 0, virt_to_phys(c->identify), 1, 0) < 0) goto release;   /* Identify Controller */
    char model[41]; copy_string(model, c->identify + 24, 40);
    copy_string(controller_serial, c->identify + 4, 20);
    uint8_t mdts = c->identify[77];
    c->vwc = c->identify[525] & 1; c->apsta = c->identify[265] & 1;
    memcpy(&c->rtd3e_us, c->identify + 88, 4);
    c->max_transfer = MAX_TRANSFER;
    if (mdts && mdts < 20 && (PAGE_BYTES << mdts) < c->max_transfer) c->max_transfer = PAGE_BYTES << mdts;
    uint32_t namespaces; memcpy(&namespaces, c->identify + 516, 4);
    controller_total++; controllers[c->index] = c;
    vga_print("[64] NVMe"); { char d[12]; uint_to_str(c->index, d); vga_print(d); }
    vga_print(" \""); vga_print(model); vga_print("\" version="); print_hex32(rd(regs, REG_VS));
    vga_print(" namespaces="); print_hex32(namespaces); vga_print(" mdts_bytes="); print_hex32(c->max_transfer);
    vga_print(" stride="); print_hex32(stride); vga_print(c->vwc ? " volatile-cache" : " no-volatile-cache");
    power_policy(c);
    vga_print(!c->apsta ? " no-APST\n" : c->apst_off ? " APST-off\n" : " APST-firmware\n");
    /* Active namespace list (CNS 2); fall back to probing 1..N when it is not supported. */
    uint32_t* active = (uint32_t*)(uintptr_t)(memory + 6 * (uint64_t)PAGE_BYTES);   /* first PRP page as scratch */
    if (admin(c, 0x06, 0, virt_to_phys(active), 2, 0) == 0) {
        uint32_t list[MAX_NAMESPACES]; unsigned found = 0;
        for (unsigned i = 0; i < 1024 && found < MAX_NAMESPACES && active[i]; i++) list[found++] = active[i];
        for (unsigned i = 0; i < found; i++) add_namespace(c, list[i], model);
    } else for (uint32_t nsid = 1; nsid <= namespaces && nsid <= 16; nsid++) add_namespace(c, nsid, model);
    if (!c->namespaces) { message(c, "no usable namespaces\n"); return; }
    c->irq_stats = &c->ns[0]->dev.stats;
    for (unsigned i = 0; i < c->namespaces; i++) { c->ns[i]->dev.irq = c->irq; block_register(&c->ns[i]->dev); }
    static struct pm_ops pm[MAX_CONTROLLERS]; pm[c->index] = (struct pm_ops){"nvme", nvme_pm_suspend, nvme_pm_resume, c}; pm_register_storage(&pm[c->index]);
    return;
release:
    wr(regs, REG_CC, rd(regs, REG_CC) & ~1u);
    wait_ready(c, false);
    block_irq_detach(pci, c->irq, c);
    arena_free(memory, (6 + PRP_PAGES) * (uint64_t)PAGE_BYTES);
}
/* Normal shutdown notification (CC.SHN = 01b) and wait for CSTS.SHST = 10b, bounded by RTD3E (1-10 s):
 * the drive commits its mapping tables instead of counting an unsafe shutdown. Call after the last flush. */
void nvme_shutdown_all(void) {
    for (unsigned i = 0; i < controller_total; i++) {
        struct nvme_ctrl* c = controllers[i];
        if (!c || c->shut_down || rd(c->regs, REG_CSTS) == ~0u || !(rd(c->regs, REG_CC) & 1)) continue;
        uint32_t ms = c->rtd3e_us / 1000; if (ms < 1000) ms = 1000; if (ms > 10000) ms = 10000;
        wr(c->regs, REG_CC, (rd(c->regs, REG_CC) & ~(3u << 14)) | CC_SHN_NORMAL);
        uint64_t deadline = native_clock_ns() + ms * 1000000ULL; bool done = false;
        while (!(done = (rd(c->regs, REG_CSTS) & 0xCu) == CSTS_SHST_DONE) && native_clock_ns() < deadline) __asm__ volatile("pause");
        c->shut_down = true;
        message(c, done ? "shutdown complete\n" : "shutdown notification timed out\n");
    }
}
bool nvme_probe_all(void) {
    pci_device_t* list = pci_get_devices();
    unsigned before = controller_total;
    for (int i = 0; i < pci_device_count(); i++)
        if (list[i].class_code == PCI_CLASS_STORAGE && list[i].subclass == 8 && list[i].prog_if == 2) controller_probe(&list[i]);
    return controller_total > before;
}
