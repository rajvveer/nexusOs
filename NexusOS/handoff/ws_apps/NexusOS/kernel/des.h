/* ============================================================================
 * NexusOS — DES block cipher (Header) — Phase 55
 * ============================================================================
 * A compact, table-driven FIPS 46 DES: single 64-bit block, ECB, 16 rounds,
 * encrypt and decrypt (decrypt = the same rounds with the subkeys reversed).
 * Integer-only (bit arrays + bytes), no 64-bit arithmetic, no tables beyond
 * the standard ~840 bytes, so it fits the freestanding kernel.
 *
 * Purpose: RFB "VNC Authentication" (security type 2) is a DES challenge/
 * response, and the classic VNC password file is DES-obfuscated. DES is
 * NOT a modern cipher — it is used here only because the VNC protocol
 * mandates it.
 * ============================================================================ */

#ifndef DES_H
#define DES_H

#include "types.h"

/* Expanded key schedule: 16 round subkeys of 48 bits (stored one bit per
 * byte for simplicity — the cipher runs a handful of times per login). */
typedef struct {
    uint8_t sub[16][48];
} des_ctx_t;

/* Expand an 8-byte key (parity bits ignored, as in every DES). */
void des_set_key(des_ctx_t* ctx, const uint8_t key[8]);

/* Encrypt / decrypt one 8-byte block (in and out may alias). */
void des_encrypt_block(const des_ctx_t* ctx, const uint8_t in[8], uint8_t out[8]);
void des_decrypt_block(const des_ctx_t* ctx, const uint8_t in[8], uint8_t out[8]);

/* Reverse the bit order of one byte (0x01 -> 0x80). VNC feeds its DES key
 * through this quirk: each password byte is bit-reversed before use. */
uint8_t des_bitrev8(uint8_t b);

/* FIPS 46 test vector: key 133457799BBCDFF1, plaintext 0123456789ABCDEF ->
 * ciphertext 85E813540F0AB405 (and back). Returns true on PASS. */
bool des_selftest(void);

/* Shell: `des selftest` prints PASS/FAIL with the bytes seen. */
void des_cmd(int argc, char* argv[]);

#endif /* DES_H */
