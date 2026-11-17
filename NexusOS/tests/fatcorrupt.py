"""Plant FAT32 damage into a disk image (the kinds a crash, a power cut, a buggy driver or another OS leave behind),
for tests/test_x64_fatcheck.py (kernel boot check) and the userland/fatfsck.py / dosfstools fsck.fat cross-checks.

    import fatcorrupt; expect = fatcorrupt.plant(path)   # returns what a correct repair must produce
    problems = fatcorrupt.verify_repaired(path, expect)   # [] when the repaired image matches

Everything lives below /fsck (and two root-level lost chains); the rest of the image is not touched."""
import importlib.util, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('fatcorrupt_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(fatput)
EOC = 0x0FFFFFFF


def first_of(e): return struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0]


def _put(fs, dirc, name, data):
    n = -(-len(data) // fs.cl_bytes)
    first = fs.alloc(n)
    for i, c in enumerate(fs.chain(first) if first else []):
        fs.wr(fs.cl_off(c), data[i * fs.cl_bytes:(i + 1) * fs.cl_bytes].ljust(fs.cl_bytes, b'\0'))
    return fs.create_entry(dirc, name, 0x20, first, len(data)), first


def _slot(fs, dirc, name):
    c, off, e = fs.find_name(dirc, name)
    return c, off, bytearray(e)


def plant(path):
    fs = fatput.Fat32(str(path))
    try:
        cs = fs.cl_bytes; expect = {'cs': cs}
        top = fs.lookup_dir(['fsck'], create=True)
        sub = fs.lookup_dir(['fsck', 'sub'], create=True)
        # 1. lost chains (an orphan's clusters after a crash)
        a = fs.alloc(5); b = fs.alloc(37)
        expect['lost'] = 42
        # 2. chain longer than the size
        _, first = _put(fs, top, 'long.bin', bytes(range(256)) * 12)          # 3072 bytes
        last = fs.chain(first)[-1]; extra = fs.alloc(4); fs.fat_set(last, extra)
        expect['long.bin'] = (3072, -(-3072 // cs)); expect['lost'] += 4
        # 3. size larger than the chain
        (c, off), first = _put(fs, top, 'short.bin', b'S' * (2 * cs))
        e = bytearray(fs.rd(fs.cl_off(c) + off, 32)); struct.pack_into('<I', e, 28, 10 * cs); fs.put_entry(c, off, e)
        expect['short.bin'] = (2 * cs, 2)
        # 4. cross-link: b.bin's second cluster points into a.bin's third (b's own tail becomes lost)
        _, fa = _put(fs, top, 'a.bin', b'A' * (4 * cs))
        _, fb = _put(fs, top, 'b.bin', b'B' * (4 * cs))
        ca, cb = fs.chain(fa), fs.chain(fb)
        fs.fat_set(cb[1], ca[2])
        expect['a.bin'] = (4 * cs, 4); expect['b.bin'] = (2 * cs, 2); expect['lost'] += 2
        # 5. orphaned long-name slots (no short entry follows)
        slots = fs.free_slots(top, 2)
        for i, (sc, so) in enumerate(slots):
            lfn = bytearray(32); lfn[0] = (2 - i) | (0x40 if i == 0 else 0); lfn[11] = 0x0F; lfn[13] = 0x99
            lfn[1:11] = 'orphan'.encode('utf-16-le')[:10]; fs.put_entry(sc, so, lfn)
        fs.put_entry(*slots[1], b'\xe5' + bytes(lfn[1:]))   # the second slot deleted: the start slot is an orphan
        # 6. an unpublished staged entry: T0000042.TMP with the long name of its final alias PENDING.TXT
        final = b'PENDING TXT'; temp = b'T0000042TMP'
        data_first = fs.alloc(2)
        wide = 'pending.txt'.encode('utf-16-le') + b'\0\0'; wide = wide.ljust(26, b'\xff')
        two = fs.free_slots(top, 2)
        lfn = bytearray(32); lfn[0] = 0x41; lfn[11] = 0x0F; lfn[13] = fs.name_checksum(final)
        lfn[1:11], lfn[14:26], lfn[28:32] = wide[:10], wide[10:22], wide[22:26]; fs.put_entry(*two[0], lfn)
        fs.put_entry(*two[1], fs.make_entry(temp, 0x20, data_first, 2 * cs))
        expect['pending'] = True; expect['lost'] += 2
        # 7. a FAT entry outside the volume at the end of a file's chain
        _, fi = _put(fs, top, 'invalid.bin', b'I' * (3 * cs))
        fs.fat_set(fs.chain(fi)[-1], fs.nclusters + 2 + 100)
        expect['invalid.bin'] = (3 * cs, 3)
        # 8. '..' of a subdirectory points elsewhere, the subdirectory entry has a size
        dotdot = bytearray(fs.rd(fs.cl_off(sub) + 32, 32)); struct.pack_into('<H', dotdot, 26, 12345); fs.wr(fs.cl_off(sub) + 32, dotdot)
        c, off, e = _slot(fs, top, 'sub'); struct.pack_into('<I', e, 28, 4096); fs.put_entry(c, off, e)
        expect['sub_dotdot'] = top
        # 9. the same file under two names (a rename interrupted between writing the new name and erasing the old)
        _, fd = _put(fs, top, 'renamed-from.txt', b'R' * (cs + 1))
        c, off, e = _slot(fs, top, 'renamed-from.txt')
        fs.create_entry(top, 'renamed-to.txt', 0x20, fd, cs + 1)
        expect['dup'] = ('renamed-from.txt', 'renamed-to.txt', cs + 1)
        # 10. a file inside the subdirectory (tree walk below a repaired '..')
        _put(fs, sub, 'deep.txt', b'deep' * 300)
        expect['deep.txt'] = (1200, -(-1200 // cs))
        # 11. an entry after the end-of-directory marker keeps its clusters
        hid = fs.alloc(3); tail = None
        for c, o, e in fs.entries(sub):
            if e[0] == 0: tail = (c, o); break
        hc, ho = tail
        nxt = (hc, ho + 32) if ho + 32 < fs.cl_bytes else None
        if nxt is None:
            raise RuntimeError('sub directory end marker at a cluster boundary')
        fs.put_entry(hc, ho, bytes(32))
        fs.put_entry(*nxt, fs.make_entry(b'HIDDEN  BIN', 0x20, hid, 3 * cs))
        expect['hidden'] = hid
        fs.flush_fat()
        # 12. FAT copies differ: copy 2 says a free cluster is allocated; 13. dirty flag in copy 1
        free_c = next(c for c in range(fs.nclusters + 1, 2, -1) if fs.fat_get(c) == 0)
        if fs.nfats > 1: fs.wr((fs.rsv + fs.fatsz) * 512 + free_c * 4, struct.pack('<I', EOC))
        state = struct.unpack('<I', fs.rd(fs.rsv * 512 + 4, 4))[0]; fs.wr(fs.rsv * 512 + 4, struct.pack('<I', state & ~0x08000000))
        expect['fatdiff_cluster'] = free_c
        expect['free_before'] = sum(1 for c in range(2, fs.nclusters + 2) if fs.fat_get(c) == 0)
    finally:
        fs.close()
    return expect


def verify_repaired(path, expect):
    """[] if the repaired image is what the plant() expectations say; else problem strings."""
    out = []
    fs = fatput.Fat32(str(path), readonly=True)
    try:
        cs = fs.cl_bytes
        top = fs.lookup_dir(['fsck']); sub = fs.lookup_dir(['fsck', 'sub'])
        def file(dirc, name):
            hit = fs.find_name(dirc, name)
            if not hit: return None
            e = hit[2]; first = first_of(e); size = struct.unpack_from('<I', e, 28)[0]
            return size, len(fs.chain(first)) if first else 0
        for name in ('long.bin', 'short.bin', 'a.bin', 'b.bin', 'invalid.bin'):
            got = file(top, name)
            if got != expect[name]: out.append('%s: %s, expected %s' % (name, got, expect[name]))
        if file(sub, 'deep.txt') != expect['deep.txt']: out.append('deep.txt: %s' % (file(sub, 'deep.txt'),))
        names = [n for n, *_ in fs.named_entries(top)]
        if any(n.lower().startswith('t0000042') or n == 'pending.txt' for n in names): out.append('pending entry still there')
        frm, to, size = expect['dup']
        present = [n for n in (frm, to) if fs.find_name(top, n)]
        if len(present) != 1: out.append('duplicate entries: %s present' % present)
        elif file(top, present[0]) != (size, 2): out.append('dup survivor %s: %s' % (present[0], file(top, present[0])))
        dotdot = fs.rd(fs.cl_off(sub) + 32, 32)
        if dotdot[:2] != b'..' or first_of(dotdot) != expect['sub_dotdot']: out.append('.. of sub points to %d' % first_of(dotdot))
        c, off, e = fs.find_name(top, 'sub')
        if struct.unpack_from('<I', e, 28)[0]: out.append('sub directory still has a size')
        if fs.fat_get(expect['hidden']) == 0: out.append('the hidden entry lost its clusters')
        if fs.fat_get(expect['fatdiff_cluster']) != 0: out.append('the cluster only FAT copy 2 had allocated is not free')
        if fs.nfats > 1 and fs.rd((fs.rsv + fs.fatsz) * 512, fs.fatsz * 512)[8:] != bytes(fs.fat)[8:]: out.append('FAT copies still differ')
        state = struct.unpack('<I', fs.rd(fs.rsv * 512 + 4, 4))[0]
        if not state & 0x08000000: out.append('clean bit not set')
    finally:
        fs.f.close()
    return out
