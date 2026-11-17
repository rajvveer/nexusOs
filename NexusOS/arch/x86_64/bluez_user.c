/* BlueZ end-to-end driver (tests/test_x64_bluez.py), run as /bin/hw64 (root: /nexus/init-as-root) by the kernel's
 * /nexus/test-hw campaign. It interprets /nexus/bt-script, one command per line, and reports to the serial console:
 *   env K=V | mkdir P | write P TEXT (\n escapes) | sleep MS | note TEXT
 *   spawn NAME LOG CMD...      background process (stdout+stderr -> LOG, "-" = console); session = the same with a
 *   session NAME LOG CMD...    stdin pipe that `send NAME TEXT` writes a line to
 *   kill NAME [SIGNAL]         SIGTERM (or the given number), SIGKILL after 3 s; reaped
 *   run SECONDS CMD...         output captured (printed with a prefix), killed at the timeout
 *   expect TEXT | reject TEXT | status N        checks on the last run
 *   waitlog LOG SECONDS TEXT   until the file contains TEXT (from its start)
 *   cat LOG [LINES]            print a file (the last LINES lines)
 *   fail-if LOG TEXT           a failure if the file contains TEXT
 * Arguments split on blanks; '...' quotes. Subcommands (argv[1]) are the socket programs the script spawns:
 *   l2-server PSM [stream|seq|le] | l2-client ADDR PSM [...] COUNT | rc-server CH | rc-client ADDR CH COUNT ... (see below). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

static int failures;
static char last[65536]; static int last_status = -1;
struct child { char name[32]; pid_t pid; int input; };
static struct child children[32];

static uint64_t ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void fail(int line, const char* what, const char* detail) { printf("[64] BZ FAIL line=%d: %s %s\n", line, what, detail ? detail : ""); fflush(stdout); failures++; }
static int split(char* text, char** argv, int max) {
    int n = 0;
    while (*text && n < max - 1) {
        while (*text == ' ' || *text == '\t') text++;
        if (!*text) break;
        if (*text == '\'') { argv[n++] = ++text; while (*text && *text != '\'') text++; }
        else { argv[n++] = text; while (*text && *text != ' ' && *text != '\t') text++; }
        if (*text) *text++ = 0;
    }
    argv[n] = NULL; return n;
}
static struct child* find(const char* name) { for (int i = 0; i < 32; i++) if (children[i].pid && !strcmp(children[i].name, name)) return &children[i]; return NULL; }
static pid_t start(char** argv, const char* log, int* input) {
    int in[2] = {-1, -1}; if (input && pipe2(in, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (input) dup2(in[0], 0); else { int z = open("/dev/null", O_RDONLY); if (z >= 0) dup2(z, 0); }
        if (strcmp(log, "-")) { int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644); if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); } }
        execvp(argv[0], argv); fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno)); _exit(127);
    }
    if (input) { close(in[0]); *input = in[1]; }
    return pid;
}
static int finish(pid_t pid, int ms) {
    int status = 0; uint64_t end = ms_now() + (uint64_t)ms;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (r < 0) return -1;
        if (ms_now() >= end) return -2;
        usleep(20000);
    }
}
static void stop(struct child* c, int number) {
    kill(-c->pid, number ? number : SIGTERM); kill(c->pid, number ? number : SIGTERM);
    if (finish(c->pid, 3000) == -2) { kill(-c->pid, SIGKILL); kill(c->pid, SIGKILL); finish(c->pid, 3000); }
    if (c->input >= 0) close(c->input);
    c->pid = 0;
}
static bool file_has(const char* path, const char* text) {
    static char buffer[1 << 20]; int fd = open(path, O_RDONLY); if (fd < 0) return false;
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1); close(fd); if (n < 0) return false;
    buffer[n] = 0; for (ssize_t i = 0; i < n; i++) if (!buffer[i]) buffer[i] = ' ';
    return strstr(buffer, text) != NULL;
}
static void print_file(const char* path, int tail) {
    static char buffer[1 << 20]; int fd = open(path, O_RDONLY); if (fd < 0) { printf("[64] BZ CAT %s: %s\n", path, strerror(errno)); return; }
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1); close(fd); if (n < 0) n = 0; buffer[n] = 0;
    char* at = buffer;
    if (tail > 0) { int lines = 0; for (char* p = buffer + n; p > buffer; p--) if (p[-1] == '\n' && ++lines > tail) { at = p; break; } }
    printf("[64] BZ ---- %s ----\n", path);
    for (char* line = strtok(at, "\n"); line; line = strtok(NULL, "\n")) printf("| %s\n", line);
    fflush(stdout);
}
static void run(int line, int seconds, char** argv) {
    int out[2], in[2]; if (pipe2(out, O_CLOEXEC) || pipe2(in, O_CLOEXEC)) { fail(line, "pipe", NULL); return; }
    pid_t pid = fork();   /* stdin: an open pipe (BlueZ's shell adds stdin to epoll, which /dev/null does not support) */
    if (pid == 0) { setpgid(0, 0); dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2); execvp(argv[0], argv); fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno)); _exit(127); }
    close(out[1]); close(in[0]);
    size_t used = 0; uint64_t end = ms_now() + (uint64_t)seconds * 1000; bool killed = false;
    for (;;) {
        int left = (int)(end > ms_now() ? end - ms_now() : 0);
        struct pollfd p = {out[0], POLLIN, 0}; int r = poll(&p, 1, left < 200 ? left : 200);
        if (r > 0) { ssize_t n = read(out[0], last + used, sizeof(last) - 1 - used); if (n <= 0) break; used += (size_t)n; if (used >= sizeof(last) - 1) break; }
        if (ms_now() >= end) { killed = true; kill(-pid, SIGTERM); kill(pid, SIGTERM); break; }
    }
    close(out[0]); close(in[1]); last[used] = 0;
    last_status = finish(pid, killed ? 3000 : 10000); if (last_status == -2) { kill(-pid, SIGKILL); kill(pid, SIGKILL); last_status = finish(pid, 3000); killed = true; }
    printf("[64] BZ RUN %s ->%s status %d\n", argv[0], killed ? " (timeout)" : "", last_status);
    for (size_t i = 0; i < used; i++) if (!last[i]) last[i] = ' ';
    static char shown[sizeof(last)]; memcpy(shown, last, used + 1);
    for (char* l = strtok(shown, "\n"); l; l = strtok(NULL, "\n")) printf("> %s\n", l);
    fflush(stdout);
}
static void unescape(char* s) { char* o = s; for (; *s; s++) { if (*s == '\\' && s[1] == 'n') { *o++ = '\n'; s++; } else if (*s == '\\' && s[1] == 't') { *o++ = '\t'; s++; } else *o++ = *s; } *o = 0; }
static int script(const char* path) {
    static char text[1 << 16]; int fd = open(path, O_RDONLY); if (fd < 0) { printf("[64] BZ no script %s\n", path); return 1; }
    ssize_t n = read(fd, text, sizeof(text) - 1); close(fd); if (n < 0) n = 0; text[n] = 0;
    for (int i = 0; i < 32; i++) children[i].input = -1;
    int number = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0; number++;
        if (!*line || *line == '#') continue;
        char copy[4096]; snprintf(copy, sizeof(copy), "%s", line);
        char* argv[64]; int argc = split(line, argv, 64); if (!argc) continue;
        const char* op = argv[0];
        if (!strcmp(op, "note")) printf("[64] BZ NOTE %s\n", copy + 5);
        else if (!strcmp(op, "env") && argc == 2) { char* eq = strchr(argv[1], '='); if (eq) { *eq = 0; setenv(argv[1], eq + 1, 1); } }
        else if (!strcmp(op, "mkdir") && argc == 2) {
            char p[512]; snprintf(p, sizeof(p), "%s", argv[1]); for (char* s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
            if (mkdir(p, 0755) && errno != EEXIST) fail(number, "mkdir", strerror(errno));
        }
        else if (!strcmp(op, "write") && argc >= 2) {
            char* body = strstr(copy, argv[1]) + strlen(argv[1]); if (*body) body++; unescape(body);
            int f = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f < 0 || write(f, body, strlen(body)) < 0) fail(number, "write", strerror(errno)); if (f >= 0) close(f);
        }
        else if (!strcmp(op, "sleep") && argc == 2) usleep((useconds_t)atoi(argv[1]) * 1000);
        else if ((!strcmp(op, "spawn") || !strcmp(op, "session")) && argc >= 4) {
            int slot = 0; while (slot < 32 && children[slot].pid) slot++;
            if (slot == 32) { fail(number, "too many children", NULL); continue; }
            struct child* c = &children[slot]; snprintf(c->name, sizeof(c->name), "%s", argv[1]); c->input = -1;
            c->pid = start(argv + 3, argv[2], op[1] == 'e' ? &c->input : NULL);
            printf("[64] BZ SPAWN %s pid %d: %s\n", c->name, (int)c->pid, argv[3]);
        }
        else if (!strcmp(op, "send") && argc >= 2) {
            struct child* c = find(argv[1]); char* body = strstr(copy, argv[1]) + strlen(argv[1]); if (*body) body++;
            char l[1024]; snprintf(l, sizeof(l), "%s\n", body); unescape(l);
            if (!c || c->input < 0 || write(c->input, l, strlen(l)) < 0) fail(number, "send", argv[1]); else printf("[64] BZ SEND %s: %s", argv[1], l);
        }
        else if (!strcmp(op, "kill") && argc >= 2) { struct child* c = find(argv[1]); if (c) stop(c, argc > 2 ? atoi(argv[2]) : 0); else fail(number, "kill: no child", argv[1]); }
        else if (!strcmp(op, "wait") && argc == 3) {                   /* wait NAME SECONDS: until the child exits */
            struct child* c = find(argv[1]); if (!c) { fail(number, "wait: no child", argv[1]); continue; }
            int r = finish(c->pid, atoi(argv[2]) * 1000);
            if (r == -2) { fail(number, "wait: still running", argv[1]); stop(c, 0); } else { printf("[64] BZ EXIT %s status %d\n", argv[1], r); last_status = r; c->pid = 0; if (c->input >= 0) close(c->input); c->input = -1; }
        }
        else if (!strcmp(op, "alive") && argc == 2) { struct child* c = find(argv[1]); if (!c || waitpid(c->pid, NULL, WNOHANG) != 0) fail(number, "not running", argv[1]); }
        else if (!strcmp(op, "run") && argc >= 3) run(number, atoi(argv[1]), argv + 2);
        else if (!strcmp(op, "expect") || !strcmp(op, "reject")) {
            const char* want = copy + 7; bool has = strstr(last, want) != NULL;
            if (has != (op[0] == 'e')) fail(number, op[0] == 'e' ? "expected" : "unexpected", want); else printf("[64] BZ OK %s '%s'\n", op, want);
        }
        else if (!strcmp(op, "status") && argc == 2) { if (last_status != atoi(argv[1])) { char d[64]; snprintf(d, sizeof(d), "%d != %s", last_status, argv[1]); fail(number, "status", d); } }
        else if (!strcmp(op, "waitlog") && argc >= 4) {
            const char* want = strstr(copy, argv[2]) + strlen(argv[2]) + 1; uint64_t end = ms_now() + (uint64_t)atoi(argv[2]) * 1000; bool ok = false;
            while (!(ok = file_has(argv[1], want)) && ms_now() < end) usleep(100000);
            if (ok) printf("[64] BZ OK waitlog %s '%s'\n", argv[1], want); else fail(number, "waitlog timeout", want);
        }
        else if (!strcmp(op, "fail-if") && argc >= 3) { const char* bad = strstr(copy, argv[1]) + strlen(argv[1]) + 1; if (file_has(argv[1], bad)) fail(number, "log has", bad); }
        else if (!strcmp(op, "cat") && argc >= 2) print_file(argv[1], argc > 2 ? atoi(argv[2]) : 0);
        else fail(number, "bad command", copy);
        fflush(stdout);
    }
    for (int i = 31; i >= 0; i--) if (children[i].pid) stop(&children[i], 0);
    return failures;
}

/* ---------- socket programs ---------- */
struct sockaddr_l2 { uint16_t family, psm; uint8_t bdaddr[6]; uint16_t cid; uint8_t type, pad; };
struct sockaddr_rc { uint16_t family; uint8_t bdaddr[6]; uint8_t channel, pad; };
static bool parse_addr(const char* s, uint8_t* out) {
    unsigned v[6]; if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[5 - i] = (uint8_t)v[i];
    return true;
}
static uint8_t pattern(unsigned i, unsigned k) { return (uint8_t)(i * 31 + k * 7 + 1); }
static int set_security(int fd, int level) { if (level <= 0) return 0; uint8_t s[2] = {(uint8_t)level, 0}; return setsockopt(fd, 274, 4, s, 2); }
static int echo_loop(int fd, bool stream, const char* what) {
    static uint8_t buffer[70000]; long total = 0, messages = 0;
    for (;;) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; printf("[64] BZ %s recv: %s\n", what, strerror(errno)); return 1; }
        for (ssize_t at = 0; at < n;) { ssize_t w = send(fd, buffer + at, (size_t)(n - at), 0); if (w <= 0) { printf("[64] BZ %s send: %s\n", what, strerror(errno)); return 1; } at += w; if (!stream && w != n) { printf("[64] BZ %s short datagram\n", what); return 1; } }
        total += n; messages++;
    }
    printf("[64] BZ %s echoed bytes=%ld messages=%ld\n", what, total, messages); return 0;
}
static int l2_server(int argc, char** argv) {   /* l2-server PSM bredr|le [security] */
    if (argc < 4) return 2;
    bool le = !strcmp(argv[3], "le"); int fd = socket(31, SOCK_SEQPACKET, 0);
    struct sockaddr_l2 a = {31, (uint16_t)strtoul(argv[2], NULL, 0), {0}, 0, (uint8_t)(le ? 1 : 0), 0};
    if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof(a)) || set_security(fd, argc > 4 ? atoi(argv[4]) : 0) || listen(fd, 2)) { printf("[64] BZ L2 SERVER setup: %s\n", strerror(errno)); return 1; }
    printf("[64] BZ L2 SERVER READY psm=0x%x %s\n", a.psm, argv[3]); fflush(stdout);
    struct sockaddr_l2 peer; socklen_t pl = sizeof(peer); int c = accept(fd, (struct sockaddr*)&peer, &pl);
    if (c < 0) { printf("[64] BZ L2 SERVER accept: %s\n", strerror(errno)); return 1; }
    printf("[64] BZ L2 SERVER accepted %02X:%02X:%02X:%02X:%02X:%02X\n", peer.bdaddr[5], peer.bdaddr[4], peer.bdaddr[3], peer.bdaddr[2], peer.bdaddr[1], peer.bdaddr[0]);
    int r = echo_loop(c, false, "L2 SERVER"); close(c); close(fd); return r;
}
static int l2_client(int argc, char** argv) {   /* l2-client ADDR PSM bredr|le COUNT [security] */
    if (argc < 6) return 2;
    bool le = !strcmp(argv[4], "le"); int fd = socket(31, SOCK_SEQPACKET, 0); unsigned count = (unsigned)atoi(argv[5]);
    struct sockaddr_l2 a = {31, (uint16_t)strtoul(argv[3], NULL, 0), {0}, 0, (uint8_t)(le ? 1 : 0), 0};
    if (fd < 0 || !parse_addr(argv[2], a.bdaddr) || set_security(fd, argc > 6 ? atoi(argv[6]) : 0)) { printf("[64] BZ L2 CLIENT setup: %s\n", strerror(errno)); return 1; }
    if (connect(fd, (struct sockaddr*)&a, sizeof(a))) { printf("[64] BZ L2 CLIENT connect: %s\n", strerror(errno)); return 1; }
    uint8_t opts[12] = {0}; socklen_t ol = sizeof(opts); getsockopt(fd, 6, 1, opts, &ol); unsigned omtu = opts[0] | opts[1] << 8;
    uint8_t sec[2] = {0}; socklen_t sl = 2; getsockopt(fd, 274, 4, sec, &sl);
    static uint8_t out[70000], in[70000]; long bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned n = 1 + (i * 97) % (omtu ? omtu : 672); if (i == count - 1) n = omtu ? omtu : 672;
        for (unsigned k = 0; k < n; k++) out[k] = pattern(i, k);
        if (send(fd, out, n, 0) != (ssize_t)n) { printf("[64] BZ L2 CLIENT send %u: %s\n", i, strerror(errno)); return 1; }
        struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, 10000) != 1) { printf("[64] BZ L2 CLIENT echo %u: timeout\n", i); return 1; }
        ssize_t m = recv(fd, in, sizeof(in), 0);
        if (m != (ssize_t)n || memcmp(in, out, n)) { printf("[64] BZ L2 CLIENT echo %u: got %zd of %u bytes or different data\n", i, m, n); return 1; }
        bytes += n;
    }
    printf("[64] BZ L2 ECHO OK messages=%u bytes=%ld omtu=%u security=%u %s\n", count, bytes, omtu, sec[0], argv[4]);
    close(fd); return 0;
}
static int rc_server(int argc, char** argv) {   /* rc-server CHANNEL [security] */
    if (argc < 3) return 2;
    int fd = socket(31, SOCK_STREAM, 3); struct sockaddr_rc a = {31, {0}, (uint8_t)atoi(argv[2]), 0};
    if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof(a)) || set_security(fd, argc > 3 ? atoi(argv[3]) : 0) || listen(fd, 2)) { printf("[64] BZ RC SERVER setup: %s\n", strerror(errno)); return 1; }
    printf("[64] BZ RC SERVER READY channel=%u\n", a.channel); fflush(stdout);
    int c = accept(fd, NULL, NULL); if (c < 0) { printf("[64] BZ RC SERVER accept: %s\n", strerror(errno)); return 1; }
    int r = echo_loop(c, true, "RC SERVER"); close(c); close(fd); return r;
}
static int rc_client(int argc, char** argv) {   /* rc-client ADDR CHANNEL BYTES [security] */
    if (argc < 5) return 2;
    int fd = socket(31, SOCK_STREAM, 3); struct sockaddr_rc a = {31, {0}, (uint8_t)atoi(argv[3]), 0}; long total = atol(argv[4]);
    if (fd < 0 || !parse_addr(argv[2], a.bdaddr) || set_security(fd, argc > 5 ? atoi(argv[5]) : 0)) { printf("[64] BZ RC CLIENT setup: %s\n", strerror(errno)); return 1; }
    if (connect(fd, (struct sockaddr*)&a, sizeof(a))) { printf("[64] BZ RC CLIENT connect: %s\n", strerror(errno)); return 1; }
    static uint8_t out[8192], in[8192]; long sent = 0, got = 0; unsigned round = 0;
    while (got < total) {
        long chunk = total - sent < (long)sizeof(out) ? total - sent : (long)sizeof(out);
        if (chunk > 0) { for (long k = 0; k < chunk; k++) out[k] = pattern((unsigned)((sent + k) / 251), (unsigned)((sent + k) % 251)); if (send(fd, out, (size_t)chunk, 0) != chunk) { printf("[64] BZ RC CLIENT send: %s\n", strerror(errno)); return 1; } sent += chunk; }
        while (got < sent) {
            struct pollfd p = {fd, POLLIN, 0}; if (poll(&p, 1, 10000) != 1) { printf("[64] BZ RC CLIENT echo timeout at %ld\n", got); return 1; }
            ssize_t m = recv(fd, in, sizeof(in), 0); if (m <= 0) { printf("[64] BZ RC CLIENT recv: %s\n", m ? strerror(errno) : "EOF"); return 1; }
            for (ssize_t k = 0; k < m; k++) if (in[k] != pattern((unsigned)((got + k) / 251), (unsigned)((got + k) % 251))) { printf("[64] BZ RC CLIENT data mismatch at %ld\n", got + k); return 1; }
            got += m;
        }
        round++;
    }
    printf("[64] BZ RC ECHO OK bytes=%ld rounds=%u\n", got, round); close(fd); return 0;
}

/* ---------- hostile peer: a raw controller on /dev/vhci injecting malformed traffic ---------- */
static int hv = -1, hctl = -1; static unsigned hindex; static int hfail;
#define HOST_ACL_MAX 8192                                                   /* the head of every ACL packet the kernel sent */
static uint8_t host_acl[HOST_ACL_MAX][64]; static unsigned host_acl_len[HOST_ACL_MAX], host_acl_count;
static uint16_t host_cmds[512]; static unsigned host_cmd_count;
#define HCHECK(t, ...) do { if (!(t)) { printf("[64] BZ HOSTILE FAIL line=%d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); hfail++; } } while (0)
static void hwrite(const uint8_t* p, unsigned n) { if (write(hv, p, n) != (ssize_t)n) { printf("[64] BZ HOSTILE vhci write(%u): %s\n", n, strerror(errno)); hfail++; } }
static void hevent(const uint8_t* data, unsigned n) { uint8_t p[260]; p[0] = 4; memcpy(p + 1, data, n); hwrite(p, n + 1); }
static void hcomplete(unsigned op, const uint8_t* rp, unsigned n) { uint8_t e[260] = {0x0E, (uint8_t)(3 + n), 1, (uint8_t)op, (uint8_t)(op >> 8)}; memcpy(e + 5, rp, n); hevent(e, 5 + n); }
static void hstatus(unsigned op, uint8_t status) { uint8_t e[6] = {0x0F, 4, status, 1, (uint8_t)op, (uint8_t)(op >> 8)}; hevent(e, 6); }
static const uint8_t HADDR[6] = {0x10, 0x00, 0x00, 0x0B, 0xAD, 0x00}, EVIL[6] = {0x66, 0x66, 0x66, 0x0E, 0x71, 0x00};
static void hl2(unsigned handle, unsigned cid, const uint8_t* data, unsigned n);
static void hservice(int ms) {                                            /* answer the kernel's commands, record its ACL */
    uint64_t end = ms_now() + (uint64_t)ms;
    for (;;) {
        int left = (int)(end > ms_now() ? end - ms_now() : 0);
        struct pollfd p = {hv, POLLIN, 0}; if (poll(&p, 1, left) <= 0) return;
        uint8_t b[1100]; ssize_t n = read(hv, b, sizeof(b)); if (n <= 0) return;
        if (b[0] == 2) {
            if (host_acl_count < HOST_ACL_MAX) { memcpy(host_acl[host_acl_count], b, n < 64 ? (size_t)n : 64); host_acl_len[host_acl_count] = (unsigned)n; } host_acl_count++;
            if (n < 5) continue;
            unsigned handle = (b[1] | b[2] << 8) & 0x0FFF;
            uint8_t done[7] = {0x13, 5, 1, (uint8_t)handle, (uint8_t)(handle >> 8), 1, 0}; hevent(done, 7);   /* a controller returns ACL credits */
            if (n >= 15 && (b[7] | b[8] << 8) == 1 && b[9] == 0x0A) {           /* the host's Information Request: no extra features */
                unsigned type = b[13] | b[14] << 8;
                uint8_t ok[12] = {0x0B, b[10], 8, 0, 2, 0, 0, 0, 0, 0, 0, 0}, no[8] = {0x0B, b[10], 4, 0, (uint8_t)type, (uint8_t)(type >> 8), 1, 0};
                if (type == 2) hl2(handle, 1, ok, 12); else hl2(handle, 1, no, 8);
            }
            continue;
        }
        if (b[0] != 1 || n < 4) continue;
        unsigned op = b[1] | b[2] << 8; uint8_t r[260] = {0};
        if (host_cmd_count < 512) host_cmds[host_cmd_count++] = (uint16_t)op;
        switch (op) {
        case 0x1001: { uint8_t v[9] = {0, 0x0B, 0, 0, 0x0B, 0xF1, 0x05, 0, 0}; hcomplete(op, v, 9); break; }
        case 0x1003: { uint8_t f[9] = {0, 0xFF, 0xFF, 0x8F, 0xFE, 0xDB, 0xFF, 0x7B, 0x87}; hcomplete(op, f, 9); break; }   /* BR/EDR + LE, SSP, EIR */
        case 0x1009: r[0] = 0; memcpy(r + 1, HADDR, 6); hcomplete(op, r, 7); break;
        case 0x1005: { uint8_t s[8] = {0, 0xFD, 0x03, 0x40, 8, 0, 8, 0}; hcomplete(op, s, 8); break; }
        case 0x2002: { uint8_t s[4] = {0, 0xFB, 0x00, 8}; hcomplete(op, s, 4); break; }
        case 0x1002: hcomplete(op, r, 65); break;
        case 0x1004: r[1] = b[4]; r[2] = 2; hcomplete(op, r, 11); break;
        case 0x0C14: hcomplete(op, r, 249); break;
        case 0x2003: case 0x201C: hcomplete(op, r, 9); break;
        case 0x1408: r[3] = 16; hcomplete(op, r, 4); break;
        case 0x0401: case 0x0405: case 0x0411: case 0x0413: case 0x041B: case 0x0419: case 0x200D: case 0x2019: case 0x2016: case 0x0409: case 0x2013: hstatus(op, 0); break;
        case 0x0406: { hstatus(op, 0); uint8_t e[6] = {0x05, 4, 0, b[4], b[5], 0x16}; hevent(e, 6); break; }
        default: if (op == 0x042B || op == 0x042C || op == 0x040B || op == 0x040C || op == 0x201A || op == 0x201B) { r[0] = 0; memcpy(r + 1, b + 4, 6); hcomplete(op, r, op >= 0x2000 ? 3 : 7); } else hcomplete(op, r, 1);
        }
    }
}
static void hacl(unsigned handle, unsigned pb, const uint8_t* data, unsigned n) { uint8_t p[1100]; p[0] = 2; p[1] = (uint8_t)handle; p[2] = (uint8_t)(handle >> 8 | pb << 4); p[3] = (uint8_t)n; p[4] = (uint8_t)(n >> 8); memcpy(p + 5, data, n); hwrite(p, n + 5); }
static void hl2(unsigned handle, unsigned cid, const uint8_t* data, unsigned n) { uint8_t f[1100]; f[0] = (uint8_t)n; f[1] = (uint8_t)(n >> 8); f[2] = (uint8_t)cid; f[3] = (uint8_t)(cid >> 8); memcpy(f + 4, data, n); hacl(handle, 2, f, n + 4); }
static int host_sent(unsigned cid, uint8_t code, unsigned since) {         /* index of a host ACL on cid with first payload byte code */
    for (unsigned i = since; i < host_acl_count && i < HOST_ACL_MAX; i++) { const uint8_t* a = host_acl[i]; if (host_acl_len[i] >= 10 && (unsigned)(a[7] | a[8] << 8) == cid && a[9] == code) return (int)i; }
    return -1;
}
static bool hmgmt(unsigned op, const uint8_t* params, unsigned n) {      /* mgmt command, servicing the controller meanwhile */
    uint8_t m[64] = {(uint8_t)op, (uint8_t)(op >> 8), (uint8_t)hindex, (uint8_t)(hindex >> 8), (uint8_t)n, 0}; memcpy(m + 6, params, n);
    if (write(hctl, m, 6 + n) != (ssize_t)(6 + n)) return false;
    for (int i = 0; i < 100; i++) {
        hservice(20); uint8_t r[400]; struct pollfd p = {hctl, POLLIN, 0};
        while (poll(&p, 1, 0) == 1) { ssize_t k = read(hctl, r, sizeof(r)); if (k >= 9 && (r[0] == 1 || r[0] == 2) && !r[1] && (unsigned)(r[6] | r[7] << 8) == op) return r[8] == 0; }
    }
    return false;
}
static int hostile(void) {
    printf("[64] BZ HOSTILE START\n");
    hctl = socket(31, SOCK_RAW | SOCK_CLOEXEC, 1); struct sockaddr { uint16_t f, d, c; } a = {31, 0xFFFF, 3};
    if (hctl < 0 || bind(hctl, (void*)&a, 6)) { printf("[64] BZ HOSTILE control socket: %s\n", strerror(errno)); return 1; }
    hv = open("/dev/vhci", O_RDWR | O_NONBLOCK); uint8_t create[2] = {0xFF, 0}, reply[4];
    if (hv < 0 || write(hv, create, 2) != 2) { printf("[64] BZ HOSTILE vhci: %s\n", strerror(errno)); return 1; }
    struct pollfd p0 = {hv, POLLIN, 0}; poll(&p0, 1, 2000); if (read(hv, reply, 4) != 4) return 1;
    hindex = reply[2] | reply[3] << 8; hservice(300);
    uint8_t on = 1;                                                         /* what bluetoothd would enable (the kernel's defaults are Linux's: BR/EDR only) */
    HCHECK(hmgmt(0x0009, &on, 1), "set bondable"); HCHECK(hmgmt(0x000D, &on, 1), "set LE"); HCHECK(hmgmt(0x002D, &on, 1), "set secure connections");
    HCHECK(hmgmt(0x0007, &on, 1), "set connectable"); HCHECK(hmgmt(0x0005, &on, 1), "power on (mgmt worker answers while this process is the controller)");
    /* events: malformed and absurd ones */
    static const uint8_t ev[][12] = {{0x0E, 3, 1, 0x34, 0x12}, {0x0E, 1, 1}, {0x0F, 4, 0, 1, 0x99, 0x99}, {0x13, 5, 1, 0x77, 0x07, 0xFF, 0xFF}, {0x13, 1, 9}, {0x05, 4, 0, 0x99, 0x0F, 0x13},
                                     {0x3E, 2, 0x01, 0}, {0x3E, 3, 0x05, 0x40, 0}, {0x02, 1, 0xFF}, {0x2F, 2, 1, 2}, {0x3E, 4, 0x02, 9, 0, 0}, {0x08, 4, 0, 0x55, 0x05, 1}, {0x18, 3, 1, 2, 3}};
    for (unsigned i = 0; i < sizeof(ev) / sizeof(ev[0]); i++) hevent(ev[i], 2U + ev[i][1]);
    uint8_t junk[4] = {9, 1, 2, 3}; errno = 0; HCHECK(write(hv, junk, 4) < 0 && errno == EINVAL, "unknown packet type refused");
    uint8_t shortev[4] = {4, 0x0E, 9, 1}; errno = 0; HCHECK(write(hv, shortev, 4) < 0 && errno == EINVAL, "event length mismatch refused");
    hservice(200);
    /* a remote connects over BR/EDR (handle 0x0040), sends hostile ACL and L2CAP */
    uint8_t cc[13] = {0x03, 11, 0, 0x40, 0x00}; memcpy(cc + 5, EVIL, 6); cc[11] = 1; cc[12] = 0; hevent(cc, 13); hservice(300);
    unsigned mark = host_acl_count;
    uint8_t echo[8] = {0x08, 0x21, 4, 0, 'p', 'i', 'n', 'g'}; hl2(0x40, 1, echo, 8); hservice(200);
    int e = host_sent(1, 0x09, mark); HCHECK(e >= 0 && host_acl[e][10] == 0x21 && !memcmp(host_acl[e] + 13, "ping", 4), "echo answered");
    mark = host_acl_count; uint8_t badcmd[4] = {0x77, 0x22, 0, 0}; hl2(0x40, 1, badcmd, 4); hservice(150);
    e = host_sent(1, 0x01, mark); HCHECK(e >= 0 && host_acl[e][10] == 0x22, "unknown signalling code: Command Reject");
    mark = host_acl_count; uint8_t trunc[6] = {0x02, 0x23, 9, 0, 1, 0}; hl2(0x40, 1, trunc, 6); hservice(150);
    HCHECK(host_sent(1, 0x01, mark) >= 0, "truncated command: Command Reject");
    mark = host_acl_count; uint8_t conn[8] = {0x02, 0x24, 4, 0, 0x01, 0x77, 0x40, 0x00}; hl2(0x40, 1, conn, 8); hservice(150);
    e = host_sent(1, 0x03, mark); HCHECK(e >= 0 && (host_acl[e][17] | host_acl[e][18] << 8) == 0x0002, "unknown PSM: PSM not supported");
    mark = host_acl_count; uint8_t cfg[12] = {0x04, 0x25, 8, 0, 0x55, 0x00, 0, 0, 0x01, 0x02, 0x00, 0x02}; hl2(0x40, 1, cfg, 12); hservice(150);
    HCHECK(host_sent(1, 0x01, mark) >= 0, "config for an unknown CID: Command Reject");
    mark = host_acl_count; uint8_t disc[8] = {0x06, 0x26, 4, 0, 0x99, 0x00, 0x40, 0x00}; hl2(0x40, 1, disc, 8); hservice(150);
    HCHECK(host_sent(1, 0x01, mark) >= 0, "disconnect of an unknown CID: Command Reject");
    mark = host_acl_count; uint8_t info[6] = {0x0A, 0x27, 2, 0, 0x02, 0x00}; hl2(0x40, 1, info, 6); hservice(150);
    e = host_sent(1, 0x0B, mark); HCHECK(e >= 0 && host_acl[e][17] == 0x80, "information request: feature mask");
    uint8_t zero_ident[8] = {0x08, 0x00, 4, 0, 1, 2, 3, 4}; hl2(0x40, 1, zero_ident, 8);
    uint8_t oversized[8] = {0xFF, 0xFF, 0x01, 0x00, 1, 2, 3, 4}; hacl(0x40, 2, oversized, 8);       /* claims 65535 bytes */
    uint8_t cont[4] = {1, 2, 3, 4}; hacl(0x40, 1, cont, 4);                                           /* continuation without start */
    uint8_t lie[8] = {0x10, 0x00, 0x01, 0x00, 1, 2, 3, 4}; hacl(0x40, 2, lie, 8); hacl(0x40, 1, cont, 4); hacl(0x40, 1, oversized, 8);   /* overflows its frame */
    hacl(0x0999, 2, echo, 8);                                                                         /* unknown handle */
    uint8_t smpbr[3] = {0x01, 3, 0}; hl2(0x40, 7, smpbr, 3);                                         /* BR/EDR SMP: not supported */
    /* RFCOMM from the hostile side: an L2CAP channel on PSM 3, then broken frames */
    mark = host_acl_count; uint8_t rconn[8] = {0x02, 0x30, 4, 0, 0x03, 0x00, 0x41, 0x00}; hl2(0x40, 1, rconn, 8); hservice(200);
    e = host_sent(1, 0x03, mark); unsigned rcid = e >= 0 ? (host_acl[e][13] | host_acl[e][14] << 8) : 0;
    HCHECK(e >= 0 && rcid >= 0x40, "RFCOMM L2CAP channel accepted");
    if (rcid) {
        uint8_t rcfg[8] = {0x04, 0x31, 4, 0, (uint8_t)rcid, (uint8_t)(rcid >> 8), 0, 0}; hl2(0x40, 1, rcfg, 8); hservice(150);
        int cr = host_sent(1, 0x04, mark);                                 /* the kernel's own configure request: accept it */
        if (cr >= 0) { uint8_t ok[10] = {0x05, host_acl[cr][10], 6, 0, (uint8_t)rcid, (uint8_t)(rcid >> 8), 0, 0, 0, 0}; hl2(0x40, 1, ok, 10); }
        hservice(150);
        uint8_t badfcs[4] = {0x03, 0x3F, 0x01, 0x00}; hl2(0x40, rcid, badfcs, 4);   /* SABM on DLCI 0 with a wrong FCS: ignored */
        uint8_t garbage[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; hl2(0x40, rcid, garbage, 6);
        uint8_t uih_unknown[5] = {0x0B, 0xEF, 0x03, 0x41, 0x00}; hl2(0x40, rcid, uih_unknown, 5);
        uint8_t longlen[5] = {0x03, 0xEF, 0xFE, 0xFF, 0x00}; hl2(0x40, rcid, longlen, 5);   /* 2-byte length beyond the frame */
        hservice(200);
    }
    /* random fuzz on the signalling channel, the RFCOMM channel and dynamic CIDs, then liveness */
    srand(42);
    for (unsigned i = 0; i < 1500; i++) {
        uint8_t r[200]; unsigned n = (unsigned)rand() % 200; for (unsigned k = 0; k < n; k++) r[k] = (uint8_t)rand();
        unsigned cid = (unsigned[]){1, 1, rcid ? rcid : 0x40, rcid ? rcid : 0x40, 0x55, 2, 3}[rand() % 7];
        if (rand() % 5) hl2(0x40, cid, r, n); else hacl(0x40, (unsigned)rand() % 4, r, n);
        if (i % 50 == 0) hservice(5);
    }
    hservice(300);
    mark = host_acl_count; echo[1] = 0x5A; hl2(0x40, 1, echo, 8); hservice(300);
    e = host_sent(1, 0x09, mark); HCHECK(e >= 0, "BR/EDR link still answers after 1500 fuzzed frames");
    uint8_t dc[6] = {0x05, 4, 0, 0x40, 0x00, 0x13}; hevent(dc, 6); hservice(200);
    /* LE: a central connects to us (handle 0x0041), hostile LE signalling and SMP */
    uint8_t le[21] = {0x3E, 19, 0x01, 0, 0x41, 0x00, 1, 0}; memcpy(le + 8, EVIL, 6); le[14] = 0x18; le[16] = 0; le[18] = 0x48; hevent(le, 21); hservice(200);
    mark = host_acl_count; uint8_t lebad[14] = {0x14, 0x40, 10, 0, 0x80, 0, 0x40, 0, 5, 0, 5, 0, 1, 0}; hl2(0x41, 5, lebad, 14); hservice(150);
    e = host_sent(5, 0x15, mark); HCHECK(e >= 0 && (host_acl[e][21] | host_acl[e][22] << 8) == 0x000B, "LE credit connection with MTU 5: unacceptable parameters");
    mark = host_acl_count; uint8_t upd[12] = {0x12, 0x41, 8, 0, 6, 0, 6, 0, 0, 0, 10, 0}; hl2(0x41, 5, upd, 12); hservice(150);
    HCHECK(host_sent(5, 0x01, mark) >= 0, "parameter update request to a central only: we are peripheral -> Command Reject");
    mark = host_acl_count; uint8_t credit[8] = {0x16, 0x42, 4, 0, 0x99, 0x00, 0xFF, 0xFF}; hl2(0x41, 5, credit, 8);
    uint8_t smp_bad[1] = {0x55}; hl2(0x41, 6, smp_bad, 1); hservice(150);
    e = host_sent(6, 0x05, mark); HCHECK(e >= 0 && host_acl[e][10] == 0x07, "unknown SMP code: Pairing Failed (Command Not Supported)");
    mark = host_acl_count; uint8_t smp_short[3] = {0x01, 3, 0}; hl2(0x41, 6, smp_short, 3); hservice(150);
    e = host_sent(6, 0x05, mark); HCHECK(e >= 0 && host_acl[e][10] == 0x0A, "short Pairing Request: Invalid Parameters");
    mark = host_acl_count; uint8_t preq[7] = {0x01, 0x03, 0, 0x09, 16, 0x00, 0x01}; hl2(0x41, 6, preq, 7); hservice(200);
    e = host_sent(6, 0x02, mark); HCHECK(e >= 0, "Pairing Request answered with a Pairing Response");
    mark = host_acl_count; uint8_t pk[65] = {0x0C}; for (int k = 1; k < 65; k++) pk[k] = (uint8_t)(k * 3); hl2(0x41, 6, pk, 65); hservice(300);   /* not on the curve */
    e = host_sent(6, 0x05, mark); HCHECK(e >= 0 && host_acl[e][10] == 0x0B, "invalid P-256 public key refused (DHKey Check Failed)");
    for (unsigned i = 0; i < 800; i++) { uint8_t r[80]; unsigned n = (unsigned)rand() % 80; for (unsigned k = 0; k < n; k++) r[k] = (uint8_t)rand(); hl2(0x41, (unsigned[]){4, 5, 6, 0x40}[rand() % 4], r, n); if (i % 40 == 0) hservice(5); }
    hservice(300);
    mark = host_acl_count; uint8_t le_disc[8] = {0x06, 0x43, 4, 0, 0x50, 0x00, 0x40, 0x00}; hl2(0x41, 5, le_disc, 8); hservice(200);
    HCHECK(host_sent(5, 0x01, mark) >= 0, "LE link still answers after 800 fuzzed frames");
    uint8_t ldc[6] = {0x05, 4, 0, 0x41, 0x00, 0x13}; hevent(ldc, 6); hservice(200);
    uint8_t off = 0; HCHECK(hmgmt(0x0005, &off, 1), "power off");
    close(hv); close(hctl);
    printf("[64] BZ HOSTILE %s failures=%d host_acl=%u commands=%u\n", hfail ? "FAILED" : "PASSED", hfail, host_acl_count, host_cmd_count);
    return hfail ? 1 : 0;
}

/* ---------- BlueZ's tester programs, one test per process (a hung or looping test cannot take the run down) ---------- */
static char out_buffer[1 << 16];
static int capture(char* const* argv, int seconds, char* out, size_t size) {   /* exit status; output bounded */
    int pipefd[2], in[2]; if (pipe2(pipefd, O_CLOEXEC) || pipe2(in, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) { setpgid(0, 0); dup2(in[0], 0); dup2(pipefd[1], 1); dup2(pipefd[1], 2); execvp(argv[0], argv); _exit(127); }
    close(pipefd[1]); close(in[0]); size_t used = 0; uint64_t end = ms_now() + (uint64_t)seconds * 1000; bool killed = false;
    for (;;) {
        int left = (int)(end > ms_now() ? end - ms_now() : 0); struct pollfd p = {pipefd[0], POLLIN, 0};
        if (poll(&p, 1, left < 200 ? left : 200) > 0) { char sink[4096]; ssize_t n = read(pipefd[0], used < size - 1 ? out + used : sink, used < size - 1 ? size - 1 - used : sizeof(sink)); if (n <= 0) break; if (used < size - 1) used += (size_t)n; }
        if (ms_now() >= end) { killed = true; kill(-pid, SIGKILL); kill(pid, SIGKILL); break; }
    }
    close(pipefd[0]); close(in[1]); out[used] = 0;
    int status = finish(pid, 5000); return killed ? -2 : status;
}
static void strip_colors(char* s) { char* o = s; for (char* p = s; *p; p++) { if (*p == 0x1B && p[1] == '[') { p += 2; while (*p && *p != 'm') p++; if (!*p) break; continue; } *o++ = *p; } *o = 0; }
static int suite(int argc, char** argv) {                                  /* suite PROGRAM [SECONDS] [PREFIX] */
    if (argc < 3) return 2;
    int seconds = argc > 3 ? atoi(argv[3]) : 30; const char* prefix = argc > 4 ? argv[4] : "";
    char* list_argv[] = {argv[2], (char*)"-l", NULL};
    static char list[1 << 16]; capture(list_argv, 30, list, sizeof(list)); strip_colors(list);
    unsigned passed = 0, failed = 0, timeouts = 0, total = 0;
    for (char *line = list, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        while (*line == ' ') line++;
        if (!*line || strncmp(line, prefix, strlen(prefix)) || strstr(line, "Test Summary") || line[0] == '-') continue;
        char name[256]; snprintf(name, sizeof(name), "%s", line); char* tail = name + strlen(name); while (tail > name && tail[-1] == ' ') *--tail = 0;
        bool verbose = getenv("SUITE_VERBOSE") != NULL;
        char* run_argv[] = {argv[2], (char*)(verbose ? "-p" : "-q"), verbose ? name : (char*)"-p", verbose ? NULL : name, NULL};
        int status = capture(run_argv, seconds, out_buffer, sizeof(out_buffer)); strip_colors(out_buffer);
        unsigned t = 0, ok = 0, bad = 0; const char* sum = strstr(out_buffer, "Total: ");
        if (sum) sscanf(sum, "Total: %u, Passed: %u (%*[^)]), Failed: %u", &t, &ok, &bad);
        bool pass = status != -2 && sum && t && ok == t && !bad;           /* a prefix may also select longer names: all must pass */
        if (status == -2) timeouts++; else if (pass) passed++; else failed++;
        const char* verdict = status == -2 ? "Killed (time limit)" : pass ? "Passed" : strstr(out_buffer, "Timed out") ? "Timed out" : "Failed";
        total++; printf("[64] BZ SUITE %s | %s | %s\n", argv[2], name, verdict);
        if (!pass && getenv("SUITE_DETAIL")) {                             /* the failing test's last lines (debugging) */
            int keep = getenv("SUITE_VERBOSE") ? 45 : 14;
            char* at = out_buffer + strlen(out_buffer); for (int lines = 0; at > out_buffer && lines < keep; at--) if (at[-1] == '\n') lines++;
            for (char* l = strtok(at, "\n"); l; l = strtok(NULL, "\n")) printf("    %s\n", l);
        }
        fflush(stdout);
    }
    printf("[64] BZ SUITE SUMMARY %s total=%u passed=%u failed=%u killed=%u\n", argv[2], total, passed, failed, timeouts);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "hostile")) return hostile();
    if (argc > 1 && !strcmp(argv[1], "suite")) return suite(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "l2-server")) return l2_server(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "l2-client")) return l2_client(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "rc-server")) return rc_server(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "rc-client")) return rc_client(argc, argv);
    if (argc > 1 && argv[1][0] != '/' && argv[1][1]) { printf("unknown subcommand %s\n", argv[1]); return 2; }
    const char* path = argc > 1 && argv[1][0] == '/' ? argv[1] : "/nexus/bt-script";   /* the campaign passes a mode letter */
    printf("[64] BZ START uid=%d script=%s\n", (int)getuid(), path);
    int f = script(path);
    printf(f ? "[64] BZ TEST FAILED failures=%d\n" : "[64] BZ TEST PASSED failures=%d\n", f);
    return f ? 1 : 0;
}
