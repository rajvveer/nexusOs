/* /sbin/init of tests/test_x64_osinstall.py. On the USB system (root on sdX): when the NVMe disk holds no
 * installed system yet, checks nexus-os-install's refusals, requests the installation of nvme0n1 and restarts;
 * when it does, only reports. On the installed system (root on nvme0n1p3): checks the copied files, that the
 * request is gone and that the root is writable, then exits (the kernel powers off). Markers: "[64] OSTEST ...". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { printf("[64] OSTEST FAIL line=%d errno=%d (%s)\n", __LINE__, errno, #x); fflush(stdout); return 1; } } while (0)
static int run(char* const argv[], char* output, size_t capacity) {
    int pipefd[2]; if (pipe(pipefd)) return -1;
    pid_t pid = fork();
    if (!pid) { dup2(pipefd[1], 1); dup2(pipefd[1], 2); close(pipefd[0]); close(pipefd[1]); execv(argv[0], argv); _exit(127); }
    close(pipefd[1]); size_t used = 0; ssize_t n;
    while ((n = read(pipefd[0], output + used, capacity - 1 - used)) > 0) used += (size_t)n;
    output[used] = 0; close(pipefd[0]);
    int status = 0; waitpid(pid, &status, 0);
    printf("%s", output); fflush(stdout);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
static int root_source(char* out, size_t capacity) {
    FILE* f = fopen("/proc/self/mounts", "re"); if (!f) return -1;
    char line[512]; int found = -1;
    while (fgets(line, sizeof(line), f)) { char s[256], p[256]; if (sscanf(line, "%255s %255s", s, p) == 2 && !strcmp(p, "/")) { snprintf(out, capacity, "%s", s); found = 0; } }
    fclose(f); return found;
}
int main(void) {
    char root[256], text[8192];
    CHECK(root_source(root, sizeof(root)) == 0);
    printf("[64] OSTEST root=%s\n", root); fflush(stdout);
    if (!strncmp(root, "/dev/sd", 7)) {
        if (!access("/sys/block/nvme0n1/nvme0n1p3", F_OK)) { printf("[64] OSTEST LIVE with an installed disk present\n"); fflush(stdout); return 0; }
        char* list[] = {"/usr/bin/nexus-os-install", NULL};
        CHECK(run(list, text, sizeof(text)) == 0 && strstr(text, "nvme0n1") && strstr(text, "nvme1n1"));
        char self[16]; snprintf(self, sizeof(self), "%.3s", root + 5);
        char* own[] = {"/usr/bin/nexus-os-install", self, "--yes", NULL};
        CHECK(run(own, text, sizeof(text)) != 0 && strstr(text, "not a disk"));
        char* small[] = {"/usr/bin/nexus-os-install", "nvme1n1", "--yes", NULL};
        CHECK(run(small, text, sizeof(text)) != 0 && strstr(text, "too small"));
        CHECK(access("/home/nexus/.nexus-os-install", F_OK) == -1);
        printf("[64] OSTEST REFUSALS PASSED: own drive, too small disk\n"); fflush(stdout);
        int fd = open("/home/nexus/live-proof.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0 && write(fd, "written in the live session\n", 28) == 28 && fsync(fd) == 0 && close(fd) == 0);
        int encrypt = access("/nexus/osinstall-encrypt", F_OK) == 0;   /* tests/test_x64_osinstall.py case encrypt */
        char* go[] = {"/usr/bin/nexus-os-install", "nvme0n1", "--yes", "--reboot", encrypt ? "--encrypt" : NULL, NULL};
        pid_t child = fork();   /* output straight to the console: the machine restarts before the command exits */
        if (!child) { execv(go[0], go); _exit(127); }
        int status; waitpid(child, &status, 0);
        printf("[64] OSTEST FAIL reboot did not happen\n"); fflush(stdout);
        return 1;
    }
    CHECK(!strcmp(root, "/dev/nvme0n1p3") || (!strcmp(root, "/dev/mapper/cryptroot") && access("/nexus/osinstall-encrypt", F_OK) == 0));
    char data[128] = ""; int fd = open("/home/nexus/live-proof.txt", O_RDONLY);
    CHECK(fd >= 0 && read(fd, data, sizeof(data)) == 28 && !memcmp(data, "written in the live session\n", 28)); close(fd);
    CHECK(access("/home/nexus/.nexus-os-install", F_OK) == -1 && errno == ENOENT);
    CHECK(access("/usr/bin/nexus-os-install", X_OK) == 0);
    fd = open("/home/nexus/installed-proof.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "ok\n", 3) == 3 && fsync(fd) == 0 && close(fd) == 0);
    char* list[] = {"/usr/bin/nexus-os-install", NULL};
    CHECK(run(list, text, sizeof(text)) == 0);
    CHECK(run((char*[]){"/usr/bin/nexus-os-install", "sda", "--yes", NULL}, text, sizeof(text)) != 0 && strstr(text, "not from a USB drive"));
    if (!strcmp(root, "/dev/mapper/cryptroot")) {   /* the user picks a German keyboard in Aurora: the next boot prompt must use it */
        if (access("/home/nexus/.aurora/keyboard.conf", F_OK) != 0) {
            mkdir("/home/nexus/.aurora", 0755);
            fd = open("/home/nexus/.aurora/keyboard.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            CHECK(fd >= 0 && write(fd, "layouts=de\nvariants=\n", 21) == 21 && fsync(fd) == 0 && close(fd) == 0);
            printf("[64] OSTEST KEYBOARD set to de\n");
        } else printf("[64] OSTEST KEYBOARD de configured\n");
    }
    printf("[64] OSTEST INSTALLED SYSTEM PASSED: root %s, live-session file copied, request removed, root writable\n", root); fflush(stdout);
    return 0;
}
