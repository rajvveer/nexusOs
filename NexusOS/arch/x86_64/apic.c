/* Local APIC access in xAPIC or x2APIC mode (see apic.h).
 *
 * x2APIC: IA32_APIC_BASE (MSR 0x1B) bits 11 (enable) + 10 (extended); registers are MSRs 0x800 + MMIO offset / 16; the
 * ID register holds the full 32-bit x2APIC id (xAPIC: bits 31..24); the ICR is one 64-bit MSR (0x830, destination in
 * bits 63..32) without a delivery-status bit; EOI must be written with 0. Leaving x2APIC mode goes through the disabled
 * state (EN = EXTD = 0), entering it from disabled goes through xAPIC (SDM 10.12.5). The firmware may hand over either
 * mode, and an AP keeps x2APIC mode across INIT, so every CPU calls apic_enter_mode() before touching a register.
 * IOAPIC redirection entries and MSI addresses still carry 8-bit destinations (no interrupt remapping here): device
 * interrupts go to APIC ids below 255, which every processor the MADT parser accepts has.
 * A platform that advertises x2APIC but refuses the switch (WHPX with QEMU's user-space APIC raises #GP on the
 * IA32_APIC_BASE write; trap64 turns it into an error return) or its MSRs keeps xAPIC mode. */
#include "native.h"
#include "apic.h"
#include "boot_info.h"
#include "../../kernel/vga.h"

#define APIC_MMIO 0xFEE00000ULL
#define BASE_ENABLE (1ULL << 11)
#define BASE_EXTENDED (1ULL << 10)
bool apic_x2;

static uint64_t msr_read(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return (uint64_t)hi << 32 | lo; }
static void msr_write(uint32_t msr, uint64_t value) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory"); }
/* int apic_wrmsr_safe(uint32_t msr, uint64_t value): 0, or -5 after a #GP (process.c trap64 fixup) */
int apic_wrmsr_safe(uint32_t msr, uint64_t value);
int hw_rdmsr_safe(uint32_t msr, uint64_t* value);
__asm__(".text\n"
        ".globl apic_wrmsr_safe, apic_wrmsr_fault, apic_wrmsr_fixup\n"
        "apic_wrmsr_safe:\n"
        "    movl %edi, %ecx\n"
        "    movl %esi, %eax\n"
        "    movq %rsi, %rdx\n"
        "    shrq $32, %rdx\n"
        "apic_wrmsr_fault:\n"
        "    wrmsr\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        "apic_wrmsr_fixup:\n"
        "    movl $-5, %eax\n"
        "    ret\n");
static void cpuid1(uint32_t* ecx, uint32_t* edx) { uint32_t a, b; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(*ecx), "=d"(*edx) : "a"(1), "c"(0)); }
static bool cpu_has_x2apic(void) { uint32_t ecx, edx; cpuid1(&ecx, &edx); return (edx & (1U << 9)) && (ecx & (1U << 21)); }

uint32_t apic_read(unsigned reg) {
    if (apic_x2) return (uint32_t)msr_read(0x800 + (reg >> 4));
    return *(volatile uint32_t*)phys_to_virt(APIC_MMIO + reg);
}
void apic_write(unsigned reg, uint32_t value) {
    if (apic_x2) {
        if (reg == 0x310) return;                                         /* ICR high: part of the 64-bit ICR MSR */
        if (reg == 0x300) { msr_write(0x830, value); return; }
        msr_write(0x800 + (reg >> 4), value); return;
    }
    *(volatile uint32_t*)phys_to_virt(APIC_MMIO + reg) = value;
}
void apic_icr(uint32_t destination, uint32_t command) {
    if (apic_x2) {
        /* wrmsr to the ICR is not serializing: earlier stores (the IPI's request data) must be visible first */
        __asm__ volatile("mfence; lfence" ::: "memory");
        msr_write(0x830, (uint64_t)destination << 32 | command);
        return;
    }
    volatile uint32_t* low = phys_to_virt(APIC_MMIO + 0x300), *high = phys_to_virt(APIC_MMIO + 0x310);
    while (*low & (1U << 12)) __asm__ volatile("pause");
    *high = destination << 24; *low = command;
}
uint32_t apic_id_self(void) { return apic_x2 ? apic_read(0x20) : apic_read(0x20) >> 24; }

bool apic_enter_mode(void) {
    uint32_t ecx, edx; cpuid1(&ecx, &edx);
    if (!(edx & (1U << 9))) return false;
    uint64_t base = msr_read(0x1B);
    bool extended = (base & BASE_EXTENDED) != 0;
    if (extended && !apic_x2) {                                           /* x2APIC -> disabled -> xAPIC */
        msr_write(0x1B, base & ~(BASE_EXTENDED | BASE_ENABLE));
        msr_write(0x1B, (base & ~BASE_EXTENDED) | BASE_ENABLE);
    } else if (!extended && apic_x2) {                                    /* (disabled ->) xAPIC -> x2APIC */
        if (!(base & BASE_ENABLE)) { msr_write(0x1B, base | BASE_ENABLE); base = msr_read(0x1B); }
        uint64_t id;
        if (apic_wrmsr_safe(0x1B, base | BASE_ENABLE | BASE_EXTENDED) || hw_rdmsr_safe(0x802, &id)) {
            if (msr_read(0x1B) & BASE_EXTENDED) {                         /* accepted but no MSRs: back to xAPIC */
                msr_write(0x1B, base & ~(BASE_EXTENDED | BASE_ENABLE)); msr_write(0x1B, (base & ~BASE_EXTENDED) | BASE_ENABLE);
            }
            return false;
        }
    } else if (!(base & BASE_ENABLE)) msr_write(0x1B, base | BASE_ENABLE);
    base = msr_read(0x1B);
    if (!(base & BASE_ENABLE) || ((base & BASE_EXTENDED) != 0) != apic_x2) return false;
    return apic_x2 || (base & 0xFFFFFF000ULL) == APIC_MMIO;
}
bool apic_select(void) {
    uint64_t base = msr_read(0x1B);
    bool firmware_x2 = (base & BASE_EXTENDED) && (base & BASE_ENABLE), have = cpu_has_x2apic();
    apic_x2 = have && !boot_option("nox2apic") && (firmware_x2 || boot_option("x2apic"));
    if (firmware_x2 && !apic_x2) vga_print("[64] APIC: firmware left the local APIC in x2APIC mode; switched to xAPIC (nox2apic)\n");
    if (boot_option("x2apic") && !have) vga_print("[64] APIC: x2apic requested but the CPU has no x2APIC; xAPIC\n");
    if (!apic_enter_mode()) {
        if (!apic_x2) return false;
        vga_print("[64] APIC: the platform refused x2APIC mode; xAPIC\n");
        apic_x2 = false; if (!apic_enter_mode()) return false;
    }
    if (apic_x2) {
        vga_print("[64] APIC: x2APIC mode (MSR interface");
        vga_print(firmware_x2 ? ", kept from the firmware), BSP x2APIC id " : ", kernel command line), BSP x2APIC id ");
        char digits[12]; unsigned n = 0, id = apic_id_self(); do { digits[n++] = (char)('0' + id % 10); id /= 10; } while (id);
        char text[12]; unsigned k = 0; while (n) text[k++] = digits[--n]; text[k] = 0; vga_print(text); vga_print("\n");
    }
    return true;
}
