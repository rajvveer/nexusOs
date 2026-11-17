/* procfs files desktop applications read: /proc/{stat,uptime,loadavg,version,filesystems,mounts},
 * /proc/sys/{fs,kernel,user,vm}/..., and per-process status, stat, statm, cmdline, environ, comm, maps,
 * mounts, mountinfo and oom_score_adj (proc.c resolves the paths and delegates the contents here). */
#include "native.h"
#include "uaccess.h"
#include "../../kernel/string.h"
#include "swap.h"

/* Global entries; a trailing '/' marks a directory. */
static const char* const globals[] = {
    "stat", "uptime", "loadavg", "version", "filesystems", "mounts", "sys/",
    "sys/fs/", "sys/fs/inotify/", "sys/fs/inotify/max_user_watches", "sys/fs/inotify/max_user_instances",
    "sys/fs/inotify/max_queued_events", "sys/fs/file-max", "sys/fs/nr_open",
    "sys/kernel/", "sys/kernel/osrelease", "sys/kernel/ostype", "sys/kernel/hostname", "sys/kernel/pid_max",
    "sys/kernel/threads-max", "sys/kernel/ngroups_max", "sys/kernel/cap_last_cap", "sys/kernel/unprivileged_userns_clone",
    "sys/kernel/random/", "sys/kernel/random/boot_id", "sys/kernel/random/uuid", "sys/kernel/yama/", "sys/kernel/yama/ptrace_scope",
    "sys/kernel/randomize_va_space", "sys/kernel/kptr_restrict", "sys/kernel/dmesg_restrict",
    "sys/kernel/seccomp/", "sys/kernel/seccomp/actions_avail", "sys/kernel/seccomp/actions_logged",
    "sys/user/", "sys/user/max_user_namespaces", "sys/vm/", "sys/vm/overcommit_memory", "sys/vm/max_map_count",
    "swaps", "vmstat", "zoneinfo", "sys/vm/swappiness", "sys/vm/drop_caches", "sys/vm/min_free_kbytes", "sys/vm/page-cluster",
    "sys/vm/watermark_scale_factor",   /* swap.c, reclaim.c */
    "net/", "net/dev", "net/route", "net/arp", "net/tcp", "net/udp", "net/snmp",   /* netlink.c: netif_proc_text */
    "net/if_inet6", "net/ipv6_route", "net/tcp6", "net/udp6", "net/raw", "net/raw6", "net/icmp", "net/icmp6", "net/snmp6",
    "net/igmp", "net/igmp6", "net/packet", "net/netstat", "net/sockstat", "net/sockstat6", "net/dev_mcast", "net/nf_conntrack",
    "net/unix", "net/protocols", "net/wireless", "net/mcfilter", "net/mcfilter6",
    "kallsyms",                                                                     /* kernel symbols (addresses gated by kptr_restrict + CAP_SYSLOG) */
    "cmdline",                                                                    /* the kernel command line (bootslot.c) */
    "partitions",                                                                   /* blkdev_node.c */
    "mdstat",                                                                       /* md.c */
    "devices", "misc",                                                              /* majors: libdevmapper finds device-mapper here */
    "sysvipc/", "sysvipc/shm", "sysvipc/sem", "sysvipc/msg", "sys/kernel/sem", "sys/kernel/msgmax", "sys/kernel/msgmnb",   /* sysvipc.c */
    "sys/kernel/msgmni", "sys/kernel/shmmax", "sys/kernel/shmall", "sys/kernel/shmmni", "sys/kernel/shm_rmid_forced",
    "interrupts", "timer_list",                                                     /* tick.c: interrupt counts, tickless idle state */
};
unsigned storage_proc_partitions(char* out, unsigned capacity);
__attribute__((weak)) unsigned md_proc_mdstat(char* out, unsigned capacity) { out[0] = 0; (void)capacity; return 0; }
#define GLOBALS (sizeof(globals) / sizeof(globals[0]))
static const char* const process_files[] = {"status", "stat", "statm", "cmdline", "comm", "environ", "maps", "mounts", "mountinfo", "oom_score_adj", "mem",
                                            "smaps", "smaps_rollup", "oom_score"};
#define TASK_FILES (sizeof(process_files) / sizeof(process_files[0]))
enum { STATUS, STAT, STATM, CMDLINE, COMM, ENVIRON, MAPS, MOUNTS, MOUNTINFO, OOM_SCORE_ADJ, MEM, SMAPS, SMAPS_ROLLUP, OOM_SCORE };

static unsigned entry_length(const char* entry) { unsigned n = strlen(entry); return n && entry[n - 1] == '/' ? n - 1 : n; }
int procext_global(const char* path) {
    int net = netproc_lookup(path);   /* /proc/sys/net (netproc.c) */
    if (net >= 0) return (int)(NETPROC_BASE + (unsigned)net);
    for (unsigned i = 0; i < GLOBALS; i++) {
        unsigned n = entry_length(globals[i]);
        if (strlen(path) == n && !strncmp(path, globals[i], n)) return (int)i;
    }
    return -1;
}
bool procext_global_directory(unsigned index) {
    if (index >= NETPROC_BASE) return netproc_directory(index - NETPROC_BASE);
    return index < GLOBALS && globals[index][strlen(globals[index]) - 1] == '/';
}
const char* procext_global_path(unsigned index, unsigned* length) {
    if (index >= NETPROC_BASE) { static char path[128]; if (!netproc_path(index - NETPROC_BASE, path)) path[0] = 0; *length = strlen(path); return path; }
    *length = entry_length(globals[index]); return globals[index];
}
/* Children of a global directory (`parent` -1: the /proc root), in table order. */
int procext_global_child(int parent, uint64_t position, char* name, bool* directory, unsigned* index) {
    if (parent >= (int)NETPROC_BASE) {
        int found = netproc_child((unsigned)parent - NETPROC_BASE, position, name, directory, index);
        if (found > 0) *index += NETPROC_BASE;
        return found;
    }
    const char* prefix = parent < 0 ? "" : globals[parent]; unsigned prefix_size = parent < 0 ? 0 : strlen(prefix);
    uint64_t seen = 0;
    for (unsigned i = 0; i < GLOBALS; i++) {
        unsigned n = entry_length(globals[i]);
        if (n <= prefix_size || strncmp(globals[i], prefix, prefix_size)) continue;
        bool nested = false; for (unsigned k = prefix_size; k < n; k++) if (globals[i][k] == '/') nested = true;
        if (nested || seen++ != position) continue;
        memcpy(name, globals[i] + prefix_size, n - prefix_size); name[n - prefix_size] = 0;
        *directory = procext_global_directory(i); *index = i; return 1;
    }
    if (parent >= 0 && !strcmp(prefix, "sys/") && seen == position) {   /* /proc/sys/net (netproc.c) */
        strcpy(name, "net"); *directory = true; *index = NETPROC_BASE + 1; return 1;
    }
    return 0;
}
int procext_task(const char* name) {
    for (unsigned i = 0; i < TASK_FILES; i++) if (!strcmp(name, process_files[i])) return (int)i;
    return -1;
}
const char* procext_task_name(unsigned index) { return index < TASK_FILES ? process_files[index] : NULL; }
static bool vm_sysctl(unsigned index) { return index < GLOBALS && !strncmp(globals[index], "sys/vm/", 7) && globals[index][7] && strcmp(globals[index], "sys/vm/overcommit_memory") && strcmp(globals[index], "sys/vm/max_map_count"); }
uint32_t procext_mode(bool task, unsigned index) {
    if (!task && index >= NETPROC_BASE) return netproc_mode(index - NETPROC_BASE);
    if (!task && index < GLOBALS && sysvipc_proc_mode(globals[index])) return sysvipc_proc_mode(globals[index]);
    if (!task && vm_sysctl(index)) return !strcmp(globals[index], "sys/vm/drop_caches") ? 0100200 : 0100644;   /* reclaim.c sysctls */
    if (!task && index < GLOBALS) {
        const char* n = globals[index];
        if (!strcmp(n, "sys/kernel/seccomp/actions_logged") || !strcmp(n, "sys/kernel/kptr_restrict") ||
            !strcmp(n, "sys/kernel/dmesg_restrict") || !strcmp(n, "sys/kernel/randomize_va_space")) return 0100644;
        if (!strcmp(n, "kallsyms")) return native_get_kptr_restrict() >= 2 ? 0100400 : 0100444;   /* kptr_restrict=2: root only */
    }
    return task && (index == OOM_SCORE_ADJ || index == COMM) ? 0100644 : task && index == MEM ? 0100600 : 0100444;
}
/* /proc/sys/vm writes (proc.c checked the mode for the opener): the value from the start of the buffer. */
int64_t procext_write_global(unsigned index, uint64_t address, uint64_t count) {
    if (index < GLOBALS && sysvipc_proc_mode(globals[index]) == 0100644) {   /* sysvipc.c sysctls */
        char text[96]; if (!count) return 0; if (count >= sizeof(text)) return -22;
        if (!accessible(address, count, false)) return -14;
        copy_from_user(text, (const void*)address, count); text[count] = 0;
        int64_t error = sysvipc_sysctl_write(globals[index], text); return error ? error : (int64_t)count;
    }
    if (index < GLOBALS && !strcmp(globals[index], "sys/kernel/seccomp/actions_logged")) {   /* seccomp.c */
        if (!count || count > 128) return -22;
        char value[129]; if (!accessible(address, count, false)) return -14;
        copy_from_user(value, (void*)address, count); value[count] = 0;
        int error = seccomp_actions_logged_write(value); return error ? error : (int64_t)count;
    }
    if (index < GLOBALS && (!strcmp(globals[index], "sys/kernel/kptr_restrict") || !strcmp(globals[index], "sys/kernel/dmesg_restrict") ||
                            !strcmp(globals[index], "sys/kernel/randomize_va_space"))) {   /* root-writable (mode 0644), 0..2/0..1 */
        if (!credentials_capable(credentials_current(), NULL, 21 /* CAP_SYS_ADMIN */)) return -1;
        if (!count || count > 16) return -22;
        char value[17]; if (!accessible(address, count, false)) return -14;
        copy_from_user(value, (void*)address, count); value[count] = 0;
        if (value[0] < '0' || value[0] > '9') return -22;
        int v = value[0] - '0';
        if (!strcmp(globals[index], "sys/kernel/kptr_restrict")) { if (v > 2) return -22; native_set_kptr_restrict(v); }
        else if (!strcmp(globals[index], "sys/kernel/dmesg_restrict")) { if (v > 1) return -22; native_set_dmesg_restrict(v); }
        else { if (v > 2) return -22; native_set_randomize(v); }
        return (int64_t)count;
    }
    if (!vm_sysctl(index)) return -13;
    if (!count) return 0;
    if (count > 64) return -22;
    char value[65]; if (!accessible(address, count, false)) return -14;
    copy_from_user(value, (void*)address, count); value[count] = 0;
    char unused[8]; int64_t result = reclaim_sysctl(globals[index] + 7, unused, sizeof(unused), value, (unsigned)count);
    return result < 0 ? result : (int64_t)count;
}
/* /proc/PID/mem: the memory of the address space bound when the file was opened (proc.c checked ptrace access for
 * the opener then, as Linux does), at file offset = address; 0 bytes copied is EIO. */
static int64_t procext_mem(struct address_space* space, uint64_t address, uint64_t count, uint64_t offset, bool write) {
    if (!count) return 0;
    if (count > 0x7FFFF000) count = 0x7FFFF000;
    uint8_t* bounce = (void*)page_alloc(); if (!bounce) return -12;
    uint64_t done = 0;
    while (done < count) {
        uint64_t part = count - done < 4096 ? count - done : 4096;
        if (!accessible(address + done, part, !write)) { if (!done) done = (uint64_t)-14; break; }
        if (write && copy_from_user(bounce, (void*)(address + done), part)) { if (!done) done = (uint64_t)-14; break; }
        int64_t moved = space_access(space, offset + done, bounce, part, write);   /* may fault pages in and sleep */
        if (!write && moved > 0 && !accessible(address + done, (uint64_t)moved, true)) { if (!done) done = (uint64_t)-14; break; }
        if (!write && moved > 0 && copy_to_user((void*)(address + done), bounce, (uint64_t)moved)) { if (!done) done = (uint64_t)-14; break; }
        if (moved > 0) done += (uint64_t)moved;
        if (moved < (int64_t)part) { if (!done) done = (uint64_t)-5; break; }
    }
    page_free((uint64_t)bounce); return (int64_t)done;
}

/* ---- text generation into the caller's window [offset, offset + capacity) ---- */
struct output { uint64_t skip, address, capacity, copied; int error; };
static void emit(struct output* out, const char* text, uint64_t length) {
    if (out->error) return;
    if (out->skip >= length) { out->skip -= length; return; }
    text += out->skip; length -= out->skip; out->skip = 0;
    uint64_t room = out->capacity - out->copied; if (length > room) length = room;
    if (!length) return;
    if (!accessible(out->address + out->copied, length, true)) { out->error = -14; return; }
    if (copy_to_user((void*)(out->address + out->copied), text, length)) { out->error = -14; return; } out->copied += length;
}
static void text(struct output* out, const char* value) { emit(out, value, strlen(value)); }
static void decimal(struct output* out, uint64_t value) { char number[24]; number64(value, number); text(out, number); }
static void signed_decimal(struct output* out, int64_t value) { if (value < 0) { text(out, "-"); decimal(out, (uint64_t)-value); } else decimal(out, (uint64_t)value); }
static void padded(struct output* out, uint64_t value, unsigned width) {
    char number[24]; number64(value, number);
    for (unsigned n = strlen(number); n < width; n++) text(out, " ");
    text(out, number);
}
static void hexadecimal(struct output* out, uint64_t value, unsigned width) {
    char digits[17]; for (unsigned i = 0; i < 16; i++) digits[i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    digits[16] = 0; text(out, digits + 16 - width);
}
static void hundredths(struct output* out, uint64_t ns) {
    decimal(out, ns / 1000000000ULL); text(out, ".");
    uint64_t fraction = ns % 1000000000ULL / 10000000ULL; if (fraction < 10) text(out, "0"); decimal(out, fraction);
}
static void uuid(struct output* out, const uint8_t bytes[16]) {
    for (unsigned i = 0; i < 16; i++) { if (i == 4 || i == 6 || i == 8 || i == 10) text(out, "-"); hexadecimal(out, bytes[i], 2); }
    text(out, "\n");
}
/* /proc/kallsyms: "ADDRESS TYPE name" per line, like Linux. The address is shown only when kptr_restrict is 0 and the
 * reader has CAP_SYSLOG; otherwise it is all-zero (the kernel.map on the host symbolizes offline). The symbol table is
 * the compact blob linked into the .kallsyms section (gen_kallsyms.py, build.py); weak symbols so a kernel built
 * without it just yields an empty file. Addresses assume no KASLR slide (the kernel runs at its link addresses). */
extern const uint8_t kallsyms_blob[] __attribute__((weak));
extern const uint8_t kallsyms_blob_end[] __attribute__((weak));
static void kallsyms_emit(struct output* out) {
    const uint8_t* p = kallsyms_blob; const uint8_t* end = kallsyms_blob_end;
    if (!p || p >= end) return;   /* no table linked in */
    bool reveal = native_get_kptr_restrict() == 0 && credentials_capable(credentials_current(), NULL, 34 /* CAP_SYSLOG */);
    uint32_t count; memcpy(&count, p, 4); p += 4;
    for (uint32_t i = 0; i < count && !out->error && out->copied < out->capacity; i++) {
        if (p + 10 > end) break;
        uint64_t addr; memcpy(&addr, p, 8); uint8_t type = p[8], len = p[9]; p += 10;
        if (p + len > end) break;
        hexadecimal(out, reveal ? addr : 0, 16);
        text(out, " "); { char t[2] = {(char)type, 0}; text(out, t); } text(out, " ");
        char name[256]; memcpy(name, p, len); name[len] = 0; text(out, name); text(out, "\n");
        p += len;
    }
}
/* The mount table (mount.c): the root, the virtual filesystems and every mounted filesystem. In mountinfo the
 * ids match name_to_handle_at/statx (20 + device) and device numbers match stat st_dev. */
unsigned mount_proc_text(char* out, unsigned capacity, bool mountinfo);
static void mount_table(struct output* out, bool info) {
    char* buffer = (void*)arena_alloc(32768); if (!buffer) { out->error = -12; return; }
    unsigned length = mount_proc_text(buffer, 32768, info); emit(out, buffer, length);
    arena_free((uint64_t)buffer, 32768);
}
static void mounts(struct output* out) { mount_table(out, false); }
static void mountinfo(struct output* out) { mount_table(out, true); }
static uint8_t boot_id[16]; static bool boot_id_ready;
static void global_file(struct output* out, unsigned index) {
    const char* name = globals[index];
    if (!strcmp(name, "stat")) {
        uint64_t busy[MAX_CPUS] = {0}, uptime = native_clock_ns(), total_busy = 0, total_idle = 0; unsigned running = 0, count = 0;
        task_cpu_usage(busy, &running, &count);
        unsigned online = 0; for (unsigned c = 0; c < MAX_CPUS; c++) if (cpus[c].online || c == 0) online = c + 1;
        for (unsigned c = 0; c < online; c++) { total_busy += busy[c]; total_idle += uptime > busy[c] ? uptime - busy[c] : 0; }
        text(out, "cpu  "); decimal(out, total_busy / 10000000); text(out, " 0 0 "); decimal(out, total_idle / 10000000); text(out, " 0 0 0 0 0 0\n");
        for (unsigned c = 0; c < online; c++) {
            text(out, "cpu"); decimal(out, c); text(out, " "); decimal(out, busy[c] / 10000000); text(out, " 0 0 ");
            decimal(out, (uptime > busy[c] ? uptime - busy[c] : 0) / 10000000); text(out, " 0 0 0 0 0 0\n");
        }
        text(out, "intr "); decimal(out, kstat.irqs); text(out, "\nctxt "); decimal(out, kstat.switches);
        text(out, "\nbtime "); decimal(out, (native_realtime_ns() - uptime) / 1000000000ULL);
        text(out, "\nprocesses "); decimal(out, kstat.forks + 1); text(out, "\nprocs_running "); decimal(out, running);
        text(out, "\nprocs_blocked 0\nsoftirq 0 0 0 0 0 0 0 0 0 0 0\n");
    } else if (!strcmp(name, "uptime")) {
        uint64_t busy[MAX_CPUS] = {0}, uptime = native_clock_ns(), idle = 0; unsigned running, count;
        task_cpu_usage(busy, &running, &count);
        for (unsigned c = 0; c < MAX_CPUS; c++) if (cpus[c].online || c == 0) idle += uptime > busy[c] ? uptime - busy[c] : 0;
        hundredths(out, uptime); text(out, " "); hundredths(out, idle); text(out, "\n");
    } else if (!strcmp(name, "loadavg")) {
        /* Load averages are not sampled (sysinfo reports the same zeros). */
        uint64_t busy[MAX_CPUS]; unsigned running = 0, count = 0; task_cpu_usage(busy, &running, &count);
        text(out, "0.00 0.00 0.00 "); decimal(out, running); text(out, "/"); decimal(out, count); text(out, " "); decimal(out, kstat.forks + 1); text(out, "\n");
    } else if (!strcmp(name, "version")) text(out, "Linux version 6.12.0-nexusos-amd64 (nexus@localhost) (zig cc 0.15.2) #1 SMP PREEMPT_DYNAMIC NexusOS\n");
    else if (!strcmp(name, "cmdline")) { text(out, boot_cmdline()); text(out, "\n"); }
    else if (sysvipc_proc_mode(name) && strcmp(name, "sysvipc/")) {   /* sysvipc.c: tables and sysctls of the caller's namespace */
        char* buffer = (char*)arena_alloc(262144);
        if (!buffer) out->error = -12;
        else { int n = sysvipc_proc_text(name, buffer, 262144); if (n < 0) out->error = n; else emit(out, buffer, (uint64_t)n); arena_free((uint64_t)buffer, 262144); }
    }
    else if (!strcmp(name, "filesystems")) text(out, "nodev\tsysfs\nnodev\tproc\nnodev\ttmpfs\nnodev\tdevtmpfs\nnodev\tdevpts\nnodev\tpipefs\nnodev\tsockfs\n\tvfat\n"
                                                   "\text4\n\texfat\n\tiso9660\nnodev\tfuse\n\tfuseblk\nnodev\tfusectl\n");
    else if (!strcmp(name, "mounts")) mounts(out);
    else if (!strcmp(name, "partitions")) { char buffer[4096]; storage_proc_partitions(buffer, sizeof(buffer)); text(out, buffer); }
    else if (!strcmp(name, "mdstat")) { char buffer[4096]; md_proc_mdstat(buffer, sizeof(buffer)); text(out, buffer); }
    else if (!strcmp(name, "devices")) text(out, "Character devices:\n  1 mem\n  4 tty\n  5 /dev/tty\n  5 /dev/console\n  5 /dev/ptmx\n 10 misc\n 13 input\n"
                                               " 29 fb\n116 alsa\n136 pts\n226 drm\n253 rtc\n\nBlock devices:\n  7 loop\n  8 sd\n  9 md\n 11 sr\n253 device-mapper\n259 blkext\n");
    else if (!strcmp(name, "misc")) text(out, "229 fuse\n236 device-mapper\n237 loop-control\n");
    else if (!strcmp(name, "interrupts") || !strcmp(name, "timer_list")) {   /* tick.c */
        unsigned tick_proc_interrupts(char* out, unsigned capacity), tick_proc_timer_list(char* out, unsigned capacity);
        char* buffer = (char*)arena_alloc(16384); if (!buffer) { out->error = -12; return; }
        emit(out, buffer, name[0] == 'i' ? tick_proc_interrupts(buffer, 16384) : tick_proc_timer_list(buffer, 16384));
        arena_free((uint64_t)buffer, 16384);
    }
    else if (!strcmp(name, "swaps") || !strcmp(name, "vmstat") || !strcmp(name, "zoneinfo") || vm_sysctl(index)) {
        char* buffer = (void*)page_alloc(); if (!buffer) { out->error = -12; return; }
        int64_t n = !strcmp(name, "swaps") ? swap_proc_swaps(buffer, 4096) :
                    !strncmp(name, "sys/vm/", 7) ? reclaim_sysctl(name + 7, buffer, 4096, NULL, 0) : reclaim_proc_text(name, buffer, 4096);
        if (n < 0) out->error = (int)n; else emit(out, buffer, (uint64_t)n);
        page_free((uint64_t)buffer);
    }
    else if (!strncmp(name, "net/", 4)) {   /* netprocnet.c; tables can be long (sockets, routes) */
        char* buffer = (char*)arena_alloc(65536);
        if (!buffer) out->error = -12;
        else { if (netif_proc_text(name + 4, buffer, 65536)) text(out, buffer); else out->error = -2; arena_free((uint64_t)buffer, 65536); }
    }
    else if (!strcmp(name, "sys/kernel/random/boot_id") || !strcmp(name, "sys/kernel/random/uuid")) {
        uint8_t fresh[16], *value = fresh;
        if (!strcmp(name, "sys/kernel/random/boot_id")) {
            if (!boot_id_ready && native_random(boot_id, 16)) { boot_id[6] = (boot_id[6] & 15) | 0x40; boot_id[8] = (boot_id[8] & 0x3F) | 0x80; boot_id_ready = true; }
            if (!boot_id_ready) { out->error = -11; return; }
            value = boot_id;
        } else {
            if (!native_random(fresh, 16)) { out->error = -11; return; }
            fresh[6] = (fresh[6] & 15) | 0x40; fresh[8] = (fresh[8] & 0x3F) | 0x80;
        }
        uuid(out, value);
    } else if (!strcmp(name, "sys/kernel/randomize_va_space")) {
        decimal(out, (uint64_t)(unsigned)native_get_randomize()); text(out, "\n");   /* ASLR level 0/1/2, honoured by the ELF loader */
    } else if (!strcmp(name, "sys/kernel/kptr_restrict")) {
        decimal(out, (uint64_t)(unsigned)native_get_kptr_restrict()); text(out, "\n");
    } else if (!strcmp(name, "sys/kernel/dmesg_restrict")) {
        decimal(out, (uint64_t)(unsigned)native_get_dmesg_restrict()); text(out, "\n");
    } else if (!strcmp(name, "kallsyms")) {
        kallsyms_emit(out);
    } else if (!strcmp(name, "sys/kernel/seccomp/actions_avail") || !strcmp(name, "sys/kernel/seccomp/actions_logged")) {
        char buffer[128]; unsigned n = seccomp_actions_text(name[18] == 'l', buffer, sizeof(buffer)); emit(out, buffer, n);   /* seccomp.c */
    } else if (!strcmp(name, "sys/kernel/yama/ptrace_scope")) {
        decimal(out, (uint64_t)(unsigned)native_yama_scope()); text(out, "\n");       /* Yama ptrace restriction (process.c) */
    } else if (!strcmp(name, "sys/kernel/hostname") || !strcmp(name, "sys/kernel/domainname")) {
        text(out, name[11] == 'h' ? uts_hostname() : uts_domainname()); text(out, "\n");   /* sethostname (uts.c) */
    } else {
        static const struct { const char* name; const char* value; } values[] = {
            {"sys/fs/inotify/max_user_watches", "65536\n"}, {"sys/fs/inotify/max_user_instances", "128\n"},
            {"sys/fs/inotify/max_queued_events", "16384\n"}, {"sys/fs/file-max", "9223372036854775807\n"}, {"sys/fs/nr_open", "1048576\n"},
            {"sys/kernel/osrelease", "6.12.0-nexusos-amd64\n"}, {"sys/kernel/ostype", "Linux\n"},
            {"sys/kernel/pid_max", "4194304\n"}, {"sys/kernel/threads-max", "63431\n"}, {"sys/kernel/ngroups_max", "65536\n"},
            {"sys/kernel/cap_last_cap", "40\n"}, {"sys/kernel/unprivileged_userns_clone", "1\n"},
            {"sys/user/max_user_namespaces", "15000\n"}, {"sys/vm/overcommit_memory", "0\n"}, {"sys/vm/max_map_count", "65530\n"},
        };
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) if (!strcmp(name, values[i].name)) { text(out, values[i].value); return; }
        out->error = -21;
    }
}
static void memory_range(struct output* out, struct address_space* space, uint64_t begin, uint64_t end) {
    uint8_t chunk[256];
    for (uint64_t at = begin; at < end && !out->error && out->copied < out->capacity;) {
        uint64_t part = end - at < sizeof(chunk) ? end - at : sizeof(chunk);
        if (out->skip >= part) { out->skip -= part; at += part; continue; }
        uint64_t got = space_read(space, at, chunk, part); if (!got) break;
        emit(out, (const char*)chunk, got); at += got;
        if (got < part) break;
    }
}
static void task_file(struct output* out, unsigned index, uint32_t tid, bool thread) {
    struct task_info info; if (!task_info(tid, thread, &info)) { out->error = -3; return; }
    uint64_t stack = 0, data = 0, size = info.space ? space_virtual_bytes(info.space, &stack, &data) : 0;
    uint64_t resident = info.space ? space_resident_pages(info.space) : 0, swapped = 0;
    if (index == STATUS && info.space) { struct vm_usage usage; vm_space_usage(info.space, &usage); swapped = usage.swap; }
    uint32_t foreground = 0, terminal = terminal_device(info.terminal, &foreground);
    uint32_t pid = task_visible_pid(info.pid), id = task_visible_pid(info.tid), ppid = info.ppid ? task_visible_pid(info.ppid) : 0;
    switch (index) {
    case STATUS:
        text(out, "Name:\t"); text(out, info.name); text(out, "\nState:\t"); emit(out, &info.state, 1);
        text(out, info.state == 'R' ? " (running)" : info.state == 'T' ? " (stopped)" : info.state == 'D' ? " (disk sleep)" : " (sleeping)");
        text(out, "\nTgid:\t"); decimal(out, pid); text(out, "\nNgid:\t0\nPid:\t"); decimal(out, thread ? id : pid);
        text(out, "\nPPid:\t"); decimal(out, ppid); text(out, "\nTracerPid:\t0\nUid:");
        for (unsigned k = 0; k < 4; k++) { text(out, "\t"); decimal(out, info.uid[k]); }
        text(out, "\nGid:"); for (unsigned k = 0; k < 4; k++) { text(out, "\t"); decimal(out, info.gid[k]); }
        text(out, "\nFDSize:\t64\nGroups:\t\nNStgid:\t"); decimal(out, pid); text(out, "\nNSpid:\t"); decimal(out, thread ? id : pid);
        text(out, "\nNSpgid:\t"); decimal(out, task_visible_pid(info.group)); text(out, "\nNSsid:\t"); decimal(out, task_visible_pid(info.session));
        text(out, "\nVmPeak:\t"); padded(out, size / 1024, 8); text(out, " kB\nVmSize:\t"); padded(out, size / 1024, 8);
        text(out, " kB\nVmLck:\t"); padded(out, info.space ? space_locked_bytes(info.space) / 1024 : 0, 8);
        text(out, " kB\nVmPin:\t       0 kB\nVmHWM:\t"); padded(out, resident * 4, 8);
        text(out, " kB\nVmRSS:\t"); padded(out, resident * 4, 8); text(out, " kB\nVmData:\t"); padded(out, data / 1024, 8);
        text(out, " kB\nVmStk:\t"); padded(out, stack / 1024, 8); text(out, " kB\nVmSwap:\t"); padded(out, swapped * 4, 8);
        text(out, " kB\nThreads:\t"); decimal(out, info.threads);
        { int sc_mode; unsigned sc_filters; task_seccomp_info(tid, &sc_mode, &sc_filters);
          text(out, "\nSeccomp:\t"); decimal(out, (uint64_t)sc_mode); text(out, "\nSeccomp_filters:\t"); decimal(out, sc_filters); }
        text(out, "\nCpus_allowed_list:\t0-"); decimal(out, cpu_count ? cpu_count - 1 : 0);
        text(out, "\nvoluntary_ctxt_switches:\t"); decimal(out, info.voluntary);
        text(out, "\nnonvoluntary_ctxt_switches:\t"); decimal(out, info.involuntary); text(out, "\n");
        break;
    case STAT: {
        uint64_t range[4] = {0}; if (info.space) space_arguments(info.space, range);
        decimal(out, thread ? id : pid); text(out, " ("); text(out, info.name); text(out, ") "); emit(out, &info.state, 1); text(out, " ");
        decimal(out, ppid); text(out, " "); decimal(out, task_visible_pid(info.group)); text(out, " "); decimal(out, task_visible_pid(info.session));
        text(out, " "); decimal(out, terminal); text(out, " "); signed_decimal(out, foreground ? (int64_t)foreground : -1);
        text(out, " 4194304 "); decimal(out, info.faults); text(out, " "); decimal(out, info.child_faults); text(out, " 0 0 ");
        decimal(out, info.cpu_ns / 10000000); text(out, " 0 "); decimal(out, info.child_cpu_ns / 10000000); text(out, " 0 ");
        signed_decimal(out, 20 + info.nice); text(out, " "); signed_decimal(out, info.nice); text(out, " "); decimal(out, info.threads);
        text(out, " 0 "); decimal(out, info.start_ns / 10000000); text(out, " "); decimal(out, size); text(out, " "); decimal(out, resident);
        text(out, " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 "); decimal(out, info.processor);
        text(out, " 0 0 0 0 0 0 0 0 "); decimal(out, range[0]); text(out, " "); decimal(out, range[1]); text(out, " ");
        decimal(out, range[2]); text(out, " "); decimal(out, range[3]); text(out, " 0\n");
        break;
    }
    case STATM:
        decimal(out, size / 4096); text(out, " "); decimal(out, resident); text(out, " 0 0 0 "); decimal(out, data / 4096); text(out, " 0\n");
        break;
    case CMDLINE: {
        uint64_t range[4] = {0}; if (info.space) space_arguments(info.space, range);
        memory_range(out, info.space, range[0], range[1]);
        break;
    }
    case COMM: text(out, info.name); text(out, "\n"); break;
    case MOUNTS: mounts(out); break;
    case MOUNTINFO: mountinfo(out); break;
    case OOM_SCORE_ADJ: signed_decimal(out, info.oom_score_adj); text(out, "\n"); break;
    case OOM_SCORE: { uint64_t score = 0; if (info.space) reclaim_oom_badness(info.space, info.oom_score_adj, &score); decimal(out, score); text(out, "\n"); break; }
    case SMAPS: case SMAPS_ROLLUP: {   /* reads the live space (Linux binds it at open; proc.c does that for maps) */
        if (!task_inspect_allowed(tid)) { out->error = -13; break; }   /* addresses: ptrace read access, like maps */
        char* buffer = (void*)page_alloc(); if (!buffer) { out->error = -12; break; }
        for (uint64_t i = 0; info.space && !out->error && out->copied < out->capacity; i++) {
            int n = space_smaps_entry(info.space, i, buffer, 4096, index == SMAPS_ROLLUP); if (n <= 0) break;
            emit(out, buffer, (uint64_t)n);
        }
        page_free((uint64_t)buffer); break;
    }
    }
}
/* mem, environ and maps read the address space bound at open (proc.c image files: ptrace access was checked for the
 * opener then, and the file ends with that image, as Linux binds the mm in __mem_open/proc_maps_open). */
bool procext_image_file(unsigned index) { return index == MEM || index == ENVIRON || index == MAPS; }
int64_t procext_image(unsigned index, struct address_space* space, uint64_t address, uint64_t count, uint64_t offset, bool write) {
    if (index == MEM) return procext_mem(space, address, count, offset, write);
    if (write) return -22;
    if (!count) return 0;
    if (!user_range(address, count)) return -14;
    struct output out = {offset, address, count, 0, 0};
    if (index == ENVIRON) { uint64_t range[4]; space_arguments(space, range); memory_range(&out, space, range[2], range[3]); }
    else {
        char line[FAT32_PATH_MAX + 128];
        for (uint64_t i = 0; out.copied < out.capacity && !out.error; i++) {
            int n = space_map_line(space, i, line, sizeof(line)); if (n <= 0) break;
            emit(&out, line, (uint64_t)n);
        }
    }
    return out.error && !out.copied ? out.error : (int64_t)out.copied;
}
int64_t procext_read(bool task, unsigned index, uint32_t tid, bool thread, uint64_t address, uint64_t count, uint64_t offset) {
    if (!task && index >= NETPROC_BASE) return netproc_read(index - NETPROC_BASE, address, count, offset);
    if (!count) return 0;
    if (!user_range(address, count)) return -14;
    struct output out = {offset, address, count, 0, 0};
    if (task) task_file(&out, index, tid, thread); else global_file(&out, index);
    return out.error && !out.copied ? out.error : (int64_t)out.copied;
}
int64_t procext_write(unsigned index, uint32_t tid, uint64_t address, uint64_t count) {
    if (index >= NETPROC_BASE) return netproc_write(index - NETPROC_BASE, address, count);   /* /proc/sys/net (EXT_FILE) */
    if (index == COMM) {   /* the thread name; a trailing newline is dropped */
        char name[16] = {0}; uint64_t n = count < 15 ? count : 15;
        if (!count) return 0;
        if (!accessible(address, n, false)) return -14;
        copy_from_user(name, (void*)address, n); if (n && name[n - 1] == 0x0A) name[n - 1] = 0;
        int error = task_set_name_tid(tid, name); return error ? error : (int64_t)count;
    }
    if (index != OOM_SCORE_ADJ) return -13;
    if (!count || count > 16) return -22;
    char value[17]; if (!accessible(address, count, false)) return -14;
    copy_from_user(value, (void*)address, count); value[count] = 0;
    int64_t number = 0; bool negative = value[0] == '-'; unsigned i = negative ? 1 : 0;
    if (value[i] < '0' || value[i] > '9') return -22;
    for (; value[i] >= '0' && value[i] <= '9'; i++) number = number * 10 + (value[i] - '0');
    if (value[i] && value[i] != '\n') return -22;
    int error = task_set_oom_score(tid, (int)(negative ? -number : number));
    return error ? error : (int64_t)count;
}
