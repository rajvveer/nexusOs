/* ============================================================================
 * NexusOS — JPEG (baseline) Decoder (Implementation) — Phase 39
 * ============================================================================
 * Baseline sequential DCT JPEG decoder: DQT/DHT/SOF0/DRI/SOS parsing, canonical
 * Huffman decode, dequantization, an integer AAN-style IDCT, chroma upsampling,
 * and YCbCr->RGB. Structured after Martin Fiedler's public-domain NanoJPEG.
 * Handles grayscale and 3-component colour with arbitrary sampling factors.
 * ============================================================================ */

#include "image.h"
#include "string.h"
#include "heap.h"

#define JMAXCOMP 3

typedef struct {
    int id, ssx, ssy, qtsel, dctab, actab, dcpred;
    int width, height, stride;
    uint8_t* pixels;
} jcomp_t;

typedef struct {
    const uint8_t* pos;
    const uint8_t* end;
    int width, height, ncomp;
    int mbw, mbh, mbsx, mbsy;
    int rstinterval;
    jcomp_t comp[JMAXCOMP];
    int qtab[4][64];
    uint8_t hdc_cnt[4][17], hdc_sym[4][256];
    uint8_t hac_cnt[4][17], hac_sym[4][256];
    int buf, bufbits;
    int marker;
    int error;
} jpeg_t;

static const uint8_t ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

static uint8_t njclip(int x) { return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x)); }

/* -------------------- IDCT (integer, NanoJPEG) -------------------- */
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void row_idct(int* blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[4] << 11) | (x2 = blk[6]) | (x3 = blk[2]) |
          (x4 = blk[1]) | (x5 = blk[7]) | (x6 = blk[5]) | (x7 = blk[3]))) {
        blk[0] = blk[1] = blk[2] = blk[3] = blk[4] = blk[5] = blk[6] = blk[7] = blk[0] << 3;
        return;
    }
    x0 = (blk[0] << 11) + 128;
    x8 = W7 * (x4 + x5);  x4 = x8 + (W1 - W7) * x4;  x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);  x6 = x8 - (W3 - W5) * x6;  x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1;  x0 -= x1;
    x1 = W6 * (x3 + x2);  x2 = x1 - (W2 + W6) * x2;  x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6;  x4 -= x6;  x6 = x5 + x7;  x5 -= x7;
    x7 = x8 + x3;  x8 -= x3;  x3 = x0 + x2;  x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;  x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[0] = (x7 + x1) >> 8;  blk[1] = (x3 + x2) >> 8;
    blk[2] = (x0 + x4) >> 8;  blk[3] = (x8 + x6) >> 8;
    blk[4] = (x8 - x6) >> 8;  blk[5] = (x0 - x4) >> 8;
    blk[6] = (x3 - x2) >> 8;  blk[7] = (x7 - x1) >> 8;
}

static void col_idct(const int* blk, uint8_t* out, int stride) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[8 * 4] << 8) | (x2 = blk[8 * 6]) | (x3 = blk[8 * 2]) |
          (x4 = blk[8 * 1]) | (x5 = blk[8 * 7]) | (x6 = blk[8 * 5]) | (x7 = blk[8 * 3]))) {
        x1 = njclip(((blk[0] + 32) >> 6) + 128);
        for (x0 = 0; x0 < 8; x0++) out[x0 * stride] = (uint8_t)x1;
        return;
    }
    x0 = (blk[0] << 8) + 8192;
    x8 = W7 * (x4 + x5) + 4;  x4 = (x8 + (W1 - W7) * x4) >> 3;  x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;  x6 = (x8 - (W3 - W5) * x6) >> 3;  x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1;  x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;  x2 = (x1 - (W2 + W6) * x2) >> 3;  x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6;  x4 -= x6;  x6 = x5 + x7;  x5 -= x7;
    x7 = x8 + x3;  x8 -= x3;  x3 = x0 + x2;  x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;  x4 = (181 * (x4 - x5) + 128) >> 8;
    out[0]          = njclip(((x7 + x1) >> 14) + 128);
    out[stride]     = njclip(((x3 + x2) >> 14) + 128);
    out[2 * stride] = njclip(((x0 + x4) >> 14) + 128);
    out[3 * stride] = njclip(((x8 + x6) >> 14) + 128);
    out[4 * stride] = njclip(((x8 - x6) >> 14) + 128);
    out[5 * stride] = njclip(((x0 - x4) >> 14) + 128);
    out[6 * stride] = njclip(((x3 - x2) >> 14) + 128);
    out[7 * stride] = njclip(((x7 - x1) >> 14) + 128);
}

/* -------------------- bit reader (entropy stream) -------------------- */
static uint8_t j_nextbyte(jpeg_t* j) {
    if (j->marker) return 0xFF;             /* after a marker, feed 1-bits */
    if (j->pos >= j->end) { j->marker = 0xD9; return 0xFF; }
    uint8_t b = *j->pos++;
    if (b == 0xFF) {
        uint8_t b2 = (j->pos < j->end) ? *j->pos++ : 0xD9;
        if (b2 == 0) return 0xFF;           /* stuffed 0xFF00 */
        j->marker = b2;                      /* real marker reached */
        return 0xFF;
    }
    return b;
}

static int j_getbits(jpeg_t* j, int n) {
    if (n == 0) return 0;
    while (j->bufbits < n) {
        j->buf = (j->buf << 8) | j_nextbyte(j);
        j->bufbits += 8;
    }
    j->bufbits -= n;
    return (j->buf >> j->bufbits) & ((1 << n) - 1);
}

static int j_huff(jpeg_t* j, const uint8_t* cnt, const uint8_t* sym) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 16; len++) {
        code |= j_getbits(j, 1);
        int c = cnt[len];
        if (code - first < c) return sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}

static int j_extend(jpeg_t* j, int s) {
    if (s == 0) return 0;
    int v = j_getbits(j, s);
    if (v < (1 << (s - 1))) v += 1 - (1 << s);
    return v;
}

/* -------------------- block decode -------------------- */
static void j_decode_block(jpeg_t* j, jcomp_t* c, uint8_t* out) {
    int blk[64];
    for (int i = 0; i < 64; i++) blk[i] = 0;

    int qsel = c->qtsel & 3, dt = c->dctab & 3, at = c->actab & 3;
    int t = j_huff(j, j->hdc_cnt[dt], j->hdc_sym[dt]);
    if (t < 0) { j->error = 1; return; }
    c->dcpred += j_extend(j, t);
    blk[0] = c->dcpred * j->qtab[qsel][0];

    int k = 1;
    while (k < 64) {
        int rs = j_huff(j, j->hac_cnt[at], j->hac_sym[at]);
        if (rs < 0) { j->error = 1; return; }
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15) break;             /* EOB */
            k += 16;
        } else {
            k += r;
            if (k > 63) break;
            blk[(int)ZZ[k]] = j_extend(j, s) * j->qtab[qsel][k];
            k++;
        }
    }

    for (int i = 0; i < 8; i++) row_idct(&blk[i * 8]);
    for (int i = 0; i < 8; i++) col_idct(&blk[i], &out[i], c->stride);
}

/* -------------------- segment parsers -------------------- */
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void j_dqt(jpeg_t* j, const uint8_t* p, const uint8_t* e) {
    while (p < e) {
        int pq = *p >> 4, tq = *p & 15; p++;
        if (tq > 3) { j->error = 1; return; }
        for (int i = 0; i < 64; i++) {
            if (pq) { j->qtab[tq][i] = be16(p); p += 2; }
            else    { j->qtab[tq][i] = *p++; }
        }
    }
}

static void j_dht(jpeg_t* j, const uint8_t* p, const uint8_t* e) {
    while (p < e) {
        int tc = *p >> 4, th = *p & 15; p++;
        if (th > 3) { j->error = 1; return; }
        uint8_t* cnt = tc ? j->hac_cnt[th] : j->hdc_cnt[th];
        uint8_t* sym = tc ? j->hac_sym[th] : j->hdc_sym[th];
        int total = 0;
        cnt[0] = 0;
        for (int i = 1; i <= 16; i++) { cnt[i] = p[i - 1]; total += cnt[i]; }
        p += 16;
        for (int i = 0; i < total && i < 256; i++) sym[i] = p[i];
        p += total;
    }
}

static void j_sof(jpeg_t* j, const uint8_t* p, const uint8_t* e) {
    if (p + 6 > e) { j->error = 1; return; }
    p++;                                    /* precision (8) */
    j->height = be16(p); p += 2;
    j->width  = be16(p); p += 2;
    j->ncomp  = *p++;
    if (j->ncomp != 1 && j->ncomp != 3) { j->error = 1; return; }

    int hmax = 1, vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        if (p + 3 > e) { j->error = 1; return; }
        j->comp[i].id = *p++;
        int s = *p++;
        j->comp[i].ssx = s >> 4;
        j->comp[i].ssy = s & 15;
        j->comp[i].qtsel = *p++;
        if (j->comp[i].ssx > hmax) hmax = j->comp[i].ssx;
        if (j->comp[i].ssy > vmax) vmax = j->comp[i].ssy;
    }
    j->mbsx = hmax * 8; j->mbsy = vmax * 8;
    j->mbw = (j->width + j->mbsx - 1) / j->mbsx;
    j->mbh = (j->height + j->mbsy - 1) / j->mbsy;

    for (int i = 0; i < j->ncomp; i++) {
        jcomp_t* c = &j->comp[i];
        c->width  = j->mbw * c->ssx * 8;
        c->height = j->mbh * c->ssy * 8;
        c->stride = c->width;
        c->pixels = (uint8_t*)kmalloc((uint32_t)c->width * c->height);
        if (!c->pixels) { j->error = 1; return; }
    }
    /* stash hmax/vmax for upsampling via mbsx/mbsy already (=hmax*8). */
}

static void j_sos(jpeg_t* j, const uint8_t* p, const uint8_t* e) {
    int ns = *p++;
    for (int i = 0; i < ns; i++) {
        if (p + 2 > e) { j->error = 1; return; }
        int id = *p++, tt = *p++;
        for (int k = 0; k < j->ncomp; k++) {
            if (j->comp[k].id == id) {
                j->comp[k].dctab = tt >> 4;
                j->comp[k].actab = tt & 15;
            }
        }
    }
    /* skip Ss, Se, Ah/Al */
}

/* -------------------- scan decode -------------------- */
static void j_decode_scan(jpeg_t* j) {
    int hmax = j->mbsx / 8, vmax = j->mbsy / 8;
    int restart = j->rstinterval;
    int todo = restart ? restart : 0x7FFFFFFF;

    for (int my = 0; my < j->mbh && !j->error; my++) {
        for (int mx = 0; mx < j->mbw && !j->error; mx++) {
            for (int ci = 0; ci < j->ncomp; ci++) {
                jcomp_t* c = &j->comp[ci];
                for (int by = 0; by < c->ssy; by++) {
                    for (int bx = 0; bx < c->ssx; bx++) {
                        int px = (mx * c->ssx + bx) * 8;
                        int py = (my * c->ssy + by) * 8;
                        j_decode_block(j, c, c->pixels + py * c->stride + px);
                        if (j->error) return;
                    }
                }
            }
            if (restart && --todo == 0 && !(my == j->mbh - 1 && mx == j->mbw - 1)) {
                /* Restart: byte-align, consume RSTn, reset predictors. */
                j->bufbits = 0;
                if (j->marker >= 0xD0 && j->marker <= 0xD7) j->marker = 0;
                for (int k = 0; k < j->ncomp; k++) j->comp[k].dcpred = 0;
                todo = restart;
            }
        }
    }
    (void)hmax; (void)vmax;
}

/* -------------------- top level -------------------- */
bool jpeg_decode(const uint8_t* data, uint32_t size, image_t* img) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;

    jpeg_t* j = (jpeg_t*)kmalloc(sizeof(jpeg_t));
    if (!j) return false;
    memset(j, 0, sizeof(*j));
    j->end = data + size;

    const uint8_t* p = data + 2;
    const uint8_t* end = data + size;
    bool have_scan = false;

    while (p + 2 <= end && !j->error) {
        if (p[0] != 0xFF) { p++; continue; }
        uint8_t m = p[1]; p += 2;
        if (m == 0xD9) break;                       /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   /* standalone */
        if (p + 2 > end) break;
        int seglen = be16(p);
        const uint8_t* seg = p + 2;
        const uint8_t* sege = p + seglen;
        if (sege > end) break;

        if (m == 0xDB) j_dqt(j, seg, sege);
        else if (m == 0xC4) j_dht(j, seg, sege);
        else if (m == 0xC0) j_sof(j, seg, sege);     /* baseline */
        else if (m == 0xC1) j_sof(j, seg, sege);     /* extended seq (treat as baseline) */
        else if (m == 0xDD) j->rstinterval = be16(seg);
        else if (m == 0xC2) { j->error = 1; }        /* progressive: unsupported */
        else if (m == 0xDA) {                          /* SOS */
            j_sos(j, seg, sege);
            j->pos = sege; j->marker = 0; j->buf = 0; j->bufbits = 0;
            have_scan = true;
            break;
        }
        p = sege;
    }

    bool ok = false;
    if (have_scan && !j->error && image_dim_ok(j->width, j->height) && j->comp[0].pixels) {
        j_decode_scan(j);

        if (!j->error) {
            uint32_t* px = (uint32_t*)kmalloc((uint32_t)j->width * j->height * 4);
            if (px) {
                int hmax = j->mbsx / 8, vmax = j->mbsy / 8;
                for (int y = 0; y < j->height; y++) {
                    for (int x = 0; x < j->width; x++) {
                        jcomp_t* cy = &j->comp[0];
                        int Y = cy->pixels[(y * cy->ssy / vmax) * cy->stride + (x * cy->ssx / hmax)];
                        int r, g, b;
                        if (j->ncomp == 1) {
                            r = g = b = Y;
                        } else {
                            jcomp_t* cb = &j->comp[1];
                            jcomp_t* cr = &j->comp[2];
                            int Cb = cb->pixels[(y * cb->ssy / vmax) * cb->stride + (x * cb->ssx / hmax)] - 128;
                            int Cr = cr->pixels[(y * cr->ssy / vmax) * cr->stride + (x * cr->ssx / hmax)] - 128;
                            r = Y + ((91881 * Cr) >> 16);
                            g = Y - ((22554 * Cb + 46802 * Cr) >> 16);
                            b = Y + ((116130 * Cb) >> 16);
                        }
                        px[y * j->width + x] =
                            ((uint32_t)njclip(r) << 16) | ((uint32_t)njclip(g) << 8) | njclip(b);
                    }
                }
                img->pixels = px; img->width = j->width; img->height = j->height;
                img->frames = 1; img->format = "JPEG";
                ok = true;
            }
        }
    }

    for (int i = 0; i < JMAXCOMP; i++) if (j->comp[i].pixels) kfree(j->comp[i].pixels);
    kfree(j);
    return ok;
}
