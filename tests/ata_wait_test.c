/* Compile the production driver with simulated ports; no privileged host I/O. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#define PORT_H
static uint64_t polls, ready_after;
static uint8_t waiting, ready;
static unsigned commands;
static uint8_t port_byte_in(uint16_t port) {
    if (port == 0x1F7) return ++polls < ready_after ? waiting : ready;
    return 0;
}
static void port_byte_out(uint16_t port, uint8_t value) { (void)value; if (port == 0x1F7) commands++; }
static uint16_t port_word_in(uint16_t port) { (void)port; return 0; }
static void port_word_out(uint16_t port, uint16_t value) { (void)port; (void)value; }
static void port_read_words(uint16_t port, void* buffer, size_t words) {
    uint16_t* data = buffer; for (size_t i = 0; i < words; i++) data[i] = port_word_in(port);
}
static void port_write_words(uint16_t port, const void* buffer, size_t words) {
    const uint16_t* data = buffer; for (size_t i = 0; i < words; i++) port_word_out(port, data[i]);
}
#include "../kernel/ata.c"
void vga_print(const char* s) { (void)s; }
void vga_print_color(const char* s, uint8_t color) { (void)s; (void)color; }
void uint_to_str(uint32_t n, char* s) { sprintf(s, "%u", n); }
void int_to_str(int n, char* s) { sprintf(s, "%d", n); }
static uint64_t clock_ticks(void) { return polls; }
static void device(uint8_t initial, uint8_t final, uint64_t delay) {
    waiting = initial; ready = final; ready_after = delay; polls = 0;
}
int main(void) {
    ata_set_clock(clock_ticks, 1000000);
    device(ATA_SR_BSY | ATA_SR_ERR, ATA_SR_DRQ, 250000);
    assert(ata_wait_drq() == 0 && polls == 250000);
    device(ATA_SR_BSY, 0, 250000);
    assert(ata_wait_bsy() == 0 && polls == 250000);
    device(ATA_SR_DRQ, 0, 250000);
    assert(ata_wait_done(3) == 0 && polls == 250000);
    device(ATA_SR_DRQ, 0x40, 250000);
    assert(ata_wait_status(ATA_IDLE, 3) == 0 && polls == 250000);
    device(ATA_SR_BSY, ATA_SR_DRQ, 250000);
    assert(ata_wait_bsy() == 0 && polls == 250000);
    device(ATA_SR_BSY, ATA_SR_DRQ, 3000001);
    assert(ata_wait_drq() == 0 && polls == 3000001);
    device(ATA_SR_BSY, ATA_SR_BSY, 0);
    assert(ata_wait_drq() == -1 && polls >= 3000000 && polls < 3000100);
    device(ATA_SR_DRQ, ATA_SR_DRQ, 0);
    assert(ata_wait_done(3) == -1 && polls >= 3000000 && polls < 3000100);
    device(ATA_SR_BSY, 0, 5000000);
    assert(ata_wait_done(30) == 0 && polls == 5000000);
    device(0, ATA_SR_ERR, 0); assert(ata_wait_drq() == -1 && polls < 10);
    device(0, ATA_SR_DF, 0); assert(ata_wait_done(3) == -1 && polls < 10);
    device(0, ATA_SR_ERR, 0); assert(ata_wait_bsy() == 0);
    disk_present = true; disk_sectors = 100;
    uint16_t buffer[256];
    assert(ata_read_sectors(100, 1, buffer) == -1);
    assert(ata_write_sectors(99, 2, buffer) == -1);
    assert(ata_read_sectors(0xFFFFFFFF, 2, buffer) == -1);
    assert(ata_read_sectors(0, 1, NULL) == -1);
    assert(ata_write_sectors(0, 1, NULL) == -1);
    assert(ata_read_sectors(100, 0, NULL) == 0);
    assert(ata_write_sectors(100, 0, NULL) == 0 && commands == 0);
    puts("PASS ATA slow/busy/error/data-completion/flush deadlines and range guards");
}
