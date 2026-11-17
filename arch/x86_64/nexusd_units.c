/* nexusd: socket, timer and path units, journal rate limiting (included by nexusd.c; shares its state).
 *
 * .socket  [Socket] ListenStream= ListenDatagram= ListenSequentialPacket= (AF_UNIX /path or @abstract; PORT or
 *          ADDR:PORT = IPv4, [ADDR6]:PORT = IPv6), Accept=, Service=, MaxConnections= (64), Backlog= (128),
 *          SocketMode= (0666), SocketUser= SocketGroup= DirectoryMode= (0755), PassCredentials=, ReceiveBuffer=,
 *          SendBuffer=, KeepAlive=, ReusePort=, RemoveOnStop=, FileDescriptorName=, TriggerLimitIntervalSec= (2s),
 *          TriggerLimitBurst= (20, Accept=yes 200). Accept=no: the first connection/datagram starts the service (Service=, default the
 *          socket's stem .service) with the listening sockets as fds 3.. (LISTEN_FDS, LISTEN_PID, LISTEN_FDNAMES, as
 *          sd_listen_fds(3) reads them); nexusd stops watching them while the service runs. Accept=yes: nexusd accepts
 *          and starts one instance of the template stem@.service per connection (stem@N-PID-UID.service for AF_UNIX,
 *          stem@N-LOCAL-REMOTE.service for TCP) with the connection as fd 3 (and as stdin/stdout with
 *          StandardInput=socket); instances are removed when they end; beyond MaxConnections connections are closed.
 *          A service started directly also gets the fds of its active Accept=no socket (Sockets= names more).
 * .timer   [Timer] OnCalendar= (systemd calendar events: [WEEKDAYS] [[YEAR-]MONTH-DAY] [HOUR:MINUTE[:SECOND]] [UTC],
 *          components *, N, N..M, A,B, N/STEP; minutely hourly daily weekly monthly quarterly semiannually yearly),
 *          OnBootSec= OnStartupSec= OnActiveSec= OnUnitActiveSec= OnUnitInactiveSec=, Persistent= (a missed calendar
 *          elapse while the system was off runs at the next start; stamp /var/lib/nexus/timers/), RandomizedDelaySec=,
 *          AccuracySec= (accepted), Unit= (default the timer's stem .service), RemainAfterElapse=.
 * .path    [Path] PathExists= PathExistsGlob= PathChanged= PathModified= DirectoryNotEmpty= (inotify), Unit=,
 *          MakeDirectory= DirectoryMode=, TriggerLimitIntervalSec= (2s), TriggerLimitBurst= (200).
 * Debian's own units: .socket/.path units whose service NexusOS ships itself (dbus, cups: not socket-activation aware)
 * are not used; Debian's .timer units are not started from their wants links because Debian's cron jobs
 * (/etc/cron.daily ...) already run the same work on a system without systemd ("nexusctl enable X.timer" starts one).
 *
 * Journal rate limiting (journald.conf RateLimitIntervalSec= 30s, RateLimitBurst= 10000; 0 = off): per source (unit,
 * else syslog identifier, "kernel"), records above the burst in one interval are dropped and counted; the next record
 * of that source after the interval (or the periodic flush) writes "Suppressed N messages from SOURCE". nexusd's own
 * records are never limited. */

/* ------------------------------------------------------------------------------------------------ shared parts */
/* struct actpart and its parts: nexusd_units.h */
static uint64_t manager_start_ms;
static unsigned accept_serial;

static uint64_t realtime_s(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec; }
static int64_t realtime_ms(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
/* "foo.socket" -> "foo.service" (or another suffix) */
static void same_stem(const char* name, const char* suffix, char* out, size_t size) {
    const char* dot = strrchr(name, '.'); size_t n = dot ? (size_t)(dot - name) : strlen(name);
    snprintf(out, size, "%.*s%s", (int)n, name, suffix);
}
static struct actpart* act_new(void) {
    struct actpart* a = xcalloc(1, sizeof(*a));
    a->socket_mode = 0666; a->dir_mode = 0755; a->max_conn = 64; a->backlog = 128; a->ino_fd = -1; a->remain_after_elapse = true;
    a->on_boot = a->on_startup = a->on_active = a->on_unit_active = a->on_unit_inactive = INFINITE_MS; a->random_delay = 0;
    a->next_mono = INFINITE_MS; a->next_real = -1; a->last_trigger_real = -1; a->trig_interval = 2000;
    for (unsigned i = 0; i < MAX_LISTEN; i++) a->l[i].fd = -1;
    return a;
}
/* the unit a socket/timer/path unit starts */
static struct unit* act_target(struct unit* u) {
    char name[160];
    if (u->trigger[0]) copy_str(name, sizeof(name), u->trigger);
    else same_stem(u->name, u->kind == K_SOCKET && u->act && u->act->accept ? "@.service" : ".service", name, sizeof(name));
    return load_unit(name);
}
static bool trigger_limited(struct unit* u) {
    struct actpart* a = u->act; uint64_t t = now_ms();
    if (!a->trig_burst || a->trig_interval == INFINITE_MS) return false;
    if (t - a->trig_window >= a->trig_interval) { a->trig_window = t; a->trig_count = 0; }
    if (++a->trig_count <= a->trig_burst) return false;
    mlog(3, "%s: triggered %u times within %llu ms, giving up (trigger-limit-hit)", u->name, a->trig_count - 1, (unsigned long long)a->trig_interval);
    return true;
}

/* ------------------------------------------------------------------------------------------------ calendar events */
static bool cal_number(const char** s, long* v) { char* e; errno = 0; long x = strtol(*s, &e, 10); if (e == *s || errno) return false; *v = x; *s = e; return true; }
/* one component: "*", "N", "N..M", "N/S", "*\/S", "A,B,..." into bits lo..hi (bit index = value) */
static bool cal_field(const char* text, int lo, int hi, uint64_t* bits) {
    *bits = 0; char buf[96]; if (!copy_str(buf, sizeof(buf), text) || !*buf) return false;
    for (char* save = NULL, *item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        const char* s = item; long a = lo, b = hi, step = 1;
        if (*s == '*') s++;
        else { if (!cal_number(&s, &a)) return false; b = a;
            if (s[0] == '.' && s[1] == '.') { s += 2; if (!cal_number(&s, &b)) return false; } }
        if (*s == '/') { s++; if (!cal_number(&s, &step) || step < 1) return false; if (item[0] != '*' && b == a) b = hi; }
        if (*s || a < lo || b > hi || a > b) return false;
        for (long v = a; v <= b; v += step) *bits |= 1ULL << v;
    }
    return *bits != 0;
}
static int weekday_index(const char* s, size_t n) {   /* Mon = 0 .. Sun = 6 */
    static const char* const names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    static const char* const longn[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
    for (int i = 0; i < 7; i++) if ((n == 3 && !strncasecmp(s, names[i], 3)) || (n == strlen(longn[i]) && !strncasecmp(s, longn[i], n))) return i;
    return -1;
}
static bool cal_weekdays(const char* text, uint8_t* bits) {
    *bits = 0; char buf[96]; if (!copy_str(buf, sizeof(buf), text)) return false;
    for (char* save = NULL, *item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        char* dots = strstr(item, ".."); int a, b;
        if (dots) { a = weekday_index(item, (size_t)(dots - item)); b = weekday_index(dots + 2, strlen(dots + 2)); }
        else a = b = weekday_index(item, strlen(item));
        if (a < 0 || b < 0) return false;
        for (int d = a;; d = (d + 1) % 7) { *bits |= (uint8_t)(1 << d); if (d == b) break; }
    }
    return *bits != 0;
}
static bool cal_parse(const char* in, struct calspec* c) {
    static const struct { const char* name; const char* spec; } shorthand[] = {
        {"minutely", "*-*-* *:*:00"}, {"hourly", "*-*-* *:00:00"}, {"daily", "*-*-* 00:00:00"}, {"monthly", "*-*-01 00:00:00"},
        {"weekly", "Mon *-*-* 00:00:00"}, {"yearly", "*-01-01 00:00:00"}, {"annually", "*-01-01 00:00:00"},
        {"quarterly", "*-01,04,07,10-01 00:00:00"}, {"semiannually", "*-01,07-01 00:00:00"}};
    char buf[160]; if (!copy_str(buf, sizeof(buf), in)) return false;
    char* s = trim(buf);
    for (unsigned i = 0; i < sizeof(shorthand) / sizeof(*shorthand); i++) if (!strcasecmp(s, shorthand[i].name)) { copy_str(buf, sizeof(buf), shorthand[i].spec); s = buf; break; }
    memset(c, 0, sizeof(*c)); c->dow = 0x7f; c->year_min = 1970; c->year_max = 2199; c->year_step = 1;
    char* words[4]; unsigned n = 0;
    for (char* save = NULL, *w = strtok_r(s, " ", &save); w; w = strtok_r(NULL, " ", &save)) { if (n == 4) return false; words[n++] = w; }
    if (n && (!strcmp(words[n - 1], "UTC") || !strcmp(words[n - 1], "utc"))) { c->utc = true; n--; }
    unsigned i = 0;
    if (i < n && isalpha((unsigned char)words[i][0])) { if (!cal_weekdays(words[i], &c->dow)) return false; i++; }
    const char* date = "*-*-*"; const char* tod = "00:00:00";
    for (; i < n; i++) { if (strchr(words[i], '-')) date = words[i]; else if (strchr(words[i], ':')) tod = words[i]; else return false; }
    /* date: [YEAR-]MONTH-DAY */
    char d[96]; if (!copy_str(d, sizeof(d), date)) return false;
    char* parts[3]; unsigned np = 0;
    for (char* p = d, *start = d;; p++) { if (*p == '-' || !*p) { bool end = !*p; *p = 0; if (np == 3) return false; parts[np++] = start; start = p + 1; if (end) break; } }
    uint64_t bits;
    if (np == 3) {
        if (!strcmp(parts[0], "*")) {}
        else { const char* y = parts[0]; long a, b; if (!cal_number(&y, &a)) return false; b = a;
            if (y[0] == '.' && y[1] == '.') { y += 2; if (!cal_number(&y, &b)) return false; }
            long step = 1; if (*y == '/') { y++; if (!cal_number(&y, &step) || step < 1) return false; if (b == a) b = 2199; }
            if (*y || a < 1970 || b > 2199 || a > b) return false; c->year_min = (int)a; c->year_max = (int)b; c->year_step = (int)step; }
        parts[0] = parts[1]; parts[1] = parts[2]; np = 2;
    }
    if (np != 2) return false;
    if (!cal_field(parts[0], 1, 12, &bits)) return false; c->months = (uint16_t)bits;
    if (!cal_field(parts[1], 1, 31, &bits)) return false; c->days = (uint32_t)bits;
    /* time: HOUR:MINUTE[:SECOND] */
    char t[96]; if (!copy_str(t, sizeof(t), tod)) return false;
    char* tp[3]; unsigned nt = 0;
    for (char* p = t, *start = t;; p++) { if (*p == ':' || !*p) { bool end = !*p; *p = 0; if (nt == 3) return false; tp[nt++] = start; start = p + 1; if (end) break; } }
    if (nt < 2) return false;
    if (!cal_field(tp[0], 0, 23, &bits)) return false; c->hours = (uint32_t)bits;
    if (!cal_field(tp[1], 0, 59, &c->minutes)) return false;
    if (nt == 3) { char* dot = strchr(tp[2], '.'); if (dot) *dot = 0; if (!cal_field(tp[2], 0, 59, &c->seconds)) return false; }
    else c->seconds = 1;   /* :00 */
    return true;
}
static int days_in_month(int year, int month) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && ((year % 4 == 0 && year % 100) || year % 400 == 0) ? 29 : d[month - 1];
}
/* the first elapse strictly after `after` (realtime seconds), or -1 */
static int64_t cal_next(const struct calspec* c, int64_t after) {
    time_t t0 = (time_t)after + 1; struct tm tm;
    if (c->utc) gmtime_r(&t0, &tm); else localtime_r(&t0, &tm);
    int year = tm.tm_year + 1900, month = tm.tm_mon + 1, day = tm.tm_mday;
    int sh = tm.tm_hour, sm = tm.tm_min, ss = tm.tm_sec; bool first = true;
    for (int steps = 0; steps < 366 * 9; steps++) {
        bool ok = year >= c->year_min && year <= c->year_max && (year - c->year_min) % c->year_step == 0 && (c->months >> month & 1) && (c->days >> day & 1);
        if (ok) { struct tm probe = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = 12, .tm_isdst = -1};
            time_t noon = c->utc ? timegm(&probe) : mktime(&probe); struct tm w; if (c->utc) gmtime_r(&noon, &w); else localtime_r(&noon, &w);
            ok = c->dow >> ((w.tm_wday + 6) % 7) & 1; }
        if (ok) {
            for (int h = first ? sh : 0; h < 24; h++) {
                if (!(c->hours >> h & 1)) continue;
                for (int m = (first && h == sh) ? sm : 0; m < 60; m++) {
                    if (!(c->minutes >> m & 1)) continue;
                    for (int s = (first && h == sh && m == sm) ? ss : 0; s < 60; s++) {
                        if (!(c->seconds >> s & 1)) continue;
                        struct tm e = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = h, .tm_min = m, .tm_sec = s, .tm_isdst = -1};
                        time_t v = c->utc ? timegm(&e) : mktime(&e);
                        if (v != (time_t)-1 && (int64_t)v > after) return (int64_t)v;   /* a DST gap may move it: still after `after` */
                    }
                }
            }
        }
        first = false;
        if (++day > days_in_month(year, month)) { day = 1; if (++month > 12) { month = 1; if (++year > c->year_max) return -1; } }
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------------ unit file keys */
static long long parse_size(const char* v) {
    char* e; long long n = strtoll(v, &e, 10); if (e == v || n < 0) return -1;
    if (*e == 'K' || *e == 'k') n <<= 10; else if (*e == 'M' || *e == 'm') n <<= 20; else if (*e) return -1;
    return n;
}
static bool listen_add(struct unit* u, int type, const char* value) {
    struct actpart* a = u->act; if (a->nl >= MAX_LISTEN) return false;
    struct listen_spec* l = &a->l[a->nl]; memset(l, 0, sizeof(*l)); l->fd = -1; l->type = type; l->kind = 's';
    if (value[0] == '/' || value[0] == '@') {
        if (strlen(value) >= sizeof(((struct sockaddr_un*)0)->sun_path) || strstr(value, "/../") || ends_with(value, "/..")) return false;
        l->family = AF_UNIX;
    } else if (value[0] == '[') { l->family = AF_INET6; if (type == SOCK_SEQPACKET || !strchr(value, ']')) return false; }
    else { char* e; long port = strtol(value, &e, 10);
        if (*e == 0 && (port < 1 || port > 65535)) return false;
        if (*e && !strchr(value, ':')) return false;
        if (type == SOCK_SEQPACKET) return false;
        /* a plain PORT: [::]:PORT taking IPv4 too (dual stack, as systemd does), IPv4 only when IPv6 is missing */
        if (*e == 0) { l->family = AF_INET6; l->dual = true; } else l->family = AF_INET; }
    if (!copy_str(l->addr, sizeof(l->addr), value)) return false;
    a->nl++; return true;
}
static bool listen_add_fifo(struct unit* u, const char* value) {
    struct actpart* a = u->act; if (a->nl >= MAX_LISTEN || value[0] != '/' || strstr(value, "/../") || ends_with(value, "/..")) return false;
    struct listen_spec* l = &a->l[a->nl]; memset(l, 0, sizeof(*l)); l->fd = -1; l->kind = 'f';
    if (!copy_str(l->addr, sizeof(l->addr), value)) return false;
    a->nl++; return true;
}
/* ListenNetlink=FAMILY [GROUP]: systemd's family names; GROUP is the bind(2) nl_groups value */
static bool listen_add_netlink(struct unit* u, const char* value) {
    static const struct { const char* name; int proto; } families[] = {{"route", 0}, {"firewall", 3}, {"inet-diag", 4}, {"sock-diag", 4}, {"nflog", 5},
        {"xfrm", 6}, {"selinux", 7}, {"iscsi", 8}, {"audit", 9}, {"fib-lookup", 10}, {"connector", 11}, {"netfilter", 12}, {"ip6-fw", 13},
        {"dnrtmsg", 14}, {"kobject-uevent", 15}, {"generic", 16}, {"scsitransport", 18}, {"ecryptfs", 19}, {"rdma", 20}, {"crypto", 21}};
    struct actpart* a = u->act; if (a->nl >= MAX_LISTEN) return false;
    char name[32]; unsigned long groups = 0; size_t n = strcspn(value, " \t");
    if (!n || n >= sizeof(name)) return false;
    memcpy(name, value, n); name[n] = 0;
    const char* rest = value + n; while (*rest == ' ' || *rest == '\t') rest++;
    if (*rest) { char* e; groups = strtoul(rest, &e, 10); if (*e || groups > 0xFFFFFFFFUL) return false; }
    for (unsigned i = 0; i < sizeof(families) / sizeof(families[0]); i++) if (!strcmp(families[i].name, name)) {
        struct listen_spec* l = &a->l[a->nl]; memset(l, 0, sizeof(*l)); l->fd = -1; l->kind = 'n'; l->type = SOCK_RAW;
        l->family = AF_NETLINK; l->proto = families[i].proto; l->groups = (unsigned)groups;
        copy_str(l->addr, sizeof(l->addr), value); a->nl++; return true;
    }
    return false;
}
static uint64_t key_ms(const char* value, bool* ok) {
    long long ms = parse_time_ms(value); *ok = ms >= -1; return ms < 0 ? INFINITE_MS : (uint64_t)ms;
}
/* [Socket] [Timer] [Path] keys and the services' Sockets=/StandardInput=. Returns true when the key was handled. */
static bool act_set_key(struct unit* u, const char* section, const char* key, const char* value) {
    bool reset = !*value, ok = true;
    if (!strcmp(section, "Service")) {
        if (!strcmp(key, "Sockets")) { if (reset) sl_clear(&u->sockets_opt); else sl_words(&u->sockets_opt, value); return true; }
        if (!strcmp(key, "StandardInput")) { u->stdin_socket = !strcmp(value, "socket"); return !strcmp(value, "socket") || !strcmp(value, "null") || !strcmp(value, "tty"); }
        if (!strcmp(key, "NonBlocking") || !strcmp(key, "FileDescriptorStoreMax")) return true;
        return false;
    }
    struct actpart* a = u->act;
    if (!a) return false;
    if (!strcmp(section, "Socket") && u->kind == K_SOCKET) {
        if (!strcmp(key, "ListenStream") || !strcmp(key, "ListenDatagram") || !strcmp(key, "ListenSequentialPacket")) {
            if (reset) { a->nl = 0; return true; }
            int type = key[6] == 'S' && key[7] == 't' ? SOCK_STREAM : key[6] == 'D' ? SOCK_DGRAM : SOCK_SEQPACKET;
            if (!listen_add(u, type, value)) { mlog(4, "%s: invalid %s=%s ignored", u->name, key, value); }
            return true;
        }
        if (!strcmp(key, "Accept")) { a->accept = truthy(value); return true; }
        if (!strcmp(key, "Service")) { if (!ends_with(value, ".service") || !valid_unit_name(value)) { mlog(4, "%s: invalid Service=%s ignored", u->name, value); return true; }
            copy_str(u->trigger, sizeof(u->trigger), value); return true; }
        if (!strcmp(key, "MaxConnections")) { long n = atol(value); if (n >= 1 && n <= 4096) a->max_conn = (unsigned)n; return true; }
        if (!strcmp(key, "Backlog")) { long n = atol(value); if (n >= 1 && n <= 4096) a->backlog = (unsigned)n; return true; }
        if (!strcmp(key, "SocketMode")) { a->socket_mode = (mode_t)strtol(value, NULL, 8) & 07777; return true; }
        if (!strcmp(key, "DirectoryMode")) { a->dir_mode = (mode_t)strtol(value, NULL, 8) & 07777; return true; }
        if (!strcmp(key, "SocketUser")) { copy_str(a->sock_user, sizeof(a->sock_user), value); return true; }
        if (!strcmp(key, "SocketGroup")) { copy_str(a->sock_group, sizeof(a->sock_group), value); return true; }
        if (!strcmp(key, "PassCredentials")) { a->pass_cred = truthy(value); return true; }
        if (!strcmp(key, "KeepAlive")) { a->keepalive = truthy(value); return true; }
        if (!strcmp(key, "ReusePort")) { a->reuseport = truthy(value); return true; }
        if (!strcmp(key, "RemoveOnStop")) { a->remove_on_stop = truthy(value); return true; }
        if (!strcmp(key, "FileDescriptorName")) { bool good = *value && strlen(value) < sizeof(a->fdname);
            for (const char* p = value; *p; p++) if ((unsigned char)*p <= ' ' || *p == ':' || *p == 0x7f) good = false;
            if (good) copy_str(a->fdname, sizeof(a->fdname), value); else mlog(4, "%s: invalid FileDescriptorName ignored", u->name); return true; }
        if (!strcmp(key, "ReceiveBuffer") || !strcmp(key, "SendBuffer")) { long long n = parse_size(value); if (n > 0 && n < (1 << 26)) { if (key[0] == 'R') a->rcvbuf = (int)n; else a->sndbuf = (int)n; } return true; }
        if (!strcmp(key, "TriggerLimitIntervalSec")) { a->trig_interval = key_ms(value, &ok); return true; }
        if (!strcmp(key, "TriggerLimitBurst")) { a->trig_burst = (unsigned)atoi(value); return true; }
        if (!strcmp(key, "BindIPv6Only")) { a->bind6 = !strcmp(value, "both") ? 1 : !strcmp(value, "ipv6-only") ? 2 : 0;
            if (a->bind6 == 0 && strcmp(value, "default") && *value) mlog(4, "%s: invalid BindIPv6Only=%s ignored", u->name, value); return true; }
        if (!strcmp(key, "ListenFIFO")) { if (reset) { a->nl = 0; return true; } if (!listen_add_fifo(u, value)) mlog(4, "%s: invalid ListenFIFO=%s ignored", u->name, value); return true; }
        if (!strcmp(key, "ListenNetlink")) { if (reset) { a->nl = 0; return true; } if (!listen_add_netlink(u, value)) mlog(4, "%s: invalid ListenNetlink=%s ignored", u->name, value); return true; }
        if (!strcmp(key, "NoDelay") || !strcmp(key, "FreeBind") || !strcmp(key, "Transparent") || !strcmp(key, "Symlinks") || !strcmp(key, "Timestamping") ||
            !strcmp(key, "MaxConnectionsPerSource") || !strcmp(key, "SocketProtocol") || !strcmp(key, "PollLimitIntervalSec") || !strcmp(key, "PollLimitBurst")) return true;
        if (!strcmp(key, "ListenSpecial") || !strcmp(key, "ListenMessageQueue") || !strcmp(key, "ListenUSBFunction")) {
            mlog(4, "%s: %s is not supported, ignored", u->name, key); return true; }
        return false;
    }
    if (!strcmp(section, "Timer") && u->kind == K_TIMER) {
        if (!strcmp(key, "OnCalendar")) {
            if (reset) { a->ncal = 0; return true; }
            if (a->ncal < MAX_CAL && cal_parse(value, &a->cal[a->ncal])) { copy_str(a->cal_text[a->ncal], sizeof(a->cal_text[0]), value); a->ncal++; }
            else mlog(4, "%s: invalid OnCalendar=%s ignored", u->name, value);
            return true;
        }
        uint64_t* slot = !strcmp(key, "OnBootSec") ? &a->on_boot : !strcmp(key, "OnStartupSec") ? &a->on_startup : !strcmp(key, "OnActiveSec") ? &a->on_active :
                         !strcmp(key, "OnUnitActiveSec") ? &a->on_unit_active : !strcmp(key, "OnUnitInactiveSec") ? &a->on_unit_inactive :
                         !strcmp(key, "RandomizedDelaySec") ? &a->random_delay : NULL;
        if (slot) { uint64_t v = reset ? INFINITE_MS : key_ms(value, &ok); if (!ok) { mlog(4, "%s: invalid %s=%s ignored", u->name, key, value); return true; }
            *slot = slot == &a->random_delay && v == INFINITE_MS ? 0 : v; return true; }
        if (!strcmp(key, "Persistent")) { a->persistent = truthy(value); return true; }
        if (!strcmp(key, "RemainAfterElapse")) { a->remain_after_elapse = truthy(value); return true; }
        if (!strcmp(key, "Unit")) { if (!valid_unit_name(value) || ends_with(value, ".timer")) { mlog(4, "%s: invalid Unit=%s ignored", u->name, value); return true; }
            copy_str(u->trigger, sizeof(u->trigger), value); return true; }
        if (!strcmp(key, "AccuracySec") || !strcmp(key, "WakeSystem") || !strcmp(key, "OnClockChange") || !strcmp(key, "OnTimezoneChange") || !strcmp(key, "FixedRandomDelay")) return true;
        return false;
    }
    if (!strcmp(section, "Path") && u->kind == K_PATH) {
        char kind = !strcmp(key, "PathExists") ? 'e' : !strcmp(key, "PathExistsGlob") ? 'g' : !strcmp(key, "PathChanged") ? 'c' :
                    !strcmp(key, "PathModified") ? 'm' : !strcmp(key, "DirectoryNotEmpty") ? 'n' : 0;
        if (kind) {
            if (reset) { a->np = 0; return true; }
            if (value[0] != '/' || a->np >= MAX_PATHS || strlen(value) >= UPATH || strstr(value, "/../")) { mlog(4, "%s: invalid %s=%s ignored", u->name, key, value); return true; }
            a->p[a->np].kind = kind; copy_str(a->p[a->np].path, UPATH, value); a->np++; return true;
        }
        if (!strcmp(key, "Unit")) { if (!valid_unit_name(value) || ends_with(value, ".path")) { mlog(4, "%s: invalid Unit=%s ignored", u->name, value); return true; }
            copy_str(u->trigger, sizeof(u->trigger), value); return true; }
        if (!strcmp(key, "MakeDirectory")) { a->make_dir = truthy(value); return true; }
        if (!strcmp(key, "DirectoryMode")) { a->dir_mode = (mode_t)strtol(value, NULL, 8) & 07777; return true; }
        if (!strcmp(key, "TriggerLimitIntervalSec")) { a->trig_interval = key_ms(value, &ok); return true; }
        if (!strcmp(key, "TriggerLimitBurst")) { a->trig_burst = (unsigned)atoi(value); return true; }
        return false;
    }
    return false;
}
/* after parsing: kind defaults */
static void act_finish(struct unit* u) {
    if (!u->act) return;
    if (u->kind == K_SOCKET && !u->act->trig_burst) u->act->trig_burst = u->act->accept ? 200 : 20;   /* Accept=yes: every connection triggers */
    if (u->kind == K_PATH && !u->act->trig_burst) u->act->trig_burst = 200;
}

/* ------------------------------------------------------------------------------------------------ sockets */
static void socket_close_all(struct unit* u, bool remove) {
    struct actpart* a = u->act;
    for (unsigned i = 0; i < a->nl; i++) {
        if (a->l[i].fd >= 0) { close(a->l[i].fd); a->l[i].fd = -1; }
        if (remove && (a->l[i].kind == 'f' || a->l[i].family == AF_UNIX) && a->l[i].addr[0] == '/') {
            struct stat st; if (!lstat(a->l[i].addr, &st) && (a->l[i].kind == 'f' ? S_ISFIFO(st.st_mode) : S_ISSOCK(st.st_mode))) unlink(a->l[i].addr); }
    }
}
static int socket_address(const struct listen_spec* l, struct sockaddr_storage* ss, socklen_t* len) {
    memset(ss, 0, sizeof(*ss));
    if (l->family == AF_UNIX) {
        struct sockaddr_un* a = (struct sockaddr_un*)ss; a->sun_family = AF_UNIX; size_t n = strlen(l->addr);
        memcpy(a->sun_path, l->addr, n); if (l->addr[0] == '@') a->sun_path[0] = 0;
        *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + (l->addr[0] == '@' ? 0 : 1)); return 0;
    }
    char host[UPATH]; long port;
    if (l->family == AF_INET6 && l->dual && l->addr[0] != '[') {   /* a plain PORT */
        struct sockaddr_in6* a = (struct sockaddr_in6*)ss; a->sin6_family = AF_INET6; port = atol(l->addr); a->sin6_port = htons((uint16_t)port);
        if (port < 1 || port > 65535) return -1;
        *len = sizeof(*a); return 0;
    }
    if (l->family == AF_INET6) {
        const char* close_br = strchr(l->addr, ']'); if (!close_br || close_br[1] != ':') return -1;
        snprintf(host, sizeof(host), "%.*s", (int)(close_br - l->addr - 1), l->addr + 1); port = atol(close_br + 2);
        struct sockaddr_in6* a = (struct sockaddr_in6*)ss; a->sin6_family = AF_INET6; a->sin6_port = htons((uint16_t)port);
        if (port < 1 || port > 65535 || (host[0] && inet_pton(AF_INET6, host, &a->sin6_addr) != 1)) return -1;
        *len = sizeof(*a); return 0;
    }
    const char* colon = strrchr(l->addr, ':');
    if (colon) { snprintf(host, sizeof(host), "%.*s", (int)(colon - l->addr), l->addr); port = atol(colon + 1); } else { host[0] = 0; port = atol(l->addr); }
    struct sockaddr_in* a = (struct sockaddr_in*)ss; a->sin_family = AF_INET; a->sin_port = htons((uint16_t)port);
    if (port < 1 || port > 65535 || (host[0] && inet_pton(AF_INET, host, &a->sin_addr) != 1)) return -1;
    *len = sizeof(*a); return 0;
}
static bool socket_open_all(struct unit* u, char* why, size_t wsize) {
    struct actpart* a = u->act;
    if (!a->nl) { snprintf(why, wsize, "no Listen*= address"); return false; }
    uid_t owner = (uid_t)-1; gid_t group = (gid_t)-1;
    if (a->sock_user[0]) { struct passwd* pw = getpwnam(a->sock_user); if (!pw) { snprintf(why, wsize, "unknown SocketUser %s", a->sock_user); return false; } owner = pw->pw_uid; }
    if (a->sock_group[0]) { struct group* gr = getgrnam(a->sock_group); if (!gr) { snprintf(why, wsize, "unknown SocketGroup %s", a->sock_group); return false; } group = gr->gr_gid; }
    if (a->accept) for (unsigned i = 0; i < a->nl; i++) if (a->l[i].kind != 's' || a->l[i].type != SOCK_STREAM) {
        snprintf(why, wsize, "Accept=yes needs stream sockets only (%s)", a->l[i].addr); return false; }
    for (unsigned i = 0; i < a->nl; i++) {
        struct listen_spec* l = &a->l[i]; struct sockaddr_storage ss; socklen_t len;
        if (l->kind == 'f') {   /* ListenFIFO: made if missing, opened read-write (never end-of-file, never blocking) */
            char dir[UPATH]; copy_str(dir, sizeof(dir), l->addr); char* slash = strrchr(dir, '/');
            if (slash && slash != dir) { *slash = 0; mkdir_p(dir, a->dir_mode); }
            struct stat st;
            if (!lstat(l->addr, &st) && !S_ISFIFO(st.st_mode)) { snprintf(why, wsize, "%s exists and is not a FIFO", l->addr); socket_close_all(u, false); return false; }
            if (lstat(l->addr, &st) && mkfifo(l->addr, a->socket_mode)) { snprintf(why, wsize, "mkfifo %s: %s", l->addr, strerror(errno)); socket_close_all(u, false); return false; }
            int fd = open(l->addr, O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
            if (fd < 0) { snprintf(why, wsize, "open %s: %s", l->addr, strerror(errno)); socket_close_all(u, false); return false; }
            if (chmod(l->addr, a->socket_mode)) mlog(4, "%s: chmod %s: %s", u->name, l->addr, strerror(errno));   /* by path: set even under a umask */
            if ((owner != (uid_t)-1 || group != (gid_t)-1) && root_mode && lchown(l->addr, owner, group)) mlog(4, "%s: chown %s: %s", u->name, l->addr, strerror(errno));
            l->fd = fd; continue;
        }
        if (l->kind == 'n') {   /* ListenNetlink */
            int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, l->proto);
            if (fd < 0) { snprintf(why, wsize, "netlink %s: %s", l->addr, strerror(errno)); socket_close_all(u, false); return false; }
            struct sockaddr_nl nl = {.nl_family = AF_NETLINK, .nl_groups = l->groups};
            if (a->rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &a->rcvbuf, sizeof(a->rcvbuf));
            if (a->pass_cred) { int on = 1; setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)); }
            if (bind(fd, (struct sockaddr*)&nl, sizeof(nl))) { snprintf(why, wsize, "bind netlink %s: %s", l->addr, strerror(errno)); close(fd); socket_close_all(u, false); return false; }
            l->fd = fd; continue;
        }
        if (socket_address(l, &ss, &len)) { snprintf(why, wsize, "bad address %s", l->addr); socket_close_all(u, false); return false; }
        if (l->family == AF_UNIX && l->addr[0] == '/') {
            char dir[UPATH]; copy_str(dir, sizeof(dir), l->addr); char* slash = strrchr(dir, '/');
            if (slash && slash != dir) { *slash = 0; mkdir_p(dir, a->dir_mode); }
            struct stat st; if (!lstat(l->addr, &st)) {
                if (!S_ISSOCK(st.st_mode)) { snprintf(why, wsize, "%s exists and is not a socket", l->addr); socket_close_all(u, false); return false; }
                unlink(l->addr);
            }
        }
        int fd = socket(l->family, l->type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0 && l->dual && errno == EAFNOSUPPORT && a->bind6 != 2) {   /* no IPv6: the plain PORT on IPv4 */
            struct sockaddr_in* v4 = (struct sockaddr_in*)&ss; memset(&ss, 0, sizeof(ss)); v4->sin_family = AF_INET; v4->sin_port = htons((uint16_t)atol(l->addr)); len = sizeof(*v4);
            fd = socket(AF_INET, l->type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        }
        if (fd < 0) { snprintf(why, wsize, "socket %s: %s", l->addr, strerror(errno)); socket_close_all(u, false); return false; }
        int on = 1, off = 0;
        if (l->family != AF_UNIX) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (ss.ss_family == AF_INET6) {   /* BindIPv6Only=: ipv6-only, both; default: a plain PORT both, [ADDR]:PORT the kernel's default */
            if (a->bind6 == 2) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
            else if (a->bind6 == 1 || l->dual) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        }
        if (a->reuseport) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        if (a->pass_cred) setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));
        if (a->keepalive) setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        if (a->rcvbuf) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &a->rcvbuf, sizeof(a->rcvbuf));
        if (a->sndbuf) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &a->sndbuf, sizeof(a->sndbuf));
        if (bind(fd, (struct sockaddr*)&ss, len) || (l->type != SOCK_DGRAM && listen(fd, (int)a->backlog))) {
            snprintf(why, wsize, "%s %s: %s", l->type == SOCK_DGRAM ? "bind" : "bind/listen", l->addr, strerror(errno)); close(fd); socket_close_all(u, false); return false;
        }
        if (l->family == AF_UNIX && l->addr[0] == '/') {
            if (chmod(l->addr, a->socket_mode)) mlog(4, "%s: chmod %s: %s", u->name, l->addr, strerror(errno));
            if ((owner != (uid_t)-1 || group != (gid_t)-1) && root_mode && lchown(l->addr, owner, group)) mlog(4, "%s: chown %s: %s", u->name, l->addr, strerror(errno));
        }
        l->fd = fd;
    }
    return true;
}
/* Accept=yes: a connection arrived on listening socket fd */
static void socket_accept(struct unit* u, struct listen_spec* l) {
    struct actpart* a = u->act;
    for (;;) {
        struct sockaddr_storage peer; socklen_t plen = sizeof(peer);
        int c = accept4(l->fd, (struct sockaddr*)&peer, &plen, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR) continue; if (errno != EAGAIN && errno != EWOULDBLOCK) mlog(4, "%s: accept: %s", u->name, strerror(errno)); return; }
        if (a->connections >= a->max_conn) { mlog(4, "%s: too many connections (%u), refusing one", u->name, a->connections); close(c); continue; }
        if (trigger_limited(u)) { close(c); socket_close_all(u, false); unit_enter(u, S_FAILED, "trigger-limit-hit"); return; }
        char tpl[160], inst[160], who[96] = "";
        if (u->trigger[0]) copy_str(tpl, sizeof(tpl), u->trigger); else same_stem(u->name, "@.service", tpl, sizeof(tpl));
        if (l->family == AF_UNIX) { struct ucred cr; socklen_t cl = sizeof(cr);
            if (!getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl)) snprintf(who, sizeof(who), "%ld-%u", (long)cr.pid, (unsigned)cr.uid); else copy_str(who, sizeof(who), "unknown"); }
        else { struct sockaddr_storage me; socklen_t ml = sizeof(me); char la[64] = "?", ra[64] = "?"; unsigned lp = 0, rp = 0;
            if (!getsockname(c, (struct sockaddr*)&me, &ml)) {
                if (me.ss_family == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in*)&me)->sin_addr, la, sizeof(la)); lp = ntohs(((struct sockaddr_in*)&me)->sin_port); }
                else if (me.ss_family == AF_INET6) { inet_ntop(AF_INET6, &((struct sockaddr_in6*)&me)->sin6_addr, la, sizeof(la)); lp = ntohs(((struct sockaddr_in6*)&me)->sin6_port); } }
            if (peer.ss_family == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in*)&peer)->sin_addr, ra, sizeof(ra)); rp = ntohs(((struct sockaddr_in*)&peer)->sin_port); }
            else if (peer.ss_family == AF_INET6) { inet_ntop(AF_INET6, &((struct sockaddr_in6*)&peer)->sin6_addr, ra, sizeof(ra)); rp = ntohs(((struct sockaddr_in6*)&peer)->sin6_port); }
            snprintf(who, sizeof(who), "%s:%u-%s:%u", la, lp, ra, rp); }
        const char* at = strchr(tpl, '@');
        if (!at || strcmp(at, "@.service")) { mlog(3, "%s: Accept=yes needs a template service (%s)", u->name, tpl); close(c); return; }
        snprintf(inst, sizeof(inst), "%.*s@%u-%s.service", (int)(at - tpl), tpl, accept_serial++, who);
        for (char* p = inst; *p; p++) if (!(isalnum((unsigned char)*p) || strchr("-_.@:", *p))) *p = '_';
        struct unit* s = load_unit(inst);
        if (!s) { mlog(3, "%s: cannot load %s (template %s missing?)", u->name, inst, tpl); close(c); return; }
        s->transient = true; s->conn_fd = c; copy_str(s->accepted_by, sizeof(s->accepted_by), u->name); a->connections++;
        request_start(s, 0);
    }
}
/* Accept=no: data or a connection is waiting: start the service once */
static void socket_trigger(struct unit* u) {
    struct unit* s = act_target(u);
    if (!s) { mlog(3, "%s: its service is missing", u->name); socket_close_all(u, false); unit_enter(u, S_FAILED, "resources"); return; }
    if (s->state == S_ACTIVE || s->state == S_STARTING || s->state == S_WAITING || s->state == S_STOPPING || s->state == S_RESTART_WAIT) return;
    if (s->state == S_FAILED && !strcmp(s->result, "start-limit-hit")) { mlog(3, "%s: its service hit its start limit", u->name); socket_close_all(u, false); unit_enter(u, S_FAILED, "service-start-limit-hit"); return; }
    if (trigger_limited(u)) { socket_close_all(u, false); unit_enter(u, S_FAILED, "trigger-limit-hit"); return; }
    mlog(6, "%s: incoming traffic, starting %s", u->name, s->name);
    s->nstarts = 0; request_start(s, 0);
}
/* is this socket unit's Accept=no service busy (then its fds are not watched) */
static bool socket_service_busy(struct unit* u) {
    if (u->act->accept) return false;
    char name[160]; if (u->trigger[0]) copy_str(name, sizeof(name), u->trigger); else same_stem(u->name, ".service", name, sizeof(name));
    struct unit* s = find_unit(name);
    return s && s->state != S_INACTIVE && s->state != S_FAILED;
}
/* fds a service's main process gets: the connection of an Accept=yes instance, else the listening sockets of the
 * active Accept=no sockets that name it (or share its stem) and of its Sockets= list. */
static unsigned service_fds(struct unit* s, int* fds, char* names, size_t nsize, unsigned max) {
    unsigned n = 0; names[0] = 0;
    if (s->conn_fd >= 0) { fds[n++] = s->conn_fd; snprintf(names, nsize, "connection"); return n; }
    for (unsigned i = 0; i < nunits; i++) {
        struct unit* u = units[i]; if (u->kind != K_SOCKET || !u->act || u->act->accept || u->state != S_ACTIVE) continue;
        char target[160]; if (u->trigger[0]) copy_str(target, sizeof(target), u->trigger); else same_stem(u->name, ".service", target, sizeof(target));
        if (strcmp(target, s->name) && !sl_has(&s->sockets_opt, u->name)) continue;
        for (unsigned k = 0; k < u->act->nl && n < max; k++) if (u->act->l[k].fd >= 0) {
            fds[n++] = u->act->l[k].fd;
            size_t used = strlen(names); snprintf(names + used, nsize - used, "%s%s", used ? ":" : "", u->act->fdname[0] ? u->act->fdname : u->name);
        }
    }
    return n;
}

/* ------------------------------------------------------------------------------------------------ timers */
static void timer_stamp_path(struct unit* u, char* out, size_t size) { snprintf(out, size, "%s/stamp-%s", timers_dir, u->name); }
static void timer_stamp_write(struct unit* u, int64_t when) {
    char p[PATH_MAX]; timer_stamp_path(u, p, sizeof(p)); mkdir_p(timers_dir, 0755);
    char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof(tmp), "%s.new", p);
    FILE* f = fopen(tmp, "we"); if (!f) return;
    fprintf(f, "%lld\n", (long long)when); bool ok = !fflush(f) && !fsync(fileno(f)); fclose(f);
    if (!ok || rename(tmp, p)) unlink(tmp);
}
static int64_t timer_stamp_read(struct unit* u) {
    char p[PATH_MAX]; timer_stamp_path(u, p, sizeof(p)); FILE* f = fopen(p, "re");
    if (!f) {   /* systemd's own stamp (its modification time), e.g. on a disk that ran Debian with systemd */
        struct stat st; snprintf(p, sizeof(p), "%s/stamp-%s", root_mode ? "/var/lib/systemd/timers" : "", u->name);
        return root_mode && !stat(p, &st) && S_ISREG(st.st_mode) && st.st_mtime > 0 ? (int64_t)st.st_mtime : -1;
    }
    long long v = -1; if (fscanf(f, "%lld", &v) != 1 || v < 0) v = -1; fclose(f); return v;
}
static uint64_t random_ms(uint64_t limit) { if (!limit || limit == INFINITE_MS) return 0; uint64_t r; if (getrandom(&r, sizeof(r), 0) != sizeof(r)) r = mono_us(); return r % limit; }
static void timer_compute(struct unit* u) {
    struct actpart* a = u->act; struct unit* t = act_target(u); uint64_t next = INFINITE_MS;
    /* CLOCK_MONOTONIC (now_ms) counts from the kernel's boot: OnBootSec= compares with it directly */
    if (a->on_boot != INFINITE_MS && !a->boot_done) next = a->on_boot;
    if (a->on_startup != INFINITE_MS && !a->startup_done && manager_start_ms + a->on_startup < next) next = manager_start_ms + a->on_startup;
    if (a->on_active != INFINITE_MS && !a->active_done && a->activated_ms + a->on_active < next) next = a->activated_ms + a->on_active;
    if (t && a->on_unit_active != INFINITE_MS && t->active_enter_ms && t->active_enter_ms + a->on_unit_active < next) next = t->active_enter_ms + a->on_unit_active;
    if (t && a->on_unit_inactive != INFINITE_MS && t->inactive_enter_ms && t->inactive_enter_ms + a->on_unit_inactive < next) next = t->inactive_enter_ms + a->on_unit_inactive;
    if (next != INFINITE_MS && a->random_delay) next += random_ms(a->random_delay);
    a->next_mono = next;
    int64_t now_s = (int64_t)realtime_s(), base = now_s, best = -1;
    if (a->persistent && a->last_trigger_real >= 0 && a->last_trigger_real < now_s) base = a->last_trigger_real;   /* a missed elapse is due now */
    for (unsigned i = 0; i < a->ncal; i++) { int64_t v = cal_next(&a->cal[i], base); if (v >= 0 && (best < 0 || v < best)) best = v; }
    a->next_real = best; a->delay_cal = best >= 0 ? random_ms(a->random_delay) : 0;
    if (t) { a->seen_active_enter = t->active_enter_ms; a->seen_inactive_enter = t->inactive_enter_ms; }
}
static void timer_start(struct unit* u) {
    struct actpart* a = u->act;
    if (!a->ncal && a->on_boot == INFINITE_MS && a->on_startup == INFINITE_MS && a->on_active == INFINITE_MS && a->on_unit_active == INFINITE_MS && a->on_unit_inactive == INFINITE_MS) {
        mlog(3, "%s: no trigger (On*= setting)", u->name); unit_enter(u, S_FAILED, "resources"); return; }
    if (!act_target(u)) { mlog(3, "%s: the unit it starts does not exist", u->name); unit_enter(u, S_FAILED, "resources"); return; }
    a->activated_ms = now_ms(); a->active_done = false;
    a->last_trigger_real = a->persistent ? timer_stamp_read(u) : -1;
    timer_compute(u);
    u->active_since_ms = now_ms(); unit_enter(u, S_ACTIVE, NULL);
    if (a->persistent && a->last_trigger_real >= 0 && a->next_real >= 0 && a->next_real <= (int64_t)realtime_s())
        mlog(5, "%s: an elapse was missed while the system was off; running it now", u->name);
}
static void timer_fire(struct unit* u, bool calendar) {
    struct actpart* a = u->act; struct unit* t = act_target(u); uint64_t m = now_ms();
    int64_t r = calendar && a->next_real >= 0 ? a->next_real : (int64_t)realtime_s();   /* a calendar elapse: its scheduled time */
    if (calendar && r < (int64_t)realtime_s() - 2) r = (int64_t)realtime_s();             /* a missed (Persistent) one: now */
    if (!calendar) {   /* which relative trigger elapsed: the one-shot ones are done */
        if (a->on_boot != INFINITE_MS && !a->boot_done && m >= a->on_boot) a->boot_done = true;
        if (a->on_startup != INFINITE_MS && !a->startup_done && m >= manager_start_ms + a->on_startup) a->startup_done = true;
        if (a->on_active != INFINITE_MS && !a->active_done && m >= a->activated_ms + a->on_active) a->active_done = true;
    }
    a->last_trigger_mono = m; a->last_trigger_real = r;
    if (a->persistent) timer_stamp_write(u, r);
    if (!t) { mlog(3, "%s: the unit it starts is missing", u->name); unit_enter(u, S_FAILED, "resources"); return; }
    if (t->state == S_ACTIVE || t->state == S_STARTING || t->state == S_WAITING) mlog(6, "%s: %s still running, elapse skipped", u->name, t->name);
    else { mlog(6, "%s: elapsed, starting %s", u->name, t->name); t->nstarts = 0; request_start(t, 0); }
    timer_compute(u);
    if (a->next_mono == INFINITE_MS && a->next_real < 0 && !a->remain_after_elapse) unit_enter(u, S_INACTIVE, "success");
}
/* called every loop: fires elapsed timers, lowers *next (ms) to the earliest pending elapse */
static void timers_run(uint64_t* next) {
    uint64_t m = now_ms(); int64_t rms = realtime_ms();
    for (unsigned i = 0; i < nunits; i++) {
        struct unit* u = units[i]; if (u->kind != K_TIMER || u->state != S_ACTIVE || !u->act) continue;
        struct actpart* a = u->act; struct unit* t = find_unit(u->trigger[0] ? u->trigger : "");
        if (!t) { char name[160]; same_stem(u->name, ".service", name, sizeof(name)); t = find_unit(name); }
        if (t && (t->active_enter_ms != a->seen_active_enter || t->inactive_enter_ms != a->seen_inactive_enter)) timer_compute(u);   /* OnUnit*Sec rebase */
        if (a->next_mono != INFINITE_MS && m >= a->next_mono) { timer_fire(u, false); if (u->state != S_ACTIVE) continue; }
        if (a->next_real >= 0 && rms >= a->next_real * 1000 + (int64_t)a->delay_cal) { timer_fire(u, true); if (u->state != S_ACTIVE) continue; }
        if (a->next_mono != INFINITE_MS && a->next_mono < *next) *next = a->next_mono;
        if (a->next_real >= 0) {   /* wall-clock elapses: re-check at least once a minute (the clock may be set meanwhile) */
            int64_t left = a->next_real * 1000 + (int64_t)a->delay_cal - rms; uint64_t at = m + (uint64_t)(left > 60000 ? 60000 : left > 0 ? left : 0);
            if (at < *next) *next = at;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ paths */
static bool path_level(const struct pathspec* p) {
    struct stat st;
    if (p->kind == 'e') return !stat(p->path, &st);
    if (p->kind == 'g') { glob_t g; int r = glob(p->path, GLOB_NOSORT, NULL, &g); bool any = r == 0 && g.gl_pathc > 0; if (r == 0) globfree(&g); return any; }
    if (p->kind == 'n') { DIR* d = opendir(p->path); if (!d) return false; struct dirent* e; bool any = false;
        while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { any = true; break; } closedir(d); return any; }
    return false;
}
static void path_watch(struct unit* u) {
    struct actpart* a = u->act;
    if (a->ino_fd >= 0) close(a->ino_fd);
    a->ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (a->ino_fd < 0) { mlog(4, "%s: inotify: %s (checked every 5 s instead)", u->name, strerror(errno)); return; }
    for (unsigned i = 0; i < a->np; i++) {
        struct pathspec* p = &a->p[i]; char dir[UPATH]; copy_str(dir, sizeof(dir), p->path);
        uint32_t self = p->kind == 'm' ? IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF :
                        p->kind == 'c' ? IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF :
                        IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF;
        p->wd = p->kind != 'g' ? inotify_add_watch(a->ino_fd, p->path, self | IN_MASK_ADD) : -1;   /* fails until it exists: the parent watch sees it appear */
        char* slash = strrchr(dir, '/'); if (slash == dir) slash[1] = 0; else if (slash) *slash = 0;
        /* the parent, else the nearest existing ancestor: creating the missing folders triggers a re-watch */
        const uint32_t parent = IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM | IN_CLOSE_WRITE | IN_ATTRIB | IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_MASK_ADD;
        p->pwd = -1; bool direct = true;
        for (;;) {
            int wd = inotify_add_watch(a->ino_fd, dir, parent);
            if (wd >= 0) { if (direct) p->pwd = wd; break; }
            direct = false;
            char* up = strrchr(dir, '/'); if (!up || up == dir) { inotify_add_watch(a->ino_fd, "/", parent); break; }
            *up = 0;
        }
    }
}
static void path_check(struct unit* u, bool edge) {
    struct actpart* a = u->act; bool level = false;
    for (unsigned i = 0; i < a->np; i++) if (a->p[i].kind == 'e' || a->p[i].kind == 'g' || a->p[i].kind == 'n') level |= path_level(&a->p[i]);
    struct unit* t = act_target(u);
    if (!t) { mlog(3, "%s: the unit it starts is missing", u->name); unit_enter(u, S_FAILED, "resources"); return; }
    if (!(level || edge)) return;
    if (t->state == S_ACTIVE || t->state == S_STARTING || t->state == S_WAITING || t->state == S_STOPPING || t->state == S_RESTART_WAIT) return;
    if (trigger_limited(u)) { if (a->ino_fd >= 0) { close(a->ino_fd); a->ino_fd = -1; } unit_enter(u, S_FAILED, "trigger-limit-hit"); return; }
    mlog(6, "%s: %s, starting %s", u->name, edge ? "changed" : "condition met", t->name);
    t->nstarts = 0; request_start(t, 0);
}
static void path_input(struct unit* u) {
    struct actpart* a = u->act; bool edge = false, rewatch = false;
    for (;;) {
        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event)))); ssize_t n = read(a->ino_fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            struct inotify_event* e = (struct inotify_event*)p; p += sizeof(*e) + e->len;
            if (e->mask & (IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED | IN_Q_OVERFLOW)) rewatch = true;
            for (unsigned i = 0; i < a->np; i++) {
                const struct pathspec* s = &a->p[i]; if (s->kind != 'c' && s->kind != 'm') continue;
                const char* base = strrchr(s->path, '/'); base = base ? base + 1 : s->path;
                /* an event on the path itself (or, for a folder, on an entry in it), or on its name in the parent */
                bool on_self = e->wd == s->wd && s->wd >= 0, named = e->wd == s->pwd && s->pwd >= 0 && e->len && !strcmp(e->name, base);
                uint32_t want = s->kind == 'm' ? (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF)
                                               : (IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF);
                if ((e->mask & want) && (on_self || named)) edge = true;
            }
        }
    }
    if (rewatch) path_watch(u);
    path_check(u, edge);
}
static void path_start(struct unit* u) {
    struct actpart* a = u->act;
    if (!a->np) { mlog(3, "%s: no Path*= setting", u->name); unit_enter(u, S_FAILED, "resources"); return; }
    if (!act_target(u)) { mlog(3, "%s: the unit it starts does not exist", u->name); unit_enter(u, S_FAILED, "resources"); return; }
    if (a->make_dir) for (unsigned i = 0; i < a->np; i++) if (a->p[i].kind != 'g') {
        char dir[UPATH]; copy_str(dir, sizeof(dir), a->p[i].path);
        if (a->p[i].kind != 'n') { char* slash = strrchr(dir, '/'); if (slash && slash != dir) *slash = 0; }
        mkdir_p(dir, a->dir_mode);
    }
    a->trig_window = now_ms(); a->trig_count = 0;
    path_watch(u);
    u->active_since_ms = now_ms(); unit_enter(u, S_ACTIVE, NULL);
    path_check(u, false);
}

/* ------------------------------------------------------------------------------------------------ dispatch */
static void act_start(struct unit* u) {
    if (!u->act) { unit_enter(u, S_FAILED, "resources"); return; }
    mlog(6, "Starting %s%s%s...", u->name, u->description[0] ? " - " : "", u->description);
    if (u->kind == K_SOCKET) {
        char why[256] = "", name[160], path[PATH_MAX], tpl[160];
        if (u->trigger[0]) copy_str(name, sizeof(name), u->trigger); else same_stem(u->name, u->act->accept ? "@.service" : ".service", name, sizeof(name));
        const char* at = strchr(name, '@');
        if (u->act->accept && (!at || strcmp(at, "@.service"))) { mlog(3, "%s: Accept=yes needs a template service, not %s", u->name, name); unit_enter(u, S_FAILED, "resources"); return; }
        if (u->act->accept ? !unit_source(name, path, sizeof(path), tpl, sizeof(tpl), NULL) : !load_unit(name)) {
            mlog(3, "%s: its service %s does not exist (template missing?)", u->name, name); unit_enter(u, S_FAILED, "resources"); return; }
        if (!socket_open_all(u, why, sizeof(why))) { mlog(3, "%s: %s", u->name, why); unit_enter(u, S_FAILED, "resources"); return; }
        u->act->trig_window = now_ms(); u->act->trig_count = 0;
        u->active_since_ms = now_ms(); unit_enter(u, S_ACTIVE, NULL);
    } else if (u->kind == K_TIMER) timer_start(u);
    else if (u->kind == K_PATH) path_start(u);
}
static void act_stop(struct unit* u) {
    if (u->act) {
        if (u->kind == K_SOCKET) socket_close_all(u, u->act->remove_on_stop);
        if (u->kind == K_PATH && u->act->ino_fd >= 0) { close(u->act->ino_fd); u->act->ino_fd = -1; }
        if (u->kind == K_TIMER) { u->act->next_mono = INFINITE_MS; u->act->next_real = -1; }
    }
    unit_enter(u, u->result[0] && strcmp(u->result, "success") ? S_FAILED : S_INACTIVE, NULL);
}
/* a Debian (systemd-directory) socket/path unit whose service NexusOS ships itself is not used: those services start
 * their own listeners (dbus, cups) */
static bool act_shadowed(struct unit* u) {
    if (u->kind == K_SERVICE || !u->systemd) return false;
    char name[160], path[PATH_MAX], tpl[160]; unsigned from = 0;
    if (u->trigger[0]) copy_str(name, sizeof(name), u->trigger); else same_stem(u->name, u->kind == K_SOCKET && u->act && u->act->accept ? "@.service" : ".service", name, sizeof(name));
    return unit_source(name, path, sizeof(path), tpl, sizeof(tpl), &from) && from < NEXUS_UNIT_DIRS;
}

/* ------------------------------------------------------------------------------------------------ journal rate limit */
#define RL_SOURCES 128
static struct { char key[96]; uint64_t window; unsigned count, suppressed; bool used; } rl[RL_SOURCES];
#define rl_interval journal.rl_interval
#define rl_burst journal.rl_burst
static void rl_notice(unsigned i) {
    char msg[200]; int n = snprintf(msg, sizeof(msg), "Suppressed %u messages from %s", rl[i].suppressed, rl[i].key);
    rl[i].suppressed = 0;
    if (n > 0) journal_write_raw(4, "nexusd", "nexusd", getpid(), msg, (size_t)n);
}
/* true when the record may be written */
static bool rl_allow(const char* unit, const char* ident, long pid) {
    if (!rl_burst || !rl_interval || rl_interval == INFINITE_MS) return true;
    if (unit && !strcmp(unit, "nexusd")) return true;
    char key[96];
    if (unit && *unit && strcmp(unit, "-")) copy_str(key, sizeof(key), unit);
    else snprintf(key, sizeof(key), "%s[%ld]", ident && *ident ? ident : "-", pid);
    unsigned h = 5381; for (const char* p = key; *p; p++) h = h * 33 + (unsigned char)*p;
    unsigned slot = RL_SOURCES, oldest = 0; uint64_t t = now_ms();
    for (unsigned k = 0; k < RL_SOURCES; k++) {
        unsigned i = (h + k) % RL_SOURCES;
        if (rl[i].used && !strcmp(rl[i].key, key)) { slot = i; break; }
        if (!rl[i].used) { slot = i; break; }
        if (rl[i].window < rl[oldest].window) oldest = i;
    }
    if (slot == RL_SOURCES) { if (rl[oldest].suppressed) rl_notice(oldest); slot = oldest; rl[slot].used = false; }
    if (!rl[slot].used) { memset(&rl[slot], 0, sizeof(rl[slot])); copy_str(rl[slot].key, sizeof(rl[slot].key), key); rl[slot].used = true; rl[slot].window = t; }
    if (t - rl[slot].window >= rl_interval) { if (rl[slot].suppressed) rl_notice(slot); rl[slot].window = t; rl[slot].count = 0; }
    if (++rl[slot].count <= rl_burst) return true;
    rl[slot].suppressed++;
    return false;
}
/* periodic: report sources whose interval ended with suppressed records */
static void rl_flush(void) {
    uint64_t t = now_ms();
    for (unsigned i = 0; i < RL_SOURCES; i++) if (rl[i].used && rl[i].suppressed && t - rl[i].window >= rl_interval) { rl_notice(i); rl[i].window = t; rl[i].count = 0; }
}
#undef rl_interval
#undef rl_burst

/* ------------------------------------------------------------------------------------------------ status, lists */
static void fmt_real(int64_t s, char* out, size_t size) {
    if (s < 0) { copy_str(out, size, "n/a"); return; }
    time_t t = (time_t)s; struct tm tm; localtime_r(&t, &tm); strftime(out, size, "%a %Y-%m-%d %H:%M:%S %Z", &tm);
}
static void fmt_left(int64_t seconds, char* out, size_t size) {
    if (seconds < 0) { copy_str(out, size, "-"); return; }
    if (seconds < 120) snprintf(out, size, "%llds", (long long)seconds);
    else if (seconds < 7200) snprintf(out, size, "%lldmin %llds", (long long)seconds / 60, (long long)seconds % 60);
    else if (seconds < 172800) snprintf(out, size, "%lldh %lldmin", (long long)seconds / 3600, (long long)seconds / 60 % 60);
    else snprintf(out, size, "%lld days", (long long)seconds / 86400);
}
/* the next elapse of a timer as wall-clock seconds (relative triggers converted), or -1 */
static int64_t timer_next_real(struct unit* u) {
    struct actpart* a = u->act; int64_t r = (int64_t)realtime_s(), best = a->next_real >= 0 ? a->next_real + (int64_t)(a->delay_cal / 1000) : -1;
    if (a->next_mono != INFINITE_MS) { uint64_t m = now_ms(); int64_t v = r + (int64_t)(a->next_mono > m ? (a->next_mono - m + 999) / 1000 : 0); if (best < 0 || v < best) best = v; }
    return best;
}
static const char* listen_kind(const struct listen_spec* l) {
    return l->kind == 'f' ? "FIFO" : l->kind == 'n' ? "Netlink" : l->type == SOCK_STREAM ? (l->dual ? "Stream, IPv4+IPv6" : "Stream") :
           l->type == SOCK_DGRAM ? (l->dual ? "Datagram, IPv4+IPv6" : "Datagram") : "SequentialPacket"; }
static void act_status(int fd, struct unit* u) {
    struct actpart* a = u->act; if (!a) return;
    char target[160]; struct unit* t = NULL;
    if (u->trigger[0]) copy_str(target, sizeof(target), u->trigger); else same_stem(u->name, u->kind == K_SOCKET && a->accept ? "@.service" : ".service", target, sizeof(target));
    t = find_unit(target);
    if (u->kind == K_SOCKET) {
        for (unsigned i = 0; i < a->nl; i++) reply(fd, "     Listen: %s (%s)%s\n", a->l[i].addr, listen_kind(&a->l[i]), a->l[i].fd >= 0 ? "" : " closed");
        if (a->accept) reply(fd, "   Accepted: %u; Connected: %u; MaxConnections: %u\n", accept_serial, a->connections, a->max_conn);
        reply(fd, "   Triggers: %s%s%s\n", target, t ? " " : "", t ? state_text(t) : " (not found)");
    } else if (u->kind == K_TIMER) {
        char when[96], left[48], last[96]; int64_t next = u->state == S_ACTIVE ? timer_next_real(u) : -1;
        fmt_real(next, when, sizeof(when)); fmt_left(next >= 0 ? next - (int64_t)realtime_s() : -1, left, sizeof(left)); fmt_real(a->last_trigger_real, last, sizeof(last));
        reply(fd, "    Trigger: %s; %s left\n", when, left);
        reply(fd, "       Last: %s\n", last);
        for (unsigned i = 0; i < a->ncal; i++) reply(fd, " OnCalendar: %s\n", a->cal_text[i]);
        reply(fd, "   Triggers: %s%s%s%s\n", target, t ? " " : "", t ? state_text(t) : " (not found)", a->persistent ? "; Persistent" : "");
    } else if (u->kind == K_PATH) {
        static const char* const kinds = "egcmn"; static const char* const names[] = {"PathExists", "PathExistsGlob", "PathChanged", "PathModified", "DirectoryNotEmpty"};
        for (unsigned i = 0; i < a->np; i++) { const char* k = strchr(kinds, a->p[i].kind); reply(fd, "       Path: %s=%s\n", k ? names[k - kinds] : "?", a->p[i].path); }
        reply(fd, "   Triggers: %s%s%s%s\n", target, t ? " " : "", t ? state_text(t) : " (not found)", a->ino_fd >= 0 ? "" : "; polled");
    }
}
static void act_show(int fd, struct unit* u) {
    struct actpart* a = u->act; if (!a) return;
    if (u->kind == K_TIMER) { int64_t next = u->state == S_ACTIVE ? timer_next_real(u) : -1;   /* microseconds of the wall clock, 0 = none */
        reply(fd, "NextElapseUSecRealtime=%lld\nLastTriggerUSec=%lld\nPersistent=%s\n", next >= 0 ? (long long)next * 1000000 : 0LL,
              a->last_trigger_real >= 0 ? (long long)a->last_trigger_real * 1000000 : 0LL, a->persistent ? "yes" : "no"); }
    if (u->kind == K_SOCKET) reply(fd, "Accept=%s\nNConnections=%u\nNAccepted=%u\n", a->accept ? "yes" : "no", a->connections, accept_serial);
    if (u->kind == K_PATH) reply(fd, "Paths=%u\n", a->np);
}
static void act_list(int fd, enum ukind kind) {
    if (kind == K_TIMER) reply(fd, "%-32s %-10s %-36s %-24s %s\n", "NEXT", "LEFT", "LAST", "UNIT", "ACTIVATES");
    else if (kind == K_SOCKET) reply(fd, "%-40s %-26s %-10s %s\n", "LISTEN", "UNIT", "STATE", "ACTIVATES");
    else reply(fd, "%-26s %-10s %s\n", "UNIT", "STATE", "ACTIVATES");
    for (unsigned i = 0; i < nunits; i++) {
        struct unit* u = units[i]; if (u->kind != kind || u->is_template || !u->act) continue;
        char target[160]; if (u->trigger[0]) copy_str(target, sizeof(target), u->trigger); else same_stem(u->name, kind == K_SOCKET && u->act->accept ? "@.service" : ".service", target, sizeof(target));
        if (kind == K_TIMER) {
            char when[96], left[48], last[96]; int64_t next = u->state == S_ACTIVE ? timer_next_real(u) : -1;
            fmt_real(next, when, sizeof(when)); fmt_left(next >= 0 ? next - (int64_t)realtime_s() : -1, left, sizeof(left)); fmt_real(u->act->last_trigger_real, last, sizeof(last));
            reply(fd, "%-32s %-10s %-36s %-24s %s\n", when, left, last, u->name, target);
        } else if (kind == K_SOCKET) {
            for (unsigned k = 0; k < u->act->nl || k == 0; k++) reply(fd, "%-40s %-26s %-10s %s\n", u->act->nl ? u->act->l[k].addr : "-", u->name, state_text(u), target);
        } else reply(fd, "%-26s %-10s %s\n", u->name, state_text(u), target);
    }
}
