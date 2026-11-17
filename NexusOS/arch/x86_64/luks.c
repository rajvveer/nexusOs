/* LUKS1 and LUKS2 for the encrypted root (cryptroot.c) and the installer's "Encrypt disk" (osinstall.c), written
 * from the LUKS1 On-Disk Format Specification 1.2.3 and the LUKS2 On-Disk Format Specification 1.1.x (cryptsetup):
 *   unlock: every enabled keyslot is tried with the passphrase -- LUKS2: argon2id / argon2i / pbkdf2-sha256 KDFs,
 *   raw keyslot areas encrypted with any dm-crypt cipher of kcrypto.h, the luks1 anti-forensic splitter (sha256),
 *   pbkdf2 digests; LUKS1: pbkdf2-sha256 keyslots; both header copies of LUKS2 are checked (SHA-256 checksum,
 *   the higher seqid wins) -- and yields the volume key and the data segment (offset, size, cipher, sector size);
 *   format: a LUKS2 header pair like cryptsetup 2.x writes (16 KiB headers, 16 MiB data offset, keyslot 0 with
 *   argon2id and a 4000-stripe AF split, aes-xts-plain64 with a 512-bit volume key, a pbkdf2-sha256 digest).
 * Hostile input: every JSON value, number, offset and size is bounds-checked against the device; a malformed header
 * is reported as "not LUKS" or "corrupt", never trusted. Keys and derived material are wiped after use. */
#include "native.h"
#include "kcrypto.h"
#include "luks.h"
#include "storage_util.h"
#include "../../kernel/vga.h"

/* ---- small JSON reader over a bounded text (spans into the header buffer) ---- */
struct span { const char* p; uint32_t n; };
static const char* skip_ws(const char* p, const char* end) { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; return p; }
static const char* skip_value(const char* p, const char* end, unsigned depth);
static const char* skip_string(const char* p, const char* end) {
    if (p >= end || *p != '"') return NULL;
    for (p++; p < end; p++) { if (*p == '\\') { if (++p >= end) return NULL; continue; } if (*p == '"') return p + 1; }
    return NULL;
}
static const char* skip_value(const char* p, const char* end, unsigned depth) {
    p = skip_ws(p, end); if (p >= end || depth > 16) return NULL;
    if (*p == '"') return skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']'; bool object = *p == '{';
        p = skip_ws(p + 1, end); if (p < end && *p == close) return p + 1;
        for (;;) {
            if (object) { p = skip_string(skip_ws(p, end), end); if (!p) return NULL; p = skip_ws(p, end); if (p >= end || *p != ':') return NULL; p++; }
            p = skip_value(p, end, depth + 1); if (!p) return NULL;
            p = skip_ws(p, end); if (p >= end) return NULL;
            if (*p == ',') { p++; continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    const char* start = p;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= 'a' && *p <= 'z'))) p++;
    return p > start ? p : NULL;
}
static bool span_is(struct span s, const char* text) { unsigned n = strlen(text); return s.n == n + 2 && s.p[0] == '"' && !memcmp(s.p + 1, text, n); }
/* the next member of an object (*cursor NULL at first): key and value spans; false past the end. One pass over the
 * object for all its members (a hostile header with many members costs linear, not quadratic, time). */
static bool json_next(struct span object, const char** cursor, struct span* key, struct span* value) {
    const char *p = *cursor, *end = object.p + object.n;
    if (!p) { if (!object.n || *object.p != '{') return false; p = skip_ws(object.p + 1, end); }
    if (p >= end || *p == '}') return false;
    const char* k = p; p = skip_string(p, end); if (!p) return false;
    key->p = k; key->n = (uint32_t)(p - k);
    p = skip_ws(p, end); if (p >= end || *p != ':') return false;
    const char* v = skip_ws(p + 1, end); p = skip_value(v, end, 0); if (!p) return false;
    value->p = v; value->n = (uint32_t)(p - v);
    p = skip_ws(p, end); if (p < end && *p == ',') p = skip_ws(p + 1, end);
    *cursor = p; return true;
}
static bool json_get(struct span object, const char* name, struct span* value) {
    struct span key; const char* cursor = NULL;
    while (json_next(object, &cursor, &key, value)) if (span_is(key, name)) return true;
    return false;
}
#define LUKS2_SLOTS 32   /* cryptsetup's keyslot/digest limit; more members are ignored */
static bool json_string(struct span s, char* out, unsigned capacity) {   /* no escapes in LUKS strings */
    if (s.n < 2 || s.p[0] != '"' || s.p[s.n - 1] != '"' || s.n - 2 >= capacity) return false;
    for (uint32_t i = 1; i + 1 < s.n; i++) if (s.p[i] == '\\') return false;
    memcpy(out, s.p + 1, s.n - 2); out[s.n - 2] = 0; return true;
}
static bool json_u64(struct span s, uint64_t* out) {   /* a number or a decimal string (LUKS2 64-bit fields) */
    const char* p = s.p; uint32_t n = s.n;
    if (n >= 2 && p[0] == '"' && p[n - 1] == '"') { p++; n -= 2; }
    if (!n || n > 20) return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) { if (p[i] < '0' || p[i] > '9') return false; uint64_t d = (uint64_t)(p[i] - '0'); if (v > (~0ULL - d) / 10) return false; v = v * 10 + d; }
    *out = v; return true;
}
static bool json_array_has(struct span array, const char* text) {   /* ["0","1"] contains "text" */
    const char *p = array.p, *end = array.p + array.n;
    if (!array.n || *p != '[') return false;
    p = skip_ws(p + 1, end);
    while (p < end && *p != ']') {
        const char* v = p; p = skip_value(p, end, 0); if (!p) return false;
        if (span_is((struct span){v, (uint32_t)(p - v)}, text)) return true;
        p = skip_ws(p, end); if (p < end && *p == ',') p = skip_ws(p + 1, end);
    }
    return false;
}
static int base64_value(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
}
static int base64_decode(struct span s, uint8_t* out, unsigned capacity) {   /* bytes, or -1 */
    char text[256]; if (!json_string(s, text, sizeof(text))) return -1;
    unsigned n = 0, bits = 0, value = 0;
    for (const char* p = text; *p && *p != '='; p++) {
        int v = base64_value(*p); if (v < 0) return -1;
        value = value << 6 | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= capacity) return -1; out[n++] = (uint8_t)(value >> bits); }
    }
    return (int)n;
}
static void base64_encode(const uint8_t* data, unsigned bytes, char* out) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned n = 0;
    for (unsigned i = 0; i < bytes; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < bytes ? (uint32_t)data[i + 1] << 8 : 0) | (i + 2 < bytes ? data[i + 2] : 0);
        out[n++] = table[v >> 18 & 63]; out[n++] = table[v >> 12 & 63];
        out[n++] = i + 1 < bytes ? table[v >> 6 & 63] : '='; out[n++] = i + 2 < bytes ? table[v & 63] : '=';
    }
    out[n] = 0;
}

/* ---- anti-forensic splitter (LUKS1 AF, sha256) ---- */
static void diffuse(uint8_t* block, unsigned size) {
    uint8_t digest[32]; unsigned i = 0;
    for (; i * 32 < size; i++) {
        unsigned n = size - i * 32 < 32 ? size - i * 32 : 32;
        uint8_t iv[4] = {(uint8_t)(i >> 24), (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i};
        struct sha256 s; sha256_init(&s); sha256_update(&s, iv, 4); sha256_update(&s, block + i * 32, n); sha256_final(&s, digest);
        memcpy(block + i * 32, digest, n);
    }
    crypto_wipe(digest, sizeof(digest));
}
static void af_merge(const uint8_t* split, unsigned key_bytes, unsigned stripes, uint8_t* key) {
    uint8_t buffer[64]; memset(buffer, 0, sizeof(buffer));
    for (unsigned i = 0; i + 1 < stripes; i++) { for (unsigned k = 0; k < key_bytes; k++) buffer[k] ^= split[i * key_bytes + k]; diffuse(buffer, key_bytes); }
    for (unsigned k = 0; k < key_bytes; k++) key[k] = split[(stripes - 1) * key_bytes + k] ^ buffer[k];
    crypto_wipe(buffer, sizeof(buffer));
}
static bool af_split(const uint8_t* key, unsigned key_bytes, unsigned stripes, uint8_t* split) {
    uint8_t buffer[64]; memset(buffer, 0, sizeof(buffer));
    if (!native_random(split, (uint64_t)key_bytes * (stripes - 1))) return false;
    for (unsigned i = 0; i + 1 < stripes; i++) { for (unsigned k = 0; k < key_bytes; k++) buffer[k] ^= split[i * key_bytes + k]; diffuse(buffer, key_bytes); }
    for (unsigned k = 0; k < key_bytes; k++) split[(stripes - 1) * key_bytes + k] = key[k] ^ buffer[k];
    crypto_wipe(buffer, sizeof(buffer)); return true;
}

/* ---- keyslot area: decrypt/encrypt with a dm-crypt cipher, 512-byte units numbered from the area start ---- */
static int area_crypt(const struct block_partition* part, uint64_t offset, uint8_t* data, uint64_t bytes, int mode,
                      const uint8_t* key, unsigned key_bytes, bool decrypt, bool write) {
    struct xts_key* x = (void*)arena_alloc(sizeof(struct xts_key)); if (!x) return -12;
    int e = cipher_set_key(x, mode, key, key_bytes);
    uint64_t sectors = (bytes + 511) / 512;
    if (!e && !write && block_part_read(part, offset / 512, (uint32_t)sectors, data) < 0) e = -5;
    for (uint64_t s = 0; !e && s < sectors; s++) xts_crypt(x, data + s * 512, 512, s, decrypt);
    if (!e && write && block_part_write(part, offset / 512, (uint32_t)sectors, data) < 0) e = -5;
    xts_wipe(x); arena_free((uint64_t)x, sizeof(*x));
    return e;
}

/* ---- LUKS2 header ---- */
#define HDR_MAX (4u << 20)   /* metadata area limit: 4 MiB (JSON up to 4 MiB - 4 KiB) */
static const uint8_t magic1[6] = {'L', 'U', 'K', 'S', 0xBA, 0xBE}, magic2[6] = {'S', 'K', 'U', 'L', 0xBA, 0xBE};
#define be64 get_be64
#define be32 get_be32
/* reads and checks one header copy at `offset`; returns its area (arena, hdr_size bytes) or NULL */
static uint8_t* read_header(const struct block_partition* part, uint64_t offset, uint64_t* size, uint64_t* seqid) {
    uint8_t first[512];
    if (offset % 512 || block_part_read(part, offset / 512, 1, first) < 0) return NULL;
    if (memcmp(first, offset ? magic2 : magic1, 6) || (first[6] << 8 | first[7]) != 2) return NULL;
    uint64_t hdr_size = be64(first + 8);
    if (hdr_size < 16384 || hdr_size > HDR_MAX || hdr_size % 4096 || be64(first + 256) != offset) return NULL;
    if (memcmp(first + 72, "sha256", 7)) return NULL;   /* the only checksum cryptsetup writes */
    if ((offset + 2 * hdr_size) / 512 > block_part_sectors(part)) return NULL;
    uint8_t* area = (void*)arena_alloc(hdr_size + 1); if (!area) return NULL;
    if (block_part_read(part, offset / 512, (uint32_t)(hdr_size / 512), area) < 0) { arena_free((uint64_t)area, hdr_size + 1); return NULL; }
    uint8_t stored[32], computed[32]; memcpy(stored, area + 448, 32); memset(area + 448, 0, 64);
    sha256(area, hdr_size, computed);
    memcpy(area + 448, stored, 32);
    if (memcmp(stored, computed, 32)) { arena_free((uint64_t)area, hdr_size + 1); return NULL; }
    area[hdr_size] = 0; *size = hdr_size; *seqid = be64(area + 16);
    return area;
}
struct keyslot_try { const struct block_partition* part; const char* passphrase; unsigned passphrase_bytes; struct luks_volume* out; };
static int digest_check(struct span digests, const char* slot_name, const uint8_t* key, unsigned key_bytes) {   /* 0 match, -1 no */
    struct span name, d; const char* cursor = NULL;
    for (unsigned i = 0; i < LUKS2_SLOTS && json_next(digests, &cursor, &name, &d); i++) {
        struct span type, keyslots, hash, iterations, salt, digest; uint64_t iters; char text[16];
        if (!json_get(d, "type", &type) || !span_is(type, "pbkdf2") || !json_get(d, "keyslots", &keyslots) || !json_array_has(keyslots, slot_name)) continue;
        if (!json_get(d, "hash", &hash) || !json_string(hash, text, sizeof(text)) || strcmp(text, "sha256")) continue;
        if (!json_get(d, "iterations", &iterations) || !json_u64(iterations, &iters) || !iters || iters > 0xFFFFFFFFULL) continue;
        uint8_t salt_bytes[64], want[64], got[64];
        int sn = json_get(d, "salt", &salt) ? base64_decode(salt, salt_bytes, sizeof(salt_bytes)) : -1;
        int dn = json_get(d, "digest", &digest) ? base64_decode(digest, want, sizeof(want)) : -1;
        if (sn <= 0 || dn <= 0) continue;
        pbkdf2_sha256(key, key_bytes, salt_bytes, (unsigned)sn, (uint32_t)iters, got, (unsigned)dn);
        int diff = 0; for (int k = 0; k < dn; k++) diff |= got[k] ^ want[k];
        crypto_wipe(got, sizeof(got));
        if (!diff) return 0;
    }
    return -1;
}
static int luks2_keyslot(const struct block_partition* part, struct span slot, const char* slot_name, struct span digests,
                         const char* passphrase, unsigned passphrase_bytes, uint8_t* key, unsigned* key_bytes) {
    struct span type, size, af, area, kdf, v; uint64_t ks, offset, area_size, stripes; char text[64];
    if (!json_get(slot, "type", &type) || !span_is(type, "luks2")) return -95;
    if (!json_get(slot, "key_size", &size) || !json_u64(size, &ks) || !ks || ks > 64) return -22;
    if (!json_get(slot, "af", &af) || !json_get(af, "type", &v) || !span_is(v, "luks1") || !json_get(af, "stripes", &v) || !json_u64(v, &stripes) ||
        !stripes || stripes > 65536 || !json_get(af, "hash", &v) || !json_string(v, text, sizeof(text)) || strcmp(text, "sha256")) return -95;
    if (!json_get(slot, "area", &area) || !json_get(area, "type", &v) || !span_is(v, "raw") || !json_get(area, "offset", &v) || !json_u64(v, &offset) ||
        !json_get(area, "size", &v) || !json_u64(v, &area_size) || !json_get(area, "encryption", &v) || !json_string(v, text, sizeof(text))) return -22;
    int mode = cipher_mode(text); uint64_t area_key = 0;
    if (!mode || !json_get(area, "key_size", &v) || !json_u64(v, &area_key) || !area_key || area_key > 64) return -95;
    uint64_t split_bytes = ks * stripes;
    if (offset % 512 || split_bytes > area_size || offset / 512 + (split_bytes + 511) / 512 > block_part_sectors(part) || split_bytes > (4u << 20)) return -22;
    if (!json_get(slot, "kdf", &kdf) || !json_get(kdf, "type", &v)) return -22;
    uint8_t salt[128], derived[64]; struct span salt_span;
    int salt_bytes = json_get(kdf, "salt", &salt_span) ? base64_decode(salt_span, salt, sizeof(salt)) : -1;
    if (salt_bytes <= 0) return -22;
    int e;
    if (span_is(v, "pbkdf2")) {
        struct span hash, iterations; uint64_t iters;
        if (!json_get(kdf, "hash", &hash) || !json_string(hash, text, sizeof(text)) || strcmp(text, "sha256")) return -95;
        if (!json_get(kdf, "iterations", &iterations) || !json_u64(iterations, &iters) || !iters || iters > 0xFFFFFFFFULL) return -22;
        e = pbkdf2_sha256((const uint8_t*)passphrase, passphrase_bytes, salt, (unsigned)salt_bytes, (uint32_t)iters, derived, (unsigned)area_key);
    } else if (span_is(v, "argon2id") || span_is(v, "argon2i")) {
        struct span t, m, c; uint64_t time, memory, cpus;
        if (!json_get(kdf, "time", &t) || !json_u64(t, &time) || !json_get(kdf, "memory", &m) || !json_u64(m, &memory) ||
            !json_get(kdf, "cpus", &c) || !json_u64(c, &cpus) || !time || time > 1000 || memory < 8 || memory > (4u << 20) || !cpus || cpus > 64) return -22;
        e = argon2_hash(span_is(v, "argon2id") ? 2 : 1, (const uint8_t*)passphrase, passphrase_bytes, salt, (unsigned)salt_bytes,
                        (uint32_t)time, (uint32_t)memory, (uint32_t)cpus, derived, (unsigned)area_key);
    } else return -95;
    if (e) { crypto_wipe(derived, sizeof(derived)); return e; }
    uint64_t buffer_bytes = (split_bytes + 511) / 512 * 512;
    uint8_t* split = (void*)arena_alloc(buffer_bytes);
    if (!split) { crypto_wipe(derived, sizeof(derived)); return -12; }
    e = area_crypt(part, offset, split, split_bytes, mode, derived, (unsigned)area_key, true, false);
    crypto_wipe(derived, sizeof(derived));
    if (!e) {
        af_merge(split, (unsigned)ks, (unsigned)stripes, key);
        e = digest_check(digests, slot_name, key, (unsigned)ks) ? -1 : 0;   /* -1: wrong passphrase for this slot */
        if (e) crypto_wipe(key, 64); else *key_bytes = (unsigned)ks;
    }
    crypto_wipe(split, buffer_bytes); arena_free((uint64_t)split, buffer_bytes);
    return e;
}
static int luks2_unlock(const struct block_partition* part, const char* passphrase, unsigned passphrase_bytes, struct luks_volume* out) {
    static const uint64_t secondary[] = {16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304};
    uint64_t size1 = 0, seq1 = 0, size2 = 0, seq2 = 0, size = 0;
    uint8_t* h1 = read_header(part, 0, &size1, &seq1); uint8_t* h2 = NULL;
    for (unsigned i = 0; i < sizeof(secondary) / sizeof(secondary[0]) && !h2; i++) h2 = read_header(part, secondary[i], &size2, &seq2);
    uint8_t* h = h1 && (!h2 || seq1 >= seq2) ? h1 : h2;
    if (!h) return -61;   /* no valid LUKS2 header copy (ENODATA) */
    size = h == h1 ? size1 : size2;
    if (h1 && h1 != h) arena_free((uint64_t)h1, size1 + 1);
    if (h2 && h2 != h) arena_free((uint64_t)h2, size2 + 1);
    struct span json = {(const char*)h + 4096, 0};
    while (json.n < size - 4096 && json.p[json.n]) json.n++;
    const char* end = skip_value(json.p, json.p + json.n, 0);
    int result = -22;
    struct span keyslots, segments, digests, name, slot, segment, v;
    memcpy(out->uuid, h + 168, 40); out->uuid[39] = 0;
    if (!end || !json_get(json, "keyslots", &keyslots) || !json_get(json, "segments", &segments) || !json_get(json, "digests", &digests)) goto done;
    const char* cursor = NULL;
    if (!json_next(segments, &cursor, &name, &segment) || !json_get(segment, "type", &v) || !span_is(v, "crypt")) { result = -95; goto done; }
    uint64_t offset, seg_size = 0, sector_size = 512, iv_tweak = 0; char cipher[64];
    if (!json_get(segment, "offset", &v) || !json_u64(v, &offset) || offset % 512 || offset / 512 >= block_part_sectors(part)) goto done;
    if (!json_get(segment, "size", &v)) goto done;
    if (!span_is(v, "dynamic") && (!json_u64(v, &seg_size) || seg_size % 512 || seg_size / 512 > block_part_sectors(part) - offset / 512)) goto done;
    if (json_get(segment, "sector_size", &v) && (!json_u64(v, &sector_size) || sector_size < 512 || sector_size > 4096 || (sector_size & (sector_size - 1)))) goto done;
    if (json_get(segment, "iv_tweak", &v) && !json_u64(v, &iv_tweak)) goto done;
    if (!json_get(segment, "encryption", &v) || !json_string(v, cipher, sizeof(cipher)) || !cipher_mode(cipher)) { result = -95; goto done; }
    if (json_get(segment, "integrity", &v)) { result = -95; goto done; }   /* dm-integrity is not available */
    result = -1;   /* no keyslot opened */
    int unusable = 0; bool tried = false; cursor = NULL;
    for (unsigned i = 0; i < LUKS2_SLOTS && json_next(keyslots, &cursor, &name, &slot); i++) {
        char slot_name[8]; if (!json_string(name, slot_name, sizeof(slot_name))) continue;
        struct span priority; uint64_t p;
        if (json_get(slot, "priority", &priority) && json_u64(priority, &p) && p == 0) continue;   /* "ignore" priority */
        int e = luks2_keyslot(part, slot, slot_name, digests, passphrase, passphrase_bytes, out->key, &out->key_bytes);
        if (!e) { out->keyslot = (int)i; result = 0; break; }
        if (e == -12) { result = -12; break; }
        if (e == -1) tried = true; else unusable = e;
    }
    if (result == -1 && !tried) result = unusable ? unusable : -22;   /* no keyslot could even be tried: not "wrong passphrase" */
    if (!result) {
        out->version = 2; out->offset_sectors = offset / 512;
        out->size_sectors = seg_size ? seg_size / 512 : block_part_sectors(part) - offset / 512;
        out->sector_size = (uint32_t)sector_size; out->iv_tweak = iv_tweak;
        strcpy(out->cipher, cipher);
    }
done:
    crypto_wipe(h, size); arena_free((uint64_t)h, size + 1);
    return result;
}

/* ---- LUKS1 ---- */
static int luks1_unlock(const struct block_partition* part, const char* passphrase, unsigned passphrase_bytes, struct luks_volume* out) {
    uint8_t h[1024];
    if (block_part_read(part, 0, 2, h) < 0) return -5;
    if (memcmp(h, magic1, 6) || (h[6] << 8 | h[7]) != 1) return -61;
    char name[33], mode_text[33], hash[33]; memcpy(name, h + 8, 32); name[32] = 0; memcpy(mode_text, h + 40, 32); mode_text[32] = 0; memcpy(hash, h + 72, 32); hash[32] = 0;
    if (strcmp(hash, "sha256")) return -95;   /* sha1 LUKS1 volumes (cryptsetup < 1.7) are not supported */
    char cipher[72]; strcpy(cipher, name); strcat(cipher, "-"); strcat(cipher, mode_text);
    int mode = cipher_mode(cipher); if (!mode) return -95;
    uint32_t payload = be32(h + 104), key_bytes = be32(h + 108), digest_iterations = be32(h + 164);
    if (!key_bytes || key_bytes > 64 || !digest_iterations || payload >= block_part_sectors(part)) return -22;
    memcpy(out->uuid, h + 168, 40); out->uuid[39] = 0;
    bool tried = false;
    for (unsigned s = 0; s < 8; s++) {
        const uint8_t* k = h + 208 + 48 * s;
        if (be32(k) != 0x00AC71F3) continue;
        uint32_t iterations = be32(k + 4), material = be32(k + 40), stripes = be32(k + 44);
        uint64_t split_bytes = (uint64_t)key_bytes * stripes;
        if (!iterations || !stripes || stripes > 65536 || split_bytes > (4u << 20) || material + (split_bytes + 511) / 512 > payload) continue;
        tried = true;
        uint8_t derived[64];
        pbkdf2_sha256((const uint8_t*)passphrase, passphrase_bytes, k + 8, 32, iterations, derived, key_bytes);
        uint64_t buffer_bytes = (split_bytes + 511) / 512 * 512;
        uint8_t* split = (void*)arena_alloc(buffer_bytes); if (!split) { crypto_wipe(derived, sizeof(derived)); return -12; }
        int e = area_crypt(part, (uint64_t)material * 512, split, split_bytes, mode, derived, key_bytes, true, false);
        crypto_wipe(derived, sizeof(derived));
        if (!e) {
            af_merge(split, key_bytes, stripes, out->key);
            uint8_t check[20]; pbkdf2_sha256(out->key, key_bytes, h + 132, 32, digest_iterations, check, 20);
            int diff = 0; for (unsigned i = 0; i < 20; i++) diff |= check[i] ^ h[112 + i];
            if (!diff) {
                crypto_wipe(split, buffer_bytes); arena_free((uint64_t)split, buffer_bytes);
                out->version = 1; out->keyslot = (int)s; out->key_bytes = key_bytes; out->offset_sectors = payload;
                out->size_sectors = block_part_sectors(part) - payload; out->sector_size = 512; out->iv_tweak = 0; strcpy(out->cipher, cipher);
                return 0;
            }
            crypto_wipe(out->key, sizeof(out->key));
        }
        crypto_wipe(split, buffer_bytes); arena_free((uint64_t)split, buffer_bytes);
        if (e == -12) return -12;
    }
    return tried ? -1 : -22;   /* no usable active keyslot: damaged, not a wrong passphrase */
}

int luks_probe(const struct block_partition* part) {   /* 1 / 2 (version), 0 not LUKS */
    uint8_t h[512];
    if (!part || block_part_read(part, 0, 1, h) < 0 || memcmp(h, magic1, 6)) return 0;
    unsigned version = (unsigned)(h[6] << 8 | h[7]);
    return version == 1 || version == 2 ? (int)version : 0;
}
int luks_unlock(const struct block_partition* part, const char* passphrase, unsigned passphrase_bytes, struct luks_volume* out) {
    memset(out, 0, sizeof(*out));
    int version = luks_probe(part);
    int e = version == 2 ? luks2_unlock(part, passphrase, passphrase_bytes, out) : version == 1 ? luks1_unlock(part, passphrase, passphrase_bytes, out) : -61;
    if (e) crypto_wipe(out->key, sizeof(out->key));
    return e;
}

/* ---- LUKS2 format (installer) ---- */
static void uuid_text(const uint8_t r[16], char* out) {
    static const char hex[] = "0123456789abcdef"; unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        uint8_t b = r[i]; if (i == 6) b = (uint8_t)((b & 0x0F) | 0x40); if (i == 8) b = (uint8_t)((b & 0x3F) | 0x80);
        out[n++] = hex[b >> 4]; out[n++] = hex[b & 15];
    }
    out[n] = 0;
}
static void append_number(char* text, uint64_t v) { char n[24] = ""; append_u64(n, v); strcat(text, n); }
int luks2_format(const struct block_partition* part, const char* passphrase, unsigned passphrase_bytes, const struct luks_format_options* o, struct luks_volume* out) {
    enum { HDR = 16384, AREA_OFFSET = 32768, DATA_OFFSET = 16777216, STRIPES = 4000, KEY = 64 };
    uint64_t sectors = block_part_sectors(part);
    if (sectors < DATA_OFFSET / 512 + 2048) return -28;
    memset(out, 0, sizeof(*out));
    uint8_t volume_key[KEY], salt[32], digest_salt[32], digest[32], derived[KEY], random_uuid[16];
    if (!native_random(volume_key, KEY) || !native_random(salt, 32) || !native_random(digest_salt, 32) || !native_random(random_uuid, 16)) return -5;
    char uuid[40]; uuid_text(random_uuid, uuid);
    uint32_t time = o && o->time ? o->time : 4, memory = o && o->memory_kib ? o->memory_kib : 262144, lanes = o && o->lanes ? o->lanes : 1;
    uint32_t digest_iterations = o && o->digest_iterations ? o->digest_iterations : 200000;
    int e = argon2_hash(2, (const uint8_t*)passphrase, passphrase_bytes, salt, 32, time, memory, lanes, derived, KEY);
    uint64_t split_bytes = (uint64_t)KEY * STRIPES, area_size = (split_bytes + 4095) / 4096 * 4096;
    uint8_t* split = e ? NULL : (void*)arena_alloc(area_size);
    char* json = split ? (void*)arena_alloc(HDR) : NULL;
    if (!e && (!split || !json)) e = -12;
    if (!e && !af_split(volume_key, KEY, STRIPES, split)) e = -5;
    if (!e) { memset(split + split_bytes, 0, area_size - split_bytes); e = area_crypt(part, AREA_OFFSET, split, area_size, CIPHER_XTS_PLAIN64, derived, KEY, false, true); }
    crypto_wipe(derived, sizeof(derived));
    if (!e) pbkdf2_sha256(volume_key, KEY, digest_salt, 32, digest_iterations, digest, 32);
    if (!e) {
        char b64_salt[48], b64_dsalt[48], b64_digest[48]; base64_encode(salt, 32, b64_salt); base64_encode(digest_salt, 32, b64_dsalt); base64_encode(digest, 32, b64_digest);
        char* t = json + 4096; memset(json, 0, HDR);
        strcpy(t, "{\"keyslots\":{\"0\":{\"type\":\"luks2\",\"key_size\":64,\"af\":{\"type\":\"luks1\",\"stripes\":4000,\"hash\":\"sha256\"},"
                  "\"area\":{\"type\":\"raw\",\"offset\":\"32768\",\"size\":\""); append_number(t, area_size);
        strcat(t, "\",\"encryption\":\"aes-xts-plain64\",\"key_size\":64},\"kdf\":{\"type\":\"argon2id\",\"time\":"); append_number(t, time);
        strcat(t, ",\"memory\":"); append_number(t, memory); strcat(t, ",\"cpus\":"); append_number(t, lanes);
        strcat(t, ",\"salt\":\""); strcat(t, b64_salt); strcat(t, "\"}}},\"tokens\":{},\"segments\":{\"0\":{\"type\":\"crypt\",\"offset\":\"16777216\","
                  "\"size\":\"dynamic\",\"iv_tweak\":\"0\",\"encryption\":\"aes-xts-plain64\",\"sector_size\":512}},"
                  "\"digests\":{\"0\":{\"type\":\"pbkdf2\",\"keyslots\":[\"0\"],\"segments\":[\"0\"],\"hash\":\"sha256\",\"iterations\":");
        append_number(t, digest_iterations); strcat(t, ",\"salt\":\""); strcat(t, b64_dsalt); strcat(t, "\",\"digest\":\""); strcat(t, b64_digest);
        strcat(t, "\"}},\"config\":{\"json_size\":\"12288\",\"keyslots_size\":\""); append_number(t, DATA_OFFSET - AREA_OFFSET); strcat(t, "\"}}");
        for (unsigned copy = 0; copy < 2 && !e; copy++) {
            uint8_t* b = (uint8_t*)json; memset(b, 0, 4096);
            memcpy(b, copy ? magic2 : magic1, 6); b[6] = 0; b[7] = 2;
            put_be64(b + 8, HDR); put_be64(b + 16, 1);
            memcpy(b + 72, "sha256", 6);
            if (!native_random(b + 104, 64)) { e = -5; break; }
            memcpy(b + 168, uuid, 36);
            put_be64(b + 256, copy ? HDR : 0);
            uint8_t sum[32]; sha256(b, HDR, sum); memcpy(b + 448, sum, 32);
            if (block_part_write(part, copy ? HDR / 512 : 0, HDR / 512, b) < 0) e = -5;
        }
    }
    if (!e && block_flush(part->dev) < 0) e = -5;
    if (split) { crypto_wipe(split, area_size); arena_free((uint64_t)split, area_size); }
    if (json) { crypto_wipe(json, HDR); arena_free((uint64_t)json, HDR); }
    if (!e) {
        out->version = 2; out->keyslot = 0; memcpy(out->key, volume_key, KEY); out->key_bytes = KEY;
        out->offset_sectors = DATA_OFFSET / 512; out->size_sectors = sectors - DATA_OFFSET / 512; out->sector_size = 512; out->iv_tweak = 0;
        strcpy(out->cipher, "aes-xts-plain64"); strcpy(out->uuid, uuid);
    }
    crypto_wipe(volume_key, sizeof(volume_key)); crypto_wipe(digest, sizeof(digest));
    return e;
}

/* dm-crypt mapping NAME over the volume's data segment, keyed with the unlocked volume key (dm.c) */
int dm_crypt_create(const char* name, const char* uuid, const struct block_partition* under, uint64_t offset_sectors, uint64_t sectors,
                    const char* cipher, const uint8_t* key, unsigned key_bytes, uint32_t sector_size, uint64_t iv_offset,
                    unsigned flags, struct block_device** out);
int luks_activate_flags(const struct block_partition* part, const struct luks_volume* v, const char* name, unsigned flags, struct block_device** out) {
    char uuid[129]; strcpy(uuid, "CRYPT-LUKS"); append_number(uuid, (uint64_t)v->version); strcat(uuid, "-");   /* cryptsetup's dm uuid */
    for (const char* p = v->uuid; *p && strlen(uuid) < 60; p++) if (*p != '-') { char c[2] = {*p, 0}; strcat(uuid, c); }
    if (strlen(uuid) + 1 + strlen(name) >= sizeof(uuid)) return -22;
    strcat(uuid, "-"); strcat(uuid, name);
    uint64_t iv = v->iv_tweak;   /* plain64 IV: the 512-byte sector number (+ iv_tweak), also with 4096-byte data units (cryptsetup's table) */
    return dm_crypt_create(name, uuid, part, v->offset_sectors, v->size_sectors - v->size_sectors % (v->sector_size / 512), v->cipher,
                           v->key, v->key_bytes, v->sector_size, iv, flags, out);
}
int luks_activate(const struct block_partition* part, const struct luks_volume* v, const char* name, struct block_device** out) {
    return luks_activate_flags(part, v, name, 0, out);
}
