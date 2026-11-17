/* Cryptographic primitives of the native kernel, written from their specifications: BLAKE2s (RFC 7693), used by
 * WireGuard (hash, keyed MAC, HMAC/HKDF) and as the RFC 7217 stable-privacy PRF of IPv6 address generation;
 * ChaCha20-Poly1305 AEAD (RFC 8439) and XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha) for WireGuard; X25519
 * (RFC 7748) with 51-bit limbs (64x64->128 multiplies, constant time). Self-tests with the RFC test vectors run at
 * boot (wgcrypto_selftest) and in tests/wgcrypto_native_test.c. */
#include "wgcrypto.h"
#include "../../kernel/string.h"

static inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void store_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static inline void store_le64(uint8_t* p, uint64_t v) { store_le32(p, (uint32_t)v); store_le32(p + 4, (uint32_t)(v >> 32)); }
void wg_memzero(void* data, unsigned length) { volatile uint8_t* p = data; while (length--) *p++ = 0; }
bool wg_equal(const void* a, const void* b, unsigned length) {
    const uint8_t *x = a, *y = b; uint8_t diff = 0;
    for (unsigned i = 0; i < length; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}

/* ---- BLAKE2s ---- */
static const uint32_t blake2s_iv[8] = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU, 0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
static const uint8_t sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};
static void blake2s_compress(struct blake2s_state* s, const uint8_t* block, uint32_t increment) {
    uint32_t m[16], v[16];
    s->t[0] += increment; if (s->t[0] < increment) s->t[1]++;
    for (unsigned i = 0; i < 16; i++) m[i] = le32(block + 4 * i);
    for (unsigned i = 0; i < 8; i++) { v[i] = s->h[i]; v[i + 8] = blake2s_iv[i]; }
    v[12] ^= s->t[0]; v[13] ^= s->t[1]; v[14] ^= s->f;
#define G(a, b, c, d, x, y) do { v[a] += v[b] + (x); v[d] = rotr32(v[d] ^ v[a], 16); v[c] += v[d]; v[b] = rotr32(v[b] ^ v[c], 12); \
        v[a] += v[b] + (y); v[d] = rotr32(v[d] ^ v[a], 8); v[c] += v[d]; v[b] = rotr32(v[b] ^ v[c], 7); } while (0)
    for (unsigned r = 0; r < 10; r++) {
        const uint8_t* z = sigma[r];
        G(0, 4, 8, 12, m[z[0]], m[z[1]]); G(1, 5, 9, 13, m[z[2]], m[z[3]]); G(2, 6, 10, 14, m[z[4]], m[z[5]]); G(3, 7, 11, 15, m[z[6]], m[z[7]]);
        G(0, 5, 10, 15, m[z[8]], m[z[9]]); G(1, 6, 11, 12, m[z[10]], m[z[11]]); G(2, 7, 8, 13, m[z[12]], m[z[13]]); G(3, 4, 9, 14, m[z[14]], m[z[15]]);
    }
#undef G
    for (unsigned i = 0; i < 8; i++) s->h[i] ^= v[i] ^ v[i + 8];
    wg_memzero(m, sizeof(m)); wg_memzero(v, sizeof(v));
}
void blake2s_init(struct blake2s_state* s, unsigned out_length, const void* key, unsigned key_length) {
    memset(s, 0, sizeof(*s));
    for (unsigned i = 0; i < 8; i++) s->h[i] = blake2s_iv[i];
    s->h[0] ^= 0x01010000U ^ (key_length << 8) ^ out_length;
    s->out_length = out_length;
    if (key_length) { uint8_t block[64] = {0}; memcpy(block, key, key_length); blake2s_update(s, block, 64); wg_memzero(block, 64); }
}
void blake2s_update(struct blake2s_state* s, const void* data, unsigned length) {
    const uint8_t* p = data;
    while (length) {
        if (s->buffered == 64) { blake2s_compress(s, s->buffer, 64); s->buffered = 0; }   /* keep the last block for final */
        unsigned take = 64 - s->buffered; if (take > length) take = length;
        memcpy(s->buffer + s->buffered, p, take); s->buffered += take; p += take; length -= take;
    }
}
void blake2s_final(struct blake2s_state* s, uint8_t* out) {
    memset(s->buffer + s->buffered, 0, 64 - s->buffered);
    s->f = 0xFFFFFFFFU; blake2s_compress(s, s->buffer, s->buffered);
    uint8_t full[32]; for (unsigned i = 0; i < 8; i++) store_le32(full + 4 * i, s->h[i]);
    memcpy(out, full, s->out_length); wg_memzero(full, 32); wg_memzero(s, sizeof(*s));
}
void blake2s(uint8_t* out, unsigned out_length, const void* key, unsigned key_length, const void* data, unsigned length) {
    struct blake2s_state s; blake2s_init(&s, out_length, key, key_length); blake2s_update(&s, data, length); blake2s_final(&s, out);
}
void hmac_blake2s(uint8_t out[32], const void* key, unsigned key_length, const void* data, unsigned length) {
    uint8_t k[64] = {0}, pad[64]; struct blake2s_state s;
    if (key_length > 64) blake2s(k, 32, NULL, 0, key, key_length); else memcpy(k, key, key_length);
    for (unsigned i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    uint8_t inner[32]; blake2s_init(&s, 32, NULL, 0); blake2s_update(&s, pad, 64); blake2s_update(&s, data, length); blake2s_final(&s, inner);
    for (unsigned i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    blake2s_init(&s, 32, NULL, 0); blake2s_update(&s, pad, 64); blake2s_update(&s, inner, 32); blake2s_final(&s, out);
    wg_memzero(k, 64); wg_memzero(pad, 64); wg_memzero(inner, 32);
}

/* ---- ChaCha20 (RFC 8439) ---- */
#define QR(a, b, c, d) do { x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16); x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12); \
                            x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8); x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7); } while (0)
static void chacha_rounds(uint32_t x[16]) {
    for (unsigned i = 0; i < 10; i++) {
        QR(0, 4, 8, 12); QR(1, 5, 9, 13); QR(2, 6, 10, 14); QR(3, 7, 11, 15);
        QR(0, 5, 10, 15); QR(1, 6, 11, 12); QR(2, 7, 8, 13); QR(3, 4, 9, 14);
    }
}
static void chacha_block(const uint32_t state[16], uint8_t out[64]) {
    uint32_t x[16]; memcpy(x, state, 64); chacha_rounds(x);
    for (unsigned i = 0; i < 16; i++) store_le32(out + 4 * i, x[i] + state[i]);
    wg_memzero(x, sizeof(x));
}
static void chacha_setup(uint32_t state[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    state[0] = 0x61707865U; state[1] = 0x3320646EU; state[2] = 0x79622D32U; state[3] = 0x6B206574U;
    for (unsigned i = 0; i < 8; i++) state[4 + i] = le32(key + 4 * i);
    state[12] = counter; state[13] = le32(nonce); state[14] = le32(nonce + 4); state[15] = le32(nonce + 8);
}
void chacha20_xor(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    uint32_t state[16]; uint8_t stream[64]; chacha_setup(state, key, counter, nonce);
    while (length) {
        chacha_block(state, stream); state[12]++;
        unsigned n = length < 64 ? length : 64;
        for (unsigned i = 0; i < n; i++) out[i] = in[i] ^ stream[i];
        out += n; in += n; length -= n;
    }
    wg_memzero(state, sizeof(state)); wg_memzero(stream, sizeof(stream));
}
void hchacha20(uint8_t out[32], const uint8_t key[32], const uint8_t nonce[16]) {
    uint32_t x[16]; x[0] = 0x61707865U; x[1] = 0x3320646EU; x[2] = 0x79622D32U; x[3] = 0x6B206574U;
    for (unsigned i = 0; i < 8; i++) x[4 + i] = le32(key + 4 * i);
    for (unsigned i = 0; i < 4; i++) x[12 + i] = le32(nonce + 4 * i);
    chacha_rounds(x);
    for (unsigned i = 0; i < 4; i++) { store_le32(out + 4 * i, x[i]); store_le32(out + 16 + 4 * i, x[12 + i]); }
    wg_memzero(x, sizeof(x));
}
#undef QR

/* ---- Poly1305 (RFC 8439), 44-bit limbs with 64x64->128 products ---- */
struct poly1305 { uint64_t r[3], h[3], pad[2]; uint8_t buffer[16]; unsigned buffered; };
static void poly_init(struct poly1305* p, const uint8_t key[32]) {
    uint64_t t0 = le64(key), t1 = le64(key + 8);
    p->r[0] = t0 & 0xFFC0FFFFFFFULL; p->r[1] = ((t0 >> 44) | (t1 << 20)) & 0xFFFFFC0FFFFULL; p->r[2] = (t1 >> 24) & 0x00FFFFFFC0FULL;
    p->h[0] = p->h[1] = p->h[2] = 0; p->pad[0] = le64(key + 16); p->pad[1] = le64(key + 24); p->buffered = 0;
}
static void poly_blocks(struct poly1305* p, const uint8_t* m, unsigned length, uint64_t hibit) {
    const uint64_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], s1 = r1 * (5 << 2), s2 = r2 * (5 << 2);
    uint64_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2];
    while (length >= 16) {
        uint64_t t0 = le64(m), t1 = le64(m + 8);
        h0 += t0 & 0xFFFFFFFFFFFULL; h1 += ((t0 >> 44) | (t1 << 20)) & 0xFFFFFFFFFFFULL; h2 += ((t1 >> 24) & 0x3FFFFFFFFFFULL) | hibit;
        unsigned __int128 d0 = (unsigned __int128)h0 * r0 + (unsigned __int128)h1 * s2 + (unsigned __int128)h2 * s1;
        unsigned __int128 d1 = (unsigned __int128)h0 * r1 + (unsigned __int128)h1 * r0 + (unsigned __int128)h2 * s2;
        unsigned __int128 d2 = (unsigned __int128)h0 * r2 + (unsigned __int128)h1 * r1 + (unsigned __int128)h2 * r0;
        uint64_t c = (uint64_t)(d0 >> 44); h0 = (uint64_t)d0 & 0xFFFFFFFFFFFULL;
        d1 += c; c = (uint64_t)(d1 >> 44); h1 = (uint64_t)d1 & 0xFFFFFFFFFFFULL;
        d2 += c; c = (uint64_t)(d2 >> 42); h2 = (uint64_t)d2 & 0x3FFFFFFFFFFULL;
        h0 += c * 5; c = h0 >> 44; h0 &= 0xFFFFFFFFFFFULL; h1 += c;
        m += 16; length -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2;
}
static void poly_update(struct poly1305* p, const uint8_t* m, unsigned length) {
    if (p->buffered) {
        unsigned take = 16 - p->buffered; if (take > length) take = length;
        memcpy(p->buffer + p->buffered, m, take); p->buffered += take; m += take; length -= take;
        if (p->buffered < 16) return;
        poly_blocks(p, p->buffer, 16, 1ULL << 40); p->buffered = 0;
    }
    unsigned whole = length & ~15U;
    if (whole) { poly_blocks(p, m, whole, 1ULL << 40); m += whole; length -= whole; }
    if (length) { memcpy(p->buffer, m, length); p->buffered = length; }
}
static void poly_final(struct poly1305* p, uint8_t tag[16]) {
    if (p->buffered) {
        p->buffer[p->buffered] = 1; memset(p->buffer + p->buffered + 1, 0, 15 - p->buffered);
        poly_blocks(p, p->buffer, 16, 0);
    }
    uint64_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], c;
    c = h1 >> 44; h1 &= 0xFFFFFFFFFFFULL; h2 += c; c = h2 >> 42; h2 &= 0x3FFFFFFFFFFULL;
    h0 += c * 5; c = h0 >> 44; h0 &= 0xFFFFFFFFFFFULL; h1 += c; c = h1 >> 44; h1 &= 0xFFFFFFFFFFFULL; h2 += c;
    c = h2 >> 42; h2 &= 0x3FFFFFFFFFFULL; h0 += c * 5; c = h0 >> 44; h0 &= 0xFFFFFFFFFFFULL; h1 += c;
    uint64_t g0 = h0 + 5; c = g0 >> 44; g0 &= 0xFFFFFFFFFFFULL;
    uint64_t g1 = h1 + c; c = g1 >> 44; g1 &= 0xFFFFFFFFFFFULL;
    uint64_t g2 = h2 + c - (1ULL << 42);
    c = (g2 >> 63) - 1;   /* all ones when h >= p */
    g0 &= c; g1 &= c; g2 &= c; c = ~c;
    h0 = (h0 & c) | g0; h1 = (h1 & c) | g1; h2 = (h2 & c) | g2;
    uint64_t t0 = p->pad[0], t1 = p->pad[1];
    h0 += t0 & 0xFFFFFFFFFFFULL; c = h0 >> 44; h0 &= 0xFFFFFFFFFFFULL;
    h1 += (((t0 >> 44) | (t1 << 20)) & 0xFFFFFFFFFFFULL) + c; c = h1 >> 44; h1 &= 0xFFFFFFFFFFFULL;
    h2 += ((t1 >> 24) & 0x3FFFFFFFFFFULL) + c; h2 &= 0x3FFFFFFFFFFULL;
    h0 = h0 | (h1 << 44); h1 = (h1 >> 20) | (h2 << 24);
    store_le64(tag, h0); store_le64(tag + 8, h1);
    wg_memzero(p, sizeof(*p));
}
void poly1305(uint8_t tag[16], const uint8_t* message, unsigned length, const uint8_t key[32]) {
    struct poly1305 p; poly_init(&p, key); poly_update(&p, message, length); poly_final(&p, tag);
}

/* ---- AEAD ---- */
static void aead_tag(uint8_t tag[16], const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, unsigned ad_length,
                     const uint8_t* ciphertext, unsigned length) {
    uint8_t block[64] = {0}, otk[32];
    chacha20_xor(block, block, 64, key, 0, nonce); memcpy(otk, block, 32);
    struct poly1305 p; poly_init(&p, otk);
    static const uint8_t zeros[16] = {0};
    poly_update(&p, ad, ad_length); if (ad_length & 15) poly_update(&p, zeros, 16 - (ad_length & 15));
    poly_update(&p, ciphertext, length); if (length & 15) poly_update(&p, zeros, 16 - (length & 15));
    uint8_t lengths[16]; store_le64(lengths, ad_length); store_le64(lengths + 8, length); poly_update(&p, lengths, 16);
    poly_final(&p, tag);
    wg_memzero(block, 64); wg_memzero(otk, 32);
}
static void nonce64(uint8_t nonce[12], uint64_t counter) { store_le32(nonce, 0); store_le64(nonce + 4, counter); }
void chacha20poly1305_encrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, uint64_t counter, const uint8_t key[32]) {
    uint8_t nonce[12]; nonce64(nonce, counter);
    chacha20_xor(out, in, length, key, 1, nonce);
    aead_tag(out + length, key, nonce, ad, ad_length, out, length);
}
bool chacha20poly1305_decrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, uint64_t counter, const uint8_t key[32]) {
    if (length < 16) return false;
    uint8_t nonce[12], tag[16]; nonce64(nonce, counter);
    aead_tag(tag, key, nonce, ad, ad_length, in, length - 16);
    if (!wg_equal(tag, in + length - 16, 16)) { wg_memzero(tag, 16); return false; }
    chacha20_xor(out, in, length - 16, key, 1, nonce);
    return true;
}
void xchacha20poly1305_encrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, const uint8_t nonce[24], const uint8_t key[32]) {
    uint8_t subkey[32], n[12] = {0}; hchacha20(subkey, key, nonce); memcpy(n + 4, nonce + 16, 8);
    chacha20_xor(out, in, length, subkey, 1, n); aead_tag(out + length, subkey, n, ad, ad_length, out, length);
    wg_memzero(subkey, 32);
}
bool xchacha20poly1305_decrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, const uint8_t nonce[24], const uint8_t key[32]) {
    if (length < 16) return false;
    uint8_t subkey[32], n[12] = {0}, tag[16]; hchacha20(subkey, key, nonce); memcpy(n + 4, nonce + 16, 8);
    aead_tag(tag, subkey, n, ad, ad_length, in, length - 16);
    bool ok = wg_equal(tag, in + length - 16, 16);
    if (ok) chacha20_xor(out, in, length - 16, subkey, 1, n);
    wg_memzero(subkey, 32);
    return ok;
}

/* ---- X25519 (RFC 7748): field arithmetic mod 2^255-19 in five 51-bit limbs ---- */
typedef uint64_t fe[5];
#define MASK51 0x7FFFFFFFFFFFFULL
static void fe_frombytes(fe h, const uint8_t s[32]) {
    uint64_t a = le64(s), b = le64(s + 8), c = le64(s + 16), d = le64(s + 24);
    h[0] = a & MASK51; h[1] = ((a >> 51) | (b << 13)) & MASK51; h[2] = ((b >> 38) | (c << 26)) & MASK51;
    h[3] = ((c >> 25) | (d << 39)) & MASK51; h[4] = (d >> 12) & MASK51;   /* the top bit is ignored */
}
static void fe_carry(fe h) {
    uint64_t c;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c; c = h[1] >> 51; h[1] &= MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= MASK51; h[3] += c; c = h[3] >> 51; h[3] &= MASK51; h[4] += c;
    c = h[4] >> 51; h[4] &= MASK51; h[0] += c * 19;
}
static void fe_tobytes(uint8_t s[32], const fe f) {
    fe h; memcpy(h, f, sizeof(fe)); fe_carry(h); fe_carry(h);
    /* subtract p if h >= p */
    uint64_t q = (h[0] + 19) >> 51; q = (h[1] + q) >> 51; q = (h[2] + q) >> 51; q = (h[3] + q) >> 51; q = (h[4] + q) >> 51;
    h[0] += 19 * q;
    uint64_t c = h[0] >> 51; h[0] &= MASK51; h[1] += c; c = h[1] >> 51; h[1] &= MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= MASK51; h[3] += c; c = h[3] >> 51; h[3] &= MASK51; h[4] += c; h[4] &= MASK51;
    store_le64(s, h[0] | (h[1] << 51)); store_le64(s + 8, (h[1] >> 13) | (h[2] << 38));
    store_le64(s + 16, (h[2] >> 26) | (h[3] << 25)); store_le64(s + 24, (h[3] >> 39) | (h[4] << 12));
}
static void fe_add(fe h, const fe f, const fe g) { for (unsigned i = 0; i < 5; i++) h[i] = f[i] + g[i]; }
static void fe_sub(fe h, const fe f, const fe g) {
    /* add 2p first so limbs stay positive */
    h[0] = f[0] + 0xFFFFFFFFFFFDAULL - g[0]; h[1] = f[1] + 0xFFFFFFFFFFFFEULL - g[1]; h[2] = f[2] + 0xFFFFFFFFFFFFEULL - g[2];
    h[3] = f[3] + 0xFFFFFFFFFFFFEULL - g[3]; h[4] = f[4] + 0xFFFFFFFFFFFFEULL - g[4];
    fe_carry(h);
}
static void fe_mul(fe h, const fe f, const fe g) {
    typedef unsigned __int128 u128;
    uint64_t g1_19 = g[1] * 19, g2_19 = g[2] * 19, g3_19 = g[3] * 19, g4_19 = g[4] * 19;
    u128 r0 = (u128)f[0] * g[0] + (u128)f[1] * g4_19 + (u128)f[2] * g3_19 + (u128)f[3] * g2_19 + (u128)f[4] * g1_19;
    u128 r1 = (u128)f[0] * g[1] + (u128)f[1] * g[0] + (u128)f[2] * g4_19 + (u128)f[3] * g3_19 + (u128)f[4] * g2_19;
    u128 r2 = (u128)f[0] * g[2] + (u128)f[1] * g[1] + (u128)f[2] * g[0] + (u128)f[3] * g4_19 + (u128)f[4] * g3_19;
    u128 r3 = (u128)f[0] * g[3] + (u128)f[1] * g[2] + (u128)f[2] * g[1] + (u128)f[3] * g[0] + (u128)f[4] * g4_19;
    u128 r4 = (u128)f[0] * g[4] + (u128)f[1] * g[3] + (u128)f[2] * g[2] + (u128)f[3] * g[1] + (u128)f[4] * g[0];
    uint64_t c;
    c = (uint64_t)(r0 >> 51); h[0] = (uint64_t)r0 & MASK51; r1 += c;
    c = (uint64_t)(r1 >> 51); h[1] = (uint64_t)r1 & MASK51; r2 += c;
    c = (uint64_t)(r2 >> 51); h[2] = (uint64_t)r2 & MASK51; r3 += c;
    c = (uint64_t)(r3 >> 51); h[3] = (uint64_t)r3 & MASK51; r4 += c;
    c = (uint64_t)(r4 >> 51); h[4] = (uint64_t)r4 & MASK51; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
}
static void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }
static void fe_mul121666(fe h, const fe f) {
    typedef unsigned __int128 u128; u128 t[5]; uint64_t c = 0;
    for (unsigned i = 0; i < 5; i++) t[i] = (u128)f[i] * 121666;
    for (unsigned i = 0; i < 5; i++) { t[i] += c; h[i] = (uint64_t)t[i] & MASK51; c = (uint64_t)(t[i] >> 51); }
    h[0] += c * 19; c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
}
static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3; unsigned i;
    fe_sq(t0, z); fe_sq(t1, t0); fe_sq(t1, t1); fe_mul(t1, z, t1); fe_mul(t0, t0, t1); fe_sq(t2, t0); fe_mul(t1, t1, t2);
    fe_sq(t2, t1); for (i = 1; i < 5; i++) fe_sq(t2, t2); fe_mul(t1, t2, t1);
    fe_sq(t2, t1); for (i = 1; i < 10; i++) fe_sq(t2, t2); fe_mul(t2, t2, t1);
    fe_sq(t3, t2); for (i = 1; i < 20; i++) fe_sq(t3, t3); fe_mul(t2, t3, t2);
    fe_sq(t2, t2); for (i = 1; i < 10; i++) fe_sq(t2, t2); fe_mul(t1, t2, t1);
    fe_sq(t2, t1); for (i = 1; i < 50; i++) fe_sq(t2, t2); fe_mul(t2, t2, t1);
    fe_sq(t3, t2); for (i = 1; i < 100; i++) fe_sq(t3, t3); fe_mul(t2, t3, t2);
    fe_sq(t2, t2); for (i = 1; i < 50; i++) fe_sq(t2, t2); fe_mul(t1, t2, t1);
    fe_sq(t1, t1); for (i = 1; i < 5; i++) fe_sq(t1, t1); fe_mul(out, t1, t0);
}
static void fe_cswap(fe f, fe g, uint64_t swap) {
    uint64_t mask = 0 - swap;
    for (unsigned i = 0; i < 5; i++) { uint64_t x = (f[i] ^ g[i]) & mask; f[i] ^= x; g[i] ^= x; }
}
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32]; memcpy(e, scalar, 32); e[0] &= 248; e[31] &= 127; e[31] |= 64;
    fe x1, x2 = {1, 0, 0, 0, 0}, z2 = {0}, x3, z3 = {1, 0, 0, 0, 0}, a, aa, b, bb, ee, c, d, da, cb;
    fe_frombytes(x1, point); memcpy(x3, x1, sizeof(fe));
    uint64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        uint64_t bit = (e[t >> 3] >> (t & 7)) & 1;
        swap ^= bit; fe_cswap(x2, x3, swap); fe_cswap(z2, z3, swap); swap = bit;
        fe_add(a, x2, z2); fe_sq(aa, a); fe_sub(b, x2, z2); fe_sq(bb, b); fe_sub(ee, aa, bb);
        fe_add(c, x3, z3); fe_sub(d, x3, z3); fe_mul(da, d, a); fe_mul(cb, c, b);
        fe_add(x3, da, cb); fe_sq(x3, x3); fe_sub(z3, da, cb); fe_sq(z3, z3); fe_mul(z3, z3, x1);
        fe_mul(x2, aa, bb); fe_mul121666(z2, ee); fe_add(z2, z2, bb); fe_mul(z2, z2, ee);   /* E*(BB + 121666*E) = E*(AA + 121665*E) */
    }
    fe_cswap(x2, x3, swap); fe_cswap(z2, z3, swap);
    fe_invert(z2, z2); fe_mul(x2, x2, z2); fe_tobytes(out, x2);
    wg_memzero(e, 32);
}
void x25519_public(uint8_t out[32], const uint8_t secret[32]) { static const uint8_t base[32] = {9}; x25519(out, secret, base); }

/* ---- self-test (RFC vectors) ---- */
static int hex_nibble(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static void from_hex(uint8_t* out, const char* hex) { while (hex[0] && hex[1]) { *out++ = (uint8_t)(hex_nibble(hex[0]) << 4 | hex_nibble(hex[1])); hex += 2; } }
bool wgcrypto_selftest(void) {
    bool ok = true; uint8_t out[64], expect[64];
    /* RFC 7693 appendix B: BLAKE2s-256("abc") */
    blake2s(out, 32, NULL, 0, "abc", 3);
    from_hex(expect, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"); ok = ok && !memcmp(out, expect, 32);
    /* RFC 7748 5.2, first X25519 vector */
    uint8_t k[32], u[32];
    from_hex(k, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    from_hex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    x25519(out, k, u); from_hex(expect, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"); ok = ok && !memcmp(out, expect, 32);
    /* RFC 7748 6.1: Alice's public key */
    from_hex(k, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    x25519_public(out, k); from_hex(expect, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"); ok = ok && !memcmp(out, expect, 32);
    /* RFC 8439 2.5.2: Poly1305 */
    uint8_t key[32]; from_hex(key, "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    poly1305(out, (const uint8_t*)"Cryptographic Forum Research Group", 34, key);
    from_hex(expect, "a8061dc1305136c6c22b8baf0c0127a9"); ok = ok && !memcmp(out, expect, 16);
    /* RFC 8439 2.8.2: AEAD (first 16 bytes of the ciphertext and the tag) */
    static const char plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    uint8_t aad[12], nonce[12], cipher[114 + 16];
    from_hex(key, "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"); from_hex(aad, "50515253c0c1c2c3c4c5c6c7");
    from_hex(nonce, "070000004041424344454647");
    chacha20_xor(cipher, (const uint8_t*)plain, 114, key, 1, nonce); aead_tag(cipher + 114, key, nonce, aad, 12, cipher, 114);
    from_hex(expect, "d31a8d34648e60db7b86afbc53ef7ec2"); ok = ok && !memcmp(cipher, expect, 16);
    from_hex(expect, "1ae10b594f09e26a7e902ecbd0600691"); ok = ok && !memcmp(cipher + 114, expect, 16);
    return ok;
}
