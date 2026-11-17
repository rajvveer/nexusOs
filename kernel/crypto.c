/* ============================================================================
 * NexusOS — Symmetric crypto primitives (Phase 57)
 * ============================================================================
 * Own implementations (FIPS 180-4, RFC 2104, RFC 5869, FIPS 197, SP 800-38D).
 * 64-bit arithmetic is limited to shifts/adds/xors (inlined by GCC on i386;
 * no libgcc division helpers are ever needed).
 * ============================================================================ */
#include "crypto.h"
#include "string.h"

/* ============================================================================
 * SHA-256
 * ============================================================================ */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define RD32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (uint32_t)(p)[3])
#define WR32(p, v) do { (p)[0] = (uint8_t)((v) >> 24); (p)[1] = (uint8_t)((v) >> 16); \
                        (p)[2] = (uint8_t)((v) >> 8); (p)[3] = (uint8_t)(v); } while (0)

static void sha256_block(sha256_ctx_t* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = RD32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(sha256_ctx_t* c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c; c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len_lo = c->len_hi = 0; c->buflen = 0;
}

void sha256_update(sha256_ctx_t* c, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t old = c->len_lo;
    c->len_lo += len;
    if (c->len_lo < old) c->len_hi++;
    if (c->buflen) {
        uint32_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; len -= take;
        if (c->buflen < 64) return;
        sha256_block(c, c->buf);
        c->buflen = 0;
    }
    while (len >= 64) { sha256_block(c, p); p += 64; len -= 64; }
    if (len) { memcpy(c->buf, p, len); c->buflen = len; }
}

void sha256_final(sha256_ctx_t* c, uint8_t out[32]) {
    uint32_t bits_lo = c->len_lo << 3;
    uint32_t bits_hi = (c->len_hi << 3) | (c->len_lo >> 29);
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56) sha256_update(c, &z, 1);
    uint8_t lb[8];
    WR32(lb, bits_hi); WR32(lb + 4, bits_lo);
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) WR32(out + 4 * i, c->h[i]);
}

void sha256(const void* data, uint32_t len, uint8_t out[32]) {
    sha256_ctx_t c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ============================================================================
 * SHA-512 / SHA-384
 * ============================================================================ */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static uint64_t rd64(const uint8_t* p) {
    return ((uint64_t)RD32(p) << 32) | (uint64_t)RD32(p + 4);
}
static void wr64(uint8_t* p, uint64_t v) {
    WR32(p, (uint32_t)(v >> 32)); WR32(p + 4, (uint32_t)v);
}

static void sha512_block(sha512_ctx_t* c, const uint8_t* p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) w[i] = rd64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i-15], 1) ^ ROR64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = ROR64(w[i-2], 19) ^ ROR64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint64_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha512_init(sha512_ctx_t* c) {
    c->h[0] = 0x6a09e667f3bcc908ULL; c->h[1] = 0xbb67ae8584caa73bULL;
    c->h[2] = 0x3c6ef372fe94f82bULL; c->h[3] = 0xa54ff53a5f1d36f1ULL;
    c->h[4] = 0x510e527fade682d1ULL; c->h[5] = 0x9b05688c2b3e6c1fULL;
    c->h[6] = 0x1f83d9abfb41bd6bULL; c->h[7] = 0x5be0cd19137e2179ULL;
    c->len_lo = c->len_hi = 0; c->buflen = 0; c->outlen = 64;
}

void sha384_init(sha512_ctx_t* c) {
    c->h[0] = 0xcbbb9d5dc1059ed8ULL; c->h[1] = 0x629a292a367cd507ULL;
    c->h[2] = 0x9159015a3070dd17ULL; c->h[3] = 0x152fecd8f70e5939ULL;
    c->h[4] = 0x67332667ffc00b31ULL; c->h[5] = 0x8eb44a8768581511ULL;
    c->h[6] = 0xdb0c2e0d64f98fa7ULL; c->h[7] = 0x47b5481dbefa4fa4ULL;
    c->len_lo = c->len_hi = 0; c->buflen = 0; c->outlen = 48;
}

void sha512_update(sha512_ctx_t* c, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t old = c->len_lo;
    c->len_lo += len;
    if (c->len_lo < old) c->len_hi++;
    if (c->buflen) {
        uint32_t take = 128 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; len -= take;
        if (c->buflen < 128) return;
        sha512_block(c, c->buf);
        c->buflen = 0;
    }
    while (len >= 128) { sha512_block(c, p); p += 128; len -= 128; }
    if (len) { memcpy(c->buf, p, len); c->buflen = len; }
}

void sha512_final(sha512_ctx_t* c, uint8_t* out) {
    uint32_t bits_lo = c->len_lo << 3;
    uint32_t bits_hi = (c->len_hi << 3) | (c->len_lo >> 29);
    uint8_t pad = 0x80;
    sha512_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 112) sha512_update(c, &z, 1);
    uint8_t lb[16];
    memset(lb, 0, 8);
    WR32(lb + 8, bits_hi); WR32(lb + 12, bits_lo);
    sha512_update(c, lb, 16);
    for (int i = 0; i < c->outlen / 8; i++) wr64(out + 8 * i, c->h[i]);
}

/* ============================================================================
 * Generic hash by id
 * ============================================================================ */
int hash_len(int id) {
    switch (id) {
        case HASH_SHA256: return 32;
        case HASH_SHA384: return 48;
        case HASH_SHA512: return 64;
        default: return 0;
    }
}

int hash_compute2(int id, const void* a, uint32_t alen, const void* b, uint32_t blen, uint8_t* out) {
    if (id == HASH_SHA256) {
        sha256_ctx_t c; sha256_init(&c);
        sha256_update(&c, a, alen); if (b && blen) sha256_update(&c, b, blen);
        sha256_final(&c, out); return 32;
    }
    if (id == HASH_SHA384 || id == HASH_SHA512) {
        sha512_ctx_t c;
        if (id == HASH_SHA384) sha384_init(&c); else sha512_init(&c);
        sha512_update(&c, a, alen); if (b && blen) sha512_update(&c, b, blen);
        sha512_final(&c, out); return c.outlen;
    }
    return 0;
}

int hash_compute(int id, const void* data, uint32_t len, uint8_t* out) {
    return hash_compute2(id, data, len, NULL, 0, out);
}

/* ============================================================================
 * HMAC-SHA256 / HKDF
 * ============================================================================ */
void hmac_sha256(const uint8_t* key, uint32_t klen, const void* data, uint32_t len, uint8_t out[32]) {
    uint8_t k[64], pad[64], inner[32];
    memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    sha256_ctx_t c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, data, len); sha256_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

void hkdf_extract(const uint8_t* salt, uint32_t slen, const uint8_t* ikm, uint32_t ilen, uint8_t prk[32]) {
    uint8_t zero[32];
    if (!salt || slen == 0) { memset(zero, 0, 32); salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t* info, uint32_t ilen, uint8_t* out, uint32_t olen) {
    uint8_t t[32], buf[32 + 255 + 1];
    uint32_t tlen = 0, done = 0;
    uint8_t ctr = 1;
    while (done < olen) {
        uint32_t n = 0;
        if (tlen) { memcpy(buf, t, tlen); n = tlen; }
        memcpy(buf + n, info, ilen); n += ilen;
        buf[n++] = ctr++;
        hmac_sha256(prk, 32, buf, n, t);
        tlen = 32;
        uint32_t take = olen - done < 32 ? olen - done : 32;
        memcpy(out + done, t, take);
        done += take;
    }
}

void hkdf_expand_label(const uint8_t secret[32], const char* label,
                       const uint8_t* ctx, uint32_t clen, uint8_t* out, uint32_t olen) {
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    uint32_t ll = strlen(label);
    uint32_t n = 0;
    info[n++] = (uint8_t)(olen >> 8); info[n++] = (uint8_t)olen;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = (uint8_t)clen;
    if (clen) { memcpy(info + n, ctx, clen); n += clen; }
    hkdf_expand(secret, info, n, out, olen);
}

/* ============================================================================
 * AES-128 (FIPS 197) — T-tables built at first use from the S-box
 * ============================================================================ */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint32_t Te0[256], Te1[256], Te2[256], Te3[256];
static bool te_ready = false;

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes_build_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t s = SBOX[i], s2 = xtime(s), s3 = (uint8_t)(s2 ^ s);
        uint32_t t = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
        Te0[i] = t;
        Te1[i] = (t >> 8) | (t << 24);
        Te2[i] = (t >> 16) | (t << 16);
        Te3[i] = (t >> 24) | (t << 8);
    }
    te_ready = true;
}

void aes128_set_key(aes128_ctx_t* c, const uint8_t key[16]) {
    if (!te_ready) aes_build_tables();
    uint32_t* rk = c->rk;
    for (int i = 0; i < 4; i++) rk[i] = RD32(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if ((i & 3) == 0) {
            t = (t << 8) | (t >> 24);               /* RotWord */
            t = ((uint32_t)SBOX[t >> 24] << 24) | ((uint32_t)SBOX[(t >> 16) & 0xff] << 16) |
                ((uint32_t)SBOX[(t >> 8) & 0xff] << 8) | SBOX[t & 0xff];
            t ^= (uint32_t)rcon << 24;
            rcon = xtime(rcon);
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

void aes128_encrypt_block(const aes128_ctx_t* c, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t* rk = c->rk;
    uint32_t s0 = RD32(in) ^ rk[0], s1 = RD32(in + 4) ^ rk[1];
    uint32_t s2 = RD32(in + 8) ^ rk[2], s3 = RD32(in + 12) ^ rk[3];
    uint32_t t0, t1, t2, t3;
    rk += 4;
    for (int r = 0; r < 9; r++) {
        t0 = Te0[s0 >> 24] ^ Te1[(s1 >> 16) & 0xff] ^ Te2[(s2 >> 8) & 0xff] ^ Te3[s3 & 0xff] ^ rk[0];
        t1 = Te0[s1 >> 24] ^ Te1[(s2 >> 16) & 0xff] ^ Te2[(s3 >> 8) & 0xff] ^ Te3[s0 & 0xff] ^ rk[1];
        t2 = Te0[s2 >> 24] ^ Te1[(s3 >> 16) & 0xff] ^ Te2[(s0 >> 8) & 0xff] ^ Te3[s1 & 0xff] ^ rk[2];
        t3 = Te0[s3 >> 24] ^ Te1[(s0 >> 16) & 0xff] ^ Te2[(s1 >> 8) & 0xff] ^ Te3[s2 & 0xff] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
        rk += 4;
    }
    /* final round: SubBytes + ShiftRows + AddRoundKey */
    t0 = ((uint32_t)SBOX[s0 >> 24] << 24) | ((uint32_t)SBOX[(s1 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s2 >> 8) & 0xff] << 8) | SBOX[s3 & 0xff];
    t1 = ((uint32_t)SBOX[s1 >> 24] << 24) | ((uint32_t)SBOX[(s2 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s3 >> 8) & 0xff] << 8) | SBOX[s0 & 0xff];
    t2 = ((uint32_t)SBOX[s2 >> 24] << 24) | ((uint32_t)SBOX[(s3 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s0 >> 8) & 0xff] << 8) | SBOX[s1 & 0xff];
    t3 = ((uint32_t)SBOX[s3 >> 24] << 24) | ((uint32_t)SBOX[(s0 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s1 >> 8) & 0xff] << 8) | SBOX[s2 & 0xff];
    t0 ^= rk[0]; t1 ^= rk[1]; t2 ^= rk[2]; t3 ^= rk[3];
    WR32(out, t0); WR32(out + 4, t1); WR32(out + 8, t2); WR32(out + 12, t3);
}

/* ============================================================================
 * GCM (SP 800-38D) — GHASH with 4-bit tables (Shoup)
 * ============================================================================ */
static const uint64_t last4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0
};

void gcm_set_key(gcm_ctx_t* g, const uint8_t key[16]) {
    uint8_t zero[16], h[16];
    memset(zero, 0, 16);
    aes128_set_key(&g->aes, key);
    aes128_encrypt_block(&g->aes, zero, h);
    uint64_t vh = rd64(h), vl = rd64(h + 8);
    g->hl[8] = vl; g->hh[8] = vh;
    g->hl[0] = 0; g->hh[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        uint32_t T = (uint32_t)(vl & 1) * 0xe1000000u;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((uint64_t)T << 32);
        g->hl[i] = vl; g->hh[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2) {
        uint64_t* HiL = g->hl + i; uint64_t* HiH = g->hh + i;
        vh = *HiH; vl = *HiL;
        for (int j = 1; j < i; j++) {
            HiH[j] = vh ^ g->hh[j];
            HiL[j] = vl ^ g->hl[j];
        }
    }
}

static void gcm_mult(const gcm_ctx_t* g, uint8_t x[16]) {
    uint8_t lo = x[15] & 0xf, hi = x[15] >> 4, rem;
    uint64_t zh = g->hh[lo], zl = g->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 0xf; hi = x[i] >> 4;
        if (i != 15) {
            rem = (uint8_t)(zl & 0xf);
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4);
            zh ^= last4[rem] << 48;
            zh ^= g->hh[lo]; zl ^= g->hl[lo];
        }
        rem = (uint8_t)(zl & 0xf);
        zl = (zh << 60) | (zl >> 4);
        zh = (zh >> 4);
        zh ^= last4[rem] << 48;
        zh ^= g->hh[hi]; zl ^= g->hl[hi];
    }
    wr64(x, zh); wr64(x + 8, zl);
}

static void ghash_update(const gcm_ctx_t* g, uint8_t y[16], const uint8_t* data, uint32_t len) {
    while (len >= 16) {
        for (int i = 0; i < 16; i++) y[i] ^= data[i];
        gcm_mult(g, y);
        data += 16; len -= 16;
    }
    if (len) {
        for (uint32_t i = 0; i < len; i++) y[i] ^= data[i];
        gcm_mult(g, y);
    }
}

static void gcm_crypt(const gcm_ctx_t* g, const uint8_t iv[12], const uint8_t* aad, uint32_t aadlen,
                      const uint8_t* in, uint8_t* out, uint32_t len, uint8_t tag[16], bool encrypt) {
    uint8_t j0[16], ctr[16], ek[16], y[16];
    memcpy(j0, iv, 12); j0[12] = j0[13] = j0[14] = 0; j0[15] = 1;
    memcpy(ctr, j0, 16);
    memset(y, 0, 16);
    ghash_update(g, y, aad, aadlen);

    uint32_t done = 0;
    while (done < len) {
        /* increment the 32-bit counter (big-endian) */
        for (int i = 15; i >= 12; i--) { if (++ctr[i]) break; }
        aes128_encrypt_block(&g->aes, ctr, ek);
        uint32_t n = len - done < 16 ? len - done : 16;
        if (encrypt) {
            for (uint32_t i = 0; i < n; i++) out[done + i] = in[done + i] ^ ek[i];
            ghash_update(g, y, out + done, n);
        } else {
            ghash_update(g, y, in + done, n);
            for (uint32_t i = 0; i < n; i++) out[done + i] = in[done + i] ^ ek[i];
        }
        done += n;
    }
    /* lengths block: [len(A)]64 || [len(C)]64 in bits */
    uint8_t lb[16];
    memset(lb, 0, 16);
    WR32(lb + 4, aadlen << 3);  lb[3] = (uint8_t)(aadlen >> 29);
    WR32(lb + 12, len << 3);    lb[11] = (uint8_t)(len >> 29);
    ghash_update(g, y, lb, 16);
    aes128_encrypt_block(&g->aes, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ ek[i];
}

void gcm_encrypt(const gcm_ctx_t* g, const uint8_t iv[12], const uint8_t* aad, uint32_t aadlen,
                 const uint8_t* in, uint8_t* out, uint32_t len, uint8_t tag[16]) {
    gcm_crypt(g, iv, aad, aadlen, in, out, len, tag, true);
}

int gcm_decrypt(const gcm_ctx_t* g, const uint8_t iv[12], const uint8_t* aad, uint32_t aadlen,
                const uint8_t* in, uint8_t* out, uint32_t len, const uint8_t tag[16]) {
    uint8_t t[16];
    gcm_crypt(g, iv, aad, aadlen, in, out, len, t, false);
    return crypto_memeq(t, tag, 16);
}

/* ============================================================================
 * Misc
 * ============================================================================ */
int crypto_memeq(const void* a, const void* b, uint32_t len) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    uint8_t d = 0;
    for (uint32_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d ? -1 : 0;
}

int crypto_random(uint8_t* out, uint32_t len) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 30))) return -1;
    while (len) {
        uint32_t word = 0;
        uint8_t ok = 0;
        for (int tries = 0; tries < 10 && !ok; tries++)
            __asm__ volatile("rdrand %0; setc %1" : "=r"(word), "=qm"(ok) : : "cc");
        if (!ok) return -1;
        uint32_t n = len < 4 ? len : 4;
        memcpy(out, &word, n);
        out += n; len -= n;
    }
    return 0;
}

uint32_t crc32_update(uint32_t crc, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}
