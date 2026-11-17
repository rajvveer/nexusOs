/* Kernel key retention service (Linux security/keys subset): add_key(2), request_key(2), keyctl(2), and the lookup
 * dm-crypt uses for ":<size>:logon:<description>" keys (cryptsetup hands LUKS volume keys over this way).
 *
 * Key types: "user" (payload readable by its owner), "logon" (never readable from user space; description must have a
 * "<subtype>:" prefix), "keyring". Special keyrings: thread (-1) and process (-2) keyrings belong to the calling process
 * (identified by PID and start time, so a recycled PID never inherits them), session (-3) belongs to the session ID
 * (KEYCTL_JOIN_SESSION_KEYRING gives the caller a new anonymous or named one), user (-4) and user-session (-5) to the
 * UID. A key lives while a keyring links it; keys of exited processes' private keyrings are reclaimed when a later call
 * notices the process is gone. Permissions: the owner (or a CAP_SYS_ADMIN caller) may view, read (not logon keys),
 * write, search, link and set attributes; users without CAP_SYS_ADMIN may own 200 keys / 20000 bytes (Linux quota).
 * keyctl: GET_KEYRING_ID, JOIN_SESSION_KEYRING, UPDATE, REVOKE, CHOWN, SETPERM, DESCRIBE, CLEAR, LINK, UNLINK, SEARCH,
 * READ, SET_TIMEOUT, INVALIDATE, GET_PERSISTENT (= the user keyring); the rest are EOPNOTSUPP. Payloads are wiped
 * when keys are freed. */
#include "native.h"
#include "../../kernel/vga.h"
#include "storage_util.h"
bool credentials_kernel_from_visible(uint32_t id, bool group, uint32_t* result);   /* credentials.c */

#define MAX_KEYS 1024
#define MAX_LINKS 64
#define PAYLOAD_MAX 32767
enum { K_USER = 1, K_LOGON, K_KEYRING };
enum { OWNER_PROCESS = 1, OWNER_THREAD, OWNER_SESSION, OWNER_USER, OWNER_USER_SESSION };
struct key {
    int32_t serial; unsigned type; char description[256];
    uint8_t* payload; uint32_t length;
    uint32_t uid, gid, perm; uint64_t expiry; bool revoked;
    int32_t links[MAX_LINKS]; unsigned link_count;      /* keyrings: their members */
    unsigned owner_kind; uint64_t owner_id, owner_stamp;  /* special keyrings: whose they are */
    unsigned referenced;                                  /* links to this key from keyrings */
};
static struct key* keys[MAX_KEYS];
static int32_t next_serial = 0x10000;
static struct key* find(int32_t serial) { for (unsigned i = 0; i < MAX_KEYS; i++) if (keys[i] && keys[i]->serial == serial) return keys[i]; return NULL; }
static bool expired(const struct key* k) { return k->revoked || (k->expiry && native_realtime_ns() >= k->expiry); }
static void wipe_free(struct key* k) {
    for (unsigned i = 0; i < MAX_KEYS; i++) if (keys[i] == k) keys[i] = NULL;
    if (k->payload) { memset(k->payload, 0, k->length); arena_free((uint64_t)k->payload, (k->length + 4095) / 4096 * 4096); }
    memset(k, 0, sizeof(*k)); arena_free((uint64_t)k, sizeof(*k));
}
static void unlink_member(struct key* ring, unsigned index);
static void drop(struct key* k) {   /* a link went: free it when nothing links it and it is not someone's special keyring */
    if (k->referenced) k->referenced--;
    if (k->referenced || k->owner_kind) return;
    if (k->type == K_KEYRING) while (k->link_count) unlink_member(k, k->link_count - 1);
    wipe_free(k);
}
static void unlink_member(struct key* ring, unsigned index) {
    struct key* member = find(ring->links[index]);
    ring->links[index] = ring->links[--ring->link_count];
    if (member) drop(member);
}
static struct key* new_key(unsigned type, const char* description, const uint8_t* payload, uint32_t length) {
    unsigned slot = 0; while (slot < MAX_KEYS && keys[slot]) slot++;
    if (slot == MAX_KEYS) return NULL;
    struct key* k = (void*)arena_alloc(sizeof(*k)); if (!k) return NULL;
    memset(k, 0, sizeof(*k));
    if (length) {
        k->payload = (void*)arena_alloc((length + 4095) / 4096 * 4096);
        if (!k->payload) { arena_free((uint64_t)k, sizeof(*k)); return NULL; }
        memcpy(k->payload, payload, length); k->length = length;
    }
    k->serial = next_serial++; if (next_serial < 0x10000) next_serial = 0x10000;
    k->type = type; strncpy(k->description, description, 255);
    struct credentials* who = credentials_current();
    k->uid = credentials_kernel_id(who, false, 1); k->gid = credentials_kernel_id(who, true, 1); k->perm = 0x3f010000;
    keys[slot] = k; return k;
}
static bool link_into(struct key* ring, struct key* k) {
    for (unsigned i = 0; i < ring->link_count; i++) if (ring->links[i] == k->serial) return true;
    if (ring->link_count == MAX_LINKS) return false;
    ring->links[ring->link_count++] = k->serial; k->referenced++; return true;
}
/* The calling process's identity: PID and start time. */
static void process_identity(uint64_t* id, uint64_t* stamp, uint64_t* session) {
    struct task_info info; uint32_t tid = task_tid();
    if (!task_info(tid, false, &info)) { *id = tid; *stamp = 0; *session = 0; return; }
    *id = info.pid; *stamp = info.start_ns; *session = info.session;
}
static void reap(void) {   /* private keyrings of processes that are gone */
    for (unsigned i = 0; i < MAX_KEYS; i++) {
        struct key* k = keys[i];
        if (!k || (k->owner_kind != OWNER_PROCESS && k->owner_kind != OWNER_THREAD)) continue;
        struct task_info info;
        if (task_info((uint32_t)k->owner_id, false, &info) && info.start_ns == k->owner_stamp) continue;
        k->owner_kind = 0; k->referenced++; drop(k);
    }
}
static struct key* special(int32_t id, bool create) {
    uint64_t pid, stamp, session; process_identity(&pid, &stamp, &session);
    uint32_t uid = credentials_kernel_id(credentials_current(), false, 1);
    unsigned kind; uint64_t owner; uint64_t owner_stamp = 0;
    switch (id) {
    case -1: kind = OWNER_THREAD; owner = pid; owner_stamp = stamp; break;   /* one keyring per process for its threads */
    case -2: kind = OWNER_PROCESS; owner = pid; owner_stamp = stamp; break;
    case -3: kind = OWNER_SESSION; owner = session; break;
    case -4: kind = OWNER_USER; owner = uid; break;
    case -5: kind = OWNER_USER_SESSION; owner = uid; break;
    default: return NULL;
    }
    for (unsigned i = 0; i < MAX_KEYS; i++) {
        struct key* k = keys[i];
        if (k && k->owner_kind == kind && k->owner_id == owner && k->owner_stamp == owner_stamp && !k->revoked) return k;
    }
    if (kind == OWNER_SESSION) {   /* no session keyring yet: the user-session keyring stands in (Linux) */
        struct key* user_session = special(-5, create);
        if (user_session) return user_session;
    }
    if (!create && kind != OWNER_USER && kind != OWNER_USER_SESSION) return NULL;
    reap();
    char name[64];
    strcpy(name, kind == OWNER_THREAD ? "_tid" : kind == OWNER_PROCESS ? "_pid" : kind == OWNER_SESSION ? "_ses" : kind == OWNER_USER ? "_uid." : "_uid_ses.");
    if (kind == OWNER_USER || kind == OWNER_USER_SESSION) append_u64(name, credentials_visible_id(uid, false));
    struct key* k = new_key(K_KEYRING, name, NULL, 0); if (!k) return NULL;
    k->owner_kind = kind; k->owner_id = owner; k->owner_stamp = owner_stamp;
    return k;
}
static struct key* resolve(int32_t id, bool create) { return id < 0 ? special(id, create) : find(id); }
static bool permitted(const struct key* k) {
    return k->uid == credentials_kernel_id(credentials_current(), false, 1) || credentials_capable(credentials_current(), NULL, 21);
}
static int type_of(const char* name) { return !strcmp(name, "user") ? K_USER : !strcmp(name, "logon") ? K_LOGON : !strcmp(name, "keyring") ? K_KEYRING : 0; }
static const char* type_name(unsigned type) { return type == K_USER ? "user" : type == K_LOGON ? "logon" : "keyring"; }
static struct key* search(struct key* ring, unsigned type, const char* description, unsigned depth) {
    if (!ring || depth > 6 || expired(ring)) return NULL;
    for (unsigned i = 0; i < ring->link_count; i++) {
        struct key* k = find(ring->links[i]);
        if (!k || expired(k)) continue;
        if (k->type == type && !strcmp(k->description, description)) return k;
    }
    for (unsigned i = 0; i < ring->link_count; i++) {
        struct key* k = find(ring->links[i]);
        if (k && k->type == K_KEYRING) { struct key* found = search(k, type, description, depth + 1); if (found) return found; }
    }
    return NULL;
}
static struct key* search_process(unsigned type, const char* description) {   /* thread, process, session, user keyrings */
    static const int32_t order[4] = {-1, -2, -3, -4};
    for (unsigned i = 0; i < 4; i++) { struct key* k = search(special(order[i], false), type, description, 0); if (k) return k; }
    return NULL;
}
/* dm.c: the payload of a logon/user key the current process can reach; its length, or -ENOKEY/-EKEYREVOKED. */
int keyring_lookup(const char* type_text, const char* description, uint8_t* out, unsigned capacity) {
    int type = type_of(type_text); if (type != K_LOGON && type != K_USER) return -22;
    struct key* k = search_process((unsigned)type, description);
    if (!k) return -126;
    if (k->revoked) return -128;
    if (k->length > capacity) return -22;
    memcpy(out, k->payload, k->length); return (int)k->length;
}

static int copy_string(uint64_t address, char* out, unsigned capacity) {
    for (unsigned i = 0; i < capacity; i++) {
        if (!accessible(address + i, 1, false)) return -14;
        out[i] = (char)uget8(address + i);
        if (!out[i]) return 0;
    }
    return -22;
}
static int64_t add_key(struct registers* r) {
    char type_text[32], description[256];
    int e = copy_string(r->rdi, type_text, sizeof(type_text)); if (e) return e;
    int type = type_of(type_text); if (!type) return -19;
    if (!r->rsi) return -22;
    if ((e = copy_string(r->rsi, description, sizeof(description)))) return e;
    if (!description[0]) return -22;
    if (type == K_LOGON && !s_strchr(description, ':')) return -22;
    uint64_t length = r->r10;
    if (type == K_KEYRING ? (r->rdx || length) : length > PAYLOAD_MAX) return -22;
    if (length && !accessible(r->rdx, length, false)) return -14;
    struct key* ring = resolve((int32_t)r->r8, true);
    if (!ring || ring->type != K_KEYRING || expired(ring)) return -126;
    if (!permitted(ring)) return -13;
    for (unsigned i = 0; i < ring->link_count; i++) {   /* same type and description: updated in place */
        struct key* k = find(ring->links[i]);
        if (!k || k->type != (unsigned)type || strcmp(k->description, description) || k->type == K_KEYRING) continue;
        uint8_t* fresh = length ? (void*)arena_alloc((length + 4095) / 4096 * 4096) : NULL;
        if (length && !fresh) return -12;
        if (length) copy_from_user(fresh, (const void*)r->rdx, length);
        if (k->payload) { memset(k->payload, 0, k->length); arena_free((uint64_t)k->payload, (k->length + 4095) / 4096 * 4096); }
        k->payload = fresh; k->length = (uint32_t)length; k->revoked = false;
        return k->serial;
    }
    if (!credentials_capable(credentials_current(), NULL, 21)) {   /* Linux key quota for users: 200 keys, 20000 bytes */
        uint32_t uid = credentials_kernel_id(credentials_current(), false, 1); unsigned count = 0; uint64_t bytes = length;
        for (unsigned i = 0; i < MAX_KEYS; i++) if (keys[i] && keys[i]->uid == uid) { count++; bytes += keys[i]->length; }
        if (count >= 200 || bytes > 20000) return -122;
    }
    uint8_t* copy = NULL;
    if (length) { copy = (void*)arena_alloc((length + 4095) / 4096 * 4096); if (!copy) return -12; copy_from_user(copy, (const void*)r->rdx, length); }
    struct key* k = new_key((unsigned)type, description, copy, (uint32_t)length);
    if (copy) { memset(copy, 0, length); arena_free((uint64_t)copy, (length + 4095) / 4096 * 4096); }
    if (!k) return -122;   /* EDQUOT */
    if (!link_into(ring, k)) { wipe_free(k); return -122; }
    return k->serial;
}
static int64_t request_key(struct registers* r) {
    char type_text[32], description[256];
    int e = copy_string(r->rdi, type_text, sizeof(type_text)); if (e) return e;
    if ((e = copy_string(r->rsi, description, sizeof(description)))) return e;
    int type = type_of(type_text); if (!type) return -19;
    struct key* k = search_process((unsigned)type, description);
    if (!k) return -126;   /* no upcall to /sbin/request-key */
    if (k->revoked) return -128;
    if ((int32_t)r->r10) { struct key* dest = resolve((int32_t)r->r10, true); if (dest && dest->type == K_KEYRING && permitted(dest)) link_into(dest, k); }
    return k->serial;
}
static int64_t keyctl(struct registers* r) {
    uint64_t op = r->rdi; int32_t id = (int32_t)r->rsi;
    switch (op) {
    case 0: { struct key* k = resolve(id, r->rdx != 0); return k ? k->serial : -126; }   /* GET_KEYRING_ID */
    case 1: {   /* JOIN_SESSION_KEYRING: a new session keyring for this session ID */
        char name[256] = "_ses";
        if (r->rsi && copy_string(r->rsi, name, sizeof(name))) return -14;
        uint64_t pid, stamp, session; process_identity(&pid, &stamp, &session);
        for (unsigned i = 0; i < MAX_KEYS; i++) if (keys[i] && keys[i]->owner_kind == OWNER_SESSION && keys[i]->owner_id == session) { keys[i]->owner_kind = 0; keys[i]->referenced++; drop(keys[i]); }
        struct key* k = new_key(K_KEYRING, name, NULL, 0); if (!k) return -122;
        k->owner_kind = OWNER_SESSION; k->owner_id = session; return k->serial;
    }
    case 17: return -95;   /* GET_SECURITY */
    case 22: { struct key* k = special(-4, true); if (!k) return -126; if ((int32_t)r->rdx) { struct key* dest = resolve((int32_t)r->rdx, true); if (dest && dest->type == K_KEYRING) link_into(dest, k); } return k->serial; }
    }
    struct key* k = resolve(id, false);
    if (!k) return -126;
    if (!permitted(k)) return -13;
    switch (op) {
    case 2: {   /* UPDATE */
        if (k->type == K_KEYRING) return -95;
        uint64_t length = r->r10; if (length > PAYLOAD_MAX) return -22;
        if (length && !accessible(r->rdx, length, false)) return -14;
        uint8_t* fresh = length ? (void*)arena_alloc((length + 4095) / 4096 * 4096) : NULL;
        if (length && !fresh) return -12;
        if (length) copy_from_user(fresh, (const void*)r->rdx, length);
        if (k->payload) { memset(k->payload, 0, k->length); arena_free((uint64_t)k->payload, (k->length + 4095) / 4096 * 4096); }
        k->payload = fresh; k->length = (uint32_t)length; return 0;
    }
    case 3: k->revoked = true; if (k->payload) memset(k->payload, 0, k->length); return 0;   /* REVOKE */
    case 4: {   /* CHOWN */
        if (!credentials_capable(credentials_current(), NULL, 21)) return -13;
        uint32_t value;
        if ((int32_t)r->rdx != -1) { if (!credentials_kernel_from_visible((uint32_t)r->rdx, false, &value)) return -22; k->uid = value; }
        if ((int32_t)r->r10 != -1) { if (!credentials_kernel_from_visible((uint32_t)r->r10, true, &value)) return -22; k->gid = value; }
        return 0;
    }
    case 5: k->perm = (uint32_t)r->rdx; return 0;   /* SETPERM */
    case 6: {   /* DESCRIBE: "type;uid;gid;perm;description" */
        char text[400]; strcpy(text, type_name(k->type)); strcat(text, ";"); append_u64(text, credentials_visible_id(k->uid, false));
        strcat(text, ";"); append_u64(text, credentials_visible_id(k->gid, true)); strcat(text, ";"); append_hex(text, k->perm, 8);
        strcat(text, ";"); strcat(text, k->description);
        uint64_t length = strlen(text) + 1;
        if (r->rdx && r->r10) { uint64_t n = length < r->r10 ? length : r->r10; if (!accessible(r->rdx, n, true)) return -14; copy_to_user((void*)r->rdx, text, n); }
        return (int64_t)length;
    }
    case 7: if (k->type != K_KEYRING) return -20; while (k->link_count) unlink_member(k, k->link_count - 1); return 0;   /* CLEAR */
    case 8: {   /* LINK key into keyring */
        struct key* ring = resolve((int32_t)r->rdx, true);
        if (!ring || ring->type != K_KEYRING) return -20;
        if (ring == k) return -35;
        return link_into(ring, k) ? 0 : -122;
    }
    case 9: {   /* UNLINK */
        struct key* ring = resolve((int32_t)r->rdx, false);
        if (!ring || ring->type != K_KEYRING) return -20;
        for (unsigned i = 0; i < ring->link_count; i++) if (ring->links[i] == k->serial) { unlink_member(ring, i); return 0; }
        return -2;
    }
    case 10: {   /* SEARCH ring type description dest */
        char type_text[32], description[256];
        if (copy_string(r->rdx, type_text, sizeof(type_text)) || copy_string(r->r10, description, sizeof(description))) return -14;
        int type = type_of(type_text); if (!type) return -19;
        if (k->type != K_KEYRING) return -20;
        struct key* found = search(k, (unsigned)type, description, 0);
        if (!found) return -126;
        if ((int32_t)r->r8) { struct key* dest = resolve((int32_t)r->r8, true); if (dest && dest->type == K_KEYRING) link_into(dest, found); }
        return found->serial;
    }
    case 11: {   /* READ */
        if (k->type == K_LOGON) return -95;
        if (expired(k)) return -128;
        uint64_t length = k->type == K_KEYRING ? k->link_count * 4ULL : k->length;
        if (r->rdx && r->r10) {
            uint64_t n = length < r->r10 ? length : r->r10;
            if (!accessible(r->rdx, n, true)) return -14;
            if (k->type == K_KEYRING) { for (unsigned i = 0; i < k->link_count && (i + 1) * 4ULL <= n; i++) copy_to_user((uint8_t*)r->rdx + i * 4, &k->links[i], 4); }
            else copy_to_user((void*)r->rdx, k->payload, n);
        }
        return (int64_t)length;
    }
    case 15: k->expiry = r->rdx ? native_realtime_ns() + r->rdx * 1000000000ULL : 0; return 0;   /* SET_TIMEOUT */
    case 21: {   /* INVALIDATE: gone from every keyring (k may be freed by the last unlink) */
        int32_t serial = k->serial; k->revoked = true;
        for (unsigned i = 0; i < MAX_KEYS; i++) {
            struct key* ring = keys[i]; if (!ring || ring->type != K_KEYRING) continue;
            for (unsigned j = 0; j < ring->link_count; j++) if (ring->links[j] == serial) { unlink_member(ring, j); break; }
        }
        return 0;
    }
    }
    return -95;
}
int64_t keyring_call(struct registers* r) {
    switch (r->rax) {
    case 248: return add_key(r);
    case 249: return request_key(r);
    case 250: return keyctl(r);
    }
    return -38;
}
