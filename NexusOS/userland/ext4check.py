#!/usr/bin/env python3
"""ext4check.py -- read-only consistency checker for NexusOS ext4 images (an e2fsck -fn substitute).

    python -I ext4check.py <image> [--no-replay] [--replay-in-place] [--json] [--expect-clean]

Checks the superblock, group descriptors, block/inode bitmaps against actual usage (extent trees,
directories, metadata), free counts, link counts, directory structure ('.', '..', rec_len, names,
file types), htree index structure and hash ranges, orphan list, and every metadata_csum checksum
(superblock, descriptors, bitmaps, inodes, extent blocks, directory leaf/index blocks, journal).
A journal that needs recovery is replayed in memory first (the image is not modified) unless
--no-replay is given (then it counts as an error) or --replay-in-place is given (the image is updated).
--expect-clean additionally fails when the journal needed recovery or the volume is marked in use.
Exit status 0 = clean, 1 = errors found.
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ext4lib', os.path.join(HERE, 'ext4lib.py'))
ext4lib = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ext4lib)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('--no-replay', action='store_true')
    ap.add_argument('--replay-in-place', action='store_true')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--expect-clean', action='store_true')
    a = ap.parse_args(argv)
    if a.replay_in_place:
        fs = ext4lib.Ext4(a.image, writable=True)
        n = ext4lib.journal_replay(fs, apply_to_overlay=False)
        if n:
            sb = ext4lib.Super(fs._read_raw(1024, 1024))
            sb.incompat = sb.incompat & ~ext4lib.INCOMPAT_RECOVER
            sb.checksum = fs.sb_csum(sb.raw)
            fs.f.seek(fs.base + 1024)
            fs.f.write(sb.raw)
        fs.close()
        print('[ext4check] replayed %d journal blocks in place' % n)
    c = ext4lib.Checker(a.image, replay=not a.no_replay)
    errors = c.run()
    stats = getattr(c, 'stats', {})
    if a.expect_clean:
        if c.replayed:
            errors.append('journal needed recovery (%d blocks)' % c.replayed)
        if not (c.fs.sb.state & ext4lib.STATE_VALID):
            errors.append('volume not cleanly unmounted (state %#x)' % c.fs.sb.state)
        if c.fs.sb.incompat & ext4lib.INCOMPAT_RECOVER:
            errors.append('needs_recovery flag still set')
    if a.json:
        print(json.dumps({'errors': errors, 'stats': stats}))
    else:
        for e in errors[:200]:
            print('ERROR:', e)
        print('[ext4check] %s: %s; %d inodes, %d free blocks, %d free inodes, replayed %d journal blocks, %d orphans' % (
            a.image, 'CLEAN' if not errors else '%d ERRORS' % len(errors), stats.get('inodes', 0),
            stats.get('free_blocks', 0), stats.get('free_inodes', 0), c.replayed, stats.get('orphans', 0)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
