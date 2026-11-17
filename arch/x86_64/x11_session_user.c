/* nexus-x11-session: a rootful Xwayland X screen inside one Aurora window, plus X clients on it.
 *
 *   nexus-x11-session [--geometry WxH] [--display N] [CLIENT ...]
 *   nexus-x11-session --rootless PROGRAM [ARGS...]   (PROGRAM on Aurora's rootless display: see rootless() below)
 *
 * Starts Xwayland :N (default 1, geometry 1280x800) rootful with shared-memory buffers, waits until it accepts
 * connections (-displayfd), exports DISPLAY=:N and starts every CLIENT (a program path, run without arguments;
 * default: xterm if installed, else xeyes, else nexus-xtest; with none installed, Xwayland runs alone). The session
 * ends when Xwayland exits (its window is closed) or when every client has exited; the rest is then terminated.
 * Programs inherit WAYLAND_DISPLAY and
 * XDG_RUNTIME_DIR from Aurora and get DISPLAY=:N plus XAUTHORITY (a fresh MIT-MAGIC-COOKIE-1, $XDG_RUNTIME_DIR/nexus-xauth-N). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 8
static void say(const char* text) { printf("[64] X11 SESSION %s\n", text); fflush(stdout); }

/* nexus-x11-session --rootless PROGRAM [ARGS...]: PROGRAM on Aurora's rootless X display (aurora_xwm.c publishes it in
 * $XDG_RUNTIME_DIR/aurora-x11.env), each X window a real Aurora window. */
static int rootless(int argc, char** argv) {
    if (argc < 3) { fputs("usage: nexus-x11-session --rootless PROGRAM [ARGS...]\n", stderr); return 2; }
    const char* runtime = getenv("XDG_RUNTIME_DIR"); char path[256], line[320]; bool display = false;
    snprintf(path, sizeof(path), "%s/aurora-x11.env", runtime && runtime[0] == '/' ? runtime : "/run/user/1000");
    FILE* f = fopen(path, "re");
    if (!f) { fprintf(stderr, "Aurora offers no rootless X display (%s: %s): Xwayland missing, or rootless=0 in ~/.aurora/x11.conf\n", path, strerror(errno)); return 1; }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "DISPLAY=:", 9) && strlen(line) < 20) { setenv("DISPLAY", line + 8, 1); display = true; }
        else if (!strncmp(line, "XAUTHORITY=/", 12)) setenv("XAUTHORITY", line + 11, 1);
    }
    fclose(f);
    if (!display) { fprintf(stderr, "%s names no display\n", path); return 1; }
    execvp(argv[2], argv + 2); fprintf(stderr, "%s: %s\n", argv[2], strerror(errno)); return 127;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--rootless")) return rootless(argc, argv);
    const char* geometry = "1280x800"; int display = 1, first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1] == '-') {
        if (!strcmp(argv[first], "--geometry") && first + 1 < argc) { geometry = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--display") && first + 1 < argc) { display = atoi(argv[first + 1]); first += 2; }
        else { fprintf(stderr, "usage: %s [--geometry WxH] [--display N] [CLIENT ...]\n", argv[0]); return 2; }
    }
    if (display < 0 || display > 63) { fputs("display number out of range\n", stderr); return 2; }
    const char* defaults[] = {"/usr/bin/xterm", "/usr/bin/xeyes", "/usr/bin/nexus-xtest"};
    const char* clients[MAX_CLIENTS]; int count = 0;
    for (int i = first; i < argc && count < MAX_CLIENTS; i++) clients[count++] = argv[i];
    if (!count) for (unsigned i = 0; i < 3 && !count; i++) if (!access(defaults[i], X_OK)) clients[count++] = defaults[i];
    if (!count) say("NO CLIENT installed (xterm, xeyes, nexus-xtest): Xwayland runs alone until its window is closed");
    if (!getenv("WAYLAND_DISPLAY")) { fputs("WAYLAND_DISPLAY is not set: start the session from Aurora\n", stderr); return 1; }
    if (mkdir("/tmp/.X11-unix", 01777) && errno != EEXIST) perror("mkdir /tmp/.X11-unix");
    /* Access control as xinit does it: a fresh MIT-MAGIC-COOKIE-1 in an Xauthority file given to Xwayland (-auth) and to
     * the clients (XAUTHORITY). A FamilyWild entry matches local connections whatever the host name. */
    char authority[256]; const char* runtime = getenv("XDG_RUNTIME_DIR");
    snprintf(authority, sizeof(authority), "%s/nexus-xauth-%d", runtime && runtime[0] ? runtime : "/tmp", display);
    uint8_t cookie[16];
    if (getrandom(cookie, sizeof(cookie), 0) != (ssize_t)sizeof(cookie)) { perror("getrandom"); return 1; }
    {
        char number[8]; int digits = snprintf(number, sizeof(number), "%d", display);
        uint8_t entry[64]; size_t n = 0;
        entry[n++] = 0xFF; entry[n++] = 0xFF; entry[n++] = 0; entry[n++] = 0;            /* family Wild, empty address */
        entry[n++] = 0; entry[n++] = (uint8_t)digits; memcpy(entry + n, number, (size_t)digits); n += (size_t)digits;
        entry[n++] = 0; entry[n++] = 18; memcpy(entry + n, "MIT-MAGIC-COOKIE-1", 18); n += 18;
        entry[n++] = 0; entry[n++] = 16; memcpy(entry + n, cookie, 16); n += 16;
        int fd = open(authority, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, entry, n) != (ssize_t)n || close(fd)) { perror(authority); return 1; }
    }
    setenv("XAUTHORITY", authority, 1);
    int ready[2]; if (pipe2(ready, O_CLOEXEC)) { perror("pipe"); return 1; }
    char name[16], fd_text[16]; snprintf(name, sizeof(name), ":%d", display);
    pid_t server = fork();
    if (server < 0) { perror("fork"); return 1; }
    if (!server) {
        int fd = dup(ready[1]); if (fd < 0) _exit(127);     /* the dup is not close-on-exec */
        snprintf(fd_text, sizeof(fd_text), "%d", fd);
        /* -dumbSched: no SIGALRM time slicing (setitimer); -shm: software buffers, no glamor */
        execl("/usr/bin/Xwayland", "Xwayland", name, "-geometry", geometry, "-shm", "-noreset", "-nolisten", "tcp",
              "-auth", authority, "-dumbSched", "-displayfd", fd_text, (char*)NULL);
        perror("exec Xwayland"); _exit(127);
    }
    close(ready[1]);
    char answer[32] = {0}; size_t got = 0; struct pollfd p = {ready[0], POLLIN, 0};
    while (got < sizeof(answer) - 1 && !memchr(answer, '\n', got)) {
        int r = poll(&p, 1, 60000);
        if (r <= 0) { say("FAILED Xwayland did not become ready"); kill(server, SIGTERM); return 1; }
        ssize_t n = read(ready[0], answer + got, sizeof(answer) - 1 - got);
        if (n <= 0) { int status; waitpid(server, &status, 0); printf("[64] X11 SESSION FAILED Xwayland exited status=%d\n", status); fflush(stdout); return 1; }
        got += (size_t)n;
    }
    close(ready[0]);
    snprintf(name, sizeof(name), ":%d", atoi(answer)); setenv("DISPLAY", name, 1);
    printf("[64] X11 SESSION READY DISPLAY=%s pid=%d\n", name, (int)server); fflush(stdout);
    pid_t pids[MAX_CLIENTS]; int running = 0;
    for (int i = 0; i < count; i++) {
        pids[i] = fork();
        if (!pids[i]) { execl(clients[i], clients[i], (char*)NULL); perror(clients[i]); _exit(127); }
        if (pids[i] > 0) { running++; printf("[64] X11 SESSION CLIENT %s pid=%d\n", clients[i], (int)pids[i]); fflush(stdout); }
    }
    int status = 0, result = 0;
    for (;;) {
        pid_t done = wait(&status);
        if (done < 0) { if (errno == EINTR) continue; break; }
        if (done == server) { printf("[64] X11 SESSION Xwayland exited status=%d\n", status); fflush(stdout); result = WIFEXITED(status) ? WEXITSTATUS(status) : 1; server = -1; break; }
        for (int i = 0; i < count; i++) if (pids[i] == done) { pids[i] = -1; running--; printf("[64] X11 SESSION CLIENT %s exited status=%d\n", clients[i], status); fflush(stdout); }
        if (!running) break;
    }
    for (int i = 0; i < count; i++) if (pids[i] > 0) kill(pids[i], SIGTERM);
    if (server > 0) { kill(server, SIGTERM); waitpid(server, &status, 0); }
    while (wait(&status) > 0) {}
    unlink(authority); say("END"); return result;
}
