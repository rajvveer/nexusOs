/* Bluetooth cryptography (bt.h): AES-128/192/256 (FIPS-197) encrypt/decrypt, AES-CMAC (RFC 4493), the SMP functions of
 * the Core specification (Vol 3 Part H 2.2: e, c1, s1, ah, f4, f5, f6, g2, h6, h7) and P-256 ECDH (FIPS 186-4 curve,
 * Vol 2 Part H 7.6: public keys are validated - not on the curve, out of range or the point at infinity are refused,
 * CVE-2018-5383). Software only, constant time where secrets are involved: AES S-boxes come from GF(2^8) inversion
 * by a fixed multiplication chain (no table indexed by secret data), field arithmetic is Montgomery multiplication with
 * branch-free final subtraction, scalar multiplication a Montgomery ladder with conditional swaps.
 * SMP values are little-endian byte arrays as they travel in PDUs; the spec's MSB-first functions are applied to
 * byte-swapped copies, as the spec's notation requires.
 * bt_crypto_selftest() checks FIPS-197 and RFC 4493 known answers, the Core specification's SMP sample data and the
 * SMP debug key pair (private -> public), and ECDH agreement on random keys. */
#include "bt.h"
#include "../../kernel/string.h"

/* ---------- AES ---------- */
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (unsigned i = 0; i < 8; i++) { p ^= (uint8_t)(-(b & 1) & a); uint8_t high = (uint8_t)-(a >> 7); a = (uint8_t)((a << 1) ^ (high & 0x1B)); b >>= 1; }
    return p;
}
static uint8_t ginv(uint8_t x) {   /* x^254 */
    uint8_t x2 = gmul(x, x), x3 = gmul(x2, x), x6 = gmul(x3, x3), x12 = gmul(x6, x6), x15 = gmul(x12, x3);
    uint8_t x30 = gmul(x15, x15), x60 = gmul(x30, x30), x120 = gmul(x60, x60), x126 = gmul(x120, x6), x252 = gmul(x126, x126);
    return gmul(x252, x2);
}
static uint8_t rotl8(uint8_t x, unsigned n) { return (uint8_t)(x << n | x >> (8 - n)); }
static uint8_t sbox(uint8_t x) { uint8_t b = ginv(x); return (uint8_t)(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63); }
static uint8_t inv_sbox(uint8_t x) { return ginv((uint8_t)(rotl8(x, 1) ^ rotl8(x, 3) ^ rotl8(x, 6) ^ 0x05)); }
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (-(x >> 7) & 0x1B)); }

int bt_aes_key(struct bt_aes* k, const uint8_t* key, unsigned bytes) {
    if (bytes != 16 && bytes != 24 && bytes != 32) return -22;
    unsigned nk = bytes / 4, nr = nk + 6, words = 4 * (nr + 1);
    uint8_t* w = k->round; memcpy(w, key, bytes); uint8_t rcon = 1;
    for (unsigned i = nk; i < words; i++) {
        uint8_t t[4]; memcpy(t, w + 4 * (i - 1), 4);
        if (i % nk == 0) { uint8_t first = t[0]; t[0] = (uint8_t)(sbox(t[1]) ^ rcon); t[1] = sbox(t[2]); t[2] = sbox(t[3]); t[3] = sbox(first); rcon = xtime(rcon); }
        else if (nk > 6 && i % nk == 4) for (unsigned j = 0; j < 4; j++) t[j] = sbox(t[j]);
        for (unsigned j = 0; j < 4; j++) w[4 * i + j] = (uint8_t)(w[4 * (i - nk) + j] ^ t[j]);
    }
    k->rounds = nr; return 0;
}
void bt_aes_wipe(struct bt_aes* k) { volatile uint8_t* p = (volatile uint8_t*)k; for (unsigned i = 0; i < sizeof(*k); i++) p[i] = 0; }
static void add_key(uint8_t* s, const uint8_t* k) { for (unsigned i = 0; i < 16; i++) s[i] ^= k[i]; }
void bt_aes_encrypt(const struct bt_aes* k, uint8_t* s) {
    add_key(s, k->round);
    for (unsigned round = 1; round <= k->rounds; round++) {
        uint8_t t[16];
        for (unsigned c = 0; c < 4; c++) for (unsigned r = 0; r < 4; r++) t[4 * c + r] = sbox(s[4 * ((c + r) % 4) + r]);   /* SubBytes + ShiftRows */
        memcpy(s, t, 16);
        if (round != k->rounds) for (unsigned c = 0; c < 4; c++) {
            uint8_t* col = s + 4 * c; uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
            col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1))); col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
            col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3))); col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
        }
        add_key(s, k->round + 16 * round);
    }
}
void bt_aes_decrypt(const struct bt_aes* k, uint8_t* s) {
    add_key(s, k->round + 16 * k->rounds);
    for (unsigned round = k->rounds; round-- > 0;) {
        uint8_t t[16];
        for (unsigned c = 0; c < 4; c++) for (unsigned r = 0; r < 4; r++) t[4 * ((c + r) % 4) + r] = inv_sbox(s[4 * c + r]);   /* InvShiftRows + InvSubBytes */
        memcpy(s, t, 16); add_key(s, k->round + 16 * round);
        if (round) for (unsigned c = 0; c < 4; c++) {
            uint8_t* col = s + 4 * c; uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9)); col[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            col[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11)); col[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }
}

/* ---------- AES-CMAC (RFC 4493), MSB-first strings ---------- */
static void dbl(uint8_t* b) { uint8_t carry = (uint8_t)(b[0] >> 7); for (unsigned i = 0; i < 15; i++) b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7); b[15] = (uint8_t)((b[15] << 1) ^ (-carry & 0x87)); }
void bt_cmac_init(struct bt_cmac* m, const struct bt_aes* key) { memset(m, 0, sizeof(*m)); m->key = key; }
void bt_cmac_update(struct bt_cmac* m, const uint8_t* data, unsigned length) {
    while (length) {
        if (m->used == 16) { add_key(m->x, m->block); bt_aes_encrypt(m->key, m->x); m->used = 0; }   /* only a block known not to be last */
        unsigned take = 16 - m->used; if (take > length) take = length;
        memcpy(m->block + m->used, data, take); m->used += take; data += take; length -= take;
    }
}
void bt_cmac_final(struct bt_cmac* m, uint8_t* tag) {
    uint8_t k1[16] = {0}; bt_aes_encrypt(m->key, k1); dbl(k1);
    if (m->used == 16) add_key(m->block, k1);
    else { uint8_t k2[16]; memcpy(k2, k1, 16); dbl(k2); m->block[m->used] = 0x80; memset(m->block + m->used + 1, 0, 15 - m->used); add_key(m->block, k2); }
    add_key(m->x, m->block); bt_aes_encrypt(m->key, m->x); memcpy(tag, m->x, 16);
    volatile uint8_t* p = (volatile uint8_t*)m; for (unsigned i = 0; i < sizeof(*m); i++) p[i] = 0;
}
void bt_cmac(const uint8_t* key, unsigned key_bytes, const uint8_t* data, unsigned length, uint8_t* tag) {
    struct bt_aes k; bt_aes_key(&k, key, key_bytes); struct bt_cmac m; bt_cmac_init(&m, &k); bt_cmac_update(&m, data, length); bt_cmac_final(&m, tag); bt_aes_wipe(&k);
}

/* ---------- SMP functions (little-endian arguments) ---------- */
static void swap(uint8_t* out, const uint8_t* in, unsigned n) { for (unsigned i = 0; i < n; i++) out[i] = in[n - 1 - i]; }
static void wipe(void* p, unsigned n) { volatile uint8_t* q = p; while (n--) *q++ = 0; }
void bt_smp_e(const uint8_t* key, const uint8_t* in, uint8_t* out) {   /* e(k, r) */
    uint8_t k[16], b[16]; swap(k, key, 16); swap(b, in, 16);
    struct bt_aes a; bt_aes_key(&a, k, 16); bt_aes_encrypt(&a, b); bt_aes_wipe(&a); swap(out, b, 16); wipe(k, 16); wipe(b, 16);
}
static void cmac_le(const uint8_t* key, const uint8_t* m, unsigned n, uint8_t* out) {   /* AES-CMAC_key(m) on LE values */
    uint8_t k[16], msb[96], t[16]; swap(k, key, 16); swap(msb, m, n); bt_cmac(k, 16, msb, n, t); swap(out, t, 16); wipe(k, 16); wipe(msb, n); wipe(t, 16);
}
void bt_smp_c1(const uint8_t* k, const uint8_t* r, const uint8_t* preq, const uint8_t* pres, uint8_t iat, const uint8_t* ia, uint8_t rat, const uint8_t* ra, uint8_t* out) {
    uint8_t p1[16], p2[16], v[16];
    p1[0] = iat; p1[1] = rat; memcpy(p1 + 2, preq, 7); memcpy(p1 + 9, pres, 7);   /* p1 = pres || preq || rat' || iat' */
    memcpy(p2, ra, 6); memcpy(p2 + 6, ia, 6); memset(p2 + 12, 0, 4);             /* p2 = padding || ia || ra */
    for (unsigned i = 0; i < 16; i++) v[i] = r[i] ^ p1[i];
    bt_smp_e(k, v, v); for (unsigned i = 0; i < 16; i++) v[i] ^= p2[i]; bt_smp_e(k, v, out); wipe(v, 16);
}
void bt_smp_s1(const uint8_t* k, const uint8_t* r1, const uint8_t* r2, uint8_t* out) { uint8_t r[16]; memcpy(r, r2, 8); memcpy(r + 8, r1, 8); bt_smp_e(k, r, out); wipe(r, 16); }
void bt_smp_ah(const uint8_t* irk, const uint8_t* r, uint8_t* out) { uint8_t b[16] = {0}; memcpy(b, r, 3); bt_smp_e(irk, b, b); memcpy(out, b, 3); }
void bt_smp_f4(const uint8_t* u, const uint8_t* v, const uint8_t* x, uint8_t z, uint8_t* out) {
    uint8_t m[65]; m[0] = z; memcpy(m + 1, v, 32); memcpy(m + 33, u, 32); cmac_le(x, m, 65, out);
}
void bt_smp_f5(const uint8_t* w, const uint8_t* n1, const uint8_t* n2, const uint8_t* a1, const uint8_t* a2, uint8_t* mackey, uint8_t* ltk) {
    static const uint8_t salt[16] = {0xbe, 0x83, 0x60, 0x5a, 0xdb, 0x0b, 0x37, 0x60, 0x38, 0xa5, 0xf5, 0xaa, 0x91, 0x83, 0x88, 0x6c};
    uint8_t t[16], m[53]; cmac_le(salt, w, 32, t);
    m[0] = 0x00; m[1] = 0x01; memcpy(m + 2, a2, 7); memcpy(m + 9, a1, 7); memcpy(m + 16, n2, 16); memcpy(m + 32, n1, 16);
    m[48] = 0x65; m[49] = 0x6c; m[50] = 0x74; m[51] = 0x62;   /* "btle" */
    m[52] = 0; cmac_le(t, m, 53, mackey); m[52] = 1; cmac_le(t, m, 53, ltk); wipe(t, 16); wipe(m, 53);
}
void bt_smp_f6(const uint8_t* w, const uint8_t* n1, const uint8_t* n2, const uint8_t* r, const uint8_t* iocap, const uint8_t* a1, const uint8_t* a2, uint8_t* out) {
    uint8_t m[65]; memcpy(m, a2, 7); memcpy(m + 7, a1, 7); memcpy(m + 14, iocap, 3); memcpy(m + 17, r, 16); memcpy(m + 33, n2, 16); memcpy(m + 49, n1, 16);
    cmac_le(w, m, 65, out);
}
uint32_t bt_smp_g2(const uint8_t* u, const uint8_t* v, const uint8_t* x, const uint8_t* y) {
    uint8_t m[80], t[16]; memcpy(m, y, 16); memcpy(m + 16, v, 32); memcpy(m + 48, u, 32); cmac_le(x, m, 80, t);
    uint32_t value = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24; return value % 1000000U;
}
void bt_smp_h6(const uint8_t* w, const uint8_t* key_id, uint8_t* out) { cmac_le(w, key_id, 4, out); }
void bt_smp_h7(const uint8_t* salt, const uint8_t* w, uint8_t* out) { cmac_le(salt, w, 16, out); }

/* ---------- P-256 ---------- */
typedef unsigned __int128 u128;
typedef struct { uint64_t v[4]; } fe;          /* field element in Montgomery form (or a scalar), little-endian limbs */
static const fe P = {{0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL, 0, 0xFFFFFFFF00000001ULL}};
static const fe N = {{0xF3B9CAC2FC632551ULL, 0xBCE6FAADA7179E84ULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL}};
static const fe B = {{0x3BCE3C3E27D2604BULL, 0x651D06B0CC53B0F6ULL, 0xB3EBBD55769886BCULL, 0x5AC635D8AA3A93E7ULL}};
static const fe GX = {{0xF4A13945D898C296ULL, 0x77037D812DEB33A0ULL, 0xF8BCE6E563A440F2ULL, 0x6B17D1F2E12C4247ULL}};
static const fe GY = {{0xCBB6406837BF51F5ULL, 0x2BCE33576B315ECEULL, 0x8EE7EB4A7C0F9E16ULL, 0x4FE342E2FE1A7F9BULL}};
static fe RR, ONE;                             /* R^2 mod p, R mod p (Montgomery one) */
static bool curve_ready;

static uint64_t sub4(fe* r, const fe* a, const fe* b) {   /* r = a - b, returns borrow */
    uint64_t borrow = 0;
    for (unsigned i = 0; i < 4; i++) { u128 d = (u128)a->v[i] - b->v[i] - borrow; r->v[i] = (uint64_t)d; borrow = (uint64_t)(d >> 64) & 1; }
    return borrow;
}
static uint64_t add4(fe* r, const fe* a, const fe* b) {
    uint64_t carry = 0;
    for (unsigned i = 0; i < 4; i++) { u128 s = (u128)a->v[i] + b->v[i] + carry; r->v[i] = (uint64_t)s; carry = (uint64_t)(s >> 64); }
    return carry;
}
static void select4(fe* r, const fe* a, const fe* b, uint64_t pick_b) { uint64_t m = -pick_b; for (unsigned i = 0; i < 4; i++) r->v[i] = (a->v[i] & ~m) | (b->v[i] & m); }
static bool less4(const fe* a, const fe* b) { fe t; return sub4(&t, a, b) != 0; }
static bool zero4(const fe* a) { return !(a->v[0] | a->v[1] | a->v[2] | a->v[3]); }
static void fadd(fe* r, const fe* a, const fe* b) {   /* mod p, inputs < p */
    fe s, t; uint64_t carry = add4(&s, a, b), borrow = sub4(&t, &s, &P);
    select4(r, &s, &t, carry | (borrow ^ 1));
}
static void fsub(fe* r, const fe* a, const fe* b) { fe d, t; uint64_t borrow = sub4(&d, a, b); add4(&t, &d, &P); select4(r, &d, &t, borrow); }
static void fmul(fe* r, const fe* a, const fe* b) {   /* Montgomery: a*b/R mod p (CIOS, -p^-1 mod 2^64 = 1) */
    uint64_t t[6] = {0};
    for (unsigned i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (unsigned j = 0; j < 4; j++) { u128 x = (u128)a->v[j] * b->v[i] + t[j] + carry; t[j] = (uint64_t)x; carry = (uint64_t)(x >> 64); }
        u128 x = (u128)t[4] + carry; t[4] = (uint64_t)x; t[5] = (uint64_t)(x >> 64);
        uint64_t m = t[0];
        x = (u128)m * P.v[0] + t[0]; carry = (uint64_t)(x >> 64);
        for (unsigned j = 1; j < 4; j++) { x = (u128)m * P.v[j] + t[j] + carry; t[j - 1] = (uint64_t)x; carry = (uint64_t)(x >> 64); }
        x = (u128)t[4] + carry; t[3] = (uint64_t)x; t[4] = t[5] + (uint64_t)(x >> 64);
    }
    fe s = {{t[0], t[1], t[2], t[3]}}, d; uint64_t borrow = sub4(&d, &s, &P);
    select4(r, &s, &d, t[4] | (borrow ^ 1));
}
static void fsqr(fe* r, const fe* a) { fmul(r, a, a); }
static void to_mont(fe* r, const fe* a) { fmul(r, a, &RR); }
static void from_mont(fe* r, const fe* a) { fe one = {{1, 0, 0, 0}}; fmul(r, a, &one); }
static void finv(fe* r, const fe* a) {   /* a^(p-2), fixed square-and-multiply over the public exponent */
    fe e; fe two = {{2, 0, 0, 0}}; sub4(&e, &P, &two);
    fe result = ONE, base = *a;
    for (int bit = 255; bit >= 0; bit--) { fsqr(&result, &result); if ((e.v[bit / 64] >> (bit % 64)) & 1) fmul(&result, &result, &base); }
    *r = result;
}
static void curve_init(void) {
    if (curve_ready) return;
    fe zero = {{0}}; sub4(&ONE, &zero, &P);              /* R mod p = 2^256 - p */
    RR = ONE; for (unsigned i = 0; i < 256; i++) fadd(&RR, &RR, &RR);   /* R * 2^256 = R^2 mod p */
    curve_ready = true;
}
struct jpoint { fe x, y, z; };                          /* Jacobian, Montgomery form; z = 0: infinity */
static void pdouble(struct jpoint* r, const struct jpoint* p) {   /* a = -3 (dbl-2001-b) */
    if (zero4(&p->z)) { *r = *p; return; }
    fe delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    fsqr(&delta, &p->z); fsqr(&gamma, &p->y); fmul(&beta, &p->x, &gamma);
    fsub(&t1, &p->x, &delta); fadd(&t2, &p->x, &delta); fmul(&alpha, &t1, &t2); fadd(&t1, &alpha, &alpha); fadd(&alpha, &alpha, &t1);
    fsqr(&x3, &alpha); fadd(&t1, &beta, &beta); fadd(&t1, &t1, &t1); fadd(&t2, &t1, &t1); fsub(&x3, &x3, &t2);       /* alpha^2 - 8 beta */
    fadd(&z3, &p->y, &p->z); fsqr(&z3, &z3); fsub(&z3, &z3, &gamma); fsub(&z3, &z3, &delta);
    fsub(&t1, &t1, &x3); fmul(&y3, &alpha, &t1); fsqr(&t2, &gamma); fadd(&t2, &t2, &t2); fadd(&t2, &t2, &t2); fadd(&t2, &t2, &t2); fsub(&y3, &y3, &t2);
    r->x = x3; r->y = y3; r->z = z3;
}
static void padd(struct jpoint* r, const struct jpoint* p, const struct jpoint* q) {   /* add-2007-bl */
    if (zero4(&p->z)) { *r = *q; return; }
    if (zero4(&q->z)) { *r = *p; return; }
    fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;
    fsqr(&z1z1, &p->z); fsqr(&z2z2, &q->z); fmul(&u1, &p->x, &z2z2); fmul(&u2, &q->x, &z1z1);
    fmul(&s1, &p->y, &q->z); fmul(&s1, &s1, &z2z2); fmul(&s2, &q->y, &p->z); fmul(&s2, &s2, &z1z1);
    fsub(&h, &u2, &u1); fsub(&rr, &s2, &s1);
    if (zero4(&h)) { if (zero4(&rr)) { pdouble(r, p); return; } memset(r, 0, sizeof(*r)); return; }
    fadd(&i, &h, &h); fsqr(&i, &i); fmul(&j, &h, &i); fadd(&rr, &rr, &rr); fmul(&v, &u1, &i);
    struct jpoint o;
    fsqr(&o.x, &rr); fsub(&o.x, &o.x, &j); fsub(&o.x, &o.x, &v); fsub(&o.x, &o.x, &v);
    fsub(&t, &v, &o.x); fmul(&o.y, &rr, &t); fmul(&t, &s1, &j); fadd(&t, &t, &t); fsub(&o.y, &o.y, &t);
    fadd(&o.z, &p->z, &q->z); fsqr(&o.z, &o.z); fsub(&o.z, &o.z, &z1z1); fsub(&o.z, &o.z, &z2z2); fmul(&o.z, &o.z, &h);
    *r = o;
}
static void cswap(struct jpoint* a, struct jpoint* b, uint64_t bit) {
    uint64_t m = -bit; uint64_t* x = (uint64_t*)a; uint64_t* y = (uint64_t*)b;
    for (unsigned i = 0; i < sizeof(*a) / 8; i++) { uint64_t d = (x[i] ^ y[i]) & m; x[i] ^= d; y[i] ^= d; }
}
static void scalar_mult(struct jpoint* r, const fe* k, const struct jpoint* p) {   /* Montgomery ladder */
    struct jpoint r0, r1 = *p; memset(&r0, 0, sizeof(r0));
    for (int bit = 255; bit >= 0; bit--) {
        uint64_t b = (k->v[bit / 64] >> (bit % 64)) & 1;
        cswap(&r0, &r1, b); padd(&r1, &r0, &r1); pdouble(&r0, &r0); cswap(&r0, &r1, b);
    }
    *r = r0; wipe(&r0, sizeof(r0)); wipe(&r1, sizeof(r1));
}
static void le_to_fe(fe* r, const uint8_t* b) { for (unsigned i = 0; i < 4; i++) { r->v[i] = 0; for (unsigned j = 0; j < 8; j++) r->v[i] |= (uint64_t)b[8 * i + j] << (8 * j); } }
static void fe_to_le(uint8_t* b, const fe* a) { for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 8; j++) b[8 * i + j] = (uint8_t)(a->v[i] >> (8 * j)); }
static bool affine(const struct jpoint* p, uint8_t* x_le, uint8_t* y_le) {
    if (zero4(&p->z)) return false;
    fe zi, zi2, x, y; finv(&zi, &p->z); fsqr(&zi2, &zi); fmul(&x, &p->x, &zi2); fmul(&zi2, &zi2, &zi); fmul(&y, &p->y, &zi2);
    from_mont(&x, &x); from_mont(&y, &y); fe_to_le(x_le, &x); if (y_le) fe_to_le(y_le, &y); return true;
}
/* A public key (x || y, 32 + 32 bytes little-endian): coordinates below p and y^2 = x^3 - 3x + b. */
static bool load_point(struct jpoint* out, const uint8_t* key) {
    fe x, y; le_to_fe(&x, key); le_to_fe(&y, key + 32);
    if (!less4(&x, &P) || !less4(&y, &P)) return false;
    fe mx, my, lhs, rhs, t, b; to_mont(&mx, &x); to_mont(&my, &y); to_mont(&b, &B);
    fsqr(&lhs, &my); fsqr(&rhs, &mx); fmul(&rhs, &rhs, &mx); fadd(&t, &mx, &mx); fadd(&t, &t, &mx); fsub(&rhs, &rhs, &t); fadd(&rhs, &rhs, &b);
    fsub(&t, &lhs, &rhs); if (!zero4(&t)) return false;
    out->x = mx; out->y = my; out->z = ONE; return true;
}
bool bt_p256_valid(const uint8_t* key) { curve_init(); struct jpoint p; return load_point(&p, key); }
/* A key pair: private key uniform in [1, n-1] (rejection sampling), public = d*G. */
bool bt_p256_keypair(uint8_t* private_le, uint8_t* public_le) {
    curve_init(); fe d; fe one = {{1, 0, 0, 0}}, n1; sub4(&n1, &N, &one);
    for (unsigned tries = 0;; tries++) {
        if (tries > 64 || !native_random(private_le, 32)) return false;
        le_to_fe(&d, private_le);
        if (!zero4(&d) && less4(&d, &n1)) break;          /* d in [1, n-2]: d < n - 1 (one value off is immaterial) */
    }
    return bt_p256_public(private_le, public_le);
}
bool bt_p256_public(const uint8_t* private_le, uint8_t* public_le) {
    curve_init(); fe d; le_to_fe(&d, private_le);
    if (zero4(&d) || !less4(&d, &N)) return false;
    struct jpoint g = {.z = ONE}, q; to_mont(&g.x, &GX); to_mont(&g.y, &GY);
    scalar_mult(&q, &d, &g); wipe(&d, sizeof(d));
    return affine(&q, public_le, public_le + 32);
}
/* DHKey = x(d * peer) little-endian; false for an invalid peer key (the caller fails the pairing). */
bool bt_p256_dhkey(const uint8_t* private_le, const uint8_t* peer_public_le, uint8_t* dhkey_le) {
    curve_init(); struct jpoint q, r; fe d;
    if (!load_point(&q, peer_public_le)) return false;
    le_to_fe(&d, private_le); if (zero4(&d) || !less4(&d, &N)) return false;
    scalar_mult(&r, &d, &q); wipe(&d, sizeof(d));
    return affine(&r, dhkey_le, NULL);
}

/* ---------- known answers ---------- */
static bool same(const uint8_t* a, const char* hex_msb_first, bool little_endian) {
    unsigned n = (unsigned)strlen(hex_msb_first) / 2;
    for (unsigned i = 0; i < n; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) { char c = hex_msb_first[2 * i + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }
        if (a[little_endian ? n - 1 - i : i] != v) return false;
    }
    return true;
}
static void parse(uint8_t* out, const char* hex_msb_first, bool little_endian) {
    unsigned n = (unsigned)strlen(hex_msb_first) / 2;
    for (unsigned i = 0; i < n; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) { char c = hex_msb_first[2 * i + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }
        out[little_endian ? n - 1 - i : i] = (uint8_t)v;
    }
}
unsigned bt_crypto_selftest(void) {
    unsigned failed = 0; uint8_t key[32], b[80], t[16], u[32], v[32], x[16], y[16], w[32];
    /* FIPS-197 Appendix B and C.1/C.3 */
    parse(key, "2b7e151628aed2a6abf7158809cf4f3c", false); parse(b, "3243f6a8885a308d313198a2e0370734", false);
    struct bt_aes a; bt_aes_key(&a, key, 16); bt_aes_encrypt(&a, b); failed += !same(b, "3925841d02dc09fbdc118597196a0b32", false);
    bt_aes_decrypt(&a, b); failed += !same(b, "3243f6a8885a308d313198a2e0370734", false);
    for (unsigned i = 0; i < 32; i++) key[i] = (uint8_t)i;
    for (unsigned i = 0; i < 16; i++) b[i] = (uint8_t)(i * 0x11);
    bt_aes_key(&a, key, 32); bt_aes_encrypt(&a, b); failed += !same(b, "8ea2b7ca516745bfeafc49904b496089", false);
    bt_aes_decrypt(&a, b); failed += !same(b, "00112233445566778899aabbccddeeff", false);
    /* RFC 4493 section 4 */
    parse(key, "2b7e151628aed2a6abf7158809cf4f3c", false);
    parse(b, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", false);
    bt_cmac(key, 16, b, 0, t); failed += !same(t, "bb1d6929e95937287fa37d129b756746", false);
    bt_cmac(key, 16, b, 16, t); failed += !same(t, "070a16b46b4d4144f79bdd9dd04a287c", false);
    bt_cmac(key, 16, b, 40, t); failed += !same(t, "dfa66747de9ae63030ca32611497c827", false);
    bt_cmac(key, 16, b, 64, t); failed += !same(t, "51f0bebf7e3b9d92fc49741779363cfe", false);
    /* Core spec Vol 3 Part H 2.2.3 c1, 2.2.4 s1, 2.2.2 ah (sample data, MSB first) */
    uint8_t zero[16] = {0}, r[16], preq[7], pres[7], ia[6], ra[6];
    parse(r, "5783d52156ad6f0e6388274ec6702ee0", true); parse(preq, "07071000000101", true); parse(pres, "05000800000302", true);
    parse(ia, "a1a2a3a4a5a6", true); parse(ra, "b1b2b3b4b5b6", true);
    bt_smp_c1(zero, r, preq, pres, 1, ia, 0, ra, t); failed += !same(t, "1e1e3fef878988ead2a74dc5bef13b86", true);
    uint8_t r1[16], r2[16]; parse(r1, "000f0e0d0c0b0a091122334455667788", true); parse(r2, "010203040506070899aabbccddeeff00", true);
    bt_smp_s1(zero, r1, r2, t); failed += !same(t, "9a1fe1f0e8b0f49b5b4216ae796da062", true);
    uint8_t irk[16], prand[3], hash[3]; parse(irk, "ec0234a357c8ad05341010a60a397d9b", true); parse(prand, "708194", true);
    bt_smp_ah(irk, prand, hash); failed += !same(hash, "0dfbaa", true);
    /* Appendix D: f4, f5, f6, g2, h6 */
    parse(u, "20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6", true);
    parse(v, "55188b3d32f6bb9a900afcfbeed4e72a59cb9ac2f19d7cfb6b4fdd49f47fc5fd", true);
    parse(x, "d5cb8454d177733effffb2ec712baeab", true);
    bt_smp_f4(u, v, x, 0, t); failed += !same(t, "f2c916f107a9bd1cf1eda1bea974872d", true);
    parse(w, "ec0234a357c8ad05341010a60a397d9b99796b13b4f866f1868d34f373bfa698", true);
    uint8_t n2[16], a1[7], a2[7], mackey[16], ltk[16]; parse(n2, "a6e8e7cc25a75f6e216583f7ff3dc4cf", true);
    parse(a1, "0056123737bfce", true); parse(a2, "00a713702dcfc1", true);
    bt_smp_f5(w, x, n2, a1, a2, mackey, ltk);
    failed += !same(mackey, "2965f176a1084a02fd3f6a20ce636e20", true); failed += !same(ltk, "6986791169d7cd23980522b594750a38", true);
    uint8_t rr[16], io[3]; parse(rr, "12a3343bb453bb5408da42d20c2d0fc8", true); parse(io, "010102", true);
    bt_smp_f6(mackey, x, n2, rr, io, a1, a2, t); failed += !same(t, "e3c473989cd0e8c5d26c0b09da958f61", true);
    parse(y, "a6e8e7cc25a75f6e216583f7ff3dc4cf", true);
    failed += bt_smp_g2(u, v, x, y) != 0x2f9ed5baU % 1000000U;
    uint8_t keyid[4]; parse(keyid, "6c656272", true); parse(w, "ec0234a357c8ad05341010a60a397d9b", true);
    bt_smp_h6(w, keyid, t); failed += !same(t, "2d9ae102e76dc91ce8d3a9e280b16399", true);
    /* SMP debug key pair (Vol 3 Part H 2.3.5.6.1): private -> public; the public key is on the curve */
    uint8_t sk[32], pk[64], expect[64];
    parse(sk, "3f49f6d4a3c55f3874c9b3e3d2103f504aff607beb40b7995899b8a6cd3c1abd", true);
    parse(expect, "20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6", true);
    parse(expect + 32, "dc809c49652aeb6d63329abf5a52155c766345c28fed3024741c8ed01589d28b", true);
    failed += !bt_p256_public(sk, pk) || memcmp(pk, expect, 64) || !bt_p256_valid(expect);
    expect[40] ^= 1; failed += bt_p256_valid(expect); expect[40] ^= 1;                 /* off the curve */
    memset(b, 0, 64); failed += bt_p256_valid(b);                                       /* (0, 0) */
    /* ECDH agreement: fresh keys when the CPU has a random source (RDRAND), else two fixed scalars */
    uint8_t sa[32], pa[64], sb[32], pb[64], k1[32], k2[32];
    if (native_random_available()) failed += !bt_p256_keypair(sa, pa) || !bt_p256_keypair(sb, pb);
    else { memcpy(sa, sk, 32); sa[0] ^= 0x5A; memcpy(sb, sk, 32); sb[1] ^= 0xA5; failed += !bt_p256_public(sa, pa) || !bt_p256_public(sb, pb); }
    failed += !bt_p256_dhkey(sa, pb, k1) || !bt_p256_dhkey(sb, pa, k2) || memcmp(k1, k2, 32);
    failed += bt_p256_dhkey(sa, expect + 0, k1) != true;                                /* the debug key is a valid peer */
    expect[63] ^= 0x80; failed += bt_p256_dhkey(sa, expect, k1); expect[63] ^= 0x80;    /* invalid peer key refused */
    wipe(sa, 32); wipe(sb, 32); wipe(sk, 32); wipe(k1, 32); wipe(k2, 32); bt_aes_wipe(&a);
    return failed;
}
