/* ============================================================================
 * NexusOS — GIF Decoder (Implementation) — Phase 39
 * ============================================================================
 * GIF87a/89a decoder: logical screen + global/local colour tables, LZW
 * decompression (variable code width, clear/EOI), interlacing, transparency,
 * and multi-frame compositing with disposal. gif_decode() returns frame 0;
 * gif_decode_frame() composites up to frame N for animation playback.
 * ============================================================================ */

#include "image.h"
#include "string.h"
#include "heap.h"

/* LZW tables — static (BSS) to keep them off the small kernel stack. */
static uint16_t lzw_prefix[4096];
static uint8_t  lzw_suffix[4096];
static uint8_t  lzw_stack[4096];

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* --------------------------------------------------------------------------
 * LZW decode the gathered code stream into `out` (capacity `outcap` indices).
 * Returns the number of indices written.
 * -------------------------------------------------------------------------- */
static uint32_t lzw_decode(const uint8_t* in, uint32_t inlen, int min_code,
                           uint8_t* out, uint32_t outcap) {
    int clear = 1 << min_code;
    int eoi   = clear + 1;
    int code_size = min_code + 1;
    int next = eoi + 1;
    int oldcode = -1;
    int first = 0;

    uint32_t bitpos = 0, outpos = 0;
    int sp = 0;

    for (;;) {
        if (bitpos + code_size > inlen * 8) break;
        /* Read `code_size` bits, LSB first. */
        int code = 0;
        for (int i = 0; i < code_size; i++) {
            uint32_t byte = (bitpos + i) >> 3;
            code |= ((in[byte] >> ((bitpos + i) & 7)) & 1) << i;
        }
        bitpos += code_size;

        if (code == clear) {
            code_size = min_code + 1; next = eoi + 1; oldcode = -1; continue;
        }
        if (code == eoi) break;

        if (oldcode == -1) {                 /* first literal after a clear */
            if (code >= clear) break;
            if (outpos < outcap) out[outpos++] = (uint8_t)code;
            first = code; oldcode = code; continue;
        }

        int incode = code;
        sp = 0;
        if (code >= next) {                   /* KwKwK: string not yet in dict */
            lzw_stack[sp++] = (uint8_t)first;
            code = oldcode;
        }
        while (code >= clear) {               /* walk prefix chain (reversed) */
            if (sp >= 4096) break;
            lzw_stack[sp++] = lzw_suffix[code];
            code = lzw_prefix[code];
        }
        first = code & 0xFF;                  /* root literal byte */
        if (sp < 4096) lzw_stack[sp++] = (uint8_t)first;
        while (sp > 0 && outpos < outcap) out[outpos++] = lzw_stack[--sp];

        if (next < 4096) {                    /* add oldcode + first */
            lzw_prefix[next] = (uint16_t)oldcode;
            lzw_suffix[next] = (uint8_t)first;
            next++;
            if (next == (1 << code_size) && code_size < 12) code_size++;
        }
        oldcode = incode;
    }
    return outpos;
}

/* --------------------------------------------------------------------------
 * gif_render: parse the whole file, compositing up to `target`, returning the
 * canvas at that frame in img->pixels and the total frame count in img->frames.
 * -------------------------------------------------------------------------- */
static bool gif_render(const uint8_t* d, uint32_t size, int target, image_t* img) {
    if (size < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F') return false;

    int cw = rd16(d + 6), ch = rd16(d + 8);
    if (!image_dim_ok(cw, ch)) return false;
    uint8_t packed = d[10];
    uint8_t bg = d[11];

    uint32_t pos = 13;
    uint8_t gct[256 * 3];
    int gct_n = 0;
    if (packed & 0x80) {
        gct_n = 2 << (packed & 7);
        if (pos + (uint32_t)gct_n * 3 > size) return false;
        memcpy(gct, d + pos, (uint32_t)gct_n * 3);
        pos += (uint32_t)gct_n * 3;
    }

    uint32_t* canvas = (uint32_t*)kmalloc((uint32_t)cw * ch * 4);
    uint32_t* out    = (uint32_t*)kmalloc((uint32_t)cw * ch * 4);
    uint8_t*  lct    = (uint8_t*)kmalloc(256 * 3);
    uint8_t*  idx    = (uint8_t*)kmalloc((uint32_t)cw * ch);
    uint8_t*  lzwbuf = (uint8_t*)kmalloc(size);   /* >= total code bytes */
    if (!canvas || !out || !lct || !idx || !lzwbuf) {
        if (canvas) kfree(canvas);
        if (out) kfree(out);
        if (lct) kfree(lct);
        if (idx) kfree(idx);
        if (lzwbuf) kfree(lzwbuf);
        return false;
    }

    uint32_t bgcol = 0;
    if (gct_n && bg < gct_n)
        bgcol = ((uint32_t)gct[bg*3]<<16)|((uint32_t)gct[bg*3+1]<<8)|gct[bg*3+2];
    for (int i = 0; i < cw * ch; i++) canvas[i] = bgcol;

    int frames = 0;
    bool captured = false;
    int transp = -1, disposal = 0;

    while (pos < size) {
        uint8_t b = d[pos++];
        if (b == 0x3B) break;                          /* trailer */

        if (b == 0x21) {                               /* extension */
            if (pos >= size) break;
            uint8_t label = d[pos++];
            if (label == 0xF9 && pos < size) {         /* graphic control */
                uint8_t bs = d[pos];
                if (pos + 1 + bs <= size && bs >= 4) {
                    uint8_t p2 = d[pos + 1];
                    disposal = (p2 >> 2) & 7;
                    transp = (p2 & 1) ? d[pos + 4] : -1;
                }
                pos += 1 + bs;
            }
            while (pos < size) {                        /* skip sub-blocks */
                uint8_t sz = d[pos++];
                if (sz == 0) break;
                pos += sz;
            }
            continue;
        }

        if (b != 0x2C) break;                           /* unexpected */

        /* Image descriptor */
        if (pos + 9 > size) break;
        int lx = rd16(d + pos), ly = rd16(d + pos + 2);
        int lw = rd16(d + pos + 4), lh = rd16(d + pos + 6);
        uint8_t ip = d[pos + 8];
        pos += 9;

        const uint8_t* table = gct;
        int table_n = gct_n;
        if (ip & 0x80) {                                /* local colour table */
            int n = 2 << (ip & 7);
            if (pos + (uint32_t)n * 3 > size) break;
            memcpy(lct, d + pos, (uint32_t)n * 3);
            pos += (uint32_t)n * 3;
            table = lct; table_n = n;
        }
        bool interlace = (ip & 0x40) != 0;

        if (pos >= size) break;
        int min_code = d[pos++];

        /* Gather LZW sub-blocks. */
        uint32_t lzwlen = 0;
        while (pos < size) {
            uint8_t sz = d[pos++];
            if (sz == 0) break;
            if (pos + sz > size) { sz = (uint8_t)(size - pos); }
            memcpy(lzwbuf + lzwlen, d + pos, sz);
            lzwlen += sz; pos += sz;
        }

        if (lw > 0 && lh > 0 && lw <= cw && lh <= ch) {
            uint32_t want = (uint32_t)lw * lh;
            uint32_t got = lzw_decode(lzwbuf, lzwlen, min_code, idx, want);

            /* Build destination-row order (handles interlace). */
            for (uint32_t si = 0; si < got; si++) {
                int srow = (int)(si / lw);
                int scol = (int)(si % lw);
                int drow;
                if (!interlace) drow = srow;
                else {
                    /* map storage row -> image row across 4 passes */
                    int p1 = (lh + 7) / 8, p2 = (lh + 3) / 8;
                    int p3 = (lh + 1) / 4;
                    if (srow < p1) drow = srow * 8;
                    else if (srow < p1 + p2) drow = (srow - p1) * 8 + 4;
                    else if (srow < p1 + p2 + p3) drow = (srow - p1 - p2) * 4 + 2;
                    else drow = (srow - p1 - p2 - p3) * 2 + 1;
                }
                int cx = lx + scol, cy = ly + drow;
                if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) continue;
                int ci = idx[si];
                if (ci == transp) continue;             /* transparent */
                if (ci < table_n)
                    canvas[cy * cw + cx] =
                        ((uint32_t)table[ci*3]<<16)|((uint32_t)table[ci*3+1]<<8)|table[ci*3+2];
            }
        }

        if (frames == target) {
            memcpy(out, canvas, (uint32_t)cw * ch * 4);
            captured = true;
        }
        frames++;

        if (disposal == 2) {                            /* restore to background */
            for (int y = ly; y < ly + lh && y < ch; y++)
                for (int x = lx; x < lx + lw && x < cw; x++)
                    if (x >= 0 && y >= 0) canvas[y * cw + x] = bgcol;
        }
        transp = -1; disposal = 0;
    }

    if (!captured) memcpy(out, canvas, (uint32_t)cw * ch * 4);

    kfree(canvas); kfree(lct); kfree(idx); kfree(lzwbuf);
    img->pixels = out; img->width = cw; img->height = ch;
    img->frames = frames > 0 ? frames : 1; img->format = "GIF";
    return true;
}

bool gif_decode(const uint8_t* data, uint32_t size, image_t* img) {
    return gif_render(data, size, 0, img);
}

bool gif_decode_frame(const uint8_t* data, uint32_t size, image_t* img, int frame) {
    return gif_render(data, size, frame, img);
}
