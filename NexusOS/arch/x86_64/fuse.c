/* FUSE: /dev/fuse and the "fuse", "fuse.<subtype>" and "fuseblk" filesystem types (Linux fs/fuse, kernel protocol
 * 7.31), so libfuse 3 daemons (sshfs, fuse2fs, ntfs-3g's fuse-lite, ...) run unchanged.
 *
 * Connection: each open of /dev/fuse is one (FUSE_DEV_IOC_CLONE attaches more descriptors to it). mount(2) with type
 * fuse/fuseblk and "fd=N,rootmode=M,user_id=U,group_id=G[,default_permissions][,allow_other][,max_read=][,blksize=]"
 * binds it to the new mount and queues FUSE_INIT without waiting (the daemon answers it from its loop); requests made
 * before the answer wait for it. Requests: the caller's request is queued, the daemon reads it (blocking unless
 * O_NONBLOCK, poll/epoll readable when one is queued), writes the reply, and the caller (asleep without the kernel
 * lock) gets the reply's payload or error. Notifications from the daemon (unique 0) are accepted and ignored.
 * Abort: when the last /dev/fuse descriptor of a connection closes (the daemon exited or was killed) or on
 * umount -f, every queued and pending request fails with ENOTCONN, as does every later operation on the mount;
 * the daemon's read() returns ENODEV after the mount is gone. umount: open file handles are released, fuseblk
 * daemons get FUSE_DESTROY (ntfs-3g writes its volume back then), then the connection is aborted.
 * Mapping onto the path-based mount layer: a node table per connection (nodeid, parent, name, attributes with their
 * validity), LOOKUP per component when an entry has expired; open handles: one read and one write FUSE file handle
 * per node opened on first use and released at unlink-release or umount; directories: OPENDIR once per node and
 * READDIR with the daemon's offsets as cookies; symbolic links: READLINK through read. Permissions: mount.c applies
 * mode bits only with default_permissions and lets only the mounting user (user_id) in unless allow_other.
 * Unprivileged mount(2) of FUSE is EPERM, as on Linux: users mount through the set-user-ID fusermount3. */
#include "mount.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define FUSE_MINOR 31
enum { LOOKUP = 1, FORGET = 2, GETATTR = 3, SETATTR = 4, READLINK = 5, SYMLINK = 6, MKNOD = 8, MKDIR = 9, UNLINK = 10, RMDIR = 11,
       RENAME = 12, LINK = 13, OPEN = 14, READ = 15, WRITE = 16, STATFS = 17, RELEASE = 18, FSYNC = 20, SETXATTR = 21, GETXATTR = 22,
       LISTXATTR = 23, REMOVEXATTR = 24, FLUSH = 25, INIT = 26, OPENDIR = 27, READDIR = 28, RELEASEDIR = 29, FSYNCDIR = 30,
       ACCESS = 34, CREATE = 35, DESTROY = 38, BATCH_FORGET = 42, FALLOCATE = 43, RENAME2 = 45 };
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define IN_HEADER 40u
#define OUT_HEADER 16u
#define MIN_READ_BUFFER 8192u
#define NODE_CHUNK 256u
struct open_file* kernel_file(uint64_t fd);
int kernel_file_device(struct open_file* f, struct input_reader** reader);   /* kernel.c: storage number of a device description */

struct fuse_attr { uint64_t ino, size, blocks, atime, mtime, ctime; uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags; };
struct fuse_entry_out { uint64_t nodeid, generation, entry_valid, attr_valid; uint32_t entry_valid_nsec, attr_valid_nsec; struct fuse_attr attr; };
struct fuse_attr_out { uint64_t attr_valid; uint32_t attr_valid_nsec, dummy; struct fuse_attr attr; };
_Static_assert(sizeof(struct fuse_attr) == 88, "fuse_attr ABI");
_Static_assert(sizeof(struct fuse_entry_out) == 128, "fuse_entry_out ABI");
_Static_assert(sizeof(struct fuse_attr_out) == 104, "fuse_attr_out ABI");

struct request {
    struct request* next;
    uint64_t unique; uint32_t opcode;
    uint8_t* in; uint32_t in_length, in_capacity;
    uint8_t* out; uint32_t out_capacity, out_length;
    int error; bool sent, done, noreply, abandoned;
};
struct node {
    uint64_t nodeid, generation, nlookup;
    uint32_t parent; char* name;               /* parent index (root: itself), name (root: "") */
    struct fuse_attr attr; uint64_t attr_expire, entry_expire;
    uint64_t rfh, wfh, dfh; bool rfh_open, wfh_open, dfh_open, gone;
};
struct conn {
    struct conn* next; unsigned id, descriptors; bool mounted, initialized, aborted, block;
    struct mount* m;
    struct request *queue, *pending;            /* to send, sent */
    unsigned active;                            /* mount operations in progress: they may sleep in a request */
    uint64_t unique;
    uint32_t minor, max_write, max_read, init_flags;
    uint32_t owner_uid, owner_gid, rootmode;
    struct node** chunks; unsigned chunk_count, node_count;
};
static struct conn* conns;
static unsigned next_conn_id = 1;
static unsigned fuse_device;   /* storage number of /dev/fuse */

/* ---- connections and requests ---- */
static struct node* node_at(struct conn* c, uint32_t index) { return index < c->node_count ? &c->chunks[index / NODE_CHUNK][index % NODE_CHUNK] : NULL; }
static void free_request(struct request* r) {
    if (r->in) arena_free((uint64_t)r->in, (r->in_capacity + 4095) / 4096 * 4096);
    if (r->out) arena_free((uint64_t)r->out, (r->out_capacity + 4095) / 4096 * 4096);
    arena_free((uint64_t)r, sizeof(*r));
}
static void finish(struct conn* c, struct request* r, int error) {
    r->error = error; r->done = true;
    if (r->abandoned) { free_request(r); return; }
    task_wake_channel(r); (void)c;
}
static void abort_conn(struct conn* c) {
    if (c->aborted) return;
    c->aborted = true;
    while (c->queue) { struct request* r = c->queue; c->queue = r->next; if (r->noreply) free_request(r); else finish(c, r, -107); }
    while (c->pending) { struct request* r = c->pending; c->pending = r->next; finish(c, r, -107); }
    if (c->m) c->m->dead = true;
    task_wake_channel(c); event_source_changed(c, 1 | 0x10);   /* readers see ENODEV, pollers POLLERR */
    vga_print("[64] FUSE connection aborted\n");
}
static void release_conn(struct conn* c) {
    if (c->descriptors || c->mounted || c->active) return;
    for (struct conn** link = &conns; *link; link = &(*link)->next) if (*link == c) { *link = c->next; break; }
    for (unsigned i = 0; i < c->node_count; i++) { struct node* n = node_at(c, i); if (n->name) arena_free((uint64_t)n->name, 256); }
    for (unsigned i = 0; i < c->chunk_count; i++) arena_free((uint64_t)c->chunks[i], NODE_CHUNK * sizeof(struct node));
    if (c->chunks) arena_free((uint64_t)c->chunks, 4096);
    arena_free((uint64_t)c, sizeof(*c));
}
static struct request* new_request(struct conn* c, uint32_t opcode, uint64_t nodeid, uint32_t args, uint32_t out) {
    struct request* r = (void*)arena_alloc(sizeof(*r)); if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->in_capacity = IN_HEADER + args; r->in = (void*)arena_alloc((r->in_capacity + 4095) / 4096 * 4096);
    r->out_capacity = out; r->out = out ? (void*)arena_alloc((out + 4095) / 4096 * 4096) : NULL;
    if (!r->in || (out && !r->out)) { free_request(r); return NULL; }
    memset(r->in, 0, r->in_capacity);
    r->opcode = opcode; r->unique = c->unique += 2; r->in_length = IN_HEADER + args;
    struct credentials* who = credentials_current();
    put_le32(r->in + 0, r->in_length); put_le32(r->in + 4, opcode); put_le64(r->in + 8, r->unique); put_le64(r->in + 16, nodeid);
    put_le32(r->in + 24, credentials_visible_id(credentials_kernel_id(who, false, 3), false));
    put_le32(r->in + 28, credentials_visible_id(credentials_kernel_id(who, true, 3), true));
    put_le32(r->in + 32, task_visible_pid(task_pid()));
    return r;
}
bool boot_option(const char* word);
static int tracing = -1;   /* kernel option fuse.trace: one line per request and reply */
static bool trace(void) { if (tracing < 0) tracing = boot_option("fuse.trace"); return tracing == 1; }
static void trace_line(const char* what, uint64_t a, uint64_t b, int64_t c) {
    char t[96] = "[64] FUSE "; strcat(t, what); strcat(t, " "); append_u64(t, a); strcat(t, " "); append_u64(t, b);
    strcat(t, c < 0 ? " -" : " "); append_u64(t, (uint64_t)(c < 0 ? -c : c)); strcat(t, "\n"); vga_print(t);
}
static void enqueue(struct conn* c, struct request* r) {
    if (trace()) trace_line("request op/unique/node", r->opcode, r->unique, (int64_t)get_le64(r->in + 16));
    r->next = NULL; struct request** link = &c->queue; while (*link) link = &(*link)->next; *link = r;
    task_wake_channel(c); event_source_changed(c, 1);
}
/* Queue r and sleep until it is answered; 0 or -errno. r stays the caller's (done_with): r->done unless a signal
 * interrupted the wait after the daemon had read r, which then stays pending and is freed by its reply or the abort.
 * A signal (or SIGKILL) ends the wait with EINTR; a request the daemon has not read yet is withdrawn (Linux does the
 * same; it would also send FUSE_INTERRUPT for one already read). */
static int call(struct conn* c, struct request* r) {
    while (!c->initialized && !c->aborted && r->opcode != INIT)
        if (task_wait_channel(c) && !c->initialized && !c->aborted) { r->done = true; return -4; }
    if (c->aborted) { r->done = true; return -107; }
    enqueue(c, r);
    while (!r->done) {
        if (!task_wait_channel(r) || r->done) continue;
        for (struct request** link = &c->queue; *link; link = &(*link)->next)
            if (*link == r) { *link = r->next; if (!c->queue) event_source_changed(c, 0); r->done = true; break; }
        return -4;
    }
    return r->error;
}
static void send_forget(struct conn* c, uint64_t nodeid, uint64_t nlookup) {
    if (c->aborted || !nlookup) return;
    struct request* r = new_request(c, FORGET, nodeid, 8, 0); if (!r) return;
    put_le64(r->in + IN_HEADER, nlookup); r->noreply = true; enqueue(c, r);
}

/* ---- /dev/fuse ---- */
static struct conn* conn_of(const struct input_reader* reader) { return reader ? (struct conn*)(uintptr_t)reader->sequence : NULL; }
static int dev_open(uint64_t flags, struct input_reader* reader) {
    (void)flags;
    struct conn* c = (void*)arena_alloc(sizeof(*c)); if (!c) return -12;
    memset(c, 0, sizeof(*c)); c->id = next_conn_id++; c->descriptors = 1; c->unique = 0; c->max_write = 4096;
    c->next = conns; conns = c;
    reader->sequence = (uint64_t)(uintptr_t)c;
    return 0;
}
static void dev_close(struct input_reader* reader) {
    struct conn* c = conn_of(reader); if (!c) return;
    reader->sequence = 0;
    if (c->descriptors) c->descriptors--;
    if (!c->descriptors) abort_conn(c);   /* the daemon is gone */
    release_conn(c);
}
static int64_t dev_message(struct input_reader* reader, const uint8_t* data, uint64_t count);
static int64_t dev_io(struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags) {
    struct conn* c = conn_of(reader); if (!c) return -9;
    if (!write) {
        if (count < MIN_READ_BUFFER) return -22;
        for (;;) {
            if (c->aborted) return -19;
            if (c->queue) break;
            if (!c->mounted && c->initialized) return -19;   /* unmounted: the daemon's loop ends */
            if (flags & 0x800) return -11;
            int e = task_wait_channel(c); if (e) return e;
        }
        struct request* r = c->queue;
        if (r->in_length > count) return -22;
        if (!accessible(buffer, r->in_length, true)) return -14;
        if (copy_to_user((void*)buffer, r->in, r->in_length)) return -14;
        c->queue = r->next;
        if (!c->queue) event_source_changed(c, 0);
        uint32_t length = r->in_length;
        if (r->noreply) free_request(r);
        else { r->sent = true; r->next = c->pending; c->pending = r; }
        return length;
    }
    if (count < OUT_HEADER || count > (2u << 20)) return -22;
    if (!accessible(buffer, count, false)) return -14;
    uint64_t size = (count + 4095) / 4096 * 4096;
    uint8_t* copy = (void*)arena_alloc(size); if (!copy) return -12;
    if (copy_from_user(copy, (const void*)buffer, count)) { arena_free((uint64_t)copy, size); return -14; }
    int64_t result = dev_message(reader, copy, count);
    arena_free((uint64_t)copy, size);
    return result;
}
/* One reply or notification from the daemon, in kernel memory. */
static int64_t dev_message(struct input_reader* reader, const uint8_t* data, uint64_t count) {
    struct conn* c = conn_of(reader); if (!c) return -9;
    if (count < OUT_HEADER) return -22;
    const uint8_t* header = data; const uint64_t buffer = (uint64_t)(uintptr_t)data;
    uint32_t length = get_le32(header); int32_t error = (int32_t)get_le32(header + 4); uint64_t unique = get_le64(header + 8);
    if (length != count) return -22;
    if (!unique) return (int64_t)count;   /* a notification: accepted (caches here only expire) */
    if (error > 0 || error < -4095) return -22;
    if (trace()) trace_line("reply unique/length/error", unique, length, error);
    struct request** link = &c->pending;
    while (*link && (*link)->unique != unique) link = &(*link)->next;
    if (!*link) return -2;
    struct request* r = *link; *link = r->next;
    uint32_t payload = length - OUT_HEADER;
    if (!error) {
        if (payload > r->out_capacity) payload = r->out_capacity;
        if (payload) memcpy(r->out, (uint8_t*)buffer + OUT_HEADER, payload);
        r->out_length = payload;
    }
    if (r->opcode == INIT) {   /* nobody waits for INIT (abandoned from the start) */
        if (error || payload < 24 || get_le32(r->out) != 7) { vga_print("[64] FUSE INIT refused by the daemon\n"); finish(c, r, error ? error : -71); abort_conn(c); return (int64_t)count; }
        c->minor = get_le32(r->out + 4) < FUSE_MINOR ? get_le32(r->out + 4) : FUSE_MINOR;
        c->init_flags = get_le32(r->out + 12);
        uint32_t max_write = get_le32(r->out + 20);
        c->max_write = (c->init_flags & 32) && max_write >= 4096 ? (max_write > (1u << 20) ? 1u << 20 : max_write) : 4096;
        c->initialized = true; task_wake_channel(c);
        char n[16] = ""; append_u64(n, c->minor); vga_print("[64] FUSE connection initialized: protocol 7."); vga_print(n);
        n[0] = 0; append_u64(n, c->max_write); vga_print(" max_write="); vga_print(n); vga_print("\n");
        free_request(r); return (int64_t)count;
    }
    finish(c, r, error);
    return (int64_t)count;
}
static int dev_ioctl(struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags;
    if (command != 0x8004E500ULL) return -25;   /* FUSE_DEV_IOC_CLONE: this descriptor joins another's connection */
    if (!accessible(argument, 4, false)) return -14;
    uint32_t old_fd = uget32(argument);
    struct open_file* f = kernel_file(old_fd); struct input_reader* other = NULL;
    if (!f || kernel_file_device(f, &other) != (int)fuse_device || !conn_of(other)) return -22;
    struct conn* mine = conn_of(reader), *target = conn_of(other);
    if (mine == target) return 0;
    if (mine && (mine->mounted || mine->queue || mine->pending)) return -22;
    if (mine) { mine->descriptors--; release_conn(mine); }
    target->descriptors++; reader->sequence = (uint64_t)(uintptr_t)target;
    return 0;
}
static uint32_t dev_ready(const struct input_reader* reader) {
    struct conn* c = conn_of(reader); if (!c) return 0x10;
    if (c->aborted || (!c->mounted && c->initialized)) return 0x10 | 1;
    return (c->queue ? 1 : 0) | 4;
}
static void* dev_source(const struct input_reader* reader) { return conn_of(reader); }
static const struct storage_char_ops dev_ops = {.open = dev_open, .close = dev_close, .io = dev_io, .ioctl = dev_ioctl, .ready = dev_ready, .source = dev_source,
                                                  .write_message = dev_message};
void fuse_init(void) { storage_fuse_ops = &dev_ops; int n = storage_lookup("/dev/fuse"); fuse_device = n >= 0 ? (unsigned)n : 0; }

/* ---- nodes ---- */
static int new_node(struct conn* c, uint64_t nodeid, uint32_t parent, const char* name, uint32_t* out) {
    for (unsigned i = 0; i < c->node_count; i++) {   /* a node the daemon already gave us under another entry (hard link) */
        struct node* n = node_at(c, i);
        if (n->nodeid == nodeid && !n->gone && n->parent == parent && n->name && !strcmp(n->name, name)) { *out = i; return 0; }
    }
    if (c->node_count == c->chunk_count * NODE_CHUNK) {
        if (c->chunk_count == 4096 / sizeof(struct node*)) return -12;
        if (!c->chunks) { c->chunks = (void*)arena_alloc(4096); if (!c->chunks) return -12; memset(c->chunks, 0, 4096); }
        c->chunks[c->chunk_count] = (void*)arena_alloc(NODE_CHUNK * sizeof(struct node));
        if (!c->chunks[c->chunk_count]) return -12;
        memset(c->chunks[c->chunk_count], 0, NODE_CHUNK * sizeof(struct node)); c->chunk_count++;
    }
    struct node* n = &c->chunks[c->node_count / NODE_CHUNK][c->node_count % NODE_CHUNK];   /* not yet counted: node_at() would refuse it */
    memset(n, 0, sizeof(*n)); n->nodeid = nodeid; n->parent = parent;
    n->name = (void*)arena_alloc(256); if (!n->name) return -12;
    strncpy(n->name, name, 255); n->name[255] = 0;
    *out = c->node_count++; return 0;
}
static uint64_t expiry(uint64_t seconds, uint32_t nsec) {
    if (seconds > 1000000) seconds = 1000000;
    return native_clock_ns() + seconds * 1000000000ULL + nsec;
}
static void fill(struct conn* c, uint32_t index, fat32_file_t* f) {
    struct node* n = node_at(c, index); const struct fuse_attr* a = &n->attr;
    memset(f, 0, sizeof(*f));
    f->first = index; f->dir_lba = index ? index + 1 : MOUNT_ROOT_LBA;
    f->is_dir = (a->mode & S_IFMT) == S_IFDIR; f->size = f->is_dir ? 0 : a->size;
    f->mode = a->mode; f->nlink = a->nlink; f->rdev = a->rdev; f->blocks = a->blocks;
    uint32_t k; f->uid = credentials_kernel_from_visible(a->uid, false, &k) ? k : a->uid; f->gid = credentials_kernel_from_visible(a->gid, true, &k) ? k : a->gid;
    f->ino = (uint32_t)a->ino; f->generation = (uint32_t)(a->ino >> 32);
    f->atime = (int64_t)a->atime * 1000000000LL + a->atimensec; f->mtime = (int64_t)a->mtime * 1000000000LL + a->mtimensec;
    f->ctime = (int64_t)a->ctime * 1000000000LL + a->ctimensec; f->btime = f->ctime;
    if (n->name) { strncpy(f->name, n->name, FAT32_LFN_MAX); f->name[FAT32_LFN_MAX] = 0; }
    f->detached = n->gone;
}
static void done_with(struct request* r, int e);
static int getattr(struct conn* c, uint32_t index, bool force) {
    struct node* n = node_at(c, index); if (!n) return -5;
    if (!force && n->attr_expire > native_clock_ns()) return 0;
    struct request* r = new_request(c, GETATTR, n->nodeid, 16, sizeof(struct fuse_attr_out)); if (!r) return -12;
    if (n->rfh_open) { put_le32(r->in + IN_HEADER, 1); put_le64(r->in + IN_HEADER + 8, n->rfh); }   /* FUSE_GETATTR_FH */
    int e = call(c, r);
    if (!e && r->out_length < sizeof(struct fuse_attr_out)) e = -5;
    if (!e) { struct fuse_attr_out* o = (void*)r->out; n = node_at(c, index); n->attr = o->attr; n->attr_expire = expiry(o->attr_valid, o->attr_valid_nsec); }
    done_with(r, e);
    return e;
}
static int entry_reply(struct conn* c, uint32_t parent, const char* name, struct request* r, uint32_t* out) {
    if (r->out_length < sizeof(struct fuse_entry_out)) return -5;
    struct fuse_entry_out* o = (void*)r->out;
    if (!o->nodeid) return -2;   /* a negative entry */
    if (o->nodeid == 1 && parent) return -5;
    uint32_t index; int e = new_node(c, o->nodeid, parent, name, &index);
    if (e) { send_forget(c, o->nodeid, 1); return e; }
    struct node* n = node_at(c, index);
    n->nlookup++; n->generation = o->generation; n->attr = o->attr;
    n->attr_expire = expiry(o->attr_valid, o->attr_valid_nsec); n->entry_expire = expiry(o->entry_valid, o->entry_valid_nsec);
    *out = index; return 0;
}
static void done_with(struct request* r, int e) { (void)e; if (r->done) free_request(r); else r->abandoned = true; }
static int lookup_child(struct conn* c, uint32_t parent, const char* name, uint32_t* out) {
    uint64_t now = native_clock_ns();
    for (unsigned i = 1; i < c->node_count; i++) {
        struct node* n = node_at(c, i);
        if (!n->gone && n->parent == parent && n->name && !strcmp(n->name, name) && n->entry_expire > now) { *out = i; return 0; }
    }
    unsigned length = strlen(name) + 1;
    struct request* r = new_request(c, LOOKUP, node_at(c, parent)->nodeid, length, sizeof(struct fuse_entry_out)); if (!r) return -12;
    memcpy(r->in + IN_HEADER, name, length);
    int e = call(c, r);
    if (!e) {
        for (unsigned i = 1; i < c->node_count; i++) {   /* an expired entry for the same name goes stale */
            struct node* n = node_at(c, i);
            if (!n->gone && n->parent == parent && n->name && !strcmp(n->name, name) && n->nodeid != ((struct fuse_entry_out*)r->out)->nodeid) n->gone = true;
        }
        e = entry_reply(c, parent, name, r, out);
    }
    done_with(r, e);
    return e;
}
static struct conn* hold(struct mount* m) { struct conn* c = m->sb; if (c) c->active++; return c; }
static void put(struct conn* c) { if (!--c->active) release_conn(c); }
static int walk(struct conn* c, const char* path, uint32_t* parent_out, const char** leaf, bool want_parent) {
    uint32_t at = 0; char part[256];
    for (const char* p = path;;) {
        while (*p == '/') p++;
        const char* end = p; while (*end && *end != '/') end++;
        const char* after = end; while (*after == '/') after++;
        if (!*p) { *parent_out = at; *leaf = p; return 0; }
        if (want_parent && !*after) { *parent_out = at; *leaf = p; return 0; }
        unsigned n = (unsigned)(end - p); if (n >= sizeof(part)) return -36;
        memcpy(part, p, n); part[n] = 0;
        uint32_t child; int e = lookup_child(c, at, part, &child); if (e) return e;
        if ((node_at(c, child)->attr.mode & S_IFMT) != S_IFDIR && *after) return -20;
        at = child; p = after;
    }
}
static void leaf_copy(const char* leaf, char* name) { unsigned n = 0; while (leaf[n] && leaf[n] != '/' && n < 255) { name[n] = leaf[n]; n++; } name[n] = 0; }

/* ---- mount ops ---- */
static int do_lookup(struct conn* c, struct mount* m, const char* path, fat32_file_t* out) {
    (void)m; if (c->aborted) return -107;
    uint32_t index; const char* leaf; int e = walk(c, path, &index, &leaf, false); if (e) return e;
    if ((e = getattr(c, index, false))) return e;
    fill(c, index, out); return 0;
}
static int do_refresh(struct conn* c, struct mount* m, fat32_file_t* f) {
    (void)m; if (c->aborted) return -107;
    struct node* n = node_at(c, f->first); if (!n) return -5;
    char name[FAT32_LFN_MAX + 1]; strcpy(name, f->name);
    int e = getattr(c, f->first, false); if (e) return e;
    uint32_t cursor = f->cursor; fill(c, f->first, f); f->cursor = cursor; strcpy(f->name, name);
    return 0;
}
static int open_handle(struct conn* c, uint32_t index, bool write, uint64_t* fh) {
    struct node* n = node_at(c, index); if (!n) return -5;
    if (write ? n->wfh_open : n->rfh_open) { *fh = write ? n->wfh : n->rfh; return 0; }
    struct request* r = new_request(c, OPEN, n->nodeid, 8, 16); if (!r) return -12;
    put_le32(r->in + IN_HEADER, write ? 2 : 0);   /* O_RDWR for writes: page-cache writes may need reads around them */
    int e = call(c, r);
    if (!e && r->out_length < 16) e = -5;
    if (!e) { n = node_at(c, index); if (write) { n->wfh = get_le64(r->out); n->wfh_open = true; } else { n->rfh = get_le64(r->out); n->rfh_open = true; } *fh = get_le64(r->out); }
    done_with(r, e); return e;
}
static void release_handles(struct conn* c, uint32_t index) {
    struct node* n = node_at(c, index); if (!n || c->aborted) return;
    for (unsigned which = 0; which < 3; which++) {
        bool open = which == 0 ? n->rfh_open : which == 1 ? n->wfh_open : n->dfh_open;
        if (!open) continue;
        uint64_t fh = which == 0 ? n->rfh : which == 1 ? n->wfh : n->dfh;
        if (which == 0) n->rfh_open = false; else if (which == 1) n->wfh_open = false; else n->dfh_open = false;
        struct request* r = new_request(c, which == 2 ? RELEASEDIR : RELEASE, n->nodeid, 24, 0); if (!r) continue;
        put_le64(r->in + IN_HEADER, fh); put_le32(r->in + IN_HEADER + 8, which == 1 ? 2 : 0);
        if (which == 1) {   /* a FLUSH first, as close(2) would */
            struct request* f = new_request(c, FLUSH, n->nodeid, 24, 0);
            if (f) { put_le64(f->in + IN_HEADER, fh); int fe = call(c, f); done_with(f, fe); }
        }
        int e = call(c, r); done_with(r, e);
        n = node_at(c, index);
    }
}
static int64_t do_read(struct conn* c, struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, void* buffer) {
    (void)m; if (c->aborted) return -107;
    struct node* n = node_at(c, f->first); if (!n) return -5;
    if ((n->attr.mode & S_IFMT) == S_IFLNK) {   /* the link target */
        struct request* r = new_request(c, READLINK, n->nodeid, 0, 4096); if (!r) return -12;
        int e = call(c, r); int64_t result = e;
        if (!e) { uint64_t size = r->out_length; result = 0; if (offset < size) { result = (int64_t)(length < size - offset ? length : size - offset); memcpy(buffer, r->out + offset, (size_t)result); } }
        done_with(r, e); return result;
    }
    if (f->is_dir) return -21;
    uint64_t fh; int e = open_handle(c, f->first, false, &fh); if (e) return e;
    uint64_t done = 0;
    while (done < length) {
        uint32_t chunk = length - done > c->max_write ? c->max_write : (uint32_t)(length - done);
        struct request* r = new_request(c, READ, node_at(c, f->first)->nodeid, 40, chunk); if (!r) return -12;
        put_le64(r->in + IN_HEADER, fh); put_le64(r->in + IN_HEADER + 8, offset + done); put_le32(r->in + IN_HEADER + 16, chunk);
        e = call(c, r);
        uint32_t got = e ? 0 : r->out_length;
        if (got) memcpy((uint8_t*)buffer + done, r->out, got);
        done_with(r, e);
        if (e) return done ? (int64_t)done : e;
        done += got;
        if (got < chunk) break;   /* end of file */
    }
    return (int64_t)done;
}
static int64_t do_write(struct conn* c, struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, const void* buffer) {
    (void)m; if (c->aborted) return -107;
    if (f->is_dir) return -21;
    if (f->detached) return (int64_t)length;
    uint64_t fh; int e = open_handle(c, f->first, true, &fh); if (e) return e;
    uint64_t done = 0;
    while (done < length) {
        uint32_t chunk = length - done > c->max_write ? c->max_write : (uint32_t)(length - done);
        struct request* r = new_request(c, WRITE, node_at(c, f->first)->nodeid, 40 + chunk, 8); if (!r) return -12;
        put_le64(r->in + IN_HEADER, fh); put_le64(r->in + IN_HEADER + 8, offset + done); put_le32(r->in + IN_HEADER + 16, chunk);
        memcpy(r->in + IN_HEADER + 40, (const uint8_t*)buffer + done, chunk);
        e = call(c, r);
        uint32_t wrote = !e && r->out_length >= 4 ? get_le32(r->out) : 0;
        done_with(r, e);
        if (e) return done ? (int64_t)done : e;
        if (!wrote || wrote > chunk) return done ? (int64_t)done : -5;
        done += wrote;
    }
    struct node* n = node_at(c, f->first);
    if (offset + done > n->attr.size) n->attr.size = offset + done;
    if (offset + done > f->size) f->size = offset + done;
    n->attr_expire = 0;
    return (int64_t)done;
}
static int setattr(struct conn* c, uint32_t index, uint32_t valid, const struct mount_attr* a, uint64_t size) {
    struct node* n = node_at(c, index); if (!n) return -5;
    struct request* r = new_request(c, SETATTR, n->nodeid, 88, sizeof(struct fuse_attr_out)); if (!r) return -12;
    uint8_t* in = r->in + IN_HEADER;
    if (n->wfh_open && (valid & 8)) { valid |= 64; put_le64(in + 8, n->wfh); }
    put_le32(in, valid); put_le64(in + 16, size);
    if (a) {
        put_le64(in + 32, (uint64_t)(a->atime / 1000000000LL)); put_le64(in + 40, (uint64_t)(a->mtime / 1000000000LL)); put_le64(in + 48, (uint64_t)(a->ctime / 1000000000LL));
        put_le32(in + 56, (uint32_t)(a->atime % 1000000000LL)); put_le32(in + 60, (uint32_t)(a->mtime % 1000000000LL)); put_le32(in + 64, (uint32_t)(a->ctime % 1000000000LL));
        put_le32(in + 68, a->mode); put_le32(in + 76, credentials_visible_id(a->uid, false)); put_le32(in + 80, credentials_visible_id(a->gid, true));
    }
    int e = call(c, r);
    if (!e && r->out_length >= sizeof(struct fuse_attr_out)) { struct fuse_attr_out* o = (void*)r->out; n = node_at(c, index); n->attr = o->attr; n->attr_expire = expiry(o->attr_valid, o->attr_valid_nsec); }
    done_with(r, e); return e;
}
static int do_truncate(struct conn* c, struct mount* m, fat32_file_t* f, uint64_t size) {
    (void)m; if (c->aborted) return -107;
    int e = setattr(c, f->first, 8, NULL, size); if (!e) f->size = size; return e;
}
static int do_setattr(struct conn* c, struct mount* m, fat32_file_t* f, const struct mount_attr* a) {
    (void)m; if (c->aborted) return -107;
    uint32_t valid = 0;
    if (a->valid & EXT4_ATTR_MODE) valid |= 1;
    if (a->valid & EXT4_ATTR_UID) valid |= 2;
    if (a->valid & EXT4_ATTR_GID) valid |= 4;
    if (a->valid & MOUNT_ATTR_SIZE) valid |= 8;
    if (a->valid & EXT4_ATTR_ATIME) valid |= 16;
    if (a->valid & EXT4_ATTR_MTIME) valid |= 32;
    if (a->valid & EXT4_ATTR_CTIME) valid |= 1024;
    if (!valid) return 0;
    int e = setattr(c, f->first, valid, a, a->size);
    if (!e) { char name[FAT32_LFN_MAX + 1]; strcpy(name, f->name); fill(c, f->first, f); strcpy(f->name, name); }
    return e;
}
static int do_readdir(struct conn* c, struct mount* m, const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) {
    (void)m; if (c->aborted) return -107;
    struct node* n = node_at(c, dir->first); if (!n) return -5;
    if ((n->attr.mode & S_IFMT) != S_IFDIR) return -20;
    if (!n->dfh_open) {
        struct request* r = new_request(c, OPENDIR, n->nodeid, 8, 16); if (!r) return -12;
        int e = call(c, r); if (!e && r->out_length < 16) e = -5;
        if (!e) { n = node_at(c, dir->first); n->dfh = get_le64(r->out); n->dfh_open = true; }
        done_with(r, e); if (e) return e;
    }
    for (;;) {
        n = node_at(c, dir->first);
        struct request* r = new_request(c, READDIR, n->nodeid, 40, 4096); if (!r) return -12;
        put_le64(r->in + IN_HEADER, n->dfh); put_le64(r->in + IN_HEADER + 8, *cookie); put_le32(r->in + IN_HEADER + 16, 4096);
        int e = call(c, r);
        if (e) { done_with(r, e); return e; }
        uint32_t at = 0; int result = 0;
        while (at + 24 <= r->out_length) {   /* fuse_dirent: ino, off, namelen, type, name (8-byte aligned) */
            uint64_t ino = get_le64(r->out + at), off = get_le64(r->out + at + 8); uint32_t length = get_le32(r->out + at + 16), type = get_le32(r->out + at + 20);
            if (!length || length > 255 || at + 24 + length > r->out_length) { result = -5; break; }
            char name[256]; memcpy(name, r->out + at + 24, length); name[length] = 0;
            at += (24 + length + 7) & ~7u;
            *cookie = off;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            memset(out, 0, sizeof(*out));
            static const uint32_t modes[16] = {0, 0010000, 0020000, 0, 0040000, 0, 0060000, 0, 0100000, 0, 0120000, 0, 0140000, 0, 0, 0};
            out->mode = modes[type & 15] ? modes[type & 15] | 0644 : 0100644; out->is_dir = (out->mode & S_IFMT) == S_IFDIR;
            out->ino = (uint32_t)ino; out->generation = (uint32_t)(ino >> 32); out->dir_lba = 0xFFFFFFFE; out->first = 0xFFFFFFFF;
            strncpy(out->name, name, FAT32_LFN_MAX); out->name[FAT32_LFN_MAX] = 0;
            result = 1; break;
        }
        bool empty = r->out_length == 0;
        done_with(r, 0);
        if (result) return result;
        if (empty) return 0;
    }
}
static int make(struct conn* c, struct mount* m, const char* path, uint32_t opcode, uint32_t mode, uint32_t rdev, const char* target, fat32_file_t* out) {
    (void)m; if (c->aborted) return -107;
    uint32_t parent; const char* leaf; int e = walk(c, path, &parent, &leaf, true); if (e) return e;
    char name[256]; leaf_copy(leaf, name); if (!name[0]) return -17;
    unsigned nl = strlen(name) + 1, tl = target ? strlen(target) + 1 : 0;
    uint32_t args = opcode == MKNOD ? 16 + nl : opcode == MKDIR ? 8 + nl : opcode == SYMLINK ? nl + tl : 16 + nl;
    struct request* r = new_request(c, opcode, node_at(c, parent)->nodeid, args, sizeof(struct fuse_entry_out) + 16); if (!r) return -12;
    uint8_t* in = r->in + IN_HEADER; unsigned umask = filesystem_umask();
    if (opcode == MKNOD) { put_le32(in, mode); put_le32(in + 4, rdev); put_le32(in + 8, umask); memcpy(in + 16, name, nl); }
    else if (opcode == MKDIR) { put_le32(in, mode); put_le32(in + 4, umask); memcpy(in + 8, name, nl); }
    else if (opcode == SYMLINK) { memcpy(in, name, nl); memcpy(in + nl, target, tl); }
    else { put_le32(in, 0x41 /* O_CREAT|O_WRONLY */ | 0x80); put_le32(in + 4, mode); put_le32(in + 8, umask); memcpy(in + 16, name, nl); }
    e = call(c, r);
    uint32_t index = 0;
    if (!e) e = entry_reply(c, parent, name, r, &index);
    if (!e && opcode == CREATE && r->out_length >= sizeof(struct fuse_entry_out) + 16) {   /* keep the handle as this node's write handle */
        struct node* n = node_at(c, index); n->wfh = get_le64(r->out + sizeof(struct fuse_entry_out)); n->wfh_open = true;
    }
    done_with(r, e);
    if (!e) fill(c, index, out);
    return e;
}
static int do_create(struct conn* c, struct mount* m, const char* path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, const char* target, fat32_file_t* out) {
    (void)uid; (void)gid;
    uint32_t type = mode & S_IFMT;
    if (type == S_IFDIR) return make(c, m, path, MKDIR, mode & 07777, 0, NULL, out);
    if (type == S_IFLNK) return make(c, m, path, SYMLINK, 0, 0, target, out);
    if (type == S_IFREG) {
        int e = make(c, m, path, CREATE, mode, 0, NULL, out);
        if (e != -38) return e;   /* ENOSYS: the daemon has no CREATE */
    }
    return make(c, m, path, MKNOD, mode, rdev, NULL, out);
}
static int do_unlink(struct conn* c, struct mount* m, const char* path, int directory, fat32_file_t* victim) {
    (void)m; if (c->aborted) return -107;
    uint32_t parent; const char* leaf; int e = walk(c, path, &parent, &leaf, true); if (e) return e;
    char name[256]; leaf_copy(leaf, name);
    uint32_t index; if ((e = lookup_child(c, parent, name, &index))) return e;
    if ((e = getattr(c, index, true))) return e;
    bool dir = (node_at(c, index)->attr.mode & S_IFMT) == S_IFDIR;
    if (directory == 1 && !dir) return -20;
    if (directory == 0 && dir) return -21;
    unsigned nl = strlen(name) + 1;
    struct request* r = new_request(c, dir ? RMDIR : UNLINK, node_at(c, parent)->nodeid, nl, 0); if (!r) return -12;
    memcpy(r->in + IN_HEADER, name, nl);
    e = call(c, r); done_with(r, e);
    if (e) return e;
    fill(c, index, victim);
    struct node* n = node_at(c, index); n->gone = true; victim->detached = true;
    return 0;
}
static int do_release(struct conn* c, struct mount* m, fat32_file_t* f) {
    (void)m;
    if (!f->detached || !node_at(c, f->first)) return 0;
    release_handles(c, f->first);
    struct node* n = node_at(c, f->first);
    if (n->nlookup) { send_forget(c, n->nodeid, n->nlookup); n->nlookup = 0; }
    return 0;
}
static int do_rename(struct conn* c, struct mount* m, const char* from, const char* to, unsigned flags, fat32_file_t* moved, fat32_file_t* replaced) {
    (void)m; if (c->aborted) return -107;
    uint32_t sp, tp; const char *sl, *tl;
    int e = walk(c, from, &sp, &sl, true); if (e) return e;
    if ((e = walk(c, to, &tp, &tl, true))) return e;
    char sname[256], tname[256]; leaf_copy(sl, sname); leaf_copy(tl, tname);
    uint32_t source; if ((e = lookup_child(c, sp, sname, &source))) return e;
    uint32_t target = 0; bool exists = !lookup_child(c, tp, tname, &target);
    unsigned a = strlen(sname) + 1, b = strlen(tname) + 1;
    /* RENAME_NOREPLACE is answered here (many daemons, sshfs among them, refuse RENAME2 flags): EEXIST, else a plain
     * RENAME; only RENAME_EXCHANGE and RENAME_WHITEOUT need the daemon's RENAME2. */
    if ((flags & 1) && exists) return -17;
    if (flags & 1) flags &= ~1u;
    bool v2 = flags != 0;
    struct request* r = new_request(c, v2 ? RENAME2 : RENAME, node_at(c, sp)->nodeid, (v2 ? 16 : 8) + a + b, 0); if (!r) return -12;
    uint8_t* in = r->in + IN_HEADER; put_le64(in, node_at(c, tp)->nodeid);
    if (v2) put_le32(in + 8, flags);
    memcpy(in + (v2 ? 16 : 8), sname, a); memcpy(in + (v2 ? 16 : 8) + a, tname, b);
    e = call(c, r); done_with(r, e);
    if (e) return e;
    memset(replaced, 0, sizeof(*replaced));
    if (exists && target != source && !(flags & 2)) { fill(c, target, replaced); node_at(c, target)->gone = true; replaced->detached = true; }
    struct node* n = node_at(c, source); n->parent = tp; strncpy(n->name, tname, 255); n->entry_expire = 0;
    if (flags & 2 && exists) { struct node* t = node_at(c, target); t->parent = sp; strncpy(t->name, sname, 255); }   /* RENAME_EXCHANGE */
    getattr(c, source, true);
    fill(c, source, moved);
    return 0;
}
static int do_link(struct conn* c, struct mount* m, fat32_file_t* f, const char* path) {
    (void)m; if (c->aborted) return -107;
    uint32_t parent; const char* leaf; int e = walk(c, path, &parent, &leaf, true); if (e) return e;
    char name[256]; leaf_copy(leaf, name); unsigned nl = strlen(name) + 1;
    struct request* r = new_request(c, LINK, node_at(c, parent)->nodeid, 8 + nl, sizeof(struct fuse_entry_out)); if (!r) return -12;
    put_le64(r->in + IN_HEADER, node_at(c, f->first)->nodeid); memcpy(r->in + IN_HEADER + 8, name, nl);
    e = call(c, r); uint32_t index;
    if (!e) e = entry_reply(c, parent, name, r, &index);
    done_with(r, e);
    if (!e) getattr(c, f->first, true);
    return e;
}
static int do_statfs(struct conn* c, struct mount* m, struct mount_statfs* s) {
    (void)m; if (c->aborted) return -107;
    struct request* r = new_request(c, STATFS, 1, 0, 80); if (!r) return -12;
    int e = call(c, r);
    if (!e && r->out_length >= 56) {
        s->type = 0x65735546; s->blocks = get_le64(r->out); s->free = get_le64(r->out + 8); s->available = get_le64(r->out + 16);
        s->files = get_le64(r->out + 24); s->free_files = get_le64(r->out + 32); s->block_size = get_le32(r->out + 40); s->name_max = get_le32(r->out + 44);
        if (!s->block_size) s->block_size = 4096;
    } else if (!e) e = -5;
    done_with(r, e); return e;
}
static int do_fsync(struct conn* c, struct mount* m, fat32_file_t* f) {
    (void)m; if (c->aborted) return -107;
    struct node* n = node_at(c, f->first); if (!n || !n->wfh_open) return 0;
    struct request* r = new_request(c, FSYNC, n->nodeid, 16, 0); if (!r) return -12;
    put_le64(r->in + IN_HEADER, n->wfh);
    int e = call(c, r); done_with(r, e);
    return e == -38 ? 0 : e;
}
static int fu_sync(struct mount* m) { (void)m; return 0; }
static int do_fallocate(struct conn* c, struct mount* m, fat32_file_t* f, unsigned mode, uint64_t offset, uint64_t length) {
    (void)m; if (c->aborted) return -107;
    uint64_t fh; int e = open_handle(c, f->first, true, &fh); if (e) return e;
    struct request* r = new_request(c, FALLOCATE, node_at(c, f->first)->nodeid, 32, 0); if (!r) return -12;
    put_le64(r->in + IN_HEADER, fh); put_le64(r->in + IN_HEADER + 8, offset); put_le64(r->in + IN_HEADER + 16, length); put_le32(r->in + IN_HEADER + 24, mode);
    e = call(c, r); done_with(r, e);
    if (!e) getattr(c, f->first, true);
    return e == -38 ? -95 : e;
}
/* op: 0 get, 1 set, 2 list, 3 remove (mount.c convention) */
static int64_t do_xattr(struct conn* c, struct mount* m, fat32_file_t* f, unsigned op, const char* name, void* value, uint64_t size, unsigned flags) {
    (void)m; if (c->aborted) return -107;
    struct node* n = node_at(c, f->first); if (!n) return -5;
    unsigned nl = name ? strlen(name) + 1 : 0;
    if (size > 65536) size = 65536;
    struct request* r;
    if (op == 0 || op == 2) {
        r = new_request(c, op ? LISTXATTR : GETXATTR, n->nodeid, 8 + nl, size ? (uint32_t)size : 8); if (!r) return -12;
        put_le32(r->in + IN_HEADER, (uint32_t)size); if (nl) memcpy(r->in + IN_HEADER + 8, name, nl);
    } else if (op == 1) {
        r = new_request(c, SETXATTR, n->nodeid, 8 + nl + (uint32_t)size, 0); if (!r) return -12;
        put_le32(r->in + IN_HEADER, (uint32_t)size); put_le32(r->in + IN_HEADER + 4, flags);
        memcpy(r->in + IN_HEADER + 8, name, nl); memcpy(r->in + IN_HEADER + 8 + nl, value, (size_t)size);
    } else { r = new_request(c, REMOVEXATTR, n->nodeid, nl, 0); if (!r) return -12; memcpy(r->in + IN_HEADER, name, nl); }
    int e = call(c, r); int64_t result = e;
    if (!e && (op == 0 || op == 2)) {
        if (!size) result = r->out_length >= 4 ? get_le32(r->out) : -5;   /* fuse_getxattr_out.size */
        else { result = r->out_length; memcpy(value, r->out, r->out_length); }
    }
    done_with(r, e);
    return result == -38 ? -95 : result;
}
static int fu_lookup(struct mount* m, const char* path, fat32_file_t* out) { struct conn* c = hold(m); if (!c) return -107; int r = do_lookup(c, m, path, out); put(c); return r; }
static int fu_refresh(struct mount* m, fat32_file_t* f) { struct conn* c = hold(m); if (!c) return -107; int r = do_refresh(c, m, f); put(c); return r; }
static int64_t fu_read(struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, void* buffer) { struct conn* c = hold(m); if (!c) return -107; int64_t r = do_read(c, m, f, offset, length, buffer); put(c); return r; }
static int64_t fu_write(struct mount* m, fat32_file_t* f, uint64_t offset, uint64_t length, const void* buffer) { struct conn* c = hold(m); if (!c) return -107; int64_t r = do_write(c, m, f, offset, length, buffer); put(c); return r; }
static int fu_truncate(struct mount* m, fat32_file_t* f, uint64_t size) { struct conn* c = hold(m); if (!c) return -107; int r = do_truncate(c, m, f, size); put(c); return r; }
static int fu_setattr(struct mount* m, fat32_file_t* f, const struct mount_attr* a) { struct conn* c = hold(m); if (!c) return -107; int r = do_setattr(c, m, f, a); put(c); return r; }
static int fu_readdir(struct mount* m, const fat32_file_t* dir, uint64_t* cookie, fat32_file_t* out) { struct conn* c = hold(m); if (!c) return -107; int r = do_readdir(c, m, dir, cookie, out); put(c); return r; }
static int fu_create(struct mount* m, const char* path, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev, const char* target, fat32_file_t* out) { struct conn* c = hold(m); if (!c) return -107; int r = do_create(c, m, path, mode, uid, gid, rdev, target, out); put(c); return r; }
static int fu_unlink(struct mount* m, const char* path, int directory, fat32_file_t* victim) { struct conn* c = hold(m); if (!c) return -107; int r = do_unlink(c, m, path, directory, victim); put(c); return r; }
static int fu_release(struct mount* m, fat32_file_t* f) { struct conn* c = hold(m); if (!c) return -107; int r = do_release(c, m, f); put(c); return r; }
static int fu_rename(struct mount* m, const char* from, const char* to, unsigned flags, fat32_file_t* moved, fat32_file_t* replaced) { struct conn* c = hold(m); if (!c) return -107; int r = do_rename(c, m, from, to, flags, moved, replaced); put(c); return r; }
static int fu_link(struct mount* m, fat32_file_t* f, const char* path) { struct conn* c = hold(m); if (!c) return -107; int r = do_link(c, m, f, path); put(c); return r; }
static int fu_statfs(struct mount* m, struct mount_statfs* s) { struct conn* c = hold(m); if (!c) return -107; int r = do_statfs(c, m, s); put(c); return r; }
static int fu_fsync(struct mount* m, fat32_file_t* f) { struct conn* c = hold(m); if (!c) return -107; int r = do_fsync(c, m, f); put(c); return r; }
static int fu_fallocate(struct mount* m, fat32_file_t* f, unsigned mode, uint64_t offset, uint64_t length) { struct conn* c = hold(m); if (!c) return -107; int r = do_fallocate(c, m, f, mode, offset, length); put(c); return r; }
static int64_t fu_xattr(struct mount* m, fat32_file_t* f, unsigned op, const char* name, void* value, uint64_t size, unsigned flags) { struct conn* c = hold(m); if (!c) return -107; int64_t r = do_xattr(c, m, f, op, name, value, size, flags); put(c); return r; }
static void show_options(struct conn* c, struct mount* m, const char* type) {
    m->options[0] = 0; char* o = m->options;
    strcat(o, "user_id="); append_u64(o, credentials_visible_id(c->owner_uid, false));
    strcat(o, ",group_id="); append_u64(o, credentials_visible_id(c->owner_gid, true));
    if (m->fsflags & MOUNT_F_DEFAULT_PERMISSIONS) strcat(o, ",default_permissions");
    if (m->fsflags & MOUNT_F_ALLOW_OTHER) strcat(o, ",allow_other");
    if (c->max_read) { strcat(o, ",max_read="); append_u64(o, c->max_read); }
    (void)type;
}
static int fu_mount_common(struct mount* m, char* options, bool block) {
    if (!credentials_capable(credentials_current(), NULL, 21)) return -1;   /* as Linux: fusermount3 (set-user-ID) for users */
    char* cursor = options; char key[32], value[64]; int64_t fd = -1; uint32_t rootmode = 0, uid = 0, gid = 0; bool have_root = false, have_uid = false, have_gid = false;
    unsigned fsflags = 0; uint32_t max_read = 0;
    while (mount_option(&cursor, key, sizeof(key), value, sizeof(value))) {
        uint64_t v = 0; const char* end = value; bool number = value[0] && parse_u64(value, &v, &end) && !*end;
        bool octal = value[0] != 0; uint64_t oct = 0; for (const char* p = value; *p; p++) { if (*p < '0' || *p > '7') { octal = false; break; } oct = oct * 8 + (uint64_t)(*p - '0'); }
        if (!strcmp(key, "fd") && number && v < 0x7FFFFFFF) fd = (int64_t)v;
        else if (!strcmp(key, "rootmode") && octal) { rootmode = (uint32_t)oct; have_root = true; }
        else if (!strcmp(key, "user_id") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, false, &k)) return -22; uid = k; have_uid = true; }
        else if (!strcmp(key, "group_id") && number) { uint32_t k; if (!credentials_kernel_from_visible((uint32_t)v, true, &k)) return -22; gid = k; have_gid = true; }
        else if (!strcmp(key, "default_permissions")) fsflags |= MOUNT_F_DEFAULT_PERMISSIONS;
        else if (!strcmp(key, "allow_other")) fsflags |= MOUNT_F_ALLOW_OTHER;
        else if (!strcmp(key, "max_read") && number) max_read = (uint32_t)v;
        else if (!strcmp(key, "blksize") && number) { if (!block) return -22; }
        else return -22;
    }
    if (fd < 0 || !have_root || !have_uid || !have_gid || (rootmode & S_IFMT) != S_IFDIR) return -22;
    struct open_file* f = kernel_file((uint64_t)fd); struct input_reader* reader = NULL;
    if (!f || kernel_file_device(f, &reader) != (int)fuse_device || !conn_of(reader)) return -22;
    struct conn* c = conn_of(reader);
    if (c->mounted || c->initialized || c->aborted) return -22;
    if (!c->chunks) { uint32_t root; int e = new_node(c, 1, 0, "", &root); if (e) return e; }
    struct node* root = node_at(c, 0); root->nlookup = 0; root->attr.mode = rootmode; root->attr.ino = 1; root->attr.nlink = 2;
    root->attr.uid = credentials_visible_id(uid, false); root->attr.gid = credentials_visible_id(gid, true); root->entry_expire = ~0ULL;
    c->mounted = true; c->m = m; c->owner_uid = uid; c->owner_gid = gid; c->rootmode = rootmode; c->max_read = max_read; c->block = block;
    m->sb = c; m->owner = uid; m->fsflags = fsflags;
    show_options(c, m, m->type);
    struct request* r = new_request(c, INIT, 0, 64, 64);
    if (!r) { c->mounted = false; c->m = NULL; m->sb = NULL; return -12; }
    put_le32(r->in + IN_HEADER, 7); put_le32(r->in + IN_HEADER + 4, FUSE_MINOR); put_le32(r->in + IN_HEADER + 8, 128 * 1024);
    put_le32(r->in + IN_HEADER + 12, 1 | 32 | 64 | 2048);   /* ASYNC_READ, BIG_WRITES, DONT_MASK, ASYNC_DIO */
    r->abandoned = true; enqueue(c, r);   /* answered from the daemon's loop: mount(2) does not wait for it */
    vga_print("[64] FUSE mounted "); vga_print(m->type); vga_print(" connection "); char n[12] = ""; append_u64(n, c->id); vga_print(n); vga_print("\n");
    return 0;
}
static int fu_mount(struct mount* m, char* options) { return fu_mount_common(m, options, false); }
static int fub_mount(struct mount* m, char* options) { return fu_mount_common(m, options, true); }
static int fu_unmount(struct mount* m, bool force) {
    struct conn* c = m->sb; if (!c) return 0;
    c->active++;
    if (!c->aborted && !force) {
        for (unsigned i = 0; i < c->node_count; i++) release_handles(c, i);
        if (c->block && c->initialized) { struct request* r = new_request(c, DESTROY, 0, 0, 0); if (r) { int e = call(c, r); done_with(r, e); } }
    }
    c->mounted = false; c->m = NULL; m->sb = NULL;
    abort_conn(c);
    put(c);
    return 0;
}
static int fu_remount(struct mount* m, uint64_t flags, char* options) { (void)m; (void)flags; (void)options; return 0; }
const struct mount_ops fuse_mount_ops = {
    .name = "fuse", .flags = MOUNT_POSIX | MOUNT_FUSE,
    .mount = fu_mount, .unmount = fu_unmount, .sync = fu_sync, .statfs = fu_statfs, .remount = fu_remount,
    .lookup = fu_lookup, .refresh = fu_refresh, .read = fu_read, .write = fu_write, .truncate = fu_truncate, .readdir = fu_readdir,
    .create = fu_create, .unlink = fu_unlink, .release = fu_release, .rename = fu_rename, .link = fu_link, .setattr = fu_setattr,
    .fsync = fu_fsync, .fallocate = fu_fallocate, .xattr = fu_xattr, .max_file_size = 0x7FFFFFFFFFFFFFFFULL,
};
const struct mount_ops fuseblk_mount_ops = {
    .name = "fuseblk", .flags = MOUNT_POSIX | MOUNT_FUSE | MOUNT_NEEDS_BLOCK,
    .mount = fub_mount, .unmount = fu_unmount, .sync = fu_sync, .statfs = fu_statfs, .remount = fu_remount,
    .lookup = fu_lookup, .refresh = fu_refresh, .read = fu_read, .write = fu_write, .truncate = fu_truncate, .readdir = fu_readdir,
    .create = fu_create, .unlink = fu_unlink, .release = fu_release, .rename = fu_rename, .link = fu_link, .setattr = fu_setattr,
    .fsync = fu_fsync, .fallocate = fu_fallocate, .xattr = fu_xattr, .max_file_size = 0x7FFFFFFFFFFFFFFFULL,
};
