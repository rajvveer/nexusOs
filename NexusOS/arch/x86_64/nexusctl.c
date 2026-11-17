/* nexusctl - control the NexusOS service manager (nexusd) and read the system journal.
 *
 *   nexusctl [list]                                   units and their states
 *   nexusctl start|stop|restart|try-restart|reload|reset-failed|kill UNIT...   (start/restart wait for the result
 *                                                     unless --no-block; mutating verbs need root, i.e. sudo)
 *   nexusctl status|show|is-active|is-enabled|is-failed UNIT...
 *   nexusctl enable|disable UNIT... [--now]
 *   nexusctl daemon-reload | session [UID] | session-end [UID] | ping
 *   nexusctl list-timers | list-sockets | list-paths
 *   nexusctl logs [-u UNIT] [-k] [-p PRIORITY] [-n LINES] [-b] [-f] [-g TEXT] [--since SECONDS] [-o short|raw|cat]
 *        (also `nexusctl journal ...`; reading needs root or membership of group adm)
 *   nexusctl --user ...   the same for the caller's own user manager (user@UID.service: $XDG_RUNTIME_DIR/nexus/control,
 *                         journal ~/.local/state/nexus/journal/user.journal); UNIT names default to .service
 * Exit status follows systemctl/journalctl: 0 ok, 1 failure, 3 not active, 4 permission, 5 unit not found.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "nexusd_proto.h"

static char control[108] = NEXUSD_CONTROL, journal_dir[PATH_MAX] = NEXUS_JOURNAL_DIR, journal_file[64] = NEXUS_JOURNAL_FILE;
static bool user_scope;
/* --user: the caller's own manager */
static int use_user_manager(void) {
    const char* rt = getenv("XDG_RUNTIME_DIR"); char fallback[64]; snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
    if (!rt || rt[0] != '/') rt = fallback;
    if ((size_t)snprintf(control, sizeof(control), "%s/%s/control", rt, NEXUSD_USER_RUN) >= sizeof(control)) { fprintf(stderr, "nexusctl: XDG_RUNTIME_DIR too long\n"); return 1; }
    struct passwd* pw = getpwuid(getuid()); const char* home = pw && pw->pw_dir[0] == '/' ? pw->pw_dir : getenv("HOME");
    if (!home) { fprintf(stderr, "nexusctl: no home folder\n"); return 1; }
    snprintf(journal_dir, sizeof(journal_dir), "%s/%s", home, NEXUS_USER_JOURNAL_DIR); snprintf(journal_file, sizeof(journal_file), "%s", NEXUS_USER_JOURNAL_FILE);
    user_scope = true; return 0;
}
static int request(const char* line, bool echo, char* captured, size_t capsize) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (fd < 0) { perror("nexusctl: socket"); return 1; }
    struct sockaddr_un a = {.sun_family = AF_UNIX}; snprintf(a.sun_path, sizeof(a.sun_path), "%s", control);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a))) {
        fprintf(stderr, "nexusctl: the %s is not running (%s: %s)\n", user_scope ? "user manager" : "service manager", control, strerror(errno)); close(fd); return 1;
    }
    size_t n = strlen(line);
    if (send(fd, line, n, MSG_NOSIGNAL) != (ssize_t)n || send(fd, "\n", 1, MSG_NOSIGNAL) != 1) { perror("nexusctl: send"); close(fd); return 1; }
    char buf[8192]; size_t used = 0, cap_used = 0; int status = 1; bool done = false;
    for (;;) {
        ssize_t r = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (r <= 0) break;
        used += (size_t)r; buf[used] = 0;
        char* start = buf;
        for (char* nl; (nl = strchr(start, '\n')); start = nl + 1) {
            *nl = 0;
            if (start[0] == NEXUSD_REPLY_END) { status = atoi(start + 1); done = true; continue; }
            if (echo) puts(start);
            if (captured && cap_used + strlen(start) + 2 < capsize) { strcpy(captured + cap_used, start); cap_used += strlen(start); captured[cap_used++] = '\n'; captured[cap_used] = 0; }
        }
        used = strlen(start); memmove(buf, start, used + 1);
        if (used >= sizeof(buf) - 1) used = 0;
    }
    close(fd);
    if (!done) { fprintf(stderr, "nexusctl: no reply from the service manager\n"); return 1; }
    return status;
}
static bool valid_arg(const char* s) {
    if (!*s || strlen(s) > 150) return false;
    for (const char* p = s; *p; p++) if ((unsigned char)*p <= ' ' || *p == 0x7f) return false;
    return true;
}
/* start/restart: wait until the unit leaves activating */
static int wait_settled(const char* unit, int seconds) {
    char line[256], out[4096]; snprintf(line, sizeof(line), "show %s", unit);
    for (int i = 0; i < seconds * 10; i++) {
        out[0] = 0; if (request(line, false, out, sizeof(out)) == 1 && !out[0]) return 1;
        char* state = strstr(out, "ActiveState="); if (!state) return 1;
        state += 12;
        if (!strncmp(state, "active", 6)) return 0;
        if (!strncmp(state, "failed", 6)) { fprintf(stderr, "Job for %s failed. See \"nexusctl status %s\" and \"nexusctl logs -u %s\".\n", unit, unit, unit); return 1; }
        if (!strncmp(state, "inactive", 8)) {
            char* result = strstr(out, "Result="); if (result && (!strncmp(result + 7, "success", 7) || !strncmp(result + 7, "condition", 9))) return 0;
            fprintf(stderr, "%s did not start (%.*s).\n", unit, result ? (int)strcspn(result + 7, "\n") : 7, result ? result + 7 : "unknown"); return 1;
        }
        struct timespec t = {0, 100000000}; nanosleep(&t, NULL);
    }
    fprintf(stderr, "nexusctl: timed out waiting for %s\n", unit); return 1;
}
/* stop: wait until the unit is no longer active or deactivating */
static int wait_stopped(const char* unit, int seconds) {
    char line[256], out[4096]; snprintf(line, sizeof(line), "show %s", unit);
    for (int i = 0; i < seconds * 10; i++) {
        out[0] = 0; if (request(line, false, out, sizeof(out)) == 1 && !out[0]) return 1;
        char* state = strstr(out, "ActiveState="); if (!state) return 1;
        state += 12;
        if (!strncmp(state, "inactive", 8) || !strncmp(state, "failed", 6)) return 0;
        struct timespec t = {0, 100000000}; nanosleep(&t, NULL);
    }
    fprintf(stderr, "nexusctl: timed out waiting for %s to stop\n", unit); return 1;
}

/* ------------------------------------------------------------------------------------------------ journal */
struct record { unsigned long long real, mono; char boot[17]; int pri; char unit[160], ident[96]; long pid; char* message; };
static bool parse_record(char* line, struct record* r) {
    char* fields[7]; char* p = line;
    for (int i = 0; i < 7; i++) { fields[i] = p; p = strchr(p, ' '); if (!p) return false; *p++ = 0; }
    r->real = strtoull(fields[0], NULL, 10); r->mono = strtoull(fields[1], NULL, 10);
    snprintf(r->boot, sizeof(r->boot), "%s", fields[2]); r->pri = atoi(fields[3]);
    snprintf(r->unit, sizeof(r->unit), "%s", fields[4]); snprintf(r->ident, sizeof(r->ident), "%s", fields[5]);
    r->pid = atol(fields[6]);
    char* o = p; r->message = p;           /* unescape \xHH in place */
    for (char* s = p; *s; ) {
        if (s[0] == '\\' && s[1] == 'x' && s[2] && s[3]) { char hex[3] = {s[2], s[3], 0}; *o++ = (char)strtol(hex, NULL, 16); s += 4; }
        else *o++ = *s++;
    }
    *o = 0; return true;
}
struct filter { const char* unit; bool kernel; int max_pri; bool this_boot; char boot[17]; const char* grep; unsigned long long since_us; const char* output; };
static bool matches(const struct filter* f, const struct record* r) {
    if (r->pri > f->max_pri) return false;
    if (f->kernel && strcmp(r->unit, "kernel")) return false;
    if (f->unit && strcmp(r->unit, f->unit) && strcmp(r->ident, f->unit)) {
        /* "-u nexusd" also names nexusd's own records (unit "nexusd"); syslog records of a unit's process name it too */
        char stem[160]; snprintf(stem, sizeof(stem), "%s", f->unit); char* dot = strstr(stem, ".service"); if (dot) *dot = 0;
        if (strcmp(r->unit, stem) && (strcmp(r->ident, stem) || strcmp(r->unit, "-"))) return false;
    }
    if (f->this_boot && strcmp(r->boot, f->boot)) return false;
    if (f->since_us && r->real < f->since_us) return false;
    if (f->grep && !strstr(r->message, f->grep)) return false;
    return true;
}
static void print_record(const struct filter* f, const struct record* r, const char* host) {
    if (f->output && !strcmp(f->output, "cat")) { puts(r->message); return; }
    if (f->output && !strcmp(f->output, "raw")) { printf("%llu %d %s %s %ld %s\n", r->real, r->pri, r->unit, r->ident, r->pid, r->message); return; }
    time_t t = (time_t)(r->real / 1000000); struct tm tm; localtime_r(&t, &tm); char stamp[32]; strftime(stamp, sizeof(stamp), "%b %d %H:%M:%S", &tm);
    if (r->pid > 0) printf("%s %s %s[%ld]: %s\n", stamp, host, r->ident, r->pid, r->message);
    else printf("%s %s %s: %s\n", stamp, host, r->ident, r->message);
}
static char** read_lines(const char* path, size_t* count) {
    *count = 0; FILE* fp = fopen(path, "re"); if (!fp) return NULL;
    char** lines = NULL; size_t cap = 0; char* line = NULL; size_t lcap = 0; ssize_t n;
    while ((n = getline(&line, &lcap, fp)) > 0) {
        if (line[n - 1] == '\n') line[--n] = 0;
        if (*count == cap) { cap = cap ? cap * 2 : 1024; char** grown = realloc(lines, cap * sizeof(char*)); if (!grown) break; lines = grown; }
        lines[(*count)++] = strdup(line);
    }
    free(line); fclose(fp); return lines;
}
static int logs(int argc, char** argv) {
    struct filter f = {.max_pri = 7}; long lines = -1; bool follow = false;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if ((!strcmp(a, "-u") || !strcmp(a, "--unit") || !strcmp(a, "--user-unit")) && i + 1 < argc) { static char unit[160]; snprintf(unit, sizeof(unit), "%s%s", argv[i + 1], strchr(argv[i + 1], '.') ? "" : ".service"); f.unit = unit; i++; }
        else if (!strcmp(a, "-k") || !strcmp(a, "--dmesg")) f.kernel = true;
        else if ((!strcmp(a, "-n") || !strcmp(a, "--lines")) && i + 1 < argc) lines = atol(argv[++i]);
        else if (!strcmp(a, "-f") || !strcmp(a, "--follow")) follow = true;
        else if (!strcmp(a, "-b") || !strcmp(a, "--boot")) f.this_boot = true;
        else if ((!strcmp(a, "-g") || !strcmp(a, "--grep")) && i + 1 < argc) f.grep = argv[++i];
        else if (!strcmp(a, "--since") && i + 1 < argc) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); f.since_us = ((unsigned long long)t.tv_sec - strtoull(argv[++i], NULL, 10)) * 1000000ULL; }
        else if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && i + 1 < argc) f.output = argv[++i];
        else if ((!strcmp(a, "-p") || !strcmp(a, "--priority")) && i + 1 < argc) {
            static const char* const names[] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};
            const char* v = argv[++i]; f.max_pri = -1;
            for (int k = 0; k < 8; k++) if (!strcmp(v, names[k])) f.max_pri = k;
            if (f.max_pri < 0) f.max_pri = atoi(v);
        } else { fprintf(stderr, "nexusctl logs: unknown option %s\n", a); return 2; }
    }
    if (f.this_boot) { int fd = open(NEXUSD_RUN "/boot_id", O_RDONLY | O_CLOEXEC); if (fd >= 0) { ssize_t n = read(fd, f.boot, 16); close(fd); if (n == 16) f.boot[16] = 0; else f.this_boot = false; } else f.this_boot = false; }
    char host[64] = "nexus"; gethostname(host, sizeof(host)); host[sizeof(host) - 1] = 0;
    /* oldest file first: system.journal.N .. .1, system.journal */
    char path[PATH_MAX]; struct record* matched = NULL; char** keep = NULL; size_t nm = 0, cap = 0; bool any = false, denied = false;
    for (int i = 100; i >= 0; i--) {
        if (i) snprintf(path, sizeof(path), "%s/%s.%d", journal_dir, journal_file, i); else snprintf(path, sizeof(path), "%s/%s", journal_dir, journal_file);
        if (access(path, F_OK)) continue;
        size_t count; char** all = read_lines(path, &count);
        if (!all) { if (errno == EACCES) denied = true; continue; }
        any = true;
        for (size_t k = 0; k < count; k++) {
            struct record r; if (!all[k] || !parse_record(all[k], &r) || !matches(&f, &r)) { free(all[k]); continue; }
            if (nm == cap) { cap = cap ? cap * 2 : 4096; struct record* g1 = realloc(matched, cap * sizeof(*g1)); char** g2 = realloc(keep, cap * sizeof(*g2)); if (!g1 || !g2) { fprintf(stderr, "nexusctl: out of memory\n"); return 1; } matched = g1; keep = g2; }
            matched[nm] = r; keep[nm] = all[k]; nm++;
        }
        free(all);
    }
    if (!any) { fprintf(stderr, denied ? "nexusctl: no permission to read the journal (root or group adm)\n" : "nexusctl: no journal files in %s\n", journal_dir); return denied ? 4 : 1; }
    size_t first = lines >= 0 && (size_t)lines < nm ? nm - (size_t)lines : 0;
    for (size_t k = first; k < nm; k++) print_record(&f, &matched[k], host);
    fflush(stdout);
    for (size_t k = 0; k < nm; k++) free(keep[k]);
    free(matched); free(keep);
    if (!follow) return 0;
    /* follow: poll the current file for appended lines; reopen after a rotation */
    snprintf(path, sizeof(path), "%s/%s", journal_dir, journal_file);
    FILE* fp = fopen(path, "re"); if (!fp) return 1;
    fseek(fp, 0, SEEK_END); struct stat st; fstat(fileno(fp), &st); ino_t ino = st.st_ino;
    char* line = NULL; size_t lcap = 0;
    for (;;) {
        ssize_t n;
        while ((n = getline(&line, &lcap, fp)) > 0) {
            if (line[n - 1] != '\n') { fseek(fp, -n, SEEK_CUR); break; }
            line[n - 1] = 0; struct record r; if (parse_record(line, &r) && matches(&f, &r)) { print_record(&f, &r, host); fflush(stdout); }
        }
        clearerr(fp);
        struct stat now; if (!stat(path, &now) && now.st_ino != ino) { fclose(fp); fp = fopen(path, "re"); if (!fp) return 1; ino = now.st_ino; }
        struct timespec t = {0, 250000000}; nanosleep(&t, NULL);
    }
}

static void usage(void) {
    fputs("usage: nexusctl [--user] [list] | start|stop|restart|try-restart|reload|status|show|is-active|is-enabled|is-failed|enable|disable|reset-failed|kill UNIT...\n"
          "       nexusctl [--user] daemon-reload | list-timers | list-sockets | list-paths | ping | logs [-u UNIT] [-k] [-p PRI] [-n N] [-b] [-f] [-g TEXT] [--since SEC] [-o short|raw|cat]\n"
          "       nexusctl session [UID] | session-end [UID]\n", stderr);
}
static bool has_suffix(const char* s) {
    static const char* const suffixes[] = {".service", ".socket", ".timer", ".path"};
    for (unsigned i = 0; i < 4; i++) { size_t a = strlen(s), b = strlen(suffixes[i]); if (a > b && !strcmp(s + a - b, suffixes[i])) return true; }
    return false;
}
int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--user")) { if (use_user_manager()) return 1; argv[1] = argv[0]; argv++; argc--; }
    if (argc < 2 || !strcmp(argv[1], "list") || !strcmp(argv[1], "list-units")) return request("list", true, NULL, 0);
    const char* verb = argv[1];
    if (!strcmp(verb, "-h") || !strcmp(verb, "--help") || !strcmp(verb, "help")) { usage(); return 0; }
    if (!strcmp(verb, "logs") || !strcmp(verb, "journal")) return logs(argc - 2, argv + 2);
    if (!strcmp(verb, "list-timers")) return request("timers", true, NULL, 0);
    if (!strcmp(verb, "list-sockets")) return request("sockets", true, NULL, 0);
    if (!strcmp(verb, "list-paths")) return request("paths", true, NULL, 0);
    bool no_block = false, now = false; char line[2048]; size_t n = (size_t)snprintf(line, sizeof(line), "%s", verb);
    const char* units[64]; int nunits = 0;
    if (!valid_arg(verb)) { usage(); return 2; }
    bool quiet = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-block")) { no_block = true; continue; }
        if (!strcmp(argv[i], "--now")) { now = true; continue; }
        if (!strcmp(argv[i], "--quiet") || !strcmp(argv[i], "-q")) { quiet = true; continue; }
        if (!valid_arg(argv[i]) || nunits == 64) { fprintf(stderr, "nexusctl: invalid argument\n"); return 2; }
        units[nunits++] = argv[i];
        if (n + strlen(argv[i]) + 2 >= sizeof(line)) { fprintf(stderr, "nexusctl: too many arguments\n"); return 2; }
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %s", argv[i]);
    }
    if ((!strcmp(verb, "session") || !strcmp(verb, "session-end")) && !nunits) { snprintf(line, sizeof(line), "%s %u", verb, (unsigned)getuid()); }
    int status = request(line, !quiet, NULL, 0);
    if (status) return status;
    if (now && (!strcmp(verb, "enable") || !strcmp(verb, "disable"))) {
        snprintf(line, sizeof(line), "%s", verb[0] == 'e' ? "start" : "stop");
        for (int i = 0; i < nunits; i++) { size_t l = strlen(line); snprintf(line + l, sizeof(line) - l, " %s", units[i]); }
        status = request(line, true, NULL, 0); if (status) return status;
        verb = verb[0] == 'e' ? "start" : "stop";
    }
    if (!no_block && (!strcmp(verb, "start") || !strcmp(verb, "restart") || !strcmp(verb, "stop")))
        for (int i = 0; i < nunits; i++) {
            char unit[200]; snprintf(unit, sizeof(unit), "%s%s", units[i], has_suffix(units[i]) ? "" : ".service");
            int r = !strcmp(verb, "stop") ? wait_stopped(unit, 120) : wait_settled(unit, 120); if (r) status = r;
        }
    return status;
}
