/* nsfs magic links + namespace syscalls (tests/test_x64_nsfs.py, /bin/nsfs64; arch/x86_64/namespaces.c).
 *
 * /proc/PID/ns/<type> and /proc/PID/fd/N are magic links on Linux: access/stat/open THROUGH them reach the namespace or
 * the open file, which is what browser sandboxes probe (Chromium NamespaceUtils::KernelSupportsUnprivilegedNamespace
 * = access("/proc/self/ns/<type>", F_OK); Firefox SandboxInfo needs user/pid/net/ipc). Then CLONE_NEWNET/CLONE_NEWIPC,
 * unshare and setns(2) with the Linux capability rules (a user-namespace root cannot setns back into the initial
 * network namespace). Runs unprivileged (uid 1000). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] NSFS FAIL line=%d errno=%d pid=%d\n", __LINE__, errno, getpid()); _exit(99); } } while (0)

static ino_t ns_inode(const char* path) { struct stat st; CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode)); return st.st_ino; }
static ino_t fd_inode(int fd) { struct stat st; CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode)); return st.st_ino; }
static void link_names(const char* path, const char* type, ino_t inode) {
    char text[64] = {0}, expect[64];
    snprintf(expect, sizeof(expect), "%s:[%llu]", type, (unsigned long long)inode);
    CHECK(readlink(path, text, sizeof(text) - 1) == (ssize_t)strlen(expect) && !strcmp(text, expect));
}
static uint64_t capabilities(void) {
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2]; CHECK(syscall(SYS_capget, &header, data) == 0);
    return (uint64_t)data[1].effective << 32 | data[0].effective;
}
static int wait_child(pid_t pid) { int status; CHECK(waitpid(pid, &status, 0) == pid); return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status); }
static int reaches(const char* ip) {   /* connect() to a non-loopback address: ENETUNREACH inside a new netns */
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); CHECK(s >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(9)}; inet_pton(AF_INET, ip, &a.sin_addr);
    int r = connect(s, (struct sockaddr*)&a, sizeof(a)); int e = errno; close(s);
    return r == 0 || e == EINPROGRESS || e == ECONNREFUSED ? 0 : e;
}

static void probes(void) {
    /* 1. the browser probes: access(F_OK) through the link succeeds for every supported namespace type */
    const char* types[] = {"user", "ipc", "net", "pid"};
    for (unsigned i = 0; i < 4; i++) {
        char path[64]; snprintf(path, sizeof(path), "/proc/self/ns/%s", types[i]);
        CHECK(access(path, F_OK) == 0 && access(path, R_OK) == 0);
        CHECK(syscall(SYS_faccessat, AT_FDCWD, path, F_OK, 0) == 0);
        struct stat st, lst; CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0444);
        CHECK(lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode));
        link_names(path, types[i], st.st_ino);
        int fd = open(path, O_RDONLY | O_CLOEXEC); CHECK(fd >= 0 && fd_inode(fd) == st.st_ino);
        char fdpath[64]; snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
        link_names(fdpath, types[i], st.st_ino);                       /* the open file names the namespace */
        CHECK(access(fdpath, F_OK) == 0 && ns_inode(fdpath) == st.st_ino);
        CHECK(open(path, O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
        CHECK(close(fd) == 0);
    }
    CHECK(access("/proc/self/ns/bogus", F_OK) == -1 && errno == ENOENT);
    CHECK(ns_inode("/proc/self/ns/ipc") == 0xEFFFFFFFU);              /* Linux PROC_IPC_INIT_INO */
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/ns/net", getpid());
    CHECK(ns_inode(path) == ns_inode("/proc/self/ns/net"));
    unsigned seen = 0; DIR* d = opendir("/proc/self/ns"); CHECK(d);
    for (struct dirent* e; (e = readdir(d));) {
        for (unsigned i = 0; i < 4; i++) if (!strcmp(e->d_name, types[i])) { CHECK(e->d_type == DT_LNK); seen |= 1u << i; }
        if (!strcmp(e->d_name, "pid_for_children")) seen |= 16;
    }
    closedir(d); CHECK(seen == 31);
    puts("[64] NSFS PASS: /proc/self/ns/{user,ipc,net,pid}: access/stat/open/readlink through the magic link, ELOOP with O_NOFOLLOW");

    /* 2. /proc/self/fd/N magic links to non-path objects: pipe, socket, eventfd, memfd */
    int pipefd[2], pair[2]; CHECK(pipe(pipefd) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int event = eventfd(0, 0), memory = memfd_create("nsfs", 0); CHECK(event >= 0 && memory >= 0);
    CHECK(write(memory, "magic", 5) == 5);
    char fdpath[64], text[64]; struct stat st;
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", pipefd[1]);
    CHECK(access(fdpath, W_OK) == 0 && stat(fdpath, &st) == 0 && S_ISFIFO(st.st_mode));
    memset(text, 0, sizeof(text)); CHECK(readlink(fdpath, text, sizeof(text) - 1) > 6 && !strncmp(text, "pipe:[", 6));
    int again = open(fdpath, O_WRONLY); CHECK(again >= 0 && write(again, "x", 1) == 1 && read(pipefd[0], text, 1) == 1 && text[0] == 'x');
    CHECK(close(again) == 0);
    /* the access mode picks the end (Linux fifo_open): a second reader through the write end's link */
    int reader = open(fdpath, O_RDONLY | O_CLOEXEC); CHECK(reader >= 0 && fcntl(reader, F_GETFD) == FD_CLOEXEC);
    CHECK(write(pipefd[1], "y", 1) == 1 && read(reader, text, 1) == 1 && text[0] == 'y');
    CHECK(close(pipefd[0]) == 0);                                   /* the reopened reader keeps the pipe readable */
    signal(SIGPIPE, SIG_IGN);
    CHECK(write(pipefd[1], "z", 1) == 1 && read(reader, text, 1) == 1 && text[0] == 'z');
    CHECK(close(reader) == 0 && write(pipefd[1], "w", 1) == -1 && errno == EPIPE);   /* no reader left */
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", pipefd[1]);
    reader = open(fdpath, O_RDONLY); CHECK(reader >= 0);           /* a reader comes back */
    CHECK(write(pipefd[1], "v", 1) == 1 && read(reader, text, 1) == 1 && text[0] == 'v');
    CHECK(open(fdpath, O_RDWR) == -1 && errno == ENXIO);
    pipefd[0] = reader;
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", pair[0]);
    CHECK(open(fdpath, O_RDWR) == -1 && errno == ENXIO);           /* sockets cannot be opened (Linux ENXIO) */
    CHECK(access(fdpath, F_OK) == 0 && stat(fdpath, &st) == 0 && S_ISSOCK(st.st_mode));
    memset(text, 0, sizeof(text)); CHECK(readlink(fdpath, text, sizeof(text) - 1) > 8 && !strncmp(text, "socket:[", 8));
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", event);
    CHECK(access(fdpath, F_OK) == 0);
    memset(text, 0, sizeof(text)); CHECK(readlink(fdpath, text, sizeof(text) - 1) > 0 && !strcmp(text, "anon_inode:[eventfd]"));
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", memory);
    CHECK(access(fdpath, R_OK) == 0 && stat(fdpath, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 5);
    again = open(fdpath, O_RDONLY); CHECK(again >= 0 && pread(again, text, 5, 0) == 5 && !memcmp(text, "magic", 5) && close(again) == 0);
    CHECK(access("/proc/self/fd/987", F_OK) == -1 && errno == ENOENT);
    close(pipefd[0]); close(pipefd[1]); close(pair[0]); close(pair[1]); close(event); close(memory);
    puts("[64] NSFS PASS: /proc/self/fd/N magic links (pipe, socket, eventfd, memfd): access/stat/open reach the object");
}

static void namespaces(void) {
    ino_t net0 = ns_inode("/proc/self/ns/net"), ipc0 = ns_inode("/proc/self/ns/ipc");
    int net_root = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC), file = open("/proc/self/stat", O_RDONLY);
    CHECK(net_root >= 0 && file >= 0);
    /* 3. unprivileged: no new net/IPC namespace without CAP_SYS_ADMIN, no setns, type filter, non-ns fds */
    CHECK(capabilities() == 0);
    CHECK(unshare(CLONE_NEWNET) == -1 && errno == EPERM && unshare(CLONE_NEWIPC) == -1 && errno == EPERM);
    CHECK(setns(net_root, CLONE_NEWNET) == -1 && errno == EPERM);
    CHECK(setns(net_root, CLONE_NEWIPC) == -1 && errno == EINVAL);
    CHECK(setns(file, 0) == -1 && errno == EINVAL && setns(-1, 0) == -1 && errno == EBADF);
    CHECK(syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWIPC | CLONE_SYSVSEM | SIGCHLD, 0, 0, 0, 0) == -1 && errno == EINVAL);
    CHECK(reaches("10.0.2.2") != ENETUNREACH);
    puts("[64] NSFS PASS: unprivileged CLONE_NEWNET/NEWIPC and setns are EPERM; nstype filter, non-namespace fd EINVAL");

    /* 4. CLONE_NEWUSER|CLONE_NEWNET|CLONE_NEWIPC together (the sandbox case) */
    int gate[2]; CHECK(pipe(gate) == 0);
    pid_t child = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWIPC | SIGCHLD, 0, 0, 0, 0);
    CHECK(child >= 0);
    if (!child) {
        close(gate[1]);
        ino_t net1 = ns_inode("/proc/self/ns/net"), ipc1 = ns_inode("/proc/self/ns/ipc");
        CHECK(net1 != net0 && ipc1 != ipc0 && access("/proc/self/ns/net", F_OK) == 0);
        link_names("/proc/self/ns/net", "net", net1);
        CHECK(reaches("10.0.2.2") == ENETUNREACH);                      /* isolated: loopback only */
        int udp = socket(AF_INET, SOCK_DGRAM, 0); CHECK(udp >= 0);       /* ...for datagrams sent unconnected too */
        struct sockaddr_in far = {.sin_family = AF_INET, .sin_port = htons(53)}; inet_pton(AF_INET, "10.0.2.3", &far.sin_addr);
        CHECK(sendto(udp, "x", 1, 0, (struct sockaddr*)&far, sizeof(far)) == -1 && errno == ENETUNREACH);
        struct sockaddr_in near = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        CHECK(sendto(udp, "x", 1, 0, (struct sockaddr*)&near, sizeof(near)) == 1 && close(udp) == 0);
        int lo = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        CHECK(lo >= 0 && bind(lo, (struct sockaddr*)&a, sizeof(a)) == 0 && close(lo) == 0);
        /* root in the new user namespace, but the initial netns is owned by the initial user namespace */
        CHECK(capabilities() == ((1ULL << 41) - 1));
        CHECK(setns(net_root, CLONE_NEWNET) == -1 && errno == EPERM && ns_inode("/proc/self/ns/net") == net1);
        /* a nested netns created here IS owned by this user namespace: setns between the two works */
        int net1_fd = open("/proc/self/ns/net", O_RDONLY); CHECK(net1_fd >= 0);
        CHECK(unshare(CLONE_NEWNET) == 0); ino_t net2 = ns_inode("/proc/self/ns/net"); CHECK(net2 != net1 && net2 != net0);
        int net2_fd = open("/proc/self/ns/net", O_RDONLY); CHECK(net2_fd >= 0);
        CHECK(setns(net1_fd, CLONE_NEWNET) == 0 && ns_inode("/proc/self/ns/net") == net1);
        CHECK(setns(net2_fd, 0) == 0 && ns_inode("/proc/self/ns/net") == net2);
        CHECK(unshare(CLONE_NEWIPC) == 0 && ns_inode("/proc/self/ns/ipc") != ipc1);
        char c; CHECK(read(gate[0], &c, 1) == 1);                       /* parent inspected us */
        _exit(0);
    }
    close(gate[0]);
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/ns/net", child);
    int child_net = -1;
    for (int tries = 0; tries < 200; tries++) {   /* wait until the child is in its nested namespace */
        int fd = open(path, O_RDONLY);
        if (fd >= 0 && fd_inode(fd) != net0) { child_net = fd; break; }
        if (fd >= 0) close(fd);
        usleep(10000);
    }
    CHECK(child_net >= 0);
    ino_t held = fd_inode(child_net);
    CHECK(write(gate[1], "x", 1) == 1 && wait_child(child) == 0 && close(gate[1]) == 0);
    CHECK(fd_inode(child_net) == held);              /* an open ns file keeps the namespace alive past its last task */
    CHECK(setns(child_net, CLONE_NEWNET) == -1 && errno == EPERM);
    CHECK(close(child_net) == 0 && close(net_root) == 0 && close(file) == 0);
    CHECK(ns_inode("/proc/self/ns/net") == net0);
    puts("[64] NSFS PASS: CLONE_NEWUSER|NEWNET|NEWIPC isolates (ENETUNREACH), userns root cannot setns into the initial netns, setns between its own netns works");

    /* 5. setns(CLONE_NEWUSER): the owner of a user namespace may enter it (and gains its capabilities there) */
    int ready[2], hold[2]; CHECK(pipe(ready) == 0 && pipe(hold) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(ready[0]); close(hold[1]);
        CHECK(unshare(CLONE_NEWUSER) == 0 && write(ready[1], "r", 1) == 1);
        char c; CHECK(read(hold[0], &c, 1) == 1); _exit(0);
    }
    close(ready[1]); close(hold[0]);
    char c; CHECK(read(ready[0], &c, 1) == 1);
    pid_t joiner = fork(); CHECK(joiner >= 0);
    if (!joiner) {
        snprintf(path, sizeof(path), "/proc/%d/ns/user", child);
        int user = open(path, O_RDONLY); CHECK(user >= 0);
        ino_t target = fd_inode(user);
        CHECK(setns(user, CLONE_NEWPID) == -1 && errno == EINVAL);
        /* a process sharing its root/cwd (CLONE_FS) may not enter (Linux userns_install: fs->users != 1) */
        pid_t sharer = syscall(SYS_clone, CLONE_FS | SIGCHLD, 0, 0, 0, 0); CHECK(sharer >= 0);
        if (!sharer) _exit(setns(user, CLONE_NEWUSER) == -1 && errno == EINVAL ? 0 : 99);
        CHECK(wait_child(sharer) == 0);
        CHECK(setns(user, CLONE_NEWUSER) == 0 && ns_inode("/proc/self/ns/user") == target);
        CHECK(capabilities() == ((1ULL << 41) - 1));
        CHECK(setns(user, CLONE_NEWUSER) == -1 && errno == EINVAL);   /* already there */
        /* PID: the active PID namespace is owned by the initial user namespace -> EPERM from here */
        int pid_root = open("/proc/self/ns/pid", O_RDONLY); CHECK(pid_root >= 0);
        CHECK(setns(pid_root, CLONE_NEWPID) == -1 && errno == EPERM);
        _exit(0);
    }
    CHECK(wait_child(joiner) == 0);
    CHECK(write(hold[1], "x", 1) == 1 && wait_child(child) == 0);
    close(ready[0]); close(hold[1]);
    puts("[64] NSFS PASS: setns(CLONE_NEWUSER) by the namespace owner (full caps inside, EINVAL re-entering), setns(CLONE_NEWPID) EPERM outside the owner");

    /* 6. setns(CLONE_NEWPID) into a descendant PID namespace owned by our user namespace */
    child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(unshare(CLONE_NEWUSER | CLONE_NEWPID) == 0);
        int go[2]; CHECK(pipe(go) == 0);
        pid_t init = fork(); CHECK(init >= 0);
        if (!init) { close(go[1]); char x; CHECK(getpid() == 1 && read(go[0], &x, 1) == 1); _exit(0); }
        close(go[0]);
        int children = open("/proc/self/ns/pid_for_children", O_RDONLY); CHECK(children >= 0);
        int active = open("/proc/self/ns/pid", O_RDONLY); CHECK(active >= 0 && fd_inode(active) != fd_inode(children));
        CHECK(setns(children, CLONE_NEWPID) == 0 && ns_inode("/proc/self/ns/pid_for_children") == fd_inode(children));
        CHECK(setns(active, CLONE_NEWPID) == -1 && errno == EPERM);   /* the initial pid ns is not ours */
        CHECK(write(go[1], "x", 1) == 1 && wait_child(init) == 0);
        _exit(0);
    }
    CHECK(wait_child(child) == 0);
    puts("[64] NSFS PASS: setns(CLONE_NEWPID) into a descendant PID namespace owned by our user namespace");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    probes();
    namespaces();
    puts("[64] NSFS ALL PASSED: nsfs magic links, CLONE_NEWNET/NEWIPC, unshare, setns(net/ipc/user/pid)");
    return 0;
}
