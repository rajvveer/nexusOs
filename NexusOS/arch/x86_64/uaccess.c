/* User-memory access support (uaccess.h): the exception table of user copies, SMAP violation classification and
 * user string copies. The copy loop itself is uaccess.asm. */
#include "native.h"

extern const uint64_t uaccess_ex_table[], uaccess_ex_table_end[];
uint64_t uaccess_fixups;   /* user copies that faulted and were fixed up (reported by hardening_report users) */

/* A kernel-mode #PF/#GP at a registered user-copy instruction: resume at its fixup with RFLAGS.AC clear, so the copy
 * returns the bytes it did not move. False for any other instruction (a real kernel fault). */
bool uaccess_fixup(struct registers* r) {
    if (USER_MODE(r)) return false;
    for (const uint64_t* e = uaccess_ex_table; e + 1 < uaccess_ex_table_end; e += 2) {
        if (r->rip != e[0]) continue;
        r->rip = e[1]; r->flags &= ~(1ULL << 18);
        __atomic_fetch_add(&uaccess_fixups, 1, __ATOMIC_RELAXED);
        return true;
    }
    return false;
}

/* Linux fault.c: with SMAP on, a ring-0 access to a user address while RFLAGS.AC is clear is a kernel bug, whatever
 * the page state - it must not be "resolved" (the access would fault again forever on a present page). */
bool uaccess_smap_violation(const struct registers* r, uint64_t address) {
    return smap_enabled && !USER_MODE(r) && !(r->flags & (1ULL << 18)) && user_range(address, 1);
}

/* Copy a NUL-terminated user string into out[capacity]: page by page, each page validated (and faulted in) before
 * it is read, so a string that ends just before an unmapped page is fine. Returns its length without the NUL, -14
 * (EFAULT) when a byte before the NUL is not readable, -36 (ENAMETOOLONG) when capacity bytes hold no NUL. */
int64_t strncpy_from_user(char* out, uint64_t address, uint64_t capacity) {
    uint64_t done = 0;
    while (done < capacity) {
        if (address > ~0ULL - done) return -14;
        uint64_t at = address + done, part = 4096 - (at & 4095);
        if (part > capacity - done) part = capacity - done;
        if (!accessible(at, part, false)) return -14;
        if (copy_from_user(out + done, (const void*)at, part)) return -14;
        for (uint64_t i = 0; i < part; i++) if (!out[done + i]) return (int64_t)(done + i);
        done += part;
    }
    return -36;
}

/* getrandom(2), /dev/urandom: random bytes generated in a kernel buffer, then copied out (the caller validated the
 * range). Returns length, -11 (EAGAIN: no hardware randomness) or -14. */
int64_t native_random_user(uint64_t address, uint64_t length) {
    uint8_t chunk[256];
    for (uint64_t done = 0; done < length;) {
        uint64_t part = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
        if (!native_random(chunk, part)) return done ? (int64_t)done : -11;
        if (copy_to_user((void*)(address + done), chunk, part)) return done ? (int64_t)done : -14;
        done += part;
    }
    __builtin_memset(chunk, 0, sizeof(chunk));
    return (int64_t)length;
}
