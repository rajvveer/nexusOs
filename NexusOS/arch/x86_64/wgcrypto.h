#ifndef NEXUS_WGCRYPTO_H
#define NEXUS_WGCRYPTO_H
#include "../../kernel/types.h"
struct blake2s_state { uint32_t h[8], t[2], f; uint8_t buffer[64]; unsigned buffered, out_length; };
void blake2s_init(struct blake2s_state* state, unsigned out_length, const void* key, unsigned key_length);
void blake2s_update(struct blake2s_state* state, const void* data, unsigned length);
void blake2s_final(struct blake2s_state* state, uint8_t* out);
void blake2s(uint8_t* out, unsigned out_length, const void* key, unsigned key_length, const void* data, unsigned length);
void hmac_blake2s(uint8_t out[32], const void* key, unsigned key_length, const void* data, unsigned length);
void chacha20_xor(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]);
void hchacha20(uint8_t out[32], const uint8_t key[32], const uint8_t nonce[16]);
void poly1305(uint8_t tag[16], const uint8_t* message, unsigned length, const uint8_t key[32]);
/* WireGuard AEAD: 96-bit nonce = 32 zero bits + little-endian counter; out = ciphertext || 16-byte tag. */
void chacha20poly1305_encrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, uint64_t counter, const uint8_t key[32]);
bool chacha20poly1305_decrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, uint64_t counter, const uint8_t key[32]);
void xchacha20poly1305_encrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, const uint8_t nonce[24], const uint8_t key[32]);
bool xchacha20poly1305_decrypt(uint8_t* out, const uint8_t* in, unsigned length, const uint8_t* ad, unsigned ad_length, const uint8_t nonce[24], const uint8_t key[32]);
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_public(uint8_t out[32], const uint8_t secret[32]);
void wg_memzero(void* data, unsigned length);
bool wg_equal(const void* a, const void* b, unsigned length);
bool wgcrypto_selftest(void);
#endif
