/* Block-device layer: device registry, partition list, root-volume selection, 512-byte sector
 * view, DMA bounce buffer, storage IRQ dispatch and completion waits. See blockdev.h. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "../../kernel/ata.h"
#include "../../kernel/pci.h"
#include "storage_util.h"

#define BLOCK_PAGE 32              /* device slots per page; pages are allocated as slots are first used, never freed */
#define BLOCK_PAGES 16
#define BLOCK_MAX_DEVICES (BLOCK_PAGE * BLOCK_PAGES)   /* 512: disks, loop, dm and md devices together */
#define BLOCK_MAX_PARTITIONS 256        /* listed partitions (pointers) */
#define BLOCK_BOOT_PARTITIONS 64        /* the boot-time scan array */
#define BOUNCE_BYTES (128 * 1024ULL)
#define SCAN_BYTES (64 * 1024ULL)
struct device_page {   /* per slot: the device (NULL hole: released hot-plug device), its whole-disk view, its hot-plug partitions */
    struct block_device* dev[BLOCK_PAGE]; struct block_partition whole[BLOCK_PAGE]; struct block_partition* chunk[BLOCK_PAGE];
};
static struct device_page* pages[BLOCK_PAGES];   /* readers on other CPUs never see a page move or go */
#define DEV(i) (pages[(i) / BLOCK_PAGE]->dev[(i) % BLOCK_PAGE])
#define WHOLE(i) (pages[(i) / BLOCK_PAGE]->whole[(i) % BLOCK_PAGE])
#define CHUNK(i) (pages[(i) / BLOCK_PAGE]->chunk[(i) % BLOCK_PAGE])
static unsigned device_total;
static struct block_partition* partitions;                 /* boot-time scan array */
static struct block_partition* part_table[BLOCK_MAX_PARTITIONS]; /* listed partitions; NULL holes */
static unsigned partition_total;
static void block_linux_identity(struct block_device* dev);
static void partition_list(struct block_partition* p);
static void select_root(uint8_t* sector);
int block_root_wanted(struct block_partition* p);
static bool root_quiet;
bool boot_option(const char* word);
static const struct block_partition* root;
static uint8_t *bounce, *rmw;
static uint64_t tsc_hz;

static void print_dec(uint64_t value) { char text[24]; number64(value, text); vga_print(text); }
uint64_t block_ticks(void) { return native_tsc(); } /* deadlines may be checked on another CPU */
uint64_t block_tsc_hz(void) { return tsc_hz; }
void block_name_number(char* name, const char* prefix, unsigned number, const char* suffix) {
    char digits[12]; uint_to_str(number, digits);
    strcpy(name, prefix); strcat(name, digits); if (suffix) strcat(name, suffix);
}

/* ---- interrupts: storage controllers through the platform layer (irq.c) ----
 * Storage vectors are IRQ_WAKE: they may interrupt irq_wait_restricted, which lets only those (and
 * the tick) in, so NIC/input handlers never run in the middle of a filesystem operation. */
#define IRQ_SLOTS 8
struct irq_slot { void (*handler)(void*); void* context; };
static struct irq_slot irq_slots[IRQ_SLOTS];
static unsigned irq_slot_count;
static void storage_interrupt(unsigned vector, void* context) {
    (void)vector; struct irq_slot* slot = context;
    if (slot->handler) slot->handler(slot->context);
    block_io_completed();   /* a task sleeping in block_wait checks its device again */
}
static struct irq_slot* irq_slot_new(void (*handler)(void*), void* context) {
    if (!handler || irq_slot_count == IRQ_SLOTS) return NULL;
    irq_slots[irq_slot_count].handler = handler; irq_slots[irq_slot_count].context = context;
    return &irq_slots[irq_slot_count++];
}
bool block_irq_attach(unsigned line, void (*handler)(void* context), void* context) {
    if (!line || line == 2 || line >= 16) return false;
    struct irq_slot* slot = irq_slot_new(handler, context); if (!slot) return false;
    unsigned flags = IRQ_WAKE | (line == 14 ? 0 : IRQ_PCI_INTX); /* IDE: ISA edge line; PCI INTx: level */
    if (irq_attach_isa(line, flags, storage_interrupt, slot) >= 0) return true;
    irq_slot_count--; return false;
}
/* MSI, else MSI-X entry 0, else the firmware-assigned INTx line. Returns a non-zero token for
 * block_wait (the line, or 0x100 + vector), 0 when there is no interrupt (the driver polls). */
unsigned block_pci_irq(pci_device_t* pci, void (*handler)(void* context), void* context) {
    bool msi = pci_find_capability(pci, 0x05) != 0, msix = !msi && pci_msix_count(pci) > 0;
    if (msi || msix) {
        struct irq_slot* slot = irq_slot_new(handler, context);
        int vector = slot ? irq_alloc_vector(IRQ_WAKE) : -1;
        if (vector >= 0) {
            if (!irq_register((unsigned)vector, storage_interrupt, slot) &&
                !(msi ? pci_enable_msi(pci, (unsigned)vector, 0) : pci_enable_msix(pci, 0, (unsigned)vector, 0)))
                return 0x100u + (unsigned)vector;
            irq_free_vector((unsigned)vector);
        }
        if (slot) irq_slot_count--;
    }
    unsigned line = pci->irq_line;   /* firmware-assigned legacy line; 0/0xFF = not routed */
    return line && line < 16 && line != 2 && block_irq_attach(line, handler, context) ? line : 0;
}
/* NVMe: MSI-X first (the mode controllers are validated with; Linux's order), then MSI, then INTx. */
unsigned block_pci_irq_msix(pci_device_t* pci, void (*handler)(void* context), void* context) {
    if (pci_msix_count(pci) > 0) {
        struct irq_slot* slot = irq_slot_new(handler, context);
        int vector = slot ? irq_alloc_vector(IRQ_WAKE) : -1;
        if (vector >= 0) {
            if (!irq_register((unsigned)vector, storage_interrupt, slot) && !pci_enable_msix(pci, 0, (unsigned)vector, 0)) return 0x100u + (unsigned)vector;
            irq_free_vector((unsigned)vector);
        }
        if (slot) irq_slot_count--;
    }
    return block_pci_irq(pci, handler, context);
}
/* A controller that failed after attaching: its handler never runs again; MSI/MSI-X is switched off. */
void block_irq_detach(pci_device_t* pci, unsigned irq, void* context) {
    for (unsigned i = 0; i < irq_slot_count; i++) if (irq_slots[i].context == context) irq_slots[i].handler = NULL;
    if (irq < 0x100) return;
    if (pci_find_capability(pci, 0x05)) pci_disable_msi(pci);
    if (pci_msix_count(pci) > 0) pci_disable_msix(pci);
    irq_free_vector(irq - 0x100);
}
/* One step of a completion wait. Device interrupts reach only the bootstrap CPU: there it halts until
 * a storage interrupt (or the tick); application processors, and devices without an interrupt, poll. */
void block_wait(unsigned irq) {
    if (block_io_sleep()) return;   /* task context: sleep without the big lock (blockio.c) */
    if (!irq || cpu_self()->index) {
        /* Polling: pace register reads (~4 us apart). In a VM every MMIO read is an exit that takes the
         * device model's global lock, and a tight loop of them delays the very completion awaited.
         * Shootdowns aimed at this CPU are answered meanwhile (the big lock is held here). */
        uint64_t until = block_ticks() + tsc_hz / 250000;
        do { smp_tlb_poll(); __asm__ volatile("pause"); } while (block_ticks() < until);
        return;
    }
    irq_wait_restricted();
}

/* ---- memory ---- */
/* Kernel RAM (direct map or kernel image) is physically contiguous: DMA-able as is, when it lies below the
 * device's addressing limit (32-bit controllers: 4 GiB). Anything else goes through the bounce buffer. */
bool block_dma_direct(const void* buffer, uint64_t bytes, uint64_t limit) {
    uint64_t address = (uintptr_t)buffer;
    if (!bytes || (address & 3) || !memory_kernel_buffer(buffer, bytes)) return false;
    return virt_to_phys(buffer) + bytes <= limit;
}
uint8_t* block_bounce(uint64_t* bytes) { *bytes = bounce ? BOUNCE_BYTES : 0; return bounce; }
extern uint64_t boot_pd[];
volatile void* block_mmio(uint64_t physical, uint64_t bytes) {
    /* Above 4 GiB (UEFI places 64-bit BARs there): the platform's uncached kernel window. */
    if (physical >= 0x100000000ULL && bytes && physical + bytes > physical) return mmio_map(physical, bytes, MMIO_UNCACHED);
    /* The kernel's direct map covers the low 4 GiB with 2 MiB pages; mark the BAR's pages uncached. */
    if (!physical || !bytes || physical > 0x100000000ULL - bytes || physical < 0x1000000) return NULL;
    if (memory_kernel_buffer(phys_to_virt(physical), 1) || memory_kernel_buffer(phys_to_virt(physical + bytes - 1), 1)) return NULL; /* RAM, not a BAR */
    bool changed = false;
    for (uint64_t i = physical >> 21; i <= (physical + bytes - 1) >> 21; i++) {
        if (!(boot_pd[i] & 0x80) || !(boot_pd[i] & 1)) return NULL;
        if ((boot_pd[i] & 0x18) != 0x18) {
            boot_pd[i] |= 0x18; changed = true;
            __asm__ volatile("invlpg (%0)" :: "r"(phys_to_virt(i << 21)) : "memory");
        }
    }
    if (changed) smp_tlb_shootdown(0, false);
    return phys_to_virt(physical);
}

volatile void* block_pci_bar(pci_device_t* pci, unsigned index, uint64_t bytes) {
    if (index > 5 || (pci->bar[index] & 1)) return NULL;               /* I/O-space BAR */
    uint64_t base = pci->bar[index] & ~0xFull;
    if ((pci->bar[index] & 6) == 4) {                                   /* 64-bit memory BAR */
        if (index == 5) return NULL;
        base |= (uint64_t)pci->bar[index + 1] << 32;
    }
    return base ? block_mmio(base, bytes) : NULL;
}

/* ---- registry and I/O ---- */
int block_register(struct block_device* dev) {
    if (!dev || !dev->ops || (!dev->blocks && !dev->hotplug) || (dev->block_size != 512 && dev->block_size != 4096)) return -1;   /* hot-plug: an empty loop device */
    unsigned slot = 0; while (slot < device_total && DEV(slot)) slot++;
    if (slot == BLOCK_MAX_DEVICES) return -1;
    if (!pages[slot / BLOCK_PAGE]) {
        struct device_page* page = (void*)arena_alloc(sizeof(struct device_page)); if (!page) return -1;
        memset(page, 0, sizeof(*page)); pages[slot / BLOCK_PAGE] = page;
    }
    DEV(slot) = dev; if (slot == device_total) device_total++;
    if (!dev->references) dev->references = 1;   /* the driver's */
    block_linux_identity(dev);
    vga_print("[64] BLOCK "); vga_print(dev->name); vga_print(" "); vga_print(dev->kind);
    vga_print(" \""); vga_print(dev->model); vga_print("\" blocks="); print_dec(dev->blocks);
    vga_print(" bs="); print_dec(dev->block_size); vga_print(" MiB="); print_dec(dev->blocks * dev->block_size >> 20);
    vga_print(" irq="); print_dec(dev->irq); vga_print("\n");
    return 0;
}
unsigned block_device_count(void) { return device_total; }
struct block_device* block_device_get(unsigned index) { return index < device_total ? DEV(index) : NULL; }
unsigned block_partition_count(void) { return partition_total; }
const struct block_partition* block_partition_get(unsigned index) { return index < partition_total ? part_table[index] : NULL; }
const struct block_partition* block_root(void) { return root; }
static bool in_range(const struct block_device* dev, uint64_t lba, uint32_t count, const void* buffer) {
    return dev && dev->ops && !dev->removed && buffer && lba <= dev->blocks && count <= dev->blocks - lba;
}
/* Another task's request may be in flight: tasks wait, task-less contexts must not have started (blockio.c). */
static bool io_allowed(void) {
    block_io_barrier();
    if (!block_io_busy()) { block_io_request_start(); return true; }
    vga_print("[64] BLOCKIO request from a task-less context while a disk wait sleeps (refused)\n"); return false;
}
int block_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buffer) {
    if (!count) return 0;
    if (!in_range(dev, lba, count, buffer) || !io_allowed()) return -1;
    return dev->ops->read(dev, lba, count, buffer) < 0 ? -1 : 0;
}
int block_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buffer) {
    if (!count) return 0;
    if (!in_range(dev, lba, count, buffer) || !io_allowed()) return -1;
    if (dev->ops->write(dev, lba, count, buffer) < 0) { dev->flush_pending = true; return -1; }
    dev->flush_pending = true;
    return 0;
}
int block_flush(struct block_device* dev) {
    if (!dev || !dev->flush_pending) return 0;
    if (!io_allowed()) return -1;
    if (dev->ops->flush(dev) < 0) return -1;
    dev->flush_pending = false;
    return 0;
}
int block_flush_all(void) {
    int result = 0;
    for (unsigned i = 0; i < device_total; i++) if (DEV(i) && !DEV(i)->removed && block_flush(DEV(i)) < 0) result = -1;
    return result;
}

/* 512-byte sectors on any logical block size: aligned runs go straight to the device. An unaligned
 * request that fits the bounce buffer becomes one command over its covering blocks (reads), or one
 * write after reading only the partial head/tail blocks (read-modify-write); larger ones are split. */
uint64_t block_part_sectors(const struct block_partition* part) {   /* whole-disk views follow the device's size (loop, media change) */
    if (!part) return 0;
    return (!part->number && part->scheme == BLOCK_SCHEME_WHOLE ? part->dev->blocks : part->blocks) * (part->dev->block_size / 512);
}
static int part_io(const struct block_partition* part, uint64_t sector, uint32_t count, uint8_t* data, bool write) {
    if (!count) return 0;
    if (!part || !data || sector > block_part_sectors(part) || count > block_part_sectors(part) - sector) return -1;
    struct block_device* dev = part->dev;
    uint32_t per = dev->block_size / 512;
    uint64_t at = part->start * per + sector;
    if (per == 1) return write ? block_write(dev, at, count, data) : block_read(dev, at, count, data);
    uint64_t first = at / per, last = (at + count - 1) / per; uint32_t offset = (uint32_t)(at % per);
    uint32_t blocks = (uint32_t)(last - first + 1);
    if ((offset || count % per) && (uint64_t)blocks * dev->block_size <= BOUNCE_BYTES) {
        /* The bounce buffer is DMA32 RAM, so drivers DMA into it directly (no nested bounce). */
        if (!write) {
            if (block_read(dev, first, blocks, bounce) < 0) return -1;
            memcpy(data, bounce + offset * 512, (uint64_t)count * 512); return 0;
        }
        if (offset && block_read(dev, first, 1, bounce) < 0) return -1;
        if ((offset + count) % per && (last != first || !offset) &&
            block_read(dev, last, 1, bounce + (uint64_t)(blocks - 1) * dev->block_size) < 0) return -1;
        memcpy(bounce + offset * 512, data, (uint64_t)count * 512);
        return block_write(dev, first, blocks, bounce);
    }
    while (count) {
        uint64_t block = at / per; uint32_t offset = (uint32_t)(at % per);
        if (offset || count < per) {
            uint32_t n = per - offset; if (n > count) n = count;
            if (block_read(dev, block, 1, rmw) < 0) return -1;
            if (write) { memcpy(rmw + offset * 512, data, n * 512); if (block_write(dev, block, 1, rmw) < 0) return -1; }
            else memcpy(data, rmw + offset * 512, n * 512);
            at += n; count -= n; data += (uint64_t)n * 512;
            continue;
        }
        uint32_t blocks = count / per;
        if (write ? block_write(dev, block, blocks, data) < 0 : block_read(dev, block, blocks, data) < 0) return -1;
        at += (uint64_t)blocks * per; count -= blocks * per; data += (uint64_t)blocks * dev->block_size;
    }
    return 0;
}
int block_part_read(const struct block_partition* part, uint64_t sector, uint32_t count, void* buffer) { return part_io(part, sector, count, buffer, false); }
int block_part_write(const struct block_partition* part, uint64_t sector, uint32_t count, const void* buffer) { return part_io(part, sector, count, (uint8_t*)buffer, true); }

/* ---- IDE (kernel/ata.c, primary master) as a block device ---- */
static int ide_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buffer) { (void)dev; return ata_read_sectors((uint32_t)lba, count, buffer); }
static int ide_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buffer) { (void)dev; return ata_write_sectors((uint32_t)lba, count, buffer); }
static int ide_flush(struct block_device* dev) { (void)dev; return ata_flush(); }
static const struct block_ops ide_ops = {ide_read, ide_write, ide_flush};
static struct block_device ide_device;
static void ide_irq(void* context) { ((struct block_device*)context)->stats.irqs++; }
static void ide_sync_stats(void) {
    if (!ide_device.ops) return;
    struct ata_stats s; ata_get_stats(&s);
    ide_device.stats.read_cmds = s.read_cmds; ide_device.stats.read_sectors = s.read_sectors;
    ide_device.stats.write_cmds = s.write_cmds; ide_device.stats.write_sectors = s.write_sectors;
    ide_device.stats.flushes = s.flushes; ide_device.stats.busy_ticks = s.busy_ticks;
}
static uint64_t ata_dma_physical(const volatile void* pointer) { return virt_to_phys(pointer); }
static void ide_probe(void) {
    /* Legacy ports exist only behind an IDE-class (01:01) function; AHCI-only chipsets (q35) skip them. */
    if (!pci_find_class(PCI_CLASS_STORAGE, 1)) { vga_print("[64] BLOCK no IDE controller\n"); return; }
    uint64_t dma = dma32_alloc(65536); /* IDE bus-master PRDs are 32-bit */
    if (dma) { ata_set_dma_translate(ata_dma_physical); ata_set_dma_buffer((void*)(uintptr_t)dma); }
    if (!ata_init()) { if (dma) { ata_set_dma_buffer(NULL); arena_free(dma, 65536); } return; }
    struct block_device* dev = &ide_device;
    strcpy(dev->name, "ide0"); strcpy(dev->model, "ATA primary master"); dev->kind = "IDE";
    dev->block_size = 512; dev->blocks = ata_get_sectors(); dev->max_blocks = 128; dev->ops = &ide_ops;
    /* Primary IDE IRQ14 (slave PIC line 6 via cascade) wakes the hlt-based ATA waits. */
    if (block_irq_attach(14, ide_irq, dev)) dev->irq = 14;
    block_register(dev);
}

/* ---- Linux names, hot-plug devices, references (see blockdev.h) ----
 * Every disk has a Linux name and number (sdX 8:16n for SATA/IDE/USB/SCSI disks in registration order, NVMe
 * namespaces 259:n, loop 7:n and dm-N 254:n by their drivers) and a whole-disk view (number 0, the slot's page) that
 * /dev/<disk> and whole-disk mounts use. Listed partitions are what block_partition_get() iterates (root
 * selection, ext4 discovery): boot scans live in the boot array, hot-plug devices and rescans allocate a
 * chunk per device (by device slot) that lives until the device is released. */
#define HOT_PARTS 16
void (*block_event_hook)(struct block_device* dev, struct block_partition* part, const char* action);
static uint32_t sd_used;
static uint8_t ext_used[32];          /* major 259 minors 0..255 */
static uint32_t ext_minor(void) {
    for (unsigned i = 0; i < 256; i++) if (!(ext_used[i / 8] & (1U << (i % 8)))) { ext_used[i / 8] |= (uint8_t)(1U << (i % 8)); return i; }
    return 255;
}
static void ext_minor_free(uint32_t minor) { if (minor < 256) ext_used[minor / 8] &= (uint8_t)~(1U << (minor % 8)); }
static int device_slot(const struct block_device* dev) {
    for (unsigned i = 0; i < device_total; i++) if (DEV(i) == dev) return (int)i;
    return -1;
}
void block_assign_name(struct block_device* dev, const char* prefix, unsigned number, uint32_t major) {
    block_name_number(dev->linux_name, prefix, number, NULL); dev->major = major; dev->minor = number;
}
uint32_t block_sd_index(const struct block_device* dev) { return dev->major == 8 ? dev->minor / 16 : 0xFFFFFFFFU; }
static void block_linux_identity(struct block_device* dev) {
    if (!dev->linux_name[0]) {
        if (dev->kind && !strcmp(dev->kind, "NVMe")) { strcpy(dev->linux_name, dev->name); dev->major = 259; dev->minor = ext_minor(); }
        else {
            unsigned i = 0; while (i < 26 && (sd_used & (1U << i))) i++;
            if (i < 26) { sd_used |= 1U << i; strcpy(dev->linux_name, "sd"); dev->linux_name[2] = (char)('a' + i); dev->linux_name[3] = 0; dev->major = 8; dev->minor = i * 16; }
            else { strcpy(dev->linux_name, dev->name); dev->major = 259; dev->minor = ext_minor(); }
        }
    }
    if (!dev->name[0]) strcpy(dev->name, dev->linux_name);   /* hot-plug disks are named after their Linux node */
    int slot = device_slot(dev);
    if (slot < 0) return;
    struct block_partition* w = &WHOLE(slot); memset(w, 0, sizeof(*w));
    w->dev = dev; w->scheme = BLOCK_SCHEME_WHOLE; w->blocks = dev->blocks; strcpy(w->label, dev->name);
    strcpy(w->linux_name, dev->linux_name); w->major = dev->major; w->minor = dev->minor;
}
static void partition_identity(struct block_partition* p) {
    struct block_device* dev = p->dev;
    strcpy(p->linux_name, dev->linux_name);
    if (!p->number) { p->major = dev->major; p->minor = dev->minor; return; }
    unsigned n = strlen(p->linux_name);
    if (n && p->linux_name[n - 1] >= '0' && p->linux_name[n - 1] <= '9') strcat(p->linux_name, "p");
    char digits[12]; uint_to_str(p->number, digits); strcat(p->linux_name, digits);
    if (dev->major == 8 && p->number < 16) { p->major = 8; p->minor = dev->minor + p->number; }
    else { p->major = 259; p->minor = ext_minor(); }
    if (!strcmp(dev->name, dev->linux_name)) strcpy(p->label, p->linux_name);   /* "sda1", not "sdap1" */
}
static void partition_list(struct block_partition* p) {
    partition_identity(p);
    unsigned slot = 0; while (slot < partition_total && part_table[slot]) slot++;
    if (slot == BLOCK_MAX_PARTITIONS) { vga_print("[64] PART table full: "); vga_print(p->linux_name); vga_print(" not listed\n"); return; }
    part_table[slot] = p; if (slot == partition_total) partition_total++;
    p->listed = true;
}
static void partition_unlist(struct block_partition* p) {
    for (unsigned i = 0; i < partition_total; i++) if (part_table[i] == p) part_table[i] = NULL;
    if (p->listed && p->number && p->major == 259) ext_minor_free(p->minor);
    p->listed = false;
}
struct block_partition* block_whole(struct block_device* dev) { int slot = device_slot(dev); return slot < 0 ? NULL : &WHOLE(slot); }
struct block_device* block_find(const char* name) {
    for (unsigned i = 0; i < device_total; i++) if (DEV(i) && !DEV(i)->removed && !strcmp(DEV(i)->linux_name, name)) return DEV(i);
    return NULL;
}
struct block_partition* block_find_partition(const char* name) {
    for (unsigned i = 0; i < partition_total; i++) {
        struct block_partition* p = part_table[i];
        if (p && p->number && !p->dev->removed && !strcmp(p->linux_name, name)) return p;
    }
    struct block_device* dev = block_find(name);
    return dev ? block_whole(dev) : NULL;
}
unsigned block_device_partitions(struct block_device* dev, struct block_partition** out, unsigned capacity) {
    unsigned n = 0;
    for (unsigned i = 0; i < partition_total && n < capacity; i++)
        if (part_table[i] && part_table[i]->dev == dev && part_table[i]->number) out[n++] = part_table[i];
    return n;
}
static void announce(struct block_device* dev, struct block_partition* p, const char* action) { if (block_event_hook) block_event_hook(dev, p, action); }
static void print_partition(const struct block_partition* p) {
    vga_print("[64] PART "); vga_print(p->label);
    vga_print(p->scheme == BLOCK_SCHEME_GPT ? " GPT" : p->scheme == BLOCK_SCHEME_MBR ? " MBR" : " whole-disk");
    if (p->scheme == BLOCK_SCHEME_MBR) { char t[5] = {'0', 'x', "0123456789ABCDEF"[p->mbr_type >> 4], "0123456789ABCDEF"[p->mbr_type & 15], 0}; vga_print(" type="); vga_print(t); }
    if (p->name[0]) { vga_print(" name=\""); vga_print(p->name); vga_print("\""); }
    vga_print(" start="); print_dec(p->start); vga_print(" blocks="); print_dec(p->blocks);
    vga_print(" dev="); vga_print(p->linux_name); vga_print("\n");
}
/* Scan the device's partition table into a fresh chunk and list it (the caller announces). */
static int scan_hot(struct block_device* dev, int slot) {
    struct block_partition* chunk = (void*)arena_alloc(HOT_PARTS * sizeof(struct block_partition));
    uint8_t* scratch = (void*)arena_alloc(SCAN_BYTES);
    if (!chunk || !scratch) {
        if (chunk) arena_free((uint64_t)chunk, HOT_PARTS * sizeof(struct block_partition));
        if (scratch) arena_free((uint64_t)scratch, SCAN_BYTES);
        return -12;
    }
    memset(chunk, 0, HOT_PARTS * sizeof(struct block_partition));
    unsigned count = dev->blocks ? partition_scan(dev, chunk, HOT_PARTS, scratch, SCAN_BYTES) : 0;   /* an empty loop device has none */
    arena_free((uint64_t)scratch, SCAN_BYTES);
    CHUNK(slot) = chunk;
    for (unsigned i = 0; i < count; i++) {
        if (!chunk[i].number) continue;   /* superfloppy: the whole-disk view serves it */
        chunk[i].allocated = true; partition_list(&chunk[i]); print_partition(&chunk[i]);
    }
    return 0;
}
int block_hot_add(struct block_device* dev) {
    dev->hotplug = true; dev->references = 1; dev->removed = false;
    if (block_register(dev) < 0) return -28;
    int slot = device_slot(dev);
    int error = scan_hot(dev, slot);
    if (error) vga_print("[64] PART no memory for the partition scan\n");
    announce(dev, NULL, "add");
    struct block_partition* list[HOT_PARTS]; unsigned n = block_device_partitions(dev, list, HOT_PARTS);
    for (unsigned i = 0; i < n; i++) announce(dev, list[i], "add");
    return 0;
}
static bool partitions_held(struct block_device* dev) {
    struct block_partition* list[HOT_PARTS]; unsigned n = block_device_partitions(dev, list, HOT_PARTS);
    for (unsigned i = 0; i < n; i++) if (list[i]->holders || list[i]->opens) return true;
    return false;
}
/* BLKPG: add (1), delete (2) or resize (3) one partition of a disk; start/length in bytes. */
static bool overlaps(struct block_device* dev, const struct block_partition* self, uint64_t first, uint64_t blocks) {
    struct block_partition* list[HOT_PARTS]; unsigned n = block_device_partitions(dev, list, HOT_PARTS);
    for (unsigned i = 0; i < n; i++)
        if (list[i] != self && first < list[i]->start + list[i]->blocks && list[i]->start < first + blocks) return true;
    return false;
}
int partition_table_edit(struct block_device* dev, int op, int64_t start, int64_t length, int number) {
    int slot = device_slot(dev);
    if (slot < 0 || dev->removed) return -19;
    struct block_partition* list[HOT_PARTS]; unsigned n = block_device_partitions(dev, list, HOT_PARTS);
    struct block_partition* existing = NULL; uint8_t scheme = BLOCK_SCHEME_MBR;
    for (unsigned i = 0; i < n; i++) { if (list[i]->number == (unsigned)number) existing = list[i]; scheme = list[i]->scheme; }
    if (op == 2) {
        if (!existing) return -6;
        if (existing->holders || existing->opens) return -16;
        announce(dev, existing, "remove"); partition_unlist(existing);
        return 0;
    }
    uint64_t bs = dev->block_size;
    if ((uint64_t)start % bs || (uint64_t)length % bs) return -22;
    uint64_t first = (uint64_t)start / bs, blocks = (uint64_t)length / bs;
    if (!blocks || first > dev->blocks || blocks > dev->blocks - first) return -22;
    if (op == 3) {
        if (!existing) return -6;
        if (existing->start != first) return -22;
        if (overlaps(dev, existing, first, blocks)) return -16;
        existing->blocks = blocks; announce(dev, existing, "change");
        return 0;
    }
    if (existing || overlaps(dev, NULL, first, blocks)) return -16;
    if (!CHUNK(slot)) {
        CHUNK(slot) = (void*)arena_alloc(HOT_PARTS * sizeof(struct block_partition));
        if (!CHUNK(slot)) return -12;
        memset(CHUNK(slot), 0, HOT_PARTS * sizeof(struct block_partition));
    }
    struct block_partition* p = NULL;
    for (unsigned i = 0; i < HOT_PARTS && !p; i++) if (!CHUNK(slot)[i].dev || (!CHUNK(slot)[i].listed && !CHUNK(slot)[i].holders && !CHUNK(slot)[i].opens)) p = &CHUNK(slot)[i];
    if (!p) return -28;
    memset(p, 0, sizeof(*p));
    p->dev = dev; p->number = (unsigned)number; p->scheme = scheme; p->start = first; p->blocks = blocks; p->allocated = true;
    strcpy(p->label, dev->name); block_name_number(p->label + strlen(p->label), "p", (unsigned)number, NULL);
    partition_list(p); print_partition(p); announce(dev, p, "add");
    return 0;
}
static void unlist_device(struct block_device* dev) {
    struct block_partition* list[HOT_PARTS]; unsigned n = block_device_partitions(dev, list, HOT_PARTS);
    for (unsigned i = 0; i < n; i++) { announce(dev, list[i], "remove"); partition_unlist(list[i]); }
}
void block_hot_remove(struct block_device* dev) {
    if (!dev || dev->removed) return;
    dev->removed = true;
    vga_print("[64] BLOCK "); vga_print(dev->linux_name); vga_print(" removed\n");
    unlist_device(dev);
    announce(dev, NULL, "remove");
    block_put(dev);
}
int block_rescan(struct block_device* dev) {
    int slot = device_slot(dev);
    if (slot < 0 || dev->removed) return -19;
    if (partitions_held(dev)) return -16;
    unlist_device(dev);
    struct block_partition* old = CHUNK(slot); CHUNK(slot) = NULL;
    /* Boot partitions of this disk (boot array) are unlisted too; the array itself stays. */
    for (unsigned i = 0; i < partition_total; i++) if (part_table[i] && part_table[i]->dev == dev) partition_unlist(part_table[i]);
    if (old) arena_free((uint64_t)old, HOT_PARTS * sizeof(struct block_partition));
    WHOLE(slot).blocks = dev->blocks;
    int error = scan_hot(dev, slot);
    dev->generation++;
    struct block_partition* list[HOT_PARTS]; unsigned n = block_device_partitions(dev, list, HOT_PARTS);
    for (unsigned i = 0; i < n; i++) announce(dev, list[i], "add");
    announce(dev, NULL, "change");
    return error;
}
void block_get(struct block_device* dev) { if (dev) dev->references++; }
void block_put(struct block_device* dev) {
    if (!dev || !dev->references) return;
    if (dev->references == 2 && !dev->removed && dev->extra && dev->extra->idle) { dev->references--; dev->extra->idle(dev); return; }
    if (--dev->references || !dev->removed) return;
    int slot = device_slot(dev);
    if (slot >= 0) {
        if (CHUNK(slot)) { arena_free((uint64_t)CHUNK(slot), HOT_PARTS * sizeof(struct block_partition)); CHUNK(slot) = NULL; }
        memset(&WHOLE(slot), 0, sizeof(WHOLE(slot)));
        DEV(slot) = NULL;
    }
    if (dev->major == 8 && dev->minor / 16 < 26) sd_used &= ~(1U << (dev->minor / 16));
    else if (dev->major == 259) ext_minor_free(dev->minor);
    if (dev->extra && dev->extra->release) dev->extra->release(dev);
}
/* Boot: no root on the boot-time disks. Root selection is repeated while USB mass-storage devices are still
 * being probed (or for rootwait/rootdelay), driving the USB worker; bounded by wait_ns. */
void usb_drive(void);
unsigned usbstorage_pending(void);
bool block_root_late(uint64_t wait_ns) {
    if (root) return true;
    uint8_t* sector = (void*)arena_alloc(4096); if (!sector) return false;
    uint64_t start = native_clock_ns(); bool announced = false;
    root_quiet = true;
    for (;;) {
        usb_drive();
        select_root(sector);
        if (root) break;
        uint64_t now = native_clock_ns();
        if (now - start >= wait_ns || (!usbstorage_pending() && !boot_option("rootwait"))) break;
        if (!announced) { vga_print("[64] ROOT waiting for USB storage\n"); announced = true; }
        for (uint64_t until = native_clock_ns() + 20000000ULL; native_clock_ns() < until; ) { usb_drive(); __asm__ volatile("pause"); }
    }
    root_quiet = false;
    if (!root && block_device_count()) vga_print("[64] ROOT no FAT32 volume found (late disks)\n");
    arena_free((uint64_t)sector, 4096);
    return root != NULL;
}

/* ---- statistics ---- */
void block_get_stats(struct block_stats* total) {
    ide_sync_stats(); memset(total, 0, sizeof(*total));
    for (unsigned i = 0; i < device_total; i++) {
        if (!DEV(i)) continue;
        const struct block_stats* s = &DEV(i)->stats;
        total->read_cmds += s->read_cmds; total->read_sectors += s->read_sectors;
        total->write_cmds += s->write_cmds; total->write_sectors += s->write_sectors;
        total->flushes += s->flushes; total->busy_ticks += s->busy_ticks; total->irqs += s->irqs;
        total->errors += s->errors; total->resets += s->resets;
    }
}
void block_report(void) {
    ide_sync_stats();
    for (unsigned i = 0; i < device_total; i++) {
        const struct block_device* d = DEV(i); if (!d) continue;
        const struct block_stats* s = &d->stats;
        vga_print("[64] BLOCKSTAT "); vga_print(d->name); vga_print(" "); vga_print(d->kind);
        vga_print(" rd="); print_dec(s->read_cmds); vga_print(" rdsec="); print_dec(s->read_sectors);
        vga_print(" wr="); print_dec(s->write_cmds); vga_print(" wrsec="); print_dec(s->write_sectors);
        vga_print(" flush="); print_dec(s->flushes); vga_print(" irq="); print_dec(s->irqs);
        vga_print(" err="); print_dec(s->errors); vga_print(" reset="); print_dec(s->resets);
        vga_print(" busy_ms="); print_dec(tsc_hz ? s->busy_ticks * 1000 / tsc_hz : 0);
        vga_print(d->flush_pending ? " pending=1\n" : " pending=0\n");
    }
}

/* ---- boot self-test: one large read must equal the same bytes read in odd-sized pieces into a
 * dword-aligned (not page-aligned) buffer, and through the bounce path (odd address). Exercises
 * command splitting, PRP1 offsets, PRP lists (chained when a command spans >2 MiB) and bouncing. */
#define SELFTEST_BYTES (4ULL * 1024 * 1024 + 8192)
static void block_selftest(struct block_device* dev) {
    uint32_t bs = dev->block_size;
    uint64_t blocks = SELFTEST_BYTES / bs; if (blocks > dev->blocks) blocks = dev->blocks;
    uint64_t bytes = blocks * bs, size = bytes + 8192;
    uint8_t *a = (uint8_t*)arena_alloc(size), *b = (uint8_t*)arena_alloc(size);
    bool ok = a && b && block_read(dev, 0, (uint32_t)blocks, a) == 0;
    for (uint64_t at = 0; ok && at < blocks; ) {
        uint32_t piece = !at && dev->max_blocks > 1021 ? dev->max_blocks : 1021; /* first: a full-size unaligned command */
        uint32_t n = blocks - at < piece ? (uint32_t)(blocks - at) : piece;
        ok = block_read(dev, at, n, b + 4 + at * bs) == 0; at += n;
    }
    ok = ok && !memcmp(a, b + 4, bytes);
    if (ok) { memset(b, 0, size); ok = block_read(dev, 0, (uint32_t)blocks, b + 1) == 0 && !memcmp(a, b + 1, bytes); }
    if (a) arena_free((uint64_t)a, size);
    if (b) arena_free((uint64_t)b, size);
    if (!a || !b) { vga_print("[64] BLOCK SELFTEST skipped (memory)\n"); return; }
    if (!ok) { vga_print("[64] BLOCK SELFTEST "); vga_print(dev->name); vga_print("\n"); panic("block device self-test: split/unaligned/bounced reads differ"); }
    vga_print("[64] BLOCK SELFTEST "); vga_print(dev->name); vga_print(" PASS bytes="); print_dec(bytes); vga_print(" (split, unaligned PRP/PRD, bounce)\n");
}

/* ---- root volume ---- */
static bool fat32_boot_sector(const struct block_partition* part, uint8_t* sector) {
    if (block_part_sectors(part) < 64 || block_part_read(part, 0, 1, sector) < 0) return false;
    uint16_t bytes = (uint16_t)(sector[11] | sector[12] << 8), reserved = (uint16_t)(sector[14] | sector[15] << 8);
    uint16_t root_entries = (uint16_t)(sector[17] | sector[18] << 8), fat16 = (uint16_t)(sector[22] | sector[23] << 8);
    uint32_t total, fat32; memcpy(&total, sector + 32, 4); memcpy(&fat32, sector + 36, 4);
    uint8_t cluster = sector[13];
    return sector[510] == 0x55 && sector[511] == 0xAA && bytes == 512 && cluster && !(cluster & (cluster - 1)) &&
           reserved && (sector[16] == 1 || sector[16] == 2) && !root_entries && !fat16 && fat32 && total &&
           total <= block_part_sectors(part);
}
static const char* root_reason(const struct block_partition* p) {
    static const uint8_t nexus[16] = BLOCK_GUID_NEXUS_ROOT, basic[16] = BLOCK_GUID_BASIC_DATA;
    if (p->scheme == BLOCK_SCHEME_GPT) return !memcmp(p->type_guid, nexus, 16) ? "GPT NexusOS root" : !memcmp(p->type_guid, basic, 16) ? "GPT basic data" : NULL;
    if (p->scheme == BLOCK_SCHEME_MBR) return p->mbr_type == 0x0B || p->mbr_type == 0x0C ? "MBR FAT32" : NULL;
    return "whole disk";
}
static void select_root(uint8_t* sector) {
    static const uint8_t nexus[16] = BLOCK_GUID_NEXUS_ROOT;
    if (block_unlocked_root && !block_unlocked_root->dev->removed && fat32_boot_sector(block_unlocked_root, sector)) root = block_unlocked_root;   /* LUKS root (cryptroot.c) */
    for (unsigned pass = 0; pass < 4 && !root; pass++) {
        for (unsigned i = 0; i < partition_total && !root; i++) {
            const struct block_partition* p = part_table[i];
            if (!p || p->dev->removed) continue;
            const char* reason = root_reason(p);
            int wanted = block_root_wanted((struct block_partition*)p);   /* root= on the command line (fsprobe.c) */
            if (!wanted || (wanted < 0 && !reason)) continue;
            unsigned rank = wanted > 0 ? 0 : p->scheme == BLOCK_SCHEME_GPT ? (memcmp(p->type_guid, nexus, 16) ? 1 : 0) : p->scheme == BLOCK_SCHEME_MBR ? 2 : 3;
            if (rank == pass && fat32_boot_sector(p, sector)) root = p;
        }
    }
    if (!root) { if (device_total && !root_quiet) vga_print("[64] ROOT no FAT32 volume found\n"); return; }
    ((struct block_partition*)root)->holders++;   /* the root mount */
    vga_print("[64] ROOT "); vga_print(root->label); vga_print(" ("); vga_print(block_root_wanted((struct block_partition*)root) > 0 ? "root= on the command line" : root_reason(root));
    vga_print(", FAT32) on "); vga_print(root->dev->kind); vga_print(" start="); print_dec(root->start);
    vga_print(" blocks="); print_dec(root->blocks); vga_print("\n");
}

void storage_nodes_init(void);
bool block_probe(uint64_t tsc_frequency) {
    tsc_hz = tsc_frequency;
    storage_nodes_init();   /* /dev nodes and uevents for every disk (blkdev_node.c), before any is registered */
    if (!pci_device_count()) pci_init();
    bounce = (uint8_t*)dma32_alloc(BOUNCE_BYTES); rmw = (uint8_t*)dma32_alloc(4096);
    partitions = (struct block_partition*)arena_alloc(BLOCK_BOOT_PARTITIONS * sizeof(struct block_partition));
    if (!bounce || !rmw || !partitions) { vga_print("[64] BLOCK no memory for the block layer\n"); return false; }
    ide_probe();
    ahci_probe_all();
    nvme_probe_all();
    uint8_t* scratch = (uint8_t*)arena_alloc(SCAN_BYTES);
    if (!scratch) { vga_print("[64] BLOCK no memory for partition scan\n"); return false; }
    for (unsigned i = 0; i < device_total; i++) block_selftest(DEV(i));
    unsigned scanned = 0;
    for (unsigned i = 0; i < device_total && scanned < BLOCK_BOOT_PARTITIONS; i++)
        scanned += partition_scan(DEV(i), partitions + scanned, BLOCK_BOOT_PARTITIONS - scanned, scratch, SCAN_BYTES);
    for (unsigned i = 0; i < scanned; i++) partition_list(&partitions[i]);
    for (unsigned i = 0; i < partition_total; i++) {
        const struct block_partition* p = part_table[i];
        vga_print("[64] PART "); vga_print(p->label);
        vga_print(p->scheme == BLOCK_SCHEME_GPT ? " GPT" : p->scheme == BLOCK_SCHEME_MBR ? " MBR" : " whole-disk");
        if (p->scheme == BLOCK_SCHEME_MBR) { char t[5] = {'0', 'x', "0123456789ABCDEF"[p->mbr_type >> 4], "0123456789ABCDEF"[p->mbr_type & 15], 0}; vga_print(" type="); vga_print(t); }
        if (p->name[0]) { vga_print(" name=\""); vga_print(p->name); vga_print("\""); }
        vga_print(" start="); print_dec(p->start); vga_print(" blocks="); print_dec(p->blocks); vga_print("\n");
    }
    select_root(scratch);
    arena_free((uint64_t)scratch, SCAN_BYTES);
    return root != NULL;
}
