/* nexus-auth: NexusOS account helper, /usr/libexec/nexus-auth (set-user-ID root on ext4 roots).
 *
 *   nexus-auth status                       -> "posix=0|1 writable=0|1 libcrypt=0|1"
 *   nexus-auth verify USER                  stdin: PASSWORD          exit status: AURORA_AUTH_* (0 = right password)
 *   nexus-auth passwd USER                  stdin: OLD NEW           the user (old password), or an administrator (own password)
 *   nexus-auth useradd USER FULLNAME ADMIN  stdin: CALLER NEW        an administrator: new account, private group, /home/USER
 *   nexus-auth userdel USER                 stdin: CALLER            an administrator, not the caller's account (home kept)
 *   nexus-auth admin USER 0|1               stdin: CALLER            an administrator: sudo group membership
 * (stdin: one value per line). The caller is the real user ID. A wrong password costs two seconds; five wrong ones for
 * an account within 15 minutes stop its checks for 60 s (AURORA_AUTH_THROTTLED; tally in /var/lib/nexus-auth, checks of
 * one account serialised), so neither the verify oracle nor parallel helpers allow fast guessing. /etc/passwd, group
 * and shadow are rewritten whole under /etc/.pwd.lock (write, fsync, rename). Nothing is read from the environment
 * (and the kernel runs set-ID programs in secure mode: AT_SECURE, so ld.so ignores LD_PRELOAD / LD_LIBRARY_PATH). */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "aurora_auth.h"

static char input[3][512];
static int read_lines(int count) {   /* stdin lines (bounded), newline removed */
    static char buffer[1600]; size_t used = 0; ssize_t n;
    while (used < sizeof(buffer) - 1 && (n = read(0, buffer + used, sizeof(buffer) - 1 - used)) > 0) used += (size_t)n;
    buffer[used] = 0; char* p = buffer;
    for (int i = 0; i < count; i++) {
        char* end = strchr(p, '\n'); if (!end) end = p + strlen(p);
        size_t length = (size_t)(end - p); if (length >= sizeof(input[i])) return -1;
        memcpy(input[i], p, length); input[i][length] = 0; p = *end ? end + 1 : end;
    }
    memset(buffer, 0, sizeof(buffer)); return 0;
}
static void wipe(void) { memset(input, 0, sizeof(input)); }
static int fail(int code) { wipe(); if (code == AURORA_AUTH_WRONG) sleep(2); return code; }
static bool valid_name(const char* name) {
    if (!name[0] || strlen(name) > 31 || !(islower((unsigned char)name[0]) || name[0] == '_')) return false;
    for (const char* p = name; *p; p++) if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    return true;
}
static bool valid_text(const char* text) { for (const char* p = text; *p; p++) if (*p == ':' || *p == '\n' || *p == ',' || (unsigned char)*p < 32) return false; return strlen(text) <= 64; }
static bool caller(struct aurora_user* u) { return aurora_user_by_uid(getuid(), u) || (getuid() == 0 && aurora_user_find("root", u)); }
/* Failed-password tally of one account (like pam_faillock), held locked until the helper exits (after the delay). */
#define TALLY_DIR "/var/lib/nexus-auth"
static int tally_fd = -1; static unsigned tally_count; static long tally_last;
static int tally_begin(const char* user) {
    if (geteuid() != 0) return AURORA_AUTH_OK;   /* not privileged (FAT root): /etc/shadow is readable anyway */
    if (!valid_name(user)) return AURORA_AUTH_INVALID;
    mkdir("/var", 0755); mkdir("/var/lib", 0755); mkdir(TALLY_DIR, 0700);
    char path[96]; snprintf(path, sizeof(path), TALLY_DIR "/%s", user);
    tally_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600); if (tally_fd < 0) return AURORA_AUTH_UNAVAILABLE;
    struct flock l = {.l_type = F_WRLCK, .l_whence = SEEK_SET}; bool held = false;
    for (int i = 0; i < 100 && !(held = !fcntl(tally_fd, F_SETLK, &l)); i++) usleep(100000);
    if (!held) return AURORA_AUTH_THROTTLED;
    char text[64] = ""; ssize_t n = pread(tally_fd, text, sizeof(text) - 1, 0); text[n > 0 ? n : 0] = 0;
    if (sscanf(text, "%u %ld", &tally_count, &tally_last) != 2) tally_count = 0, tally_last = 0;
    long now = (long)time(NULL); if (now - tally_last > 900 || now < tally_last) tally_count = 0;
    return tally_count >= 5 && now - tally_last < 60 ? AURORA_AUTH_THROTTLED : AURORA_AUTH_OK;
}
static int tally_end(int result) {
    if (tally_fd < 0 || (result != AURORA_AUTH_WRONG && result != AURORA_AUTH_OK)) return result;
    if (result == AURORA_AUTH_WRONG) { tally_count++; tally_last = (long)time(NULL); } else tally_count = 0;
    char text[64]; int n = snprintf(text, sizeof(text), "%u %ld\n", tally_count, tally_last);
    if (ftruncate(tally_fd, 0) || pwrite(tally_fd, text, (size_t)n, 0) != n) return AURORA_AUTH_UNAVAILABLE;
    return result;
}
static int checked_verify(const char* user, const char* password) {
    int r = tally_begin(user); return r != AURORA_AUTH_OK ? r : tally_end(aurora_auth_verify(user, password));
}
static int check_caller_password(const struct aurora_user* u, const char* password) {
    int r = checked_verify(u->name, password);
    return r == AURORA_AUTH_NO_PASSWORD ? AURORA_AUTH_OK : r;   /* an account without a password has nothing to prove */
}

/* ---- /etc files ---- */
static int lock_fd = -1;
static bool lock_accounts(void) {
    lock_fd = open("/etc/.pwd.lock", O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0) return false;
    struct flock l = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    for (int i = 0; i < 50; i++) { if (!fcntl(lock_fd, F_SETLK, &l)) return true; usleep(100000); }
    return false;
}
static char* slurp(const char* path) {   /* whole file ("" when missing), NULL on error */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? strdup("") : NULL;
    struct stat st; if (fstat(fd, &st) || st.st_size > (1 << 22)) { close(fd); return NULL; }
    char* data = calloc(1, (size_t)st.st_size + 2); ssize_t n = data ? read(fd, data, (size_t)st.st_size) : -1; close(fd);
    if (n != st.st_size) { free(data); return NULL; }
    if (n && data[n - 1] != '\n') data[n] = '\n';
    return data;
}
static bool replace(const char* path, const char* text, mode_t mode, gid_t gid) {
    char temp[64]; snprintf(temp, sizeof(temp), "%s+", path);
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode); if (fd < 0) return false;
    size_t length = strlen(text); bool ok = write(fd, text, length) == (ssize_t)length;
    ok = ok && !fchmod(fd, mode) && (!fchown(fd, 0, gid) || getuid() == geteuid()) && !fsync(fd);
    close(fd);
    if (!ok || rename(temp, path)) { unlink(temp); return false; }
    return true;
}
/* The lines of text whose first field is not name, plus extra (when given). */
static char* without(const char* text, const char* name, const char* extra) {
    size_t size = strlen(text) + (extra ? strlen(extra) : 0) + 2; char* out = malloc(size); if (!out) return NULL; out[0] = 0;
    for (const char* line = text; *line;) {
        const char* end = strchr(line, '\n'); size_t length = end ? (size_t)(end - line) + 1 : strlen(line);
        size_t key = strcspn(line, ":\n");
        if (!(key == strlen(name) && !strncmp(line, name, key))) strncat(out, line, length);
        line += length;
    }
    if (extra) strcat(out, extra);
    return out;
}
static gid_t shadow_gid(void) { struct stat st; return stat("/etc/shadow", &st) ? 0 : st.st_gid; }
static bool set_shadow(const char* name, const char* hash) {
    char* old = slurp("/etc/shadow"); if (!old) return false;
    char line[700]; snprintf(line, sizeof(line), "%s:%s:%ld:0:99999:7:::\n", name, hash ? hash : "!", (long)(time(NULL) / 86400));
    char* next = hash ? without(old, name, line) : without(old, name, NULL);
    bool ok = next && replace("/etc/shadow", next, 0640, shadow_gid());
    if (next) memset(next, 0, strlen(next));
    memset(old, 0, strlen(old)); free(old); free(next); return ok;
}
/* group: set membership of user in group (creating the group with gid when missing) */
static bool set_member(const char* group, gid_t gid_if_new, const char* user, bool member) {
    char* old = slurp("/etc/group"); if (!old) return false;
    size_t size = strlen(old) + 256; char* out = malloc(size); if (!out) { free(old); return false; } out[0] = 0; bool seen = false;
    for (char* save = NULL, *line = strtok_r(old, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t key = strcspn(line, ":");
        if (key != strlen(group) || strncmp(line, group, key)) { strcat(out, line); strcat(out, "\n"); continue; }
        seen = true;
        char* members = line; for (int i = 0; i < 3 && members; i++) { members = strchr(members, ':'); if (members) members++; }
        size_t prefix = members ? (size_t)(members - line) : strlen(line);
        char list[1024] = ""; if (members) snprintf(list, sizeof(list), "%s", members);
        strncat(out, line, prefix); if (!members) strcat(out, ":");
        bool first = true;
        for (char* s2 = NULL, *m = strtok_r(list, ",", &s2); m; m = strtok_r(NULL, ",", &s2)) if (strcmp(m, user)) { if (!first) strcat(out, ","); strcat(out, m); first = false; }
        if (member) { if (!first) strcat(out, ","); strcat(out, user); }
        strcat(out, "\n");
    }
    if (!seen && member) { char add[128]; snprintf(add, sizeof(add), "%s:x:%u:%s\n", group, (unsigned)gid_if_new, user); strcat(out, add); }
    bool ok = replace("/etc/group", out, 0644, 0); free(old); free(out); return ok;
}
static bool remove_from_all_groups(const char* user, const char* own_group) {
    char* old = slurp("/etc/group"); if (!old) return false;
    char names[64][64]; int n = 0;
    for (char* save = NULL, *line = strtok_r(old, "\n", &save); line && n < 64; line = strtok_r(NULL, "\n", &save)) { size_t k = strcspn(line, ":"); if (k < 64) { memcpy(names[n], line, k); names[n++][k] = 0; } }
    free(old); bool ok = true;
    for (int i = 0; i < n; i++) if (strcmp(names[i], own_group)) ok &= set_member(names[i], 0, user, false);
    char* text = slurp("/etc/group"); char* next = text ? without(text, own_group, NULL) : NULL;
    ok = ok && next && replace("/etc/group", next, 0644, 0); free(text); free(next); return ok;
}

/* ---- commands ---- */
static int writable(void) { return geteuid() == 0 && !faccessat(AT_FDCWD, "/etc/passwd", W_OK, AT_EACCESS) ? 1 : 0; }   /* effective IDs: access() would ask for the caller */
static int cmd_verify(const char* name) {
    if (read_lines(1)) return fail(AURORA_AUTH_INVALID);
    int r = checked_verify(name, input[0]); return fail(r);
}
static int cmd_passwd(const char* name) {
    struct aurora_user me, target; if (read_lines(2) || !caller(&me) || !aurora_user_find(name, &target) || !input[1][0]) return fail(AURORA_AUTH_INVALID);
    bool self = me.uid == target.uid;
    if (!self && !me.admin) return fail(AURORA_AUTH_DENIED);
    int r = self && target.password == AURORA_PW_NONE ? AURORA_AUTH_OK : check_caller_password(&me, input[0]);
    if (r != AURORA_AUTH_OK) return fail(r);
    if (!writable()) return fail(AURORA_AUTH_READONLY);
    char hash[512]; if (!aurora_make_hash(input[1], hash, sizeof(hash))) return fail(AURORA_AUTH_UNAVAILABLE);
    if (!lock_accounts() || !set_shadow(target.name, hash)) { memset(hash, 0, sizeof(hash)); return fail(AURORA_AUTH_UNAVAILABLE); }
    memset(hash, 0, sizeof(hash)); wipe(); printf("password changed for %s\n", target.name); return AURORA_AUTH_OK;
}
static int cmd_useradd(const char* name, const char* full, const char* admin) {
    struct aurora_user me, existing; if (read_lines(2) || !caller(&me) || !valid_name(name) || !valid_text(full) || !input[1][0]) return fail(AURORA_AUTH_INVALID);
    if (!me.admin) return fail(AURORA_AUTH_DENIED);
    int r = check_caller_password(&me, input[0]); if (r != AURORA_AUTH_OK) return fail(r);
    if (!writable()) return fail(AURORA_AUTH_READONLY);
    if (!lock_accounts() || aurora_user_find(name, &existing)) return fail(AURORA_AUTH_INVALID);
    struct aurora_user list[64]; int n = aurora_users(list, 64, true); unsigned uid = 1001;
    for (int i = 0; i < n; i++) if (list[i].uid >= uid && list[i].uid < 60000) uid = list[i].uid + 1;
    char hash[512]; if (!aurora_make_hash(input[1], hash, sizeof(hash))) return fail(AURORA_AUTH_UNAVAILABLE);
    char line[512]; snprintf(line, sizeof(line), "%s:x:%u:%u:%s:/home/%s:/bin/bash\n", name, uid, uid, full, name);
    char* passwd = slurp("/etc/passwd"); char* next = passwd ? without(passwd, name, line) : NULL;
    bool ok = next && replace("/etc/passwd", next, 0644, 0) && set_member(name, uid, name, false) && set_shadow(name, hash);
    if (ok) {   /* the private group needs a line even with no members */
        char* group = slurp("/etc/group"); char add[128]; snprintf(add, sizeof(add), "%s:x:%u:\n", name, uid);
        char* g2 = group ? without(group, name, add) : NULL; ok = g2 && replace("/etc/group", g2, 0644, 0); free(group); free(g2);
    }
    if (ok && atoi(admin)) ok = set_member("sudo", 27, name, true);
    char home[160]; snprintf(home, sizeof(home), "/home/%s", name);
    if (ok && (mkdir(home, 0700) && errno != EEXIST)) ok = false;
    if (ok && chown(home, uid, uid) && geteuid() == 0) ok = false;
    free(passwd); free(next); memset(hash, 0, sizeof(hash)); wipe();
    if (!ok) return AURORA_AUTH_UNAVAILABLE;
    printf("created %s uid=%u\n", name, uid); return AURORA_AUTH_OK;
}
static int cmd_userdel(const char* name) {
    struct aurora_user me, target; if (read_lines(1) || !caller(&me) || !aurora_user_find(name, &target)) return fail(AURORA_AUTH_INVALID);
    if (!me.admin || me.uid == target.uid || target.uid == 0) return fail(AURORA_AUTH_DENIED);
    int r = check_caller_password(&me, input[0]); if (r != AURORA_AUTH_OK) return fail(r);
    if (!writable()) return fail(AURORA_AUTH_READONLY);
    if (!lock_accounts()) return fail(AURORA_AUTH_UNAVAILABLE);
    char* passwd = slurp("/etc/passwd"); char* next = passwd ? without(passwd, name, NULL) : NULL;
    bool ok = next && replace("/etc/passwd", next, 0644, 0) && set_shadow(name, NULL) && remove_from_all_groups(name, name);
    free(passwd); free(next); wipe();
    if (!ok) return AURORA_AUTH_UNAVAILABLE;
    printf("deleted %s (home kept)\n", name); return AURORA_AUTH_OK;
}
static int cmd_admin(const char* name, const char* value) {
    struct aurora_user me, target; if (read_lines(1) || !caller(&me) || !aurora_user_find(name, &target)) return fail(AURORA_AUTH_INVALID);
    if (!me.admin || (me.uid == target.uid && !atoi(value))) return fail(AURORA_AUTH_DENIED);   /* no removing one's own rights */
    int r = check_caller_password(&me, input[0]); if (r != AURORA_AUTH_OK) return fail(r);
    if (!writable()) return fail(AURORA_AUTH_READONLY);
    if (!lock_accounts() || !set_member("sudo", 27, name, atoi(value) != 0)) return fail(AURORA_AUTH_UNAVAILABLE);
    wipe(); printf("%s %s an administrator\n", name, atoi(value) ? "is now" : "is no longer"); return AURORA_AUTH_OK;
}
int main(int argc, char** argv) {
    umask(022); aurora_auth_in_helper = true;
    if (argc == 2 && !strcmp(argv[1], "status")) {
        struct stat st; bool posix = !stat("/etc", &st) && st.st_uid == 0;   /* FAT roots report every file as the session user's */
        printf("posix=%d writable=%d euid=%u\n", posix, writable(), (unsigned)geteuid()); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "verify")) return cmd_verify(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "passwd")) return cmd_passwd(argv[2]);
    if (argc == 5 && !strcmp(argv[1], "useradd")) return cmd_useradd(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "userdel")) return cmd_userdel(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "admin")) return cmd_admin(argv[2], argv[3]);
    fprintf(stderr, "usage: nexus-auth status | verify USER | passwd USER | useradd USER FULLNAME 0|1 | userdel USER | admin USER 0|1\n");
    return AURORA_AUTH_INVALID;
}
