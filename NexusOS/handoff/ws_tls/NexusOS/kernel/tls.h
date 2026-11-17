/* ============================================================================
 * NexusOS — TLS 1.3 client (Phase 57)
 * ============================================================================
 * RFC 8446 client over a tcp_conn_t: TLS_AES_128_GCM_SHA256 with X25519 key
 * exchange, server authentication via X.509 chain validation against the
 * built-in root store (x509.c/roots.h), CertificateVerify with ECDSA
 * (P-256/P-384) or RSA-PSS, Finished HMAC, KeyUpdate, alerts. Non-blocking:
 * tls_poll()/tls_read() do a bounded amount of work per call; only sending
 * (tls_write) blocks inside tcp_send().
 *
 * Not supported (fails with a readable error): TLS 1.2 and below, HelloRetry-
 * Request, PSK/session resumption/0-RTT, client certificates, ChaCha20 and
 * SHA-384 cipher suites, certificate compression, OCSP/CRL.
 * ============================================================================ */
#ifndef TLS_H
#define TLS_H

#include "types.h"
#include "tcp.h"
#include "crypto.h"
#include "x509.h"

#define TLS_MAX_PLAINTEXT  16384
#define TLS_MAX_RECORD     (TLS_MAX_PLAINTEXT + 256)
#define TLS_REC_BUF        (5 + TLS_MAX_RECORD)
#define TLS_HS_BUF         16384          /* largest handshake message (Certificate) */
#define TLS_TX_PLAIN       2048           /* plaintext per outgoing record */
#define TLS_TX_BUF         (5 + TLS_TX_PLAIN + 1 + 16)
#define TLS_MAX_CHAIN      6
#define TLS_MAX_SPKI       600

typedef enum {
    TLS_S_IDLE = 0,
    TLS_S_WAIT_SH,          /* ClientHello sent */
    TLS_S_WAIT_EE,          /* keys derived, expecting EncryptedExtensions */
    TLS_S_WAIT_CERT,
    TLS_S_WAIT_CV,
    TLS_S_WAIT_FIN,
    TLS_S_CONNECTED,
    TLS_S_CLOSED,
    TLS_S_ERROR
} tls_state_t;

typedef struct {
    char subject[64];
    char issuer[64];
    char not_before[11];
    char not_after[11];
    int  key_type;          /* X509_KEY_* */
    int  key_bits;          /* RSA modulus bits / 256 / 384 */
    int  sig_alg, sig_hash;
} tls_cert_info_t;

typedef struct {
    tcp_conn_t* conn;
    uint32_t    conn_gen;
    tls_state_t state;
    char        host[128];
    bool        insecure;           /* X-Nexus-Insecure: downgrade cert hard-failures */

    /* handshake */
    uint8_t     priv[32], pub[32];
    sha256_ctx_t transcript;
    uint8_t     hs_secret[32];
    uint8_t     c_hs[32], s_hs[32];
    uint8_t     c_ap[32], s_ap[32];

    /* record protection */
    gcm_ctx_t   rd_gcm, wr_gcm;
    uint8_t     rd_iv[12], wr_iv[12];
    uint64_t    rd_seq, wr_seq;
    bool        rd_enc, wr_enc;

    /* incoming record assembly; after decryption the plaintext of an
     * application_data record sits at rec[pt_off..pt_end) */
    uint8_t     rec[TLS_REC_BUF];
    uint32_t    rec_len;
    uint32_t    pt_off, pt_end;

    /* handshake message reassembly */
    uint8_t     hs[TLS_HS_BUF];
    uint32_t    hs_len;

    /* leaf public key (SPKI DER copy) for CertificateVerify */
    uint8_t     leaf_spki[TLS_MAX_SPKI];
    uint32_t    leaf_spki_len;

    /* outgoing record scratch */
    uint8_t     tx[TLS_TX_BUF];

    /* results */
    bool        trusted;
    char        root_name[48];
    char        reason[80];         /* why not trusted ("" when trusted) */
    int         cert_count;
    tls_cert_info_t certs[TLS_MAX_CHAIN];
    char        error[96];
    bool        eof;                /* close_notify received or TCP closed */
    bool        close_sent;
    bool        alpn_http11;
    uint32_t    bytes_rx, bytes_tx; /* record bytes on the wire */
    uint32_t    hs_ticks;           /* handshake duration (ticks) */
} tls_ctx_t;

/* Reset a context and send the ClientHello over an ESTABLISHED connection.
 * 0 ok, -1 failed (see tls_error). */
int  tls_start(tls_ctx_t* t, tcp_conn_t* conn, const char* host, bool insecure);

/* Drive the handshake: 0 = in progress, 1 = connected (application data may
 * flow), -1 = failed. Safe to call after 1 (returns 1). */
int  tls_poll(tls_ctx_t* t);

/* Read decrypted application data: >0 bytes, 0 nothing yet, -1 EOF
 * (close_notify or TCP close), -2 error. */
int  tls_read(tls_ctx_t* t, uint8_t* buf, int max);

/* Encrypt + send application data (blocks in tcp_send). len or -1. */
int  tls_write(tls_ctx_t* t, const uint8_t* data, int len);

/* Send close_notify (if connected). The caller then tcp_close()s. */
void tls_close(tls_ctx_t* t);

const char* tls_error(const tls_ctx_t* t);
const char* tls_alert_name(int desc);

#endif /* TLS_H */
