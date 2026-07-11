/* ============================================================================
 * NexusOS — User Accounts & Authentication (Implementation) — Phase 44
 * ============================================================================
 * See users.h for the hashing caveat (teaching-grade salted KDF, not bcrypt).
 * ============================================================================ */

#include "users.h"
#include "string.h"
#include "vga.h"

extern volatile uint32_t system_ticks;

static user_t users[USER_MAX];
static uint32_t current_uid = UID_ROOT;
static uint32_t next_uid    = 1000;     /* auto-assigned uids start here */
static uint32_t salt_seed   = 0x9E3779B9u; /* advanced on each new salt */

/* ----------------------------------------------------------------------------
 * Salted hash — integer-only FNV-1a with strengthening rounds.
 * --------------------------------------------------------------------------
 * Two independent accumulators (different primes) give a 64-bit-wide digest
 * without any 64-bit math. KDF_ROUNDS re-feeds the digest through the mixer so
 * a brute-force attempt pays the round cost per guess.
 * -------------------------------------------------------------------------- */
#define FNV_OFF0  2166136261u
#define FNV_PRM0  16777619u
#define FNV_OFF1  2166136261u
#define FNV_PRM1  709607u
#define KDF_ROUNDS 64

static uint32_t mix32(uint32_t x) {
    /* A bijective integer avalanche (xorshift-multiply, 32-bit). */
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

pwhash_t users_hash(const char* password, uint32_t salt) {
    uint32_t h0 = FNV_OFF0 ^ salt;
    uint32_t h1 = FNV_OFF1 ^ mix32(salt);

    /* Absorb the salt bytes first so it genuinely participates. */
    uint8_t sb[4] = { (uint8_t)salt, (uint8_t)(salt >> 8),
                      (uint8_t)(salt >> 16), (uint8_t)(salt >> 24) };
    for (int i = 0; i < 4; i++) {
        h0 = (h0 ^ sb[i]) * FNV_PRM0;
        h1 = (h1 ^ sb[i]) * FNV_PRM1;
    }
    /* Absorb the password bytes. */
    for (const char* p = password; *p; p++) {
        h0 = (h0 ^ (uint8_t)*p) * FNV_PRM0;
        h1 = (h1 ^ (uint8_t)*p) * FNV_PRM1;
    }
    /* Strengthening rounds: keep mixing the two words into each other. */
    for (int r = 0; r < KDF_ROUNDS; r++) {
        h0 = mix32(h0 ^ h1);
        h1 = mix32(h1 + h0 + (uint32_t)r);
    }

    pwhash_t out = { salt, h0, h1 };
    return out;
}

bool users_verify(const char* password, const pwhash_t* stored) {
    if (!stored) return false;
    pwhash_t h = users_hash(password, stored->salt);
    /* Compare both words; OR the diffs so timing is the same shape regardless
     * of which word differs (still not perfectly constant-time, but uniform). */
    uint32_t diff = (h.h0 ^ stored->h0) | (h.h1 ^ stored->h1);
    return diff == 0;
}

static uint32_t fresh_salt(void) {
    salt_seed = mix32(salt_seed ^ (system_ticks + 0x1234567u));
    if (salt_seed == 0) salt_seed = 0xA5A5A5A5u;
    return salt_seed;
}

/* ----------------------------------------------------------------------------
 * DB
 * -------------------------------------------------------------------------- */
static user_t* alloc_slot(void) {
    for (int i = 0; i < USER_MAX; i++)
        if (!users[i].used) return &users[i];
    return NULL;
}

static void make_user(user_t* u, uint32_t uid, uint32_t gid,
                      const char* name, const char* password) {
    u->uid = uid;
    u->gid = gid;
    strncpy(u->name, name, USER_NAME_MAX - 1);
    u->name[USER_NAME_MAX - 1] = '\0';
    u->pw = users_hash(password, fresh_salt());
    u->used = true;
}

void users_init(void) {
    for (int i = 0; i < USER_MAX; i++) users[i].used = false;
    current_uid = UID_ROOT;
    next_uid = 1000;

    /* Built-in accounts. Default passwords are intentionally simple for a demo
     * OS; `passwd` changes them. */
    make_user(&users[0], UID_ROOT, 0,    "root",  "root");
    make_user(&users[1], 1000,     1000, "guest", "guest");

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("User accounts initialized (root, guest; salted-hash auth)\n");
}

user_t* users_find(const char* name) {
    for (int i = 0; i < USER_MAX; i++)
        if (users[i].used && strcmp(users[i].name, name) == 0) return &users[i];
    return NULL;
}

user_t* users_find_uid(uint32_t uid) {
    for (int i = 0; i < USER_MAX; i++)
        if (users[i].used && users[i].uid == uid) return &users[i];
    return NULL;
}

const char* users_name_of(uint32_t uid) {
    user_t* u = users_find_uid(uid);
    return u ? u->name : "?";
}

int users_count(void) {
    int n = 0;
    for (int i = 0; i < USER_MAX; i++) if (users[i].used) n++;
    return n;
}

user_t* users_table(void) { return users; }

uint32_t users_authenticate(const char* name, const char* password) {
    user_t* u = users_find(name);
    if (!u) return UID_INVALID;
    if (!users_verify(password, &u->pw)) return UID_INVALID;
    return u->uid;
}

uint32_t users_add(const char* name, const char* password) {
    if (!name || !name[0]) return UID_INVALID;
    if (users_find(name)) return UID_INVALID;       /* no duplicates */
    user_t* u = alloc_slot();
    if (!u) return UID_INVALID;
    uint32_t uid = next_uid++;
    make_user(u, uid, uid, name, password);
    return uid;
}

bool users_del(const char* name) {
    user_t* u = users_find(name);
    if (!u) return false;
    if (u->uid == UID_ROOT) return false;           /* never delete root */
    if (u->uid == current_uid) return false;        /* not the logged-in user */
    u->used = false;
    return true;
}

bool users_set_password(const char* name, const char* password) {
    user_t* u = users_find(name);
    if (!u) return false;
    u->pw = users_hash(password, fresh_salt());
    return true;
}

/* ----------------------------------------------------------------------------
 * Current user
 * -------------------------------------------------------------------------- */
void users_set_current(uint32_t uid) { current_uid = uid; }
uint32_t users_current_uid(void) { return current_uid; }
const char* users_current_name(void) { return users_name_of(current_uid); }
bool users_current_is_root(void) { return current_uid == UID_ROOT; }
