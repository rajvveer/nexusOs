/* fs fix round (wine-4, wine-6, wine-5, wine-46): included by posixfs_user.c (ext4 root). Unlinked-but-open files
 * are never written back, so their cache is the only copy of their data, size and write time. */
static int fsfix_misses_ext4;
#define FCHECK(test) do { if (!(test)) { dprintf(2, "[64] FSFIX MISS line=%d errno=%d\n", __LINE__, errno); fsfix_misses_ext4++; } } while (0)
static int later(struct timespec a, struct timespec b) { return a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec > b.tv_nsec); }
static void fsfix_regress(void) {
    static char data[1 << 20]; char got[4096], zero[4096]; struct stat st; memset(zero, 0, sizeof(zero));
    /* wine-4 / wine-6: SEEK_DATA/SEEK_HOLE, st_blocks and mtime of an orphan whose data was never flushed */
    int fd = open("/tmp/posix64/orphan", O_CREAT | O_RDWR | O_TRUNC, 0644); FCHECK(fd >= 0);
    struct timespec old[2] = {{1000000000, 0}, {1000000000, 0}};
    FCHECK(futimens(fd, old) == 0);
    memset(data, 'O', sizeof(data));
    FCHECK(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data) && unlink("/tmp/posix64/orphan") == 0);
    FCHECK(fstat(fd, &st) == 0 && st.st_nlink == 0 && st.st_size == (off_t)sizeof(data));
    FCHECK(st.st_mtime > 1577836800 && st.st_ctime > 1577836800);
    FCHECK(st.st_blocks >= (blkcnt_t)(sizeof(data) / 512));
    FCHECK(lseek(fd, 0, SEEK_DATA) == 0 && lseek(fd, 8192, SEEK_DATA) == 8192 && lseek(fd, 0, SEEK_HOLE) == (off_t)sizeof(data));
    FCHECK(lseek(fd, sizeof(data), SEEK_DATA) == -1 && errno == ENXIO && lseek(fd, sizeof(data), SEEK_HOLE) == -1 && errno == ENXIO);
    struct timespec first = st.st_mtim; usleep(20000);
    FCHECK(pwrite(fd, "X", 1, 10) == 1 && fstat(fd, &st) == 0 && later(st.st_mtim, first));
    FCHECK(pread(fd, got, 16, 0) == 16 && got[10] == 'X' && got[9] == 'O');
    /* wine-6 (second pass): an atime-only update keeps the orphan's write time; punch/zero-range advance it */
    first = st.st_mtim;
    struct timespec atime_now[2] = {{0, UTIME_NOW}, {0, UTIME_OMIT}}, atime_set[2] = {{1200000000, 0}, {0, UTIME_OMIT}};
    FCHECK(futimens(fd, atime_now) == 0 && fstat(fd, &st) == 0 && st.st_mtim.tv_sec == first.tv_sec && st.st_mtim.tv_nsec == first.tv_nsec);
    FCHECK(futimens(fd, atime_set) == 0 && fstat(fd, &st) == 0 && st.st_atime == 1200000000);
    FCHECK(st.st_mtim.tv_sec == first.tv_sec && st.st_mtim.tv_nsec == first.tv_nsec);
    usleep(20000);
    FCHECK(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096) == 0 && fstat(fd, &st) == 0 && later(st.st_mtim, first));
    FCHECK(st.st_size == (off_t)sizeof(data) && pread(fd, got, 16, 0) == 16 && !memcmp(got, zero, 16));
    first = st.st_mtim; usleep(20000);
    FCHECK(fallocate(fd, FALLOC_FL_ZERO_RANGE, 8192, 4096) == 0 && fstat(fd, &st) == 0 && later(st.st_mtim, first));
    FCHECK(pread(fd, got, 4096, 8192) == 4096 && !memcmp(got, zero, 4096) && pread(fd, got, 1, 12288) == 1 && got[0] == 'O');
    struct timespec mtime_set[2] = {{0, UTIME_OMIT}, {1300000000, 7}};
    FCHECK(futimens(fd, mtime_set) == 0 && fstat(fd, &st) == 0 && st.st_mtime == 1300000000 && st.st_mtim.tv_nsec == 7);
    first = st.st_mtim; FCHECK(pwrite(fd, "Y", 1, 20) == 1 && fstat(fd, &st) == 0 && later(st.st_mtim, first));
    FCHECK(close(fd) == 0);
    /* wine-5: fallocate on an orphan must not bring back data cut only in its cache (O_TRUNC before the unlink) */
    memset(data, 'S', sizeof(data));
    fd = open("/tmp/posix64/stale", O_CREAT | O_RDWR | O_TRUNC, 0644);
    FCHECK(fd >= 0 && write(fd, data, sizeof(data)) == (ssize_t)sizeof(data) && fsync(fd) == 0 && close(fd) == 0);
    fd = open("/tmp/posix64/stale", O_RDWR | O_TRUNC); FCHECK(fd >= 0 && unlink("/tmp/posix64/stale") == 0);
    FCHECK(fallocate(fd, 0, 0, 2 << 20) == 0 && fstat(fd, &st) == 0 && st.st_size == (2 << 20));
    FCHECK(pread(fd, got, 4096, 0) == 4096 && !memcmp(got, zero, 4096) && pread(fd, got, 4096, (1 << 20) - 4096) == 4096 && !memcmp(got, zero, 4096));
    FCHECK(close(fd) == 0);
    /* ... and ftruncate after the unlink: KEEP_SIZE keeps the cut size, data written since survives */
    fd = open("/tmp/posix64/stale", O_CREAT | O_RDWR | O_TRUNC, 0644);
    FCHECK(fd >= 0 && write(fd, data, sizeof(data)) == (ssize_t)sizeof(data) && fsync(fd) == 0 && unlink("/tmp/posix64/stale") == 0);
    FCHECK(ftruncate(fd, 0) == 0 && pwrite(fd, "keep", 4, 100) == 4);
    FCHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 4096) == 0 && fstat(fd, &st) == 0 && st.st_size == 104);
    FCHECK(fallocate(fd, 0, 0, 2 << 20) == 0 && fstat(fd, &st) == 0 && st.st_size == (2 << 20));
    FCHECK(pread(fd, got, 4096, 0) == 4096 && !memcmp(got, zero, 100) && !memcmp(got + 100, "keep", 4) && !memcmp(got + 104, zero, 4096 - 104));
    FCHECK(pread(fd, got, 4096, 4096) == 4096 && !memcmp(got, zero, 4096) && pread(fd, got, 4096, (1 << 20) - 4096) == 4096 && !memcmp(got, zero, 4096));
    FCHECK(close(fd) == 0);
    /* wine-46: seconds past the int64 nanosecond range saturate (2262) instead of wrapping to 1715 */
    put("/tmp/posix64/far", "f", 0644);
    struct timespec far[2] = {{0, UTIME_OMIT}, {10413792000LL, 5}};
    FCHECK(utimensat(AT_FDCWD, "/tmp/posix64/far", far, 0) == 0 && info("/tmp/posix64/far").st_mtime == 9223372035LL);
    FCHECK(unlink("/tmp/posix64/far") == 0);
    if (fsfix_misses_ext4) _exit(99);
    puts("[64] FSFIX EXT4 PASSED: orphan SEEK_DATA/SEEK_HOLE, st_blocks, mtime; orphan fallocate after a cache-only cut; far-future times");
}
