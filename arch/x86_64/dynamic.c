/* A real dynamically linked GNU libc application, not a syscall-only fixture. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/random.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/vfs.h>
/* FAT32 roots refuse writes outside the user's areas with EROFS; ext4 roots enforce modes (EACCES). */
static int denied_errno(void) { struct statfs s; return statfs("/", &s) == 0 && s.f_type == 0xEF53 ? EACCES : EROFS; }

static _Thread_local unsigned long tls_value = 0x12345678;
int main(int argc, char** argv) {
    if (argc != 2 || strcmp(argv[0], "/bin/dynamic64")) return 1;
    if (!getenv("HOME") || strcmp(getenv("HOME"), "/home/nexus")) return 39;
    if (getauxval(AT_PAGESZ) != 4096 || !getauxval(AT_PHDR) || !getauxval(AT_RANDOM)) return 2;
    if (argv[1][0] == 'd') {
        const char* dir = "/home/nexus/native64-dir";
        if (mkdir(dir, 0700)) return 60;
        if (access(dir, W_OK) || (access("/etc/native64.dat", W_OK) != -1 || errno != denied_errno())) return 84;
        struct stat st;
        if (stat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 1000 || (st.st_mode & 0077)) return 61;
        if (mkdir(dir, 0700) != -1 || errno != EEXIST) return 62;
        if (mkdir("/home/nexus/native64-dir/child", 0700)) return 63;
        if (rmdir(dir) != -1 || errno != ENOTEMPTY) return 64;
        if (mkdir("/home/nexus/../../etc/forbidden", 0700) != -1 || errno != denied_errno()) return 65;
        if (mkdir("/tmp/../bin/forbidden", 0700) != -1 || errno != denied_errno()) return 66;
        if (rmdir("/home/nexus/native64-dir/child") || rmdir(dir)) return 67;
        if (mkdir("/home/nexus/native64-persist", 0700) && errno != EEXIST) return 68;
        if (stat("/home/nexus/native64-persist", &st) || !S_ISDIR(st.st_mode)) return 69;
        const char* file_path = "/home/nexus/native64-persist/data.bin";
        unsigned char data[8193], copy[8193];
        for (unsigned i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i * 19 + 7);
        int previous = open(file_path, O_RDONLY);
        if (previous >= 0) {
            if (read(previous, copy, sizeof(copy)) != sizeof(copy) || copy[1] != 'Z') return 70;
            for (unsigned i = 4099; i < sizeof(copy); i++) if (copy[i]) return 71;
            close(previous); puts("[64] PERSISTED FILE READ PASSED");
        }
        int fd = open(file_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 3 || write(fd, data, sizeof(data)) != sizeof(data) || fsync(fd)) return 72;
        if (open(file_path, O_CREAT | O_EXCL | O_RDWR, 0600) != -1 || errno != EEXIST) return 73;
        if (write(fd, (void*)0x100000, 1) != -1 || errno != EFAULT) return 74;
        unsigned char* mapped = mmap(NULL, 8192, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED || mapped[0] != data[0] || mapped[1] != data[1] || mapped[6000] != data[6000]) return 75;
        if (pwrite(fd, "Z", 1, 1) != 1 || mapped[1] != 'Z' || lseek(fd, 0, SEEK_CUR) != sizeof(data)) return 76;
        if (ftruncate(fd, 4099) || mapped[6000] || ftruncate(fd, sizeof(data))) return 77;
        if (denied_errno() == EACCES) { if (ftruncate(fd, (off_t)1 << 32) || ftruncate(fd, sizeof(data))) return 78; } /* ext4: > 4 GiB files */
        else if (ftruncate(fd, (off_t)1 << 32) != -1 || errno != EFBIG) return 78;
        if (fstat(fd, &st) || st.st_size != sizeof(data) || st.st_mtime < 1577836800) return 79;
        if (munmap(mapped, 8192) || lseek(fd, 0, SEEK_SET) || read(fd, copy, sizeof(copy)) != sizeof(copy)) return 80;
        for (unsigned i = 0; i < sizeof(copy); i++) {
            unsigned char expected = i == 1 ? 'Z' : i < 4099 ? data[i] : 0;
            if (copy[i] != expected) return 81;
        }
        if (fsync(fd) || close(fd)) return 82;
        if (open("/home/nexus/../../etc/native64.dat", O_WRONLY) != -1 || errno != denied_errno()) return 83;
        puts("[64] FILE WRITE API PASSED: create, exclusive, write/pwrite, fsync, truncate and mmap coherence");
        puts("[64] DIRECTORY API PASSED: persistence, nonempty checks and protected paths");
        return 0;
    }
    if (argv[1][0] == 'p' || argv[1][0] == 'u') {
        volatile unsigned char* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return 20;
        page[0] = 1;
        if (argv[1][0] == 'p') { if (mprotect((void*)page, 4096, PROT_READ)) return 21; page[0] = 2; }
        else { if (munmap((void*)page, 4096)) return 22; return page[0]; }
        return 23;
    }
    if (argv[1][0] == 'b') {
        int fd = open("/etc/native64.dat", O_RDONLY);
        if (fd < 0) return 24;
        volatile unsigned char* page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 12288);
        if (page == MAP_FAILED) return 25;
        close(fd); return page[0];
    }
    if (tls_value != 0x12345678) return 3;
    tls_value += 17;
    int futex_word = 0;
    if (syscall(SYS_futex, &futex_word, FUTEX_WAKE_PRIVATE, -1, NULL, NULL, 0) != -1 || errno != EINVAL) return 52;
    if (syscall(SYS_futex, &futex_word, FUTEX_WAIT_PRIVATE, 1, NULL, NULL, 0) != -1 || errno != EAGAIN) return 53;
    if (syscall(SYS_futex, &futex_word, FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0) != -1 || errno != EINVAL) return 54;
    struct timespec before, after, resolution; struct timeval wall;
    if (clock_gettime(CLOCK_MONOTONIC, &before) || clock_getres(CLOCK_MONOTONIC, &resolution) || gettimeofday(&wall, NULL)) return 40;
    if (resolution.tv_nsec < 1 || resolution.tv_nsec > 1000000 || wall.tv_sec < 1577836800) return 41;
    /* Never backwards, and it must advance. A hypervisor's virtual TSC can stand still for a while (VMware
     * on a Hyper-V host traps RDTSC and returns +1 per read while its clock catches up), so keep reading. */
    unsigned spins = 0;
    do {
        for (volatile unsigned i = 0; i < 100000; i++) {}
        if (clock_gettime(CLOCK_MONOTONIC, &after) || after.tv_sec < before.tv_sec ||
            (after.tv_sec == before.tv_sec && after.tv_nsec < before.tv_nsec)) return 42;
    } while (after.tv_sec == before.tv_sec && after.tv_nsec == before.tv_nsec && ++spins < 10000);
    if (spins == 10000) return 42;
    printf("[64] CLOCK realtime=%ld monotonic=%ld.%09ld PASS\n", (long)wall.tv_sec, (long)after.tv_sec, after.tv_nsec);
    char formatted[32]; double number = strtod("2.5", NULL);
    if (snprintf(formatted, sizeof(formatted), "%.2f", number * number) != 4 || strcmp(formatted, "6.25")) return 26;
    printf("[64] DYNAMIC libc/TLS PASS: pid=%ld tls=%lx\n", (long)getpid(), tls_value);
    unsigned char* memory = malloc(3 * 1024 * 1024 + 17);
    if (!memory) return 4;
    memset(memory, 0xA6, 3 * 1024 * 1024 + 17);
    if (memory[3 * 1024 * 1024 + 16] != 0xA6) return 5;
    free(memory);
    memory = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 6;
    memory[4096] = 97;
    if (mprotect(memory, 8192, PROT_READ) || memory[4096] != 97 || munmap(memory, 8192)) return 7;
    puts("[64] DYNAMIC malloc/mmap/mprotect/munmap PASS");
    size_t reserve = (size_t)1 << 40;
    memory = mmap(NULL, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) return 13;
    if (mprotect(memory, 4096, PROT_READ | PROT_WRITE) ||
        mprotect(memory + reserve - 4096, 4096, PROT_READ | PROT_WRITE)) return 14;
    memory[0] = 33; memory[reserve - 1] = 77;
    if (memory[0] != 33 || memory[reserve - 1] != 77 || munmap(memory, reserve)) return 15;
    puts("[64] DYNAMIC sparse 1 TiB reservation PASS");
    for (unsigned i = 0; i < 160; i++) {
        memory = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return 27;
        for (unsigned p = 0; p < 1024 * 1024; p += 4096) { if (memory[p]) return 28; memory[p] = 0xAD; }
        if (munmap(memory, 1024 * 1024)) return 29;
    }
    puts("[64] DYNAMIC page reuse/zeroing PASS");
    memory = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 30;
    memory[0] = 0xA1; memory[4096] = 0xB2;
    if (mmap(memory, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED || errno != EEXIST) return 31;
    if (memory[0] != 0xA1 || memory[4096] != 0xB2) return 32;
    if (mprotect(memory, 8192, PROT_NONE) || mprotect(memory, 8192, PROT_READ | PROT_WRITE) || memory[0] != 0xA1) return 33;
    if (mprotect(memory, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) || mprotect(memory, 4096, PROT_READ | PROT_WRITE)) return 34;
    if (munmap(memory + 4096, 4096)) return 35;
    if (mprotect(memory, 8192, PROT_READ) != -1 || errno != ENOMEM || memory[0] != 0xA1) return 36;
    if (munmap(memory, 8192)) return 37;
    puts("[64] DYNAMIC VM boundary/protection errors PASS");
    memory = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 43;
    memory[0] = 0xD5; memory[4096] = 0x6D;
    if (madvise(memory, 4096, MADV_DONTNEED) || memory[0] || memory[4096] != 0x6D) return 44;
    if (madvise(memory + 4096, 4096, MADV_FREE) || memory[4096] || munmap(memory, 8192)) return 45;
    int mapped_fd = open("/etc/native64.dat", O_RDONLY);
    if (mapped_fd < 0) return 46;
    memory = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, mapped_fd, 0);
    if (memory == MAP_FAILED || memory[0] != 11) return 47;
    memory[0] = 0xC7;
    if (madvise(memory, 4096, MADV_DONTNEED) || memory[0] != 11 || munmap(memory, 4096)) return 48;
    memory = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mapped_fd, 0);
    if (memory == MAP_FAILED || memory[0] != 11) return 49;
    if (mprotect(memory, 4096, PROT_READ | PROT_WRITE) != -1 || errno != EACCES) return 50;
    if (munmap(memory, 4096) || close(mapped_fd)) return 51;
    puts("[64] DYNAMIC madvise anonymous/file discard PASS");
    FILE* file = fopen("/etc/native64.dat", "rb");
    if (!file) return 8;
    unsigned char block[4097];
    if (fread(block, 1, sizeof(block), file) != sizeof(block)) return 9;
    for (unsigned i = 0; i < sizeof(block); i++) if (block[i] != (unsigned char)(i * 37 + 11)) return 10;
    if (fclose(file)) return 11;
    unsigned char random[32];
    if (getrandom(random, sizeof(random), 0) != sizeof(random)) return 12;
    unsigned char second[32];
    if (getrandom(second, sizeof(second), 0) != sizeof(second) || !memcmp(random, second, sizeof(random))) return 38;
    puts("[64] DYNAMIC stdio/getrandom PASS");
    puts("[64] DYNAMIC ALL PASSED");
    return 0;
}
