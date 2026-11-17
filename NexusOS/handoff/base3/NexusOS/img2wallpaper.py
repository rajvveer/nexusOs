"""
Convert an image to NexusOS VGA text-mode wallpaper data.
Output: wallpaper_img.c containing 80x24 char+color arrays.

VGA 16-color palette (approximate RGB):
  0 Black       (0,0,0)        8 DarkGrey    (85,85,85)
  1 Blue        (0,0,170)      9 LightBlue   (85,85,255)
  2 Green       (0,170,0)     10 LightGreen  (85,255,85)
  3 Cyan        (0,170,170)   11 LightCyan   (85,255,255)
  4 Red         (170,0,0)     12 LightRed    (255,85,85)
  5 Magenta     (170,0,170)   13 LightMagenta(255,85,255)
  6 Brown       (170,85,0)    14 Yellow      (255,255,85)
  7 LightGrey   (170,170,170) 15 White       (255,255,255)
"""

from PIL import Image
import math, sys

W, H = 80, 24

# VGA palette
VGA = [
    (0,0,0),       (0,0,170),     (0,170,0),     (0,170,170),
    (170,0,0),     (170,0,170),   (170,85,0),    (170,170,170),
    (85,85,85),    (85,85,255),   (85,255,85),   (85,255,255),
    (255,85,85),   (255,85,255),  (255,255,85),  (255,255,255),
]

# Shading characters (from dense to sparse)
SHADE_CHARS = [0xDB, 0xB2, 0xB1, 0xB0, 0xFA, ord(' ')]
# DB=full, B2=dark shade, B1=med shade, B0=light shade, FA=dot, space

def color_dist(c1, c2):
    return sum((a-b)**2 for a,b in zip(c1,c2))

def find_best_vga(r, g, b):
    """Find closest VGA color index."""
    best_i, best_d = 0, float('inf')
    for i, pal in enumerate(VGA):
        d = color_dist((r,g,b), pal)
        if d < best_d:
            best_d = d
            best_i = i
    return best_i

def find_best_pair(r, g, b):
    """
    Find best fg+bg VGA pair and shade character.
    The shade char blends fg and bg colors visually.
    """
    best_fg, best_bg, best_sh = 0, 0, 0
    best_d = float('inf')

    # Blending ratios for each shade char
    ratios = [1.0, 0.75, 0.5, 0.25, 0.1, 0.0]

    target = (r, g, b)

    # Optimization: only try a subset of promising colors
    # Find top 4 closest colors
    dists = [(color_dist(target, VGA[i]), i) for i in range(16)]
    dists.sort()
    candidates = [d[1] for d in dists[:6]]

    for fi in candidates:
        for bi in candidates:
            for si, ratio in enumerate(ratios):
                # Blended color
                br = VGA[fi][0]*ratio + VGA[bi][0]*(1-ratio)
                bg_ = VGA[fi][1]*ratio + VGA[bi][1]*(1-ratio)
                bb = VGA[fi][2]*ratio + VGA[bi][2]*(1-ratio)
                d = color_dist(target, (br, bg_, bb))
                if d < best_d:
                    best_d = d
                    best_fg = fi
                    best_bg = bi
                    best_sh = si

    return best_fg, best_bg, SHADE_CHARS[best_sh]

def convert(img_path, out_path):
    img = Image.open(img_path).convert('RGB')
    img = img.resize((W, H), Image.LANCZOS)

    chars = []
    colors = []

    for y in range(H):
        row_ch = []
        row_co = []
        for x in range(W):
            r, g, b = img.getpixel((x, y))
            fg, bg, ch = find_best_pair(r, g, b)
            row_ch.append(ch)
            row_co.append((bg << 4) | fg)
        chars.append(row_ch)
        colors.append(row_co)

    # Write C file
    with open(out_path, 'w') as f:
        f.write("/* Auto-generated wallpaper image data — 80x24 VGA text art */\n")
        f.write("/* Source: landscape.png converted by img2wallpaper.py */\n\n")
        f.write("#include \"types.h\"\n\n")

        f.write("const unsigned char wp_image_chars[24][80] = {\n")
        for y in range(H):
            f.write("    {")
            f.write(",".join(f"0x{c:02X}" for c in chars[y]))
            f.write("},\n")
        f.write("};\n\n")

        f.write("const unsigned char wp_image_colors[24][80] = {\n")
        for y in range(H):
            f.write("    {")
            f.write(",".join(f"0x{c:02X}" for c in colors[y]))
            f.write("},\n")
        f.write("};\n")

    print(f"Done! Generated {out_path}")

if __name__ == "__main__":
    convert(
        r"assets\ChatGPT Image Feb 26, 2026, 02_54_43 PM.png",
        r"kernel\wallpaper_img.c"
    )
