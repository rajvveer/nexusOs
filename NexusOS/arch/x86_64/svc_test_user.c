/* nexus-svc-test - helper for tests/test_x64_services.py (installed only on test disks).
 *   notify DELAY_MS        sleep, send READY=1 + STATUS to $NOTIFY_SOCKET, then sleep forever
 *   crash FILE             append the monotonic time to FILE, exit 3 (restart/backoff/start-limit checks)
 *   flaky FILE N           like crash for the first N starts (counted in FILE), then run forever
 *   env FILE               write uid, gid, groups, cwd, $FOO, $HOME, $USER, $INVOCATION_ID set? to FILE
 *   control                hostile requests to /run/nexus/control (over-long line, control bytes, bad names): every
 *                          reply must be an error; prints "CONTROL ok" or the first problem
 *   time                   clock_settime/settimeofday/adjtimex/clock_adjtime: EPERM without CAP_SYS_TIME, EINVAL for bad
 *                          arguments, a step and a frequency setting that read back (root); prints "TIME ok" or the problem
 *   kmsg                   /dev/kmsg: write a record, read records back (record format), EINVAL for a tiny buffer,
 *                          syslog(2) READ_ALL/SIZE_BUFFER; prints "KMSG ok" or the problem
 *   rlimit                 every RLIMIT_* readable; RLIMIT_FSIZE gives SIGXFSZ/EFBIG; RLIMIT_NPROC gives EAGAIN; prints "RLIMIT ok"
 * socket activation (tests/test_x64_units.py):
 *   listen                 an Accept=no service: checks LISTEN_PID/LISTEN_FDS/LISTEN_FDNAMES (sd_listen_fds), then serves every
 *                          passed socket: a stream connection or a datagram gets "LISTEN fds=N names=NAMES uid=U fd=K <request>"
 *   inetd                  an Accept=yes instance with StandardInput=socket: reads one line from stdin, answers on stdout
 *                          "INETD fds=N uid=U <line>" (fd 3 is the same connection)
 *   conn                   an Accept=yes instance without stdin: fd 3 is the connection (LISTEN_FDS=1, not listening)
 *   client ADDR TEXT [HOLD_MS]   connect (unix path, @abstract or HOST:PORT), send TEXT + newline, print the answer
 *                          (or "CLIENT eof"); HOLD_MS keeps the connection open that long first
 *   dgram PATH TEXT        send one datagram to a unix datagram socket */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/klog.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail(const char* tag, const char* what) { printf("%s FAIL %s (errno %d %s)\n", tag, what, errno, strerror(errno)); return 1; }
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static int notify_mode(int delay) {
    usleep((useconds_t)delay * 1000);
    const char* path = getenv("NOTIFY_SOCKET"); if (!path) return fail("NOTIFY", "no NOTIFY_SOCKET");
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    const char msg[] = "READY=1\nSTATUS=serving test requests";
    if (sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr*)&a, sizeof(a)) < 0) return fail("NOTIFY", "sendto");
    for (;;) pause();
}
static int crash_mode(const char* file) {
    FILE* f = fopen(file, "a"); if (f) { fprintf(f, "%lld\n", mono_ms()); fclose(f); }
    return 3;
}
static int flaky_mode(const char* file, int n) {
    int count = 0; FILE* f = fopen(file, "r"); if (f) { if (fscanf(f, "%d", &count) != 1) count = 0; fclose(f); }
    f = fopen(file, "w"); if (f) { fprintf(f, "%d\n", count + 1); fclose(f); }
    if (count < n) return 4;
    printf("flaky: running after %d failures\n", count); fflush(stdout);
    for (;;) pause();
}
static int env_mode(const char* file) {
    FILE* f = fopen(file, "w"); if (!f) return fail("ENV", "open");
    gid_t groups[64]; int n = getgroups(64, groups);
    char cwd[256]; if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    fprintf(f, "uid=%u gid=%u ngroups=%d cwd=%s FOO=%s HOME=%s USER=%s INVOCATION=%s\n", (unsigned)getuid(), (unsigned)getgid(), n, cwd,
            getenv("FOO") ? getenv("FOO") : "-", getenv("HOME") ? getenv("HOME") : "-", getenv("USER") ? getenv("USER") : "-", getenv("INVOCATION_ID") ? "yes" : "no");
    fclose(f); printf("env written\n"); return 0;
}
static int control_reply(const char* request, size_t length, char* reply, size_t size) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, "/run/nexus/control");
    if (connect(fd, (struct sockaddr*)&a, sizeof(a))) { close(fd); return -1; }
    if (write(fd, request, length) < 0) { close(fd); return -1; }
    size_t used = 0; ssize_t r;
    while (used + 1 < size && (r = read(fd, reply + used, size - 1 - used)) > 0) used += (size_t)r;
    reply[used] = 0; close(fd); return 0;
}
static int control_mode(void) {
    char reply[8192], big[3000];
    memset(big, 'a', sizeof(big)); big[sizeof(big) - 1] = '\n';
    if (control_reply(big, sizeof(big), reply, sizeof(reply))) return fail("CONTROL", "connect");
    if (strstr(reply, "\x01" "0")) { printf("CONTROL FAIL over-long line accepted\n"); return 1; }
    const char* bad[] = {"status ../../etc/passwd\n", "start a\x02.service\n", "frobnicate x.service\n", "start\n", "status /etc/shadow\n",
                         "enable ..service\n", "status x.service.service\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        if (control_reply(bad[i], strlen(bad[i]), reply, sizeof(reply))) return fail("CONTROL", "connect");
        char* end = strrchr(reply, '\x01');
        if (!end || atoi(end + 1) == 0) { printf("CONTROL FAIL request %u accepted: %s\n", i, reply); return 1; }
    }
    if (getuid()) {   /* unprivileged: mutating verbs are refused, reading works */
        if (control_reply("stop cron.service\n", 18, reply, sizeof(reply))) return fail("CONTROL", "connect");
        char* end = strrchr(reply, '\x01'); if (!end || atoi(end + 1) != 4) { printf("CONTROL FAIL user stop not refused: %s\n", reply); return 1; }
        if (control_reply("status cron.service\n", 20, reply, sizeof(reply))) return fail("CONTROL", "connect");
        if (!strstr(reply, "cron.service")) { printf("CONTROL FAIL user status: %s\n", reply); return 1; }
    }
    /* a silent client must not block the manager: it is dropped after 10 s while others are served */
    int idle = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a = {.sun_family = AF_UNIX}; strcpy(a.sun_path, "/run/nexus/control");
    if (connect(idle, (struct sockaddr*)&a, sizeof(a))) return fail("CONTROL", "idle connect");
    if (control_reply("ping\n", 5, reply, sizeof(reply)) || !strstr(reply, "pong")) { printf("CONTROL FAIL ping with an idle client: %s\n", reply); return 1; }
    close(idle);
    printf("CONTROL ok\n"); return 0;
}
static int time_mode(void) {
    struct timespec ts; struct timeval tv; struct timex tx;
    bool root = geteuid() == 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (!root) {
        if (clock_settime(CLOCK_REALTIME, &ts) != -1 || errno != EPERM) return fail("TIME", "user clock_settime not EPERM");
        tv.tv_sec = ts.tv_sec; tv.tv_usec = 0;
        if (settimeofday(&tv, NULL) != -1 || errno != EPERM) return fail("TIME", "user settimeofday not EPERM");
        memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_FREQUENCY; tx.freq = 1 << 16;
        if (adjtimex(&tx) != -1 || errno != EPERM) return fail("TIME", "user adjtimex not EPERM");
        memset(&tx, 0, sizeof(tx));
        if (adjtimex(&tx) < 0) return fail("TIME", "user adjtimex read");
        printf("TIME ok (user)\n"); return 0;
    }
    struct timespec bad = {ts.tv_sec, 1000000000};
    if (clock_settime(CLOCK_REALTIME, &bad) != -1 || errno != EINVAL) return fail("TIME", "bad tv_nsec not EINVAL");
    if (clock_settime(CLOCK_MONOTONIC, &ts) != -1 || errno != EINVAL) return fail("TIME", "CLOCK_MONOTONIC settable");
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_TICK; tx.tick = 20000;
    if (adjtimex(&tx) != -1 || errno != EINVAL) return fail("TIME", "bad tick not EINVAL");
    memset(&tx, 0, sizeof(tx)); tx.modes = 0x0800;   /* undefined mode bit */
    if (adjtimex(&tx) != -1 || errno != EINVAL) return fail("TIME", "undefined mode not EINVAL");
    if (syscall(305, 1 /* CLOCK_MONOTONIC */, &tx) != -1 || errno != EOPNOTSUPP) return fail("TIME", "clock_adjtime(MONOTONIC) not EOPNOTSUPP");
    if (syscall(159, (void*)8) != -1 || errno != EFAULT) return fail("TIME", "adjtimex bad pointer not EFAULT");
    /* step forward by 1000 s, read it back, step back */
    clock_gettime(CLOCK_REALTIME, &ts); struct timespec mono0; clock_gettime(CLOCK_MONOTONIC, &mono0);
    struct timespec later = {ts.tv_sec + 1000, ts.tv_nsec};
    if (clock_settime(CLOCK_REALTIME, &later)) return fail("TIME", "clock_settime");
    struct timespec check, mono1; clock_gettime(CLOCK_REALTIME, &check); clock_gettime(CLOCK_MONOTONIC, &mono1);
    long long drift = (check.tv_sec - later.tv_sec) * 1000LL + (check.tv_nsec - later.tv_nsec) / 1000000;
    if (drift < 0 || drift > 2000) { printf("TIME FAIL step read back %lld ms off\n", drift); return 1; }
    if (mono1.tv_sec - mono0.tv_sec > 5) return fail("TIME", "monotonic jumped with the step");
    time_t seconds = time(NULL); if (seconds < later.tv_sec - 1) return fail("TIME", "time() (vDSO) did not follow the step");
    struct timeval now; gettimeofday(&now, NULL); if (now.tv_sec < later.tv_sec - 1) return fail("TIME", "gettimeofday (vDSO) did not follow the step");
    clock_gettime(CLOCK_REALTIME, &check); check.tv_sec -= 1000;
    if (clock_settime(CLOCK_REALTIME, &check)) return fail("TIME", "step back");
    /* frequency: +100 ppm read back; the clock then gains ~100 us per second against CLOCK_MONOTONIC. First undo what an
     * NTP daemon that ran before may have left (tick, a slew still in progress), as they add to the rate. */
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_TICK; tx.tick = 10000; adjtimex(&tx);
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_OFFSET_SINGLESHOT; tx.offset = 0; adjtimex(&tx);
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_FREQUENCY; tx.freq = 100 << 16;
    if (adjtimex(&tx) < 0) return fail("TIME", "ADJ_FREQUENCY");
    memset(&tx, 0, sizeof(tx)); if (adjtimex(&tx) < 0 || tx.freq != 100 << 16) { printf("TIME FAIL freq read back %ld\n", (long)tx.freq); return 1; }
    struct timespec r0, m0, r1, m1; clock_gettime(CLOCK_REALTIME, &r0); clock_gettime(CLOCK_MONOTONIC, &m0);
    sleep(2);
    clock_gettime(CLOCK_REALTIME, &r1); clock_gettime(CLOCK_MONOTONIC, &m1);
    long long real_ns = (r1.tv_sec - r0.tv_sec) * 1000000000LL + (r1.tv_nsec - r0.tv_nsec), mono_ns = (m1.tv_sec - m0.tv_sec) * 1000000000LL + (m1.tv_nsec - m0.tv_nsec);
    long long gained_us = (real_ns - mono_ns) / 1000;
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_FREQUENCY; tx.freq = 0; adjtimex(&tx);
    if (gained_us < 100 || gained_us > 400) { printf("TIME FAIL +100 ppm gained %lld us in 2 s\n", gained_us); return 1; }
    /* adjtime-style slew: -50 ms, read back the remaining amount */
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_OFFSET_SINGLESHOT; tx.offset = -50000;
    if (adjtimex(&tx) < 0) return fail("TIME", "ADJ_OFFSET_SINGLESHOT");
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_OFFSET_SS_READ;
    if (adjtimex(&tx) < 0 || tx.offset > -40000 || tx.offset < -50000) { printf("TIME FAIL slew remaining %ld us\n", (long)tx.offset); return 1; }
    memset(&tx, 0, sizeof(tx)); tx.modes = ADJ_OFFSET_SINGLESHOT; tx.offset = 0; adjtimex(&tx);
    printf("TIME ok (root, +100 ppm gained %lld us in 2 s)\n", gained_us); return 0;
}
static int kmsg_mode(void) {
    bool root = geteuid() == 0;
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (!root) { if (fd >= 0 || errno != EPERM) return fail("KMSG", "user open not EPERM"); char b[64]; if (klogctl(3, b, sizeof(b)) != -1 || errno != EPERM) return fail("KMSG", "user syslog not EPERM"); printf("KMSG ok (user)\n"); return 0; }
    if (fd < 0) return fail("KMSG", "open");
    int w = open("/dev/kmsg", O_WRONLY | O_CLOEXEC); if (w < 0) return fail("KMSG", "open write");
    const char rec[] = "<3>svctest: kmsg record from user space\n";
    if (write(w, rec, sizeof(rec) - 1) != (ssize_t)(sizeof(rec) - 1)) return fail("KMSG", "write");
    close(w);
    char small[8]; ssize_t r;   /* EPIPE first when older records were overwritten (as on Linux) */
    do r = read(fd, small, sizeof(small)); while (r < 0 && errno == EPIPE);
    if (r != -1 || errno != EINVAL) return fail("KMSG", "tiny buffer not EINVAL");
    char buf[2048]; bool found = false; unsigned records = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) { if (errno == EAGAIN) break; if (errno == EPIPE) continue; return fail("KMSG", "read"); }
        buf[n] = 0; records++;
        unsigned prio; unsigned long long seq, usec; char flag;
        if (sscanf(buf, "%u,%llu,%llu,%c;", &prio, &seq, &usec, &flag) != 4 || buf[n - 1] != '\n') { printf("KMSG FAIL bad record %s\n", buf); return 1; }
        if (strstr(buf, "svctest: kmsg record from user space")) { found = true; if (prio != 11) { printf("KMSG FAIL user record prio %u\n", prio); return 1; } }
    }
    close(fd);
    if (!found || records < 10) { printf("KMSG FAIL found=%d records=%u\n", found, records); return 1; }
    int size = klogctl(10, NULL, 0); if (size <= 0) return fail("KMSG", "SIZE_BUFFER");
    char* all = malloc((size_t)size); int got = klogctl(3, all, size);
    if (got <= 0 || !strstr(all, "svctest: kmsg record")) { printf("KMSG FAIL READ_ALL %d\n", got); return 1; }
    free(all);
    printf("KMSG ok (%u records)\n", records); return 0;
}
static int rlimit_mode(void) {
    for (int r = 0; r < 16; r++) { struct rlimit l; if (getrlimit(r, &l)) { printf("RLIMIT FAIL getrlimit(%d): %s\n", r, strerror(errno)); return 1; } }
    pid_t child = fork();
    if (!child) {
        struct rlimit l = {4096, 4096}; if (setrlimit(RLIMIT_FSIZE, &l)) _exit(10);
        signal(SIGXFSZ, SIG_IGN);
        int fd = open("/tmp/svctest-fsize", O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd < 0) _exit(11);
        char data[8192] = {0};
        ssize_t first = write(fd, data, sizeof(data));            /* short write up to the limit */
        ssize_t second = write(fd, data, 1);                       /* at the limit: EFBIG */
        int error = errno; close(fd); unlink("/tmp/svctest-fsize");
        _exit(first == 4096 && second == -1 && error == EFBIG ? 0 : 12);
    }
    int status; waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { printf("RLIMIT FAIL RLIMIT_FSIZE (status %d)\n", status); return 1; }
    child = fork();
    if (!child) {   /* default SIGXFSZ terminates */
        struct rlimit l = {0, 0}; setrlimit(RLIMIT_FSIZE, &l);
        int fd = open("/tmp/svctest-fsize2", O_WRONLY | O_CREAT | O_TRUNC, 0600); char c = 0;
        if (write(fd, &c, 1) < 0) {} _exit(13);
    }
    waitpid(child, &status, 0); unlink("/tmp/svctest-fsize2");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGXFSZ) { printf("RLIMIT FAIL SIGXFSZ (status %d)\n", status); return 1; }
    if (geteuid()) {
        child = fork();
        if (!child) {
            struct rlimit l = {1, 1}; if (setrlimit(RLIMIT_NPROC, &l)) _exit(20);
            pid_t g = fork(); if (g == 0) _exit(0);
            _exit(g < 0 && errno == EAGAIN ? 0 : 21);
        }
        waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { printf("RLIMIT FAIL RLIMIT_NPROC (status %d)\n", status); return 1; }
    }
    printf("RLIMIT ok\n"); return 0;
}
/* the fds a socket-activated service got (sd_listen_fds semantics); 0 when LISTEN_PID is not us */
static int listen_fds(void) {
    const char* pid = getenv("LISTEN_PID"); const char* fds = getenv("LISTEN_FDS");
    if (!pid || !fds || atol(pid) != (long)getpid()) return 0;
    int n = atoi(fds); if (n < 0 || n > 64) return 0;
    for (int fd = 3; fd < 3 + n; fd++) if (fcntl(fd, F_GETFD) < 0) return -1;
    return n;
}
static int listen_mode(void) {
    int n = listen_fds();
    if (n <= 0) { printf("LISTEN FAIL no sockets (LISTEN_PID=%s LISTEN_FDS=%s)\n", getenv("LISTEN_PID") ? getenv("LISTEN_PID") : "-", getenv("LISTEN_FDS") ? getenv("LISTEN_FDS") : "-"); return 1; }
    const char* names = getenv("LISTEN_FDNAMES") ? getenv("LISTEN_FDNAMES") : "-";
    printf("LISTEN serving %d sockets (%s) as uid %u\n", n, names, (unsigned)getuid());
    for (;;) {
        struct pollfd p[16]; int m = n < 16 ? n : 16;
        for (int i = 0; i < m; i++) { p[i].fd = 3 + i; p[i].events = POLLIN; p[i].revents = 0; }
        if (poll(p, (nfds_t)m, -1) < 0) { if (errno == EINTR) continue; return fail("LISTEN", "poll"); }
        for (int i = 0; i < m; i++) {
            if (!p[i].revents) continue;
            int type = 0; socklen_t tl = sizeof(type); getsockopt(p[i].fd, SOL_SOCKET, SO_TYPE, &type, &tl);
            char req[256] = ""; char out[512]; struct stat st;
            if (!fstat(p[i].fd, &st) && S_ISFIFO(st.st_mode)) {   /* ListenFIFO */
                ssize_t r = read(p[i].fd, req, sizeof(req) - 1); if (r <= 0) continue; req[r] = 0; char* nl = strchr(req, '\n'); if (nl) *nl = 0;
                printf("LISTEN fifo fds=%d names=%s fd=%d %s\n", n, names, p[i].fd, req); fflush(stdout);
                if (!strcmp(req, "exit")) return 0;
                continue;
            }
            if (type == SOCK_RAW) {   /* ListenNetlink: one message */
                ssize_t r = recv(p[i].fd, out, sizeof(out), 0); if (r < 16) continue;
                printf("LISTEN netlink fds=%d names=%s fd=%d bytes=%zd type=%u\n", n, names, p[i].fd, r, (unsigned)*(uint16_t*)(out + 4)); fflush(stdout);
                continue;
            }
            if (type == SOCK_DGRAM) {
                ssize_t r = recv(p[i].fd, req, sizeof(req) - 1, 0); if (r < 0) continue; req[r] = 0;
                printf("LISTEN datagram fd=%d %s\n", p[i].fd, req); continue;
            }
            struct sockaddr_storage peer; socklen_t plen = sizeof(peer); char from[80] = "";
            int c = accept(p[i].fd, (struct sockaddr*)&peer, &plen); if (c < 0) continue;
            ssize_t r = read(c, req, sizeof(req) - 1); if (r < 0) r = 0; req[r] = 0; char* nl = strchr(req, '\n'); if (nl) *nl = 0;
            if (peer.ss_family == AF_INET6) { char a[64]; inet_ntop(AF_INET6, &((struct sockaddr_in6*)&peer)->sin6_addr, a, sizeof(a)); snprintf(from, sizeof(from), " peer=%s", a); }
            else if (peer.ss_family == AF_INET) { char a[64]; inet_ntop(AF_INET, &((struct sockaddr_in*)&peer)->sin_addr, a, sizeof(a)); snprintf(from, sizeof(from), " peer=%s", a); }
            int len = snprintf(out, sizeof(out), "LISTEN fds=%d names=%s uid=%u fd=%d %s%s\n", n, names, (unsigned)getuid(), p[i].fd, req, from);
            if (write(c, out, (size_t)len) < 0) {}
            close(c);
            if (!strcmp(req, "exit")) return 0;
        }
    }
}
static int inetd_mode(void) {
    int n = listen_fds(); char line[256] = "";
    if (!fgets(line, sizeof(line), stdin)) return fail("INETD", "read stdin");
    char* nl = strchr(line, '\n'); if (nl) *nl = 0;
    int acc = -1; socklen_t al = sizeof(acc); getsockopt(3, SOL_SOCKET, SO_ACCEPTCONN, &acc, &al);
    printf("INETD fds=%d uid=%u accepting=%d %s\n", n, (unsigned)getuid(), acc, line); fflush(stdout);
    if (!strncmp(line, "sleep", 5)) sleep((unsigned)atoi(line + 5));
    return 0;
}
static int conn_mode(void) {
    int n = listen_fds(), acc = -1; socklen_t al = sizeof(acc);
    if (n != 1 || getsockopt(3, SOL_SOCKET, SO_ACCEPTCONN, &acc, &al) || acc) { printf("CONN FAIL fds=%d accepting=%d\n", n, acc); return 1; }
    char req[256]; ssize_t r = read(3, req, sizeof(req) - 1); if (r < 0) r = 0; req[r] = 0; char* nl = strchr(req, '\n'); if (nl) *nl = 0;
    char out[320]; int len = snprintf(out, sizeof(out), "CONN fds=%d stdin-null=%d %s\n", n, isatty(0) ? 0 : 1, req);
    if (write(3, out, (size_t)len) < 0) {}
    return 0;
}
static int connect_to(const char* addr, int type) {
    struct sockaddr_storage ss; socklen_t len; memset(&ss, 0, sizeof(ss));
    if (addr[0] == '/' || addr[0] == '@') {
        struct sockaddr_un* a = (struct sockaddr_un*)&ss; a->sun_family = AF_UNIX; size_t n = strlen(addr); if (n >= sizeof(a->sun_path)) return -1;
        memcpy(a->sun_path, addr, n); if (addr[0] == '@') a->sun_path[0] = 0;
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + (addr[0] == '@' ? 0 : 1));
    } else if (addr[0] == '[') {   /* [IPv6]:PORT */
        const char* close_br = strchr(addr, ']'); if (!close_br || close_br[1] != ':') return -1;
        char host[64]; snprintf(host, sizeof(host), "%.*s", (int)(close_br - addr - 1), addr + 1);
        struct sockaddr_in6* a = (struct sockaddr_in6*)&ss; a->sin6_family = AF_INET6; a->sin6_port = htons((uint16_t)atoi(close_br + 2));
        if (inet_pton(AF_INET6, host, &a->sin6_addr) != 1) return -1; len = sizeof(*a);
    } else {
        char host[64]; const char* colon = strrchr(addr, ':'); if (!colon) return -1;
        snprintf(host, sizeof(host), "%.*s", (int)(colon - addr), addr);
        struct sockaddr_in* a = (struct sockaddr_in*)&ss; a->sin_family = AF_INET; a->sin_port = htons((uint16_t)atoi(colon + 1));
        if (inet_pton(AF_INET, host, &a->sin_addr) != 1) return -1; len = sizeof(*a);
    }
    int fd = socket(ss.ss_family, type | SOCK_CLOEXEC, 0); if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr*)&ss, len)) { close(fd); return -1; }
    return fd;
}
static int client_mode(const char* addr, const char* text, int hold_ms, int type) {
    int fd = connect_to(addr, type); if (fd < 0) return fail("CLIENT", "connect");
    struct timeval tv = {10, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (hold_ms > 0) usleep((useconds_t)hold_ms * 1000);
    char req[300]; int n = snprintf(req, sizeof(req), "%s\n", text);
    if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) { printf("CLIENT eof (send %s)\n", strerror(errno)); close(fd); return 3; }
    char buf[1024]; size_t used = 0; ssize_t r;
    while (used + 1 < sizeof(buf) && (r = read(fd, buf + used, sizeof(buf) - 1 - used)) > 0) used += (size_t)r;
    buf[used] = 0; close(fd);
    if (!used) { printf("CLIENT eof\n"); return 3; }
    fputs(buf, stdout); return 0;
}
static int dgram_mode(const char* path, const char* text) {
    if (path[0] != '/') {   /* an IP address: HOST:PORT or [IPv6]:PORT */
        int fd = connect_to(path, SOCK_DGRAM);
        if (fd < 0 || send(fd, text, strlen(text), 0) < 0) return fail("DGRAM", "send");
        close(fd); printf("DGRAM sent\n"); return 0;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (fd < 0 || sendto(fd, text, strlen(text), 0, (struct sockaddr*)&a, sizeof(a)) < 0) return fail("DGRAM", "sendto");
    close(fd); printf("DGRAM sent\n"); return 0;
}
/* POSIX timers (timer_create): util-linux flock -w and others need them */
static int timerprobe_mode(void) {
    timer_t t; struct sigevent ev; memset(&ev, 0, sizeof(ev)); ev.sigev_notify = SIGEV_NONE;
    if (timer_create(CLOCK_MONOTONIC, &ev, &t)) { printf("TIMERPROBE errno=%s\n", errno == ENOSYS ? "ENOSYS" : strerror(errno)); return 0; }
    timer_delete(t); printf("TIMERPROBE ok\n"); return 0;
}
/* a FIFO opened for writing without blocking: ENXIO while nobody reads it */
static int fifoprobe_mode(const char* path) {
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { printf("FIFOPROBE errno=%s\n", errno == ENXIO ? "ENXIO" : strerror(errno)); return 0; }
    struct stat st; fstat(fd, &st); printf("FIFOPROBE opened fifo=%d\n", S_ISFIFO(st.st_mode) ? 1 : 0); close(fd); return 0;
}
int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "fifoprobe")) return fifoprobe_mode(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "timerprobe")) return timerprobe_mode();
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "listen")) return listen_mode();
    if (argc >= 2 && !strcmp(argv[1], "inetd")) return inetd_mode();
    if (argc >= 2 && !strcmp(argv[1], "conn")) return conn_mode();
    if (argc >= 4 && !strcmp(argv[1], "client")) return client_mode(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0, SOCK_STREAM);
    if (argc >= 4 && !strcmp(argv[1], "seqclient")) return client_mode(argv[2], argv[3], 0, SOCK_SEQPACKET);
    if (argc >= 4 && !strcmp(argv[1], "dgram")) return dgram_mode(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "notify")) return notify_mode(atoi(argv[2]));
    if (argc >= 3 && !strcmp(argv[1], "crash")) return crash_mode(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "flaky")) return flaky_mode(argv[2], atoi(argv[3]));
    if (argc >= 3 && !strcmp(argv[1], "env")) return env_mode(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "control")) return control_mode();
    if (argc >= 2 && !strcmp(argv[1], "time")) return time_mode();
    if (argc >= 2 && !strcmp(argv[1], "kmsg")) return kmsg_mode();
    if (argc >= 2 && !strcmp(argv[1], "rlimit")) return rlimit_mode();
    fprintf(stderr, "usage: nexus-svc-test notify MS | crash FILE | flaky FILE N | env FILE | control | time | kmsg | rlimit | listen | inetd | conn |\n"
                    "       client ADDR TEXT [HOLD_MS] | dgram PATH TEXT\n");
    return 2;
}
