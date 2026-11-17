#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/vfs.h>
/* FAT32 roots refuse writes outside the user's areas with EROFS; ext4 roots enforce modes (EACCES). */
static int denied_errno(void) { struct statfs s; return statfs("/", &s) == 0 && s.f_type == 0xEF53 ? EACCES : EROFS; }

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] NAMESPACE FAIL line=%d errno=%d pid=%d\n", __LINE__, errno, getpid()); _exit(99); } } while (0)
static volatile sig_atomic_t signal_uid = -1, signal_pid;
static void handler(int signal, siginfo_t* info, void* context) {
    (void)signal; (void)context; signal_uid = info->si_uid; signal_pid = info->si_pid;
}
static uint64_t capabilities(void) {
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2]; CHECK(syscall(SYS_capget, &header, data) == 0);
    CHECK(data[0].effective == data[0].permitted && data[1].effective == data[1].permitted);
    return (uint64_t)data[1].effective << 32 | data[0].effective;
}
static void drop_capabilities(void) {
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2] = {{0}};
    CHECK(syscall(SYS_capset, &header, data) == 0 && capabilities() == 0);
    data[0].effective = data[0].permitted = 1U << CAP_SYS_ADMIN;
    CHECK(syscall(SYS_capset, &header, data) == -1 && errno == EPERM);
}
static int write_text(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC); CHECK(fd >= 0);
    int result = (int)write(fd, text, strlen(text)), error = errno;
    CHECK(close(fd) == 0); errno = error; return result;
}
static void maps(const char* text) {
    CHECK(write_text("/proc/self/setgroups", "deny") == 4);
    CHECK(write_text("/proc/self/gid_map", text) == (int)strlen(text));
    CHECK(write_text("/proc/self/uid_map", text) == (int)strlen(text));
}
static pid_t new_namespace(void) {
    pid_t pid = (pid_t)syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, NULL, NULL, NULL, NULL);
    CHECK(pid >= 0); return pid;
}
static void check_directory(const char* path, const char* required) {
    DIR* directory = opendir(path); CHECK(directory);
    int found = 0, dot = 0, parent = 0; errno = 0;
    struct dirent* entry;
    while ((entry = readdir(directory))) {
        CHECK(entry->d_reclen >= 24 && entry->d_ino != 0);
        if (!strcmp(entry->d_name, required)) found++;
        if (!strcmp(entry->d_name, ".")) dot++;
        if (!strcmp(entry->d_name, "..")) parent++;
    }
    CHECK(errno == 0 && found == 1 && dot == 1 && parent == 1 && closedir(directory) == 0);
}
static int gate[2];
static void* waiting_thread(void* unused) { (void)unused; char byte; CHECK(read(gate[0], &byte, 1) == 1); return NULL; }
int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (argv[1][0] == 'e') {
        CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && capabilities() == 0);
        CHECK(getauxval(AT_UID) == 0 && getauxval(AT_EUID) == 0 && prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
        puts("[64] NAMESPACE no_new_privs and mapped auxv survive exec PASS"); return 0;
    }
    pid_t parent = getpid(); CHECK(getuid() == 1000 && getgid() == 1000 && capabilities() == 0);
    CHECK(getgroups(0, NULL) == 0 && setgroups(0, NULL) == -1 && errno == EPERM);
    check_directory("/", "proc"); check_directory("/", "dev"); check_directory("/dev", "urandom");
    int device = open("/dev/urandom", O_RDONLY | O_CLOEXEC); CHECK(device >= 0 && fcntl(device, F_GETFD) == FD_CLOEXEC);
    unsigned char random_a[64], random_b[64]; struct stat device_stat;
    CHECK(fstat(device, &device_stat) == 0 && S_ISCHR(device_stat.st_mode) && major(device_stat.st_rdev) == 1 && minor(device_stat.st_rdev) == 9);
    CHECK(read(device, random_a, sizeof(random_a)) == sizeof(random_a) && read(device, random_b, sizeof(random_b)) == sizeof(random_b));
    CHECK(memcmp(random_a, random_b, sizeof(random_a)) && close(device) == 0);
    device = open("/dev/random", O_RDONLY | O_NONBLOCK); CHECK(device >= 0 && read(device, random_a, sizeof(random_a)) == sizeof(random_a) && close(device) == 0);
    device = open("/dev/zero", O_RDWR); CHECK(device >= 0 && read(device, random_a, sizeof(random_a)) == sizeof(random_a));
    for (unsigned i = 0; i < sizeof(random_a); i++) CHECK(random_a[i] == 0);
    CHECK(write(device, random_b, sizeof(random_b)) == sizeof(random_b) && lseek(device, 99, SEEK_SET) == 0 && close(device) == 0);
    device = open("/dev/null", O_RDWR); CHECK(device >= 0 && read(device, random_a, sizeof(random_a)) == 0);
    CHECK(syscall(SYS_write, device, (void*)1, 128) == 128 && close(device) == 0);
    CHECK(open("/dev/urandom", O_WRONLY) == -1 && errno == EACCES);
    CHECK(open("/proc/self/ns/user", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    puts("[64] DEVICE PASS: real random streams, null/zero semantics, metadata and device-directory entries");
    check_directory("/bin", "namespaces64"); check_directory("/proc/self/fd", "1");
    char pid_text[16]; snprintf(pid_text, sizeof(pid_text), "%d", parent); check_directory("/proc/self/task", pid_text);
    int proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC); CHECK(proc >= 0);
    struct stat st;
    CHECK(fstatat(proc, "self/exe", &st, 0) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
    CHECK(fstatat(proc, "self/exe", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode));
    CHECK(fstatat(proc, "self/task/", &st, 0) == 0 && st.st_nlink == 3);
    CHECK(pipe(gate) == 0); pthread_t thread; CHECK(pthread_create(&thread, NULL, waiting_thread, NULL) == 0);
    CHECK(fstatat(proc, "self/task/", &st, 0) == 0 && st.st_nlink == 4);
    CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EINVAL);
    CHECK(write(gate[1], "x", 1) == 1 && pthread_join(thread, NULL) == 0 && close(gate[0]) == 0 && close(gate[1]) == 0);
    CHECK(fstatat(proc, "self/task/", &st, 0) == 0 && st.st_nlink == 3);
    int directory = openat(proc, "self/fd", O_RDONLY | O_DIRECTORY); CHECK(directory >= 0);
    char link[128] = {0}; CHECK(readlinkat(directory, "1", link, sizeof(link) - 1) == 12 && !strcmp(link, "/dev/console"));
    CHECK(close(directory) == 0 && close(proc) == 0);
    int inherited = open("/proc/self/uid_map", O_RDONLY); CHECK(inherited >= 0);
    int pair[2], pass = 1;
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0 && setsockopt(pair[0], SOL_SOCKET, SO_PASSCRED, &pass, sizeof(pass)) == 0);
    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO | SA_RESTART}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    pid_t child = new_namespace();
    if (!child) {
        CHECK(close(pair[0]) == 0 && getuid() == 65534 && getgid() == 65534 && capabilities() == ((1ULL << 41) - 1));
        char original[64] = {0}; CHECK(read(inherited, original, sizeof(original) - 1) == 15 && !strcmp(original, "0 0 4294967295\n"));
        CHECK(close(inherited) == 0);
        CHECK(write_text("/proc/self/uid_map", "0 0 1\n") == -1 && errno == EPERM);
        CHECK(write_text("/proc/self/gid_map", "0 1000 1\n") == -1 && errno == EPERM);
        CHECK(write_text("/proc/self/uid_map", "0 1000 0\n") == -1 && errno == EINVAL);
        maps("0 1000 1\n");
        CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
        CHECK(write_text("/proc/self/uid_map", "1 1000 1\n") == -1 && errno == EPERM);
        CHECK(write_text("/proc/self/setgroups", "allow") == -1 && errno == EPERM && setgroups(0, NULL) == -1 && errno == EPERM);
        CHECK(stat("/home/nexus", &st) == 0 && st.st_uid == 0 && st.st_gid == 0);
        CHECK(stat("/etc", &st) == 0 && st.st_uid == 65534 && st.st_gid == 65534);
        CHECK(open("/etc/namespace-forbidden", O_WRONLY | O_CREAT, 0600) == -1 && errno == denied_errno());
        CHECK(open("/proc/1/fd", O_RDONLY | O_DIRECTORY) == -1 && errno == EACCES);
        struct ucred peer; socklen_t peer_size = sizeof(peer);
        CHECK(getsockopt(pair[1], SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0 && peer.uid == 0 && peer.gid == 0 && peer.pid == parent);
        CHECK(kill(parent, SIGUSR1) == 0 && write(pair[1], "mapped", 6) == 6);
        int old_namespace = open("/proc/self/ns/user", O_RDONLY); CHECK(old_namespace >= 0);
        struct stat before, after; CHECK(fstat(old_namespace, &before) == 0);
        drop_capabilities(); CHECK(unshare(CLONE_NEWUSER) == 0 && getuid() == 65534);
        CHECK(fstat(old_namespace, &after) == 0 && before.st_ino == after.st_ino);
        CHECK(stat("/proc/self/ns/user", &after) == 0 && before.st_ino != after.st_ino);
        CHECK(write_text("/proc/self/uid_map", "0 0 1\n") == -1 && errno == EPERM);
        CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EPERM);
        CHECK(close(old_namespace) == 0); _exit(0);
    }
    CHECK(close(pair[1]) == 0 && close(inherited) == 0);
    char payload[16], control[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec vector = {payload, sizeof(payload)};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(pair[0], &message, 0) == 6 && !memcmp(payload, "mapped", 6));
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); CHECK(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS);
    struct ucred sender; memcpy(&sender, CMSG_DATA(c), sizeof(sender));
    CHECK(sender.pid == child && sender.uid == 1000 && sender.gid == 1000);
    siginfo_t info; CHECK(waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0 && info.si_uid == 1000 && info.si_status == 0);
    int status; CHECK(waitpid(child, &status, 0) == child && status == 0 && close(pair[0]) == 0);
    CHECK(signal_uid == 1000 && signal_pid == child && getuid() == 1000 && capabilities() == 0);
    puts("[64] NAMESPACE maps, parent isolation, global IPC/signal IDs and namespace-handle lifetime PASS");
    child = new_namespace();
    if (!child) {
        int fd = open("/proc/self/uid_map", O_WRONLY); CHECK(fd >= 0); drop_capabilities();
        CHECK(write(fd, "1000 1000 1\n", 12) == -1 && errno == EPERM); _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    child = new_namespace();
    if (!child) {
        maps("0 1000 1\n"); CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0); drop_capabilities();
        char* arguments[] = {"/bin/namespaces64", "e", NULL}; char* environment[] = {NULL};
        execve(arguments[0], arguments, environment); CHECK(0);
    }
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    child = new_namespace();
    if (!child) {
        for (unsigned depth = 1; depth <= 32; depth++) {
            maps("1000 1000 1\n"); CHECK(getuid() == 1000);
            if (depth < 32) CHECK(unshare(CLONE_NEWUSER) == 0);
        }
        CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EUSERS); _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    puts("[64] NAMESPACE ALL PASSED: real clone/unshare, procfs maps/task links, capability drops, exec and nesting cleanup");
    return 0;
}
