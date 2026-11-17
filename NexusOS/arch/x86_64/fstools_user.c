/* /sbin/init of tests/test_x64_fstools.py: runs storage test scripts with Debian's tools as root, and fuzzes
 * filesystem images through loop devices.
 *
 * /nexus/fstools-mode holds lines "script PATH" (dash PATH as root) and "fuzz TYPE IMAGE ROUNDS" (ROUNDS damaged
 * copies of IMAGE, each attached with LOOP_CONFIGURE, mounted as TYPE, walked (readdir, stat, read, readlink),
 * unmounted and detached; a refused mount is fine, a fault is not). The program re-executes itself through the
 * set-user-ID copy /bin/fstools-root when started as the desktop user. Markers: "[64] FSTOOLS ...". */
#define _GNU_SOURCE
#include <dirent.h>
#include <grp.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t state = 0x9E3779B97F4A7C15ULL;
static uint64_t next_random(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static int run_script(const char* path) {
    printf("[64] FSTOOLS SCRIPT %s\n", path); fflush(stdout);
    pid_t pid = fork();
    if (!pid) {
        char* argv[] = {"dash", (char*)path, NULL};
        char* envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "TERM=dumb", "LANG=C.UTF-8", "LC_ALL=C.UTF-8", NULL};
        execve("/usr/bin/dash", argv, envp); execve("/bin/dash", argv, envp); _exit(127);
    }
    int status = 0; waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    printf("[64] FSTOOLS SCRIPT DONE %s status=%d\n", path, code); fflush(stdout);
    return code;
}
static unsigned walked, read_bytes;
static void walk(const char* path, unsigned depth) {
    if (depth > 12) return;
    DIR* d = opendir(path); if (!d) return;
    struct dirent* e; unsigned entries = 0;
    while ((e = readdir(d)) && entries++ < 4096) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096]; if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) continue;
        struct stat st; if (lstat(child, &st)) continue;
        walked++;
        if (S_ISDIR(st.st_mode)) walk(child, depth + 1);
        else if (S_ISLNK(st.st_mode)) { char target[256]; (void)readlink(child, target, sizeof(target)); }
        else if (S_ISREG(st.st_mode)) {
            int fd = open(child, O_RDONLY); if (fd < 0) continue;
            char buffer[16384]; ssize_t n; unsigned total = 0;
            while (total < 262144 && (n = read(fd, buffer, sizeof(buffer))) > 0) total += (unsigned)n;
            read_bytes += total; close(fd);
        }
    }
    closedir(d);
}
static int fuzz(const char* type, const char* image, unsigned rounds) {
    int src = open(image, O_RDONLY); if (src < 0) { printf("[64] FSTOOLS FAIL fuzz: open %s errno=%d\n", image, errno); return 1; }
    struct stat st; fstat(src, &st);
    size_t size = (size_t)st.st_size; unsigned char* data = malloc(size), *copy = malloc(size);
    if (!data || !copy || pread(src, data, size, 0) != (ssize_t)size) { printf("[64] FSTOOLS FAIL fuzz: read image\n"); return 1; }
    close(src);
    mkdir("/mnt", 0755); mkdir("/mnt/fuzz", 0755);
    unsigned mounted = 0, refused = 0;
    for (unsigned round = 0; round < rounds; round++) {
        memcpy(copy, data, size);
        unsigned flips = 1 + next_random() % 24;
        for (unsigned i = 0; i < flips; i++) {   /* mostly metadata: the first 2 MiB, sometimes anywhere */
            size_t span = (next_random() & 3) ? (size < (2u << 20) ? size : (2u << 20)) : size;
            size_t at = next_random() % span;
            copy[at] = (next_random() & 1) ? (unsigned char)next_random() : (unsigned char)(copy[at] ^ (1u << (next_random() % 8)));
        }
        int fd = open("/tmp/fuzz.img", O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || write(fd, copy, size) != (ssize_t)size) { printf("[64] FSTOOLS FAIL fuzz: write copy\n"); return 1; }
        int control = open("/dev/loop-control", O_RDWR); int number = control >= 0 ? ioctl(control, LOOP_CTL_GET_FREE) : -1;
        if (number < 0) { printf("[64] FSTOOLS FAIL fuzz: LOOP_CTL_GET_FREE errno=%d\n", errno); return 1; }
        char loop[32]; snprintf(loop, sizeof(loop), "/dev/loop%d", number);
        int dev = open(loop, O_RDWR);
        struct loop_config config; memset(&config, 0, sizeof(config)); config.fd = (uint32_t)fd; config.info.lo_flags = LO_FLAGS_AUTOCLEAR;
        if (dev < 0 || ioctl(dev, LOOP_CONFIGURE, &config)) { printf("[64] FSTOOLS FAIL fuzz: LOOP_CONFIGURE errno=%d\n", errno); return 1; }
        close(fd);
        if (mount(loop, "/mnt/fuzz", type, MS_NOSUID | MS_NODEV | (round & 1 ? MS_RDONLY : 0), "") == 0) {
            mounted++; walk("/mnt/fuzz", 0);
            if (!(round & 1)) {   /* writes on damaged media must fail cleanly or work */
                int w = open("/mnt/fuzz/fuzz-write.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (w >= 0) { (void)write(w, "fuzz\n", 5); close(w); }
                mkdir("/mnt/fuzz/fuzzdir", 0755); unlink("/mnt/fuzz/fuzz-write.txt");
            }
            if (umount("/mnt/fuzz")) { printf("[64] FSTOOLS FAIL fuzz: umount errno=%d\n", errno); return 1; }
        } else refused++;
        close(dev); close(control);   /* autoclear detaches the loop at the last close */
    }
    unlink("/tmp/fuzz.img"); free(data); free(copy);
    printf("[64] FSTOOLS FUZZ PASSED type=%s rounds=%u mounted=%u refused=%u entries=%u bytes=%u\n", type, rounds, mounted, refused, walked, read_bytes);
    fflush(stdout); return 0;
}
int main(int argc, char** argv) {
    /* "fstools-root rename FROM TO": rename(2) itself (coreutils mv refuses a case-only rename as "the same file",
     * as on Linux vfat, after renameat2(RENAME_NOREPLACE) said EEXIST) */
    if (argc == 4 && !strcmp(argv[1], "rename")) { if (rename(argv[2], argv[3])) { perror("rename"); return 1; } return 0; }
    if (argc > 4 && !strcmp(argv[1], "connect")) {   /* "connect SERVER ARGS -- CLIENT ARGS": two programs over a pipe pair */
        int split = 0; for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--")) { split = i; break; }
        if (!split || split == 2 || split == argc - 1) return 2;
        int a[2], b[2]; if (pipe(a) || pipe(b)) { perror("pipe"); return 1; }
        argv[split] = NULL;
        pid_t server = fork();
        if (!server) { dup2(a[0], 0); dup2(b[1], 1); close(a[0]); close(a[1]); close(b[0]); close(b[1]); execv(argv[2], argv + 2); _exit(127); }
        pid_t client = fork();
        if (!client) { dup2(b[0], 0); dup2(a[1], 1); close(a[0]); close(a[1]); close(b[0]); close(b[1]); execv(argv[split + 1], argv + split + 1); _exit(127); }
        close(a[0]); close(a[1]); close(b[0]); close(b[1]);
        int status = 0; waitpid(client, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;   /* the server keeps serving the (daemonized) client */
    }
    if (argc == 4 && !strcmp(argv[1], "open")) {   /* "open PATH FLAGS(hex)": diagnostics for scripts */
        int fd = open(argv[2], (int)strtol(argv[3], NULL, 16)); int saved = errno; struct stat st; memset(&st, 0, sizeof(st));
        if (fd >= 0) fstat(fd, &st);
        printf("open %s 0x%s -> fd=%d errno=%d mode=%o dev=%lx ino=%lx\n", argv[2], argv[3], fd, fd < 0 ? saved : 0, st.st_mode, (unsigned long)st.st_dev, (unsigned long)st.st_ino);
        return fd < 0;
    }
    if (argc == 5 && !strcmp(argv[1], "readcmp")) {   /* "readcmp DEVICE FILE ROUNDS": random preads agree byte for byte */
        int a = open(argv[2], O_RDONLY), b = open(argv[3], O_RDONLY); unsigned rounds = (unsigned)atoi(argv[4]);
        struct stat st; if (a < 0 || b < 0 || fstat(b, &st)) { perror("readcmp"); return 1; }
        static unsigned char x[300000], y[300000];
        for (unsigned i = 0; i < rounds; i++) {
            uint64_t length = 1 + next_random() % (i & 1 ? 300000 : 5000), offset = next_random() % (uint64_t)st.st_size;
            if (offset + length > (uint64_t)st.st_size) length = (uint64_t)st.st_size - offset;
            ssize_t p = pread(a, x, length, (off_t)offset), q = pread(b, y, length, (off_t)offset);
            if (p != q || p != (ssize_t)length || memcmp(x, y, length)) {
                size_t at = 0; while (at < length && x[at] == y[at]) at++;
                printf("readcmp: offset %llu length %llu: device %zd file %zd first difference at +%zu\n", (unsigned long long)offset, (unsigned long long)length, p, q, at);
                return 1;
            }
        }
        printf("readcmp: %u random reads agree\n", rounds); return 0;
    }
    if (argc > 4 && !strcmp(argv[1], "as")) {   /* "as UID GID PROGRAM ARGS": drop to that user (setpriv needs PR_SET_KEEPCAPS) */
        uid_t uid = (uid_t)atoi(argv[2]); gid_t gid = (gid_t)atoi(argv[3]);
        if (setgroups(0, NULL) || setresgid(gid, gid, gid) || setresuid(uid, uid, uid)) { perror("as"); return 1; }
        execvp(argv[4], argv + 4); perror(argv[4]); return 127;
    }
    if (argc > 1) { fprintf(stderr, "fstools: unknown command %s\n", argv[1]); return 2; }   /* only init runs the mode file */
    if (geteuid() != 0 && access("/bin/fstools-root", X_OK) == 0) { execv("/bin/fstools-root", argv); perror("fstools-root"); return 1; }
    if (setresgid(0, 0, 0) || setresuid(0, 0, 0)) { printf("[64] FSTOOLS FAIL setresuid errno=%d\n", errno); return 1; }
    FILE* f = fopen("/nexus/fstools-mode", "r"); if (!f) { printf("[64] FSTOOLS FAIL no /nexus/fstools-mode\n"); return 1; }
    char line[512]; int failures = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char a[64], b[256], c[256]; unsigned n = 0;
        if (sscanf(line, "script %255s", b) == 1) { if (run_script(b)) failures++; }
        else if (sscanf(line, "fuzz %63s %255s %u", a, b, &n) == 3) { if (fuzz(a, b, n)) failures++; }
        else if (sscanf(line, "seed %255s", c) == 1) state = strtoull(c, NULL, 0) | 1;
    }
    fclose(f);
    printf(failures ? "[64] FSTOOLS FAILED %d\n" : "[64] FSTOOLS ALL PASSED\n", failures); fflush(stdout);
    kill(-1, SIGKILL); while (wait(NULL) > 0) {}   /* daemons a failed script left behind (FUSE) must not keep the VM up */
    sync();
    return failures ? 1 : 0;
}
