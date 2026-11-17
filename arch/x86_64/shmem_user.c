#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/memfd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 8
#endif
#ifndef MFD_EXEC
#define MFD_EXEC 16
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 32
#endif
#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] SHMEM FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static int create(unsigned flags) { int fd = syscall(SYS_memfd_create, "nexus-shared", flags); CHECK(fd >= 0); return fd; }
static int reopen(int fd, int flags) { char path[80]; snprintf(path, sizeof(path), "/proc/self/fd/%d", fd); return open(path, flags); }
static void waited(pid_t child, int expected) { int status; CHECK(waitpid(child, &status, 0) == child && status == expected); }
static void sent(int socket, int fd) {
    char byte = 'x', control[CMSG_SPACE(sizeof(fd))] = {0}; struct iovec vector = {&byte, 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd)); CHECK(sendmsg(socket, &message, 0) == 1);
}
static int received(int socket) {
    char byte, control[CMSG_SPACE(sizeof(int))]; struct iovec vector = {&byte, 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    CHECK(recvmsg(socket, &message, 0) == 1 && !(message.msg_flags & MSG_CTRUNC));
    struct cmsghdr* c = CMSG_FIRSTHDR(&message); CHECK(c && c->cmsg_type == SCM_RIGHTS);
    int fd; memcpy(&fd, CMSG_DATA(c), sizeof(fd)); return fd;
}
static void transfer(void) {
    int pair[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(pair[0]); int fd = received(pair[1]);
        unsigned char* data = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(data != MAP_FAILED && data[0] == 73);
        CHECK(close(fd) == 0); data[0] = 99; CHECK(munmap(data, 4096) == 0); close(pair[1]); _exit(0);
    }
    close(pair[1]); int fd = create(MFD_ALLOW_SEALING); CHECK(ftruncate(fd, 4096) == 0);
    unsigned char* data = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(data != MAP_FAILED); data[0] = 73;
    sent(pair[0], fd); CHECK(close(fd) == 0); waited(child, 0); CHECK(data[0] == 99 && munmap(data, 4096) == 0); close(pair[0]);
}
static void future_seals(void) {
    int fd = create(MFD_ALLOW_SEALING); CHECK(ftruncate(fd, 4096) == 0);
    unsigned char* old = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0); CHECK(old != MAP_FAILED);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
    CHECK(mprotect(old, 4096, PROT_READ | PROT_WRITE) == 0); old[0] = 81;
    CHECK(write(fd, "x", 1) == -1 && errno == EPERM);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EPERM);
    unsigned char* next = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0); CHECK(next != MAP_FAILED && next[0] == 81);
    CHECK(mprotect(next, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
    CHECK(munmap(old, 4096) == 0 && fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == 0);
    unsigned char* private = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0); CHECK(private != MAP_FAILED);
    private[0] = 90; CHECK(next[0] == 81 && munmap(private, 4096) == 0 && munmap(next, 4096) == 0);
    CHECK(ftruncate(fd, 8192) == 0 && ftruncate(fd, 4096) == 0);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
    CHECK(ftruncate(fd, 8192) == -1 && errno == EPERM && ftruncate(fd, 0) == -1 && errno == EPERM);
    CHECK(fcntl(fd, F_ADD_SEALS, 0) == -1 && errno == EPERM); close(fd);
    fd = create(0); CHECK(fcntl(fd, F_GET_SEALS) == F_SEAL_SEAL && fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EPERM); close(fd);
    fd = create(MFD_NOEXEC_SEAL); CHECK(fcntl(fd, F_GET_SEALS) == F_SEAL_EXEC);
    CHECK(fchmod(fd, 0777) == -1 && errno == EPERM && fchmod(fd, 0600) == 0);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0); close(fd);
    fd = create(MFD_EXEC | MFD_ALLOW_SEALING); CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_EXEC) == 0 && fcntl(fd, F_GET_SEALS) == 62);
    CHECK(fchmod(fd, 0600) == -1 && errno == EPERM); close(fd);
    fd = create(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
    CHECK(ftruncate(fd, 4096) == 0 && fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
    int ro = reopen(fd, O_RDONLY); CHECK(ro >= 0);
    unsigned char* writable = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char* readonly = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0);
    CHECK(writable != MAP_FAILED && readonly != MAP_FAILED); writable[0] = 38; CHECK(readonly[0] == 38);
    CHECK(pwrite(fd, "xx", 2, 4095) == -1 && errno == EPERM && readonly[4095] == 0);
    CHECK(fchmod(fd, 0400) == 0 && reopen(fd, O_RDONLY | O_TRUNC) == -1 && errno == EACCES);
    CHECK(munmap(writable, 4096) == 0 && munmap(readonly, 4096) == 0); close(ro); close(fd);
}
static volatile unsigned char* truncated_address;
static void truncated_signal(int signal, siginfo_t* info, void* context) {
    (void)context;
    _exit(signal == SIGBUS && info->si_code == BUS_ADRERR && info->si_addr == (void*)truncated_address ? 0 : 97);
}
static void truncated_mapping(void) {
    int fd = create(MFD_ALLOW_SEALING), ready[2], gate[2]; CHECK(ftruncate(fd, 8192) == 0 && pipe(ready) == 0 && pipe(gate) == 0);
    volatile unsigned char* data = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(data != MAP_FAILED);
    data[4096] = 123; pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        struct sigaction action = {.sa_sigaction = truncated_signal, .sa_flags = SA_SIGINFO}; sigemptyset(&action.sa_mask);
        CHECK(sigaction(SIGBUS, &action, NULL) == 0); truncated_address = data + 4096;
        CHECK(data[4096] == 123 && write(ready[1], "r", 1) == 1); char byte; CHECK(read(gate[0], &byte, 1) == 1);
        byte = data[4096]; _exit(byte == 123 ? 11 : 12);
    }
    char byte; CHECK(read(ready[0], &byte, 1) == 1 && ftruncate(fd, 4096) == 0 && write(gate[1], "g", 1) == 1);
    waited(child, 0);
    CHECK(ftruncate(fd, 8192) == 0 && data[4096] == 0);
    data[4095] = 21; CHECK(ftruncate(fd, 17) == 0 && ftruncate(fd, 8192) == 0 && data[4095] == 0 && data[4096] == 0);
    CHECK(munmap((void*)data, 8192) == 0); close(fd); close(ready[0]); close(ready[1]); close(gate[0]); close(gate[1]);
}
int main(int argc, char** argv) {
    (void)argv; CHECK(argc == 2);
    CHECK(syscall(SYS_memfd_create, "x", MFD_EXEC | MFD_NOEXEC_SEAL) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_memfd_create, (void*)1, 0) == -1 && errno == EFAULT);
    char name[251]; memset(name, 'a', 250); name[250] = 0;
    CHECK(syscall(SYS_memfd_create, name, 0) == -1 && errno == EINVAL);
    int fd = create(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL), ro = reopen(fd, O_RDONLY | O_CLOEXEC);
    CHECK(ro >= 0 && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR && (fcntl(ro, F_GETFL) & O_ACCMODE) == O_RDONLY);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC && fcntl(ro, F_GETFD) == FD_CLOEXEC);
    CHECK(ftruncate(ro, 1) == -1 && errno == EINVAL && write(ro, "x", 1) == -1 && errno == EBADF);
    CHECK(fcntl(ro, F_ADD_SEALS, F_SEAL_GROW) == -1 && errno == EPERM);
    struct stat st, other; CHECK(fstat(fd, &st) == 0 && st.st_size == 0 && st.st_blocks == 0 && !st.st_nlink && (st.st_mode & 0111) == 0);
    CHECK(fstat(ro, &other) == 0 && other.st_ino == st.st_ino && other.st_dev == st.st_dev);
    CHECK(ftruncate(fd, 1ULL << 40) == 0 && fstat(fd, &st) == 0 && st.st_blocks == 0);
    unsigned char* sparse = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (1ULL << 40) - 4096); CHECK(sparse != MAP_FAILED);
    sparse[0] = 31; CHECK(munmap(sparse, 4096) == 0 && fstat(fd, &st) == 0 && st.st_blocks == 8);
    CHECK(ftruncate(fd, 12288) == 0 && fstat(fd, &st) == 0 && st.st_blocks == 0);
    uint64_t packed = (uint64_t)(unsigned)ro << 32 | (unsigned)fd;
    void* abi = (void*)syscall(SYS_mmap, NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, packed, 0);
    CHECK(abi != MAP_FAILED && munmap(abi, 4096) == 0);
    CHECK(syscall(SYS_fstat, packed, &other) == 0 && other.st_ino == st.st_ino);
    CHECK(syscall(SYS_read, 0x7FFFFFFFULL << 32 | 0xFFFFFFFFU, name, 1) == -1 && errno == EBADF);
    CHECK(chdir("/etc") == 0);
    int at = syscall(SYS_openat, 0x12340000FFFFFF9CULL, "native64.dat", O_RDONLY, 0); CHECK(at >= 0); close(at); CHECK(chdir("/") == 0);
    unsigned char* a = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char* b = mmap(NULL, 12288, PROT_READ, MAP_SHARED, ro, 0);
    unsigned char* private = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_PRIVATE, ro, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED && private != MAP_FAILED && a[0] == 0);
    a[0] = 17; CHECK(b[0] == 17 && private[0] == 17); private[0] = 29;
    CHECK(a[0] == 17 && pwrite(fd, "Z", 1, 0) == 1 && a[0] == 'Z' && b[0] == 'Z' && private[0] == 29);
    CHECK(madvise(private, 4096, MADV_DONTNEED) == 0 && private[0] == 'Z');
    CHECK(mprotect(b, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    pid_t child = fork(); CHECK(child >= 0); if (!child) { *(volatile unsigned char*)b = 8; _exit(98); } waited(child, SIGSEGV);
    CHECK(mprotect(a + 4096, 4096, PROT_READ) == 0 && munmap(a + 4096, 4096) == 0);
    CHECK(close(fd) == 0 && close(ro) == 0 && a[0] == 'Z'); a[8192] = 51; CHECK(b[8192] == 51);
    CHECK(munmap(a, 4096) == 0 && munmap(a + 8192, 4096) == 0 && munmap(b, 12288) == 0 && munmap(private, 12288) == 0);
    volatile uint32_t* shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); CHECK(shared != MAP_FAILED);
    child = fork(); CHECK(child >= 0);
    if (!child) { *shared = 1234; CHECK(syscall(SYS_futex, shared, FUTEX_WAKE, 1, NULL, NULL, 0) >= 0); _exit(0); }
    while (!*shared) { long result = syscall(SYS_futex, shared, FUTEX_WAIT, 0, NULL, NULL, 0); CHECK(result == 0 || (result == -1 && errno == EAGAIN)); }
    waited(child, 0); CHECK(*shared == 1234 && munmap((void*)shared, 4096) == 0);
    transfer(); future_seals(); truncated_mapping();
    puts("[64] SHMEM ALL PASSED: sparse memfd, coherent shared pages, COW, seals, SCM_RIGHTS, futex, mapping lifetime and truncate faults");
    return 0;
}
