/* Aurora text (aurora_text.c): UTF-8 text through FreeType, HarfBuzz and fontconfig, loaded at run time (dlopen).
 *
 * macui (kernel/macui.c) draws Latin UI text from its built-in 4-bit glyph tables at 100 %. It comes here for text at
 * any other UI scale (the shell on a 125-400 % display, Aurora apps rendering at their output's scale) and for every
 * character outside those tables: the UI face is Aileron (the same face the tables were rasterized from, embedded in
 * the binary), other characters fall back to the system fonts fontconfig finds for them (Noto: CJK, Arabic, Hebrew,
 * Indic scripts, Thai, emoji in colour...). HarfBuzz shapes each run (ligatures, Indic reordering, Arabic joining; an
 * RTL run comes out in visual order). Without the libraries everything degrades to the built-in tables.
 *
 * Sizes are physical pixels in 1/64; positions are physical. Roles are macui's UF_* fonts. */
#ifndef AURORA_TEXT_H
#define AURORA_TEXT_H
#include <stdbool.h>
#include <stdint.h>

bool aurora_text_ready(void);   /* FreeType and the UI face loaded (tried once) */
/* Draw (canvas != NULL) or measure UTF-8 text s[0..n) with role font at size64 (1/64 px), emboldened by bold64 (1/64 px),
 * baseline at physical (x, baseline) into canvas (cw x ch, stride pixels). Returns the advance in 1/64 px. */
int64_t aurora_text_run(const char* s, int n, int size64, int bold64, int x, int baseline, uint32_t color,
                        uint32_t* canvas, int cw, int ch, int stride);
/* true when s[0..n) has a byte outside printable ASCII (the built-in tables cannot show it). */
bool aurora_text_needs_shaping(const char* s, int n);
#endif
