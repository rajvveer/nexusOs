#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/vfs.h>
static int ext4_root(void) { struct statfs s; return statfs("/", &s) == 0 && s.f_type == 0xEF53; } /* case-sensitive names */

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] FS FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static void cwd(const char* expected) { char path[512]; CHECK(getcwd(path, sizeof(path)) && !strcmp(path, expected)); }
static void waited(pid_t child) { int status; CHECK(child > 0 && waitpid(child, &status, 0) == child && status == 0); }
static pid_t isolated(int pid) {
    pid_t child = syscall(SYS_clone, CLONE_NEWUSER | (pid ? CLONE_NEWPID : 0) | SIGCHLD, NULL, NULL, NULL, NULL);
    CHECK(child >= 0); return child;
}
static void* cwd_thread(void* unshare_fs) {
    if (unshare_fs) CHECK(unshare(CLONE_FS) == 0);
    CHECK(chdir(unshare_fs ? "/home/nexus" : "/tmp") == 0); return NULL;
}
static int shared_child(void* detach) {
    if (detach) CHECK(unshare(CLONE_NEWUSER) == 0);
    CHECK(chdir(detach ? "/tmp" : "/etc") == 0); return 0;
}
static long raw(long n, long a) {
    long result; __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a) : "rcx", "r11", "memory"); return result;
}
static int chroot_helper(void* unused) {
    (void)unused;
    if (raw(SYS_chroot, (long)"/proc/self/fdinfo/") || raw(SYS_chdir, (long)"/")) raw(SYS_exit, 71);
    raw(SYS_exit, 0); return 72;
}
static void mutations(void) {
    CHECK(mkdir("/tmp/native64-fs", 0700) == 0 && mkdir("/tmp/native64-fs/sub", 0700) == 0);
    CHECK(symlink("/etc/native64.dat", "/tmp/native64-fs/absolute") == 0);
    CHECK(symlink("../../../etc/native64.dat", "/tmp/native64-fs/relative") == 0);
    CHECK(symlink("/sub", "/tmp/native64-fs/dir") == 0);
    pid_t child = isolated(0);
    if (!child) {
        if (ext4_root()) CHECK(chroot("/TMP/NATIVE64-FS") == -1 && errno == ENOENT && chroot("/tmp/native64-fs") == 0 && chdir("/") == 0);
        else CHECK(chroot("/TMP/NATIVE64-FS") == 0 && chdir("/") == 0);
        cwd("/");
        CHECK(chdir("dir/..") == 0); cwd("/");
        CHECK(open("//../../absolute", O_RDONLY) == -1 && errno == ENOENT);
        int dir = open("/", O_RDONLY | O_DIRECTORY); CHECK(dir >= 0);
        CHECK(openat(dir, "sub/../../../relative", O_RDONLY) == -1 && errno == ENOENT);
        CHECK(mkdir("/made", 0700) == 0 && rmdir("/") == -1 && errno == EBUSY);
        int created = creat("/created", 0600); CHECK(created >= 0 && write(created, "abc", 3) == 3 && close(created) == 0);
        created = creat("/created", 0600); struct stat stat;
        CHECK(created >= 0 && fstat(created, &stat) == 0 && stat.st_size == 0 && close(created) == 0);
        CHECK(close(dir) == 0); _exit(0);
    }
    waited(child);
    CHECK(rmdir("/tmp/native64-fs/made") == 0);
    CHECK(unlink("/tmp/native64-fs/created") == 0);
    CHECK(unlink("/tmp/native64-fs/absolute") == 0 && unlink("/tmp/native64-fs/relative") == 0 && unlink("/tmp/native64-fs/dir") == 0);
    CHECK(rmdir("/tmp/native64-fs/sub") == 0 && rmdir("/tmp/native64-fs") == 0);
    puts("[64] FS MUTATIONS PASSED: chroot symlinks, case aliases, dot-dot boundaries and scoped writes");
}
#include "fsfix_regress_fat.h"
int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (argv[1][0] == 'd') { mutations(); fsfix_regress(); return 0; }
    if (argv[1][0] == 'e') {
        CHECK(!strcmp((const char*)getauxval(AT_EXECFN), "/bin/fs64"));
        cwd("/"); int fd = open("/token", O_RDONLY); unsigned char byte;
        CHECK(fd >= 0 && read(fd, &byte, 1) == 1 && byte == 11 && close(fd) == 0);
        CHECK(open("/etc/native64.dat", O_RDONLY) == -1 && errno == ENOENT);
        puts("[64] FS exec and interpreter stay inside inherited root PASS"); return 0;
    }
    cwd("/"); CHECK(chroot("/tmp") == -1 && errno == EPERM);
    CHECK(chdir("/etc") == 0); cwd("/etc");
    int file = open("native64.dat", O_RDONLY | O_CLOEXEC); CHECK(file >= 0 && lseek(file, 37, SEEK_SET) == 37);
    CHECK(fchdir(file) == -1 && errno == ENOTDIR && chdir("native64.dat") == -1 && errno == ENOTDIR);
    char text[512], path[80]; snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", file);
    int info = open(path, O_RDONLY); CHECK(info >= 0);
    ssize_t length = read(info, text, sizeof(text) - 1); CHECK(length > 0); text[length] = 0;
    unsigned long long position, flags, mount, inode;
    CHECK(sscanf(text, "pos:\t%llu\nflags:\t%llo\nmnt_id:\t%llu\nino:\t%llu", &position, &flags, &mount, &inode) == 4);
    CHECK(position == 37 && (flags & O_CLOEXEC) && mount && inode);
    CHECK(close(file) == 0 && lseek(info, 0, SEEK_SET) == 0 && read(info, text, sizeof(text)) == -1 && errno == ENOENT); close(info);
    CHECK(readlink("/proc/self/cwd", text, sizeof(text)) == 4 && !memcmp(text, "/etc", 4));
    CHECK(syscall(SYS_getcwd, text, 1) == -1 && errno == ERANGE);
    CHECK(syscall(SYS_getcwd, (void*)1, sizeof(text)) == -1 && errno == EFAULT);
    file = openat(-123, "/etc/native64.dat", O_RDONLY); CHECK(file >= 0); close(file);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { CHECK(chdir("/tmp") == 0); _exit(0); } waited(child); cwd("/etc");
    pthread_t thread; CHECK(pthread_create(&thread, NULL, cwd_thread, NULL) == 0 && pthread_join(thread, NULL) == 0); cwd("/tmp");
    CHECK(pthread_create(&thread, NULL, cwd_thread, (void*)1) == 0 && pthread_join(thread, NULL) == 0); cwd("/tmp");
    char stack[32768] __attribute__((aligned(16)));
    waited(clone(shared_child, stack + sizeof(stack), CLONE_FS | SIGCHLD, NULL)); cwd("/etc");
    waited(clone(shared_child, stack + sizeof(stack), CLONE_FS | SIGCHLD, (void*)1)); cwd("/etc");
    CHECK(chdir("/") == 0);
    child = isolated(0);
    if (!child) {
        int outside = open("/", O_RDONLY | O_DIRECTORY); CHECK(outside >= 0);
        int outside_proc = open("/proc", O_RDONLY | O_DIRECTORY); CHECK(outside_proc >= 0);
        int retained = open("/etc/native64.dat", O_RDONLY); CHECK(retained >= 0);
        CHECK(chroot("/etc") == 0);
        CHECK(syscall(SYS_getcwd, text, sizeof(text)) > 0 && !strncmp(text, "(unreachable)", 13));
        CHECK(chdir("/") == 0 && chdir("../../../") == 0); cwd("/");
        CHECK(access("/native64.dat", R_OK) == 0 && open("/etc/native64.dat", O_RDONLY) == -1 && errno == ENOENT);
        CHECK(readlink("/", text, sizeof(text)) == -1 && errno == EINVAL);
        CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EPERM);
        int old_file = openat(outside, "etc/native64.dat", O_RDONLY); CHECK(old_file >= 0); close(old_file);
        snprintf(path, sizeof(path), "self/fd/%d/etc/native64.dat", outside);
        old_file = openat(outside_proc, path, O_RDONLY); CHECK(old_file >= 0); close(old_file);
        old_file = openat(outside_proc, "self/root/native64.dat", O_RDONLY); CHECK(old_file >= 0); close(old_file);
        CHECK(readlinkat(outside_proc, "self/root", text, sizeof(text)) == 1 && text[0] == '/');
        CHECK(fchdir(outside) == 0 && chdir("/") == 0); cwd("/");
        CHECK(read(retained, text, 1) == 1 && (unsigned char)text[0] == 11); close(retained); close(outside); close(outside_proc); _exit(0);
    }
    waited(child); cwd("/");
    child = isolated(0);
    if (!child) {
        CHECK(chroot("/nexus/fs-root") == 0 && chdir("/") == 0);
        char* args[] = {"/bin/fs64", "e", NULL}; char* env[] = {NULL}; execve(args[0], args, env); CHECK(0);
    }
    waited(child);
    child = isolated(1);
    if (!child) {
        int proc = open("/proc", O_RDONLY | O_DIRECTORY); CHECK(proc >= 0);
        char tls[16384] __attribute__((aligned(16))) = {0};
        waited(clone(chroot_helper, stack + sizeof(stack), CLONE_VM | CLONE_VFORK | CLONE_FS | CLONE_SETTLS | SIGCHLD, NULL, NULL, tls, NULL));
        CHECK(access("/proc", F_OK) == -1 && errno == ENOENT);
        CHECK(open("/../../etc/native64.dat", O_RDONLY) == -1 && errno == ENOENT);
        struct stat st; CHECK(stat("/", &st) == 0 && S_ISDIR(st.st_mode) && st.st_nlink == 0);
        CHECK(access("/", F_OK) == 0 && getcwd(text, sizeof(text)) == NULL && errno == ENOENT);
        int root = open("/", O_RDONLY | O_DIRECTORY); CHECK(root >= 0 && fstat(root, &st) == 0);
        CHECK(openat(root, "../etc/native64.dat", O_RDONLY) == -1 && errno == ENOENT); close(root);
        int info_dir = openat(proc, "self/fdinfo", O_RDONLY | O_DIRECTORY); CHECK(info_dir >= 0); close(info_dir);
        close(proc); _exit(0);
    }
    waited(child);
    puts("[64] FS ALL PASSED: cwd sharing, fdinfo, real chroot, retained directories, magic links and Chrome helper lifetime");
    return 0;
}
