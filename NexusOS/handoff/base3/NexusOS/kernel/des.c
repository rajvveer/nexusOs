/* ============================================================================
 * NexusOS — DES block cipher — Phase 55
 * ============================================================================
 * Straight FIPS 46 DES on bit arrays (one bit per byte). Deliberately simple
 * rather than fast: it runs twice per VNC login. All tables are the standard
 * published ones; the self-test checks the FIPS 46 example vector.
 * ============================================================================ */

#include "des.h"
#include "string.h"
#include "vga.h"

/* --------------------------------------------------------------------------
 * Standard DES tables (1-based bit positions, MSB of byte 0 = bit 1)
 * -------------------------------------------------------------------------- */
static const uint8_t IP_T[64] = {
    58,50,42,34,26,18,10,2, 60,52,44,36,28,20,12,4,
    62,54,46,38,30,22,14,6, 64,56,48,40,32,24,16,8,
    57,49,41,33,25,17, 9,1, 59,51,43,35,27,19,11,3,
    61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7 };

static const uint8_t FP_T[64] = {
    40,8,48,16,56,24,64,32, 39,7,47,15,55,23,63,31,
    38,6,46,14,54,22,62,30, 37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28, 35,3,43,11,51,19,59,27,
    34,2,42,10,50,18,58,26, 33,1,41, 9,49,17,57,25 };

static const uint8_t E_T[48] = {
    32, 1, 2, 3, 4, 5,  4, 5, 6, 7, 8, 9,  8, 9,10,11,12,13,
    12,13,14,15,16,17, 16,17,18,19,20,21, 20,21,22,23,24,25,
    24,25,26,27,28,29, 28,29,30,31,32, 1 };

static const uint8_t P_T[32] = {
    16, 7,20,21,29,12,28,17,  1,15,23,26, 5,18,31,10,
     2, 8,24,14,32,27, 3, 9, 19,13,30, 6,22,11, 4,25 };

static const uint8_t PC1_T[56] = {
    57,49,41,33,25,17, 9,  1,58,50,42,34,26,18, 10, 2,59,51,43,35,27,
    19,11, 3,60,52,44,36, 63,55,47,39,31,23,15,  7,62,54,46,38,30,22,
    14, 6,61,53,45,37,29, 21,13, 5,28,20,12, 4 };

static const uint8_t PC2_T[48] = {
    14,17,11,24, 1, 5,  3,28,15, 6,21,10, 23,19,12, 4,26, 8,
    16, 7,27,20,13, 2, 41,52,31,37,47,55, 30,40,51,45,33,48,
    44,49,39,56,34,53, 46,42,50,36,29,32 };

static const uint8_t SHIFTS[16] = { 1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1 };

/* S-boxes: S[box][row*16 + col] */
static const uint8_t S_T[8][64] = {
  { 14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,  0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
     4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0, 15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13 },
  { 15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,  3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
     0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15, 13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9 },
  { 10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,  13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
    13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,  1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12 },
  {  7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15, 13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
    10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,   3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14 },
  {  2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9, 14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
     4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14, 11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3 },
  { 12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,  10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
     9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,  4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13 },
  {  4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1, 13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
     1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,  6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12 },
  { 13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,   1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
     7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,  2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11 } };

/* --------------------------------------------------------------------------
 * Bit-array helpers
 * -------------------------------------------------------------------------- */
static void bytes_to_bits(const uint8_t* in, uint8_t* bits, int nbytes) {
    for (int i = 0; i < nbytes; i++)
        for (int b = 0; b < 8; b++)
            bits[i * 8 + b] = (uint8_t)((in[i] >> (7 - b)) & 1);
}

static void bits_to_bytes(const uint8_t* bits, uint8_t* out, int nbytes) {
    for (int i = 0; i < nbytes; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) v = (uint8_t)((v << 1) | (bits[i * 8 + b] & 1));
        out[i] = v;
    }
}

/* out[i] = in[table[i] - 1] */
static void permute(const uint8_t* in, uint8_t* out, const uint8_t* table, int n) {
    for (int i = 0; i < n; i++) out[i] = in[table[i] - 1];
}

/* Rotate a 28-bit half left by n (n = 1 or 2). */
static void rotl28(uint8_t* h, int n) {
    for (int k = 0; k < n; k++) {
        uint8_t first = h[0];
        for (int i = 0; i < 27; i++) h[i] = h[i + 1];
        h[27] = first;
    }
}

/* --------------------------------------------------------------------------
 * Key schedule
 * -------------------------------------------------------------------------- */
void des_set_key(des_ctx_t* ctx, const uint8_t key[8]) {
    uint8_t kb[64], cd[56];
    bytes_to_bits(key, kb, 8);
    permute(kb, cd, PC1_T, 56);            /* C = cd[0..27], D = cd[28..55] */
    for (int r = 0; r < 16; r++) {
        rotl28(cd, SHIFTS[r]);
        rotl28(cd + 28, SHIFTS[r]);
        permute(cd, ctx->sub[r], PC2_T, 48);
    }
}

/* --------------------------------------------------------------------------
 * Feistel function f(R, K) -> 32 bits
 * -------------------------------------------------------------------------- */
static void des_f(const uint8_t* R, const uint8_t* K, uint8_t* out) {
    uint8_t e[48], s[32];
    permute(R, e, E_T, 48);
    for (int i = 0; i < 48; i++) e[i] ^= K[i];
    for (int box = 0; box < 8; box++) {
        const uint8_t* g = e + box * 6;
        int row = (g[0] << 1) | g[5];
        int col = (g[1] << 3) | (g[2] << 2) | (g[3] << 1) | g[4];
        uint8_t v = S_T[box][row * 16 + col];
        s[box * 4 + 0] = (uint8_t)((v >> 3) & 1);
        s[box * 4 + 1] = (uint8_t)((v >> 2) & 1);
        s[box * 4 + 2] = (uint8_t)((v >> 1) & 1);
        s[box * 4 + 3] = (uint8_t)(v & 1);
    }
    permute(s, out, P_T, 32);
}

/* --------------------------------------------------------------------------
 * Block transform (decrypt = subkeys in reverse order)
 * -------------------------------------------------------------------------- */
static void des_block(const des_ctx_t* ctx, const uint8_t in[8], uint8_t out[8], bool decrypt) {
    uint8_t bits[64], lr[64], f[32];
    bytes_to_bits(in, bits, 8);
    permute(bits, lr, IP_T, 64);           /* L = lr[0..31], R = lr[32..63] */
    for (int r = 0; r < 16; r++) {
        const uint8_t* K = ctx->sub[decrypt ? 15 - r : r];
        des_f(lr + 32, K, f);
        /* new L = old R; new R = old L ^ f(R, K) */
        for (int i = 0; i < 32; i++) {
            uint8_t nl = lr[32 + i];
            lr[32 + i] = (uint8_t)(lr[i] ^ f[i]);
            lr[i] = nl;
        }
    }
    /* Final swap (undo the last round's swap), then FP */
    for (int i = 0; i < 32; i++) { uint8_t t = lr[i]; lr[i] = lr[32 + i]; lr[32 + i] = t; }
    permute(lr, bits, FP_T, 64);
    bits_to_bytes(bits, out, 8);
}

void des_encrypt_block(const des_ctx_t* ctx, const uint8_t in[8], uint8_t out[8]) {
    des_block(ctx, in, out, false);
}

void des_decrypt_block(const des_ctx_t* ctx, const uint8_t in[8], uint8_t out[8]) {
    des_block(ctx, in, out, true);
}

uint8_t des_bitrev8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* --------------------------------------------------------------------------
 * Self-test (FIPS 46 example) + shell command
 * -------------------------------------------------------------------------- */
static const uint8_t TV_KEY[8]   = { 0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1 };
static const uint8_t TV_PLAIN[8] = { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF };
static const uint8_t TV_CIPH[8]  = { 0x85,0xE8,0x13,0x54,0x0F,0x0A,0xB4,0x05 };

static void run_vector(uint8_t enc[8], uint8_t dec[8]) {
    des_ctx_t c;
    des_set_key(&c, TV_KEY);
    des_encrypt_block(&c, TV_PLAIN, enc);
    des_decrypt_block(&c, enc, dec);
}

bool des_selftest(void) {
    uint8_t enc[8], dec[8];
    run_vector(enc, dec);
    return memcmp(enc, TV_CIPH, 8) == 0 && memcmp(dec, TV_PLAIN, 8) == 0;
}

static void print_hex8(const uint8_t* b) {
    static const char* hx = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        vga_putchar(hx[b[i] >> 4]);
        vga_putchar(hx[b[i] & 15]);
    }
}

void des_cmd(int argc, char* argv[]) {
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        uint8_t enc[8], dec[8];
        run_vector(enc, dec);
        bool ok = memcmp(enc, TV_CIPH, 8) == 0 && memcmp(dec, TV_PLAIN, 8) == 0;
        vga_print("  DES FIPS-46 vector: key 133457799BBCDFF1 pt 0123456789ABCDEF\n");
        vga_print("  encrypt -> "); print_hex8(enc);
        vga_print("  (expect 85E813540F0AB405)\n");
        vga_print("  decrypt -> "); print_hex8(dec); vga_print("\n");
        if (ok) vga_print_color("  DES selftest: PASS\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        else    vga_print_color("  DES selftest: FAIL\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    vga_print("  Usage: des selftest      (FIPS 46 known-answer test)\n");
}
