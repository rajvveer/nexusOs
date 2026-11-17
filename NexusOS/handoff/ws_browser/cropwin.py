"""Crop the Nexus Browser window (1200x864 at 360,64 @1080p, plus a margin) out of screenshots.
usage: python -I cropwin.py <out_dir> <png|dir> ...   (pass -full to keep the whole frame but halve it)"""
import os, sys
from PIL import Image
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
full = '-full' in sys.argv
srcs = []
for a in sys.argv[2:]:
    if a == '-full': continue
    if os.path.isdir(a): srcs += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.endswith('.png')]
    else: srcs.append(a)
for p in srcs:
    im = Image.open(p).convert('RGB')
    if full: im = im.resize((im.size[0] // 2, im.size[1] // 2), Image.LANCZOS)
    elif im.size[0] >= 1900: im = im.crop((352, 24, 1584, 944))
    im.save(os.path.join(out, os.path.basename(p)))
    print("cropped", os.path.basename(p), im.size)
