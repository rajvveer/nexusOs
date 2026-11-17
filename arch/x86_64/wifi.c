/* Wi-Fi softmac (see wifi.h): 802.11 station and access-point MAC layer, software crypto (CCMP-128, BIP-CMAC-128),
 * software scanning and the virtual radio medium ("hwsim": radios created through /proc/sys/net/nexus/wifi_sim hear
 * each other's frames on the same channel, with a per-radio signal level and an out-of-range switch).
 *
 * Station: wpa_supplicant drives the SME (NL80211_CMD_AUTHENTICATE / ASSOCIATE, SAE commit/confirm built in user
 * space); this file sends the frames, retries, reports the AP's answers, keeps the BSS table from beacons and probe
 * responses, watches beacons (connection loss after 3 s without one) and moves data as 802.3 <-> 802.11 with port
 * control (only the control-port ethertype, EAPOL, passes until NL80211_STA_FLAG_AUTHORIZED).
 * Access point: hostapd drives everything through management frame registrations (auth, (re)assoc, probe requests,
 * action frames reach it as NL80211_CMD_FRAME) and NL80211_CMD_FRAME transmissions; the kernel beacons from hostapd's
 * template (inserting the TIM element), keeps the station table and keys, and bridges data between associated
 * stations and the interface. Robust management frames are CCMP-protected to MFP peers (BIP for group addressed).
 * Everything runs under the BKL (syscalls and the network poll). */
#include "wifi.h"
#include "nlmsg.h"
#include "nl80211_defs.h"
#include "../../kernel/string.h"

void native_network_kick(void);
int sysfs_add(const char* path, unsigned type, unsigned id, const char* target);   /* sysfs.c: 0 dir, 3 link */
int sysfs_add_wifi(const char* path, unsigned subid, unsigned phy);
static uint8_t sysfs_phy_created;   /* one bit per phy index: /sys nodes are added once and then persist */

struct wifi_radio* wifi_radios[WIFI_RADIO_MAX];
static unsigned radio_count;   /* 0: wifi_next_event and wifi_poll cost one compare */
const unsigned wifi_channel_freqs[WIFI_CHANNELS] = {2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
                                                    5180, 5200, 5220, 5240};
static const uint8_t bcast[6] = {255, 255, 255, 255, 255, 255};
#define ETH_P_PAE 0x888E
#define AUTH_TIMEOUT_NS 200000000ULL
#define BEACON_LOSS_NS 3000000000ULL
#define BSS_EXPIRE_NS 30000000000ULL
#define STA_BIT(f) (1U << (f))

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static bool group_addr(const uint8_t* a) { return (a[0] & 1) != 0; }
static void move_bytes(uint8_t* dst, const uint8_t* src, unsigned n) {   /* overlapping copy */
    if (dst < src) for (unsigned i = 0; i < n; i++) dst[i] = src[i]; else for (unsigned i = n; i--;) dst[i] = src[i];
}
static unsigned hdr_len(const uint8_t* f) {
    unsigned n = 24; if ((f[1] & 3) == 3) n += 6;
    if ((f[0] & 0x0C) == 0x08 && (f[0] & 0x80)) n += 2;              /* QoS data */
    return n;
}

/* ====================================================================================================
 * AES-128/256 (FIPS-197), AES-CMAC (RFC 4493) + CCM (CCMP-128/256), AES-GCM (GCMP-128/256) + GMAC (BIP).
 * CCMP/GCMP protect data and robust unicast management frames; BIP-CMAC/GMAC protect group management frames.
 * ==================================================================================================== */
static uint8_t sbox[256]; static bool sbox_ready;
static uint8_t rotl8(uint8_t x, unsigned n) { return (uint8_t)(x << n | x >> (8 - n)); }
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static void sbox_init(void) {
    if (sbox_ready) return;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4); if (q & 0x80) q ^= 0x09;
        sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63; sbox_ready = true;
}
static uint32_t subword(uint32_t t) { return (uint32_t)sbox[t >> 24] << 24 | (uint32_t)sbox[(t >> 16) & 255] << 16 | (uint32_t)sbox[(t >> 8) & 255] << 8 | sbox[t & 255]; }
/* Key schedule for a 16- or 32-byte key; returns the round count (10 or 14). */
static unsigned aes_expand(uint32_t* w, const uint8_t* key, unsigned bytes) {
    sbox_init();
    unsigned nk = bytes / 4, nr = nk + 6, words = 4 * (nr + 1);
    for (unsigned i = 0; i < nk; i++) w[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    uint8_t rcon = 1;
    for (unsigned i = nk; i < words; i++) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) { t = subword(t << 8 | t >> 24) ^ ((uint32_t)rcon << 24); rcon = xt(rcon); }
        else if (nk > 6 && i % nk == 4) t = subword(t);
        w[i] = w[i - nk] ^ t;
    }
    return nr;
}
static void add_round(uint8_t* s, const uint32_t* w) {
    for (unsigned c = 0; c < 4; c++) for (unsigned r = 0; r < 4; r++) s[4 * c + r] ^= (uint8_t)(w[c] >> (24 - 8 * r));
}
static void aes_encrypt(const uint32_t* w, unsigned nr, uint8_t* s) {
    add_round(s, w);
    for (unsigned round = 1; round <= nr; round++) {
        uint8_t t[16];
        for (unsigned c = 0; c < 4; c++) for (unsigned r = 0; r < 4; r++) t[4 * c + r] = sbox[s[4 * ((c + r) & 3) + r]];   /* SubBytes+ShiftRows */
        if (round < nr)
            for (unsigned c = 0; c < 4; c++) {
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3); s[4 * c + 1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3); s[4 * c + 3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        else memcpy(s, t, 16);
        add_round(s, w + 4 * round);
    }
}
/* ---- CCM (CCMP): AES-CTR + CBC-MAC, L=2, an 8- or 16-byte MIC ---- */
static void cmac_subkey(uint8_t* k) { uint8_t carry = (uint8_t)(k[0] >> 7); for (unsigned i = 0; i < 15; i++) k[i] = (uint8_t)(k[i] << 1 | k[i + 1] >> 7); k[15] = (uint8_t)(k[15] << 1); if (carry) k[15] ^= 0x87; }
static void aes_cmac(const uint32_t* w, unsigned nr, const uint8_t* data, unsigned length, uint8_t* out) {
    uint8_t k1[16] = {0}, k2[16], x[16] = {0}, m[16];
    aes_encrypt(w, nr, k1); cmac_subkey(k1); memcpy(k2, k1, 16); cmac_subkey(k2);
    unsigned blocks = length ? (length + 15) / 16 : 1; bool complete = length && !(length % 16);
    for (unsigned b = 0; b + 1 < blocks; b++) { for (unsigned i = 0; i < 16; i++) x[i] ^= data[16 * b + i]; aes_encrypt(w, nr, x); }
    unsigned last = (blocks - 1) * 16, rest = length - last;
    memset(m, 0, 16); memcpy(m, data + last, rest);
    if (complete) for (unsigned i = 0; i < 16; i++) m[i] ^= k1[i];
    else { m[rest] = 0x80; for (unsigned i = 0; i < 16; i++) m[i] ^= k2[i]; }
    for (unsigned i = 0; i < 16; i++) x[i] ^= m[i];
    aes_encrypt(w, nr, x); memcpy(out, x, 16);
}
static void ctr_crypt(const uint32_t* w, unsigned nr, const uint8_t* nonce, uint8_t* data, unsigned length) {
    uint8_t a[16], s[16];
    for (unsigned at = 0, counter = 1; at < length; at += 16, counter++) {
        a[0] = 0x01; memcpy(a + 1, nonce, 13); a[14] = (uint8_t)(counter >> 8); a[15] = (uint8_t)counter;
        memcpy(s, a, 16); aes_encrypt(w, nr, s);
        unsigned n = length - at < 16 ? length - at : 16;
        for (unsigned i = 0; i < n; i++) data[at + i] ^= s[i];
    }
}
static void cbc_mac(const uint32_t* w, unsigned nr, unsigned mic, const uint8_t* nonce, const uint8_t* aad, unsigned aad_length, const uint8_t* data, unsigned length, uint8_t* x) {
    uint8_t b[16];
    x[0] = (uint8_t)(0x40 | (((mic - 2) / 2) << 3) | 1); memcpy(x + 1, nonce, 13); x[14] = (uint8_t)(length >> 8); x[15] = (uint8_t)length; aes_encrypt(w, nr, x);
    memset(b, 0, 16); b[0] = (uint8_t)(aad_length >> 8); b[1] = (uint8_t)aad_length;
    unsigned n = aad_length < 14 ? aad_length : 14; memcpy(b + 2, aad, n);
    for (unsigned i = 0; i < 16; i++) x[i] ^= b[i];
    aes_encrypt(w, nr, x);
    for (unsigned at = n; at < aad_length; at += 16) {
        memset(b, 0, 16); unsigned k = aad_length - at < 16 ? aad_length - at : 16; memcpy(b, aad + at, k);
        for (unsigned i = 0; i < 16; i++) x[i] ^= b[i];
        aes_encrypt(w, nr, x);
    }
    for (unsigned at = 0; at < length; at += 16) {
        memset(b, 0, 16); unsigned k = length - at < 16 ? length - at : 16; memcpy(b, data + at, k);
        for (unsigned i = 0; i < 16; i++) x[i] ^= b[i];
        aes_encrypt(w, nr, x);
    }
}
static void ccm_tag(const uint32_t* w, unsigned nr, unsigned mic, const uint8_t* nonce, uint8_t* tag) {   /* tag ^= S0 */
    uint8_t s[16]; s[0] = 0x01; memcpy(s + 1, nonce, 13); s[14] = s[15] = 0; aes_encrypt(w, nr, s);
    for (unsigned i = 0; i < mic; i++) tag[i] ^= s[i];
}
/* ---- GCM (GCMP) and GMAC (BIP-GMAC): GHASH over GF(2^128) + AES-CTR, a 16-byte tag, a 96-bit nonce ---- */
static void gf_mul(uint8_t* x, const uint8_t* h) {   /* x = x . h, right-shift, R = 0xE1 (NIST SP 800-38D) */
    uint8_t z[16] = {0}, v[16]; memcpy(v, h, 16);
    for (unsigned i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7))) for (unsigned j = 0; j < 16; j++) z[j] ^= v[j];
        bool lsb = v[15] & 1;
        for (unsigned j = 15; j > 0; j--) v[j] = (uint8_t)(v[j] >> 1 | v[j - 1] << 7);
        v[0] >>= 1; if (lsb) v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}
static void ghash_blocks(const uint8_t* h, const uint8_t* data, unsigned length, uint8_t* y) {
    for (unsigned at = 0; at < length; at += 16) {
        unsigned n = length - at < 16 ? length - at : 16;
        for (unsigned i = 0; i < n; i++) y[i] ^= data[at + i];
        gf_mul(y, h);
    }
}
static void put_be64(uint8_t* p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }
/* GCM encrypt (decrypt=false) or decrypt+verify (decrypt=true), data in place; tag out (16 bytes). */
static void gcm_crypt(const uint32_t* w, unsigned nr, const uint8_t* nonce12, const uint8_t* aad, unsigned alen,
                      uint8_t* data, unsigned len, uint8_t* tag, bool decrypt) {
    uint8_t h[16]; memset(h, 0, 16); aes_encrypt(w, nr, h);
    uint8_t j0[16]; memcpy(j0, nonce12, 12); j0[12] = j0[13] = j0[14] = 0; j0[15] = 1;
    uint8_t ej0[16]; memcpy(ej0, j0, 16); aes_encrypt(w, nr, ej0);
    uint8_t y[16]; memset(y, 0, 16);
    if (decrypt) { ghash_blocks(h, aad, alen, y); ghash_blocks(h, data, len, y); }
    uint8_t ctr[16]; memcpy(ctr, j0, 16);
    for (unsigned at = 0; at < len; at += 16) {
        for (unsigned j = 15; j >= 12; j--) { if (++ctr[j]) break; }
        uint8_t s[16]; memcpy(s, ctr, 16); aes_encrypt(w, nr, s);
        unsigned n = len - at < 16 ? len - at : 16;
        for (unsigned i = 0; i < n; i++) data[at + i] ^= s[i];
    }
    if (!decrypt) { ghash_blocks(h, aad, alen, y); ghash_blocks(h, data, len, y); }
    uint8_t lb[16]; put_be64(lb, (uint64_t)alen * 8); put_be64(lb + 8, (uint64_t)len * 8);
    for (unsigned i = 0; i < 16; i++) y[i] ^= lb[i];
    gf_mul(y, h);
    for (unsigned i = 0; i < 16; i++) tag[i] = (uint8_t)(y[i] ^ ej0[i]);
}
/* ---- 802.11 framing shared by CCMP and GCMP ---- */
static unsigned ccmp_aad(const uint8_t* f, unsigned hlen, uint8_t* aad) {
    bool data = (f[0] & 0x0C) == 0x08;
    aad[0] = data ? (uint8_t)(f[0] & 0x8F) : f[0];
    aad[1] = (uint8_t)((f[1] & ~0x38) | 0x40);
    memcpy(aad + 2, f + 4, 18);
    aad[20] = (uint8_t)(f[22] & 0x0F); aad[21] = 0;
    unsigned n = 22;
    if ((f[1] & 3) == 3) { memcpy(aad + n, f + 24, 6); n += 6; }
    if (data && (f[0] & 0x80)) { aad[n] = (uint8_t)(f[hlen - 2] & 0x0F); aad[n + 1] = 0; n += 2; }
    return n;
}
static void ccmp_nonce(const uint8_t* f, unsigned hlen, uint64_t pn, uint8_t* nonce) {   /* CCM: 13 bytes with the priority/mgmt flags */
    bool data = (f[0] & 0x0C) == 0x08, mgmt = (f[0] & 0x0C) == 0;
    nonce[0] = (uint8_t)((data && (f[0] & 0x80) ? f[hlen - 2] & 0x0F : 0) | (mgmt ? 0x10 : 0));
    memcpy(nonce + 1, f + 10, 6);
    for (unsigned i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (8 * (5 - i)));
}
static void gcmp_nonce(const uint8_t* f, uint64_t pn, uint8_t* nonce) {   /* GCM: 12 bytes, A2 || PN */
    memcpy(nonce, f + 10, 6);
    for (unsigned i = 0; i < 6; i++) nonce[6 + i] = (uint8_t)(pn >> (8 * (5 - i)));
}
static void write_pn_header(uint8_t* c, uint64_t pn, uint8_t keyid) {
    c[0] = (uint8_t)pn; c[1] = (uint8_t)(pn >> 8); c[2] = 0; c[3] = (uint8_t)(0x20 | (keyid & 3) << 6);
    for (unsigned i = 0; i < 4; i++) c[4 + i] = (uint8_t)(pn >> (16 + 8 * i));
}
static uint64_t read_pn_header(const uint8_t* c) {
    return (uint64_t)c[0] | (uint64_t)c[1] << 8 | (uint64_t)c[4] << 16 | (uint64_t)c[5] << 24 | (uint64_t)c[6] << 32 | (uint64_t)c[7] << 40;
}
/* In place; the buffer must have 8 + mic spare bytes. Returns the new length. */
static unsigned pkt_protect(struct wifi_key* k, uint8_t* f, unsigned length) {
    bool gcm = k->cipher == WIFI_CIPHER_GCMP || k->cipher == WIFI_CIPHER_GCMP_256;
    unsigned h = hdr_len(f), mic = k->mic_len; uint64_t pn = ++k->tx_pn & 0xFFFFFFFFFFFFULL;
    move_bytes(f + h + 8, f + h, length - h);
    write_pn_header(f + h, pn, k->index); f[1] |= 0x40;
    uint8_t aad[32]; unsigned alen = ccmp_aad(f, h, aad); unsigned plen = length - h; uint8_t* body = f + h + 8;
    uint8_t x[16];
    if (gcm) { uint8_t n[12]; gcmp_nonce(f, pn, n); gcm_crypt(k->rk, k->rounds, n, aad, alen, body, plen, x, false); }
    else { uint8_t n[13]; ccmp_nonce(f, h, pn, n); cbc_mac(k->rk, k->rounds, mic, n, aad, alen, body, plen, x); ccm_tag(k->rk, k->rounds, mic, n, x); ctr_crypt(k->rk, k->rounds, n, body, plen); }
    memcpy(body + plen, x, mic);
    return length + 8 + mic;
}
/* 0: decrypted in place (*length updated), -1: bad frame/MIC, -2: replay */
static int pkt_unprotect(struct wifi_key* k, uint8_t* f, unsigned* length) {
    bool gcm = k->cipher == WIFI_CIPHER_GCMP || k->cipher == WIFI_CIPHER_GCMP_256;
    unsigned h = hdr_len(f), mic = k->mic_len, len = *length;
    if (len < h + 8 + mic) return -1;
    const uint8_t* c = f + h; if (!(c[3] & 0x20)) return -1;
    uint64_t pn = read_pn_header(c); if (pn <= k->rx_pn) return -2;
    uint8_t aad[32]; unsigned alen = ccmp_aad(f, h, aad); unsigned plen = len - h - 8 - mic; uint8_t* body = f + h + 8;
    uint8_t x[16];
    if (gcm) { uint8_t n[12]; gcmp_nonce(f, pn, n); gcm_crypt(k->rk, k->rounds, n, aad, alen, body, plen, x, true); }
    else { uint8_t n[13]; ccmp_nonce(f, h, pn, n); ctr_crypt(k->rk, k->rounds, n, body, plen); cbc_mac(k->rk, k->rounds, mic, n, aad, alen, body, plen, x); ccm_tag(k->rk, k->rounds, mic, n, x); }
    uint8_t diff = 0; for (unsigned i = 0; i < mic; i++) diff |= (uint8_t)(x[i] ^ body[plen + i]);
    if (diff) return -1;
    k->rx_pn = pn;
    move_bytes(f + h, body, plen); f[1] &= (uint8_t)~0x40; *length = h + plen;
    return 0;
}
/* ---- BIP (group management): CMAC or GMAC, an MMIE (element 76) with the IPN and a mic_len MIC ---- */
static uint8_t bip_buffer[WIFI_FRAME_MAX + 64];
static unsigned bip_aad(const uint8_t* f, unsigned length) {   /* FC(masked) || A1 A2 A3 || body (MMIE MIC already zeroed) */
    uint8_t* b = bip_buffer;
    b[0] = f[0]; b[1] = (uint8_t)(f[1] & ~0x38); memcpy(b + 2, f + 4, 18);
    memcpy(b + 20, f + 24, length - 24);
    return 20 + length - 24;
}
static void bip_mic(const struct wifi_key* k, const uint8_t* f, unsigned length, uint8_t* mic) {
    unsigned n = bip_aad(f, length);
    memset(bip_buffer + n - k->mic_len, 0, k->mic_len);   /* the MIC field inside the MMIE */
    if (k->cipher == WIFI_CIPHER_BIP_CMAC) { uint8_t full[16]; aes_cmac(k->rk, k->rounds, bip_buffer, n, full); memcpy(mic, full, 8); }
    else { uint8_t nonce[12]; memcpy(nonce, f + 10, 6); memcpy(nonce + 6, f + length - k->mic_len - 6, 6); gcm_crypt(k->rk, k->rounds, nonce, bip_buffer, n, NULL, 0, mic, false); }
}
static unsigned bip_protect(struct wifi_key* k, uint8_t* f, unsigned length) {
    uint64_t ipn = ++k->tx_pn & 0xFFFFFFFFFFFFULL; unsigned mic = k->mic_len, mmie = 10 + mic;
    uint8_t* m = f + length; m[0] = 76; m[1] = (uint8_t)(8 + mic); wr16(m + 2, k->index);
    for (unsigned i = 0; i < 6; i++) m[4 + i] = (uint8_t)(ipn >> (8 * i));
    memset(m + 10, 0, mic); length += mmie;
    bip_mic(k, f, length, f + length - mic);
    return length;
}
/* 0 ok (MMIE stripped), -1 bad/unknown key, -2 replay */
static int bip_check(struct wifi_key* igtk, unsigned count, uint8_t* f, unsigned* length) {
    unsigned len = *length;
    for (unsigned mic = 8; mic <= 16; mic += 8) {
        unsigned mmie = 10 + mic; if (len < 24 + mmie) continue;
        uint8_t* m = f + len - mmie; if (m[0] != 76 || m[1] != 8 + mic) continue;
        unsigned index = rd16(m + 2); struct wifi_key* k = NULL;
        for (unsigned i = 0; i < count; i++) if (igtk[i].set && igtk[i].index == index && igtk[i].mic_len == mic) k = &igtk[i];
        if (!k) continue;
        uint64_t ipn = 0; for (unsigned i = 0; i < 6; i++) ipn |= (uint64_t)m[4 + i] << (8 * i);
        if (ipn <= k->rx_pn) return -2;
        uint8_t want[16]; bip_mic(k, f, len, want);
        uint8_t diff = 0; for (unsigned i = 0; i < mic; i++) diff |= (uint8_t)(want[i] ^ m[10 + i]);
        if (diff) return -1;
        k->rx_pn = ipn; *length = len - mmie; return 0;
    }
    return -1;
}
static bool hex_is(const uint8_t* data, const char* hex) {
    for (unsigned i = 0; hex[2 * i]; i++) {
        unsigned hi = (unsigned)(hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10), lo = (unsigned)(hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10);
        if (data[i] != (uint8_t)(hi << 4 | lo)) return false;
    }
    return true;
}
static void hex_load(uint8_t* out, const char* hex) {
    for (unsigned i = 0; hex[2 * i]; i++) {
        unsigned hi = (unsigned)(hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10), lo = (unsigned)(hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10);
        out[i] = (uint8_t)(hi << 4 | lo);
    }
}
static unsigned selftest_result;   /* bit0 AES-128, 1 CMAC, 2 CCMP enc, 3 CCMP dec, 4 tamper, 5 AES-256, 6 GCM128, 7 GCM256, 8 CCM256, 9 GMAC */
/* Encrypt plaintext+aad with GCM into a scratch buffer and compare ct||tag (self-contained KAT for gcm_crypt). */
static bool gcm_kat(const char* keyhex, unsigned bytes, const char* noncehex, const char* aadhex, const char* pthex, const char* expect) {
    uint32_t w[60]; uint8_t key[32], nonce[12], aad[64], buf[64], tag[16];
    hex_load(key, keyhex); unsigned nr = aes_expand(w, key, bytes);
    hex_load(nonce, noncehex);
    unsigned alen = 0; for (const char* p = aadhex; p[0] && p[1]; p += 2) alen++; hex_load(aad, aadhex);
    unsigned plen = 0; for (const char* p = pthex; p[0] && p[1]; p += 2) plen++; if (plen) hex_load(buf, pthex);
    gcm_crypt(w, nr, nonce, aad, alen, buf, plen, tag, false);
    uint8_t out[80]; memcpy(out, buf, plen); memcpy(out + plen, tag, 16);
    return hex_is(out, expect);
}
bool wifi_selftest(void) {
    unsigned ok = 0; uint32_t w[60]; uint8_t key[32], block[16];
    hex_load(key, "000102030405060708090a0b0c0d0e0f"); hex_load(block, "00112233445566778899aabbccddeeff");
    unsigned nr = aes_expand(w, key, 16); aes_encrypt(w, nr, block); if (hex_is(block, "69c4e0d86a7b0430d8cdb78070b4c55a")) ok |= 1;   /* FIPS-197 C.1 */
    hex_load(key, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"); hex_load(block, "00112233445566778899aabbccddeeff");
    nr = aes_expand(w, key, 32); aes_encrypt(w, nr, block); if (hex_is(block, "8ea2b7ca516745bfeafc49904b496089")) ok |= 32;   /* FIPS-197 C.3 */
    hex_load(key, "2b7e151628aed2a6abf7158809cf4f3c"); nr = aes_expand(w, key, 16);
    uint8_t msg[16], mac[16]; hex_load(msg, "6bc1bee22e409f96e93d7e117393172a");
    aes_cmac(w, nr, msg, 0, mac); bool c0 = hex_is(mac, "bb1d6929e95937287fa37d129b756746");                   /* RFC 4493 ex. 1, 2 */
    aes_cmac(w, nr, msg, 16, mac); if (c0 && hex_is(mac, "070a16b46b4d4144f79bdd9dd04a287c")) ok |= 2;
    /* IEEE 802.11-2016 M.6.4 CCMP-128 test MPDU 1 (end to end through a real frame) */
    static uint8_t f[128]; struct wifi_key k; memset(&k, 0, sizeof(k));
    hex_load(k.key, "c97c1f67ce371185514a8a19f2bdd52f"); k.rounds = aes_expand(k.rk, k.key, 16); k.set = true; k.cipher = WIFI_CIPHER_CCMP; k.mic_len = 8;
    hex_load(f, "0848c32c0fd2e128a57c5030f1844408abaea5b8fcba8033f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050");
    k.tx_pn = 0xB5039776E70CULL - 1;
    unsigned n = pkt_protect(&k, f, 44);
    if (n == 60 && hex_is(f, "0848c32c0fd2e128a57c5030f1844408abaea5b8fcba80330ce7002076970" "3b5f3d0a2fe9a3dbf2342a643e43246e80c3c04d0197845ce0b16f97623")) ok |= 4;
    unsigned m = n; k.rx_pn = 0;
    if (!pkt_unprotect(&k, f, &m) && m == 44 && hex_is(f + 24, "f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050")) ok |= 8;
    k.tx_pn = 5; k.rx_pn = 0; n = pkt_protect(&k, f, 44); f[30] ^= 1; m = n;
    if (pkt_unprotect(&k, f, &m) == -1) ok |= 16;
    /* AES-GCM and GMAC primitives (cross-checked against Python's cryptography AESGCM); expect = ciphertext || 16-byte tag */
    if (gcm_kat("000102030405060708090a0b0c0d0e0f", 16, "11223344556600000000000a",
                "88410000000000000000000000000000000000000000", "000102030405060708090a0b0c0d0e0f10111213",
                "b6e18fc7cbb33bf5f0b8744717baee11980e5d4ed1d91997c29db27779eb3e1a792a3e9b")) ok |= 64;
    if (gcm_kat("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 32, "11223344556600000000000a",
                "88410000000000000000000000000000000000000000", "000102030405060708090a0b0c0d0e0f10111213",
                "a55af69a4c2cb7effe815b4f5fb0a9e35819b01fd786cdfedc523888e3788c5d55662782")) ok |= 128;
    /* GMAC: empty plaintext, the frame as AAD (TAG only) */
    if (gcm_kat("000102030405060708090a0b0c0d0e0f", 16, "aabbccddeeff000000000001",
                "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d", "",
                "3becce6a8a48731c536fce4a08a5ed73")) ok |= 512;
    /* CCMP-256 end to end: AES-256, 16-byte MIC */
    memset(&k, 0, sizeof(k)); hex_load(k.key, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    k.rounds = aes_expand(k.rk, k.key, 32); k.set = true; k.cipher = WIFI_CIPHER_CCMP_256; k.mic_len = 16; k.tx_pn = 0; k.rx_pn = 0;
    hex_load(f, "0848c32c0fd2e128a57c5030f1844408abaea5b8fcba8033f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050");
    n = pkt_protect(&k, f, 44); m = n; k.rx_pn = 0;
    if (!pkt_unprotect(&k, f, &m) && m == 44 && hex_is(f + 24, "f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050")) ok |= 256;
    selftest_result = ok;
    return (ok & (1|2|4|8|16|32|64|128|256|512)) == (1|2|4|8|16|32|64|128|256|512);
}

/* ====================================================================================================
 * BSS table
 * ==================================================================================================== */
static unsigned freq_channel(unsigned freq) { return freq >= 5000 ? (freq - 5000) / 5 : freq == 2484 ? 14 : (freq - 2407) / 5; }
static bool freq_known(unsigned freq) { for (unsigned i = 0; i < WIFI_CHANNELS; i++) if (wifi_channel_freqs[i] == freq) return true; return false; }
static struct wifi_bss* bss_find(struct wifi_radio* r, const uint8_t* bssid, unsigned freq) {
    for (unsigned i = 0; i < WIFI_BSS_MAX; i++)
        if (r->bss[i].used && !memcmp(r->bss[i].bssid, bssid, 6) && (!freq || r->bss[i].freq == freq)) return &r->bss[i];
    return NULL;
}
static void bss_update(struct wifi_radio* r, const uint8_t* f, unsigned length, const struct wifi_rx_status* st, bool presp) {
    const uint8_t* bssid = f + 16; uint64_t now = native_clock_ns();
    struct wifi_bss* e = bss_find(r, bssid, st->freq);
    if (!e) {
        struct wifi_bss* oldest = &r->bss[0];
        for (unsigned i = 0; i < WIFI_BSS_MAX && !e; i++) { if (!r->bss[i].used) e = &r->bss[i]; else if (r->bss[i].seen_ns < oldest->seen_ns) oldest = &r->bss[i]; }
        if (!e) {
            struct wifi_vif* v = r->vif;
            if (v && v->sme != WIFI_IDLE && !memcmp(oldest->bssid, v->bssid, 6)) return;   /* never evict our BSS */
            e = oldest;
        }
        memset(e, 0, sizeof(*e)); e->used = true; memcpy(e->bssid, bssid, 6); e->freq = st->freq;
    }
    unsigned ies = length - 36 > WIFI_IE_BYTES ? WIFI_IE_BYTES : length - 36;
    e->tsf = 0; for (unsigned i = 0; i < 8; i++) e->tsf |= (uint64_t)f[24 + i] << (8 * i);
    e->beacon_int = rd16(f + 32); e->capability = rd16(f + 34);
    memcpy(e->ies, f + 36, ies); e->ies_len = (uint16_t)ies; e->presp = presp;
    if (!presp) { memcpy(e->beacon_ies, f + 36, ies); e->beacon_ies_len = (uint16_t)ies; e->beaconed = true; }
    e->signal_mbm = st->signal_dbm * 100; e->seen_ns = now;
}
static void bss_expire(struct wifi_radio* r, uint64_t now) {
    struct wifi_vif* v = r->vif;
    for (unsigned i = 0; i < WIFI_BSS_MAX; i++) {
        struct wifi_bss* e = &r->bss[i];
        if (!e->used || now < e->seen_ns + BSS_EXPIRE_NS) continue;
        if (v && v->sme != WIFI_IDLE && !memcmp(e->bssid, v->bssid, 6)) continue;
        e->used = false;
    }
}

/* ====================================================================================================
 * Transmit helpers
 * ==================================================================================================== */
static uint8_t tx_buffer[WIFI_FRAME_MAX + 64], rx_buffer[WIFI_FRAME_MAX + 64];
struct tx_wait { struct tx_wait* next; struct wifi_vif* vif; uint64_t cookie; unsigned length; uint8_t frame[]; };
static struct tx_wait* tx_waits; static unsigned tx_wait_count;
static uint64_t next_cookie = 1;

static void radio_tune(struct wifi_radio* r, unsigned freq) {
    if (!freq || r->freq == freq) return;
    r->freq = freq; if (r->ops->set_channel) r->ops->set_channel(r, freq);
}
static void radio_mode_changed(struct wifi_radio* r) { if (r->ops->set_mode) r->ops->set_mode(r); }
static void mgmt_header(struct wifi_vif* v, uint8_t* f, unsigned subtype, const uint8_t* da, const uint8_t* bssid) {
    memset(f, 0, 24); f[0] = (uint8_t)(subtype << 4);
    memcpy(f + 4, da, 6); memcpy(f + 10, v->dev->addr, 6); memcpy(f + 16, bssid, 6);
}
/* Sequence number, then out to the driver. cookie 0: no status wanted. */
static int radio_tx(struct wifi_vif* v, uint8_t* f, unsigned length, uint64_t cookie) {
    wr16(f + 22, (uint16_t)((rd16(f + 22) & 0x000F) | (uint16_t)(v->seq++ << 4)));
    struct wifi_radio* r = v->radio;
    int e = r->ops->tx(r, f, length, cookie);
    if (e) v->dev->stats.tx_dropped++;
    return e;
}
static bool robust_mgmt(const uint8_t* f, unsigned length) {
    unsigned subtype = f[0] >> 4;
    if (subtype == 10 || subtype == 12) return true;
    if (subtype != 13 || length <= 24) return false;
    uint8_t category = f[24];
    return category != 4 && category != 7 && category != 11 && category != 15 && category != 127 && category != 20;
}
static struct wifi_station* peer_of(struct wifi_vif* v, const uint8_t* addr) {
    if (v->iftype == NL80211_IFTYPE_AP) return wifi_station_find(v, addr);
    return v->ap.used && !memcmp(v->ap.addr, addr, 6) ? &v->ap : NULL;
}
/* Protect a robust management frame for an MFP peer (CCMP), or a group addressed one (BIP); length out. */
static unsigned mgmt_protect(struct wifi_vif* v, uint8_t* f, unsigned length) {
    if (!robust_mgmt(f, length)) return length;
    if (group_addr(f + 4)) {
        if (v->iftype == NL80211_IFTYPE_AP && v->default_mgmt_key >= 4 && v->default_mgmt_key <= 5 && v->igtk_tx[v->default_mgmt_key - 4].set)
            return bip_protect(&v->igtk_tx[v->default_mgmt_key - 4], f, length);
        return length;
    }
    struct wifi_station* p = peer_of(v, f + 4);
    if (p && (p->flags & STA_BIT(NL80211_STA_FLAG_MFP)) && p->ptk.set) return pkt_protect(&p->ptk, f, length);
    return length;
}

/* ====================================================================================================
 * Station (client) MLME
 * ==================================================================================================== */
static void sta_teardown(struct wifi_vif* v) {
    v->sme = WIFI_IDLE; memset(&v->ap, 0, sizeof(v->ap));
    memset(v->gtk, 0, sizeof(v->gtk)); memset(v->igtk, 0, sizeof(v->igtk));
    v->pending_len = 0; v->cqm_state = 0;
    netdev_set_carrier(v->dev, false);
    radio_mode_changed(v->radio);
}
static void sta_send_pending(struct wifi_vif* v) {
    memcpy(tx_buffer, v->pending, v->pending_len);
    radio_tx(v, tx_buffer, v->pending_len, 0);
    v->deadline_ns = native_clock_ns() + AUTH_TIMEOUT_NS;
}
/* Report a disconnection the way cfg80211 does: the MLME event with the (sent or received) frame, then DISCONNECT. */
static void sta_report_disconnect(struct wifi_vif* v, const uint8_t* frame, unsigned length, bool by_ap) {
    unsigned subtype = frame[0] >> 4; uint16_t reason = length >= 26 ? rd16(frame + 24) : 1;
    nl80211_event_frame(v, subtype == 10 ? NL80211_CMD_DISASSOCIATE : NL80211_CMD_DEAUTHENTICATE, frame, length, v->freq, 0);
    (void)by_ap; (void)reason;
}
static void sta_disconnect_local(struct wifi_vif* v, uint16_t reason, bool transmit) {
    uint8_t f[64]; mgmt_header(v, f, 12, v->bssid, v->bssid); wr16(f + 24, reason);
    if (transmit && v->sme == WIFI_ASSOCIATED) { memcpy(tx_buffer, f, 26); unsigned n = mgmt_protect(v, tx_buffer, 26); radio_tx(v, tx_buffer, n, 0); }
    else if (transmit && v->sme != WIFI_IDLE) { memcpy(tx_buffer, f, 26); radio_tx(v, tx_buffer, 26, 0); }
    bool was = v->sme != WIFI_IDLE;
    sta_teardown(v);
    if (was) sta_report_disconnect(v, f, 26, false);
}
int wifi_authenticate(struct wifi_vif* v, const uint8_t* bssid, unsigned freq, const uint8_t* ssid, unsigned ssid_len,
                      uint16_t alg, const uint8_t* data, unsigned data_len, const uint8_t* ie, unsigned ie_len) {
    if (v->iftype != NL80211_IFTYPE_STATION) return -95;
    if (!(v->dev->flags & IFF_UP)) return -100;                                   /* ENETDOWN */
    if (alg != 0 && alg != 3) return -95;                                          /* open system and SAE */
    if (ssid_len > 32 || data_len + ie_len + 30 > WIFI_FRAME_MAX || (alg == 3 && data_len < 4)) return -22;
    struct wifi_radio* r = v->radio;
    struct wifi_bss* b = bss_find(r, bssid, freq); if (!b) return -2;              /* ENOENT: scan first */
    bool sae_next = alg == 3 && v->sme == WIFI_AUTHENTICATING && !memcmp(v->bssid, bssid, 6);
    if (!sae_next && v->sme >= WIFI_AUTHENTICATED && !memcmp(v->bssid, bssid, 6)) return -114;   /* EALREADY (cfg80211) */
    if (v->sme == WIFI_ASSOCIATED && memcmp(v->bssid, bssid, 6)) sta_disconnect_local(v, 1, false);   /* mac80211: new auth */
    else if (!sae_next && v->sme != WIFI_IDLE) { v->sme = WIFI_IDLE; v->pending_len = 0; }
    if (r->scanning) wifi_scan_abort(v);
    radio_tune(r, b->freq); v->freq = b->freq;
    memcpy(v->bssid, bssid, 6); memcpy(v->ssid, ssid, ssid_len); v->ssid_len = (uint8_t)ssid_len; v->auth_alg = alg;
    uint8_t* f = v->pending; mgmt_header(v, f, 11, bssid, bssid);
    wr16(f + 24, alg); unsigned n = 26;
    if (data_len) { memcpy(f + n, data, data_len); n += data_len; }
    else { wr16(f + 26, 1); wr16(f + 28, 0); n = 30; }
    if (ie_len) { memcpy(f + n, ie, ie_len); n += ie_len; }
    v->pending_len = n; v->tries = 1; v->sme = WIFI_AUTHENTICATING;
    sta_send_pending(v);
    return 0;
}
static const uint8_t rates_24[] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24}, ext_rates_24[] = {0x30, 0x48, 0x60, 0x6C};
static const uint8_t rates_5[] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
static unsigned put_rates(uint8_t* p, unsigned freq) {
    unsigned n = 0;
    if (freq >= 5000) { p[n++] = 1; p[n++] = 8; memcpy(p + n, rates_5, 8); n += 8; return n; }
    p[n++] = 1; p[n++] = 8; memcpy(p + n, rates_24, 8); n += 8;
    p[n++] = 50; p[n++] = 4; memcpy(p + n, ext_rates_24, 4); n += 4;
    return n;
}
int wifi_associate(struct wifi_vif* v, const uint8_t* bssid, unsigned freq, const uint8_t* ssid, unsigned ssid_len,
                   const uint8_t* ie, unsigned ie_len, const uint8_t* prev_bssid, bool control_port, uint16_t ethertype,
                   bool no_encrypt, bool mfp) {
    if (v->iftype != NL80211_IFTYPE_STATION) return -95;
    if (v->sme != WIFI_AUTHENTICATED || memcmp(v->bssid, bssid, 6)) return -67;   /* ENOLINK: not authenticated */
    if (!ssid_len || ssid_len > 32 || ie_len + 96 > WIFI_FRAME_MAX) return -22;   /* header, fixed fields, SSID, rates */
    struct wifi_bss* b = bss_find(v->radio, bssid, freq); if (!b) return -2;
    uint8_t* f = v->pending; mgmt_header(v, f, prev_bssid ? 2 : 0, bssid, bssid);
    uint16_t capability = (uint16_t)(0x0001 | (b->capability & 0x0010) | 0x0400);   /* ESS, privacy as the BSS, short slot */
    wr16(f + 24, capability); wr16(f + 26, 10); unsigned n = 28;
    if (prev_bssid) { memcpy(f + n, prev_bssid, 6); n += 6; }
    f[n++] = 0; f[n++] = (uint8_t)ssid_len; memcpy(f + n, ssid, ssid_len); n += ssid_len;
    n += put_rates(f + n, b->freq);
    if (ie_len) { memcpy(f + n, ie, ie_len); n += ie_len; }
    v->pending_len = n; v->tries = 1; v->sme = WIFI_ASSOCIATING;
    v->control_port = control_port; v->control_port_ethertype = ethertype ? ethertype : ETH_P_PAE;
    v->control_port_no_encrypt = no_encrypt; v->use_mfp = mfp;
    memcpy(v->ssid, ssid, ssid_len); v->ssid_len = (uint8_t)ssid_len;
    sta_send_pending(v);
    return 0;
}
int wifi_deauthenticate(struct wifi_vif* v, const uint8_t* bssid, uint16_t reason, bool disassoc, bool local_only) {
    if (v->iftype != NL80211_IFTYPE_STATION) return -95;
    if (v->sme == WIFI_IDLE || memcmp(v->bssid, bssid, 6)) return local_only ? 0 : -107;   /* ENOTCONN */
    uint8_t f[64]; mgmt_header(v, f, disassoc ? 10 : 12, bssid, bssid); wr16(f + 24, reason ? reason : 3);
    if (!local_only) { memcpy(tx_buffer, f, 26); unsigned n = v->sme == WIFI_ASSOCIATED ? mgmt_protect(v, tx_buffer, 26) : 26; radio_tx(v, tx_buffer, n, 0); }
    sta_teardown(v);
    sta_report_disconnect(v, f, 26, false);
    return 0;
}
static void sta_auth_rx(struct wifi_vif* v, const uint8_t* f, unsigned length, const struct wifi_rx_status* st) {
    if (v->sme != WIFI_AUTHENTICATING || memcmp(f + 10, v->bssid, 6) || length < 30) return;
    uint16_t alg = rd16(f + 24), seq = rd16(f + 26), status = rd16(f + 28);
    if (alg != v->auth_alg) return;
    if (alg == 3) {                                                                  /* SAE: user space runs it */
        v->pending_len = 0;
        if (seq == 2 && status == 0) v->sme = WIFI_AUTHENTICATED;
        else if (status != 0 && status != 76 && status != 126 && status != 127) v->sme = WIFI_IDLE;   /* not anti-clogging/H2E */
        else v->deadline_ns = native_clock_ns() + 2000000000ULL;                    /* waiting for the next user frame */
        nl80211_event_frame(v, NL80211_CMD_AUTHENTICATE, f, length, st->freq, st->signal_dbm);
        return;
    }
    if (seq != 2) return;
    v->pending_len = 0; v->sme = status == 0 ? WIFI_AUTHENTICATED : WIFI_IDLE;
    nl80211_event_frame(v, NL80211_CMD_AUTHENTICATE, f, length, st->freq, st->signal_dbm);
}
static void sta_assoc_rx(struct wifi_vif* v, const uint8_t* f, unsigned length, const struct wifi_rx_status* st) {
    if (v->sme != WIFI_ASSOCIATING || memcmp(f + 10, v->bssid, 6) || length < 30) return;
    uint16_t capability = rd16(f + 24), status = rd16(f + 26), aid = (uint16_t)(rd16(f + 28) & 0x3FFF);
    if (status == 30) { v->deadline_ns = native_clock_ns() + 1000000000ULL; v->tries = 1; return; }   /* comeback later (PMF SA Query) */
    v->pending_len = 0;
    if (status == 0) {
        uint64_t now = native_clock_ns();
        v->sme = WIFI_ASSOCIATED;
        memset(&v->ap, 0, sizeof(v->ap)); v->ap.used = true; memcpy(v->ap.addr, v->bssid, 6);
        v->ap.aid = aid; v->ap.capability = capability; v->ap.connected_ns = v->ap.last_rx_ns = now; v->ap.signal = st->signal_dbm;
        v->ap.flags = STA_BIT(NL80211_STA_FLAG_AUTHENTICATED) | STA_BIT(NL80211_STA_FLAG_ASSOCIATED) |
                      (v->use_mfp ? STA_BIT(NL80211_STA_FLAG_MFP) : 0) | (v->control_port ? 0 : STA_BIT(NL80211_STA_FLAG_AUTHORIZED));
        v->last_beacon_ns = now;
        netdev_set_carrier(v->dev, true);
        radio_mode_changed(v->radio);
    } else v->sme = WIFI_IDLE;
    nl80211_event_frame(v, NL80211_CMD_ASSOCIATE, f, length, st->freq, st->signal_dbm);
}
static void sta_timers(struct wifi_vif* v, uint64_t now) {
    if ((v->sme == WIFI_AUTHENTICATING || v->sme == WIFI_ASSOCIATING) && now >= v->deadline_ns) {
        if (v->pending_len && v->tries < 3) { v->tries++; sta_send_pending(v); return; }
        uint8_t cmd = v->sme == WIFI_AUTHENTICATING ? NL80211_CMD_AUTHENTICATE : NL80211_CMD_ASSOCIATE;
        v->sme = WIFI_IDLE; v->pending_len = 0;
        nl80211_event_timeout(v, cmd, v->bssid);
        return;
    }
    if (v->sme == WIFI_ASSOCIATED && !v->radio->scanning && now > v->last_beacon_ns + BEACON_LOSS_NS)
        sta_disconnect_local(v, 4 /* inactivity */, false);                       /* connection lost */
}
static void cqm_check(struct wifi_vif* v, int signal) {
    if (!v->cqm_threshold) return;
    int low = v->cqm_threshold - (int)v->cqm_hysteresis, high = v->cqm_threshold + (int)v->cqm_hysteresis;
    if (v->cqm_state != -1 && signal < low) { v->cqm_state = -1; nl80211_event_cqm(v, true, signal); }
    else if (v->cqm_state != 1 && signal > high) { v->cqm_state = 1; nl80211_event_cqm(v, false, signal); }
}

/* ====================================================================================================
 * Access point
 * ==================================================================================================== */
struct wifi_station* wifi_station_find(struct wifi_vif* v, const uint8_t* addr) {
    if (v->iftype != NL80211_IFTYPE_AP) return v->ap.used && !memcmp(v->ap.addr, addr, 6) ? &v->ap : NULL;
    for (unsigned i = 0; i < WIFI_STA_MAX; i++) if (v->stations[i].used && !memcmp(v->stations[i].addr, addr, 6)) return &v->stations[i];
    return NULL;
}
struct wifi_station* wifi_station_add(struct wifi_vif* v, const uint8_t* addr) {
    struct wifi_station* s = wifi_station_find(v, addr); if (s) return s;
    for (unsigned i = 0; i < WIFI_STA_MAX; i++) if (!v->stations[i].used) {
        s = &v->stations[i]; memset(s, 0, sizeof(*s)); s->used = true; memcpy(s->addr, addr, 6);
        s->connected_ns = s->last_rx_ns = native_clock_ns();
        nl80211_event_station(v, NL80211_CMD_NEW_STATION, addr);
        return s;
    }
    return NULL;
}
void wifi_station_del(struct wifi_vif* v, const uint8_t* addr) {
    for (unsigned i = 0; i < WIFI_STA_MAX; i++) {
        struct wifi_station* s = &v->stations[i];
        if (!s->used || (addr && memcmp(s->addr, addr, 6))) continue;
        uint8_t gone[6]; memcpy(gone, s->addr, 6); memset(s, 0, sizeof(*s));
        nl80211_event_station(v, NL80211_CMD_DEL_STATION, gone);
    }
}
static void beacon_send(struct wifi_vif* v, uint64_t now) {
    uint8_t* f = tx_buffer; unsigned n = v->beacon_head;
    memcpy(f, v->beacon, n);
    uint64_t tsf = now / 1000; for (unsigned i = 0; i < 8; i++) f[24 + i] = (uint8_t)(tsf >> (8 * i));
    f[n++] = 5; f[n++] = 4; f[n++] = 0; f[n++] = (uint8_t)(v->dtim ? v->dtim : 1); f[n++] = 0; f[n++] = 0;   /* TIM */
    memcpy(f + n, v->beacon + v->beacon_head, v->beacon_tail); n += v->beacon_tail;
    radio_tx(v, f, n, 0);
}
static int beacon_store(struct wifi_vif* v, const uint8_t* head, unsigned head_len, const uint8_t* tail, unsigned tail_len) {
    if (head && (head_len < 36 || head_len > WIFI_IE_BYTES)) return -22;
    if (tail_len > WIFI_IE_BYTES) return -22;
    if (head && ((head[0] & 0xFC) != 0x80)) return -22;                          /* a beacon */
    if (!head) { if (!v->beacon_head) return -22; head = NULL; }
    uint8_t old[WIFI_IE_BYTES]; unsigned old_len = v->beacon_head;
    if (!head) { memcpy(old, v->beacon, old_len); head = old; head_len = old_len; }
    memcpy(v->beacon, head, head_len); v->beacon_head = head_len;
    memcpy(v->beacon + head_len, tail, tail_len); v->beacon_tail = tail_len;
    return 0;
}
int wifi_start_ap(struct wifi_vif* v, const uint8_t* head, unsigned head_len, const uint8_t* tail, unsigned tail_len,
                  unsigned interval, unsigned dtim, const uint8_t* ssid, unsigned ssid_len, bool hidden, bool privacy, unsigned freq) {
    if (v->iftype != NL80211_IFTYPE_AP) return -95;
    if (v->ap_started) return -114;                                                /* EALREADY */
    if (!head || ssid_len > 32 || (freq && !freq_known(freq))) return -22;
    if (interval && (interval < 15 || interval > 10000)) return -22;
    if (!(v->dev->flags & IFF_UP)) return -100;
    /* STA+AP concurrency on one phy: the AP must share the channel the other interface already holds (one radio). */
    struct wifi_radio* r = v->radio; struct wifi_vif* other = v == r->vif ? r->vif2 : r->vif;
    bool locked = other && ((other->iftype == NL80211_IFTYPE_STATION && other->sme == WIFI_ASSOCIATED) || (other->iftype == NL80211_IFTYPE_AP && other->ap_started));
    if (locked && freq && freq != r->freq) return -16;                             /* EBUSY: a different channel is not possible */
    int e = beacon_store(v, head, head_len, tail, tail_len); if (e) return e;
    if (!locked && r->scanning) wifi_scan_abort(v);
    if (locked) v->freq = r->freq; else { if (freq) v->freq = freq; if (!v->freq) v->freq = r->freq; }
    if (!locked) radio_tune(r, v->freq);
    v->beacon_int = (uint16_t)(interval ? interval : 100); v->dtim = (uint16_t)(dtim ? dtim : 2);
    memcpy(v->ap_ssid, ssid, ssid_len); v->ap_ssid_len = (uint8_t)ssid_len; v->hidden = hidden; v->privacy = privacy;
    v->ap_started = true; v->next_beacon_ns = native_clock_ns();
    netdev_set_carrier(v->dev, true);
    radio_mode_changed(v->radio);
    return 0;
}
int wifi_set_beacon(struct wifi_vif* v, const uint8_t* head, unsigned head_len, const uint8_t* tail, unsigned tail_len) {
    if (v->iftype != NL80211_IFTYPE_AP || !v->ap_started) return -22;
    return beacon_store(v, head, head_len, tail, tail_len);
}
int wifi_stop_ap(struct wifi_vif* v) {
    if (v->iftype != NL80211_IFTYPE_AP) return -95;
    if (!v->ap_started) return -107;                                               /* ENOTCONN, as cfg80211 */
    v->ap_started = false; v->beacon_head = v->beacon_tail = 0;
    wifi_station_del(v, NULL);
    memset(v->gtk_tx, 0, sizeof(v->gtk_tx)); memset(v->igtk_tx, 0, sizeof(v->igtk_tx)); v->default_key = v->default_mgmt_key = 0;
    netdev_set_carrier(v->dev, false);
    radio_mode_changed(v->radio);
    return 0;
}

/* ====================================================================================================
 * Keys and port control
 * ==================================================================================================== */
static struct wifi_key* key_slot(struct wifi_vif* v, const uint8_t* addr, unsigned index, bool pairwise, bool create) {
    if (pairwise) {
        if (!addr || index > 1) return NULL;
        struct wifi_station* s = v->iftype == NL80211_IFTYPE_AP ? wifi_station_find(v, addr) : (v->ap.used && !memcmp(v->ap.addr, addr, 6) ? &v->ap : NULL);
        (void)create; return s ? &s->ptk : NULL;
    }
    bool ap = v->iftype == NL80211_IFTYPE_AP;
    if (index <= 3) return ap ? &v->gtk_tx[index] : &v->gtk[index];
    if (index <= 5) return ap ? &v->igtk_tx[index - 4] : &v->igtk[index - 4];
    return NULL;
}
int wifi_set_key(struct wifi_vif* v, const uint8_t* addr, unsigned index, uint32_t cipher, const uint8_t* key, unsigned key_len,
                 const uint8_t* seq, unsigned seq_len, bool pairwise) {
    /* pairwise/group data: CCMP-128/256, GCMP-128/256; group management: BIP-CMAC-128, BIP-GMAC-128/256 */
    unsigned want_len, mic; bool group_mgmt;
    switch (cipher) {
    case WIFI_CIPHER_CCMP: want_len = 16; mic = 8; group_mgmt = false; break;
    case WIFI_CIPHER_GCMP: want_len = 16; mic = 16; group_mgmt = false; break;
    case WIFI_CIPHER_CCMP_256: case WIFI_CIPHER_GCMP_256: want_len = 32; mic = 16; group_mgmt = false; break;
    case WIFI_CIPHER_BIP_CMAC: want_len = 16; mic = 8; group_mgmt = true; break;
    case WIFI_CIPHER_BIP_GMAC_128: want_len = 16; mic = 16; group_mgmt = true; break;
    case WIFI_CIPHER_BIP_GMAC_256: want_len = 32; mic = 16; group_mgmt = true; break;
    default: return -95;
    }
    if (key_len != want_len || seq_len > 8) return -22;
    if (group_mgmt && (pairwise || index < 4 || index > 5)) return -22;
    if (!group_mgmt && !pairwise && (index < 1 || index > 3)) return -22;
    struct wifi_key* k = key_slot(v, addr, index, pairwise, true); if (!k) return -2;
    memset(k, 0, sizeof(*k)); k->set = true; k->cipher = cipher; k->index = (uint8_t)index; k->mic_len = (uint8_t)mic;
    memcpy(k->key, key, key_len); k->rounds = (uint8_t)aes_expand(k->rk, key, key_len);
    uint64_t rsc = 0; for (unsigned i = 0; i < seq_len && i < 6; i++) rsc |= (uint64_t)seq[i] << (8 * i);
    k->rx_pn = rsc; k->tx_pn = 0;
    return 0;
}
int wifi_del_key(struct wifi_vif* v, const uint8_t* addr, unsigned index, bool pairwise) {
    struct wifi_key* k = key_slot(v, addr, index, pairwise, false); if (!k) return -2;
    memset(k, 0, sizeof(*k)); return 0;
}
struct wifi_key* wifi_get_key(struct wifi_vif* v, const uint8_t* addr, unsigned index, bool pairwise) {
    struct wifi_key* k = key_slot(v, addr, index, pairwise, false);
    return k && k->set ? k : NULL;
}
void wifi_set_authorized(struct wifi_vif* v, const uint8_t* addr, bool authorized) {
    struct wifi_station* s = peer_of(v, addr); if (!s) return;
    if (authorized) s->flags |= STA_BIT(NL80211_STA_FLAG_AUTHORIZED); else s->flags &= ~STA_BIT(NL80211_STA_FLAG_AUTHORIZED);
}

/* ====================================================================================================
 * Data path: 802.3 <-> 802.11
 * ==================================================================================================== */
static const uint8_t rfc1042[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
/* Build a data frame from an Ethernet frame into tx_buffer; returns the length (0: not sendable). */
static unsigned data_frame(struct wifi_vif* v, const uint8_t* eth, unsigned length, const uint8_t* a1, const uint8_t* a2, const uint8_t* a3, bool to_ds) {
    if (length < 14 || length - 14 + 32 + 8 > WIFI_FRAME_MAX) return 0;
    uint8_t* f = tx_buffer; memset(f, 0, 24);
    f[0] = 0x08; f[1] = to_ds ? 0x01 : 0x02;
    memcpy(f + 4, a1, 6); memcpy(f + 10, a2, 6); memcpy(f + 16, a3, 6);
    memcpy(f + 24, rfc1042, 6); memcpy(f + 30, eth + 12, 2); memcpy(f + 32, eth + 14, length - 14);
    (void)v; return 32 + length - 14;
}
static int data_send(struct wifi_vif* v, unsigned n, struct wifi_key* key, struct wifi_station* peer) {
    if (key && key->set) n = pkt_protect(key, tx_buffer, n);
    int e = radio_tx(v, tx_buffer, n, 0);
    if (!e) { v->dev->stats.tx_packets++; v->dev->stats.tx_bytes += n; if (peer) { peer->tx_packets++; peer->tx_bytes += n; } }
    return e;
}
static int wlan_xmit(struct netdev* dev, const void* data, unsigned length) {
    struct wifi_vif* v = dev->priv; const uint8_t* eth = data;
    if (!v || length < 14) return -22;
    uint16_t proto = (uint16_t)(eth[12] << 8 | eth[13]);
    if (v->iftype == NL80211_IFTYPE_STATION) {
        if (v->sme != WIFI_ASSOCIATED) { dev->stats.tx_dropped++; return 0; }
        bool control = proto == v->control_port_ethertype;
        if (!control && !(v->ap.flags & STA_BIT(NL80211_STA_FLAG_AUTHORIZED))) { dev->stats.tx_dropped++; return 0; }
        if (v->radio->scanning && v->radio->freq != v->freq) { dev->stats.tx_dropped++; return 0; }   /* off channel */
        unsigned n = data_frame(v, eth, length, v->bssid, dev->addr, eth, true); if (!n) { dev->stats.tx_dropped++; return 0; }
        bool plain = control && v->control_port_no_encrypt;
        return data_send(v, n, plain ? NULL : &v->ap.ptk, &v->ap);
    }
    if (v->iftype != NL80211_IFTYPE_AP || !v->ap_started) { dev->stats.tx_dropped++; return 0; }
    if (group_addr(eth)) {
        unsigned n = data_frame(v, eth, length, eth, dev->addr, eth + 6, false); if (!n) return 0;
        struct wifi_key* k = v->default_key >= 1 && v->default_key <= 3 ? &v->gtk_tx[v->default_key] : NULL;
        if (!k || !k->set) { bool any = false; for (unsigned i = 0; i < WIFI_STA_MAX; i++) if (v->stations[i].used && v->stations[i].ptk.set) any = true; if (any) { dev->stats.tx_dropped++; return 0; } k = NULL; }
        return data_send(v, n, k, NULL);
    }
    struct wifi_station* s = wifi_station_find(v, eth);
    if (!s || !(s->flags & STA_BIT(NL80211_STA_FLAG_ASSOCIATED))) { dev->stats.tx_dropped++; return 0; }
    if (proto != ETH_P_PAE && !(s->flags & STA_BIT(NL80211_STA_FLAG_AUTHORIZED))) { dev->stats.tx_dropped++; return 0; }
    unsigned n = data_frame(v, eth, length, eth, dev->addr, eth + 6, false); if (!n) { dev->stats.tx_dropped++; return 0; }
    return data_send(v, n, &s->ptk, s);
}
static uint8_t eth_buffer[WIFI_FRAME_MAX];
static void data_rx(struct wifi_vif* v, uint8_t* f, unsigned length, const struct wifi_rx_status* st) {
    bool to_ds = f[1] & 1, from_ds = (f[1] & 2) != 0;
    const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16;
    struct wifi_station* peer; const uint8_t *dst, *src;
    if (v->iftype == NL80211_IFTYPE_STATION) {
        if (v->sme != WIFI_ASSOCIATED || to_ds || !from_ds || memcmp(a2, v->bssid, 6)) return;
        if (!group_addr(a1) && memcmp(a1, v->dev->addr, 6)) return;
        if (!memcmp(a3, v->dev->addr, 6)) return;                                 /* our own broadcast, relayed */
        peer = &v->ap; dst = a1; src = a3;
    } else if (v->iftype == NL80211_IFTYPE_AP) {
        if (!v->ap_started || !to_ds || from_ds || memcmp(a1, v->dev->addr, 6)) return;
        peer = wifi_station_find(v, a2);
        if (!peer || !(peer->flags & STA_BIT(NL80211_STA_FLAG_ASSOCIATED))) { nl80211_event_unexpected(v, a2); return; }
        dst = a3; src = a2;
    } else return;
    unsigned subtype = f[0] >> 4;
    if (subtype & 4) return;                                                         /* null data (no payload) */
    unsigned h = hdr_len(f);
    if (f[1] & 0x40) {
        struct wifi_key* k = NULL;
        if (length < h + 8) return;
        if (!group_addr(a1)) k = peer->ptk.set ? &peer->ptk : NULL;
        else if (v->iftype == NL80211_IFTYPE_STATION) { unsigned idx = f[h + 3] >> 6; k = idx && v->gtk[idx].set ? &v->gtk[idx] : NULL; }
        if (!k || pkt_unprotect(k, f, &length)) { v->dev->stats.rx_dropped++; peer->rx_dropped++; return; }
    } else if (peer->ptk.set) { v->dev->stats.rx_dropped++; peer->rx_dropped++; return; }   /* keys installed: protected only */
    if (length < h + 8 || memcmp(f + h, rfc1042, 6)) { v->dev->stats.rx_dropped++; return; }
    uint16_t proto = (uint16_t)(f[h + 6] << 8 | f[h + 7]);
    bool control = proto == (v->iftype == NL80211_IFTYPE_STATION ? v->control_port_ethertype : ETH_P_PAE);
    if (!control && !(peer->flags & STA_BIT(NL80211_STA_FLAG_AUTHORIZED))) { v->dev->stats.rx_dropped++; return; }
    unsigned payload = length - h - 8;
    uint8_t* eth = eth_buffer; memcpy(eth, dst, 6); memcpy(eth + 6, src, 6); eth[12] = f[h + 6]; eth[13] = f[h + 7];
    memcpy(eth + 14, f + h + 8, payload);
    peer->rx_packets++; peer->rx_bytes += length; peer->last_rx_ns = native_clock_ns(); peer->signal = st->signal_dbm;
    if (v->iftype == NL80211_IFTYPE_AP && !control) {                             /* bridge inside the BSS */
        struct wifi_station* other = group_addr(dst) ? NULL : wifi_station_find(v, dst);
        if (group_addr(dst) || (other && other != peer)) {
            unsigned n = data_frame(v, eth, payload + 14, dst, v->dev->addr, src, false);
            if (n) {
                if (other) { if (other->flags & STA_BIT(NL80211_STA_FLAG_AUTHORIZED)) data_send(v, n, &other->ptk, other); return; }
                struct wifi_key* k = v->default_key >= 1 && v->default_key <= 3 && v->gtk_tx[v->default_key].set ? &v->gtk_tx[v->default_key] : NULL;
                data_send(v, n, k, NULL);
            }
        }
    }
    netdev_receive(v->dev, eth, payload + 14);
}

/* ====================================================================================================
 * Receive dispatch
 * ==================================================================================================== */
static void mgmt_rx(struct wifi_vif* v, uint8_t* f, unsigned length, const struct wifi_rx_status* st) {
    unsigned subtype = f[0] >> 4; struct wifi_radio* r = v->radio;
    if (subtype == 8 || subtype == 5) {                                              /* beacon / probe response */
        if (length < 36) return;
        if (subtype == 5 && memcmp(f + 4, v->dev->addr, 6) && !group_addr(f + 4)) return;
        if (v->iftype == NL80211_IFTYPE_STATION || r->scanning) bss_update(r, f, length, st, subtype == 5);
        if (v->iftype == NL80211_IFTYPE_STATION && v->sme == WIFI_ASSOCIATED && !memcmp(f + 16, v->bssid, 6) && st->freq == v->freq) {
            v->last_beacon_ns = native_clock_ns(); v->ap.signal = st->signal_dbm; cqm_check(v, st->signal_dbm);
        }
        return;
    }
    bool group = group_addr(f + 4);
    if (!group && memcmp(f + 4, v->dev->addr, 6)) return;
    struct wifi_station* peer = peer_of(v, f + 10);
    if (f[1] & 0x40) {                                                               /* CCMP-protected (unicast robust) */
        if (group || !peer || !peer->ptk.set || pkt_unprotect(&peer->ptk, f, &length)) return;
        f[1] |= 0x40;                     /* user space sees the Protected bit of a decrypted frame (mac80211 keeps it) */
    } else if (robust_mgmt(f, length) && peer && (peer->flags & STA_BIT(NL80211_STA_FLAG_MFP)) && peer->ptk.set) {
        if (group) {
            struct wifi_key* igtk = v->iftype == NL80211_IFTYPE_AP ? v->igtk_tx : v->igtk;
            if (bip_check(igtk, 2, f, &length)) return;
        } else {
            if (v->iftype == NL80211_IFTYPE_STATION && (subtype == 10 || subtype == 12))   /* unprotected deauth/disassoc */
                nl80211_event_frame(v, subtype == 10 ? NL80211_CMD_UNPROT_DISASSOCIATE : NL80211_CMD_UNPROT_DEAUTHENTICATE, f, length, st->freq, st->signal_dbm);
            return;
        }
    }
    if (v->iftype == NL80211_IFTYPE_AP) {
        if (!v->ap_started) return;
        if (subtype != 4 && memcmp(f + 16, v->dev->addr, 6)) return;                 /* not our BSS */
        if (subtype == 4 && st->freq != v->freq) return;
        if (peer) { peer->last_rx_ns = native_clock_ns(); peer->signal = st->signal_dbm; }
        nl80211_deliver_mgmt(v, f, length, st->freq, st->signal_dbm);
        return;
    }
    if (v->iftype != NL80211_IFTYPE_STATION) return;
    switch (subtype) {
    case 11: sta_auth_rx(v, f, length, st); return;
    case 1: case 3: sta_assoc_rx(v, f, length, st); return;
    case 10: case 12:
        if (v->sme == WIFI_IDLE || memcmp(f + 10, v->bssid, 6)) return;
        sta_teardown(v);
        nl80211_event_frame(v, subtype == 10 ? NL80211_CMD_DISASSOCIATE : NL80211_CMD_DEAUTHENTICATE, f, length, st->freq, st->signal_dbm);
        return;
    default: nl80211_deliver_mgmt(v, f, length, st->freq, st->signal_dbm); return;   /* action frames (SA Query, WNM...) */
    }
}
/* Monitor mode: every frame on the channel, with a radiotap header, to packet sockets (tcpdump DLT_IEEE802_11_RADIO). */
#define ARPHRD_IEEE80211_RADIOTAP 803
static uint8_t monitor_buf[WIFI_FRAME_MAX + 16];
static void monitor_tap(struct wifi_vif* v, const uint8_t* frame, unsigned length, const struct wifi_rx_status* st) {
    if (!packet_socket_count) return;
    uint8_t* rt = monitor_buf;
    rt[0] = 0; rt[1] = 0; wr16(rt + 2, 13);                           /* it_version, it_pad, it_len: 8 header + flags,rate,signal */
    uint32_t present = (1u << 1) | (1u << 2) | (1u << 5);             /* FLAGS, RATE, DBM_ANTSIGNAL (all byte-aligned) */
    memcpy(rt + 4, &present, 4);
    rt[8] = 0;                                                        /* flags: no FCS at the end */
    rt[9] = 2;                                                        /* rate: 1 Mbps in 500 kbps units */
    rt[10] = (uint8_t)(int8_t)st->signal_dbm;                        /* antenna signal (dBm) */
    rt[11] = 0; rt[12] = 0;                                           /* pad to it_len */
    if (length > WIFI_FRAME_MAX) length = WIFI_FRAME_MAX;
    memcpy(rt + 13, frame, length);
    packet_tap(v->dev, monitor_buf, 13 + length, 0 /* PACKET_HOST */, 0, false);
}
void wifi_rx(struct wifi_radio* r, const uint8_t* frame, unsigned length, const struct wifi_rx_status* st) {
    if (length < 24 || length > WIFI_FRAME_MAX) return;
    for (unsigned vi = 0; vi < 2; vi++) {                                            /* each interface on the phy filters by its own address */
        struct wifi_vif* v = vi ? r->vif2 : r->vif;
        if (!v || !v->dev || !(v->dev->flags & IFF_UP)) continue;
        if (v->iftype == NL80211_IFTYPE_MONITOR) { monitor_tap(v, frame, length, st); continue; }
        uint8_t* f = rx_buffer; memcpy(f, frame, length);                            /* a per-vif copy (decrypt is in place) */
        unsigned type = (f[0] >> 2) & 3;
        if (type == 0) mgmt_rx(v, f, length, st);
        else if (type == 2) data_rx(v, f, length, st);
    }
}
void wifi_tx_status(struct wifi_radio* r, uint64_t cookie, bool acked) {
    (void)r;
    if (!cookie) return;
    for (struct tx_wait** link = &tx_waits; *link; link = &(*link)->next) {
        struct tx_wait* w = *link; if (w->cookie != cookie) continue;
        *link = w->next; tx_wait_count--;
        if (w->vif) nl80211_event_tx_status(w->vif, cookie, w->frame, w->length, acked);
        net_free(w, (unsigned)sizeof(*w) + w->length);
        return;
    }
}
int wifi_tx_mgmt(struct wifi_vif* v, const uint8_t* frame, unsigned length, unsigned freq, bool no_ack, uint64_t* cookie) {
    if (length < 24 || length > WIFI_FRAME_MAX - 32 || (frame[0] & 0x0C) != 0) return -22;   /* management frames only */
    if (!(v->dev->flags & IFF_UP)) return -100;
    if (freq && freq != v->radio->freq) return -16;                                  /* no off-channel TX */
    if (v->iftype == NL80211_IFTYPE_AP && !v->ap_started && (frame[0] >> 4) != 12 && (frame[0] >> 4) != 10) return -100;
    uint8_t* f = tx_buffer; memcpy(f, frame, length);
    if (!memcmp(f + 10, "\0\0\0\0\0\0", 6)) memcpy(f + 10, v->dev->addr, 6);
    if ((f[0] >> 4) == 5 && length >= 32) { uint64_t tsf = native_clock_ns() / 1000; for (unsigned i = 0; i < 8; i++) f[24 + i] = (uint8_t)(tsf >> (8 * i)); }   /* probe response TSF */
    unsigned n = mgmt_protect(v, f, length);
    uint64_t c = no_ack ? 0 : next_cookie++;
    if (c && tx_wait_count < 64) {
        struct tx_wait* w = net_alloc((unsigned)sizeof(*w) + length);
        if (w) { w->vif = v; w->cookie = c; w->length = length; memcpy(w->frame, frame, length); w->next = tx_waits; tx_waits = w; tx_wait_count++; }
    }
    int e = radio_tx(v, f, n, c);
    if (e) { wifi_tx_status(v->radio, c, false); return e; }
    if (cookie) *cookie = c;
    return 0;
}

/* ====================================================================================================
 * Scanning
 * ==================================================================================================== */
static void scan_probe(struct wifi_radio* r) {
    struct wifi_vif* v = r->scan_vif;
    for (unsigned i = 0; i < r->scan_nssids; i++) {
        uint8_t* f = tx_buffer; mgmt_header(v, f, 4, bcast, bcast); unsigned n = 24;
        unsigned len = r->scan_ssids[i][32]; f[n++] = 0; f[n++] = (uint8_t)len; memcpy(f + n, r->scan_ssids[i], len); n += len;
        n += put_rates(f + n, r->freq);
        f[n++] = 3; f[n++] = 1; f[n++] = (uint8_t)freq_channel(r->freq);           /* DS parameter set */
        memcpy(f + n, r->scan_ie, r->scan_ie_len); n += r->scan_ie_len;
        radio_tx(v, f, n, 0);
    }
}
static void scan_channel(struct wifi_radio* r) {
    radio_tune(r, r->scan_freqs[r->scan_pos]);
    scan_probe(r);
    r->scan_dwell_end = native_clock_ns() + (r->scan_nssids ? 45000000ULL : 120000000ULL);
}
static void scan_finish(struct wifi_radio* r, bool aborted) {
    struct wifi_vif* v = r->scan_vif;
    r->scanning = false; r->scan_vif = NULL; r->generation++;
    struct wifi_vif* own = r->vif;
    if (own && ((own->iftype == NL80211_IFTYPE_STATION && own->sme != WIFI_IDLE) || (own->iftype == NL80211_IFTYPE_AP && own->ap_started)))
        radio_tune(r, own->freq);
    if (own) own->last_beacon_ns = native_clock_ns();
    radio_mode_changed(r);
    if (v) nl80211_event_scan(v, aborted ? NL80211_CMD_SCAN_ABORTED : NL80211_CMD_NEW_SCAN_RESULTS);
}
int wifi_scan_start(struct wifi_vif* v, const unsigned* freqs, unsigned nfreqs, const uint8_t (*ssids)[33], unsigned nssids,
                    const uint8_t* ie, unsigned ie_len) {
    struct wifi_radio* r = v->radio;
    if (v->iftype != NL80211_IFTYPE_STATION) return -95;
    if (!(v->dev->flags & IFF_UP)) return -100;
    if (r->scanning || v->sme == WIFI_AUTHENTICATING || v->sme == WIFI_ASSOCIATING) return -16;   /* EBUSY */
    struct wifi_vif* other = v == r->vif ? r->vif2 : r->vif;                        /* a co-hosted AP holds the channel (one radio) */
    if (other && other->iftype == NL80211_IFTYPE_AP && other->ap_started) return -16;
    if (nssids > 4 || ie_len > sizeof(r->scan_ie) || nfreqs > WIFI_CHANNELS) return -22;
    unsigned count = 0;
    if (nfreqs) { for (unsigned i = 0; i < nfreqs; i++) { if (!freq_known(freqs[i])) continue; bool dup = false; for (unsigned k = 0; k < count; k++) if (r->scan_freqs[k] == freqs[i]) dup = true; if (!dup) r->scan_freqs[count++] = freqs[i]; } if (!count) return -22; }
    else for (unsigned i = 0; i < WIFI_CHANNELS; i++) r->scan_freqs[count++] = wifi_channel_freqs[i];
    r->scan_count = count; r->scan_pos = 0; r->scan_nssids = nssids;
    for (unsigned i = 0; i < nssids; i++) memcpy(r->scan_ssids[i], ssids[i], 33);
    memcpy(r->scan_ie, ie, ie_len); r->scan_ie_len = ie_len;
    r->scanning = true; r->scan_vif = v;
    radio_mode_changed(r);
    nl80211_event_scan(v, NL80211_CMD_TRIGGER_SCAN);
    scan_channel(r);
    return 0;
}
int wifi_scan_abort(struct wifi_vif* v) {
    struct wifi_radio* r = v->radio;
    if (!r->scanning) return -2;
    scan_finish(r, true); return 0;
}

/* ====================================================================================================
 * Interfaces
 * ==================================================================================================== */
static char* put_text(char* p, char* end, const char* s);
static char* put_unsigned(char* p, unsigned value);
static char* put_hex2(char* p, uint8_t v);
/* /sys attribute text for phy and wlan nodes (sysfs.c routes content WIFI_ATTR+sub here with the phy index). */
unsigned wifi_sysfs_text(unsigned sub, unsigned phy, char* out) {
    out[0] = 0; if (phy >= WIFI_RADIO_MAX) return 0;
    struct wifi_radio* r = wifi_radios[phy]; char* p = out; char* end = out + 256;
    char num[16];
    if (!r) { if (sub == 0) { put_unsigned(num, phy); p = put_text(p, end, num); } p = put_text(p, end, "\n"); return (unsigned)(p - out); }
    struct wifi_vif* v = r->vif;
    switch (sub) {
    case 0: put_unsigned(num, phy); p = put_text(p, end, num); break;
    case 1: p = put_text(p, end, r->name); break;
    case 2: for (unsigned i = 0; i < 6; i++) { p = put_hex2(p, r->mac[i]); if (i < 5) p = put_text(p, end, ":"); } break;
    case 3: p = put_text(p, end, "DEVTYPE=wlan\nINTERFACE="); p = put_text(p, end, v ? v->dev->name : "wlan"); break;
    case 4: put_unsigned(num, v ? v->dev->index : 0); p = put_text(p, end, num); break;
    case 5: for (unsigned i = 0; i < 6 && v; i++) { p = put_hex2(p, v->dev->addr[i]); if (i < 5) p = put_text(p, end, ":"); } break;
    case 6: p = put_text(p, end, "1"); break;
    default: break;
    }
    p = put_text(p, end, "\n");
    return (unsigned)(p - out);
}
static void wifi_sysfs_register(struct wifi_radio* r) {
    if (sysfs_phy_created & (1u << r->index)) return;
    char dev[64], phydir[96], path[128], link[128], netdir[96], num[16]; put_unsigned(num, r->index);
    strcpy(dev, "/sys/devices/platform/wifi"); strcat(dev, num);
    if (sysfs_add(dev, 0, 0, NULL) < 0) return;
    sysfs_phy_created |= (uint8_t)(1u << r->index);
    strcpy(phydir, dev); strcat(phydir, "/ieee80211"); sysfs_add(phydir, 0, 0, NULL);
    strcat(phydir, "/"); strcat(phydir, r->name); sysfs_add(phydir, 0, 0, NULL);
    strcpy(path, phydir); strcat(path, "/index"); sysfs_add_wifi(path, 0, r->index);
    strcpy(path, phydir); strcat(path, "/name"); sysfs_add_wifi(path, 1, r->index);
    strcpy(path, phydir); strcat(path, "/macaddress"); sysfs_add_wifi(path, 2, r->index);
    strcpy(link, "/sys/class/ieee80211/"); strcat(link, r->name); sysfs_add(link, 3, 0, phydir);
    strcpy(netdir, dev); strcat(netdir, "/net/"); strcat(netdir, r->vif->dev->name);
    strcpy(path, dev); strcat(path, "/net"); sysfs_add(path, 0, 0, NULL); sysfs_add(netdir, 0, 0, NULL);
    strcpy(path, netdir); strcat(path, "/uevent"); sysfs_add_wifi(path, 3, r->index);
    strcpy(path, netdir); strcat(path, "/ifindex"); sysfs_add_wifi(path, 4, r->index);
    strcpy(path, netdir); strcat(path, "/address"); sysfs_add_wifi(path, 5, r->index);
    strcpy(path, netdir); strcat(path, "/type"); sysfs_add_wifi(path, 6, r->index);
    strcpy(path, netdir); strcat(path, "/phy80211"); sysfs_add(path, 3, 0, phydir);
    strcpy(link, "/sys/class/net/"); strcat(link, r->vif->dev->name); sysfs_add(link, 3, 0, netdir);
}
struct wifi_vif* wifi_vif_by_dev(const struct netdev* dev) {
    if (!dev) return NULL;
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
        struct wifi_radio* r = wifi_radios[i]; if (!r) continue;
        if (r->vif && r->vif->dev == dev) return r->vif;
        if (r->vif2 && r->vif2->dev == dev) return r->vif2;
    }
    return NULL;
}
static void vif_reset(struct wifi_vif* v) {
    if (v->radio->scanning && v->radio->scan_vif == v) scan_finish(v->radio, true);
    if (v->iftype == NL80211_IFTYPE_STATION && v->sme != WIFI_IDLE) sta_disconnect_local(v, 3, (v->dev->flags & IFF_UP) != 0);
    if (v->iftype == NL80211_IFTYPE_AP && v->ap_started) wifi_stop_ap(v);
    v->sme = WIFI_IDLE; netdev_set_carrier(v->dev, false);
}
int wifi_set_iftype(struct wifi_vif* v, uint32_t iftype) {
    if (iftype != NL80211_IFTYPE_STATION && iftype != NL80211_IFTYPE_AP && iftype != NL80211_IFTYPE_MONITOR) return -95;
    if (iftype == v->iftype) return 0;
    vif_reset(v);
    memset(v->regs, 0, sizeof(v->regs));
    v->iftype = iftype; v->owner = NULL;
    v->dev->type = (uint16_t)(iftype == NL80211_IFTYPE_MONITOR ? ARPHRD_IEEE80211_RADIOTAP : ARPHRD_ETHER);   /* tcpdump DLT */
    if (iftype == NL80211_IFTYPE_MONITOR) netdev_set_carrier(v->dev, true);   /* a monitor receives whenever it is up */
    radio_mode_changed(v->radio);
    return 0;
}
void wifi_owner_closed(struct netlink_socket* nl) {
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
        struct wifi_radio* r = wifi_radios[i]; if (!r) continue;
        for (unsigned vi = 0; vi < 2; vi++) {
            struct wifi_vif* v = vi ? r->vif2 : r->vif; if (!v) continue;
            for (unsigned k = 0; k < WIFI_REG_MAX; k++) if (v->regs[k].used && v->regs[k].nl == nl) v->regs[k].used = false;
            if (v->owner == nl) { v->owner = NULL; if (vi) wifi_del_vif(v); else vif_reset(v); }
        }
    }
}
static void wlan_state(struct netdev* dev) {
    struct wifi_vif* v = dev->priv; if (!v) return;
    if (!(dev->flags & IFF_UP)) { vif_reset(v); radio_mode_changed(v->radio); }
}
static void wlan_destroy(struct netdev* dev) { (void)dev; }
static const struct netdev_ops wlan_ops = {wlan_xmit, NULL, wlan_destroy, wlan_state};

static char* put_unsigned(char* p, unsigned value) {
    char digits[12]; unsigned n = 0; do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *p++ = digits[--n];
    *p = 0; return p;
}
int wifi_radio_register(const struct wifi_driver_ops* ops, void* priv, const uint8_t mac[6], struct wifi_radio** out) {
    unsigned slot = WIFI_RADIO_MAX;
    for (unsigned i = 0; i < WIFI_RADIO_MAX && slot == WIFI_RADIO_MAX; i++) if (!wifi_radios[i]) slot = i;
    if (slot == WIFI_RADIO_MAX) return -28;
    char ifname[IFNAMSIZ]; int e = netdev_name_template("wlan%d", "wlan%d", ifname); if (e) return e;
    struct wifi_radio* r = (void*)arena_alloc(sizeof(*r)); if (!r) return -12;
    struct wifi_vif* v = (void*)arena_alloc(sizeof(*v)); if (!v) { arena_free((uint64_t)r, sizeof(*r)); return -12; }
    memset(r, 0, sizeof(*r)); memset(v, 0, sizeof(*v));
    struct netdev* dev = netdev_create(ifname, ARPHRD_ETHER, &wlan_ops, v);
    if (!dev) { arena_free((uint64_t)v, sizeof(*v)); arena_free((uint64_t)r, sizeof(*r)); return -12; }
    r->used = true; r->index = slot; strcpy(r->name, "phy"); put_unsigned(r->name + 3, slot);
    memcpy(r->mac, mac, 6); r->ops = ops; r->priv = priv; r->freq = 2412; r->sim_signal = -40;
    r->frag_threshold = r->rts_threshold = 0xFFFFFFFFU; r->retry_short = 7; r->retry_long = 4;
    dev->mtu = 1500; dev->max_mtu = 2304; dev->mtu6 = 1500; dev->conf6.mtu = 1500; dev->carrier = false;
    memcpy(dev->addr, mac, 6); memcpy(dev->perm_addr, mac, 6);
    v->radio = r; v->dev = dev; v->iftype = NL80211_IFTYPE_STATION; v->wdev = (uint64_t)slot << 32 | 1;
    v->control_port_ethertype = ETH_P_PAE; v->freq = 0;
    r->vif = v; wifi_radios[slot] = r; radio_count++;
    wifi_sysfs_register(r);
    netproc_conf_defaults(dev);
    rtnl_notify_link(dev, 16 /* RTM_NEWLINK */);
    nl80211_event_iface(v, NL80211_CMD_NEW_INTERFACE);
    *out = r; return 0;
}
int wifi_add_vif(struct wifi_radio* r, uint32_t iftype, const char* want_name, struct wifi_vif** out) {
    if (r->vif2) return -16;                                                         /* one extra vif per phy */
    if (iftype != NL80211_IFTYPE_STATION && iftype != NL80211_IFTYPE_AP && iftype != NL80211_IFTYPE_MONITOR) return -95;
    char ifname[IFNAMSIZ]; int e = netdev_name_template(want_name && want_name[0] ? want_name : "wlan%d", "wlan%d", ifname); if (e) return e;
    struct wifi_vif* v = (void*)arena_alloc(sizeof(*v)); if (!v) return -12;
    memset(v, 0, sizeof(*v));
    uint8_t mac[6]; memcpy(mac, r->mac, 6); mac[0] |= 0x02; mac[5] ^= 0x80;           /* a distinct locally-administered address */
    struct netdev* dev = netdev_create(ifname, iftype == NL80211_IFTYPE_MONITOR ? ARPHRD_IEEE80211_RADIOTAP : ARPHRD_ETHER, &wlan_ops, v);
    if (!dev) { arena_free((uint64_t)v, sizeof(*v)); return -12; }
    dev->mtu = 1500; dev->max_mtu = 2304; dev->mtu6 = 1500; dev->conf6.mtu = 1500; dev->carrier = false;
    memcpy(dev->addr, mac, 6); memcpy(dev->perm_addr, mac, 6);
    v->radio = r; v->dev = dev; v->iftype = iftype; v->wdev = (uint64_t)r->index << 32 | 2;
    v->control_port_ethertype = ETH_P_PAE; v->freq = 0;
    r->vif2 = v;
    netproc_conf_defaults(dev);
    rtnl_notify_link(dev, 16 /* RTM_NEWLINK */);
    nl80211_event_iface(v, NL80211_CMD_NEW_INTERFACE);
    if (out) *out = v;
    return 0;
}
int wifi_del_vif(struct wifi_vif* v) {
    struct wifi_radio* r = v->radio; if (v != r->vif2) return -22;                   /* the primary vif cannot be removed */
    vif_reset(v);
    nl80211_event_iface(v, NL80211_CMD_DEL_INTERFACE);
    for (struct tx_wait* w = tx_waits; w; w = w->next) if (w->vif == v) w->vif = NULL;
    struct netdev* dev = v->dev; dev->priv = NULL;
    r->vif2 = NULL;
    netdev_unregister(dev);
    arena_free((uint64_t)v, sizeof(*v));
    return 0;
}
void wifi_radio_unregister(struct wifi_radio* r) {
    if (r->vif2) wifi_del_vif(r->vif2);
    struct wifi_vif* v = r->vif;
    if (v) {
        vif_reset(v);
        nl80211_event_iface(v, NL80211_CMD_DEL_INTERFACE);
        for (struct tx_wait* w = tx_waits; w; w = w->next) if (w->vif == v) w->vif = NULL;
        struct netdev* dev = v->dev; dev->priv = NULL;
        netdev_unregister(dev);                                                    /* (drops the creation reference) */
        arena_free((uint64_t)v, sizeof(*v));
    }
    if (r->ops->destroy) r->ops->destroy(r);
    wifi_radios[r->index] = NULL; radio_count--;
    arena_free((uint64_t)r, sizeof(*r));
}

/* ====================================================================================================
 * Timers
 * ==================================================================================================== */
void wifi_poll(uint64_t now) {
    if (!radio_count) return;
    wifi_rx_flush(); now = native_clock_ns();   /* (the flush may have stamped later times) */
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
        struct wifi_radio* r = wifi_radios[i]; if (!r) continue;
        if (r->scanning && now >= r->scan_dwell_end) { if (++r->scan_pos < r->scan_count) scan_channel(r); else scan_finish(r, false); }
        for (unsigned vi = 0; vi < 2; vi++) {
            struct wifi_vif* v = vi ? r->vif2 : r->vif; if (!v || !(v->dev->flags & IFF_UP)) continue;
            if (v->iftype == NL80211_IFTYPE_STATION) sta_timers(v, now);
            else if (v->ap_started && now >= v->next_beacon_ns) {
                if (!r->scanning) beacon_send(v, now);
                v->next_beacon_ns = now + (uint64_t)v->beacon_int * 1024000ULL;
            }
        }
        if (!(now & 0x3F)) bss_expire(r, now);
    }
}

/* ====================================================================================================
 * The virtual medium (hwsim): every radio on a channel hears every other radio there
 * ==================================================================================================== */
struct sim_frame { struct sim_frame* next; struct wifi_radio* from; unsigned freq, length; int signal; uint64_t cookie; uint8_t data[]; };
static struct sim_frame *sim_head, *sim_tail; static unsigned sim_queued;
static uint64_t sim_frames, sim_dropped, sim_protected, sim_plain;   /* data frames with / without the Protected bit */
static int sim_tx(struct wifi_radio* r, const uint8_t* frame, unsigned length, uint64_t cookie) {
    if (sim_queued >= 512) { sim_dropped++; return -11; }
    struct sim_frame* s = net_alloc((unsigned)sizeof(*s) + length); if (!s) { sim_dropped++; return -12; }
    s->next = NULL; s->from = r; s->freq = r->freq; s->length = length; s->signal = r->sim_signal; s->cookie = cookie;
    memcpy(s->data, frame, length);
    if (sim_tail) sim_tail->next = s; else sim_head = s;
    sim_tail = s; sim_queued++;
    native_network_kick();
    return 0;
}
static int sim_set_channel(struct wifi_radio* r, unsigned freq) { (void)r; (void)freq; return 0; }
/* The earliest softmac deadline (tickless core): scan dwell, auth/assoc retry, AP beacon, STA beacon loss; ~0 when idle. */
uint64_t wifi_next_event(uint64_t now) {
    if (!radio_count) return ~0ULL;
    if (sim_head) return now;                                                       /* frames on the medium */
    uint64_t next = ~0ULL;
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
        struct wifi_radio* r = wifi_radios[i]; if (!r) continue;
        if (r->scanning && r->scan_dwell_end < next) next = r->scan_dwell_end;
        for (unsigned vi = 0; vi < 2; vi++) {
            struct wifi_vif* v = vi ? r->vif2 : r->vif; if (!v || !(v->dev->flags & IFF_UP)) continue;
            if (v->iftype == NL80211_IFTYPE_STATION) {
                if ((v->sme == WIFI_AUTHENTICATING || v->sme == WIFI_ASSOCIATING) && v->deadline_ns < next) next = v->deadline_ns;
                if (v->sme == WIFI_ASSOCIATED && !r->scanning && v->last_beacon_ns + BEACON_LOSS_NS + 1 < next) next = v->last_beacon_ns + BEACON_LOSS_NS + 1;
            } else if (v->ap_started && v->next_beacon_ns < next) next = v->next_beacon_ns;
        }
    }
    return next;
}
static void sim_set_mode(struct wifi_radio* r) { (void)r; }
static void sim_destroy(struct wifi_radio* r) {
    for (struct sim_frame* s = sim_head; s; s = s->next) if (s->from == r) s->from = NULL;
}
static const struct wifi_driver_ops sim_ops = {sim_tx, sim_set_channel, sim_set_mode, sim_destroy};
bool wifi_rx_pending(void) { return sim_head != NULL; }
void wifi_rx_flush(void) {
    while (sim_head) {
        struct sim_frame* list = sim_head; sim_head = sim_tail = NULL; sim_queued = 0;
        while (list) {
            struct sim_frame* s = list; list = s->next;
            bool acked = group_addr(s->data + 4);
            if (s->from && !s->from->sim_off) {
                sim_frames++;
                if ((s->data[0] & 0x0C) == 0x08 && !(s->data[0] & 0x40)) { if (s->data[1] & 0x40) sim_protected++; else sim_plain++; }
                struct wifi_rx_status st = {s->freq, s->signal, 0};
                for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
                    struct wifi_radio* r = wifi_radios[i];
                    if (!r || r == s->from || r->ops != &sim_ops || r->freq != s->freq || r->sim_off || !r->vif) continue;
                    if (!acked && ((r->vif && !memcmp(s->data + 4, r->vif->dev->addr, 6) && (r->vif->dev->flags & IFF_UP)) ||
                                   (r->vif2 && !memcmp(s->data + 4, r->vif2->dev->addr, 6) && (r->vif2->dev->flags & IFF_UP)))) acked = true;
                    wifi_rx(r, s->data, s->length, &st);
                }
            } else acked = false;
            if (s->from && s->cookie) wifi_tx_status(s->from, s->cookie, acked);
            net_free(s, (unsigned)sizeof(*s) + s->length);
        }
    }
}
static struct wifi_radio* sim_radio(const char* name) {
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
        struct wifi_radio* r = wifi_radios[i]; if (!r) continue;
        if (!strcmp(r->name, name) || (r->vif && !strcmp(r->vif->dev->name, name))) return r;
    }
    return NULL;
}
static const char* word(const char* p, char* out, unsigned capacity) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;                               /* (',' separates too: shell-free writers) */
    unsigned n = 0; while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ',') { if (n + 1 < capacity) out[n++] = *p; p++; }
    out[n] = 0; return p;
}
static bool parse_int(const char* s, int* out) {
    bool negative = *s == '-'; if (negative) s++;
    if (!*s) return false;
    int value = 0; while (*s) { if (*s < '0' || *s > '9' || value > 100000) return false; value = value * 10 + (*s++ - '0'); }
    *out = negative ? -value : value; return true;
}
static unsigned sim_serial;
int wifi_sim_command(const char* text) {
    char verb[16], a[IFNAMSIZ + 4]; static char b[256]; const char* p = word(text, verb, sizeof(verb));
    p = word(p, a, sizeof(a)); p = word(p, b, sizeof(b));
    if (!strcmp(verb, "add")) {
        int count = 1; if (a[0] && (!parse_int(a, &count) || count < 1 || count > WIFI_RADIO_MAX)) return -22;
        for (int i = 0; i < count; i++) {
            uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, (uint8_t)(sim_serial >> 8), (uint8_t)sim_serial}; sim_serial++;
            struct wifi_radio* r; int e = wifi_radio_register(&sim_ops, NULL, mac, &r); if (e) return e;
        }
        return 0;
    }
    if (!strcmp(verb, "selftest")) { wifi_selftest(); return 0; }
    struct wifi_radio* r = sim_radio(a); if (!r) return -19;
    if (!strcmp(verb, "del")) { if (r->ops != &sim_ops) return -95; wifi_radio_unregister(r); return 0; }
    if (!strcmp(verb, "signal")) { int dbm; if (!parse_int(b, &dbm) || dbm < -100 || dbm > -10) return -22; r->sim_signal = dbm; return 0; }
    if (!strcmp(verb, "inject")) {                                                  /* inject RADIO HEX: a raw frame from that radio */
        static uint8_t frame[128]; unsigned n = 0;
        for (const char* h = b; h[0] && h[1]; h += 2) {
            int hi = h[0] <= '9' ? h[0] - '0' : (h[0] | 32) - 'a' + 10, lo = h[1] <= '9' ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
            if (hi < 0 || hi > 15 || lo < 0 || lo > 15 || n >= sizeof(frame)) return -22;
            frame[n++] = (uint8_t)(hi << 4 | lo);
        }
        if (!n) return -22;
        return sim_tx(r, frame, n, 0);
    }
    if (!strcmp(verb, "tune")) {                                                    /* tune RADIO FREQ: an idle radio's channel */
        int freq; if (!parse_int(b, &freq) || !freq_known((unsigned)freq)) return -22;
        struct wifi_vif* v = r->vif;
        if (r->scanning || (v && ((v->iftype == NL80211_IFTYPE_STATION && v->sme != WIFI_IDLE) || (v->iftype == NL80211_IFTYPE_AP && v->ap_started)))) return -16;
        radio_tune(r, (unsigned)freq); return 0;
    }
    if (!strcmp(verb, "range")) { if (!strcmp(b, "on")) r->sim_off = false; else if (!strcmp(b, "off")) r->sim_off = true; else return -22; return 0; }
    return -22;
}
static char* put_text(char* p, char* end, const char* s) { while (*s && p + 1 < end) *p++ = *s++; *p = 0; return p; }
static char* put_hex2(char* p, uint8_t v) { const char* h = "0123456789abcdef"; p[0] = h[v >> 4]; p[1] = h[v & 15]; p[2] = 0; return p + 2; }
unsigned wifi_sim_text(char* out, unsigned capacity) {
    char* p = out; char* end = out + capacity; *p = 0;
    static const char* const states[] = {"idle", "authenticating", "authenticated", "associating", "associated"};
    for (unsigned i = 0; i < WIFI_RADIO_MAX && end - p > 200; i++) {
        struct wifi_radio* r = wifi_radios[i]; if (!r || !r->vif) continue; struct wifi_vif* v = r->vif;
        char num[16];
        p = put_text(p, end, r->name); p = put_text(p, end, " "); p = put_text(p, end, v->dev->name); p = put_text(p, end, " ");
        for (unsigned k = 0; k < 6; k++) { p = put_hex2(p, v->dev->addr[k]); if (k < 5) p = put_text(p, end, ":"); }
        p = put_text(p, end, " freq "); put_unsigned(num, r->freq); p = put_text(p, end, num);
        p = put_text(p, end, " signal -"); put_unsigned(num, (unsigned)-r->sim_signal); p = put_text(p, end, num);
        p = put_text(p, end, r->sim_off ? " range off" : " range on");
        p = put_text(p, end, v->iftype == NL80211_IFTYPE_AP ? " mode ap " : " mode station ");
        p = put_text(p, end, v->iftype == NL80211_IFTYPE_AP ? (v->ap_started ? "started" : "stopped") : states[v->sme]);
        if (v->iftype == NL80211_IFTYPE_AP) { unsigned n = 0; for (unsigned k = 0; k < WIFI_STA_MAX; k++) if (v->stations[k].used) n++; p = put_text(p, end, " stations "); put_unsigned(num, n); p = put_text(p, end, num); }
        else if (v->sme != WIFI_IDLE) { p = put_text(p, end, " bssid "); for (unsigned k = 0; k < 6; k++) { p = put_hex2(p, v->bssid[k]); if (k < 5) p = put_text(p, end, ":"); } }
        p = put_text(p, end, "\n");
    }
    char num[24];
    p = put_text(p, end, "medium frames "); put_unsigned(num, (unsigned)sim_frames); p = put_text(p, end, num);
    p = put_text(p, end, " dropped "); put_unsigned(num, (unsigned)sim_dropped); p = put_text(p, end, num);
    p = put_text(p, end, " data-protected "); put_unsigned(num, (unsigned)sim_protected); p = put_text(p, end, num);
    p = put_text(p, end, " data-plain "); put_unsigned(num, (unsigned)sim_plain); p = put_text(p, end, num);
    {   /* the tickless getters (power2's core asks them): time to the next deadline */
        uint64_t now = native_clock_ns(), w = wifi_next_event(now), m = mc_next_event(now);
        p = put_text(p, end, "\nnext-event wifi "); if (w == ~0ULL) p = put_text(p, end, "none"); else { put_unsigned(num, w > now ? (unsigned)((w - now) / 1000000ULL) : 0); p = put_text(p, end, num); p = put_text(p, end, "ms"); }
        p = put_text(p, end, " mcast "); if (m == ~0ULL) p = put_text(p, end, "none"); else { put_unsigned(num, m > now ? (unsigned)((m - now) / 1000000ULL) : 0); p = put_text(p, end, num); p = put_text(p, end, "ms"); }
    }
    p = put_text(p, end, "\nselftest aes "); p = put_text(p, end, !selftest_result ? "not-run" : (selftest_result & (1 | 32)) == (1 | 32) ? "ok" : "FAIL");
    p = put_text(p, end, " cmac "); p = put_text(p, end, !selftest_result ? "not-run" : (selftest_result & 2) ? "ok" : "FAIL");
    p = put_text(p, end, " ccmp "); p = put_text(p, end, !selftest_result ? "not-run" : (selftest_result & 28) == 28 ? "ok" : "FAIL");
    p = put_text(p, end, " gcmp "); p = put_text(p, end, !selftest_result ? "not-run" : (selftest_result & (64 | 128 | 256 | 512)) == (64 | 128 | 256 | 512) ? "ok" : "FAIL");
    p = put_text(p, end, "\n");
    return (unsigned)(p - out);
}
