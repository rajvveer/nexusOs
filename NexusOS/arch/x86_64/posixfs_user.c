/* POSIX filesystem semantics on an ext4 root: run as /bin/posix64 u (uid 1000, no capabilities) and as
 * /bin/posix64-root r (the same program installed set-user-ID root). Mode p verifies data, owners, modes
 * and times written by an earlier boot. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/falloc.h>
#include <grp.h>
#include <sys/fsuid.h>
#include <sys/auxv.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] POSIX FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define GiB (1024LL * 1024 * 1024)
static const char* base = "/tmp/posix64";

static void cleanup(const char* path) {
    DIR* d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent* e; char child[512];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(child, &st) && S_ISDIR(st.st_mode)) { chmod(child, 0700); cleanup(child); } else unlink(child);
    }
    closedir(d); rmdir(path);
}
static void put(const char* path, const char* text, mode_t mode) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
    CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text) && close(fd) == 0);
}
static int slurp(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    ssize_t n = read(fd, out, size - 1); close(fd);
    if (n < 0) return -1;
    out[n] = 0; return (int)n;
}
static struct stat info(const char* path) { struct stat st; memset(&st, 0, sizeof(st)); CHECK(lstat(path, &st) == 0); return st; }

static void permissions_user(void) {
    char p[256], q[256]; struct stat st;
    CHECK(umask(022) == 022);
    snprintf(p, sizeof(p), "%s/own", base); put(p, "owned", 0666);
    st = info(p); CHECK(st.st_uid == 1000 && st.st_gid == 1000 && (st.st_mode & 0777) == 0644);
    CHECK(chmod(p, 0400) == 0 && open(p, O_WRONLY) == -1 && errno == EACCES && open(p, O_RDONLY | O_TRUNC) == -1 && errno == EACCES);
    CHECK(chmod(p, 0200) == 0 && open(p, O_RDONLY) == -1 && errno == EACCES && access(p, R_OK) == -1 && errno == EACCES && access(p, W_OK) == 0);
    CHECK(chmod(p, 0644) == 0 && (info(p).st_mode & 07777) == 0644);
    CHECK(umask(077) == 022); snprintf(q, sizeof(q), "%s/masked", base); put(q, "m", 0666);
    CHECK((info(q).st_mode & 0777) == 0600); CHECK(umask(022) == 077);
    snprintf(q, sizeof(q), "%s/noexec-dir", base);
    CHECK(mkdir(q, 0755) == 0); snprintf(p, sizeof(p), "%s/noexec-dir/inside", base); put(p, "x", 0644);
    CHECK(chmod(q, 0644) == 0 && stat(p, &st) == -1 && errno == EACCES && open(p, O_RDONLY) == -1 && errno == EACCES);
    CHECK(chmod(q, 0555) == 0 && unlink(p) == -1 && errno == EACCES && stat(p, &st) == 0);
    CHECK(chmod(q, 0755) == 0 && unlink(p) == 0 && rmdir(q) == 0);
    /* system files belong to root */
    CHECK(open("/etc/posix64-forbidden", O_CREAT | O_WRONLY, 0600) == -1 && errno == EACCES);
    CHECK(unlink("/etc/native64.dat") == -1 && errno == EACCES);
    CHECK(chmod("/etc/native64.dat", 0777) == -1 && errno == EPERM);
    CHECK(chown("/etc/native64.dat", 1000, 1000) == -1 && errno == EPERM);
    CHECK(access("/etc/native64.dat", R_OK) == 0 && access("/etc/native64.dat", W_OK) == -1 && errno == EACCES);
    CHECK(faccessat(AT_FDCWD, "/etc/native64.dat", W_OK, AT_EACCESS) == -1 && errno == EACCES);
    snprintf(p, sizeof(p), "%s/own", base);
    CHECK(chown(p, 0, -1) == -1 && errno == EPERM && chown(p, -1, 0) == -1 && errno == EPERM);
    CHECK(chown(p, 1000, 1000) == 0 && chown(p, -1, -1) == 0);
    CHECK(mknod("/tmp/posix64/chardev", S_IFCHR | 0600, makedev(1, 3)) == -1 && errno == EPERM);
    /* set-ID bits: a non-member cannot keep setgid on chmod; writes by others drop setuid */
    CHECK(chmod(p, 04755) == 0 && (info(p).st_mode & 07777) == 04755);
    CHECK(chmod(p, 02755) == 0 && (info(p).st_mode & 07777) == 02755); /* own group: allowed */
    int fd = open(p, O_WRONLY | O_APPEND); CHECK(fd >= 0 && write(fd, "!", 1) == 1 && close(fd) == 0);
    CHECK((info(p).st_mode & 07777) == 0755);
    puts("[64] POSIX PERMISSIONS PASSED: owner/mode checks, umask, search permission, root-owned files, chown/chmod rules, set-ID clearing");
}
static void links(void) {
    char a[256], b[256], c[256], buf[64]; struct stat sa, sb;
    snprintf(a, sizeof(a), "%s/link-a", base); snprintf(b, sizeof(b), "%s/link-b", base); snprintf(c, sizeof(c), "%s/linkdir", base);
    put(a, "shared bytes", 0644);
    CHECK(link(a, b) == 0); sa = info(a); sb = info(b);
    CHECK(sa.st_ino == sb.st_ino && sa.st_nlink == 2 && sb.st_nlink == 2);
    CHECK(link(a, b) == -1 && errno == EEXIST);
    CHECK(mkdir(c, 0755) == 0 && link(c, "/tmp/posix64/dirlink") == -1 && errno == EPERM);
    CHECK(linkat(AT_FDCWD, a, AT_FDCWD, "/tmp/posix64/missing/x", 0) == -1 && errno == ENOENT);
    int fd = open(a, O_RDONLY); CHECK(fd >= 0);
    CHECK(unlink(a) == 0 && info(b).st_nlink == 1 && slurp(b, buf, sizeof(buf)) == 12 && !strcmp(buf, "shared bytes"));
    struct stat sf; CHECK(fstat(fd, &sf) == 0 && sf.st_nlink == 1 && sf.st_ino == sb.st_ino && close(fd) == 0);
    CHECK(linkat(AT_FDCWD, b, AT_FDCWD, a, 0) == 0 && info(a).st_nlink == 2);
    char d[256]; snprintf(d, sizeof(d), "%s/sub", c);
    CHECK(mkdir(d, 0755) == 0 && info(c).st_nlink == 3 && rmdir(d) == 0 && info(c).st_nlink == 2);
    CHECK(unlink(a) == 0 && unlink(b) == 0 && rmdir(c) == 0);
    puts("[64] POSIX LINKS PASSED: hard link counts, shared inode, EEXIST/EPERM/ENOENT, unlinked-but-open, directory nlink");
}
static void symlinks(void) {
    char target[2048], got[2048], p[256], q[256]; struct stat st;
    snprintf(p, sizeof(p), "%s/data", base); put(p, "symlink target data", 0644);
    CHECK(symlink("data", "/tmp/posix64/fast") == 0);
    st = info("/tmp/posix64/fast"); CHECK(S_ISLNK(st.st_mode) && st.st_size == 4 && st.st_blocks == 0);
    CHECK(readlink("/tmp/posix64/fast", got, sizeof(got)) == 4 && !memcmp(got, "data", 4));
    CHECK(stat("/tmp/posix64/fast", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 19);
    memset(target, 0, sizeof(target)); strcpy(target, "/tmp/posix64/");
    for (int i = 13; i < 400; i++) target[i] = 'a' + i % 26; /* kernel paths are limited to 512 bytes */
    target[400] = 0;
    CHECK(symlink(target, "/tmp/posix64/slow") == 0);
    st = info("/tmp/posix64/slow"); CHECK(S_ISLNK(st.st_mode) && st.st_size == 400 && st.st_blocks > 0);
    CHECK(readlink("/tmp/posix64/slow", got, sizeof(got)) == 400 && !memcmp(got, target, 400));
    memset(q, 'e', 60); q[60] = 0; /* 59 bytes fit in the inode, 60 need a block */
    CHECK(symlink(q, "/tmp/posix64/edge") == 0 && info("/tmp/posix64/edge").st_size == 60 && info("/tmp/posix64/edge").st_blocks > 0);
    q[59] = 0; CHECK(symlink(q, "/tmp/posix64/edge59") == 0 && info("/tmp/posix64/edge59").st_size == 59 && info("/tmp/posix64/edge59").st_blocks == 0);
    CHECK(readlink("/tmp/posix64/edge59", got, sizeof(got)) == 59 && !memcmp(got, q, 59));
    CHECK(open("/tmp/posix64/slow", O_RDONLY) == -1 && errno == ENOENT); /* dangling */
    CHECK(symlink("loop-b", "/tmp/posix64/loop-a") == 0 && symlink("loop-a", "/tmp/posix64/loop-b") == 0);
    CHECK(open("/tmp/posix64/loop-a", O_RDONLY) == -1 && errno == ELOOP);
    CHECK(open("/tmp/posix64/fast", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    CHECK(syscall(452 /* fchmodat2 */, AT_FDCWD, "/tmp/posix64/fast", 0600, AT_SYMLINK_NOFOLLOW) == -1 && errno == EOPNOTSUPP);
    CHECK(lchown("/tmp/posix64/fast", 1000, 1000) == 0);
    CHECK(symlink("data", "/tmp/posix64/fast") == -1 && errno == EEXIST);
    const char* names[] = {"fast", "slow", "edge", "edge59", "loop-a", "loop-b", "data"};
    for (unsigned i = 0; i < 7; i++) { snprintf(p, sizeof(p), "%s/%s", base, names[i]); CHECK(unlink(p) == 0); }
    puts("[64] POSIX SYMLINKS PASSED: fast (<60) and slow (60/400-byte) links, readlink/lstat/stat, dangling, ELOOP, O_NOFOLLOW, lchown");
}
static void renames(void) {
    char buf[64]; struct stat st;
    CHECK(mkdir("/tmp/posix64/r", 0755) == 0 && mkdir("/tmp/posix64/r/a", 0755) == 0 && mkdir("/tmp/posix64/r/b", 0755) == 0);
    put("/tmp/posix64/r/a/one", "one", 0644); put("/tmp/posix64/r/a/two", "two", 0644);
    struct stat one = info("/tmp/posix64/r/a/one"), two = info("/tmp/posix64/r/a/two");
    int old = open("/tmp/posix64/r/a/two", O_RDONLY); CHECK(old >= 0);
    CHECK(rename("/tmp/posix64/r/a/one", "/tmp/posix64/r/a/two") == 0);
    st = info("/tmp/posix64/r/a/two"); CHECK(st.st_ino == one.st_ino && slurp("/tmp/posix64/r/a/two", buf, sizeof(buf)) == 3 && !strcmp(buf, "one"));
    CHECK(access("/tmp/posix64/r/a/one", F_OK) == -1 && errno == ENOENT);
    CHECK(fstat(old, &st) == 0 && st.st_ino == two.st_ino && st.st_nlink == 0 && read(old, buf, 3) == 3 && !memcmp(buf, "two", 3) && close(old) == 0);
    put("/tmp/posix64/r/a/three", "3", 0644);
    CHECK(syscall(SYS_renameat2, AT_FDCWD, "/tmp/posix64/r/a/three", AT_FDCWD, "/tmp/posix64/r/a/two", RENAME_NOREPLACE) == -1 && errno == EEXIST);
    CHECK(link("/tmp/posix64/r/a/three", "/tmp/posix64/r/a/three-link") == 0);
    CHECK(rename("/tmp/posix64/r/a/three", "/tmp/posix64/r/a/three-link") == 0 && access("/tmp/posix64/r/a/three", F_OK) == 0);
    CHECK(mkdir("/tmp/posix64/r/a/sub", 0755) == 0 && mkdir("/tmp/posix64/r/b/empty", 0755) == 0 && mkdir("/tmp/posix64/r/b/full", 0755) == 0);
    put("/tmp/posix64/r/b/full/x", "x", 0644);
    CHECK(rename("/tmp/posix64/r/a/sub", "/tmp/posix64/r/b/full") == -1 && errno == ENOTEMPTY);
    CHECK(rename("/tmp/posix64/r/a/two", "/tmp/posix64/r/b/empty") == -1 && errno == EISDIR);
    CHECK(rename("/tmp/posix64/r/a/sub", "/tmp/posix64/r/a/two") == -1 && errno == ENOTDIR);
    CHECK(rename("/tmp/posix64/r/a", "/tmp/posix64/r/a/sub/inside") == -1 && errno == EINVAL);
    struct stat a0 = info("/tmp/posix64/r/a"), b0 = info("/tmp/posix64/r/b");
    CHECK(rename("/tmp/posix64/r/a/sub", "/tmp/posix64/r/b/empty") == 0);
    struct stat a1 = info("/tmp/posix64/r/a"), b1 = info("/tmp/posix64/r/b"), up;
    CHECK(a1.st_nlink == a0.st_nlink - 1 && b1.st_nlink == b0.st_nlink);
    CHECK(stat("/tmp/posix64/r/b/empty/..", &up) == 0 && up.st_ino == b1.st_ino);
    CHECK(rename("/tmp/posix64/r/b/empty", "/tmp/posix64/r/a/moved") == 0 && info("/tmp/posix64/r/a").st_nlink == a0.st_nlink);
    CHECK(stat("/tmp/posix64/r/a/moved/..", &up) == 0 && up.st_ino == a1.st_ino);
    char big[64]; /* many entries force an htree directory, renames inside it keep the index valid */
    CHECK(mkdir("/tmp/posix64/r/many", 0755) == 0);
    for (int i = 0; i < 600; i++) { snprintf(big, sizeof(big), "/tmp/posix64/r/many/entry-with-a-long-name-%04d", i); put(big, "e", 0644); }
    for (int i = 0; i < 600; i += 3) {
        char to[64]; snprintf(big, sizeof(big), "/tmp/posix64/r/many/entry-with-a-long-name-%04d", i);
        snprintf(to, sizeof(to), "/tmp/posix64/r/many/renamed-%04d", i); CHECK(rename(big, to) == 0);
    }
    for (int i = 0; i < 600; i++) {
        if (i % 3) snprintf(big, sizeof(big), "/tmp/posix64/r/many/entry-with-a-long-name-%04d", i);
        else snprintf(big, sizeof(big), "/tmp/posix64/r/many/renamed-%04d", i);
        CHECK(access(big, F_OK) == 0);
        if (i % 2) CHECK(unlink(big) == 0);
    }
    DIR* d = opendir("/tmp/posix64/r/many"); int count = 0; struct dirent* e;
    CHECK(d); while ((e = readdir(d))) count++; closedir(d);
    CHECK(count == 300 + 2);
    puts("[64] POSIX RENAME PASSED: replace (old inode stays open), NOREPLACE, hard-link no-op, ENOTEMPTY/EISDIR/ENOTDIR/EINVAL, '..' and nlink updates, 600-entry htree");
}
/* data blocks plus up to two extent-tree blocks (fragmented free space needs an index level) */
static int blocks_ok(const struct stat* st, long long data) { return st->st_blocks >= data && st->st_blocks <= data + 16; }
static void holes(void) {
    int fd = open("/tmp/posix64/holes", O_CREAT | O_RDWR | O_TRUNC, 0644); CHECK(fd >= 0);
    char block[4096]; memset(block, 'D', sizeof(block));
    CHECK(pwrite(fd, block, 4096, 0) == 4096 && pwrite(fd, block, 4096, 8 << 20) == 4096 && fsync(fd) == 0);
    struct stat st; CHECK(fstat(fd, &st) == 0 && st.st_size == (8 << 20) + 4096 && st.st_blocks == 16);
    char zero[4096], got[4096]; memset(zero, 0, sizeof(zero));
    CHECK(pread(fd, got, 4096, 4 << 20) == 4096 && !memcmp(got, zero, 4096));
    CHECK(lseek(fd, 4096, SEEK_DATA) == (8 << 20) && lseek(fd, 0, SEEK_HOLE) == 4096 && lseek(fd, (8 << 20) + 4096, SEEK_DATA) == -1 && errno == ENXIO);
    CHECK(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096) == 0);
    CHECK(pread(fd, got, 4096, 0) == 4096 && !memcmp(got, zero, 4096) && fstat(fd, &st) == 0 && blocks_ok(&st, 8) && st.st_size == (8 << 20) + 4096);
    CHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, 16 << 20, 1 << 20) == 0 && fstat(fd, &st) == 0 && st.st_size == (8 << 20) + 4096 && blocks_ok(&st, 8 + 2048));
    CHECK(fallocate(fd, 0, 1 << 20, 3 << 20) == 0 && fstat(fd, &st) == 0 && blocks_ok(&st, 8 + 2048 + 6144));
    CHECK(pread(fd, got, 4096, 2 << 20) == 4096 && !memcmp(got, zero, 4096));
    CHECK(pwrite(fd, "W", 1, (2 << 20) + 100) == 1 && fsync(fd) == 0 && pread(fd, got, 4096, 2 << 20) == 4096 && got[100] == 'W' && got[99] == 0 && got[101] == 0);
    CHECK(fallocate(fd, FALLOC_FL_ZERO_RANGE, (8 << 20) + 10, 20) == 0 && pread(fd, got, 40, 8 << 20) == 40 && got[9] == 'D' && got[10] == 0 && got[29] == 0 && got[30] == 'D');
    CHECK(ftruncate(fd, 4096) == 0 && fsync(fd) == 0 && fstat(fd, &st) == 0 && st.st_blocks == 0 && close(fd) == 0);
    CHECK(truncate("/tmp/posix64/holes", 12345) == 0 && info("/tmp/posix64/holes").st_size == 12345 && unlink("/tmp/posix64/holes") == 0);
    puts("[64] POSIX HOLES PASSED: sparse reads, st_blocks, SEEK_DATA/SEEK_HOLE, punch/allocate/zero-range fallocate, unwritten extents, truncate");
}
static void huge(void) {
    const off_t size = 5 * GiB;
    int fd = open("/tmp/posix64/huge", O_CREAT | O_RDWR | O_TRUNC, 0644); CHECK(fd >= 0);
    CHECK(ftruncate(fd, size) == 0);
    struct stat st; CHECK(fstat(fd, &st) == 0 && st.st_size == size && st.st_blocks == 0);
    const char* tail = "end-of-a-five-GiB-file"; size_t tl = strlen(tail);
    CHECK(pwrite(fd, "beyond-4GiB", 11, 4 * GiB + 123) == 11 && pwrite(fd, tail, tl, size - (off_t)tl) == (ssize_t)tl);
    CHECK(pwrite(fd, "low", 3, 5000) == 3 && fsync(fd) == 0 && close(fd) == 0);
    fd = open("/tmp/posix64/huge", O_RDONLY); CHECK(fd >= 0);
    char buf[64] = {0};
    CHECK(pread(fd, buf, 11, 4 * GiB + 123) == 11 && !memcmp(buf, "beyond-4GiB", 11));
    CHECK(pread(fd, buf, tl, size - (off_t)tl) == (ssize_t)tl && !memcmp(buf, tail, tl));
    CHECK(pread(fd, buf, 16, size) == 0 && pread(fd, buf, 4, 3 * GiB) == 4 && !buf[0] && !buf[3]);
    CHECK(lseek(fd, 0, SEEK_END) == size && lseek(fd, 4 * GiB, SEEK_DATA) == 4 * GiB);
    CHECK(fstat(fd, &st) == 0 && st.st_blocks <= 3 * 8 && st.st_blocks >= 3 * 8);
    unsigned char* page = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 4 * GiB);
    CHECK(page != MAP_FAILED && !memcmp(page + 123, "beyond-4GiB", 11) && munmap(page, 4096) == 0);
    struct statx sx; CHECK(statx(fd, "", AT_EMPTY_PATH, STATX_SIZE | STATX_BLOCKS, &sx) == 0 && sx.stx_size == (uint64_t)size);
    CHECK(close(fd) == 0);
    puts("[64] POSIX 5GiB SPARSE PASSED: ftruncate/pwrite/pread/lseek/mmap beyond 4 GiB with three allocated blocks");
}
static void times_and_statx(void) {
    put("/tmp/posix64/times", "t", 0644);
    struct statx sx; CHECK(statx(AT_FDCWD, "/tmp/posix64/times", 0, STATX_ALL, &sx) == 0);
    CHECK((sx.stx_mask & (STATX_BASIC_STATS | STATX_BTIME)) == (STATX_BASIC_STATS | STATX_BTIME));
    struct stat st = info("/tmp/posix64/times");
    CHECK(sx.stx_ino == st.st_ino && sx.stx_nlink == 1 && sx.stx_uid == 1000 && sx.stx_size == 1 && (sx.stx_mode & 0777) == 0644 && S_ISREG(sx.stx_mode));
    CHECK(sx.stx_btime.tv_sec > 1577836800 && sx.stx_btime.tv_sec <= sx.stx_mtime.tv_sec + 1);
    struct timespec set[2] = {{1000000000, 123456789}, {1100000000, 987654321}};
    CHECK(utimensat(AT_FDCWD, "/tmp/posix64/times", set, 0) == 0);
    st = info("/tmp/posix64/times");
    CHECK(st.st_atim.tv_sec == 1000000000 && st.st_atim.tv_nsec == 123456789 && st.st_mtim.tv_sec == 1100000000 && st.st_mtim.tv_nsec == 987654321);
    CHECK(st.st_ctim.tv_sec > 1577836800);
    struct timespec omit[2] = {{0, UTIME_OMIT}, {0, UTIME_NOW}};
    CHECK(utimensat(AT_FDCWD, "/tmp/posix64/times", omit, 0) == 0);
    st = info("/tmp/posix64/times"); CHECK(st.st_atim.tv_sec == 1000000000 && st.st_mtim.tv_sec > 1577836800);
    int fd = open("/tmp/posix64/times", O_RDONLY); CHECK(fd >= 0 && futimens(fd, set) == 0 && close(fd) == 0);
    CHECK(info("/tmp/posix64/times").st_mtim.tv_nsec == 987654321);
    CHECK(utimensat(AT_FDCWD, "/etc/native64.dat", set, 0) == -1 && errno == EPERM);
    CHECK(utimensat(AT_FDCWD, "/etc/native64.dat", NULL, 0) == -1 && errno == EACCES);
    CHECK(symlink("times", "/tmp/posix64/times-link") == 0 && utimensat(AT_FDCWD, "/tmp/posix64/times-link", set, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(statx(AT_FDCWD, "/tmp/posix64/times-link", AT_SYMLINK_NOFOLLOW, STATX_ALL, &sx) == 0 && S_ISLNK(sx.stx_mode) && sx.stx_mtime.tv_sec == 1100000000);
    CHECK(statx(AT_FDCWD, "/tmp/posix64/times-link", 0, STATX_ALL, &sx) == 0 && S_ISREG(sx.stx_mode));
    CHECK(statx(AT_FDCWD, "/tmp/posix64/times", 0x6000, STATX_ALL, &sx) == -1 && errno == EINVAL);
    fd = open("/tmp/posix64/times", O_WRONLY | O_APPEND); CHECK(fd >= 0 && write(fd, "x", 1) == 1);
    CHECK(fsync(fd) == 0 && fdatasync(fd) == 0 && syncfs(fd) == 0 && close(fd) == 0);
    CHECK(info("/tmp/posix64/times").st_mtim.tv_sec > 1577836800);
    CHECK(mknod("/tmp/posix64/fifo", S_IFIFO | 0644, 0) == 0 && S_ISFIFO(info("/tmp/posix64/fifo").st_mode));
    CHECK(mknodat(AT_FDCWD, "/tmp/posix64/plain", S_IFREG | 0640, 0) == 0 && (info("/tmp/posix64/plain").st_mode & 07777) == 0640);
    CHECK(mknod("/tmp/posix64/sock", S_IFSOCK | 0600, 0) == 0 && S_ISSOCK(info("/tmp/posix64/sock").st_mode));
    CHECK(unlink("/tmp/posix64/fifo") == 0 && unlink("/tmp/posix64/plain") == 0 && unlink("/tmp/posix64/sock") == 0);
    CHECK(unlink("/tmp/posix64/times-link") == 0 && unlink("/tmp/posix64/times") == 0);
    puts("[64] POSIX STATX/TIMES PASSED: statx basic+btime, nanosecond utimensat/futimens/OMIT/NOW, symlink times, fsync/fdatasync/syncfs, mknod fifo/socket/regular");
}
static void persist_write(void) {
    cleanup("/home/nexus/posix-persist");
    CHECK(mkdir("/home/nexus/posix-persist", 0750) == 0);
    put("/home/nexus/posix-persist/data", "persisted ext4 data", 0640);
    struct timespec set[2] = {{1200000000, 5}, {1300000000, 777777777}};
    CHECK(utimensat(AT_FDCWD, "/home/nexus/posix-persist/data", set, 0) == 0);
    CHECK(symlink("data", "/home/nexus/posix-persist/link") == 0 && link("/home/nexus/posix-persist/data", "/home/nexus/posix-persist/hard") == 0);
    int fd = open("/home/nexus/posix-persist/sparse", O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && pwrite(fd, "far", 3, 4 * GiB + 7) == 3 && fsync(fd) == 0 && close(fd) == 0);
    /* a large directory left on disk: the kernel builds and splits its htree index (checked by e2fsck) */
    char name[128], to[128];
    CHECK(mkdir("/home/nexus/posix-persist/index", 0755) == 0);
    for (int i = 0; i < 900; i++) { snprintf(name, sizeof(name), "/home/nexus/posix-persist/index/a-rather-long-file-name-number-%05d.dat", i); put(name, "i", 0644); }
    for (int i = 0; i < 900; i += 3) { snprintf(name, sizeof(name), "/home/nexus/posix-persist/index/a-rather-long-file-name-number-%05d.dat", i); CHECK(unlink(name) == 0); }
    for (int i = 1; i < 900; i += 5) {
        if (i % 3 == 0) continue; /* already unlinked */
        snprintf(name, sizeof(name), "/home/nexus/posix-persist/index/a-rather-long-file-name-number-%05d.dat", i);
        snprintf(to, sizeof(to), "/home/nexus/posix-persist/index/renamed-%05d", i); CHECK(rename(name, to) == 0);
    }
}
static void persist_verify(void) {
    char buf[64]; struct stat st;
    if (access("/home/nexus/posix-persist/data", F_OK)) return;
    st = info("/home/nexus/posix-persist/data");
    CHECK(slurp("/home/nexus/posix-persist/data", buf, sizeof(buf)) == 19 && !strcmp(buf, "persisted ext4 data"));
    CHECK((st.st_mode & 07777) == 0640 && st.st_uid == 1000 && st.st_nlink == 2 && st.st_mtim.tv_sec == 1300000000 && st.st_mtim.tv_nsec == 777777777);
    CHECK((info("/home/nexus/posix-persist").st_mode & 07777) == 0750 && info("/home/nexus/posix-persist/hard").st_ino == st.st_ino);
    CHECK(readlink("/home/nexus/posix-persist/link", buf, sizeof(buf)) == 4 && !memcmp(buf, "data", 4));
    int fd = open("/home/nexus/posix-persist/sparse", O_RDONLY);
    CHECK(fd >= 0 && pread(fd, buf, 3, 4 * GiB + 7) == 3 && !memcmp(buf, "far", 3) && close(fd) == 0);
    CHECK(info("/home/nexus/posix-persist/sparse").st_size == 4 * GiB + 10);
    DIR* d = opendir("/home/nexus/posix-persist/index"); int count = 0; struct dirent* e; CHECK(d);
    while ((e = readdir(d))) count++;
    CHECK(closedir(d) == 0 && count == 600 + 2 && access("/home/nexus/posix-persist/index/renamed-00001", F_OK) == 0);
    puts("[64] POSIX PERSISTED PASSED: data, mode, owner, nanosecond mtime, hard link, symlink, >4 GiB sparse file and a 600-entry htree directory survived reboot");
}

/* A volume made by Linux (tests/ext4_linux_validator.py linuxmade): mke2fs defaults, a 1500-entry htree built
 * by the Linux kernel, a fast and a slow symlink. First boot looks every name up, unlinks 375 and adds 400
 * (insertions into the Linux-built index); later boots verify that state. */
static void linux_index(void) {
    const char* dir = "/home/nexus/linux-index";
    if (access(dir, F_OK)) return;
    char name[160], buf[160];
    int mutated = access("/home/nexus/linux-index/nexus-added-0000", F_OK) == 0;
    for (int i = 0; i < 1500; i++) {
        snprintf(name, sizeof(name), "%s/linux-made-entry-name-%d", dir, i);
        if (mutated && i % 4 == 0) { CHECK(access(name, F_OK) == -1 && errno == ENOENT); continue; }
        CHECK(slurp(name, buf, sizeof(buf)) > 0 && atoi(buf) == i);
    }
    if (!mutated) {
        for (int i = 0; i < 1500; i += 4) { snprintf(name, sizeof(name), "%s/linux-made-entry-name-%d", dir, i); CHECK(unlink(name) == 0); }
        for (int i = 0; i < 400; i++) {
            snprintf(name, sizeof(name), "%s/nexus-added-%04d", dir, i); snprintf(buf, sizeof(buf), "%d", i + 5000); put(name, buf, 0644);
        }
    }
    for (int i = 0; i < 400; i++) {
        snprintf(name, sizeof(name), "%s/nexus-added-%04d", dir, i); CHECK(slurp(name, buf, sizeof(buf)) > 0 && atoi(buf) == i + 5000);
    }
    DIR* d = opendir(dir); int count = 0; struct dirent* e; CHECK(d);
    while ((e = readdir(d))) count++;
    CHECK(closedir(d) == 0 && count == 2 + 1500 - 375 + 400);
    CHECK(readlink("/home/nexus/linux-link", buf, sizeof(buf)) == 47 && slurp("/home/nexus/linux-link", buf, sizeof(buf)) > 0 && atoi(buf) == 7);
    ssize_t slow = readlink("/home/nexus/linux-slow-link", buf, sizeof(buf));
    CHECK(slow > 60 && slurp("/home/nexus/linux-slow-link", buf, sizeof(buf)) > 0 && atoi(buf) == 9);
    struct stat st = info(dir); CHECK(S_ISDIR(st.st_mode) && st.st_uid == 1000);
    puts(mutated ? "[64] POSIX LINUX INDEX VERIFIED: a Linux-built htree with 375 unlinks and 400 NexusOS insertions survived reboot"
                 : "[64] POSIX LINUX INDEX PASSED: 1500 lookups in a Linux-built htree, 375 unlinks, 400 insertions, Linux fast/slow symlinks");
}

/* ---- as root (set-user-ID program owned by root) ---- */
static void as_root(void) {
    uid_t r, e, s; gid_t rg, eg, sg;
    CHECK(getresuid(&r, &e, &s) == 0 && r == 1000 && e == 0 && s == 0);
    CHECK(getauxval(AT_SECURE) == 1);   /* set-ID exec: secure mode (ld.so ignores LD_PRELOAD / LD_LIBRARY_PATH) */
    CHECK(setresuid(0, 0, 0) == 0 && getresuid(&r, &e, &s) == 0 && !r && !e && !s);
    CHECK(setresgid(0, 0, 0) == 0 && getresgid(&rg, &eg, &sg) == 0 && !rg && !eg && !sg);
    umask(022);
    put("/etc/posix64-root", "root data", 0600);
    struct stat st = info("/etc/posix64-root"); CHECK(st.st_uid == 0 && st.st_gid == 0 && (st.st_mode & 0777) == 0600);
    CHECK(chmod("/etc/posix64-root", 0) == 0 && open("/etc/posix64-root", O_RDWR) >= 0); /* DAC override */
    CHECK(chmod("/etc/posix64-root", 0644) == 0);
    CHECK(chown("/etc/posix64-root", 4321, 8765) == 0 && info("/etc/posix64-root").st_uid == 4321 && info("/etc/posix64-root").st_gid == 8765);
    CHECK(chmod("/etc/posix64-root", 06755) == 0 && chown("/etc/posix64-root", 0, 0) == 0 && (info("/etc/posix64-root").st_mode & 07777) == 0755);
    CHECK(chmod("/etc/posix64-root", 0644) == 0 && execl("/etc/posix64-root", "x", NULL) == -1 && errno == EACCES); /* no x bit even for root */
    CHECK(mknod("/tmp/posix64/null", S_IFCHR | 0666, makedev(1, 3)) == 0);
    st = info("/tmp/posix64/null"); CHECK(S_ISCHR(st.st_mode) && major(st.st_rdev) == 1 && minor(st.st_rdev) == 3);
    CHECK(mknod("/tmp/posix64/bigdev", S_IFBLK | 0600, makedev(259, 70000)) == 0);
    st = info("/tmp/posix64/bigdev"); CHECK(S_ISBLK(st.st_mode) && major(st.st_rdev) == 259 && minor(st.st_rdev) == 70000);
    /* setgid directory inheritance */
    CHECK(mkdir("/tmp/posix64/sgid", 0755) == 0 && chown("/tmp/posix64/sgid", 0, 4444) == 0 && chmod("/tmp/posix64/sgid", 02775) == 0);
    put("/tmp/posix64/sgid/file", "g", 0644); CHECK(mkdir("/tmp/posix64/sgid/dir", 0755) == 0);
    CHECK(info("/tmp/posix64/sgid/file").st_gid == 4444 && info("/tmp/posix64/sgid/dir").st_gid == 4444 && (info("/tmp/posix64/sgid/dir").st_mode & 02000));
    /* sticky /tmp and other users */
    put("/tmp/posix64/rootfile", "r", 0644); put("/tmp/posix64/private", "p", 0640);
    CHECK(chmod("/tmp/posix64", 01777) == 0);
    int fd = open("/etc/native64.dat", O_RDONLY); CHECK(fd >= 0);
    CHECK(linkat(fd, "", AT_FDCWD, "/tmp/posix64/fdlink", AT_EMPTY_PATH) == 0 && info("/tmp/posix64/fdlink").st_ino == info("/etc/native64.dat").st_ino);
    CHECK(unlink("/tmp/posix64/fdlink") == 0 && close(fd) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(setgroups(0, NULL) == 0 && setresgid(1001, 1001, 1001) == 0 && setresuid(1001, 1001, 1001) == 0);
        CHECK(unlink("/tmp/posix64/rootfile") == -1 && errno == EPERM);
        CHECK(open("/tmp/posix64/rootfile", O_WRONLY) == -1 && errno == EACCES && open("/tmp/posix64/rootfile", O_RDONLY) >= 0);
        CHECK(open("/tmp/posix64/private", O_RDONLY) == -1 && errno == EACCES);
        put("/tmp/posix64/mine", "m", 0644); CHECK(unlink("/tmp/posix64/mine") == 0);
        CHECK(open("/home/nexus/posix64-other", O_CREAT | O_WRONLY, 0600) == -1 && errno == EACCES);
        CHECK(chown("/tmp/posix64/rootfile", 1001, 1001) == -1 && errno == EPERM);
        CHECK(setuid(0) == -1 && errno == EPERM);
        _exit(0);
    }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* dropping root clears capabilities */
    child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(setresuid(1000, 1000, 1000) == 0 && chown("/tmp/posix64/rootfile", 1000, 1000) == -1 && errno == EPERM);
        CHECK(open("/etc/posix64-root", O_WRONLY) == -1 && errno == EACCES);
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(setfsuid(1000) == 0 && open("/etc/posix64-root", O_WRONLY) == -1 && errno == EACCES && setfsuid(0) == 1000);
    CHECK(unlink("/etc/posix64-root") == 0);
    puts("[64] POSIX ROOT PASSED: set-user-ID exec, setresuid/setfsuid, DAC override, chown/chmod set-ID rules, device nodes, setgid dirs, sticky /tmp, AT_EMPTY_PATH link, capability drop");
}
#include "fsfix_regress_ext4.h"
#include "posix_ptrace.h"
int main(int argc, char** argv) {
    int helper = ptrace_helper(argc, argv); if (helper >= 0) return helper;
    CHECK(argc == 2);
    struct statfs fs; CHECK(statfs("/", &fs) == 0 && fs.f_type == 0xEF53);
    CHECK(getauxval(AT_SECURE) == (argv[1][0] == 'r'));
    if (argv[1][0] == 'r') {
        CHECK(access(base, F_OK) == 0); /* created by the unprivileged run */
        as_root(); cleanup(base);
        puts("[64] POSIX ROOT ALL PASSED"); return 0;
    }
    persist_verify(); linux_index();
    cleanup(base);
    CHECK(mkdir(base, 0755) == 0);
    permissions_user(); links(); symlinks(); renames(); holes(); huge(); times_and_statx(); fsfix_regress();
    ptrace_self = argv[0]; ptrace_checks(base);
    persist_write();
    puts("[64] POSIX USER ALL PASSED"); fflush(stdout);
    /* execve of the set-user-ID root copy: the kernel must switch the effective and saved IDs */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { execl("/bin/posix64-root", "/bin/posix64-root", "r", (char*)NULL); _exit(98); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}
