/* Landlock (kabi, wave 2): landlock_create_ruleset(2), landlock_add_rule(2), landlock_restrict_self(2), ABI v4.
 *
 * A ruleset (an anon fd) handles filesystem rights (v1 EXECUTE .. MAKE_SYM, v2 REFER, v3 TRUNCATE) and TCP rights (v4
 * BIND_TCP, CONNECT_TCP) and holds PATH_BENEATH rules (a directory or file and the rights granted beneath it) and
 * NET_PORT rules. restrict_self (no_new_privs or CAP_SYS_ADMIN) stacks a copy of it as a new layer of the calling
 * thread's domain (at most 16); threads and processes created afterwards inherit the domain, exec keeps it. An access is
 * allowed when every layer allows it: a right the layer does not handle is allowed, a handled one needs a rule on the
 * path or one of its ancestors (rights accumulate up the hierarchy). REFER is denied by every filesystem layer unless a
 * rule grants it; moving or linking into another directory needs REFER on both sides and must not give the file more
 * rights than it had (EXDEV), the basic rights failing first (EACCES), as Linux's current_check_refer_path.
 *
 * Checked operations (native_dispatch calls landlock_check before the handler, on the resolved canonical path):
 * open/openat/openat2/creat (READ_FILE, WRITE_FILE, READ_DIR for directories, TRUNCATE for O_TRUNC, MAKE_REG when it
 * creates; O_PATH is not checked), execve/execveat (EXECUTE on the program), mkdir (MAKE_DIR), mknod (MAKE_REG/CHAR/
 * BLOCK/FIFO/SOCK), symlink (MAKE_SYM), unlink/rmdir (REMOVE_FILE/REMOVE_DIR), link/rename (above), truncate(2)
 * (TRUNCATE), ftruncate (TRUNCATE as allowed when the description was opened: layers added later do not apply),
 * bind of a path AF_UNIX socket (MAKE_SOCK), and TCP bind/connect (BIND_TCP/CONNECT_TCP by port; AF_UNSPEC connect is
 * always allowed). open_by_handle_at is refused while a layer handles filesystem rights. ptrace and the
 * PTRACE_MODE_READ /proc files of a task are allowed only into the same or a nested domain (process.c).
 *
 * Differences from Linux (documented): rules are tied to the path a rule's fd had when it was added (Linux: the inode),
 * so a renamed rule directory or another bind mount of it does not carry the rule; the ELF interpreter and #! programs
 * are not checked for EXECUTE (Linux: open_exec); the check and the operation are two steps under the big lock, a
 * concurrent rename of a parent between them is not excluded. Locking: the big kernel lock. */
#include "native.h"
#include "uaccess.h"
#include "../../kernel/string.h"

#define ABI_VERSION 4
#define FS_EXECUTE 1ULL
#define FS_WRITE_FILE 2ULL
#define FS_READ_FILE 4ULL
#define FS_READ_DIR 8ULL
#define FS_REMOVE_DIR 16ULL
#define FS_REMOVE_FILE 32ULL
#define FS_MAKE_CHAR 64ULL
#define FS_MAKE_DIR 128ULL
#define FS_MAKE_REG 256ULL
#define FS_MAKE_SOCK 512ULL
#define FS_MAKE_FIFO 1024ULL
#define FS_MAKE_BLOCK 2048ULL
#define FS_MAKE_SYM 4096ULL
#define FS_REFER 8192ULL
#define FS_TRUNCATE 16384ULL
#define FS_ALL 0x7FFFULL
#define LL_FILE_RIGHTS (FS_EXECUTE | FS_WRITE_FILE | FS_READ_FILE | FS_TRUNCATE)
#define NET_BIND_TCP 1ULL
#define NET_CONNECT_TCP 2ULL
#define NET_ALL 3ULL
#define MAX_LAYERS 16
#define S_IFMT 0170000
#define S_IFDIR 0040000

struct fs_rule { struct fs_rule* next; uint64_t access; unsigned length; char path[]; };
struct net_rule { struct net_rule* next; uint64_t access; uint16_t port; };
struct ruleset { unsigned references; uint64_t handled_fs, handled_net, watermark; struct fs_rule* fs; struct net_rule* net; };
struct domain { unsigned references, layers; struct ruleset* layer[MAX_LAYERS]; };
struct binding { struct binding* next; uint32_t tid; struct domain* domain; };
static struct kslab binding_slab = {.size = sizeof(struct binding)};
static struct kslab net_slab = {.size = (sizeof(struct net_rule) + 7) & ~7U};
static struct binding* bindings;
unsigned landlock_users;   /* bound threads: kernel.c skips landlock_check while 0 */

uint64_t fd_description_id(struct open_file* description);   /* kernel.c */
uint64_t fd_next_description_id(void);
int kernel_user_path(uint64_t address, char* path);
int kernel_path_at(uint64_t directory, char* path, bool follow, struct open_file** bound);
int kernel_resolved_stat(const char* path, struct open_file* bound, bool follow, struct linux_stat* st);
bool inet_socket_tcp(struct inet_socket* socket, int* family);   /* inet.c */

/* ---- rulesets and domains ---- */
static struct ruleset* ruleset_new(uint64_t fs, uint64_t net) {
    struct ruleset* r = kalloc(sizeof(*r)); if (!r) return NULL;
    memset(r, 0, sizeof(*r)); r->references = 1; r->handled_fs = fs; r->handled_net = net; return r;
}
static void ruleset_put(struct ruleset* r) {
    if (!r || --r->references) return;
    while (r->fs) { struct fs_rule* f = r->fs; r->fs = f->next; kfree(f, sizeof(*f) + f->length + 1); }
    while (r->net) { struct net_rule* n = r->net; r->net = n->next; kslab_free(&net_slab, n); }
    kfree(r, sizeof(*r));
}
static bool rule_add_fs(struct ruleset* r, const char* path, uint64_t access) {
    unsigned length = strlen(path);
    for (struct fs_rule* f = r->fs; f; f = f->next) if (f->length == length && !strcmp(f->path, path)) { f->access |= access; return true; }
    struct fs_rule* f = kalloc(sizeof(*f) + length + 1); if (!f) return false;
    f->access = access; f->length = length; memcpy(f->path, path, length + 1); f->next = r->fs; r->fs = f; return true;
}
static bool rule_add_net(struct ruleset* r, uint16_t port, uint64_t access) {
    for (struct net_rule* n = r->net; n; n = n->next) if (n->port == port) { n->access |= access; return true; }
    struct net_rule* n = kslab_alloc(&net_slab); if (!n) return false;
    n->access = access; n->port = port; n->next = r->net; r->net = n; return true;
}
static struct ruleset* ruleset_copy(const struct ruleset* source) {   /* the immutable layer restrict_self stacks */
    struct ruleset* r = ruleset_new(source->handled_fs, source->handled_net); if (!r) return NULL;
    for (struct fs_rule* f = source->fs; f; f = f->next) if (!rule_add_fs(r, f->path, f->access)) { ruleset_put(r); return NULL; }
    for (struct net_rule* n = source->net; n; n = n->next) if (!rule_add_net(r, n->port, n->access)) { ruleset_put(r); return NULL; }
    r->watermark = fd_next_description_id(); return r;
}
static void domain_put(struct domain* d) {
    if (!d || --d->references) return;
    for (unsigned i = 0; i < d->layers; i++) ruleset_put(d->layer[i]);
    kfree(d, sizeof(*d));
}
static struct domain* domain_of(uint32_t tid) {
    for (struct binding* b = bindings; b; b = b->next) if (b->tid == tid) return b->domain;
    return NULL;
}
static bool bind_thread(uint32_t tid, struct domain* d) {   /* takes a reference of d */
    for (struct binding* b = bindings; b; b = b->next) if (b->tid == tid) { domain_put(b->domain); b->domain = d; return true; }
    struct binding* b = kslab_alloc(&binding_slab); if (!b) return false;
    b->tid = tid; b->domain = d; b->next = bindings; bindings = b; landlock_users++; return true;
}
void landlock_fork(uint32_t parent, uint32_t child) {   /* process.c task_clone */
    if (!landlock_users) return;
    struct domain* d = domain_of(parent); if (!d) return;
    d->references++; if (!bind_thread(child, d)) domain_put(d);
}
void landlock_exit(uint32_t tid) {   /* process.c: the thread's resources go */
    if (!landlock_users) return;
    for (struct binding** link = &bindings; *link; link = &(*link)->next)
        if ((*link)->tid == tid) { struct binding* b = *link; *link = b->next; domain_put(b->domain); kslab_free(&binding_slab, b); landlock_users--; return; }
}
/* process.c ptrace/inspection: into the same or a nested domain only (Linux hook_ptrace_access_check) */
bool landlock_ptrace_allowed(uint32_t tracer, uint32_t tracee) {
    if (!landlock_users) return true;
    struct domain *a = domain_of(tracer), *b = domain_of(tracee);
    if (!a) return true;
    if (!b || b->layers < a->layers) return false;
    for (unsigned i = 0; i < a->layers; i++) if (a->layer[i] != b->layer[i]) return false;
    return true;
}

/* ---- the ruleset descriptor ---- */
static int64_t ruleset_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    (void)object; (void)vectors; (void)count; (void)total; (void)write; (void)flags; return -22;
}
static int ruleset_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) { (void)object; (void)command; (void)argument; (void)flags; return -25; }
static uint32_t ruleset_ready(void* object) { (void)object; return 0; }
static void* ruleset_source(void* object) { return object; }
static void ruleset_release(void* object) { ruleset_put(object); }
static void ruleset_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 6; stat->ino = 1 + ((uint64_t)object >> 3 & 0xFFFFFFFULL); stat->nlink = 1; stat->mode = 0600;
    stat->uid = credentials_visible_id(credentials_kernel_id(credentials_current(), false, 1), false);
    stat->gid = credentials_visible_id(credentials_kernel_id(credentials_current(), true, 1), true); stat->block_size = 4096;
}
static const struct object_operations ruleset_operations = {ruleset_io, ruleset_ioctl, ruleset_ready, ruleset_source, ruleset_release, ruleset_stat};

static int64_t create_ruleset_call(uint64_t address, uint64_t size, uint64_t flags) {
    if (flags) return flags == 1 && !address && !size ? ABI_VERSION : -22;   /* LANDLOCK_CREATE_RULESET_VERSION */
    if (!address) return -14;
    if (size < 8) return -22;
    if (size > 4096) return -7;
    uint64_t attr[2] = {0, 0};
    if (!accessible(address, size, false)) return -14;
    copy_from_user(attr, (const void*)address, size < 16 ? size : 16);
    for (uint64_t at = 16; at < size; at++) { uint8_t byte = uget8(address + at); if (byte) return -7; }
    if (attr[0] & ~FS_ALL) return -22;
    if (attr[1] & ~NET_ALL) return -22;
    if (!attr[0] && !attr[1]) return -42;   /* ENOMSG: an empty ruleset */
    struct ruleset* r = ruleset_new(attr[0], attr[1]); if (!r) return -12;
    int fd = fd_create_object(&ruleset_operations, r, 2 | 0x80000, "anon_inode:[landlock-ruleset]");
    if (fd < 0) ruleset_put(r);
    return fd;
}
static int64_t add_rule_call(uint64_t ruleset_fd, uint64_t type, uint64_t address, uint64_t flags) {
    if (flags) return -22;
    struct open_file* f = fd_lookup(ruleset_fd); if (!f) return -9;
    struct ruleset* r = fd_object(f, &ruleset_operations); if (!r) return -77;   /* EBADFD: not a ruleset */
    if (!(fd_status_flags(f) & 2) && (fd_status_flags(f) & 3) != 1) return -1;   /* opened without write access: EPERM */
    if (type == 1) {   /* LANDLOCK_RULE_PATH_BENEATH: { u64 allowed_access; s32 parent_fd } packed */
        uint8_t attr[12]; if (!accessible(address, 12, false)) return -14;
        copy_from_user(attr, (const void*)address, 12);
        uint64_t access; int32_t parent; memcpy(&access, attr, 8); memcpy(&parent, attr + 8, 4);
        if (!access) return -42;   /* ENOMSG */
        if (access & ~r->handled_fs) return -22;
        struct open_file* p = fd_lookup((uint64_t)(int64_t)parent); if (!p) return -9;
        if (fd_object(p, &ruleset_operations)) return -77;
        struct linux_stat st; if (fd_description_stat(p, &st)) return -77;
        const char* path = fd_internal_path(p);
        if (!path || path[0] != '/') return -77;   /* pipes, sockets, anonymous objects: no place in the hierarchy */
        if ((st.mode & S_IFMT) != S_IFDIR && (access & ~LL_FILE_RIGHTS)) return -22;
        return rule_add_fs(r, path, access) ? 0 : -12;
    }
    if (type == 2) {   /* LANDLOCK_RULE_NET_PORT: { u64 allowed_access; u64 port } packed */
        uint64_t attr[2]; if (!accessible(address, 16, false)) return -14;
        copy_from_user(attr, (const void*)address, 16);
        if (!attr[0]) return -42;   /* ENOMSG */
        if (attr[0] & ~r->handled_net) return -22;
        if (attr[1] > 65535) return -22;
        return rule_add_net(r, (uint16_t)attr[1], attr[0]) ? 0 : -12;
    }
    return -22;
}
static int64_t restrict_self_call(uint64_t ruleset_fd, uint64_t flags) {
    if (flags) return -22;
    struct credentials* self = credentials_current();
    if (!credentials_no_new_privs() && !credentials_capable(self, credentials_namespace(self), 21)) return -1;
    struct open_file* f = fd_lookup(ruleset_fd); if (!f) return -9;
    struct ruleset* r = fd_object(f, &ruleset_operations); if (!r) return -77;
    if ((fd_status_flags(f) & 3) == 1) return -1;   /* not readable: EPERM (FMODE_CAN_READ) */
    struct domain* old = domain_of(task_tid());
    if (old && old->layers >= MAX_LAYERS) return -7;
    struct domain* d = kalloc(sizeof(*d)); if (!d) return -12;
    memset(d, 0, sizeof(*d)); d->references = 1;
    for (unsigned i = 0; old && i < old->layers; i++) { d->layer[i] = old->layer[i]; d->layer[i]->references++; d->layers++; }
    if (!(d->layer[d->layers] = ruleset_copy(r))) { domain_put(d); return -12; }
    d->layers++;
    if (!bind_thread(task_tid(), d)) { domain_put(d); return -12; }
    return 0;
}
int64_t landlock_call(struct registers* r) {
    switch (r->rax) {
    case 444: return create_ruleset_call(r->rdi, r->rsi, (uint32_t)r->rdx);
    case 445: return add_rule_call((uint32_t)r->rdi, (uint32_t)r->rsi, r->rdx, (uint32_t)r->r10);
    case 446: return restrict_self_call((uint32_t)r->rdi, (uint32_t)r->rsi);
    }
    return -38;
}

/* ---- access checks ---- */
static bool beneath(const char* path, const struct fs_rule* rule) {
    if (rule->length == 1 && rule->path[0] == '/') return path[0] == '/';
    return !strncmp(path, rule->path, rule->length) && (!path[rule->length] || path[rule->length] == '/');
}
static uint64_t granted(const struct ruleset* layer, const char* path) {
    uint64_t g = 0; for (struct fs_rule* f = layer->fs; f; f = f->next) if (beneath(path, f)) g |= f->access;
    return g;
}
static uint64_t handled(const struct ruleset* layer) { return layer->handled_fs ? layer->handled_fs | FS_REFER : 0; }   /* REFER: always */
/* every layer that existed when `id` was opened (id 0: all layers) allows `access` on path */
static bool allowed(const struct domain* d, const char* path, uint64_t access, uint64_t id) {
    for (unsigned i = 0; i < d->layers; i++) {
        const struct ruleset* l = d->layer[i];
        if (id && id < l->watermark) continue;
        uint64_t want = access & handled(l);
        if (want & ~granted(l, path)) return false;
    }
    return true;
}
static bool handles_fs(const struct domain* d) { for (unsigned i = 0; i < d->layers; i++) if (d->layer[i]->handled_fs) return true; return false; }
static void parent_of(const char* path, char* out) {
    strcpy(out, path); char* slash = NULL;
    for (char* c = out; *c; c++) if (*c == '/') slash = c;
    if (!slash || slash == out) strcpy(out, "/"); else *slash = 0;
}
static uint64_t make_right(uint32_t mode) {
    switch (mode & S_IFMT) {
    case S_IFDIR: return FS_MAKE_DIR;
    case 0020000: return FS_MAKE_CHAR;
    case 0060000: return FS_MAKE_BLOCK;
    case 0010000: return FS_MAKE_FIFO;
    case 0140000: return FS_MAKE_SOCK;
    case 0120000: return FS_MAKE_SYM;
    default: return FS_MAKE_REG;
    }
}
static uint64_t remove_right(uint32_t mode) { return (mode & S_IFMT) == S_IFDIR ? FS_REMOVE_DIR : FS_REMOVE_FILE; }
/* A path argument resolved as the handler will: 0 (path, *exists, *mode), or an error for the handler to report. */
static int resolve(uint64_t directory, uint64_t address, bool follow, char* path, bool* exists, uint32_t* mode) {
    if (kernel_user_path(address, path) || !path[0]) return -1;
    struct open_file* bound = NULL;
    if (kernel_path_at(directory, path, follow, &bound)) return -1;
    if (bound) { const char* p = fd_internal_path(bound); if (!p || p[0] != '/') return -1; strcpy(path, p); }
    struct linux_stat st; *exists = !kernel_resolved_stat(path, bound, follow, &st); *mode = *exists ? st.mode : 0;
    return 0;
}
static int64_t check(const struct domain* d, const char* path, uint64_t access) { return allowed(d, path, access, 0) ? 0 : -13; }
static int64_t check_open(const struct domain* d, uint64_t directory, uint64_t address, uint64_t flags) {
    if (flags & 0x200000) return 0;   /* O_PATH */
    char path[FAT32_PATH_MAX]; bool exists; uint32_t mode;
    if (resolve(directory, address, !(flags & 0x20000) && !((flags & 0xC0) == 0xC0), path, &exists, &mode)) return 0;
    if (exists && (flags & 0xC0) == 0xC0) return 0;   /* O_CREAT|O_EXCL: EEXIST from the handler */
    if (!exists) {
        if (!(flags & 0x40)) return 0;   /* ENOENT from the handler */
        char parent[FAT32_PATH_MAX]; parent_of(path, parent);
        if (!allowed(d, parent, FS_MAKE_REG, 0)) return -13;
        return check(d, path, ((flags & 3) != 1 ? FS_READ_FILE : 0) | ((flags & 3) ? FS_WRITE_FILE : 0));
    }
    if ((mode & S_IFMT) == S_IFDIR) return (flags & 3) ? 0 : check(d, path, FS_READ_DIR);   /* writing a directory: EISDIR */
    uint64_t access = ((flags & 3) != 1 ? FS_READ_FILE : 0) | ((flags & 3) ? FS_WRITE_FILE : 0);
    if ((flags & 0x200) && (mode & S_IFMT) == 0100000) access |= FS_TRUNCATE;
    return check(d, path, access);
}
/* rename/link (Linux current_check_refer_path); exchange: RENAME_EXCHANGE */
static int64_t check_refer(const struct domain* d, uint64_t old_dir, uint64_t old_address, uint64_t new_dir, uint64_t new_address,
                           bool follow_old, bool removable, bool exchange) {
    char old_path[FAT32_PATH_MAX], new_path[FAT32_PATH_MAX], old_parent[FAT32_PATH_MAX], new_parent[FAT32_PATH_MAX];
    bool old_exists, new_exists; uint32_t old_mode, new_mode;
    if (resolve(old_dir, old_address, follow_old, old_path, &old_exists, &old_mode) || !old_exists) return 0;
    if (resolve(new_dir, new_address, false, new_path, &new_exists, &new_mode)) return 0;
    if (exchange && !new_exists) return 0;
    parent_of(old_path, old_parent); parent_of(new_path, new_parent);
    uint64_t request1 = exchange ? make_right(new_mode) : 0, request2 = make_right(old_mode);   /* source dir, target dir */
    if (removable) { request1 |= remove_right(old_mode); if (new_exists) request2 |= remove_right(new_mode); }
    if (!strcmp(old_parent, new_parent)) return allowed(d, new_parent, request1 | request2, 0) ? 0 : -13;
    bool denied = false, exdev = false;
    for (unsigned i = 0; i < d->layers; i++) {
        const struct ruleset* l = d->layer[i]; uint64_t h = handled(l);
        if (!h) continue;
        uint64_t g1 = granted(l, old_parent), g2 = granted(l, new_parent);
        if ((request1 & h & ~g1) || (request2 & h & ~g2)) { denied = true; continue; }
        if (!(g1 & FS_REFER) || !(g2 & FS_REFER)) { exdev = true; continue; }
        /* no more rights at the destination than the file had at its source (both ways for an exchange) */
        if ((g2 & h & ~granted(l, old_path)) || (exchange && (g1 & h & ~granted(l, new_path)))) exdev = true;
    }
    return denied ? -13 : exdev ? -18 : 0;
}
static int64_t check_parent(const struct domain* d, uint64_t directory, uint64_t address, uint64_t access) {
    char path[FAT32_PATH_MAX], parent[FAT32_PATH_MAX]; bool exists; uint32_t mode;
    if (resolve(directory, address, false, path, &exists, &mode)) return 0;
    parent_of(path, parent); return check(d, parent, access);
}
static int64_t check_remove(const struct domain* d, uint64_t directory, uint64_t address, bool directory_only) {
    char path[FAT32_PATH_MAX], parent[FAT32_PATH_MAX]; bool exists; uint32_t mode;
    if (resolve(directory, address, false, path, &exists, &mode) || !exists) return 0;
    parent_of(path, parent); return check(d, parent, directory_only ? FS_REMOVE_DIR : remove_right(mode));
}
static int64_t check_net(const struct domain* d, uint64_t fd, uint64_t address, uint64_t length, uint64_t access) {
    bool net = false; for (unsigned i = 0; i < d->layers; i++) if (d->layer[i]->handled_net) net = true;
    struct open_file* f = fd_lookup(fd);
    if (!f) return 0;
    uint64_t flags; struct inet_socket* s = fd_inet(f, &flags); int family;
    if (!s) {   /* AF_UNIX bind of a path: MAKE_SOCK in its directory */
        if (access != NET_BIND_TCP || !fd_socket(fd, &flags) || length <= 2 || !handles_fs(d) || !accessible(address, 3, false)) return 0;
        char first = (char)uget8(address + 2);
        if (!first) return 0;   /* abstract */
        char name[110]; uint64_t n = length - 2 < sizeof(name) - 1 ? length - 2 : sizeof(name) - 1;
        if (!accessible(address + 2, n, false)) return 0;
        copy_from_user(name, (const void*)(address + 2), n); name[n] = 0;
        char path[FAT32_PATH_MAX], parent[FAT32_PATH_MAX]; strcpy(path, name); struct open_file* bound = NULL;
        if (kernel_path_at((uint64_t)-100, path, false, &bound) || bound) return 0;
        parent_of(path, parent); return check(d, parent, FS_MAKE_SOCK);
    }
    if (!net || !inet_socket_tcp(s, &family)) return 0;
    if (length < 2 || !accessible(address, length < 28 ? length : 28, false)) return length < 2 ? -22 : 0;
    uint16_t sa_family = uget16(address);
    if (sa_family != 0 && sa_family != 2 && sa_family != 10) return 0;
    if ((sa_family == 0 || sa_family == 2) && length < 16) return -22;
    if (sa_family == 10 && length < 24) return -22;
    if (sa_family == 0) {
        if (access == NET_CONNECT_TCP) return 0;   /* disconnecting is always allowed */
        uint32_t any = uget32(address + 4); if (any) return -97;   /* EAFNOSUPPORT */
    } else if (sa_family != (uint16_t)family) return -22;
    uint16_t raw = uget16(address + 2); uint16_t port = (uint16_t)(raw >> 8 | raw << 8);
    for (unsigned i = 0; i < d->layers; i++) {
        const struct ruleset* l = d->layer[i]; uint64_t want = access & l->handled_net;
        if (!want) continue;
        uint64_t g = 0; for (struct net_rule* n = l->net; n; n = n->next) if (n->port == port) g |= n->access;
        if (want & ~g) return -13;
    }
    return 0;
}
/* native_dispatch, before the handler: 0, or the error the call fails with */
int64_t landlock_check(struct registers* r) {
    struct domain* d = domain_of(task_tid()); if (!d) return 0;
    switch (r->rax) {
    case 2: return check_open(d, (uint64_t)-100, r->rdi, r->rsi);
    case 85: return check_open(d, (uint64_t)-100, r->rdi, 0x241);
    case 257: return check_open(d, r->rdi, r->rsi, r->rdx);
    case 437: {   /* openat2(dirfd, path, struct open_how { u64 flags, mode, resolve }, size) */
        uint64_t how; if (r->r10 < 8 || !accessible(r->rdx, 8, false)) return 0;
        how = uget64(r->rdx); return check_open(d, r->rdi, r->rsi, how);
    }
    case 59: case 322: {
        char path[FAT32_PATH_MAX]; bool exists; uint32_t mode;
        uint64_t directory = r->rax == 59 ? (uint64_t)-100 : r->rdi, address = r->rax == 59 ? r->rdi : r->rsi;
        if (r->rax == 322 && (r->r8 & 0x1000)) {   /* AT_EMPTY_PATH */
            char first = 1; if (accessible(address, 1, false)) first = (char)uget8(address);
            if (!first) { struct open_file* f = fd_lookup(directory); const char* p = f ? fd_internal_path(f) : NULL; return p && p[0] == '/' ? check(d, p, FS_EXECUTE) : 0; }
        }
        if (resolve(directory, address, !(r->rax == 322 && (r->r8 & 0x100)), path, &exists, &mode) || !exists) return 0;
        return check(d, path, FS_EXECUTE);
    }
    case 83: return check_parent(d, (uint64_t)-100, r->rdi, FS_MAKE_DIR);
    case 258: return check_parent(d, r->rdi, r->rsi, FS_MAKE_DIR);
    case 133: return check_parent(d, (uint64_t)-100, r->rdi, make_right((uint32_t)r->rsi & S_IFMT ? (uint32_t)r->rsi : 0100000));
    case 259: return check_parent(d, r->rdi, r->rsi, make_right((uint32_t)r->rdx & S_IFMT ? (uint32_t)r->rdx : 0100000));
    case 88: return check_parent(d, (uint64_t)-100, r->rsi, FS_MAKE_SYM);
    case 266: return check_parent(d, r->rsi, r->rdx, FS_MAKE_SYM);
    case 84: return check_remove(d, (uint64_t)-100, r->rdi, true);
    case 87: return check_remove(d, (uint64_t)-100, r->rdi, false);
    case 263: return check_remove(d, r->rdi, r->rsi, (r->rdx & 0x200) != 0);
    case 82: return check_refer(d, (uint64_t)-100, r->rdi, (uint64_t)-100, r->rsi, false, true, false);
    case 264: return check_refer(d, r->rdi, r->rsi, r->rdx, r->r10, false, true, false);
    case 316: return check_refer(d, r->rdi, r->rsi, r->rdx, r->r10, false, true, (r->r8 & 2) != 0);
    case 86: return check_refer(d, (uint64_t)-100, r->rdi, (uint64_t)-100, r->rsi, false, false, false);
    case 265: return check_refer(d, r->rdi, r->rsi, r->rdx, r->r10, (r->r8 & 0x400) != 0, false, false);
    case 76: {
        char path[FAT32_PATH_MAX]; bool exists; uint32_t mode;
        if (resolve((uint64_t)-100, r->rdi, true, path, &exists, &mode) || !exists) return 0;
        return check(d, path, FS_TRUNCATE);
    }
    case 77: {   /* ftruncate: the rights of the layers that existed when the description was opened */
        struct open_file* f = fd_lookup(r->rdi); const char* p = f ? fd_internal_path(f) : NULL;
        if (!p || p[0] != '/') return 0;
        return allowed(d, p, FS_TRUNCATE, fd_description_id(f)) ? 0 : -13;
    }
    case 304: return handles_fs(d) ? -13 : 0;   /* open_by_handle_at: no path to check */
    case 49: return check_net(d, r->rdi, r->rsi, r->rdx, NET_BIND_TCP);
    case 42: return check_net(d, r->rdi, r->rsi, r->rdx, NET_CONNECT_TCP);
    }
    return 0;
}
