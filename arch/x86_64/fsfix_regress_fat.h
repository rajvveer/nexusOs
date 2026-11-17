/* fs fix round (wine-7, wine-45, wine-8, wine-46, wine-24): included by fs_user.c in mode 'd' (both roots). */
#include <sys/xattr.h>
static int fsfix_misses;
#define FCHECK(test) do { if (!(test)) { dprintf(2, "[64] FSFIX MISS line=%d errno=%d\n", __LINE__, errno); fsfix_misses++; } } while (0)
static void fsfix_regress(void) {
    const char* name = "/tmp/native64-fsfix"; char path[64], got[16]; struct stat st;
    if (!ext4_root()) {
        /* truncate("/proc/self/fd/N") and fchmod act on the unlinked file, never on a new file at its old name */
        int old = open(name, O_CREAT | O_RDWR | O_TRUNC, 0644); FCHECK(old >= 0 && write(old, "old-data", 8) == 8);
        FCHECK(unlink(name) == 0);
        int fresh = open(name, O_CREAT | O_RDWR | O_EXCL, 0644);
        FCHECK(fresh >= 0 && write(fresh, "new-data", 8) == 8 && fsync(fresh) == 0);
        snprintf(path, sizeof(path), "/proc/self/fd/%d", old);
        FCHECK(truncate(path, 3) == 0);
        FCHECK(fstat(old, &st) == 0 && st.st_size == 3 && st.st_nlink == 0 && pread(old, got, 8, 0) == 3 && !memcmp(got, "old", 3));
        FCHECK(fstat(fresh, &st) == 0 && st.st_size == 8 && pread(fresh, got, 8, 0) == 8 && !memcmp(got, "new-data", 8));
        FCHECK(stat(name, &st) == 0 && st.st_size == 8);
        FCHECK(fchmod(old, 0500) == 0 && fstat(old, &st) == 0 && (st.st_mode & 0777) == 0500);
        FCHECK(stat(name, &st) == 0 && (st.st_mode & 0200));
        int again = open(name, O_WRONLY); FCHECK(again >= 0 && close(again) == 0);
        FCHECK(fchmod(old, 0700) == 0 && fstat(old, &st) == 0 && (st.st_mode & 0777) == 0700);
        FCHECK(close(old) == 0 && close(fresh) == 0);
        /* FAT write times: 2107 and 2300 clamp to the newest stamp that reads back (2106-02-07 06:28:14) */
        int f = open(name, O_RDWR); FCHECK(f >= 0);
        struct timespec far[2] = {{0, UTIME_OMIT}, {4354819198LL, 0}};
        FCHECK(futimens(f, far) == 0 && fstat(f, &st) == 0 && st.st_mtime == 4294967294LL);
        far[1].tv_sec = 10413792000LL;
        FCHECK(utimensat(AT_FDCWD, name, far, 0) == 0 && stat(name, &st) == 0 && st.st_mtime == 4294967294LL);
        FCHECK(close(f) == 0 && stat(name, &st) == 0 && st.st_mtime == 4294967294LL);
        far[1].tv_sec = 1300000000LL;
        FCHECK(utimensat(AT_FDCWD, name, far, 0) == 0 && stat(name, &st) == 0 && st.st_mtime == 1300000000LL);
        struct stat out; FCHECK(fstat(1, &out) == 0);
        dprintf(2, "[64] FSFIX stdout dev=%llu mode=%o\n", (unsigned long long)out.st_dev, (unsigned)out.st_mode);
        FCHECK(fchown(1, -1, -1) == 0 || errno == EPERM || errno == EROFS);   /* a console descriptor has no disk path */
        if (!fsfix_misses) puts("[64] FSFIX FAT PASSED: truncate/fchmod through an unlinked descriptor, far-future FAT times");
    } else { int f = open(name, O_CREAT | O_RDWR | O_TRUNC, 0644); FCHECK(f >= 0 && close(f) == 0); }
    /* xattr names: XATTR_NAME_MAX is 255; longer or empty is ERANGE (and never overruns the kernel buffer) */
    char attr[600]; memset(attr, 'n', sizeof(attr)); memcpy(attr, "user.", 5);
    attr[511] = 0; FCHECK(setxattr(name, attr, "v", 1, 0) == -1 && errno == ERANGE);
    attr[300] = 0; FCHECK(setxattr(name, attr, "v", 1, 0) == -1 && errno == ERANGE && removexattr(name, attr) == -1 && errno == ERANGE);
    attr[256] = 0; FCHECK(lsetxattr(name, attr, "v", 1, 0) == -1 && errno == ERANGE);
    attr[255] = 0; FCHECK(setxattr(name, attr, "v", 1, 0) == -1 && errno == EOPNOTSUPP);
    FCHECK(setxattr(name, "", "v", 1, 0) == -1 && errno == ERANGE);
    int f = open(name, O_RDONLY); memset(attr + 5, 'n', sizeof(attr) - 5); attr[400] = 0;
    FCHECK(f >= 0 && fsetxattr(f, attr, "v", 1, 0) == -1 && errno == ERANGE && fremovexattr(f, attr) == -1 && errno == ERANGE && close(f) == 0);
    FCHECK(unlink(name) == 0);
    if (fsfix_misses) _exit(99);
    puts("[64] FSFIX XATTR PASSED: 256..511-byte and empty names are ERANGE");
}
