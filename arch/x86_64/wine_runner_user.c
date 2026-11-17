/* nexus-wine-runner: runs a test script for the Wine campaign (tests/test_wine.py) and reports to the console.
 *
 *   nexus-wine-runner SCRIPT
 *
 * One command per line; arguments are split on blanks with '...'/"..." quoting and backslash escapes:
 *   env NAME=VALUE | unset NAME | cd DIR | mark TEXT       environment, working folder, a "[64] WR MARK TEXT" line
 *   run TAG SECONDS [<FILE] [>FILE] PROGRAM ARGS...        foreground command (own process group); stdout+stderr lines
 *                                                         are printed as "[64] WR TAG| ..." unless redirected to FILE
 *   bg TAG [<FILE] [>FILE] PROGRAM ARGS...                background command, output prefixed the same way
 *   wait TAG SECONDS | kill TAG [SIGNAL]                   wait for / signal a background command's process group
 *   sleep MS | waitfile PATH SECONDS | waitgone PATH SECONDS
 *   cat PATH | ls PATH | tree PATH DEPTH | du PATH | exists PATH | write PATH TEXT | rm PATH | cmp A B | size PATH
 *   sync                                                   flush every file system buffer (before the host stops the VM)
 *   procs TEXT | procwait TEXT SECONDS                     list processes whose command line contains TEXT / wait until none
 *   host PORT [ADDRESS]                                    host test server (default 10.0.2.2:PORT, QEMU user network;
 *                                                         ADDRESS = an IPv4 or IPv6 literal on other networks)
 *   gate NAME SECONDS                                      GET /gate/NAME until the host answers 200
 *   fetch NAME PATH | post PATH NAME                        GET /file/NAME into PATH, POST PATH to /upload/NAME
 * Every command ends with "[64] WR END tag=TAG status=S ms=T" (status: exit code, sig N, timeout, error); the script
 * ends with "[64] WR DONE failures=N". The runner never stops on a failing command: the host test decides.
 * A command is finished when its process has exited; programs it started in the background (wine start, a desktop
 * launcher) may keep the output pipe open: their lines keep being printed under the tag until they close it. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_JOBS 16
struct job { char tag[64]; pid_t pid; int out; char line[4096]; size_t used; bool done; int status; uint64_t started; };
static struct job jobs[MAX_JOBS];
static int failures, host_port = 8920;

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void report(const char* format, ...) {
    char text[8192]; va_list list; va_start(list, format); vsnprintf(text, sizeof(text), format, list); va_end(list);
    printf("[64] WR %s\n", text); fflush(stdout);
}
static void emit(struct job* j, bool flush) {
    char* start = j->line; size_t left = j->used;
    for (;;) {
        char* nl = memchr(start, '\n', left);
        if (!nl && !(flush && left) && left < sizeof(j->line) - 1) break;
        size_t n = nl ? (size_t)(nl - start) : left;
        if (n && start[n - 1] == '\r') n--;
        printf("[64] WR %s| %.*s\n", j->tag, (int)n, start);
        size_t consumed = nl ? (size_t)(nl - start) + 1 : left;
        start += consumed; left -= consumed;
        if (!nl) break;
    }
    fflush(stdout);
    memmove(j->line, start, left); j->used = left;
}
static void pump(int timeout_ms) {
    struct pollfd fds[MAX_JOBS]; int map[MAX_JOBS], n = 0;
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].out >= 0) { fds[n] = (struct pollfd){jobs[i].out, POLLIN, 0}; map[n++] = i; }
    if (!n) { if (timeout_ms > 0) usleep((useconds_t)timeout_ms * 1000); }
    else if (poll(fds, (nfds_t)n, timeout_ms) > 0) {
        for (int k = 0; k < n; k++) {
            if (!fds[k].revents) continue;
            struct job* j = &jobs[map[k]];
            ssize_t got = read(j->out, j->line + j->used, sizeof(j->line) - 1 - j->used);
            if (got > 0) { j->used += (size_t)got; emit(j, false); }
            else if (got == 0 || (errno != EINTR && errno != EAGAIN)) { emit(j, true); close(j->out); j->out = -1; }
        }
    }
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job* j = &jobs[i]; int status; if (j->pid <= 0 || j->done) continue;
        pid_t w = waitpid(j->pid, &status, WNOHANG);
        if (w == j->pid) { j->done = true; j->status = status; }
        else if (w < 0 && errno == ECHILD) { j->done = true; j->status = -1; }   /* no longer our child: status unknown */
    }
    /* reparented grandchildren (wineserver daemons); a job that ends between the two loops is reaped here */
    int status; pid_t reaped;
    while ((reaped = waitpid(-1, &status, WNOHANG)) > 0)
        for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].pid == reaped && !jobs[i].done) { jobs[i].done = true; jobs[i].status = status; }
}
static const char* status_text(int status, char* buffer, size_t size) {
    if (status == -1) snprintf(buffer, size, "unknown");
    else if (WIFEXITED(status)) snprintf(buffer, size, "%d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) snprintf(buffer, size, "sig%d", WTERMSIG(status));
    else snprintf(buffer, size, "raw%d", status);
    return buffer;
}
static struct job* find_job(const char* tag) {
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].pid > 0 && !strcmp(jobs[i].tag, tag)) return &jobs[i];
    return NULL;
}
static void finish_job(struct job* j, bool timed_out) {
    if (timed_out) { kill(-j->pid, SIGKILL); kill(j->pid, SIGKILL); }
    while (!j->done) pump(50);
    /* trailing output for up to a second; a background program that inherited the pipe keeps it (pump() goes on reading) */
    for (uint64_t end = now_ms() + 1000; j->out >= 0 && now_ms() < end;) pump(50);
    char text[32]; status_text(j->status, text, sizeof(text));
    report("END tag=%s status=%s ms=%llu", j->tag, timed_out ? "timeout" : text, (unsigned long long)(now_ms() - j->started));
    if (timed_out || j->status == -1 || !WIFEXITED(j->status) || WEXITSTATUS(j->status)) failures++;
    j->pid = 0;
}
static struct job* start(const char* tag, char** argv) {
    struct job* j = NULL;
    for (int i = 0; i < MAX_JOBS && !j; i++) if (jobs[i].pid <= 0 && jobs[i].out < 0) j = &jobs[i];
    if (!j) { report("END tag=%s status=error no free job slot", tag); failures++; return NULL; }
    const char *input = "/dev/null", *output = NULL;
    while (argv[0] && (argv[0][0] == '<' || argv[0][0] == '>')) { if (argv[0][0] == '<') input = argv[0] + 1; else output = argv[0] + 1; argv++; }
    if (!argv[0]) { report("END tag=%s status=error no program", tag); failures++; return NULL; }
    int out[2]; if (pipe2(out, O_CLOEXEC)) { report("END tag=%s status=error pipe %s", tag, strerror(errno)); failures++; return NULL; }
    snprintf(j->tag, sizeof(j->tag), "%s", tag); j->used = 0; j->done = false; j->status = 0; j->started = now_ms();
    pid_t pid = fork();
    if (pid < 0) { report("END tag=%s status=error fork %s", tag, strerror(errno)); close(out[0]); close(out[1]); failures++; return NULL; }
    if (!pid) {
        setpgid(0, 0); signal(SIGPIPE, SIG_DFL);
        int in = open(input, O_RDONLY); if (in < 0 || dup2(in, 0) < 0) _exit(126);
        if (output) { int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0 || dup2(fd, 1) < 0) _exit(126); }
        else if (dup2(out[1], 1) < 0) _exit(126);
        if (dup2(out[1], 2) < 0) _exit(126);
        execvp(argv[0], argv); fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno)); _exit(127);
    }
    setpgid(pid, pid); close(out[1]); fcntl(out[0], F_SETFL, O_NONBLOCK);
    j->pid = pid; j->out = out[0];
    char line[1024] = ""; size_t used = 0;
    for (char** a = argv; *a && used < sizeof(line) - 2; a++) used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", a == argv ? "" : " ", *a);
    report("START tag=%s pid=%d cmd=%s", tag, (int)pid, line);
    return j;
}
static void wait_job(struct job* j, int seconds) {
    uint64_t deadline = now_ms() + (uint64_t)seconds * 1000;
    while (!j->done && now_ms() < deadline) pump(100);
    finish_job(j, !j->done);
}
/* ---- files ---- */
static void print_file(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) { report("cat %s: %s", path, strerror(errno)); failures++; return; }
    char line[4096]; while (fgets(line, sizeof(line), f)) { size_t n = strlen(line); if (n && line[n - 1] == '\n') line[--n] = 0; printf("[64] WR cat| %s\n", line); }
    fclose(f); fflush(stdout);
}
static void walk(const char* path, int depth, bool print, uint64_t* bytes, uint64_t* files, uint64_t* dirs) {
    DIR* d = opendir(path); if (!d) { report("dir %s: %s", path, strerror(errno)); return; }
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[4096]; snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st; if (lstat(full, &st)) { report("lstat %s: %s", full, strerror(errno)); continue; }
        char kind = S_ISDIR(st.st_mode) ? 'D' : S_ISLNK(st.st_mode) ? 'L' : S_ISREG(st.st_mode) ? 'F' : '?';
        if (print) {
            char target[512] = ""; if (kind == 'L') { ssize_t n = readlink(full, target, sizeof(target) - 1); if (n >= 0) target[n] = 0; }
            printf("[64] WR ls| %c %10lld %s%s%s\n", kind, (long long)st.st_size, full, kind == 'L' ? " -> " : "", target);
        }
        if (kind == 'D') { (*dirs)++; if (depth > 1) walk(full, depth - 1, print, bytes, files, dirs); }
        else { (*files)++; *bytes += (uint64_t)st.st_size; }
    }
    closedir(d); fflush(stdout);
}
static int compare(const char* a, const char* b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb"); int result = -1;
    if (x && y) { result = 0; for (;;) { int p = fgetc(x), q = fgetc(y); if (p != q) { result = 1; break; } if (p == EOF) break; } }
    if (x) fclose(x); if (y) fclose(y); return result;
}
/* ---- processes: /proc/PID/cmdline (NUL-separated arguments shown with blanks) ---- */
static int matching_processes(const char* text, bool print) {
    DIR* d = opendir("/proc"); if (!d) return -1;
    struct dirent* e; int count = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9' || atoi(e->d_name) == getpid()) continue;
        char file[64], line[1024]; snprintf(file, sizeof(file), "/proc/%s/cmdline", e->d_name);
        int fd = open(file, O_RDONLY); if (fd < 0) continue;
        ssize_t n = read(fd, line, sizeof(line) - 1); close(fd); if (n <= 0) continue;
        for (ssize_t i = 0; i < n; i++) if (!line[i]) line[i] = ' ';
        line[n] = 0;
        if (!strstr(line, text)) continue;
        count++; if (print) printf("[64] WR procs| %s %s\n", e->d_name, line);
    }
    closedir(d); fflush(stdout); return count;
}
/* ---- host channel (HTTP/1.0 to the QEMU user-network host) ---- */
static char host_address[64] = "10.0.2.2";   /* "host PORT [ADDRESS]": an IPv4 or IPv6 literal (tests/test_x64_ipv6_apps.py) */
static int http(const char* method, const char* target, const char* upload, const char* download) {
    struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)host_port)};
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)host_port)};
    bool v6 = inet_pton(AF_INET6, host_address, &a6.sin6_addr) == 1;
    if (!v6 && inet_pton(AF_INET, host_address, &a.sin_addr) != 1) return -1;
    int s = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); if (s < 0) return -1;
    if (v6 ? connect(s, (struct sockaddr*)&a6, sizeof(a6)) : connect(s, (struct sockaddr*)&a, sizeof(a))) { close(s); return -1; }
    FILE* in = upload ? fopen(upload, "rb") : NULL; long size = 0;
    if (upload) { if (!in) { close(s); return -2; } fseek(in, 0, SEEK_END); size = ftell(in); fseek(in, 0, SEEK_SET); }
    char head[1024]; int n = snprintf(head, sizeof(head), "%s %s HTTP/1.0\r\nHost: %s%s%s\r\nContent-Length: %ld\r\n\r\n", method, target,
                                      v6 ? "[" : "", host_address, v6 ? "]" : "", size);
    bool ok = write(s, head, (size_t)n) == n;
    char buffer[65536];
    while (ok && in) { size_t got = fread(buffer, 1, sizeof(buffer), in); if (!got) break; ok = write(s, buffer, got) == (ssize_t)got; }
    if (in) fclose(in);
    size_t used = 0; int code = -1; FILE* out = NULL; bool body = false;
    for (;;) {
        ssize_t got = read(s, buffer + used, sizeof(buffer) - used); if (got <= 0) break; used += (size_t)got;
        if (!body) {
            char* end = memmem(buffer, used, "\r\n\r\n", 4); if (!end) { if (used == sizeof(buffer)) break; continue; }
            sscanf(buffer, "HTTP/%*s %d", &code); body = true;
            size_t rest = used - (size_t)(end + 4 - buffer);
            if (download && code == 200) { out = fopen(download, "wb"); if (!out) { code = -3; break; } fwrite(end + 4, 1, rest, out); }
            used = 0; continue;
        }
        if (out) fwrite(buffer, 1, used, out); used = 0;
    }
    if (out && fclose(out)) code = -3;
    close(s); return code;
}
/* ---- script ---- */
static int split(char* line, char** argv, int max) {
    char *r = line, *w = line; int n = 0;
    while (*r) {
        while (*r == ' ' || *r == '\t') r++;
        if (!*r || *r == '#') break;
        if (n + 1 >= max) return -1;
        argv[n++] = w; char quote = 0;
        while (*r && (quote || (*r != ' ' && *r != '\t'))) {
            char c = *r++;
            if (c == '\\' && quote != '\'' && *r) *w++ = *r++;
            else if ((c == '\'' || c == '"') && (!quote || quote == c)) quote = quote ? 0 : c;
            else *w++ = c;
        }
        if (*r) r++;
        *w++ = 0;
    }
    argv[n] = NULL; return n;
}
static void command(int argc, char** argv) {
    const char* c = argv[0];
    if (!strcmp(c, "env") && argc == 2) { char* eq = strchr(argv[1], '='); if (eq) { *eq = 0; setenv(argv[1], eq + 1, 1); } return; }
    if (!strcmp(c, "unset") && argc == 2) { unsetenv(argv[1]); return; }
    if (!strcmp(c, "cd") && argc == 2) { if (chdir(argv[1])) { report("cd %s: %s", argv[1], strerror(errno)); failures++; } return; }
    if (!strcmp(c, "mark")) {   /* snprintf returns the untruncated length: stop once the text is full */
        char text[2048] = ""; size_t u = 0;
        for (int i = 1; i < argc && u < sizeof(text) - 1; i++) { int k = snprintf(text + u, sizeof(text) - u, "%s%s", i > 1 ? " " : "", argv[i]); if (k < 0) break; u += (size_t)k; }
        report("MARK %s", text); return;
    }
    if (!strcmp(c, "run") && argc >= 4) { struct job* j = start(argv[1], argv + 3); if (j) wait_job(j, atoi(argv[2])); return; }
    if (!strcmp(c, "bg") && argc >= 3) { start(argv[1], argv + 2); return; }
    if (!strcmp(c, "wait") && argc == 3) { struct job* j = find_job(argv[1]); if (j) wait_job(j, atoi(argv[2])); else { report("END tag=%s status=error unknown", argv[1]); failures++; } return; }
    if (!strcmp(c, "kill") && argc >= 2) { struct job* j = find_job(argv[1]); int sig = argc > 2 ? atoi(argv[2]) : SIGTERM; if (j) { kill(-j->pid, sig); report("KILL tag=%s signal=%d", argv[1], sig); } return; }
    if (!strcmp(c, "sleep") && argc == 2) { uint64_t end = now_ms() + (uint64_t)atoll(argv[1]); while (now_ms() < end) pump(50); return; }
    if ((!strcmp(c, "waitfile") || !strcmp(c, "waitgone")) && argc == 3) {
        bool want = !strcmp(c, "waitfile"); uint64_t t0 = now_ms(), end = t0 + (uint64_t)atoi(argv[2]) * 1000; struct stat st;
        while ((stat(argv[1], &st) == 0) != want && now_ms() < end) pump(100);
        bool ok = (stat(argv[1], &st) == 0) == want;
        report("FILE %s path=%s %s ms=%llu", c, argv[1], ok ? "ok" : "timeout", (unsigned long long)(now_ms() - t0)); if (!ok) failures++; return;
    }
    if (!strcmp(c, "cat") && argc == 2) { print_file(argv[1]); return; }
    if ((!strcmp(c, "ls") || !strcmp(c, "tree") || !strcmp(c, "du")) && argc >= 2) {
        uint64_t bytes = 0, files = 0, dirs = 0; int depth = !strcmp(c, "ls") ? 1 : !strcmp(c, "du") ? 64 : argc > 2 ? atoi(argv[2]) : 8;
        uint64_t t0 = now_ms(); walk(argv[1], depth, strcmp(c, "du") != 0, &bytes, &files, &dirs);
        report("%s path=%s bytes=%llu files=%llu dirs=%llu ms=%llu", c, argv[1], (unsigned long long)bytes, (unsigned long long)files, (unsigned long long)dirs, (unsigned long long)(now_ms() - t0)); return;
    }
    if (!strcmp(c, "exists") && argc == 2) { struct stat st; bool ok = !lstat(argv[1], &st); report("EXISTS path=%s %s", argv[1], ok ? "yes" : "no"); return; }
    if (!strcmp(c, "size") && argc == 2) { struct stat st; if (stat(argv[1], &st)) report("SIZE path=%s error=%s", argv[1], strerror(errno)); else report("SIZE path=%s bytes=%lld", argv[1], (long long)st.st_size); return; }
    if (!strcmp(c, "write") && argc == 3) { FILE* f = fopen(argv[1], "w"); if (!f || fprintf(f, "%s\n", argv[2]) < 0 || fclose(f)) { report("write %s: %s", argv[1], strerror(errno)); failures++; } return; }
    if (!strcmp(c, "rm") && argc == 2) { if (unlink(argv[1])) report("rm %s: %s", argv[1], strerror(errno)); return; }
    if (!strcmp(c, "cmp") && argc == 3) { int r = compare(argv[1], argv[2]); report("CMP %s %s %s", argv[1], argv[2], r == 0 ? "same" : r > 0 ? "differ" : "error"); if (r) failures++; return; }
    if (!strcmp(c, "procs") && argc == 2) { report("PROCS text=%s count=%d", argv[1], matching_processes(argv[1], true)); return; }
    if (!strcmp(c, "procwait") && argc == 3) {
        uint64_t t0 = now_ms(), end = t0 + (uint64_t)atoi(argv[2]) * 1000; int count;
        while ((count = matching_processes(argv[1], false)) > 0 && now_ms() < end) pump(250);
        if (count) matching_processes(argv[1], true);
        report("PROCWAIT text=%s remaining=%d ms=%llu", argv[1], count, (unsigned long long)(now_ms() - t0)); if (count) failures++; return;
    }
    if (!strcmp(c, "host") && (argc == 2 || argc == 3)) {
        host_port = atoi(argv[1]);
        if (argc == 3) snprintf(host_address, sizeof(host_address), "%s", argv[2]);
        return;
    }
    if (!strcmp(c, "sync") && argc == 1) { uint64_t t0 = now_ms(); sync(); report("SYNC ms=%llu", (unsigned long long)(now_ms() - t0)); return; }
    if (!strcmp(c, "gate") && argc == 3) {
        char target[512]; snprintf(target, sizeof(target), "/gate/%s", argv[1]); uint64_t t0 = now_ms(), end = t0 + (uint64_t)atoi(argv[2]) * 1000; int code;
        report("GATE WAIT %s", argv[1]);
        while ((code = http("GET", target, NULL, NULL)) != 200 && now_ms() < end) pump(300);
        report("GATE %s %s ms=%llu", argv[1], code == 200 ? "open" : "timeout", (unsigned long long)(now_ms() - t0)); if (code != 200) failures++; return;
    }
    if (!strcmp(c, "fetch") && argc == 3) { char target[512]; snprintf(target, sizeof(target), "/file/%s", argv[1]); int code = http("GET", target, NULL, argv[2]); report("FETCH %s -> %s code=%d", argv[1], argv[2], code); if (code != 200) failures++; return; }
    if (!strcmp(c, "post") && argc == 3) { char target[512]; snprintf(target, sizeof(target), "/upload/%s", argv[2]); int code = http("POST", target, argv[1], NULL); report("POST %s -> %s code=%d", argv[1], argv[2], code); if (code != 200) failures++; return; }
    report("BAD COMMAND %s (argc=%d)", c, argc); failures++;
}
int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s SCRIPT\n", argv[0]); return 2; }
    FILE* script = fopen(argv[1], "r"); if (!script) { perror(argv[1]); return 2; }
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_JOBS; i++) jobs[i].out = -1;
    report("BEGIN script=%s pid=%d", argv[1], (int)getpid());
    char line[8192]; int number = 0;
    while (fgets(line, sizeof(line), script)) {
        number++; size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char* args[256]; int count = split(line, args, 256);
        if (count < 0) { report("BAD LINE %d", number); failures++; continue; }
        if (count) command(count, args);
    }
    fclose(script);
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].pid > 0) wait_job(&jobs[i], 1);
    report("DONE failures=%d", failures);
    return failures ? 1 : 0;
}
