#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] FILEMAP FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
static const char* path = "/home/nexus/native64-persist/mapped.bin";
static unsigned char bytes[8193];
static void verify(int fd) {
    CHECK(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        unsigned char expected = i == 0 ? 'A' : i == 1 ? 'D' : i == 2 ? 'E' : i == 3 ? 'H' : i == 4096 ? 'I' : i == 8192 ? 'J' : 0;
        CHECK(bytes[i] == expected);
    }
}
static void readonly(void) {
    int fd = open("/etc/native64.dat", O_RDONLY); CHECK(fd >= 0);
    unsigned char* shared = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    unsigned char* private = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(shared != MAP_FAILED && private != MAP_FAILED && shared[0] == 11);
    private[0] = 99; CHECK(shared[0] == 11);
    CHECK(mprotect(shared, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    CHECK(msync(private, 4096, MS_SYNC) == 0 && fsync(fd) == 0);
    CHECK(pread(fd, bytes, 1, 0) == 1 && bytes[0] == 11);
    CHECK(msync(shared + 1, 4096, MS_SYNC) == -1 && errno == EINVAL);
    CHECK(msync(shared, 4096, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL);
    CHECK(close(fd) == 0 && munmap(shared, 4096) == 0 && munmap(private, 4096) == 0);
    puts("[64] FILEMAP READONLY PASSED: cached file pages, private COW and mapping permissions");
}
static void executable(void) {
    const char* target = "/home/nexus/filemap-exec";
    int input = open("/bin/filemap64", O_RDONLY), output = open(target, O_RDWR | O_CREAT | O_EXCL, 0700); CHECK(input >= 0 && output >= 0);
    ssize_t count; while ((count = read(input, bytes, sizeof(bytes))) > 0) CHECK(write(output, bytes, count) == count);
    CHECK(count == 0 && close(input) == 0);
    char* arguments[] = {(char*)target, "e", NULL}; char* environment[] = {NULL};
    CHECK(execve(target, arguments, environment) == -1 && errno == ETXTBSY);
    CHECK(syscall(SYS_execveat,output,"",arguments,environment,AT_EMPTY_PATH)==-1&&errno==ETXTBSY);
    void* map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, output, 0); CHECK(map != MAP_FAILED && close(output) == 0);
    CHECK(execve(target, arguments, environment) == -1 && errno == ETXTBSY);
    CHECK(mprotect(map, 4096, PROT_READ) == 0 && execve(target, arguments, environment) == -1 && errno == ETXTBSY);
    CHECK(munmap(map, 4096) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { execve(target, arguments, environment); CHECK(0); }
    int status; CHECK(waitpid(child, &status, 0) == child && status == 0);
    int directory=open("/home/nexus",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(directory>=0);
    CHECK(syscall(SYS_execveat,directory,"filemap-exec",arguments,environment,0x8000)==-1&&errno==EINVAL);
    CHECK(symlink("filemap-exec","/home/nexus/filemap-exec-link")==0);
    CHECK(syscall(SYS_execveat,directory,"filemap-exec-link",arguments,environment,AT_SYMLINK_NOFOLLOW)==-1&&errno==ELOOP);
    child=fork();CHECK(child>=0);if(!child){syscall(SYS_execveat,directory,"filemap-exec-link",arguments,environment,0);CHECK(0);}
    CHECK(waitpid(child,&status,0)==child&&status==0&&unlink("/home/nexus/filemap-exec-link")==0);
    CHECK(syscall(SYS_execveat,directory,"",arguments,environment,AT_EMPTY_PATH)==-1&&errno==EACCES);
    CHECK(syscall(SYS_execveat,-1,"",arguments,environment,AT_EMPTY_PATH)==-1&&errno==EBADF);
    CHECK(syscall(SYS_execveat,directory,"",arguments,environment,0)==-1&&errno==ENOENT);
    CHECK(close(directory)==0);
    int executable_fd=open(target,O_RDONLY|O_CLOEXEC);CHECK(executable_fd>=0&&unlink(target)==0);
    output=open(target,O_WRONLY|O_CREAT|O_EXCL,0700);CHECK(output>=0&&write(output,"replacement data",16)==16&&close(output)==0);
    char fd_number[24],proc_path[64];snprintf(fd_number,sizeof(fd_number),"%d",executable_fd);snprintf(proc_path,sizeof(proc_path),"/proc/self/fd/%d",executable_fd);
    char* fd_arguments[]={(char*)target,"f",fd_number,NULL};
    for(unsigned mode=0;mode<2;mode++){
        child=fork();CHECK(child>=0);if(!child){if(mode)execve(proc_path,fd_arguments,environment);else fexecve(executable_fd,fd_arguments,environment);CHECK(0);}
        CHECK(waitpid(child,&status,0)==child&&status==(37<<8));
    }
    CHECK(close(executable_fd)==0&&unlink(target)==0);
    puts("[64] FD EXEC PASSED: relative execveat, flags, unlinked/replaced inode, proc descriptor and CLOEXEC");
}
static void mutations(void) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { verify(fd); CHECK(close(fd) == 0); puts("[64] FILEMAP persisted bytes survived reboot PASS"); }
    else CHECK(errno == ENOENT);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600); CHECK(fd >= 0 && ftruncate(fd, 12288) == 0);
    int ro = open(path, O_RDONLY); CHECK(ro >= 0);
    unsigned char* a = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char* b = mmap(NULL, 12288, PROT_READ, MAP_SHARED, ro, 0);
    unsigned char* private = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_PRIVATE, ro, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED && private != MAP_FAILED && a[0] == 0);
    a[0] = 'A'; a[4096] = 'B'; a[8192] = 'C'; CHECK(b[0] == 'A' && b[4096] == 'B' && b[8192] == 'C');
    CHECK(pread(ro, bytes, 1, 0) == 1 && bytes[0] == 'A'); private[0] = 'P';
    CHECK(msync(a, 4096, MS_SYNC) == 0); a[1] = 'D';
    CHECK(pread(ro, bytes, 1, 1) == 1 && bytes[0] == 'D');
    CHECK(pwrite(fd, "E", 1, 2) == 1 && b[2] == 'E' && private[0] == 'P' && private[2] == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { a[4097] = 'F'; _exit(0); }
    int status; CHECK(waitpid(child, &status, 0) == child && status == 0 && b[4097] == 'F');
    CHECK(mprotect(a + 8192, 4096, PROT_READ) == 0 && mprotect(a + 8192, 4096, PROT_READ | PROT_WRITE) == 0);
    a[8192] = 'G'; CHECK(fsync(ro) == 0 && close(fd) == 0 && close(ro) == 0);
    a[3] = 'H'; CHECK(msync(a, 8192, MS_SYNC) == 0);
    CHECK(munmap(a + 8192, 4096) == 0 && munmap(private, 12288) == 0 && munmap(b, 12288) == 0);
    fd = open(path, O_RDWR); CHECK(fd >= 0 && ftruncate(fd, 4097) == 0 && ftruncate(fd, 8193) == 0);
    CHECK(a[4096] == 'B' && a[4097] == 0); a[4096] = 'I';
    unsigned char* tail = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 8192); CHECK(tail != MAP_FAILED);
    tail[0] = 'J'; tail[1] = 'X'; CHECK(msync(tail, 4096, MS_SYNC) == 0 && close(fd) == 0);
    CHECK(munmap(tail, 4096) == 0 && munmap(a, 8192) == 0);
    fd = open(path, O_RDONLY); CHECK(fd >= 0); verify(fd); CHECK(close(fd) == 0);
    executable();
    puts("[64] FILEMAP WRITE PASSED: shared coherence, redirty after sync, close/unmap, truncate, EOF tail and exec protection");
}
static void errors(void) {
    int fd = open("/home/nexus/writeback-errors.dat", O_RDWR); CHECK(fd >= 0);
    volatile unsigned char* map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECK(map != MAP_FAILED && map[0] == '1');
    map[0] = 'Z';
    CHECK(fsync(fd) == -1 && errno == EIO && pread(fd, bytes, 1, 0) == 1 && bytes[0] == 'Z');
    CHECK(msync((void*)map, 4096, MS_SYNC) == -1 && errno == EIO && map[0] == 'Z');
    CHECK(munmap((void*)map, 8192) == -1 && errno == EIO && map[0] == 'Z');
    CHECK(close(fd) == -1 && errno == EIO && map[0] == 'Z');
    puts("[64] FILEMAP ERROR PASSED: real device write errors retain mapped bytes and propagate to fsync/msync/munmap/close");
}
int main(int argc, char** argv) {
    if(argc==3&&!strcmp(argv[1],"f")){
        CHECK(fcntl(atoi(argv[2]),F_GETFD)==-1&&errno==EBADF);
        char executable[600];ssize_t size=readlink("/proc/self/exe",executable,sizeof(executable)-1);CHECK(size>0);executable[size]=0;
        CHECK(!strcmp(executable,"/home/nexus/filemap-exec (deleted)"));
        int replacement=open(argv[0],O_WRONLY);CHECK(replacement>=0&&close(replacement)==0);return 37;
    }
    CHECK(argc == 2);
    if (argv[1][0] == 'e') { CHECK(open(argv[0], O_RDWR) == -1 && errno == ETXTBSY); return 0; }
    if (argv[1][0] == 'd') mutations();
    else if (argv[1][0] == 'x') errors();
    else readonly();
    return 0;
}
