/* ============================================================================
 * NexusOS — Big integers + public-key verification (Phase 57)
 * ============================================================================
 * 32-bit-limb Montgomery arithmetic (up to 4096 bits) used for:
 *   - RSA signature verification (PKCS#1 v1.5 and RSASSA-PSS), 1024-4096 bit
 *   - ECDSA verification on NIST P-256 and P-384 (Jacobian coordinates)
 *   - X25519 key agreement (RFC 7748, Montgomery ladder over 2^255-19)
 * Everything is integer-only; the only 64-bit operations are 32x32->64
 * products and adds, which GCC inlines on i386 (no libgcc helpers).
 * ============================================================================ */
#ifndef BIGNUM_H
#define BIGNUM_H

#include "types.h"

#define BN_MAX_LIMBS 128            /* 4096 bits */

/* Montgomery context for an odd modulus */
typedef struct {
    int      n;                     /* limbs in use */
    uint32_t m[BN_MAX_LIMBS];       /* modulus */
    uint32_t rr[BN_MAX_LIMBS];      /* R^2 mod m (R = 2^(32n)) */
    uint32_t n0;                    /* -m^-1 mod 2^32 */
} mont_ctx_t;

/* Core */
int  bn_from_bytes(uint32_t* r, int n, const uint8_t* be, uint32_t len); /* 0 ok, -1 too big */
void bn_to_bytes(const uint32_t* a, int n, uint8_t* out, uint32_t len);  /* big-endian, zero-padded */
int  bn_cmp(const uint32_t* a, const uint32_t* b, int n);
bool bn_is_zero(const uint32_t* a, int n);
int  mont_init(mont_ctx_t* c, const uint8_t* mod_be, uint32_t len);     /* 0 ok */
void mont_mul(uint32_t* r, const uint32_t* a, const uint32_t* b, const mont_ctx_t* c);
void mont_exp(uint32_t* r, const uint32_t* base, const uint32_t* exp, int exp_limbs, const mont_ctx_t* c);

/* ---- RSA ----
 * Verify `sig` (modulus-length bytes) over the message digest `hash` (hash_id
 * from crypto.h). pss=false: PKCS#1 v1.5 with DigestInfo; pss=true: RSASSA-PSS
 * with MGF1 of the same hash and salt length == digest length.
 * Returns 0 if valid, -1 if invalid, -2 if unsupported/too large. */
int rsa_verify(const uint8_t* n, uint32_t nlen, const uint8_t* e, uint32_t elen,
               const uint8_t* sig, uint32_t siglen, int hash_id, const uint8_t* hash, bool pss);

/* ---- ECDSA ----
 * curve: EC_P256 or EC_P384. point = 0x04 || X || Y (65 or 97 bytes).
 * r/s are big-endian integers (any length <= curve size after stripping zeros).
 * Returns 0 valid, -1 invalid, -2 unsupported. */
#define EC_P256 1
#define EC_P384 2
int ec_point_len(int curve);        /* 65 / 97 */
int ecdsa_verify(int curve, const uint8_t* point, uint32_t plen,
                 const uint8_t* hash, uint32_t hlen,
                 const uint8_t* r, uint32_t rlen, const uint8_t* s, uint32_t slen);

/* ---- X25519 ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

#endif /* BIGNUM_H */
