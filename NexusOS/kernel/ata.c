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

/* ATA commands */
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_IDENTIFY 0xEC

/* Status bits */
#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

static bool disk_present = false;
static uint32_t disk_size_bytes = 0;

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
static int ata_wait_bsy(void) {
    int timeout = 100000;
    while ((port_byte_in(ATA_STATUS) & ATA_SR_BSY) && --timeout > 0);
    return timeout > 0 ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * ata_wait_drq: Wait until DRQ is set (data ready)
 * -------------------------------------------------------------------------- */
static int ata_wait_drq(void) {
    int timeout = 100000;
    while (--timeout > 0) {
        uint8_t status = port_byte_in(ATA_STATUS);
        if (status & ATA_SR_ERR) return -1;
        if (status & ATA_SR_DRQ) return 0;
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * ata_init: Detect primary master ATA disk
 * -------------------------------------------------------------------------- */
bool ata_init(void) {
    disk_present = false;

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
    if (status == 0) {
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

    /* Extract total sectors (words 60-61 for 28-bit LBA) */
    uint32_t total_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    uint32_t size_mb = total_sectors / 2048;

    disk_size_bytes = total_sectors * ATA_SECTOR_SIZE;
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
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer) {
    if (!disk_present) return -1;
    uint16_t* buf = (uint16_t*)buffer;

    for (uint32_t s = 0; s < count; s++) {
        uint32_t cur_lba = lba + s;

        if (ata_wait_bsy() < 0) return -1;

        /* Select drive + LBA high bits */
        port_byte_out(ATA_DRIVE_HEAD, 0xE0 | ((cur_lba >> 24) & 0x0F));
        port_byte_out(ATA_SECTOR_CNT, 1);
        port_byte_out(ATA_LBA_LO, cur_lba & 0xFF);
        port_byte_out(ATA_LBA_MID, (cur_lba >> 8) & 0xFF);
        port_byte_out(ATA_LBA_HI, (cur_lba >> 16) & 0xFF);
        port_byte_out(ATA_COMMAND, ATA_CMD_READ);

        ata_400ns_delay();

        if (ata_wait_drq() < 0) return -1;

        /* Read 256 words (512 bytes) */
        for (int i = 0; i < 256; i++) {
            buf[s * 256 + i] = port_word_in(ATA_DATA);
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * ata_write_sectors: Write sectors using 28-bit LBA PIO
 * -------------------------------------------------------------------------- */
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
    if (!disk_present) return -1;
    const uint16_t* buf = (const uint16_t*)buffer;

    for (uint32_t s = 0; s < count; s++) {
        uint32_t cur_lba = lba + s;

        if (ata_wait_bsy() < 0) return -1;

        port_byte_out(ATA_DRIVE_HEAD, 0xE0 | ((cur_lba >> 24) & 0x0F));
        port_byte_out(ATA_SECTOR_CNT, 1);
        port_byte_out(ATA_LBA_LO, cur_lba & 0xFF);
        port_byte_out(ATA_LBA_MID, (cur_lba >> 8) & 0xFF);
        port_byte_out(ATA_LBA_HI, (cur_lba >> 16) & 0xFF);
        port_byte_out(ATA_COMMAND, ATA_CMD_WRITE);

        ata_400ns_delay();

        if (ata_wait_drq() < 0) return -1;

        /* Write 256 words (512 bytes) */
        for (int i = 0; i < 256; i++) {
            port_word_out(ATA_DATA, buf[s * 256 + i]);
        }

        /* Flush cache */
        port_byte_out(ATA_COMMAND, 0xE7);
        if (ata_wait_bsy() < 0) return -1;
    }
    return 0;
}

bool ata_disk_present(void) { return disk_present; }
uint32_t ata_get_size(void) { return disk_size_bytes; }
