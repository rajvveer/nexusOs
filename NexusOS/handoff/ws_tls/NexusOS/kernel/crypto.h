/* ============================================================================
 * NexusOS — Symmetric crypto primitives (Phase 57)
 * ============================================================================
 * SHA-256, SHA-384/512, HMAC-SHA256, HKDF (RFC 5869 + TLS 1.3 Expand-Label),
 * AES-128 and AES-128-GCM. Integer-only, no tables bigger than 4 KB,
 * written for -O2. Used by tls.c, x509.c and bignum.c (RSA-PSS MGF1).
 * ============================================================================ */
#ifndef CRYPTO_H
#define CRYPTO_H

#include "types.h"

/* ---- hash identifiers (shared with x509/bignum) ---- */
#define HASH_NONE    0
#define HASH_SHA256  1
#define HASH_SHA384  2
#define HASH_SHA512  3

typedef struct {
    uint32_t h[8];
    uint32_t len_lo, len_hi;     /* total bytes */
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_ctx_t;

typedef struct {
    uint64_t h[8];
    uint32_t len_lo, len_hi;
    uint8_t  buf[128];
    uint32_t buflen;
    int      outlen;             /* 48 (SHA-384) or 64 (SHA-512) */
} sha512_ctx_t;

void sha256_init(sha256_ctx_t* c);
void sha256_update(sha256_ctx_t* c, const void* data, uint32_t len);
void sha256_final(sha256_ctx_t* c, uint8_t out[32]);
void sha256(const void* data, uint32_t len, uint8_t out[32]);

void sha384_init(sha512_ctx_t* c);
void sha512_init(sha512_ctx_t* c);
void sha512_update(sha512_ctx_t* c, const void* data, uint32_t len);
void sha512_final(sha512_ctx_t* c, uint8_t* out);   /* writes c->outlen bytes */

/* Generic one-shot hash by id. Returns the digest length (0 = unknown id). */
int  hash_len(int id);
int  hash_compute(int id, const void* data, uint32_t len, uint8_t* out);
/* Hash of two concatenated buffers (saves a copy for signature checks). */
int  hash_compute2(int id, const void* a, uint32_t alen, const void* b, uint32_t blen, uint8_t* out);

void hmac_sha256(const uint8_t* key, uint32_t klen, const void* data, uint32_t len, uint8_t out[32]);
void hkdf_extract(const uint8_t* salt, uint32_t slen, const uint8_t* ikm, uint32_t ilen, uint8_t prk[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t* info, uint32_t ilen, uint8_t* out, uint32_t olen);
/* TLS 1.3 HKDF-Expand-Label(secret, "tls13 " + label, context, olen) */
void hkdf_expand_label(const uint8_t secret[32], const char* label,
                       const uint8_t* ctx, uint32_t clen, uint8_t* out, uint32_t olen);

/* ---- AES-128 ---- */
typedef struct { uint32_t rk[44]; } aes128_ctx_t;
void aes128_set_key(aes128_ctx_t* c, const uint8_t key[16]);
void aes128_encrypt_block(const aes128_ctx_t* c, const uint8_t in[16], uint8_t out[16]);

/* ---- AES-128-GCM (12-byte nonce, 16-byte tag) ---- */
typedef struct {
    aes128_ctx_t aes;
    uint64_t hh[16], hl[16];     /* 4-bit GHASH tables */
} gcm_ctx_t;
void gcm_set_key(gcm_ctx_t* g, const uint8_t key[16]);
void gcm_encrypt(const gcm_ctx_t* g, const uint8_t iv[12], const uint8_t* aad, uint32_t aadlen,
                 const uint8_t* in, uint8_t* out, uint32_t len, uint8_t tag[16]);
/* in/out may alias. Returns 0 on success, -1 on tag mismatch (out is still written). */
int  gcm_decrypt(const gcm_ctx_t* g, const uint8_t iv[12], const uint8_t* aad, uint32_t aadlen,
                 const uint8_t* in, uint8_t* out, uint32_t len, const uint8_t tag[16]);

/* ---- misc ---- */
/* Pseudo-random bytes: SHA-256 of a running counter mixed with rdtsc, the PIT
 * tick count and the RTC. Not a cryptographic RNG in the strict sense (there
 * is no hardware entropy source on this machine model) — documented limitation. */
void crypto_random(uint8_t* out, uint32_t len);
/* Constant-time compare: 0 if equal */
int  crypto_memeq(const void* a, const void* b, uint32_t len);
/* CRC-32 (IEEE, reflected), incremental: start with 0xFFFFFFFF, finish with ~crc */
uint32_t crc32_update(uint32_t crc, const void* data, uint32_t len);

#endif /* CRYPTO_H */
