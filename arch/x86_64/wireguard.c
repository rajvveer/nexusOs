/* In-kernel WireGuard (https://www.wireguard.com/protocol/), configured by Debian's wireguard-tools over generic
 * netlink (the "wireguard" family behind nlctrl) and created with `ip link add wg0 type wireguard`.
 *  - Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s handshakes as initiator and responder (wgcrypto.c primitives, RFC test
 *    vectors checked at boot), mac1 on every handshake message, TAI64N timestamps with replay rejection.
 *  - Transport data: ChaCha20-Poly1305 with the 64-bit counter nonce, 16-byte padding, a 64-packet replay window,
 *    cryptokey routing (outgoing: longest allowed-IP prefix picks the peer; incoming: the inner source must be one of
 *    the sending peer's allowed IPs), endpoint roaming, keepalives (passive and persistent).
 *  - Timers: handshake retransmission every 5 s (REKEY_TIMEOUT) for up to 90 s, rekey after 120 s / 2^60 messages,
 *    keys rejected after 180 s; packets wait in a small per-peer queue while a handshake runs.
 *  - The device fwmark marks the encapsulated UDP, so `ip rule not fwmark M table T` + a default route through wg0
 *    (wg-quick's policy routing) sends everything through the tunnel except the tunnel itself.
 * Not implemented: cookie replies / under-load mode (mac2 is never demanded; received cookie messages are ignored).
 * Everything runs under the kernel lock (UDP delivery, the xmit path, socket calls and netdev_poll). */
#include "nlmsg.h"
#include "wgcrypto.h"
#include "../../kernel/string.h"

#define WG_DEVICES 4
#define WG_PEERS 16
#define WG_ALLOWED 16
#define WG_QUEUE 16
#define REKEY_TIMEOUT_NS 5000000000ULL
#define REKEY_ATTEMPT_NS 90000000000ULL
#define REKEY_AFTER_NS 120000000000ULL
#define REJECT_AFTER_NS 180000000000ULL
#define KEEPALIVE_NS 10000000000ULL
#define REKEY_AFTER_MESSAGES (1ULL << 60)
#define MSG_INITIATION 1
#define MSG_RESPONSE 2
#define MSG_COOKIE 3
#define MSG_DATA 4

static const char CONSTRUCTION[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
static const char IDENTIFIER[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";
static const char LABEL_MAC1[] = "mac1----";

struct wg_keypair {
    bool valid, initiator, confirmed;
    uint8_t send[32], recv[32];
    uint32_t local_index, remote_index;
    uint64_t send_counter, recv_max, recv_window, created_ns;
};
struct wg_handshake {
    int state;                          /* 0 none, 1 initiation sent, 2 response sent (awaiting the first data) */
    uint8_t e_priv[32], e_pub[32], remote_e[32], chain[32], hash[32];
    uint32_t local_index, remote_index;
    uint64_t sent_ns, started_ns;
};
struct wg_allowed { uint8_t family, cidr; struct ip6a addr; };
struct wg_peer {
    bool used;
    uint8_t public_key[32], psk[32], static_static[32];
    struct ip6a endpoint; uint16_t endpoint_port; bool has_endpoint;
    struct wg_allowed allowed[WG_ALLOWED]; unsigned allowed_count;
    uint16_t keepalive_s;
    uint64_t last_handshake_rt, rx_bytes, tx_bytes, last_sent_ns, last_received_ns, last_data_received_ns, last_keepalive_ns;
    uint8_t greatest_timestamp[12];
    struct wg_keypair current, previous, next;
    struct wg_handshake hs;
    uint8_t* queue[WG_QUEUE]; unsigned queue_length[WG_QUEUE], queued;
};
struct wg_device {
    struct netdev* dev;
    bool has_private; uint8_t private_key[32], public_key[32], mac1_key[32];
    uint16_t listen_port; uint32_t fwmark;
    struct wg_peer peers[WG_PEERS];
    unsigned sending;                   /* re-entry guard: a tunnel routed into itself */
};
static struct wg_device* devices[WG_DEVICES];

/* ---- Noise helpers ---- */
static void hash2(uint8_t out[32], const void* a, unsigned al, const void* b, unsigned bl) {
    struct blake2s_state s; blake2s_init(&s, 32, NULL, 0); blake2s_update(&s, a, al); if (bl) blake2s_update(&s, b, bl); blake2s_final(&s, out);
}
static void mix_hash(uint8_t h[32], const void* data, unsigned length) { uint8_t t[32]; hash2(t, h, 32, data, length); memcpy(h, t, 32); }
/* HKDF over HMAC-BLAKE2s: up to three 32-byte outputs */
static void kdf(uint8_t* t1, uint8_t* t2, uint8_t* t3, const uint8_t key[32], const void* input, unsigned length) {
    uint8_t secret[32], buffer[33];
    hmac_blake2s(secret, key, 32, input, length);
    uint8_t one = 1; uint8_t out1[32]; hmac_blake2s(out1, secret, 32, &one, 1);
    if (t1) memcpy(t1, out1, 32);
    if (t2 || t3) {
        memcpy(buffer, out1, 32); buffer[32] = 2; uint8_t out2[32]; hmac_blake2s(out2, secret, 32, buffer, 33);
        if (t2) memcpy(t2, out2, 32);
        if (t3) { memcpy(buffer, out2, 32); buffer[32] = 3; hmac_blake2s(t3, secret, 32, buffer, 33); }
        wg_memzero(out2, 32);
    }
    wg_memzero(secret, 32); wg_memzero(out1, 32); wg_memzero(buffer, 33);
}
static void initial_chain(uint8_t chain[32], uint8_t hash[32], const uint8_t responder_public[32]) {
    hash2(chain, CONSTRUCTION, sizeof(CONSTRUCTION) - 1, NULL, 0);
    hash2(hash, chain, 32, IDENTIFIER, sizeof(IDENTIFIER) - 1);
    mix_hash(hash, responder_public, 32);
}
static void mac1_key_for(uint8_t key[32], const uint8_t public_key[32]) { hash2(key, LABEL_MAC1, 8, public_key, 32); }
static void put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_le64(uint8_t* p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t le64(const uint8_t* p) { uint64_t v = 0; for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }
static void tai64n(uint8_t out[12]) {
    uint64_t now = native_realtime_ns(), seconds = 0x400000000000000AULL + now / 1000000000ULL;
    uint32_t nanos = (uint32_t)(now % 1000000000ULL) & ~0xFFFFFU;   /* coarsened like Linux (no timing oracle) */
    for (unsigned i = 0; i < 8; i++) out[i] = (uint8_t)(seconds >> (56 - 8 * i));
    out[8] = (uint8_t)(nanos >> 24); out[9] = (uint8_t)(nanos >> 16); out[10] = (uint8_t)(nanos >> 8); out[11] = (uint8_t)nanos;
}
static bool random_bytes(void* out, unsigned length) {
    if (native_random(out, length)) return true;
    uint8_t* p = out; uint64_t x = native_tsc() ^ native_clock_ns();
    for (unsigned i = 0; i < length; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; p[i] = (uint8_t)x; }
    return true;
}
static void generate_ephemeral(uint8_t priv[32], uint8_t pub[32]) {
    random_bytes(priv, 32); priv[0] &= 248; priv[31] = (uint8_t)((priv[31] & 127) | 64); x25519_public(pub, priv);
}
static bool zero32(const uint8_t* k) { uint8_t acc = 0; for (unsigned i = 0; i < 32; i++) acc |= k[i]; return !acc; }

/* ---- device and peer lookup ---- */
static struct wg_device* device_of(struct netdev* dev) {
    for (unsigned i = 0; i < WG_DEVICES; i++) if (devices[i] && devices[i]->dev == dev) return devices[i];
    return NULL;
}
static struct wg_peer* peer_by_key(struct wg_device* w, const uint8_t key[32]) {
    for (unsigned i = 0; i < WG_PEERS; i++) if (w->peers[i].used && wg_equal(w->peers[i].public_key, key, 32)) return &w->peers[i];
    return NULL;
}
static bool prefix_match(const struct wg_allowed* a, const struct ip6a* addr, bool v4) {
    if (v4 != (a->family == AF_INET)) return false;
    return v4 ? ip6a_prefix_eq(addr, &a->addr, 96 + a->cidr) : ip6a_prefix_eq(addr, &a->addr, a->cidr);
}
/* cryptokey routing: the peer whose allowed IPs hold the longest prefix of `addr` */
static struct wg_peer* peer_for(struct wg_device* w, const struct ip6a* addr, bool v4) {
    struct wg_peer* best = NULL; int best_cidr = -1;
    for (unsigned i = 0; i < WG_PEERS; i++) {
        struct wg_peer* p = &w->peers[i]; if (!p->used) continue;
        for (unsigned k = 0; k < p->allowed_count; k++)
            if (prefix_match(&p->allowed[k], addr, v4) && (int)p->allowed[k].cidr > best_cidr) { best = p; best_cidr = p->allowed[k].cidr; }
    }
    return best;
}
static uint32_t new_index(void) {
    for (;;) {
        uint32_t v; random_bytes(&v, 4); if (!v) continue;
        bool used = false;
        for (unsigned d = 0; d < WG_DEVICES && !used; d++) if (devices[d]) for (unsigned i = 0; i < WG_PEERS; i++) {
            struct wg_peer* p = &devices[d]->peers[i];
            if (p->used && (p->hs.local_index == v || p->current.local_index == v || p->previous.local_index == v || p->next.local_index == v)) used = true;
        }
        if (!used) return v;
    }
}

/* ---- UDP transport of WireGuard messages ---- */
int ip4_output(uint32_t src, uint32_t dst, uint8_t protocol, const void* payload, unsigned length, const struct ip6_send_options* options);
static int send_udp(struct wg_device* w, struct wg_peer* p, const uint8_t* message, unsigned length) {
    if (!p->has_endpoint || !w->listen_port) return -89;   /* EDESTADDRREQ */
    if (w->sending > 2) return -40;                          /* ELOOP: the endpoint routes through this tunnel */
    uint8_t* buffer = (void*)arena_alloc(8 + length); if (!buffer) return -12;
    put16(buffer, w->listen_port); put16(buffer + 2, p->endpoint_port); put16(buffer + 4, (uint16_t)(8 + length)); buffer[6] = buffer[7] = 0;
    memcpy(buffer + 8, message, length);
    struct ip6_send_options o; ip6_options_default(&o); o.mark = w->fwmark;
    int result; w->sending++;
    if (ip6a_is4(&p->endpoint)) {
        uint32_t dst = ip6a_v4(&p->endpoint); struct route_result r; uint32_t src = 0;
        if (!route_lookup(AF_INET, &p->endpoint, NULL, 0, w->fwmark, &r) && ip6a_is4(&r.source)) src = ip6a_v4(&r.source);
        uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + IPPROTO_UDP + 8 + length;
        uint16_t c = net_fold(net_sum(sum, buffer, 8 + length)); put16(buffer + 6, c ? c : 0xFFFF);
        result = ip4_output(src, dst, IPPROTO_UDP, buffer, 8 + length, &o);
    } else {
        struct route_result r; struct ip6a src;
        if (route_lookup(AF_INET6, &p->endpoint, NULL, ip6a_linklocal(&p->endpoint) ? 0 : 0, w->fwmark, &r)) { result = -101; goto done; }
        src = r.source; o.source = &src;
        uint16_t sum = ip6_checksum(&src, &p->endpoint, IPPROTO_UDP, buffer, 8 + length); put16(buffer + 6, sum ? sum : 0xFFFF);
        result = ip6_send(&p->endpoint, IPPROTO_UDP, buffer, 8 + length, &o);
    }
done:
    w->sending--;
    arena_free((uint64_t)buffer, 8 + length);
    if (result >= 0) { p->tx_bytes += length; p->last_sent_ns = native_clock_ns(); }
    return result;
}

/* ---- handshake: initiator ---- */
static void send_initiation(struct wg_device* w, struct wg_peer* p) {
    if (!w->has_private || !p->has_endpoint) return;
    uint64_t now = native_clock_ns();
    struct wg_handshake* hs = &p->hs;
    if (hs->state != 1 || !hs->started_ns) hs->started_ns = now;
    uint8_t msg[148]; memset(msg, 0, sizeof(msg));
    msg[0] = MSG_INITIATION; hs->local_index = new_index(); put_le32(msg + 4, hs->local_index);
    initial_chain(hs->chain, hs->hash, p->public_key);
    generate_ephemeral(hs->e_priv, hs->e_pub);
    memcpy(msg + 8, hs->e_pub, 32);
    kdf(hs->chain, NULL, NULL, hs->chain, hs->e_pub, 32);
    mix_hash(hs->hash, hs->e_pub, 32);
    uint8_t dh[32], key[32];
    x25519(dh, hs->e_priv, p->public_key); kdf(hs->chain, key, NULL, hs->chain, dh, 32);
    chacha20poly1305_encrypt(msg + 40, w->public_key, 32, hs->hash, 32, 0, key);
    mix_hash(hs->hash, msg + 40, 48);
    kdf(hs->chain, key, NULL, hs->chain, p->static_static, 32);
    uint8_t stamp[12]; tai64n(stamp);
    chacha20poly1305_encrypt(msg + 88, stamp, 12, hs->hash, 32, 0, key);
    mix_hash(hs->hash, msg + 88, 28);
    uint8_t mkey[32]; mac1_key_for(mkey, p->public_key);
    blake2s(msg + 116, 16, mkey, 32, msg, 116);
    wg_memzero(dh, 32); wg_memzero(key, 32);
    hs->state = 1; hs->sent_ns = now;
    send_udp(w, p, msg, sizeof(msg));
}
static void derive_keypair(struct wg_peer* p, bool initiator) {
    struct wg_handshake* hs = &p->hs; uint8_t t1[32], t2[32];
    kdf(t1, t2, NULL, hs->chain, NULL, 0);
    struct wg_keypair* k = initiator ? &p->current : &p->next;
    if (initiator) { p->previous = p->current; }
    memset(k, 0, sizeof(*k));
    k->valid = true; k->initiator = initiator; k->confirmed = initiator;
    memcpy(k->send, initiator ? t1 : t2, 32); memcpy(k->recv, initiator ? t2 : t1, 32);
    k->local_index = hs->local_index; k->remote_index = hs->remote_index; k->created_ns = native_clock_ns();
    wg_memzero(t1, 32); wg_memzero(t2, 32);
    wg_memzero(hs->e_priv, 32); wg_memzero(hs->chain, 32); hs->state = 0; hs->started_ns = 0;
    p->last_handshake_rt = native_realtime_ns();
}
static void flush_queue(struct wg_device* w, struct wg_peer* p);
static bool consume_response(struct wg_device* w, const uint8_t* msg, unsigned length, const struct inet_rx_info* info) {
    if (length != 92) return false;
    uint8_t mac[16]; blake2s(mac, 16, w->mac1_key, 32, msg, 60);
    if (!wg_equal(mac, msg + 60, 16)) return false;
    uint32_t receiver = le32(msg + 8);
    struct wg_peer* p = NULL;
    for (unsigned i = 0; i < WG_PEERS; i++) if (w->peers[i].used && w->peers[i].hs.state == 1 && w->peers[i].hs.local_index == receiver) p = &w->peers[i];
    if (!p) return false;
    struct wg_handshake hs = p->hs;   /* work on a copy: a forged response must not corrupt the pending state */
    const uint8_t* e_r = msg + 12;
    kdf(hs.chain, NULL, NULL, hs.chain, e_r, 32); mix_hash(hs.hash, e_r, 32);
    uint8_t dh[32]; x25519(dh, hs.e_priv, e_r); kdf(hs.chain, NULL, NULL, hs.chain, dh, 32);
    x25519(dh, w->private_key, e_r); kdf(hs.chain, NULL, NULL, hs.chain, dh, 32);
    uint8_t tau[32], key[32]; kdf(hs.chain, tau, key, hs.chain, p->psk, 32);
    mix_hash(hs.hash, tau, 32);
    uint8_t empty[1];
    bool ok = chacha20poly1305_decrypt(empty, msg + 44, 16, hs.hash, 32, 0, key);
    wg_memzero(dh, 32); wg_memzero(tau, 32); wg_memzero(key, 32);
    if (!ok) return false;
    mix_hash(hs.hash, msg + 44, 16);
    hs.remote_index = le32(msg + 4);
    p->hs = hs;
    derive_keypair(p, true);
    p->endpoint = info->src; p->endpoint_port = info->src_port; p->has_endpoint = true;   /* roaming */
    p->rx_bytes += length; p->last_received_ns = native_clock_ns();
    flush_queue(w, p);
    return true;
}

/* ---- handshake: responder ---- */
static bool consume_initiation(struct wg_device* w, const uint8_t* msg, unsigned length, const struct inet_rx_info* info) {
    if (length != 148 || !w->has_private) return false;
    uint8_t mac[16]; blake2s(mac, 16, w->mac1_key, 32, msg, 116);
    if (!wg_equal(mac, msg + 116, 16)) return false;
    uint8_t chain[32], hash[32]; initial_chain(chain, hash, w->public_key);
    const uint8_t* e_i = msg + 8;
    kdf(chain, NULL, NULL, chain, e_i, 32); mix_hash(hash, e_i, 32);
    uint8_t dh[32], key[32], peer_static[32];
    x25519(dh, w->private_key, e_i); kdf(chain, key, NULL, chain, dh, 32);
    if (!chacha20poly1305_decrypt(peer_static, msg + 40, 48, hash, 32, 0, key)) return false;   /* (length: ciphertext + tag) */
    mix_hash(hash, msg + 40, 48);
    struct wg_peer* p = peer_by_key(w, peer_static); if (!p) return false;
    kdf(chain, key, NULL, chain, p->static_static, 32);
    uint8_t stamp[12];
    if (!chacha20poly1305_decrypt(stamp, msg + 88, 28, hash, 32, 0, key)) return false;
    mix_hash(hash, msg + 88, 28);
    if (memcmp(stamp, p->greatest_timestamp, 12) <= 0) return false;   /* replayed initiation */
    memcpy(p->greatest_timestamp, stamp, 12);
    /* the response */
    struct wg_handshake* hs = &p->hs; memset(hs, 0, sizeof(*hs));
    memcpy(hs->chain, chain, 32); memcpy(hs->hash, hash, 32); memcpy(hs->remote_e, e_i, 32);
    hs->remote_index = le32(msg + 4); hs->local_index = new_index();
    uint8_t out[92]; memset(out, 0, sizeof(out));
    out[0] = MSG_RESPONSE; put_le32(out + 4, hs->local_index); put_le32(out + 8, hs->remote_index);
    generate_ephemeral(hs->e_priv, hs->e_pub); memcpy(out + 12, hs->e_pub, 32);
    kdf(hs->chain, NULL, NULL, hs->chain, hs->e_pub, 32); mix_hash(hs->hash, hs->e_pub, 32);
    x25519(dh, hs->e_priv, e_i); kdf(hs->chain, NULL, NULL, hs->chain, dh, 32);
    x25519(dh, hs->e_priv, peer_static); kdf(hs->chain, NULL, NULL, hs->chain, dh, 32);
    uint8_t tau[32]; kdf(hs->chain, tau, key, hs->chain, p->psk, 32);
    mix_hash(hs->hash, tau, 32);
    chacha20poly1305_encrypt(out + 44, NULL, 0, hs->hash, 32, 0, key);
    mix_hash(hs->hash, out + 44, 16);
    uint8_t mkey[32]; mac1_key_for(mkey, peer_static); blake2s(out + 60, 16, mkey, 32, out, 60);
    wg_memzero(dh, 32); wg_memzero(key, 32); wg_memzero(tau, 32);
    p->endpoint = info->src; p->endpoint_port = info->src_port; p->has_endpoint = true;
    p->rx_bytes += length; p->last_received_ns = native_clock_ns();
    derive_keypair(p, false);   /* the next keypair, confirmed by the initiator's first data message */
    send_udp(w, p, out, sizeof(out));
    return true;
}

/* ---- transport ---- */
static int encrypt_and_send(struct wg_device* w, struct wg_peer* p, const uint8_t* packet, unsigned length) {
    struct wg_keypair* k = &p->current;
    unsigned padded = (length + 15) & ~15U; if (padded > w->dev->mtu && length <= w->dev->mtu) padded = w->dev->mtu;
    if (padded < length) padded = length;
    unsigned total = 16 + padded + 16;
    uint8_t* msg = (void*)arena_alloc(total); if (!msg) return -12;
    uint8_t* plain = (void*)arena_alloc(padded ? padded : 1); if (!plain) { arena_free((uint64_t)msg, total); return -12; }
    memset(plain, 0, padded ? padded : 1); if (length) memcpy(plain, packet, length);
    memset(msg, 0, 16); msg[0] = MSG_DATA; put_le32(msg + 4, k->remote_index); put_le64(msg + 8, k->send_counter);
    chacha20poly1305_encrypt(msg + 16, plain, padded, NULL, 0, k->send_counter, k->send);
    k->send_counter++;
    int result = send_udp(w, p, msg, total);
    arena_free((uint64_t)plain, padded ? padded : 1); arena_free((uint64_t)msg, total);
    return result;
}
static bool keys_usable(const struct wg_keypair* k, uint64_t now) {
    return k->valid && k->confirmed && now - k->created_ns < REJECT_AFTER_NS && k->send_counter < REKEY_AFTER_MESSAGES;
}
static void flush_queue(struct wg_device* w, struct wg_peer* p) {
    if (!p->queued) { encrypt_and_send(w, p, NULL, 0); return; }   /* a keepalive confirms the new keys */
    for (unsigned i = 0; i < p->queued; i++) { encrypt_and_send(w, p, p->queue[i], p->queue_length[i]); arena_free((uint64_t)p->queue[i], p->queue_length[i]); }
    p->queued = 0;
}
static bool replay_ok(struct wg_keypair* k, uint64_t counter) {
    if (counter >= REKEY_AFTER_MESSAGES) return false;
    if (counter + 64 <= k->recv_max && k->recv_max >= 64) return false;          /* too old */
    if (counter > k->recv_max || (!k->recv_max && !k->recv_window)) {
        uint64_t shift = counter > k->recv_max ? counter - k->recv_max : 0;
        k->recv_window = shift >= 64 ? 0 : k->recv_window << shift;
        k->recv_window |= 1; k->recv_max = counter; return true;
    }
    uint64_t bit = 1ULL << (k->recv_max - counter);
    if (k->recv_window & bit) return false;                                       /* replayed */
    k->recv_window |= bit; return true;
}
static bool consume_data(struct wg_device* w, const uint8_t* msg, unsigned length, const struct inet_rx_info* info) {
    if (length < 32) return false;
    uint32_t receiver = le32(msg + 4); uint64_t counter = le64(msg + 8);
    struct wg_peer* p = NULL; struct wg_keypair* k = NULL;
    for (unsigned i = 0; i < WG_PEERS && !k; i++) {
        struct wg_peer* q = &w->peers[i]; if (!q->used) continue;
        if (q->current.valid && q->current.local_index == receiver) { p = q; k = &q->current; }
        else if (q->next.valid && q->next.local_index == receiver) { p = q; k = &q->next; }
        else if (q->previous.valid && q->previous.local_index == receiver) { p = q; k = &q->previous; }
    }
    if (!k) return false;
    uint64_t now = native_clock_ns();
    if (now - k->created_ns >= REJECT_AFTER_NS) return false;
    unsigned plain_length = length - 32;
    uint8_t* plain = (void*)arena_alloc(plain_length ? plain_length : 1); if (!plain) return true;
    if (!chacha20poly1305_decrypt(plain, msg + 16, plain_length + 16, NULL, 0, counter, k->recv)) { arena_free((uint64_t)plain, plain_length ? plain_length : 1); return false; }
    if (!replay_ok(k, counter)) { arena_free((uint64_t)plain, plain_length ? plain_length : 1); return true; }
    if (k == &p->next) { p->previous = p->current; p->current = p->next; p->current.confirmed = true; memset(&p->next, 0, sizeof(p->next)); k = &p->current; flush_queue(w, p); }
    p->endpoint = info->src; p->endpoint_port = info->src_port; p->has_endpoint = true;
    p->rx_bytes += length; p->last_received_ns = now;
    if (plain_length) {
        p->last_data_received_ns = now;
        unsigned real = 0; struct ip6a source; bool v4 = false;
        if ((plain[0] >> 4) == 4 && plain_length >= 20) { real = be16(plain + 2); v4 = true; source = ip6a_from4(be32(plain + 12)); }
        else if ((plain[0] >> 4) == 6 && plain_length >= 40) { real = 40u + be16(plain + 4); memcpy(&source, plain + 8, 16); }
        bool allowed = false;
        if (real && real <= plain_length) for (unsigned i = 0; i < p->allowed_count; i++) if (prefix_match(&p->allowed[i], &source, v4)) allowed = true;
        if (allowed && (w->dev->flags & IFF_UP)) netdev_input_l3(w->dev, plain, real, v4 ? ETH_TYPE_IPV4 : ETH_TYPE_IPV6, 0);
        else w->dev->stats.rx_dropped++;   /* cryptokey routing: not one of the peer's addresses */
    }
    arena_free((uint64_t)plain, plain_length ? plain_length : 1);
    return true;
}
/* UDP delivery hook (inet.c): true when a WireGuard device consumed the datagram */
bool inet_wireguard_port(uint16_t port, uint8_t family) {
    (void)family;
    for (unsigned i = 0; i < WG_DEVICES; i++) if (devices[i] && devices[i]->listen_port == port && devices[i]->dev && (devices[i]->dev->flags & IFF_UP)) return true;
    return false;
}
bool wireguard_receive(const struct inet_rx_info* info, const void* data, unsigned length) {
    const uint8_t* msg = data; if (length < 4) return true;
    for (unsigned i = 0; i < WG_DEVICES; i++) {
        struct wg_device* w = devices[i];
        if (!w || w->listen_port != info->dst_port || !w->dev || !(w->dev->flags & IFF_UP)) continue;
        if (msg[1] || msg[2] || msg[3]) return true;            /* reserved bytes must be zero */
        switch (msg[0]) {
        case MSG_INITIATION: consume_initiation(w, msg, length, info); break;
        case MSG_RESPONSE: consume_response(w, msg, length, info); break;
        case MSG_DATA: consume_data(w, msg, length, info); break;
        default: break;                                          /* cookie replies (not implemented) and garbage */
        }
        return true;
    }
    return false;
}
/* netdev xmit: an L3 packet routed to wg0 */
static int wg_xmit(struct netdev* dev, const void* data, unsigned length) {
    struct wg_device* w = device_of(dev); if (!w || !length) return -100;
    const uint8_t* packet = data; struct ip6a dst; bool v4;
    if ((packet[0] >> 4) == 4 && length >= 20) { dst = ip6a_from4(be32(packet + 16)); v4 = true; }
    else if ((packet[0] >> 4) == 6 && length >= 40) { memcpy(&dst, packet + 24, 16); v4 = false; }
    else return -22;
    struct wg_peer* p = peer_for(w, &dst, v4);
    if (!p) { dev->stats.tx_errors++; return -113; }            /* EHOSTUNREACH: no peer for it (Linux: ICMP unreachable) */
    uint64_t now = native_clock_ns();
    if (keys_usable(&p->current, now)) {
        int r = encrypt_and_send(w, p, packet, length);
        if (p->current.initiator && now - p->current.created_ns >= REKEY_AFTER_NS && p->hs.state != 1) send_initiation(w, p);
        return r < 0 ? r : 0;
    }
    /* no session: queue it and start a handshake */
    if (p->queued == WG_QUEUE) { arena_free((uint64_t)p->queue[0], p->queue_length[0]); for (unsigned i = 1; i < WG_QUEUE; i++) { p->queue[i - 1] = p->queue[i]; p->queue_length[i - 1] = p->queue_length[i]; } p->queued--; }
    uint8_t* copy = (void*)arena_alloc(length); if (!copy) return -11;
    memcpy(copy, packet, length); p->queue[p->queued] = copy; p->queue_length[p->queued] = length; p->queued++;
    if (p->hs.state != 1 || now - p->hs.sent_ns >= REKEY_TIMEOUT_NS) send_initiation(w, p);
    return 0;
}
static void clear_peer(struct wg_peer* p) {
    for (unsigned i = 0; i < p->queued; i++) arena_free((uint64_t)p->queue[i], p->queue_length[i]);
    wg_memzero(p, sizeof(*p));
}
static void wg_destroy(struct netdev* dev) {
    for (unsigned i = 0; i < WG_DEVICES; i++) if (devices[i] && devices[i]->dev == dev) {
        struct wg_device* w = devices[i];
        for (unsigned k = 0; k < WG_PEERS; k++) if (w->peers[k].used) clear_peer(&w->peers[k]);
        wg_memzero(w->private_key, 32);
        arena_free((uint64_t)w, (sizeof(*w) + 4095) & ~4095UL); devices[i] = NULL;
    }
}
static const struct netdev_ops wg_ops = {wg_xmit, NULL, wg_destroy, NULL};
int wireguard_create(const char* name, struct netdev** out) {
    unsigned slot = WG_DEVICES; for (unsigned i = 0; i < WG_DEVICES; i++) if (!devices[i]) { slot = i; break; }
    if (slot == WG_DEVICES) return -28;
    struct wg_device* w = (void*)arena_alloc((sizeof(*w) + 4095) & ~4095UL); if (!w) return -12;
    memset(w, 0, sizeof(*w));
    struct netdev* dev = netdev_create(name, ARPHRD_NONE, &wg_ops, w);
    if (!dev) { arena_free((uint64_t)w, (sizeof(*w) + 4095) & ~4095UL); return -17; }
    strcpy(dev->kind, "wireguard"); dev->mtu = 1420; dev->max_mtu = 65535; dev->min_mtu = 68; dev->mtu6 = 1420; dev->conf6.mtu = 1420;
    dev->flags = IFF_POINTOPOINT | IFF_NOARP; dev->carrier = true;
    w->dev = dev; devices[slot] = w; *out = dev;
    return 0;
}
/* timers (netdev_poll): handshake retransmission, keepalives, key expiry */
void wireguard_poll(uint64_t now) {
    for (unsigned d = 0; d < WG_DEVICES; d++) {
        struct wg_device* w = devices[d]; if (!w || !w->dev || !(w->dev->flags & IFF_UP)) continue;
        for (unsigned i = 0; i < WG_PEERS; i++) {
            struct wg_peer* p = &w->peers[i]; if (!p->used) continue;
            if (p->hs.state == 1 && now - p->hs.sent_ns >= REKEY_TIMEOUT_NS) {
                if (now - p->hs.started_ns >= REKEY_ATTEMPT_NS) {   /* give up: drop the queued packets */
                    p->hs.state = 0; p->hs.started_ns = 0;
                    for (unsigned k = 0; k < p->queued; k++) arena_free((uint64_t)p->queue[k], p->queue_length[k]);
                    p->queued = 0;
                } else send_initiation(w, p);
            }
            if (p->current.valid && now - p->current.created_ns >= REJECT_AFTER_NS) memset(&p->current, 0, sizeof(p->current));
            if (p->previous.valid && now - p->previous.created_ns >= REJECT_AFTER_NS) memset(&p->previous, 0, sizeof(p->previous));
            if (keys_usable(&p->current, now)) {
                bool persistent = p->keepalive_s && now - p->last_sent_ns >= (uint64_t)p->keepalive_s * 1000000000ULL;
                bool passive = p->last_data_received_ns > p->last_sent_ns && now - p->last_data_received_ns >= KEEPALIVE_NS;
                if (persistent || passive) encrypt_and_send(w, p, NULL, 0);
            } else if (p->keepalive_s && p->has_endpoint && p->hs.state != 1 && now - p->last_sent_ns >= (uint64_t)p->keepalive_s * 1000000000ULL) send_initiation(w, p);
        }
    }
}

/* ====================================================================================================
 * Generic netlink: nlctrl (family resolution) and the "wireguard" family
 * ==================================================================================================== */
#define GENL_ID_CTRL 0x10
#define WG_GENL_ID 0x22
#define CTRL_CMD_NEWFAMILY 1
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_VERSION 3
#define CTRL_ATTR_HDRSIZE 4
#define CTRL_ATTR_MAXATTR 5
#define CTRL_ATTR_OPS 6
#define CTRL_ATTR_OP_ID 1
#define CTRL_ATTR_OP_FLAGS 2
#define WG_CMD_GET_DEVICE 0
#define WG_CMD_SET_DEVICE 1
#define WGDEVICE_A_IFINDEX 1
#define WGDEVICE_A_IFNAME 2
#define WGDEVICE_A_PRIVATE_KEY 3
#define WGDEVICE_A_PUBLIC_KEY 4
#define WGDEVICE_A_FLAGS 5
#define WGDEVICE_A_LISTEN_PORT 6
#define WGDEVICE_A_FWMARK 7
#define WGDEVICE_A_PEERS 8
#define WGDEVICE_F_REPLACE_PEERS 1
#define WGPEER_A_PUBLIC_KEY 1
#define WGPEER_A_PRESHARED_KEY 2
#define WGPEER_A_FLAGS 3
#define WGPEER_A_ENDPOINT 4
#define WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL 5
#define WGPEER_A_LAST_HANDSHAKE_TIME 6
#define WGPEER_A_RX_BYTES 7
#define WGPEER_A_TX_BYTES 8
#define WGPEER_A_ALLOWEDIPS 9
#define WGPEER_A_PROTOCOL_VERSION 10
#define WGPEER_F_REMOVE_ME 1
#define WGPEER_F_REPLACE_ALLOWEDIPS 2
#define WGPEER_F_UPDATE_ONLY 4
#define WGALLOWEDIP_A_FAMILY 1
#define WGALLOWEDIP_A_IPADDR 2
#define WGALLOWEDIP_A_CIDR_MASK 3

static void genl_begin(struct nl_builder* b, uint16_t family, uint8_t cmd, uint8_t version, uint16_t flags, uint32_t seq, uint32_t portid) {
    nl_begin(b, family, flags, seq, portid);
    uint8_t g[4] = {cmd, version, 0, 0}; nl_put(b, g, 4);
}
/* the elements of a nested list attribute (wireguard peers / allowed IPs use type 0 for every element) */
static unsigned items(const struct nl_attr_view* list, struct nl_attr_view* out, unsigned max) {
    unsigned n = 0;
    for (unsigned at = 0; list->present && at + 4 <= list->length && n < max;) {
        uint16_t size; memcpy(&size, list->data + at, 2);
        if (size < 4 || size > list->length - at) break;
        out[n].data = list->data + at + 4; out[n].length = (uint16_t)(size - 4); out[n].present = true; n++;
        at += (size + 3u) & ~3u;
    }
    return n;
}
static int family_reply(struct netlink_socket* nl, const struct nlmsghdr* rq, const char* name) {
    if (!strcmp(name, "nl80211")) return nl80211_family_reply(nl, rq);     /* nl80211.c */
    bool wg = !strcmp(name, "wireguard"), ctrl = !strcmp(name, "nlctrl");
    if (!wg && !ctrl) return -2;
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return -12;
    genl_begin(b, GENL_ID_CTRL, CTRL_CMD_NEWFAMILY, 2, 0, rq->seq, nl_portid(nl));
    nl_attr_string(b, CTRL_ATTR_FAMILY_NAME, name); nl_attr16(b, CTRL_ATTR_FAMILY_ID, wg ? WG_GENL_ID : GENL_ID_CTRL);
    nl_attr32(b, CTRL_ATTR_VERSION, wg ? 1 : 2); nl_attr32(b, CTRL_ATTR_HDRSIZE, 0); nl_attr32(b, CTRL_ATTR_MAXATTR, wg ? 8 : 10);
    nl_nest_begin(b, CTRL_ATTR_OPS);
    for (unsigned op = 0; op < 2; op++) {
        nl_nest_begin(b, (uint16_t)(op + 1));
        nl_attr32(b, CTRL_ATTR_OP_ID, wg ? op : CTRL_CMD_GETFAMILY); nl_attr32(b, CTRL_ATTR_OP_FLAGS, 0x1A);   /* ADMIN_PERM|CMD_CAP_DO|DUMP */
        nl_nest_end(b);
    }
    nl_nest_end(b);
    nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b));
    return 0;   /* the ACK follows when asked for */
}
static struct wg_device* device_from(const struct nl_attr_view* a) {
    struct netdev* dev = NULL;
    if (a[WGDEVICE_A_IFINDEX].present) dev = netdev_by_index(nl_u32(&a[WGDEVICE_A_IFINDEX]));
    else if (a[WGDEVICE_A_IFNAME].present) { char name[IFNAMSIZ + 1]; if (nl_string(&a[WGDEVICE_A_IFNAME], name, sizeof(name))) dev = netdev_by_name(name); }
    return dev ? device_of(dev) : NULL;
}
static void put_endpoint(struct nl_builder* b, const struct wg_peer* p) {
    if (!p->has_endpoint) return;
    if (ip6a_is4(&p->endpoint)) {
        uint8_t sa[16] = {0}; sa[0] = AF_INET; put16(sa + 2, p->endpoint_port); put32(sa + 4, ip6a_v4(&p->endpoint));
        nl_attr(b, WGPEER_A_ENDPOINT, sa, 16);
    } else {
        uint8_t sa[28] = {0}; sa[0] = AF_INET6; put16(sa + 2, p->endpoint_port); memcpy(sa + 8, p->endpoint.b, 16);
        nl_attr(b, WGPEER_A_ENDPOINT, sa, 28);
    }
}
static void put_peer(struct nl_builder* b, const struct wg_peer* p) {
    nl_nest_begin(b, 0);
    nl_attr(b, WGPEER_A_PUBLIC_KEY, p->public_key, 32); nl_attr(b, WGPEER_A_PRESHARED_KEY, p->psk, 32);
    int64_t stamp[2] = {(int64_t)(p->last_handshake_rt / 1000000000ULL), (int64_t)(p->last_handshake_rt % 1000000000ULL)};
    nl_attr(b, WGPEER_A_LAST_HANDSHAKE_TIME, stamp, 16);
    nl_attr16(b, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL, p->keepalive_s);
    nl_attr64(b, WGPEER_A_TX_BYTES, p->tx_bytes); nl_attr64(b, WGPEER_A_RX_BYTES, p->rx_bytes);
    nl_attr32(b, WGPEER_A_PROTOCOL_VERSION, 1);
    put_endpoint(b, p);
    nl_nest_begin(b, WGPEER_A_ALLOWEDIPS);
    for (unsigned k = 0; k < p->allowed_count; k++) {
        nl_nest_begin(b, 0);
        nl_attr16(b, WGALLOWEDIP_A_FAMILY, p->allowed[k].family);
        if (p->allowed[k].family == AF_INET) { uint32_t v = __builtin_bswap32(ip6a_v4(&p->allowed[k].addr)); nl_attr(b, WGALLOWEDIP_A_IPADDR, &v, 4); }
        else nl_attr(b, WGALLOWEDIP_A_IPADDR, p->allowed[k].addr.b, 16);
        nl_attr8(b, WGALLOWEDIP_A_CIDR_MASK, p->allowed[k].cidr);
        nl_nest_end(b);
    }
    nl_nest_end(b);
    nl_nest_end(b);
}
static void device_head(struct nl_builder* b, struct netlink_socket* nl, const struct nlmsghdr* rq, struct wg_device* w, bool full) {
    genl_begin(b, WG_GENL_ID, WG_CMD_GET_DEVICE, 1, NLM_F_MULTI, rq->seq, nl_portid(nl));
    nl_attr32(b, WGDEVICE_A_IFINDEX, w->dev->index); nl_attr_string(b, WGDEVICE_A_IFNAME, w->dev->name);
    if (full) {
        if (w->has_private) { nl_attr(b, WGDEVICE_A_PRIVATE_KEY, w->private_key, 32); nl_attr(b, WGDEVICE_A_PUBLIC_KEY, w->public_key, 32); }
        nl_attr16(b, WGDEVICE_A_LISTEN_PORT, w->listen_port); nl_attr32(b, WGDEVICE_A_FWMARK, w->fwmark);
    }
    nl_nest_begin(b, WGDEVICE_A_PEERS);
}
/* One device; when its peers do not fit one message they continue in further messages that repeat the interface
 * (as Linux does), wg merges them. A message stays within NLMSG_GOODSIZE: wg reads with a 4 KiB libmnl buffer and
 * a longer datagram would be truncated (ENOSPC in wg). */
#define WG_DUMP_BYTES 3776
static int get_device(struct netlink_socket* nl, const struct nlmsghdr* rq, const struct nl_attr_view* a) {
    struct wg_device* w = device_from(a); if (!w) return -19;
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return -12;
    nl_dump_begin(nl);
    device_head(b, nl, rq, w, true); unsigned in_message = 0;
    for (unsigned i = 0; i < WG_PEERS; i++) {
        struct wg_peer* p = &w->peers[i]; if (!p->used) continue;
        unsigned used = b->used, depth = b->depth;
        put_peer(b, p);
        if ((b->overflow || b->used > WG_DUMP_BYTES) && in_message) {
            b->used = used; b->depth = depth; b->overflow = false;
            nl_nest_end(b); nl_send(nl, b);
            device_head(b, nl, rq, w, false); in_message = 0;
            put_peer(b, p);
        }
        in_message++;
    }
    nl_nest_end(b);
    nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b));
    nl_send_done(nl, rq); nl_dump_end(nl);
    return 1;
}
static int set_peer(struct wg_device* w, const struct nl_attr_view* element) {
    struct nl_attr_view a[11]; if (!nl_parse(element->data, element->length, a, 10)) return -22;
    if (!a[WGPEER_A_PUBLIC_KEY].present || a[WGPEER_A_PUBLIC_KEY].length != 32) return -22;
    uint32_t flags = nl_u32(&a[WGPEER_A_FLAGS]);
    struct wg_peer* p = peer_by_key(w, a[WGPEER_A_PUBLIC_KEY].data);
    if (flags & WGPEER_F_REMOVE_ME) { if (p) clear_peer(p); return 0; }
    if (!p) {
        if (flags & WGPEER_F_UPDATE_ONLY) return 0;
        if (w->has_private && wg_equal(a[WGPEER_A_PUBLIC_KEY].data, w->public_key, 32)) return 0;   /* ourselves: ignored, as Linux */
        for (unsigned i = 0; i < WG_PEERS && !p; i++) if (!w->peers[i].used) p = &w->peers[i];
        if (!p) return -28;
        memset(p, 0, sizeof(*p)); p->used = true; memcpy(p->public_key, a[WGPEER_A_PUBLIC_KEY].data, 32);
        if (w->has_private) x25519(p->static_static, w->private_key, p->public_key);
    }
    if (a[WGPEER_A_PRESHARED_KEY].present && a[WGPEER_A_PRESHARED_KEY].length == 32) memcpy(p->psk, a[WGPEER_A_PRESHARED_KEY].data, 32);
    if (a[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL].present) p->keepalive_s = nl_u16(&a[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL]);
    if (a[WGPEER_A_ENDPOINT].present) {
        const uint8_t* sa = a[WGPEER_A_ENDPOINT].data; unsigned n = a[WGPEER_A_ENDPOINT].length;
        if (n >= 16 && sa[0] == AF_INET) { p->endpoint = ip6a_from4(be32(sa + 4)); p->endpoint_port = be16(sa + 2); p->has_endpoint = true; }
        else if (n >= 28 && sa[0] == AF_INET6) { memcpy(p->endpoint.b, sa + 8, 16); p->endpoint_port = be16(sa + 2); p->has_endpoint = true; }
        else return -22;
    }
    if (flags & WGPEER_F_REPLACE_ALLOWEDIPS) p->allowed_count = 0;
    struct nl_attr_view list[WG_ALLOWED + 8]; unsigned n = items(&a[WGPEER_A_ALLOWEDIPS], list, WG_ALLOWED + 8);
    for (unsigned i = 0; i < n; i++) {
        struct nl_attr_view ip[4]; if (!nl_parse(list[i].data, list[i].length, ip, 3)) return -22;
        uint16_t family = nl_u16(&ip[WGALLOWEDIP_A_FAMILY]); uint8_t cidr = nl_u8(&ip[WGALLOWEDIP_A_CIDR_MASK]);
        struct wg_allowed entry; memset(&entry, 0, sizeof(entry)); entry.family = (uint8_t)family; entry.cidr = cidr;
        if (family == AF_INET && ip[WGALLOWEDIP_A_IPADDR].length == 4 && cidr <= 32) entry.addr = ip6a_from4(be32(ip[WGALLOWEDIP_A_IPADDR].data));
        else if (family == AF_INET6 && ip[WGALLOWEDIP_A_IPADDR].length == 16 && cidr <= 128) memcpy(entry.addr.b, ip[WGALLOWEDIP_A_IPADDR].data, 16);
        else return -22;
        ip6a_mask(&entry.addr, family == AF_INET ? 96u + cidr : cidr);
        /* an allowed IP belongs to one peer only: move it here */
        for (unsigned d = 0; d < WG_PEERS; d++) {
            struct wg_peer* q = &w->peers[d]; if (!q->used) continue;
            for (unsigned k = 0; k < q->allowed_count;)
                if (q->allowed[k].family == entry.family && q->allowed[k].cidr == entry.cidr && ip6a_eq(&q->allowed[k].addr, &entry.addr)) q->allowed[k] = q->allowed[--q->allowed_count];
                else k++;
        }
        if (p->allowed_count == WG_ALLOWED) return -28;
        p->allowed[p->allowed_count++] = entry;
    }
    if (keys_usable(&p->current, native_clock_ns()) == false && p->queued && p->has_endpoint) send_initiation(w, p);
    return 0;
}
static int set_device(const struct nl_attr_view* a) {
    struct wg_device* w = device_from(a); if (!w) return -19;
    uint32_t flags = nl_u32(&a[WGDEVICE_A_FLAGS]);
    if (a[WGDEVICE_A_LISTEN_PORT].present) {
        uint16_t port = nl_u16(&a[WGDEVICE_A_LISTEN_PORT]);
        for (unsigned i = 0; i < WG_DEVICES; i++) if (devices[i] && devices[i] != w && port && devices[i]->listen_port == port) return -98;   /* EADDRINUSE */
        w->listen_port = port;
        if (!w->listen_port) { uint16_t r; random_bytes(&r, 2); w->listen_port = (uint16_t)(32768 + (r % 28232)); }
    }
    if (a[WGDEVICE_A_FWMARK].present) w->fwmark = nl_u32(&a[WGDEVICE_A_FWMARK]);
    if (a[WGDEVICE_A_PRIVATE_KEY].present) {
        if (a[WGDEVICE_A_PRIVATE_KEY].length != 32) return -22;
        if (zero32(a[WGDEVICE_A_PRIVATE_KEY].data)) { w->has_private = false; wg_memzero(w->private_key, 32); }
        else {
            memcpy(w->private_key, a[WGDEVICE_A_PRIVATE_KEY].data, 32);
            w->private_key[0] &= 248; w->private_key[31] = (uint8_t)((w->private_key[31] & 127) | 64);
            x25519_public(w->public_key, w->private_key); w->has_private = true; mac1_key_for(w->mac1_key, w->public_key);
            for (unsigned i = 0; i < WG_PEERS; i++) if (w->peers[i].used) {
                if (wg_equal(w->peers[i].public_key, w->public_key, 32)) { clear_peer(&w->peers[i]); continue; }
                x25519(w->peers[i].static_static, w->private_key, w->peers[i].public_key);
                memset(&w->peers[i].current, 0, sizeof(struct wg_keypair)); memset(&w->peers[i].next, 0, sizeof(struct wg_keypair));
            }
        }
    }
    if (!w->listen_port && w->has_private) { uint16_t r; random_bytes(&r, 2); w->listen_port = (uint16_t)(32768 + (r % 28232)); }
    if (flags & WGDEVICE_F_REPLACE_PEERS) for (unsigned i = 0; i < WG_PEERS; i++) if (w->peers[i].used) clear_peer(&w->peers[i]);
    struct nl_attr_view list[WG_PEERS + 8]; unsigned n = items(&a[WGDEVICE_A_PEERS], list, WG_PEERS + 8);
    for (unsigned i = 0; i < n; i++) { int error = set_peer(w, &list[i]); if (error) return error; }
    return 0;
}
int genl_request(struct netlink_socket* nl, const struct nlmsghdr* rq, const uint8_t* payload, unsigned length) {
    if (length < 4) return -22;
    if (nl80211_family_id(rq->type)) return nl80211_request(nl, rq, payload, length);
    uint8_t cmd = payload[0];
    struct nl_attr_view a[12]; if (!nl_parse(payload + 4, length - 4, a, 11)) return -22;
    if (rq->type == GENL_ID_CTRL) {
        if (cmd != CTRL_CMD_GETFAMILY) return -95;
        char name[32]; if (!nl_string(&a[CTRL_ATTR_FAMILY_NAME], name, sizeof(name))) {
            if (a[CTRL_ATTR_FAMILY_ID].present) { uint16_t id = nl_u16(&a[CTRL_ATTR_FAMILY_ID]); return family_reply(nl, rq, id == WG_GENL_ID ? "wireguard" : id == GENL_ID_CTRL ? "nlctrl" : nl80211_family_id(id) ? "nl80211" : ""); }
            return -22;
        }
        return family_reply(nl, rq, name);
    }
    if (rq->type != WG_GENL_ID) return -2;
    if (!netlink_admin()) return -1;                             /* GENL_UNS_ADMIN_PERM: CAP_NET_ADMIN (or the desktop owner) */
    if (cmd == WG_CMD_GET_DEVICE) return get_device(nl, rq, a);
    if (cmd == WG_CMD_SET_DEVICE) return set_device(a);
    return -95;
}
