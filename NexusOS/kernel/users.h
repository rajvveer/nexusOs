/* ============================================================================
 * NexusOS — User Accounts & Authentication (Header) — Phase 44
 * ============================================================================
 * A small in-memory user database with salted password hashing, plus the
 * "current user" tracking that the rest of the security layer (file
 * permissions, process ownership) keys off of.
 *
 * HASHING NOTE: we have no libc/libm/libgcc and no 64-bit math, so this uses an
 * integer-only salted FNV-1a key-derivation with multiple strengthening rounds
 * — NOT bcrypt/argon2/SHA. It is a *teaching-grade* KDF: it salts (so equal
 * passwords hash differently) and iterates (so brute force costs more), which
 * is already infinitely better than the previous "accept anything" login, but
 * it is not production cryptography. CRC32 (in pkg.c) was deliberately NOT
 * reused — it is a non-cryptographic checksum, trivially reversible.
 * ============================================================================ */

#ifndef USERS_H
#define USERS_H

#include "types.h"

#define USER_NAME_MAX   32
#define USER_MAX        16        /* max accounts */
#define UID_ROOT        0u
#define UID_INVALID     0xFFFFFFFFu

/* A salted password hash: two 32-bit words derived from password+salt, so
 * collisions are far less likely than a single word would give. */
typedef struct {
    uint32_t salt;
    uint32_t h0;
    uint32_t h1;
} pwhash_t;

typedef struct {
    uint32_t uid;
    uint32_t gid;
    char     name[USER_NAME_MAX];
    pwhash_t pw;
    bool     used;
} user_t;

/* Initialise the user DB with the built-in accounts (root + guest). Call once
 * at boot BEFORE login_run(). */
void users_init(void);

/* --- Hashing (exposed for tests / passwd) --- */
/* Derive a salted hash of `password` using `salt`. */
pwhash_t users_hash(const char* password, uint32_t salt);
/* Constant-shape compare of a password against a stored hash. */
bool users_verify(const char* password, const pwhash_t* stored);

/* --- Lookup --- */
user_t*     users_find(const char* name);
user_t*     users_find_uid(uint32_t uid);
const char* users_name_of(uint32_t uid);   /* "?" if unknown */
int         users_count(void);
user_t*     users_table(void);             /* for listing */

/* --- Mutation --- */
/* Authenticate: returns the uid on success, UID_INVALID on failure. */
uint32_t users_authenticate(const char* name, const char* password);
/* Add a user (auto-assigned uid >= 1000). Returns uid or UID_INVALID. */
uint32_t users_add(const char* name, const char* password);
/* Remove a user by name (root and the current user cannot be removed).
 * Returns true on success. */
bool users_del(const char* name);
/* Change a user's password. Returns true on success. */
bool users_set_password(const char* name, const char* password);

/* --- Current user (the logged-in identity) --- */
void        users_set_current(uint32_t uid);
uint32_t    users_current_uid(void);
const char* users_current_name(void);
bool        users_current_is_root(void);

#endif /* USERS_H */
