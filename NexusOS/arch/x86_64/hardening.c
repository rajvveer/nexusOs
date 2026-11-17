/* Kernel self-protection: SMEP, a boot-time stack-protector canary, and a W^X audit of every kernel mapping.
 *
 * SMEP (CR4.SMEP) stops ring 0 from executing ring-3 pages; the kernel never jumps into user memory, so it is a
 * pure win with no compatibility cost. It is enabled per-CPU in cpu_local_init (BSP and every AP).
 *
 * The stack canary (__stack_chk_guard) is seeded by entry.asm (RDRAND, RDTSC fallback, low byte zeroed like glibc)
 * before the first C function runs, so every -fstack-protector-strong prologue/epilogue sees a consistent random
 * value; __stack_chk_fail panics.
 *
 * SMAP is on by default (boot option `nosmap` turns it off): every user access goes through uaccess.h (copies with
 * STAC/CLAC and fault fixups), and any other ring-0 user access is reported as an SMAP violation. UMIP is on with
 * the Linux sgdt/sidt/sldt/smsw/str emulation (umip.c). */
#include "native.h"
#include "../../kernel/vga.h"

#define PRESENT 1ULL
#define WRITE 2ULL
#define USER 4ULL
#define LARGE 128ULL
#define NX (1ULL << 63)
#define ADDRESS 0x000FFFFFFFFFF000ULL

uint64_t __stack_chk_guard;   /* seeded by entry.asm before kernel64_main; referenced by every protected prologue */
void __attribute__((noreturn)) __stack_chk_fail(void) { panic("[64] KERNEL_FAULT stack smashing detected (__stack_chk_fail)"); }

static uint64_t security_cr4;   /* recorded for hardening_report */
static bool smep_on, smap_on, umip_on, smap_present, umip_present;
bool smap_enabled;   /* uaccess.h gate: true once CR4.SMAP is on and every user access goes through a stac/clac window */
extern bool smap_wanted;   /* kernel.c: on unless the `nosmap` boot option */

/* Per-CPU: enable SMEP, UMIP (with emulation) and - when the audit is on - SMAP. */
void security_cpu_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    smap_present = (b & (1u << 20)) != 0;
    umip_present = (c & (1u << 2)) != 0;
    uint64_t cr4; __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if (b & (1u << 7)) { cr4 |= (1ULL << 20); smep_on = true; }                       /* SMEP */
    if (umip_present) { cr4 |= (1ULL << 11); umip_on = true; umip_enabled = true; }   /* UMIP (emulated: umip.c) */
    if (smap_present && smap_wanted) { cr4 |= (1ULL << 21); smap_on = true; smap_enabled = true; }  /* SMAP */
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory");
    if (smap_on) __asm__ volatile("clac" ::: "cc", "memory");   /* start with AC clear */
    security_cr4 = cr4;
}

/* Walk the live kernel page tables (CR3) over the upper half (PML4 256..511: kernel image, direct map, MMIO window)
 * and assert the W^X invariant: no page is both writable and executable. Returns true when the invariant holds.
 * Counts executable and writable-executable leaves for the boot report. */
static void walk(uint64_t table_phys, unsigned level, uint64_t* exec, uint64_t* wx, uint64_t* user_leaks) {
    uint64_t* table = phys_to_virt(table_phys);
    unsigned first = level == 3 ? 256 : 0;   /* only the upper half at the top level */
    for (unsigned i = first; i < 512; i++) {
        uint64_t e = table[i];
        if (!(e & PRESENT)) continue;
        bool leaf = level == 0 || (e & LARGE);
        if (leaf) {
            bool executable = !(e & NX), writable = (e & WRITE) != 0;
            if (executable) (*exec)++;
            if (executable && writable) (*wx)++;
            if (executable && (e & USER)) (*user_leaks)++;   /* a user-executable page in the kernel half: never expected */
        } else {
            walk(e & ADDRESS, level - 1, exec, wx, user_leaks);
        }
    }
}
bool security_verify_wx(void) {
    uint64_t root; __asm__ volatile("mov %%cr3,%0" : "=r"(root)); root &= ADDRESS;
    uint64_t exec = 0, wx = 0, user_leaks = 0;
    walk(root, 3, &exec, &wx, &user_leaks);
    vga_print("[64] W^X audit: executable kernel pages="); { char n[24]; number64(exec, n); vga_print(n); }
    vga_print(" writable+executable="); { char n[24]; number64(wx, n); vga_print(n); }
    vga_print(" user-exec-in-kernel-half="); { char n[24]; number64(user_leaks, n); vga_print(n); } vga_print("\n");
    return wx == 0 && user_leaks == 0;
}

void hardening_report(void) {
    vga_print("[64] Hardening: SMEP="); vga_print(smep_on ? "on" : "off");
    vga_print(" SMAP="); vga_print(smap_on ? "on" : smap_present ? "present-off" : "absent");
    vga_print(" UMIP="); vga_print(umip_on ? "on" : umip_present ? "present-off" : "absent");
    vga_print(" stack-canary="); vga_print(__stack_chk_guard ? "on" : "off"); vga_print("\n");
}
