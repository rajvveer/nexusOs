/* Page-table side of swap and page reclaim. Included by kernel.c (after the VMA helpers, before page_resolve) because
 * it works on struct address_space / struct vm_area and their static helpers; see swap.h for the design and locking.
 *
 * Reclaim scan (a clock over every address space's PTEs, Linux 2.4 swap_out style, driven by the hardware accessed
 * bit): a present user page whose accessed bit is set loses it (second chance); one found clear is unmapped. Private
 * pages (anonymous memory, COW copies of file pages) get a swap entry and enter the swap cache; page-cache pages
 * mapped from files and shared memory are just unmapped (their cache keeps them; reclaim.c shrinks the caches).
 * Never touched: mlocked areas (mlock/MAP_LOCKED/mlockall), /dev/fb0 and device (DMA) mappings, MMIO entries, kernel
 * memory, and any space whose lock is busy (a running syscall or fault owns it). Entries are cleared with an atomic
 * exchange (it captures the dirty bit), the space is shot down once per batch, and only then are frames swapped or
 * dropped (other CPUs may write through stale TLB entries until the acknowledgement). */
#define PTE_ACCESSED 32ULL
#define PTE_DIRTY 64ULL
#define RECLAIM_BATCH 64
#include "swap.h"
static bool space_lock_other(struct address_space* space);
uint64_t memory_file_peek(struct memory_file* file, uint64_t index);   /* shmem.c: resident frame or 0 (no I/O) */
int memory_file_unuse(unsigned type);                                   /* shmem.c: swapoff for shared memory */
uint64_t task_yields(void);                                             /* process.c: changes when the task slept */

/* This CPU may edit the space's tables: it holds the lock already (its own task's space), or takes it now. */
static bool vm_trylock(struct address_space* space, bool* taken) {
    *taken = false;
    if (spin_held(&space->vm_lock)) return true;
    if (!spin_try_acquire(&space->vm_lock)) return false;
    *taken = true; return true;
}
/* The level-0 slot for the first address >= *va (below end) whose page table exists; whole missing branches are
 * skipped, so sparse reservations cost nothing. NULL past `end`. */
static uint64_t* vm_next_slot(uint64_t root, uint64_t* va, uint64_t end) {
    while (*va < end) {
        uint64_t* table = phys_to_virt(root); unsigned shift = 39;
        for (; shift >= 21; shift -= 9) {
            uint64_t entry = table[(*va >> shift) & 511];
            if (!(entry & PRESENT) || (entry & LARGE) || !(entry & USER)) break;
            table = phys_to_virt(entry & ADDRESS);
        }
        if (shift < 21) return &table[(*va >> 12) & 511];
        uint64_t next = (*va & ~((1ULL << shift) - 1)) + (1ULL << shift);
        if (next <= *va) return NULL;
        *va = next;
    }
    return NULL;
}
/* Areas whose pages may leave memory. */
static bool vm_reclaimable(const struct vm_area* a) { return !a->locked && !a->framebuffer && !a->device_memory; }
/* A private mapping's page that is still its file's (or shared memory's) cache page, not a copy of it. */
static bool vm_cache_page(const struct vm_area* a, uint64_t va, uint64_t frame) {
    return a->memory && memory_file_peek(a->memory, (a->offset + va - a->start) / PAGE) == frame;
}
struct reclaim_item { uint64_t* slot; uint64_t old; bool swap; struct vm_area* area; uint64_t va; };
static struct address_space* scan_cursor;
static uint64_t scan_va;
static bool space_listed(struct address_space* space) { for (struct address_space* s = spaces; s; s = s->next) if (s == space) return true; return false; }
/* Up to `budget` PTEs of one space from scan_va; returns pages unmapped. The space's lock is held. */
static unsigned scan_space(struct address_space* space, unsigned budget, unsigned* anon, bool file, unsigned priority, unsigned* scanned, bool* finished) {
    struct reclaim_item items[RECLAIM_BATCH]; unsigned n = 0, done = 0;
    uint64_t va = scan_va < USER_START ? USER_START : scan_va;
    struct vm_area* a = space->maps; while (a && a->end <= va) a = a->next;
    while (a && *scanned < budget && n < RECLAIM_BATCH) {
        if (va < a->start) va = a->start;
        if (va >= a->end || !vm_reclaimable(a)) { a = a->next; continue; }
        uint64_t* slot = vm_next_slot(space->root, &va, a->end);
        if (!slot) { a = a->next; continue; }
        uint64_t entry = *slot; (*scanned)++;
        if ((entry & PRESENT) && !(entry & MMIO)) {
            if (entry & PTE_ACCESSED) __atomic_and_fetch(slot, ~PTE_ACCESSED, __ATOMIC_RELAXED);   /* second chance */
            else {
                bool cache = a->shared ? a->memory != NULL : vm_cache_page(a, va, entry & ADDRESS);
                /* mapped program text gets more chances (Linux keeps VM_EXEC file pages active) */
                bool keep = cache && (a->prot & 4) && priority < 8;
                if (!keep && (cache ? file : *anon > 0)) { items[n++] = (struct reclaim_item){slot, 0, !cache, a, va}; if (!cache) (*anon)--; }
            }
        }
        va += PAGE;
    }
    *finished = !a; scan_va = va;
    if (!n) return 0;
    for (unsigned i = 0; i < n; i++) items[i].old = __atomic_exchange_n(items[i].slot, 0, __ATOMIC_SEQ_CST);
    space_flush(space);   /* every CPU stops using the old translations before the frames change hands */
    for (unsigned i = 0; i < n; i++) {
        uint64_t old = items[i].old, frame = old & ADDRESS;
        if (!(old & PRESENT)) continue;
        struct vm_area* area = items[i].area;
        if (!items[i].swap && (old & PTE_DIRTY) && area->shared && area->memory && area->memory->disk_backed)   /* the store was not seen */
            memory_file_dirty(area->memory, (area->offset + items[i].va - area->start) / PAGE);
        if (items[i].swap) {
            uint64_t entry = swap_out_page(frame, (old & PTE_DIRTY) != 0);
            if (!entry) { *items[i].slot = old; continue; }   /* swap is full: the page stays */
            *items[i].slot = entry;
        }
        page_free(frame); done++;
    }
    return done;
}
unsigned vm_reclaim_scan(unsigned budget, unsigned max_anon, bool file, unsigned priority, unsigned* scanned) {
    unsigned unmapped = 0, total = 0; *scanned = 0;
    if (max_anon && !swap_writable()) max_anon = 0;
    if (!max_anon && !file) return 0;
    if (scan_cursor && !space_listed(scan_cursor)) { scan_cursor = NULL; scan_va = 0; }
    for (struct address_space* s = spaces; s; s = s->next) total++;
    for (unsigned seen = 0; *scanned < budget && seen <= total;) {
        if (!scan_cursor) { scan_cursor = spaces; scan_va = 0; if (!scan_cursor) break; }
        struct address_space* space = scan_cursor; bool taken, finished = true;
        if (space != &kernel_space && vm_trylock(space, &taken)) {
            unmapped += scan_space(space, budget, &max_anon, file, priority, scanned, &finished);
            if (taken) spin_release(&space->vm_lock);
        }
        if (finished) { scan_cursor = space->next; scan_va = 0; seen++; }
    }
    return unmapped;
}

/* ---- mm.audit (debugging): page ownership cross-check, every 5 s from kswapd (big lock held) ----
 * Holders seen (user PTEs + page-cache slots) must not exceed a frame's reference count, a mapped frame must be
 * allocated, a shared mapping of a file must map the file's current page, and a freed file must be unmapped. */
bool vm_audit_on;
static uint8_t* audit_seen; static uint64_t audit_slots; static unsigned audit_lines;
static void audit_visit(uint64_t frame, void* context) {
    (void)context; int64_t s = memory_frame_slot(frame, NULL);
    if (s >= 0 && audit_seen[s] < 255) audit_seen[s]++;
}
static uint32_t audit_pid(struct address_space* space) {
    for (uint32_t pid = task_next_id(0, 0); pid; pid = task_next_id(0, pid)) {
        struct task_info info; if (task_info(pid, false, &info) && info.space == space) return pid;
    }
    return 0;
}
static void audit_line(const char* what, struct address_space* space, struct vm_area* a, uint64_t va, uint64_t entry, uint64_t extra) {
    if (audit_lines >= 40) return;
    audit_lines++;
    say("[64] AUDIT "); say(what); say(" pid="); hex(audit_pid(space)); say(" va="); hex(va); say(" pte="); hex(entry); say(" extra="); hex(extra);
    if (a) { say(" area="); hex(a->start); put('-'); hex(a->end); say(" prot="); hex(a->prot); say(" shared="); hex(a->shared); say(" mem="); hex((uint64_t)a->memory); say(" path="); say(a->path); }
    put('\n');
}
static bool audit_area(const struct vm_area* a) { return !a->device_memory && !a->framebuffer; }
static void audit_where(uint64_t frame, uint64_t extra) {
    for (struct address_space* space = spaces; space; space = space->next) {
        if (space == &kernel_space) continue;
        bool taken; if (!vm_trylock(space, &taken)) continue;
        for (struct vm_area* a = space->maps; a; a = a->next) {
            if (!audit_area(a)) continue;
            for (uint64_t va = a->start; va < a->end; va += PAGE) {
                uint64_t* slot = vm_next_slot(space->root, &va, a->end); if (!slot) break;
                if ((*slot & PRESENT) && !(*slot & MMIO) && (*slot & ADDRESS) == frame) audit_line("OVER_MAP", space, a, va, *slot, extra);
            }
        }
        if (taken) spin_release(&space->vm_lock);
    }
}
void memory_file_audit_free(struct memory_file* file) {
    for (struct address_space* space = spaces; space; space = space->next)
        for (struct vm_area* a = space->maps; a; a = a->next)
            if (a->memory == file) { audit_lines = 0; audit_line("FREED_FILE_MAPPED", space, a, a->start, 0, (uint64_t)file); }
}
uint32_t audit_sum(const void* data, uint64_t bytes) {
    const uint8_t* p = data; uint64_t h = 0xCBF29CE484222325ULL;
    for (uint64_t i = 0; i + 8 <= bytes; i += 8) { uint64_t q; memcpy(&q, p + i, 8); h = (h ^ q) * 0x100000001B3ULL; h ^= h >> 29; }
    for (uint64_t i = bytes & ~7ULL; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ULL;
    uint32_t r = (uint32_t)(h ^ (h >> 32)); return r ? r : 1;
}
static unsigned audit_reports;
void audit_report(const char* what, const char* name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (audit_reports >= 64) return;
    audit_reports++;
    say("[64] AUDIT "); say(what); if (name) { say(" name="); say(name); }
    say(" a="); hex(a); say(" b="); hex(b); say(" c="); hex(c); say(" d="); hex(d); say(" pid="); hex(task_pid()); put('\n');
}
void vm_audit(void) {
    memory_file_audit_init();
    if (!audit_seen) { audit_slots = memory_frame_slots(); audit_seen = (uint8_t*)arena_alloc(audit_slots); if (!audit_seen) return; }
    memset(audit_seen, 0, audit_slots); audit_lines = 0;
    uint64_t pages = 0, skipped = 0, stale = 0, freed = 0, over = 0;
    for (struct address_space* space = spaces; space; space = space->next) {
        if (space == &kernel_space) continue;
        bool taken; if (!vm_trylock(space, &taken)) { skipped++; continue; }
        for (struct vm_area* a = space->maps; a; a = a->next) {
            if (!audit_area(a)) continue;
            for (uint64_t va = a->start; va < a->end; va += PAGE) {
                uint64_t* slot = vm_next_slot(space->root, &va, a->end); if (!slot) break;
                uint64_t entry = *slot; if (!(entry & PRESENT) || (entry & MMIO)) continue;
                uint64_t frame = entry & ADDRESS; uint32_t refs = 0; int64_t s = memory_frame_slot(frame, &refs);
                if (s < 0) continue;
                pages++; if (audit_seen[s] < 255) audit_seen[s]++;
                if (!refs) { freed++; audit_line("MAPPED_FREE", space, a, va, entry, 0); }
                if (a->shared && a->memory) {
                    uint64_t current = memory_file_peek(a->memory, (a->offset + va - a->start) / PAGE);
                    if (current != frame) { stale++; audit_line("STALE_SHARED", space, a, va, entry, current); }
                }
            }
        }
        if (taken) spin_release(&space->vm_lock);
    }
    memory_file_audit(audit_visit, NULL);
    for (uint64_t s = 0; s < audit_slots; s++) {
        if (!audit_seen[s]) continue;
        uint64_t frame = memory_slot_frame(s); uint32_t refs = 0; memory_frame_slot(frame, &refs);
        if (audit_seen[s] <= refs) continue;
        over++;
        if (over <= 4) audit_where(frame, (uint64_t)refs << 8 | audit_seen[s]);
    }
    say("[64] AUDIT pages="); hex(pages); say(" skipped_spaces="); hex(skipped); say(" stale="); hex(stale);
    say(" mapped_free="); hex(freed); say(" over="); hex(over); put('\n');
}

/* ---- swap-in ---- */
/* Map a swapped-out page from its retained frame (the caller's reference becomes the PTE's): the only user takes it
 * privately (writable), otherwise it is shared read-only (COW in writable areas) and the entry's reference drops. */
static void vm_swap_map(struct vm_area* a, uint64_t* slot, uint64_t va, uint64_t entry, uint64_t frame) {
    uint64_t flags = page_flags(a->prot);
    if (!swap_take_exclusive(entry, frame)) {
        swap_free(entry);
        if (a->prot & 2) flags = (flags & ~WRITE) | COW;
    }
    *slot = frame | flags;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}
/* Lockless fault path: a page still in the swap cache maps without the big lock (no I/O). */
static bool vm_swap_fault_fast(struct vm_area* a, uint64_t* slot, uint64_t va) {
    uint64_t entry = *slot, frame = swap_cache_get(entry);
    if (!frame) return false;
    vm_swap_map(a, slot, va, entry, frame);
    return true;
}
/* Locked fault path (big lock, own space lock): from the swap cache or the device. Reading may sleep (disk, direct
 * reclaim, an OOM victim's exit), so the PTE is looked up again afterwards; a page another thread brought in or an
 * unmapped range just retries the access. False: the slot could not be read (I/O error: SIGBUS) or no memory. */
static bool vm_swap_fault(struct address_space* space, uint64_t* slot, uint64_t va) {
    uint64_t entry = *slot; int error = 0;
    uint64_t frame = swap_cache_get(entry);
    if (!frame) {
        frame = swap_read(entry, true, &error);
        slot = pte(space->root, va, false);
        if (!slot || *slot != entry) { if (frame) page_free(frame); return true; }   /* resolved or unmapped meanwhile: retry */
        if (!frame) return error == -12 && !task_exiting();   /* no memory: reclaim and the OOM killer ran, retry; I/O: SIGBUS */
    }
    struct vm_area* a = space_area(space, va);
    if (!a) { page_free(frame); return false; }
    vm_swap_map(a, slot, va, entry, frame);
    return true;
}
/* A user page for the locked fault path. 0 with *retry: direct reclaim (and, failing that, the OOM killer) ran and may
 * have slept (page-table pointers are stale): the access is retried, as Linux retries a VM_FAULT_OOM fault -- a valid
 * mapping never fails for want of memory. The reserve below the min watermark stays for the kernel and for exiting
 * tasks (OOM victims: they finish unwinding, so the OOM path cannot deadlock); others get it only when the OOM killer
 * found nothing to kill, or where reclaim cannot sleep (space_access holds another space's lock). 0 otherwise: no
 * memory even in the reserve. */
static uint64_t vm_user_frame(bool* retry) {
    uint64_t frame = frame_alloc_user(); if (frame) return frame;
    if (!task_exiting() && reclaim_may_sleep() && (reclaim_direct(1) || !reclaim_oom_hopeless())) { *retry = true; return 0; }
    frame = frame_alloc();
    if (!frame && !task_exiting()) *retry = true;
    return frame;
}

/* ---- accounting: /proc/PID/status VmSwap, smaps, meminfo, the OOM killer ---- */
struct vm_area_usage { uint64_t rss, pss, shared_clean, shared_dirty, private_clean, private_dirty, referenced, anon, swap, swap_pss, cache; };
static unsigned vm_mapcount(uint64_t frame, bool cache) {
    uint32_t references = page_references(frame); unsigned extra = (cache ? 1 : 0) + (swap_frame_entry(frame) ? 1 : 0);
    return references > extra ? references - extra : 1;
}
static void vm_area_count(struct address_space* space, struct vm_area* a, struct vm_area_usage* u) {
    memset(u, 0, sizeof(*u));
    for (uint64_t va = a->start; ; va += PAGE) {
        uint64_t* slot = vm_next_slot(space->root, &va, a->end); if (!slot) break;
        uint64_t entry = *slot;
        if (swap_entry_is(entry)) {
            unsigned count = swap_entry_count(entry);
            u->swap += PAGE; u->swap_pss += count ? PAGE / count : PAGE; continue;
        }
        if (!(entry & PRESENT) || (entry & MMIO)) continue;
        uint64_t frame = entry & ADDRESS;
        bool cache = a->memory && (a->shared || vm_cache_page(a, va, frame));
        unsigned maps = vm_mapcount(frame, cache);
        bool dirty = (entry & PTE_DIRTY) || (!cache && !swap_frame_entry(frame));
        u->rss += PAGE; u->pss += PAGE / maps;
        if (maps > 1) { if (dirty) u->shared_dirty += PAGE; else u->shared_clean += PAGE; }
        else if (dirty) u->private_dirty += PAGE; else u->private_clean += PAGE;
        if (entry & PTE_ACCESSED) u->referenced += PAGE;
        if (!cache) u->anon += PAGE; else u->cache += PAGE;
    }
}
static uint64_t vm_table_pages(uint64_t physical, unsigned level, uint64_t base) {
    uint64_t* table = phys_to_virt(physical), count = 1, span = 1ULL << (12 + 9 * level);
    if (!level) return 1;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t entry = table[i], address = base + i * span;
        if (!(entry & PRESENT) || !(entry & USER) || (entry & LARGE) || address >= USER_END) continue;
        count += vm_table_pages(entry & ADDRESS, level - 1, address);
    }
    return count;
}
void vm_space_usage(struct address_space* space, struct vm_usage* usage) {
    memset(usage, 0, sizeof(*usage));
    if (!space || space == &kernel_space) return;
    bool locked = space_lock_other(space);
    for (struct vm_area* a = space->maps; a; a = a->next) {
        if (a->framebuffer || a->device_memory) continue;
        struct vm_area_usage u; vm_area_count(space, a, &u);
        usage->resident += u.rss / PAGE; usage->swap += u.swap / PAGE;
        if (!a->memory || !a->shared) usage->anon += u.anon / PAGE;
        if (a->memory && a->memory->disk_backed) usage->file += u.cache / PAGE; else usage->shmem += u.cache / PAGE;
        if (a->locked) usage->locked += u.rss / PAGE;
    }
    usage->page_tables = vm_table_pages(space->root, 3, 0);
    if (locked) spin_release(&space->vm_lock);
}
/* Kernel pages a fork of the current space needs at once: its page tables and one page per area (space_clone). */
unsigned vm_fork_pages(void) {
    struct address_space* space = active_space; unsigned count = 0;
    if (!space || space == &kernel_space) return 16;
    for (struct vm_area* a = space->maps; a; a = a->next) count++;
    uint64_t tables = vm_table_pages(space->root, 3, 0), total = tables + count + 32;
    return total > 65536 ? 65536 : (unsigned)total;
}
void vm_usage_total(struct vm_usage* usage) {
    memset(usage, 0, sizeof(*usage));
    for (struct address_space* s = spaces; s; s = s->next) {
        if (s == &kernel_space) continue;
        struct vm_usage u; vm_space_usage(s, &u);
        usage->resident += u.resident; usage->anon += u.anon; usage->file += u.file; usage->shmem += u.shmem;
        usage->swap += u.swap; usage->locked += u.locked; usage->page_tables += u.page_tables;
    }
}
/* /proc/PID/smaps: entry `index` (the maps line and its counters), or the smaps_rollup totals when `rollup`. 0 past
 * the last entry, -36 when the text does not fit. */
static unsigned smaps_field(char* text, unsigned n, unsigned capacity, const char* name, uint64_t bytes) {
    char number[24]; number64(bytes / 1024, number);
    unsigned name_length = strlen(name), number_length = strlen(number);
    if (n + name_length + 22 + 4 >= capacity) return capacity;
    memcpy(text + n, name, name_length); n += name_length;
    for (unsigned pad = name_length + number_length; pad < 26; pad++) text[n++] = ' ';
    memcpy(text + n, number, number_length); n += number_length;
    memcpy(text + n, " kB\n", 4); return n + 4;
}
int space_smaps_entry(struct address_space* space, uint64_t index, char* text, unsigned capacity, bool rollup) {
    if (!space || space == &kernel_space || capacity < 1024) return 0;
    struct vm_area_usage total = {0}; uint64_t size = 0, locked_bytes = 0; struct vm_area* a = NULL; int n = 0;
    bool locked = space_lock_other(space);
    if (rollup) {
        if (index) { if (locked) spin_release(&space->vm_lock); return 0; }
        uint64_t lo = 0, hi = 0;
        for (struct vm_area* v = space->maps; v; v = v->next) {
            if (!lo) lo = v->start; hi = v->end;
            if (v->framebuffer || v->device_memory) continue;
            struct vm_area_usage u; vm_area_count(space, v, &u);
            total.rss += u.rss; total.pss += u.pss; total.shared_clean += u.shared_clean; total.shared_dirty += u.shared_dirty;
            total.private_clean += u.private_clean; total.private_dirty += u.private_dirty; total.referenced += u.referenced;
            total.anon += u.anon; total.swap += u.swap; total.swap_pss += u.swap_pss;
            if (v->locked) locked_bytes += u.pss;
        }
        static const char digits[] = "0123456789abcdef"; char line[128]; unsigned k = 0;
        for (unsigned part = 0; part < 2; part++) {
            uint64_t value = part ? hi : lo; unsigned width = 8; while (width < 16 && (value >> (4 * width))) width++;
            for (unsigned d = width; d--;) line[k++] = digits[(value >> (4 * d)) & 15];
            line[k++] = part ? ' ' : '-';
        }
        const char* rest = "---p 00000000 00:00 0                          [rollup]\n";
        memcpy(line + k, rest, strlen(rest)); k += strlen(rest);
        memcpy(text, line, k); n = (int)k;
    } else {
        a = space->maps; for (uint64_t i = index; a && i; i--) a = a->next;
        if (!a) { if (locked) spin_release(&space->vm_lock); return 0; }
        n = space_map_line(space, index, text, capacity / 2);
        if (n <= 0) { if (locked) spin_release(&space->vm_lock); return n; }
        size = a->end - a->start;
        if (!a->framebuffer && !a->device_memory) vm_area_count(space, a, &total);
        if (a->locked) locked_bytes = total.pss;
    }
    if (locked) spin_release(&space->vm_lock);
    unsigned m = (unsigned)n;
    if (!rollup) {
        m = smaps_field(text, m, capacity, "Size:", size);
        m = smaps_field(text, m, capacity, "KernelPageSize:", PAGE); m = smaps_field(text, m, capacity, "MMUPageSize:", PAGE);
    }
    m = smaps_field(text, m, capacity, "Rss:", total.rss); m = smaps_field(text, m, capacity, "Pss:", total.pss);
    m = smaps_field(text, m, capacity, "Pss_Dirty:", total.private_dirty + total.shared_dirty);
    if (rollup) { m = smaps_field(text, m, capacity, "Pss_Anon:", total.anon); m = smaps_field(text, m, capacity, "Pss_File:", total.rss - total.anon); }
    m = smaps_field(text, m, capacity, "Shared_Clean:", total.shared_clean); m = smaps_field(text, m, capacity, "Shared_Dirty:", total.shared_dirty);
    m = smaps_field(text, m, capacity, "Private_Clean:", total.private_clean); m = smaps_field(text, m, capacity, "Private_Dirty:", total.private_dirty);
    m = smaps_field(text, m, capacity, "Referenced:", total.referenced); m = smaps_field(text, m, capacity, "Anonymous:", total.anon);
    m = smaps_field(text, m, capacity, "KSM:", 0); m = smaps_field(text, m, capacity, "LazyFree:", 0);
    m = smaps_field(text, m, capacity, "AnonHugePages:", 0); m = smaps_field(text, m, capacity, "ShmemPmdMapped:", 0);
    m = smaps_field(text, m, capacity, "FilePmdMapped:", 0); m = smaps_field(text, m, capacity, "Shared_Hugetlb:", 0);
    m = smaps_field(text, m, capacity, "Private_Hugetlb:", 0); m = smaps_field(text, m, capacity, "Swap:", total.swap);
    m = smaps_field(text, m, capacity, "SwapPss:", total.swap_pss); m = smaps_field(text, m, capacity, "Locked:", locked_bytes);
    if (m + 96 >= capacity) return -36;
    if (!rollup) {
        const char* flags[] = {a->prot & 1 ? "rd " : "", a->prot & 2 ? "wr " : "", a->prot & 4 ? "ex " : "", a->shared ? "sh " : "",
                               "mr ", a->may_write ? "mw " : "", "me ", a->shared ? "ms " : "", a->stack ? "gd " : "",
                               a->locked ? "lo " : "", a->framebuffer || a->device_memory ? "io " : "", a->dontdump ? "dd " : "",
                               !a->shared && (a->prot & 2) ? "ac " : ""};
        memcpy(text + m, "THPeligible:    0\nVmFlags: ", 26); m += 26;
        for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) { unsigned l = strlen(flags[i]); memcpy(text + m, flags[i], l); m += l; }
        text[m++] = '\n';
    }
    return (int)m;
}

/* ---- swapoff: bring every page of one area back ---- */
/* Each space is retained and walked by address; around a read (which may sleep) another space's lock is dropped and
 * the PTE checked again afterwards. Rounds repeat until nothing refers to the area (a fork may copy entries into a
 * space the walk has passed). */
static int vm_unuse_space(struct address_space* space, unsigned type, bool* found) {
    int result = 0; bool locked = space_lock_other(space);
    struct vm_area* a = space->maps;
    for (uint64_t va = USER_START; va < USER_END && !result;) {
        if (!a || a->end <= va) { a = space->maps; while (a && a->end <= va) a = a->next; }
        if (!a) break;
        if (va < a->start) va = a->start;
        uint64_t* slot = vm_next_slot(space->root, &va, a->end);
        if (!slot) { va = a->end; continue; }
        uint64_t entry = *slot;
        if (!swap_entry_is(entry) || swap_entry_type(entry) != type) { va += PAGE; continue; }
        *found = true;
        uint64_t frame = swap_cache_get(entry);
        if (!frame) {
            int error = 0;
            if (locked) spin_release(&space->vm_lock);
            frame = swap_read(entry, false, &error);   /* may sleep: the space may change meanwhile */
            locked = space_lock_other(space);
            a = NULL;
            slot = pte(space->root, va, false);
            if (!slot || *slot != entry) { if (frame) page_free(frame); continue; }   /* changed: look at it again */
            if (!frame) { result = error == -5 ? -5 : -12; break; }
            if (!(a = space_area(space, va))) { page_free(frame); va += PAGE; continue; }
        }
        vm_swap_map(a, slot, va, entry, frame);
        va += PAGE;
    }
    if (locked) spin_release(&space->vm_lock);
    return result;
}
int vm_swap_unuse(unsigned type) {
    for (unsigned round = 0; round < 32; round++) {
        bool found = false;
        for (struct address_space* space = spaces; space; ) {
            space_retain(space);
            int error = vm_unuse_space(space, type, &found);
            struct address_space* next = space->next;
            space_release(space);   /* may free it (its task exited while a read slept): next was read first */
            if (error) return error;
            if (task_exiting() || signals_interrupt_pending()) return -4;
            space = next;
        }
        int error = memory_file_unuse(type); if (error) return error;
        if (swap_area_idle(type)) return 0;
    }
    return 0;   /* swap_off reports what is still in use */
}

/* ---- mincore(2): resident pages (present, in the swap cache, or in a file's/shared memory's cache) ---- */
static int64_t vm_mincore(uint64_t address, uint64_t length, uint64_t vector) {
    if (address & (PAGE - 1)) return -22;
    if (!length) return 0;
    if (length > USER_END - USER_START) return -12;
    uint64_t pages = (length + PAGE - 1) / PAGE;
    if (!user_range(address, pages * PAGE)) return -12;
    for (uint64_t at = address, end = address + pages * PAGE; at < end;) {   /* Linux: the whole range is mapped, or ENOMEM */
        struct vm_area* a = area_find(at); if (!a) return -12;
        at = a->end < end ? a->end : end;
    }
    if (!accessible(vector, pages, true)) return -14;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = address + i * PAGE, *slot = pte(process_root, va, false), entry = slot ? *slot : 0;
        struct vm_area* a = area_find(va); uint8_t resident = 0;
        if (entry & PRESENT) resident = 1;
        else if (swap_entry_is(entry)) { uint64_t frame = swap_cache_get(entry); if (frame) { page_free(frame); resident = 1; } }
        else if (a && a->memory && !a->device_memory) resident = memory_file_peek(a->memory, (a->offset + va - a->start) / PAGE) != 0;
        uput8(vector + i, resident);
    }
    return 0;
}
