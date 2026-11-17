/* ============================================================================
 * NexusOS — ATA PIO Driver (Implementation) — Phase 19
 * ============================================================================
 * ATA PIO mode using I/O ports 0x1F0-0x1F7 (primary bus).
 * 28-bit LBA addressing. Supports up to 128GB.
 * ============================================================================ */

#include "ata.h"
#include "port.h"
#include "vga.h"
#include "string.h"
#ifdef NEXUS_NATIVE64
#include "pci.h"
#endif

/* ATA I/O ports (primary bus) */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_CONTROL     0x3F6

/* ATA commands */
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH   0xE7

/* Status bits */
#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DF       0x20
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

static bool disk_present = false;
/* Kept in sectors: sectors*512 overflows 32 bits at 4 GiB (common VM sizes). */
static uint32_t disk_sectors = 0;
static uint64_t (*ata_clock)(void);
#ifdef NEXUS_NATIVE64
static void (*ata_wait_hook)(void);
static bool flush_pending;
static struct ata_stats stats;
void ata_set_wait_hook(void (*hook)(void)) { ata_wait_hook = hook; }
void ata_get_stats(struct ata_stats* out) { *out = stats; }
static void ata_pause(void) { if (ata_wait_hook) ata_wait_hook(); else __asm__ volatile("pause"); }
#endif
static uint64_t ata_frequency;
static bool reset_needed;
#ifdef NEXUS_NATIVE64
static uint16_t dma_base;
static bool dma_enabled;
static uint8_t dma_mode;
/* 64 KiB bounce buffer below 4 GiB, supplied by the block layer (arena memory, not .bss). */
#define ATA_DMA_BUFFER_BYTES 65536u
static uint8_t* dma_buffer;
void ata_set_dma_buffer(void* buffer) { dma_buffer = buffer; }
static uint64_t (*dma_translate)(const volatile void*);
void ata_set_dma_translate(uint64_t (*translate)(const volatile void*)) { dma_translate = translate; }
static uint64_t dma_physical(const volatile void* pointer) { return dma_translate ? dma_translate(pointer) : (uintptr_t)pointer; }
static struct { uint32_t address, size; } dma_prd[2] __attribute__((aligned(16)));
#endif

void ata_set_clock(uint64_t (*read_ticks)(void), uint64_t ticks_per_second) {
    if (read_ticks && ticks_per_second && ticks_per_second <= 10000000000ULL) {
        ata_clock = read_ticks;
        ata_frequency = ticks_per_second;
    }
}

static uint64_t ata_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static bool ata_calibrate(void) {
    if (ata_clock) return true;
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1u << 4))) return false;
    /* Channel 2 measures 10 ms without changing IRQ0 or requiring interrupts. */
    uint8_t speaker = port_byte_in(0x61);
    port_byte_out(0x61, speaker & ~3u);
    port_byte_out(0x43, 0xB0);
    port_byte_out(0x42, 11932 & 255); port_byte_out(0x42, 11932 >> 8);
    uint64_t start = ata_tsc();
    port_byte_out(0x61, (speaker & ~2u) | 1u);
    uint32_t spins = 20000000;
    while (!(port_byte_in(0x61) & 0x20) && --spins) __asm__ volatile("pause");
    uint64_t elapsed = ata_tsc() - start;
    port_byte_out(0x61, speaker);
    if (!spins || elapsed < 10000 || elapsed > 100000000) return false;
    ata_set_clock(ata_tsc, elapsed * 100);
    return true;
}
static int ata_error(const char* phase, uint8_t status) {
    char number[12];
    vga_print("[ATA] "); vga_print(phase); vga_print(" status="); uint_to_str(status, number); vga_print(number);
    vga_print(" error="); uint_to_str(port_byte_in(ATA_ERROR), number); vga_print(number); vga_print("\n"); return -1;
}

/* --------------------------------------------------------------------------
 * ata_400ns_delay: Wait ~400ns by reading status port 4 times
 * -------------------------------------------------------------------------- */
static void ata_400ns_delay(void) {
    port_byte_in(0x3F6);
    port_byte_in(0x3F6);
    port_byte_in(0x3F6);
    port_byte_in(0x3F6);
}

/* --------------------------------------------------------------------------
 * ata_wait_bsy: Wait until BSY clears
 * -------------------------------------------------------------------------- */
enum ata_wait { ATA_IDLE, ATA_DATA_READY, ATA_COMPLETE, ATA_BUSY_CLEAR };
static int ata_wait_status(enum ata_wait mode, unsigned seconds) {
    uint64_t start = ata_clock(), limit = ata_frequency * seconds;
    uint32_t polls = 0; bool expired = false;
    for (;;) {
        uint8_t status = port_byte_in(ATA_STATUS);
        if (!(status & ATA_SR_BSY)) {
            if (mode == ATA_BUSY_CLEAR) return 0;
            if (mode == ATA_IDLE) { if (!(status & ATA_SR_DRQ)) return 0; }
            else {
                if (status & (ATA_SR_ERR | ATA_SR_DF)) return ata_error("command error", status);
                if ((mode == ATA_DATA_READY) == !!(status & ATA_SR_DRQ)) return 0;
            }
        }
        /* Check fresh status once more when the deadline crosses an I/O completion. */
        if (expired) { reset_needed = true; return ata_error(mode == ATA_DATA_READY ? "command timeout (data)" : mode == ATA_COMPLETE ? "command timeout (complete)" : "command timeout (idle)", status); }
        if (!(++polls & 63) && ata_clock() - start >= limit) expired = true;
#ifdef NEXUS_NATIVE64
        if (polls > 256) { ata_pause(); continue; }   /* a long wait (FLUSH CACHE, recovery): the platform hook may sleep */
#endif
        __asm__ volatile("pause");
    }
}
static int ata_wait_bsy(void) { return ata_wait_status(ATA_BUSY_CLEAR, 3); }

/* --------------------------------------------------------------------------
 * ata_wait_drq: Wait until DRQ is set (data ready)
 * -------------------------------------------------------------------------- */
static int ata_wait_drq(void) { return ata_wait_status(ATA_DATA_READY, 3); }

/* --------------------------------------------------------------------------
 * ata_wait_done: Wait for BSY and DRQ to clear, rejecting command errors.
 * -------------------------------------------------------------------------- */
static int ata_wait_done(unsigned seconds) { return ata_wait_status(ATA_COMPLETE, seconds); }

#ifdef NEXUS_NATIVE64
static void ata_dma_ack(void) {
    port_byte_out(dma_base + 2, (port_byte_in(dma_base + 2) & 0x60) | 6);
}
static int ata_dma_stop(void) {
    if (!dma_base) return 0;
    port_byte_out(dma_base, port_byte_in(dma_base) & 8);
    uint64_t start = ata_clock();
    unsigned polls = 0;
    while (port_byte_in(dma_base + 2) & 1) {
        if (!(++polls & 63) && ata_clock() - start >= ata_frequency * 3) {
            reset_needed = true; return ata_error("DMA stop timeout", port_byte_in(ATA_STATUS));
        }
        __asm__ volatile("pause");
    }
    __asm__ volatile("" ::: "memory");
    return 0;
}
static int ata_dma_select_mode(void) {
    if (ata_wait_status(ATA_IDLE, 3) < 0) return -1;
    port_byte_out(ATA_DRIVE_HEAD, 0xA0); ata_400ns_delay();
    port_byte_out(ATA_ERROR, 3); /* SET FEATURES: select transfer mode. */
    port_byte_out(ATA_SECTOR_CNT, dma_mode);
    port_byte_out(ATA_COMMAND, 0xEF); ata_400ns_delay();
    return ata_wait_done(3);
}
static bool ata_dma_prds(uint64_t address, uint32_t bytes) {
    if (!bytes || bytes > ATA_DMA_BUFFER_BYTES || (bytes & 1) || (address & 1) ||
        address > 0x100000000ULL - bytes) return false;
    uint32_t first = 65536 - ((uint32_t)address & 65535);
    if (first > bytes) first = bytes;
    dma_prd[0].address = (uint32_t)address;
    dma_prd[0].size = first & 65535;
    dma_prd[1].address = dma_prd[1].size = 0;
    if (first < bytes) {
        dma_prd[1].address = (uint32_t)(address + first);
        dma_prd[1].size = (bytes - first) | 0x80000000u;
    } else dma_prd[0].size |= 0x80000000u;
    return true;
}
#endif

static void ata_delay_us(unsigned microseconds) {
#if defined(__i386__)
    /* Reset delays are below one second; avoid a libgcc 64-bit division dependency. */
    uint64_t start = ata_clock(), scaled = ata_frequency * microseconds;
    for (;;) {
        uint64_t elapsed = ata_clock() - start;
        if (elapsed >= ata_frequency || elapsed * 1000000 >= scaled) break;
        port_byte_in(ATA_CONTROL); __asm__ volatile("pause");
    }
#else
    uint64_t start = ata_clock(), ticks = (ata_frequency * microseconds + 999999) / 1000000;
    while (ata_clock() - start < ticks) { port_byte_in(ATA_CONTROL); __asm__ volatile("pause"); }
#endif
}
static int ata_recover(void) {
    if (!reset_needed) return 0;
#ifdef NEXUS_NATIVE64
    if (ata_dma_stop() < 0) return -1;
#endif
    /* A timed-out transfer still owns the task-file registers until reset. */
    port_byte_out(ATA_CONTROL, 4); ata_delay_us(20);
    port_byte_out(ATA_CONTROL, 0); ata_delay_us(150000);
    if (ata_wait_done(30) < 0) return -1;
    uint8_t status = port_byte_in(ATA_STATUS);
    if (!(status & ATA_SR_DRDY) || port_byte_in(ATA_LBA_MID) || port_byte_in(ATA_LBA_HI)) return ata_error("reset failed", status);
    reset_needed = false;
#ifdef NEXUS_NATIVE64
    if (dma_enabled && ata_dma_select_mode() < 0) { reset_needed = true; return -1; }
#endif
    vga_print("[ATA] channel recovered\n"); return 0;
}

#ifdef NEXUS_NATIVE64
static int ata_dma_init(const uint16_t* identify) {
    char model[41];
    for (unsigned i = 0; i < 20; i++) {
        model[i * 2] = (char)(identify[27 + i] >> 8);
        model[i * 2 + 1] = (char)identify[27 + i];
    }
    model[40] = 0;
    for (unsigned i = 40; i && model[i - 1] == ' '; i--) model[i - 1] = 0;
    /* shortcut: only QEMU PIIX3 DMA is validated; physical controllers need timing setup and testing. */
    if (strcmp(model, "QEMU HARDDISK") || (identify[49] & 0x300) != 0x300 || !(identify[63] & 7)) return 0;
    pci_device_t* device = pci_find_device(0x8086, 0x7010);
    if (!device || device->class_code != 1 || device->subclass != 1 || device->prog_if != 0x80) return 0;
    uint32_t bar = pci_read32(device->bus, device->device, device->function, PCI_BAR4);
    if ((bar & 0xFFFF000F) != 1 || !(bar & 0xFFF0)) return 0;
    uint64_t buffer = dma_buffer ? dma_physical(dma_buffer) : 0, table = dma_physical(dma_prd);
    if (!buffer || buffer > 0xFFFF0000ULL || table > 0xFFFFFFF0ULL || (table & 3) ||
        (table & 4095) > 4096 - sizeof(dma_prd)) return 0;
    uint16_t command = pci_read16(device->bus, device->device, device->function, PCI_COMMAND);
    if (!(command & 1)) return 0;
    dma_base = (uint16_t)(bar & 0xFFF0);
    if (ata_dma_stop() < 0) return -1;
    ata_dma_ack();
    /* Upper-half PCI status bits are W1C: never echo them back with COMMAND. */
    pci_write32(device->bus, device->device, device->function, PCI_COMMAND, command | 4u);
    if ((pci_read16(device->bus, device->device, device->function, PCI_COMMAND) & 5) != 5) {
        pci_write32(device->bus, device->device, device->function, PCI_COMMAND, command);
        dma_base = 0; return 0;
    }
    dma_mode = (identify[63] & 4) ? 0x22 : (identify[63] & 2) ? 0x21 : 0x20;
    if (ata_dma_select_mode() < 0) {
        if (reset_needed) {
            if (ata_recover() < 0) return -1;
            vga_print("[ATA] DMA setup timeout recovered; using PIO\n");
        } else if ((port_byte_in(ATA_STATUS) & (ATA_SR_BSY | ATA_SR_DRQ | ATA_SR_DF | ATA_SR_ERR)) == ATA_SR_ERR &&
                   port_byte_in(ATA_ERROR) == 4) vga_print("[ATA] DMA setup unsupported; using PIO\n");
        else return -1;
        pci_write32(device->bus, device->device, device->function, PCI_COMMAND, command);
        dma_base = 0; return 0;
    }
    dma_enabled = true;
    vga_print("[64] ATA DMA READY PIIX3\n"); return 0;
}
#endif

/* --------------------------------------------------------------------------
 * ata_init: Detect primary master ATA disk
 * -------------------------------------------------------------------------- */
bool ata_init(void) {
    disk_present = false;
    disk_sectors = 0;
#ifdef NEXUS_NATIVE64
    if (dma_base && (ata_dma_stop() < 0 || (reset_needed && ata_recover() < 0))) return false;
    dma_enabled = false; dma_base = 0;
#endif
    reset_needed = false;
    if (!ata_calibrate()) { vga_print("[ATA] clock calibration failed\n"); return false; }

    /* Select primary master drive */
    port_byte_out(ATA_DRIVE_HEAD, 0xA0);
    ata_400ns_delay();

    /* Send IDENTIFY command */
    port_byte_out(ATA_SECTOR_CNT, 0);
    port_byte_out(ATA_LBA_LO, 0);
    port_byte_out(ATA_LBA_MID, 0);
    port_byte_out(ATA_LBA_HI, 0);
    port_byte_out(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    /* Check if drive exists */
    uint8_t status = port_byte_in(ATA_STATUS);
    if (status == 0 || status == 0xFF) { /* no drive, or a floating bus */
        vga_print_color("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("No ATA disk detected\n");
        return false;
    }

    /* Wait for BSY to clear */
    if (ata_wait_bsy() < 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("ATA disk timeout\n");
        return false;
    }

    /* Check for non-ATA device (ATAPI, SATA, etc.) */
    if (port_byte_in(ATA_LBA_MID) != 0 || port_byte_in(ATA_LBA_HI) != 0) {
        vga_print_color("[--] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("Non-ATA device on primary\n");
        return false;
    }

    /* Wait for DRQ or ERR */
    if (ata_wait_drq() < 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("ATA IDENTIFY failed\n");
        return false;
    }

    /* Read 256 words of IDENTIFY data */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = port_word_in(ATA_DATA);
    }
    ata_400ns_delay();
    if (ata_wait_done(3) < 0) return false;

    /* Extract total sectors (words 60-61 for 28-bit LBA) */
    uint32_t total_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    if (!total_sectors) return false;
    if (total_sectors > 0x10000000u) total_sectors = 0x10000000u;
#ifdef NEXUS_NATIVE64
    if (ata_dma_init(identify) < 0) return false;
#endif
    uint32_t size_mb = total_sectors / 2048;

    disk_sectors = total_sectors;
    disk_present = true;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("ATA disk: ");
    char buf[12];
    int_to_str(size_mb, buf);
    vga_print(buf);
    vga_print(" MB\n");

    return true;
}

/* --------------------------------------------------------------------------
 * ata_read_sectors: Read sectors using 28-bit LBA PIO
 * -------------------------------------------------------------------------- */
static int ata_read_once(uint32_t lba, uint32_t count, void* buffer) {
    uint16_t* buf = (uint16_t*)buffer;

    for (uint32_t s = 0; s < count;) {
        uint32_t cur_lba = lba + s;
        uint32_t batch = count - s; if (batch > 256) batch = 256;

        if (ata_wait_status(ATA_IDLE, 3) < 0) return -1;

        /* Select drive + LBA high bits */
        port_byte_out(ATA_DRIVE_HEAD, 0xE0 | ((cur_lba >> 24) & 0x0F));
        port_byte_out(ATA_SECTOR_CNT, (uint8_t)batch); /* zero encodes 256 sectors */
        port_byte_out(ATA_LBA_LO, cur_lba & 0xFF);
        port_byte_out(ATA_LBA_MID, (cur_lba >> 8) & 0xFF);
        port_byte_out(ATA_LBA_HI, (cur_lba >> 16) & 0xFF);
        port_byte_out(ATA_COMMAND, ATA_CMD_READ);

        ata_400ns_delay();

        for (uint32_t n = 0; n < batch; n++) {
            if (ata_wait_drq() < 0) return -1;
            port_read_words(ATA_DATA, buf + (size_t)(s + n) * 256, 256);
            ata_400ns_delay();
        }
        if (ata_wait_done(3) < 0) return -1;
        s += batch;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * ata_write_sectors: Write sectors using 28-bit LBA PIO
 * -------------------------------------------------------------------------- */
static int ata_write_once(uint32_t lba, uint32_t count, const void* buffer) {
    const uint16_t* buf = (const uint16_t*)buffer;

    for (uint32_t s = 0; s < count;) {
        uint32_t batch = count - s; if (batch > 256) batch = 256;
        uint32_t cur_lba = lba + s;

        if (ata_wait_status(ATA_IDLE, 3) < 0) return -1;

        port_byte_out(ATA_DRIVE_HEAD, 0xE0 | ((cur_lba >> 24) & 0x0F));
        port_byte_out(ATA_SECTOR_CNT, (uint8_t)batch);
        port_byte_out(ATA_LBA_LO, cur_lba & 0xFF);
        port_byte_out(ATA_LBA_MID, (cur_lba >> 8) & 0xFF);
        port_byte_out(ATA_LBA_HI, (cur_lba >> 16) & 0xFF);
        port_byte_out(ATA_COMMAND, ATA_CMD_WRITE);

        ata_400ns_delay();

        for (uint32_t n = 0; n < batch; n++) {
            if (ata_wait_drq() < 0) return -1;
            port_write_words(ATA_DATA, buf + (size_t)(s + n) * 256, 256);
            ata_400ns_delay();
        }
        if (ata_wait_done(3) < 0) return -1;
        s += batch;
    }

    /* Flush the write cache — only once the drive is idle (a command written
     * while BSY is ignored) — so the data is durable when we return; NPFS's
     * journal ordering relies on this. */
    port_byte_out(ATA_COMMAND, ATA_CMD_FLUSH);
    ata_400ns_delay();
    return ata_wait_done(30);
}

#ifdef NEXUS_NATIVE64
/* -2 is a hard I/O failure: return it without replay, but reset before a later request. */
static int ata_dma_once(uint32_t lba, uint32_t count, void* buffer, bool writing) {
    uint8_t* data = buffer;
    for (uint32_t done = 0; done < count;) {
        uint32_t batch = count - done; if (batch > 128) batch = 128;
        uint32_t bytes = batch * 512, current = lba + done;
        if (ata_dma_stop() < 0 || ata_wait_status(ATA_IDLE, 3) < 0) return -1;
        if (!ata_dma_prds(dma_physical(dma_buffer), bytes)) return -2;
        if (writing) memcpy(dma_buffer, data + (size_t)done * 512, bytes);
        ata_dma_ack();
        __asm__ volatile("" ::: "memory");
        port_dword_out(dma_base + 4, (uint32_t)dma_physical(dma_prd));
        uint8_t direction = writing ? 0 : 8;
        port_byte_out(dma_base, direction);
        port_byte_out(ATA_DRIVE_HEAD, 0xE0 | ((current >> 24) & 15)); ata_400ns_delay();
        port_byte_out(ATA_SECTOR_CNT, (uint8_t)batch);
        port_byte_out(ATA_LBA_LO, current & 255);
        port_byte_out(ATA_LBA_MID, (current >> 8) & 255);
        port_byte_out(ATA_LBA_HI, (current >> 16) & 255);
        port_byte_out(ATA_COMMAND, writing ? 0xCA : 0xC8);
        port_byte_out(dma_base, direction | 1);
        uint64_t start = ata_clock();
        unsigned polls = 0; bool expired = false;
        uint8_t status;
        for (;;) {
            status = port_byte_in(dma_base + 2);
            if ((status & 2) || (status & 5) == 4 || expired) break;
            if (!(++polls & 63) && ata_clock() - start >= ata_frequency * 3) expired = true;
            ata_pause();
        }
        stats.busy_ticks += ata_clock() - start;
        if (writing) { stats.write_cmds++; stats.write_sectors += batch; } else { stats.read_cmds++; stats.read_sectors += batch; }
        if (ata_dma_stop() < 0) return (status & 2) ? -2 : -1;
        uint8_t final = port_byte_in(dma_base + 2), drive = port_byte_in(ATA_STATUS);
        ata_dma_ack();
        if ((status | final) & 2 || (!(drive & ATA_SR_BSY) && (drive & (ATA_SR_ERR | ATA_SR_DF)))) {
            reset_needed = true; ata_error("DMA command error", drive); return -2;
        }
        if ((status & 5) != 4) {
            reset_needed = true; return ata_error("command timeout (DMA)", drive);
        }
        if (ata_wait_done(3) < 0) {
            if (reset_needed) return -1;
            reset_needed = true; return -2;
        }
        __asm__ volatile("" ::: "memory");
        if (!writing) memcpy(data + (size_t)done * 512, dma_buffer, bytes);
        done += batch;
    }
    if (writing) flush_pending = true;
    return 0;
}
int ata_flush(void) {
    if (!disk_present || !flush_pending) return 0;
    if (ata_recover() < 0) return -1;
    uint64_t start = ata_clock();
    port_byte_out(ATA_COMMAND, ATA_CMD_FLUSH); ata_400ns_delay();
    int result = ata_wait_done(30);
    stats.busy_ticks += ata_clock() - start; stats.flushes++;
    if (!result) flush_pending = false;
    return result;
}
#endif

int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    if (!disk_present || lba > disk_sectors || count > disk_sectors - lba || (count && !buffer)) return -1;
    if (!count) return 0;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (ata_recover() < 0) return -1;
        int result;
#ifdef NEXUS_NATIVE64
        result = dma_enabled ? ata_dma_once(lba, count, buffer, false) : ata_read_once(lba, count, buffer);
#else
        result = ata_read_once(lba, count, buffer);
#endif
        if (!result) return 0;
        if (result == -2 || !reset_needed) break;
    }
    return -1;
}
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    if (!disk_present || lba > disk_sectors || count > disk_sectors - lba || (count && !buffer)) return -1;
    if (!count) return 0;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (ata_recover() < 0) return -1;
        /* A reset may discard unflushed data: replay the entire request, including its flush. */
        int result;
#ifdef NEXUS_NATIVE64
        result = dma_enabled ? ata_dma_once(lba, count, (void*)buffer, true) : ata_write_once(lba, count, buffer);
#else
        result = ata_write_once(lba, count, buffer);
#endif
        if (!result) return 0;
        if (result == -2 || !reset_needed) break;
    }
    return -1;
}

bool ata_disk_present(void) { return disk_present; }
uint32_t ata_get_sectors(void) { return disk_sectors; }
uint32_t ata_get_size(void) {
    return disk_sectors >= 0x800000u ? 0xFFFFFFFFu : disk_sectors * ATA_SECTOR_SIZE;
}
