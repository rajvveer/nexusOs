/* Aurora control socket (aurora_control.c): AURORA_CONTROL_SOCKET, an AF_UNIX stream socket on which Settings (and
 * tests) send line commands to the compositor. A request is one line, or a block "COMMAND ARGS\n...\nend\n" for
 * commands registered as blocks; every reply ends with a line "OK" or "ERROR <reason>". Peers must be root or the
 * session user (SO_PEERCRED). Modules register their commands; the replies of a handler go through reply(). */
#ifndef AURORA_CONTROL_H
#define AURORA_CONTROL_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <wayland-server.h>

#define AURORA_CONTROL_SOCKET "/run/user/1000/aurora-control"   /* beside wayland-0 (writable on FAT roots too) */
struct aurora_control_reply;
/* args: the rest of the command line; body: the lines of a block command (NULL otherwise). Return true for OK; on
 * false, error holds the reason. */
typedef bool (*aurora_control_handler)(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t error_size);
void aurora_control_print(struct aurora_control_reply* reply, const char* format, ...) __attribute__((format(printf, 2, 3)));
bool aurora_control_register(const char* command, bool block, aurora_control_handler handler);
bool aurora_control_init(struct wl_event_loop* loop);
void aurora_control_session_user(uid_t uid);   /* the logged-in user who may use the socket (besides root) */
/* aurora_session.c: another user's session socket (their runtime folder): peers must be that user or root; every
 * command needs its requester to be root or the active session's user. */
bool aurora_control_add_listener(int fd, uid_t owner);
void aurora_control_remove_listener(int fd);
uid_t aurora_control_peer_uid(void);   /* inside a handler: who sent the command (0 = root) */
pid_t aurora_control_peer_pid(void);   /* ... and its process (SO_PEERCRED at connect) */
int aurora_control_peer_fd(void);      /* inside a handler: the descriptor sent with the request (SCM_RIGHTS), now the
                                          handler's to close; -1 if none */
#endif
