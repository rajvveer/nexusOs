/* mremap, fallocate, sendfile, copy_file_range, statx, link, chmod/chown (vfat), truncate,
 * name_to_handle_at and MADV_REMOVE checks (run as /bin/sysext64 by the apps campaign). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] SYSEXT FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define HOME "/home/nexus/sysext-test"
static void fill(char* p, size_t n, int seed) { for (size_t i = 0; i < n; i++) p[i] = (char)(i * 7 + seed); }
static int same(const char* p, size_t n, int seed) { for (size_t i = 0; i < n; i++) if (p[i] != (char)(i * 7 + seed)) return 0; return 1; }
static int same_at(const char* base, size_t start, size_t n, int seed) { for (size_t i = start; i < start + n; i++) if (base[i] != (char)(i * 7 + seed)) return 0; return 1; }
static int zero(const char* p, size_t n) { for (size_t i = 0; i < n; i++) if (p[i]) return 0; return 1; }
static int wstatus_epoll;
static ssize_t slurp(const char* path, char* buffer, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    size_t used = 0; ssize_t got;
    while (used + 1 < size && (got = read(fd, buffer + used, size - used - 1)) > 0) used += (size_t)got;
    close(fd); buffer[used] = 0; return (ssize_t)used;
}
static int listed(const char* directory, const char* name) {
    DIR* d = opendir(directory); if (!d) return 0; struct dirent* e; int found = 0;
    while ((e = readdir(d))) if (!strcmp(e->d_name, name)) found = 1;
    closedir(d); return found;
}
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--script-args")) /* interpreter of the #! scripts below */
        return argc == 5 && !strcmp(argv[2], HOME "/inner") && !strcmp(argv[3], "one") && !strcmp(argv[4], "two") ? 42 :
               argc == 6 && !strcmp(argv[2], HOME "/inner") && !strcmp(argv[3], "spaced option") && !strcmp(argv[4], HOME "/outer") && !strcmp(argv[5], "x") ? 43 : 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid() == 0) { CHECK(setresgid(1000, 1000, 1000) == 0 && setresuid(1000, 1000, 1000) == 0); }
    long page = sysconf(_SC_PAGESIZE);
    /* ---- mremap ---- */
    char* a = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(a != MAP_FAILED);
    fill(a, 4 * page, 1);
    CHECK(mremap(a, 4 * page, 2 * page, 0) == a && same(a, 2 * page, 1));                                /* shrink */
    char* guard = mmap(a + 2 * page, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(guard == a + 2 * page);
    CHECK(mremap(a, 2 * page, 8 * page, 0) == MAP_FAILED && errno == ENOMEM);                          /* blocked in place */
    char* b = mremap(a, 2 * page, 8 * page, MREMAP_MAYMOVE); CHECK(b != MAP_FAILED && b != a);           /* moved */
    CHECK(same(b, 2 * page, 1) && zero(b + 2 * page, 6 * page));
    b[8 * page - 1] = 5; CHECK(munmap(guard, page) == 0);
    char* fixed = mmap(NULL, 16 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(fixed != MAP_FAILED);
    CHECK(mremap(b, 8 * page, 8 * page, MREMAP_MAYMOVE | MREMAP_FIXED, fixed + 4 * page) == fixed + 4 * page);
    CHECK(same(fixed + 4 * page, 2 * page, 1) && fixed[12 * page - 1] == 5);
    CHECK(mremap(fixed + 4 * page, 8 * page, 9 * page, MREMAP_MAYMOVE) != MAP_FAILED);                  /* grows into PROT_NONE? no: moves */
    CHECK(mremap((void*)0x1001, page, page, 0) == MAP_FAILED && errno == EINVAL);
    CHECK(mremap(a + 64 * page, page, 2 * page, MREMAP_FIXED, NULL) == MAP_FAILED && errno == EINVAL);
    char* big = mmap(NULL, 64 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(big != MAP_FAILED);
    fill(big, 64 * page, 3);
    char* grown = mremap(big, 64 * page, 1024 * page, MREMAP_MAYMOVE); CHECK(grown != MAP_FAILED && same(grown, 64 * page, 3));
    grown[1024 * page - 1] = 9; CHECK(munmap(grown, 1024 * page) == 0);
    /* shared anonymous: duplicate with old_size 0, then DONTUNMAP keeps the source mapped and empty */
    char* shared = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); CHECK(shared != MAP_FAILED);
    char* alias = mremap(shared, 0, 2 * page, MREMAP_MAYMOVE); CHECK(alias != MAP_FAILED && alias != shared);
    shared[10] = 42; CHECK(alias[10] == 42);
    char* priv = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); priv[0] = 7;
    char* kept = mremap(priv, page, page, MREMAP_MAYMOVE | MREMAP_DONTUNMAP); CHECK(kept != MAP_FAILED && kept[0] == 7 && priv[0] == 0);
    /* ---- MADV_REMOVE and fallocate on shared memory ---- */
    CHECK(madvise(shared, page, MADV_REMOVE) == 0 && alias[10] == 0);
    CHECK(madvise(priv, page, MADV_REMOVE) == -1 && errno == EINVAL);
    int memory = memfd_create("sysext", MFD_CLOEXEC); CHECK(memory >= 0);
    struct stat st; CHECK(fallocate(memory, 0, 0, 3 * page) == 0 && fstat(memory, &st) == 0 && st.st_size == 3 * page);
    CHECK(fallocate(memory, FALLOC_FL_KEEP_SIZE, 0, 5 * page) == 0 && fstat(memory, &st) == 0 && st.st_size == 3 * page);
    char* view = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0); CHECK(view != MAP_FAILED);
    fill(view, 3 * page, 4);
    CHECK(fallocate(memory, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, page / 2, 2 * page) == 0);
    CHECK(same_at(view, 0, page / 2, 4) && zero(view + page / 2, 2 * page) && same_at(view, 2 * page + page / 2, page / 2, 4));
    CHECK(fallocate(memory, FALLOC_FL_PUNCH_HOLE, 0, page) == -1 && errno == EINVAL);
    CHECK(fallocate(memory, FALLOC_FL_COLLAPSE_RANGE, 0, page) == -1 && errno == EOPNOTSUPP);
    CHECK(fallocate(memory, 0, -1, page) == -1 && errno == EINVAL);
    int pipes[2]; CHECK(pipe(pipes) == 0 && fallocate(pipes[1], 0, 0, page) == -1 && errno == ESPIPE);
    /* ---- FAT files: fallocate, truncate, sendfile, copy_file_range ---- */
    mkdir(HOME, 0700); unlink(HOME "/one"); unlink(HOME "/two"); unlink(HOME "/link");
    int one = open(HOME "/one", O_CREAT | O_RDWR | O_TRUNC, 0600); CHECK(one >= 0);
    CHECK(fallocate(one, 0, 0, 10000) == 0 && fstat(one, &st) == 0 && st.st_size == 10000);
    CHECK(fallocate(one, 0, 0, 100) == 0 && fstat(one, &st) == 0 && st.st_size == 10000);
    CHECK(fallocate(one, FALLOC_FL_KEEP_SIZE, 0, 20000) == -1 && errno == EOPNOTSUPP);
    CHECK(fallocate(one, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 100) == -1 && errno == EOPNOTSUPP);
    static char data[200000], back[200000];
    fill(data, sizeof(data), 6); CHECK(pwrite(one, data, sizeof(data), 0) == (ssize_t)sizeof(data));
    CHECK(truncate(HOME "/one", 150000) == 0 && stat(HOME "/one", &st) == 0 && st.st_size == 150000);
    int two = open(HOME "/two", O_CREAT | O_RDWR | O_TRUNC, 0600); CHECK(two >= 0);
    off_t offset = 1000;
    CHECK(sendfile(two, one, &offset, 120000) == 120000 && offset == 121000 && lseek(one, 0, SEEK_CUR) == 0);
    CHECK(pread(two, back, 120000, 0) == 120000 && !memcmp(back, data + 1000, 120000));
    CHECK(lseek(one, 140000, SEEK_SET) == 140000 && sendfile(two, one, NULL, 50000) == 10000 && lseek(one, 0, SEEK_CUR) == 150000);
    CHECK(sendfile(pipes[1], one, &offset, 100) == 100 && read(pipes[0], back, 100) == 100 && !memcmp(back, data + 121000, 100));
    CHECK(sendfile(two, pipes[0], NULL, 10) == -1 && errno == EINVAL);
    loff_t from = 10, to = 5;
    CHECK(copy_file_range(one, &from, two, &to, 5000, 0) == 5000 && from == 5010 && to == 5005);
    CHECK(pread(two, back, 5000, 5) == 5000 && !memcmp(back, data + 10, 5000));
    CHECK(copy_file_range(one, NULL, two, NULL, 10, 1) == -1 && errno == EINVAL);
    from = 0; to = 100; CHECK(copy_file_range(one, &from, one, &to, 1000, 0) == -1 && errno == EINVAL);  /* overlap */
    /* ---- statx agrees with stat ---- */
    struct statx x; CHECK(statx(AT_FDCWD, HOME "/one", 0, STATX_BASIC_STATS, &x) == 0 && stat(HOME "/one", &st) == 0);
    CHECK((x.stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS && x.stx_ino == st.st_ino && x.stx_size == (uint64_t)st.st_size);
    CHECK(x.stx_mode == st.st_mode && makedev(x.stx_dev_major, x.stx_dev_minor) == st.st_dev && x.stx_mtime.tv_sec == st.st_mtime);
    CHECK(statx(one, "", AT_EMPTY_PATH, STATX_ALL, &x) == 0 && x.stx_ino == st.st_ino);
    CHECK(statx(AT_FDCWD, "/dev/null", 0, STATX_BASIC_STATS, &x) == 0 && x.stx_rdev_major == 1 && x.stx_rdev_minor == 3 && S_ISCHR(x.stx_mode));
    CHECK(statx(AT_FDCWD, "/proc/self", AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &x) == 0 && S_ISLNK(x.stx_mode));
    CHECK(statx(AT_FDCWD, HOME "/missing", 0, STATX_BASIC_STATS, &x) == -1 && errno == ENOENT);
    CHECK(statx(AT_FDCWD, HOME "/one", 0x80000000, STATX_BASIC_STATS, &x) == -1 && errno == EINVAL);
    /* ---- link: FAT has no hard links (vfat EPERM), after the usual checks ---- */
    CHECK(link(HOME "/one", HOME "/two") == -1 && errno == EEXIST);
    CHECK(link(HOME "/missing", HOME "/link") == -1 && errno == ENOENT);
    CHECK(link(HOME "/one", HOME "/link") == -1 && errno == EPERM);
    CHECK(linkat(AT_FDCWD, HOME "/one", AT_FDCWD, HOME "/link", 0x8000) == -1 && errno == EINVAL);
    CHECK(link(HOME "/one", "/etc/new-link") == -1 && errno == EROFS);
    /* ---- chmod/chown with vfat semantics ---- */
    CHECK(chmod(HOME "/one", 0500) == 0 && stat(HOME "/one", &st) == 0 && (st.st_mode & 0777) == 0500);
    CHECK(open(HOME "/one", O_WRONLY) == -1 && errno == EACCES);
    CHECK(fchmod(two, 0700) == 0 && chmod(HOME "/one", 0700) == 0 && stat(HOME "/one", &st) == 0 && (st.st_mode & 0777) == 0700);
    int again = open(HOME "/one", O_WRONLY); CHECK(again >= 0 && close(again) == 0);
    CHECK(chmod(HOME "/one", 0600) == 0 && stat(HOME "/one", &st) == 0 && (st.st_mode & 0777) == 0700);   /* not representable: ignored */
    CHECK(chmod(HOME "/one", 04700) == -1 && errno == EPERM);
    CHECK(symlink("one", HOME "/link") == 0 && fchmodat(AT_FDCWD, HOME "/link", 0700, AT_SYMLINK_NOFOLLOW) == -1 && errno == EOPNOTSUPP);
    CHECK(fchmodat(AT_FDCWD, HOME "/one", 0700, AT_SYMLINK_NOFOLLOW) == 0 && unlink(HOME "/link") == 0);
    CHECK(chmod("/etc/hosts", 0644) == -1 && errno == EPERM);
    CHECK(chown(HOME "/one", getuid(), getgid()) == 0 && fchown(one, -1, -1) == 0);
    CHECK(chown(HOME "/one", 0, 0) == -1 && errno == EPERM && lchown(HOME "/one", 1001, -1) == -1 && errno == EPERM);
    CHECK(fchmod(memory, 0600) == 0 && fstat(memory, &st) == 0 && (st.st_mode & 0777) == 0600);
    /* ---- name_to_handle_at ---- */
    struct file_handle* handle = malloc(sizeof(*handle) + 128); int mount = -1;
    handle->handle_bytes = 0;
    CHECK(name_to_handle_at(AT_FDCWD, HOME "/one", handle, &mount, 0) == -1 && errno == EOVERFLOW && handle->handle_bytes > 0 && mount > 0);
    CHECK(name_to_handle_at(AT_FDCWD, HOME "/one", handle, &mount, 0) == 0 && handle->handle_type != 0);
    unsigned char first[128]; memcpy(first, handle->f_handle, handle->handle_bytes); unsigned bytes = handle->handle_bytes;
    CHECK(name_to_handle_at(one, "", handle, &mount, AT_EMPTY_PATH) == 0 && handle->handle_bytes == bytes && !memcmp(first, handle->f_handle, bytes));
    CHECK(name_to_handle_at(AT_FDCWD, "/proc/cpuinfo", handle, &mount, 0) == -1 && errno == EOPNOTSUPP);
    /* ---- procfs files applications read ---- */
    static char text[65536]; char expect[64];
    CHECK(slurp("/proc/self/status", text, sizeof(text)) > 0 && strstr(text, "Name:\tsysext64\n") && strstr(text, "State:\tR (running)"));
    snprintf(expect, sizeof(expect), "\nPid:\t%d\n", getpid()); CHECK(strstr(text, expect) && strstr(text, "\nThreads:\t1\n") && strstr(text, "VmRSS:"));
    CHECK(slurp("/proc/self/stat", text, sizeof(text)) > 0);
    snprintf(expect, sizeof(expect), "%d (sysext64) R %d ", getpid(), getppid()); CHECK(!strncmp(text, expect, strlen(expect)));
    CHECK(slurp("/proc/self/statm", text, sizeof(text)) > 0 && atol(text) > 0);
    ssize_t length = slurp("/proc/self/cmdline", text, sizeof(text)); CHECK(length > 0 && !strcmp(text, argv[0]) && text[length - 1] == 0);
    CHECK(slurp("/proc/self/maps", text, sizeof(text)) > 0 && strstr(text, "[stack]") && strstr(text, "/bin/sysext64") && strstr(text, "r-xp"));
    pthread_attr_t attributes; void* stack_base; size_t stack_size;
    CHECK(pthread_getattr_np(pthread_self(), &attributes) == 0 && pthread_attr_getstack(&attributes, &stack_base, &stack_size) == 0);
    CHECK((char*)stack_base < (char*)&attributes && (char*)&attributes < (char*)stack_base + stack_size);
    CHECK(slurp("/proc/self/comm", text, sizeof(text)) > 0 && !strcmp(text, "sysext64\n"));
    snprintf(expect, sizeof(expect), "/proc/%d/task/%d/status", getpid(), getpid()); CHECK(slurp(expect, text, sizeof(text)) > 0 && strstr(text, "Name:"));
    CHECK(slurp("/proc/stat", text, sizeof(text)) > 0 && !strncmp(text, "cpu  ", 5) && strstr(text, "\ncpu0 ") && strstr(text, "\nbtime "));
    double up = 0, idle = 0; CHECK(slurp("/proc/uptime", text, sizeof(text)) > 0 && sscanf(text, "%lf %lf", &up, &idle) == 2 && up > 0);
    CHECK(slurp("/proc/sys/fs/inotify/max_user_watches", text, sizeof(text)) > 0 && atoi(text) == 65536);
    CHECK(slurp("/proc/sys/kernel/random/boot_id", text, sizeof(text)) == 37 && text[8] == '-' && text[14] == '4');
    char again_id[64]; CHECK(slurp("/proc/sys/kernel/random/boot_id", again_id, sizeof(again_id)) == 37 && !strcmp(text, again_id));
    CHECK(slurp("/proc/mounts", text, sizeof(text)) > 0 && strstr(text, " / vfat ") && strstr(text, " /dev/shm tmpfs "));
    CHECK(slurp("/proc/self/mountinfo", text, sizeof(text)) > 0 && strstr(text, " / / ") && strstr(text, " - devpts "));
    CHECK(listed("/proc", "stat") && listed("/proc", "sys") && listed("/proc/sys", "kernel") && listed("/proc/self", "status") && listed("/proc/sys/fs/inotify", "max_queued_events"));
    int adjust = open("/proc/self/oom_score_adj", O_WRONLY); CHECK(adjust >= 0 && write(adjust, "300\n", 4) == 4 && close(adjust) == 0);
    CHECK(slurp("/proc/self/oom_score_adj", text, sizeof(text)) > 0 && atoi(text) == 300);
    CHECK(open("/proc/self/status", O_WRONLY) == -1 && errno == EACCES && open("/proc/nope", O_RDONLY) == -1 && errno == ENOENT);
    struct stat proc_file; CHECK(stat("/proc/sys/kernel/osrelease", &proc_file) == 0 && S_ISREG(proc_file.st_mode) && stat("/proc/sys/kernel", &proc_file) == 0 && S_ISDIR(proc_file.st_mode));
    /* ---- nested epoll (Electron watches libuv's epoll fd from its own epoll) ---- */
    int inner = epoll_create1(EPOLL_CLOEXEC), outer = epoll_create1(EPOLL_CLOEXEC), wake[2]; CHECK(inner >= 0 && outer >= 0 && pipe(wake) == 0);
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 7}, got[2];
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, wake[0], &event) == 0);
    event.data.u64 = 9; event.events = EPOLLIN | EPOLLET; CHECK(epoll_ctl(outer, EPOLL_CTL_ADD, inner, &event) == 0);
    CHECK(epoll_wait(outer, got, 2, 0) == 0);
    pid_t poker = fork(); CHECK(poker >= 0);
    if (!poker) { usleep(100000); _exit(write(wake[1], "x", 1) == 1 ? 0 : 1); }
    CHECK(epoll_wait(outer, got, 2, 5000) == 1 && got[0].data.u64 == 9 && (got[0].events & EPOLLIN));
    CHECK(epoll_wait(outer, got, 2, 0) == 0);                                      /* edge-triggered: reported once */
    CHECK(epoll_wait(inner, got, 2, 0) == 1 && got[0].data.u64 == 7);
    event.events = EPOLLIN; CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, outer, &event) == -1 && errno == ELOOP);  /* cycle */
    int chain[6]; chain[0] = outer;
    for (int level = 1; level < 6; level++) { chain[level] = epoll_create1(0); CHECK(chain[level] >= 0); }
    CHECK(epoll_ctl(chain[1], EPOLL_CTL_ADD, chain[0], &event) == 0 && epoll_ctl(chain[2], EPOLL_CTL_ADD, chain[1], &event) == 0);
    CHECK(epoll_ctl(chain[3], EPOLL_CTL_ADD, chain[2], &event) == 0);
    CHECK(epoll_ctl(chain[4], EPOLL_CTL_ADD, chain[3], &event) == -1 && errno == ELOOP);                /* depth limit */
    for (int level = 1; level < 6; level++) close(chain[level]);
    CHECK(waitpid(poker, &wstatus_epoll, 0) == poker && close(inner) == 0 && close(outer) == 0 && close(wake[0]) == 0 && close(wake[1]) == 0);
    /* ---- /dev/shm (tmpfs) and umask ---- */
    CHECK(access("/dev/shm", W_OK | X_OK) == 0 && stat("/dev/shm", &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 01777);
    mode_t old_mask = umask(027); CHECK(umask(old_mask) == 027);
    shm_unlink("/sysext.shm");
    int named = shm_open("/sysext.shm", O_RDWR | O_CREAT | O_EXCL, 0666); CHECK(named >= 0);
    CHECK(shm_open("/sysext.shm", O_RDWR | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST);
    CHECK(fstat(named, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == (0666 & ~old_mask) && st.st_size == 0);
    CHECK(ftruncate(named, 3 * page) == 0 && stat("/dev/shm/sysext.shm", &st) == 0 && st.st_size == 3 * page);
    char* shared_view = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, named, 0); CHECK(shared_view != MAP_FAILED);
    pid_t writer = fork(); CHECK(writer >= 0);
    if (!writer) {
        int again_fd = shm_open("/sysext.shm", O_RDWR, 0); if (again_fd < 0) _exit(2);
        char* view2 = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, again_fd, page); if (view2 == MAP_FAILED) _exit(3);
        strcpy(view2, "shared through /dev/shm"); _exit(0);
    }
    int wstatus; CHECK(waitpid(writer, &wstatus, 0) == writer && WIFEXITED(wstatus) && !WEXITSTATUS(wstatus) && !strcmp(shared_view + page, "shared through /dev/shm"));
    DIR* listing = opendir("/dev/shm"); CHECK(listing); int seen_entry = 0; struct dirent* item;
    while ((item = readdir(listing))) if (!strcmp(item->d_name, "sysext.shm")) seen_entry = 1;
    CHECK(closedir(listing) == 0 && seen_entry);
    CHECK(rename("/dev/shm/sysext.shm", "/dev/shm/sysext2.shm") == 0 && access("/dev/shm/sysext.shm", F_OK) == -1 && errno == ENOENT);
    CHECK(rename("/dev/shm/sysext2.shm", HOME "/moved") == -1 && errno == EXDEV);
    CHECK(shm_unlink("/sysext2.shm") == 0 && shm_open("/sysext2.shm", O_RDWR, 0) == -1 && errno == ENOENT);
    CHECK(strcmp(shared_view + page, "shared through /dev/shm") == 0 && munmap(shared_view, 3 * page) == 0 && close(named) == 0);
    CHECK(open("/dev/shm/a/b", O_RDWR | O_CREAT, 0600) == -1 && errno == ENOENT);
    /* ---- #! scripts, including a script interpreter (binfmt_script) ---- */
    int script = open(HOME "/inner", O_CREAT | O_WRONLY | O_TRUNC, 0700);
    CHECK(script >= 0 && write(script, "#! /bin/sysext64  --script-args\n", 32) == 32 && close(script) == 0);
    script = open(HOME "/outer", O_CREAT | O_WRONLY | O_TRUNC, 0700);
    CHECK(script >= 0 && write(script, "#!" HOME "/inner spaced option \r\nbody", 29 + (int)strlen(HOME)) > 0 && close(script) == 0);
    for (int round = 0; round < 3; round++) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) {
            if (round == 0) execl(HOME "/inner", "inner", "one", "two", (char*)NULL);
            else if (round == 1) execl(HOME "/outer", "outer", "x", (char*)NULL);
            else { script = open(HOME "/inner", O_WRONLY); execl(HOME "/inner", "inner", (char*)NULL); }
            _exit(errno == ETXTBSY ? 44 : 2);
        }
        int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 42 + round);
    }
    CHECK(unlink(HOME "/inner") == 0 && unlink(HOME "/outer") == 0);
    CHECK(close(one) == 0 && close(two) == 0 && unlink(HOME "/one") == 0 && unlink(HOME "/two") == 0 && rmdir(HOME) == 0);
    puts("[64] SYSEXT ALL PASSED: mremap shrink/grow/move/fixed/dontunmap/shared-dup, MADV_REMOVE, memfd fallocate keep/punch, "
         "FAT fallocate/truncate, sendfile, copy_file_range, statx, vfat link/chmod/chown, name_to_handle_at, #! scripts, /dev/shm tmpfs and umask, nested epoll, procfs status/stat/statm/cmdline/maps/mounts/sys");
    return 0;
}
