/* Kernel crypto (arch/x86_64/wgcrypto.c) against RFC 7693/7748/8439 vectors and, through stdin/stdout, against
 * Python's `cryptography` (tests/test_native_net6.py drives the random cross-check). */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NDEBUG
#include <assert.h>
#include "../arch/x86_64/wgcrypto.c"
static void hex(const uint8_t* p, unsigned n) { for (unsigned i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); }
static unsigned unhex(uint8_t* out, const char* text) { unsigned n = 0; while (text[0] && text[1] && text[0] != '\n') { out[n++] = (uint8_t)(hex_nibble(text[0]) << 4 | hex_nibble(text[1])); text += 2; } return n; }
int main(int argc, char** argv) {
    assert(wgcrypto_selftest());
    /* RFC 7748 5.2: X25519 iterated once and 1000 times */
    uint8_t k[32] = {9}, u[32] = {9}, r[32], expect[32];
    for (unsigned i = 0; i < 1000; i++) { x25519(r, k, u); memcpy(u, k, 32); memcpy(k, r, 32); if (i == 0) { from_hex(expect, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"); assert(!memcmp(k, expect, 32)); } }
    from_hex(expect, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"); assert(!memcmp(k, expect, 32));
    /* RFC 7748 6.1: shared secret */
    uint8_t a[32], b[32], pa[32], pb[32], s1[32], s2[32];
    from_hex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"); from_hex(b, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    x25519_public(pa, a); x25519_public(pb, b); x25519(s1, a, pb); x25519(s2, b, pa);
    from_hex(expect, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"); assert(!memcmp(s1, expect, 32) && !memcmp(s2, expect, 32));
    /* RFC 7693 appendix: keyed BLAKE2s self-test digest of the reference implementation (blake2s-ref selftest) */
    { uint8_t out[32]; struct blake2s_state s; blake2s_init(&s, 32, NULL, 0); blake2s_update(&s, "a", 1); blake2s_update(&s, "bc", 2); blake2s_final(&s, out);
      from_hex(expect, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"); assert(!memcmp(out, expect, 32)); }
    /* draft-irtf-cfrg-xchacha A.3.1 */
    { uint8_t key[32], nonce[24], aad[12], out[114 + 16], back[114];
      static const char plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
      from_hex(key, "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"); from_hex(nonce, "404142434445464748494a4b4c4d4e4f5051525354555657");
      from_hex(aad, "50515253c0c1c2c3c4c5c6c7");
      xchacha20poly1305_encrypt(out, (const uint8_t*)plain, 114, aad, 12, nonce, key);
      from_hex(expect, "bd6d179d3e83d43b9576579493c0e939"); assert(!memcmp(out, expect, 16));
      from_hex(expect, "c0875924c1c7987947deafd8780acf49"); assert(!memcmp(out + 114, expect, 16));
      assert(xchacha20poly1305_decrypt(back, out, 130, aad, 12, nonce, key) && !memcmp(back, plain, 114));
      out[3] ^= 1; assert(!xchacha20poly1305_decrypt(back, out, 130, aad, 12, nonce, key)); }
    if (argc > 1 && !strcmp(argv[1], "serve")) {
        /* line protocol: "x <scalar> <point>" | "a <key> <counter> <ad> <plain>" | "b <key-or-empty> <outlen> <data>" | "h <key> <data>" */
        char line[16384]; static uint8_t p1[4096], p2[4096], p3[4096], out[8192];
        while (fgets(line, sizeof(line), stdin)) {
            char* fields[6]; unsigned count = 0; char* save = line;
            for (char* t = strtok(save, " \n"); t && count < 6; t = strtok(NULL, " \n")) fields[count++] = t;
            if (!count) continue;
            if (fields[0][0] == 'x' && count == 3) { unhex(p1, fields[1]); unhex(p2, fields[2]); x25519(out, p1, p2); hex(out, 32); }
            else if (fields[0][0] == 'a' && count == 5) {
                unhex(p1, fields[1]); uint64_t counter = strtoull(fields[2], NULL, 10);
                unsigned ad = strcmp(fields[3], "-") ? unhex(p2, fields[3]) : 0, n = strcmp(fields[4], "-") ? unhex(p3, fields[4]) : 0;
                chacha20poly1305_encrypt(out, p3, n, p2, ad, counter, p1); hex(out, n + 16);
                static uint8_t back[4096]; assert(chacha20poly1305_decrypt(back, out, n + 16, p2, ad, counter, p1) && !memcmp(back, p3, n));
            } else if (fields[0][0] == 'b' && count == 4) {
                unsigned key = strcmp(fields[1], "-") ? unhex(p1, fields[1]) : 0, length = (unsigned)atoi(fields[2]), n = strcmp(fields[3], "-") ? unhex(p2, fields[3]) : 0;
                blake2s(out, length, p1, key, p2, n); hex(out, length);
            } else if (fields[0][0] == 'h' && count == 3) {
                unsigned key = unhex(p1, fields[1]), n = strcmp(fields[2], "-") ? unhex(p2, fields[2]) : 0;
                hmac_blake2s(out, p1, key, p2, n); hex(out, 32);
            } else { printf("?\n"); }
            fflush(stdout);
        }
        return 0;
    }
    puts("PASS wgcrypto: BLAKE2s, X25519 (RFC 7748 incl. 1000 iterations), Poly1305/ChaCha20-Poly1305 (RFC 8439), XChaCha20-Poly1305");
    return 0;
}
