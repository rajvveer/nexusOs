/* /proc/PID/{mem,maps,environ} (tests/test_x64_procmem.py boots this as /sbin/init): ptrace access is checked for the
 * opener at open whatever the access mode, and the file is bound to the address space the target ran then, as Linux
 * mm_access does (CVE-2012-0056). I/O after the target's exec or exit moves nothing, a reopen through /proc/self/fd
 * binds the current image, and an idle descriptor does not keep a dead image (its executable) busy. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { dprintf(2, "[64] PROCMEM FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)

#define INITIAL 0x1122334455667788ULL
static volatile uint64_t marker = INITIAL;
static ssize_t peek(int fd, uint64_t address, uint64_t* value) { return pread(fd, value, 8, (off_t)address); }
static ssize_t poke(int fd, uint64_t address, uint64_t value) { return pwrite(fd, &value, 8, (off_t)address); }
static ssize_t slurp(int fd, char* text, size_t size) { ssize_t n = pread(fd, text, size - 1, 0); if (n >= 0) text[n] = 0; return n; }
static uint64_t here(void) { return (uint64_t)(uintptr_t)&marker; }
/* Tell the parent we run (with this image's marker address), then wait: 'x' execs /sbin/init into the same mode. */
static int hold(char** argv) {
    int ready = atoi(argv[2]), go = atoi(argv[3]); char message[9] = {argv[4][0]}; uint64_t address = here();
    memcpy(message + 1, &address, 8); CHECK(write(ready, message, 9) == 9);
    char command = 0; ssize_t n = read(go, &command, 1); CHECK(n >= 0);
    if (n == 1 && command == 'x') { char* args[] = {"/sbin/init", "hold", argv[2], argv[3], "n", NULL}; execv(args[0], args); CHECK(0); }
    return 0;
}
/* Runs after an exec with descriptors opened on the previous image's /proc/self/{mem,maps,environ}. */
static int after_exec(char** argv) {
    int mem = atoi(argv[2]), maps = atoi(argv[3]), environment = atoi(argv[4]); uint64_t value = 0; char text[4096], path[64];
    CHECK(peek(mem, here(), &value) == 0 && poke(mem, here(), 0xBAD) == 0 && marker == INITIAL);
    CHECK(slurp(maps, text, sizeof(text)) == 0 && slurp(environment, text, sizeof(text)) == 0);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", mem); int again = open(path, O_RDWR | O_CLOEXEC);
    CHECK(again >= 0 && peek(again, here(), &value) == 8 && value == INITIAL && poke(again, here(), 0x600D) == 8 && marker == 0x600D);
    CHECK(peek(mem, here(), &value) == 0 && close(again) == 0);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", maps); again = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(again >= 0 && slurp(again, text, sizeof(text)) > 0 && strstr(text, "/sbin/init") && close(again) == 0);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", environment); again = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(again >= 0 && slurp(again, text, sizeof(text)) >= 25 && !memcmp(text, "PROCMEM_PROBE=after-exec", 25) && close(again) == 0);
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 4 && !strcmp(argv[1], "exec")) return after_exec(argv);
    if (argc > 4 && !strcmp(argv[1], "hold")) return hold(argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    uint64_t value = 0; char text[4096], path[64], c; int status, ready[2], go[2]; pid_t child;

    int self = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    CHECK(self >= 0 && peek(self, here(), &value) == 8 && value == INITIAL && poke(self, here(), 0xA5A5) == 8 && marker == 0xA5A5);
    puts("[64] PROCMEM own image read/write PASS");

    /* Another live process through bound descriptors; nothing once it is a zombie and once it is reaped. */
    CHECK(pipe2(ready, O_CLOEXEC) == 0 && pipe2(go, O_CLOEXEC) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) { marker = 0xC0FFEE; CHECK(write(ready[1], "r", 1) == 1 && read(go[0], &c, 1) == 1); _exit(marker == 0xFEEDFACE ? 0 : 1); }
    CHECK(read(ready[0], &c, 1) == 1);
    snprintf(path, sizeof(path), "/proc/%d/mem", child); int target = open(path, O_RDWR | O_CLOEXEC);
    snprintf(path, sizeof(path), "/proc/%d/maps", child); int maps = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(target >= 0 && maps >= 0 && peek(target, here(), &value) == 8 && value == 0xC0FFEE);
    CHECK(poke(target, here(), 0xFEEDFACE) == 8 && marker == 0xA5A5 && slurp(maps, text, sizeof(text)) > 0 && strstr(text, "/sbin/init"));
    CHECK(write(go[1], "g", 1) == 1);
    siginfo_t info; memset(&info, 0, sizeof(info));
    CHECK(waitid(P_PID, child, &info, WEXITED | WNOWAIT) == 0 && info.si_pid == child && info.si_code == CLD_EXITED && info.si_status == 0);
    CHECK(peek(target, here(), &value) == 0 && poke(target, here(), 1) == 0 && slurp(maps, text, sizeof(text)) == 0);
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    CHECK(peek(target, here(), &value) == 0 && poke(target, here(), 1) == 0 && slurp(maps, text, sizeof(text)) == 0);
    CHECK(close(target) == 0 && close(maps) == 0 && close(ready[0]) == 0 && close(ready[1]) == 0 && close(go[0]) == 0 && close(go[1]) == 0);
    puts("[64] PROCMEM live target read/write, nothing after exit (zombie and reaped) PASS");

    /* Access is checked for the opener at open in every mode: a non-dumpable target, or an opener in another user
     * namespace (no ptrace capability over the target), is refused even read-only, while a descriptor opened with
     * access keeps working for whoever holds it, and a process always opens its own image files. (The same uid
     * opening a dumpable process was allowed above.) */
    for (unsigned variant = 0; variant < 2; variant++) {
        CHECK(prctl(PR_SET_DUMPABLE, variant ? 1 : 0) == 0);
        child = fork(); CHECK(child >= 0);
        if (!child) {
            marker = 0xBEEF;
            if (variant) CHECK(unshare(CLONE_NEWUSER) == 0);
            static const char* const files[] = {"mem", "maps", "environ"};
            for (unsigned i = 0; i < 3; i++) {
                snprintf(path, sizeof(path), "/proc/%d/%s", getppid(), files[i]); errno = 0; CHECK(open(path, O_RDONLY) == -1 && errno == EACCES);
            }
            snprintf(path, sizeof(path), "/proc/%d/mem", getppid()); errno = 0; CHECK(open(path, O_RDWR) == -1 && errno == EACCES);
            CHECK(peek(self, here(), &value) == 8 && value == 0xA5A5);
            int own = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); CHECK(own >= 0 && slurp(own, text, sizeof(text)) > 0 && close(own) == 0);
            _exit(0);
        }
        CHECK(waitpid(child, &status, 0) == child && status == 0);
    }
    CHECK(prctl(PR_GET_DUMPABLE) == 1);
    puts("[64] PROCMEM open-time access for the opener (EACCES for a non-dumpable target and across user namespaces, inherited descriptor kept) PASS");

    /* Exec ends the image: descriptors on the previous image move nothing; a reopen binds the new one. */
    child = fork(); CHECK(child >= 0);
    if (!child) {
        int mem = open("/proc/self/mem", O_RDWR), map = open("/proc/self/maps", O_RDONLY), env = open("/proc/self/environ", O_RDONLY);
        CHECK(mem >= 0 && map >= 0 && env >= 0);
        char numbers[3][16]; snprintf(numbers[0], 16, "%d", mem); snprintf(numbers[1], 16, "%d", map); snprintf(numbers[2], 16, "%d", env);
        char* args[] = {"/sbin/init", "exec", numbers[0], numbers[1], numbers[2], NULL}; char* environment[] = {"PROCMEM_PROBE=after-exec", NULL};
        execve(args[0], args, environment); CHECK(0);
    }
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    puts("[64] PROCMEM exec ends bound mem/maps/environ, reopen through /proc/self/fd binds the new image PASS");

    /* An idle descriptor does not pin a dead image: once the target execs away from a copy of this program, the copy is
     * writable again after the next procfs operation (no ETXTBSY), while it is busy as long as the target runs it. */
    const char* copy = "/home/nexus/procmem-old"; unlink(copy);
    int input = open("/sbin/init", O_RDONLY | O_CLOEXEC), output = open(copy, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    CHECK(input >= 0 && output >= 0); ssize_t n;
    while ((n = read(input, text, sizeof(text))) > 0) CHECK(write(output, text, n) == n);
    CHECK(n == 0 && close(input) == 0 && close(output) == 0 && pipe(ready) == 0 && pipe(go) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        char r[16], g[16]; snprintf(r, sizeof(r), "%d", ready[1]); snprintf(g, sizeof(g), "%d", go[0]);
        CHECK(close(ready[0]) == 0 && close(go[1]) == 0);
        char* args[] = {(char*)copy, "hold", r, g, "o", NULL}; execv(copy, args); CHECK(0);
    }
    char message[9]; uint64_t address;
    CHECK(close(ready[1]) == 0 && close(go[0]) == 0 && read(ready[0], message, 9) == 9 && message[0] == 'o'); memcpy(&address, message + 1, 8);
    snprintf(path, sizeof(path), "/proc/%d/mem", child); target = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(target >= 0 && peek(target, address, &value) == 8 && value == INITIAL);
    errno = 0; CHECK(open(copy, O_WRONLY | O_CLOEXEC) == -1 && errno == ETXTBSY);
    CHECK(write(go[1], "x", 1) == 1 && read(ready[0], message, 9) == 9 && message[0] == 'n');
    int probe = open("/proc/self/stat", O_RDONLY | O_CLOEXEC); CHECK(probe >= 0 && close(probe) == 0);
    int writable = open(copy, O_WRONLY | O_CLOEXEC); CHECK(writable >= 0 && close(writable) == 0);
    CHECK(peek(target, address, &value) == 0 && close(target) == 0);
    CHECK(close(go[1]) == 0 && waitpid(child, &status, 0) == child && status == 0 && close(ready[0]) == 0 && unlink(copy) == 0);
    puts("[64] PROCMEM dead image released (executable writable again) PASS");

    CHECK(close(self) == 0);
    puts("[64] PROCMEM ALL PASSED: open-time ptrace access for the opener, image bound at open, exec/exit end of file, reopen, no pinning");
    return 0;
}
