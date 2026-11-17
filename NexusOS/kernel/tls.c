/* ============================================================================
 * NexusOS — TLS 1.3 client (Phase 57)
 * ============================================================================ */
#include "tls.h"
#include "bignum.h"
#include "string.h"
#include "ip.h"

extern volatile uint32_t system_ticks;

/* ---- content types / handshake types ---- */
#define CT_CCS        0x14
#define CT_ALERT      0x15
#define CT_HANDSHAKE  0x16
#define CT_APPDATA    0x17

#define HS_CLIENT_HELLO        1
#define HS_SERVER_HELLO        2
#define HS_NEW_SESSION_TICKET  4
#define HS_ENCRYPTED_EXT       8
#define HS_CERTIFICATE        11
#define HS_CERT_REQUEST       13
#define HS_CERT_VERIFY        15
#define HS_FINISHED           20
#define HS_KEY_UPDATE         24

static const uint8_t HRR_RANDOM[32] = {
    0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
    0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C
};

/* ============================================================================
 * Helpers
 * ============================================================================ */
static void str_append(char* dst, uint32_t sz, const char* s) {
    uint32_t n = strlen(dst);
    while (*s && n < sz - 1) dst[n++] = *s++;
    dst[n] = '\0';
}

static int fail(tls_ctx_t* t, const char* msg, const char* detail) {
    t->error[0] = '\0';
    str_append(t->error, sizeof(t->error), msg);
    if (detail) str_append(t->error, sizeof(t->error), detail);
    t->state = TLS_S_ERROR;
    return -1;
}

const char* tls_error(const tls_ctx_t* t) { return t->error; }

const char* tls_alert_name(int d) {
    switch (d) {
        case 0:   return "close_notify";
        case 10:  return "unexpected_message";
        case 20:  return "bad_record_mac";
        case 22:  return "record_overflow";
        case 40:  return "handshake_failure";
        case 42:  return "bad_certificate";
        case 43:  return "unsupported_certificate";
        case 44:  return "certificate_revoked";
        case 45:  return "certificate_expired";
        case 46:  return "certificate_unknown";
        case 47:  return "illegal_parameter";
        case 48:  return "unknown_ca";
        case 49:  return "access_denied";
        case 50:  return "decode_error";
        case 51:  return "decrypt_error";
        case 70:  return "protocol_version";
        case 71:  return "insufficient_security";
        case 80:  return "internal_error";
        case 86:  return "inappropriate_fallback";
        case 90:  return "user_canceled";
        case 109: return "missing_extension";
        case 110: return "unsupported_extension";
        case 112: return "unrecognized_name";
        case 113: return "bad_certificate_status_response";
        case 115: return "unknown_psk_identity";
        case 116: return "certificate_required";
        case 120: return "no_application_protocol";
        default:  return "alert";
    }
}

static bool conn_ok(const tls_ctx_t* t) {
    return t->conn && t->conn->active && t->conn->gen == t->conn_gen;
}

static void transcript_hash(const tls_ctx_t* t, uint8_t out[32]) {
    sha256_ctx_t c = t->transcript;
    sha256_final(&c, out);
}

static void derive_secret(const uint8_t secret[32], const char* label, const uint8_t thash[32], uint8_t out[32]) {
    hkdf_expand_label(secret, label, thash, 32, out, 32);
}

static void set_traffic_keys(gcm_ctx_t* g, uint8_t iv[12], const uint8_t secret[32]) {
    uint8_t key[16];
    hkdf_expand_label(secret, "key", NULL, 0, key, 16);
    hkdf_expand_label(secret, "iv", NULL, 0, iv, 12);
    gcm_set_key(g, key);
}

static void make_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) nonce[4 + i] ^= (uint8_t)(seq >> (56 - 8 * i));
}

/* ============================================================================
 * Record layer: send
 * ============================================================================ */
static int send_record(tls_ctx_t* t, uint8_t type, const uint8_t* data, uint32_t len) {
    if (!conn_ok(t)) return fail(t, "connection closed", NULL);
    if (len > TLS_TX_PLAIN) return fail(t, "record too large to send", NULL);
    uint8_t* out = t->tx;
    uint32_t total;
    if (!t->wr_enc) {
        out[0] = type; out[1] = 0x03; out[2] = 0x01;
        out[3] = (uint8_t)(len >> 8); out[4] = (uint8_t)len;
        memcpy(out + 5, data, len);
        total = 5 + len;
    } else {
        uint32_t inner = len + 1, clen = inner + 16;
        out[0] = CT_APPDATA; out[1] = 0x03; out[2] = 0x03;
        out[3] = (uint8_t)(clen >> 8); out[4] = (uint8_t)clen;
        memcpy(out + 5, data, len);
        out[5 + len] = type;
        uint8_t nonce[12];
        make_nonce(nonce, t->wr_iv, t->wr_seq);
        gcm_encrypt(&t->wr_gcm, nonce, out, 5, out + 5, out + 5, inner, out + 5 + inner);
        t->wr_seq++;
        total = 5 + clen;
    }
    t->bytes_tx += total;
    int r = tcp_send(t->conn, out, (uint16_t)total);
    if (r != (int)total) return fail(t, "send failed", NULL);
    return 0;
}

static int send_alert(tls_ctx_t* t, uint8_t level, uint8_t desc) {
    uint8_t a[2] = { level, desc };
    return send_record(t, CT_ALERT, a, 2);
}

/* ============================================================================
 * ClientHello
 * ============================================================================ */
static bool host_is_ip(const char* h) {
    for (const char* p = h; *p; p++) if (*p != '.' && (*p < '0' || *p > '9')) return false;
    return true;
}

static int send_client_hello(tls_ctx_t* t) {
    uint8_t ch[512];
    uint32_t p = 4;                           /* handshake header filled at the end */
    ch[p++] = 0x03; ch[p++] = 0x03;           /* legacy_version */
    uint8_t rnd[32];
    if (crypto_random(rnd, 32) != 0) return fail(t, "secure random source unavailable", NULL);
    memcpy(ch + p, rnd, 32); p += 32;
    ch[p++] = 32;                             /* legacy_session_id */
    if (crypto_random(rnd, 32) != 0) return fail(t, "secure random source unavailable", NULL);
    memcpy(ch + p, rnd, 32); p += 32;
    ch[p++] = 0x00; ch[p++] = 0x02; ch[p++] = 0x13; ch[p++] = 0x01;   /* TLS_AES_128_GCM_SHA256 */
    ch[p++] = 0x01; ch[p++] = 0x00;           /* compression: null */

    uint32_t ext_len_pos = p; p += 2;
    uint32_t hl = strlen(t->host);
    if (!host_is_ip(t->host) && hl < 250) {   /* server_name */
        ch[p++] = 0x00; ch[p++] = 0x00;
        uint32_t el = hl + 5;
        ch[p++] = (uint8_t)(el >> 8); ch[p++] = (uint8_t)el;
        ch[p++] = (uint8_t)((hl + 3) >> 8); ch[p++] = (uint8_t)(hl + 3);
        ch[p++] = 0x00;                       /* host_name */
        ch[p++] = (uint8_t)(hl >> 8); ch[p++] = (uint8_t)hl;
        memcpy(ch + p, t->host, hl); p += hl;
    }
    /* supported_groups: x25519 */
    static const uint8_t ext_groups[] = {0x00,0x0a, 0x00,0x04, 0x00,0x02, 0x00,0x1d};
    memcpy(ch + p, ext_groups, sizeof(ext_groups)); p += sizeof(ext_groups);
    /* signature_algorithms */
    static const uint8_t ext_sigalgs[] = {0x00,0x0d, 0x00,0x12, 0x00,0x10,
        0x04,0x03, 0x08,0x04, 0x05,0x03, 0x08,0x05, 0x08,0x06, 0x04,0x01, 0x05,0x01, 0x06,0x01};
    memcpy(ch + p, ext_sigalgs, sizeof(ext_sigalgs)); p += sizeof(ext_sigalgs);
    /* supported_versions: 1.3 only */
    static const uint8_t ext_versions[] = {0x00,0x2b, 0x00,0x03, 0x02, 0x03,0x04};
    memcpy(ch + p, ext_versions, sizeof(ext_versions)); p += sizeof(ext_versions);
    /* psk_key_exchange_modes: psk_dhe_ke */
    static const uint8_t ext_pskmodes[] = {0x00,0x2d, 0x00,0x02, 0x01, 0x01};
    memcpy(ch + p, ext_pskmodes, sizeof(ext_pskmodes)); p += sizeof(ext_pskmodes);
    /* ALPN: http/1.1 */
    static const uint8_t ext_alpn[] = {0x00,0x10, 0x00,0x0b, 0x00,0x09, 0x08, 'h','t','t','p','/','1','.','1'};
    memcpy(ch + p, ext_alpn, sizeof(ext_alpn)); p += sizeof(ext_alpn);
    /* key_share: x25519 */
    static const uint8_t ext_ks[] = {0x00,0x33, 0x00,0x26, 0x00,0x24, 0x00,0x1d, 0x00,0x20};
    memcpy(ch + p, ext_ks, sizeof(ext_ks)); p += sizeof(ext_ks);
    memcpy(ch + p, t->pub, 32); p += 32;

    uint32_t ext_len = p - ext_len_pos - 2;
    ch[ext_len_pos] = (uint8_t)(ext_len >> 8); ch[ext_len_pos + 1] = (uint8_t)ext_len;
    uint32_t body = p - 4;
    ch[0] = HS_CLIENT_HELLO; ch[1] = (uint8_t)(body >> 16); ch[2] = (uint8_t)(body >> 8); ch[3] = (uint8_t)body;

    sha256_update(&t->transcript, ch, p);
    return send_record(t, CT_HANDSHAKE, ch, p);
}

/* ============================================================================
 * Handshake message handlers
 * ============================================================================ */
static int handle_server_hello(tls_ctx_t* t, const uint8_t* b, uint32_t len) {
    uint32_t p = 0;
    if (len < 2 + 32 + 1) return fail(t, "malformed ServerHello", NULL);
    if (b[0] != 0x03 || b[1] != 0x03) return fail(t, "ServerHello: bad legacy version", NULL);
    p = 2;
    if (memcmp(b + p, HRR_RANDOM, 32) == 0) return fail(t, "server sent HelloRetryRequest (unsupported)", NULL);
    p += 32;
    uint32_t sidl = b[p++];
    if (sidl > 32 || p + sidl + 3 > len) return fail(t, "malformed ServerHello", NULL);
    p += sidl;
    uint16_t cipher = (uint16_t)((b[p] << 8) | b[p + 1]); p += 2;
    if (cipher != 0x1301) {
        char hx[12]; hex_to_str(cipher, hx);
        return fail(t, "server chose unsupported cipher suite ", hx);
    }
    if (b[p++] != 0) return fail(t, "ServerHello: compression", NULL);
    bool got_version = false, got_ks = false;
    uint8_t server_pub[32];
    if (p + 2 <= len) {
        uint32_t el = (uint32_t)((b[p] << 8) | b[p + 1]); p += 2;
        if (p + el > len) return fail(t, "malformed ServerHello extensions", NULL);
        uint32_t end = p + el;
        while (p + 4 <= end) {
            uint16_t et = (uint16_t)((b[p] << 8) | b[p + 1]);
            uint32_t xl = (uint32_t)((b[p + 2] << 8) | b[p + 3]);
            p += 4;
            if (p + xl > end) return fail(t, "malformed ServerHello extension", NULL);
            if (et == 43) {
                if (xl != 2 || b[p] != 0x03 || b[p + 1] != 0x04) return fail(t, "server does not support TLS 1.3", NULL);
                got_version = true;
            } else if (et == 51) {
                if (xl != 36 || b[p] != 0x00 || b[p + 1] != 0x1d || b[p + 2] != 0x00 || b[p + 3] != 0x20)
                    return fail(t, "ServerHello: unexpected key_share group", NULL);
                memcpy(server_pub, b + p + 4, 32);
                got_ks = true;
            }
            p += xl;
        }
    }
    if (!got_version) return fail(t, "server does not support TLS 1.3 (no supported_versions)", NULL);
    if (!got_ks) return fail(t, "ServerHello: no key_share", NULL);

    /* ECDHE */
    uint8_t shared[32];
    x25519(shared, t->priv, server_pub);
    uint8_t z = 0;
    for (int i = 0; i < 32; i++) z |= shared[i];
    if (!z) return fail(t, "bad server key share", NULL);

    /* key schedule up to the handshake traffic secrets */
    uint8_t zeros[32], early[32], derived[32], empty_hash[32], thash[32];
    memset(zeros, 0, 32);
    hkdf_extract(zeros, 32, zeros, 32, early);
    sha256(NULL, 0, empty_hash);
    derive_secret(early, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, shared, 32, t->hs_secret);
    transcript_hash(t, thash);                    /* CH..SH (SH already added by caller) */
    derive_secret(t->hs_secret, "c hs traffic", thash, t->c_hs);
    derive_secret(t->hs_secret, "s hs traffic", thash, t->s_hs);
    set_traffic_keys(&t->rd_gcm, t->rd_iv, t->s_hs);
    set_traffic_keys(&t->wr_gcm, t->wr_iv, t->c_hs);
    t->rd_seq = t->wr_seq = 0;
    t->rd_enc = t->wr_enc = true;
    t->state = TLS_S_WAIT_EE;
    return 0;
}

static int handle_encrypted_ext(tls_ctx_t* t, const uint8_t* b, uint32_t len) {
    if (len < 2) return fail(t, "malformed EncryptedExtensions", NULL);
    uint32_t el = (uint32_t)((b[0] << 8) | b[1]);
    uint32_t p = 2, end = 2 + el;
    if (end > len) return fail(t, "malformed EncryptedExtensions", NULL);
    while (p + 4 <= end) {
        uint16_t et = (uint16_t)((b[p] << 8) | b[p + 1]);
        uint32_t xl = (uint32_t)((b[p + 2] << 8) | b[p + 3]);
        p += 4;
        if (p + xl > end) break;
        if (et == 16 && xl >= 3) {               /* ALPN */
            uint32_t nl = b[p + 2];
            if (nl == 8 && xl >= 3 + 8 && memcmp(b + p + 3, "http/1.1", 8) == 0) t->alpn_http11 = true;
        }
        p += xl;
    }
    t->state = TLS_S_WAIT_CERT;
    return 0;
}

static void fill_cert_info(tls_cert_info_t* ci, const x509_cert_t* c) {
    strncpy(ci->subject, c->subject_cn, sizeof(ci->subject) - 1); ci->subject[sizeof(ci->subject) - 1] = '\0';
    strncpy(ci->issuer, c->issuer_cn, sizeof(ci->issuer) - 1);   ci->issuer[sizeof(ci->issuer) - 1] = '\0';
    x509_format_date(c->not_before, ci->not_before);
    x509_format_date(c->not_after, ci->not_after);
    ci->key_type = c->key_type;
    ci->sig_alg = c->sig_alg; ci->sig_hash = c->sig_hash;
    if (c->key_type == X509_KEY_RSA) {
        uint32_t n = c->rsa_n_len; const uint8_t* m = c->rsa_n;
        while (n && *m == 0) { m++; n--; }
        ci->key_bits = (int)n * 8;
    } else if (c->key_type == X509_KEY_P256) ci->key_bits = 256;
    else if (c->key_type == X509_KEY_P384) ci->key_bits = 384;
    else ci->key_bits = 0;
}

static int handle_certificate(tls_ctx_t* t, const uint8_t* b, uint32_t len) {
    static x509_cert_t certs[TLS_MAX_CHAIN];    /* ~2 KB: static, single-threaded */
    if (len < 4) return fail(t, "malformed Certificate", NULL);
    uint32_t p = 0;
    uint32_t ctxl = b[p++];
    p += ctxl;
    if (p + 3 > len) return fail(t, "malformed Certificate", NULL);
    uint32_t list = (uint32_t)((b[p] << 16) | (b[p + 1] << 8) | b[p + 2]); p += 3;
    if (p + list > len) return fail(t, "malformed Certificate list", NULL);
    uint32_t end = p + list;
    int n = 0;
    while (p + 3 <= end) {
        uint32_t cl = (uint32_t)((b[p] << 16) | (b[p + 1] << 8) | b[p + 2]); p += 3;
        if (p + cl + 2 > end) return fail(t, "malformed Certificate entry", NULL);
        if (n < TLS_MAX_CHAIN) {
            if (x509_parse(b + p, cl, &certs[n]) != 0) return fail(t, "cannot parse server certificate", NULL);
            fill_cert_info(&t->certs[n], &certs[n]);
            n++;
        }
        p += cl;
        uint32_t xl = (uint32_t)((b[p] << 8) | b[p + 1]); p += 2;
        if (p + xl > end) return fail(t, "malformed Certificate extensions", NULL);
        p += xl;
    }
    if (n == 0) return fail(t, "server sent no certificate", NULL);
    t->cert_count = n;
    if (certs[0].spki_len > TLS_MAX_SPKI) return fail(t, "server key too large", NULL);
    if (certs[0].key_type == X509_KEY_OTHER) return fail(t, "server key type unsupported (not RSA/P-256/P-384)", NULL);
    memcpy(t->leaf_spki, certs[0].spki, certs[0].spki_len);
    t->leaf_spki_len = certs[0].spki_len;

    x509_chain_result_t res;
    int r = x509_verify_chain(certs, n, t->host, t->insecure, &res);
    t->trusted = res.trusted;
    strncpy(t->root_name, res.root_name, sizeof(t->root_name) - 1); t->root_name[sizeof(t->root_name) - 1] = '\0';
    strncpy(t->reason, res.reason, sizeof(t->reason) - 1); t->reason[sizeof(t->reason) - 1] = '\0';
    if (r != 0) {
        send_alert(t, 2, 42);
        return fail(t, "certificate rejected: ", res.reason);
    }
    t->state = TLS_S_WAIT_CV;
    return 0;
}

static int handle_cert_verify(tls_ctx_t* t, const uint8_t* b, uint32_t len) {
    if (len < 4) return fail(t, "malformed CertificateVerify", NULL);
    uint16_t scheme = (uint16_t)((b[0] << 8) | b[1]);
    uint32_t sl = (uint32_t)((b[2] << 8) | b[3]);
    if (4 + sl > len) return fail(t, "malformed CertificateVerify", NULL);
    int alg, hash;
    switch (scheme) {
        case 0x0403: alg = X509_SIG_ECDSA;   hash = HASH_SHA256; break;
        case 0x0503: alg = X509_SIG_ECDSA;   hash = HASH_SHA384; break;
        case 0x0804: alg = X509_SIG_RSA_PSS; hash = HASH_SHA256; break;
        case 0x0805: alg = X509_SIG_RSA_PSS; hash = HASH_SHA384; break;
        case 0x0806: alg = X509_SIG_RSA_PSS; hash = HASH_SHA512; break;
        default: {
            char hx[12]; hex_to_str(scheme, hx);
            return fail(t, "unsupported CertificateVerify scheme ", hx);
        }
    }
    /* content = 64 x 0x20 || "TLS 1.3, server CertificateVerify" || 0x00 || transcript hash */
    uint8_t content[64 + 33 + 1 + 32];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    transcript_hash(t, content + 98);
    x509_cert_t key;
    memset(&key, 0, sizeof(key));
    if (x509_parse_spki(t->leaf_spki, t->leaf_spki_len, &key) != 0) return fail(t, "bad server key", NULL);
    int r = x509_verify_raw(&key, alg, hash, content, sizeof(content), b + 4, sl);
    if (r == -2) return fail(t, "CertificateVerify: unsupported key/scheme combination", NULL);
    if (r != 0) { send_alert(t, 2, 51); return fail(t, "CertificateVerify signature invalid", NULL); }
    t->state = TLS_S_WAIT_FIN;
    return 0;
}

static int handle_finished(tls_ctx_t* t, const uint8_t* b, uint32_t len, const uint8_t* msg, uint32_t mlen) {
    if (len != 32) return fail(t, "malformed Finished", NULL);
    uint8_t thash[32], fk[32], expect[32];
    transcript_hash(t, thash);                          /* CH..CertificateVerify */
    hkdf_expand_label(t->s_hs, "finished", NULL, 0, fk, 32);
    hmac_sha256(fk, 32, thash, 32, expect);
    if (crypto_memeq(expect, b, 32) != 0) { send_alert(t, 2, 51); return fail(t, "server Finished MAC invalid", NULL); }
    sha256_update(&t->transcript, msg, mlen);           /* ..server Finished */
    transcript_hash(t, thash);

    /* master secret + application traffic secrets */
    uint8_t zeros[32], derived[32], empty_hash[32], master[32];
    memset(zeros, 0, 32);
    sha256(NULL, 0, empty_hash);
    derive_secret(t->hs_secret, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, zeros, 32, master);
    derive_secret(master, "c ap traffic", thash, t->c_ap);
    derive_secret(master, "s ap traffic", thash, t->s_ap);

    /* client Finished (still under the handshake keys) */
    uint8_t fin[36];
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    hkdf_expand_label(t->c_hs, "finished", NULL, 0, fk, 32);
    hmac_sha256(fk, 32, thash, 32, fin + 4);
    if (send_record(t, CT_HANDSHAKE, fin, 36) != 0) return -1;

    set_traffic_keys(&t->wr_gcm, t->wr_iv, t->c_ap); t->wr_seq = 0;
    set_traffic_keys(&t->rd_gcm, t->rd_iv, t->s_ap); t->rd_seq = 0;
    t->state = TLS_S_CONNECTED;
    t->hs_ticks = system_ticks - t->hs_ticks;
    return 1;
}

static int handle_key_update(tls_ctx_t* t, const uint8_t* b, uint32_t len) {
    if (len != 1) return fail(t, "malformed KeyUpdate", NULL);
    uint8_t ns[32];
    hkdf_expand_label(t->s_ap, "traffic upd", NULL, 0, ns, 32);
    memcpy(t->s_ap, ns, 32);
    set_traffic_keys(&t->rd_gcm, t->rd_iv, t->s_ap); t->rd_seq = 0;
    if (b[0] == 1) {
        uint8_t ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
        if (send_record(t, CT_HANDSHAKE, ku, 5) != 0) return -1;
        hkdf_expand_label(t->c_ap, "traffic upd", NULL, 0, ns, 32);
        memcpy(t->c_ap, ns, 32);
        set_traffic_keys(&t->wr_gcm, t->wr_iv, t->c_ap); t->wr_seq = 0;
    }
    return 0;
}

/* Dispatch one complete handshake message. Returns <0 error, 1 handshake done, 0 ok. */
static int handle_hs_message(tls_ctx_t* t, uint8_t type, const uint8_t* body, uint32_t len,
                             const uint8_t* msg, uint32_t mlen) {
    int r;
    switch (t->state) {
        case TLS_S_WAIT_SH:
            if (type != HS_SERVER_HELLO) return fail(t, "expected ServerHello", NULL);
            sha256_update(&t->transcript, msg, mlen);
            return handle_server_hello(t, body, len);
        case TLS_S_WAIT_EE:
            if (type != HS_ENCRYPTED_EXT) return fail(t, "expected EncryptedExtensions", NULL);
            r = handle_encrypted_ext(t, body, len);
            sha256_update(&t->transcript, msg, mlen);
            return r;
        case TLS_S_WAIT_CERT:
            if (type == HS_CERT_REQUEST) return fail(t, "server requested a client certificate (unsupported)", NULL);
            if (type != HS_CERTIFICATE) return fail(t, "expected Certificate", NULL);
            r = handle_certificate(t, body, len);
            sha256_update(&t->transcript, msg, mlen);
            return r;
        case TLS_S_WAIT_CV:
            if (type != HS_CERT_VERIFY) return fail(t, "expected CertificateVerify", NULL);
            r = handle_cert_verify(t, body, len);
            sha256_update(&t->transcript, msg, mlen);
            return r;
        case TLS_S_WAIT_FIN:
            if (type != HS_FINISHED) return fail(t, "expected Finished", NULL);
            return handle_finished(t, body, len, msg, mlen);
        case TLS_S_CONNECTED:
            if (type == HS_NEW_SESSION_TICKET) return 0;        /* ignored: no resumption */
            if (type == HS_KEY_UPDATE) return handle_key_update(t, body, len);
            return 0;                                           /* ignore others */
        default:
            return fail(t, "unexpected handshake message", NULL);
    }
}

static int process_handshake(tls_ctx_t* t) {
    int done = 0;
    while (t->hs_len >= 4) {
        uint32_t mlen = ((uint32_t)t->hs[1] << 16) | ((uint32_t)t->hs[2] << 8) | t->hs[3];
        if (4 + mlen > TLS_HS_BUF) return fail(t, "handshake message too large", NULL);
        if (t->hs_len < 4 + mlen) return done;
        int r = handle_hs_message(t, t->hs[0], t->hs + 4, mlen, t->hs, 4 + mlen);
        if (r < 0) return -1;
        if (r == 1) done = 1;
        uint32_t rest = t->hs_len - (4 + mlen);
        if (rest) memcpy(t->hs, t->hs + 4 + mlen, rest);
        t->hs_len = rest;
    }
    return done;
}

/* ============================================================================
 * Record layer: receive one record. Returns 1 record processed, 0 need more
 * data, -1 error. Sets t->eof on close.
 * ============================================================================ */
static int recv_record(tls_ctx_t* t) {
    if (t->pt_off < t->pt_end) return 0;            /* app data not yet consumed */
    if (t->eof) return 0;
    if (!conn_ok(t)) {
        t->eof = true;
        if (t->state != TLS_S_CONNECTED && t->state != TLS_S_CLOSED)
            return fail(t, "connection closed during handshake", NULL);
        return 0;
    }
    while (t->rec_len < 5) {
        int n = tcp_recv(t->conn, t->rec + t->rec_len, (uint16_t)(5 - t->rec_len));
        if (n == 0) return 0;
        if (n < 0) {
            t->eof = true;
            if (t->rec_len == 0 && (t->state == TLS_S_CONNECTED || t->state == TLS_S_CLOSED)) return 0;
            return fail(t, "connection closed by server", NULL);
        }
        t->rec_len += (uint32_t)n;
    }
    uint32_t body = ((uint32_t)t->rec[3] << 8) | t->rec[4];
    if (body > TLS_MAX_RECORD) return fail(t, "record too large", NULL);
    while (t->rec_len < 5 + body) {
        int n = tcp_recv(t->conn, t->rec + t->rec_len, (uint16_t)(5 + body - t->rec_len));
        if (n == 0) return 0;
        if (n < 0) { t->eof = true; return fail(t, "truncated record (connection closed)", NULL); }
        t->rec_len += (uint32_t)n;
    }
    t->bytes_rx += 5 + body;
    t->rec_len = 0;                                 /* consumed (payload still at rec+5) */

    uint8_t type = t->rec[0];
    uint8_t* payload = t->rec + 5;
    uint32_t plen = body;
    if (type == CT_CCS) return 1;                   /* middlebox compatibility: ignore */
    if (t->rd_enc && type == CT_APPDATA) {
        if (body < 17) return fail(t, "record too short", NULL);
        uint8_t nonce[12];
        make_nonce(nonce, t->rd_iv, t->rd_seq);
        plen = body - 16;
        if (gcm_decrypt(&t->rd_gcm, nonce, t->rec, 5, payload, payload, plen, payload + plen) != 0) {
            send_alert(t, 2, 20);
            return fail(t, "bad record MAC", NULL);
        }
        t->rd_seq++;
        while (plen > 0 && payload[plen - 1] == 0) plen--;   /* strip padding */
        if (plen == 0) return fail(t, "empty inner record", NULL);
        type = payload[plen - 1];
        plen--;
    } else if (t->rd_enc && type != CT_ALERT) {
        return fail(t, "unencrypted record after key exchange", NULL);
    }

    switch (type) {
        case CT_HANDSHAKE:
            if (t->hs_len + plen > TLS_HS_BUF) return fail(t, "handshake message too large", NULL);
            memcpy(t->hs + t->hs_len, payload, plen);
            t->hs_len += plen;
            if (process_handshake(t) < 0) return -1;
            return 1;
        case CT_APPDATA:
            if (t->state != TLS_S_CONNECTED) return fail(t, "application data before handshake end", NULL);
            t->pt_off = (uint32_t)(payload - t->rec);
            t->pt_end = t->pt_off + plen;
            return 1;
        case CT_ALERT: {
            if (plen < 2) return fail(t, "malformed alert", NULL);
            uint8_t desc = payload[1];
            if (desc == 0) {
                t->eof = true;
                if (t->state == TLS_S_CONNECTED) { t->state = TLS_S_CLOSED; return 1; }
                return fail(t, "server closed the connection (close_notify)", NULL);
            }
            char d[8]; int_to_str(desc, d);
            t->error[0] = '\0';
            str_append(t->error, sizeof(t->error), "TLS alert ");
            str_append(t->error, sizeof(t->error), tls_alert_name(desc));
            str_append(t->error, sizeof(t->error), " (");
            str_append(t->error, sizeof(t->error), d);
            str_append(t->error, sizeof(t->error), ")");
            t->state = TLS_S_ERROR;
            return -1;
        }
        default:
            return fail(t, "unexpected record type", NULL);
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */
int tls_start(tls_ctx_t* t, tcp_conn_t* conn, const char* host, bool insecure) {
    /* zero everything but the big buffers (they are overwritten before use) */
    memset(t, 0, (size_t)((uint8_t*)t->rec - (uint8_t*)t));
    t->rec_len = 0; t->pt_off = t->pt_end = 0; t->hs_len = 0; t->leaf_spki_len = 0;
    t->trusted = false; t->root_name[0] = 0; t->reason[0] = 0; t->cert_count = 0;
    t->error[0] = 0; t->eof = false; t->close_sent = false; t->alpn_http11 = false;
    t->bytes_rx = t->bytes_tx = 0;
    t->conn = conn;
    t->conn_gen = conn ? conn->gen : 0;
    t->insecure = insecure;
    strncpy(t->host, host ? host : "", sizeof(t->host) - 1);
    t->host[sizeof(t->host) - 1] = '\0';
    t->hs_ticks = system_ticks;
    if (!conn_ok(t) || !conn->connected) return fail(t, "not connected", NULL);

    if (crypto_random(t->priv, 32) != 0) return fail(t, "secure random source unavailable (RDRAND required)", NULL);
    x25519_base(t->pub, t->priv);
    sha256_init(&t->transcript);
    t->state = TLS_S_WAIT_SH;
    if (send_client_hello(t) != 0) return -1;
    return 0;
}

int tls_poll(tls_ctx_t* t) {
    if (t->state == TLS_S_ERROR) return -1;
    if (t->state == TLS_S_CONNECTED) return 1;
    if (t->state == TLS_S_IDLE || t->state == TLS_S_CLOSED) return -1;
    for (int i = 0; i < 8; i++) {
        int r = recv_record(t);
        if (r < 0) return -1;
        if (t->state == TLS_S_CONNECTED) return 1;
        if (t->state == TLS_S_ERROR) return -1;
        if (r == 0) break;
    }
    return 0;
}

int tls_read(tls_ctx_t* t, uint8_t* buf, int max) {
    if (t->state == TLS_S_ERROR) return -2;
    if (max <= 0) return 0;
    for (int i = 0; i < 4; i++) {
        if (t->pt_off < t->pt_end) {
            uint32_t n = t->pt_end - t->pt_off;
            if (n > (uint32_t)max) n = (uint32_t)max;
            memcpy(buf, t->rec + t->pt_off, n);
            t->pt_off += n;
            return (int)n;
        }
        if (t->eof || t->state == TLS_S_CLOSED) return -1;
        if (t->state != TLS_S_CONNECTED) return 0;
        int r = recv_record(t);
        if (r < 0) return -2;
        if (r == 0) {
            if (t->pt_off < t->pt_end) continue;
            return (t->eof || t->state == TLS_S_CLOSED) ? -1 : 0;
        }
    }
    return 0;
}

int tls_write(tls_ctx_t* t, const uint8_t* data, int len) {
    if (t->state != TLS_S_CONNECTED) return -1;
    int done = 0;
    while (done < len) {
        uint32_t n = (uint32_t)(len - done);
        if (n > TLS_TX_PLAIN) n = TLS_TX_PLAIN;
        if (send_record(t, CT_APPDATA, data + done, n) != 0) return -1;
        done += (int)n;
    }
    return len;
}

void tls_close(tls_ctx_t* t) {
    if (t->state == TLS_S_CONNECTED && !t->close_sent && conn_ok(t)) {
        t->close_sent = true;
        send_alert(t, 1, 0);
    }
    if (t->state != TLS_S_ERROR) t->state = TLS_S_CLOSED;
}
