#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/vfs.h>
/* FAT32 roots refuse writes outside the user's areas with EROFS; ext4 roots enforce modes (EACCES). */
static int denied_errno(void) { struct statfs s; return statfs("/", &s) == 0 && s.f_type == 0xEF53 ? EACCES : EROFS; }

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] RENAME FAIL line=%d errno=%d pid=%d\n", __LINE__, errno, getpid()); _exit(99); } } while (0)
static const char* root = "/tmp/native64-rename";
static const volatile unsigned char lazy[16384] __attribute__((aligned(4096))) = {[12288] = 0xA5};
static int create(const char* path, char byte) {
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0700);
    CHECK(fd >= 0 && ftruncate(fd, 4096) == 0 && pwrite(fd, &byte, 1, 0) == 1); return fd;
}
static struct stat info(int fd) { struct stat st; CHECK(fstat(fd, &st) == 0); return st; }
static void first(int fd, char expected) { char byte; CHECK(pread(fd, &byte, 1, 0) == 1 && byte == expected); }
static void waited(pid_t child) { int status; CHECK(waitpid(child, &status, 0) == child && status == 0); }
static void receive(int fd) { char byte; CHECK(read(fd, &byte, 1) == 1); }
static void fd_link(int fd, const char* expected) {
    char path[64], value[512] = {0}; snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    CHECK(readlink(path, value, sizeof(value) - 1) == (ssize_t)strlen(expected) && !strcmp(value, expected));
}
static void files(void) {
    int source = create("left/source", 'S'), target = create("right/target", 'T');
    struct stat old_source = info(source), old_target = info(target);
    char* a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, source, 0);
    char* b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, target, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED && a[0] == 'S' && b[0] == 'T');
    CHECK(flock(source, LOCK_EX) == 0);
    CHECK(rename("left/source", "right/target") == 0);
    CHECK(info(source).st_ino == old_source.st_ino && info(source).st_nlink == 1);
    CHECK(info(target).st_ino == old_target.st_ino && info(target).st_nlink == 0);
    int current = open("right/target", O_RDWR); CHECK(current >= 0 && info(current).st_ino == old_source.st_ino);
    CHECK(flock(current, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    fd_link(source, "/tmp/native64-rename/right/target"); fd_link(target, "/tmp/native64-rename/right/target (deleted)");
    a[0] = 'A'; b[0] = 'B'; CHECK(msync(a, 4096, MS_SYNC) == 0 && msync(b, 4096, MS_SYNC) == 0);
    first(source, 'A'); first(current, 'A'); first(target, 'B');
    CHECK(unlink("right/target") == 0 && info(source).st_nlink == 0);
    int fresh = create("right/target", 'N');
    CHECK(info(fresh).st_ino != old_source.st_ino && info(fresh).st_ino != old_target.st_ino);
    char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", source);
    int reopened = open(path, O_RDONLY); CHECK(reopened >= 0 && info(reopened).st_ino == old_source.st_ino && info(reopened).st_nlink == 0);
    first(reopened, 'A'); CHECK(close(reopened) == 0 && close(source) == 0 && close(target) == 0 && close(current) == 0);
    CHECK(a[0] == 'A' && b[0] == 'B'); a[0] = 'C'; b[0] = 'D';
    CHECK(msync(a, 4096, MS_SYNC) == 0 && msync(b, 4096, MS_SYNC) == 0);
    first(fresh, 'N'); CHECK(munmap(a, 4096) == 0 && munmap(b, 4096) == 0 && close(fresh) == 0 && unlink("right/target") == 0);
    int left = open("left", O_RDONLY | O_DIRECTORY), right = open("right", O_RDONLY | O_DIRECTORY);
    CHECK(left >= 0 && right >= 0); source = create("left/again", 'a'); CHECK(close(source) == 0);
    CHECK(renameat(left, "again", right, "moved") == 0);
    CHECK(renameat2(right, "moved", right, "moved", RENAME_NOREPLACE) == -1 && errno == EEXIST);
    CHECK(renameat2(right, "moved", left, "again", RENAME_NOREPLACE) == 0);
    CHECK(syscall(SYS_rename, (void*)1, "bad") == -1 && errno == EFAULT);
    CHECK(rename("left/again", "/etc/forbidden") == -1 && errno == denied_errno());
    CHECK(rename("left/again", "missing/child") == -1 && errno == ENOENT);
    CHECK(rename("left/again", "left/again") == 0 && unlinkat(left, "again", 0) == 0);
    CHECK(close(left) == 0 && close(right) == 0);
    puts("[64] RENAME FILES PASSED: stable identity, live replacement, shared maps, locks, deleted reopen and slot reuse");
}
static void directories(void) {
    CHECK(mkdir("left/tree", 0700) == 0 && mkdir("left/tree/deep", 0700) == 0);
    int fd = create("left/tree/deep/data", 'd'); CHECK(close(fd) == 0);
    int ready[2], gate[2]; CHECK(pipe(ready) == 0 && pipe(gate) == 0);
    pid_t child = syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, NULL, NULL, NULL, NULL); CHECK(child >= 0);
    if (!child) {
        CHECK(chroot("left/tree") == 0 && chdir("/deep") == 0 && write(ready[1], "r", 1) == 1);
        receive(gate[0]); char cwd[512]; CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/deep"));
        int data = open("../deep/data", O_RDONLY); CHECK(data >= 0); first(data, 'd'); close(data); _exit(0);
    }
    receive(ready[0]); int directory = open("left/tree/deep", O_RDONLY | O_DIRECTORY); CHECK(directory >= 0);
    CHECK(rename("left/tree", "left/tree/deep/cycle") == -1 && errno == EINVAL);
    CHECK(rename("left/tree", "right/moved tree") == 0);
    fd_link(directory, "/tmp/native64-rename/right/moved tree/deep");
    fd = openat(directory, "data", O_RDONLY); CHECK(fd >= 0); first(fd, 'd'); CHECK(close(fd) == 0);
    CHECK(write(gate[1], "g", 1) == 1); waited(child);
    CHECK(close(ready[0]) == 0 && close(ready[1]) == 0 && close(gate[0]) == 0 && close(gate[1]) == 0 && close(directory) == 0);
    CHECK(symlink("deep/data", "right/moved tree/link") == 0 && rename("right/moved tree/link", "left/link") == 0);
    char text[64] = {0}; CHECK(readlink("left/link", text, sizeof(text)) == 9 && !strcmp(text, "deep/data"));
    CHECK(unlink("left/link") == 0 && unlink("right/moved tree/deep/data") == 0 && rmdir("right/moved tree/deep") == 0);
    CHECK(mkdir("left/empty", 0700) == 0);
    int saved = open(".", O_RDONLY | O_DIRECTORY); CHECK(saved >= 0 && chdir("left/empty") == 0);
    CHECK(rmdir(".") == -1 && errno == EINVAL && rmdir("..") == -1 && errno == ENOTEMPTY);
    CHECK(fchdir(saved) == 0 && close(saved) == 0);
    directory = open("left/empty", O_RDONLY | O_DIRECTORY); CHECK(directory >= 0);
    CHECK(rename("right/moved tree", "left/empty") == 0 && info(directory).st_nlink == 0);
    CHECK(openat(directory, "new", O_CREAT | O_RDWR, 0600) == -1 && errno == ENOENT);
    CHECK(close(directory) == 0 && rmdir("left/empty") == 0);
    puts("[64] RENAME DIRECTORIES PASSED: live dirfd/cwd/chroot, ancestry, symlink identity and empty-directory replacement");
}
static void sockets(void) {
    struct sockaddr_un address = {.sun_family = AF_UNIX}; strcpy(address.sun_path, "left/bound");
    int server = socket(AF_UNIX, SOCK_STREAM, 0), client = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(server >= 0 && client >= 0);
    CHECK(bind(server, (void*)&address, 2 + strlen(address.sun_path) + 1) == 0 && listen(server, 1) == 0);
    CHECK(rename("left/bound", "right/bound") == 0); strcpy(address.sun_path, "right/bound");
    CHECK(connect(client, (void*)&address, 2 + strlen(address.sun_path) + 1) == 0);
    int peer = accept(server, NULL, NULL); CHECK(peer >= 0 && write(client, "s", 1) == 1); receive(peer);
    CHECK(close(peer) == 0 && close(client) == 0 && close(server) == 0 && unlink("right/bound") == 0);
    puts("[64] RENAME SOCKET PASSED: moved pathname resolves the existing listener");
}
static void executable(void) {
    int source = open("/bin/rename64", O_RDONLY), target = open("left/program", O_CREAT | O_EXCL | O_WRONLY, 0700); CHECK(source >= 0 && target >= 0);
    char data[8192]; ssize_t count;
    while ((count = read(source, data, sizeof(data))) > 0) CHECK(write(target, data, count) == count);
    CHECK(count == 0 && close(source) == 0 && close(target) == 0);
    int ready[2], gate[2]; CHECK(pipe(ready) == 0 && pipe(gate) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        char one[24], two[24]; snprintf(one, sizeof(one), "%d", ready[1]); snprintf(two, sizeof(two), "%d", gate[0]);
        char* args[] = {"left/program", one, two, NULL}; char* env[] = {NULL}; execve(args[0], args, env); CHECK(0);
    }
    receive(ready[0]); CHECK(rename("left/program", "right/program moved") == 0 && unlink("right/program moved") == 0);
    target = create("right/program moved", 'N'); CHECK(close(target) == 0);
    CHECK(write(gate[1], "g", 1) == 1); waited(child);
    CHECK(close(ready[0]) == 0 && close(ready[1]) == 0 && close(gate[0]) == 0 && close(gate[1]) == 0);
    CHECK(unlink("right/program moved") == 0);
    puts("[64] RENAME EXEC PASSED: lazy executable pages survive rename/unlink/reuse until exit");
}
int main(int argc, char** argv) {
    if (argc == 3) {
        CHECK(write(atoi(argv[1]), "r", 1) == 1); receive(atoi(argv[2]));
        char path[512] = {0}; const char* expected = "/tmp/native64-rename/right/program moved (deleted)";
        CHECK(readlink("/proc/self/exe", path, sizeof(path) - 1) == (ssize_t)strlen(expected) && !strcmp(path, expected));
        CHECK(madvise((void*)(lazy + 12288), 4096, MADV_DONTNEED) == 0 && lazy[12288] == 0xA5);
        return 0;
    }
    CHECK(argc == 2 && mkdir(root, 0700) == 0 && chdir(root) == 0 && mkdir("left", 0700) == 0 && mkdir("right", 0700) == 0);
    files(); directories(); sockets(); executable();
    CHECK(rmdir("left") == 0 && rmdir("right") == 0 && chdir("/") == 0 && rmdir(root) == 0);
    puts("[64] RENAME ALL PASSED"); return 0;
}
