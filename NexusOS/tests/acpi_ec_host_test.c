/* arch/x86_64/acpi_ec.c against a simulated ACPI embedded controller (ACPI 6.5 chapter 12: status OBF/IBF/BURST/SCI_EVT,
 * commands RD_EC 0x80, WR_EC 0x81, BE_EC 0x82 -> 0x90, BD_EC 0x83, QR_EC 0x84). The controller answers after a few status
 * polls, keeps a 256-byte address space and an event queue, can decline burst mode or stop responding (stuck IBF).
 * Time is simulated: every status read advances the clock by 1 us. Built and run by tests/test_acpi_ec_host.py. */
#define NEXUS_HOSTED 1
#include <stdio.h>
#include <string.h>
#include "../arch/x86_64/acpi_ec.c"

static int failures;
#define CHECK(test, ...) do { if (!(test)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { IDLE, ADDRESS, DATA };
static struct {
    uint8_t space[256], output, queue[8]; unsigned queued, polls, delay, phase; int command, pending_input; bool obf, ibf, burst, no_burst, stuck;
    uint64_t now; unsigned log_count; uint8_t log[64]; uint8_t address;
} ec_sim;
static void reset_ec(void) { memset(&ec_sim, 0, sizeof(ec_sim)); ec_sim.delay = 3; ec_sim.command = -1; ec_sim.pending_input = -1; }
static void put_output(uint8_t value) { ec_sim.output = value; ec_sim.obf = true; }
static void process(void) {   /* the controller consumes what the host wrote */
    if (!ec_sim.ibf || ec_sim.stuck || ++ec_sim.polls < ec_sim.delay) return;
    ec_sim.polls = 0; ec_sim.ibf = false;
    int value = ec_sim.pending_input; ec_sim.pending_input = -1;
    if (ec_sim.phase == IDLE) {   /* a command */
        ec_sim.command = value; if (ec_sim.log_count < 64) ec_sim.log[ec_sim.log_count++] = (uint8_t)value;
        switch (value) {
        case 0x80: case 0x81: ec_sim.phase = ADDRESS; break;
        case 0x82: if (!ec_sim.no_burst) { ec_sim.burst = true; put_output(0x90); } break;
        case 0x83: ec_sim.burst = false; break;
        case 0x84: put_output(ec_sim.queued ? ec_sim.queue[0] : 0); if (ec_sim.queued) { memmove(ec_sim.queue, ec_sim.queue + 1, --ec_sim.queued); } break;
        }
    } else if (ec_sim.phase == ADDRESS) {
        ec_sim.address = (uint8_t)value;
        if (ec_sim.command == 0x80) { put_output(ec_sim.space[ec_sim.address]); ec_sim.phase = IDLE; } else ec_sim.phase = DATA;
    } else { ec_sim.space[ec_sim.address] = (uint8_t)value; ec_sim.phase = IDLE; }
}
static uint8_t sim_in(uint16_t port) {
    if (port == 0x66) {
        ec_sim.now += 1000; process();
        return (uint8_t)((ec_sim.obf ? EC_OBF : 0) | (ec_sim.ibf ? EC_IBF : 0) | (ec_sim.burst ? EC_BURST : 0) | (ec_sim.queued ? EC_SCI_EVT : 0));
    }
    ec_sim.obf = false; return ec_sim.output;
}
static void sim_out(uint16_t port, uint8_t value) {
    if (ec_sim.ibf) { printf("FAIL: host wrote while IBF was set\n"); failures++; }
    ec_sim.ibf = true; ec_sim.pending_input = value;
    if (port == 0x66 && ec_sim.phase != IDLE) { printf("FAIL: command 0x%02x in the middle of a transaction\n", value); failures++; }
}
static uint64_t sim_now(void) { return ec_sim.now; }
static const struct ec_io sim_io = {sim_in, sim_out, sim_now};

int main(void) {
    struct ec_port port = {&sim_io, 0x62, 0x66, 0, 2000000};   /* 2 ms per step */
    uint8_t byte = 0; uint64_t value = 0;
    reset_ec(); ec_sim.space[0x10] = 0x5A;
    CHECK(ec_read_byte(&port, 0x10, &byte) && byte == 0x5A, "RD_EC %02x", byte);
    CHECK(ec_write_byte(&port, 0x20, 0xA5) && ec_sim.space[0x20] == 0xA5, "WR_EC");
    reset_ec(); memcpy(ec_sim.space + 0x30, "\x11\x22\x33\x44", 4);
    CHECK(ec_transfer_bytes(&port, 0x30, 4, &value, false) == 0 && value == 0x44332211, "burst read %llx", (unsigned long long)value);
    CHECK(ec_sim.log_count == 6 && ec_sim.log[0] == 0x82 && ec_sim.log[1] == 0x80 && ec_sim.log[5] == 0x83 && !ec_sim.burst, "burst framing (%u commands)", ec_sim.log_count);
    value = 0xBEEF; CHECK(ec_transfer_bytes(&port, 0x40, 2, &value, true) == 0 && ec_sim.space[0x40] == 0xEF && ec_sim.space[0x41] == 0xBE, "burst write");
    reset_ec(); ec_sim.no_burst = true; memcpy(ec_sim.space + 0x50, "\x01\x02", 2); port.errors = 0;
    CHECK(ec_transfer_bytes(&port, 0x50, 2, &value, false) == 0 && value == 0x0201, "byte-wise without burst %llx", (unsigned long long)value);
    CHECK(port.errors == 1, "the declined burst costs one timeout (%llu)", (unsigned long long)port.errors);
    reset_ec(); ec_sim.queue[0] = 0x11; ec_sim.queue[1] = 0x42; ec_sim.queued = 2;
    CHECK(ec_event_pending(&port), "SCI_EVT");
    CHECK(ec_query(&port, &byte) == 1 && byte == 0x11 && ec_query(&port, &byte) == 1 && byte == 0x42, "QR_EC order");
    CHECK(!ec_event_pending(&port) && ec_query(&port, &byte) == 0, "queue drained");
    CHECK(ec_transfer_bytes(&port, 254, 4, &value, false) == -22 && ec_transfer_bytes(&port, 0, 0, &value, false) == -22 &&
          ec_transfer_bytes(&port, 0, 9, &value, false) == -22, "range checks");
    reset_ec(); ec_sim.stuck = true; port.errors = 0; uint64_t start = ec_sim.now;
    CHECK(!ec_read_byte(&port, 0x10, &byte) && port.errors == 1, "stuck controller must fail");
    CHECK(ec_sim.now - start <= 2 * 2000000ULL + 10000, "a stuck controller costs one timeout, not a hang (%llu ns)", (unsigned long long)(ec_sim.now - start));
    CHECK(ec_transfer_bytes(&port, 0x10, 2, &value, true) == -110, "stuck transfer");
    if (failures) { printf("EC HOST TESTS FAILED: %d\n", failures); return 1; }
    printf("EC HOST TESTS PASSED\n");
    return 0;
}
