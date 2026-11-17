#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] PIPE FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static unsigned char source[200000], output[200000];
static volatile sig_atomic_t broken;
static int shared_pipe[2];
static void handler(int number) { if (number == SIGPIPE) broken++; }
static void* writer(void* value) {
    unsigned char a[1024], b[1024]; memset(a, (int)(uintptr_t)value, sizeof(a)); memcpy(b, a, sizeof(b));
    struct iovec vectors[] = {{a, sizeof(a)}, {b, sizeof(b)}};
    for (int i = 0; i < 100; i++) CHECK(writev(shared_pipe[1], vectors, 2) == 2048);
    return NULL;
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    int pair[2];
    if (argv[1][0] == 'p') {
        CHECK(pipe(pair) == 0 && close(pair[0]) == 0); write(pair[1], "x", 1); CHECK(0);
    }
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i * 31 + 5);
    pair[0] = 123; pair[1] = 456;
    CHECK(pipe2(pair, 0x100) == -1 && errno == EINVAL && pair[0] == 123 && pair[1] == 456);
    CHECK(syscall(SYS_pipe2, (void*)1, 0) == -1 && errno == EFAULT);
    CHECK(pipe2(pair, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(fcntl(pair[0], F_GETFD) == FD_CLOEXEC && fcntl(pair[1], F_GETFD) == FD_CLOEXEC);
    CHECK((fcntl(pair[0], F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDONLY | O_NONBLOCK));
    CHECK((fcntl(pair[1], F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_WRONLY | O_NONBLOCK));
    struct stat read_stat, write_stat;
    CHECK(fstat(pair[0], &read_stat) == 0 && fstat(pair[1], &write_stat) == 0 && S_ISFIFO(read_stat.st_mode));
    CHECK(read_stat.st_ino == write_stat.st_ino && read_stat.st_dev == write_stat.st_dev);
    CHECK(lseek(pair[0], 0, SEEK_SET) == -1 && errno == ESPIPE);
    CHECK(read(pair[1], output, 1) == -1 && errno == EBADF && write(pair[0], source, 1) == -1 && errno == EBADF);
    CHECK(read(pair[0], output, 1) == -1 && errno == EAGAIN);
    CHECK(write(pair[1], NULL, 0) == 0 && read(pair[0], NULL, 0) == 0);
    CHECK(write(pair[1], source, 50000) == 50000 && read(pair[0], output, 45000) == 45000 && !memcmp(source, output, 45000));
    CHECK(write(pair[1], source + 50000, 30000) == 30000);
    int available;
    CHECK(ioctl(pair[0], FIONREAD, &available) == 0 && available == 35000);
    CHECK(fcntl(pair[0], F_SETPIPE_SZ, 4096) == -1 && errno == EBUSY);
    CHECK(fcntl(pair[1], F_SETPIPE_SZ, 100000) == 131072 && fcntl(pair[0], F_GETPIPE_SZ) == 131072);
    struct iovec read_vectors[] = {{output, 17000}, {output + 17000, 18000}};
    CHECK(readv(pair[0], read_vectors, 2) == 35000 && !memcmp(source + 45000, output, 35000));
    CHECK(fcntl(pair[1], F_SETPIPE_SZ, 4096) == 4096);
    CHECK(write(pair[1], source, 4096) == 4096 && write(pair[1], source, 1) == -1 && errno == EAGAIN);
    CHECK(read(pair[0], output, 1) == 1 && write(pair[1], source, 2) == -1 && errno == EAGAIN);
    CHECK(read(pair[0], output + 1, 4095) == 4095 && !memcmp(source, output, 4096));
    CHECK(write(pair[1], source, 5000) == 4096 && read(pair[0], output, 5000) == 4096 && !memcmp(source, output, 4096));
    struct iovec bad[] = {{source, 10}, {(void*)1, 10}};
    CHECK(writev(pair[1], bad, 2) == -1 && errno == EFAULT && ioctl(pair[0], FIONREAD, &available) == 0 && available == 0);
    CHECK(write(pair[1], source, 20) == 20);
    bad[0].iov_base = output;
    CHECK(readv(pair[0], bad, 2) == -1 && errno == EFAULT && ioctl(pair[0], FIONREAD, &available) == 0 && available == 20);
    CHECK(read(pair[0], output, 20) == 20 && !memcmp(source, output, 20));
    int copy = dup(pair[1]); CHECK(copy >= 0 && close(pair[1]) == 0);
    CHECK(read(pair[0], output, 1) == -1 && errno == EAGAIN);
    CHECK(close(copy) == 0 && read(pair[0], output, 1) == 0 && close(pair[0]) == 0);
    CHECK(pipe(pair) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(close(pair[1]) == 0); size_t total = 0;
        for (;;) {
            ssize_t bytes = read(pair[0], output, 1021); CHECK(bytes >= 0);
            if (!bytes) break;
            CHECK(total + bytes <= sizeof(source) && !memcmp(source + total, output, bytes)); total += bytes;
        }
        CHECK(total == sizeof(source) && close(pair[0]) == 0); _exit(0);
    }
    CHECK(close(pair[0]) == 0 && write(pair[1], source, sizeof(source)) == sizeof(source) && close(pair[1]) == 0);
    int status; CHECK(waitpid(child, &status, 0) == child && status == 0);
    CHECK(pipe(shared_pipe) == 0 && fcntl(shared_pipe[0], F_SETPIPE_SZ, 4096) == 4096);
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, writer, (void*)'A') == 0 && pthread_create(&threads[1], NULL, writer, (void*)'B') == 0);
    int counts[2] = {0};
    for (int i = 0; i < 200; i++) {
        CHECK(read(shared_pipe[0], output, 2048) == 2048 && (output[0] == 'A' || output[0] == 'B'));
        for (unsigned j = 1; j < 2048; j++) CHECK(output[j] == output[0]);
        counts[output[0] - 'A']++;
    }
    CHECK(counts[0] == 100 && counts[1] == 100 && pthread_join(threads[0], NULL) == 0 && pthread_join(threads[1], NULL) == 0);
    CHECK(close(shared_pipe[0]) == 0 && close(shared_pipe[1]) == 0);
    struct sigaction action = {.sa_handler = handler}; sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGPIPE, &action, NULL) == 0 && pipe(pair) == 0 && close(pair[0]) == 0);
    CHECK(write(pair[1], source, 1) == -1 && errno == EPIPE && broken == 1 && close(pair[1]) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    struct iovec record[] = {{source, 123}, {source + 123, 321}};
    CHECK(writev(pair[0], record, 2) == 444);
    record[0] = (struct iovec){output, 200}; record[1] = (struct iovec){output + 200, 300};
    CHECK(readv(pair[1], record, 2) == 444 && !memcmp(source, output, 444));
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    puts("[64] PIPE ALL PASSED: buffering/resize, nonblocking/EOF, fork transfer, atomic concurrent writev, SIGPIPE and vector validation");
    return 0;
}
