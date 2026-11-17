#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] JIT FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static void waited(pid_t child, int expected) { int status; CHECK(waitpid(child, &status, 0) == child && status == expected); }
int main(int argc, char** argv) {
    (void)argv; CHECK(argc == 2);
    size_t size = 512ULL * 1024 * 1024;
    unsigned char* code = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(code != MAP_FAILED && mprotect(code, 4096, PROT_READ | PROT_WRITE) == 0);
    const unsigned char stub[] = {0xB8, 42, 0, 0, 0, 0xC3}; memcpy(code, stub, sizeof(stub));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { ((int (*)(void))code)(); _exit(98); }
    waited(child, SIGSEGV);
    CHECK(mprotect(code, size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 && madvise(code, size, MADV_DONTNEED) == 0);
    memcpy(code, stub, sizeof(stub)); CHECK(((int (*)(void))code)() == 42);
    child = fork(); CHECK(child >= 0);
    if (!child) { code[1] = 73; CHECK(((int (*)(void))code)() == 73); _exit(0); }
    waited(child, 0); CHECK(((int (*)(void))code)() == 42);
    CHECK(mprotect(code, 4096, PROT_READ | PROT_EXEC) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) { *(volatile unsigned char*)(code + 1) = 11; _exit(98); }
    waited(child, SIGSEGV); CHECK(((int (*)(void))code)() == 42);
    CHECK(mprotect((void*)0x100000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == -1 && errno == ENOMEM);
    CHECK(mmap((void*)0xFFFFFFFF80000000ULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap((void*)0x1000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EPERM);
    CHECK(mprotect(code, 4096, 15) == -1 && errno == EINVAL);
    int fd = open("/etc/native64.dat", O_RDONLY); CHECK(fd >= 0);
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES);
    CHECK(close(fd) == 0 && munmap(code, size) == 0);
    code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(code != MAP_FAILED);
    memcpy(code, stub, sizeof(stub)); CHECK(((int (*)(void))code)() == 42 && munmap(code, 4096) == 0);
    puts("[64] JIT ALL PASSED: explicit user RWX, V8-sized reserve/commit/discard, default NX, COW and kernel/file protection");
    return 0;
}
