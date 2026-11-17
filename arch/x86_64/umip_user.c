/* UMIP emulation conformance (tests/test_x64_umip.py). With CR4.UMIP set, SGDT/SIDT/SLDT/SMSW/STR from ring 3
 * #GP and the kernel emulates them (umip.c): this program runs each, in memory and register operand forms and a
 * few addressing modes, and checks it gets the Linux-compatible dummy values and never a SIGSEGV. Run under -cpu
 * max (UMIP present). */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(t) do { if (!(t)) { dprintf(2, "[64] UMIP FAIL line=%d\n", __LINE__); _exit(70); } } while (0)

#define UMIP_GDT_BASE 0xfffffffffffe0000ULL
#define UMIP_IDT_BASE 0xffffffffffff0000ULL

static uint64_t desc_base(const uint8_t d[10]) {
    uint64_t b = 0; for (int i = 0; i < 8; i++) b |= (uint64_t)d[2 + i] << (8 * i); return b;
}

int main(void) {
    uint8_t gdt[10], idt[10];
    __asm__ volatile("sgdt %0" : "=m"(gdt));
    __asm__ volatile("sidt %0" : "=m"(idt));
    uint16_t glim = gdt[0] | gdt[1] << 8, ilim = idt[0] | idt[1] << 8;
    CHECK(desc_base(gdt) == UMIP_GDT_BASE && glim == 0);
    CHECK(desc_base(idt) == UMIP_IDT_BASE && ilim == 0);

    /* SGDT through a computed address (stack array + index) exercises ModRM/SIB decode. */
    uint8_t buf[64]; memset(buf, 0xAA, sizeof(buf));
    volatile int idx = 16;
    __asm__ volatile("sgdt %0" : "=m"(buf[idx]));
    CHECK(desc_base((const uint8_t*)&buf[16]) == UMIP_GDT_BASE);

    uint16_t ldt = 0x1234, tr = 0x5678;
    __asm__ volatile("sldt %0" : "=m"(ldt));
    __asm__ volatile("str %0" : "=m"(tr));
    CHECK(ldt == 0 && tr == 0);

    /* register operand forms (SLDT/STR/SMSW) */
    uint64_t ldt_r = 0xDEAD, tr_r = 0xBEEF, msw_r = 0;
    __asm__ volatile("sldt %0" : "=r"(ldt_r));
    __asm__ volatile("str %0" : "=r"(tr_r));
    __asm__ volatile("smsw %0" : "=r"(msw_r));
    CHECK((ldt_r & 0xFFFF) == 0 && (tr_r & 0xFFFF) == 0);
    CHECK((msw_r & 1) == 1);   /* CR0.PE set (protected/long mode) */

    uint16_t msw_m = 0;
    __asm__ volatile("smsw %0" : "=m"(msw_m));
    CHECK((msw_m & 1) == 1);

    /* 16-bit operand-size register SMSW (0x66 prefix) */
    uint16_t msw16 = 0;
    __asm__ volatile(".byte 0x66\n\t smsw %0" : "=m"(msw16));
    CHECK((msw16 & 1) == 1);

    dprintf(1, "[64] UMIP ALL PASSED: SGDT/SIDT dummy base, SLDT/STR=0, SMSW=CR0 low, mem+reg operands, no SIGSEGV\n");
    return 0;
}
