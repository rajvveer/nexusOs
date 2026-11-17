/* ============================================================================
 * NexusOS — Big integers + public-key verification (Phase 57)
 * ============================================================================ */
#include "bignum.h"
#include "crypto.h"
#include "string.h"

/* ============================================================================
 * Basic multi-limb helpers (little-endian limb order)
 * ============================================================================ */
int bn_from_bytes(uint32_t* r, int n, const uint8_t* be, uint32_t len) {
    memset(r, 0, (size_t)n * 4);
    /* strip leading zeros */
    while (len > 0 && be[0] == 0) { be++; len--; }
    if (len > (uint32_t)n * 4) return -1;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte_index = len - 1 - i;      /* 0 = least significant */
        r[byte_index / 4] |= (uint32_t)be[i] << (8 * (byte_index % 4));
    }
    return 0;
}

void bn_to_bytes(const uint32_t* a, int n, uint8_t* out, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte_index = len - 1 - i;
        uint32_t limb = byte_index / 4;
        out[i] = (limb < (uint32_t)n) ? (uint8_t)(a[limb] >> (8 * (byte_index % 4))) : 0;
    }
}

int bn_cmp(const uint32_t* a, const uint32_t* b, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

bool bn_is_zero(const uint32_t* a, int n) {
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) acc |= a[i];
    return acc == 0;
}

static uint32_t bn_add(uint32_t* r, const uint32_t* a, const uint32_t* b, int n) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

static uint32_t bn_sub(uint32_t* r, const uint32_t* a, const uint32_t* b, int n) {
    uint32_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint32_t ai = a[i], bi = b[i];
        uint32_t d = ai - bi;
        uint32_t nb = (ai < bi);
        uint32_t d2 = d - borrow;
        nb |= (d < borrow);
        r[i] = d2;
        borrow = nb;
    }
    return borrow;
}

/* r = a + b mod m (a, b < m) */
static void bn_mod_add(uint32_t* r, const uint32_t* a, const uint32_t* b, const uint32_t* m, int n) {
    uint32_t t[BN_MAX_LIMBS];
    uint32_t carry = bn_add(t, a, b, n);
    if (carry || bn_cmp(t, m, n) >= 0) bn_sub(t, t, m, n);
    memcpy(r, t, (size_t)n * 4);
}

/* r = a - b mod m (a, b < m) */
static void bn_mod_sub(uint32_t* r, const uint32_t* a, const uint32_t* b, const uint32_t* m, int n) {
    uint32_t t[BN_MAX_LIMBS];
    uint32_t borrow = bn_sub(t, a, b, n);
    if (borrow) bn_add(t, t, m, n);
    memcpy(r, t, (size_t)n * 4);
}

/* r = 2a mod m */
static void bn_mod_dbl(uint32_t* r, const uint32_t* a, const uint32_t* m, int n) {
    bn_mod_add(r, a, a, m, n);
}

static int bn_bits(const uint32_t* a, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i]) {
            int b = 0; uint32_t v = a[i];
            while (v) { b++; v >>= 1; }
            return i * 32 + b;
        }
    }
    return 0;
}

/* ============================================================================
 * Montgomery arithmetic (CIOS)
 * ============================================================================ */
int mont_init(mont_ctx_t* c, const uint8_t* mod_be, uint32_t len) {
    while (len > 0 && mod_be[0] == 0) { mod_be++; len--; }
    if (len == 0 || len > BN_MAX_LIMBS * 4) return -1;
    int n = (int)((len + 3) / 4);
    c->n = n;
    if (bn_from_bytes(c->m, n, mod_be, len) != 0) return -1;
    if (!(c->m[0] & 1)) return -1;                   /* must be odd */

    /* n0 = -m^-1 mod 2^32 via Newton iteration */
    uint32_t m0 = c->m[0], inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - m0 * inv;  /* inv = m0^-1 mod 2^32 */
    c->n0 = (uint32_t)(0u - inv);

    /* rr = R^2 mod m: start from 1, double 2*32n times with reduction */
    memset(c->rr, 0, (size_t)n * 4);
    c->rr[0] = 1;
    /* fast forward: shift to just below m's bit length, then double bit by bit */
    int mb = bn_bits(c->m, n);
    /* rr = 2^(mb-1) < m */
    memset(c->rr, 0, (size_t)n * 4);
    c->rr[(mb - 1) / 32] = 1u << ((mb - 1) % 32);
    int total = 2 * 32 * n - (mb - 1);
    for (int i = 0; i < total; i++) bn_mod_dbl(c->rr, c->rr, c->m, n);
    return 0;
}

/* r = a*b*R^-1 mod m. a, b < R (ideally < m); r may alias a or b. */
void mont_mul(uint32_t* r, const uint32_t* a, const uint32_t* b, const mont_ctx_t* c) {
    int n = c->n;
    uint32_t t[BN_MAX_LIMBS + 2];
    memset(t, 0, (size_t)(n + 2) * 4);
    for (int i = 0; i < n; i++) {
        uint64_t carry = 0;
        uint32_t bi = b[i];
        for (int j = 0; j < n; j++) {
            carry += (uint64_t)t[j] + (uint64_t)a[j] * bi;
            t[j] = (uint32_t)carry;
            carry >>= 32;
        }
        carry += t[n];
        t[n] = (uint32_t)carry;
        t[n + 1] = (uint32_t)(carry >> 32);

        uint32_t mm = t[0] * c->n0;
        carry = (uint64_t)t[0] + (uint64_t)mm * c->m[0];
        carry >>= 32;
        for (int j = 1; j < n; j++) {
            carry += (uint64_t)t[j] + (uint64_t)mm * c->m[j];
            t[j - 1] = (uint32_t)carry;
            carry >>= 32;
        }
        carry += t[n];
        t[n - 1] = (uint32_t)carry;
        t[n] = t[n + 1] + (uint32_t)(carry >> 32);
    }
    if (t[n] || bn_cmp(t, c->m, n) >= 0) bn_sub(t, t, c->m, n);
    memcpy(r, t, (size_t)n * 4);
}

static void mont_to(uint32_t* r, const uint32_t* a, const mont_ctx_t* c) {
    mont_mul(r, a, c->rr, c);
}

static void mont_from(uint32_t* r, const uint32_t* a, const mont_ctx_t* c) {
    uint32_t one[BN_MAX_LIMBS];
    memset(one, 0, (size_t)c->n * 4);
    one[0] = 1;
    mont_mul(r, a, one, c);
}

/* Montgomery form of 1 (= R mod m) */
static void mont_one(uint32_t* r, const mont_ctx_t* c) {
    uint32_t one[BN_MAX_LIMBS];
    memset(one, 0, (size_t)c->n * 4);
    one[0] = 1;
    mont_to(r, one, c);
}

/* r = base^exp mod m; base/result in normal (non-Montgomery) form */
void mont_exp(uint32_t* r, const uint32_t* base, const uint32_t* exp, int exp_limbs, const mont_ctx_t* c) {
    uint32_t acc[BN_MAX_LIMBS], b[BN_MAX_LIMBS];
    mont_to(b, base, c);
    mont_one(acc, c);
    int bits = bn_bits(exp, exp_limbs);
    for (int i = bits - 1; i >= 0; i--) {
        mont_mul(acc, acc, acc, c);
        if ((exp[i / 32] >> (i % 32)) & 1) mont_mul(acc, acc, b, c);
    }
    mont_from(r, acc, c);
}

/* Montgomery-form exponentiation: r_m = a_m ^ exp (a_m, r_m in Montgomery form) */
static void mont_exp_m(uint32_t* r, const uint32_t* a_m, const uint32_t* exp, int exp_limbs, const mont_ctx_t* c) {
    uint32_t acc[BN_MAX_LIMBS];
    mont_one(acc, c);
    int bits = bn_bits(exp, exp_limbs);
    for (int i = bits - 1; i >= 0; i--) {
        mont_mul(acc, acc, acc, c);
        if ((exp[i / 32] >> (i % 32)) & 1) mont_mul(acc, acc, a_m, c);
    }
    memcpy(r, acc, (size_t)c->n * 4);
}

/* a_m^-1 (Montgomery form) for prime m via Fermat: a^(m-2) */
static void mont_inv_m(uint32_t* r, const uint32_t* a_m, const mont_ctx_t* c) {
    uint32_t e[BN_MAX_LIMBS], two[BN_MAX_LIMBS];
    memset(two, 0, (size_t)c->n * 4); two[0] = 2;
    bn_sub(e, c->m, two, c->n);
    mont_exp_m(r, a_m, e, c->n, c);
}

/* ============================================================================
 * RSA
 * ============================================================================ */
static const uint8_t DI_SHA256[] = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
static const uint8_t DI_SHA384[] = {0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30};
static const uint8_t DI_SHA512[] = {0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40};

static void mgf1(int hash_id, const uint8_t* seed, uint32_t slen, uint8_t* mask, uint32_t mlen) {
    uint8_t ctr[4], dig[64];
    uint32_t hl = (uint32_t)hash_len(hash_id);
    uint32_t done = 0, i = 0;
    while (done < mlen) {
        ctr[0] = (uint8_t)(i >> 24); ctr[1] = (uint8_t)(i >> 16); ctr[2] = (uint8_t)(i >> 8); ctr[3] = (uint8_t)i;
        hash_compute2(hash_id, seed, slen, ctr, 4, dig);
        uint32_t take = mlen - done < hl ? mlen - done : hl;
        memcpy(mask + done, dig, take);
        done += take; i++;
    }
}

int rsa_verify(const uint8_t* n, uint32_t nlen, const uint8_t* e, uint32_t elen,
               const uint8_t* sig, uint32_t siglen, int hash_id, const uint8_t* hash, bool pss) {
    static mont_ctx_t ctx;                 /* ~1 KB: keep off the stack */
    uint32_t s[BN_MAX_LIMBS], ex[BN_MAX_LIMBS], em_l[BN_MAX_LIMBS];
    uint8_t em[BN_MAX_LIMBS * 4];
    uint32_t hl = (uint32_t)hash_len(hash_id);
    if (hl == 0) return -2;

    while (nlen > 0 && n[0] == 0) { n++; nlen--; }
    if (nlen < 64 || nlen > BN_MAX_LIMBS * 4) return -2;   /* 512..4096 bit */
    if (mont_init(&ctx, n, nlen) != 0) return -2;
    int limbs = ctx.n;
    if (bn_from_bytes(s, limbs, sig, siglen) != 0) return -1;
    if (bn_cmp(s, ctx.m, limbs) >= 0) return -1;
    if (bn_from_bytes(ex, limbs, e, elen) != 0) return -2;
    if (bn_is_zero(ex, limbs)) return -2;

    mont_exp(em_l, s, ex, limbs, &ctx);
    uint32_t k = nlen;                     /* octet length of the modulus */
    bn_to_bytes(em_l, limbs, em, k);

    if (!pss) {
        const uint8_t* di; uint32_t dilen;
        if (hash_id == HASH_SHA256)      { di = DI_SHA256; dilen = sizeof(DI_SHA256); }
        else if (hash_id == HASH_SHA384) { di = DI_SHA384; dilen = sizeof(DI_SHA384); }
        else                             { di = DI_SHA512; dilen = sizeof(DI_SHA512); }
        uint32_t tlen = dilen + hl;
        if (k < tlen + 11) return -1;
        if (em[0] != 0x00 || em[1] != 0x01) return -1;
        uint32_t i = 2;
        while (i < k && em[i] == 0xff) i++;
        if (i < 10 || i >= k || em[i] != 0x00) return -1;
        i++;
        if (k - i != tlen) return -1;
        if (memcmp(em + i, di, dilen) != 0) return -1;
        return crypto_memeq(em + i + dilen, hash, hl);
    }

    /* RSASSA-PSS verify (RFC 8017 9.1.2), emBits = modBits - 1 */
    int modbits = bn_bits(ctx.m, limbs);
    uint32_t embits = (uint32_t)modbits - 1;
    uint32_t emlen = (embits + 7) / 8;
    const uint8_t* EM = em;
    if (emlen < k) {
        if (em[0] != 0) return -1;
        EM = em + (k - emlen);
    }
    if (emlen < hl + hl + 2) return -1;
    if (EM[emlen - 1] != 0xbc) return -1;
    uint32_t dblen = emlen - hl - 1;
    const uint8_t* maskedDB = EM;
    const uint8_t* H = EM + dblen;
    uint32_t topbits = 8 * emlen - embits;              /* 0..7 */
    if (topbits && (maskedDB[0] >> (8 - topbits)) != 0) return -1;
    uint8_t db[BN_MAX_LIMBS * 4];
    mgf1(hash_id, H, hl, db, dblen);
    for (uint32_t i = 0; i < dblen; i++) db[i] ^= maskedDB[i];
    if (topbits) db[0] &= (uint8_t)(0xff >> topbits);
    /* DB = PS (zeros) || 0x01 || salt */
    uint32_t i = 0;
    while (i < dblen && db[i] == 0) i++;
    if (i >= dblen || db[i] != 0x01) return -1;
    i++;
    uint32_t slen = dblen - i;
    if (slen != hl) return -1;                          /* TLS 1.3 / X.509: sLen == hLen */
    /* H' = Hash(0x00*8 || mHash || salt) */
    uint8_t mp[8 + 64 + 64];
    memset(mp, 0, 8);
    memcpy(mp + 8, hash, hl);
    memcpy(mp + 8 + hl, db + i, slen);
    uint8_t h2[64];
    hash_compute(hash_id, mp, 8 + hl + slen, h2);
    return crypto_memeq(H, h2, hl);
}

/* ============================================================================
 * Elliptic curves: NIST P-256 / P-384 (a = -3), Jacobian coordinates
 * ============================================================================ */
typedef struct {
    int n;                      /* limbs: 8 or 12 */
    const uint8_t* p;  const uint8_t* b;  const uint8_t* gx; const uint8_t* gy; const uint8_t* order;
    int bytes;                  /* 32 / 48 */
} ec_curve_def_t;

static const uint8_t P256_P[32]  = {0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t P256_B[32]  = {0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};
static const uint8_t P256_GX[32] = {0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
static const uint8_t P256_GY[32] = {0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
static const uint8_t P256_N[32]  = {0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};

static const uint8_t P384_P[48]  = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff};
static const uint8_t P384_B[48]  = {0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef};
static const uint8_t P384_GX[48] = {0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7};
static const uint8_t P384_GY[48] = {0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f};
static const uint8_t P384_N[48]  = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73};

static const ec_curve_def_t CURVES[2] = {
    { 8,  P256_P, P256_B, P256_GX, P256_GY, P256_N, 32 },
    { 12, P384_P, P384_B, P384_GX, P384_GY, P384_N, 48 },
};

#define EC_MAX 12

typedef struct {
    bool ready;
    mont_ctx_t fp;          /* field */
    mont_ctx_t fn;          /* order */
    uint32_t three_m[EC_MAX]; /* 3 in Montgomery form (for a = -3 => we use subtraction) */
    uint32_t b_m[EC_MAX];
    uint32_t gx_m[EC_MAX], gy_m[EC_MAX];
} ec_ctx_t;

static ec_ctx_t ec_cache[2];

typedef struct { uint32_t x[EC_MAX], y[EC_MAX], z[EC_MAX]; } ec_point_t;  /* Jacobian, Montgomery form */

static ec_ctx_t* ec_get(int curve) {
    int idx = (curve == EC_P256) ? 0 : (curve == EC_P384) ? 1 : -1;
    if (idx < 0) return NULL;
    ec_ctx_t* c = &ec_cache[idx];
    if (!c->ready) {
        const ec_curve_def_t* d = &CURVES[idx];
        if (mont_init(&c->fp, d->p, (uint32_t)d->bytes) != 0) return NULL;
        if (mont_init(&c->fn, d->order, (uint32_t)d->bytes) != 0) return NULL;
        uint32_t t[EC_MAX];
        bn_from_bytes(t, d->n, d->b, (uint32_t)d->bytes);  mont_to(c->b_m, t, &c->fp);
        bn_from_bytes(t, d->n, d->gx, (uint32_t)d->bytes); mont_to(c->gx_m, t, &c->fp);
        bn_from_bytes(t, d->n, d->gy, (uint32_t)d->bytes); mont_to(c->gy_m, t, &c->fp);
        c->ready = true;
    }
    return c;
}

int ec_point_len(int curve) { return curve == EC_P256 ? 65 : curve == EC_P384 ? 97 : 0; }

#define FMUL(r, a, b) mont_mul((r), (a), (b), fp)
#define FSQR(r, a)    mont_mul((r), (a), (a), fp)
#define FADD(r, a, b) bn_mod_add((r), (a), (b), fp->m, n)
#define FSUB(r, a, b) bn_mod_sub((r), (a), (b), fp->m, n)
#define FDBL(r, a)    bn_mod_dbl((r), (a), fp->m, n)

static bool ec_is_inf(const ec_point_t* P, int n) { return bn_is_zero(P->z, n); }

/* dbl-2001-b (a = -3) */
static void ec_double(ec_point_t* R, const ec_point_t* P, const mont_ctx_t* fp) {
    int n = fp->n;
    uint32_t delta[EC_MAX], gamma[EC_MAX], beta[EC_MAX], alpha[EC_MAX], t1[EC_MAX], t2[EC_MAX];
    if (ec_is_inf(P, n)) { *R = *P; return; }
    FSQR(delta, P->z);
    FSQR(gamma, P->y);
    FMUL(beta, P->x, gamma);
    FSUB(t1, P->x, delta);
    FADD(t2, P->x, delta);
    FMUL(alpha, t1, t2);              /* (X-d)(X+d) */
    FDBL(t1, alpha); FADD(alpha, alpha, t1);   /* 3(...) */
    /* X3 = alpha^2 - 8 beta */
    FSQR(t1, alpha);
    FDBL(t2, beta); FDBL(t2, t2); FDBL(t2, t2);  /* 8 beta */
    uint32_t x3[EC_MAX], y3[EC_MAX], z3[EC_MAX];
    FSUB(x3, t1, t2);
    /* Z3 = (Y+Z)^2 - gamma - delta */
    FADD(t1, P->y, P->z); FSQR(t1, t1); FSUB(t1, t1, gamma); FSUB(z3, t1, delta);
    /* Y3 = alpha (4 beta - X3) - 8 gamma^2 */
    FDBL(t1, beta); FDBL(t1, t1); FSUB(t1, t1, x3); FMUL(t1, alpha, t1);
    FSQR(t2, gamma); FDBL(t2, t2); FDBL(t2, t2); FDBL(t2, t2);
    FSUB(y3, t1, t2);
    memcpy(R->x, x3, (size_t)n * 4); memcpy(R->y, y3, (size_t)n * 4); memcpy(R->z, z3, (size_t)n * 4);
}

/* add-2007-bl (general Jacobian addition) */
static void ec_add(ec_point_t* R, const ec_point_t* P, const ec_point_t* Q, const mont_ctx_t* fp) {
    int n = fp->n;
    if (ec_is_inf(P, n)) { *R = *Q; return; }
    if (ec_is_inf(Q, n)) { *R = *P; return; }
    uint32_t z1z1[EC_MAX], z2z2[EC_MAX], u1[EC_MAX], u2[EC_MAX], s1[EC_MAX], s2[EC_MAX];
    uint32_t h[EC_MAX], i[EC_MAX], j[EC_MAX], r[EC_MAX], v[EC_MAX], t[EC_MAX];
    FSQR(z1z1, P->z); FSQR(z2z2, Q->z);
    FMUL(u1, P->x, z2z2); FMUL(u2, Q->x, z1z1);
    FMUL(s1, P->y, Q->z); FMUL(s1, s1, z2z2);
    FMUL(s2, Q->y, P->z); FMUL(s2, s2, z1z1);
    FSUB(h, u2, u1);
    FSUB(r, s2, s1);
    if (bn_is_zero(h, n)) {
        if (bn_is_zero(r, n)) { ec_double(R, P, fp); return; }
        memset(R, 0, sizeof(*R));     /* point at infinity */
        return;
    }
    FDBL(r, r);                       /* r = 2(S2-S1) */
    FDBL(i, h); FSQR(i, i);           /* I = (2H)^2 */
    FMUL(j, h, i);                    /* J = H*I */
    FMUL(v, u1, i);                   /* V = U1*I */
    uint32_t x3[EC_MAX], y3[EC_MAX], z3[EC_MAX];
    FSQR(t, r); FSUB(t, t, j); FDBL(u2, v); FSUB(x3, t, u2);     /* X3 = r^2 - J - 2V */
    FSUB(t, v, x3); FMUL(t, r, t); FMUL(u2, s1, j); FDBL(u2, u2); FSUB(y3, t, u2); /* Y3 */
    FADD(t, P->z, Q->z); FSQR(t, t); FSUB(t, t, z1z1); FSUB(t, t, z2z2); FMUL(z3, t, h); /* Z3 */
    memcpy(R->x, x3, (size_t)n * 4); memcpy(R->y, y3, (size_t)n * 4); memcpy(R->z, z3, (size_t)n * 4);
}

/* R = k * P (k normal form, k < order), double-and-add MSB first */
static void ec_mul(ec_point_t* R, const uint32_t* k, const ec_point_t* P, const mont_ctx_t* fp) {
    int n = fp->n;
    ec_point_t acc;
    memset(&acc, 0, sizeof(acc));
    int bits = bn_bits(k, n);
    for (int i = bits - 1; i >= 0; i--) {
        ec_double(&acc, &acc, fp);
        if ((k[i / 32] >> (i % 32)) & 1) ec_add(&acc, &acc, P, fp);
    }
    *R = acc;
}

/* affine point -> Jacobian Montgomery; checks it lies on the curve */
static int ec_load_point(ec_point_t* P, const ec_ctx_t* ec, const ec_curve_def_t* d, const uint8_t* point, uint32_t plen) {
    const mont_ctx_t* fp = &ec->fp;
    int n = fp->n;
    if (plen != (uint32_t)(1 + 2 * d->bytes) || point[0] != 0x04) return -1;
    uint32_t x[EC_MAX], y[EC_MAX];
    if (bn_from_bytes(x, n, point + 1, (uint32_t)d->bytes) != 0) return -1;
    if (bn_from_bytes(y, n, point + 1 + d->bytes, (uint32_t)d->bytes) != 0) return -1;
    if (bn_cmp(x, fp->m, n) >= 0 || bn_cmp(y, fp->m, n) >= 0) return -1;
    mont_to(P->x, x, fp); mont_to(P->y, y, fp);
    memset(P->z, 0, (size_t)n * 4); mont_one(P->z, fp);
    /* on-curve: y^2 == x^3 - 3x + b */
    uint32_t l[EC_MAX], r[EC_MAX], t[EC_MAX];
    FSQR(l, P->y);
    FSQR(t, P->x); FMUL(t, t, P->x);          /* x^3 */
    FDBL(r, P->x); FADD(r, r, P->x);          /* 3x */
    FSUB(t, t, r); FADD(r, t, ec->b_m);
    return bn_cmp(l, r, n) == 0 ? 0 : -1;
}

int ecdsa_verify(int curve, const uint8_t* point, uint32_t plen,
                 const uint8_t* hash, uint32_t hlen,
                 const uint8_t* r, uint32_t rlen, const uint8_t* s, uint32_t slen) {
    int idx = (curve == EC_P256) ? 0 : (curve == EC_P384) ? 1 : -1;
    if (idx < 0) return -2;
    const ec_curve_def_t* d = &CURVES[idx];
    ec_ctx_t* ec = ec_get(curve);
    if (!ec) return -2;
    const mont_ctx_t* fp = &ec->fp;
    const mont_ctx_t* fn = &ec->fn;
    int n = fp->n;

    ec_point_t Q, G, R1, R2, R;
    if (ec_load_point(&Q, ec, d, point, plen) != 0) return -1;
    memcpy(G.x, ec->gx_m, (size_t)n * 4); memcpy(G.y, ec->gy_m, (size_t)n * 4);
    memset(G.z, 0, (size_t)n * 4); mont_one(G.z, fp);

    uint32_t rr[EC_MAX], ss[EC_MAX], e[EC_MAX], w[EC_MAX], u1[EC_MAX], u2[EC_MAX], t[EC_MAX];
    if (bn_from_bytes(rr, n, r, rlen) != 0 || bn_from_bytes(ss, n, s, slen) != 0) return -1;
    if (bn_is_zero(rr, n) || bn_is_zero(ss, n)) return -1;
    if (bn_cmp(rr, fn->m, n) >= 0 || bn_cmp(ss, fn->m, n) >= 0) return -1;

    /* e = leftmost order-bits of the hash */
    uint32_t take = hlen < (uint32_t)d->bytes ? hlen : (uint32_t)d->bytes;
    bn_from_bytes(e, n, hash, take);
    /* order bit length is a multiple of 8 for both curves: no bit shift needed.
     * e may be >= order; mont_to handles inputs < R. */

    /* w = s^-1 mod n; u1 = e*w; u2 = r*w  (all mod order) */
    mont_to(t, ss, fn); mont_inv_m(w, t, fn);          /* w in Montgomery form */
    mont_to(t, e, fn);  mont_mul(u1, t, w, fn); mont_from(u1, u1, fn);
    mont_to(t, rr, fn); mont_mul(u2, t, w, fn); mont_from(u2, u2, fn);

    ec_mul(&R1, u1, &G, fp);
    ec_mul(&R2, u2, &Q, fp);
    ec_add(&R, &R1, &R2, fp);
    if (ec_is_inf(&R, n)) return -1;

    /* x = X / Z^2 */
    uint32_t zi[EC_MAX], x[EC_MAX];
    mont_inv_m(zi, R.z, fp);
    FSQR(zi, zi);
    FMUL(x, R.x, zi);
    mont_from(x, x, fp);
    /* v = x mod order (x < p < 2*order) */
    if (bn_cmp(x, fn->m, n) >= 0) bn_sub(x, x, fn->m, n);
    return bn_cmp(x, rr, n) == 0 ? 0 : -1;
}

/* ============================================================================
 * X25519 (RFC 7748) over p = 2^255 - 19 using the Montgomery field engine
 * ============================================================================ */
static const uint8_t P25519[32] = {
    0x7f,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xed
};
static mont_ctx_t f25519;
static bool f25519_ready = false;
static uint32_t a24_m[8];

static void cswap(uint32_t swap, uint32_t* a, uint32_t* b, int n) {
    uint32_t mask = 0u - swap;
    for (int i = 0; i < n; i++) {
        uint32_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t; b[i] ^= t;
    }
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    if (!f25519_ready) {
        mont_init(&f25519, P25519, 32);
        uint32_t t[8]; memset(t, 0, 32); t[0] = 121665;
        mont_to(a24_m, t, &f25519);
        f25519_ready = true;
    }
    const mont_ctx_t* fp = &f25519;
    const int n = 8;
    uint8_t k[32], u[32];
    memcpy(k, scalar, 32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;
    /* little-endian u -> big-endian bytes for bn_from_bytes */
    for (int i = 0; i < 32; i++) u[i] = point[31 - i];
    u[0] &= 0x7f;

    uint32_t x1[8], x2[8], z2[8], x3[8], z3[8];
    uint32_t A[8], AA[8], B[8], BB[8], E[8], C[8], D[8], DA[8], CB[8], t[8];
    bn_from_bytes(t, n, u, 32);
    mont_to(x1, t, fp);                      /* x1 may be >= p before conversion: fine */
    mont_one(x2, fp); memset(z2, 0, 32);
    memcpy(x3, x1, 32); mont_one(z3, fp);
    uint32_t swap = 0;
    for (int i = 254; i >= 0; i--) {
        uint32_t kt = (k[i / 8] >> (i % 8)) & 1;
        swap ^= kt;
        cswap(swap, x2, x3, n); cswap(swap, z2, z3, n);
        swap = kt;
        FADD(A, x2, z2);  FSQR(AA, A);
        FSUB(B, x2, z2);  FSQR(BB, B);
        FSUB(E, AA, BB);
        FADD(C, x3, z3);  FSUB(D, x3, z3);
        FMUL(DA, D, A);   FMUL(CB, C, B);
        FADD(t, DA, CB);  FSQR(x3, t);
        FSUB(t, DA, CB);  FSQR(t, t); FMUL(z3, x1, t);
        FMUL(x2, AA, BB);
        FMUL(t, a24_m, E); FADD(t, AA, t); FMUL(z2, E, t);
    }
    cswap(swap, x2, x3, n); cswap(swap, z2, z3, n);
    mont_inv_m(t, z2, fp);
    FMUL(x2, x2, t);
    mont_from(x2, x2, fp);
    uint8_t be[32];
    bn_to_bytes(x2, n, be, 32);
    for (int i = 0; i < 32; i++) out[i] = be[31 - i];
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t nine[32];
    memset(nine, 0, 32); nine[0] = 9;
    x25519(out, scalar, nine);
}
