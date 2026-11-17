/* Process file descriptors and clone3: pidfd_open(2), pidfd_send_signal(2), pidfd_getfd(2), waitid(P_PIDFD),
 * clone3(2) (CLONE_PIDFD, exit_signal, stack + stack_size, tls, CLONE_CLEAR_SIGHAND; clone(2)'s CLONE_PIDFD too).
 *
 * A pidfd holds a reference on the process's pid identity (pid numbers are never reused here, so the identity's
 * internal number names the process for good). It polls readable (POLLIN) once every thread of the process has exited
 * and POLLIN|POLLHUP once the process was reaped; process.c reports both moments (pidfd_process_changed), and the
 * identity is the event source of every pidfd of that process. read/write: EINVAL (Linux). A pidfd is O_CLOEXEC.
 * Not supported: PIDFD_THREAD pidfds (EINVAL, as before Linux 6.9), /proc/<pid> directory fds as pidfds (EBADF),
 * clone3 set_tid (EPERM without CAP_SYS_ADMIN, else EINVAL: pid numbers here only grow), CLONE_INTO_CGROUP (EBADF: no
 * cgroup2 directories), CLONE_NEWTIME (EINVAL). Locking: big kernel lock. */
#include "native.h"
#include "uaccess.h"
#include "../../kernel/string.h"

#define CLONE_PIDFD 0x1000ULL
#define CLONE_PARENT_SETTID 0x00100000ULL
#define CLONE_DETACHED 0x00400000ULL
#define CLONE_THREAD 0x10000ULL
#define CLONE_SIGHAND 0x800ULL
#define CLONE_PARENT 0x8000ULL
#define CLONE_NEWTIME 0x80ULL
#define CLONE_CLEAR_SIGHAND 0x100000000ULL
#define CLONE_INTO_CGROUP 0x200000000ULL
struct pidfd { struct pid_identity* identity; uint32_t pid; uint32_t pad; };
static struct kslab pidfd_slab = {.size = sizeof(struct pidfd)};
static const struct object_operations pidfd_operations;

static int64_t pidfd_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    (void)object; (void)vectors; (void)count; (void)total; (void)write; (void)flags; return -22;
}
static int pidfd_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) { (void)object; (void)command; (void)argument; (void)flags; return -25; }
static uint32_t pidfd_ready(void* object) {
    int state = task_pidfd_state(((struct pidfd*)object)->pid);
    return state == 2 ? 1U | 0x10U : state == 1 ? 1U : 0;   /* POLLIN (+ POLLRDNORM in fd_ready), POLLHUP once reaped */
}
static void* pidfd_source(void* object) { return ((struct pidfd*)object)->identity; }
static void pidfd_release(void* object) { struct pidfd* p = object; pid_release(p->identity); kslab_free(&pidfd_slab, p); }
static void pidfd_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 6; stat->ino = ((struct pidfd*)object)->pid; stat->nlink = 1; stat->mode = 0600;
    stat->uid = credentials_visible_id(credentials_kernel_id(credentials_current(), false, 1), false);
    stat->gid = credentials_visible_id(credentials_kernel_id(credentials_current(), true, 1), true); stat->block_size = 4096;
}
/* /proc/PID/fdinfo (Linux pidfd_show_fdinfo): the pid in the reader's PID namespace, -1 once reaped or not visible */
static unsigned pidfd_fdinfo(void* object, char* out, unsigned capacity) {
    struct pidfd* p = object; int64_t nr = task_pidfd_state(p->pid) == 2 ? -1 : (int64_t)pid_number(p->identity, task_pid_namespace());
    if (!nr) nr = -1;
    return kfmt(out, capacity, 0, "Pid:\t%lld\nNSpid:\t%lld\n", nr, nr);
}
static const struct object_operations pidfd_operations = {pidfd_io, pidfd_ioctl, pidfd_ready, pidfd_source, pidfd_release, pidfd_stat};
unsigned pidfd_fdinfo_text(struct open_file* description, char* out, unsigned capacity) {   /* kernel.c fd_information_text */
    void* object = fd_object(description, &pidfd_operations); return object ? pidfd_fdinfo(object, out, capacity) : 0;
}
void pidfd_process_changed(struct pid_identity* identity) { if (identity) event_source_changed(identity, 1); }
static int pidfd_create(struct pid_identity* identity, uint64_t flags) {
    struct pidfd* p = kslab_alloc(&pidfd_slab); if (!p) return -12;
    pid_retain(identity); p->identity = identity; p->pid = pid_number(identity, pid_namespace_root());
    int fd = fd_create_object(&pidfd_operations, p, 2 | 0x80000 | (flags & 0x800), "anon_inode:[pidfd]");
    if (fd < 0) { pid_release(identity); kslab_free(&pidfd_slab, p); }
    return fd;
}
/* clone/clone3 CLONE_PIDFD (process.c task_clone, before the child runs): install the pidfd, store it at `address`. */
int pidfd_clone_install(struct pid_identity* identity, uint64_t address) {
    if (!accessible(address, 4, true)) return -14;
    int fd = pidfd_create(identity, 0); if (fd < 0) return fd;
    uput32(address, (uint32_t)fd); return 0;
}
static struct pidfd* pidfd_get(uint64_t fd, struct open_file** file) {
    struct open_file* f = fd_lookup(fd); if (file) *file = f;
    return f ? fd_object(f, &pidfd_operations) : NULL;
}
static int64_t pidfd_open_call(int64_t pid, uint64_t flags) {
    if (flags & ~0x800ULL) return -22;   /* PIDFD_NONBLOCK; PIDFD_THREAD is not supported */
    if (pid <= 0 || pid > 0x7FFFFFFF) return -22;
    struct pid_identity* identity = pid_lookup((uint32_t)pid, task_pid_namespace()); if (!identity) return -3;
    uint32_t internal = pid_number(identity, pid_namespace_root());
    if (task_pidfd_state(internal) == 2) return -22;   /* not a thread-group leader (or a reaped process): no TGID task */
    return pidfd_create(identity, flags);
}
static int64_t pidfd_send_signal_call(uint64_t fd, uint64_t number, uint64_t address, uint64_t flags) {
    if ((flags & ~7ULL) || (flags & (flags - 1))) return -22;   /* PIDFD_SIGNAL_THREAD / _THREAD_GROUP / _PROCESS_GROUP */
    struct open_file* f; struct pidfd* p = pidfd_get(fd, &f); if (!f || !p) return -9;
    uint32_t visible = pid_number(p->identity, task_pid_namespace()); if (!visible) return -22;
    if (number > 64) return -22;
    if (address) {
        int32_t info[32];
        if (!accessible(address, 128, false)) return -14;
        copy_from_user(info, (const void*)address, 128);
        if ((uint32_t)info[0] != number) return -22;
        if ((p->pid != task_tid() || flags == 4) && (info[2] >= 0 || info[2] == -6)) return -1;   /* Linux: type > PIDTYPE_TGID */
        if (task_pidfd_state(p->pid) == 2) return -3;
        if (flags == 4) {
            uint32_t tid = task_live_thread(p->pid); struct task_info ti; if (!tid || !task_info(tid, false, &ti)) return -3;
            return task_signal_info(-(int64_t)task_visible_pid(ti.group), 0, info);
        }
        return task_signal_info(visible, flags == 1 ? (int32_t)visible : 0, info);
    }
    if (task_pidfd_state(p->pid) == 2) return -3;
    if (flags == 4) {   /* the process group of the process */
        uint32_t tid = task_live_thread(p->pid); if (!tid) return -3;
        struct task_info info; if (!task_info(tid, false, &info)) return -3;
        return task_signal(-(int64_t)task_visible_pid(info.group), number, 0);
    }
    if (flags == 1) return task_thread_signal((int32_t)visible, (int32_t)visible, number);
    return task_signal(visible, number, 0);
}
static int64_t pidfd_getfd_call(uint64_t fd, uint64_t target, uint64_t flags) {
    if (flags) return -22;
    struct open_file* f; struct pidfd* p = pidfd_get(fd, &f); if (!f || !p) return -9;
    if (task_pidfd_state(p->pid) == 2) return -3;
    uint32_t tid = task_live_thread(p->pid); if (!tid) return -9;   /* exited: no descriptor table left (Linux EBADF) */
    if (!task_attach_allowed(tid)) return -1;
    struct open_file* description = fd_lookup_table(task_files(tid), (uint32_t)target); if (!description) return -9;
    return fd_install(description, 0, true);
}
/* waitid(P_PIDFD, fd, info, options, rusage): P_PID on the pidfd's process; a non-blocking pidfd gives EAGAIN instead
 * of waiting (Linux kernel_waitid). */
static int64_t pidfd_waitid(struct registers* r) {
    uint64_t options = r->r10;
    if ((options & ~(0x100000FULL | 0xE0000000ULL)) || !(options & 14)) return -22;
    struct open_file* f; struct pidfd* p = pidfd_get(r->rsi, &f); if (!f || !p) return -9;
    uint32_t visible = pid_number(p->identity, task_pid_namespace()); if (!visible) return -10;
    bool nonblock = (fd_status_flags(f) & 0x800) && !(options & 1);
    int result = task_waitid(1, visible, r->rdx, options | (nonblock ? 1 : 0), r->r8);
    if (!result && nonblock && !task_waitid_found()) return -11;
    return result;
}
struct clone_args64 { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup; };
_Static_assert(sizeof(struct clone_args64) == 88, "struct clone_args (CLONE_ARGS_SIZE_VER2)");
static int64_t clone3_call(struct registers* r) {
    uint64_t address = r->rdi, size = r->rsi;
    if (size > 4096) return -7;   /* E2BIG */
    if (size < 64) return -22;    /* CLONE_ARGS_SIZE_VER0 */
    if (!accessible(address, size, false)) return -14;
    struct clone_args64 args = {0};
    copy_from_user(&args, (const void*)address, size < sizeof(args) ? size : sizeof(args));
    for (uint64_t at = sizeof(args); at < size; at++) {   /* a newer struct: unknown fields must be zero */
        uint8_t byte = uget8(address + at); if (byte) return -7;
    }
    if (args.set_tid_size > 32 || (!args.set_tid && args.set_tid_size) || (args.set_tid && !args.set_tid_size)) return -22;
    if ((args.exit_signal & ~0xFFULL) || args.exit_signal > 64) return -22;
    if ((args.flags & CLONE_INTO_CGROUP) && (args.cgroup > 0x7FFFFFFF || size < 88)) return -22;
    if (args.flags & ~(0xFFFFFFFFULL | CLONE_CLEAR_SIGHAND | CLONE_INTO_CGROUP)) return -22;
    if (args.flags & (CLONE_DETACHED | (0xFFULL & ~CLONE_NEWTIME))) return -22;
    if ((args.flags & (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)) == (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)) return -22;
    if ((args.flags & (CLONE_THREAD | CLONE_PARENT)) && args.exit_signal) return -22;
    if (args.stack ? !args.stack_size || !user_range(args.stack, args.stack_size) : args.stack_size != 0) return -22;
    if ((args.flags & CLONE_PIDFD) && (args.flags & CLONE_PARENT_SETTID) && args.pidfd == args.parent_tid) return -22;
    if (args.flags & CLONE_NEWTIME) return -22;   /* no time namespaces */
    if (args.set_tid_size) {
        if (!accessible(args.set_tid, args.set_tid_size * 4, false)) return -14;
        if (!credentials_capable(credentials_current(), NULL, 21)) return -1;
        return -22;   /* set_tid: pid numbers here only grow, a chosen number cannot be given out */
    }
    if (args.flags & CLONE_INTO_CGROUP) return -9;
    struct registers arguments = *r;
    arguments.rax = 56; arguments.rdi = (args.flags & 0xFFFFFF00ULL) | args.exit_signal;
    arguments.rsi = args.stack ? args.stack + args.stack_size : 0;
    arguments.rdx = args.parent_tid; arguments.r10 = args.child_tid; arguments.r8 = args.tls;
    struct clone3_request request = {.frame = r, .pidfd = args.pidfd, .clear_sighand = (args.flags & CLONE_CLEAR_SIGHAND) != 0};
    return task_clone3(&request, &arguments);
}
int64_t pidfd_call(struct registers* r) {
    switch (r->rax) {
    case 434: return pidfd_open_call((int64_t)(int32_t)r->rdi, r->rsi);
    case 424: return pidfd_send_signal_call((uint32_t)r->rdi, r->rsi, r->rdx, r->r10);
    case 438: return pidfd_getfd_call((uint32_t)r->rdi, r->rsi, r->rdx);
    case 435: return clone3_call(r);
    case 247: return pidfd_waitid(r);
    }
    return -38;
}
