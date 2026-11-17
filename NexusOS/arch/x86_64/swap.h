#ifndef NEXUS_SWAP_H
#define NEXUS_SWAP_H
/* Virtual memory under pressure: Linux-compatible swap (swap.c), page reclaim, kswapd and the OOM killer (reclaim.c),
 * and the page-table side of both (vm_reclaim.inc, compiled inside kernel.c next to the address-space code).
 *
 * Swap entries. A page that was swapped out leaves a non-present entry with bit 1 set in its PTE:
 *     (offset << 12) | (type << 2) | 2          type = swap area (0..SWAP_MAX_AREAS-1), offset = page slot in it
 * Shared-memory caches (shmem.c, memfd, /dev/shm, MAP_SHARED|MAP_ANONYMOUS) keep the same value in their frame index.
 * Every entry holds one reference on its slot (swap_dup/swap_free); a slot whose data is also in RAM is in the swap
 * cache (it holds one reference on that frame, and the frame's aux word names the entry), so pages shared by fork are
 * written once and come back as one frame.
 *
 * Locking: swap_lock (swap.c) protects the areas' slot maps, the swap cache and the frames' aux words. Order:
 * BKL -> address-space vm_lock -> cache_lock -> swap_lock -> pmm_lock. Disk I/O (swap_read, swap_writeback, swapon/off)
 * starts with the BKL held and no spinlock but its own address-space lock (the disk wait sleeps without the BKL,
 * blockio.c); everything else never sleeps and may run on the lockless fault path.
 *
 * Watermarks (reclaim.c, memory.c): per zone, Linux's min_free_kbytes = 4 * sqrt(lowmem KiB) split by zone size, low =
 * min + max(min / 4, 0.1% of the zone), high = low + the same step. User-page allocations (frame_alloc_user) leave
 * `min` free for the kernel; a zone below `low` wakes kswapd, which reclaims until every zone is above `high`. */
#include "../../kernel/types.h"
#include "../../kernel/fat32.h"
struct memory_file;
struct address_space;
struct registers;

/* ---- kernel services used by this code (smp.c, process.c, kernel.c) ---- */
bool spin_try_acquire(volatile int* lock);    /* a spinlock if free (counted like spin_acquire) */
unsigned spin_held_count(void);               /* spinlocks this CPU holds */
void task_sleep_uninterruptible(void* channel, uint64_t deadline);
void kernel_io_sleep(void* channel, uint64_t deadline); /* the same, the kernel globals (space, files, credentials) kept */
void task_set_io_free(bool io_free);          /* the running system call never reaches a filesystem (blockio.c) */

/* ---- memory.c ---- */
struct memory_zone_info { uint64_t begin, end; uint32_t managed, free, min, low, high; };
uint64_t frame_alloc_user(void);              /* zeroed user page that keeps the zones' min watermark; 0 = none */
bool memory_pressure(void);                   /* some zone fell below its low watermark (or an allocation failed) */
void memory_pressure_clear(void);
uint64_t memory_free_pages(void);
unsigned memory_zone_count(void);
bool memory_zone_get(unsigned index, struct memory_zone_info* info);
void memory_zone_watermarks(unsigned index, uint32_t min, uint32_t low, uint32_t high);
int memory_frame_zone(uint64_t address);      /* zone index of a managed frame, -1 otherwise */
uint32_t* memory_frame_aux(uint64_t address); /* the frame's swap-cache word (swap_lock), NULL if unmanaged */
/* mm.audit (debugging): a global frame index and raw reference count; every frame the page caches hold. */
uint64_t memory_frame_slots(void);
int64_t memory_frame_slot(uint64_t address, uint32_t* references);
uint64_t memory_slot_frame(uint64_t slot);
void memory_file_audit(void (*visit)(uint64_t frame, void* context), void* context);
void memory_file_audit_free(struct memory_file* file);
extern bool vm_audit_on;
void vm_audit(void);
bool reclaim_oom_hopeless(void);   /* the last OOM attempt found nothing it could kill */
bool reclaim_may_sleep(void);      /* direct reclaim (and the OOM wait) may sleep in this context */
uint32_t audit_sum(const void* data, uint64_t bytes);                       /* never 0 */
void audit_report(const char* what, const char* name, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
void memory_file_audit_init(void);

/* ---- swap entries ---- */
#define SWAP_MAX_AREAS 16                     /* Linux MAX_SWAPFILES is 32 minus migration/hwpoison types */
#define SWAP_MAX_PAGES (1ULL << 27)           /* slots per area (512 GiB): the aux word packs type and offset */
static inline bool swap_entry_is(uint64_t entry) { return (entry & 3) == 2; }
static inline uint64_t swap_entry_make(unsigned type, uint64_t offset) { return offset << 12 | (uint64_t)type << 2 | 2; }
static inline unsigned swap_entry_type(uint64_t entry) { return (unsigned)(entry >> 2) & 31; }
static inline uint64_t swap_entry_offset(uint64_t entry) { return (entry >> 12) & 0xFFFFFFFFFFULL; }

/* ---- swap.c ---- */
int64_t swap_call(struct registers* r);       /* swapon(167), swapoff(168) */
void swap_init(void);                         /* boot: the lock-free parts (nothing is allocated) */
int swap_boot(void);                          /* before the init session: /etc/fstab swap entries (returns how many) */
bool swap_writable(void);                     /* some area accepts new pages */
void swap_counts(uint64_t* total, uint64_t* free, uint64_t* cached); /* pages */
uint64_t swap_out_page(uint64_t frame, bool dirty); /* entry for a frame leaving a PTE or shmem slot (+1 reference), 0 = full */
bool swap_dup(uint64_t entry);                /* +1 reference (fork); false: invalid entry or count overflow */
void swap_free(uint64_t entry);               /* -1 reference; the last one frees the slot (and its cached copy) */
uint64_t swap_cache_get(uint64_t entry);      /* the cached frame, retained, or 0 (never sleeps) */
/* The caller holds `entry` (one reference) and a retained `frame` from swap_cache_get/swap_read. If that reference is
 * the only one and nobody else maps the frame, the entry and the cached copy are dropped and the frame becomes the
 * caller's private page (true). */
bool swap_take_exclusive(uint64_t entry, uint64_t frame);
uint64_t swap_frame_entry(uint64_t frame);    /* entry of a swap-cache frame, 0 if not cached */
uint64_t swap_read(uint64_t entry, bool may_oom, int* error); /* retained frame with the slot's data (cache, or disk + read-ahead) */
unsigned swap_entry_count(uint64_t entry);    /* references to the slot (smaps SwapPss) */
bool swap_area_idle(unsigned type);           /* no slot of that area is in use */
unsigned swap_writeback(unsigned budget, uint64_t* freed); /* write/free swap-cache pages nobody maps; returns pages written */
bool swap_area_unusable(unsigned type);       /* swapoff in progress on that area */
bool swap_file_busy(const struct memory_file* cache); /* an active swap file: writes/truncation get ETXTBSY */
int swap_proc_swaps(char* text, unsigned capacity);    /* /proc/swaps */
void swap_stats(uint64_t counters[4]);        /* pswpin, pswpout, swap-cache hits, read-ahead pages */

/* ---- reclaim.c ---- */
void reclaim_init(void);                      /* boot: watermarks, kswapd */
void reclaim_wake(void);                      /* kswapd: memory fell below a low watermark */
void reclaim_trace(void);                     /* "[64] MEMSTAT ..." line (session trace) */
void task_stack_pool_init(void);              /* process.c: kernel stacks kept for thread creation under pressure */
/* Direct reclaim from a sleepable context (page fault slow path, syscall entry): reclaims, may OOM-kill another
 * process and wait for it. True when a user page should be available now. */
bool reclaim_direct(unsigned pages);
bool reclaim_low(void);                       /* a user page would dip below the min watermark */
/* Before a large kernel allocation that cannot reclaim on its own (fork copies page tables and areas): direct reclaim,
 * no OOM kill, until `pages` fit above the min watermark. Only where sleeping is safe; true when they fit. */
bool reclaim_kernel_room(unsigned pages);
unsigned vm_fork_pages(void);                 /* vm_reclaim.inc: kernel pages a fork of the current space needs */
bool memory_file_read_failed(void);           /* shmem.c: the last memory_file_page failure was a read error, not memory */
uint64_t reclaim_alloc_user(bool may_oom);    /* frame_alloc_user with direct reclaim (and the OOM killer) behind it; may sleep */
int reclaim_swappiness(void);
uint64_t reclaim_available(void);             /* MemAvailable in pages */
unsigned reclaim_page_cluster(void);         /* vm.page-cluster: swap read-ahead of 1 << n pages */
void memory_file_swappable(struct memory_file* file); /* shmem.c: its pages may be swapped */
uint64_t memory_file_shmem_pages(void);
int64_t reclaim_sysctl(const char* name, char* text, unsigned capacity, const char* input, unsigned length); /* /proc/sys/vm */
int reclaim_proc_text(const char* name, char* text, unsigned capacity); /* /proc/vmstat, /proc/zoneinfo, /proc/meminfo */
uint64_t reclaim_oom_badness(struct address_space* space, int adj, uint64_t* points); /* /proc/PID/oom_score */
bool reclaim_oom_victim(uint32_t pid);        /* the OOM killer chose this process (its exit frees memory) */

/* ---- shmem.c ---- */
uint64_t memory_file_shrink(struct memory_file* file, uint64_t target); /* free clean unreferenced cache pages of a file */
uint64_t memory_file_reclaim(uint64_t target);  /* the same over every disk cache, round robin */
uint64_t memory_file_cached_pages(uint64_t* dirty);
uint64_t memory_file_pinned_pages(void);   /* orphan pages that hold the only copy of their data (not reclaimable) */
uint64_t memory_file_peek(struct memory_file* file, uint64_t index);  /* resident frame or 0 (no I/O) */
int memory_file_unuse(unsigned type);         /* swapoff: shared-memory pages of that area come back */
unsigned memory_file_swap_out(unsigned budget); /* shared-memory pages nobody maps go to swap */

/* ---- vm_reclaim.inc (kernel.c) ---- */
struct vm_usage { uint64_t resident, anon, file, shmem, swap, locked, page_tables; };
/* Reclaim scan: look at up to `budget` PTEs of the address spaces (a clock) and unmap the cold ones (accessed bit
 * clear): at most `max_anon` private pages go to the swap cache; file and shared-memory pages (when `file`) are
 * dropped from the page tables. Returns the pages unmapped. */
unsigned vm_reclaim_scan(unsigned budget, unsigned max_anon, bool file, unsigned priority, unsigned* scanned);
void vm_space_usage(struct address_space* space, struct vm_usage* usage);
int vm_swap_unuse(unsigned type);             /* swapoff: bring every page of that area back (0 or -ENOMEM) */
void vm_usage_total(struct vm_usage* usage);  /* all address spaces (meminfo AnonPages/Mapped) */
int space_smaps_entry(struct address_space* space, uint64_t index, char* text, unsigned capacity, bool rollup);
#endif
