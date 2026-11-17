#!/usr/bin/env python3
"""
gen_uifont.py — rasterize the NexusOS "Aurora" UI fonts into kernel/uifont.h.

Glyphs are rendered at 4x with FreeType (Pillow) and box-downsampled, which
gives true coverage anti-aliasing, then packed as 4-bit alpha (2 px per byte).

  * UI fonts:  Aileron Regular — the CC0 neo-grotesque that Pillow bundles as
               its default font (ImageFont.load_default). Semibold weights are
               synthesized with a sub-pixel stroke at 4x.
  * Cell font: Cascadia Mono (SIL Open Font License 1.1, Microsoft), fitted to
               the 8x16 character cell every NexusOS app draws with.

Usage:  python gen_uifont.py   (writes kernel/uifont.h next to this script)
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "kernel", "uifont.h")
SS = 4  # supersampling factor
ASCII = [chr(c) for c in range(32, 127)]

def aileron(px):
    return ImageFont.load_default(size=px * SS)

def find_cascadia():
    for p in (r"C:\Windows\Fonts\CascadiaMono.ttf", "/usr/share/fonts/truetype/cascadia/CascadiaMono.ttf"):
        if os.path.exists(p): return p
    sys.exit("CascadiaMono.ttf not found")

def render_glyph(font, ch, stroke):
    """Render one glyph at SSx; return (img_L_downsampled, xoff, yoff, adv_q4)."""
    asc, desc = font.getmetrics()
    pad = 8 * SS + stroke * 2
    W = int(font.getlength(ch)) + 2 * pad + SS * 4
    H = asc + desc + 2 * pad
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    # Origin chosen on a multiple of SS so the downsample grid aligns with pixels.
    ox, base = pad - pad % SS, pad - pad % SS + asc - (asc % SS)
    d.text((ox, base), ch, fill=255, font=font, anchor="ls",
           stroke_width=stroke, stroke_fill=255)
    adv_q4 = round((font.getlength(ch) + stroke * 1.5) / SS * 4)  # stroke widens the pen
    bb = im.getbbox()
    if not bb:
        return None, 0, 0, adv_q4
    x0 = (bb[0] // SS) * SS; y0 = (bb[1] // SS) * SS
    x1 = -(-bb[2] // SS) * SS; y1 = -(-bb[3] // SS) * SS
    crop = im.crop((x0, y0, x1, y1)).reduce(SS)
    return crop, (x0 - ox) // SS, (y0 - base) // SS, adv_q4

def pack4(img):
    w, h = img.size
    px = img.load()
    out = []
    for y in range(h):
        row = [min(15, (px[x, y] + 8) // 17) for x in range(w)]
        if w % 2: row.append(0)
        for i in range(0, len(row), 2):
            out.append(row[i] | (row[i + 1] << 4))
    return out

def build_prop(name, font, chars, stroke=0):
    glyphs, bits = [], []
    asc, desc = font.getmetrics()
    for ch in chars:
        img, xo, yo, adv = render_glyph(font, ch, stroke)
        if img is None:
            glyphs.append((0, 0, 0, 0, adv, len(bits)))
            continue
        glyphs.append((img.size[0], img.size[1], xo, yo, adv, len(bits)))
        bits += pack4(img)
    return dict(name=name, first=ord(chars[0]), count=len(chars),
                ascent=round(asc / SS), line=round((asc + desc) / SS) + 2,
                glyphs=glyphs, bits=bits)

def build_cell(font_path):
    """8x16 cells: Cascadia Mono sized so its advance is exactly 8 px."""
    probe = ImageFont.truetype(font_path, 100)
    size = 8.0 * 100 / probe.getlength("M")
    font = ImageFont.truetype(font_path, round(size * SS))
    cw, chh, base = 8 * SS, 16 * SS, 12 * SS
    bits = []
    for ch in ASCII:
        im = Image.new("L", (cw, chh), 0)
        ImageDraw.Draw(im).text((0, base), ch, fill=255, font=font, anchor="ls")
        bits += pack4(im.reduce(SS))
    return bits

def build_cursor():
    """macOS-style arrow: black fill, white rim, soft drop shadow. ARGB 20x26."""
    from PIL import ImageFilter
    W, H = 20, 26
    pts = [(1, 1), (1, 18.2), (5.2, 14.2), (8.2, 21.2), (11.0, 20.0), (8.1, 13.3), (13.6, 13.3)]
    big = [(x * SS, y * SS) for x, y in pts]
    def layer(fill, width):
        im = Image.new("L", (W * SS, H * SS), 0)
        d = ImageDraw.Draw(im)
        d.polygon(big, fill=fill)
        if width: d.line(big + [big[0]], fill=fill, width=width, joint="curve")
        return im
    rim = layer(255, int(2.6 * SS))           # white body incl. 1.3 px rim
    core = layer(255, 0)                      # black interior
    shadow = rim.filter(ImageFilter.GaussianBlur(1.6 * SS))
    out = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    sh = Image.new("RGBA", out.size, (0, 0, 0, 0)); sh.putalpha(shadow.point(lambda v: v * 95 // 255))
    out.alpha_composite(sh, (int(0.6 * SS), int(1.4 * SS)))
    white = Image.new("RGBA", out.size, (255, 255, 255, 0)); white.putalpha(rim)
    out.alpha_composite(white)
    black = Image.new("RGBA", out.size, (0, 0, 0, 0)); black.putalpha(core)
    out.alpha_composite(black)
    out = out.resize((W, H), Image.LANCZOS)
    px = out.load()
    return W, H, [(px[x, y][3] << 24) | (px[x, y][0] << 16) | (px[x, y][1] << 8) | px[x, y][2]
                  for y in range(H) for x in range(W)]

def c_array(name, data, per=24):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per):
        lines.append("    " + ",".join(str(b) for b in data[i:i + per]) + ",")
    lines.append("};")
    return "\n".join(lines)

def main():
    fonts = [
        build_prop("ui",      aileron(14), ASCII),
        build_prop("uib",     aileron(14), ASCII, stroke=1),
        build_prop("small",   aileron(12), ASCII),
        build_prop("title",   aileron(22), ASCII, stroke=2),
        build_prop("huge",    aileron(96), [chr(c) for c in range(ord('0'), ord(':') + 1)], stroke=9),
    ]
    cell = build_cell(find_cascadia())
    out = ["/* AUTO-GENERATED by gen_uifont.py - do not edit.",
           " * UI fonts: Aileron Regular (CC0 1.0, as bundled with Pillow).",
           " * Cell font: Cascadia Mono (c) Microsoft, SIL Open Font License 1.1.",
           " * Format: 4-bit alpha, 2 px/byte (low nibble = left pixel), rows padded",
           " * to whole bytes. Advances are in quarter pixels. */",
           "#ifndef UIFONT_H", "#define UIFONT_H", '#include "types.h"', "",
           "typedef struct { uint8_t w, h; int8_t xoff, yoff; uint8_t adv_q4; uint32_t off; } uifont_glyph_t;",
           "typedef struct { uint8_t first, count, ascent, line; const uifont_glyph_t* g; const uint8_t* bits; } uifont_t;", ""]
    total = 0
    for f in fonts:
        out.append(c_array(f"uf_{f['name']}_bits", f["bits"]))
        out.append(f"static const uifont_glyph_t uf_{f['name']}_glyphs[{f['count']}] = {{")
        for (w, h, xo, yo, adv, off) in f["glyphs"]:
            out.append(f"    {{{w},{h},{xo},{yo},{adv},{off}}},")
        out.append("};")
        out.append(f"static const uifont_t uf_{f['name']} = {{ {f['first']}, {f['count']}, {f['ascent']}, {f['line']}, "
                   f"uf_{f['name']}_glyphs, uf_{f['name']}_bits }};")
        out.append("")
        total += len(f["bits"]) + 8 * f["count"]
    out.append("/* 8x16 cell glyphs for ASCII 32..126, 64 bytes each. */")
    out.append(c_array("uf_cell_bits", cell, per=32))
    total += len(cell)
    cw, chh, cur = build_cursor()
    out.append("")
    out.append(f"/* Pointer: {cw}x{chh} ARGB (alpha in the top byte), hotspot (1,1). */")
    out.append(f"#define UF_CURSOR_W {cw}")
    out.append(f"#define UF_CURSOR_H {chh}")
    out.append(f"static const uint32_t uf_cursor[{len(cur)}] = {{")
    for i in range(0, len(cur), 10):
        out.append("    " + ",".join(f"0x{v:08X}" for v in cur[i:i + 10]) + ",")
    out.append("};")
    total += len(cur) * 4
    out += ["", "#endif /* UIFONT_H */", ""]
    open(OUT, "w", newline="\n").write("\n".join(out))
    print(f"wrote {OUT}: ~{total // 1024} KB of font data")

if __name__ == "__main__":
    main()
