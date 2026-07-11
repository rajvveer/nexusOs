/* ============================================================================
 * NexusOS — PNG Decoder (Implementation) — Phase 39
 * ============================================================================
 * Decodes non-interlaced 8-bit PNGs (grayscale / RGB / palette / gray+alpha /
 * RGBA). Includes a self-contained DEFLATE/zlib inflater (stored + fixed +
 * dynamic Huffman, structured after Mark Adler's public-domain puff.c) and the
 * five PNG scanline filters (none/sub/up/avg/paeth).
 * ============================================================================ */

#include "image.h"
#include "string.h"
#include "heap.h"

/* ==========================================================================
 * DEFLATE inflate (puff-style)
 * ========================================================================== */
typedef struct {
    const uint8_t* in;
    uint32_t incnt, inlen;
    int      bitbuf, bitcnt;
    uint8_t* out;
    uint32_t outcnt, outlen;
    int      err;
} inflate_t;

static int infl_bits(inflate_t* s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->err = 1; return 0; }
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

/* Canonical Huffman tables (count per length + sorted symbols). */
#define MAXBITS 15
static int huff_construct(short* count, short* symbol, const uint8_t* length, int n) {
    int len, left;
    short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) count[len] = 0;
    for (int s = 0; s < n; s++) count[length[s]]++;
    if (count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) { left <<= 1; left -= count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + count[len];
    for (int s = 0; s < n; s++) if (length[s] != 0) symbol[offs[length[s]]++] = (short)s;
    return left;
}

static int huff_decode(inflate_t* s, const short* count, const short* symbol) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= infl_bits(s, 1);
        if (s->err) return -1;
        int cnt = count[len];
        if (code - cnt < first) return symbol[index + (code - first)];
        index += cnt; first += cnt; first <<= 1; code <<= 1;
    }
    return -1;
}

static const short L_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short L_EXT [29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short D_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short D_EXT [30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int infl_codes(inflate_t* s, const short* lc, const short* ls,
                                    const short* dc, const short* ds) {
    for (;;) {
        int sym = huff_decode(s, lc, ls);
        if (s->err || sym < 0) return -1;
        if (sym < 256) {
            if (s->outcnt < s->outlen) s->out[s->outcnt] = (uint8_t)sym;
            s->outcnt++;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257; if (sym >= 29) return -1;
            int len = L_BASE[sym] + infl_bits(s, L_EXT[sym]);
            int dsym = huff_decode(s, dc, ds);
            if (s->err || dsym < 0 || dsym >= 30) return -1;
            int dist = D_BASE[dsym] + infl_bits(s, D_EXT[dsym]);
            for (int i = 0; i < len; i++) {
                if (s->outcnt < s->outlen)
                    s->out[s->outcnt] = ((uint32_t)dist <= s->outcnt) ? s->out[s->outcnt - dist] : 0;
                s->outcnt++;
            }
        }
        if (s->err) return -1;
    }
}

/* Shared Huffman storage (single-threaded decode). */
static short g_lcount[MAXBITS + 1], g_lsym[288];
static short g_dcount[MAXBITS + 1], g_dsym[30];
static uint8_t g_lengths[288 + 30];

static int infl_stored(inflate_t* s) {
    s->bitbuf = 0; s->bitcnt = 0;             /* discard to byte boundary */
    if (s->incnt + 4 > s->inlen) return -1;
    int len = s->in[s->incnt] | (s->in[s->incnt + 1] << 8);
    s->incnt += 4;                             /* skip LEN + NLEN */
    for (int i = 0; i < len; i++) {
        if (s->incnt >= s->inlen) return -1;
        if (s->outcnt < s->outlen) s->out[s->outcnt] = s->in[s->incnt];
        s->outcnt++; s->incnt++;
    }
    return 0;
}

static int infl_fixed(inflate_t* s) {
    for (int i = 0;   i < 144; i++) g_lengths[i] = 8;
    for (int i = 144; i < 256; i++) g_lengths[i] = 9;
    for (int i = 256; i < 280; i++) g_lengths[i] = 7;
    for (int i = 280; i < 288; i++) g_lengths[i] = 8;
    huff_construct(g_lcount, g_lsym, g_lengths, 288);
    for (int i = 0; i < 30; i++) g_lengths[i] = 5;
    huff_construct(g_dcount, g_dsym, g_lengths, 30);
    return infl_codes(s, g_lcount, g_lsym, g_dcount, g_dsym);
}

static int infl_dynamic(inflate_t* s) {
    static const short ORDER[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int nlen  = infl_bits(s, 5) + 257;
    int ndist = infl_bits(s, 5) + 1;
    int ncode = infl_bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;

    uint8_t clen[19];
    for (int i = 0; i < 19; i++) clen[i] = 0;
    for (int i = 0; i < ncode; i++) clen[ORDER[i]] = (uint8_t)infl_bits(s, 3);
    short ccount[MAXBITS + 1], csym[19];
    if (huff_construct(ccount, csym, clen, 19) != 0) return -1;

    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = huff_decode(s, ccount, csym);
        if (s->err || sym < 0) return -1;
        if (sym < 16) {
            g_lengths[idx++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (idx == 0) return -1;
            int rep = 3 + infl_bits(s, 2);
            uint8_t prev = g_lengths[idx - 1];
            while (rep-- && idx < nlen + ndist) g_lengths[idx++] = prev;
        } else if (sym == 17) {
            int rep = 3 + infl_bits(s, 3);
            while (rep-- && idx < nlen + ndist) g_lengths[idx++] = 0;
        } else { /* 18 */
            int rep = 11 + infl_bits(s, 7);
            while (rep-- && idx < nlen + ndist) g_lengths[idx++] = 0;
        }
    }
    if (huff_construct(g_lcount, g_lsym, g_lengths, nlen) < 0) return -1;
    if (huff_construct(g_dcount, g_dsym, g_lengths + nlen, ndist) < 0) return -1;
    return infl_codes(s, g_lcount, g_lsym, g_dcount, g_dsym);
}

/* Inflate a raw DEFLATE stream into out[0..outlen). Returns bytes produced. */
static uint32_t inflate(const uint8_t* in, uint32_t inlen, uint8_t* out, uint32_t outlen) {
    inflate_t s;
    s.in = in; s.incnt = 0; s.inlen = inlen;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.outcnt = 0; s.outlen = outlen; s.err = 0;

    int last, type, rc = 0;
    do {
        last = infl_bits(&s, 1);
        type = infl_bits(&s, 2);
        if (s.err) break;
        if      (type == 0) rc = infl_stored(&s);
        else if (type == 1) rc = infl_fixed(&s);
        else if (type == 2) rc = infl_dynamic(&s);
        else { rc = -1; }
        if (rc != 0 || s.err) break;
    } while (!last);

    return s.outcnt;
}

/* ==========================================================================
 * PNG container + filters
 * ========================================================================== */
static uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int ipaeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

bool png_decode(const uint8_t* d, uint32_t size, image_t* img) {
    static const uint8_t SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (size < 8 + 12 + 13) return false;
    for (int i = 0; i < 8; i++) if (d[i] != SIG[i]) return false;

    int w = 0, h = 0, bitdepth = 0, colortype = 0, interlace = 0;
    uint8_t palette[256 * 3]; int pal_n = 0;

    /* IDAT may be split across chunks; concatenate into one buffer. */
    uint8_t* idat = (uint8_t*)kmalloc(size);
    if (!idat) return false;
    uint32_t idat_len = 0;

    uint32_t pos = 8;
    bool have_ihdr = false;
    while (pos + 8 <= size) {
        uint32_t clen = rd32be(d + pos);
        const uint8_t* type = d + pos + 4;
        uint32_t body = pos + 8;
        if (body + clen + 4 > size) break;

        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R' && clen >= 13) {
            w = (int)rd32be(d + body);
            h = (int)rd32be(d + body + 4);
            bitdepth  = d[body + 8];
            colortype = d[body + 9];
            interlace = d[body + 12];
            have_ihdr = true;
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            pal_n = (int)(clen / 3); if (pal_n > 256) pal_n = 256;
            memcpy(palette, d + body, (uint32_t)pal_n * 3);
        } else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
            if (idat_len + clen <= size) { memcpy(idat + idat_len, d + body, clen); idat_len += clen; }
        } else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
            break;
        }
        pos = body + clen + 4;   /* skip CRC */
    }

    if (!have_ihdr || !image_dim_ok(w, h) || bitdepth != 8 || interlace != 0 || idat_len < 3) {
        kfree(idat); return false;
    }

    int channels;
    switch (colortype) {
        case 0: channels = 1; break;   /* gray              */
        case 2: channels = 3; break;   /* RGB               */
        case 3: channels = 1; break;   /* palette index     */
        case 4: channels = 2; break;   /* gray + alpha      */
        case 6: channels = 4; break;   /* RGBA              */
        default: kfree(idat); return false;
    }
    if (colortype == 3 && pal_n == 0) { kfree(idat); return false; }

    uint32_t stride = 1 + (uint32_t)w * channels;
    uint32_t rawlen = stride * h;
    uint8_t* raw = (uint8_t*)kmalloc(rawlen);
    if (!raw) { kfree(idat); return false; }

    /* zlib wrapper: skip 2-byte header, inflate the DEFLATE body. */
    uint32_t produced = inflate(idat + 2, idat_len - 2, raw, rawlen);
    kfree(idat);
    if (produced < rawlen) { kfree(raw); return false; }

    /* Unfilter scanlines in place (bpp = channels for 8-bit). */
    int bpp = channels;
    for (int y = 0; y < h; y++) {
        uint8_t f = raw[(uint32_t)y * stride];
        uint8_t* cur = raw + (uint32_t)y * stride + 1;
        uint8_t* prev = (y > 0) ? raw + (uint32_t)(y - 1) * stride + 1 : NULL;
        int rb = w * channels;
        for (int x = 0; x < rb; x++) {
            int a = (x >= bpp) ? cur[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = cur[x];
            switch (f) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: v += ipaeth(a, b, c); break;
                default: break;
            }
            cur[x] = (uint8_t)v;
        }
    }

    /* Convert to ARGB. */
    uint32_t* px = (uint32_t*)kmalloc((uint32_t)w * h * 4);
    if (!px) { kfree(raw); return false; }
    for (int y = 0; y < h; y++) {
        uint8_t* row = raw + (uint32_t)y * stride + 1;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            const uint8_t* s = row + (uint32_t)x * channels;
            switch (colortype) {
                case 0: r = g = b = s[0]; break;
                case 2: r = s[0]; g = s[1]; b = s[2]; break;
                case 3: { int i = s[0]; if (i >= pal_n) i = 0;
                          r = palette[i*3]; g = palette[i*3+1]; b = palette[i*3+2]; } break;
                case 4: r = g = b = s[0]; break;
                default: r = s[0]; g = s[1]; b = s[2]; break; /* 6: RGBA */
            }
            px[y * w + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    kfree(raw);

    img->pixels = px; img->width = w; img->height = h;
    img->frames = 1; img->format = "PNG";
    return true;
}
