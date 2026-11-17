/* Kernel crypto used by dm-crypt (dm.c) and the encrypted-root unlock (luks.c): AES (aes.c: AES-NI or constant-time
 * software), the dm-crypt cipher modes, SHA-256/HMAC/PBKDF2 (sha256.c), BLAKE2b and Argon2 (argon2.c). */
#ifndef NEXUS_KCRYPTO_H
#define NEXUS_KCRYPTO_H
#include "../../kernel/types.h"

struct aes_key { uint8_t enc[240], dec[240]; unsigned rounds; };
/* A dm-crypt cipher instance (the name says "xts" for history: it holds every mode). */
enum { CIPHER_XTS_PLAIN64 = 1, CIPHER_XTS_PLAIN, CIPHER_CBC_ESSIV_SHA256, CIPHER_CBC_PLAIN64, CIPHER_CBC_PLAIN };
struct xts_key { struct aes_key data, tweak; int mode; };

int  aes_set_key(struct aes_key* k, const uint8_t* key, unsigned bytes);
void aes_encrypt_block(const struct aes_key* k, uint8_t* block);
void aes_decrypt_block(const struct aes_key* k, uint8_t* block);
bool aes_hardware(void);
int  cipher_mode(const char* name);                       /* "aes-xts-plain64" ... -> CIPHER_*, 0: unsupported */
const char* cipher_name(int mode);
int  cipher_set_key(struct xts_key* x, int mode, const uint8_t* key, unsigned bytes);   /* 0 or -22 */
int  xts_set_key(struct xts_key* x, const uint8_t* key, unsigned bytes);               /* aes-xts-plain64 */
void xts_wipe(struct xts_key* x);
/* One data unit (512..4096 bytes, a multiple of 16) with IV sector number `iv` (already offset/shifted by dm-crypt). */
void xts_crypt(const struct xts_key* x, uint8_t* data, unsigned bytes, uint64_t iv, bool decrypt);
bool aes_selftest(void);
uint64_t aes_throughput_mbs(int mode, bool decrypt);       /* boot benchmark (MB/s on this CPU) */

struct sha256 { uint32_t h[8]; uint64_t length; uint8_t block[64]; unsigned used; };
void sha256_init(struct sha256* s);
void sha256_update(struct sha256* s, const void* data, uint64_t bytes);
void sha256_final(struct sha256* s, uint8_t out[32]);
void sha256(const void* data, uint64_t bytes, uint8_t out[32]);
void hmac_sha256(const uint8_t* key, unsigned key_bytes, const void* data, uint64_t bytes, uint8_t out[32]);
int  pbkdf2_sha256(const uint8_t* password, unsigned password_bytes, const uint8_t* salt, unsigned salt_bytes,
                   uint32_t iterations, uint8_t* out, unsigned out_bytes);
bool sha256_selftest(void);

/* Argon2 (RFC 9106): type 0 Argon2d, 1 Argon2i, 2 Argon2id; memory in KiB; 0, -12 (memory), -22 (parameters). */
int  argon2_hash(int type, const uint8_t* password, unsigned password_bytes, const uint8_t* salt, unsigned salt_bytes,
                 uint32_t time_cost, uint32_t memory_kib, uint32_t lanes, uint8_t* out, unsigned out_bytes);
void blake2b(const void* data, uint64_t bytes, const uint8_t* key, unsigned key_bytes, uint8_t* out, unsigned out_bytes);
bool argon2_selftest(void);

static inline void crypto_wipe(void* p, uint64_t n) { volatile uint8_t* v = (volatile uint8_t*)p; while (n--) *v++ = 0; }
#endif
