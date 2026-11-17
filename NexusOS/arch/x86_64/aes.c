/* AES (FIPS-197) and the dm-crypt cipher modes: aes-xts-plain64 / aes-xts-plain (IEEE 1619, NIST SP 800-38E),
 * aes-cbc-essiv:sha256, aes-cbc-plain64 and aes-cbc-plain (NIST SP 800-38A CBC with the dm-crypt IV generators).
 *
 * With AES-NI (CPUID.1:ECX.25) the block work runs in aes_ni.asm: eight blocks in flight for XTS and CBC decryption
 * (the AES rounds pipeline), one for CBC encryption (inherently serial); each routine saves and restores the XMM
 * registers it uses (see aes_ni.asm for why that is the whole FPU-state contract here). Without it a constant-time
 * software AES is used: no table lookups indexed by secret data (S-box values come from GF(2^8) inversion by
 * fixed-schedule multiplication and the affine map; MixColumns with masked xtime), so timing does not depend on keys
 * or data; it is slow (~1 MB/s) and only meant for CPUs without AES-NI (the boot option noaesni forces it, for tests).
 * aes_selftest() checks FIPS-197, SP 800-38A CBC, IEEE 1619 XTS and ESSIV known answers on both paths, compares the
 * two paths on pseudo-random data units of every mode and key size, and measures the throughput at boot. Key schedules
 * are wiped by xts_wipe(). */
#include "native.h"
#include "boot_info.h"
#include "kcrypto.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

void aesni_ecb1(const uint8_t* round_keys, unsigned rounds, uint8_t* block, int decrypt);
void aesni_xts8(const uint8_t* round_keys, unsigned rounds, uint8_t* data, uint64_t blocks, uint8_t* tweak, int decrypt);
void aesni_cbc_encrypt(const uint8_t* round_keys, unsigned rounds, uint8_t* data, uint64_t blocks, uint8_t* iv);
void aesni_cbc_decrypt8(const uint8_t* dec_keys, unsigned rounds, uint8_t* data, uint64_t blocks, uint8_t* iv);

static int have_aesni = -1;
static bool force_software;   /* selftest: the software path on an AES-NI CPU */
static bool aesni(void) {
    if (have_aesni < 0) {
        uint32_t a = 1, b, c, d; __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        uint64_t cr4; __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        have_aesni = ((c >> 25) & 1) && (cr4 & (1u << 9)) && !boot_option("noaesni");   /* AES-NI and OSFXSR; noaesni forces software */
    }
    return have_aesni == 1 && !force_software;
}
bool aes_hardware(void) { return aesni(); }

/* ---- constant-time GF(2^8) arithmetic ---- */
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (unsigned i = 0; i < 8; i++) {
        p ^= (uint8_t)(-(b & 1) & a);
        uint8_t high = (uint8_t)-(a >> 7);
        a = (uint8_t)((a << 1) ^ (high & 0x1B)); b >>= 1;
    }
    return p;
}
static uint8_t ginv(uint8_t x) {   /* x^254 (0 -> 0) */
    uint8_t x2 = gmul(x, x), x3 = gmul(x2, x), x6 = gmul(x3, x3), x12 = gmul(x6, x6), x15 = gmul(x12, x3);
    uint8_t x30 = gmul(x15, x15), x60 = gmul(x30, x30), x120 = gmul(x60, x60), x126 = gmul(x120, x6);
    uint8_t x252 = gmul(x126, x126); return gmul(x252, x2);
}
static uint8_t rotl8(uint8_t x, unsigned n) { return (uint8_t)(x << n | x >> (8 - n)); }
static uint8_t sbox(uint8_t x) { uint8_t b = ginv(x); return (uint8_t)(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63); }
static uint8_t inv_sbox(uint8_t x) { return ginv((uint8_t)(rotl8(x, 1) ^ rotl8(x, 3) ^ rotl8(x, 6) ^ 0x05)); }
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (-(x >> 7) & 0x1B)); }
static void inv_mix_column(uint8_t* c) {
    uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
    c[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
    c[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
    c[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
    c[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
}

int aes_set_key(struct aes_key* k, const uint8_t* key, unsigned bytes) {
    if (bytes != 16 && bytes != 24 && bytes != 32) return -22;
    unsigned nk = bytes / 4, nr = nk + 6, words = 4 * (nr + 1);
    uint8_t* w = k->enc; memcpy(w, key, bytes);
    uint8_t rcon = 1;
    for (unsigned i = nk; i < words; i++) {
        uint8_t t[4]; memcpy(t, w + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t first = t[0]; t[0] = (uint8_t)(sbox(t[1]) ^ rcon); t[1] = sbox(t[2]); t[2] = sbox(t[3]); t[3] = sbox(first);
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) for (unsigned j = 0; j < 4; j++) t[j] = sbox(t[j]);
        for (unsigned j = 0; j < 4; j++) w[4 * i + j] = (uint8_t)(w[4 * (i - nk) + j] ^ t[j]);
    }
    k->rounds = nr;
    memcpy(k->dec, k->enc + 16 * nr, 16);   /* the equivalent inverse cipher's schedule, in use order (AES-NI) */
    for (unsigned i = 1; i < nr; i++) { memcpy(k->dec + 16 * i, k->enc + 16 * (nr - i), 16); for (unsigned c = 0; c < 4; c++) inv_mix_column(k->dec + 16 * i + 4 * c); }
    memcpy(k->dec + 16 * nr, k->enc, 16);
    return 0;
}
static void add_round_key(uint8_t* s, const uint8_t* k) { for (unsigned i = 0; i < 16; i++) s[i] ^= k[i]; }
static void sw_encrypt(const struct aes_key* k, uint8_t* s) {
    add_round_key(s, k->enc);
    for (unsigned round = 1; round <= k->rounds; round++) {
        for (unsigned i = 0; i < 16; i++) s[i] = sbox(s[i]);
        uint8_t t[16];   /* ShiftRows: row r (bytes r, r+4, r+8, r+12) rotates left by r */
        for (unsigned c = 0; c < 4; c++) for (unsigned r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
        memcpy(s, t, 16);
        if (round != k->rounds) for (unsigned c = 0; c < 4; c++) {
            uint8_t* col = s + 4 * c; uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
            col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1))); col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
            col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3))); col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
        }
        add_round_key(s, k->enc + 16 * round);
    }
}
static void sw_decrypt(const struct aes_key* k, uint8_t* s) {
    add_round_key(s, k->enc + 16 * k->rounds);
    for (unsigned round = k->rounds; round-- > 0; ) {
        uint8_t t[16];   /* InvShiftRows */
        for (unsigned c = 0; c < 4; c++) for (unsigned r = 0; r < 4; r++) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
        for (unsigned i = 0; i < 16; i++) s[i] = inv_sbox(t[i]);
        add_round_key(s, k->enc + 16 * round);
        if (round) for (unsigned c = 0; c < 4; c++) inv_mix_column(s + 4 * c);
    }
}
void aes_encrypt_block(const struct aes_key* k, uint8_t* block) {
    if (aesni()) aesni_ecb1(k->enc, k->rounds, block, 0); else sw_encrypt(k, block);
}
void aes_decrypt_block(const struct aes_key* k, uint8_t* block) {
    if (aesni()) aesni_ecb1(k->dec, k->rounds, block, 1); else sw_decrypt(k, block);
}

/* ---- modes ---- */
static const struct { const char* name; int mode; } ciphers[] = {
    {"aes-xts-plain64", CIPHER_XTS_PLAIN64}, {"capi:xts(aes)-plain64", CIPHER_XTS_PLAIN64}, {"aes-xts-plain", CIPHER_XTS_PLAIN},
    {"aes-cbc-essiv:sha256", CIPHER_CBC_ESSIV_SHA256}, {"capi:cbc(aes)-essiv:sha256", CIPHER_CBC_ESSIV_SHA256},
    {"aes-cbc-plain64", CIPHER_CBC_PLAIN64}, {"capi:cbc(aes)-plain64", CIPHER_CBC_PLAIN64}, {"aes-cbc-plain", CIPHER_CBC_PLAIN}};
int cipher_mode(const char* name) {
    for (unsigned i = 0; i < sizeof(ciphers) / sizeof(ciphers[0]); i++) if (!strcmp(name, ciphers[i].name)) return ciphers[i].mode;
    return 0;
}
const char* cipher_name(int mode) {
    for (unsigned i = 0; i < sizeof(ciphers) / sizeof(ciphers[0]); i++) if (ciphers[i].mode == mode) return ciphers[i].name;
    return "?";
}
int cipher_set_key(struct xts_key* x, int mode, const uint8_t* key, unsigned bytes) {
    memset(x, 0, sizeof(*x)); x->mode = mode;
    if (mode == CIPHER_XTS_PLAIN64 || mode == CIPHER_XTS_PLAIN) {   /* key1 || key2; key1 == key2 allowed (Linux outside FIPS mode) */
        if (bytes != 32 && bytes != 48 && bytes != 64) return -22;
        int e = aes_set_key(&x->data, key, bytes / 2); if (e) return e;
        return aes_set_key(&x->tweak, key + bytes / 2, bytes / 2);
    }
    if (mode != CIPHER_CBC_ESSIV_SHA256 && mode != CIPHER_CBC_PLAIN64 && mode != CIPHER_CBC_PLAIN) return -22;
    int e = aes_set_key(&x->data, key, bytes); if (e) return e;
    if (mode == CIPHER_CBC_ESSIV_SHA256) {   /* ESSIV: IV = E_{SHA-256(key)}(sector) */
        uint8_t salt[32]; sha256(key, bytes, salt); e = aes_set_key(&x->tweak, salt, 32); crypto_wipe(salt, sizeof(salt));
    }
    return e;
}
int xts_set_key(struct xts_key* x, const uint8_t* key, unsigned bytes) { return cipher_set_key(x, CIPHER_XTS_PLAIN64, key, bytes); }
void xts_wipe(struct xts_key* x) { crypto_wipe(x, sizeof(*x)); }
static void gf_double(uint8_t* t) {   /* T * alpha, little-endian 128-bit, constant time */
    uint8_t carry = (uint8_t)(t[15] >> 7);
    for (int i = 15; i > 0; i--) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
    t[0] = (uint8_t)((t[0] << 1) ^ (-carry & 0x87));
}
static void iv_block(const struct xts_key* x, uint64_t iv, uint8_t out[16]) {   /* the 16-byte IV of one data unit */
    memset(out, 0, 16);
    put_le64(out, x->mode == CIPHER_XTS_PLAIN || x->mode == CIPHER_CBC_PLAIN ? (iv & 0xFFFFFFFFULL) : iv);
    if (x->mode == CIPHER_XTS_PLAIN64 || x->mode == CIPHER_XTS_PLAIN || x->mode == CIPHER_CBC_ESSIV_SHA256) aes_encrypt_block(&x->tweak, out);
}
static void crypt_sw(const struct xts_key* x, uint8_t* data, unsigned bytes, uint64_t iv, bool decrypt) {
    uint8_t t[16]; iv_block(x, iv, t);
    if (x->mode == CIPHER_XTS_PLAIN64 || x->mode == CIPHER_XTS_PLAIN) {
        for (unsigned at = 0; at + 16 <= bytes; at += 16) {
            uint8_t* b = data + at;
            for (unsigned i = 0; i < 16; i++) b[i] ^= t[i];
            if (decrypt) sw_decrypt(&x->data, b); else sw_encrypt(&x->data, b);
            for (unsigned i = 0; i < 16; i++) b[i] ^= t[i];
            gf_double(t);
        }
    } else if (!decrypt) {
        for (unsigned at = 0; at + 16 <= bytes; at += 16) {
            uint8_t* b = data + at; for (unsigned i = 0; i < 16; i++) b[i] ^= t[i];
            sw_encrypt(&x->data, b); memcpy(t, b, 16);
        }
    } else {
        uint8_t c[16];
        for (unsigned at = 0; at + 16 <= bytes; at += 16) {
            uint8_t* b = data + at; memcpy(c, b, 16);
            sw_decrypt(&x->data, b); for (unsigned i = 0; i < 16; i++) b[i] ^= t[i];
            memcpy(t, c, 16);
        }
        crypto_wipe(c, sizeof(c));
    }
    crypto_wipe(t, sizeof(t));
}
/* One data unit (512..4096 bytes, a multiple of 16) with the IV sector number `iv`. */
void xts_crypt(const struct xts_key* x, uint8_t* data, unsigned bytes, uint64_t iv, bool decrypt) {
    if (!aesni()) { crypt_sw(x, data, bytes, iv, decrypt); return; }
    uint8_t t[16]; iv_block(x, iv, t);
    if (x->mode == CIPHER_XTS_PLAIN64 || x->mode == CIPHER_XTS_PLAIN)
        aesni_xts8(decrypt ? x->data.dec : x->data.enc, x->data.rounds, data, bytes / 16, t, decrypt);
    else if (decrypt) aesni_cbc_decrypt8(x->data.dec, x->data.rounds, data, bytes / 16, t);
    else aesni_cbc_encrypt(x->data.enc, x->data.rounds, data, bytes / 16, t);
    crypto_wipe(t, sizeof(t));
}

/* ---- known answers, path comparison, throughput ---- */
static bool hex_equal(const uint8_t* data, const char* hex) {
    for (unsigned i = 0; hex[2 * i]; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) { char c = hex[2 * i + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); }
        if (data[i] != v) return false;
    }
    return true;
}
static void from_hex(uint8_t* out, const char* hex) {
    for (unsigned i = 0; hex[2 * i]; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) { char c = hex[2 * i + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); }
        out[i] = (uint8_t)v;
    }
}
static bool known_answers(struct xts_key* x, uint8_t* buffer) {
    struct aes_key k; uint8_t key[64], block[16]; bool ok = true;
    for (unsigned i = 0; i < 32; i++) key[i] = (uint8_t)i;
    for (unsigned i = 0; i < 16; i++) block[i] = (uint8_t)(i * 0x11);
    aes_set_key(&k, key, 16); aes_encrypt_block(&k, block); ok &= hex_equal(block, "69c4e0d86a7b0430d8cdb78070b4c55a");   /* FIPS-197 C.1 */
    aes_decrypt_block(&k, block); for (unsigned i = 0; i < 16; i++) ok &= block[i] == (uint8_t)(i * 0x11);
    aes_set_key(&k, key, 24); aes_encrypt_block(&k, block); ok &= hex_equal(block, "dda97ca4864cdfe06eaf70a0ec0d7191");   /* C.2 */
    for (unsigned i = 0; i < 16; i++) block[i] = (uint8_t)(i * 0x11);
    aes_set_key(&k, key, 32); aes_encrypt_block(&k, block); ok &= hex_equal(block, "8ea2b7ca516745bfeafc49904b496089");   /* C.3 */
    aes_decrypt_block(&k, block); for (unsigned i = 0; i < 16; i++) ok &= block[i] == (uint8_t)(i * 0x11);
    /* SP 800-38A F.2.1/F.2.5 CBC-AES128/256 with IV 000102..0f: aes-cbc-plain64 on sector 0x0f0e0d0c0b0a0908 0706050403020100
     * would need a 128-bit IV, so the plain64 IV of sector 0x0706050403020100 is used with the upper half forced through
     * a 32-byte unit whose first block is the SP vector XORed with the IV difference (computed here). */
    from_hex(key, "2b7e151628aed2a6abf7158809cf4f3c");
    ok &= cipher_set_key(x, CIPHER_CBC_PLAIN64, key, 16) == 0;
    from_hex(buffer, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    for (unsigned i = 8; i < 16; i++) buffer[i] ^= (uint8_t)i;   /* the SP IV's upper half 08..0f folded into the plaintext */
    xts_crypt(x, buffer, 32, 0x0706050403020100ULL, false);
    ok &= hex_equal(buffer, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2");
    xts_crypt(x, buffer, 32, 0x0706050403020100ULL, true);
    for (unsigned i = 8; i < 16; i++) buffer[i] ^= (uint8_t)i;
    ok &= hex_equal(buffer, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    from_hex(key, "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    ok &= cipher_set_key(x, CIPHER_CBC_PLAIN64, key, 32) == 0;
    from_hex(buffer, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    for (unsigned i = 8; i < 16; i++) buffer[i] ^= (uint8_t)i;
    xts_crypt(x, buffer, 32, 0x0706050403020100ULL, false);
    ok &= hex_equal(buffer, "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d");
    /* IEEE 1619-2007 XTS-AES-128 vectors 1 and 2 (32-byte data units) */
    memset(key, 0, 32); x->mode = CIPHER_XTS_PLAIN64;
    aes_set_key(&x->data, key, 16); aes_set_key(&x->tweak, key + 16, 16);   /* vector 1: key1 = key2 = 0 */
    memset(buffer, 0, 32); xts_crypt(x, buffer, 32, 0, false);
    ok &= hex_equal(buffer, "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e");
    memset(key, 0x11, 16); memset(key + 16, 0x22, 16); ok &= xts_set_key(x, key, 32) == 0;
    memset(buffer, 0x44, 32); xts_crypt(x, buffer, 32, 0x3333333333ULL, false);
    ok &= hex_equal(buffer, "c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0");
    xts_crypt(x, buffer, 32, 0x3333333333ULL, true); for (unsigned i = 0; i < 32; i++) ok &= buffer[i] == 0x44;
    /* ESSIV: the IV of sector 5 is AES-256_{SHA-256(key)}(5 LE), so CBC-ESSIV equals CBC-plain64 with that IV block */
    for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(0xA0 + i);
    ok &= cipher_set_key(x, CIPHER_CBC_ESSIV_SHA256, key, 16) == 0;
    uint8_t salt[32], ivb[16] = {5}; sha256(key, 16, salt); aes_set_key(&k, salt, 32); aes_encrypt_block(&k, ivb);
    memset(buffer, 0x5C, 64); xts_crypt(x, buffer, 64, 5, false);
    struct aes_key data; aes_set_key(&data, key, 16);
    uint8_t expect[64]; memset(expect, 0x5C, 64);
    for (unsigned b = 0; b < 4; b++) { for (unsigned i = 0; i < 16; i++) expect[16 * b + i] ^= b ? expect[16 * b - 16 + i] : ivb[i]; aes_encrypt_block(&data, expect + 16 * b); }
    ok &= !memcmp(buffer, expect, 64);
    crypto_wipe(&k, sizeof(k)); crypto_wipe(&data, sizeof(data)); crypto_wipe(salt, sizeof(salt));
    return ok;
}
bool aes_selftest(void) {
    struct xts_key* x = (void*)arena_alloc(sizeof(struct xts_key)); uint8_t* buffer = (void*)arena_alloc(8192), *copy = buffer + 4096;
    if (!x || !buffer) return false;
    bool hardware = aesni(), ok = sha256_selftest();
    force_software = true; ok &= known_answers(x, buffer); force_software = false;
    if (hardware) ok &= known_answers(x, buffer);
    /* both paths agree, and decryption inverts encryption, on 4 KiB / 512-byte units, every mode and key size */
    static const int modes[5] = {CIPHER_XTS_PLAIN64, CIPHER_XTS_PLAIN, CIPHER_CBC_ESSIV_SHA256, CIPHER_CBC_PLAIN64, CIPHER_CBC_PLAIN};
    uint64_t seed = 0x243F6A8885A308D3ULL; uint8_t key[64];
    for (unsigned m = 0; m < 5 && ok; m++) for (unsigned size = 0; size < 2 && ok; size++) {
        bool xts = modes[m] <= CIPHER_XTS_PLAIN; unsigned bytes = xts ? (size ? 64 : 32) : (size ? 32 : 16), unit = size ? 4096 : 512 + 16 * m;
        for (unsigned i = 0; i < bytes; i++) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; key[i] = (uint8_t)(seed >> 56); }
        ok &= cipher_set_key(x, modes[m], key, bytes) == 0;
        for (unsigned i = 0; i < unit; i++) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; buffer[i] = copy[i] = (uint8_t)(seed >> 56); }
        uint64_t iv = 0x100000077ULL + m;
        force_software = true; xts_crypt(x, copy, unit, iv, false); force_software = false;
        xts_crypt(x, buffer, unit, iv, false); ok &= !memcmp(buffer, copy, unit);
        xts_crypt(x, buffer, unit, iv, true);
        force_software = true; xts_crypt(x, copy, unit, iv, true); force_software = false;
        ok &= !memcmp(buffer, copy, unit);
    }
    crypto_wipe(key, sizeof(key)); xts_wipe(x); arena_free((uint64_t)x, sizeof(struct xts_key)); arena_free((uint64_t)buffer, 8192);
    vga_print(ok ? "[64] PASS AES/XTS known answers (FIPS-197, IEEE 1619), " : "[64] AES SELFTEST FAILED, ");
    vga_print(hardware ? "AES-NI matches the constant-time software path\n" : "constant-time software AES (no AES-NI)\n");
    if (ok) {
        vga_print("[64] AES throughput MB/s: xts-plain64 encrypt "); char n[24] = ""; append_u64(n, aes_throughput_mbs(CIPHER_XTS_PLAIN64, false)); vga_print(n);
        n[0] = 0; append_u64(n, aes_throughput_mbs(CIPHER_XTS_PLAIN64, true)); vga_print(" decrypt "); vga_print(n);
        n[0] = 0; append_u64(n, aes_throughput_mbs(CIPHER_CBC_ESSIV_SHA256, false)); vga_print(", cbc-essiv encrypt "); vga_print(n);
        n[0] = 0; append_u64(n, aes_throughput_mbs(CIPHER_CBC_ESSIV_SHA256, true)); vga_print(" decrypt "); vga_print(n); vga_print("\n");
    }
    return ok;
}
uint64_t aes_throughput_mbs(int mode, bool decrypt) {   /* 4 KiB units: up to 16 MiB or 5 ms of wall time */
    struct xts_key* x = (void*)arena_alloc(sizeof(struct xts_key)); uint8_t* buffer = (void*)arena_alloc(65536);
    if (!x || !buffer) { if (x) arena_free((uint64_t)x, sizeof(*x)); if (buffer) arena_free((uint64_t)buffer, 65536); return 0; }
    uint8_t key[64]; for (unsigned i = 0; i < 64; i++) key[i] = (uint8_t)(i * 7 + 1);
    cipher_set_key(x, mode, key, mode <= CIPHER_XTS_PLAIN ? 64 : 32); memset(buffer, 0x3C, 65536);
    uint64_t total = 0, start = native_clock_ns(), ns = 0;
    while (total < (16ULL << 20) && ns < 5000000ULL) {
        for (unsigned k = 0; k < 16; k++, total += 4096) xts_crypt(x, buffer + total % 65536, 4096, total / 512, decrypt);
        ns = native_clock_ns() - start;
    }
    xts_wipe(x); arena_free((uint64_t)x, sizeof(*x)); crypto_wipe(buffer, 65536); arena_free((uint64_t)buffer, 65536); crypto_wipe(key, sizeof(key));
    return ns ? total * 1000000000ULL / ns / 1000000ULL : 0;
}
