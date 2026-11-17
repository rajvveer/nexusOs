/* Small Linux ABI calls (kabi, wave 2): quotactl(2), quotactl_fd(2), getcpu(2), and the deliberate rseq(2) answer.
 *
 * quotactl: NexusOS filesystems keep no disk quotas. The answers are those Linux gives for the same situation:
 * the device must name a mounted block device (ENOENT / ENOTBLK / ENODEV), a filesystem without quota operations
 * (vfat, exFAT, ISO 9660, FUSE) answers ENOSYS, and an ext4 volume (quota operations, quota accounting off) answers
 * per command: Q_SYNC/Q_XQUOTASYNC 0, Q_QUOTAOFF 0, Q_GETFMT/Q_GETINFO/Q_GETQUOTA/Q_GETNEXTQUOTA/Q_SETQUOTA/
 * Q_SETINFO/Q_XGETQUOTA/Q_XGETNEXTQUOTA/Q_XSETQLIM ESRCH ("quotas not on"), Q_QUOTAON EINVAL (no quota mount option),
 * Q_XGETQSTAT(V)/Q_XQUOTAON/Q_XQUOTAOFF/Q_XQUOTARM ENOSYS - after Linux's permission rules (CAP_SYS_ADMIN, or the
 * caller's own uid/gid for the GETQUOTA queries). Firefox asks Q_GETQUOTA for the profile's disk and falls back to
 * statfs on any error.
 *
 * rseq: not implemented, on purpose (ENOSYS, not logged as missing). glibc >= 2.35 registers every thread and on
 * failure sets __rseq_size = 0 and rseq_area.cpu_id = RSEQ_CPU_ID_REGISTRATION_FAILED, so sched_getcpu() uses
 * getcpu (vDSO/syscall) and nothing else in glibc depends on it (the glibc.pthread.rseq=0 tunable gives the same
 * state); tcmalloc and librseq users fall back the same way. A registered rseq would have to be updated (cpu_id) and
 * checked for critical-section aborts on every return to user mode after preemption, migration or a signal, which
 * the lockless preemption path cannot do safely. tests/test_x64_kabi.py proves the fallback (rseq64). */
#include "native.h"
#include "uaccess.h"
#include "mount.h"
#include "../../kernel/string.h"

#define Q_SYNC 0x800001U
#define Q_QUOTAON 0x800002U
#define Q_QUOTAOFF 0x800003U
#define Q_GETFMT 0x800004U
#define Q_GETINFO 0x800005U
#define Q_SETINFO 0x800006U
#define Q_GETQUOTA 0x800007U
#define Q_SETQUOTA 0x800008U
#define Q_GETNEXTQUOTA 0x800009U
#define Q_XQUOTAON 0x5801U
#define Q_XQUOTAOFF 0x5802U
#define Q_XGETQUOTA 0x5803U
#define Q_XSETQLIM 0x5804U
#define Q_XGETQSTAT 0x5805U
#define Q_XQUOTARM 0x5806U
#define Q_XQUOTASYNC 0x5807U
#define Q_XGETQSTATV 0x5808U
#define Q_XGETNEXTQUOTA 0x5809U
#define CAP_SYS_ADMIN 21

enum { FS_NONE, FS_NO_QUOTA_OPS, FS_EXT4 };
/* The filesystem mounted from block device `path` (canonical): matched against the mount table's sources. */
static int device_filesystem(const char* path) {
    char* table = (void*)arena_alloc(32768); if (!table) return -12;
    unsigned length = mount_proc_text(table, 32768, false); table[length < 32768 ? length : 32767] = 0;
    int result = FS_NONE; unsigned n = strlen(path);
    for (char* line = table; line && *line;) {
        char* end = line; while (*end && *end != '\n') end++;
        char* space = line; while (space < end && *space != ' ') space++;
        if ((unsigned)(space - line) == n && !memcmp(line, path, n) && space < end) {
            char* type = space + 1; while (type < end && *type != ' ') type++;   /* skip the mount point */
            if (type < end) type++;
            result = !strncmp(type, "ext4 ", 5) || !strncmp(type, "ext3 ", 5) || !strncmp(type, "ext2 ", 5) ? FS_EXT4 : FS_NO_QUOTA_OPS;
            if (result == FS_EXT4) break;
        }
        line = *end ? end + 1 : NULL;
    }
    arena_free((uint64_t)table, 32768);
    return result;
}
/* Linux do_quotactl for a filesystem of kind `fs` that has no active quotas. */
static int64_t quota_command(int fs, uint32_t command, uint32_t type, uint32_t id, uint64_t address) {
    if (fs == FS_NO_QUOTA_OPS) return -38;
    struct credentials* c = credentials_current();
    bool admin = credentials_capable(c, NULL, CAP_SYS_ADMIN);
    switch (command) {   /* check_quotactl_permission */
    case Q_GETFMT: case Q_SYNC: case Q_GETINFO: case Q_XGETQSTAT: case Q_XGETQSTATV: case Q_XQUOTASYNC: break;
    case Q_GETQUOTA: case Q_XGETQUOTA: {
        bool own = type == 0 ? credentials_id(c, false, 1) == id : type == 1 && credentials_id(c, true, 1) == id;
        if (!own && !admin) return -1;
        break;
    }
    default: if (!admin) return -1;
    }
    switch (command) {
    case Q_SYNC: case Q_XQUOTASYNC: case Q_QUOTAOFF: return 0;
    case Q_QUOTAON: return -22;
    case Q_GETFMT: case Q_GETINFO: case Q_GETQUOTA: case Q_GETNEXTQUOTA: case Q_XGETQUOTA: case Q_XGETNEXTQUOTA: return -3;
    case Q_SETINFO: return accessible(address, 72, false) ? -3 : -14;          /* struct if_dqinfo */
    case Q_SETQUOTA: return accessible(address, 72, false) ? -3 : -14;         /* struct if_dqblk */
    case Q_XSETQLIM: return accessible(address, 104, false) ? -3 : -14;        /* struct fs_disk_quota */
    case Q_XGETQSTAT: case Q_XGETQSTATV: case Q_XQUOTAON: case Q_XQUOTAOFF: case Q_XQUOTARM: return -38;
    }
    return -22;
}
static int64_t quotactl_call(uint32_t cmd, uint64_t special, uint32_t id, uint64_t address) {
    uint32_t type = cmd & 0xFF, command = cmd >> 8;
    if (type >= 3) return -22;
    if (!special) return command == Q_SYNC ? 0 : -19;   /* Q_SYNC of every filesystem; anything else needs a device */
    char path[FAT32_PATH_MAX]; struct open_file* bound = NULL;
    int error = native_path_resolve((uint64_t)-100, special, true, path, &bound); if (error) return error;
    struct linux_stat stat; error = native_path_stat(path, bound, true, &stat); if (error) return error;
    if ((stat.mode & 0170000) != 0060000) return -15;   /* ENOTBLK */
    int fs = device_filesystem(path); if (fs < 0) return fs;
    if (fs == FS_NONE) return -19;                     /* ENODEV: nothing mounted from it */
    return quota_command(fs, command, type, id, address);
}
static int64_t quotactl_fd_call(uint32_t fd, uint32_t cmd, uint32_t id, uint64_t address) {
    struct open_file* f = fd_lookup(fd); if (!f) return -9;
    uint32_t type = cmd & 0xFF, command = cmd >> 8;
    if (type >= 3) return -22;
    struct linux_stat stat; int error = fd_description_stat(f, &stat); if (error) return error;
    uint64_t values[12]; int fs;
    if (stat.dev == 1) fs = disk_posix() ? FS_EXT4 : FS_NO_QUOTA_OPS;              /* the root filesystem */
    else if (mount_statfs_dev(stat.dev, values)) fs = values[0] == 0xEF53 ? FS_EXT4 : FS_NO_QUOTA_OPS;
    else fs = FS_NO_QUOTA_OPS;                                                    /* proc, sysfs, pipes, sockets */
    if (command == Q_QUOTAON && fs == FS_EXT4) return credentials_capable(credentials_current(), NULL, CAP_SYS_ADMIN) ? -22 : -1;
    return quota_command(fs, command, type, id, address);
}
/* getcpu(cpu, node, cache): this CPU's index, NUMA node 0 (one node). */
static int64_t getcpu_call(uint64_t cpu, uint64_t node) {
    if ((cpu && !accessible(cpu, 4, true)) || (node && !accessible(node, 4, true))) return -14;
    uint32_t index = cpu_self()->index, zero = 0;
    if (cpu && uput32(cpu, index)) return -14;
    if (node && uput32(node, zero)) return -14;
    return 0;
}
/* ---- memory protection keys: no CPU protection-key support is enabled (CR4.PKE off), which Linux reports as
 * pkey_alloc ENOSPC (only the default key 0 exists), pkey_mprotect with key -1 or 0 = mprotect (kernel.c), any other
 * key EINVAL, pkey_free(0) 0. V8 (Chrome, Electron) probes pkey_alloc and runs without keys. ---- */
static int64_t pkey_alloc_call(uint64_t flags, uint64_t init) { return flags || (init & ~3ULL) ? -22 : -28; }
static int64_t pkey_free_call(int32_t key) { return key == 0 ? 0 : -22; }

/* ---- NUMA memory policy: one node (0). set_mempolicy/get_mempolicy/mbind validate as Linux mm/mempolicy.c does;
 * the thread's mode (with its MPOL_F_* mode flags) is kept and reported back, every policy places memory on node 0. */
#define MPOL_MODE_FLAGS 0xE000U   /* MPOL_F_STATIC_NODES 1<<15, MPOL_F_RELATIVE_NODES 1<<14, MPOL_F_NUMA_BALANCING 1<<13 */
uint16_t* task_mempolicy(void);
/* The nodemask argument: node 0 set and/or other (absent) nodes set; -14 unreadable, -22 malformed. */
static int read_nodes(uint64_t mask, uint64_t maxnode, bool* node0, bool* others) {
    *node0 = *others = false;
    if (!mask || !maxnode) return 0;
    maxnode--;   /* Linux get_nodes: maxnode counts one bit too many */
    if (maxnode > 4096ULL * 8) return -22;
    uint64_t words = (maxnode + 63) / 64; if (!words) return 0;
    if (!accessible(mask, words * 8, false)) return -14;
    for (uint64_t i = 0; i < words; i++) {
        uint64_t word = uget64(mask + i * 8);
        if (i == words - 1 && maxnode % 64) word &= (1ULL << (maxnode % 64)) - 1;
        if (!i && (word & 1)) *node0 = true;
        if (i ? word != 0 : (word & ~1ULL) != 0) *others = true;
    }
    return 0;
}
/* MPOL_DEFAULT 0, PREFERRED 1, BIND 2, INTERLEAVE 3, LOCAL 4, PREFERRED_MANY 5, WEIGHTED_INTERLEAVE 6 */
static int check_policy(uint64_t mode, uint64_t mask, uint64_t maxnode) {
    uint32_t flags = (uint32_t)mode & MPOL_MODE_FLAGS, base = (uint32_t)mode & ~MPOL_MODE_FLAGS;
    if (mode > 0xFFFFFFFFULL || base > 6) return -22;
    if ((flags & 0xC000U) == 0xC000U) return -22;                 /* STATIC and RELATIVE together */
    if ((flags & 0x2000U) && base != 2 && base != 1) return -22;  /* NUMA_BALANCING: MPOL_BIND / PREFERRED only */
    bool node0, others; int error = read_nodes(mask, maxnode, &node0, &others); if (error) return error;
    bool empty = !node0 && !others;
    if (base == 0 || base == 4) return empty && !flags ? 0 : -22;  /* DEFAULT/LOCAL: no nodes, no mode flags */
    if (base == 1 && empty) return flags & 0xC000U ? -22 : 0;      /* PREFERRED without nodes = local allocation */
    return node0 ? 0 : -22;   /* the nodes, masked with the online ones (node 0), must not be empty */
}
static int64_t set_mempolicy_call(uint64_t mode, uint64_t mask, uint64_t maxnode) {
    int error = check_policy(mode, mask, maxnode); if (error) return error;
    uint16_t* policy = task_mempolicy(); if (policy) *policy = (uint16_t)mode;
    return 0;
}
static int64_t mbind_call(uint64_t start, uint64_t length, uint64_t mode, uint64_t mask, uint64_t maxnode, uint64_t flags) {
    if (flags & ~7ULL) return -22;                                  /* MPOL_MF_STRICT | MOVE | MOVE_ALL */
    if ((flags & 4) && !credentials_capable(credentials_current(), NULL, 23)) return -1;   /* MOVE_ALL: CAP_SYS_NICE */
    if (start & 4095) return -22;
    if ((uint32_t)mode & 0x2000U) return -22;                       /* NUMA_BALANCING is not an mbind flag */
    int error = check_policy(mode, mask, maxnode); if (error) return error;
    uint64_t end = start + ((length + 4095) & ~4095ULL);
    if (end < start) return -22;
    if (end == start) return 0;
    if (!user_range(start, end - start)) return -14;
    for (uint64_t page = start; page < end; page += 4096) if (!accessible(page, 1, false)) return -14;
    return 0;
}
static int64_t get_mempolicy_call(uint64_t policy_address, uint64_t mask, uint64_t maxnode, uint64_t address, uint64_t flags) {
    if (flags & ~7ULL) return -22;                                  /* MPOL_F_NODE 1, MPOL_F_ADDR 2, MPOL_F_MEMS_ALLOWED 4 */
    if ((flags & 4) && (flags & 3)) return -22;
    if (mask && maxnode < 1) return -22;                            /* nr_node_ids */
    if (mask && (maxnode - 1) > 4096ULL * 8) return -22;
    uint16_t* stored = task_mempolicy(); uint32_t mode = stored ? *stored : 0, result = mode;
    if (flags & 2) {   /* the policy of the area at `address` (no per-area policies: the thread's applies) */
        if (!user_range(address, 1) || !accessible(address & ~4095ULL, 1, false)) return -14;
        if (flags & 1) result = 0;   /* MPOL_F_NODE | MPOL_F_ADDR: the page's node */
    } else if (flags & 1) {
        if ((mode & ~MPOL_MODE_FLAGS) != 3 && (mode & ~MPOL_MODE_FLAGS) != 6) return -22;   /* next interleave node */
        result = 0;
    } else if (flags & 4) result = 0;
    bool with_nodes = (flags & 4) || ((mode & ~MPOL_MODE_FLAGS) != 0 && (mode & ~MPOL_MODE_FLAGS) != 4);
    if (policy_address) { if (!accessible(policy_address, 4, true)) return -14; if (uput32(policy_address, (uint32_t)(int32_t)result)) return -14; }
    if (mask) {
        uint64_t words = (maxnode - 1 + 63) / 64;
        if (words && !accessible(mask, words * 8, true)) return -14;
        for (uint64_t i = 0; i < words; i++) { uint64_t word = !i && with_nodes ? 1 : 0; if (uput64(mask + i * 8, word)) return -14; }
    }
    return 0;
}

/* ---- I/O priority (ioprio_set/ioprio_get): kept per thread and inherited; the block layer here does not order
 * requests by it. An unset priority reads as Linux 6.x reports it: best effort, level (nice + 20) / 5. ---- */
int task_ioprio(uint32_t tid, int set, uint16_t* value, int* nice, uint32_t* uid);
uint32_t task_next_thread(uint32_t after);
uint32_t task_thread_group(uint32_t tid);
static uint16_t effective_ioprio(uint16_t value, int nice) { return value >> 13 ? value : (uint16_t)(2 << 13 | (unsigned)(nice + 20) / 5); }
static int64_t ioprio_call(bool set, int32_t which, int32_t who, uint32_t value) {
    if (which < 1 || which > 3) return -22;   /* IOPRIO_WHO_PROCESS, _PGRP, _USER */
    if (set) {
        uint32_t class = value >> 13 & 7, level = value & 7, hint = value >> 3 & 0x3FF;
        if (value > 0xFFFF || hint > 7) return -22;
        if (class == 1 && !credentials_capable(credentials_current(), NULL, 23) && !credentials_capable(credentials_current(), NULL, 21)) return -1;
        if ((class == 0 && (level || hint)) || class > 3) return -22;
    }
    struct credentials* c = credentials_current();
    uint32_t my_uid = credentials_kernel_id(c, false, 0), my_euid = credentials_kernel_id(c, false, 1);
    uint32_t single = 0, group = 0, user = credentials_kernel_id(c, false, 0);
    if (which == 1) { if (who < 0 || !(single = who ? task_internal_pid((uint32_t)who) : task_tid())) return -3; }
    if (which == 2) { if (who < 0) return -22; group = who ? task_internal_pid((uint32_t)who) : task_thread_group(task_tid()); if (!group) return -3; }
    if (which == 3 && who && !credentials_kernel_from_visible((uint32_t)who, false, &user)) return -3;
    bool any = false, denied = false, have = false; uint16_t best = 0;
    for (uint32_t tid = which == 1 ? single : task_next_thread(0); tid; tid = which == 1 ? 0 : task_next_thread(tid)) {
        uint16_t value_now; int nice; uint32_t uid;
        if (task_ioprio(tid, 0, &value_now, &nice, &uid)) continue;
        if ((which == 2 && task_thread_group(tid) != group) || (which == 3 && uid != user)) continue;
        any = true;
        if (set) {
            if (uid != my_uid && uid != my_euid && !credentials_capable(c, NULL, 23)) { denied = true; continue; }
            uint16_t v = (uint16_t)value; task_ioprio(tid, 1, &v, NULL, NULL);
        } else {   /* PGRP/USER: the best (lowest class, then lowest level) priority, as Linux ioprio_best */
            uint16_t e = effective_ioprio(value_now, nice);
            if (!have || (e >> 13) < (best >> 13) || ((e >> 13) == (best >> 13) && (e & 7) < (best & 7))) best = e;
            have = true;
        }
    }
    if (!any) return -3;
    if (set) return denied ? -1 : 0;
    return best;
}

/* epoll_pwait2 (fcitx5's sd-event waits with it): epoll_pwait with a timespec timeout, rounded up to the 1 ms
 * granularity of the kernel's timeouts (NULL: wait without a timeout, {0, 0}: poll). */
static int64_t epoll_pwait2_call(struct registers* r) {
    int64_t ms = -1;
    if (r->r10) {
        if (!accessible(r->r10, 16, false)) return -14;
        int64_t ts[2]; copy_from_user(ts, (const void*)r->r10, sizeof(ts));
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) return -22;
        uint64_t whole = (uint64_t)ts[0] > 0x7FFFFFFFULL / 1000 ? 0x7FFFFFFFULL : (uint64_t)ts[0] * 1000 + ((uint64_t)ts[1] + 999999) / 1000000;
        ms = whole > 0x7FFFFFFFULL ? 0x7FFFFFFF : (int64_t)whole;
    }
    struct registers call = *r; call.rax = 281; call.r10 = (uint64_t)ms;
    return event_call(&call);
}
/* preadv/pwritev (295/296) and preadv2/pwritev2 (327/328): vectored pread/pwrite (PostgreSQL writes its WAL with
 * pwritev). The offset is pos_l (x86-64: pos_h is ignored); preadv2/pwritev2 offset -1 uses and moves the file position;
 * RWF_HIPRI/DSYNC/SYNC/NOWAIT are accepted (DSYNC/SYNC write the data out), other flags EOPNOTSUPP. Each vector goes
 * through the description's pread/pwrite path (fd_transfer); a short transfer ends the call. */
struct memory_file* kernel_file_cache(struct open_file* f, uint64_t* flags, const char** path);
static int64_t vectored_call(uint64_t fd, uint64_t address, uint64_t count, int64_t position, uint64_t flags, bool write, bool two) {
    struct open_file* f = fd_lookup(fd); if (!f) return -9;
    uint64_t status = fd_status_flags(f);
    if ((status & 0x200000) || (write ? !(status & 3) : (status & 3) == 1)) return -9;
    if (count > 1024) return -22;
    if (flags & ~0xFULL) return -95;   /* RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_NOWAIT */
    bool positional = !(two && position == -1);
    if (positional && position < 0) return -22;
    if (count && !accessible(address, count * 16, false)) return -14;
    uint64_t total = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t size = uget64(address + i * 16 + 8);
        if (size > 0x7FFFFFFFFFFFFFFFULL - total) return -22;
        total += size;
    }
    int64_t done = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t base = uget64(address + i * 16), size = uget64(address + i * 16 + 8);
        if (!size) continue;
        int64_t part = fd_transfer(f, base, size, positional ? (uint64_t)position + (uint64_t)done : 0, positional, write);
        if (part < 0) { if (!done) done = part; break; }
        done += part;
        if ((uint64_t)part != size) break;
    }
    if (write && done > 0 && (flags & 6)) { struct memory_file* cache = kernel_file_cache(f, NULL, NULL); if (cache && memory_file_sync(cache, 0, ~0ULL)) return -5; }
    return done;
}
/* PR_SET_PDEATHSIG / PR_GET_PDEATHSIG (credentials.c prctl): a signal per thread, sent to the thread's process when
 * its parent process is gone (process.c reparenting; Linux: when the parent *thread* exits, which is the same for a
 * single-threaded parent such as PostgreSQL's postmaster). Not inherited by fork, kept over exec. */
#define PDEATH_MAX 1024
static struct { uint32_t tid; uint8_t number; } pdeaths[PDEATH_MAX];
static unsigned pdeath_count;
static int pdeath_slot(uint32_t tid) { for (unsigned i = 0; i < pdeath_count; i++) if (pdeaths[i].tid == tid) return (int)i; return -1; }
int64_t kabi_pdeath_set(uint64_t number) {
    if (number > 64) return -22;
    uint32_t tid = task_tid(); int slot = pdeath_slot(tid);
    if (!number) { if (slot >= 0) pdeaths[slot] = pdeaths[--pdeath_count]; return 0; }
    if (slot < 0) { if (pdeath_count == PDEATH_MAX) return -12; slot = (int)pdeath_count++; pdeaths[slot].tid = tid; }
    pdeaths[slot].number = (uint8_t)number; return 0;
}
int64_t kabi_pdeath_get(uint64_t address) {
    int slot = pdeath_slot(task_tid()); int32_t number = slot >= 0 ? pdeaths[slot].number : 0;
    if (!accessible(address, 4, true)) return -14;
    return uput32(address, (uint32_t)number);
}
unsigned kabi_pdeath_signal(uint32_t tid) { int slot = pdeath_slot(tid); return slot >= 0 ? pdeaths[slot].number : 0; }
void kabi_pdeath_forget(uint32_t tid) { if (!pdeath_count) return; int slot = pdeath_slot(tid); if (slot >= 0) pdeaths[slot] = pdeaths[--pdeath_count]; }
int64_t kabi_call(struct registers* r) {
    switch (r->rax) {
    case 295: case 296: return vectored_call(r->rdi, r->rsi, r->rdx, (int64_t)r->r10, 0, r->rax == 296, false);
    case 327: case 328: return vectored_call(r->rdi, r->rsi, r->rdx, (int64_t)r->r10, (uint32_t)r->r9, r->rax == 328, true);
    case 179: return quotactl_call((uint32_t)r->rdi, r->rsi, (uint32_t)r->rdx, r->r10);
    case 443: return quotactl_fd_call((uint32_t)r->rdi, (uint32_t)r->rsi, (uint32_t)r->rdx, r->r10);
    case 309: return getcpu_call(r->rdi, r->rsi);
    case 334: return -38;   /* rseq: see above */
    case 330: return pkey_alloc_call(r->rdi, r->rsi);
    case 331: return pkey_free_call((int32_t)r->rdi);
    case 237: return mbind_call(r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9);
    case 238: return set_mempolicy_call(r->rdi, r->rsi, r->rdx);
    case 239: return get_mempolicy_call(r->rdi, r->rsi, r->rdx, r->r10, r->r8);
    case 251: return ioprio_call(true, (int32_t)r->rdi, (int32_t)r->rsi, (uint32_t)r->rdx);
    case 252: return ioprio_call(false, (int32_t)r->rdi, (int32_t)r->rsi, 0);
    case 441: return epoll_pwait2_call(r);
    }
    return -38;
}

/* ---- 32-bit syscall arguments (native_dispatch) ----
 * Linux casts every system call argument to the type its SYSCALL_DEFINE declares: an `int` or `unsigned int`
 * argument ignores the upper half of its register, which the x86-64 C ABI leaves undefined (Firefox's
 * memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL) arrived as 0x1_0000000B and failed with
 * EINVAL here, then MOZ_CRASHed). The handlers here read whole registers, so native_dispatch narrows those arguments
 * in the frame first and restores the caller's registers afterwards (the ABI preserves them): an int holding a number
 * (fd, dirfd, pid, signal, timeout, clock id) is sign-extended, exactly as glibc passes it; unsigned int, uid_t, u32
 * and int flag/mode/option sets are zero-extended, so a handler's 64-bit unknown-bit test sees the 32 bits Linux
 * looks at (__WCLONE is bit 31). seccomp filters still see the full registers, as on Linux. Table: bits 0-5 = the
 * argument is 32 bits wide, bits 8-13 = it is sign-extended. execve(at), clone, exit, rt_sigreturn are not listed. */
#define U32(n) (1u << (n))
#define I32(n) (0x101u << (n))
static const uint16_t narrow_table[453] = {
    [0] = U32(0), [1] = U32(0), [2] = U32(1), [3] = U32(0), [5] = U32(0), [7] = U32(1) | I32(2),
    [8] = U32(0) | U32(2), [13] = I32(0), [14] = I32(0), [16] = U32(0) | U32(1), [17] = U32(0), [18] = U32(0),
    [21] = U32(1), [23] = I32(0), [26] = U32(2), [28] = I32(2), [32] = U32(0), [33] = U32(0) | U32(1), [36] = I32(0),
    [37] = U32(0), [38] = I32(0), [40] = I32(0) | I32(1), [41] = I32(0) | U32(1) | I32(2), [42] = I32(0) | I32(2),
    [43] = I32(0), [44] = I32(0) | U32(3) | I32(5), [45] = I32(0) | U32(3), [46] = I32(0) | U32(2),
    [47] = I32(0) | U32(2), [48] = I32(0) | I32(1), [49] = I32(0) | I32(2), [50] = I32(0) | I32(1), [51] = I32(0),
    [52] = I32(0), [53] = I32(0) | U32(1) | I32(2), [54] = I32(0) | I32(1) | I32(2) | I32(4),
    [55] = I32(0) | I32(1) | I32(2), [61] = I32(0) | U32(2), [62] = I32(0) | I32(1), [72] = U32(0) | U32(1),
    [73] = U32(0) | U32(1), [74] = U32(0), [75] = U32(0), [77] = U32(0), [81] = U32(0), [89] = I32(2), [91] = U32(0),
    [92] = U32(1) | U32(2), [93] = U32(0) | U32(1) | U32(2), [94] = U32(1) | U32(2), [95] = U32(0), [97] = U32(0),
    [98] = I32(0), [103] = I32(0) | I32(2), [105] = U32(0), [106] = U32(0), [109] = I32(0) | I32(1),
    [113] = U32(0) | U32(1), [114] = U32(0) | U32(1), [115] = I32(0), [116] = I32(0),
    [117] = U32(0) | U32(1) | U32(2), [119] = U32(0) | U32(1) | U32(2), [121] = I32(0), [122] = U32(0),
    [123] = U32(0), [124] = I32(0), [129] = I32(0) | I32(1), [135] = U32(0), [138] = U32(0), [140] = I32(0) | I32(1),
    [141] = I32(0) | I32(1) | I32(2), [142] = I32(0), [143] = I32(0), [144] = I32(0) | I32(1), [145] = I32(0),
    [146] = I32(0), [147] = I32(0), [151] = U32(0), [154] = I32(0), [157] = I32(0), [158] = I32(0), [160] = U32(0),
    [166] = U32(1), [167] = U32(1), [169] = U32(0) | U32(1) | U32(2), [170] = I32(1), [171] = I32(1),
    [179] = U32(0) | U32(2), [187] = I32(0), [188] = U32(4), [189] = U32(4), [190] = I32(0) | U32(4), [193] = I32(0),
    [196] = I32(0), [199] = I32(0), [200] = I32(0) | I32(1), [202] = U32(1), [203] = I32(0) | U32(1),
    [204] = I32(0) | U32(1), [213] = I32(0), [217] = U32(0), [221] = I32(0) | I32(3), [222] = I32(0),
    [223] = I32(0) | U32(1), [224] = I32(0), [225] = I32(0), [226] = I32(0), [227] = I32(0), [228] = I32(0),
    [229] = I32(0), [230] = I32(0) | U32(1), [232] = I32(0) | I32(2) | I32(3), [233] = I32(0) | I32(1) | I32(2),
    [234] = I32(0) | I32(1) | I32(2), [237] = U32(5), [238] = U32(0), [247] = I32(0) | I32(1) | U32(3),
    [248] = I32(4), [249] = I32(3), [250] = I32(0), [251] = I32(0) | I32(1) | U32(2), [252] = I32(0) | I32(1),
    [254] = I32(0) | U32(2), [255] = I32(0) | I32(1), [257] = I32(0) | U32(2), [258] = I32(0),
    [259] = I32(0) | U32(3), [260] = I32(0) | U32(2) | U32(3) | U32(4), [261] = I32(0), [262] = I32(0) | U32(3),
    [263] = I32(0) | U32(2), [264] = I32(0) | I32(2), [265] = I32(0) | I32(2) | U32(4), [266] = I32(1),
    [267] = I32(0) | I32(3), [268] = I32(0), [269] = I32(0) | U32(2), [270] = I32(0), [271] = U32(1),
    [277] = I32(0) | U32(3), [280] = I32(0) | U32(3), [281] = I32(0) | I32(2) | I32(3), [282] = I32(0),
    [283] = I32(0) | U32(1), [284] = U32(0), [285] = I32(0) | U32(1), [286] = I32(0) | U32(1), [287] = I32(0),
    [288] = I32(0) | U32(3), [289] = I32(0) | U32(3), [290] = U32(0) | U32(1), [291] = U32(0),
    [292] = U32(0) | U32(1) | U32(2), [293] = U32(1), [294] = U32(0), [297] = I32(0) | I32(1) | I32(2),
    [299] = I32(0) | U32(2) | U32(3), [302] = I32(0) | U32(1), [303] = I32(0) | U32(4), [304] = I32(0) | U32(2),
    [305] = I32(0), [306] = I32(0), [307] = I32(0) | U32(2) | U32(3), [308] = I32(0) | U32(1),
    [316] = I32(0) | I32(2) | U32(4), [317] = U32(0) | U32(1), [318] = U32(2), [319] = U32(1), [325] = U32(2),
    [326] = I32(0) | I32(2) | U32(5), [327] = U32(5), [328] = U32(5), [329] = I32(3), [331] = I32(0),
    [332] = I32(0) | U32(2) | U32(3), [424] = I32(0) | I32(1) | U32(3), [434] = I32(0) | U32(1),
    [436] = U32(0) | U32(1) | U32(2), [437] = I32(0), [438] = I32(0) | I32(1) | U32(2),
    [439] = I32(0) | U32(2) | U32(3), [441] = I32(0) | I32(2), [443] = U32(0) | U32(1) | U32(2),
    [452] = I32(0) | U32(3)
};
uint16_t kabi_narrow(struct registers* r, uint64_t saved[6]) {
    if (r->rax >= sizeof(narrow_table) / sizeof(narrow_table[0])) return 0;
    uint16_t mask = narrow_table[r->rax]; if (!mask) return 0;
    uint64_t* args[6] = {&r->rdi, &r->rsi, &r->rdx, &r->r10, &r->r8, &r->r9};
    for (unsigned i = 0; i < 6; i++) {
        saved[i] = *args[i];
        if (mask >> i & 1) *args[i] = mask >> (8 + i) & 1 ? (uint64_t)(int64_t)(int32_t)*args[i] : (uint32_t)*args[i];
    }
    return mask;
}
void kabi_restore(struct registers* r, uint16_t mask, const uint64_t saved[6]) {
    uint64_t* args[6] = {&r->rdi, &r->rsi, &r->rdx, &r->r10, &r->r8, &r->r9};
    for (unsigned i = 0; i < 6; i++) if (mask >> i & 1) *args[i] = saved[i];
}
