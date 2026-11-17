/* NexusOS service manager (nexusd) <-> nexusctl: paths, journal record format, control protocol.
 *
 * Control: AF_UNIX stream socket NEXUSD_CONTROL. The client sends one request line "VERB [ARG...]\n" (arguments are
 * separated by single spaces and may not contain spaces or control characters); the server answers with text lines and
 * a final line "\x01<status>\n" (0 = success, otherwise a nexusctl exit status), then closes. Mutating verbs need the
 * caller (SO_PEERCRED) to be root, except `start/stop/restart <template>@<uid>.service` of a unit marked
 * AllowUserInstance=yes by that uid and `session <uid>` by that uid.
 *
 * Journal: text files NEXUS_JOURNAL_DIR/system.journal (current) and system.journal.1 .. .N (older), one record per line:
 *   <realtime usec> <monotonic usec> <boot id: 16 hex> <priority 0-7> <unit> <identifier> <pid> <message>
 * unit is the service the record belongs to ("cups.service", "-" when none; "kernel" for /dev/kmsg, "nexusd"), the
 * identifier is the SyslogIdentifier/unit stem, the syslog tag, or "kernel"; message bytes < 0x20 and
 * '\\' are escaped as \\xHH. Records are appended with O_APPEND and fdatasync'ed every few seconds (immediately for
 * priority <= 3). Rotation renames system.journal -> .1 -> .2 ... when it exceeds SystemMaxFileSize. */
#ifndef NEXUSD_PROTO_H
#define NEXUSD_PROTO_H
#define NEXUSD_RUN "/run/nexus"
#define NEXUSD_CONTROL "/run/nexus/control"
#define NEXUSD_SYSLOG "/run/nexus/log"           /* /dev/log is a symbolic link here */
#define NEXUSD_NOTIFY_DIR "/run/nexus/notify"     /* one datagram socket per Type=notify unit */
#define NEXUSD_LOCK "/run/nexus/nexusd.lock"
#define NEXUS_VENDOR_UNITS "/usr/lib/nexus/services"
#define NEXUS_ADMIN_UNITS "/etc/nexus/services"
#define NEXUS_ENABLED_DIR "/etc/nexus/enabled"     /* <unit> symlinks: enabled */
#define NEXUS_DISABLED_DIR "/etc/nexus/disabled"   /* <unit> markers: a DefaultEnabled unit the admin disabled */
#define NEXUS_JOURNAL_DIR "/var/log/journal"
#define NEXUS_JOURNAL_CONF "/etc/nexus/journald.conf"
#define NEXUS_JOURNAL_FILE "system.journal"
#define NEXUSD_REPLY_END '\x01'
/* Per-user managers ("nexusd --user", started as user@UID.service with the user's session): control socket
 * $XDG_RUNTIME_DIR/nexus/control (/run/user/UID, mode 0700), units ~/.config/nexus/user (the user's), /etc/nexus/user
 * (admin), /usr/lib/nexus/user (vendor), then ~/.config/systemd/user, /etc/systemd/user, /usr/lib/systemd/user;
 * enable/disable marks in ~/.config/nexus/user-enabled|user-disabled; journal ~/.local/state/nexus/journal/user.journal;
 * timer stamps ~/.local/state/nexus/timers (system: /var/lib/nexus/timers). */
#define NEXUSD_USER_RUN "nexus"                    /* below $XDG_RUNTIME_DIR */
#define NEXUS_USER_ADMIN_UNITS "/etc/nexus/user"
#define NEXUS_USER_VENDOR_UNITS "/usr/lib/nexus/user"
#define NEXUS_USER_JOURNAL_DIR ".local/state/nexus/journal"   /* below $HOME */
#define NEXUS_USER_JOURNAL_FILE "user.journal"
#define NEXUS_TIMERS_DIR "/var/lib/nexus/timers"
#endif
