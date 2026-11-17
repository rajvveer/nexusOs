/* Simulated PIO sector transfers, including DRQ handshakes and durable flush. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#define PORT_H
static uint8_t registers[8];
static uint32_t bases[16], amounts[16], active_lba, remaining, at, transferred, fail_sector, stall_after;
static unsigned commands, busy, flushes, resets;
static uint64_t polls;
static uint64_t reset_tick, settle_until, clock_hz;
static bool completion_error, writing, flushing, flush_error, stalled, resetting, always_stall, stall_flush, reset_failed;
static uint8_t status(void) {
    if (stalled || resetting || (reset_failed && resets)) return 0x80;
    if (busy) { busy--; return 0x80; }
    if (remaining && transferred / 256 == fail_sector) return 0x41;
    return remaining ? 0x48 : (completion_error && commands) || (flush_error && flushing) ? 0x41 : 0x40;
}
static uint8_t port_byte_in(uint16_t port) {
    if (port != 0x3F6) assert(!resetting && polls >= settle_until);
    if (port == 0x1F7 || port == 0x3F6) { polls++; return status(); }
    return port == 0x1F1 ? 4 : 0;
}
static void port_byte_out(uint16_t port, uint8_t value) {
    if (port == 0x3F6) {
        if (value == 4) { assert(!resetting); resetting = true; resets++; reset_tick = polls; remaining = busy = 0; stalled = flushing = false; registers[4] = registers[5] = 0; }
        else {
            assert(value == 0 && resetting && (polls - reset_tick) * 1000000 >= clock_hz * 20);
            resetting = false; busy = 7;
            settle_until = polls + (clock_hz * 150000 + 999999) / 1000000;
        }
        return;
    }
    assert(!resetting && polls >= settle_until);
    assert(port >= 0x1F0 && port <= 0x1F7); registers[port - 0x1F0] = value;
    if (port != 0x1F7) return;
    if (value == 0xE7) { assert(writing && !remaining && !busy); flushes++; flushing = true; busy = 7; if (stall_flush && !resets) stalled = true; return; }
    assert((value == 0x20 || value == 0x30) && !remaining && commands < 16); writing = value == 0x30;
    active_lba = registers[3] | (uint32_t)registers[4] << 8 | (uint32_t)registers[5] << 16 | (uint32_t)(registers[6] & 15) << 24;
    unsigned sectors = registers[2] ? registers[2] : 256;
    bases[commands] = active_lba; amounts[commands++] = sectors;
    remaining = sectors * 256; at = 0; busy = 7;
}
static uint16_t value_at(uint32_t lba, unsigned word) { return (uint16_t)((lba * 257u + word) ^ 0xA55Au); }
static uint16_t port_word_in(uint16_t port) {
    assert(!resetting && polls >= settle_until);
    assert(port == 0x1F0 && remaining && !busy && transferred / 256 != fail_sector);
    uint16_t result = value_at(active_lba + at / 256, at % 256);
    at++; transferred++; remaining--;
    if ((always_stall ? at : transferred) == stall_after && (!resets || always_stall)) stalled = true;
    if (!(at % 256)) busy = 7;
    return result;
}
static void port_word_out(uint16_t port, uint16_t value) {
    assert(!resetting && polls >= settle_until);
    assert(port == 0x1F0 && writing && remaining && !busy && transferred / 256 != fail_sector);
    assert(value == value_at(active_lba + at / 256, at % 256));
    at++; transferred++; remaining--; if (!(at % 256)) busy = 7;
    if ((always_stall ? at : transferred) == stall_after && (!resets || always_stall)) stalled = true;
}
static unsigned read_blocks, write_blocks;
static void port_read_words(uint16_t port, void* buffer, size_t words) {
    assert(words == 256 && !(at % 256)); read_blocks++;
    uint16_t* data = buffer; for (size_t i = 0; i < words; i++) data[i] = port_word_in(port);
}
static void port_write_words(uint16_t port, const void* buffer, size_t words) {
    assert(words == 256 && !(at % 256)); write_blocks++;
    const uint16_t* data = buffer; for (size_t i = 0; i < words; i++) port_word_out(port, data[i]);
}
#include "../kernel/ata.c"
void vga_print(const char* s) { (void)s; }
void vga_print_color(const char* s, uint8_t color) { (void)s; (void)color; }
void uint_to_str(uint32_t n, char* s) { sprintf(s, "%u", n); }
void int_to_str(int n, char* s) { sprintf(s, "%d", n); }
static uint64_t clock_ticks(void) { return polls; }
static void reset(void) {
    commands = busy = remaining = at = transferred = flushes = resets = read_blocks = write_blocks = 0; polls = settle_until = 0;
    fail_sector = 0xFFFFFFFF; completion_error = writing = flushing = flush_error = false;
    stall_after = 0xFFFFFFFF; stalled = resetting = always_stall = stall_flush = reset_failed = reset_needed = false;
}
static uint16_t buffer[513 * 256];
int main(void) {
    ata_set_clock(clock_ticks, clock_hz = 1000000); disk_present = true; disk_sectors = 0x10000000;
    reset(); assert(ata_read_sectors(0xFFFFF0, 513, buffer) == 0 && commands == 3);
    assert(read_blocks == 513 && !write_blocks);
    assert(amounts[0] == 256 && amounts[1] == 256 && amounts[2] == 1);
    assert(bases[0] == 0xFFFFF0 && bases[1] == 0x10000F0 && bases[2] == 0x10001F0);
    for (unsigned i = 0; i < 513 * 256; i++) assert(buffer[i] == value_at(0xFFFFF0 + i / 256, i % 256));
    reset(); assert(ata_read_sectors(8, 8, buffer) == 0 && commands == 1 && amounts[0] == 8);
    reset(); fail_sector = 2;
    assert(ata_read_sectors(8, 8, buffer) == -1 && commands == 1 && transferred == 2 * 256);
    reset(); fail_sector = 257;
    assert(ata_read_sectors(8, 513, buffer) == -1 && commands == 2 && transferred == 257 * 256);
    reset(); completion_error = true;
    assert(ata_read_sectors(8, 8, buffer) == -1 && commands == 1 && transferred == 8 * 256);
    reset(); assert(ata_read_sectors(0xFFFFFFF, 1, buffer) == 0 && commands == 1);
    reset(); assert(ata_read_sectors(0xFFFFFFF, 2, buffer) == -1 && commands == 0);
    assert(ata_read_sectors(0x10000000, 0, NULL) == 0 && commands == 0);
    for (unsigned i = 0; i < 513 * 256; i++) buffer[i] = value_at(0xFFFFF0 + i / 256, i % 256);
    reset(); assert(ata_write_sectors(0xFFFFF0, 513, buffer) == 0 && commands == 3 && flushes == 1);
    assert(write_blocks == 513 && !read_blocks);
    assert(amounts[0] == 256 && amounts[1] == 256 && amounts[2] == 1);
    assert(bases[0] == 0xFFFFF0 && bases[1] == 0x10000F0 && bases[2] == 0x10001F0);
    reset(); fail_sector = 2; assert(ata_write_sectors(0xFFFFF0, 8, buffer) == -1 && transferred == 2 * 256 && !flushes);
    reset(); fail_sector = 257; assert(ata_write_sectors(0xFFFFF0, 513, buffer) == -1 && commands == 2 && transferred == 257 * 256 && !flushes);
    reset(); completion_error = true; assert(ata_write_sectors(0xFFFFF0, 8, buffer) == -1 && transferred == 8 * 256 && !flushes);
    reset(); flush_error = true; assert(ata_write_sectors(0xFFFFF0, 8, buffer) == -1 && transferred == 8 * 256 && flushes == 1);
    reset(); assert(ata_write_sectors(0xFFFFFFF, 2, buffer) == -1 && !commands && !flushes);
    assert(ata_write_sectors(0x10000000, 0, NULL) == 0 && !commands && !flushes);
    assert(ata_write_sectors(0, 1, NULL) == -1 && !commands && !flushes);
    ata_set_clock(clock_ticks, clock_hz = 10000);
    reset(); stall_after = 257 * 256;
    assert(ata_read_sectors(0xFFFFF0, 513, buffer) == 0 && resets == 1 && commands == 5);
    assert(bases[2] == 0xFFFFF0 && amounts[2] == 256);
    for (unsigned i = 0; i < 513 * 256; i++) assert(buffer[i] == value_at(0xFFFFF0 + i / 256, i % 256));
    reset(); stall_after = 257 * 256;
    assert(ata_write_sectors(0xFFFFF0, 513, buffer) == 0 && resets == 1 && commands == 5 && flushes == 1);
    assert(bases[2] == 0xFFFFF0 && amounts[2] == 256);
    reset(); stall_flush = true;
    assert(ata_write_sectors(0xFFFFF0, 513, buffer) == 0 && resets == 1 && commands == 6 && flushes == 2);
    assert(bases[3] == 0xFFFFF0 && amounts[3] == 256);
    reset(); remaining = 256;
    assert(ata_read_sectors(0xFFFFF0, 1, buffer) == 0 && resets == 1 && commands == 1);
    reset(); stall_after = 256; always_stall = true;
    assert(ata_read_sectors(0xFFFFF0, 8, buffer) == -1 && resets == 1 && commands == 2 && reset_needed);
    always_stall = false;
    assert(ata_read_sectors(0xFFFFF0, 1, buffer) == 0 && resets == 2 && !reset_needed);
    reset(); stall_after = 256; reset_failed = true;
    assert(ata_read_sectors(0xFFFFF0, 8, buffer) == -1 && resets == 1 && commands == 1);
    reset(); fail_sector = 2;
    assert(ata_read_sectors(0xFFFFF0, 8, buffer) == -1 && !resets && commands == 1);
    const unsigned frequencies[] = {1000000, 1234567};
    for (unsigned i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); i++) {
        ata_set_clock(clock_ticks, clock_hz = frequencies[i]);
        reset(); reset_needed = true;
        assert(ata_read_sectors(0xFFFFF0, 1, buffer) == 0 && resets == 1 && commands == 1 && !reset_needed);
    }
    puts("PASS ATA multi-sector reads: 256-sector batches, per-sector DRQ, LBA carry, mid-command and completion errors");
    puts("PASS ATA multi-sector writes: data/batch/LBA/DRQ checks, partial/completion/flush failures and one durable flush per request");
    puts("PASS ATA timeout recovery: reset timing, whole-request read/write/flush replay, stale DRQ, bounded retries, reset failure and unchanged hard-error propagation");
}
