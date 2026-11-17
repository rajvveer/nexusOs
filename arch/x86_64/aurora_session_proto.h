/* Aurora (pid 1, the desktop user) <-> nexus-session (the privileged session launcher, nexus_session_user.c).
 *
 * Aurora creates a SOCK_SEQPACKET socketpair, forks, puts one end on fd 3 and the password (one line) on fd 0, and
 * executes  NEXUS_SESSION_PROGRAM start USER SESSION-ID  (set-user-ID root on an ext4 root). The launcher refuses unless
 * fd 3's peer (SO_PEERCRED of the socketpair = the process that made it) is pid 1. It checks the password (through
 * nexus-auth verify: failure tally, delay), prepares the user's home (first login: /etc/skel, XDG user folders) and
 * runtime folder /run/user/UID (0700), makes the user's listening sockets there (wayland-0, aurora-control,
 * aurora-notify) and hands them to Aurora, starts the user's D-Bus session bus (nexusd "session UID"), becomes the user
 * for good (supplementary groups, gid, uid) and stays as the session agent: every program of the session is its child.
 *
 * Records (one per packet): a verb and fields, each terminated by NUL.
 *   launcher -> Aurora   "auth" code                         password verdict (AURORA_AUTH_*; non-zero: the launcher exits)
 *                        "ready" pid uid gid first home      + SCM_RIGHTS: wayland listener, control listener, notify socket
 *                        "started" tag pid | "failed" tag errno
 *                        "exit" pid status                   a session program ended (waitpid status)
 *                        "bye"                               logout finished, the agent exits
 *   Aurora -> agent      "run" tag argc argv... envc env...  start a program (env: KEY=VALUE overrides)
 *                        "logout"                            end every process of the user, clean the runtime folder
 *                        "ping"                              -> "pong"
 * Limits: a record is at most NEXUS_SESSION_RECORD bytes, at most 256 arguments + 64 environment entries. */
#ifndef AURORA_SESSION_PROTO_H
#define AURORA_SESSION_PROTO_H
#define NEXUS_SESSION_PROGRAM "/usr/libexec/nexus-session"
#define NEXUS_SESSION_FD 3
#define NEXUS_SESSION_RECORD 65536
#define NEXUS_SESSION_MAX_ARGS 256
#define NEXUS_SESSION_MAX_ENV 64
#endif
