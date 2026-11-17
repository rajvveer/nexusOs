/* ============================================================================
 * NexusOS — ASN.1/DER + X.509 certificate parsing and chain validation
 * (Phase 57)
 * ============================================================================
 * Parses the subset of X.509 v3 needed by a TLS client: tbsCertificate,
 * serial, issuer/subject Names, validity, SubjectPublicKeyInfo (RSA or EC
 * P-256/P-384), basicConstraints, subjectAltName dNSNames, and the outer
 * signature. Validation walks leaf -> intermediates -> built-in root store
 * (roots.h), checking signatures, dates, CA flag and the hostname.
 * All pointers in x509_cert_t point INTO the caller's DER buffer.
 * ============================================================================ */
#ifndef X509_H
#define X509_H

#include "types.h"

#define X509_KEY_NONE   0
#define X509_KEY_RSA    1
#define X509_KEY_P256   2
#define X509_KEY_P384   3
#define X509_KEY_OTHER  4       /* recognised SPKI but unsupported algorithm/curve */

/* signature algorithm ids (sig_alg) */
#define X509_SIG_UNKNOWN      0
#define X509_SIG_RSA_PKCS1    1     /* sha{256,384,512}WithRSAEncryption; hash in sig_hash */
#define X509_SIG_RSA_PSS      2     /* RSASSA-PSS, hash in sig_hash */
#define X509_SIG_ECDSA        3     /* ecdsa-with-SHA{256,384}; hash in sig_hash */
#define X509_SIG_UNSUPPORTED  4     /* recognised but unsupported (sha1WithRSA, ...) */

typedef struct {
    uint32_t date;              /* YYYYMMDD */
    uint32_t time;              /* HHMMSS   */
} x509_time_t;

typedef struct {
    const uint8_t* raw;      uint32_t raw_len;       /* the whole Certificate */
    const uint8_t* tbs;      uint32_t tbs_len;       /* tbsCertificate incl. its header (signed bytes) */
    const uint8_t* serial;   uint32_t serial_len;
    const uint8_t* issuer;   uint32_t issuer_len;    /* Name DER incl. SEQUENCE header */
    const uint8_t* subject;  uint32_t subject_len;
    const uint8_t* spki;     uint32_t spki_len;      /* SubjectPublicKeyInfo DER */
    x509_time_t not_before, not_after;
    int  version;                                    /* 1..3 */
    int  sig_alg, sig_hash;                          /* X509_SIG_* / HASH_* */
    const uint8_t* sig;      uint32_t sig_len;       /* signatureValue (BIT STRING payload) */
    /* public key */
    int  key_type;                                   /* X509_KEY_* */
    const uint8_t* rsa_n;    uint32_t rsa_n_len;
    const uint8_t* rsa_e;    uint32_t rsa_e_len;
    const uint8_t* ec_point; uint32_t ec_point_len;  /* 04 || X || Y */
    /* extensions */
    bool has_basic_constraints, is_ca;
    int path_len;                                   /* -1 = unconstrained */
    bool has_key_usage, digital_signature, key_cert_sign;
    bool has_eku, server_auth, unsupported_constraint;
    const uint8_t* san;      uint32_t san_len;       /* GeneralNames SEQUENCE payload (no header) */
    bool self_signed;                                /* issuer == subject */
    char subject_cn[64];                             /* CN (or O) for display */
    char issuer_cn[64];
} x509_cert_t;

/* Parse one DER certificate. 0 ok, -1 malformed/unsupported structure. */
int x509_parse(const uint8_t* der, uint32_t len, x509_cert_t* out);

/* Parse a bare SubjectPublicKeyInfo (used for the root store). */
int x509_parse_spki(const uint8_t* der, uint32_t len, x509_cert_t* key_out);

/* Hostname check against SAN dNSNames (wildcard *.example.com = one label),
 * falling back to the subject CN when there is no SAN. */
bool x509_match_hostname(const x509_cert_t* c, const char* host);

/* Validity against the RTC: 0 ok, -1 not yet valid, -2 expired. */
int x509_check_validity(const x509_cert_t* c);

/* Verify c's signature with the public key in `issuer` (any x509_cert_t whose
 * key fields are filled). 0 ok, -1 bad signature, -2 unsupported algorithm. */
int x509_verify_signature(const x509_cert_t* c, const x509_cert_t* issuer);

/* Verify a raw signature made by `key` over `msg` (TLS CertificateVerify).
 * sig_alg = X509_SIG_xxx, sig_hash = HASH_xxx; for ECDSA `sig` is the DER
 * SEQUENCE { r, s }. 0 ok, -1 bad, -2 unsupported. */
int x509_verify_raw(const x509_cert_t* key, int sig_alg, int sig_hash,
                    const uint8_t* msg, uint32_t msg_len,
                    const uint8_t* sig, uint32_t sig_len);

/* Chain validation result */
typedef struct {
    bool  trusted;              /* chain ends at a built-in root */
    bool  hard_fail;            /* bad signature / expired / hostname mismatch / not a CA */
    const char* root_name;      /* matching root ("" if none) */
    char  reason[80];           /* why trusted == false or hard_fail */
    int   depth;                /* certificates checked */
} x509_chain_result_t;

/* certs[0] is the leaf; the rest in any order. Untrusted certificates fail
 * unless the caller explicitly selected insecure mode. */
int x509_verify_chain(const x509_cert_t* certs, int count, const char* hostname,
                      bool insecure, x509_chain_result_t* res);

/* Format "YYYY-MM-DD" into buf (>= 11 bytes) */
void x509_format_date(x509_time_t t, char* buf);

#endif /* X509_H */
