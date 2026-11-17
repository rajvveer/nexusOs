/* Disk waits without the big kernel lock.
 *
 * A task whose block request is in flight sleeps (task_sleep_uninterruptible) instead of holding the big lock in a
 * halt/poll loop: other CPUs keep running system calls meanwhile. The storage interrupt (blockdev.c) or the next
 * 1 ms tick wakes it; the driver's completion loop (nvme.c execute, ahci.c port_exec, kernel/ata.c ata_wait_status)
 * then checks its device again under the big lock, exactly as before, so timeouts, ATA recovery and the DMA rules are
 * unchanged.
 *
 * The filesystems, the page cache and the block drivers were written for the big lock alone. So the first task that
 * sleeps in a disk wait becomes the I/O owner: it keeps that role (and its partly done filesystem operation stays
 * consistent) until it next releases the big lock for any other reason -- returning to user mode, a generic sleep,
 * exiting (kernel_lock_release ends it). While another task owns the I/O path, nobody may enter filesystem or block
 * code: block_io_barrier() waits (sleeping) where a task may start such work -- system call entry for every call
 * outside the I/O-free set (syscall_io_free), resuming from a generic sleep (task_wait_channel), page faults that may
 * read a file or swap, reclaim and kswapd, the block layer's own entry points -- and contexts without a task (the
 * BSP's scheduler loop: reaping, write-back, descriptor collection) skip such work until the owner is done.
 * I/O-free calls (process/time/signal/futex/scheduling queries, pipes, sockets, eventfd, poll/epoll, anonymous
 * page faults) run concurrently with the sleeping owner. Request and queue state of every controller is touched only
 * by the owner and by the storage interrupt handlers, both under the big lock. */
#include "native.h"
#include "blockdev.h"
#include "swap.h"
#include "boot_info.h"
#include "../../kernel/vga.h"

void kernel_io_sleep(void* channel, uint64_t deadline);   /* kernel.c: uninterruptible, the kernel globals kept */
unsigned spin_held_count(void);                           /* smp.c */
bool fd_io_free(uint64_t fd);                                         /* kernel.c: pipe, socket, eventfd, epoll, ... */

static void* volatile owner;          /* cpu_self()->task of the I/O sequence in progress, NULL: none */
static volatile bool owner_sleeping;  /* the owner sleeps in a disk wait (its big-lock release keeps the role) */
static int completion_channel, barrier_channel;
static uint64_t sleeps, barrier_waits, skipped;
/* Page faults (code page-in, mapped files, swap-in) wait the first millisecond of a request the old way (no switch, no
 * wake-up latency): a latency-sensitive task (an audio player resuming playback) must not pay a sleep per read
 * (test_x64_hda pause/resume). Requests made by system calls (read, write, fsync, write-back) sleep at once, which is
 * what lets other CPUs run beside heavy I/O (test_x64_blockio). Only one task issues block requests at a time. */
#define SPIN_FIRST_NS 1000000ULL
static volatile uint64_t request_start;
void block_io_request_start(void) { request_start = native_clock_ns(); }
static void* volatile faulting[64];   /* tasks inside a page fault (block_io_fault), whatever CPU they resume on */
void block_io_fault(bool on) {
    void* t = cpu_self()->task; if (!t) return;
    for (unsigned i = 0; i < 64; i++) {
        if (on && !faulting[i]) { faulting[i] = t; return; }
        if (!on && faulting[i] == t) { faulting[i] = NULL; return; }
    }
}
static bool in_fault(void* t) { for (unsigned i = 0; i < 64; i++) if (faulting[i] == t) return true; return false; }
static bool enabled = true;

static inline void* self(void) { return cpu_self()->task; }
/* Sleeping is allowed with the big lock and at most this task's own address-space lock (it is dropped while the task
 * is switched out); any other spinlock would make CPUs spin, interrupts off, through the whole disk wait. */
static bool may_sleep(void) { return self() && kernel_lock_held() && spin_held_count() == (cpu_self()->vm_held ? 1u : 0u); }
bool block_io_busy(void) { void* o = owner; return o && o != self(); }
bool block_io_owner_self(void) { void* o = owner; return o && o == self(); }
void block_io_barrier(void) {
    if (!may_sleep()) return;   /* callers holding other locks did their barrier before taking them (space_access) */
    while (block_io_busy()) { barrier_waits++; kernel_io_sleep(&barrier_channel, native_clock_ns() + 1000000ULL); }
}
/* One step of a completion wait in task context: sleep until a storage interrupt or the next tick. False: the caller
 * must wait the old way (no task, the big lock not held, or the switch is off). */
bool block_io_sleep(void) {
    void* t = self();
    static int mode = -1;   /* kernel option blockio.spin: the old waits with the big lock (A/B measurements) */
    if (mode < 0) { mode = !boot_option("blockio.spin"); if (!mode) vga_print("[64] BLOCKIO disk waits keep the big lock (blockio.spin)\n"); }
    if (!enabled || !mode || !may_sleep() || pm_freezing()) return false;   /* system sleep: no new sleeping owner (pm.c) */
    if (owner && owner != t) return false;   /* a missing barrier: keep the old serialized wait (never two sequences) */
    if (in_fault(t) && native_clock_ns() - request_start < SPIN_FIRST_NS) return false;   /* page-in: short waits spin */
    owner = t; owner_sleeping = true; sleeps++;
    kernel_io_sleep(&completion_channel, native_clock_ns() + 1000000ULL);
    owner_sleeping = false;
    return true;
}
void block_io_completed(void) { task_wake_channel(&completion_channel); }   /* storage interrupt handlers */
/* kernel_lock_release: a big-lock release outside a disk wait ends the owner's I/O sequence. */
void block_io_unlocking(void) {
    if (owner && owner == self() && !owner_sleeping) { owner = NULL; task_wake_channel(&barrier_channel); }
}
/* Contexts without a task (BSP loop polls) must not start filesystem or block work while an owner sleeps. */
bool block_io_defer(void) { if (!block_io_busy()) return false; skipped++; return true; }
void block_io_stats(uint64_t out[3]) { out[0] = sleeps; out[1] = barrier_waits; out[2] = skipped; }
const void* block_io_barrier_channel(void) { return &barrier_channel; }   /* pm.c: is pid 1 held at the barrier */
void block_io_enable(bool on) { enabled = on; }

/* System calls that never touch a filesystem, the page cache or a block device (Linux numbers). */
bool syscall_io_free(const struct registers* r) {
    switch (r->rax) {
    case 13: case 14: case 15: case 24: case 35: case 39: case 62: case 63: case 96: case 97: case 98: case 99: case 100:
    case 102: case 104: case 107: case 108: case 110: case 111: case 115: case 118: case 120: case 121: case 124:
    case 127: case 131: case 140: case 141: case 143: case 144: case 145: case 146: case 147: case 148: case 158: case 160:
    case 186: case 200: case 201: case 202: case 203: case 204: case 218: case 228: case 229: case 230: case 234: case 273:
    case 274: case 302: case 309: case 318:
        return true;   /* signals, ids, time, sleeps, scheduling, futex, limits, uname/sysinfo, getrandom, arch_prctl */
    case 7: case 23: case 213: case 232: case 233: case 270: case 271: case 281: case 284: case 290: case 291:
        return true;   /* poll/select/epoll/eventfd: readiness only (regular files are always ready) */
    case 41: case 44: case 45: case 46: case 47: case 48: case 51: case 52: case 53: case 54: case 55: case 299: case 307:
        return true;   /* sockets without names (Unix-domain bind/connect resolve paths: not here) */
    case 0: case 1: case 19: case 20:
        return fd_io_free(r->rdi);   /* read/write/readv/writev on a pipe, socket, eventfd, ... */
    default:
        return false;
    }
}
