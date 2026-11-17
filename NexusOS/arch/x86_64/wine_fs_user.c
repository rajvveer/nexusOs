/* What a Wine prefix needs from the NexusOS root filesystem (FAT32 with NexusOS symlinks), checked from user space:
 * WINEPREFIX layout with dosdevices/c: -> ../drive_c and z: -> / symlinks (names containing ':'), paths through those
 * links, readlink/lstat, case-insensitive lookups, directory listing of reserved-character names, rename, fcntl
 * record locks (wineserver's lock file), a multi-megabyte file and statfs. Prints [64] WINEFS ... lines; exit 0 = pass. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

static char base[256];
#define CHECK(x) do { if (!(x)) { printf("[64] WINEFS FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); fflush(stdout); exit(1); } } while (0)
static const char* at(const char* relative) { static char path[2][512]; static int k; k ^= 1; snprintf(path[k], sizeof(path[k]), "%s/%s", base, relative); return path[k]; }
static void pass(const char* what) { printf("[64] WINEFS %s PASSED\n", what); fflush(stdout); }
static bool listed(const char* dir, const char* name) {
    DIR* d = opendir(dir); CHECK(d); struct dirent* e; bool found = false;
    while ((e = readdir(d))) if (!strcmp(e->d_name, name)) found = true;
    closedir(d); return found;
}
int main(void) {
    const char* home = getenv("HOME"); if (!home) home = "/home/nexus";
    snprintf(base, sizeof(base), "%s/.wine-fstest-%d", home, (int)getpid());
    CHECK(!mkdir(base, 0755) && !mkdir(at("drive_c"), 0755) && !mkdir(at("drive_c/windows"), 0755) && !mkdir(at("dosdevices"), 0755));
    /* dosdevices symlinks exactly as wineboot creates them */
    CHECK(!symlink("../drive_c", at("dosdevices/c:")) && !symlink("/", at("dosdevices/z:")));
    char target[256]; ssize_t n = readlink(at("dosdevices/c:"), target, sizeof(target) - 1);
    if (n >= 0) target[n] = 0;
    printf("[64] WINEFS readlink c: = %zd '%s'\n", n, n >= 0 ? target : ""); fflush(stdout);
    CHECK(n == 10 && !strcmp(target, "../drive_c"));
    struct stat st; CHECK(!lstat(at("dosdevices/c:"), &st) && S_ISLNK(st.st_mode)); CHECK(!stat(at("dosdevices/c:"), &st) && S_ISDIR(st.st_mode));
    CHECK(!stat(at("dosdevices/z:/home"), &st) && S_ISDIR(st.st_mode));
    CHECK(listed(at("dosdevices"), "c:") && listed(at("dosdevices"), "z:"));
    pass("SYMLINK c: z:");
    /* files through the drive link, then case-insensitive lookups (Windows semantics on top of FAT) */
    int fd = open(at("dosdevices/c:/windows/win.ini"), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    CHECK(write(fd, "[wine]\n", 7) == 7 && !close(fd));
    CHECK(!stat(at("drive_c/windows/win.ini"), &st) && st.st_size == 7);
    char text[16] = {0}; fd = open(at("DOSDEVICES/C:/WINDOWS/WIN.INI"), O_RDONLY | O_CLOEXEC); CHECK(fd >= 0); CHECK(read(fd, text, sizeof(text)) == 7 && !close(fd));
    CHECK(!memcmp(text, "[wine]\n", 7));
    pass("CASE-INSENSITIVE LOOKUP");
    CHECK(!rename(at("drive_c/windows/win.ini"), at("drive_c/windows/System.INI")) && !stat(at("dosdevices/c:/windows/system.ini"), &st) && st.st_size == 7);
    CHECK(listed(at("drive_c/windows"), "System.INI") && !listed(at("drive_c/windows"), "win.ini"));
    pass("RENAME");
    /* every character FAT reserves round-trips in names (Windows itself forbids them; Wine only needs ':') */
    const char* reserved = "\"*:<>?|";   /* backslash is refused by the kernel's path layer (EINVAL); Windows paths never contain one in a name */
    fd = open(at("a\\b"), O_CREAT | O_WRONLY | O_CLOEXEC, 0644); CHECK(fd < 0 && errno == EINVAL);
    for (const char* c = reserved; *c; c++) {
        char odd[16]; snprintf(odd, sizeof(odd), "a%cb", *c);
        fd = open(at(odd), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        if (fd < 0) { printf("[64] WINEFS name with '%c' rejected errno=%d\n", *c, errno); fflush(stdout); }
        CHECK(fd >= 0 && !close(fd));
        CHECK(listed(base, odd) && !stat(at(odd), &st) && !unlink(at(odd)) && !listed(base, odd));
    }
    pass("RESERVED CHARACTERS");
    /* wineserver: record lock on its lock file */
    fd = open(at("lock"), O_CREAT | O_RDWR | O_CLOEXEC, 0600); CHECK(fd >= 0);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    CHECK(!fcntl(fd, F_SETLK, &lock));
    struct flock query = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    CHECK(!fcntl(fd, F_GETLK, &query));
    CHECK(!close(fd));
    pass("FCNTL LOCK");
    /* a registry-sized file (8 MiB) written and read back; FAT32 caps files at 4 GiB - 1 */
    static char block[65536]; for (size_t i = 0; i < sizeof(block); i++) block[i] = (char)(i * 7);
    fd = open(at("system.reg"), O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644); CHECK(fd >= 0);
    for (int i = 0; i < 128; i++) CHECK(write(fd, block, sizeof(block)) == (ssize_t)sizeof(block));
    CHECK(!fstat(fd, &st) && st.st_size == 128 * 65536);
    static char back[65536]; CHECK(pread(fd, back, sizeof(back), 77 * 65536) == (ssize_t)sizeof(back) && !memcmp(back, block, sizeof(back)));
    CHECK(!ftruncate(fd, 1000) && !fstat(fd, &st) && st.st_size == 1000 && !close(fd));
    pass("LARGE FILE");
    struct statfs fs; CHECK(!statfs(base, &fs) && fs.f_bsize > 0 && fs.f_bavail > 0);
    printf("[64] WINEFS STATFS type=0x%lx bsize=%ld free_mib=%llu\n", (long)fs.f_type, (long)fs.f_bsize, (unsigned long long)fs.f_bavail * fs.f_bsize >> 20);
    /* clean up */
    CHECK(!unlink(at("system.reg")) && !unlink(at("lock")) && !unlink(at("drive_c/windows/System.INI")));
    CHECK(!unlink(at("dosdevices/c:")) && !unlink(at("dosdevices/z:")) && !rmdir(at("dosdevices")));
    CHECK(!rmdir(at("drive_c/windows")) && !rmdir(at("drive_c")) && !rmdir(base));
    puts("[64] WINEFS PASSED"); fflush(stdout); return 0;
}
