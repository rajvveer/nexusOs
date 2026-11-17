/* Freestanding SHA-256 and RSA-2048 PKCS#1 v1.5 signature verification for the UEFI loader's secure chain
 * (BOOTX64.EFI verifies KERNEL.BIN before jumping to it). No libc, no heap: everything is on the stack.
 *
 * RSA verification is the public operation s^e mod n with e = 65537. Modular multiplication is done bit-serially
 * (double-and-add with a single conditional subtract), which is slow but obviously correct and needs no bignum
 * division - fine for the ~18 modular multiplications done once at boot. The key is in sb_pubkey.h as 32 little-endian
 * 64-bit limbs (2048-bit modulus) and the exponent. */
#ifndef NEXUS_SB_VERIFY_H
#define NEXUS_SB_VERIFY_H
#include <stdint.h>
#include <stdbool.h>

/* ---- SHA-256 ---- */
struct sha256 { uint32_t h[8]; uint64_t length; uint8_t block[64]; unsigned used; };
static uint32_t sb_ror(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
static void sha256_init(struct sha256* c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (int i = 0; i < 8; i++) c->h[i] = iv[i];
    c->length = 0; c->used = 0;
}
static void sha256_block(struct sha256* c, const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sb_ror(w[i-15],7) ^ sb_ror(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = sb_ror(w[i-2],17) ^ sb_ror(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sb_ror(e,6) ^ sb_ror(e,11) ^ sb_ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = sb_ror(a,2) ^ sb_ror(a,13) ^ sb_ror(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
static void sha256_update(struct sha256* c, const uint8_t* data, uint64_t n) {
    c->length += n;
    while (n) {
        unsigned take = 64 - c->used; if (take > n) take = (unsigned)n;
        for (unsigned i = 0; i < take; i++) c->block[c->used + i] = data[i];
        c->used += take; data += take; n -= take;
        if (c->used == 64) { sha256_block(c, c->block); c->used = 0; }
    }
}
static void sha256_final(struct sha256* c, uint8_t out[32]) {
    uint64_t bits = c->length * 8;
    uint8_t pad = 0x80; sha256_update(c, &pad, 1);
    uint8_t zero = 0; while (c->used != 56) sha256_update(c, &zero, 1);
    uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8*i)); sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) { out[i*4]=(uint8_t)(c->h[i]>>24); out[i*4+1]=(uint8_t)(c->h[i]>>16); out[i*4+2]=(uint8_t)(c->h[i]>>8); out[i*4+3]=(uint8_t)c->h[i]; }
}

/* ---- bignum (RSA_LIMBS 64-bit little-endian limbs) ---- */
#define RSA_LIMBS 32   /* 2048 bits */
struct bn { uint64_t v[RSA_LIMBS]; };
static int bn_cmp(const struct bn* a, const struct bn* b) {
    for (int i = RSA_LIMBS - 1; i >= 0; i--) { if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1; }
    return 0;
}
static void bn_sub(struct bn* a, const struct bn* b) {   /* a -= b (assumes a >= b) */
    unsigned __int128 borrow = 0;
    for (int i = 0; i < RSA_LIMBS; i++) {
        unsigned __int128 t = (unsigned __int128)a->v[i] - b->v[i] - borrow;
        a->v[i] = (uint64_t)t; borrow = (t >> 64) & 1;
    }
}
static int bn_shl1(struct bn* a) {   /* a <<= 1, returns the bit shifted out of the top */
    uint64_t carry = 0;
    for (int i = 0; i < RSA_LIMBS; i++) { uint64_t nc = a->v[i] >> 63; a->v[i] = (a->v[i] << 1) | carry; carry = nc; }
    return (int)carry;
}
/* result = (a * b) mod n, bit-serial double-and-add. a,b < n. */
static void bn_mulmod(struct bn* result, const struct bn* a, const struct bn* b, const struct bn* n) {
    struct bn r; for (int i = 0; i < RSA_LIMBS; i++) r.v[i] = 0;
    for (int i = RSA_LIMBS - 1; i >= 0; i--) {
        for (int bit = 63; bit >= 0; bit--) {
            int top = bn_shl1(&r);                       /* r <<= 1 */
            if (top || bn_cmp(&r, n) >= 0) bn_sub(&r, n); /* reduce (r was < n, so < 2n after <<1) */
            if ((b->v[i] >> bit) & 1) {                   /* r += a */
                unsigned __int128 carry = 0;
                for (int j = 0; j < RSA_LIMBS; j++) { unsigned __int128 t = (unsigned __int128)r.v[j] + a->v[j] + carry; r.v[j] = (uint64_t)t; carry = t >> 64; }
                if (carry || bn_cmp(&r, n) >= 0) bn_sub(&r, n);
            }
        }
    }
    *result = r;
}
/* out = base^65537 mod n */
static void rsa_pow65537(struct bn* out, const struct bn* base, const struct bn* n) {
    struct bn acc = *base;
    for (int i = 0; i < 16; i++) bn_mulmod(&acc, &acc, &acc, n);   /* base^(2^16) */
    bn_mulmod(out, &acc, base, n);                                  /* * base = base^(2^16+1) */
}
/* Load a big-endian byte array (most-significant first, RSA_LIMBS*8 bytes) into limbs. */
static void bn_from_be(struct bn* a, const uint8_t* be) {
    for (int i = 0; i < RSA_LIMBS; i++) {
        uint64_t limb = 0; const uint8_t* p = be + (RSA_LIMBS - 1 - i) * 8;
        for (int j = 0; j < 8; j++) limb = (limb << 8) | p[j];
        a->v[i] = limb;
    }
}
static void bn_to_be(const struct bn* a, uint8_t* be) {
    for (int i = 0; i < RSA_LIMBS; i++) {
        uint64_t limb = a->v[RSA_LIMBS - 1 - i]; uint8_t* p = be + i * 8;
        for (int j = 0; j < 8; j++) p[j] = (uint8_t)(limb >> (56 - 8*j));
    }
}

/* Verify a 256-byte RSA-2048 PKCS#1 v1.5 (SHA-256) signature over [data,len) against the embedded public key.
 * Public key: modulus_be[256] big-endian, exponent assumed 65537. Returns true on a valid signature. */
static bool rsa_sha256_verify(const uint8_t* data, uint64_t len, const uint8_t* sig256, const uint8_t* modulus_be) {
    struct bn n, s, m;
    bn_from_be(&n, modulus_be);
    bn_from_be(&s, sig256);
    if (bn_cmp(&s, &n) >= 0) return false;
    rsa_pow65537(&m, &s, &n);
    uint8_t em[RSA_LIMBS * 8]; bn_to_be(&m, em);   /* the recovered EM, big-endian, 256 bytes */
    /* EM = 0x00 0x01 0xFF..(>=8).. 0x00 || T, T = DigestInfo(SHA-256) 19 bytes || 32-byte hash */
    static const uint8_t prefix[19] = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    unsigned total = RSA_LIMBS * 8;
    if (em[0] != 0x00 || em[1] != 0x01) return false;
    unsigned i = 2; while (i < total && em[i] == 0xFF) i++;
    if (i < 10 || i >= total || em[i] != 0x00) return false;   /* at least 8 padding bytes, then a separator */
    i++;
    if (total - i != 19 + 32) return false;
    for (unsigned j = 0; j < 19; j++) if (em[i + j] != prefix[j]) return false;
    uint8_t hash[32]; struct sha256 ctx; sha256_init(&ctx); sha256_update(&ctx, data, len); sha256_final(&ctx, hash);
    const uint8_t* want = em + i + 19;
    uint8_t diff = 0; for (unsigned j = 0; j < 32; j++) diff |= (uint8_t)(hash[j] ^ want[j]);
    return diff == 0;
}
#endif
