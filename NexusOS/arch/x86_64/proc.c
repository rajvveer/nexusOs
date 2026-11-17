/* Native procfs exposes live task metadata and actual credential namespace maps. */
#include "native.h"
#include "../../kernel/string.h"
#include "gpu.h"
#include "swap.h"

enum kind { ROOT, PROCESS, TASKS, THREAD, FDS, FD_LINK, UID_MAP, GID_MAP, SETGROUPS, NS_DIR, NS_USER, SELF, THREAD_SELF, EXE, NS_PID, NS_CHILDREN, FDINFO_DIR, FDINFO, CWD_LINK, ROOT_LINK, CPUINFO, MEMINFO, SYSFS, EXT_FILE, EXT_DIR, EXT_TASK, RESOLV, NS_NET, NS_IPC };
#define EXT_THREAD 0x80000000U /* EXT_TASK index flag: below /proc/<pid>/task/<tid> */
struct node { enum kind kind; uint32_t tid, group, fd; };
struct proc_handle {
    struct node node; struct user_namespace* space; struct credentials* opener;
    struct pid_namespace* pid_space;
    int ns_kind; uint32_t ns_slot;                       /* ns/net, ns/ipc: the namespace bound at open (retained) */
    struct address_space* image; unsigned image_users;   /* /proc/PID/{mem,environ,maps}: the space bound at open (retained), I/O in flight */
    struct proc_handle* next_image;                       /* bound_images list (big kernel lock) */
};
/* This procfs is mounted in the initial PID namespace; self links use its IDs. */
static bool namespace_node(enum kind kind) { return kind == NS_USER || kind == NS_PID || kind == NS_CHILDREN || kind == NS_NET || kind == NS_IPC; }
/* ns/net and ns/ipc name a namespace slot (namespaces.c) */
static int nsobj_kind(enum kind kind) { return kind == NS_NET ? NSOBJ_NET : kind == NS_IPC ? NSOBJ_IPC : -1; }
static struct pid_namespace* node_pid_space(struct node node) {
    if (node.kind == NS_CHILDREN) return task_children_namespace(node.tid);
    struct pid_identity* identity = pid_lookup(node.tid, pid_namespace_root());
    return identity ? identity->space : NULL;
}
static uint64_t pid_inode(struct pid_namespace* space) { return 0x100000001ULL + space->id; }
/* /etc/resolv.conf comes from the DHCP lease while one is held (dhcp64.c); otherwise the file on disk is used. */
static bool resolv_path(const char* path) { return !strcmp(path, "/etc/resolv.conf") && native_dhcp_bound(); }
bool proc_path(const char* path) { return (!strncmp(path, "/proc", 5) && (!path[5] || path[5] == '/')) || sysfs_path(path) || resolv_path(path); }
void number64(uint64_t value, char* text) {
    char reversed[20]; unsigned count = 0;
    do { reversed[count++] = '0' + value % 10; value /= 10; } while (value);
    for (unsigned i = 0; i < count; i++) text[i] = reversed[count - i - 1];
    text[count] = 0;
}
static bool component(const char** path, const char* name) {
    unsigned size = strlen(name);
    if (strncmp(*path, name, size) || ((*path)[size] && (*path)[size] != '/')) return false;
    *path += size; if (**path == '/') (*path)++; return true;
}
static bool decimal(const char** path, uint32_t* value, bool zero) {
    const char* p = *path; uint64_t result = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') { result = result * 10 + (unsigned)(*p++ - '0'); if (result > 0x7FFFFFFF) return false; }
    if ((*p && *p != '/') || (!zero && !result)) return false;
    *value = (uint32_t)result; *path = *p == '/' ? p + 1 : p; return true;
}
static int resolve(const char* path, bool follow, struct node* node) {
    if (!proc_path(path)) return -2;
    if (resolv_path(path)) { *node = (struct node){.kind = RESOLV}; return 0; }
    if (sysfs_path(path)) { *node = (struct node){.kind = SYSFS}; return sysfs_resolve(path, follow, &node->fd); }
    *node = (struct node){.kind = ROOT}; path += 5; if (*path == '/') path++;
    if (!*path) return 0;
    int extension = procext_global(path);
    if (extension >= 0) { node->kind = procext_global_directory((unsigned)extension) ? EXT_DIR : EXT_FILE; node->fd = (uint32_t)extension; return 0; }
    if (component(&path, "cpuinfo")) { node->kind = CPUINFO; return *path ? -20 : 0; }
    if (component(&path, "meminfo")) { node->kind = MEMINFO; return *path ? -20 : 0; }
    if (component(&path, "self")) {
        node->tid = task_pid(); if (!*path && !follow) { node->kind = SELF; return 0; }
    } else if (component(&path, "thread-self")) {
        node->tid = task_tid(); if (!*path && !follow) { node->kind = THREAD_SELF; return 0; }
    } else if (!decimal(&path, &node->tid, false)) return -2;
    node->group = task_process_id(node->tid); if (!node->group) return -2;
    node->kind = PROCESS;
    if (!*path) return 0;
    if (component(&path, "task")) {
        node->kind = TASKS; if (!*path) return 0;
        uint32_t tid;
        if (!decimal(&path, &tid, false) || task_process_id(tid) != node->group) return -2;
        node->tid = tid; node->kind = THREAD; if (!*path) return 0;
    }
    bool info = component(&path, "fdinfo");
    if (info || component(&path, "fd")) {
        if (!task_inspect_allowed(node->tid)) return -13;
        node->kind = info ? FDINFO_DIR : FDS; if (!*path) return 0;
        if (!decimal(&path, &node->fd, true) || *path || !fd_lookup_table(task_files(node->tid), node->fd)) return -2;
        node->kind = info ? FDINFO : FD_LINK; return 0;
    }
    if (component(&path, "ns")) {
        if (!task_inspect_allowed(node->tid)) return -13;
        node->kind = NS_DIR; if (!*path) return 0;
        if (component(&path, "user")) node->kind = NS_USER;
        else if (component(&path, "net")) node->kind = NS_NET;
        else if (component(&path, "ipc")) node->kind = NS_IPC;
        else if (component(&path, "pid")) node->kind = NS_PID;
        else if (component(&path, "pid_for_children")) node->kind = NS_CHILDREN;
        else return -2;
        if (*path) return -2;
        if (follow && node->kind == NS_CHILDREN && !task_children_namespace(node->tid)->init) return -2;
        return 0;
    }
    if (component(&path, "uid_map")) node->kind = UID_MAP;
    else if (component(&path, "gid_map")) node->kind = GID_MAP;
    else if (component(&path, "setgroups")) node->kind = SETGROUPS;
    else if (component(&path, "exe")) { if (!task_inspect_allowed(node->tid)) return -13; node->kind = EXE; }
    else if (component(&path, "cwd")) { if (!task_inspect_allowed(node->tid)) return -13; node->kind = CWD_LINK; }
    else if (component(&path, "root")) { if (!task_inspect_allowed(node->tid)) return -13; node->kind = ROOT_LINK; }
    else if ((extension = procext_task(path)) >= 0) { node->fd = (uint32_t)extension | (node->kind == THREAD ? EXT_THREAD : 0); node->kind = EXT_TASK; return 0; }
    else return -2;
    return *path ? -2 : 0;
}
static bool directory(enum kind kind) { return kind == ROOT || kind == PROCESS || kind == TASKS || kind == THREAD || kind == FDS || kind == NS_DIR || kind == FDINFO_DIR || kind == EXT_DIR; }
static bool node_directory(struct node node) { return node.kind == SYSFS ? sysfs_directory(node.fd) : directory(node.kind); }
static bool magic_link(enum kind kind) { return kind == FD_LINK || kind == EXE || kind == CWD_LINK || kind == ROOT_LINK; }
static uint64_t inode(struct node node) {
    if (node.kind >= EXT_FILE) return (1ULL << 62) | (uint64_t)node.tid << 32 | (uint64_t)(node.kind - EXT_FILE) << 30 | (node.fd & 0x3FFFFFFFU) << 1 | node.fd >> 31;
    return node.kind == FD_LINK || node.kind == FDINFO ? (1ULL << 63) | (uint64_t)(node.kind == FDINFO) << 62 | (uint64_t)node.tid << 31 | node.fd : ((uint64_t)node.tid << 16) | (node.kind + 1);
}
static int fill_stat(struct node node, bool follow, struct linux_stat* stat) {
    if (node.kind == SYSFS) return sysfs_stat(node.fd, stat);
    if ((node.kind == CWD_LINK || node.kind == ROOT_LINK) && follow) {
        struct filesystem_state* fs = task_filesystem(node.tid); if (!fs) return -2;
        struct open_file* target = filesystem_directory(fs, node.kind == ROOT_LINK);
        return target ? fd_description_stat(target, stat) : disk_path_stat("/", stat);
    }
    if (node.kind == EXE && follow) {
        if (task_executable_deleted(node.tid)) return -2;
        const char* path = task_executable(node.tid); return path ? disk_path_stat(path, stat) : -2;
    }
    if (node.kind == FD_LINK && follow) {
        struct open_file* target = fd_lookup_table(task_files(node.tid), node.fd);
        return target ? fd_description_stat(target, stat) : -2;
    }
    memset(stat, 0, sizeof(*stat)); stat->dev = 7; stat->ino = inode(node); stat->nlink = directory(node.kind) ? 2 : 1; stat->block_size = 4096;
    if (node.kind == TASKS) stat->nlink += task_thread_count(node.group);
    if (node.kind == PROCESS) stat->nlink += 4;
    if (node.kind == THREAD) stat->nlink += 3;
    if (node.kind == ROOT) for (uint32_t pid = task_next_id(0, 0); pid; pid = task_next_id(0, pid)) stat->nlink++;
    uint32_t uid = 0, gid = 0;
    struct credentials* owner = task_credentials(node.tid);
    if (node.tid && !owner) stat->nlink = 0;
    if (owner && task_is_dumpable(node.tid)) { uid = credentials_kernel_id(owner, false, 1); gid = credentials_kernel_id(owner, true, 1); }
    stat->uid = credentials_visible_id(uid, false); stat->gid = credentials_visible_id(gid, true);
    stat->mode = directory(node.kind) ? 0040555 : 0100644;
    if (node.kind == FDINFO || node.kind == CPUINFO || node.kind == MEMINFO || node.kind == RESOLV) stat->mode = 0100444;
    if (node.kind == EXT_FILE || node.kind == EXT_TASK) stat->mode = procext_mode(node.kind == EXT_TASK, node.fd & ~EXT_THREAD);
    if (node.kind == RESOLV) {   /* size and change time of the generated contents (glibc's reload check) */
        unsigned length; uint64_t changed = native_dhcp_resolv_stamp(&length); stat->size = length;
        stat->atime = stat->mtime = stat->ctime = (int64_t)(changed / 1000000000ULL);
        stat->atime_ns = stat->mtime_ns = stat->ctime_ns = changed % 1000000000ULL;
    }
    if (magic_link(node.kind) || node.kind == SELF || node.kind == THREAD_SELF || (namespace_node(node.kind) && !follow)) stat->mode = 0120777;
    if ((node.kind == NS_PID || node.kind == NS_CHILDREN) && follow) {
        struct pid_namespace* space = node_pid_space(node); if (!space || !space->init) return -2;
        stat->dev = 8; stat->ino = pid_inode(space); stat->mode = 0100444;
        stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true);
    }
    if (node.kind == NS_USER && follow) {
        if (!owner) return -2;
        stat->dev = 8; stat->ino = namespace_id(credentials_namespace(owner)) + 1; stat->mode = 0100444;
        stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true);
    }
    if (nsobj_kind(node.kind) >= 0 && follow) {
        int slot = task_nsobj_of(node.tid, nsobj_kind(node.kind)); if (slot < 0) return -2;
        stat->dev = 8; stat->ino = nsobj_inode(nsobj_kind(node.kind), slot); stat->mode = 0100444;
        stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true);
    }
    return 0;
}
int proc_path_stat(const char* path, bool follow, struct linux_stat* stat) {
    struct node node; int error = resolve(path, follow, &node); return error ? error : fill_stat(node, follow, stat);
}
bool procext_image_file(unsigned index);
int64_t procext_image(unsigned index, struct address_space* space, uint64_t address, uint64_t count, uint64_t offset, bool write);
/* /proc/PID/{mem,environ,maps}: like Linux __mem_open/proc_maps_open (mm_access), checked against the opener and bound
 * to the address space the target runs at open. */
static bool image_node(struct node node) { return node.kind == EXT_TASK && procext_image_file(node.fd & ~EXT_THREAD); }
/* Open checks for an existing node (also on a reopen through a bound descriptor). As on Linux, O_CREAT, O_EXCL,
 * O_NOCTTY, O_TRUNC, O_APPEND and O_(D)SYNC are accepted and ignored (O_TRUNC still needs write access), O_CREAT|O_EXCL
 * is EEXIST, and an image file needs ptrace access by the opener whatever the access mode, unless the opener runs in
 * that very space (Linux mm_access); a target without one (exiting) opens as an empty file. */
static int open_check(struct node node, uint64_t flags) {
    if (flags & 0x200000ULL) return (flags & 0x10000) && !node_directory(node) ? -20 : 0;   /* O_PATH: locate only */
    if ((flags & 3) == 3 || (flags & ~(3ULL | 0x40ULL | 0x80ULL | 0x100ULL | 0x200ULL | 0x400ULL | 0x800ULL | 0x101000ULL |
                                       0x80000ULL | 0x10000ULL | 0x20000ULL | 0x8000ULL)) || (flags & 0x10040) == 0x10040) return -22;
    if ((flags & 0xC0) == 0xC0) return -17;
    if ((flags & 0x10000) && !node_directory(node)) return -20;
    if ((flags & 0x40) && node_directory(node)) return -21;
    if (magic_link(node.kind)) return -6;
    bool write = (flags & 3) || (flags & 0x200);
    if (node.kind == EXT_FILE && node.fd < NETPROC_BASE && (procext_mode(false, node.fd) & 0600) != 0400) {   /* /proc/sys/vm: root-owned modes */
        uint32_t mode = procext_mode(false, node.fd);
        bool root = credentials_kernel_id(credentials_current(), false, 3) == 0 || credentials_capable(credentials_current(), NULL, 1);
        if (((flags & 3) != 1 && !(mode & 0400)) || (write && (!(mode & 0200) || !root))) return -13;
        return 0;
    }
    if (write && node.kind != UID_MAP && node.kind != GID_MAP && node.kind != SETGROUPS && !(node.kind == SYSFS && sysfs_writable(node.fd)) &&
        !(node.kind == EXT_TASK && (procext_mode(true, node.fd & ~EXT_THREAD) & 0200)) &&
        !(node.kind == EXT_FILE && node.fd >= NETPROC_BASE && (procext_mode(false, node.fd) & 0200))) return node_directory(node) ? -21 : -13;
    if (write && node.kind != SYSFS && !(node.kind == EXT_FILE && node.fd >= NETPROC_BASE) &&   /* (sysctls: netproc_write decides) */
        !task_is_dumpable(node.tid) && credentials_kernel_id(credentials_current(), false, 3) != 0 &&
        !credentials_capable(credentials_current(), NULL, 1)) return -13;
    struct address_space* image = image_node(node) ? task_space(node.tid) : NULL;
    if (image && image != space_current() && !task_inspect_allowed(node.tid)) return -13;
    return 0;
}
/* An image file serves the bound space while any task still runs in it (Linux: mm_users); after the target's exec
 * or exit it reads and writes nothing, and a descriptor that outlives the process, its PID or its image never
 * reaches another one (the retained space cannot be reused). Usually the bound thread runs it. */
static struct proc_handle* bound_images;
static bool image_live(struct proc_handle* handle) {
    struct address_space* image = handle->image; uint32_t group = handle->node.group;
    if (task_space(handle->node.tid) == image) return true;
    for (uint32_t tid = task_next_id(group, 0); tid; tid = task_next_id(group, tid)) if (task_space(tid) == image) return true;
    for (uint32_t pid = task_next_id(0, 0); pid; pid = task_next_id(0, pid))   /* e.g. a CLONE_VM child of another process */
        for (uint32_t tid = pid == group ? 0 : task_next_id(pid, 0); tid; tid = task_next_id(pid, tid)) if (task_space(tid) == image) return true;
    return false;
}
/* Drop the dead images no I/O uses any more, so an idle descriptor does not keep an exited or exec'd image's pages,
 * mapped files and executables (ETXTBSY) alive: on every procfs open, clone, close, read and write, and from
 * process.c as soon as a space loses a task (exec, or an exited thread's resources released). */
static void release_dead_images(void) {
    for (struct proc_handle** link = &bound_images; *link;) {
        struct proc_handle* handle = *link;
        if (handle->image_users || image_live(handle)) { link = &handle->next_image; continue; }
        struct address_space* image = handle->image; *link = handle->next_image; handle->image = NULL;
        space_release(image); link = &bound_images;   /* restart: releasing may close other descriptions */
    }
}
void proc_release_dead_images(void) { release_dead_images(); }
static void image_bind(struct proc_handle* handle) {
    handle->image = image_node(handle->node) ? task_space(handle->node.tid) : NULL; handle->image_users = 0; handle->next_image = NULL;
    if (handle->image) { space_retain(handle->image); handle->next_image = bound_images; bound_images = handle; }
}
struct proc_handle* proc_open(const char* path, uint64_t flags, int* error) {
    release_dead_images();
    struct node node; bool resolved = false;
    if (flags & 0x20000) {
        struct node link; *error = resolve(path, false, &link); if (*error) return NULL;
        struct linux_stat st;
        if ((flags & 0x200000) && link.kind == SYSFS && !sysfs_stat(link.fd, &st) && (st.mode & 0170000) == 0120000) { node = link; resolved = true; }   /* O_PATH|O_NOFOLLOW */
        else if (link.kind == SELF || link.kind == THREAD_SELF || magic_link(link.kind) || namespace_node(link.kind)) {
            *error = -40; return NULL;
        }
    }
    if (!resolved) { *error = resolve(path, true, &node); if (*error) return NULL; }
    if ((*error = open_check(node, flags))) return NULL;
    int ns_kind = nsobj_kind(node.kind), ns_slot = ns_kind >= 0 ? task_nsobj_of(node.tid, (unsigned)ns_kind) : 0;
    if (ns_slot < 0) { *error = ns_slot; return NULL; }
    struct proc_handle* handle = (void*)page_alloc(); if (!handle) { *error = -12; return NULL; }
    handle->node = node;
    handle->opener = credentials_clone(credentials_current(), false, error);
    if (!handle->opener) { page_free((uint64_t)handle); return NULL; }
    struct credentials* owner = task_credentials(node.tid);
    if (owner) {
        handle->space = credentials_namespace(owner); namespace_retain(handle->space);
    }
    if (node.kind == NS_PID || node.kind == NS_CHILDREN) {
        handle->pid_space = node_pid_space(node); pid_namespace_retain(handle->pid_space);
    }
    handle->ns_kind = ns_kind;
    if (ns_kind >= 0) { handle->ns_slot = (uint32_t)ns_slot; nsobj_retain((unsigned)ns_kind, handle->ns_slot); }
    image_bind(handle);
    *error = 0; return handle;
}
struct proc_handle* proc_clone(struct proc_handle* source, uint64_t flags, int* error) {
    release_dead_images();
    if ((*error = open_check(source->node, flags))) return NULL;
    *error = -12;
    struct proc_handle* handle = (void*)page_alloc(); if (!handle) return NULL;
    *handle = *source; handle->image = NULL;
    handle->opener = credentials_clone(credentials_current(), false, error);
    if (!handle->opener) { page_free((uint64_t)handle); return NULL; }
    if (handle->space) namespace_retain(handle->space);
    if (handle->pid_space) pid_namespace_retain(handle->pid_space);
    if (handle->ns_kind >= 0) nsobj_retain(handle->ns_kind, handle->ns_slot);
    image_bind(handle);   /* a reopen is a fresh open: the current image, checked above */
    *error = 0; return handle;
}
void proc_close(struct proc_handle* handle) {
    if (handle->space) namespace_release(handle->space);
    if (handle->pid_space) pid_namespace_release(handle->pid_space);
    if (handle->ns_kind >= 0) nsobj_release(handle->ns_kind, handle->ns_slot);
    if (handle->image) {   /* still bound: unlink it */
        struct proc_handle** link = &bound_images; while (*link != handle) link = &(*link)->next_image;
        *link = handle->next_image; space_release(handle->image);
    }
    credentials_destroy(handle->opener); page_free((uint64_t)handle);
    release_dead_images();
}
int proc_descriptor_path(struct proc_handle* handle, char* result, unsigned capacity) {
    struct node node = handle->node; char text[96] = "/proc", number[24];
    if (node.kind == SYSFS) return sysfs_node_path(node.fd, result, capacity);
    if (node.kind == NS_USER) {
        strcpy(text, "user:["); number64(namespace_id(handle->space) + 1, number); strcat(text, number); strcat(text, "]");
    } else if (handle->ns_kind >= 0) {
        strcpy(text, node.kind == NS_NET ? "net:[" : "ipc:["); number64(nsobj_inode(handle->ns_kind, handle->ns_slot), number);
        strcat(text, number); strcat(text, "]");
    } else if (node.kind == NS_PID || node.kind == NS_CHILDREN) {
        strcpy(text, "pid:["); number64(pid_inode(handle->pid_space), number); strcat(text, number); strcat(text, "]");
    } else if (node.kind == RESOLV) strcpy(text, "/etc/resolv.conf");
    else if (node.kind == CPUINFO) strcat(text, "/cpuinfo");
    else if (node.kind == MEMINFO) strcat(text, "/meminfo");
    else if (node.kind == EXT_FILE || node.kind == EXT_DIR) {
        unsigned length; const char* name = procext_global_path(node.fd, &length);
        strcat(text, "/"); unsigned at = strlen(text); if (at + length >= sizeof(text)) return -36;
        memcpy(text + at, name, length); text[at + length] = 0;
    }
    else if (node.kind != ROOT) {
        strcat(text, "/"); number64(node.kind == THREAD ? node.group : node.tid, number); strcat(text, number);
        if (node.kind == THREAD) { strcat(text, "/task/"); number64(node.tid, number); strcat(text, number); }
        else if (node.kind == TASKS) strcat(text, "/task");
        else if (node.kind == FDS) strcat(text, "/fd");
        else if (node.kind == FDINFO_DIR || node.kind == FDINFO) {
            strcat(text, "/fdinfo");
            if (node.kind == FDINFO) { strcat(text, "/"); number64(node.fd, number); strcat(text, number); }
        }
        else if (node.kind == NS_DIR) strcat(text, "/ns");
        else if (node.kind == UID_MAP) strcat(text, "/uid_map");
        else if (node.kind == GID_MAP) strcat(text, "/gid_map");
        else if (node.kind == SETGROUPS) strcat(text, "/setgroups");
        else if (node.kind == EXT_TASK) {
            if (node.fd & EXT_THREAD) { strcpy(text, "/proc/"); number64(node.group, number); strcat(text, number); strcat(text, "/task/"); number64(node.tid, number); strcat(text, number); }
            strcat(text, "/"); strcat(text, procext_task_name(node.fd & ~EXT_THREAD));
        }
    }
    unsigned size = strlen(text); if (size >= capacity) return -36;
    memcpy(result, text, size + 1); return size;
}
/* setns(2) on an opened /proc/PID/ns/<type> file: the namespace bound at open (namespaces.c checks and installs). */
int proc_setns(struct proc_handle* handle, uint64_t nstype) {
    switch (handle->node.kind) {
    case NS_USER: return handle->space ? namespace_setns(0x10000000UL, 0, handle->space, NULL, nstype) : -22;
    case NS_PID: case NS_CHILDREN: return namespace_setns(0x20000000UL, 0, NULL, handle->pid_space, nstype);
    case NS_NET: return namespace_setns(0x40000000UL, handle->ns_slot, NULL, NULL, nstype);
    case NS_IPC: return namespace_setns(0x08000000UL, handle->ns_slot, NULL, NULL, nstype);
    default: return -22;
    }
}
int proc_stat(struct proc_handle* handle, struct linux_stat* stat) {
    if (handle->pid_space) {
        memset(stat, 0, sizeof(*stat)); stat->dev = 8; stat->ino = pid_inode(handle->pid_space);
        stat->mode = 0100444; stat->nlink = 1; stat->block_size = 4096;
        stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true); return 0;
    }
    if (handle->ns_kind >= 0) {
        memset(stat, 0, sizeof(*stat)); stat->dev = 8; stat->ino = nsobj_inode(handle->ns_kind, handle->ns_slot);
        stat->mode = 0100444; stat->nlink = 1; stat->block_size = 4096;
        stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true); return 0;
    }
    if (handle->node.kind == NS_USER && handle->space) {
        memset(stat, 0, sizeof(*stat)); stat->dev = 8; stat->ino = namespace_id(handle->space) + 1;
        stat->mode = 0100444; stat->nlink = 1; stat->block_size = 4096;
        stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true); return 0;
    }
    return fill_stat(handle->node, true, stat);
}
static int copy_text(const char* text, unsigned length, uint64_t* skip, uint64_t address, uint64_t capacity, uint64_t* copied) {
    if (*skip >= length) { *skip -= length; return 0; }
    unsigned count = length - (unsigned)*skip; if (count > capacity - *copied) count = (unsigned)(capacity - *copied);
    if (count && !accessible(address + *copied, count, true)) return -14;
    if (count && copy_to_user((void*)(address + *copied), text + *skip, count)) return -14;
    *copied += count; *skip = 0; return 0;
}
static void cpu_field(char* text, const char* key, uint32_t value) {
    char number[12]; uint_to_str(value, number); strcat(text, key); strcat(text, "\t: "); strcat(text, number); strcat(text, "\n");
}
static void cpu_text(char text[1024]) {
    uint32_t a, b, c, d; char vendor[13], brand[49] = {0};
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4); vendor[12] = 0;
    /* NexusOS currently schedules only the boot CPU; identity comes from its actual CPUID. */
    strcpy(text, "processor\t: 0\nvendor_id\t: "); strcat(text, vendor); strcat(text, "\n");
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    unsigned family = (a >> 8) & 15, model = (a >> 4) & 15;
    if (family == 6 || family == 15) model |= ((a >> 16) & 15) << 4;
    if (family == 15) family += (a >> 20) & 255;
    cpu_field(text, "cpu family", family); cpu_field(text, "model", model); cpu_field(text, "stepping", a & 15);
    cpu_field(text, "apicid", b >> 24); cpu_field(text, "initial apicid", b >> 24);
    strcat(text, "flags\t\t:");
    if (d & 1) strcat(text, " fpu"); if (d & (1u << 4)) strcat(text, " tsc");
    if (d & (1u << 15)) strcat(text, " cmov"); if (d & (1u << 23)) strcat(text, " mmx");
    if (d & (1u << 24)) strcat(text, " fxsr"); if (d & (1u << 25)) strcat(text, " sse"); if (d & (1u << 26)) strcat(text, " sse2");
    if (c & 1) strcat(text, " pni"); if (c & (1u << 9)) strcat(text, " ssse3");
    if (c & (1u << 19)) strcat(text, " sse4_1"); if (c & (1u << 20)) strcat(text, " sse4_2");
    if (c & (1u << 23)) strcat(text, " popcnt"); if (c & (1u << 25)) strcat(text, " aes"); if (c & (1u << 30)) strcat(text, " rdrand");
    /* AVX/XSAVE are not advertised until the scheduler saves the extended state. */
    strcat(text, "\n");
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a >= 0x80000004u) {
        for (unsigned i = 0; i < 3; i++) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002u + i), "c"(0));
            memcpy(brand + i * 16, &a, 4); memcpy(brand + i * 16 + 4, &b, 4); memcpy(brand + i * 16 + 8, &c, 4); memcpy(brand + i * 16 + 12, &d, 4);
        }
        strcat(text, "model name\t: "); strcat(text, brand); strcat(text, "\n");
    }
    strcat(text, "\n");
}
/* I/O on an image file: the space bound at open while a task still runs in it, else nothing (Linux: mmget_not_zero
 * fails); image_users keeps it bound across a sleep in the copy. */
static int64_t image_io(struct proc_handle* handle, uint64_t address, uint64_t count, uint64_t offset, bool write) {
    if (!handle->image || !image_live(handle)) return 0;
    handle->image_users++;
    int64_t result = procext_image(handle->node.fd & ~EXT_THREAD, handle->image, address, count, offset, write);
    handle->image_users--; return result;
}
int64_t proc_read(struct proc_handle* handle, uint64_t address, uint64_t count, uint64_t offset) {
    enum kind kind = handle->node.kind;
    release_dead_images();
    if (image_node(handle->node)) return image_io(handle, address, count, offset, false);
    if (kind == SYSFS) return sysfs_read(handle->node.fd, address, count, offset);
    if (directory(kind)) return -21;
    if (kind == EXT_FILE || kind == EXT_TASK)
        return procext_read(kind == EXT_TASK, handle->node.fd & ~EXT_THREAD, handle->node.tid, (handle->node.fd & EXT_THREAD) != 0, address, count, offset);
    if (kind != UID_MAP && kind != GID_MAP && kind != SETGROUPS && kind != FDINFO && kind != CPUINFO && kind != MEMINFO && kind != RESOLV) return -22;
    if (!count) return 0;
    if (!user_range(address, count)) return -14;
    uint64_t copied = 0;
    if (kind == RESOLV) {
        char text[512]; unsigned length = native_dhcp_resolv(text, sizeof(text)); uint64_t skip = offset;
        int error = copy_text(text, length, &skip, address, count, &copied); return error ? error : (int64_t)copied;
    }
    if (kind == MEMINFO) {   /* reclaim.c: the Linux field set, swap included */
        char* text = (void*)page_alloc(); if (!text) return -12;
        int length = reclaim_proc_text("meminfo", text, 4096);
        int error = length < 0 ? length : copy_text(text, (unsigned)length, &offset, address, count, &copied);
        page_free((uint64_t)text); return error ? error : (int64_t)copied;
    }
    if (kind == CPUINFO) {
        char text[1024]; cpu_text(text);
        int error = copy_text(text, strlen(text), &offset, address, count, &copied); return error ? error : (int64_t)copied;
    }
    if (kind == FDINFO) {
        if (!task_inspect_allowed(handle->node.tid)) return -13;
        uint64_t values[4]; int error = fd_information(task_files(handle->node.tid), handle->node.fd, values);
        if (error) return error;
        char text[160] = "pos:\t", number[24], reversed[24]; number64(values[0], number); strcat(text, number);
        strcat(text, "\nflags:\t0"); unsigned n = 0;
        do { reversed[n++] = '0' + (values[1] & 7); values[1] >>= 3; } while (values[1]);
        for (unsigned i = 0; i < n; i++) number[i] = reversed[n - 1 - i]; number[n] = 0; strcat(text, number);
        strcat(text, "\nmnt_id:\t"); number64(values[2], number); strcat(text, number);
        strcat(text, "\nino:\t"); number64(values[3], number); strcat(text, number); strcat(text, "\n");
        char* full = (char*)arena_alloc(65536); if (!full) return -12;   /* + the type's lines (fdinfo.c) */
        strcpy(full, text); unsigned length = strlen(full);
        length += fd_information_text(task_files(handle->node.tid), handle->node.fd, full + length, 65536 - length);
        error = copy_text(full, length, &offset, address, count, &copied); arena_free((uint64_t)full, 65536);
        return error ? error : (int64_t)copied;
    }
    if (kind == SETGROUPS) {
        const char* text = namespace_groups_allowed(handle->space) ? "allow\n" : "deny\n";
        int error = copy_text(text, strlen(text), &offset, address, count, &copied); return error ? error : (int64_t)copied;
    }
    for (unsigned i = 0; copied < count; i++) {
        uint32_t values[3];
        if (!namespace_mapping(handle->space, credentials_namespace(handle->opener), kind == GID_MAP, i, values)) break;
        char line[40], number[12]; line[0] = 0;
        for (unsigned j = 0; j < 3; j++) { uint_to_str(values[j], number); strcat(line, number); strcat(line, j == 2 ? "\n" : " "); }
        int error = copy_text(line, strlen(line), &offset, address, count, &copied); if (error) return copied ? (int64_t)copied : error;
    }
    return copied;
}
int64_t proc_write(struct proc_handle* handle, uint64_t address, uint64_t count, uint64_t offset) {
    release_dead_images();
    if (image_node(handle->node)) return image_io(handle, address, count, offset, true);
    if (handle->node.kind == EXT_TASK) return procext_write(handle->node.fd & ~EXT_THREAD, handle->node.tid, address, count);
    if (handle->node.kind == SYSFS) return sysfs_write(handle->node.fd, address, count);
    if (handle->node.kind == EXT_FILE && handle->node.fd >= NETPROC_BASE) return procext_write(handle->node.fd, 0, address, count);   /* sysctl */
    if (handle->node.kind == EXT_FILE) return procext_write_global(handle->node.fd, address, count);
    if (handle->node.kind != UID_MAP && handle->node.kind != GID_MAP && handle->node.kind != SETGROUPS) return -13;
    if (!count || count >= 4096) return -22;
    if (!accessible(address, count, false)) return -14;
    char text[4096]; copy_from_user(text, (void*)address, count);
    return handle->node.kind == SETGROUPS ? namespace_write_groups(handle->space, handle->opener, text, count, offset) :
        namespace_write_map(handle->space, handle->opener, handle->node.kind == GID_MAP, text, count, offset);
}
int proc_directory(struct proc_handle* handle, uint64_t position, struct directory_item* item) {
    struct node node = handle->node;
    if (node.kind == SYSFS) return sysfs_entry(node.fd, position, item);
    if (!directory(node.kind)) return -20;
    if (node.tid && !task_credentials(node.tid)) return -2;
    if ((node.kind == FDS || node.kind == FDINFO_DIR) && !task_inspect_allowed(node.tid)) return -13;
    memset(item, 0, sizeof(*item)); item->next = position + 1; item->type = 4;
    if (position < 2) { strcpy(item->name, position ? ".." : "."); item->inode = position ? 1 : inode(node); return 1; }
    if (node.kind == ROOT) {
        if (position < 4) { strcpy(item->name, position == 2 ? "self" : "thread-self"); item->type = 10; item->inode = position; return 1; }
        if (position == 4) { strcpy(item->name, "cpuinfo"); item->type = 8; item->inode = inode((struct node){.kind = CPUINFO}); return 1; }
        if (position == 5) { strcpy(item->name, "meminfo"); item->type = 8; item->inode = inode((struct node){.kind = MEMINFO}); return 1; }
        bool directory_child; unsigned child;
        if (position >= 0x40000000ULL) {   /* global procext entries follow the process directories */
            if (!procext_global_child(-1, position - 0x40000000ULL, item->name, &directory_child, &child)) return 0;
            item->type = directory_child ? 4 : 8; item->inode = inode((struct node){.kind = directory_child ? EXT_DIR : EXT_FILE, .fd = child}); return 1;
        }
        uint32_t pid = position - 6 > 0x7FFFFFFF ? 0 : task_next_id(0, position - 6);
        if (!pid) {
            if (!procext_global_child(-1, 0, item->name, &directory_child, &child)) return 0;
            item->next = 0x40000001ULL; item->type = directory_child ? 4 : 8;
            item->inode = inode((struct node){.kind = directory_child ? EXT_DIR : EXT_FILE, .fd = child}); return 1;
        }
        uint_to_str(pid, item->name); item->next = (uint64_t)pid + 6; item->inode = inode((struct node){.kind = PROCESS, .tid = pid}); return 1;
    }
    if (node.kind == TASKS) {
        if (position - 2 > 0x7FFFFFFF) return 0;
        uint32_t tid = task_next_id(node.group, position - 2); if (!tid) return 0;
        uint_to_str(tid, item->name); item->next = (uint64_t)tid + 2; item->inode = inode((struct node){.kind = THREAD, .tid = tid}); return 1;
    }
    if (node.kind == FDS || node.kind == FDINFO_DIR) {
        if (position - 2 > 0x7FFFFFFF) return 0;
        int fd = fd_next_number(task_files(node.tid), position - 2); if (fd < 0) return 0;
        uint_to_str((uint32_t)fd, item->name); item->next = (uint64_t)fd + 3; item->type = node.kind == FDS ? 10 : 8;
        item->inode = inode((struct node){.kind = node.kind == FDS ? FD_LINK : FDINFO, .tid = node.tid, .fd = fd}); return 1;
    }
    if (node.kind == NS_DIR) {
        if (position > 6) return 0;
        const char* names[] = {"user", "ipc", "net", "pid", "pid_for_children"};
        const enum kind kinds[] = {NS_USER, NS_IPC, NS_NET, NS_PID, NS_CHILDREN};
        strcpy(item->name, names[position - 2]); item->type = 10;
        item->inode = inode((struct node){.kind = kinds[position - 2], .tid = node.tid}); return 1;
    }
    if (node.kind == EXT_DIR) {
        bool directory_child; unsigned child;
        if (!procext_global_child((int)node.fd, position - 2, item->name, &directory_child, &child)) return 0;
        item->type = directory_child ? 4 : 8; item->inode = inode((struct node){.kind = directory_child ? EXT_DIR : EXT_FILE, .fd = child}); return 1;
    }
    const char* names[] = {"task", "fd", "uid_map", "gid_map", "setgroups", "ns", "exe", "fdinfo", "cwd", "root"};
    const enum kind kinds[] = {TASKS, FDS, UID_MAP, GID_MAP, SETGROUPS, NS_DIR, EXE, FDINFO_DIR, CWD_LINK, ROOT_LINK};
    uint64_t index = position - 2 + (node.kind == THREAD);
    if (index >= 10) {
        const char* name = index - 10 < 0x10000 ? procext_task_name((unsigned)(index - 10)) : NULL; if (!name) return 0;
        strcpy(item->name, name); item->type = 8;
        item->inode = inode((struct node){.kind = EXT_TASK, .tid = node.tid, .fd = (uint32_t)(index - 10) | (node.kind == THREAD ? EXT_THREAD : 0)}); return 1;
    }
    strcpy(item->name, names[index]); node.kind = kinds[index]; item->inode = inode(node);
    item->type = directory(node.kind) ? 4 : magic_link(node.kind) ? 10 : 8; return 1;
}
int proc_readlink(const char* path, char* result, unsigned capacity) {
    struct node node; int error = resolve(path, false, &node); if (error) return error;
    if (node.kind == SYSFS) return sysfs_readlink(node.fd, result, capacity);
    char text[512], value[24]; text[0] = 0;
    if (node.kind == SELF) number64(task_pid(), text);
    else if (node.kind == THREAD_SELF) { number64(task_pid(), text); strcat(text, "/task/"); number64(task_tid(), value); strcat(text, value); }
    else if (node.kind == NS_USER) {
        struct credentials* owner = task_credentials(node.tid); if (!owner) return -2;
        strcpy(text, "user:["); number64(namespace_id(credentials_namespace(owner)) + 1, value); strcat(text, value); strcat(text, "]");
    } else if (nsobj_kind(node.kind) >= 0) {
        int slot = task_nsobj_of(node.tid, nsobj_kind(node.kind)); if (slot < 0) return -2;
        strcpy(text, node.kind == NS_NET ? "net:[" : "ipc:["); number64(nsobj_inode(nsobj_kind(node.kind), slot), value);
        strcat(text, value); strcat(text, "]");
    } else if (node.kind == NS_PID || node.kind == NS_CHILDREN) {
        struct pid_namespace* space = node_pid_space(node); if (!space || !space->init) return -2;
        strcpy(text, "pid:["); number64(pid_inode(space), value); strcat(text, value); strcat(text, "]");
    } else if (node.kind == EXE) {
        const char* name = task_executable(node.tid); if (!name) return -2; strcpy(text, filesystem_visible_path(name));
        if (task_executable_deleted(node.tid)) {
            if (strlen(text) + 10 >= sizeof(text)) return -36;
            strcat(text, " (deleted)");
        }
    } else if (node.kind == CWD_LINK || node.kind == ROOT_LINK) {
        struct filesystem_state* fs = task_filesystem(node.tid); if (!fs) return -2;
        struct open_file* directory = filesystem_directory(fs, node.kind == ROOT_LINK);
        if (directory) return fd_description_path(directory, result, capacity);
        strcpy(text, filesystem_visible_path(filesystem_path(fs, node.kind == ROOT_LINK)));
    } else if (node.kind == FD_LINK) {
        struct open_file* file = fd_lookup_table(task_files(node.tid), node.fd);
        return file ? fd_description_path(file, result, capacity) : -2;
    } else return -22;
    unsigned size = strlen(text); if (size >= capacity) return -36;
    memcpy(result, text, size + 1); return size;
}
/* /proc/PID/fd/N: the open file it names (kernel.c reopens a pipe through it), else NULL */
struct open_file* proc_fd_object(const char* path) {
    struct node node; if (resolve(path, true, &node) || node.kind != FD_LINK) return NULL;
    return fd_lookup_table(task_files(node.tid), node.fd);
}
int proc_magic_target(const char* path, char* result, unsigned capacity, struct open_file** bound) {
    struct node node; int error = resolve(path, false, &node); if (error) return error;
    if (!magic_link(node.kind)) return 0;
    const char* target = NULL; *bound = NULL;
    if (node.kind == FD_LINK) {
        *bound = fd_lookup_table(task_files(node.tid), node.fd); if (!*bound) return -2;
        target = fd_internal_path(*bound); if (target[0] != '/') { *bound = NULL; return 0; }
    } else if (node.kind == EXE) {
        /* shortcut: opening an unlinked executable needs a retained proc magic target. */
        if (task_executable_deleted(node.tid)) return -2;
        target = task_executable(node.tid);
    }
    else {
        struct filesystem_state* fs = task_filesystem(node.tid); if (!fs) return -2;
        *bound = filesystem_directory(fs, node.kind == ROOT_LINK);
        target = filesystem_path(fs, node.kind == ROOT_LINK);
    }
    if (!target) return -2;
    if (strlen(target) >= capacity) return -36;
    strcpy(result, target); return 1;
}
