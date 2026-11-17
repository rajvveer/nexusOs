/* Page reclaim: per-zone watermarks, the kswapd kernel thread, direct reclaim, the OOM killer, vm sysctls and the
 * memory statistics files. See swap.h for the overall design.
 *
 * One reclaim round (shrink): free swap-cache pages nobody maps (writing dirty ones), drop clean page-cache pages
 * nobody maps, run the PTE clock (vm_reclaim_scan) which unmaps cold pages -- anonymous ones into the swap cache (at
 * most a vm.swappiness share of the round when file pages remain), file and shared-memory ones from the page tables
 * -- then write back what it produced, write back aged dirty file data, and push unmapped shared-memory pages to swap.
 * kswapd wakes every 20 ms (or when the allocator or a fault reports pressure) and reclaims while any zone is below
 * its low watermark, until every zone is above high or eight rounds in a row free nothing. Direct reclaim runs the
 * same rounds in a faulting task (big lock held, no other spinlock) when a user page cannot keep the min watermark;
 * if that fails, the OOM killer picks the process with the highest Linux badness (RSS + swap entries + page tables,
 * plus oom_score_adj * total/1000; never init or -1000), kills it like SIGKILL, logs it the Linux way and waits for
 * its memory; while a victim is still exiting no second one is chosen. */
#include "native.h"
#include "swap.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define PAGE 4096ULL
#define MS 1000000ULL
bool task_kernel_thread(void (*entry)(void), const char* name);   /* process.c */
void task_kernel_sleep(void* channel, uint64_t deadline);
int task_oom_kill(uint32_t pid);
bool task_process_alive(uint32_t pid);
bool task_space_held(struct address_space* space);
void task_sched_yield(void);

static int swappiness = 60, page_cluster = 3, watermark_scale = 10;
static uint64_t min_free_kbytes, user_min_free_kbytes;
static int kswapd_channel;
static volatile bool kswapd_idle;   /* sleeping: a wake-up is worth sending */
static uint32_t oom_victim; static uint64_t oom_victim_since;
static struct address_space* oom_victim_space;   /* compared only, never dereferenced: held until the victim is reaped */
static uint64_t stat_kswapd_wakes, stat_direct, stat_steal_kswapd, stat_steal_direct, stat_scan_kswapd, stat_scan_direct;
static uint64_t stat_oom_kills, stat_drop_pagecache, stat_kswapd_failures;
int reclaim_swappiness(void) { return swappiness; }
unsigned reclaim_page_cluster(void) { return (unsigned)page_cluster; }

static uint64_t isqrt(uint64_t x) { uint64_t r = 0; for (uint64_t bit = 1ULL << 62; bit; bit >>= 2) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; } return r; }
/* Linux init_per_zone_wmark_min / __setup_per_zone_wmarks with watermark_scale_factor. */
static void compute_watermarks(void) {
    uint64_t total = 0; struct memory_zone_info z;
    for (unsigned i = 0; memory_zone_get(i, &z); i++) total += z.managed;
    if (!total) return;
    if (!user_min_free_kbytes) {
        min_free_kbytes = isqrt(total * 4 * 16);
        if (min_free_kbytes < 128) min_free_kbytes = 128;
        if (min_free_kbytes > 262144) min_free_kbytes = 262144;
    } else min_free_kbytes = user_min_free_kbytes;
    uint64_t min_pages = min_free_kbytes / 4;
    for (unsigned i = 0; memory_zone_get(i, &z); i++) {
        uint64_t zmin = min_pages * z.managed / total, step = z.managed * (uint64_t)watermark_scale / 10000;
        if (step < zmin / 4) step = zmin / 4;
        if (zmin + 2 * step > z.managed / 2) { zmin = z.managed / 64; step = z.managed / 64; }   /* tiny zones */
        memory_zone_watermarks(i, (uint32_t)zmin, (uint32_t)(zmin + step), (uint32_t)(zmin + 2 * step));
    }
}
/* Zones whose free pages are below a watermark (0 min, 1 low, 2 high), as a bit mask. */
static uint64_t zones_below(unsigned mark) {
    uint64_t mask = 0; struct memory_zone_info z;
    for (unsigned i = 0; i < 64 && memory_zone_get(i, &z); i++) {
        uint32_t level = mark == 0 ? z.min : mark == 1 ? z.low : z.high;
        if (z.free < level) mask |= 1ULL << i;
    }
    return mask;
}
bool reclaim_low(void);
/* User pages can be allocated now: some zone keeps its min watermark after `pages` more and a few kernel pages (the
 * cache index or page table the same fault may need), so a retried allocation does not fail again at once. */
static bool user_room(unsigned pages) {
    struct memory_zone_info z;
    for (unsigned i = 0; memory_zone_get(i, &z); i++) if (z.free >= z.min + pages + 8) return true;
    return false;
}

/* ---- one reclaim round ----
 * `priority` (Linux scan priority, inverted): the PTE clock looks at target * 16 << priority entries, so a round that
 * meets only recently used pages (whose accessed bits it clears) is followed by wider ones that find them cold. */
static uint64_t shrink(uint64_t target, bool direct, unsigned priority) {
    uint64_t freed = 0, f = 0; unsigned scanned = 0, total_scanned = 0;
    if (priority > 12) priority = 12;
    swap_writeback(64, &f); freed += f;
    freed += memory_file_reclaim(target);
    if (freed < target) {
        bool swap = swap_writable();
        uint64_t wide = (target * 16) << priority, cap = 1ULL << 22;
        unsigned budget = (unsigned)(wide > cap ? cap : wide);
        /* the anonymous share follows vm.swappiness and grows with the priority like the scan (pages shared by fork
         * leave memory only once every mapping was unmapped, so wide rounds must unmap many) */
        uint64_t anon = swap ? (target * (uint64_t)swappiness / 100) << priority : 0, all = target << priority;
        unsigned max_anon = (unsigned)(anon > budget ? budget : anon), max_all = (unsigned)(all > budget ? budget : all);
        unsigned written = (unsigned)(max_all > 2 * target ? max_all : 2 * target);
        vm_reclaim_scan(budget, max_anon, true, priority, &scanned); total_scanned += scanned;
        swap_writeback(written, &f); freed += f;
        freed += memory_file_reclaim(target);
        if (freed < target && memory_file_dirty_total()) {   /* aged dirty data first; it is clean (droppable) afterwards */
            memory_file_writeback(false, direct ? 0 : 500 * MS, 2 << 20);
            freed += memory_file_reclaim(target - (freed < target ? freed : target));
        }
        if (freed < target / 2 && swap && (swappiness || !freed)) {   /* file pages ran short: anonymous memory goes */
            vm_reclaim_scan(budget, max_all, false, priority, &scanned); total_scanned += scanned;
            swap_writeback(written, &f); freed += f;
        }
        if (freed < target && swap) {
            memory_file_swap_out((unsigned)target);
            swap_writeback((unsigned)target, &f); freed += f;
        }
    }
    if (direct) { stat_scan_direct += total_scanned; stat_steal_direct += freed; }
    else { stat_scan_kswapd += total_scanned; stat_steal_kswapd += freed; }
    return freed;
}

/* ---- kswapd ---- */
static void kswapd_main(void) {
    vm_audit_on = boot_option("mm.audit"); uint64_t audited = 0;   /* debugging: page ownership cross-check */
    for (;;) {
        if (vm_audit_on && native_clock_ns() - audited > 5000 * MS) {
            audited = native_clock_ns(); kernel_lock_acquire(); block_io_barrier(); vm_audit(); kernel_lock_release();
        }
        /* Woken by the fault and file I/O paths when the allocator reported pressure; the timeout only catches
         * pressure from other allocations. It never runs, and never preempts anybody, while memory is plentiful. */
        kswapd_idle = true;
        task_kernel_sleep(&kswapd_channel, native_clock_ns() + 1000 * MS);
        kswapd_idle = false;
        if (!memory_pressure() && !zones_below(1)) continue;
        kernel_lock_acquire();
        block_io_barrier();
        stat_kswapd_wakes++;
        unsigned failures = 0;
        while (zones_below(2) && failures < 8) {
            uint64_t freed = shrink(256, false, failures * 2);
            failures = freed ? 0 : failures + 1;
            if (failures == 8) stat_kswapd_failures++;
            kernel_lock_release(); task_sched_yield(); kernel_lock_acquire(); block_io_barrier();   /* let syscalls and faults run */
        }
        memory_pressure_clear();
        kernel_lock_release();
    }
}
void reclaim_wake(void) { if (kswapd_idle) { kswapd_idle = false; task_wake_channel(&kswapd_channel); } }
void reclaim_init(void) {
    vm_audit_on = boot_option("mm.audit");
    compute_watermarks();
    if (!task_kernel_thread(kswapd_main, "kswapd0")) vga_print("[64] RECLAIM no memory for kswapd\n");
    task_stack_pool_init();   /* after kswapd: the pool is full whenever only permanent tasks run */
}

/* ---- the OOM killer ---- */
uint64_t reclaim_oom_badness(struct address_space* space, int adj, uint64_t* score) {
    uint64_t swap_total, swap_free, cached; swap_counts(&swap_total, &swap_free, &cached);
    uint64_t total = memory_total_pages() + swap_total;
    struct vm_usage u; vm_space_usage(space, &u);
    int64_t points = (int64_t)(u.resident + u.swap + u.page_tables) + (int64_t)adj * (int64_t)(total / 1000);
    /* /proc/PID/oom_score (Linux proc_oom_score): (1000 + badness * 1000 / total) * 2 / 3, 0 for OOM_SCORE_ADJ_MIN */
    int64_t scaled = adj == -1000 ? 0 : (1000 + points * 1000 / (int64_t)(total ? total : 1)) * 2 / 3;
    *score = scaled < 0 ? 0 : (uint64_t)scaled;
    return points < 0 ? 0 : (uint64_t)points;
}
bool reclaim_oom_victim(uint32_t pid) { return pid && pid == oom_victim; }
static void kb(const char* label, uint64_t pages) { char n[24]; vga_print(label); number64(pages * 4, n); vga_print(n); vga_print("kB"); }
static bool oom_hopeless;   /* the last OOM attempt found nothing killable */
/* Choose and kill a victim. 0: nothing killable (only init, -1000 or kernel threads). */
static uint32_t oom_kill(unsigned pages) {
    uint64_t swap_total, swap_free, cached; swap_counts(&swap_total, &swap_free, &cached);
    uint64_t total = memory_total_pages() + swap_total;
    uint32_t best = 0; int64_t best_points = 0; struct task_info chosen; struct vm_usage usage;
    for (uint32_t pid = task_next_id(0, 0); pid; pid = task_next_id(0, pid)) {
        struct task_info info;
        if (pid == 1 || !task_info(pid, false, &info) || !info.space || info.oom_score_adj == -1000) continue;
        struct vm_usage u; vm_space_usage(info.space, &u);
        int64_t points = (int64_t)(u.resident + u.swap + u.page_tables) + (int64_t)info.oom_score_adj * (int64_t)(total / 1000);
        if (!best || points > best_points) { best = pid; best_points = points; chosen = info; usage = u; }
    }
    char n[24];
    vga_print("[64] OOM "); task_get_name(n); vga_print(n); vga_print(" invoked oom-killer: order=0, pages="); number64(pages, n); vga_print(n);
    vga_print(" free="); number64(memory_free_pages(), n); vga_print(n); vga_print(" swap_free="); number64(swap_free, n); vga_print(n);
    uint64_t dirty, file = memory_file_cached_pages(&dirty);
    vga_print(" swap_cached="); number64(cached, n); vga_print(n); vga_print(" file="); number64(file, n); vga_print(n);
    vga_print(" last_victim="); number64(oom_victim, n); vga_print(n); vga_print(task_space_held(oom_victim_space) ? " (holds memory)" : " (gone)");
    vga_print(" since_ms="); number64(oom_victim ? (native_clock_ns() - oom_victim_since) / MS : 0, n); vga_print(n); vga_print("\n");
    if (!best) { vga_print("[64] OOM Out of memory and no killable processes\n"); return 0; }
    task_oom_kill(best); stat_oom_kills++;
    oom_victim = best; oom_victim_space = chosen.space; oom_victim_since = native_clock_ns();
    vga_print("[64] OOM Out of memory: Killed process "); number64(best, n); vga_print(n);   /* global pid (dmesg) */
    vga_print(" ("); vga_print(chosen.name); vga_print(")");
    uint64_t stack, data, size = space_virtual_bytes(chosen.space, &stack, &data);
    kb(" total-vm:", size / PAGE); kb(", anon-rss:", usage.anon); kb(", file-rss:", usage.file); kb(", shmem-rss:", usage.shmem);
    vga_print(", UID:"); number64(chosen.uid[0], n); vga_print(n); kb(" pgtables:", usage.page_tables);
    vga_print(" oom_score_adj:"); if (chosen.oom_score_adj < 0) { vga_print("-"); number64((uint64_t)-chosen.oom_score_adj, n); } else number64((uint64_t)chosen.oom_score_adj, n);
    vga_print(n); vga_print("\n");
    return best;
}
/* Wait (sleeping, the big lock released) until the victim's memory came back or it is gone. */
static bool oom_wait(struct address_space* victim, unsigned pages) {
    int channel;
    for (unsigned i = 0; i < 1000; i++) {
        if (user_room(pages)) return true;
        if (task_exiting()) return true;   /* this task is the victim (or dying anyway): let it unwind */
        if (!task_space_held(victim)) break;   /* reaped: its memory is free now */
        task_wait_until(&channel, native_clock_ns() + 10 * MS);
        shrink(pages + 16, true, 4);
    }
    return user_room(pages) || task_exiting();
}
static bool reclaim(unsigned pages, bool may_oom) {
    if (!cpu_self()->task || !kernel_lock_held()) return false;
    if (task_exiting()) return true;
    /* Another spinlock held (space_access keeps a foreign address space locked): nothing below may sleep -- one pass
     * over the clean page cache only, no swap I/O, no OOM wait. */
    if (spin_held_count() != (cpu_self()->vm_held ? 1u : 0u)) { memory_file_reclaim(32 + pages); return user_room(pages); }
    block_io_barrier();   /* reclaim writes swap and dirty files: not inside another task's disk wait */
    stat_direct++; reclaim_wake();
    for (unsigned priority = 0; priority <= 12; priority++) {   /* ever wider scans before the OOM killer */
        if (user_room(pages)) return true;
        shrink(32 + pages, true, priority);
        if (user_room(pages)) return true;
    }
    if (!may_oom) return false;
    /* Linux: no second victim while the last one still holds its memory (exiting, or a zombie not reaped yet) */
    if (oom_victim && task_space_held(oom_victim_space) && native_clock_ns() - oom_victim_since < 10000 * MS) {
        if (oom_wait(oom_victim_space, pages)) return true;
        if (task_space_held(oom_victim_space)) return false;
    }
    uint32_t victim = oom_kill(pages);
    oom_hopeless = !victim;
    if (!victim) return false;
    if (victim == task_pid()) return true;   /* this process: the retried access ends in its exit */
    return oom_wait(oom_victim_space, pages);
}
bool reclaim_direct(unsigned pages) { return reclaim(pages, true); }
bool reclaim_oom_hopeless(void) { return oom_hopeless; }
/* Direct reclaim may sleep here (a task with the big lock and no spinlock but its own address-space lock). */
bool reclaim_may_sleep(void) {
    return cpu_self()->task && kernel_lock_held() && spin_held_count() == (cpu_self()->vm_held ? 1u : 0u);
}
bool reclaim_low(void) { return !user_room(1); }
bool reclaim_kernel_room(unsigned pages) { return user_room(pages) || reclaim(pages, false); }
/* Hibernation (hibernate.c; tasks frozen, other CPUs parked, the boot CPU's power transition with the big lock): free
 * memory until `want_free` pages are free so that the image fits the restoring kernel (Linux's
 * hibernate_preallocate_memory) - clean page cache is dropped, anonymous pages and dirty file data are written to swap
 * and disk. Never sleeps on a task (disk waits halt the boot CPU) and never OOM-kills. Returns the free pages. */
uint64_t reclaim_hibernate(uint64_t want_free) {
    unsigned failures = 0;
    for (unsigned round = 0; memory_free_pages() < want_free && failures < 8 && round < 65536; round++) {
        uint64_t missing = want_free - memory_free_pages();
        uint64_t freed = shrink(missing > 4096 ? 4096 : missing, true, failures * 2);
        failures = freed ? 0 : failures + 1;
    }
    memory_pressure_clear();
    return memory_free_pages();
}
uint64_t reclaim_alloc_user(bool may_oom) {
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        uint64_t frame = frame_alloc_user(); if (frame) return frame;
        if (!reclaim(1, may_oom)) break;
        if (task_exiting()) break;
    }
    /* last resort, the reserve below min: only for exiting tasks (OOM victims) or when nothing could be killed --
     * everybody else retries the access after the victim's exit (vm_user_frame) */
    return may_oom && (task_exiting() || oom_hopeless || !reclaim_may_sleep()) ? frame_alloc() : 0;
}

/* Pages memory can still give without swapping (Linux si_mem_available): free above the low watermarks plus the
 * part of the page cache that is not needed to stay above them. */
uint64_t reclaim_available(void) {
    uint64_t dirty, file = memory_file_cached_pages(&dirty), free = memory_free_pages(), low = 0, pinned = memory_file_pinned_pages(); struct memory_zone_info z;
    file = file > pinned ? file - pinned : 0;   /* unlinked files' only copies stay */
    for (unsigned i = 0; memory_zone_get(i, &z); i++) low += z.low;
    int64_t available = (int64_t)free - (int64_t)low + (int64_t)file - (int64_t)(file / 2 < low ? file / 2 : low);
    return available > 0 ? (uint64_t)available : 0;
}
/* Session trace (native_io_poll every 5 s with process tracing): memory, swap and disk-wait counters. */
void reclaim_trace(void) {
    uint64_t total, free, cached, s[4], io[3]; swap_counts(&total, &free, &cached); swap_stats(s); block_io_stats(io);
    char n[24];
    vga_print("[64] MEMSTAT free_kb="); number64(memory_free_pages() * 4, n); vga_print(n);
    vga_print(" swap_used_kb="); number64((total - free) * 4, n); vga_print(n); vga_print(" swap_cached_kb="); number64(cached * 4, n); vga_print(n);
    vga_print(" pswpin="); number64(s[0], n); vga_print(n); vga_print(" pswpout="); number64(s[1], n); vga_print(n);
    vga_print(" oom_kills="); number64(stat_oom_kills, n); vga_print(n); vga_print(" kswapd_wakes="); number64(stat_kswapd_wakes, n); vga_print(n);
    vga_print(" disk_sleeps="); number64(io[0], n); vga_print(n); vga_print(" io_barrier_waits="); number64(io[1], n); vga_print(n);
    vga_print(" pinned_kb="); number64(memory_file_pinned_pages() * 4, n); vga_print(n); vga_print("\n");
}
/* ---- /proc/sys/vm ---- */
static unsigned put_number(char* text, unsigned capacity, int64_t value) {
    char n[24]; if (value < 0) { text[0] = '-'; number64((uint64_t)-value, n + 1); n[0] = '-'; } else number64((uint64_t)value, n);
    unsigned length = strlen(n); if (length + 2 > capacity) return 0;
    memcpy(text, n, length); text[length] = '\n'; text[length + 1] = 0; return length + 1;
}
static bool parse_number(const char* input, unsigned length, int64_t* value) {
    unsigned i = 0; bool negative = false; int64_t v = 0;
    while (i < length && (input[i] == ' ' || input[i] == '\t')) i++;
    if (i < length && input[i] == '-') { negative = true; i++; }
    if (i >= length || input[i] < '0' || input[i] > '9') return false;
    for (; i < length && input[i] >= '0' && input[i] <= '9'; i++) { v = v * 10 + (input[i] - '0'); if (v > 0x7FFFFFFF) return false; }
    while (i < length && (input[i] == '\n' || input[i] == ' ')) i++;
    if (i != length) return false;
    *value = negative ? -v : v; return true;
}
/* Read (input NULL: the text) or write one vm sysctl. Returns the text length, the bytes consumed, or -errno. */
int64_t reclaim_sysctl(const char* name, char* text, unsigned capacity, const char* input, unsigned length) {
    int64_t value = 0;
    if (!strcmp(name, "drop_caches")) {
        if (!input) return -13;   /* mode 0200 */
        if (!parse_number(input, length, &value) || value < 1 || value > 4) return -22;
        if (value & 1) { stat_drop_pagecache++; memory_file_reclaim(~0ULL); }
        if (!(value & 4)) {
            char n[24]; task_get_name(n); vga_print("[64] "); vga_print(n); vga_print(" ("); number64(task_visible_pid(task_pid()), n);
            vga_print(n); vga_print("): drop_caches: "); number64((uint64_t)value, n); vga_print(n); vga_print("\n");
        }
        return length;
    }
    int* integer = !strcmp(name, "swappiness") ? &swappiness : !strcmp(name, "page-cluster") ? &page_cluster :
                   !strcmp(name, "watermark_scale_factor") ? &watermark_scale : NULL;
    if (!input) {
        if (integer) return put_number(text, capacity, *integer);
        if (!strcmp(name, "min_free_kbytes")) return put_number(text, capacity, (int64_t)min_free_kbytes);
        return -2;
    }
    if (!parse_number(input, length, &value)) return -22;
    if (integer == &swappiness) { if (value < 0 || value > 200) return -22; swappiness = (int)value; }
    else if (integer == &page_cluster) { if (value < 0 || value > 3) return -22; page_cluster = (int)value; }
    else if (integer == &watermark_scale) { if (value < 1 || value > 3000) return -22; watermark_scale = (int)value; compute_watermarks(); }
    else if (!strcmp(name, "min_free_kbytes")) { if (value < 0 || (uint64_t)value > memory_total_pages() * 4 / 4) return -22; user_min_free_kbytes = value ? (uint64_t)value : 0; compute_watermarks(); }
    else return -2;
    return length;
}

/* ---- /proc/meminfo, /proc/vmstat, /proc/zoneinfo ---- */
struct text_out { char* text; unsigned at, capacity; };
static void out_text(struct text_out* o, const char* s) { while (*s && o->at + 1 < o->capacity) o->text[o->at++] = *s++; o->text[o->at] = 0; }
static void out_number(struct text_out* o, uint64_t value) { char n[24]; number64(value, n); out_text(o, n); }
static void meminfo_line(struct text_out* o, const char* name, uint64_t kib) {
    out_text(o, name); char n[24]; number64(kib, n);
    for (unsigned pad = strlen(name) + strlen(n); pad < 24; pad++) out_text(o, " ");
    out_text(o, n); out_text(o, " kB\n");
}
uint64_t memory_file_shmem_pages(void);   /* shmem.c */
int reclaim_proc_text(const char* name, char* text, unsigned capacity) {
    struct text_out o = {text, 0, capacity}; text[0] = 0;
    uint64_t total = memory_total_pages(), free = memory_free_pages(), dirty = 0, file = memory_file_cached_pages(&dirty);
    uint64_t swap_total, swap_free, swap_cached; swap_counts(&swap_total, &swap_free, &swap_cached);
    uint64_t shmem = memory_file_shmem_pages(); struct memory_zone_info z;
    if (!strcmp(name, "meminfo")) {
        struct vm_usage u; vm_usage_total(&u);
        uint64_t available = reclaim_available();
        uint64_t used = total - free, accounted = file + shmem + swap_cached + u.anon + u.page_tables;
        uint64_t kernel = used > accounted ? used - accounted : 0;
        meminfo_line(&o, "MemTotal:", total * 4); meminfo_line(&o, "MemFree:", free * 4);
        meminfo_line(&o, "MemAvailable:", available * 4); meminfo_line(&o, "Buffers:", 0);
        meminfo_line(&o, "Cached:", (file + shmem) * 4); meminfo_line(&o, "SwapCached:", swap_cached * 4);
        uint64_t mapped_file = u.file < file ? u.file : file;
        meminfo_line(&o, "Active:", (u.anon + mapped_file) * 4); meminfo_line(&o, "Inactive:", (file - mapped_file + shmem) * 4);
        meminfo_line(&o, "Active(anon):", u.anon * 4); meminfo_line(&o, "Inactive(anon):", shmem * 4);
        meminfo_line(&o, "Active(file):", mapped_file * 4); meminfo_line(&o, "Inactive(file):", (file - mapped_file) * 4);
        meminfo_line(&o, "Unevictable:", u.locked * 4); meminfo_line(&o, "Mlocked:", u.locked * 4);
        meminfo_line(&o, "SwapTotal:", swap_total * 4); meminfo_line(&o, "SwapFree:", swap_free * 4);
        meminfo_line(&o, "Dirty:", dirty * 4); meminfo_line(&o, "Writeback:", 0);
        meminfo_line(&o, "AnonPages:", u.anon * 4); meminfo_line(&o, "Mapped:", (u.file + u.shmem) * 4);
        meminfo_line(&o, "Shmem:", shmem * 4); meminfo_line(&o, "KReclaimable:", 0);
        meminfo_line(&o, "Slab:", kernel * 4); meminfo_line(&o, "SReclaimable:", 0); meminfo_line(&o, "SUnreclaim:", kernel * 4);
        meminfo_line(&o, "KernelStack:", task_count() * 68); meminfo_line(&o, "PageTables:", u.page_tables * 4);
        meminfo_line(&o, "NFS_Unstable:", 0); meminfo_line(&o, "Bounce:", 0); meminfo_line(&o, "WritebackTmp:", 0);
        meminfo_line(&o, "CommitLimit:", (total / 2 + swap_total) * 4); meminfo_line(&o, "Committed_AS:", (u.anon + shmem + (swap_total - swap_free)) * 4);
        meminfo_line(&o, "VmallocTotal:", 34359738367ULL); meminfo_line(&o, "VmallocUsed:", 0); meminfo_line(&o, "VmallocChunk:", 0);
        meminfo_line(&o, "Percpu:", 0); meminfo_line(&o, "AnonHugePages:", 0); meminfo_line(&o, "ShmemHugePages:", 0);
        meminfo_line(&o, "ShmemPmdMapped:", 0); meminfo_line(&o, "FileHugePages:", 0); meminfo_line(&o, "FilePmdMapped:", 0);
        out_text(&o, "HugePages_Total:       0\nHugePages_Free:        0\nHugePages_Rsvd:        0\nHugePages_Surp:        0\n");
        meminfo_line(&o, "Hugepagesize:", 2048); meminfo_line(&o, "Hugetlb:", 0);
        return (int)o.at;
    }
    if (!strcmp(name, "vmstat")) {
        uint64_t s[4]; swap_stats(s);
        static const char* const names[] = {"nr_free_pages ", "nr_file_pages ", "nr_dirty ", "nr_writeback 0\nnr_shmem ", "nr_swapcached ",
            "pswpin ", "pswpout ", "swap_ra ", "swap_ra_hit ", "pgfault ", "pgscan_kswapd ", "pgscan_direct ", "pgsteal_kswapd ",
            "pgsteal_direct ", "kswapd_wakeups ", "allocstall ", "oom_kill ", "drop_pagecache ", "kswapd_failures "};
        uint64_t values[] = {free, file + shmem + swap_cached, dirty, shmem, swap_cached, s[0], s[1], s[3], s[2], kstat.faults,
            stat_scan_kswapd, stat_scan_direct, stat_steal_kswapd, stat_steal_direct, stat_kswapd_wakes, stat_direct, stat_oom_kills,
            stat_drop_pagecache, stat_kswapd_failures};
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) { out_text(&o, names[i]); out_number(&o, values[i]); out_text(&o, "\n"); }
        return (int)o.at;
    }
    if (!strcmp(name, "zoneinfo")) {
        for (unsigned i = 0; memory_zone_get(i, &z); i++) {
            out_text(&o, "Node 0, zone "); out_text(&o, z.end <= 0x100000000ULL ? "   DMA32" : "  Normal");
            out_text(&o, "\n  pages free     "); out_number(&o, z.free); out_text(&o, "\n        min      "); out_number(&o, z.min);
            out_text(&o, "\n        low      "); out_number(&o, z.low); out_text(&o, "\n        high     "); out_number(&o, z.high);
            out_text(&o, "\n        spanned  "); out_number(&o, (z.end - z.begin) / PAGE); out_text(&o, "\n        present  ");
            out_number(&o, (z.end - z.begin) / PAGE); out_text(&o, "\n        managed  "); out_number(&o, z.managed);
            out_text(&o, "\n  start_pfn:           "); out_number(&o, z.begin / PAGE); out_text(&o, "\n");
        }
        return (int)o.at;
    }
    return -2;
}
