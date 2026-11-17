/* inotify(7): watches on (device, inode) identities. The file layer (kernel.c) reports operations on
 * canonical paths with inotify_event(); events go to watches on the object itself and on its parent
 * directory (with the entry name), following Linux fsnotify routing, merging and overflow rules. */
#include "native.h"
#include "../../kernel/string.h"

#define QUEUE_BYTES 65536U     /* per instance; Linux max_queued_events = 16384 events */
#define MAX_WATCHES 65536U     /* fs.inotify.max_user_watches */
#define MAX_INSTANCES 128U     /* fs.inotify.max_user_instances */
#define ALL_EVENTS 0xFFFU
#define IN_UNMOUNT 0x2000U
#define IN_Q_OVERFLOW 0x4000U
#define IN_IGNORED 0x8000U
#define IN_ONLYDIR 0x01000000U
#define IN_DONT_FOLLOW 0x02000000U
#define IN_EXCL_UNLINK 0x04000000U
#define IN_MASK_CREATE 0x10000000U
#define IN_MASK_ADD 0x20000000U
#define IN_ISDIR 0x40000000U
#define IN_ONESHOT 0x80000000U
#define SELF_EVENTS (IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_OPEN | IN_DELETE_SELF | IN_MOVE_SELF)
#define CHILD_EVENTS (IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_OPEN | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE)
#define BUCKETS 512U
struct inotify;
struct watch {
    struct watch *next, *hash_next; struct inotify* owner;
    uint64_t device, inode; uint32_t mask; int wd;
};
#define WATCHES_PER_PAGE ((4096 - 16) / sizeof(struct watch))
struct watch_page { struct watch_page* next; unsigned used, pad; struct watch slots[WATCHES_PER_PAGE]; };
_Static_assert(sizeof(struct watch_page) <= 4096, "watch slab page");
struct inotify {
    struct inotify* next; struct watch* watches; uint8_t* queue;
    unsigned head, used, last, last_size; /* last: offset (from head) of the newest queued event */
    int next_wd; bool overflowed; uint64_t id;
};
unsigned inotify_watches;
static unsigned instances;
static uint64_t next_id;
static uint32_t cookies;
static struct watch_page* watch_pages;
static struct watch** buckets; /* one page, allocated while any watch exists */
static const struct object_operations inotify_operations;

uint32_t inotify_cookie(void) { if (!++cookies) cookies = 1; return cookies; }
static unsigned bucket(uint64_t device, uint64_t inode) { return (unsigned)(((inode ^ device * 31) * 0x9E3779B97F4A7C15ULL) >> 55) & (BUCKETS - 1); }
static struct watch* watch_alloc(void) {
    struct watch_page* page = watch_pages;
    while (page && page->used == WATCHES_PER_PAGE) page = page->next;
    if (!page) {
        page = (void*)page_alloc(); if (!page) return NULL;
        page->next = watch_pages; watch_pages = page;
    }
    for (unsigned i = 0; i < WATCHES_PER_PAGE; i++) if (!page->slots[i].owner) { page->used++; return &page->slots[i]; }
    panic("inotify watch slab accounting");
}
static void watch_free(struct watch* w) {
    struct watch_page* page = (void*)((uint64_t)w & ~4095ULL);
    memset(w, 0, sizeof(*w));
    if (--page->used) return;
    struct watch_page** link = &watch_pages; while (*link && *link != page) link = &(*link)->next;
    if (!*link) panic("unowned inotify watch page");
    *link = page->next; page_free((uint64_t)page);
}
/* ---- event queue: a byte ring of struct inotify_event records ---- */
static void ring_write(struct inotify* in, unsigned offset, const void* data, unsigned bytes) {
    const uint8_t* source = data;
    for (unsigned i = 0; i < bytes; i++) in->queue[(in->head + offset + i) % QUEUE_BYTES] = source[i];
}
static void ring_read(struct inotify* in, unsigned offset, void* data, unsigned bytes) {
    uint8_t* target = data;
    for (unsigned i = 0; i < bytes; i++) target[i] = in->queue[(in->head + offset + i) % QUEUE_BYTES];
}
static void notify(struct inotify* in) { task_wake_channel(in); event_source_changed(in, 1); }
static void queue_event(struct inotify* in, int wd, uint32_t mask, uint32_t cookie, const char* name) {
    unsigned name_size = name ? strlen(name) : 0;
    unsigned length = name_size ? (name_size + 1 + 15) & ~15U : 0, size = 16 + length;
    uint32_t header[4] = {(uint32_t)wd, mask, cookie, length};
    /* Linux merges an event identical to the newest one still queued. */
    if (in->used && in->last_size == size && !cookie) {
        uint32_t last[4]; ring_read(in, in->last, last, 16);
        if (!memcmp(last, header, 16)) {
            bool same = true; char c;
            for (unsigned i = 0; i < name_size && same; i++) { ring_read(in, in->last + 16 + i, &c, 1); same = c == name[i]; }
            if (same) return;
        }
    }
    if (in->used + size > QUEUE_BYTES - 16) {
        if (in->overflowed) return;
        in->overflowed = true; wd = -1; mask = IN_Q_OVERFLOW; cookie = 0; length = 0; size = 16; name_size = 0;
        header[0] = (uint32_t)-1; header[1] = mask; header[2] = 0; header[3] = 0;
    }
    ring_write(in, in->used, header, 16);
    if (length) {
        ring_write(in, in->used + 16, name, name_size);
        uint8_t zero[16] = {0}; ring_write(in, in->used + 16 + name_size, zero, length - name_size);
    }
    in->last = in->used; in->last_size = size; in->used += size; notify(in);
}
static void watch_remove(struct watch* w, bool ignored) {
    struct inotify* in = w->owner;
    if (ignored) queue_event(in, w->wd, IN_IGNORED, 0, NULL);
    struct watch** link = &in->watches; while (*link && *link != w) link = &(*link)->next;
    if (*link) *link = w->next;
    link = &buckets[bucket(w->device, w->inode)]; while (*link && *link != w) link = &(*link)->hash_next;
    if (*link) *link = w->hash_next;
    watch_free(w);
    if (!--inotify_watches) { page_free((uint64_t)buckets); buckets = NULL; }
}
static void deliver(uint64_t inode, uint32_t mask, uint32_t cookie, const char* name, bool unlinked) {
    for (struct watch *w = buckets[bucket(1, inode)], *next; w; w = next) {
        next = w->hash_next;
        if (w->device != 1 || w->inode != inode) continue;
        uint32_t events = mask & w->mask & ALL_EVENTS;
        if (!events || (name && unlinked && (w->mask & IN_EXCL_UNLINK))) continue;
        queue_event(w->owner, w->wd, events | (mask & IN_ISDIR), cookie, name);
        if (w->mask & IN_ONESHOT) { watch_remove(w, true); continue; }
        if (!name && (mask & IN_DELETE_SELF)) watch_remove(w, true);
    }
}
/* Report `mask` for the object at canonical `path` (file: its directory entry, or NULL to look it up). */
void inotify_event(const char* path, const fat32_file_t* file, uint32_t mask, uint32_t cookie) {
    if (!inotify_watches || !buckets) return;
    fat32_file_t found;
    if (!file) { if (!fat32_lookup(path, &found)) return; file = &found; }
    uint64_t inode = disk_inode_id(file); uint32_t directory = file->is_dir ? IN_ISDIR : 0; bool unlinked = file->detached;
    if ((mask & CHILD_EVENTS) && path[0] == '/' && path[1]) {
        char parent[FAT32_PATH_MAX]; unsigned size = strlen(path), leaf = size;
        if (size >= sizeof(parent)) return;
        while (leaf && path[leaf - 1] != '/') leaf--;
        memcpy(parent, path, leaf > 1 ? leaf - 1 : 1); parent[leaf > 1 ? leaf - 1 : 1] = 0;
        fat32_file_t directory_entry;
        if (path[leaf] && fat32_lookup(parent, &directory_entry))
            deliver(disk_inode_id(&directory_entry), (mask & CHILD_EVENTS) | directory, cookie, path + leaf, unlinked);
    }
    if (mask & SELF_EVENTS) deliver(inode, (mask & SELF_EVENTS) | directory, cookie, NULL, unlinked);
}
/* ---- descriptor operations ---- */
static int64_t inotify_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct inotify* in = object;
    if (write) return -22;
    for (;;) {
        if (in->used) {
            unsigned bytes = 0;
            while (bytes < in->used) {
                uint32_t header[4]; ring_read(in, bytes, header, 16);
                if (bytes + 16 + header[3] > total) break;
                bytes += 16 + header[3];
            }
            if (!bytes) return -22; /* the next event does not fit */
            if (!iov_access(vectors, count, 0, bytes, true)) return -14;
            unsigned first = QUEUE_BYTES - in->head; if (first > bytes) first = bytes;
            iov_copy(vectors, count, 0, in->queue + in->head, first, false);
            if (bytes > first) iov_copy(vectors, count, first, in->queue, bytes - first, false);
            in->head = (in->head + bytes) % QUEUE_BYTES; in->used -= bytes;
            if (in->last >= bytes) in->last -= bytes; else in->last_size = 0;
            if (!in->used) { in->head = 0; in->overflowed = false; in->last_size = 0; }
            return bytes;
        }
        if (flags & 0x800) return -11;
        int error = task_wait_channel(in); if (error) return error;
    }
}
static int inotify_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct inotify* in = object;
    if ((uint32_t)command != 0x541B) return -25;
    if (!accessible(argument, 4, true)) return -14;
    return uput32(argument, (uint32_t)in->used);
}
static uint32_t inotify_ready(void* object) { return ((struct inotify*)object)->used ? 1U : 0; }
static void* inotify_source(void* object) { return object; }
static void inotify_release(void* object) {
    struct inotify* in = object;
    while (in->watches) watch_remove(in->watches, false);
    arena_free((uint64_t)in->queue, QUEUE_BYTES); page_free((uint64_t)in); instances--;
}
static void inotify_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 6; stat->ino = ((struct inotify*)object)->id; stat->nlink = 1; stat->mode = 0600;
    stat->uid = credentials_visible_id(credentials_kernel_id(credentials_current(), false, 1), false);
    stat->gid = credentials_visible_id(credentials_kernel_id(credentials_current(), true, 1), true); stat->block_size = 4096;
}
/* /proc/PID/fdinfo (Linux inotify_fdinfo): one line per watch, newest first, with name_to_handle_at's handle (fsext.c) */
static unsigned inotify_fdinfo(void* object, char* out, unsigned capacity) {
    unsigned used = 0;
    for (struct watch* w = ((struct inotify*)object)->watches; w; w = w->next) {
        used = kfmt(out, capacity, used, "inotify wd:%x ino:%lx sdev:%x mask:%x ignored_mask:0 ", w->wd, w->inode, fdinfo_sdev(w->device), w->mask);
        if (w->device != 7 && w->device != 8) {   /* procfs and nsfs cannot be exported: no handle */
            uint8_t handle[12]; uint32_t device = (uint32_t)w->device; memcpy(handle, &w->inode, 8); memcpy(handle + 8, &device, 4);
            used = kfmt(out, capacity, used, "fhandle-bytes:c fhandle-type:%x f_handle:", w->device == 1 ? 0x71U : 0x1U);
            for (unsigned i = 0; i < sizeof(handle); i++) used = kfmt(out, capacity, used, "%02x", (unsigned)handle[i]);
        }
        used = kfmt(out, capacity, used, "\n");
    }
    return used;
}
static const struct object_operations inotify_operations = {inotify_io, inotify_ioctl, inotify_ready, inotify_source, inotify_release, inotify_stat};
unsigned inotify_fdinfo_text(struct open_file* description, char* out, unsigned capacity) {   /* kernel.c fd_information_text */
    void* object = fd_object(description, &inotify_operations); return object ? inotify_fdinfo(object, out, capacity) : 0;
}
static int create(uint64_t flags) {
    if (flags & ~(0x800ULL | 0x80000ULL)) return -22;
    if (instances >= MAX_INSTANCES) return -24;
    struct inotify* in = (void*)page_alloc(); if (!in) return -12;
    in->queue = (void*)arena_alloc(QUEUE_BYTES);
    if (!in->queue) { page_free((uint64_t)in); return -12; }
    in->id = ++next_id; instances++;
    int fd = fd_create_object(&inotify_operations, in, flags | 0, "anon_inode:inotify");
    if (fd < 0) { arena_free((uint64_t)in->queue, QUEUE_BYTES); page_free((uint64_t)in); instances--; }
    return fd;
}
static int add_watch(struct inotify* in, uint64_t address, uint32_t mask) {
    if (!(mask & ALL_EVENTS)) return -22;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)) return -22;
    char path[FAT32_PATH_MAX]; struct open_file* bound = NULL; struct linux_stat stat;
    int error = native_path_resolve((uint64_t)-100, address, !(mask & IN_DONT_FOLLOW), path, &bound);
    if (!error) error = native_path_stat(path, bound, !(mask & IN_DONT_FOLLOW), &stat);
    if (error) return error;
    if ((mask & IN_ONLYDIR) && (stat.mode & 0170000) != 0040000) return -20;
    uint32_t stored = mask & (ALL_EVENTS | IN_EXCL_UNLINK | IN_ONESHOT);
    for (struct watch* w = in->watches; w; w = w->next) if (w->device == stat.dev && w->inode == stat.ino) {
        if (mask & IN_MASK_CREATE) return -17;
        w->mask = (mask & IN_MASK_ADD) ? w->mask | stored : stored; return w->wd;
    }
    if (inotify_watches >= MAX_WATCHES) return -28;
    if (!buckets) { buckets = (void*)page_alloc(); if (!buckets) return -12; }
    struct watch* w = watch_alloc();
    if (!w) { if (!inotify_watches) { page_free((uint64_t)buckets); buckets = NULL; } return -12; }
    if (in->next_wd == 0x7FFFFFFF) in->next_wd = 0;
    *w = (struct watch){in->watches, NULL, in, stat.dev, stat.ino, stored, ++in->next_wd};
    in->watches = w; unsigned b = bucket(w->device, w->inode); w->hash_next = buckets[b]; buckets[b] = w;
    inotify_watches++; return w->wd;
}
int64_t inotify_call(struct registers* r) {
    if (r->rax == 253) return create(0);
    if (r->rax == 294) return create(r->rdi);
    struct open_file* description = fd_lookup(r->rdi); if (!description) return -9;
    struct inotify* in = fd_object(description, &inotify_operations); if (!in) return -22;
    if (r->rax == 254) { fd_pin(description); int result = add_watch(in, r->rsi, (uint32_t)r->rdx); fd_unpin(description); return result; }
    for (struct watch* w = in->watches; w; w = w->next) if (w->wd == (int)r->rsi) { watch_remove(w, true); return 0; }
    return -22;
}
