/* Aurora accounts and passwords (aurora_auth.h). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "aurora_auth.h"

/* ---- SHA-512 ---- */
struct sha512 { uint64_t h[8], bytes; uint8_t block[128]; unsigned used; };
static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static void sha512_block(struct sha512* s, const uint8_t* p) {
    uint64_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 16; i++) { w[i] = 0; for (int k = 0; k < 8; k++) w[i] = w[i] << 8 | p[i * 8 + k]; }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7), s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + (ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint64_t t2 = (ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha512_init(struct sha512* s) {
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                                   0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    memcpy(s->h, iv, sizeof(iv)); s->bytes = 0; s->used = 0;
}
static void sha512_update(struct sha512* s, const void* data, size_t n) {
    const uint8_t* p = data; s->bytes += n;
    while (n) { size_t take = 128 - s->used < n ? 128 - s->used : n; memcpy(s->block + s->used, p, take); s->used += (unsigned)take; p += take; n -= take;
        if (s->used == 128) { sha512_block(s, s->block); s->used = 0; } }
}
static void sha512_final(struct sha512* s, uint8_t out[64]) {
    uint64_t bits = s->bytes * 8; uint8_t pad = 0x80, zero = 0;
    sha512_update(s, &pad, 1); while (s->used != 112) sha512_update(s, &zero, 1);
    uint8_t length[16] = {0}; for (int i = 0; i < 8; i++) length[15 - i] = (uint8_t)(bits >> (8 * i));
    sha512_update(s, length, 16);
    for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) out[i * 8 + k] = (uint8_t)(s->h[i] >> (56 - 8 * k));
}

/* ---- SHA-512-crypt (glibc's algorithm, $6$) ---- */
static const char B64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
bool aurora_sha512_crypt(const char* key, const char* setting, char* out, size_t size) {
    if (strncmp(setting, "$6$", 3)) return false;
    const char* salt = setting + 3; unsigned long rounds = 5000; bool custom = false;
    if (!strncmp(salt, "rounds=", 7)) {
        char* end; unsigned long r = strtoul(salt + 7, &end, 10); if (*end != '$') return false;
        rounds = r < 1000 ? 1000 : r > 999999999 ? 999999999 : r; custom = true; salt = end + 1;
    }
    size_t salt_len = strcspn(salt, "$"); if (salt_len > 16) salt_len = 16;
    size_t key_len = strlen(key); uint8_t a[64], b[64], dp[64], ds[64]; struct sha512 ctx;
    sha512_init(&ctx); sha512_update(&ctx, key, key_len); sha512_update(&ctx, salt, salt_len); sha512_update(&ctx, key, key_len); sha512_final(&ctx, b);
    sha512_init(&ctx); sha512_update(&ctx, key, key_len); sha512_update(&ctx, salt, salt_len);
    size_t n; for (n = key_len; n > 64; n -= 64) sha512_update(&ctx, b, 64);
    sha512_update(&ctx, b, n);
    for (n = key_len; n > 0; n >>= 1) { if (n & 1) sha512_update(&ctx, b, 64); else sha512_update(&ctx, key, key_len); }
    sha512_final(&ctx, a);
    sha512_init(&ctx); for (n = 0; n < key_len; n++) sha512_update(&ctx, key, key_len); sha512_final(&ctx, dp);
    uint8_t* p = malloc(key_len + 1); if (!p) return false;
    for (n = 0; n + 64 <= key_len; n += 64) memcpy(p + n, dp, 64);
    memcpy(p + n, dp, key_len - n);
    sha512_init(&ctx); for (n = 0; n < 16u + a[0]; n++) sha512_update(&ctx, salt, salt_len); sha512_final(&ctx, ds);
    uint8_t s[16]; memcpy(s, ds, salt_len);
    for (unsigned long r = 0; r < rounds; r++) {
        sha512_init(&ctx);
        if (r & 1) sha512_update(&ctx, p, key_len); else sha512_update(&ctx, a, 64);
        if (r % 3) sha512_update(&ctx, s, salt_len);
        if (r % 7) sha512_update(&ctx, p, key_len);
        if (r & 1) sha512_update(&ctx, a, 64); else sha512_update(&ctx, p, key_len);
        sha512_final(&ctx, a);
    }
    memset(p, 0, key_len); free(p);
    char text[160]; int used = custom ? snprintf(text, sizeof(text), "$6$rounds=%lu$%.*s$", rounds, (int)salt_len, salt) : snprintf(text, sizeof(text), "$6$%.*s$", (int)salt_len, salt);
    static const uint8_t order[21][3] = {{0,21,42},{22,43,1},{44,2,23},{3,24,45},{25,46,4},{47,5,26},{6,27,48},{28,49,7},{50,8,29},{9,30,51},{31,52,10},
                                          {53,11,32},{12,33,54},{34,55,13},{56,14,35},{15,36,57},{37,58,16},{59,17,38},{18,39,60},{40,61,19},{62,20,41}};
    for (int i = 0; i < 21; i++) { uint32_t w = (uint32_t)a[order[i][0]] << 16 | (uint32_t)a[order[i][1]] << 8 | a[order[i][2]];
        for (int k = 0; k < 4; k++) { text[used++] = B64[w & 63]; w >>= 6; } }
    { uint32_t w = a[63]; text[used++] = B64[w & 63]; w >>= 6; text[used++] = B64[w & 63]; }
    text[used] = 0; memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b)); memset(dp, 0, sizeof(dp));
    if (strlen(text) + 1 > size) return false;
    memcpy(out, text, strlen(text) + 1); return true;
}

/* ---- libxcrypt (yescrypt and the rest) ---- */
static char* (*p_crypt_rn)(const char*, const char*, void*, int);
static char* (*p_crypt_gensalt_rn)(const char*, unsigned long, const char*, int, char*, int);
static bool libcrypt(void) {
    static int state;
    if (!state) {
        void* lib = dlopen("libcrypt.so.1", RTLD_NOW | RTLD_LOCAL); state = -1;
        if (lib && (p_crypt_rn = (char* (*)(const char*, const char*, void*, int))dlsym(lib, "crypt_rn")) &&
            (p_crypt_gensalt_rn = (char* (*)(const char*, unsigned long, const char*, int, char*, int))dlsym(lib, "crypt_gensalt_rn"))) state = 1;
    }
    return state > 0;
}
#define CRYPT_DATA 32768   /* sizeof(struct crypt_data) in libxcrypt */
static bool hash_with(const char* password, const char* setting, char* out, size_t size) {
    if (!strncmp(setting, "$6$", 3) && !libcrypt()) return aurora_sha512_crypt(password, setting, out, size);
    if (!libcrypt()) return false;
    void* data = calloc(1, CRYPT_DATA); if (!data) return false;
    char* result = p_crypt_rn(password, setting, data, CRYPT_DATA);
    bool ok = result && result[0] != '*' && strlen(result) < size;
    if (ok) memcpy(out, result, strlen(result) + 1);
    memset(data, 0, CRYPT_DATA); free(data); return ok;
}
int aurora_check_hash(const char* password, const char* hash) {
    if (!hash[0]) return AURORA_AUTH_NO_PASSWORD;
    if (hash[0] == '!' || hash[0] == '*') return AURORA_AUTH_LOCKED;
    char computed[512];
    if (!hash_with(password, hash, computed, sizeof(computed))) return AURORA_AUTH_UNAVAILABLE;
    size_t a = strlen(computed), b = strlen(hash); unsigned diff = (unsigned)(a ^ b);
    for (size_t i = 0; i < a && i < b; i++) diff |= (unsigned char)(computed[i] ^ hash[i]);
    memset(computed, 0, sizeof(computed));
    return diff ? AURORA_AUTH_WRONG : AURORA_AUTH_OK;
}
bool aurora_make_hash(const char* password, char* out, size_t size) {
    uint8_t random[16]; if (getrandom(random, sizeof(random), 0) != (ssize_t)sizeof(random)) return false;
    char setting[128];
    if (libcrypt() && p_crypt_gensalt_rn("$y$", 0, (const char*)random, (int)sizeof(random), setting, (int)sizeof(setting))) return hash_with(password, setting, out, size);
    char salt[17]; for (int i = 0; i < 16; i++) salt[i] = B64[random[i] & 63]; salt[16] = 0;
    snprintf(setting, sizeof(setting), "$6$%s", salt);
    return aurora_sha512_crypt(password, setting, out, size);
}

/* ---- passwd / group / shadow ---- */
static bool field(const char* line, int index, char* out, size_t size) {   /* index-th ':' field */
    for (int i = 0; i < index; i++) { line = strchr(line, ':'); if (!line) return false; line++; }
    size_t n = strcspn(line, ":\n"); if (n >= size) n = size - 1;
    memcpy(out, line, n); out[n] = 0; return true;
}
static bool member_of(const char* user, unsigned gid, const char* group_name) {   /* in group_name (or its gid is the user's) */
    FILE* f = fopen("/etc/group", "re"); if (!f) return false;
    char line[2048]; bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char name[64], id[16], members[1024];
        if (!field(line, 0, name, sizeof(name)) || strcmp(name, group_name) || !field(line, 2, id, sizeof(id)) || !field(line, 3, members, sizeof(members))) continue;
        if ((unsigned)strtoul(id, NULL, 10) == gid) found = true;
        for (char* save = NULL, *m = strtok_r(members, ",", &save); m && !found; m = strtok_r(NULL, ",", &save)) if (!strcmp(m, user)) found = true;
    }
    fclose(f); return found;
}
bool aurora_shadow_hash(const char* user, char* hash, size_t size) {
    FILE* f = fopen("/etc/shadow", "re");
    if (!f) {
        if (errno != ENOENT) return false;
        /* no shadow file: the password field of /etc/passwd ("x" there means a shadow entry that does not exist: none) */
        FILE* p = fopen("/etc/passwd", "re"); if (!p) return false;
        char line[1024]; bool found = false; hash[0] = 0;
        while (!found && fgets(line, sizeof(line), p)) { char name[64]; if (field(line, 0, name, sizeof(name)) && !strcmp(name, user)) { found = true; field(line, 1, hash, size); if (!strcmp(hash, "x")) hash[0] = 0; } }
        fclose(p); return found;
    }
    char line[1024]; bool found = false; hash[0] = 0;
    while (!found && fgets(line, sizeof(line), f)) { char name[64]; if (field(line, 0, name, sizeof(name)) && !strcmp(name, user)) { found = true; field(line, 1, hash, size); } }
    memset(line, 0, sizeof(line)); fclose(f);
    if (!found) hash[0] = 0;   /* no entry: no password */
    return true;
}
static int password_state(const char* user) {
    char hash[512]; if (!aurora_shadow_hash(user, hash, sizeof(hash))) return AURORA_PW_UNKNOWN;
    int state = !hash[0] ? AURORA_PW_NONE : (hash[0] == '!' || hash[0] == '*') ? AURORA_PW_LOCKED : AURORA_PW_SET;
    memset(hash, 0, sizeof(hash)); return state;
}
static bool parse_user(const char* line, struct aurora_user* u) {
    char uid[16], gid[16]; memset(u, 0, sizeof(*u));
    if (!field(line, 0, u->name, sizeof(u->name)) || !field(line, 2, uid, sizeof(uid)) || !field(line, 3, gid, sizeof(gid)) ||
        !field(line, 4, u->gecos, sizeof(u->gecos)) || !field(line, 5, u->home, sizeof(u->home)) || !field(line, 6, u->shell, sizeof(u->shell)) || !u->name[0]) return false;
    char* comma = strchr(u->gecos, ','); if (comma) *comma = 0;   /* full name only */
    u->uid = (unsigned)strtoul(uid, NULL, 10); u->gid = (unsigned)strtoul(gid, NULL, 10);
    u->admin = u->uid == 0 || member_of(u->name, u->gid, "sudo") || member_of(u->name, u->gid, "wheel");
    u->password = password_state(u->name);
    return true;
}
int aurora_users(struct aurora_user* out, int capacity, bool with_root) {
    FILE* f = fopen("/etc/passwd", "re"); int n = 0;
    if (!f) {   /* no account database (a bare disk): the session user only */
        if (capacity > 0) { memset(&out[0], 0, sizeof(out[0])); snprintf(out[0].name, sizeof(out[0].name), "nexus"); snprintf(out[0].gecos, sizeof(out[0].gecos), "nexus");
            snprintf(out[0].home, sizeof(out[0].home), "/home/nexus"); snprintf(out[0].shell, sizeof(out[0].shell), "/bin/sh"); out[0].uid = out[0].gid = 1000; out[0].password = AURORA_PW_NONE; return 1; }
        return 0;
    }
    char line[1024];
    while (n < capacity && fgets(line, sizeof(line), f)) {
        struct aurora_user u; if (!parse_user(line, &u)) continue;
        if ((u.uid >= 1000 && u.uid < 60000) || (with_root && u.uid == 0)) out[n++] = u;
    }
    fclose(f); return n;
}
bool aurora_user_find(const char* name, struct aurora_user* out) {
    struct aurora_user list[64]; int n = aurora_users(list, 64, true);
    for (int i = 0; i < n; i++) if (!strcmp(list[i].name, name)) { *out = list[i]; return true; }
    return false;
}
bool aurora_user_by_uid(unsigned uid, struct aurora_user* out) {
    struct aurora_user list[64]; int n = aurora_users(list, 64, true);
    for (int i = 0; i < n; i++) if (list[i].uid == uid) { *out = list[i]; return true; }
    return false;
}

/* ---- verification ---- */
bool aurora_auth_in_helper;   /* set by nexus-auth itself: never call the helper from there */
static int helper_verify(const char* user, const char* password) {   /* AURORA_AUTH_UNAVAILABLE: absent, or not privileged */
    if (aurora_auth_in_helper || access(AURORA_HELPER, X_OK)) return AURORA_AUTH_UNAVAILABLE;
    int in[2]; if (pipe2(in, O_CLOEXEC)) return AURORA_AUTH_UNAVAILABLE;
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); return AURORA_AUTH_UNAVAILABLE; }
    if (!pid) {
        dup2(in[0], 0); int devnull = open("/dev/null", O_WRONLY); if (devnull >= 0) { dup2(devnull, 1); }
        execl(AURORA_HELPER, "nexus-auth", "verify", user, (char*)NULL); _exit(AURORA_AUTH_UNAVAILABLE);
    }
    close(in[0]);
    size_t length = strlen(password); ssize_t wrote = write(in[1], password, length); (void)!write(in[1], "\n", 1); close(in[1]);
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (wrote != (ssize_t)length || !WIFEXITED(status)) return AURORA_AUTH_UNAVAILABLE;
    return WEXITSTATUS(status);
}
/* Always through the helper, even when this process could read /etc/shadow itself: for privileged callers (pkexec,
 * nexus-polkitd) so they get the helper's five-tries lockout and 2 s delay. UNAVAILABLE (= refuse) without it. */
int aurora_auth_verify_helper(const char* user, const char* password) { return helper_verify(user, password); }
/* Debian's PAM password checker (libpam-modules-bin, set-group-ID shadow): the caller's own password, with the system's
 * libcrypt, so every hash passwd/chpasswd/PAM write is understood ($y$ yescrypt - Debian's default -, $gy$, $6$, $5$...).
 * Protocol (Linux-PAM unix_chkpwd.c): argv USER nonull, the password on stdin (not a tty) ending in NUL; exit 0 right,
 * 7 (PAM_AUTH_ERR) wrong. It refuses other users' passwords, so it is used for the caller's account only. */
static int chkpwd_verify(const char* user, const char* password) {
    static const char* const paths[] = {"/usr/sbin/unix_chkpwd", "/sbin/unix_chkpwd"};
    const char* path = NULL; struct aurora_user me;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(*paths) && !path; i++) if (!access(paths[i], X_OK)) path = paths[i];
    if (!path || !aurora_user_by_uid(getuid(), &me) || strcmp(me.name, user)) return AURORA_AUTH_UNAVAILABLE;
    int in[2]; if (pipe2(in, O_CLOEXEC)) return AURORA_AUTH_UNAVAILABLE;
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); return AURORA_AUTH_UNAVAILABLE; }
    if (!pid) {
        dup2(in[0], 0); int devnull = open("/dev/null", O_WRONLY); if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execl(path, "unix_chkpwd", user, "nonull", (char*)NULL); _exit(127);
    }
    close(in[0]);
    size_t length = strlen(password) + 1; ssize_t wrote = write(in[1], password, length); close(in[1]);   /* with its NUL */
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (wrote != (ssize_t)length || !WIFEXITED(status)) return AURORA_AUTH_UNAVAILABLE;
    return WEXITSTATUS(status) == 0 ? AURORA_AUTH_OK : WEXITSTATUS(status) == 7 ? AURORA_AUTH_WRONG : AURORA_AUTH_UNAVAILABLE;
}
/* In this process when /etc/shadow is readable (FAT roots, the set-user-ID helper itself); else the helper nexus-auth
 * (set-user-ID root: any account, failure tally); else, or when neither can compute the stored hash, unix_chkpwd. */
int aurora_auth_verify(const char* user, const char* password) {
    char hash[512]; int result;
    struct aurora_user u; if (!aurora_user_find(user, &u) && strcmp(user, "nexus")) return AURORA_AUTH_UNKNOWN_USER;
    if (aurora_shadow_hash(user, hash, sizeof(hash))) { result = aurora_check_hash(password, hash); memset(hash, 0, sizeof(hash)); }
    else result = helper_verify(user, password);   /* /etc/shadow is not ours to read */
    if (result == AURORA_AUTH_UNAVAILABLE) result = chkpwd_verify(user, password);
    return result;
}
const char* aurora_auth_message(int code) {
    switch (code) {
    case AURORA_AUTH_OK: return "OK";
    case AURORA_AUTH_WRONG: return "Incorrect password";
    case AURORA_AUTH_NO_PASSWORD: return "No password is set for this account";
    case AURORA_AUTH_LOCKED: return "This account is locked";
    case AURORA_AUTH_UNKNOWN_USER: return "No such user";
    case AURORA_AUTH_UNAVAILABLE: return "Password check unavailable";
    case AURORA_AUTH_DENIED: return "Not allowed";
    case AURORA_AUTH_READONLY: return "Accounts are read-only on this disk";
    case AURORA_AUTH_THROTTLED: return "Too many attempts: try again in a minute";
    default: return "Invalid request";
    }
}

/* ---- login.conf ---- */
void aurora_login_conf(struct aurora_login_conf* conf) {
    *conf = (struct aurora_login_conf){.autologin = false, .lock_on_sleep = true, .lock_after = 300};
    FILE* f = fopen(AURORA_LOGIN_CONF, "re"); if (!f) return;
    char line[128]; int v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "autologin=%d", &v) == 1) conf->autologin = v != 0;
        else if (sscanf(line, "lock_on_sleep=%d", &v) == 1) conf->lock_on_sleep = v != 0;
        else if (sscanf(line, "lock_after=%d", &v) == 1 && v >= 0 && v <= 86400) conf->lock_after = v;
    }
    fclose(f);
}
bool aurora_login_conf_save(const struct aurora_login_conf* conf) {
    mkdir("/home/nexus/.aurora", 0755);
    FILE* f = fopen(AURORA_LOGIN_CONF ".tmp", "we"); if (!f) return false;
    fprintf(f, "# Aurora login and lock settings (System Settings > Users).\nautologin=%d\nlock_after=%d\nlock_on_sleep=%d\n", conf->autologin, conf->lock_after, conf->lock_on_sleep);
    bool ok = !fflush(f) && !fsync(fileno(f)); fclose(f);
    return ok && !rename(AURORA_LOGIN_CONF ".tmp", AURORA_LOGIN_CONF);
}
