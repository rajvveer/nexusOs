/* Boot-time proofs for the MSI/MSI-X/MMIO-window API (irq.c), run only when the QEMU test devices
 * are present (`-device edu`, `-device virtio-rng-pci`). Each test drives a real interrupt through
 * irq_alloc_vector + irq_register + pci_enable_msi/msix and waits for the handler, then returns
 * every resource so the kernel's page accounting stays exact. */
#include "native.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

static void print_hex(uint64_t value) {
    char text[19]; text[0] = '0'; text[1] = 'x';
    unsigned digits = 1; while (digits < 16 && (value >> (4 * digits))) digits++;
    for (unsigned i = 0; i < digits; i++) text[2 + i] = "0123456789ABCDEF"[(value >> (4 * (digits - 1 - i))) & 15];
    text[2 + digits] = 0; vga_print(text);
}
static bool wait_count(volatile unsigned* counter, unsigned target) {
    uint64_t deadline = native_clock_ns() + 2000000000ULL;
    while (*counter < target && native_clock_ns() < deadline) __asm__ volatile("sti; hlt; cli" ::: "memory");
    return *counter >= target;
}
static void failed(const char* what) { vga_print("[64] FAIL: "); vga_print(what); vga_print("\n"); panic(what); }

/* ---- MSI: QEMU "edu" (1234:11e8). 0x60 raises an interrupt, 0x24 reads the cause, 0x64 acks. ---- */
static volatile unsigned edu_hits, edu_vector; static volatile uint32_t edu_cause;
static void edu_interrupt(unsigned vector, void* context) {
    volatile uint32_t* regs = context;
    uint32_t cause = regs[0x24 / 4];
    if (!cause) return;
    regs[0x64 / 4] = cause;
    edu_cause |= cause; edu_vector = vector; edu_hits++;
}
static void msi_test(void) {
    pci_device_t* d = pci_find_device(0x1234, 0x11E8);
    if (!d) return;
    uint64_t bar = pci_bar_address(d, 0);
    volatile uint32_t* regs = bar ? (volatile uint32_t*)mmio_map(bar, 0x1000, MMIO_UNCACHED) : NULL;
    if (!regs || (regs[0] & 0xFF) != 0xED) failed("edu device registers");
    regs[1] = 0x12345678; if (regs[1] != ~0x12345678U) failed("edu liveness register");
    int vector = irq_alloc_vector(0);
    if (vector < 0 || irq_register((unsigned)vector, edu_interrupt, (void*)regs)) failed("MSI vector allocation");
    if (pci_enable_msi(d, (unsigned)vector, 0)) failed("pci_enable_msi");
    if (!(pci_read16(d->bus, d->device, d->function, PCI_COMMAND) & 0x400)) failed("MSI left INTx enabled");
    regs[0x60 / 4] = 0x5A;
    if (!wait_count(&edu_hits, 1) || edu_vector != (unsigned)vector || !(edu_cause & 0x5A)) failed("edu MSI not delivered");
    regs[0x60 / 4] = 0x100;
    if (!wait_count(&edu_hits, 2) || !(edu_cause & 0x100)) failed("second edu MSI not delivered");
    pci_disable_msi(d); irq_free_vector((unsigned)vector);
    vga_print("[64] PASS PCI MSI: edu messages delivered to vector "); print_hex((unsigned)vector);
    vga_print(irq_ioapic_mode() ? " (IOAPIC mode)\n" : " (PIC mode)\n");
}

/* ---- MSI-X: virtio-rng (1af4:1005) through its legacy I/O interface; MSI-X table in BAR1. ---- */
struct virtq_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
static volatile unsigned rng_hits, rng_vector;
static void rng_interrupt(unsigned vector, void* context) { (void)context; rng_vector = vector; rng_hits++; }
static void window_check(pci_device_t* d) {
    /* The modern virtio common configuration usually sits in a 64-bit BAR: under OVMF above 4 GiB. */
    for (uint8_t at = pci_read8(d->bus, d->device, d->function, 0x34) & 0xFC, guard = 0; at >= 0x40 && guard < 48; guard++) {
        if (pci_read8(d->bus, d->device, d->function, at) == 0x09 && pci_read8(d->bus, d->device, d->function, at + 3) == 1) {
            unsigned bar = pci_read8(d->bus, d->device, d->function, at + 4);
            uint32_t offset = pci_read32(d->bus, d->device, d->function, at + 8);
            uint64_t base = pci_bar_address(d, bar);
            if (!base) failed("virtio common configuration BAR");
            volatile uint8_t* common = (volatile uint8_t*)mmio_map(base + offset, 0x38, MMIO_UNCACHED);
            if (!common || *(volatile uint16_t*)(common + 0x12) != 1) failed("MMIO window: virtio common configuration");
            vga_print("[64] PASS MMIO window: virtio common config phys="); print_hex(base + offset);
            vga_print(" virt="); print_hex((uint64_t)common);
            vga_print(base + offset >= 0x100000000ULL ? " (above 4 GiB, kernel window) num_queues=1\n" : " (direct map) num_queues=1\n");
            return;
        }
        at = pci_read8(d->bus, d->device, d->function, at + 1) & 0xFC;
    }
}
static void msix_test(void) {
    pci_device_t* d = pci_find_device(0x1AF4, 0x1005);
    if (!d) return;
    uint32_t bar0 = pci_read32(d->bus, d->device, d->function, PCI_BAR0);
    if (!(bar0 & 1)) failed("virtio-rng legacy I/O BAR");
    uint16_t io = (uint16_t)(bar0 & ~3U);
    uint16_t command = pci_read16(d->bus, d->device, d->function, PCI_COMMAND);
    pci_write32(d->bus, d->device, d->function, PCI_COMMAND, (uint32_t)(command | 1));
    int entries = pci_msix_count(d);
    if (entries < 1) failed("virtio-rng MSI-X capability");
    port_byte_out(io + 0x12, 0); port_byte_out(io + 0x12, 1); port_byte_out(io + 0x12, 3);
    (void)port_dword_in(io + 0x00); port_dword_out(io + 0x04, 0);
    port_word_out(io + 0x0E, 0);
    unsigned size = port_word_in(io + 0x0C);
    if (!size || size > 1024 || (size & (size - 1))) failed("virtio-rng queue size");
    uint64_t avail_offset = 16ULL * size, used_offset = (avail_offset + 6 + 2ULL * size + 4095) & ~4095ULL;
    uint64_t ring_bytes = used_offset + ((6 + 8ULL * size + 4095) & ~4095ULL);
    uint64_t ring = dma32_alloc(ring_bytes), buffer = dma32_alloc(4096);
    int vector = irq_alloc_vector(0);
    if (!ring || !buffer || vector < 0 || irq_register((unsigned)vector, rng_interrupt, NULL)) failed("MSI-X test resources");
    if (pci_enable_msix(d, 0, (unsigned)vector, 0)) failed("pci_enable_msix");
    window_check(d);
    port_word_out(io + 0x14, 0xFFFF);                      /* no configuration-change vector */
    port_word_out(io + 0x16, 0);                           /* queue 0 -> MSI-X table entry 0 */
    if (port_word_in(io + 0x16) != 0) failed("virtio-rng rejected MSI-X queue vector");
    port_dword_out(io + 0x08, (uint32_t)(virt_to_phys((void*)ring) >> 12));
    port_byte_out(io + 0x12, 7);                           /* ACKNOWLEDGE | DRIVER | DRIVER_OK */
    volatile struct virtq_desc* desc = (void*)ring;
    volatile uint16_t* avail = (void*)(ring + avail_offset);
    volatile uint16_t* used = (void*)(ring + used_offset);
    desc[0].address = virt_to_phys((void*)buffer); desc[0].length = 64; desc[0].flags = 2; desc[0].next = 0;
    avail[2] = 0;
    __asm__ volatile("mfence" ::: "memory");
    avail[1] = 1;
    __asm__ volatile("mfence" ::: "memory");
    port_word_out(io + 0x10, 0);                           /* notify queue 0 */
    if (!wait_count(&rng_hits, 1) || rng_vector != (unsigned)vector) failed("virtio-rng MSI-X interrupt not delivered");
    uint32_t written = *(volatile uint32_t*)((uint8_t*)used + 8);
    bool random = false;
    for (unsigned i = 0; i < 64; i++) if (((volatile uint8_t*)buffer)[i]) random = true;
    if (used[1] != 1 || !written || written > 64 || !random) failed("virtio-rng used ring");
    port_byte_out(io + 0x12, 0);                           /* reset: the device forgets the ring */
    pci_disable_msix(d); irq_free_vector((unsigned)vector);
    arena_free(ring, ring_bytes); arena_free(buffer, 4096);
    vga_print("[64] PASS PCI MSI-X: virtio-rng queue interrupt on vector "); print_hex((unsigned)vector);
    vga_print(" (table entries="); print_hex((unsigned)entries); vga_print(")\n");
}

void platform_selftest(void) {
    msi_test();
    msix_test();
}
