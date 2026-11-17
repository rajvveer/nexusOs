/* ============================================================================
 * NexusOS — ASN.1/DER + X.509 (Phase 57)
 * ============================================================================ */
#include "x509.h"
#include "crypto.h"
#include "bignum.h"
#include "string.h"
#include "rtc.h"
#include "roots.h"

/* ============================================================================
 * Minimal DER reader
 * ============================================================================ */
typedef struct {
    const uint8_t* p;       /* start of the TLV */
    uint32_t tag;
    const uint8_t* val;     /* start of the content */
    uint32_t len;           /* content length */
    const uint8_t* end;     /* first byte after the TLV */
} der_t;

/* Read one TLV at [p, end). 0 ok, -1 malformed. */
static int der_read(const uint8_t* p, const uint8_t* end, der_t* out) {
    if (!p || p + 2 > end) return -1;
    out->p = p;
    out->tag = p[0];
    if ((p[0] & 0x1f) == 0x1f) return -1;     /* multi-byte tags: not used by X.509 */
    uint32_t len = p[1];
    const uint8_t* q = p + 2;
    if (len & 0x80) {
        uint32_t nb = len & 0x7f;
        if (nb == 0 || nb > 4 || q + nb > end) return -1;
        len = 0;
        for (uint32_t i = 0; i < nb; i++) len = (len << 8) | *q++;
    }
    if ((uint32_t)(end - q) < len) return -1;
    out->val = q;
    out->len = len;
    out->end = q + len;
    return 0;
}

/* Read a TLV and require a tag */
static int der_expect(const uint8_t* p, const uint8_t* end, uint32_t tag, der_t* out) {
    if (der_read(p, end, out) != 0) return -1;
    return out->tag == tag ? 0 : -1;
}

#define TAG_BOOL     0x01
#define TAG_INT      0x02
#define TAG_BITSTR   0x03
#define TAG_OCTSTR   0x04
#define TAG_NULL     0x05
#define TAG_OID      0x06
#define TAG_UTF8     0x0c
#define TAG_PRINT    0x13
#define TAG_IA5      0x16
#define TAG_UTC      0x17
#define TAG_GENTIME  0x18
#define TAG_SEQ      0x30
#define TAG_SET      0x31
#define TAG_CTX(n)   (0xa0 | (n))

/* ============================================================================
 * OIDs (DER-encoded contents)
 * ============================================================================ */
static const uint8_t OID_RSA_ENC[]        = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
static const uint8_t OID_SHA256_RSA[]     = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const uint8_t OID_SHA384_RSA[]     = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c};
static const uint8_t OID_SHA512_RSA[]     = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d};
static const uint8_t OID_SHA1_RSA[]       = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x05};
static const uint8_t OID_RSA_PSS[]        = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a};
static const uint8_t OID_EC_PUBKEY[]      = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
static const uint8_t OID_P256[]           = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
static const uint8_t OID_P384[]           = {0x2b,0x81,0x04,0x00,0x22};
static const uint8_t OID_ECDSA_SHA256[]   = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_SHA384[]   = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const uint8_t OID_ECDSA_SHA512[]   = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x04};
static const uint8_t OID_SHA256[]         = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01};
static const uint8_t OID_SHA384[]         = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02};
static const uint8_t OID_SHA512[]         = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03};
static const uint8_t OID_CN[]             = {0x55,0x04,0x03};
static const uint8_t OID_O[]              = {0x55,0x04,0x0a};
static const uint8_t OID_BASIC_CONSTR[]   = {0x55,0x1d,0x13};
static const uint8_t OID_SAN[]            = {0x55,0x1d,0x11};

static bool oid_eq(const der_t* d, const uint8_t* oid, uint32_t len) {
    return d->tag == TAG_OID && d->len == len && memcmp(d->val, oid, len) == 0;
}
#define OID_IS(d, o) oid_eq((d), (o), sizeof(o))

/* ============================================================================
 * Field helpers
 * ============================================================================ */
static void copy_str(char* dst, uint32_t dstsz, const uint8_t* src, uint32_t len) {
    uint32_t n = len < dstsz - 1 ? len : dstsz - 1;
    for (uint32_t i = 0; i < n; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '?';
    dst[n] = '\0';
}

/* Extract CN (fallback O) from a Name */
static void name_display(const uint8_t* name, uint32_t len, char* out, uint32_t outsz) {
    out[0] = '\0';
    der_t seq;
    if (der_expect(name, name + len, TAG_SEQ, &seq) != 0) return;
    const uint8_t* p = seq.val;
    char org[64] = "";
    while (p < seq.end) {
        der_t set, s2, oid, val;
        if (der_expect(p, seq.end, TAG_SET, &set) != 0) return;
        if (der_expect(set.val, set.end, TAG_SEQ, &s2) == 0 &&
            der_read(s2.val, s2.end, &oid) == 0 &&
            der_read(oid.end, s2.end, &val) == 0) {
            if (OID_IS(&oid, OID_CN)) { copy_str(out, outsz, val.val, val.len); return; }
            if (OID_IS(&oid, OID_O) && !org[0]) copy_str(org, sizeof(org), val.val, val.len);
        }
        p = set.end;
    }
    if (!out[0]) strncpy(out, org, outsz - 1), out[outsz - 1] = '\0';
}

static int two_digits(const uint8_t* p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static int parse_time(const der_t* t, x509_time_t* out) {
    const uint8_t* p = t->val;
    uint32_t year;
    if (t->tag == TAG_UTC) {                       /* YYMMDDHHMMSSZ */
        if (t->len < 13) return -1;
        int yy = two_digits(p); if (yy < 0) return -1;
        year = (uint32_t)(yy >= 50 ? 1900 + yy : 2000 + yy);
        p += 2;
    } else if (t->tag == TAG_GENTIME) {            /* YYYYMMDDHHMMSSZ */
        if (t->len < 15) return -1;
        int a = two_digits(p), b = two_digits(p + 2);
        if (a < 0 || b < 0) return -1;
        year = (uint32_t)(a * 100 + b);
        p += 4;
    } else return -1;
    int mo = two_digits(p), d = two_digits(p + 2), h = two_digits(p + 4), mi = two_digits(p + 6), s = two_digits(p + 8);
    if (mo < 0 || d < 0 || h < 0 || mi < 0 || s < 0) return -1;
    out->date = year * 10000 + (uint32_t)mo * 100 + (uint32_t)d;
    out->time = (uint32_t)h * 10000 + (uint32_t)mi * 100 + (uint32_t)s;
    return 0;
}

void x509_format_date(x509_time_t t, char* buf) {
    uint32_t y = t.date / 10000, m = (t.date / 100) % 100, d = t.date % 100;
    buf[0] = (char)('0' + y / 1000); buf[1] = (char)('0' + (y / 100) % 10);
    buf[2] = (char)('0' + (y / 10) % 10); buf[3] = (char)('0' + y % 10);
    buf[4] = '-'; buf[5] = (char)('0' + m / 10); buf[6] = (char)('0' + m % 10);
    buf[7] = '-'; buf[8] = (char)('0' + d / 10); buf[9] = (char)('0' + d % 10);
    buf[10] = '\0';
}

/* AlgorithmIdentifier -> (sig_alg, hash) */
static void parse_sig_alg(const der_t* alg, int* sig_alg, int* hash) {
    der_t oid;
    *sig_alg = X509_SIG_UNKNOWN; *hash = HASH_NONE;
    if (der_read(alg->val, alg->end, &oid) != 0 || oid.tag != TAG_OID) return;
    if (OID_IS(&oid, OID_SHA256_RSA))      { *sig_alg = X509_SIG_RSA_PKCS1; *hash = HASH_SHA256; }
    else if (OID_IS(&oid, OID_SHA384_RSA)) { *sig_alg = X509_SIG_RSA_PKCS1; *hash = HASH_SHA384; }
    else if (OID_IS(&oid, OID_SHA512_RSA)) { *sig_alg = X509_SIG_RSA_PKCS1; *hash = HASH_SHA512; }
    else if (OID_IS(&oid, OID_ECDSA_SHA256)) { *sig_alg = X509_SIG_ECDSA; *hash = HASH_SHA256; }
    else if (OID_IS(&oid, OID_ECDSA_SHA384)) { *sig_alg = X509_SIG_ECDSA; *hash = HASH_SHA384; }
    else if (OID_IS(&oid, OID_ECDSA_SHA512)) { *sig_alg = X509_SIG_ECDSA; *hash = HASH_SHA512; }
    else if (OID_IS(&oid, OID_SHA1_RSA))   { *sig_alg = X509_SIG_UNSUPPORTED; }
    else if (OID_IS(&oid, OID_RSA_PSS)) {
        /* RSASSA-PSS-params: [0] hashAlgorithm (default SHA-1 -> unsupported) */
        *sig_alg = X509_SIG_RSA_PSS; *hash = HASH_NONE;
        der_t params, h0, halg, hoid;
        if (der_expect(oid.end, alg->end, TAG_SEQ, &params) == 0 &&
            der_expect(params.val, params.end, TAG_CTX(0), &h0) == 0 &&
            der_expect(h0.val, h0.end, TAG_SEQ, &halg) == 0 &&
            der_read(halg.val, halg.end, &hoid) == 0) {
            if (OID_IS(&hoid, OID_SHA256)) *hash = HASH_SHA256;
            else if (OID_IS(&hoid, OID_SHA384)) *hash = HASH_SHA384;
            else if (OID_IS(&hoid, OID_SHA512)) *hash = HASH_SHA512;
        }
        if (*hash == HASH_NONE) *sig_alg = X509_SIG_UNSUPPORTED;
    }
}

/* SubjectPublicKeyInfo */
int x509_parse_spki(const uint8_t* der, uint32_t len, x509_cert_t* c) {
    der_t spki, alg, oid, bits;
    if (der_expect(der, der + len, TAG_SEQ, &spki) != 0) return -1;
    c->spki = spki.p; c->spki_len = (uint32_t)(spki.end - spki.p);
    if (der_expect(spki.val, spki.end, TAG_SEQ, &alg) != 0) return -1;
    if (der_read(alg.val, alg.end, &oid) != 0 || oid.tag != TAG_OID) return -1;
    if (der_expect(alg.end, spki.end, TAG_BITSTR, &bits) != 0 || bits.len < 2 || bits.val[0] != 0) return -1;
    c->key_type = X509_KEY_OTHER;
    if (OID_IS(&oid, OID_RSA_ENC)) {
        der_t rsa, n, e;
        if (der_expect(bits.val + 1, bits.end, TAG_SEQ, &rsa) != 0) return -1;
        if (der_expect(rsa.val, rsa.end, TAG_INT, &n) != 0) return -1;
        if (der_expect(n.end, rsa.end, TAG_INT, &e) != 0) return -1;
        c->rsa_n = n.val; c->rsa_n_len = n.len;
        c->rsa_e = e.val; c->rsa_e_len = e.len;
        c->key_type = X509_KEY_RSA;
    } else if (OID_IS(&oid, OID_EC_PUBKEY)) {
        der_t curve;
        if (der_read(oid.end, alg.end, &curve) != 0 || curve.tag != TAG_OID) return -1;
        c->ec_point = bits.val + 1; c->ec_point_len = bits.len - 1;
        if (OID_IS(&curve, OID_P256) && c->ec_point_len == 65) c->key_type = X509_KEY_P256;
        else if (OID_IS(&curve, OID_P384) && c->ec_point_len == 97) c->key_type = X509_KEY_P384;
    }
    return 0;
}

/* Extensions: basicConstraints, subjectAltName */
static int parse_extensions(const der_t* exts, x509_cert_t* c) {
    const uint8_t* p = exts->val;
    while (p < exts->end) {
        der_t ext, oid, val;
        if (der_expect(p, exts->end, TAG_SEQ, &ext) != 0) return -1;
        if (der_read(ext.val, ext.end, &oid) != 0 || oid.tag != TAG_OID) return -1;
        const uint8_t* q = oid.end;
        if (der_read(q, ext.end, &val) != 0) return -1;
        if (val.tag == TAG_BOOL) {                 /* critical flag */
            if (der_read(val.end, ext.end, &val) != 0) return -1;
        }
        if (val.tag != TAG_OCTSTR) return -1;
        if (OID_IS(&oid, OID_BASIC_CONSTR)) {
            der_t bc, ca;
            c->has_basic_constraints = true;
            c->is_ca = false;
            if (der_expect(val.val, val.end, TAG_SEQ, &bc) == 0 && bc.len > 0 &&
                der_read(bc.val, bc.end, &ca) == 0 && ca.tag == TAG_BOOL && ca.len == 1)
                c->is_ca = ca.val[0] != 0;
        } else if (OID_IS(&oid, OID_SAN)) {
            der_t gn;
            if (der_expect(val.val, val.end, TAG_SEQ, &gn) == 0) { c->san = gn.val; c->san_len = gn.len; }
        }
        p = ext.end;
    }
    return 0;
}

int x509_parse(const uint8_t* der, uint32_t len, x509_cert_t* c) {
    memset(c, 0, sizeof(*c));
    der_t cert, tbs, d, alg, sigval;
    if (der_expect(der, der + len, TAG_SEQ, &cert) != 0) return -1;
    c->raw = cert.p; c->raw_len = (uint32_t)(cert.end - cert.p);
    if (der_expect(cert.val, cert.end, TAG_SEQ, &tbs) != 0) return -1;
    c->tbs = tbs.p; c->tbs_len = (uint32_t)(tbs.end - tbs.p);

    const uint8_t* p = tbs.val;
    /* [0] version */
    if (der_read(p, tbs.end, &d) != 0) return -1;
    c->version = 1;
    if (d.tag == TAG_CTX(0)) {
        der_t v;
        if (der_expect(d.val, d.end, TAG_INT, &v) != 0 || v.len != 1) return -1;
        c->version = v.val[0] + 1;
        p = d.end;
        if (der_read(p, tbs.end, &d) != 0) return -1;
    }
    /* serialNumber */
    if (d.tag != TAG_INT) return -1;
    c->serial = d.val; c->serial_len = d.len;
    p = d.end;
    /* signature AlgorithmIdentifier (inner) */
    if (der_expect(p, tbs.end, TAG_SEQ, &alg) != 0) return -1;
    parse_sig_alg(&alg, &c->sig_alg, &c->sig_hash);
    p = alg.end;
    /* issuer */
    if (der_expect(p, tbs.end, TAG_SEQ, &d) != 0) return -1;
    c->issuer = d.p; c->issuer_len = (uint32_t)(d.end - d.p);
    p = d.end;
    /* validity */
    if (der_expect(p, tbs.end, TAG_SEQ, &d) != 0) return -1;
    {
        der_t nb, na;
        if (der_read(d.val, d.end, &nb) != 0 || der_read(nb.end, d.end, &na) != 0) return -1;
        if (parse_time(&nb, &c->not_before) != 0 || parse_time(&na, &c->not_after) != 0) return -1;
    }
    p = d.end;
    /* subject */
    if (der_expect(p, tbs.end, TAG_SEQ, &d) != 0) return -1;
    c->subject = d.p; c->subject_len = (uint32_t)(d.end - d.p);
    p = d.end;
    /* subjectPublicKeyInfo */
    if (der_expect(p, tbs.end, TAG_SEQ, &d) != 0) return -1;
    if (x509_parse_spki(d.p, (uint32_t)(d.end - d.p), c) != 0) return -1;
    p = d.end;
    /* optional [1] issuerUniqueID, [2] subjectUniqueID, [3] extensions */
    while (p < tbs.end) {
        if (der_read(p, tbs.end, &d) != 0) return -1;
        if (d.tag == TAG_CTX(3)) {
            der_t exts;
            if (der_expect(d.val, d.end, TAG_SEQ, &exts) != 0) return -1;
            if (parse_extensions(&exts, c) != 0) return -1;
        }
        p = d.end;
    }
    /* outer signatureAlgorithm + signatureValue */
    if (der_expect(tbs.end, cert.end, TAG_SEQ, &alg) != 0) return -1;
    {
        int a, h;
        parse_sig_alg(&alg, &a, &h);
        /* the outer algorithm must match the inner one */
        if (a != c->sig_alg || h != c->sig_hash) c->sig_alg = X509_SIG_UNKNOWN;
    }
    if (der_expect(alg.end, cert.end, TAG_BITSTR, &sigval) != 0 || sigval.len < 2 || sigval.val[0] != 0) return -1;
    c->sig = sigval.val + 1; c->sig_len = sigval.len - 1;

    c->self_signed = (c->issuer_len == c->subject_len && memcmp(c->issuer, c->subject, c->issuer_len) == 0);
    name_display(c->subject, c->subject_len, c->subject_cn, sizeof(c->subject_cn));
    name_display(c->issuer, c->issuer_len, c->issuer_cn, sizeof(c->issuer_cn));
    return 0;
}

/* ============================================================================
 * Hostname matching
 * ============================================================================ */
static char lower(char ch) { return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }

static bool name_matches(const uint8_t* pat, uint32_t plen, const char* host) {
    uint32_t hlen = strlen(host);
    if (plen >= 2 && pat[0] == '*' && pat[1] == '.') {
        /* wildcard: exactly one label, at least two labels remain */
        const char* dot = host;
        while (*dot && *dot != '.') dot++;
        if (!*dot) return false;
        const char* rest = dot;                   /* ".example.com" */
        uint32_t rlen = hlen - (uint32_t)(rest - host);
        if (rlen != plen - 1) return false;
        for (uint32_t i = 0; i < rlen; i++) if (lower((char)pat[1 + i]) != lower(rest[i])) return false;
        return true;
    }
    if (plen != hlen) return false;
    for (uint32_t i = 0; i < hlen; i++) if (lower((char)pat[i]) != lower(host[i])) return false;
    return true;
}

/* dotted-quad -> 4 bytes; false if `s` is not an IPv4 literal */
static bool parse_ipv4(const char* s, uint8_t out[4]) {
    int part = 0, val = -1;
    for (; ; s++) {
        if (*s >= '0' && *s <= '9') {
            val = (val < 0 ? 0 : val * 10) + (*s - '0');
            if (val > 255) return false;
        } else if (*s == '.' || *s == '\0') {
            if (val < 0 || part > 3) return false;
            out[part++] = (uint8_t)val; val = -1;
            if (*s == '\0') return part == 4;
        } else return false;
    }
}

bool x509_match_hostname(const x509_cert_t* c, const char* host) {
    if (!host || !host[0]) return false;
    uint8_t ip[4];
    bool is_ip = parse_ipv4(host, ip);
    if (c->san) {
        const uint8_t* p = c->san; const uint8_t* end = c->san + c->san_len;
        bool any_dns = false;
        while (p < end) {
            der_t gn;
            if (der_read(p, end, &gn) != 0) break;
            if (gn.tag == 0x82) {                  /* [2] dNSName IA5String */
                any_dns = true;
                if (!is_ip && name_matches(gn.val, gn.len, host)) return true;
            } else if (gn.tag == 0x87 && gn.len == 4) {   /* [7] iPAddress */
                if (is_ip && memcmp(gn.val, ip, 4) == 0) return true;
            }
            p = gn.end;
        }
        if (any_dns || is_ip) return false;        /* SAN present: CN must not be used */
    }
    if (is_ip) return false;
    /* fall back to CN */
    der_t seq;
    if (der_expect(c->subject, c->subject + c->subject_len, TAG_SEQ, &seq) != 0) return false;
    const uint8_t* p = seq.val;
    while (p < seq.end) {
        der_t set, s2, oid, val;
        if (der_expect(p, seq.end, TAG_SET, &set) != 0) return false;
        if (der_expect(set.val, set.end, TAG_SEQ, &s2) == 0 && der_read(s2.val, s2.end, &oid) == 0 &&
            der_read(oid.end, s2.end, &val) == 0 && OID_IS(&oid, OID_CN)) {
            return name_matches(val.val, val.len, host);
        }
        p = set.end;
    }
    return false;
}

/* ============================================================================
 * Validity
 * ============================================================================ */
int x509_check_validity(const x509_cert_t* c) {
    rtc_time_t t;
    rtc_read(&t);
    x509_time_t now;
    now.date = (uint32_t)t.year * 10000 + (uint32_t)t.month * 100 + t.day;
    now.time = (uint32_t)t.hour * 10000 + (uint32_t)t.minute * 100 + t.second;
    if (now.date < c->not_before.date || (now.date == c->not_before.date && now.time < c->not_before.time)) return -1;
    if (now.date > c->not_after.date || (now.date == c->not_after.date && now.time > c->not_after.time)) return -2;
    return 0;
}

/* ============================================================================
 * Signature verification
 * ============================================================================ */
int x509_verify_raw(const x509_cert_t* key, int sig_alg, int sig_hash,
                    const uint8_t* msg, uint32_t msg_len,
                    const uint8_t* sig, uint32_t sig_len) {
    uint8_t dig[64];
    int hl = hash_compute(sig_hash, msg, msg_len, dig);
    if (hl == 0) return -2;
    if (sig_alg == X509_SIG_RSA_PKCS1 || sig_alg == X509_SIG_RSA_PSS) {
        if (key->key_type != X509_KEY_RSA) return -1;
        return rsa_verify(key->rsa_n, key->rsa_n_len, key->rsa_e, key->rsa_e_len,
                          sig, sig_len, sig_hash, dig, sig_alg == X509_SIG_RSA_PSS);
    }
    if (sig_alg == X509_SIG_ECDSA) {
        int curve;
        if (key->key_type == X509_KEY_P256) curve = EC_P256;
        else if (key->key_type == X509_KEY_P384) curve = EC_P384;
        else return key->key_type == X509_KEY_OTHER ? -2 : -1;
        der_t seq, r, s;
        if (der_expect(sig, sig + sig_len, TAG_SEQ, &seq) != 0) return -1;
        if (der_expect(seq.val, seq.end, TAG_INT, &r) != 0) return -1;
        if (der_expect(r.end, seq.end, TAG_INT, &s) != 0) return -1;
        return ecdsa_verify(curve, key->ec_point, key->ec_point_len, dig, (uint32_t)hl,
                            r.val, r.len, s.val, s.len);
    }
    return -2;
}

int x509_verify_signature(const x509_cert_t* c, const x509_cert_t* issuer) {
    if (c->sig_alg == X509_SIG_UNKNOWN || c->sig_alg == X509_SIG_UNSUPPORTED) return -2;
    if (issuer->key_type == X509_KEY_OTHER) return -2;
    return x509_verify_raw(issuer, c->sig_alg, c->sig_hash, c->tbs, c->tbs_len, c->sig, c->sig_len);
}

/* ============================================================================
 * Chain validation
 * ============================================================================ */
static void set_reason(x509_chain_result_t* r, const char* a, const char* b) {
    uint32_t n = 0;
    while (a[n] && n < sizeof(r->reason) - 1) { r->reason[n] = a[n]; n++; }
    if (b) while (*b && n < sizeof(r->reason) - 1) r->reason[n++] = *b++;
    r->reason[n] = '\0';
}

static const root_cert_t* find_root(const uint8_t* issuer, uint32_t len) {
    for (int i = 0; i < ROOT_STORE_COUNT; i++) {
        if (ROOT_STORE[i].subject_len == len && memcmp(ROOT_STORE[i].subject, issuer, len) == 0)
            return &ROOT_STORE[i];
    }
    return NULL;
}

int x509_verify_chain(const x509_cert_t* certs, int count, const char* hostname,
                      bool insecure, x509_chain_result_t* res) {
    memset(res, 0, sizeof(*res));
    res->root_name = "";
    if (count < 1) { set_reason(res, "no certificate", NULL); res->hard_fail = true; return -1; }
    const x509_cert_t* leaf = &certs[0];
    bool hard = false;

    /* leaf checks */
    if (!x509_match_hostname(leaf, hostname)) {
        set_reason(res, "hostname mismatch (cert is for ", leaf->subject_cn);
        uint32_t n = strlen(res->reason);
        if (n < sizeof(res->reason) - 1) { res->reason[n] = ')'; res->reason[n + 1] = '\0'; }
        hard = true;
    }
    int v = x509_check_validity(leaf);
    if (v != 0 && !hard) { set_reason(res, v == -2 ? "certificate expired" : "certificate not yet valid", NULL); hard = true; }

    /* walk up */
    const x509_cert_t* cur = leaf;
    bool used[8] = {false};
    used[0] = true;
    for (int depth = 0; depth < 6; depth++) {
        res->depth = depth + 1;
        const root_cert_t* root = find_root(cur->issuer, cur->issuer_len);
        if (root) {
            x509_cert_t rk;
            memset(&rk, 0, sizeof(rk));
            if (x509_parse_spki(root->spki, root->spki_len, &rk) != 0) { set_reason(res, "bad root key", NULL); break; }
            int r = x509_verify_signature(cur, &rk);
            if (r == 0) {
                res->trusted = !hard;
                res->root_name = root->name;
                if (!hard) res->reason[0] = '\0';
                break;
            }
            if (r == -2) { set_reason(res, "unsupported signature algorithm (issuer ", cur->issuer_cn); break; }
            if (!hard) set_reason(res, "bad signature by ", root->name);
            hard = true;
            break;
        }
        /* intermediate from the chain */
        const x509_cert_t* next = NULL;
        for (int i = 1; i < count && i < 8; i++) {
            if (used[i]) continue;
            if (certs[i].subject_len == cur->issuer_len &&
                memcmp(certs[i].subject, cur->issuer, cur->issuer_len) == 0) { next = &certs[i]; used[i] = true; break; }
        }
        if (!next) {
            if (cur->self_signed) set_reason(res, "self-signed certificate", NULL);
            else set_reason(res, "unknown issuer: ", cur->issuer_cn);
            break;
        }
        int r = x509_verify_signature(cur, next);
        if (r == -2) { set_reason(res, "unsupported signature algorithm (issuer ", cur->issuer_cn); break; }
        if (r != 0) { if (!hard) set_reason(res, "bad signature by ", next->subject_cn); hard = true; break; }
        if (!next->has_basic_constraints || !next->is_ca) {
            if (!hard) set_reason(res, "issuer is not a CA: ", next->subject_cn);
            hard = true; break;
        }
        v = x509_check_validity(next);
        if (v != 0) { if (!hard) set_reason(res, "intermediate expired: ", next->subject_cn); hard = true; break; }
        cur = next;
    }
    if (hard) {
        res->trusted = false;
        res->hard_fail = !insecure;
        return insecure ? 0 : -1;
    }
    return 0;
}
