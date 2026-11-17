/* Disk waits without the big kernel lock (tests/test_x64_blockio.py boots this as /sbin/init, as root, on 2+ CPUs).
 * A syscall-heavy CPU-bound child (getppid/clock_gettime system calls and pipe round trips with a helper) measures
 * its rate alone, then while an I/O child does synchronous disk I/O (write + fsync of 4 MiB blocks, then cold reads
 * after drop_caches) and the I/O rate is measured too. The run prints both rates; the harness compares kernels.
 * Lines "[64] BLOCKIO ..."; "[64] BLOCKIO FAIL line=N" on errors. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { dprintf(2, "[64] BLOCKIO FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); _exit(99); } } while (0)
static double now(void) { struct timespec t; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* Syscall loop for `seconds`: real system calls (no vDSO) and pipe round trips through a helper process. */
static double syscall_rate(double seconds) {
    int to[2], from[2]; CHECK(pipe(to) == 0 && pipe(from) == 0);
    pid_t echo = fork(); CHECK(echo >= 0);
    if (!echo) { char c; close(to[1]); close(from[0]); while (read(to[0], &c, 1) == 1) if (write(from[1], &c, 1) != 1) break; _exit(0); }
    close(to[0]); close(from[1]);
    uint64_t calls = 0; double start = now(), end = start + seconds;
    while (now() < end) {
        for (int i = 0; i < 200; i++) { syscall(SYS_getppid); struct timespec t; syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t); calls += 2; }
        char c = 'x'; CHECK(write(to[1], &c, 1) == 1 && read(from[0], &c, 1) == 1); calls += 4;
    }
    double elapsed = now() - start;
    close(to[1]); close(from[0]); waitpid(echo, NULL, 0);
    return calls / elapsed;
}
static void io_loop(int report) {   /* reports the running MiB/s after every 4 MiB transferred */
    static char block[4 << 20];
    for (unsigned i = 0; i < sizeof(block); i++) block[i] = (char)(i * 31 + 7);
    uint64_t bytes = 0; double start = now();
    for (;;) {
        int fd = open("/tmp/blockio.dat", O_RDWR | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0);
        for (int k = 0; k < 16; k++) {
            CHECK(write(fd, block, sizeof(block)) == (ssize_t)sizeof(block) && fsync(fd) == 0); bytes += sizeof(block);
            double rate = bytes / (now() - start) / 1048576.0; CHECK(write(report, &rate, sizeof(rate)) == sizeof(rate));
        }
        int drop = open("/proc/sys/vm/drop_caches", O_WRONLY); if (drop >= 0) { CHECK(write(drop, "1", 1) == 1); close(drop); }
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        ssize_t got; while ((got = read(fd, block, sizeof(block))) > 0) {
            bytes += (uint64_t)got;
            double rate = bytes / (now() - start) / 1048576.0; CHECK(write(report, &rate, sizeof(rate)) == sizeof(rate));
        }
        CHECK(got == 0 && close(fd) == 0);
    }
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    double alone = syscall_rate(3.0);
    printf("[64] BLOCKIO syscall rate alone: %.0f calls/s (%ld CPUs)\n", alone, cpus);
    int rates[2]; CHECK(pipe(rates) == 0);
    pid_t io = fork(); CHECK(io >= 0);
    if (!io) { close(rates[0]); io_loop(rates[1]); _exit(0); }
    close(rates[1]);
    double warm = now(); while (now() - warm < 1.0) { struct timespec t = {0, 50000000}; nanosleep(&t, NULL); }   /* let the I/O start */
    double busy = syscall_rate(6.0);
    kill(io, SIGKILL); int status; CHECK(waitpid(io, &status, 0) == io);
    double rate = 0, r; int samples = 0; while (read(rates[0], &r, sizeof(r)) == sizeof(r)) { rate = r; samples++; }
    CHECK(samples > 0);
    printf("[64] BLOCKIO syscall rate during disk I/O: %.0f calls/s (%.0f%% of alone), disk I/O %.1f MiB/s\n", busy, 100.0 * busy / alone, rate);
    printf("[64] BLOCKIO RESULT ratio=%.3f io_mib_s=%.1f\n", busy / alone, rate);
    unlink("/tmp/blockio.dat");
    printf("[64] BLOCKIO DONE\n");
    return 0;
}
