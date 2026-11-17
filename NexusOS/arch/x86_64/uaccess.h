/* User-memory access layer (SMAP). The kernel runs with CR4.SMAP on (default; boot option `nosmap` turns it off),
 * so a ring-0 load or store to a user page faults unless RFLAGS.AC is set. Every intended access to user memory
 * goes through these helpers: they open a STAC/CLAC window around one copy, and a fault inside that copy (the
 * page vanished after validation: another thread's munmap while a disk wait had dropped the big lock, a racing
 * mprotect) is fixed up instead of crashing the kernel - the copy stops and reports the bytes it did not move,
 * like Linux copy_{to,from}_user (uaccess.asm holds the copy loop and its exception-table entry, uaccess.c the
 * table lookup used by the #PF/#GP path in native_dispatch). Any other ring-0 user access is an SMAP violation:
 * reported (KERNEL_FAULT ... SMAP) and contained, never silently allowed.
 *
 * The validation (accessible(), user_range()) is unchanged and still required before an access: these helpers do
 * not decide what a process may touch, they make the touch itself safe. Conventions:
 *   copy_to_user(user, kernel, n) / copy_from_user(kernel, user, n): 0 or the bytes NOT copied (a failed
 *       copy_from_user zero-fills the rest of the kernel buffer, as Linux).
 *   uget8/16/32/64(address): value (0 if it faulted); uput8/16/32/64(address, value): 0 or -EFAULT (-14).
 *   strncpy_from_user(out, address, capacity): length without the NUL, -14 (unmapped) or -36 (no NUL in capacity).
 *   uaccess_begin()/uaccess_end(): a raw window for code that cannot use a copy (no fixup: keep it to plain loads/
 *       stores of validated memory, never a call that may sleep or fault on other pages).
 * STAC/CLAC are legal only when the CPU has SMAP, hence the smap_enabled gate (hardening.c sets it per boot). */
#ifndef NEXUS_UACCESS_H
#define NEXUS_UACCESS_H
#include "../../kernel/types.h"

extern bool smap_enabled;
uint64_t uaccess_copy(void* to, const void* from, uint64_t n);   /* uaccess.asm: bytes NOT copied */
uint64_t uaccess_clear(void* to, uint64_t n);                    /* uaccess.asm: bytes NOT zeroed */
static inline uint64_t clear_user(uint64_t address, uint64_t n) { return n ? uaccess_clear((void*)address, n) : 0; }
int64_t strncpy_from_user(char* out, uint64_t address, uint64_t capacity);   /* uaccess.c */
int64_t native_random_user(uint64_t address, uint64_t length);   /* uaccess.c: RDRAND into validated user memory: length, -11, -14 */
struct registers;
bool uaccess_fixup(struct registers* r);                    /* uaccess.c: kernel #PF/#GP inside a user copy */
bool uaccess_smap_violation(const struct registers* r, uint64_t address);   /* uaccess.c: ring-0 user access, AC clear */

static inline void uaccess_begin(void) { if (smap_enabled) __asm__ volatile("stac" ::: "cc", "memory"); }
static inline void uaccess_end(void)   { if (smap_enabled) __asm__ volatile("clac" ::: "cc", "memory"); }

static inline uint64_t copy_to_user(void* user, const void* kernel, uint64_t n) { return n ? uaccess_copy(user, kernel, n) : 0; }
static inline uint64_t copy_from_user(void* kernel, const void* user, uint64_t n) {
    uint64_t left = n ? uaccess_copy(kernel, user, n) : 0;
    if (left) __builtin_memset((uint8_t*)kernel + (n - left), 0, left);
    return left;
}
static inline uint8_t uget8(uint64_t a)   { uint8_t v = 0;  copy_from_user(&v, (const void*)a, 1); return v; }
static inline uint16_t uget16(uint64_t a) { uint16_t v = 0; copy_from_user(&v, (const void*)a, 2); return v; }
static inline uint32_t uget32(uint64_t a) { uint32_t v = 0; copy_from_user(&v, (const void*)a, 4); return v; }
static inline uint64_t uget64(uint64_t a) { uint64_t v = 0; copy_from_user(&v, (const void*)a, 8); return v; }
static inline int uput8(uint64_t a, uint8_t v)   { return copy_to_user((void*)a, &v, 1) ? -14 : 0; }
static inline int uput16(uint64_t a, uint16_t v) { return copy_to_user((void*)a, &v, 2) ? -14 : 0; }
static inline int uput32(uint64_t a, uint32_t v) { return copy_to_user((void*)a, &v, 4) ? -14 : 0; }
static inline int uput64(uint64_t a, uint64_t v) { return copy_to_user((void*)a, &v, 8) ? -14 : 0; }
#endif
