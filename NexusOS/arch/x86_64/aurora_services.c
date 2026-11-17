/* Aurora is /sbin/init (pid 1, the desktop user). At boot it starts the service manager /usr/lib/nexus/nexusd
 * (set-user-ID root on an ext4 root; see nexusd.c), then asks it for the desktop user's session units (D-Bus session
 * bus, notification bridge) once its control socket answers. Applications get DBUS_SESSION_BUS_ADDRESS when D-Bus is
 * installed. At shutdown nexusd is stopped first, so services end in order and the journal is flushed before the
 * remaining processes are signalled. A nexusd that dies unexpectedly is restarted (at most 3 times). */
#define _GNU_SOURCE
#include "aurora_services.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "nexusd_proto.h"

#define NEXUSD_PROGRAM "/usr/lib/nexus/nexusd"
/* Desktop notifications: nexus-notifyd (org.freedesktop.Notifications on the session bus) sends one datagram
 * "<app>\x1f<summary>\x1f<body>" per Notify to $XDG_RUNTIME_DIR/aurora-notify (mode 0600, the user's runtime directory). */
static void (*notify_show)(const char* text);
static int notify_ready(int fd, uint32_t mask, void* data) {
    (void)mask; (void)data;
    for (;;) {
        char buf[1024]; ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n <= 0) return 0;
        buf[n] = 0;
        char* fields[3] = {buf, "", ""}; unsigned count = 1;
        for (char* p = buf; *p && count < 3; p++) if (*p == '\x1f') { *p = 0; fields[count++] = p + 1; }
        for (unsigned i = 0; i < 3; i++) for (char* p = fields[i]; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
        char text[512];
        snprintf(text, sizeof(text), "%s%s%s%s%s", fields[0], fields[0][0] ? ": " : "", fields[1], fields[2][0] ? " - " : "", fields[2]);
        printf("[64] AURORA NOTIFY app=%s summary=%s\n", fields[0], fields[1]); fflush(stdout);
        if (notify_show) notify_show(text);
    }
}
static void notify_listen(struct wl_event_loop* loop) {
    const char* dir = getenv("XDG_RUNTIME_DIR"); if (!loop || !dir || dir[0] != '/') return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if ((size_t)snprintf(a.sun_path, sizeof(a.sun_path), "%s/aurora-notify", dir) >= sizeof(a.sun_path)) return;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) return;
    unlink(a.sun_path);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) || chmod(a.sun_path, 0600) || !wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, notify_ready, NULL)) {
        perror("aurora notifications"); close(fd); return;
    }
}
static pid_t services_pid = -1, session_pid = -1;
static unsigned restarts;
static bool stopping;

static void spawn_manager(void) {
    pid_t pid = fork();
    if (!pid) {
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL); signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        execl(NEXUSD_PROGRAM, "nexusd", "--system", (char*)NULL); _exit(127);
    }
    if (pid > 0) { services_pid = pid; printf("[64] AURORA SERVICES nexusd pid=%ld\n", (long)pid); fflush(stdout); }
}
/* child: wait for the control socket, then "session <uid>" */
static void spawn_session(void) {
    pid_t pid = fork();
    if (pid) { if (pid > 0) session_pid = pid; return; }
    signal(SIGCHLD, SIG_DFL);
    for (int attempt = 0; attempt < 600; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) _exit(1);
        struct sockaddr_un a = {.sun_family = AF_UNIX}; strcpy(a.sun_path, NEXUSD_CONTROL);
        if (!connect(fd, (struct sockaddr*)&a, sizeof(a))) {
            char line[64]; int n = snprintf(line, sizeof(line), "session %u\n", (unsigned)getuid());
            if (write(fd, line, (size_t)n) == n) { char reply[512]; while (read(fd, reply, sizeof(reply)) > 0) {} }
            close(fd); _exit(0);
        }
        close(fd);
        struct timespec t = {0, 100000000}; nanosleep(&t, NULL);
    }
    _exit(1);
}
void aurora_services_start(struct wl_event_loop* loop, void (*notify)(const char* text)) {
    notify_show = notify; notify_listen(loop);
    if (access(NEXUSD_PROGRAM, X_OK)) return;
    if (!access("/usr/bin/dbus-daemon", X_OK)) {
        char address[96]; snprintf(address, sizeof(address), "unix:path=/run/user/%u/bus", (unsigned)getuid());
        setenv("DBUS_SESSION_BUS_ADDRESS", address, 0);
    }
    spawn_manager();
    if (services_pid > 0) spawn_session();
}
bool aurora_services_reaped(pid_t pid, int status, bool shutting_down) {
    if (pid == session_pid) { session_pid = -1; return true; }
    if (pid != services_pid) return false;
    services_pid = -1;
    printf("[64] AURORA SERVICES nexusd exited status=%d\n", status); fflush(stdout);
    bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!shutting_down && !stopping && !clean && restarts < 3) { restarts++; spawn_manager(); if (services_pid > 0) spawn_session(); }
    return true;
}
/* nexusd runs as root, so pid 1 (the desktop user) cannot signal it: it asks over the control socket, which accepts
 * "shutdown" from pid 1. A nexusd that kept the desktop user's identity (FAT32 root) is signalled directly. */
void aurora_services_stop(void) {
    if (stopping) return;
    stopping = true;
    if (services_pid <= 0) return;
    if (!kill(services_pid, SIGTERM)) return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX}; strcpy(a.sun_path, NEXUSD_CONTROL);
    if (fd >= 0 && !connect(fd, (struct sockaddr*)&a, sizeof(a))) {
        if (write(fd, "shutdown\n", 9) == 9) { char reply[64]; struct timeval tv = {5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); if (read(fd, reply, sizeof(reply)) < 0) {} }
        printf("[64] AURORA SERVICES shutdown requested\n"); fflush(stdout);
    } else { perror("aurora: cannot reach the service manager"); }
    if (fd >= 0) close(fd);
}
bool aurora_services_running(void) { return services_pid > 0; }
