/* kguard test triggers (tests/test_x64_kguard.py). Two faults that must be contained by the kguard path in
 * native_dispatch - a task killed, the system alive - instead of panicking:
 *   - a stray ring-0 read of a validated user pointer with no uaccess window (an SMAP violation), and
 *   - an unbounded kernel-stack recursion that overflows the guard page (delivered as #PF or #DF).
 * Both run only in the context of a user task that made the private syscall below, and only when armed by the
 * `kguard.probe` boot option, so nothing fires in normal operation. The triggers live here, away from the syscall
 * dispatcher, so the stray access has a clean, identifiable kernel rip. */
#include "native.h"
#include "../../kernel/vga.h"

bool kguard_probe_armed;   /* set in kernel.c from the `kguard.probe` boot option */

/* Stray ring-0 read of a user address the caller passed (a valid, mapped user page): no STAC window, so with SMAP
 * on this #PFs with AC clear and native_dispatch reports it as an SMAP violation and kills the task. */
int64_t kguard_probe_smap(uint64_t user_address) {
    if (!kguard_probe_armed || !accessible(user_address, 8, false)) return -38;
    vga_print("[64] KGUARD PROBE stray ring-0 read of a mapped user page\n");
    uint64_t value = *(volatile uint64_t*)user_address;   /* no uaccess window: faults under SMAP, contained */
    vga_print("[64] KGUARD PROBE read returned (SMAP did NOT enforce)\n");   /* reached only if containment failed */
    return (int64_t)value;
}

/* Overflow this task's kernel stack. Each frame keeps a touched buffer so the recursion walks down into the guard
 * page quickly; volatile + the external sink defeat tail-call/dead-store elimination. The depth limit is volatile
 * and effectively infinite, so the compiler cannot prove the recursion ends (no -Winfinite-recursion) while at run
 * time the stack overflows long before the limit. */
static volatile uint64_t kguard_sink;
static volatile uint64_t kguard_depth_limit = 0xFFFFFFFFULL;
static uint64_t kguard_recurse(uint64_t depth) {
    if (depth >= kguard_depth_limit) return 0;
    volatile uint8_t buffer[512];
    for (unsigned i = 0; i < sizeof(buffer); i += 64) buffer[i] = (uint8_t)(depth + i);
    kguard_sink += buffer[(depth * 64) % sizeof(buffer)];
    return buffer[0] + kguard_recurse(depth + 1);   /* overflows the guard page well before the limit */
}
int64_t kguard_probe_overflow(void) {
    if (!kguard_probe_armed) return -38;
    vga_print("[64] KGUARD PROBE kernel-stack overflow\n");
    kguard_sink += kguard_recurse(1);
    vga_print("[64] KGUARD PROBE recursion returned (overflow NOT trapped)\n");   /* reached only if containment failed */
    return (int64_t)kguard_sink;
}
