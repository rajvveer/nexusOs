/* Sparse backing pages shared by descriptors and VM areas; dirty FAT data is retained on I/O failure. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "../../kernel/ata.h"
#include "swap.h"

#define PAGE 4096ULL
#define DIRTY 1ULL
/* An orphan's (unlinked, still open or mapped) page whose data is newer than its clusters: write-back skips orphans, so
 * this cache holds the only copy and reclaim must keep it. (Bit 1 marks swap entries in shared memory: not used.) */
#define PINNED 4ULL
#define FRAME (~(PAGE - 1))
struct memory_block { struct memory_block* next; uint64_t first; uint64_t frames[510]; };
_Static_assert(sizeof(struct memory_block) == PAGE, "shared memory index page");
_Static_assert(sizeof(struct memory_file) <= PAGE, "file cache record page");
static uint64_t next_inode;
static struct memory_file* disk_files;
/* Frames and blocks of every cache: removals, shared-memory insertions and the lockless fault
 * path's lookups (taken after an address-space lock, before the page allocator's). */
static volatile int cache_lock;
/* Write-back accounting: dirty pages across all disk caches and files with dirty state. */
static uint64_t dirty_total, dirty_files;
uint64_t memory_file_dirty_total(void) { return dirty_total; }
uint64_t memory_file_dirty_files(void) { return dirty_files; }
static void mark_dirty_file(struct memory_file* file) {
    if (!file->dirty_since) { file->dirty_since = native_clock_ns() | 1; dirty_files++; }
}
static void mark_clean_file(struct memory_file* file) {
    if (file->dirty_since) { file->dirty_since = 0; if (dirty_files) dirty_files--; }
}
static void drop_dirty_frame(struct memory_file* file, uint64_t* frame) {
    if (!(*frame & DIRTY)) return;
    *frame &= ~DIRTY; if (file->dirty_pages) file->dirty_pages--; if (dirty_total) dirty_total--;
}
/* Shared memory that may be swapped (memory_file_swappable): linked through `next`/`shmem_prev`. A slot of its blocks
 * holds a frame or a swap entry (swap.h encoding; bit 0, DIRTY, is never set on shared memory). */
static struct memory_file* shmem_files;
static uint64_t shmem_swapped;   /* slots holding swap entries */
static uint64_t pinned_total;   /* PINNED orphan pages: cached, but not reclaimable (MemAvailable leaves them out) */
static void pin_frame(uint64_t* frame) {
    if (!(*frame & PINNED)) { *frame |= PINNED; __atomic_add_fetch(&pinned_total, 1, __ATOMIC_RELAXED); }
}
uint64_t memory_file_pinned_pages(void) { return __atomic_load_n(&pinned_total, __ATOMIC_RELAXED); }
static void slot_release(uint64_t* slot) {
    if (swap_entry_is(*slot)) { swap_free(*slot); shmem_swapped--; }
    else { if (*slot & PINNED) __atomic_sub_fetch(&pinned_total, 1, __ATOMIC_RELAXED); page_free(*slot & FRAME); }
    *slot = 0;
}
static void fsum_forget(const struct memory_file* file, uint64_t from);
static void cache_free(struct memory_file* file) {
    if (vm_audit_on) { memory_file_audit_free(file); fsum_forget(file, 0); }
    mark_clean_file(file);
    spin_acquire(&cache_lock);
    if (file->swappable) {
        if (file->shmem_prev) file->shmem_prev->next = file->next; else shmem_files = file->next;
        if (file->next) file->next->shmem_prev = file->shmem_prev;
        file->swappable = false;
    }
    while (file->blocks) {
        struct memory_block* block = file->blocks; file->blocks = block->next;
        for (unsigned i = 0; i < 510; i++) if (block->frames[i]) { drop_dirty_frame(file, &block->frames[i]); slot_release(&block->frames[i]); }
        page_free((uint64_t)block);
    }
    spin_release(&cache_lock);
    page_free((uint64_t)file);
}
struct memory_file* memory_file_disk_find(const fat32_file_t* disk) {
    uint64_t inode = disk_inode_id(disk);
    for (struct memory_file* f = disk_files; f; f = f->next)
        if (disk_inode_id(&f->disk) == inode) return f;
    return NULL;
}
struct memory_file* memory_file_disk_get(const fat32_file_t* disk) {
    struct memory_file* file = memory_file_disk_find(disk);
    if (!file) {
        file = memory_file_create(0); if (!file) return NULL;
        file->disk_backed = true; file->disk = *disk; file->size = file->valid_size = disk->size;
        file->changed = (uint64_t)disk_mtime_ns(disk);
        file->next = disk_files; disk_files = file;
    }
    memory_file_retain(file); return file;
}
uint64_t memory_file_disk_pages(void) {
    uint64_t count = 0;
    for (struct memory_file* f = disk_files; f; f = f->next) count += 1 + f->index_pages + f->pages;
    return count;
}
void memory_file_disk_moved(const fat32_file_t* old, const fat32_file_t* moved) {
    struct memory_file* file = memory_file_disk_find(old); if (!file) return;
    file->disk = *moved;
    if (moved->detached) {   /* an orphan's data stays in this cache only: nothing is left to write back */
        spin_acquire(&cache_lock);
        /* Unwritten pages stay as the only copy (PINNED): page reclaim dropped them as clean and the next fault read the
         * stale clusters back (Chrome's unlinked shared-memory files and its deleted BrowserMetrics .pma mapping lost
         * histogram records under memory pressure, and the browser crashed on the corrupt allocator). */
        for (struct memory_block* b = file->blocks; b; b = b->next) for (unsigned i = 0; i < 510; i++)
            if (b->frames[i] & DIRTY) { drop_dirty_frame(file, &b->frames[i]); pin_frame(&b->frames[i]); }
        spin_release(&cache_lock);
        file->metadata_dirty = false; file->io_error = 0; mark_clean_file(file);
    }
    if (moved->detached && file->references == 1) { memory_file_retain(file); memory_file_release(file); }
}
struct memory_file* memory_file_create(unsigned flags) {
    struct memory_file* file = (void*)page_alloc(); if (!file) return NULL;
    file->references = 1; file->inode = ++next_inode; file->mode = flags & 8 ? 0666 : 0777;
    file->seals = flags & 8 ? 32 : flags & 2 ? 0 : 1;
    file->uid = credentials_kernel_id(credentials_current(), false, 3);
    file->gid = credentials_kernel_id(credentials_current(), true, 3);
    file->changed = native_realtime_ns(); return file;
}
void memory_file_retain(struct memory_file* file) {
    if (!file->references || file->references == ~0u) panic("shared memory reference overflow");
    file->references++;
}
void memory_file_release(struct memory_file* file) {
    if (!file || !file->references) panic("unowned shared memory file");
    file->references--;
    if (file->disk_backed && file->references == 1) {
        if (file->disk.detached) file_refresh_views(&file->disk);
        /* Unwritten data (or a failed writeback) keeps the cache alive for the flusher. */
        else if (file->io_error || file->metadata_dirty || file->dirty_pages) return;
        struct memory_file** link = &disk_files; while (*link && *link != file) link = &(*link)->next;
        if (!*link) panic("unowned disk page cache");
        *link = file->next; file->references = 0;
    }
    if (file->references) return;
    cache_free(file);
}
uint64_t disk_file_max_size(const fat32_file_t* file);   /* mount.c: the limit of the file's filesystem */
/* Unmount (mount.c): caches of that mount nobody uses any more go, written or not (a dead device keeps failing). */
void memory_file_forget_mount(unsigned id) {
    struct memory_file** link = &disk_files;
    while (*link) {
        struct memory_file* f = *link;
        if (f->disk.mount == id && f->references == 1) {
            spin_acquire(&cache_lock);
            for (struct memory_block* b = f->blocks; b; b = b->next) for (unsigned i = 0; i < 510; i++) drop_dirty_frame(f, &b->frames[i]);
            spin_release(&cache_lock);
            *link = f->next; f->references = 0; cache_free(f); continue;
        }
        link = &f->next;
    }
}
static uint8_t* writeback_buffer;
static bool run_buffer_busy;   /* writeback_buffer holds an outer run: nested page-cache I/O (loop devices) goes page by page */
/* mm.audit (debugging): checksum of every clean disk-cache page that is dropped, compared when it is loaded again. */
struct fsum { const struct memory_file* file; uint64_t index; uint32_t sum, bytes; };
#define FSUM_SLOTS 65536U
static struct fsum* fsums;
void memory_file_audit_init(void) {
    if (!fsums) { fsums = (struct fsum*)arena_alloc(FSUM_SLOTS * sizeof(struct fsum)); if (fsums) vga_print("[64] AUDIT file checksums on\n"); }
}
static unsigned fsum_hash(const struct memory_file* file, uint64_t index) {
    uint64_t h = ((uint64_t)file >> 12) * 0x9E3779B97F4A7C15ULL ^ index * 0xC2B2AE3D27D4EB4FULL; return (unsigned)(h >> 40) % FSUM_SLOTS;
}
static void fsum_put(const struct memory_file* file, uint64_t index, uint64_t frame) {
    if (!fsums || index * PAGE >= file->size) return;
    uint64_t bytes = file->size - index * PAGE < PAGE ? file->size - index * PAGE : PAGE;
    struct fsum* e = &fsums[fsum_hash(file, index)];
    *e = (struct fsum){file, index, audit_sum(phys_to_virt(frame), bytes), (uint32_t)bytes};
}
static void fsum_check(const struct memory_file* file, uint64_t index, uint64_t frame, const char* how) {
    if (!fsums) return;
    struct fsum* e = &fsums[fsum_hash(file, index)];
    if (e->file != file || e->index != index) return;
    uint32_t now = audit_sum(phys_to_virt(frame), e->bytes);
    if (now != e->sum) audit_report(how, file->disk.name, index, file->size, file->valid_size, (uint64_t)e->sum << 32 | now);
    e->file = NULL;
}
static void fsum_forget(const struct memory_file* file, uint64_t from) {
    if (!fsums) return;
    for (unsigned i = 0; i < FSUM_SLOTS; i++) if (fsums[i].file == file && fsums[i].index >= from) fsums[i].file = NULL;
}
static uint64_t readahead_pages, cache_misses;
void memory_file_cache_stats(uint64_t* misses, uint64_t* readahead) { *misses = cache_misses; *readahead = readahead_pages; }
/* memory_file_page returned 0 because a read failed (SIGBUS), not for want of memory (retry after reclaim). Big lock. */
static bool read_failed;
bool memory_file_read_failed(void) { return read_failed; }
static uint64_t new_frame(struct memory_file* file, uint64_t index) {
    uint64_t frame = frame_alloc_user(); if (!frame) return 0;   /* cache pages are user memory: the kernel reserve stays */
    uint64_t offset = index * PAGE;
    if (file->disk_backed && offset < file->valid_size) {
        cache_misses++;
        uint32_t count = file->valid_size - offset < PAGE ? file->valid_size - offset : PAGE;
        if (fat32_read_file(&file->disk, offset, count, phys_to_virt(frame)) != (int32_t)count) {
            vga_print("[64] FILE_CACHE read failed: "); vga_print(file->disk.name); vga_print("\n");
            read_failed = true; page_free(frame); return 0;
        }
    }
    if (file->disk_backed) fsum_check(file, index, frame, offset < file->valid_size ? "FILE_REREAD_MISMATCH" : "FILE_ZEROFILL_MISMATCH");
    return frame;
}
static uint64_t cache_page(struct memory_file* file, uint64_t index, bool create);
static uint64_t* cache_slot(struct memory_file* file, uint64_t index) {
    uint64_t first = index / 510 * 510;
    for (struct memory_block* b = file->blocks; b && b->first <= first; b = b->next) if (b->first == first) return &b->frames[index - first];
    return NULL;
}
uint64_t memory_file_page(struct memory_file* file, uint64_t index, bool create) {
    read_failed = false;
    if (file->disk_backed) return cache_page(file, index, create); /* only this (locked) path fills it */
    for (;;) {
        spin_acquire(&cache_lock); /* shared memory: the fault path may add pages as well */
        uint64_t* slot = cache_slot(file, index), entry = slot ? *slot : 0;
        if (!swap_entry_is(entry)) { uint64_t frame = cache_page(file, index, create); spin_release(&cache_lock); return frame; }
        spin_release(&cache_lock);
        int error; uint64_t frame = swap_read(entry, true, &error);   /* may sleep */
        if (!frame) {
            spin_acquire(&cache_lock); slot = cache_slot(file, index); bool same = slot && *slot == entry; spin_release(&cache_lock);
            if (!same) continue;   /* truncated or brought back while it was read: look again */
            read_failed = error != -12; return 0;
        }
        spin_acquire(&cache_lock);
        slot = cache_slot(file, index);
        bool installed = slot && *slot == entry;
        if (installed) { *slot = frame; shmem_swapped--; }   /* the retained reference becomes the cache's */
        spin_release(&cache_lock);
        if (installed) { swap_free(entry); return frame; }
        page_free(frame);   /* changed while it was read: look again */
    }
}
uint64_t memory_file_fault_page(struct memory_file* file, uint64_t index, bool create) {
    uint64_t frame = 0;
    spin_acquire(&cache_lock);
    if (index < file->size / PAGE + ((file->size & (PAGE - 1)) != 0)) {
        frame = cache_page(file, index, create && !file->disk_backed);
        if (frame) page_retain(frame);
    }
    spin_release(&cache_lock); return frame;
}
/* Blocks and frames are published with release stores: the fault path may be reading the list. */
static uint64_t cache_page(struct memory_file* file, uint64_t index, bool create) {
    uint64_t first = index / 510 * 510;
    struct memory_block** link = &file->blocks;
    while (*link && (*link)->first < first) link = &(*link)->next;
    struct memory_block* block = *link;
    if (!block || block->first != first) {
        if (!create) return 0;
        block = (void*)page_alloc(); if (!block) return 0;
        uint64_t frame = new_frame(file, index); if (!frame) { page_free((uint64_t)block); return 0; }
        block->first = first; block->frames[index - first] = frame; block->next = *link;
        __atomic_store_n(link, block, __ATOMIC_RELEASE); file->pages++; file->index_pages++;
        return frame;
    }
    uint64_t* frame = &block->frames[index - first];
    if (swap_entry_is(*frame)) return 0;   /* swapped shared memory: memory_file_page brings it back */
    if (!*frame && create) {
        uint64_t count = 1;
        if (file->disk_backed && writeback_buffer && !run_buffer_busy && index * PAGE < file->valid_size) {
            while (count < 16 && index - first + count < 510 && !block->frames[index - first + count] &&
                   (index + count) * PAGE < file->valid_size) count++;
        }
        if (count > 1) {
            uint64_t offset = index * PAGE, bytes = count * PAGE;
            if (offset + bytes > file->valid_size) bytes = file->valid_size - offset;
            cache_misses++;
            run_buffer_busy = true;
            bool read_ok = fat32_read_file(&file->disk, offset, bytes, writeback_buffer) == (int32_t)bytes;
            run_buffer_busy = false;
            if (read_ok) {
                for (uint64_t i = 0; i < count; i++) {
                    uint64_t page = frame_alloc_user(); if (!page) break;
                    uint64_t have = bytes > i * PAGE ? bytes - i * PAGE : 0; if (have > PAGE) have = PAGE;
                    memcpy(phys_to_virt(page), writeback_buffer + i * PAGE, have);
                    fsum_check(file, index + i, page, "FILE_READAHEAD_MISMATCH");
                    __atomic_store_n(&block->frames[index - first + i], page, __ATOMIC_RELEASE); file->pages++;
                    if (i) readahead_pages++;
                }
                return block->frames[index - first] & FRAME;
            }
        }
        uint64_t fresh = new_frame(file, index);
        __atomic_store_n(frame, fresh, __ATOMIC_RELEASE); if (fresh) file->pages++;
    }
    return *frame & FRAME;
}
void memory_file_dirty(struct memory_file* file, uint64_t index) {
    file->changed = native_realtime_ns();
    if (!file->disk_backed) return;
    if (file->disk.detached) {   /* orphans are never written back: the store pins the page instead */
        for (struct memory_block* b = file->blocks; b; b = b->next) if (index >= b->first && index - b->first < 510) {
            if (b->frames[index - b->first]) pin_frame(&b->frames[index - b->first]);
            return;
        }
        return;
    }
    if (!disk_posix()) file->changed = file->changed / 2000000000 * 2000000000; /* FAT write-time granularity */
    for (struct memory_block* b = file->blocks; b; b = b->next) if (index >= b->first && index - b->first < 510) {
        if (!b->frames[index - b->first]) break;
        if (!(b->frames[index - b->first] & DIRTY)) { b->frames[index - b->first] |= DIRTY; file->dirty_pages++; dirty_total++; }
        file->metadata_dirty = true; mark_dirty_file(file); return;
    }
    panic("dirty file page has no backing");
}
/* Runs of consecutive dirty pages are written with one FAT call (one ATA command per
 * physically contiguous span) through a contiguous bounce buffer. */
#define WRITEBACK_RUN_PAGES 64ULL
#define WRITEBACK_KEEP_PAGES 2048ULL /* clean pages kept per file; larger streams are trimmed */
/* Allocated once at boot so later program runs see no new kernel-owned pages. */
void memory_file_writeback_init(void) { if (!writeback_buffer) writeback_buffer = (uint8_t*)arena_alloc(WRITEBACK_RUN_PAGES * PAGE); }
static int write_run(struct memory_file* file, uint64_t first, uint64_t pages, uint64_t** slots, bool shared) {
    uint64_t offset = first * PAGE, bytes = pages * PAGE;
    if (offset + bytes > file->size) bytes = file->size - offset;
    /* Clean before the data is read (Linux page_mkclean ahead of write-back): a store through a shared mapping while
     * the run is on its way to disk faults and dirties the page again. Cleaning afterwards lost such stores, and page
     * reclaim (reclaim.c) then dropped the newer data with the "clean" page. */
    if (shared) memory_file_writeprotect_range(file, first, pages);
    for (uint64_t i = 0; i < pages; i++) drop_dirty_frame(file, slots[i]);
    int error = 0;
    if (writeback_buffer && pages > 1 && !run_buffer_busy) {
        for (uint64_t i = 0; i < pages; i++) memcpy(writeback_buffer + i * PAGE, phys_to_virt(*slots[i] & FRAME), PAGE);
        run_buffer_busy = true;
        int result = fat32_write_file(&file->disk, offset, bytes, writeback_buffer);
        run_buffer_busy = false;
        if (result != (int)bytes) error = result == -2 ? -28 : -5;
    } else for (uint64_t i = 0; i < pages && !error; i++) {
        uint64_t at = offset + i * PAGE, count = file->size - at < PAGE ? file->size - at : PAGE;
        int result = fat32_write_file(&file->disk, at, count, phys_to_virt(*slots[i] & FRAME));
        if (result != (int)count) error = result == -2 ? -28 : -5;
    }
    if (error) { for (uint64_t i = 0; i < pages; i++) memory_file_dirty(file, first + i); return error; }   /* retried later */
    /* The disk was cut to valid_size before this pass, so everything below the run is either
     * written data, zero-filled holes, or still-dirty cached pages. */
    if (offset + bytes > file->valid_size && offset + bytes <= file->disk.size) file->valid_size = offset + bytes;
    return 0;
}
static int sync_file(struct memory_file* file, uint64_t begin, uint64_t end, uint64_t budget, uint64_t* spent_out, bool* complete) {
    *complete = true; if (spent_out) *spent_out = 0;
    if (!file->disk_backed) return 0;
    if (!file->metadata_dirty && !file->dirty_pages) { file->io_error = 0; mark_clean_file(file); return 0; }
    kstat.syncs++; kstat.sync_pages += file->dirty_pages;
    int error = fat32_refresh_file(&file->disk); bool written = false, partial = false;
    uint32_t view_first = file->disk.first; uint64_t view_size = file->disk.size;
    if (!error && file->metadata_dirty) {
        if (file->valid_size < file->disk.size) error = fat32_truncate_file(&file->disk, file->valid_size);
    }
    bool shared = !error && memory_file_writable_maps(file);
    uint64_t spent = 0, run_first = 0, run_pages = 0; uint64_t* slots[WRITEBACK_RUN_PAGES];
    for (struct memory_block* b = file->blocks; !error && b; b = b->next)
        for (unsigned i = 0; i < 510 && !error; i++) {
            uint64_t index = b->first + i, offset = index * PAGE;
            bool in_range = (b->frames[i] & DIRTY) && offset < end && offset + PAGE > begin;
            if (in_range && offset >= file->size) { drop_dirty_frame(file, &b->frames[i]); in_range = false; }
            if (in_range && run_pages && index == run_first + run_pages && run_pages < WRITEBACK_RUN_PAGES) { slots[run_pages++] = &b->frames[i]; continue; }
            if (run_pages) { error = write_run(file, run_first, run_pages, slots, shared); written = true; spent += run_pages * PAGE; run_pages = 0; }
            if (!in_range || error) continue;
            if (spent >= budget) { partial = true; continue; }
            run_first = index; slots[0] = &b->frames[i]; run_pages = 1;
        }
    if (!error && run_pages) { error = write_run(file, run_first, run_pages, slots, shared); written = true; spent += run_pages * PAGE; }
    if (spent_out) *spent_out = spent + PAGE;
    if (!error && partial) { *complete = false; return 0; }
    /* Dirty pages extend the file with their actual data; zero only the remaining holes/tail. */
    if (!error && file->metadata_dirty && file->disk.size != file->size) error = fat32_truncate_file(&file->disk, file->size);
    if (!error && (written || file->metadata_dirty)) error = disk_file_stamp(&file->disk, file->changed);
    if (error) {
        *complete = false;
        file->io_error = error == -2 || error == -28 ? -28 : -5;
        vga_print("[64] FILE_WRITEBACK failed; dirty data retained: "); vga_print(file->disk.name); vga_print("\n");
        return file->io_error;
    }
    file->valid_size = file->size; file->metadata_dirty = false;
    if (!file->dirty_pages) { file->io_error = 0; mark_clean_file(file); }
    if (file->disk.first != view_first || file->disk.size != view_size) memory_file_refresh_views(file);
    /* Clean pages without a mapping can be reloaded; streaming writes stay bounded. */
    if (file->pages <= WRITEBACK_KEEP_PAGES) return 0;
    spin_acquire(&cache_lock); /* a fault may be taking a reference to one of these frames */
    struct memory_block** link = &file->blocks;
    while (*link) {
        struct memory_block* b = *link; bool used = false;
        for (unsigned i = 0; i < 510; i++) {
            if (b->frames[i] && !(b->frames[i] & (DIRTY | PINNED)) && page_references(b->frames[i] & FRAME) == 1) {
                fsum_put(file, b->first + i, b->frames[i] & FRAME);
                page_free(b->frames[i] & FRAME); b->frames[i] = 0; file->pages--;
            }
            if (b->frames[i]) used = true;
        }
        if (used) link = &b->next;
        else { *link = b->next; page_free((uint64_t)b); file->index_pages--; }
    }
    spin_release(&cache_lock);
    return 0;
}
int memory_file_sync(struct memory_file* file, uint64_t begin, uint64_t end) {
    bool complete; return sync_file(file, begin, end, ~0ULL, NULL, &complete);
}
/* Flush dirty caches older than min_age (all: every dirty cache, unbudgeted, retrying errors).
 * Caches no longer referenced by any descriptor or mapping are released once clean.
 * Returns how many dirty caches remain unwritten. */
int memory_file_writeback(bool all, uint64_t min_age_ns, uint64_t budget) {
    uint64_t now = native_clock_ns(), spent = 0; int pending = 0;
    if (all) budget = ~0ULL;
    kstat.flush_passes++;
    struct memory_file** link = &disk_files;
    while (*link) {
        struct memory_file* f = *link;
        bool dirty = f->metadata_dirty || f->dirty_pages;
        if (dirty && !f->disk.detached && (all || (!f->io_error && now - f->dirty_since >= min_age_ns))) {
            if (spent < budget) {
                uint64_t wrote = 0; bool complete = true;
                int error = sync_file(f, 0, ~0ULL, budget - spent, &wrote, &complete);
                spent += wrote;
                if (error || !complete) pending++;
            } else pending++;
        }
        if (f->references == 1 && (f->disk.detached || (!f->io_error && !f->metadata_dirty && !f->dirty_pages))) {
            if (f->disk.detached) file_refresh_views(&f->disk);
            *link = f->next; f->references = 0; cache_free(f); continue;
        }
        link = &f->next;
    }
    disk_writeback_complete(all, spent != 0);
    return pending;
}
int64_t memory_file_io(struct memory_file* file, uint64_t address, uint64_t count, uint64_t offset, bool write) {
    if ((int64_t)offset < 0) return -22;
    if (count > 0x7FFFF000) count = 0x7FFFF000;
    if (!count) return 0;
    if (write) {
        if (file->seals & 24) return -1;
        if (file->disk_backed && swap_file_busy(file)) return -26;   /* Linux: an active swap file is immutable */
        if (count > 0x7FFFFFFFFFFFFFFFULL - offset) return -27;
        if (file->disk_backed && (offset > disk_file_max_size(&file->disk) || count > disk_file_max_size(&file->disk) - offset)) return -27;
        if ((file->seals & 4) && offset + count > file->size) return -1;
    } else {
        if (offset >= file->size) return 0;
        if (count > file->size - offset) count = file->size - offset;
    }
    if (!accessible(address, count, !write)) return -14;
    uint64_t done = 0;
    while (done < count) {
        uint64_t at = offset + done, part = PAGE - (at & (PAGE - 1)); if (part > count - done) part = count - done;
        /* Dirty throttling (Linux balance_dirty_pages): past 10% of RAM in dirty cache the writer writes back first. */
        if (write && file->disk_backed && dirty_total > memory_total_pages() / 10) memory_file_writeback(false, 0, 1 << 20);
        if (memory_pressure()) reclaim_wake();
        uint64_t frame = memory_file_page(file, at / PAGE, write || file->disk_backed);
        /* No page for want of memory: reclaim (or the OOM killer) and retry, like the fault path -- no ENOMEM on a valid
         * buffer while something can still be freed; a read error ends the copy at once. */
        for (unsigned attempt = 0; !frame && (write || file->disk_backed) && !memory_file_read_failed() && !task_exiting() && attempt < 64; attempt++) {
            if (!reclaim_direct(1) && reclaim_oom_hopeless()) break;
            frame = memory_file_page(file, at / PAGE, true);
        }
        if ((write || file->disk_backed) && !frame) return done ? (int64_t)done : file->disk_backed ? -5 : -12;
        if (frame) page_retain(frame);   /* the copy may fault on the user buffer: reclaim must not take the page */
        if (write) {   /* SMAP copies: a user buffer unmapped meanwhile (the disk wait dropped the BKL) ends the copy short */
            uint64_t left = uaccess_copy((uint8_t*)phys_to_virt(frame) + (at & (PAGE - 1)), (void*)(address + done), part); part -= left;
            if (at + part > file->size) file->size = at + part;
            memory_file_dirty(file, at / PAGE);
            if (left) { page_free(frame); return done + part ? (int64_t)(done + part) : -14; }
        } else if (frame) { if (copy_to_user((void*)(address + done), (uint8_t*)phys_to_virt(frame) + (at & (PAGE - 1)), part)) { page_free(frame); return done ? (int64_t)done : -14; } }
        else if (clear_user(address + done, part)) return done ? (int64_t)done : -14;
        if (frame) page_free(frame);
        done += part;
    }
    return done;
}
int memory_file_truncate(struct memory_file* file, uint64_t size) {
    if ((int64_t)size < 0) return -22;
    if (file->disk_backed && swap_file_busy(file)) return -26;
    if (file->disk_backed && size > disk_file_max_size(&file->disk)) return -27;
    if ((size < file->size && (file->seals & 2)) || (size > file->size && (file->seals & 4))) return -1;
    if (size < file->size) {
        if (file->disk_backed && size < file->valid_size) file->valid_size = size;
        /* New end first: once the mappings are gone, no fault may map a page past it again. */
        spin_acquire(&cache_lock); file->size = size; spin_release(&cache_lock);
        memory_file_invalidate(file, size);
        uint64_t end = (size + PAGE - 1) / PAGE;
        fsum_forget(file, size / PAGE);
        spin_acquire(&cache_lock);
        struct memory_block** link = &file->blocks;
        while (*link) {
            struct memory_block* block = *link; bool used = false;
            for (unsigned i = 0; i < 510; i++) if (block->frames[i]) {
                if (block->first + i >= end) { drop_dirty_frame(file, &block->frames[i]); slot_release(&block->frames[i]); file->pages--; }
                else used = true;
            }
            if (used) link = &block->next;
            else { *link = block->next; page_free((uint64_t)block); file->index_pages--; }
        }
        spin_release(&cache_lock);
        if (size & (PAGE - 1)) {
            uint64_t frame = memory_file_page(file, size / PAGE, false);
            if (frame) memset((uint8_t*)phys_to_virt(frame) + (size & (PAGE - 1)), 0, PAGE - (size & (PAGE - 1)));
        }
    } else if (file->disk_backed && size > file->size && (file->size & (PAGE - 1))) {
        uint64_t frame = memory_file_page(file, file->size / PAGE, false);
        if (frame) memset((uint8_t*)phys_to_virt(frame) + (file->size & (PAGE - 1)), 0, PAGE - (file->size & (PAGE - 1)));
    }
    if (file->disk_backed && !file->disk.detached) { file->metadata_dirty = true; mark_dirty_file(file); }
    file->size = size; file->changed = native_realtime_ns(); return 0;
}
/* fallocate(PUNCH_HOLE) / madvise(MADV_REMOVE): free whole pages in the range, zero partial ones. */
int memory_file_punch(struct memory_file* file, uint64_t offset, uint64_t length) {
    if (file->disk_backed) return -95;
    if (file->seals & 24) return -1;
    uint64_t end = length > file->size - (offset < file->size ? offset : file->size) ? file->size : offset + length;
    if (offset >= end) return 0;
    uint64_t first = (offset + PAGE - 1) / PAGE, last = end / PAGE;
    for (uint64_t at = offset; at < end;) {
        uint64_t index = at / PAGE, stop = (index + 1) * PAGE < end ? (index + 1) * PAGE : end;
        if (index < first || index >= last) {
            uint64_t frame = memory_file_page(file, index, false);
            if (frame) memset((uint8_t*)phys_to_virt(frame) + (at & (PAGE - 1)), 0, stop - at);
        }
        at = stop;
    }
    if (first >= last) { file->changed = native_realtime_ns(); return 0; }
    /* Mappings first (each space locked), then the cache slots under cache_lock -- the lockless fault path reads the
     * blocks under it -- and the mappings once more for a fault that raced in between. Unlocked, a fault on another
     * CPU could read a freed index block or keep mapping a page the file no longer has (Chrome purges discardable
     * shared memory this way under memory pressure). */
    memory_file_invalidate_range(file, first, last);
    spin_acquire(&cache_lock);
    struct memory_block** link = &file->blocks;
    while (*link) {
        struct memory_block* block = *link; bool used = false;
        for (unsigned i = 0; i < 510; i++) if (block->frames[i]) {
            uint64_t index = block->first + i;
            if (index >= first && index < last) {
                drop_dirty_frame(file, &block->frames[i]);
                slot_release(&block->frames[i]); file->pages--;
            } else used = true;
        }
        if (used) link = &block->next;
        else { *link = block->next; page_free((uint64_t)block); file->index_pages--; }
    }
    spin_release(&cache_lock);
    memory_file_invalidate_range(file, first, last);
    file->changed = native_realtime_ns(); return 0;
}
int memory_file_seal(struct memory_file* file, uint64_t seals) {
    if (seals & ~63ULL) return -22;
    if (file->seals & 1) return -1;
    if ((seals & 32) && (file->mode & 0111)) seals |= 2 | 4 | 8 | 16;
    if ((seals & 8) && !(file->seals & 8) && memory_file_writable_maps(file)) return -16;
    file->seals |= seals; return 0;
}
bool memory_file_access(struct memory_file* file, bool write) {
    struct credentials* credentials = credentials_current();
    unsigned shift = credentials_kernel_id(credentials, false, 3) == file->uid ? 6 :
                     credentials_kernel_id(credentials, true, 3) == file->gid ? 3 : 0;
    return ((file->mode >> shift) & (write ? 2 : 4)) || credentials_capable(credentials, NULL, 1) ||
           (!write && credentials_capable(credentials, NULL, 2));
}
int memory_file_chmod(struct memory_file* file, uint64_t mode) {
    if (credentials_kernel_id(credentials_current(), false, 3) != file->uid && !credentials_capable(credentials_current(), NULL, 3)) return -1;
    /* shortcut: set-ID inode execution is unsupported; do not promise those bits. */
    if (mode & ~0777ULL) return -95;
    if ((file->seals & 32) && ((file->mode ^ mode) & 0111)) return -1;
    file->mode = mode; file->changed = native_realtime_ns(); return 0;
}
void memory_file_stat(struct memory_file* file, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 10; stat->ino = file->inode;
    stat->mode = 0100000 | file->mode; stat->uid = credentials_visible_id(file->uid, false); stat->gid = credentials_visible_id(file->gid, true);
    stat->size = file->size; stat->block_size = PAGE; stat->blocks = file->pages * 8;
    stat->atime = stat->mtime = stat->ctime = file->changed / 1000000000;
    stat->atime_ns = stat->mtime_ns = stat->ctime_ns = file->changed % 1000000000;
}

/* ---- page reclaim (reclaim.c) ----
 * Clean pages of disk caches that only the cache holds (no mapping, no copy in progress) can be read again: they
 * are freed. Dirty pages are written by memory_file_writeback first. Index pages left empty are freed too. */
uint64_t memory_file_shrink(struct memory_file* file, uint64_t target) {
    uint64_t freed = 0;
    if (!file->disk_backed) return 0;
    spin_acquire(&cache_lock); /* a fault may be taking a reference to one of these frames */
    struct memory_block** link = &file->blocks;
    while (*link && freed < target) {
        struct memory_block* b = *link; bool used = false;
        for (unsigned i = 0; i < 510; i++) {
            if (b->frames[i] && !(b->frames[i] & (DIRTY | PINNED)) && freed < target && page_references(b->frames[i] & FRAME) == 1) {
                fsum_put(file, b->first + i, b->frames[i] & FRAME);
                page_free(b->frames[i] & FRAME); b->frames[i] = 0; file->pages--; freed++;
            }
            if (b->frames[i]) used = true;
        }
        if (used) link = &b->next;
        else { *link = b->next; page_free((uint64_t)b); file->index_pages--; freed++; }
    }
    spin_release(&cache_lock);
    return freed;
}
/* One round-robin pass over the disk caches (big lock held: the list is not changed meanwhile). */
uint64_t memory_file_reclaim(uint64_t target) {
    static uint64_t cursor; uint64_t freed = 0, files = 0, index = 0;
    for (struct memory_file* f = disk_files; f; f = f->next) files++;
    if (!files) return 0;
    struct memory_file* f = disk_files;
    for (uint64_t start = cursor % files; index < start; index++) f = f->next;
    for (uint64_t visited = 0; visited < files && freed < target; visited++) {
        if (!f) { f = disk_files; index = 0; }
        freed += memory_file_shrink(f, target - freed);
        f = f->next; index++;
    }
    cursor = index;
    return freed;
}
uint64_t memory_file_cached_pages(uint64_t* dirty) {
    uint64_t pages = 0; *dirty = dirty_total;
    for (struct memory_file* f = disk_files; f; f = f->next) pages += f->pages;
    return pages;
}

void memory_file_audit(void (*visit)(uint64_t frame, void* context), void* context) {
    spin_acquire(&cache_lock);
    for (int list = 0; list < 2; list++)
        for (struct memory_file* f = list ? shmem_files : disk_files; f; f = f->next)
            for (struct memory_block* b = f->blocks; b; b = b->next)
                for (unsigned i = 0; i < 510; i++) { uint64_t v = b->frames[i]; if (v && !swap_entry_is(v)) visit(v & FRAME, context); }
    spin_release(&cache_lock);
}
/* ---- shared memory and swap ---- */
void memory_file_swappable(struct memory_file* file) {
    if (file->disk_backed || file->swappable) return;
    spin_acquire(&cache_lock);
    file->swappable = true; file->shmem_prev = NULL; file->next = shmem_files;
    if (shmem_files) shmem_files->shmem_prev = file;
    shmem_files = file;
    spin_release(&cache_lock);
}
uint64_t memory_file_peek(struct memory_file* file, uint64_t index) {
    spin_acquire(&cache_lock);
    uint64_t* slot = cache_slot(file, index), value = slot ? *slot : 0;
    spin_release(&cache_lock);
    return value && !swap_entry_is(value) ? value & FRAME : 0;
}
uint64_t memory_file_shmem_pages(void) {
    uint64_t pages = 0;
    spin_acquire(&cache_lock);
    for (struct memory_file* f = shmem_files; f; f = f->next) pages += f->pages;
    pages = pages > shmem_swapped ? pages - shmem_swapped : 0;
    spin_release(&cache_lock);
    return pages;
}
/* Pages of swappable shared memory that nothing maps or copies (only the cache holds them) get swap entries; the
 * swap cache keeps the frame until swap_writeback has written it. Round robin over the files. */
unsigned memory_file_swap_out(unsigned budget) {
    static uint64_t cursor; unsigned done = 0; uint64_t files = 0, index = 0;
    if (!swap_writable()) return 0;
    spin_acquire(&cache_lock);
    for (struct memory_file* f = shmem_files; f; f = f->next) files++;
    struct memory_file* f = shmem_files;
    if (files) for (uint64_t start = cursor % files; index < start; index++) f = f->next;
    for (uint64_t visited = 0; visited < files && done < budget; visited++, f = f->next, index++) {
        if (!f) { f = shmem_files; index = 0; }
        for (struct memory_block* b = f->blocks; b && done < budget; b = b->next)
            for (unsigned i = 0; i < 510 && done < budget; i++) {
                uint64_t value = b->frames[i];
                if (!value || swap_entry_is(value) || page_references(value & FRAME) != 1) continue;
                uint64_t entry = swap_out_page(value & FRAME, true); if (!entry) { visited = files; break; }
                b->frames[i] = entry; shmem_swapped++; page_free(value & FRAME); done++;
            }
    }
    cursor = index;
    spin_release(&cache_lock);
    return done;
}
/* swapoff: every shared-memory slot of that area comes back into its file (may sleep: each file is retained). */
static bool next_swapped(struct memory_file* f, unsigned type, uint64_t from, uint64_t* found) {
    for (struct memory_block* b = f->blocks; b; b = b->next) {
        if (b->first + 510 <= from) continue;
        for (uint64_t at = from > b->first ? from - b->first : 0; at < 510; at++) {
            uint64_t value = b->frames[at];
            if (swap_entry_is(value) && swap_entry_type(value) == type) { *found = b->first + at; return true; }
        }
    }
    return false;
}
int memory_file_unuse(unsigned type) {
    spin_acquire(&cache_lock); struct memory_file* f = shmem_files; if (f) f->references++; spin_release(&cache_lock);
    while (f) {
        for (uint64_t index = 0; ; index++) {
            spin_acquire(&cache_lock); bool more = next_swapped(f, type, index, &index); spin_release(&cache_lock);
            if (!more) break;
            if (!memory_file_page(f, index, false)) { memory_file_release(f); return -12; }
        }
        spin_acquire(&cache_lock); struct memory_file* following = f->swappable ? f->next : NULL; if (following) following->references++; spin_release(&cache_lock);
        memory_file_release(f);
        f = following;
        if (f && (task_exiting() || signals_interrupt_pending())) { memory_file_release(f); return -4; }
    }
    return 0;
}
