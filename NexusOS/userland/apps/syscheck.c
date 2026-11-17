#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <nexus.h>

static void float_process(double seed) {
    double actual;
    __asm__ volatile("fldl %0" :: "m"(seed));
    for (int i = 0; i < 1000; i++) {
        int result;
        __asm__ volatile("int $0x80" : "=a"(result) : "a"(7) : "memory", "cc");
        __asm__ volatile("fld1; faddp");
    }
    __asm__ volatile("fstpl %0" : "=m"(actual));
    assert(actual == seed + 1000);
}
int main(int argc, char **argv) {
    if (argc > 1) {
        if (argc > 2) assert(fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF);
        float_process(atoi(argv[1])); return 23;
    }
    unlink("/disk/check.ok");
    assert(fabs(sin(1.0) - 0.8414709848078965) < 1e-14);
    assert(pow(2.0, 10.0) == 1024.0 && sqrt(81.0) == 9.0);
    char number[64]; snprintf(number, sizeof(number), "%lld %.3f", 1234567890123LL, 1.25);
    assert(strcmp(number, "1234567890123 1.250") == 0);
    char *p = malloc(256 * 1024); assert(p); memset(p, 0x5a, 256 * 1024);
    p = realloc(p, 512 * 1024); assert(p && p[65535] == 0x5a); free(p);
    jmp_buf jump;
    if (!setjmp(jump)) longjmp(jump, 3);
    puts("PASS math, printf, allocation, setjmp");
    FILE *f = fopen("/disk/check.tmp", "w+"); assert(f);
    for (int i = 0; i < 100; i++) assert(fprintf(f, "%04d\n", i) == 5);
    assert(fflush(f) == 0 && fseek(f, 45, SEEK_SET) == 0);
    char buf[8]; assert(fgets(buf, sizeof(buf), f) && strcmp(buf, "0009\n") == 0);
    assert(fclose(f) == 0);
    struct stat st; assert(stat("/disk/check.tmp", &st) == 0 && st.st_size == 500);
    int fd = open("/disk/check.tmp", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd == -1 && errno == EEXIST);
    fd = open("/disk/check.tmp", O_RDONLY); assert(fd >= 0);
    assert(read(fd, (void *)1, 1) == -1 && errno == EFAULT); close(fd);
    assert(unlink("/disk/check.tmp") == 0);
    DIR *d = opendir("/disk/bin"); assert(d && readdir(d)); closedir(d);
    puts("PASS FAT files, seek, stat, exclusive create, bad pointer, directories");
    int fds[2]; assert(pipe(fds) == 0);
    assert(write(fds[1], "pipe", 4) == 4); close(fds[1]);
    assert(read(fds[0], buf, sizeof(buf)) == 4 && !memcmp(buf, "pipe", 4));
    assert(read(fds[0], buf, sizeof(buf)) == 0); close(fds[0]);
    fd = open("/dev/null", O_RDONLY); assert(fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    assert(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    char fdtext[12]; snprintf(fdtext, sizeof(fdtext), "%d", fd);
    char *a[] = {"syscheck", "1", fdtext, NULL}, *b[] = {"syscheck", "100", fdtext, NULL};
    int x = spawn("/disk/bin/syscheck", a, NULL), y = spawn("/disk/bin/syscheck", b, NULL);
    assert(x > 0 && y > 0);
    close(fd);
    int status; assert(waitpid(x, &status, 0) == x && WIFEXITED(status) && WEXITSTATUS(status) == 23);
    assert(waitpid(y, &status, 0) == y && WIFEXITED(status) && WEXITSTATUS(status) == 23);
    puts("PASS pipes, spawn/wait, concurrent x87 state");
    puts("SYSCHECK ALL PASSED");
    f = fopen("/disk/check.ok", "w"); assert(f);
    assert(fputs("SYSCHECK ALL PASSED\n", f) >= 0 && fclose(f) == 0);
    return 0;
}
