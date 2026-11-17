/* Production native ATA DMA with simulated PCI/IDE/bus-master registers. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#include "../kernel/pci.h"
#define NEXUS_NATIVE64 1
#define PORT_H
static uint8_t port_byte_in(uint16_t port);
static void port_byte_out(uint16_t port, uint8_t value);
static uint16_t port_word_in(uint16_t port);
static void port_read_words(uint16_t port, void* out, size_t words);
static void port_write_words(uint16_t port, const void* in, size_t words);
static void port_dword_out(uint16_t port, uint32_t value);
#include "../kernel/ata.c"
/* The driver no longer owns a 64 KiB .bss buffer: the kernel hands it arena memory (ata_set_dma_buffer). */
static uint8_t test_dma_buffer[ATA_DMA_BUFFER_BYTES] __attribute__((aligned(512)));

#define BM 0xC040
enum fault { OK, BM_ERROR, DRIVE_ERROR, DRIVE_FAULT, NO_IRQ, STALL, EARLY_IRQ, STUCK_IRQ, STUCK_STOP, STALE_DRQ };
static uint16_t id[256], pci_command;
static uint32_t pci_status, bar;
static pci_device_t device;
static bool found, refuse_enable, pending, active_write, resetting, reject_mode, mode_timeout, mode_fault, restore_mode_fail;
static bool flush_error, flush_timeout, reset_failed;
static uint8_t regs[8], drive_status, drive_error, bm_command, bm_status, selected_mode;
static uint32_t prd_address, command_lba, command_sectors, bases[16], counts[16];
static unsigned id_at, commands, starts, stops, resets, mode_sets, flushes, bm_polls, positive_markers, prd_writes;
static unsigned fail_command;
static enum fault failure;
static uint64_t ticks, reset_started, settle_until;
static bool repeat_fault, pending_early_irq;
static unsigned char output[257 * 512 + 32], input[257 * 512];

void vga_print(const char* s) { if (!strcmp(s, "[64] ATA DMA READY PIIX3\n")) positive_markers++; }
void vga_print_color(const char* s, uint8_t color) { (void)s; (void)color; }
void uint_to_str(uint32_t n, char* s) { sprintf(s, "%u", n); }
void int_to_str(int n, char* s) { sprintf(s, "%d", n); }
static uint64_t clock_ticks(void) { ticks += 100; return ticks; }
pci_device_t* pci_find_device(uint16_t vendor, uint16_t product) {
    assert(vendor == 0x8086 && product == 0x7010); return found ? &device : NULL;
}
static void pci_address_check(uint8_t bus, uint8_t dev, uint8_t function) {
    assert(bus == 0 && dev == 1 && function == 1);
}
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t function, uint8_t offset) {
    pci_address_check(bus, dev, function); assert(offset == PCI_BAR4); return bar;
}
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t function, uint8_t offset) {
    pci_address_check(bus, dev, function); assert(offset == PCI_COMMAND); return pci_command;
}
void pci_write32(uint8_t bus, uint8_t dev, uint8_t function, uint8_t offset, uint32_t value) {
    pci_address_check(bus, dev, function); assert(offset == PCI_COMMAND && !(value >> 16));
    if (value & 4) assert(!(bm_command & 1) && !(bm_status & 7));
    pci_status &= ~(value >> 16); pci_command = (uint16_t)value;
    if (refuse_enable) pci_command &= ~4u;
}
static uint8_t expected(uint32_t lba, unsigned byte) { return (uint8_t)((lba * 37 + byte * 11) ^ 0xA5); }
static enum fault current_fault(void) {
    return commands == fail_command || (repeat_fault && commands >= fail_command) ? failure : OK;
}
static void transfer(void) {
    assert(prd_address == (uintptr_t)dma_prd && !(prd_address & 3) && (prd_address & 4095) <= 4080);
    unsigned total = 0;
    for (unsigned i = 0; i < 2; i++) {
        uint32_t n = dma_prd[i].size & 65535; if (!n) n = 65536;
        assert(!(dma_prd[i].size & 0x7FFF0001) && !(dma_prd[i].address & 1));
        assert((dma_prd[i].address & 65535) + n <= 65536);
        assert(dma_prd[i].address == (uintptr_t)dma_buffer + total && n <= ATA_DMA_BUFFER_BYTES - total);
        for (unsigned j = 0; j < n; j++) {
            uint8_t value = expected(command_lba + (total + j) / 512, (total + j) % 512);
            if (active_write) assert(dma_buffer[total + j] == value);
            else dma_buffer[total + j] = value;
        }
        total += n;
        if (dma_prd[i].size & 0x80000000u) break;
        assert(i == 0);
    }
    assert(total == command_sectors * 512);
}
static void advance_dma(void) {
    if (!pending || !(bm_command & 1) || ++bm_polls < 3) return;
    enum fault fault = current_fault();
    if (fault == STALL || fault == STUCK_STOP) return;
    if (fault == STUCK_IRQ) { bm_status |= 4; return; }
    if (fault == EARLY_IRQ && bm_polls < 6) { bm_status |= 4; pending_early_irq = true; return; }
    transfer(); pending = pending_early_irq = false; bm_status = 0x64; drive_status = 0x40;
    if (fault == BM_ERROR) { bm_status = 0x62; drive_status = 0x80; }
    if (fault == DRIVE_ERROR) { drive_status = 0x41; drive_error = 0x40; }
    if (fault == DRIVE_FAULT) drive_status = 0x60;
    if (fault == NO_IRQ) bm_status = 0x60;
    if (fault == STALE_DRQ) drive_status = 0x48;
}
static uint8_t port_byte_in(uint16_t port) {
    ticks++;
    if (port == 0x61) return 0;
    if (port == BM) return bm_command;
    if (port == BM + 2) { advance_dma(); return bm_status; }
    if (port != ATA_CONTROL) assert(!resetting && ticks >= settle_until);
    if (port == ATA_STATUS || port == ATA_CONTROL) return drive_status;
    if (port == ATA_ERROR) return drive_error;
    if (port == ATA_LBA_MID || port == ATA_LBA_HI) return regs[port - ATA_DATA];
    assert(0); return 0;
}
static void port_byte_out(uint16_t port, uint8_t value) {
    ticks++;
    if (port == 0x61 || port == 0x43 || port == 0x42) { assert(0); return; }
    if (port == BM + 2) {
        assert(!(bm_command & 1) && (value & 0x60) == (bm_status & 0x60));
        bm_status = (bm_status & 1) | (value & 0x60) | (bm_status & ~value & 6); return;
    }
    if (port == BM) {
        assert(!(value & ~9));
        if (value & 1) {
            assert(pending && !(bm_command & 1) && !(bm_status & 7) && selected_mode >= 0x20 && selected_mode <= 0x22);
            assert(!!(value & 8) != active_write && prd_address == (uintptr_t)dma_prd);
            starts++; bm_status |= 1; bm_polls = 0;
        } else {
            assert(!pending_early_irq); /* Do not abort IRQ-before-memory-drain completion. */
            stops++;
            if ((bm_status & 1) && current_fault() != STUCK_STOP) bm_status &= ~1u;
            if (bm_command & 1) pending = false;
        }
        bm_command = value; return;
    }
    if (port == ATA_CONTROL) {
        assert(!(bm_command & 1) && !(bm_status & 1));
        if (value == 4) {
            assert(!resetting); resetting = true; resets++; reset_started = ticks;
            drive_status = 0x80; pending = false; selected_mode = 0; pending_early_irq = false;
        } else {
            assert(value == 0 && resetting && ticks - reset_started >= 20);
            resetting = false; settle_until = ticks + 150000;
            drive_status = reset_failed ? 0x80 : 0x40; drive_error = 0; regs[4] = regs[5] = 0;
        }
        return;
    }
    assert(!resetting && ticks >= settle_until && !(bm_command & 1) && !(bm_status & 1));
    assert(port >= ATA_ERROR && port <= ATA_COMMAND); regs[port - ATA_DATA] = value;
    if (port != ATA_COMMAND) return;
    drive_error = 0;
    if (value == ATA_CMD_IDENTIFY) { id_at = 0; drive_status = 0x48; return; }
    if (value == 0xEF) {
        assert(regs[1] == 3 && regs[2] >= 0x20 && regs[2] <= 0x22);
        assert(id[63] & (1u << (regs[2] - 0x20)));
        mode_sets++; drive_status = 0x40; selected_mode = regs[2];
        if (reject_mode || (restore_mode_fail && resets)) { drive_status = 0x41; drive_error = 4; }
        if (mode_fault) drive_status = 0x60;
        if (mode_timeout && !resets) drive_status = 0x80;
        return;
    }
    if (value == ATA_CMD_FLUSH) {
        assert(active_write && !pending); flushes++; drive_status = flush_error ? 0x41 : 0x40;
        if (flush_error) drive_error = 0x40;
        if (flush_timeout && !resets) drive_status = 0x80;
        return;
    }
    assert(value == 0xC8 || value == 0xCA); /* Runtime DMA errors must never issue a PIO fallback. */
    assert(commands < 16 && !pending); active_write = value == 0xCA;
    command_lba = regs[3] | (uint32_t)regs[4] << 8 | (uint32_t)regs[5] << 16 | (uint32_t)(regs[6] & 15) << 24;
    command_sectors = regs[2]; assert(command_sectors && command_sectors <= 128);
    bases[commands] = command_lba; counts[commands++] = command_sectors;
    pending = true; drive_status = 0x48;
}
static uint16_t port_word_in(uint16_t port) {
    assert(port == ATA_DATA && id_at < 256 && drive_status == 0x48);
    uint16_t result = id[id_at++]; if (id_at == 256) drive_status = 0x40; return result;
}
static void port_read_words(uint16_t port, void* out, size_t words) { (void)port; (void)out; (void)words; assert(0); }
static void port_write_words(uint16_t port, const void* in, size_t words) { (void)port; (void)in; (void)words; assert(0); }
static void port_dword_out(uint16_t port, uint32_t value) {
    assert(port == BM + 4 && !(bm_command & 1) && !(bm_status & 1)); prd_address = value; prd_writes++;
}
static void reset_fixture(void) {
    disk_present = reset_needed = dma_enabled = false; dma_base = 0;
    memset(id, 0, sizeof(id)); memset(regs, 0, sizeof(regs));
    const char model[41] = "QEMU HARDDISK                            ";
    for (unsigned i = 0; i < 20; i++) id[27 + i] = (uint16_t)(uint8_t)model[i * 2] << 8 | (uint8_t)model[i * 2 + 1];
    id[49] = 0x300; id[63] = 7; id[60] = 0; id[61] = 0x1000;
    device = (pci_device_t){.bus=0,.device=1,.function=1,.class_code=1,.subclass=1,.prog_if=0x80};
    found = true; bar = BM | 1; pci_command = 0x141; pci_status = 0x3800;
    ticks = reset_started = settle_until = 0;
    commands = starts = stops = resets = mode_sets = flushes = bm_polls = positive_markers = prd_writes = 0;
    pending = active_write = resetting = refuse_enable = reject_mode = mode_timeout = mode_fault = restore_mode_fail = false;
    flush_error = flush_timeout = repeat_fault = pending_early_irq = reset_failed = false;
    drive_status = 0x40; drive_error = bm_command = selected_mode = 0; bm_status = 0x66;
    failure = OK; fail_command = 0; ata_set_clock(clock_ticks, 1000000);
}
static void ready(void) {
    reset_fixture(); assert(ata_init() && disk_present && dma_enabled && dma_base == BM);
    assert(selected_mode == 0x22 && mode_sets == 1 && positive_markers == 1);
    assert(pci_command == 0x145 && pci_status == 0x3800 && (bm_status & 0x60) == 0x60);
}
static void fill_input(uint32_t lba) {
    for (unsigned i = 0; i < sizeof(input); i++) input[i] = expected(lba + i / 512, i % 512);
}
static void check_output(uint32_t lba, unsigned sectors) {
    for (unsigned i = 0; i < sectors * 512; i++) assert(output[i] == expected(lba + i / 512, i % 512));
    for (unsigned i = sectors * 512; i < sizeof(output); i++) assert(output[i] == 0x5A);
}
int main(void) {
    ata_set_dma_buffer(test_dma_buffer);
    assert((uintptr_t)dma_buffer <= 0xFFFF0000ULL && (uintptr_t)dma_prd <= 0xFFFFFFF0ULL);
    assert(ata_dma_prds(0x120000, 65536) && dma_prd[0].size == 0x80000000u && !dma_prd[1].size);
    assert(ata_dma_prds(0x12FE00, 65536) && dma_prd[0].size == 512 && dma_prd[1].address == 0x130000 && dma_prd[1].size == (0x80000000u | 65024));
    assert(ata_dma_prds(0xFFFFFFFE, 2) && dma_prd[0].size == 0x80000002u);
    assert(!ata_dma_prds(0xFFFFFFFE, 4) && !ata_dma_prds(0x100000000ULL, 512));
    assert(!ata_dma_prds(0x100001, 512) && !ata_dma_prds(0x100000, 511) && !ata_dma_prds(0x100000, 65538) && !ata_dma_prds(0x100000, 0));
    ready();
    for (unsigned mode = 0; mode < 3; mode++) { reset_fixture(); id[63] = 1u << mode; assert(ata_init() && dma_enabled && selected_mode == 0x20 + mode); }
    for (unsigned bad = 0; bad < 9; bad++) {
        reset_fixture();
        if (bad == 0) found = false;
        if (bad == 1) device.prog_if = 0x81;
        if (bad == 2) id[27] = 0;
        if (bad == 3) id[49] &= ~0x100;
        if (bad == 4) id[63] = 0;
        if (bad == 5) bar = 0;
        if (bad == 6) bar = 0x10001;
        if (bad == 7) refuse_enable = true;
        if (bad == 8) pci_command &= ~1u;
        assert(ata_init() && !dma_enabled && !positive_markers && !starts && pci_status == 0x3800);
        if (bad == 7) assert(pci_command == 0x141);
    }
    reset_fixture(); bm_command = 9; bm_status = 0x67;
    assert(ata_dma_init(id) == 0 && dma_enabled && !(bm_command & 1) && !(bm_status & 7));
    reset_fixture(); reject_mode = true; assert(ata_init() && !dma_enabled && !reset_needed && !resets && pci_command == 0x141);
    reset_fixture(); mode_timeout = true; assert(ata_init() && !dma_enabled && !reset_needed && resets == 1 && pci_command == 0x141);
    reset_fixture(); mode_timeout = reset_failed = true;
    assert(!ata_init() && !disk_present && !dma_enabled && reset_needed && resets == 1 && !positive_markers);
    reset_fixture(); mode_fault = true; assert(!ata_init() && !disk_present && !dma_enabled);
    puts("PASS ATA DMA setup: QEMU/PIIX3/capability/BAR gates, negotiated modes, PCI/W1C preservation and recovered unsupported setup");

    ready(); memset(output, 0x5A, sizeof(output));
    assert(ata_read_sectors(0xFFFFF0, 257, output) == 0 && commands == 3 && starts == 3 && !flushes);
    assert(counts[0] == 128 && counts[1] == 128 && counts[2] == 1 && bases[1] == 0x1000070 && bases[2] == 0x10000F0);
    check_output(0xFFFFF0, 257);
    ready(); fill_input(0xFFFFF0);
    assert(ata_write_sectors(0xFFFFF0, 257, input) == 0 && commands == 3 && starts == 3 && flushes == 1);
    ready(); memset(output, 0x5A, sizeof(output)); failure = EARLY_IRQ; fail_command = 1;
    assert(ata_read_sectors(23, 1, output) == 0 && commands == 1 && !resets); check_output(23, 1);
    unsigned issued = commands;
    assert(ata_read_sectors(0x10000000, 1, output) == -1 && ata_write_sectors(0xFFFFFFF, 2, input) == -1);
    assert(ata_read_sectors(1, 1, NULL) == -1 && ata_write_sectors(1, 1, NULL) == -1);
    assert(ata_read_sectors(0x10000000, 0, NULL) == 0 && ata_write_sectors(0x10000000, 0, NULL) == 0 && commands == issued);
    puts("PASS ATA DMA transfer: PRD physical/boundary bounds, 128-sector batching/LBA carry, exact data/canaries, IRQ-before-drain and one durable flush");

    for (enum fault fault = BM_ERROR; fault <= DRIVE_FAULT; fault++) {
        ready(); memset(output, 0x5A, sizeof(output)); failure = fault; fail_command = 1;
        assert(ata_read_sectors(23, 1, output) == -1 && commands == 1 && !resets && reset_needed && !(bm_status & 1));
        for (unsigned i = 0; i < sizeof(output); i++) assert(output[i] == 0x5A);
        failure = OK;
        assert(ata_read_sectors(23, 1, output) == 0 && resets == 1 && mode_sets == 2); check_output(23, 1);
        ready(); fill_input(23); failure = fault; fail_command = 1;
        assert(ata_write_sectors(23, 1, input) == -1 && commands == 1 && !flushes && !resets);
    }
    ready(); fill_input(23); flush_error = true;
    assert(ata_write_sectors(23, 1, input) == -1 && commands == 1 && flushes == 1 && !resets);
    puts("PASS ATA DMA hard errors: bus/device/flush failures propagate without PIO fallback or same-request replay; next request resets safely");

    enum fault retryable[] = {STALL, NO_IRQ, STUCK_IRQ, STALE_DRQ};
    for (unsigned i = 0; i < sizeof(retryable) / sizeof(retryable[0]); i++) {
        ready(); memset(output, 0x5A, sizeof(output)); failure = retryable[i]; fail_command = 2;
        assert(ata_read_sectors(0xFFFFF0, 257, output) == 0 && resets == 1 && commands == 5 && mode_sets == 2);
        assert(bases[2] == 0xFFFFF0); check_output(0xFFFFF0, 257);
        ready(); fill_input(0xFFFFF0); failure = retryable[i]; fail_command = 2;
        assert(ata_write_sectors(0xFFFFF0, 257, input) == 0 && resets == 1 && commands == 5 && mode_sets == 2 && flushes == 1);
        assert(bases[2] == 0xFFFFF0);
    }
    ready(); fill_input(23); flush_timeout = true;
    assert(ata_write_sectors(23, 257, input) == 0 && resets == 1 && commands == 6 && flushes == 2);
    ready(); failure = STALL; fail_command = 1; repeat_fault = true; memset(output, 0x5A, sizeof(output));
    assert(ata_read_sectors(23, 1, output) == -1 && commands == 2 && resets == 1 && reset_needed);
    for (unsigned i = 0; i < sizeof(output); i++) assert(output[i] == 0x5A);
    ready(); failure = STUCK_STOP; fail_command = 1;
    memset(dma_buffer, 0xA5, ATA_DMA_BUFFER_BYTES);
    assert(ata_read_sectors(23, 1, output) == -1 && commands == 1 && !resets && (bm_status & 1) && prd_writes == 1);
    fill_input(23); assert(ata_write_sectors(23, 1, input) == -1 && commands == 1 && !resets && prd_writes == 1);
    for (unsigned i = 0; i < ATA_DMA_BUFFER_BYTES; i++) assert(dma_buffer[i] == 0xA5);
    ready(); failure = STALL; fail_command = 1; restore_mode_fail = true;
    assert(ata_read_sectors(23, 1, output) == -1 && commands == 1 && resets == 1 && mode_sets == 2 && reset_needed);
    puts("PASS ATA DMA timeout recovery: confirmed STOP before reset, whole-request replay, mode restore, flush replay, bounded failure and no partial read publication");
}
