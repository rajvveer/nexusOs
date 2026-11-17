#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] STATFS FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static int ext4_root(void) { struct statfs s; return statfs("/", &s) == 0 && s.f_type == 0xEF53; }
static void basics(void) {
    struct statfs root, home, value;
    /* FAT32 reports the non-user areas read-only; an ext4 root is writable and enforces modes instead */
    CHECK(statfs("/", &root) == 0 && (root.f_type == 0x4D44 || root.f_type == 0xEF53) && root.f_blocks && root.f_bfree <= root.f_blocks && root.f_bavail == root.f_bfree);
    CHECK(root.f_bsize >= 512 && root.f_frsize == root.f_bsize && root.f_namelen == 255 && (ext4_root() ? !(root.f_flags & ST_RDONLY) : (root.f_flags & ST_RDONLY)));
    CHECK(statfs("/home/nexus", &home) == 0 && !memcmp(&root.f_fsid, &home.f_fsid, sizeof(root.f_fsid)) && !(home.f_flags & ST_RDONLY));
    int fd = open("/etc/native64.dat", O_RDONLY); CHECK(fd >= 0 && fstatfs(fd, &value) == 0 && value.f_blocks == root.f_blocks); close(fd);
    CHECK(statfs("/proc/self", &value) == 0 && value.f_type == 0x9FA0 && value.f_blocks == 0);
    CHECK(statfs("/proc/self/ns/user", &value) == 0 && value.f_type == 0x6E736673);
    fd = open("/dev/null", O_RDWR); CHECK(fd >= 0 && fstatfs(fd, &value) == 0 && value.f_type == 0x01021994 && (value.f_flags & ST_RDONLY)); close(fd);
    fd = memfd_create("statfs", 0); CHECK(fd >= 0 && fstatfs(fd, &value) == 0 && value.f_type == 0x01021994 && value.f_blocks == 0 && !(value.f_flags & ST_RDONLY)); close(fd);
    int pair[2]; CHECK(pipe(pair) == 0 && fstatfs(pair[0], &value) == 0 && value.f_type == 0x50495045); close(pair[0]); close(pair[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && fstatfs(pair[0], &value) == 0 && value.f_type == 0x534F434B); close(pair[0]); close(pair[1]);
    fd = epoll_create1(0); CHECK(fd >= 0 && fstatfs(fd, &value) == 0 && value.f_type == 0x09041934); close(fd);
    struct statvfs portable; CHECK(statvfs("/home/nexus", &portable) == 0 && portable.f_blocks == home.f_blocks && portable.f_frsize == (unsigned long)home.f_frsize);
    CHECK(statfs("/missing-volume-path", &value) == -1 && errno == ENOENT);
    CHECK(fstatfs(-1, &value) == -1 && errno == EBADF);
    CHECK(syscall(SYS_statfs, "/", (void*)1) == -1 && errno == EFAULT);
    puts("[64] STATFS API PASSED: real FAT geometry, mount views and descriptor filesystem identities");
}
static void mutations(void) {
    struct statfs baseline, grown, shrunk, final;
    int fd = open("/tmp/native64-space", O_RDWR | O_CREAT | O_EXCL, 0600); CHECK(fd >= 0 && fstatfs(fd, &baseline) == 0);
    if (ext4_root()) { /* ext4 extends sparsely: blocks are allocated by data, not by the new size */
        CHECK(ftruncate(fd, baseline.f_bsize * 3 + 1) == 0 && fstatfs(fd, &grown) == 0 && grown.f_bfree == baseline.f_bfree);
        for (int i = 0; i < 4; i++) CHECK(pwrite(fd, "x", 1, (off_t)baseline.f_bsize * i) == 1);
    } else CHECK(ftruncate(fd, baseline.f_bsize * 3 + 1) == 0);
    CHECK(fstatfs(fd, &grown) == 0 && grown.f_bfree == baseline.f_bfree - 4);
    CHECK(ftruncate(fd, baseline.f_bsize + 1) == 0 && fstatfs(fd, &shrunk) == 0 && shrunk.f_bfree == baseline.f_bfree - 2);
    CHECK(ftruncate(fd, 0) == 0 && fstatfs(fd, &final) == 0 && final.f_bfree == baseline.f_bfree);
    CHECK(close(fd) == 0 && unlink("/tmp/native64-space") == 0);
    fd = open("/home/nexus/native64-persist/stats.bin", O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 24) == 0 && fstatfs(fd, &final) == 0);
    uint64_t values[] = {final.f_frsize, final.f_blocks, final.f_bfree};
    CHECK(pwrite(fd, values, sizeof(values), 0) == sizeof(values) && fsync(fd) == 0 && close(fd) == 0);
    puts("[64] STATFS COUNTS PASSED: allocation/free deltas and persisted volume snapshot");
}
static void readonly(void) {
    const char* path = "/home/nexus/readonly.dat"; struct statfs value;
    CHECK(statfs("/home/nexus", &value) == 0 && (value.f_flags & ST_RDONLY));
    int fd = open(path, O_RDONLY); char byte;
    CHECK(fd >= 0 && fstatfs(fd, &value) == 0 && (value.f_flags & ST_RDONLY));
    CHECK(read(fd, &byte, 1) == 1 && byte == '1' && close(fd) == 0);
    CHECK(open(path, O_RDWR) == -1 && errno == EROFS);
    CHECK(open(path, O_RDONLY | O_TRUNC) == -1 && errno == EROFS);
    CHECK(open("/tmp/new", O_WRONLY | O_CREAT, 0600) == -1 && errno == EROFS);
    CHECK(mkdir("/tmp/new", 0700) == -1 && errno == EROFS);
    CHECK(unlink(path) == -1 && errno == EROFS);
    CHECK(symlink(path, "/tmp/new") == -1 && errno == EROFS);
    fd = memfd_create("still-writable", 0);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
    puts("[64] STATFS READONLY PASSED: dirty FAT readable, data/namespace writes refused, memfd unaffected");
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    if (argv[1][0] == 'd') mutations(); else if (argv[1][0] == 'r') readonly(); else basics();
    return 0;
}
