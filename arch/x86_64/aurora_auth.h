/* Aurora accounts (aurora_auth.c): the users of /etc/passwd, their groups (/etc/group: sudo or wheel = administrator)
 * and their passwords (/etc/shadow). Shared by the compositor (lock screen, greeter), System Settings > Users and the
 * privileged helper nexus-auth (nexus_auth_user.c, /usr/libexec/nexus-auth).
 *
 * Password hashes: yescrypt ($y$, Debian's default) and everything else libxcrypt knows through libcrypt.so.1 (dlopen;
 * desk_install.py "auth" installs it), SHA-512-crypt ($6$) also without it. New hashes are yescrypt when libcrypt is
 * there, SHA-512-crypt otherwise. Reading /etc/shadow needs privileges on an ext4 (POSIX) root: there the compositor
 * asks the set-user-ID helper; on a FAT root every file is readable and nothing in /etc can be written (kernel.c
 * private_path), so accounts can be used but not changed. */
#ifndef AURORA_AUTH_H
#define AURORA_AUTH_H
#include <stdbool.h>
#include <stddef.h>

#define AURORA_HELPER "/usr/libexec/nexus-auth"
#define AURORA_LOGIN_CONF "/home/nexus/.aurora/login.conf"   /* autologin=0|1, lock_after=SECONDS, lock_on_sleep=0|1 */
struct aurora_user { char name[33], gecos[65], home[129], shell[65]; unsigned uid, gid; bool admin; int password; };
enum { AURORA_PW_NONE, AURORA_PW_SET, AURORA_PW_LOCKED, AURORA_PW_UNKNOWN };   /* password states */
enum { AURORA_AUTH_OK, AURORA_AUTH_WRONG, AURORA_AUTH_NO_PASSWORD, AURORA_AUTH_LOCKED, AURORA_AUTH_UNKNOWN_USER, AURORA_AUTH_UNAVAILABLE,
       AURORA_AUTH_DENIED, AURORA_AUTH_READONLY, AURORA_AUTH_INVALID, AURORA_AUTH_THROTTLED };

/* Login accounts: uid 1000..59999 (and root only when asked), in /etc/passwd order; password state filled in
 * (AURORA_PW_UNKNOWN when /etc/shadow is unreadable here). */
int aurora_users(struct aurora_user* out, int capacity, bool with_root);
bool aurora_user_find(const char* name, struct aurora_user* out);
bool aurora_user_by_uid(unsigned uid, struct aurora_user* out);
/* The stored hash of user ("" none, "!..."/"*" locked); false when /etc/shadow cannot be read here. */
bool aurora_shadow_hash(const char* user, char* hash, size_t size);
/* In-process check against a hash (constant-time compare): AURORA_AUTH_OK / WRONG / NO_PASSWORD / LOCKED / UNAVAILABLE. */
int aurora_check_hash(const char* password, const char* hash);
/* A new hash for password (yescrypt or SHA-512-crypt, random salt); false when no random source. */
bool aurora_make_hash(const char* password, char* out, size_t size);
/* Full check of user's password: in process when /etc/shadow is readable, else through the helper nexus-auth; when
 * neither can check it (no set-user-ID helper, or a hash this process cannot compute), Debian's unix_chkpwd for the
 * caller's own account. */
int aurora_auth_verify(const char* user, const char* password);
/* The same check, but always through nexus-auth (its per-account lockout and delay), never in process; for privileged
 * callers that could read /etc/shadow themselves (pkexec, nexus-polkitd). AURORA_AUTH_UNAVAILABLE without the helper. */
int aurora_auth_verify_helper(const char* user, const char* password);
extern bool aurora_auth_in_helper;   /* nexus-auth sets it: no recursion into the helper */
/* SHA-512-crypt of key with setting "$6$[rounds=N$]salt" (glibc's algorithm); false on a bad setting. */
bool aurora_sha512_crypt(const char* key, const char* setting, char* out, size_t size);
const char* aurora_auth_message(int code);
/* login.conf values (defaults: autologin off, lock after 300 s idle, lock before sleep). */
struct aurora_login_conf { bool autologin, lock_on_sleep; int lock_after; };
void aurora_login_conf(struct aurora_login_conf* conf);
bool aurora_login_conf_save(const struct aurora_login_conf* conf);
#endif
