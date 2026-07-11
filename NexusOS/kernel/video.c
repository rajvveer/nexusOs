/* ============================================================================
 * NexusOS — Video Subsystem (Implementation) — Phase 40
 * ============================================================================
 * RIFF/AVI container parser + media player. Walks hdrl (avih + per-stream
 * strh/strf) and movi (interleaved 'NNdc' video / 'NNwb' audio chunks), decodes
 * Motion-JPEG frames via the JPEG decoder, renders them with image_present(),
 * and plays the concatenated PCM stream through the AC'97 mixer. Video is paced
 * off the audio clock (one frame's worth of samples per frame) for A/V sync,
 * falling back to tick pacing when there is no audio.
 * ============================================================================ */

#include "video.h"
#include "image.h"
#include "audio.h"
#include "keyboard.h"
#include "string.h"
#include "heap.h"
#include "vga.h"
#include "sample_video.h"

extern volatile uint32_t system_ticks;
#define TICK_MS 55

#define MAX_VFRAMES 1024
#define MAX_ACHUNKS 2048

typedef struct {
    avi_info_t info;
    int        vstream, astream;
    int        nv, na;
    uint32_t   voff[MAX_VFRAMES], vlen[MAX_VFRAMES];
    uint32_t   aoff[MAX_ACHUNKS], alen[MAX_ACHUNKS];
} avi_t;

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool fourcc(const uint8_t* p, const char* s) {
    return p[0]==s[0] && p[1]==s[1] && p[2]==s[2] && p[3]==s[3];
}

/* --------------------------------------------------------------------------
 * Parse hdrl + movi into `a`.
 * -------------------------------------------------------------------------- */
static bool avi_load(const uint8_t* d, uint32_t size, avi_t* a) {
    memset(a, 0, sizeof(*a));
    a->vstream = -1; a->astream = -1;
    if (size < 12 || !fourcc(d, "RIFF") || !fourcc(d + 8, "AVI ")) return false;

    uint32_t movi_start = 0, movi_end = 0;
    int stream_idx = 0;
    uint32_t pos = 12;

    while (pos + 8 <= size) {
        const uint8_t* fcc = d + pos;
        uint32_t sz = rd32(d + pos + 4);
        uint32_t body = pos + 8;
        if (body + sz > size) break;

        if (fourcc(fcc, "LIST")) {
            const uint8_t* lt = d + body;
            if (fourcc(lt, "hdrl")) {
                uint32_t p = body + 4, e = body + sz;
                while (p + 8 <= e) {
                    const uint8_t* f = d + p;
                    uint32_t s = rd32(d + p + 4);
                    uint32_t b = p + 8;
                    if (b + s > size) break;
                    if (fourcc(f, "avih")) {
                        a->info.us_per_frame = rd32(d + b + 0);
                        a->info.frame_count  = (int)rd32(d + b + 16);
                        a->info.width        = (int)rd32(d + b + 32);
                        a->info.height       = (int)rd32(d + b + 36);
                    } else if (fourcc(f, "LIST") && fourcc(d + b, "strl")) {
                        uint32_t sp = b + 4, se = p + 8 + s;
                        while (sp + 8 <= se) {
                            const uint8_t* sf = d + sp;
                            uint32_t ss = rd32(d + sp + 4);
                            uint32_t sb = sp + 8;
                            if (sb + ss > size) break;
                            if (fourcc(sf, "strh")) {
                                if (fourcc(d + sb, "vids")) {
                                    a->vstream = stream_idx;
                                    for (int k = 0; k < 4; k++) a->info.vcodec[k] = (char)d[sb + 4 + k];
                                    a->info.vcodec[4] = '\0';
                                } else if (fourcc(d + sb, "auds")) {
                                    a->astream = stream_idx;
                                }
                            } else if (fourcc(sf, "strf")) {
                                if (a->astream == stream_idx && ss >= 16) {
                                    a->info.audio_channels = rd16(d + sb + 2);
                                    a->info.audio_rate     = (int)rd32(d + sb + 4);
                                    a->info.audio_bits     = rd16(d + sb + 14);
                                    a->info.has_audio      = true;
                                }
                            }
                            sp = sb + ss + (ss & 1);
                        }
                        stream_idx++;
                    }
                    p = b + s + (s & 1);
                }
            } else if (fourcc(lt, "movi")) {
                movi_start = body + 4;
                movi_end   = body + sz;
                if (movi_end > size) movi_end = size;
            }
        }
        pos = body + sz + (sz & 1);
    }

    /* Walk movi chunks: 'NNdc'/'NNdb' = video, 'NNwb' = audio. */
    if (movi_start) {
        uint32_t p = movi_start;
        while (p + 8 <= movi_end) {
            const uint8_t* id = d + p;
            uint32_t s = rd32(d + p + 4);
            uint32_t b = p + 8;
            if (b + s > size) break;
            int stream = (id[0] - '0') * 10 + (id[1] - '0');
            if (stream == a->vstream && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) {
                if (a->nv < MAX_VFRAMES) { a->voff[a->nv] = b; a->vlen[a->nv] = s; a->nv++; }
            } else if (stream == a->astream && id[2] == 'w' && id[3] == 'b') {
                if (a->na < MAX_ACHUNKS) { a->aoff[a->na] = b; a->alen[a->na] = s; a->na++; }
            }
            p = b + s + (s & 1);
        }
    }

    a->info.audio_chunks = a->na;
    if (a->info.frame_count <= 0) a->info.frame_count = a->nv;
    a->info.valid = (a->nv > 0);
    return a->info.valid;
}

/* --------------------------------------------------------------------------
 * Public: parse metadata only.
 * -------------------------------------------------------------------------- */
bool avi_parse(const uint8_t* data, uint32_t size, avi_info_t* out) {
    avi_t* a = (avi_t*)kmalloc(sizeof(avi_t));
    if (!a) return false;
    bool ok = avi_load(data, size, a);
    if (ok && out) *out = a->info;
    kfree(a);
    return ok;
}

/* --------------------------------------------------------------------------
 * Player
 * -------------------------------------------------------------------------- */
static void append(char* dst, const char* s) { while (*dst) dst++; while (*s) *dst++ = *s++; *dst = 0; }
static void append_int(char* dst, int v) { char b[12]; int_to_str(v, b); append(dst, b); }

void video_play(const uint8_t* d, uint32_t size, const char* title) {
    avi_t* a = (avi_t*)kmalloc(sizeof(avi_t));
    if (!a) { vga_print("  Out of memory.\n"); return; }
    if (!avi_load(d, size, a)) {
        vga_print_color("  Not a valid AVI file.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        kfree(a); return;
    }

    /* Concatenate the PCM audio into one contiguous buffer. */
    int16_t* abuf = NULL;
    uint32_t aframes = 0;
    int ch = a->info.audio_channels ? a->info.audio_channels : 1;
    int rate = a->info.audio_rate;
    if (a->info.has_audio && a->na > 0 && a->info.audio_bits == 16) {
        uint32_t bytes = 0;
        for (int i = 0; i < a->na; i++) bytes += a->alen[i];
        abuf = (int16_t*)kmalloc(bytes);
        if (abuf) {
            uint32_t o = 0;
            for (int i = 0; i < a->na; i++) {
                memcpy((uint8_t*)abuf + o, d + a->aoff[i], a->alen[i]);
                o += a->alen[i];
            }
            aframes = bytes / (uint32_t)(2 * ch);
        }
    }

    uint32_t frame_ms = a->info.us_per_frame / 1000;
    if (frame_ms == 0) frame_ms = 100;
    /* samples-per-frame without 32-bit overflow */
    uint32_t spf = (uint32_t)rate * (a->info.us_per_frame / 1000) / 1000;
    uint32_t fps = a->info.us_per_frame ? 1000000u / a->info.us_per_frame : 0;

    uint32_t apos = 0;
    for (int i = 0; i < a->nv; i++) {
        image_t img;
        if (jpeg_decode(d + a->voff[i], a->vlen[i], &img)) {
            char cap[96]; cap[0] = '\0';
            append(cap, title); append(cap, "  ");
            append_int(cap, img.width); append(cap, "x"); append_int(cap, img.height);
            append(cap, "  frame "); append_int(cap, i + 1);
            append(cap, "/"); append_int(cap, a->nv);
            append(cap, "  "); append_int(cap, (int)fps); append(cap, " fps");
            if (a->info.has_audio) { append(cap, "  +audio"); }
            image_present(&img, cap);
            image_free(&img);
        }

        if (keyboard_has_key()) { keyboard_getchar(); break; }

        if (abuf && spf > 0 && apos < aframes) {
            uint32_t n = spf;
            if (apos + n > aframes) n = aframes - apos;
            audio_play_pcm(abuf + apos * ch, n, (uint16_t)ch, (uint32_t)rate,
                           audio_get_master_volume());   /* blocks ~one frame */
            apos += n;
        } else {
            uint32_t ticks = frame_ms / TICK_MS; if (ticks == 0) ticks = 1;
            uint32_t st = system_ticks;
            while ((system_ticks - st) < ticks) {
                if (keyboard_has_key()) break;
                __asm__ volatile("hlt");
            }
        }
    }

    if (abuf) kfree(abuf);
    kfree(a);
}

const uint8_t* video_demo(uint32_t* size_out) {
    if (size_out) *size_out = DEMO_AVI_LEN;
    return demo_avi;
}

void video_init(void) {
    avi_info_t info;
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    if (avi_parse(demo_avi, DEMO_AVI_LEN, &info)) {
        vga_print("Video subsystem ready (AVI/MJPEG), demo ");
        char b[12]; int_to_str(info.width, b); vga_print(b);
        vga_print("x"); int_to_str(info.height, b); vga_print(b);
        vga_print(" "); int_to_str(info.frame_count, b); vga_print(b);
        vga_print(" frames\n");
    } else {
        vga_print("Video subsystem ready (AVI/MJPEG)\n");
    }
}
