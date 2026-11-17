/* ACPI embedded controller byte protocol (acpi_ec.h). Every step waits for the status register with a deadline: a
 * controller that stops answering costs at most timeout_ns per byte and counts an error, it never hangs the caller. */
#include "acpi_ec.h"

static bool wait_status(struct ec_port* ec, uint8_t mask, uint8_t want) {
    uint64_t deadline = ec->io->now_ns() + (ec->timeout_ns ? ec->timeout_ns : 500000000ULL);   /* 1 ms per byte is typical */
    while ((ec->io->in(ec->command) & mask) != want) {
        if (ec->io->now_ns() > deadline) { ec->errors++; return false; }
        __asm__ volatile("pause");
    }
    return true;
}
static bool command(struct ec_port* ec, uint8_t value) { if (!wait_status(ec, EC_IBF, 0)) return false; ec->io->out(ec->command, value); return true; }
static bool write_data(struct ec_port* ec, uint8_t value) { if (!wait_status(ec, EC_IBF, 0)) return false; ec->io->out(ec->data, value); return true; }
static bool read_data(struct ec_port* ec, uint8_t* value) { if (!wait_status(ec, EC_OBF, EC_OBF)) return false; *value = ec->io->in(ec->data); return true; }
bool ec_read_byte(struct ec_port* ec, uint8_t address, uint8_t* value) { return command(ec, 0x80) && write_data(ec, address) && read_data(ec, value); }
bool ec_write_byte(struct ec_port* ec, uint8_t address, uint8_t value) {
    return command(ec, 0x81) && write_data(ec, address) && write_data(ec, value) && wait_status(ec, EC_IBF, 0);
}
bool ec_burst(struct ec_port* ec, bool enable) {
    if (!enable) return command(ec, 0x83) && wait_status(ec, EC_IBF, 0);
    uint8_t ack; return command(ec, 0x82) && read_data(ec, &ack) && ack == 0x90;
}
int ec_transfer_bytes(struct ec_port* ec, uint8_t address, unsigned width, uint64_t* value, bool write) {
    if (width < 1 || width > 8 || (unsigned)address + width > 256) return -22;
    bool burst = width > 1 && ec_burst(ec, true), ok = true;
    if (width > 1 && !burst) {   /* no burst: drain a stray acknowledge, byte transactions still work */
        if (ec->io->in(ec->command) & EC_OBF) (void)ec->io->in(ec->data);
        wait_status(ec, EC_IBF, 0);
    }
    uint64_t result = 0;
    for (unsigned i = 0; i < width && ok; i++) {
        uint8_t byte = write ? (uint8_t)(*value >> (8 * i)) : 0;
        ok = write ? ec_write_byte(ec, (uint8_t)(address + i), byte) : ec_read_byte(ec, (uint8_t)(address + i), &byte);
        result |= (uint64_t)byte << (8 * i);
    }
    if (burst) ec_burst(ec, false);
    if (!write && ok) *value = result;
    return ok ? 0 : -110;
}
bool ec_event_pending(struct ec_port* ec) { return (ec->io->in(ec->command) & EC_SCI_EVT) != 0; }
int ec_query(struct ec_port* ec, uint8_t* value) {
    if (!command(ec, 0x84) || !read_data(ec, value)) return -110;
    return *value ? 1 : 0;
}
