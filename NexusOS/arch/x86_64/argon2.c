/* BLAKE2b (RFC 7693) and Argon2d/Argon2i/Argon2id version 1.3 (RFC 9106), written from the RFCs for LUKS2 keyslots
 * (luks.c). Lanes are filled one after the other (the result does not depend on that: a segment only references
 * blocks of finished slices in other lanes). The memory (memory_kib blocks of 1 KiB) lives in page-allocated
 * two-level vectors, so no large contiguous allocation is needed; it is wiped and freed before returning. Data-
 * independent addressing (Argon2i, first half of Argon2id's first pass) does not depend on secrets; data-dependent
 * addressing (Argon2d, the rest of Argon2id) does by design. */
#include "native.h"
#include "kcrypto.h"
#include "../../kernel/string.h"

/* ---- BLAKE2b ---- */
static const uint64_t blake2b_iv[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                       0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
static const uint8_t sigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};
static inline uint64_t ror64(uint64_t x, unsigned n) { return x >> n | x << (64 - n); }
static inline uint64_t load64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static inline void store64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void store32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
struct blake2b { uint64_t h[8], t[2]; uint8_t buffer[128]; unsigned used, out; };
static void b2_compress(struct blake2b* s, const uint8_t* block, bool last) {
    uint64_t m[16], v[16];
    for (unsigned i = 0; i < 16; i++) m[i] = load64(block + 8 * i);
    for (unsigned i = 0; i < 8; i++) { v[i] = s->h[i]; v[i + 8] = blake2b_iv[i]; }
    v[12] ^= s->t[0]; v[13] ^= s->t[1]; if (last) v[14] = ~v[14];
#define B2G(a, b, c, d, x, y) do { v[a] += v[b] + (x); v[d] = ror64(v[d] ^ v[a], 32); v[c] += v[d]; v[b] = ror64(v[b] ^ v[c], 24); \
                                   v[a] += v[b] + (y); v[d] = ror64(v[d] ^ v[a], 16); v[c] += v[d]; v[b] = ror64(v[b] ^ v[c], 63); } while (0)
    for (unsigned r = 0; r < 12; r++) {
        const uint8_t* z = sigma[r];
        B2G(0, 4, 8, 12, m[z[0]], m[z[1]]); B2G(1, 5, 9, 13, m[z[2]], m[z[3]]); B2G(2, 6, 10, 14, m[z[4]], m[z[5]]); B2G(3, 7, 11, 15, m[z[6]], m[z[7]]);
        B2G(0, 5, 10, 15, m[z[8]], m[z[9]]); B2G(1, 6, 11, 12, m[z[10]], m[z[11]]); B2G(2, 7, 8, 13, m[z[12]], m[z[13]]); B2G(3, 4, 9, 14, m[z[14]], m[z[15]]);
    }
#undef B2G
    for (unsigned i = 0; i < 8; i++) s->h[i] ^= v[i] ^ v[i + 8];
    crypto_wipe(m, sizeof(m)); crypto_wipe(v, sizeof(v));
}
static void b2_init(struct blake2b* s, unsigned out, const uint8_t* key, unsigned key_bytes) {
    memset(s, 0, sizeof(*s)); s->out = out;
    for (unsigned i = 0; i < 8; i++) s->h[i] = blake2b_iv[i];
    s->h[0] ^= 0x01010000ULL ^ ((uint64_t)key_bytes << 8) ^ out;
    if (key_bytes) { memcpy(s->buffer, key, key_bytes); s->used = 128; }
}
static void b2_update(struct blake2b* s, const void* data, uint64_t bytes) {
    const uint8_t* p = data;
    while (bytes) {
        if (s->used == 128) {   /* only compress a full buffer once more input follows (the last block is final) */
            s->t[0] += 128; if (s->t[0] < 128) s->t[1]++;
            b2_compress(s, s->buffer, false); s->used = 0;
        }
        unsigned n = 128 - s->used; if (n > bytes) n = (unsigned)bytes;
        memcpy(s->buffer + s->used, p, n); s->used += n; p += n; bytes -= n;
    }
}
static void b2_final(struct blake2b* s, uint8_t* out) {
    s->t[0] += s->used; if (s->t[0] < s->used) s->t[1]++;
    memset(s->buffer + s->used, 0, 128 - s->used);
    b2_compress(s, s->buffer, true);
    uint8_t full[64]; for (unsigned i = 0; i < 8; i++) store64(full + 8 * i, s->h[i]);
    memcpy(out, full, s->out); crypto_wipe(full, sizeof(full)); crypto_wipe(s, sizeof(*s));
}
void blake2b(const void* data, uint64_t bytes, const uint8_t* key, unsigned key_bytes, uint8_t* out, unsigned out_bytes) {
    struct blake2b s; b2_init(&s, out_bytes, key, key_bytes); b2_update(&s, data, bytes); b2_final(&s, out);
}
/* H' (RFC 9106 3.3): variable-length hash */
static void hprime(uint8_t* out, uint32_t out_bytes, const uint8_t* in, uint32_t in_bytes) {
    uint8_t length[4]; store32(length, out_bytes);
    struct blake2b s;
    if (out_bytes <= 64) { b2_init(&s, out_bytes, NULL, 0); b2_update(&s, length, 4); b2_update(&s, in, in_bytes); b2_final(&s, out); return; }
    uint8_t v[64];
    b2_init(&s, 64, NULL, 0); b2_update(&s, length, 4); b2_update(&s, in, in_bytes); b2_final(&s, v);
    memcpy(out, v, 32); out += 32; out_bytes -= 32;
    while (out_bytes > 64) { blake2b(v, 64, NULL, 0, v, 64); memcpy(out, v, 32); out += 32; out_bytes -= 32; }
    blake2b(v, 64, NULL, 0, out, out_bytes);
    crypto_wipe(v, sizeof(v));
}

/* ---- Argon2 ---- */
struct block { uint64_t v[128]; };
#define BLOCKS_PER_PAGE 4ULL
#define PTRS 512ULL
struct memory { uint64_t* top; uint64_t blocks; };
static void memory_free(struct memory* m) {
    if (!m->top) return;
    for (uint64_t i = 0; i < PTRS; i++) {
        uint64_t* mid = (uint64_t*)m->top[i]; if (!mid) continue;
        for (uint64_t j = 0; j < PTRS; j++) if (mid[j]) { crypto_wipe((void*)mid[j], 4096); page_free(mid[j]); }
        page_free((uint64_t)mid);
    }
    page_free((uint64_t)m->top); m->top = NULL;
}
static bool memory_alloc(struct memory* m, uint64_t blocks) {
    uint64_t pages = (blocks + BLOCKS_PER_PAGE - 1) / BLOCKS_PER_PAGE;
    m->blocks = blocks; m->top = NULL;
    uint64_t memory_free_pages(void);
    /* refuse up front what would drain the machine (a hostile LUKS header may ask for 4 GiB): keep 16 MiB free */
    if (pages > PTRS * PTRS || pages + pages / PTRS + 2 + 4096 > memory_free_pages()) return false;
    m->top = (uint64_t*)page_alloc(); if (!m->top) return false;
    memset(m->top, 0, 4096);
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t* mid = (uint64_t*)m->top[p / PTRS];
        if (!mid) { mid = (uint64_t*)page_alloc(); if (!mid) return false; memset(mid, 0, 4096); m->top[p / PTRS] = (uint64_t)mid; }
        uint64_t page = page_alloc(); if (!page) return false;
        mid[p % PTRS] = page;
    }
    return true;
}
static inline struct block* at(const struct memory* m, uint64_t index) {
    uint64_t page = index / BLOCKS_PER_PAGE;
    return (struct block*)(((uint64_t*)m->top[page / PTRS])[page % PTRS] + (index % BLOCKS_PER_PAGE) * 1024);
}
static inline uint64_t blamka(uint64_t x, uint64_t y) { return x + y + 2 * (uint64_t)(uint32_t)x * (uint32_t)y; }
#define GB(a, b, c, d) do { a = blamka(a, b); d = ror64(d ^ a, 32); c = blamka(c, d); b = ror64(b ^ c, 24); \
                            a = blamka(a, b); d = ror64(d ^ a, 16); c = blamka(c, d); b = ror64(b ^ c, 63); } while (0)
#define ROUND(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) do { \
    GB(v0, v4, v8, v12); GB(v1, v5, v9, v13); GB(v2, v6, v10, v14); GB(v3, v7, v11, v15); \
    GB(v0, v5, v10, v15); GB(v1, v6, v11, v12); GB(v2, v7, v8, v13); GB(v3, v4, v9, v14); } while (0)
/* next = G(prev, ref) (xor: next ^= G(prev, ref), version 1.3 in passes after the first) */
static void fill_block(const struct block* prev, const struct block* ref, struct block* next, bool xor_next) {
    struct block r, tmp;
    for (unsigned i = 0; i < 128; i++) r.v[i] = prev->v[i] ^ ref->v[i];
    tmp = r;
    if (xor_next) for (unsigned i = 0; i < 128; i++) tmp.v[i] ^= next->v[i];
    for (unsigned i = 0; i < 8; i++) {
        uint64_t* q = r.v + 16 * i;
        ROUND(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
    }
    for (unsigned i = 0; i < 8; i++) {
        uint64_t* q = r.v + 2 * i;
        ROUND(q[0], q[1], q[16], q[17], q[32], q[33], q[48], q[49], q[64], q[65], q[80], q[81], q[96], q[97], q[112], q[113]);
    }
    for (unsigned i = 0; i < 128; i++) next->v[i] = tmp.v[i] ^ r.v[i];
    crypto_wipe(&r, sizeof(r)); crypto_wipe(&tmp, sizeof(tmp));
}
struct instance { struct memory memory; uint32_t passes, lanes, lane_length, segment_length, memory_blocks; int type; };
static uint32_t index_alpha(const struct instance* in, uint32_t pass, uint32_t slice, uint32_t index, uint32_t pseudo, bool same_lane) {
    uint32_t area;
    if (!pass) {
        if (!slice) area = index - 1;
        else area = same_lane ? slice * in->segment_length + index - 1 : slice * in->segment_length - (index == 0 ? 1 : 0);
    } else area = same_lane ? in->lane_length - in->segment_length + index - 1 : in->lane_length - in->segment_length - (index == 0 ? 1 : 0);
    uint64_t relative = pseudo; relative = relative * relative >> 32;
    relative = area - 1 - ((uint64_t)area * relative >> 32);
    uint32_t start = 0;
    if (pass) start = slice == 3 ? 0 : (slice + 1) * in->segment_length;
    return (uint32_t)((start + relative) % in->lane_length);
}
static void next_addresses(struct block* address, struct block* input, const struct block* zero) {
    input->v[6]++;
    fill_block(zero, input, address, false);
    fill_block(zero, address, address, false);
}
static void fill_segment(struct instance* in, uint32_t pass, uint32_t lane, uint32_t slice) {
    bool independent = in->type == 1 || (in->type == 2 && pass == 0 && slice < 2);
    struct block address, input, zero;
    memset(&zero, 0, sizeof(zero)); memset(&input, 0, sizeof(input)); memset(&address, 0, sizeof(address));
    if (independent) {
        input.v[0] = pass; input.v[1] = lane; input.v[2] = slice; input.v[3] = in->memory_blocks; input.v[4] = in->passes; input.v[5] = (uint64_t)in->type;
    }
    uint32_t start = 0;
    if (!pass && !slice) { start = 2; if (independent) next_addresses(&address, &input, &zero); }
    uint64_t current = (uint64_t)lane * in->lane_length + slice * in->segment_length + start, previous;
    previous = current % in->lane_length == 0 ? current + in->lane_length - 1 : current - 1;
    for (uint32_t i = start; i < in->segment_length; i++, current++, previous++) {
        if (current % in->lane_length == 1) previous = current - 1;
        uint64_t pseudo;
        if (independent) { if (i % 128 == 0) next_addresses(&address, &input, &zero); pseudo = address.v[i % 128]; }
        else pseudo = at(&in->memory, previous)->v[0];
        uint32_t ref_lane = (uint32_t)((pseudo >> 32) % in->lanes);
        if (!pass && !slice) ref_lane = lane;
        uint32_t ref_index = index_alpha(in, pass, slice, i, (uint32_t)pseudo, ref_lane == lane);
        fill_block(at(&in->memory, previous), at(&in->memory, (uint64_t)ref_lane * in->lane_length + ref_index), at(&in->memory, current), pass != 0);
    }
    crypto_wipe(&address, sizeof(address)); crypto_wipe(&input, sizeof(input));
}
static int argon2_full(int type, const uint8_t* password, unsigned password_bytes, const uint8_t* salt, unsigned salt_bytes,
                       const uint8_t* secret, unsigned secret_bytes, const uint8_t* ad, unsigned ad_bytes,
                       uint32_t time_cost, uint32_t memory_kib, uint32_t lanes, uint8_t* out, unsigned out_bytes) {
    if (type < 0 || type > 2 || !time_cost || !lanes || lanes > 0xFFFFFF || out_bytes < 4 || salt_bytes < 8 || memory_kib < 8 * lanes) return -22;
    struct instance in; memset(&in, 0, sizeof(in));
    in.type = type; in.passes = time_cost; in.lanes = lanes;
    in.memory_blocks = memory_kib - memory_kib % (4 * lanes);
    in.segment_length = in.memory_blocks / (4 * lanes); in.lane_length = in.segment_length * 4;
    if (!memory_alloc(&in.memory, in.memory_blocks)) { memory_free(&in.memory); return -12; }
    uint8_t h0[72], word[4];   /* H0 || LE32(column) || LE32(lane) */
    struct blake2b s; b2_init(&s, 64, NULL, 0);
    uint32_t params[6] = {lanes, out_bytes, memory_kib, time_cost, 0x13, (uint32_t)type};
    for (unsigned i = 0; i < 6; i++) { store32(word, params[i]); b2_update(&s, word, 4); }
    store32(word, password_bytes); b2_update(&s, word, 4); b2_update(&s, password, password_bytes);
    store32(word, salt_bytes); b2_update(&s, word, 4); b2_update(&s, salt, salt_bytes);
    store32(word, secret_bytes); b2_update(&s, word, 4); if (secret_bytes) b2_update(&s, secret, secret_bytes);
    store32(word, ad_bytes); b2_update(&s, word, 4); if (ad_bytes) b2_update(&s, ad, ad_bytes);
    b2_final(&s, h0);
    uint8_t bytes[1024];
    for (uint32_t l = 0; l < lanes; l++) for (uint32_t c = 0; c < 2; c++) {
        store32(h0 + 64, c); store32(h0 + 68, l); hprime(bytes, 1024, h0, 72);
        struct block* b = at(&in.memory, (uint64_t)l * in.lane_length + c);
        for (unsigned i = 0; i < 128; i++) b->v[i] = load64(bytes + 8 * i);
    }
    for (uint32_t pass = 0; pass < time_cost; pass++)
        for (uint32_t slice = 0; slice < 4; slice++)
            for (uint32_t lane = 0; lane < lanes; lane++) fill_segment(&in, pass, lane, slice);
    struct block final = *at(&in.memory, in.lane_length - 1);
    for (uint32_t l = 1; l < lanes; l++) { const struct block* b = at(&in.memory, (uint64_t)l * in.lane_length + in.lane_length - 1); for (unsigned i = 0; i < 128; i++) final.v[i] ^= b->v[i]; }
    for (unsigned i = 0; i < 128; i++) store64(bytes + 8 * i, final.v[i]);
    hprime(out, out_bytes, bytes, 1024);
    crypto_wipe(bytes, sizeof(bytes)); crypto_wipe(h0, sizeof(h0)); crypto_wipe(&final, sizeof(final));
    memory_free(&in.memory);
    return 0;
}
int argon2_hash(int type, const uint8_t* password, unsigned password_bytes, const uint8_t* salt, unsigned salt_bytes,
                uint32_t time_cost, uint32_t memory_kib, uint32_t lanes, uint8_t* out, unsigned out_bytes) {
    return argon2_full(type, password, password_bytes, salt, salt_bytes, NULL, 0, NULL, 0, time_cost, memory_kib, lanes, out, out_bytes);
}

static bool same_hex(const uint8_t* data, const char* hex) {
    for (unsigned i = 0; hex[2 * i]; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) { char c = hex[2 * i + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10); }
        if (data[i] != v) return false;
    }
    return true;
}
bool argon2_selftest(void) {   /* RFC 7693 appendix A, RFC 9106 section 5 */
    uint8_t out[64]; bool ok = true;
    blake2b("abc", 3, NULL, 0, out, 64);
    ok &= same_hex(out, "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
    uint8_t password[32], salt[16], secret[8], ad[12];
    memset(password, 1, 32); memset(salt, 2, 16); memset(secret, 3, 8); memset(ad, 4, 12);
    static const char* const tags[3] = {"512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb",
                                        "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8",
                                        "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659"};
    for (int type = 0; type < 3; type++) {
        ok &= argon2_full(type, password, 32, salt, 16, secret, 8, ad, 12, 3, 32, 4, out, 32) == 0 && same_hex(out, tags[type]);
    }
    return ok;
}
