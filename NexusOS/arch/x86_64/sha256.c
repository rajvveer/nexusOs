/* SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 8018) for dm-crypt ESSIV and LUKS
 * (luks.c: keyslot PBKDF2, the volume-key digest, the anti-forensic splitter). Plain C, no tables indexed by secrets. */
#include "native.h"
#include "kcrypto.h"
#include "../../kernel/string.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }
static void compress(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    crypto_wipe(w, sizeof(w));
}
void sha256_init(struct sha256* s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv)); s->length = 0; s->used = 0;
}
void sha256_update(struct sha256* s, const void* data, uint64_t bytes) {
    const uint8_t* p = data; s->length += bytes;
    while (bytes) {
        if (!s->used && bytes >= 64) { compress(s->h, p); p += 64; bytes -= 64; continue; }
        unsigned n = 64 - s->used; if (n > bytes) n = (unsigned)bytes;
        memcpy(s->block + s->used, p, n); s->used += n; p += n; bytes -= n;
        if (s->used == 64) { compress(s->h, s->block); s->used = 0; }
    }
}
void sha256_final(struct sha256* s, uint8_t out[32]) {
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80; sha256_update(s, &pad, 1); pad = 0;
    while (s->used != 56) sha256_update(s, &pad, 1);
    uint8_t length[8]; for (unsigned i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, length, 8);
    for (unsigned i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
    crypto_wipe(s, sizeof(*s));
}
void sha256(const void* data, uint64_t bytes, uint8_t out[32]) { struct sha256 s; sha256_init(&s); sha256_update(&s, data, bytes); sha256_final(&s, out); }

struct hmac { struct sha256 inner, outer; };
static void hmac_init(struct hmac* m, const uint8_t* key, unsigned key_bytes) {
    uint8_t block[64] = {0};
    if (key_bytes > 64) sha256(key, key_bytes, block); else memcpy(block, key, key_bytes);
    for (unsigned i = 0; i < 64; i++) block[i] ^= 0x36;
    sha256_init(&m->inner); sha256_update(&m->inner, block, 64);
    for (unsigned i = 0; i < 64; i++) block[i] ^= 0x36 ^ 0x5c;
    sha256_init(&m->outer); sha256_update(&m->outer, block, 64);
    crypto_wipe(block, sizeof(block));
}
static void hmac_final(struct hmac* m, uint8_t out[32]) {
    uint8_t inner[32]; sha256_final(&m->inner, inner);
    sha256_update(&m->outer, inner, 32); sha256_final(&m->outer, out); crypto_wipe(inner, sizeof(inner));
}
void hmac_sha256(const uint8_t* key, unsigned key_bytes, const void* data, uint64_t bytes, uint8_t out[32]) {
    struct hmac m; hmac_init(&m, key, key_bytes); sha256_update(&m.inner, data, bytes); hmac_final(&m, out); crypto_wipe(&m, sizeof(m));
}
int pbkdf2_sha256(const uint8_t* password, unsigned password_bytes, const uint8_t* salt, unsigned salt_bytes,
                  uint32_t iterations, uint8_t* out, unsigned out_bytes) {
    if (!iterations || !out_bytes) return -22;
    struct hmac base, m; hmac_init(&base, password, password_bytes);   /* the keyed state, reused for every HMAC */
    uint8_t u[32], t[32];
    for (uint32_t block = 1; out_bytes; block++) {
        uint8_t index[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block};
        m = base; sha256_update(&m.inner, salt, salt_bytes); sha256_update(&m.inner, index, 4); hmac_final(&m, u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; i++) {
            m = base; sha256_update(&m.inner, u, 32); hmac_final(&m, u);
            for (unsigned k = 0; k < 32; k++) t[k] ^= u[k];
        }
        unsigned n = out_bytes < 32 ? out_bytes : 32;
        memcpy(out, t, n); out += n; out_bytes -= n;
    }
    crypto_wipe(&base, sizeof(base)); crypto_wipe(&m, sizeof(m)); crypto_wipe(u, 32); crypto_wipe(t, 32);
    return 0;
}

static bool equal_hex(const uint8_t* data, const char* hex) {
    for (unsigned i = 0; hex[2 * i]; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) { char c = hex[2 * i + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); }
        if (data[i] != v) return false;
    }
    return true;
}
bool sha256_selftest(void) {
    uint8_t out[64]; bool ok = true;
    sha256("abc", 3, out); ok &= equal_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");   /* FIPS 180-4 */
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    ok &= equal_hex(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    uint8_t key[20]; memset(key, 0x0b, 20);                                                                     /* RFC 4231 case 1 */
    hmac_sha256(key, 20, "Hi There", 8, out); ok &= equal_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    pbkdf2_sha256((const uint8_t*)"password", 8, (const uint8_t*)"salt", 4, 1, out, 32);                          /* RFC 7914 / common vectors */
    ok &= equal_hex(out, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256((const uint8_t*)"password", 8, (const uint8_t*)"salt", 4, 4096, out, 32);
    ok &= equal_hex(out, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    pbkdf2_sha256((const uint8_t*)"passwordPASSWORDpassword", 24, (const uint8_t*)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, out, 40);
    ok &= equal_hex(out, "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
    return ok;
}
