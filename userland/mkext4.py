#!/usr/bin/env python3
"""mkext4.py -- create an ext4 volume for NexusOS (no mkfs.ext4 needed).

    python -I mkext4.py <image> [--size MiB] [--whole-disk] [--journal-blocks N] [--label NAME]
                                [--inode-ratio BYTES]

By default the image keeps the NexusOS disk layout: an MBR whose single Linux (type 0x83) partition
starts at LBA 32768; the first 16 MiB (boot/legacy/NPFS regions) are preserved when the image exists.
--whole-disk formats the entire image instead. Features: 4 KiB blocks, 64bit, extents, flex_bg,
sparse_super, filetype, dir_index, large_file, huge_file, dir_nlink, extra_isize (256-byte inodes,
nanosecond times), metadata_csum (crc32c) and an internal jbd2 journal (checksum v3).
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ext4lib', os.path.join(HERE, 'ext4lib.py'))
ext4lib = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ext4lib)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('--size', type=int, help='image size in MiB (created or grown, sparse)')
    ap.add_argument('--whole-disk', action='store_true')
    ap.add_argument('--journal-blocks', type=int)
    ap.add_argument('--label', default='nexus')
    ap.add_argument('--inode-ratio', type=int, default=16384)
    a = ap.parse_args(argv)
    size = a.size * 1024 * 1024 if a.size else None
    if not size and not os.path.exists(a.image):
        ap.error('--size is required for a new image')
    base, length = ext4lib.mkfs(a.image, size, base=0 if a.whole_disk else None, label=a.label.encode(),
                                inode_ratio=a.inode_ratio, journal_blocks=a.journal_blocks)
    errors, stats, _ = ext4lib.check_image(a.image)
    if errors:
        print('\n'.join(errors[:50]))
        raise SystemExit('mkext4: self-check failed')
    print('[mkext4] %s: ext4 at byte %d, %d MiB, %d free blocks, %d free inodes' %
          (a.image, base, length >> 20, stats['free_blocks'], stats['free_inodes']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
