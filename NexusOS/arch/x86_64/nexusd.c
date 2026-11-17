/* nexusd - the NexusOS service manager and system journal.
 *
 * Aurora (/sbin/init, pid 1, the desktop user) starts /usr/lib/nexus/nexusd at boot. On an ext4 root the binary is
 * set-user-ID root: nexusd becomes root (real, effective and saved IDs, no supplementary groups), takes the singleton
 * lock and supervises the system services. On a FAT32 root (no set-ID files) it runs as the desktop user and starts
 * only the units that need no other identity.
 *
 * Units: systemd-style INI files in /usr/lib/nexus/services (vendor) and /etc/nexus/services (admin, wins by name),
 * drop-ins <unit>.d/NAME.conf, templates name@.service (instances name@inst.service; %i %I %n %N %p %u %U %h %t %S %C %L
 * %E %T %V %H).
 *   [Unit]    Description Requires Wants After Before Conflicts ConditionPathExists ConditionPathIsDirectory
 *             ConditionFileIsExecutable DefaultDependencies
 *   [Service] Type=simple|exec|forking|oneshot|notify ExecStartPre ExecStart ExecStartPost ExecStop ExecReload
 *             (prefixes - @ + !) User Group SupplementaryGroups Environment EnvironmentFile WorkingDirectory UMask
 *             Restart=no|on-success|on-failure|on-abnormal|always RestartSec RestartMaxDelaySec StartLimitBurst
 *             StartLimitIntervalSec TimeoutStartSec TimeoutStopSec PIDFile RemainAfterExit RuntimeDirectory
 *             RuntimeDirectoryMode StateDirectory LogsDirectory CacheDirectory KillSignal KillMode SyslogIdentifier
 *             StandardOutput=journal|null|console LimitNOFILE Nice ReadyPath (NexusOS: active once the path exists)
 *             AllowUserInstance (NexusOS: user <uid> may start/stop the instance named <uid>)
 *   [Install] WantedBy (multi-user: boot; session: per-user session) DefaultEnabled=yes (NexusOS: enabled unless the
 *             admin disabled it)
 * Readiness: notify units get NOTIFY_SOCKET (a datagram socket of their own; READY=1, STATUS=, MAINPID=, STOPPING=1);
 * forking units are ready when the starter exits 0 (main PID from PIDFile); ReadyPath units when the path appears.
 * Restart: Restart= policy with exponential backoff (RestartSec doubling up to RestartMaxDelaySec) and a start limit
 * (StartLimitBurst starts within StartLimitIntervalSec -> failed, result start-limit-hit).
 * Journal: service stdout/stderr (pipes), syslog datagrams on /run/nexus/log (/dev/log), kernel records from /dev/kmsg
 * and nexusd's own events, appended to /var/log/journal/system.journal (format in nexusd_proto.h) with size rotation.
 * Control: /run/nexus/control (see nexusd_proto.h), used by nexusctl. SIGTERM: stop every unit (reverse order) and exit.
 * Socket, timer and path units, journal rate limiting: nexusd_units.c. User managers: `nexusd --user` (the
 * user@UID.service unit, started with the user's session: "session UID", stopped by "session-end UID") runs as that
 * user with its own units, journal and control socket (nexusd_proto.h); nexusctl --user talks to it.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <limits.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>
#include "nexusd_proto.h"

#define MAX_UNITS 512
#define MAX_CLIENTS 32
#define LINE_MAX_BYTES 4096
#define WORD_BYTES 2048
#define UPATH 256
#define INFINITE_MS UINT64_MAX

/* ------------------------------------------------------------------------------------------------ small helpers */
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static uint64_t mono_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
static uint64_t real_us(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
static void* xcalloc(size_t n, size_t s) { void* p = calloc(n, s); if (!p) { perror("nexusd: out of memory"); _exit(1); } return p; }
static char* xstrdup(const char* s) { char* p = strdup(s); if (!p) { perror("nexusd: out of memory"); _exit(1); } return p; }
static bool copy_str(char* out, size_t size, const char* in) { size_t n = strlen(in); if (n >= size) return false; memcpy(out, in, n + 1); return true; }
static char* trim(char* s) { while (isspace((unsigned char)*s)) s++; char* e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }
static bool ends_with(const char* s, const char* suffix) { size_t a = strlen(s), b = strlen(suffix); return a >= b && !strcmp(s + a - b, suffix); }
static bool mkdir_p(const char* path, mode_t mode) {
    char buf[PATH_MAX]; if (!copy_str(buf, sizeof(buf), path)) return false;
    for (char* p = buf + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(buf, 0755) && errno != EEXIST) return false; *p = '/'; }
    return !mkdir(buf, mode) || errno == EEXIST;
}

struct strlist { char** v; unsigned n; };
static void sl_add(struct strlist* l, const char* s) {
    char** v = realloc(l->v, (l->n + 2) * sizeof(char*)); if (!v) { perror("nexusd"); _exit(1); }
    l->v = v; l->v[l->n++] = xstrdup(s); l->v[l->n] = NULL;
}
static void sl_clear(struct strlist* l) { for (unsigned i = 0; i < l->n; i++) free(l->v[i]); free(l->v); l->v = NULL; l->n = 0; }
static bool sl_has(const struct strlist* l, const char* s) { for (unsigned i = 0; i < l->n; i++) if (!strcmp(l->v[i], s)) return true; return false; }
/* whitespace-separated words of value appended to l */
static void sl_words(struct strlist* l, const char* value) {
    char buf[LINE_MAX_BYTES]; if (!copy_str(buf, sizeof(buf), value)) return;
    for (char* save = NULL, *w = strtok_r(buf, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) if (!sl_has(l, w)) sl_add(l, w);
}

/* ------------------------------------------------------------------------------------------------ paths of this manager */
/* the system manager's, or a user manager's (nexusd --user: set up in main) */
static bool root_mode, user_mode;
static char run_dir[PATH_MAX] = NEXUSD_RUN, control_path[PATH_MAX] = NEXUSD_CONTROL, notify_dir[PATH_MAX] = NEXUSD_NOTIFY_DIR,
            lock_path[PATH_MAX] = NEXUSD_LOCK, enabled_dir[PATH_MAX] = NEXUS_ENABLED_DIR, disabled_dir[PATH_MAX] = NEXUS_DISABLED_DIR,
            journal_dir[PATH_MAX] = NEXUS_JOURNAL_DIR, journal_file[64] = NEXUS_JOURNAL_FILE, timers_dir[PATH_MAX] = NEXUS_TIMERS_DIR;
static char user_dir_buf[3][PATH_MAX];
/* Unit directories in priority order: the NexusOS ones first (admin wins over vendor), then systemd's. Debian packages
 * ship systemd units: those are used when NexusOS has no unit of that name (apt install of a daemon then starts it). */
static const char* unit_dirs[6] = {NEXUS_ADMIN_UNITS, NEXUS_VENDOR_UNITS, "/etc/systemd/system", "/usr/lib/systemd/system"};
static unsigned unit_dir_count = 4, nexus_unit_dirs = 2;

/* ------------------------------------------------------------------------------------------------ journal */
static struct {
    int fd; uint64_t size, max_size; unsigned max_files; bool console; int console_level;
    uint64_t last_sync_ms; bool dirty; char boot_id[17]; bool ok;
    uint64_t rl_interval; unsigned rl_burst;   /* rate limit per source (nexusd_units.c) */
} journal = {.fd = -1, .max_size = 4 << 20, .max_files = 8, .console_level = 3, .rl_interval = 30000, .rl_burst = 10000};
static bool rl_allow(const char* unit, const char* ident, long pid);

static long long parse_time_ms(const char* v);
static void journal_config(void) {
    FILE* f = fopen(NEXUS_JOURNAL_CONF, "re"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* s = trim(line); if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char* eq = strchr(s, '='); if (!eq) continue; *eq = 0; char *k = trim(s), *v = trim(eq + 1);
        char* end; unsigned long long n = strtoull(v, &end, 10);
        if (!strcmp(k, "SystemMaxFileSize") && end != v) {
            if (*end == 'K' || *end == 'k') n <<= 10; else if (*end == 'M' || *end == 'm') n <<= 20; else if (*end == 'G' || *end == 'g') n <<= 30;
            if (n >= 64 * 1024 && n <= (1ULL << 32)) journal.max_size = n;
        } else if (!strcmp(k, "SystemMaxFiles") && end != v && n >= 1 && n <= 100) journal.max_files = (unsigned)n;
        else if (!strcmp(k, "ForwardToConsole")) journal.console = !strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "true");
        else if (!strcmp(k, "MaxLevelConsole") && end != v && n <= 7) journal.console_level = (int)n;
        else if (!strcmp(k, "RateLimitBurst") && end != v && n <= 1000000) journal.rl_burst = (unsigned)n;
        else if (!strcmp(k, "RateLimitIntervalSec") || !strcmp(k, "RateLimitInterval")) {
            long long ms = parse_time_ms(v); if (ms >= 0 && ms <= 3600000) journal.rl_interval = (uint64_t)ms; }
    }
    fclose(f);
}
/* readable by root and group adm (Debian's log readers); the group may appear only after the first-boot install */
static void journal_owner(void) {
    if (!root_mode) return;
    struct group* adm = getgrnam("adm"); gid_t gid = adm ? adm->gr_gid : 0;
    if (chown(journal_dir, 0, gid) || chmod(journal_dir, 02750)) {}
    DIR* d = opendir(journal_dir); if (!d) return; struct dirent* e;
    while ((e = readdir(d))) if (!strncmp(e->d_name, journal_file, strlen(journal_file))) {
        char p[PATH_MAX + 300]; snprintf(p, sizeof(p), "%s/%s", journal_dir, e->d_name);
        if (lchown(p, 0, gid) || chmod(p, 0640)) {}
    }
    closedir(d);
}
static void journal_open(void) {
    if (!mkdir_p(journal_dir, user_mode ? 0700 : 02750)) return;
    char path[PATH_MAX + 64]; snprintf(path, sizeof(path), "%s/%s", journal_dir, journal_file);
    journal.fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (journal.fd < 0) return;
    journal_owner();
    struct stat st; journal.size = fstat(journal.fd, &st) ? 0 : (uint64_t)st.st_size; journal.ok = true;
}
static void journal_rotate(void) {
    char from[PATH_MAX], to[PATH_MAX];
    if (journal.fd >= 0) { fdatasync(journal.fd); close(journal.fd); journal.fd = -1; }
    snprintf(to, sizeof(to), "%s/%s.%u", journal_dir, journal_file, journal.max_files - 1);
    if (journal.max_files > 1) unlink(to);
    for (unsigned i = journal.max_files - 1; i >= 1; i--) {
        if (i == 1) snprintf(from, sizeof(from), "%s/%s", journal_dir, journal_file);
        else snprintf(from, sizeof(from), "%s/%s.%u", journal_dir, journal_file, i - 1);
        snprintf(to, sizeof(to), "%s/%s.%u", journal_dir, journal_file, i);
        rename(from, to);
    }
    if (journal.max_files == 1) { snprintf(from, sizeof(from), "%s/%s", journal_dir, journal_file); unlink(from); }
    journal_open();
}
static const char* const level_names[] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};
/* One record; message bytes need not be terminated. */
static void token(char* out, size_t size, const char* in) {
    size_t k = 0;
    for (const char* p = in && *in ? in : "-"; *p && k < size - 1; p++) out[k++] = (*p > ' ' && *p != 127) ? *p : '_';
    out[k] = 0;
}
static void journal_write_raw(int priority, const char* unit, const char* ident, long pid, const char* message, size_t length) {
    if (priority < 0 || priority > 7) priority = 6;
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r')) length--;
    if (length > 8000) length = 8000;
    static char line[8000 * 4 + 512]; size_t n = 0;
    char idbuf[96], unitbuf[160]; token(idbuf, sizeof(idbuf), ident); token(unitbuf, sizeof(unitbuf), unit);
    n = (size_t)snprintf(line, sizeof(line), "%llu %llu %s %d %s %s %ld ", (unsigned long long)real_us(), (unsigned long long)mono_us(),
                         journal.boot_id, priority, unitbuf, idbuf, pid);
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)message[i];
        if (c < 0x20 || c == '\\' || c == 0x7f) n += (size_t)snprintf(line + n, 5, "\\x%02x", c);
        else line[n++] = (char)c;
    }
    line[n++] = '\n';
    if (journal.console && priority <= journal.console_level) {
        char head[160]; int h = snprintf(head, sizeof(head), "[journal] %s[%ld] %s: ", idbuf, pid, level_names[priority]);
        if (h > 0) { ssize_t w = write(1, head, (size_t)h); (void)w; } ssize_t w = write(1, message, length); (void)w; w = write(1, "\n", 1); (void)w;
    }
    if (journal.fd < 0) return;
    if (journal.size + n > journal.max_size) journal_rotate();
    if (journal.fd < 0) return;
    ssize_t w = write(journal.fd, line, n);
    if (w > 0) journal.size += (uint64_t)w;
    journal.dirty = true;
    if (priority <= 3) { fdatasync(journal.fd); journal.dirty = false; journal.last_sync_ms = now_ms(); }
}
/* a record of a source that is within its rate limit */
static void journal_write(int priority, const char* unit, const char* ident, long pid, const char* message, size_t length) {
    if (rl_allow(unit, ident, pid)) journal_write_raw(priority, unit, ident, pid, message, length);
}
static void journal_sync(bool force) {
    if (journal.fd >= 0 && journal.dirty && (force || now_ms() - journal.last_sync_ms >= 5000)) {
        fdatasync(journal.fd); journal.dirty = false; journal.last_sync_ms = now_ms();
    }
}
/* nexusd's own events: journal (identifier nexusd) and the console (the boot log). */
static void mlog(int priority, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void mlog(int priority, const char* fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (n < 0) return; if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    journal_write(priority, "nexusd", "nexusd", getpid(), buf, (size_t)n);
    printf("[nexusd] %s\n", buf); fflush(stdout);
}

/* ------------------------------------------------------------------------------------------------ units */
enum utype { T_SIMPLE, T_EXEC, T_FORKING, T_ONESHOT, T_NOTIFY };
enum ustate { S_INACTIVE, S_WAITING, S_STARTING, S_ACTIVE, S_STOPPING, S_FAILED, S_RESTART_WAIT };
enum restart { R_NO, R_ON_SUCCESS, R_ON_FAILURE, R_ON_ABNORMAL, R_ALWAYS };
enum phase { P_NONE, P_PRE, P_START, P_POST, P_STOP, P_RELOAD };
enum ukind { K_SERVICE, K_SOCKET, K_TIMER, K_PATH };
#include "nexusd_units.h"
static const char* const state_names[] = {"inactive", "activating", "activating", "active", "deactivating", "failed", "activating"};
static const char* const type_names[] = {"simple", "exec", "forking", "oneshot", "notify"};
struct condition { char path[UPATH]; char kind; bool negate; };
struct unit {
    char name[160], file[UPATH], description[256], instance[96];
    struct strlist requires, wants, after, before, conflicts, wanted_by;
    struct condition conds[8]; unsigned nconds;
    enum utype type; struct strlist pre, start, post, stop, reload;
    char user[64], group[64]; struct strlist supp, env, env_files;
    char workdir[UPATH]; mode_t umask_value; bool umask_set;
    enum restart restart; uint64_t restart_ms, restart_max_ms, limit_interval_ms; unsigned limit_burst;
    uint64_t timeout_start_ms, timeout_stop_ms; char pidfile[UPATH]; bool remain;
    struct strlist runtime_dirs, state_dirs, logs_dirs, cache_dirs; mode_t runtime_mode;
    char ready_path[UPATH], ident[64]; int kill_signal; bool kill_group; int output; /* 0 journal 1 null 2 console */
    long long nofile; int nice; bool nice_set, allow_user, default_enabled, default_deps, loaded, is_template, systemd;
    /* runtime */
    enum ustate state; enum phase phase; unsigned exec_index; bool wanted, stop_wanted, condition_failed, restart_requested;
    pid_t main_pid, control_pid; bool main_is_child; int out_fd, out_wr; int notify_fd; char notify_path[UPATH];
    uint64_t deadline_ms, restart_at_ms, active_since_ms, next_liveness_ms, ready_poll_ms; unsigned restarts;
    uint64_t starts[32]; unsigned nstarts; char result[64]; int exit_code, exit_signal; char status_text[256];
    char linebuf[LINE_MAX_BYTES]; size_t linelen;
    /* socket/timer/path units (nexusd_units.c) and socket activation of services */
    enum ukind kind; struct actpart* act; char trigger[160];          /* Service= / Unit= */
    struct strlist sockets_opt; bool stdin_socket, output_explicit;   /* Sockets=, StandardInput=socket */
    int conn_fd; bool transient, gc; char accepted_by[160];          /* an Accept=yes instance and its connection */
    uint64_t active_enter_ms, inactive_enter_ms;                       /* for OnUnitActiveSec= / OnUnitInactiveSec= */
};
static struct unit* units[MAX_UNITS]; static unsigned nunits;
static bool shutting_down;
static int devnull = -1;

static struct unit* find_unit(const char* name) { for (unsigned i = 0; i < nunits; i++) if (!strcmp(units[i]->name, name)) return units[i]; return NULL; }
/* the identifier of a unit's output records: SyslogIdentifier, else the unit name without ".service" */
static const char* unit_ident(struct unit* u) {
    if (!u->ident[0]) { const char* dot = strrchr(u->name, '.'); size_t n = dot ? (size_t)(dot - u->name) : strlen(u->name);
        snprintf(u->ident, sizeof(u->ident), "%.*s", (int)(n < sizeof(u->ident) - 1 ? n : sizeof(u->ident) - 1), u->name); }
    return u->ident;
}
/* the unit whose main or control process is pid (syslog records), or NULL */
static const char* unit_of_pid(long pid) {
    if (pid <= 1) return NULL;
    for (unsigned i = 0; i < nunits; i++) if (units[i]->main_pid == pid || units[i]->control_pid == pid) return units[i]->name;
    return NULL;
}
static size_t unit_suffix_len(const char* name) {
    return ends_with(name, ".service") ? 8 : ends_with(name, ".socket") ? 7 : ends_with(name, ".timer") ? 6 : ends_with(name, ".path") ? 5 : 0;
}
static bool valid_unit_name(const char* name) {
    size_t n = strlen(name); if (!n || n >= 150 || !unit_suffix_len(name) || n == unit_suffix_len(name)) return false;
    for (const char* p = name; *p; p++) if (!(isalnum((unsigned char)*p) || strchr("-_.@:\\", *p))) return false;
    return name[0] != '.' && !strstr(name, "..");
}

static long long parse_time_ms(const char* v) {
    if (!strcmp(v, "infinity")) return -1;
    char* end; double x = strtod(v, &end); if (end == v || x < 0) return -2;
    while (*end == ' ') end++;
    double mult = 1000;
    if (!*end || !strcmp(end, "s") || !strcmp(end, "sec")) mult = 1000; else if (!strcmp(end, "ms")) mult = 1;
    else if (!strcmp(end, "min") || !strcmp(end, "m")) mult = 60000; else if (!strcmp(end, "h")) mult = 3600000; else return -2;
    return (long long)(x * mult);
}
static int signal_number(const char* v) {
    static const struct { const char* n; int s; } t[] = {{"SIGTERM", SIGTERM}, {"SIGINT", SIGINT}, {"SIGKILL", SIGKILL}, {"SIGHUP", SIGHUP}, {"SIGQUIT", SIGQUIT}, {"SIGUSR1", SIGUSR1}, {"SIGUSR2", SIGUSR2}};
    for (unsigned i = 0; i < sizeof(t) / sizeof(*t); i++) if (!strcmp(v, t[i].n) || !strcmp(v, t[i].n + 3)) return t[i].s;
    int n = atoi(v); return n > 0 && n < 65 ? n : SIGTERM;
}
static bool truthy(const char* v) { return !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "on"); }

/* %-specifiers of templates and identities */
static void expand_specifiers(const struct unit* u, const char* in, char* out, size_t size) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < size; p++) {
        if (*p != '%' || !p[1]) { out[n++] = *p; continue; }
        char tmp[PATH_MAX] = ""; p++;
        switch (*p) {
        case 'i': case 'I': copy_str(tmp, sizeof(tmp), u->instance); break;
        case 'n': copy_str(tmp, sizeof(tmp), u->name); break;
        case 'N': snprintf(tmp, sizeof(tmp), "%.*s", (int)(strlen(u->name) - unit_suffix_len(u->name)), u->name); break;
        case 'p': { const char* at = strchr(u->name, '@'); snprintf(tmp, sizeof(tmp), "%.*s", (int)(at ? at - u->name : (long)(strlen(u->name) - unit_suffix_len(u->name))), u->name); break; }
        case 't': if (user_mode) { const char* r = getenv("XDG_RUNTIME_DIR"); copy_str(tmp, sizeof(tmp), r ? r : "/run"); } else copy_str(tmp, sizeof(tmp), "/run"); break;
        case 'u': case 'U': case 'h': {
            /* the unit's User=, else the manager's own user (root, or a user manager's user) */
            const char* who = u->user[0] ? u->user : NULL; struct passwd* pw = !who ? getpwuid(geteuid()) : isdigit((unsigned char)who[0]) ? getpwuid((uid_t)atol(who)) : getpwnam(who);
            if (!who) who = pw ? pw->pw_name : "root";
            if (*p == 'u') copy_str(tmp, sizeof(tmp), pw ? pw->pw_name : who);
            else if (*p == 'U') snprintf(tmp, sizeof(tmp), "%ld", pw ? (long)pw->pw_uid : -1L);
            else copy_str(tmp, sizeof(tmp), pw ? pw->pw_dir : "/");
            break; }
        case 'S': case 'C': case 'L': case 'E': {   /* state, cache, logs, configuration: system or the user's XDG folders */
            const char* home = getenv("HOME"); if (!user_mode || !home) home = "";
            if (!user_mode) copy_str(tmp, sizeof(tmp), *p == 'S' ? "/var/lib" : *p == 'C' ? "/var/cache" : *p == 'L' ? "/var/log" : "/etc");
            else snprintf(tmp, sizeof(tmp), "%s/%s", home, *p == 'S' ? ".local/state" : *p == 'C' ? ".cache" : *p == 'L' ? ".local/state/log" : ".config");
            break; }
        case 'T': copy_str(tmp, sizeof(tmp), "/tmp"); break;
        case 'V': copy_str(tmp, sizeof(tmp), "/var/tmp"); break;
        case 'H': if (gethostname(tmp, sizeof(tmp))) copy_str(tmp, sizeof(tmp), "localhost"); tmp[sizeof(tmp) - 1] = 0; break;
        case '%': copy_str(tmp, sizeof(tmp), "%"); break;
        default: snprintf(tmp, sizeof(tmp), "%%%c", *p);
        }
        for (char* q = tmp; *q && n + 1 < size; q++) out[n++] = *q;
    }
    out[n] = 0;
}

static void unit_defaults(struct unit* u) {
    u->type = T_SIMPLE; u->restart = R_NO; u->restart_ms = 1000; u->restart_max_ms = 60000; u->limit_interval_ms = 120000; u->limit_burst = 6;
    u->timeout_start_ms = 90000; u->timeout_stop_ms = 30000; u->kill_signal = SIGTERM; u->kill_group = true; u->runtime_mode = 0755;
    u->nofile = -1; u->default_deps = true; u->out_fd = -1; u->out_wr = -1; u->notify_fd = -1; u->exit_code = -1; u->umask_value = 022;
    u->conn_fd = -1;
}
static bool act_set_key(struct unit* u, const char* section, const char* key, const char* value);
static void set_key(struct unit* u, const char* section, const char* key, const char* raw, const char* file, unsigned lineno) {
    char value[LINE_MAX_BYTES]; expand_specifiers(u, raw, value, sizeof(value));
    bool reset = !*value;
    if (act_set_key(u, section, key, value)) return;
#define LIST(k, l) if (!strcmp(key, k)) { if (reset) sl_clear(&u->l); else sl_add(&u->l, value); return; }
#define WORDS(k, l) if (!strcmp(key, k)) { if (reset) sl_clear(&u->l); else sl_words(&u->l, value); return; }
    if (!strcmp(section, "Unit")) {
        if (!strcmp(key, "Description")) { copy_str(u->description, sizeof(u->description), value); return; }
        WORDS("Requires", requires) WORDS("Wants", wants) WORDS("After", after) WORDS("Before", before) WORDS("Conflicts", conflicts)
        if (!strcmp(key, "DefaultDependencies")) { u->default_deps = truthy(value); return; }
        if (!strncmp(key, "Condition", 9)) {
            char kind = !strcmp(key, "ConditionPathExists") ? 'e' : !strcmp(key, "ConditionPathIsDirectory") ? 'd' :
                        !strcmp(key, "ConditionFileIsExecutable") ? 'x' : !strcmp(key, "ConditionFileNotEmpty") ? 's' :
                        !strcmp(key, "ConditionACPower") ? 'a' : !strcmp(key, "ConditionVirtualization") ? 'v' : 0;
            if (reset) { u->nconds = 0; return; }
            if (kind && u->nconds < 8) { struct condition* c = &u->conds[u->nconds++]; c->kind = kind; c->negate = value[0] == '!';
                copy_str(c->path, sizeof(c->path), value + c->negate); return; }
        }
        if (!strcmp(key, "Documentation") || !strcmp(key, "BindsTo") || !strcmp(key, "PartOf")) return;
    } else if (!strcmp(section, "Service")) {
        if (!strcmp(key, "Type")) {
            for (int t = 0; t < 5; t++) if (!strcmp(value, type_names[t])) { u->type = (enum utype)t; return; }
            if (!strcmp(value, "dbus") || !strcmp(value, "idle")) { u->type = T_SIMPLE; return; }
        }
        LIST("ExecStartPre", pre) LIST("ExecStart", start) LIST("ExecStartPost", post) LIST("ExecStop", stop) LIST("ExecReload", reload)
        LIST("Environment", env) LIST("EnvironmentFile", env_files) WORDS("SupplementaryGroups", supp)
        WORDS("RuntimeDirectory", runtime_dirs) WORDS("StateDirectory", state_dirs) WORDS("LogsDirectory", logs_dirs) WORDS("CacheDirectory", cache_dirs)
        if (!strcmp(key, "User")) { copy_str(u->user, sizeof(u->user), value); return; }
        if (!strcmp(key, "Group")) { copy_str(u->group, sizeof(u->group), value); return; }
        if (!strcmp(key, "WorkingDirectory")) { copy_str(u->workdir, sizeof(u->workdir), value); return; }
        if (!strcmp(key, "UMask")) { u->umask_value = (mode_t)strtol(value, NULL, 8) & 0777; u->umask_set = true; return; }
        if (!strcmp(key, "RuntimeDirectoryMode")) { u->runtime_mode = (mode_t)strtol(value, NULL, 8) & 07777; return; }
        if (!strcmp(key, "Restart")) {
            static const char* const r[] = {"no", "on-success", "on-failure", "on-abnormal", "always"};
            for (int i = 0; i < 5; i++) if (!strcmp(value, r[i])) { u->restart = (enum restart)i; return; }
            if (!strcmp(value, "on-abort") || !strcmp(value, "on-watchdog")) { u->restart = R_ON_ABNORMAL; return; }
        }
        if (!strcmp(key, "RestartSec") || !strcmp(key, "RestartMaxDelaySec") || !strcmp(key, "TimeoutStartSec") || !strcmp(key, "TimeoutStopSec") ||
            !strcmp(key, "TimeoutSec") || !strcmp(key, "StartLimitIntervalSec")) {
            long long ms = parse_time_ms(value);
            if (ms >= -1) {
                uint64_t v = ms < 0 || (ms == 0 && strncmp(key, "Restart", 7)) ? INFINITE_MS : (uint64_t)ms;
                if (!strcmp(key, "RestartSec")) u->restart_ms = ms < 0 ? 0 : (uint64_t)ms;
                else if (!strcmp(key, "RestartMaxDelaySec")) u->restart_max_ms = v;
                else if (!strcmp(key, "TimeoutStartSec")) u->timeout_start_ms = v;
                else if (!strcmp(key, "TimeoutStopSec")) u->timeout_stop_ms = v;
                else if (!strcmp(key, "TimeoutSec")) u->timeout_start_ms = u->timeout_stop_ms = v;
                else u->limit_interval_ms = v;
                return;
            }
        }
        if (!strcmp(key, "StartLimitBurst")) { u->limit_burst = (unsigned)atoi(value); return; }
        if (!strcmp(key, "PIDFile")) { copy_str(u->pidfile, sizeof(u->pidfile), value); return; }
        if (!strcmp(key, "RemainAfterExit")) { u->remain = truthy(value); return; }
        if (!strcmp(key, "ReadyPath")) { copy_str(u->ready_path, sizeof(u->ready_path), value); return; }
        if (!strcmp(key, "SyslogIdentifier")) { copy_str(u->ident, sizeof(u->ident), value); return; }
        if (!strcmp(key, "KillSignal")) { u->kill_signal = signal_number(value); return; }
        if (!strcmp(key, "KillMode")) { u->kill_group = strcmp(value, "process") != 0 && strcmp(value, "none") != 0; return; }
        if (!strcmp(key, "StandardOutput") || !strcmp(key, "StandardError")) {
            u->output = !strcmp(value, "null") ? 1 : !strncmp(value, "journal+console", 15) || !strcmp(value, "console") || !strcmp(value, "tty") ? 2 : 0;
            u->output_explicit = strcmp(value, "socket") && strcmp(value, "inherit"); return; }
        if (!strcmp(key, "LimitNOFILE")) { u->nofile = !strcmp(value, "infinity") ? (long long)RLIM_INFINITY : atoll(value); return; }
        if (!strcmp(key, "Nice")) { u->nice = atoi(value); u->nice_set = true; return; }
        if (!strcmp(key, "AllowUserInstance")) { u->allow_user = truthy(value); return; }
        if (!strcmp(key, "PrivateTmp") || !strcmp(key, "ProtectSystem") || !strcmp(key, "ProtectHome") || !strcmp(key, "NoNewPrivileges") ||
            !strcmp(key, "BusName") || !strcmp(key, "NotifyAccess") || !strcmp(key, "OOMScoreAdjust") || !strcmp(key, "SuccessExitStatus")) {
            if (!strcmp(key, "NoNewPrivileges") && truthy(value)) { /* honoured at exec time below */ }
            return; }
    } else if (!strcmp(section, "Socket") || !strcmp(section, "Timer") || !strcmp(section, "Path")) {
        if (u->kind == K_SERVICE) { if (!u->systemd) mlog(4, "%s:%u: [%s] section in a service ignored", file, lineno, section); return; }
    } else if (!strcmp(section, "Install")) {
        WORDS("WantedBy", wanted_by)
        if (!strcmp(key, "DefaultEnabled")) { u->default_enabled = truthy(value); return; }
        if (!strcmp(key, "Alias") || !strcmp(key, "Also") || !strcmp(key, "RequiredBy")) return;
    }
    if (!u->systemd) mlog(4, "%s:%u: unknown or invalid setting %s.%s ignored", file, lineno, section, key);
#undef LIST
#undef WORDS
}
static bool parse_file(struct unit* u, const char* path) {
    FILE* f = fopen(path, "re"); if (!f) return false;
    char section[32] = "", line[LINE_MAX_BYTES], joined[LINE_MAX_BYTES]; unsigned lineno = 0; joined[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        bool cont = n && line[n - 1] == '\\'; if (cont) line[--n] = 0;
        if (strlen(joined) + n + 2 >= sizeof(joined)) { joined[0] = 0; continue; }
        strcat(joined, line); if (cont) { strcat(joined, " "); continue; }
        char* s = trim(joined);
        if (*s && *s != '#' && *s != ';') {
            if (*s == '[') { char* e = strchr(s, ']'); if (e) { *e = 0; copy_str(section, sizeof(section), s + 1); } }
            else { char* eq = strchr(s, '='); if (eq) { *eq = 0; set_key(u, section, trim(s), trim(eq + 1), path, lineno); } }
        }
        joined[0] = 0;
    }
    fclose(f); return true;
}
static int name_cmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static void parse_dropins(struct unit* u, const char* dir_base, const char* name) {
    char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s/%s.d", dir_base, name);
    DIR* d = opendir(dir); if (!d) return;
    struct strlist files = {0}; struct dirent* e;
    while ((e = readdir(d))) if (ends_with(e->d_name, ".conf") && e->d_name[0] != '.') sl_add(&files, e->d_name);
    closedir(d);
    if (files.n) qsort(files.v, files.n, sizeof(char*), name_cmp);
    for (unsigned i = 0; i < files.n; i++) { char p[PATH_MAX]; snprintf(p, sizeof(p), "%s/%s", dir, files.v[i]); parse_file(u, p); }
    sl_clear(&files);
}
#define UNIT_DIRS unit_dir_count
#define NEXUS_UNIT_DIRS nexus_unit_dirs
/* drop-ins of the NexusOS directories, lowest priority first (the admin's win) */
static void parse_nexus_dropins(struct unit* u, const char* name) {
    for (unsigned i = NEXUS_UNIT_DIRS; i-- > 0;) parse_dropins(u, unit_dirs[i], name);
}
/* Locate the unit file (admin dir wins); templates for instances. *from: index into unit_dirs. */
static bool unit_source(const char* name, char* path, size_t size, char* tpl_name, size_t tsize, unsigned* from) {
    for (unsigned i = 0; i < UNIT_DIRS; i++) {
        snprintf(path, size, "%s/%s", unit_dirs[i], name);
        if (!access(path, R_OK)) { tpl_name[0] = 0; if (from) *from = i; return true; }
    }
    const char* at = strchr(name, '@');
    if (at && at[1] != '.') {
        const char* dot = strrchr(name, '.');
        snprintf(tpl_name, tsize, "%.*s@%s", (int)(at - name), name, dot ? dot : ".service");
        for (unsigned i = 0; i < UNIT_DIRS; i++) {
            snprintf(path, size, "%s/%s", unit_dirs[i], tpl_name);
            if (!access(path, R_OK)) { if (from) *from = i; return true; }
        }
    }
    return false;
}
static struct actpart* act_new(void);
static void act_finish(struct unit* u);
static struct unit* load_unit(const char* name) {
    struct unit* u = find_unit(name); if (u) return u;
    if (!valid_unit_name(name) || nunits >= MAX_UNITS) return NULL;
    char path[PATH_MAX], tpl[160]; unsigned from = 0;
    if (!unit_source(name, path, sizeof(path), tpl, sizeof(tpl), &from)) return NULL;
    { char target[16]; ssize_t n = readlink(path, target, sizeof(target) - 1);   /* masked: a link to /dev/null (systemctl mask) */
      if (n == 9 && !memcmp(target, "/dev/null", 9)) return NULL; }
    /* a user manager leaves the session bus to NexusOS's dbus-session@UID (a user dbus.socket would replace its socket) */
    if (user_mode && (!strcmp(name, "dbus.socket") || !strcmp(name, "dbus.service") || !strcmp(name, "dbus-broker.service"))) return NULL;
    u = xcalloc(1, sizeof(*u)); unit_defaults(u); u->systemd = from >= NEXUS_UNIT_DIRS;
    copy_str(u->name, sizeof(u->name), name); copy_str(u->file, sizeof(u->file), path);
    size_t suffix = unit_suffix_len(name);
    u->kind = suffix == 7 ? K_SOCKET : suffix == 6 ? K_TIMER : suffix == 5 ? K_PATH : K_SERVICE;
    if (u->kind != K_SERVICE) u->act = act_new();
    const char* at = strchr(name, '@');
    u->is_template = at && (size_t)(name + strlen(name) - at) == suffix + 1;
    if (at && !u->is_template) snprintf(u->instance, sizeof(u->instance), "%.*s", (int)(strlen(at + 1) - suffix), at + 1);
    if (!parse_file(u, path)) { free(u->act); free(u); return NULL; }
    const char* base = tpl[0] ? tpl : name;
    parse_nexus_dropins(u, base);
    if (tpl[0]) parse_dropins(u, unit_dirs[0], name);
    for (unsigned i = 0; i < UNIT_DIRS; i++)   /* NAME.wants/ and NAME.requires/ links (Wants=/Requires= added by packages) */
        for (unsigned r = 0; r < 2; r++) {
            char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s/%s.%s", unit_dirs[i], name, r ? "requires" : "wants");
            DIR* d = opendir(dir); if (!d) continue; struct dirent* e;
            while ((e = readdir(d))) if (valid_unit_name(e->d_name) && !sl_has(r ? &u->requires : &u->wants, e->d_name)) sl_add(r ? &u->requires : &u->wants, e->d_name);
            closedir(d);
        }
    act_finish(u);
    u->loaded = true; units[nunits++] = u; return u;
}
static void scan_units(void) {
    for (unsigned i = 0; i < UNIT_DIRS; i++) {
        DIR* d = opendir(unit_dirs[i]); if (!d) continue; struct dirent* e;
        while ((e = readdir(d))) if (valid_unit_name(e->d_name) && !find_unit(e->d_name)) load_unit(e->d_name);
        closedir(d);
    }
    /* Before= is After= seen from the other side */
    for (unsigned i = 0; i < nunits; i++)
        for (unsigned j = 0; j < units[i]->before.n; j++) { struct unit* v = find_unit(units[i]->before.v[j]); if (v && !sl_has(&v->after, units[i]->name)) sl_add(&v->after, units[i]->name); }
}
/* enabled by a Debian package's postinst (deb-systemd-helper links, also without systemd) or shipped enabled: a
 * TARGET.target.wants/ link in a systemd directory - the system's /etc/systemd/system and /usr/lib/systemd/system, a
 * user manager's ~/.config/systemd/user, /etc/systemd/user (deb-systemd-helper --user) and /usr/lib/systemd/user */
static bool wants_link(const struct unit* u) {
    static const char* const targets[] = {"multi-user", "graphical", "default", "sockets", "paths", "timers", "basic"};
    char p[PATH_MAX]; struct stat st;
    for (unsigned d = NEXUS_UNIT_DIRS; d < UNIT_DIRS; d++)
        for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
            snprintf(p, sizeof(p), "%s/%s.target.wants/%s", unit_dirs[d], targets[i], u->name); if (!lstat(p, &st)) return true; }
    return false;
}
/* Debian's timers too (apt-daily, man-db, logrotate, dpkg-db-backup, e2scrub_all, fstrim): their cron.daily twins
 * also run under cron (they look for /run/systemd/system, which NexusOS does not have), which is harmless: each job
 * keeps its own once-per-period stamp */
static bool enabled_by_default(const struct unit* u) { return u->default_enabled || wants_link(u); }
static bool unit_enabled(const struct unit* u) {
    char p[PATH_MAX]; struct stat st;
    snprintf(p, sizeof(p), "%s/%s", enabled_dir, u->name); if (!lstat(p, &st)) return true;
    snprintf(p, sizeof(p), "%s/%s", disabled_dir, u->name); if (!lstat(p, &st)) return false;
    return enabled_by_default(u);
}
/* on mains power, as systemd decides: an AC/USB supply online, or no AC supply at all (a desktop, a VM) */
static bool on_ac_power(void) {
    DIR* d = opendir("/sys/class/power_supply"); if (!d) return true;
    bool found = false, online = false; struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX], v[32] = ""; snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type", e->d_name);
        FILE* f = fopen(p, "re"); if (!f) continue; if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f);
        if (strncmp(v, "Mains", 5) && strncmp(v, "USB", 3)) continue;
        found = true; snprintf(p, sizeof(p), "/sys/class/power_supply/%s/online", e->d_name);
        f = fopen(p, "re"); if (f) { if (fgets(v, sizeof(v), f) && v[0] == '1') online = true; fclose(f); }
    }
    closedir(d); return !found || online;
}
/* ConditionVirtualization=: NexusOS is never a container; a VM shows the CPU's hypervisor flag */
static bool virtualization_matches(const char* what) {
    char line[4096]; bool vm = false; FILE* f = fopen("/proc/cpuinfo", "re");
    if (f) { while (fgets(line, sizeof(line), f)) if (!strncmp(line, "flags", 5)) { vm = strstr(line, " hypervisor") != NULL; break; } fclose(f); }
    if (!strcmp(what, "container") || !strcmp(what, "docker") || !strcmp(what, "lxc") || !strcmp(what, "podman") || !strcmp(what, "systemd-nspawn") || !strcmp(what, "wsl")) return false;
    if (!strcmp(what, "no") || !strcmp(what, "false") || !strcmp(what, "0")) return !vm;
    return vm;   /* yes, vm, kvm, qemu, vmware, microsoft, oracle, ...: any hypervisor */
}
static bool conditions_met(struct unit* u) {
    for (unsigned i = 0; i < u->nconds; i++) {
        struct condition* c = &u->conds[i];
        if (c->kind == 'a' || c->kind == 'v') {
            bool ok = c->kind == 'a' ? on_ac_power() == truthy(c->path) : virtualization_matches(c->path);
            if (ok == c->negate) return false;
            continue;
        }
        struct stat st; bool ok = !stat(c->path, &st);
        if (ok && c->kind == 'd') ok = S_ISDIR(st.st_mode);
        if (ok && c->kind == 'x') ok = S_ISREG(st.st_mode) && !access(c->path, X_OK);
        if (ok && c->kind == 's') ok = S_ISREG(st.st_mode) && st.st_size > 0;
        if (ok == c->negate) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ process control */
struct identity { bool set; uid_t uid; gid_t gid; char name[64], home[PATH_MAX], shell[PATH_MAX]; gid_t groups[64]; int ngroups; };
static int resolve_identity(struct unit* u, struct identity* id, char* why, size_t wsize) {
    memset(id, 0, sizeof(*id));
    if (!u->user[0] && !u->group[0]) {
        struct passwd* pw = getpwuid(geteuid());
        copy_str(id->name, sizeof(id->name), pw ? pw->pw_name : "root"); copy_str(id->home, sizeof(id->home), pw ? pw->pw_dir : "/root");
        copy_str(id->shell, sizeof(id->shell), pw && pw->pw_shell ? pw->pw_shell : "/bin/sh"); id->uid = geteuid(); id->gid = getegid();
        return 0;
    }
    struct passwd* pw = NULL;
    if (u->user[0]) {
        pw = isdigit((unsigned char)u->user[0]) ? getpwuid((uid_t)strtoul(u->user, NULL, 10)) : getpwnam(u->user);
        if (!pw) { snprintf(why, wsize, "unknown user %s", u->user); return -1; }
        id->uid = pw->pw_uid; id->gid = pw->pw_gid;
        copy_str(id->name, sizeof(id->name), pw->pw_name); copy_str(id->home, sizeof(id->home), pw->pw_dir);
        copy_str(id->shell, sizeof(id->shell), pw->pw_shell ? pw->pw_shell : "/bin/sh");
    } else { id->uid = geteuid(); id->gid = getegid(); copy_str(id->name, sizeof(id->name), "root"); copy_str(id->home, sizeof(id->home), "/root"); copy_str(id->shell, sizeof(id->shell), "/bin/sh"); }
    if (u->group[0]) {
        struct group* gr = isdigit((unsigned char)u->group[0]) ? getgrgid((gid_t)strtoul(u->group, NULL, 10)) : getgrnam(u->group);
        if (!gr) { snprintf(why, wsize, "unknown group %s", u->group); return -1; }
        id->gid = gr->gr_gid;
    }
    id->ngroups = 64;
    if (pw) { if (getgrouplist(pw->pw_name, id->gid, id->groups, &id->ngroups) < 0) id->ngroups = 0; }
    else { id->groups[0] = id->gid; id->ngroups = 1; }
    for (unsigned i = 0; i < u->supp.n && id->ngroups < 64; i++) {
        struct group* gr = getgrnam(u->supp.v[i]); if (gr) id->groups[id->ngroups++] = gr->gr_gid;
    }
    id->set = true;
    if (!root_mode && (id->uid != geteuid() || id->gid != getegid())) { snprintf(why, wsize, "needs the service manager to run as root"); return -1; }
    return 0;
}
/* Environment of a service: base + EnvironmentFile + Environment */
struct envset { char* v[256]; unsigned n; };
static void env_set(struct envset* e, const char* assignment) {
    const char* eq = strchr(assignment, '='); if (!eq || eq == assignment) return;
    size_t k = (size_t)(eq - assignment);
    for (unsigned i = 0; i < e->n; i++) if (!strncmp(e->v[i], assignment, k) && e->v[i][k] == '=') { free(e->v[i]); e->v[i] = xstrdup(assignment); return; }
    if (e->n < 255) { e->v[e->n++] = xstrdup(assignment); e->v[e->n] = NULL; }
}
static const char* env_get(const struct envset* e, const char* key) {
    size_t k = strlen(key);
    for (unsigned i = 0; i < e->n; i++) if (!strncmp(e->v[i], key, k) && e->v[i][k] == '=') return e->v[i] + k + 1;
    return NULL;
}
static void env_free(struct envset* e) { for (unsigned i = 0; i < e->n; i++) free(e->v[i]); e->n = 0; }
/* systemd-style word splitting: quotes, backslash escapes */
static unsigned split_words(const char* in, char out[][WORD_BYTES], unsigned max) {
    unsigned n = 0; const char* p = in;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t k = 0; char quote = 0;
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            if (!quote && (*p == '"' || *p == '\'')) { quote = *p++; continue; }
            if (quote && *p == quote) { quote = 0; p++; continue; }
            if (*p == '\\' && p[1] && quote != '\'') p++;
            if (k + 1 < WORD_BYTES) out[n][k++] = *p;
            p++;
        }
        out[n][k] = 0; n++;
    }
    return n;
}
static void load_env_file(struct envset* e, const char* spec, bool* failed) {
    bool optional = spec[0] == '-'; const char* path = spec + optional;
    FILE* f = fopen(path, "re"); if (!f) { if (!optional) *failed = true; return; }
    char line[LINE_MAX_BYTES];
    while (fgets(line, sizeof(line), f)) {
        char* s = trim(line); if (!*s || *s == '#' || *s == ';') continue;
        if (!strncmp(s, "export ", 7)) s = trim(s + 7);
        char* eq = strchr(s, '='); if (!eq) continue; *eq = 0;
        char* v = trim(eq + 1); size_t vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }
        char assignment[LINE_MAX_BYTES]; snprintf(assignment, sizeof(assignment), "%s=%s", trim(s), v); env_set(e, assignment);
    }
    fclose(f);
}
static void build_env(struct unit* u, const struct identity* id, struct envset* e, bool* failed) {
    env_set(e, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    FILE* f = fopen("/etc/default/locale", "re"); bool lang = false;
    if (f) { char line[256]; while (fgets(line, sizeof(line), f)) { char* s = trim(line); if (!strncmp(s, "LANG=", 5) || !strncmp(s, "LC_", 3)) { char* q = s; size_t l = strlen(q);
                if (l > 6 && q[5] == '"' && q[l - 1] == '"') { memmove(q + 5, q + 6, l - 6); q[l - 2] = 0; } env_set(e, q); lang |= !strncmp(s, "LANG=", 5); } } fclose(f); }
    if (!lang) env_set(e, "LANG=C.UTF-8");
    char buf[PATH_MAX + 16];
    snprintf(buf, sizeof(buf), "HOME=%s", id->home); env_set(e, buf);
    snprintf(buf, sizeof(buf), "USER=%s", id->name); env_set(e, buf);
    snprintf(buf, sizeof(buf), "LOGNAME=%s", id->name); env_set(e, buf);
    snprintf(buf, sizeof(buf), "SHELL=%s", id->shell); env_set(e, buf);
    snprintf(buf, sizeof(buf), "INVOCATION_ID=%016llx%s", (unsigned long long)real_us(), journal.boot_id); env_set(e, buf);
    if (id->uid) { snprintf(buf, sizeof(buf), "XDG_RUNTIME_DIR=/run/user/%u", (unsigned)id->uid); env_set(e, buf);
        if (user_mode) { snprintf(buf, sizeof(buf), "/run/user/%u/bus", (unsigned)id->uid); struct stat st;
            if (!stat(buf, &st) && S_ISSOCK(st.st_mode)) { snprintf(buf, sizeof(buf), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus", (unsigned)id->uid); env_set(e, buf); } } }
    if (u->notify_path[0]) { snprintf(buf, sizeof(buf), "NOTIFY_SOCKET=%s", u->notify_path); env_set(e, buf); }
    if (u->main_pid > 0) { snprintf(buf, sizeof(buf), "MAINPID=%ld", (long)u->main_pid); env_set(e, buf); }   /* ExecReload=kill -HUP $MAINPID */
    for (unsigned i = 0; i < u->env_files.n; i++) load_env_file(e, u->env_files.v[i], failed);
    static char words[32][WORD_BYTES];
    for (unsigned i = 0; i < u->env.n; i++) {
        unsigned n = split_words(u->env.v[i], words, 32);
        for (unsigned k = 0; k < n; k++) env_set(e, words[k]);
    }
}
/* Expand $VAR / ${VAR}: a lone "$VAR" word becomes several words (split on whitespace). */
static unsigned expand_command(const char* line, const struct envset* e, bool expand, char argv[][WORD_BYTES], unsigned max) {
    static char words[64][WORD_BYTES], tmp[32][WORD_BYTES];
    unsigned n = split_words(line, words, 64), out = 0;
    for (unsigned i = 0; i < n && out < max; i++) {
        const char* w = words[i];
        if (expand && w[0] == '$' && w[1] != '{' && w[1]) {
            bool plain = true; for (const char* p = w + 1; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_')) plain = false;
            if (plain) { const char* v = env_get(e, w + 1); if (v) { unsigned m = split_words(v, tmp, 32);
                    for (unsigned k = 0; k < m && out < max; k++) copy_str(argv[out++], WORD_BYTES, tmp[k]); } continue; }
        }
        size_t k = 0; char* o = argv[out];
        for (const char* p = w; *p && k + 1 < WORD_BYTES; p++) {
            if (expand && p[0] == '$' && p[1] == '{') {
                const char* end = strchr(p + 2, '}');
                if (end) { char key[128]; snprintf(key, sizeof(key), "%.*s", (int)(end - p - 2), p + 2); const char* v = env_get(e, key);
                    for (const char* q = v ? v : ""; *q && k + 1 < WORD_BYTES; q++) o[k++] = *q;
                    p = end; continue; }
            }
            o[k++] = *p;
        }
        o[k] = 0; out++;
    }
    return out;
}
static bool find_program(const char* name, char* out, size_t size) {
    if (name[0] == '/') return copy_str(out, size, name) && !access(out, X_OK);   /* a missing program fails the start now */
    const char* dirs[] = {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin"};
    for (unsigned i = 0; i < 4; i++) { snprintf(out, size, "%s/%s", dirs[i], name); if (!access(out, X_OK)) return true; }
    return false;
}
static void make_dirs(struct unit* u, const struct identity* id) {
    if (id->uid && root_mode) {   /* the user's runtime directory (logind's job on other systems) */
        char run[64]; snprintf(run, sizeof(run), "/run/user/%u", (unsigned)id->uid); struct stat st;
        if (lstat(run, &st) && !mkdir(run, 0700)) { if (chown(run, id->uid, id->gid)) mlog(4, "chown %s: %s", run, strerror(errno)); }
    }
    const struct { struct strlist* l; const char* base; mode_t mode; } sets[] = {
        {&u->runtime_dirs, "/run", u->runtime_mode}, {&u->state_dirs, "/var/lib", 0755}, {&u->logs_dirs, "/var/log", 0755}, {&u->cache_dirs, "/var/cache", 0755}};
    for (unsigned s = 0; s < 4; s++)
        for (unsigned i = 0; i < sets[s].l->n; i++) {
            const char* rel = sets[s].l->v[i]; if (rel[0] == '/' || strstr(rel, "..")) continue;
            char p[PATH_MAX]; snprintf(p, sizeof(p), "%s/%s", sets[s].base, rel);
            if (mkdir_p(p, sets[s].mode) && root_mode) { if (lchown(p, id->uid, id->gid)) {} chmod(p, sets[s].mode); }
        }
}
static unsigned service_fds(struct unit* s, int* fds, char* names, size_t nsize, unsigned max);
/* Fork + exec one command line of u. Returns the pid, or -1 (u->result set). */
static pid_t spawn(struct unit* u, const char* line, bool main_command) {
    const char* cmd = line; bool privileged = false, set_argv0 = false, noexpand = false;
    for (;; cmd++) {
        if (*cmd == '-') continue; if (*cmd == '@') { set_argv0 = true; continue; } if (*cmd == '+' || *cmd == '!') { privileged = true; continue; }
        if (*cmd == ':') { noexpand = true; continue; } break;
    }
    struct identity id; char why[160];
    if (privileged) { memset(&id, 0, sizeof(id)); id.uid = geteuid(); id.gid = getegid(); copy_str(id.name, sizeof(id.name), "root"); copy_str(id.home, sizeof(id.home), "/root"); copy_str(id.shell, sizeof(id.shell), "/bin/sh"); }
    else if (resolve_identity(u, &id, why, sizeof(why))) { copy_str(u->result, sizeof(u->result), "resources"); mlog(3, "%s: %s", u->name, why); return -1; }
    struct envset env = {0}; bool env_failed = false; build_env(u, &id, &env, &env_failed);
    if (env_failed) { env_free(&env); copy_str(u->result, sizeof(u->result), "resources"); mlog(3, "%s: an EnvironmentFile is missing", u->name); return -1; }
    static char argv_buf[64][WORD_BYTES]; unsigned argc = expand_command(cmd, &env, !noexpand, argv_buf, 63);
    if (!argc) { env_free(&env); copy_str(u->result, sizeof(u->result), "exec-condition"); mlog(3, "%s: empty command", u->name); return -1; }
    char program[PATH_MAX];
    if (!find_program(argv_buf[0], program, sizeof(program))) { env_free(&env); copy_str(u->result, sizeof(u->result), "exit-code"); mlog(3, "%s: %s not found", u->name, argv_buf[0]); return -1; }
    char* argv[65]; unsigned first = set_argv0 ? 1 : 0;
    for (unsigned i = 0; i + first < argc; i++) argv[i] = argv_buf[i + first];
    argv[argc - first] = NULL;
    if (set_argv0 && argc < 2) { env_free(&env); return -1; }
    if (main_command || u->phase == P_PRE) make_dirs(u, &id);
    if (u->output == 0 && u->out_fd < 0) {   /* one pipe per unit run: nexusd keeps the write end until the unit ends */
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC)) { env_free(&env); copy_str(u->result, sizeof(u->result), "resources"); return -1; }
        u->out_fd = pipefd[0]; u->out_wr = pipefd[1]; fcntl(u->out_fd, F_SETFL, O_NONBLOCK);
    }
    /* socket activation (sd_listen_fds): the main process gets its sockets as fds 3.. */
    int pass[16]; char names[1024] = ""; unsigned npass = main_command ? service_fds(u, pass, names, sizeof(names), 16) : 0;
    if (npass) { char b[64 + sizeof(names)]; snprintf(b, sizeof(b), "LISTEN_FDS=%u", npass); env_set(&env, b);
        snprintf(b, sizeof(b), "LISTEN_FDNAMES=%s", names); env_set(&env, b); }
    pid_t pid = fork();
    if (pid < 0) { env_free(&env); copy_str(u->result, sizeof(u->result), "resources"); return -1; }
    if (!pid) {
        sigset_t all; sigemptyset(&all); sigprocmask(SIG_SETMASK, &all, NULL);
        for (int s = 1; s < 65; s++) signal(s, SIG_DFL);
        setsid();
        int out = u->output == 1 ? devnull : u->output == 2 ? 1 : u->out_wr >= 0 ? u->out_wr : devnull;
        if (npass && u->stdin_socket) {   /* inetd style: the connection is stdin/stdout (and stderr unless set) */
            dup2(pass[0], 0); dup2(pass[0], 1); if (u->output_explicit) dup2(out, 2); else dup2(pass[0], 2);
        } else { dup2(devnull, 0); if (out != 1) dup2(out, 1); dup2(1, 2); }
        if (npass) {
            int moved[16];
            for (unsigned i = 0; i < npass; i++) moved[i] = fcntl(pass[i], F_DUPFD, (int)(3 + npass));
            for (unsigned i = 0; i < npass; i++) if (moved[i] < 0 || dup2(moved[i], (int)(3 + i)) < 0) _exit(214);
            char b[32]; snprintf(b, sizeof(b), "LISTEN_PID=%ld", (long)getpid()); env_set(&env, b);
        }
        if (syscall(436, 3 + npass, ~0U, 0)) for (int fd = (int)(3 + npass); fd < 4096; fd++) close(fd);   /* close_range */
        if (u->nofile >= 0) { struct rlimit r = {(rlim_t)u->nofile, (rlim_t)u->nofile}; setrlimit(RLIMIT_NOFILE, &r); }
        if (u->nice_set) { errno = 0; if (nice(u->nice) == -1 && errno) {} }
        umask(u->umask_value);
        if (id.set && root_mode) {
            if (setgroups((size_t)id.ngroups, id.groups) || setresgid(id.gid, id.gid, id.gid) || setresuid(id.uid, id.uid, id.uid)) {
                fprintf(stderr, "nexusd: cannot switch to %s: %s\n", id.name, strerror(errno)); _exit(216); }
        }
        const char* wd = u->workdir[0] ? u->workdir : "/"; bool wd_optional = wd[0] == '-'; if (wd_optional) wd++;
        if (!strcmp(wd, "~")) wd = id.home;
        if (chdir(wd) && (!wd_optional || chdir("/"))) { fprintf(stderr, "nexusd: cannot enter %s: %s\n", wd, strerror(errno)); _exit(200); }
        execve(program, argv, env.v);
        fprintf(stderr, "nexusd: cannot execute %s: %s\n", program, strerror(errno)); _exit(203);
    }
    env_free(&env);
    if (main_command && u->conn_fd >= 0) { close(u->conn_fd); u->conn_fd = -1; }   /* the connection now belongs to the service */
    return pid;
}

/* ------------------------------------------------------------------------------------------------ state machine */
static void act_start(struct unit* u);
static void act_stop(struct unit* u);
static bool recheck_paths;   /* a unit went inactive: path units re-check their conditions */
static void unit_enter(struct unit* u, enum ustate s, const char* result);
static void start_phase(struct unit* u, enum phase p);
static void kill_unit(struct unit* u, int sig) {
    if (u->main_pid > 0) { if (u->kill_group) kill(-u->main_pid, sig); kill(u->main_pid, sig); }
    if (u->control_pid > 0) { if (u->kill_group) kill(-u->control_pid, sig); kill(u->control_pid, sig); }
}
static void close_notify(struct unit* u) {
    if (u->notify_fd >= 0) { close(u->notify_fd); u->notify_fd = -1; }
    if (u->notify_path[0]) { unlink(u->notify_path); u->notify_path[0] = 0; }
}
static void flush_output(struct unit* u) {
    if (u->linelen) { journal_write(6, u->name, unit_ident(u), u->main_pid > 0 ? u->main_pid : u->control_pid, u->linebuf, u->linelen); u->linelen = 0; }
}
static void drain_output(struct unit* u, bool final) {
    if (u->out_fd < 0) return;
    for (;;) {
        char buf[4096]; ssize_t n = read(u->out_fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0 || final) { flush_output(u); close(u->out_fd); u->out_fd = -1; if (u->out_wr >= 0) { close(u->out_wr); u->out_wr = -1; } }
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || u->linelen == sizeof(u->linebuf) - 1) {
                int pri = 6; const char* text = u->linebuf; size_t len = u->linelen;
                if (len >= 3 && text[0] == '<' && text[1] >= '0' && text[1] <= '7' && text[2] == '>') { pri = text[1] - '0'; text += 3; len -= 3; }
                journal_write(pri, u->name, unit_ident(u), u->main_pid > 0 ? u->main_pid : u->control_pid, text, len);
                u->linelen = 0; if (buf[i] == '\n') continue;
            }
            u->linebuf[u->linelen++] = buf[i];
        }
    }
}
static bool setup_notify(struct unit* u) {
    close_notify(u);
    if (u->type != T_NOTIFY) return true;
    if (!mkdir_p(notify_dir, user_mode ? 0700 : 0755)) return false;
    snprintf(u->notify_path, sizeof(u->notify_path), "%s/%s", notify_dir, u->name);
    unlink(u->notify_path);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) { u->notify_path[0] = 0; return false; }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (!copy_str(a.sun_path, sizeof(a.sun_path), u->notify_path) || bind(fd, (struct sockaddr*)&a, sizeof(a))) { close(fd); u->notify_path[0] = 0; return false; }
    chmod(u->notify_path, 0666);
    u->notify_fd = fd; return true;
}
static void unit_active(struct unit* u) {
    if (u->state == S_ACTIVE) return;
    u->active_since_ms = now_ms(); unit_enter(u, S_ACTIVE, NULL);
    if (u->post.n) start_phase(u, P_POST);
}
static void read_pidfile(struct unit* u) {
    for (int tries = 0; tries < 20; tries++) {
        FILE* f = fopen(u->pidfile, "re");
        if (f) { long pid = 0; if (fscanf(f, "%ld", &pid) == 1 && pid > 1) { u->main_pid = (pid_t)pid; u->main_is_child = false; } fclose(f); if (u->main_pid > 0) return; }
        struct timespec t = {0, 50000000}; nanosleep(&t, NULL);
    }
}
/* run exec list entry `index` of phase p; on an empty list continue with the next phase */
static void phase_done(struct unit* u, bool ok);
static void start_phase(struct unit* u, enum phase p) {
    u->phase = p; u->exec_index = 0;
    struct strlist* l = p == P_PRE ? &u->pre : p == P_START ? &u->start : p == P_POST ? &u->post : p == P_STOP ? &u->stop : &u->reload;
    if (!l->n) { phase_done(u, true); return; }
    pid_t pid = spawn(u, l->v[0], p == P_START);
    if (pid < 0) { if (l->v[0][0] == '-') { phase_done(u, true); return; } phase_done(u, false); return; }
    if (p == P_START) {
        u->main_pid = pid; u->main_is_child = true;
        if ((u->type == T_SIMPLE || u->type == T_EXEC) && !u->ready_path[0]) unit_active(u);
    }
    else u->control_pid = pid;
}
static void next_command(struct unit* u) {
    struct strlist* l = u->phase == P_PRE ? &u->pre : u->phase == P_START ? &u->start : u->phase == P_POST ? &u->post : u->phase == P_STOP ? &u->stop : &u->reload;
    if (++u->exec_index >= l->n) { phase_done(u, true); return; }
    pid_t pid = spawn(u, l->v[u->exec_index], u->phase == P_START);
    if (pid < 0) { if (l->v[u->exec_index][0] == '-') { next_command(u); return; } phase_done(u, false); return; }
    if (u->phase == P_START) { u->main_pid = pid; u->main_is_child = true; } else u->control_pid = pid;
}
static void begin_stop(struct unit* u, const char* result);
static void phase_done(struct unit* u, bool ok) {
    enum phase p = u->phase; u->phase = P_NONE;
    if (!ok) {
        if (p == P_STOP) { kill_unit(u, u->kill_signal); u->deadline_ms = u->timeout_stop_ms == INFINITE_MS ? INFINITE_MS : now_ms() + u->timeout_stop_ms; return; }
        if (p == P_RELOAD) { mlog(4, "%s: reload failed", u->name); return; }
        if (!u->result[0]) copy_str(u->result, sizeof(u->result), "exit-code");
        begin_stop(u, u->result); return;
    }
    switch (p) {
    case P_PRE: start_phase(u, P_START); break;
    case P_START:
        if (u->type == T_ONESHOT) {   /* every ExecStart exited 0 */
            u->main_pid = 0;
            if (u->remain) unit_active(u);
            else if (u->post.n) { u->state = S_STARTING; start_phase(u, P_POST); }
            else { u->active_since_ms = now_ms(); unit_enter(u, S_INACTIVE, "success"); }
        }
        break;
    case P_POST:
        if (u->type == T_ONESHOT && !u->remain) unit_enter(u, S_INACTIVE, "success");
        break;
    case P_STOP:
        if (u->main_pid > 0 || u->control_pid > 0) { kill_unit(u, u->kill_signal); u->deadline_ms = u->timeout_stop_ms == INFINITE_MS ? INFINITE_MS : now_ms() + u->timeout_stop_ms; }
        else unit_enter(u, u->result[0] && strcmp(u->result, "success") ? S_FAILED : S_INACTIVE, NULL);
        break;
    default: break;
    }
}
static void begin_start(struct unit* u) {
    uint64_t t = now_ms();
    unsigned recent = 0; for (unsigned i = 0; i < u->nstarts; i++) if (u->limit_interval_ms == INFINITE_MS || t - u->starts[i] < u->limit_interval_ms) recent++;
    if (u->limit_burst && recent >= u->limit_burst) { u->wanted = false; unit_enter(u, S_FAILED, "start-limit-hit"); return; }
    if (u->nstarts == 32) { memmove(u->starts, u->starts + 1, 31 * sizeof(uint64_t)); u->nstarts = 31; }
    u->starts[u->nstarts++] = t;
    u->result[0] = 0; u->exit_code = -1; u->exit_signal = 0; u->status_text[0] = 0; u->condition_failed = false;
    if (!conditions_met(u)) { u->condition_failed = true; u->wanted = false; mlog(6, "%s: condition not met, skipped", u->name); unit_enter(u, S_INACTIVE, "condition"); return; }
    if (u->kind != K_SERVICE) { act_start(u); return; }
    if (!setup_notify(u)) { mlog(3, "%s: cannot create its notification socket: %s", u->name, strerror(errno)); unit_enter(u, S_FAILED, "resources"); return; }
    u->state = S_STARTING; u->deadline_ms = u->timeout_start_ms == INFINITE_MS ? INFINITE_MS : t + u->timeout_start_ms;
    u->active_enter_ms = t;   /* the activation time (OnUnitActiveSec= of timers; a oneshot service is never "active") */
    u->ready_poll_ms = 0;
    mlog(6, "Starting %s%s%s...", u->name, u->description[0] ? " - " : "", u->description);
    start_phase(u, P_PRE);
}
static void begin_stop(struct unit* u, const char* result) {
    if (result) copy_str(u->result, sizeof(u->result), result);
    if (u->state == S_INACTIVE || u->state == S_FAILED) return;
    if (u->state == S_WAITING || u->state == S_RESTART_WAIT) { unit_enter(u, S_INACTIVE, NULL); return; }
    if (u->state == S_STOPPING) return;
    if (u->kind != K_SERVICE) { mlog(6, "Stopping %s...", u->name); act_stop(u); return; }
    u->state = S_STOPPING; mlog(6, "Stopping %s...", u->name);
    if (u->control_pid > 0) { kill(u->control_pid, SIGTERM); }
    if (u->stop.n && (u->main_pid > 0 || (u->type == T_ONESHOT && u->remain)) && u->state != S_FAILED) start_phase(u, P_STOP);
    else { u->phase = P_NONE; if (u->main_pid > 0 || u->control_pid > 0) kill_unit(u, u->kill_signal); else { unit_enter(u, u->result[0] && strcmp(u->result, "success") ? S_FAILED : S_INACTIVE, NULL); return; } }
    if (u->state == S_STOPPING) u->deadline_ms = u->timeout_stop_ms == INFINITE_MS ? INFINITE_MS : now_ms() + u->timeout_stop_ms;
}
static uint64_t backoff(struct unit* u) {
    uint64_t d = u->restart_ms ? u->restart_ms : 100;
    for (unsigned i = 1; i < u->restarts && d < u->restart_max_ms; i++) d *= 2;
    return d > u->restart_max_ms ? u->restart_max_ms : d;
}
static void unit_enter(struct unit* u, enum ustate s, const char* result) {
    enum ustate old = u->state; if (result) copy_str(u->result, sizeof(u->result), result);
    u->state = s;
    if ((s == S_INACTIVE || s == S_FAILED) && old != S_INACTIVE && old != S_FAILED) { u->inactive_enter_ms = now_ms(); recheck_paths = true; }
    if (s == S_INACTIVE || s == S_FAILED) {
        close_notify(u); drain_output(u, true); u->main_pid = 0; u->control_pid = 0; u->phase = P_NONE; u->deadline_ms = INFINITE_MS;
        bool success = !strcmp(u->result, "success") || !strcmp(u->result, "condition") || !u->result[0];
        bool abnormal = !strcmp(u->result, "signal") || !strcmp(u->result, "timeout") || !strcmp(u->result, "core-dump");
        /* a `restart` request: the stop it began has finished */
        bool requested = u->restart_requested && !u->stop_wanted && !shutting_down;
        u->restart_requested = false;
        bool want_restart = u->kind == K_SERVICE && !u->transient && !requested && u->wanted && !u->stop_wanted && !shutting_down && old != S_WAITING &&
            strcmp(u->result, "start-limit-hit") && strcmp(u->result, "dependency") && strcmp(u->result, "condition") &&
            (u->restart == R_ALWAYS || (u->restart == R_ON_FAILURE && !success) || (u->restart == R_ON_SUCCESS && success) || (u->restart == R_ON_ABNORMAL && abnormal));
        if (!strcmp(u->name, "nexus-firstboot.service") && s == S_INACTIVE) journal_owner();   /* group adm exists now */
        if (s == S_FAILED) mlog(3, "%s failed (result: %s%s)", u->name, u->result[0] ? u->result : "unknown", want_restart ? ", will restart" : "");
        else if (old == S_STOPPING) mlog(6, "Stopped %s.", u->name);
        else if (old != S_INACTIVE && strcmp(u->result, "condition")) mlog(6, "%s finished (result: %s)", u->name, u->result[0] ? u->result : "success");
        if (requested) { u->state = S_RESTART_WAIT; u->restart_at_ms = now_ms(); }
        else if (want_restart) {
            if (u->active_since_ms && now_ms() - u->active_since_ms > u->restart_max_ms) u->restarts = 0;   /* it ran fine for a while */
            u->restarts++; u->state = S_RESTART_WAIT; u->restart_at_ms = now_ms() + backoff(u);
            mlog(5, "%s: scheduled restart %u in %llu ms", u->name, u->restarts, (unsigned long long)(u->restart_at_ms - now_ms()));
        } else u->wanted = false;
        u->active_since_ms = 0;
        if (u->transient && u->state != S_RESTART_WAIT) {   /* an Accept=yes instance: removed once it ended */
            if (u->conn_fd >= 0) { close(u->conn_fd); u->conn_fd = -1; }
            struct unit* by = u->accepted_by[0] ? find_unit(u->accepted_by) : NULL;
            if (by && by->act && by->act->connections && !u->gc) by->act->connections--;
            u->gc = true;
        }
    } else if (s == S_ACTIVE) mlog(6, "Started %s.", u->name);
}
static struct unit* unit_by_pid(pid_t pid, bool* main) {
    for (unsigned i = 0; i < nunits; i++) {
        if (units[i]->main_pid == pid) { *main = true; return units[i]; }
        if (units[i]->control_pid == pid) { *main = false; return units[i]; }
    }
    return NULL;
}
static void child_exited(pid_t pid, int status) {
    bool main; struct unit* u = unit_by_pid(pid, &main); if (!u) return;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (WIFEXITED(status)) u->exit_code = WEXITSTATUS(status); else if (WIFSIGNALED(status)) u->exit_signal = WTERMSIG(status);
    const char* failure = WIFSIGNALED(status) ? (WCOREDUMP(status) ? "core-dump" : "signal") : "exit-code";
    drain_output(u, false);
    if (!main) {
        u->control_pid = 0;
        if (u->state == S_STOPPING && u->phase == P_NONE) { if (!u->main_pid) unit_enter(u, u->result[0] && strcmp(u->result, "success") ? S_FAILED : S_INACTIVE, NULL); return; }
        struct strlist* l = u->phase == P_PRE ? &u->pre : u->phase == P_POST ? &u->post : u->phase == P_STOP ? &u->stop : &u->reload;
        bool ignore = u->exec_index < l->n && l->v[u->exec_index][0] == '-';
        if (!ok && !ignore) { if (!u->result[0] && u->phase != P_STOP && u->phase != P_RELOAD) copy_str(u->result, sizeof(u->result), failure); phase_done(u, false); }
        else next_command(u);
        return;
    }
    u->main_pid = 0;
    bool ignore = u->type == T_ONESHOT && u->exec_index < u->start.n && u->start.v[u->exec_index][0] == '-';
    if (u->state == S_STOPPING) {
        if (!u->control_pid) unit_enter(u, u->result[0] && strcmp(u->result, "success") ? S_FAILED : S_INACTIVE, NULL);
        return;
    }
    if (u->type == T_ONESHOT && u->state == S_STARTING) {
        if (ok || ignore) next_command(u);
        else { copy_str(u->result, sizeof(u->result), failure); phase_done(u, false); }
        return;
    }
    if (u->type == T_FORKING && u->state == S_STARTING && u->phase == P_START) {
        if (!ok) { copy_str(u->result, sizeof(u->result), failure); phase_done(u, false); return; }
        if (u->pidfile[0]) read_pidfile(u);
        u->phase = P_NONE;
        if (!u->ready_path[0]) unit_active(u);
        return;
    }
    /* the main process of a running service ended */
    if (u->state == S_STARTING && u->type == T_NOTIFY) { copy_str(u->result, sizeof(u->result), ok ? "protocol" : failure); unit_enter(u, S_FAILED, NULL); return; }
    unit_enter(u, ok ? S_INACTIVE : S_FAILED, ok ? "success" : failure);
}
static void handle_notify(struct unit* u) {
    char buf[4096];
    for (;;) {
        ssize_t n = recv(u->notify_fd, buf, sizeof(buf) - 1, 0); if (n <= 0) return;
        buf[n] = 0;
        for (char* save = NULL, *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (!strcmp(line, "READY=1") && u->state == S_STARTING) { if (!u->ready_path[0]) unit_active(u); }
            else if (!strncmp(line, "STATUS=", 7)) copy_str(u->status_text, sizeof(u->status_text), line + 7);
            else if (!strncmp(line, "MAINPID=", 8)) { long p = atol(line + 8); if (p > 1 && p != u->main_pid) { u->main_pid = (pid_t)p; u->main_is_child = false; } }
            else if (!strcmp(line, "STOPPING=1") && u->state == S_ACTIVE) u->state = S_STOPPING;
        }
    }
}

/* dependency-ordered scheduler: WAITING units start once nothing they are After= is still waiting or starting */
static bool request_start(struct unit* u, int depth);
static bool request_start(struct unit* u, int depth) {
    if (!u || depth > 32 || u->is_template) return false;
    u->stop_wanted = false;
    if (u->state == S_ACTIVE || u->state == S_STARTING || u->state == S_WAITING) { u->wanted = true; return true; }
    if (u->state == S_STOPPING) { u->wanted = true; return true; }   /* restarted when the stop completes */
    u->wanted = true; if (u->state == S_FAILED || u->state == S_RESTART_WAIT) u->state = S_INACTIVE;
    if (!strcmp(u->result, "start-limit-hit")) { u->nstarts = 0; u->result[0] = 0; }
    u->restarts = 0; u->state = S_WAITING;
    for (unsigned i = 0; i < u->requires.n; i++) {
        if (!valid_unit_name(u->requires.v[i])) continue;   /* systemd targets, mounts: not used by NexusOS */
        struct unit* d = load_unit(u->requires.v[i]);
        if (!d) { unit_enter(u, S_FAILED, "dependency"); mlog(3, "%s: required unit %s not found", u->name, u->requires.v[i]); return false; }
        request_start(d, depth + 1);
    }
    for (unsigned i = 0; i < u->wants.n; i++) { struct unit* d = load_unit(u->wants.v[i]); if (d) request_start(d, depth + 1); }
    for (unsigned i = 0; i < u->conflicts.n; i++) { struct unit* d = find_unit(u->conflicts.v[i]); if (d) { d->stop_wanted = true; begin_stop(d, NULL); } }
    return true;
}
static bool ordered_after(struct unit* u, struct unit* d) {
    if (sl_has(&u->after, d->name)) return true;
    if (u->kind == K_SERVICE && d->kind == K_SOCKET) {   /* a service starts after the sockets it is handed */
        char target[160]; if (d->trigger[0]) copy_str(target, sizeof(target), d->trigger); else { const char* dot = strrchr(d->name, '.'); snprintf(target, sizeof(target), "%.*s.service", (int)(dot - d->name), d->name); }
        if (!strcmp(target, u->name) || sl_has(&u->sockets_opt, d->name)) return true;
    }
    return u->default_deps && strcmp(u->name, "nexus-firstboot.service") && !strcmp(d->name, "nexus-firstboot.service");
}
static void schedule(void) {
    bool progress = true;
    for (int round = 0; progress && round < 64; round++) {
        progress = false;
        for (unsigned i = 0; i < nunits; i++) {
            struct unit* u = units[i]; if (u->state != S_WAITING) continue;
            bool blocked = false, dep_failed = false;
            for (unsigned j = 0; j < nunits; j++) {
                struct unit* d = units[j]; if (d == u || !ordered_after(u, d)) continue;
                if (d->state == S_WAITING || d->state == S_STARTING) blocked = true;   /* like systemd: ordering waits for the start job, not for restarts */
                if (sl_has(&u->requires, d->name) && (d->state == S_FAILED || (d->state == S_INACTIVE && !d->condition_failed && d->type != T_ONESHOT))) dep_failed = true;
            }
            for (unsigned j = 0; j < u->requires.n && !dep_failed; j++) { struct unit* d = find_unit(u->requires.v[j]); if (d && d->state == S_FAILED) dep_failed = true; }
            if (dep_failed) { u->wanted = false; unit_enter(u, S_FAILED, "dependency"); progress = true; continue; }
            if (blocked) continue;
            begin_start(u); progress = true;
        }
    }
}
static void stop_with_dependents(struct unit* u, int depth) {
    if (!u || depth > 32) return;
    u->stop_wanted = true; u->wanted = false;
    for (unsigned i = 0; i < nunits; i++) if (sl_has(&units[i]->requires, u->name) && (units[i]->state != S_INACTIVE && units[i]->state != S_FAILED)) stop_with_dependents(units[i], depth + 1);
    begin_stop(u, NULL);
}

/* ------------------------------------------------------------------------------------------------ control socket */
struct client { int fd; uid_t uid; pid_t pid; char in[1024]; size_t len; uint64_t since; };
static volatile sig_atomic_t got_term, got_hup;   /* set by signals and by the shutdown verb */
static struct client clients[MAX_CLIENTS];
static int control_fd = -1, syslog_fd = -1, kmsg_fd = -1, wake_pipe[2] = {-1, -1};
static void reply(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(int fd, const char* fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (n < 0) return; if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (int off = 0; off < n;) { ssize_t w = send(fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL); if (w <= 0) return; off += (int)w; }
}
static void finish_reply(int fd, int status) { char b[16]; int n = snprintf(b, sizeof(b), "%c%d\n", NEXUSD_REPLY_END, status); send(fd, b, (size_t)n, MSG_NOSIGNAL); }
static const char* state_text(const struct unit* u) {
    if (u->state == S_INACTIVE && !strcmp(u->result, "condition")) return "inactive";
    return state_names[u->state];
}
static bool socket_service_busy(struct unit* u);
static void act_status(int fd, struct unit* u);
static void act_show(int fd, struct unit* u);
static void act_list(int fd, enum ukind kind);
static const char* sub_text(struct unit* u) {
    if (u->kind != K_SERVICE && u->act && u->state == S_ACTIVE)
        return u->kind == K_SOCKET ? (socket_service_busy(u) ? "running" : "listening") :
               u->kind == K_TIMER ? (u->act->next_mono == INFINITE_MS && u->act->next_real < 0 ? "elapsed" : "waiting") : "waiting";
    switch (u->state) {
    case S_ACTIVE: return u->main_pid > 0 ? "running" : u->type == T_ONESHOT ? "exited" : "running";
    case S_WAITING: return "start-wait"; case S_STARTING: return u->phase == P_PRE ? "start-pre" : u->phase == P_POST ? "start-post" : "start";
    case S_STOPPING: return u->phase == P_STOP ? "stop" : "stop-sigterm"; case S_FAILED: return "failed"; case S_RESTART_WAIT: return "auto-restart";
    default: return "dead";
    }
}
/* root, or the owner of a user manager */
static bool client_privileged(const struct client* c) { return !c->uid || (user_mode && c->uid == getuid()); }
static bool user_may(const struct client* c, const struct unit* u) {
    if (client_privileged(c)) return true;
    char own[32]; snprintf(own, sizeof(own), "%u", (unsigned)c->uid);
    return u && u->allow_user && !strcmp(u->instance, own);
}
static void cmd_status(int fd, struct unit* u) {
    reply(fd, "* %s%s%s\n", u->name, u->description[0] ? " - " : "", u->description);
    reply(fd, "     Loaded: loaded (%s; %s)\n", u->file, unit_enabled(u) ? "enabled" : "disabled");
    char since[64] = "";
    if (u->state == S_ACTIVE && u->active_since_ms) snprintf(since, sizeof(since), " for %llus", (unsigned long long)((now_ms() - u->active_since_ms) / 1000));
    reply(fd, "     Active: %s (%s)%s%s%s\n", state_text(u), sub_text(u), since, u->result[0] && strcmp(u->result, "success") ? " result=" : "", u->result[0] && strcmp(u->result, "success") ? u->result : "");
    if (u->state == S_RESTART_WAIT) reply(fd, "    Restart: attempt %u in %llu ms\n", u->restarts, (unsigned long long)(u->restart_at_ms > now_ms() ? u->restart_at_ms - now_ms() : 0));
    if (u->main_pid > 0) reply(fd, "   Main PID: %ld\n", (long)u->main_pid);
    if (u->exit_code >= 0 || u->exit_signal) reply(fd, "  Last exit: %s %d\n", u->exit_signal ? "signal" : "status", u->exit_signal ? u->exit_signal : u->exit_code);
    if (u->status_text[0]) reply(fd, "     Status: \"%s\"\n", u->status_text);
    if (u->kind != K_SERVICE) { act_status(fd, u); return; }
    static const char* const restart_names[] = {"no", "on-success", "on-failure", "on-abnormal", "always"};
    reply(fd, "       Type: %s; Restart: %s; restarts: %u\n", type_names[u->type], restart_names[u->restart], u->restarts);
}
static int set_enabled(struct unit* u, bool enable) {
    char p[PATH_MAX + 200]; mkdir_p(enabled_dir, 0755); mkdir_p(disabled_dir, 0755);
    bool by_default = enabled_by_default(u);
    snprintf(p, sizeof(p), "%s/%s", enabled_dir, u->name); unlink(p);
    if (enable && !by_default && symlink(u->file, p)) return -1;
    snprintf(p, sizeof(p), "%s/%s", disabled_dir, u->name); unlink(p);
    if (!enable && by_default) { int f = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (f < 0) return -1; close(f); }
    return 0;
}
static void start_session(uid_t uid) {
    char inst[32]; snprintf(inst, sizeof(inst), "%u", (unsigned)uid);
    for (unsigned i = 0; i < nunits; i++) {
        struct unit* t = units[i]; if (!t->is_template || !sl_has(&t->wanted_by, "session")) continue;
        char name[160]; snprintf(name, sizeof(name), "%.*s@%s.service", (int)(strchr(t->name, '@') - t->name), t->name, inst);
        struct unit* u = load_unit(name); if (u && unit_enabled(t)) request_start(u, 0);
    }
}
/* the session of uid ended (logout): its session units (user@UID: the user manager, the session bus, ...) stop */
static unsigned end_session(uid_t uid) {
    char inst[32]; snprintf(inst, sizeof(inst), "%u", (unsigned)uid); unsigned n = 0;
    for (unsigned i = 0; i < nunits; i++) {
        struct unit* u = units[i]; if (u->is_template || strcmp(u->instance, inst)) continue;
        char tpl[160]; const char* dot = strrchr(u->name, '.');
        snprintf(tpl, sizeof(tpl), "%.*s@%s", (int)(strchr(u->name, '@') - u->name), u->name, dot ? dot : ".service");
        struct unit* t = find_unit(tpl);
        if (t && sl_has(&t->wanted_by, "session") && u->state != S_INACTIVE && u->state != S_FAILED) { stop_with_dependents(u, 0); n++; }
    }
    return n;
}
#include "nexusd_units.c"
/* Accept=yes instances that ended */
static void unit_free(struct unit* u) {
    struct strlist* lists[] = {&u->requires, &u->wants, &u->after, &u->before, &u->conflicts, &u->wanted_by, &u->pre, &u->start, &u->post, &u->stop,
                               &u->reload, &u->supp, &u->env, &u->env_files, &u->runtime_dirs, &u->state_dirs, &u->logs_dirs, &u->cache_dirs, &u->sockets_opt};
    for (unsigned i = 0; i < sizeof(lists) / sizeof(*lists); i++) sl_clear(lists[i]);
    close_notify(u);
    if (u->out_fd >= 0) close(u->out_fd);
    if (u->out_wr >= 0) close(u->out_wr);
    if (u->conn_fd >= 0) close(u->conn_fd);
    if (u->act) { if (u->kind == K_SOCKET) socket_close_all(u, false); if (u->act->ino_fd >= 0) close(u->act->ino_fd); free(u->act); }
    free(u);
}
static void gc_units(void) {
    unsigned k = 0;
    for (unsigned i = 0; i < nunits; i++) {
        struct unit* u = units[i];
        if (u->gc && (u->state == S_INACTIVE || u->state == S_FAILED) && u->main_pid <= 0 && u->control_pid <= 0) { unit_free(u); continue; }
        units[k++] = u;
    }
    nunits = k;
}
static void execute(struct client* c, char* line) {
    char* argv[16]; int argc = 0;
    for (char* save = NULL, *w = strtok_r(line, " ", &save); w && argc < 16; w = strtok_r(NULL, " ", &save)) argv[argc++] = w;
    int fd = c->fd;
    if (!argc) { finish_reply(fd, 2); return; }
    const char* verb = argv[0];
    if (!strcmp(verb, "list")) {
        for (unsigned i = 0; i < nunits; i++) { struct unit* u = units[i]; if (u->is_template) continue;
            reply(fd, "%-44s %-9s %-12s %-8s %s\n", u->name, state_text(u), sub_text(u), unit_enabled(u) ? "enabled" : "disabled", u->description); }
        finish_reply(fd, 0); return;
    }
    if (!strcmp(verb, "ping")) { reply(fd, "pong %s %s\n", journal.boot_id, user_mode ? "user-manager" : root_mode ? "system" : "user"); finish_reply(fd, 0); return; }
    if (!strcmp(verb, "timers")) { act_list(fd, K_TIMER); finish_reply(fd, 0); return; }
    if (!strcmp(verb, "sockets")) { act_list(fd, K_SOCKET); finish_reply(fd, 0); return; }
    if (!strcmp(verb, "paths")) { act_list(fd, K_PATH); finish_reply(fd, 0); return; }
    if (!strcmp(verb, "shutdown")) {   /* the desktop (pid 1, which may not signal a root process) or root: stop everything */
        if (!client_privileged(c) && c->pid != 1) { reply(fd, "Access denied\n"); finish_reply(fd, 4); return; }
        mlog(5, "shutdown requested by pid %ld uid %u", (long)c->pid, (unsigned)c->uid);
        got_term = 1; finish_reply(fd, 0); return;
    }
    if (!strcmp(verb, "daemon-reload")) {
        if (!client_privileged(c)) { reply(fd, "Access denied\n"); finish_reply(fd, 4); return; }
        for (unsigned i = 0; i < nunits; i++) {   /* reload the definitions of units that are not running */
            struct unit* u = units[i];
            if ((u->state == S_INACTIVE || u->state == S_FAILED) && !u->transient) {
                struct unit fresh; memset(&fresh, 0, sizeof(fresh)); unit_defaults(&fresh); copy_str(fresh.name, sizeof(fresh.name), u->name);
                copy_str(fresh.instance, sizeof(fresh.instance), u->instance); fresh.is_template = u->is_template;
                fresh.kind = u->kind; if (fresh.kind != K_SERVICE) fresh.act = act_new();
                char path[PATH_MAX], tpl[160]; unsigned from = 0;
                if (unit_source(u->name, path, sizeof(path), tpl, sizeof(tpl), &from) && (fresh.systemd = from >= NEXUS_UNIT_DIRS, parse_file(&fresh, path))) {
                    const char* base = tpl[0] ? tpl : u->name; parse_nexus_dropins(&fresh, base); act_finish(&fresh);
                    copy_str(fresh.file, sizeof(fresh.file), path);
                    fresh.active_enter_ms = u->active_enter_ms; fresh.inactive_enter_ms = u->inactive_enter_ms;
                    if (u->act) { if (u->kind == K_SOCKET) socket_close_all(u, false); if (u->act->ino_fd >= 0) close(u->act->ino_fd); free(u->act); u->act = NULL; }
                    sl_clear(&u->sockets_opt);
                    fresh.state = u->state; copy_str(fresh.result, sizeof(fresh.result), u->result); fresh.loaded = true;
                    memcpy(fresh.starts, u->starts, sizeof(u->starts)); fresh.nstarts = u->nstarts; fresh.exit_code = u->exit_code; fresh.exit_signal = u->exit_signal;
                    sl_clear(&u->requires); sl_clear(&u->wants); sl_clear(&u->after); sl_clear(&u->before); sl_clear(&u->conflicts); sl_clear(&u->wanted_by);
                    sl_clear(&u->pre); sl_clear(&u->start); sl_clear(&u->post); sl_clear(&u->stop); sl_clear(&u->reload); sl_clear(&u->supp); sl_clear(&u->env);
                    sl_clear(&u->env_files); sl_clear(&u->runtime_dirs); sl_clear(&u->state_dirs); sl_clear(&u->logs_dirs); sl_clear(&u->cache_dirs);
                    *u = fresh;
                } else { free(fresh.act); sl_clear(&fresh.sockets_opt); }
            }
        }
        scan_units(); reply(fd, "reloaded %u units\n", nunits); finish_reply(fd, 0); return;
    }
    if (!strcmp(verb, "session")) {
        if (argc != 2) { finish_reply(fd, 2); return; }
        char* end; unsigned long v = strtoul(argv[1], &end, 10); uid_t uid = (uid_t)v;
        if (*end || end == argv[1] || v > 0x7fffffffUL) { finish_reply(fd, 2); return; }   /* "session x" is not uid 0 */
        if (user_mode) { reply(fd, "sessions belong to the system manager\n"); finish_reply(fd, 2); return; }
        if (c->uid && c->uid != uid) { reply(fd, "Access denied\n"); finish_reply(fd, 4); return; }
        start_session(uid); schedule(); finish_reply(fd, 0); return;
    }
    if (!strcmp(verb, "session-end")) {   /* logout: root, the desktop (pid 1) or the user */
        if (argc != 2 || user_mode) { finish_reply(fd, 2); return; }
        char* end; unsigned long v = strtoul(argv[1], &end, 10); uid_t uid = (uid_t)v;
        if (*end || end == argv[1] || v > 0x7fffffffUL) { finish_reply(fd, 2); return; }
        if (c->uid && c->uid != uid && c->pid != 1) { reply(fd, "Access denied\n"); finish_reply(fd, 4); return; }
        unsigned n = end_session(uid); mlog(5, "session of uid %u ended (requested by uid %u): %u units stopping", (unsigned)uid, (unsigned)c->uid, n);
        schedule(); finish_reply(fd, 0); return;
    }
    if (argc < 2) { reply(fd, "%s needs a unit name\n", verb); finish_reply(fd, 2); return; }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char name[160]; snprintf(name, sizeof(name), "%s%s", argv[i], unit_suffix_len(argv[i]) ? "" : ".service");
        struct unit* u = valid_unit_name(name) ? load_unit(name) : NULL;
        if (!strcmp(verb, "is-active")) { reply(fd, "%s\n", u ? state_text(u) : "inactive"); if (!u || u->state != S_ACTIVE) status = 3; continue; }
        if (!strcmp(verb, "is-enabled")) { reply(fd, "%s\n", u ? (unit_enabled(u) ? "enabled" : "disabled") : "not-found"); if (!u || !unit_enabled(u)) status = 1; continue; }
        if (!strcmp(verb, "is-failed")) { reply(fd, "%s\n", u ? state_text(u) : "inactive"); if (!u || u->state != S_FAILED) status = 1; continue; }
        if (!u) { reply(fd, "Unit %s not found.\n", name); status = 5; continue; }
        if (!strcmp(verb, "status")) { cmd_status(fd, u); if (u->state != S_ACTIVE) status = 3; continue; }
        if (!strcmp(verb, "show")) {
            reply(fd, "Id=%s\nLoadState=loaded\nActiveState=%s\nSubState=%s\nResult=%s\nMainPID=%ld\nUnitFileState=%s\nType=%s\nNRestarts=%u\nExecMainStatus=%d\nStatusText=%s\nFragmentPath=%s\n",
                  u->name, state_text(u), sub_text(u), u->result[0] ? u->result : "success", (long)(u->main_pid > 0 ? u->main_pid : 0), unit_enabled(u) ? "enabled" : "disabled",
                  type_names[u->type], u->restarts, u->exit_code, u->status_text, u->file);
            if (u->kind != K_SERVICE) act_show(fd, u);
            continue;
        }
        bool start_like = !strcmp(verb, "start") || !strcmp(verb, "restart") || !strcmp(verb, "try-restart") || !strcmp(verb, "reload-or-restart");
        if (!strcmp(verb, "stop") || start_like || !strcmp(verb, "reload") || !strcmp(verb, "reset-failed") || !strcmp(verb, "kill")) {
            if (!user_may(c, u)) { reply(fd, "Access denied: %s needs root\n", verb); status = 4; continue; }
            if (u->is_template) { reply(fd, "%s is a template; name an instance\n", u->name); status = 2; continue; }
            if (!strcmp(verb, "stop")) { stop_with_dependents(u, 0); mlog(5, "stop %s requested by uid %u", u->name, (unsigned)c->uid); continue; }
            if (!strcmp(verb, "kill")) { kill_unit(u, SIGTERM); continue; }
            if (!strcmp(verb, "reset-failed")) { if (u->state == S_FAILED) { u->state = S_INACTIVE; u->result[0] = 0; } u->nstarts = 0; continue; }
            if (!strcmp(verb, "reload")) {
                if (u->state != S_ACTIVE) { reply(fd, "%s is not active\n", u->name); status = 1; continue; }
                if (u->reload.n) start_phase(u, P_RELOAD); else if (u->main_pid > 0) kill(u->main_pid, SIGHUP);
                continue;
            }
            if (!strcmp(verb, "try-restart") && u->state != S_ACTIVE) continue;
            if (strcmp(verb, "start") && (u->state == S_ACTIVE || u->state == S_STARTING)) {
                u->restart_requested = true; u->stop_wanted = false; begin_stop(u, NULL);
            }
            mlog(5, "%s %s requested by uid %u", verb, u->name, (unsigned)c->uid);
            if (u->state == S_STOPPING) { u->wanted = true; continue; }
            u->nstarts = 0; request_start(u, 0); continue;
        }
        if (!strcmp(verb, "enable") || !strcmp(verb, "disable")) {
            if (!client_privileged(c)) { reply(fd, "Access denied: %s needs root\n", verb); status = 4; continue; }
            if (set_enabled(u, verb[0] == 'e')) { reply(fd, "%s %s: %s\n", verb, u->name, strerror(errno)); status = 1; }
            else reply(fd, "%s %s\n", verb[0] == 'e' ? "Enabled" : "Disabled", u->name);
            continue;
        }
        reply(fd, "unknown verb %s\n", verb); status = 2; break;
    }
    schedule(); finish_reply(fd, status);
}
static void client_input(struct client* c) {
    ssize_t n = recv(c->fd, c->in + c->len, sizeof(c->in) - 1 - c->len, 0);
    if (n <= 0) { close(c->fd); c->fd = -1; return; }
    c->len += (size_t)n; c->in[c->len] = 0;
    char* nl = strchr(c->in, '\n');
    if (!nl) { if (c->len >= sizeof(c->in) - 1) { close(c->fd); c->fd = -1; } return; }
    *nl = 0;
    for (char* p = c->in; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7f) { finish_reply(c->fd, 2); close(c->fd); c->fd = -1; return; }
    execute(c, c->in);
    close(c->fd); c->fd = -1;
}
static int listen_unix(const char* path, int type, mode_t mode) {
    unlink(path);
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) return -1;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (!copy_str(a.sun_path, sizeof(a.sun_path), path) || bind(fd, (struct sockaddr*)&a, sizeof(a)) || (type == SOCK_STREAM && listen(fd, 16))) { close(fd); return -1; }
    chmod(path, mode); return fd;
}
/* RFC 3164 as glibc's syslog() writes it: "<PRI>Mmm dd hh:mm:ss TAG[PID]: message" */
static void syslog_input(void) {
    for (;;) {
        /* the sender's real pid comes from SCM_CREDENTIALS (SO_PASSCRED), not from the text, which anyone can write */
        char buf[8192], control[CMSG_SPACE(sizeof(struct ucred))]; struct iovec v = {buf, sizeof(buf) - 1};
        struct msghdr m = {.msg_iov = &v, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        ssize_t n = recvmsg(syslog_fd, &m, 0); if (n <= 0) return;
        long sender = 0;
        for (struct cmsghdr* h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
            if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_CREDENTIALS) { struct ucred cr; memcpy(&cr, CMSG_DATA(h), sizeof(cr)); sender = cr.pid; }
        buf[n] = 0; char* p = buf; int pri = 13;
        if (*p == '<') { char* e; long v = strtol(p + 1, &e, 10); if (*e == '>' && v >= 0 && v < 1024) { pri = (int)v; p = e + 1; } }
        if (strlen(p) >= 16 && p[3] == ' ' && p[6] == ' ' && p[9] == ':' && p[12] == ':' && p[15] == ' ') p += 16;
        char ident[64] = "syslog"; long pid = 0; char* colon = strstr(p, ": ");
        if (colon && colon - p < 60) {
            size_t k = (size_t)(colon - p); char tag[64]; memcpy(tag, p, k); tag[k] = 0;
            char* br = strchr(tag, '['); if (br) { pid = atol(br + 1); *br = 0; }
            if (tag[0] && !strchr(tag, ' ')) { copy_str(ident, sizeof(ident), tag); p = colon + 2; }
        }
        if (sender > 0) pid = sender;
        journal_write(pri & 7, unit_of_pid(pid), ident, pid, p, strlen(p));
    }
}
/* /dev/kmsg records: "pri,seq,usec,flags;message\n" */
static void kmsg_input(void) {
    for (;;) {
        char buf[2048]; ssize_t n = read(kmsg_fd, buf, sizeof(buf) - 1);
        if (n < 0 && errno == EPIPE) continue;     /* overwritten records */
        if (n <= 0) return;
        buf[n] = 0; char* semi = strchr(buf, ';'); if (!semi) continue;
        int pri = atoi(buf) & 7; char* msg = semi + 1; char* end = strchr(msg, '\n'); if (end) *end = 0;
        journal_write(pri, "kernel", "kernel", 0, msg, strlen(msg));
    }
}

/* ------------------------------------------------------------------------------------------------ boot */
static void clean_run(const char* dir, int depth) {
    DIR* d = opendir(dir); if (!d) return; struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (depth == 0 && (!strcmp(e->d_name, "user") || !strcmp(e->d_name, "nexus"))) continue;   /* Aurora's sockets; our lock */
        struct stat st; if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { clean_run(p, depth + 1); rmdir(p); } else unlink(p);
    }
    closedir(d);
}
static void boot_records(void) {
    struct utmpx u; memset(&u, 0, sizeof(u));
    struct timeval tv; gettimeofday(&tv, NULL);
    u.ut_type = BOOT_TIME; copy_str(u.ut_user, sizeof(u.ut_user), "reboot"); copy_str(u.ut_line, sizeof(u.ut_line), "~"); copy_str(u.ut_id, sizeof(u.ut_id), "~~");
    u.ut_tv.tv_sec = (int32_t)tv.tv_sec; u.ut_tv.tv_usec = (int32_t)tv.tv_usec;
    int f = open("/run/utmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664); if (f >= 0) { struct group* g = getgrnam("utmp"); if (g && root_mode) fchown(f, 0, g->gr_gid); close(f); }
    setutxent(); pututxline(&u);
    struct utmpx r; memset(&r, 0, sizeof(r)); r.ut_type = RUN_LVL; r.ut_pid = '5' + 256 * 'N'; copy_str(r.ut_user, sizeof(r.ut_user), "runlevel");
    copy_str(r.ut_line, sizeof(r.ut_line), "~"); copy_str(r.ut_id, sizeof(r.ut_id), "~~"); r.ut_tv = u.ut_tv;
    pututxline(&r); endutxent();
    updwtmpx("/var/log/wtmp", &u); updwtmpx("/var/log/wtmp", &r);
}
/* the host name of /etc/hostname (systemd's hostname setup); invalid names keep the kernel default */
static void set_hostname(void) {
    FILE* f = fopen("/etc/hostname", "re"); if (!f) return;
    char name[128] = ""; if (!fgets(name, sizeof(name), f)) name[0] = 0;
    fclose(f);
    char* s = trim(name); size_t n = strlen(s);
    if (!n || n > 64) return;
    for (size_t i = 0; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '.')) { mlog(4, "/etc/hostname: invalid host name ignored"); return; }
    if (sethostname(s, n)) mlog(4, "sethostname: %s", strerror(errno)); else mlog(6, "host name %s", s);
}
static void on_signal(int s) { int saved = errno; if (s == SIGTERM || s == SIGINT) got_term = 1; if (s == SIGHUP) got_hup = 1; if (wake_pipe[1] >= 0) { ssize_t w = write(wake_pipe[1], "", 1); (void)w; } errno = saved; }
static void become_root(void) {
    if (geteuid() == 0) {
        if (setgroups(0, NULL) || setresgid(0, 0, 0) || setresuid(0, 0, 0)) { perror("nexusd: cannot become root"); exit(1); }
        root_mode = true;
    }
}
/* nexusd --user: the paths of uid's manager; its runtime directory must be the user's own (0700) */
static int user_setup(uid_t uid, char* home, size_t hsize) {
    struct passwd* pw = getpwuid(uid);
    if (!pw || pw->pw_dir[0] != '/' || !copy_str(home, hsize, pw->pw_dir)) { fprintf(stderr, "nexusd --user: no home folder for uid %u\n", (unsigned)uid); return -1; }
    char runtime[64]; snprintf(runtime, sizeof(runtime), "/run/user/%u", (unsigned)uid); struct stat st;
    if (lstat(runtime, &st) || !S_ISDIR(st.st_mode) || st.st_uid != uid || (st.st_mode & 077)) {
        fprintf(stderr, "nexusd --user: %s must be a folder of uid %u with mode 0700 (made by the system manager)\n", runtime, (unsigned)uid); return -1; }
    snprintf(run_dir, sizeof(run_dir), "%s/%s", runtime, NEXUSD_USER_RUN);
    snprintf(control_path, sizeof(control_path), "%s/control", run_dir); snprintf(lock_path, sizeof(lock_path), "%s/nexusd.lock", run_dir);
    snprintf(notify_dir, sizeof(notify_dir), "%s/notify", run_dir);
    snprintf(enabled_dir, sizeof(enabled_dir), "%s/.config/nexus/user-enabled", home); snprintf(disabled_dir, sizeof(disabled_dir), "%s/.config/nexus/user-disabled", home);
    snprintf(journal_dir, sizeof(journal_dir), "%s/%s", home, NEXUS_USER_JOURNAL_DIR); copy_str(journal_file, sizeof(journal_file), NEXUS_USER_JOURNAL_FILE);
    snprintf(timers_dir, sizeof(timers_dir), "%s/.local/state/nexus/timers", home);
    snprintf(user_dir_buf[0], PATH_MAX, "%s/.config/nexus/user", home); snprintf(user_dir_buf[1], PATH_MAX, "%s/.config/systemd/user", home);
    unit_dirs[0] = user_dir_buf[0]; unit_dirs[1] = NEXUS_USER_ADMIN_UNITS; unit_dirs[2] = NEXUS_USER_VENDOR_UNITS;
    unit_dirs[3] = user_dir_buf[1]; unit_dirs[4] = "/etc/systemd/user"; unit_dirs[5] = "/usr/lib/systemd/user";
    unit_dir_count = 6; nexus_unit_dirs = 3;
    setenv("HOME", home, 1); setenv("USER", pw->pw_name, 1); setenv("LOGNAME", pw->pw_name, 1); setenv("XDG_RUNTIME_DIR", runtime, 1);
    return 0;
}
int main(int argc, char** argv) {
    bool system_mode = argc > 1 && !strcmp(argv[1], "--system");
    user_mode = argc > 1 && !strcmp(argv[1], "--user");
    if (!system_mode && !user_mode) { fprintf(stderr, "usage: nexusd --system | --user (started by the desktop and the session; use nexusctl)\n"); return 2; }
    char notify_socket[UPATH] = "", home[PATH_MAX] = "";
    if (user_mode) {
        /* the binary is set-user-ID root: a user manager drops that at once and runs with the caller's identity */
        uid_t uid = getuid(); gid_t gid = getgid();
        if (setresgid(gid, gid, gid) || setresuid(uid, uid, uid) || geteuid() != uid || getegid() != gid) { perror("nexusd --user: cannot drop privileges"); return 1; }
        const char* ns = getenv("NOTIFY_SOCKET"); if (ns && ns[0] == '/') copy_str(notify_socket, sizeof(notify_socket), ns);
    } else if (getuid() != 0 && getppid() != 1) {   /* set-user-ID root: only the desktop (pid 1) or root may start the system's service manager */
        fprintf(stderr, "nexusd: only the desktop (pid 1) or root starts the service manager\n"); return 1;
    }
    if (clearenv()) return 1;
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1); setenv("LANG", "C.UTF-8", 1);
    if (user_mode) { if (user_setup(getuid(), home, sizeof(home))) return 1; }
    else become_root();
    umask(022); if (chdir(user_mode ? home : "/") && chdir("/")) return 1;
    devnull = open("/dev/null", O_RDWR | O_CLOEXEC); if (devnull < 0) return 1;
    for (int fd = 3; fd < 1024; fd++) if (fd != devnull) close(fd);
    dup2(devnull, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    manager_start_ms = now_ms();
    if (!mkdir_p(run_dir, user_mode ? 0700 : 0755)) { perror("nexusd: runtime folder"); return 1; }
    int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { fprintf(stderr, "nexusd: another service manager is running\n"); return 1; }
    setsid();
    if (root_mode) clean_run("/run", 0);
    if (!user_mode) { mkdir_p("/run/lock", 01777); chmod("/run/lock", 01777); mkdir_p("/run/user", 0755); }
    mkdir_p(notify_dir, user_mode ? 0700 : 0755);
    { char pid[32]; int n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid()); if (ftruncate(lock, 0) == 0 && write(lock, pid, (size_t)n) < 0) {} }
    uint8_t rnd[8]; if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd)) memset(rnd, 0x42, sizeof(rnd));
    for (int i = 0; i < 8; i++) snprintf(journal.boot_id + 2 * i, 3, "%02x", rnd[i]);
    if (user_mode) {   /* the system's boot id (records of one boot match across journals) */
        int f = open(NEXUSD_RUN "/boot_id", O_RDONLY | O_CLOEXEC); if (f >= 0) { char b[17]; if (read(f, b, 16) == 16) { b[16] = 0; memcpy(journal.boot_id, b, 17); } close(f); }
    } else { int f = open(NEXUSD_RUN "/boot_id", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (f >= 0) { if (write(f, journal.boot_id, 16) < 0) {} close(f); } }
    journal_config(); journal_open();
    journal_write(6, "nexusd", "nexusd", getpid(), "-- boot --", 10);
    mlog(5, "NexusOS service manager starting (%s mode, boot %s)", user_mode ? "user-manager" : root_mode ? "system" : "user", journal.boot_id);
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) && errno != EINVAL) {}
    if (root_mode) boot_records(), set_hostname();
    if (!user_mode) {
        syslog_fd = listen_unix(NEXUSD_SYSLOG, SOCK_DGRAM, 0666);
        if (syslog_fd < 0) mlog(3, "cannot receive syslog messages on %s: %s", NEXUSD_SYSLOG, strerror(errno));
        else { int on = 1; setsockopt(syslog_fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)); }
        if (syslog_fd >= 0 && root_mode) {   /* /dev/log: the kernel's /dev is synthetic; the on-disk /dev holds this link */
            struct stat st; if (!lstat("/dev/log", &st) && S_ISLNK(st.st_mode)) unlink("/dev/log");
            if (symlink(NEXUSD_SYSLOG, "/dev/log") && errno != EEXIST) mlog(4, "cannot create /dev/log: %s", strerror(errno));
        }
        kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    control_fd = listen_unix(control_path, SOCK_STREAM, user_mode ? 0600 : 0666);
    if (control_fd < 0) { mlog(2, "cannot listen on %s: %s", control_path, strerror(errno)); return 1; }
    if (notify_socket[0]) {   /* user@UID.service is Type=notify: ready once the control socket is up */
        int nf = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0); struct sockaddr_un na = {.sun_family = AF_UNIX};
        if (nf >= 0 && copy_str(na.sun_path, sizeof(na.sun_path), notify_socket)) {
            char msg[64]; int n = snprintf(msg, sizeof(msg), "READY=1\nSTATUS=user manager running\nMAINPID=%ld", (long)getpid());
            if (sendto(nf, msg, (size_t)n, 0, (struct sockaddr*)&na, sizeof(na)) < 0) mlog(4, "cannot report readiness: %s", strerror(errno));
        }
        if (nf >= 0) close(nf);
    }
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK)) return 1;
    struct sigaction sa = {.sa_handler = on_signal, .sa_flags = SA_RESTART}; sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    for (unsigned i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    scan_units();
    unsigned boot_units = 0;
    for (unsigned i = 0; i < nunits; i++) {
        struct unit* u = units[i];
        bool boot = sl_has(&u->wanted_by, "multi-user") || sl_has(&u->wanted_by, "multi-user.target") || sl_has(&u->wanted_by, "default") ||
                    sl_has(&u->wanted_by, "basic.target") ||
                    sl_has(&u->wanted_by, "default.target") || sl_has(&u->wanted_by, "graphical.target") || wants_link(u) ||
                    sl_has(&u->wanted_by, "sockets") || sl_has(&u->wanted_by, "sockets.target") || sl_has(&u->wanted_by, "timers") ||
                    sl_has(&u->wanted_by, "timers.target") || sl_has(&u->wanted_by, "paths") || sl_has(&u->wanted_by, "paths.target");
        if (boot && act_shadowed(u)) { mlog(6, "%s: not used (NexusOS ships its own %s service)", u->name, u->name); continue; }
        if (!u->is_template && !u->instance[0] && unit_enabled(u) && boot) { request_start(u, 0); boot_units++; }
    }
    mlog(5, "loaded %u units, starting %u at boot", nunits, boot_units);
    schedule();
    uint64_t stop_deadline = 0; bool announced_ready = false;
    for (;;) {
        gc_units();
        /* reap */
        for (;;) { int status; pid_t pid = waitpid(-1, &status, WNOHANG); if (pid <= 0) break; child_exited(pid, status); }
        uint64_t t = now_ms();
        if (got_hup) { got_hup = 0; journal_config(); mlog(5, "SIGHUP: journal configuration reloaded"); }
        if (got_term && !shutting_down) {
            shutting_down = true; stop_deadline = t + 45000; mlog(5, "shutting down: stopping all units");
            for (int i = (int)nunits - 1; i >= 0; i--) { units[i]->stop_wanted = true; units[i]->wanted = false; begin_stop(units[i], NULL); }
        }
        if (shutting_down) {
            bool busy = false; for (unsigned i = 0; i < nunits; i++) if (units[i]->state == S_STOPPING || units[i]->main_pid > 0 || units[i]->control_pid > 0) busy = true;
            if (!busy || t > stop_deadline) {
                if (busy) for (unsigned i = 0; i < nunits; i++) kill_unit(units[i], SIGKILL);
                if (root_mode) {   /* what no unit tracks (double-forked daemons, leftovers): SIGTERM, then SIGKILL, as at power-off */
                    kill(-1, SIGTERM);
                    for (int i = 0; i < 30; i++) { while (waitpid(-1, NULL, WNOHANG) > 0) {} struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL); }
                    kill(-1, SIGKILL); while (waitpid(-1, NULL, WNOHANG) > 0) {}
                }
                mlog(5, "service manager stopped"); journal_sync(true); return 0;
            }
        }
        /* timers: start/stop timeouts, restart backoff, ReadyPath polling, liveness of non-child main processes */
        uint64_t next = t + 5000;
        for (unsigned i = 0; i < nunits; i++) {
            struct unit* u = units[i];
            if (u->deadline_ms != INFINITE_MS && u->deadline_ms && (u->state == S_STARTING || u->state == S_STOPPING)) {
                if (t >= u->deadline_ms) {
                    if (u->state == S_STARTING) { mlog(3, "%s: start timed out", u->name); copy_str(u->result, sizeof(u->result), "timeout"); u->stop_wanted = false; begin_stop(u, "timeout"); }
                    else { mlog(3, "%s: stop timed out, killing", u->name); kill_unit(u, SIGKILL); u->deadline_ms = t + 5000; }
                } else if (u->deadline_ms < next) next = u->deadline_ms;
            }
            if (u->state == S_RESTART_WAIT) {
                if (t >= u->restart_at_ms) { u->state = S_INACTIVE; unsigned keep = u->restarts; request_start(u, 0); u->restarts = keep; }
                else if (u->restart_at_ms < next) next = u->restart_at_ms;
            }
            if (u->state == S_STARTING && u->ready_path[0] && (u->main_pid > 0 || u->type == T_FORKING) && u->phase != P_PRE) {
                struct stat st; if (!stat(u->ready_path, &st)) unit_active(u); else if (t + 100 < next) next = t + 100;
            }
            if (u->main_pid > 0 && !u->main_is_child && (u->state == S_ACTIVE || u->state == S_STOPPING)) {
                if (t >= u->next_liveness_ms) {
                    u->next_liveness_ms = t + 1000;
                    if (kill(u->main_pid, 0) && errno == ESRCH) { pid_t gone = u->main_pid; u->main_pid = 0; mlog(5, "%s: main process %ld exited", u->name, (long)gone);
                        if (u->state == S_STOPPING) { if (!u->control_pid) unit_enter(u, S_INACTIVE, NULL); }
                        else unit_enter(u, S_FAILED, "exit-code"); }
                }
                if (u->next_liveness_ms < next) next = u->next_liveness_ms;
            }
        }
        for (unsigned i = 0; i < MAX_CLIENTS; i++)   /* a client that never finishes its request line */
            if (clients[i].fd >= 0 && t - clients[i].since > 10000) { close(clients[i].fd); clients[i].fd = -1; }
        if (!shutting_down) {
            timers_run(&next);
            static uint64_t last_level_check;   /* path units: conditions after their unit ended; without inotify every 5 s */
            bool periodic = t - last_level_check >= 5000;
            if (recheck_paths || periodic) {
                bool again = recheck_paths; recheck_paths = false; if (periodic) last_level_check = t;
                for (unsigned i = 0; i < nunits; i++) if (units[i]->kind == K_PATH && units[i]->state == S_ACTIVE && (again || units[i]->act->ino_fd < 0)) path_check(units[i], false);
            }
            if (t + 5000 < next) next = t + 5000;
        }
        rl_flush();
        schedule();
        if (!announced_ready) {
            bool settling = false; unsigned active = 0;
            for (unsigned i = 0; i < nunits; i++) { if (units[i]->state == S_WAITING || units[i]->state == S_STARTING) settling = true; active += units[i]->state == S_ACTIVE; }
            if (!settling) { announced_ready = true; mlog(5, "startup finished: %u units active", active); }
        }
        journal_sync(false);
        /* poll */
        static struct pollfd fds[4 + MAX_CLIENTS + (3 + MAX_LISTEN) * MAX_UNITS]; static void* owner[4 + MAX_CLIENTS + (3 + MAX_LISTEN) * MAX_UNITS];
        static char kind[4 + MAX_CLIENTS + (3 + MAX_LISTEN) * MAX_UNITS]; unsigned nf = 0;
#define WATCH(f, k, o) do { fds[nf].fd = (f); fds[nf].events = POLLIN; fds[nf].revents = 0; kind[nf] = (k); owner[nf] = (o); nf++; } while (0)
        WATCH(wake_pipe[0], 'w', NULL); WATCH(control_fd, 'c', NULL);
        if (syslog_fd >= 0) WATCH(syslog_fd, 's', NULL);
        if (kmsg_fd >= 0) WATCH(kmsg_fd, 'k', NULL);
        for (unsigned i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0) WATCH(clients[i].fd, 'C', &clients[i]);
        for (unsigned i = 0; i < nunits; i++) { if (units[i]->out_fd >= 0) WATCH(units[i]->out_fd, 'o', units[i]); if (units[i]->notify_fd >= 0) WATCH(units[i]->notify_fd, 'n', units[i]); }
        for (unsigned i = 0; i < nunits && !shutting_down; i++) {   /* listening sockets (not while their Accept=no service runs), path watches */
            struct unit* u = units[i]; if (!u->act || u->state != S_ACTIVE) continue;
            if (u->kind == K_SOCKET && !socket_service_busy(u)) for (unsigned k = 0; k < u->act->nl; k++) if (u->act->l[k].fd >= 0) WATCH(u->act->l[k].fd, 'L', u);
            if (u->kind == K_PATH && u->act->ino_fd >= 0) WATCH(u->act->ino_fd, 'P', u);
        }
#undef WATCH
        uint64_t now2 = now_ms(); int timeout = next > now2 ? (int)(next - now2) : 0; if (journal.dirty && timeout > 5000) timeout = 5000;
        int r = poll(fds, nf, timeout);
        if (r < 0 && errno != EINTR) { mlog(2, "poll: %s", strerror(errno)); struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL); continue; }
        for (unsigned i = 0; r > 0 && i < nf; i++) {
            if (!fds[i].revents) continue;
            switch (kind[i]) {
            case 'w': { char b[64]; while (read(wake_pipe[0], b, sizeof(b)) > 0) {} break; }
            case 'c': for (;;) {
                    int fd = accept4(control_fd, NULL, NULL, SOCK_CLOEXEC); if (fd < 0) break;
                    struct ucred cred; socklen_t len = sizeof(cred);
                    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len)) { close(fd); continue; }
                    if (user_mode && cred.uid != getuid() && cred.uid != 0) { close(fd); continue; }   /* a user manager serves its user (and root) */
                    struct timeval tv = {5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                    struct client* c = NULL; for (unsigned k = 0; k < MAX_CLIENTS; k++) if (clients[k].fd < 0) { c = &clients[k]; break; }
                    if (!c) { close(fd); continue; }
                    c->fd = fd; c->uid = cred.uid; c->pid = cred.pid; c->len = 0; c->since = now_ms();
                } break;
            case 'C': client_input((struct client*)owner[i]); break;
            case 's': syslog_input(); break;
            case 'k': kmsg_input(); break;
            case 'o': drain_output((struct unit*)owner[i], false); break;
            case 'n': handle_notify((struct unit*)owner[i]); break;
            case 'L': { struct unit* u = (struct unit*)owner[i]; if (u->state != S_ACTIVE || !u->act) break;
                for (unsigned k = 0; k < u->act->nl; k++) if (u->act->l[k].fd == fds[i].fd) { if (u->act->accept) socket_accept(u, &u->act->l[k]); else socket_trigger(u); break; }
                break; }
            case 'P': { struct unit* u = (struct unit*)owner[i]; if (u->state == S_ACTIVE && u->act && u->act->ino_fd == fds[i].fd) path_input(u); break; }
            }
        }
    }
}
