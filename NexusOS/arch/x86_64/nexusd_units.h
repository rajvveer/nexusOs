/* nexusd: the state of socket, timer and path units (nexusd_units.c), included by nexusd.c before struct unit. */
#define MAX_LISTEN 8
#define MAX_CAL 8
#define MAX_PATHS 8
/* kind: 's' a socket (family AF_UNIX/AF_INET/AF_INET6; dual: a plain PORT, [::] that also takes IPv4), 'f' a FIFO,
 * 'n' a netlink socket (proto, groups) */
struct listen_spec { int type, family; char addr[UPATH]; int fd; char kind; bool dual; int proto; unsigned groups; };
struct calspec { uint8_t dow; uint16_t months; uint32_t days; uint32_t hours; uint64_t minutes, seconds; int year_min, year_max, year_step; bool utc; };
struct pathspec { char kind; char path[UPATH]; int wd, pwd; };   /* e exists, g glob, c changed, m modified, n directory not empty */
struct actpart {
    /* socket */
    struct listen_spec l[MAX_LISTEN]; unsigned nl; bool accept, pass_cred, keepalive, reuseport, remove_on_stop;
    mode_t socket_mode, dir_mode; char sock_user[64], sock_group[64], fdname[64]; unsigned max_conn, backlog, connections;
    int bind6;   /* BindIPv6Only=: 0 default, 1 both, 2 ipv6-only */
    int rcvbuf, sndbuf;
    /* timer */
    struct calspec cal[MAX_CAL]; unsigned ncal; char cal_text[MAX_CAL][96];
    uint64_t on_boot, on_startup, on_active, on_unit_active, on_unit_inactive, random_delay;   /* ms; INFINITE_MS unset */
    bool persistent, remain_after_elapse; bool boot_done, startup_done, active_done;
    uint64_t activated_ms, next_mono, last_trigger_mono, seen_active_enter, seen_inactive_enter;
    int64_t next_real, last_trigger_real; uint64_t delay_cal;
    /* path */
    struct pathspec p[MAX_PATHS]; unsigned np; bool make_dir; int ino_fd;
    /* socket + path: trigger limit */
    uint64_t trig_interval, trig_window; unsigned trig_burst, trig_count;
};
