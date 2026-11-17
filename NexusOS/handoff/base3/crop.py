"""Crop the Terminal text area (640x400 at 640,354 @1080p) out of screenshots.
usage: python -I crop.py <out_dir> <png> [<png> ...]   (also accepts a directory)"""
import os, sys
from PIL import Image
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
srcs = []
for a in sys.argv[2:]:
    if os.path.isdir(a):
        srcs += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.endswith('.png')]
    else:
        srcs.append(a)
for p in srcs:
    im = Image.open(p).convert('RGB')
    if im.size[0] >= 1280:
        im = im.crop((640, 354, 1280, 754))
    im.save(os.path.join(out, os.path.basename(p)))
    print("cropped", os.path.basename(p), im.size)
