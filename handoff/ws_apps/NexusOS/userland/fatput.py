#!/usr/bin/env python3
"""
fatput.py -- add/replace files in the FAT32 volume of an EXISTING NexusOS disk.img
(Phase 57 "apps").

    python -I fatput.py <disk.img> ls [path]
    python -I fatput.py <disk.img> mkdir <dir> [<dir> ...]
    python -I fatput.py <disk.img> put <host_file> <fat_path> [put <host_file> <fat_path> ...]
    python -I fatput.py <disk.img> rm <fat_path>

The FAT32 volume occupies LBA 0 .. NPFS_WINDOW_LBA-1 (20480 sectors = 10 MiB).
Everything from LBA 20480 on (the NPFS journaling FS window and the Phase 55
persistence slots at LBA 26624) belongs to other subsystems: this tool NEVER
reads or writes a byte at or beyond LBA 20480 -- every write goes through
`wr()` which asserts that bound. If the first 10 MiB hold no valid FAT32 BPB
(a fresh zeroed disk.img) the volume is formatted in place with the same
geometry mkfat32.py uses (4 KiB clusters, 32 reserved sectors, 2 FATs).

Names are plain 8.3 (stored upper-case, matched case-insensitively by the
kernel driver, which lower-cases them for display). Long names are rejected
rather than written as LFN entries, so every file put here is visible to the
kernel. Directories are created on demand for `put` ("bin/lua" creates BIN).
"""
import os, struct, sys, time

SECTOR = 512
LIMIT_LBA = 20480                     # == NPFS_WINDOW_LBA in kernel/npfs.h
LIMIT = LIMIT_LBA * SECTOR
EOC = 0x0FFFFFFF


class Fat32:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'r+b')
        self.f.seek(0, 2)
        self.disk_size = self.f.tell()
        if self.disk_size < LIMIT:
            raise SystemExit("disk image smaller than the 10 MiB FAT32 window")
        bpb = self.rd(0, SECTOR)
        if not self.valid_bpb(bpb):
            print("[fatput] no FAT32 volume in LBA 0..%d: formatting that window only" % (LIMIT_LBA - 1))
            self.format()
            bpb = self.rd(0, SECTOR)
        self.bps = struct.unpack_from('<H', bpb, 11)[0]
        self.spc = bpb[13]
        self.rsv = struct.unpack_from('<H', bpb, 14)[0]
        self.nfats = bpb[16]
        self.total = struct.unpack_from('<I', bpb, 32)[0]
        self.fatsz = struct.unpack_from('<I', bpb, 36)[0]
        self.rootc = struct.unpack_from('<I', bpb, 44)[0]
        self.data_lba = self.rsv + self.nfats * self.fatsz
        self.cl_bytes = self.spc * self.bps
        self.nclusters = (self.total - self.data_lba) // self.spc
        if self.total > LIMIT_LBA:
            raise SystemExit("FAT32 volume claims %d sectors, past the NPFS window" % self.total)
        self.fat = bytearray(self.rd(self.rsv * SECTOR, self.fatsz * SECTOR))

    # ---- raw I/O, bounded to the FAT32 window ------------------------------
    def rd(self, off, n):
        assert off >= 0 and off + n <= LIMIT, "read outside FAT32 window"
        self.f.seek(off)
        return self.f.read(n)

    def wr(self, off, data):
        assert off >= 0 and off + len(data) <= LIMIT, "write outside FAT32 window (LBA >= %d)" % LIMIT_LBA
        self.f.seek(off)
        self.f.write(data)

    @staticmethod
    def valid_bpb(b):
        return (b[510] == 0x55 and b[511] == 0xAA and struct.unpack_from('<H', b, 11)[0] == 512
                and b[13] in (1, 2, 4, 8, 16, 32, 64, 128) and struct.unpack_from('<I', b, 36)[0] != 0
                and b[82:87] == b'FAT32')

    def format(self):
        spc, rsv, nfats = 8, 32, 2
        total = LIMIT_LBA
        clusters = (total - rsv) // spc
        fatsz = (clusters + 127) // 128
        clusters = (total - rsv - nfats * fatsz) // spc
        bpb = bytearray(SECTOR)
        bpb[0:3] = b'\xEB\x58\x90'; bpb[3:11] = b'NEXUSOS '
        struct.pack_into('<HBHBHHBHHHII', bpb, 11, 512, spc, rsv, nfats, 0, 0, 0xF8, 0, 63, 16, 0, total)
        struct.pack_into('<IHHIHH', bpb, 36, fatsz, 0, 0, 2, 1, 6)
        bpb[64] = 0x80; bpb[66] = 0x29
        struct.pack_into('<I', bpb, 67, 0x4E585553)
        bpb[71:82] = b'NEXUSOS    '; bpb[82:90] = b'FAT32   '
        bpb[510] = 0x55; bpb[511] = 0xAA
        fsi = bytearray(SECTOR)
        struct.pack_into('<I', fsi, 0, 0x41615252); struct.pack_into('<I', fsi, 484, 0x61417272)
        struct.pack_into('<I', fsi, 488, 0xFFFFFFFF); struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)
        fsi[510] = 0x55; fsi[511] = 0xAA
        # zero reserved + FATs + root cluster, then lay down the structures
        data_lba = rsv + nfats * fatsz
        self.wr(0, bytes(data_lba * SECTOR + spc * SECTOR))
        self.wr(0, bpb); self.wr(SECTOR, fsi); self.wr(6 * SECTOR, bpb)
        fat0 = bytearray(SECTOR)
        struct.pack_into('<III', fat0, 0, 0x0FFFFFF8, EOC, EOC)
        for k in range(nfats):
            self.wr((rsv + k * fatsz) * SECTOR, fat0)
        root = bytearray(32); root[0:11] = b'NEXUSOS    '; root[11] = 0x08
        self.wr(data_lba * SECTOR, root)

    # ---- FAT helpers --------------------------------------------------------
    def fat_get(self, c):
        return struct.unpack_from('<I', self.fat, c * 4)[0] & 0x0FFFFFFF

    def fat_set(self, c, v):
        old = struct.unpack_from('<I', self.fat, c * 4)[0] & 0xF0000000
        struct.pack_into('<I', self.fat, c * 4, old | (v & 0x0FFFFFFF))

    def flush_fat(self):
        for k in range(self.nfats):
            self.wr((self.rsv + k * self.fatsz) * SECTOR, bytes(self.fat))

    def chain(self, c):
        out = []
        while 2 <= c < 0x0FFFFFF8 and len(out) <= self.nclusters:
            out.append(c); c = self.fat_get(c)
        return out

    def free_chain(self, c):
        for x in self.chain(c):
            self.fat_set(x, 0)

    def alloc(self, n):
        """Allocate n clusters as one chain; returns the first (0 if n == 0)."""
        if n == 0:
            return 0
        got = [c for c in range(2, self.nclusters + 2) if self.fat_get(c) == 0][:n]
        if len(got) < n:
            raise SystemExit("FAT32 volume full (%d clusters needed)" % n)
        for i, c in enumerate(got):
            self.fat_set(c, got[i + 1] if i + 1 < n else EOC)
            self.wr(self.cl_off(c), bytes(self.cl_bytes))
        return got[0]

    def cl_off(self, c):
        return (self.data_lba + (c - 2) * self.spc) * SECTOR

    def read_chain(self, c, size):
        out = bytearray()
        for x in self.chain(c):
            out += self.rd(self.cl_off(x), self.cl_bytes)
        return bytes(out[:size])

    def write_chain(self, c, data):
        cl = self.chain(c)
        for i, x in enumerate(cl):
            self.wr(self.cl_off(x), data[i * self.cl_bytes:(i + 1) * self.cl_bytes].ljust(self.cl_bytes, b'\0'))

    # ---- names ----------------------------------------------------------------
    @staticmethod
    def to83(name):
        name = name.upper()
        bad = set(' "*+,/:;<=>?[\\]|')
        if '.' in name:
            base, ext = name.rsplit('.', 1)
        else:
            base, ext = name, ''
        if not base or len(base) > 8 or len(ext) > 3 or any(ch in bad or ord(ch) < 32 for ch in base + ext):
            raise SystemExit("name '%s' does not fit 8.3 (keep binaries <= 8 chars, no LFN support)" % name)
        return base.ljust(8).encode('latin1') + ext.ljust(3).encode('latin1')

    @staticmethod
    def from83(raw):
        base = raw[:8].decode('latin1').rstrip(); ext = raw[8:11].decode('latin1').rstrip()
        return base + ('.' + ext if ext else '')

    # ---- directories ------------------------------------------------------------
    def entries(self, dirc):
        """Yield (cluster, byte_offset_in_cluster, entry32) for every slot of a directory."""
        for c in self.chain(dirc):
            blk = self.rd(self.cl_off(c), self.cl_bytes)
            for o in range(0, self.cl_bytes, 32):
                yield c, o, blk[o:o + 32]

    def find(self, dirc, name83):
        for c, o, e in self.entries(dirc):
            if e[0] == 0:
                return None
            if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x08):
                continue
            if e[:11] == name83:
                return c, o, e
        return None

    def lookup_dir(self, parts, create=False):
        """Walk directory components from the root; returns the directory's first cluster."""
        cur = self.rootc
        for p in parts:
            n83 = self.to83(p)
            hit = self.find(cur, n83)
            if hit is None:
                if not create:
                    raise SystemExit("no such directory: %s" % p)
                cur = self.mkdir_in(cur, n83)
                continue
            c, o, e = hit
            if not (e[11] & 0x10):
                raise SystemExit("%s is not a directory" % p)
            cur = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        return cur

    def free_slot(self, dirc):
        last = None
        for c, o, e in self.entries(dirc):
            last = c
            if e[0] in (0, 0xE5):
                return c, o
        # extend the directory by one cluster
        nc = self.alloc(1)
        self.fat_set(last, nc)
        return nc, 0

    def put_entry(self, c, o, entry):
        self.wr(self.cl_off(c) + o, entry)

    @staticmethod
    def stamp():
        t = time.localtime()
        d = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
        tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
        return d, tm

    def make_entry(self, name83, attr, first, size):
        e = bytearray(32)
        e[0:11] = name83; e[11] = attr
        d, tm = self.stamp()
        struct.pack_into('<HHHHHHHI', e, 14, tm, d, d, (first >> 16) & 0xFFFF, tm, d, first & 0xFFFF, size)
        return bytes(e)

    def mkdir_in(self, parent, name83):
        nc = self.alloc(1)
        dot = self.make_entry(b'.          ', 0x10, nc, 0)
        dotdot = self.make_entry(b'..         ', 0x10, 0 if parent == self.rootc else parent, 0)
        self.wr(self.cl_off(nc), dot + dotdot)
        c, o = self.free_slot(parent)
        self.put_entry(c, o, self.make_entry(name83, 0x10, nc, 0))
        return nc

    # ---- commands -----------------------------------------------------------------
    def split(self, fat_path):
        parts = [p for p in fat_path.replace('\\', '/').split('/') if p and p != '.']
        if not parts:
            raise SystemExit("empty path")
        return parts[:-1], parts[-1]

    def put(self, host_file, fat_path):
        data = open(host_file, 'rb').read()
        dirs, leaf = self.split(fat_path)
        dirc = self.lookup_dir(dirs, create=True)
        n83 = self.to83(leaf)
        hit = self.find(dirc, n83)
        nclu = (len(data) + self.cl_bytes - 1) // self.cl_bytes
        if hit is not None:
            c, o, e = hit
            if e[11] & 0x10:
                raise SystemExit("%s is a directory" % fat_path)
            old = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            if old >= 2:
                self.free_chain(old)
        else:
            c, o = self.free_slot(dirc)
        first = self.alloc(nclu)
        if first:
            self.write_chain(first, data)
        self.put_entry(c, o, self.make_entry(n83, 0x20, first, len(data)))
        self.flush_fat()
        print("  put %-28s -> /disk/%-20s %7d bytes (%d clusters)" % (os.path.basename(host_file), '/'.join(dirs + [leaf]).lower(), len(data), nclu))

    def mkdir(self, fat_path):
        dirs, leaf = self.split(fat_path)
        self.lookup_dir(dirs + [leaf], create=True)
        self.flush_fat()
        print("  mkdir /disk/%s" % '/'.join(dirs + [leaf]).lower())

    def rm(self, fat_path):
        dirs, leaf = self.split(fat_path)
        dirc = self.lookup_dir(dirs)
        hit = self.find(dirc, self.to83(leaf))
        if hit is None:
            raise SystemExit("not found: %s" % fat_path)
        c, o, e = hit
        first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        if first >= 2:
            self.free_chain(first)
        self.put_entry(c, o, b'\xE5' + e[1:])
        self.flush_fat()
        print("  rm /disk/%s" % fat_path.lower())

    def ls(self, fat_path='', indent=0, dirc=None):
        if dirc is None:
            dirs = [p for p in fat_path.replace('\\', '/').split('/') if p]
            dirc = self.lookup_dir(dirs)
        for c, o, e in self.entries(dirc):
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x08):
                continue
            name = self.from83(e)
            first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            if e[11] & 0x10:
                print('  ' * indent + "%-14s <DIR>" % (name.lower() + '/'))
                if name not in ('.', '..'):
                    self.ls(indent=indent + 1, dirc=first)
            else:
                print('  ' * indent + "%-14s %8d" % (name.lower(), size))

    def close(self):
        self.f.close()


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 1
    fs = Fat32(argv[1])
    i = 2
    while i < len(argv):
        cmd = argv[i]
        if cmd == 'ls':
            fs.ls(argv[i + 1] if i + 1 < len(argv) else ''); i += 2
        elif cmd == 'mkdir':
            fs.mkdir(argv[i + 1]); i += 2
        elif cmd == 'put':
            fs.put(argv[i + 1], argv[i + 2]); i += 3
        elif cmd == 'rm':
            fs.rm(argv[i + 1]); i += 2
        else:
            raise SystemExit("unknown command %s" % cmd)
    fs.close()
    free = sum(1 for c in range(2, fs.nclusters + 2) if fs.fat_get(c) == 0)
    print("[fatput] done: %d of %d clusters free (%d KiB)" % (free, fs.nclusters, free * fs.cl_bytes // 1024))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
