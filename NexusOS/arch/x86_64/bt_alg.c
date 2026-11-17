/* AF_ALG (Linux kernel crypto API sockets), the subset BlueZ's src/shared/crypto.c needs (bluetoothd, btvirt, the
 * BlueZ testers): socket(AF_ALG, SOCK_SEQPACKET, 0); bind(struct sockaddr_alg {family, type[14], feat, mask, name[64]})
 * to "skcipher"/"ecb(aes)" or "hash"/"cmac(aes)" (anything else: ENOENT); setsockopt(SOL_ALG, ALG_SET_KEY, 16/24/32
 * bytes); accept() gives an operation socket (ENOKEY without a key). skcipher: sendmsg/write data (ALG_SET_OP control
 * message: ALG_OP_ENCRYPT/DECRYPT; ALG_SET_IV accepted, unused by ECB), read returns the processed whole blocks (a
 * partial block is EINVAL, as in Linux). hash: send/write data (MSG_MORE keeps the message open), read returns the
 * 16-byte AES-CMAC tag and starts a new message. Keys are wiped when the sockets close. Software AES (bt_crypto.c). */
#include "bt.h"
#include "../../kernel/string.h"

#define ALG_DATA 4096U
struct alg { uint64_t id; bool op, bound, keyed, encrypt, finished; int kind; uint8_t key[32]; unsigned key_bytes; struct bt_aes aes; struct bt_cmac cmac; bool cmac_open; unsigned used; uint8_t data[ALG_DATA], tag[16]; };
static uint64_t alg_ids;
static int64_t alg_io(void* object, const struct iovec64* v, unsigned count, uint64_t total, bool write, uint64_t flags);
static int alg_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) { (void)object; (void)command; (void)argument; (void)flags; return E_NOTTY; }
static uint32_t alg_ready(void* object) { struct alg* a = object; return a->op ? 1U | 4U | 64U | 256U : 4U; }
static void* alg_source(void* object) { return object; }
static void alg_release(void* object) { struct alg* a = object; volatile uint8_t* p = (volatile uint8_t*)a; for (unsigned i = 0; i < sizeof(*a); i++) p[i] = 0; bt_free(a, sizeof(*a)); }
static void alg_stat(void* object, struct linux_stat* stat) { memset(stat, 0, sizeof(*stat)); stat->dev = 4; stat->ino = 0x63000000ULL + ((struct alg*)object)->id; stat->nlink = 1; stat->mode = 0140777; stat->block_size = 4096; }
static const struct object_operations alg_operations = {alg_io, alg_ioctl, alg_ready, alg_source, alg_release, alg_stat};
bool alg_descriptor(struct open_file* f) { return fd_object(f, &alg_operations) != NULL; }
static struct alg* alg_new(void) { struct alg* a = bt_zalloc(sizeof(*a)); if (a) a->id = ++alg_ids; return a; }
static void cmac_begin(struct alg* a) { if (!a->cmac_open) { bt_cmac_init(&a->cmac, &a->aes); a->cmac_open = true; } }
static int64_t take(struct alg* a, const struct iovec64* v, unsigned count, uint64_t total, bool more) {
    if (a->kind == 1 && a->used + total > ALG_DATA) return E_MSGSIZE;
    uint8_t chunk[256]; uint64_t at = 0;
    for (unsigned i = 0; i < count; i++) for (uint64_t off = 0; off < v[i].size;) {
        unsigned n = v[i].size - off < sizeof(chunk) ? (unsigned)(v[i].size - off) : sizeof(chunk);
        if (bt_from_user(chunk, v[i].address + off, n)) return E_FAULT;
        if (a->kind == 1) { memcpy(a->data + a->used, chunk, n); a->used += n; }
        else { cmac_begin(a); bt_cmac_update(&a->cmac, chunk, n); }
        off += n; at += n;
    }
    if (a->kind == 2 && !more) { cmac_begin(a); bt_cmac_final(&a->cmac, a->tag); a->cmac_open = false; a->finished = true; }
    return (int64_t)at;
}
static int64_t give(struct alg* a, const struct iovec64* v, unsigned count, uint64_t total) {
    if (a->kind == 2) {
        if (!a->finished) { cmac_begin(a); bt_cmac_final(&a->cmac, a->tag); a->cmac_open = false; }
        a->finished = false;
        return bt_copy_out_vectors(v, count, a->tag, total < 16 ? (unsigned)total : 16);
    }
    if (a->used % 16) return E_INVAL;
    unsigned n = (unsigned)(total < a->used ? total : a->used) & ~15U;
    for (unsigned i = 0; i < n; i += 16) { if (a->encrypt) bt_aes_encrypt(&a->aes, a->data + i); else bt_aes_decrypt(&a->aes, a->data + i); }
    int64_t r = bt_copy_out_vectors(v, count, a->data, n);
    bt_move(a->data, a->data + n, a->used - n); a->used -= n; return r;
}
static int64_t alg_io(void* object, const struct iovec64* v, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct alg* a = object; if (!a->op) return E_NOTCONN;
    return write ? take(a, v, count, total, (flags & 0x8000) != 0) : give(a, v, count, total);
}
struct sockaddr_alg { uint16_t family; uint8_t type[14]; uint32_t feat, mask; uint8_t name[64]; };
int64_t alg_call(struct registers* r, struct open_file* f) {
    if (r->rax == 41) {
        if ((r->rsi & 0xF) != 5) return E_SOCKTNOSUPPORT;
        if (r->rdx) return E_PROTONOSUPPORT;
        if (r->rsi & ~(0xFULL | 0x800 | 0x80000)) return E_INVAL;
        struct alg* a = alg_new(); if (!a) return E_NOMEM;
        int fd = fd_create_object(&alg_operations, a, (r->rsi & 0x800) | (r->rsi & 0x80000) | 2, "socket:[alg]");
        if (fd < 0) bt_free(a, sizeof(*a));
        return fd;
    }
    struct alg* a = fd_object(f, &alg_operations); if (!a) return -88;
    uint64_t flags = fd_status_flags(f); int64_t result = 0;
    fd_pin(f);
    switch (r->rax) {
    case 49: {
        struct sockaddr_alg s; memset(&s, 0, sizeof(s));
        if (a->op || a->bound) { result = E_INVAL; break; }
        if (r->rdx < sizeof(s)) { result = E_INVAL; break; }
        if (bt_from_user(&s, r->rsi, sizeof(s))) { result = E_FAULT; break; }
        s.type[13] = 0; s.name[63] = 0;
        if (s.family != 38) { result = E_INVAL; break; }
        if (!strcmp((char*)s.type, "skcipher") && !strcmp((char*)s.name, "ecb(aes)")) a->kind = 1;
        else if (!strcmp((char*)s.type, "hash") && !strcmp((char*)s.name, "cmac(aes)")) a->kind = 2;
        else { result = E_NOENT; break; }
        a->bound = true; break;
    }
    case 54: {
        if (r->rsi != 279 || a->op) { result = E_NOPROTOOPT; break; }
        if (r->rdx != 1) { result = r->rdx == 5 ? E_NOPROTOOPT : E_NOPROTOOPT; break; }   /* ALG_SET_KEY only */
        if (!a->bound) { result = E_NOENT; break; }
        if (r->r8 != 16 && r->r8 != 24 && r->r8 != 32) { result = E_INVAL; break; }
        uint8_t key[32]; if (bt_from_user(key, r->r10, r->r8)) { result = E_FAULT; break; }
        memcpy(a->key, key, r->r8); a->key_bytes = (unsigned)r->r8; bt_aes_key(&a->aes, key, a->key_bytes); a->keyed = true;
        { volatile uint8_t* p = key; for (unsigned i = 0; i < 32; i++) p[i] = 0; }
        break;
    }
    case 43: case 288: {
        if (a->op || !a->bound) { result = E_INVAL; break; }
        if (!a->keyed) { result = E_NOKEY; break; }
        uint64_t af = r->rax == 288 ? r->r10 : 0; if (af & ~(0x800ULL | 0x80000ULL)) { result = E_INVAL; break; }
        struct alg* o = alg_new(); if (!o) { result = E_NOMEM; break; }
        o->op = true; o->bound = true; o->keyed = true; o->kind = a->kind; o->encrypt = true; memcpy(o->key, a->key, 32); o->key_bytes = a->key_bytes; o->aes = a->aes;
        int fd = fd_create_object(&alg_operations, o, (af & 0x800) | (af & 0x80000) | 2, "socket:[alg]");
        if (fd < 0) alg_release(o);
        result = fd; break;
    }
    case 44: { if (!a->op) { result = E_NOTCONN; break; } struct iovec64 v = {r->rsi, r->rdx}; result = take(a, &v, 1, r->rdx, (r->r10 & 0x8000) != 0); break; }
    case 45: { if (!a->op) { result = E_NOTCONN; break; } struct iovec64 v = {r->rsi, r->rdx}; result = give(a, &v, 1, r->rdx); break; }
    case 46: case 47: {
        if (!a->op) { result = E_NOTCONN; break; }
        struct message64 m; struct iovec64 v[64]; unsigned count; uint64_t total;
        int e = bt_read_message(r->rsi, &m, v, &count, &total); if (e) { result = e; break; }
        if (r->rax == 47) { result = give(a, v, count, total); if (result >= 0) { m.flags = 0; m.control_size = 0; m.name_size = 0; if (bt_to_user(r->rsi, &m, sizeof(m))) result = E_FAULT; } break; }
        if (m.control && m.control_size) {                                 /* ALG_SET_OP / ALG_SET_IV / ALG_SET_AEAD_ASSOCLEN */
            uint8_t control[256]; uint64_t n = m.control_size < sizeof(control) ? m.control_size : sizeof(control);
            if (bt_from_user(control, m.control, n)) { result = E_FAULT; break; }
            for (uint64_t at = 0; at + 16 <= n;) {
                uint64_t len; int level, type; memcpy(&len, control + at, 8); memcpy(&level, control + at + 8, 4); memcpy(&type, control + at + 12, 4);
                if (len < 16 || at + len > n) { result = E_INVAL; break; }
                if (level == 279 && type == 3) { if (len < 20) { result = E_INVAL; break; } uint32_t op; memcpy(&op, control + at + 16, 4); if (op > 1) { result = E_INVAL; break; } a->encrypt = op == 1; }
                at += (len + 7) & ~7ULL;
            }
            if (result) break;
        }
        result = take(a, v, count, total, (r->rdx & 0x8000) != 0); break;
    }
    case 51: case 52: result = E_OPNOTSUPP; break;
    case 55: result = r->rsi == 1 ? bt_sol_socket_get(r->rdx, r->r10, r->r8, 5, 0, 0, 65536, 65536) : E_NOPROTOOPT; break;
    case 48: result = 0; break;
    default: result = E_OPNOTSUPP;
    }
    fd_unpin(f);
    (void)flags;
    return result;
}
