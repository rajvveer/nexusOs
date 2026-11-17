#!/usr/bin/env python3
"""
fatput.py -- add/replace files in the FAT32 volume of an EXISTING NexusOS disk.img
(Phase 57 "apps").

    python -I fatput.py <disk.img> ls [path]
    python -I fatput.py <disk.img> mkdir <dir> [<dir> ...]
    python -I fatput.py <disk.img> put <host_file> <fat_path> [put <host_file> <fat_path> ...]
    python -I fatput.py <disk.img> rm <fat_path>
    python -I fatput.py <disk.img> rmdir <fat_dir>        (empty directories only)

Legacy FAT occupies LBA 0..20479. Expanded images use an MBR FAT32 partition
at or beyond LBA 32768. All I/O is bounded to the selected volume; the fixed NPFS
and persistence region at LBA 20480..32767 is preserved. Mutations operate on a
temporary copy and replace the original only on success. Stop QEMU before use.
An empty legacy FAT window can be initialized, but corrupt nonempty volumes are
never reformatted. VFAT long UTF-8 names are supported; parent directories are
created on demand. Files are copied in bounded chunks.
"""
import os, struct, sys, time, shutil, tempfile, hashlib

SECTOR = 512
LIMIT_LBA = 20480                     # == NPFS_WINDOW_LBA in kernel/npfs.h
LIMIT = LIMIT_LBA * SECTOR
DATA_LBA = 32768                      # expanded FAT partition follows the reserved 16 MiB
EOC = 0x0FFFFFFF
# GPT partition types holding the FAT32 volume: NexusOS root, then Microsoft basic data (on-disk byte order).
GPT_FAT_TYPES = (bytes.fromhex('2C4F1B7D863A594E9C0D4E45585553F1'), bytes.fromhex('A2A0D0EBE5B9334487C068B6B72699C7'))


def is_gpt(sector0):
    return sector0[510:512] == b'\x55\xaa' and any(sector0[off + 4] == 0xEE for off in range(446, 510, 16))


class Fat32:
    def __init__(self, path, create=False, readonly=False):
        self.path = path
        self.readonly = readonly  # fat2ext4 reads source images without opening them for writing
        self.f = open(path, 'rb' if readonly else 'r+b')
        self.f.seek(0, 2)
        self.disk_size = self.f.tell()
        self.base = 0
        self.limit = LIMIT
        if self.disk_size < LIMIT:
            self.f.close()
            raise SystemExit("disk image smaller than the 10 MiB FAT32 window")
        bpb = self.rd(0, SECTOR)
        if not self.valid_bpb(bpb) and bpb[510:512] == b'\x55\xaa':
            for off in range(446, 510, 16):
                start, sectors = struct.unpack_from('<II', bpb, off + 8)
                if bpb[off + 4] in (0x0B, 0x0C) and start >= DATA_LBA and sectors:
                    if start + sectors > self.disk_size // SECTOR:
                        raise ValueError('FAT partition extends beyond the disk')
                    self.base, self.limit = start * SECTOR, sectors * SECTOR
                    bpb = self.rd(0, SECTOR)
                    break
            else:
                found = self.gpt_volume() if is_gpt(bpb) else None
                if found:
                    self.base, self.limit = found
                    bpb = self.rd(0, SECTOR)
        if not self.valid_bpb(bpb):
            if self.base or not create or any(self.rd(0, LIMIT)):
                self.f.close()
                raise ValueError("No valid FAT32 volume; only an entirely empty FAT window may be formatted")
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
        if (self.nfats not in (1, 2) or self.rsv < 2 or self.data_lba >= self.total
                or self.total * SECTOR > self.limit or not 2 <= self.rootc < self.nclusters + 2
                or (self.nclusters + 2) * 4 > self.fatsz * SECTOR):
            self.f.close()
            raise ValueError("Invalid or out-of-bounds FAT32 geometry")
        self.fat = bytearray(self.rd(self.rsv * SECTOR, self.fatsz * SECTOR))
        self.next_free = 2

    def gpt_volume(self):
        """GPT disks (userland/gptwrap.py): the first NexusOS-root or basic-data partition."""
        for sector in (512, 4096):
            self.f.seek(sector); h = self.f.read(92)
            if h[:8] != b'EFI PART': continue
            table, count, size = struct.unpack_from('<QII', h, 72)
            self.f.seek(table * sector); entries = self.f.read(count * size)
            for i in range(count):
                e = entries[i * size:(i + 1) * size]
                if e[:16] in GPT_FAT_TYPES:
                    first, last = struct.unpack_from('<QQ', e, 32)
                    if (last + 1) * sector > self.disk_size: raise ValueError('GPT partition extends beyond the disk')
                    return first * sector, (last - first + 1) * sector
        return None

    # ---- raw I/O, bounded to the FAT32 window ------------------------------
    def rd(self, off, n):
        if off < 0 or n < 0 or off + n > self.limit:
            raise ValueError("read outside FAT32 window")
        self.f.seek(self.base + off)
        data = self.f.read(n)
        if len(data) != n: raise ValueError('short disk read')
        return data

    def wr(self, off, data):
        if off < 0 or off + len(data) > self.limit:
            raise ValueError("write outside FAT32 window")
        self.f.seek(self.base + off)
        self.f.write(data)

    @staticmethod
    def valid_bpb(b):
        return (b[510] == 0x55 and b[511] == 0xAA and struct.unpack_from('<H', b, 11)[0] == 512
                and b[13] in (1, 2, 4, 8, 16, 32, 64, 128) and struct.unpack_from('<I', b, 36)[0] != 0
                and b[82:87] == b'FAT32')

    def format(self, total=LIMIT_LBA):
        spc, rsv, nfats = (64 if total > 16 * 1024 * 1024 else 8), 32, 2
        # New partitions use enough clusters to be FAT32; legacy 10 MiB images remain readable.
        while self.base and spc > 1 and (total - 4096) // spc < 65525:
            spc //= 2
        clusters = (total - rsv) // spc
        fatsz = (clusters + 127) // 128
        clusters = (total - rsv - nfats * fatsz) // spc
        bpb = bytearray(SECTOR)
        bpb[0:3] = b'\xEB\x58\x90'; bpb[3:11] = b'NEXUSOS '
        struct.pack_into('<HBHBHHBHHHII', bpb, 11, 512, spc, rsv, nfats, 0, 0, 0xF8, 0, 63, 16, 0, total)
        struct.pack_into('<I', bpb, 28, self.base // SECTOR)
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
        seen = set()
        while c < 0x0FFFFFF8:
            if not 2 <= c < self.nclusters + 2 or c in seen:
                raise ValueError("Invalid or cyclic FAT chain")
            seen.add(c)
            out.append(c); c = self.fat_get(c)
        return out

    def free_chain(self, c):
        for x in self.chain(c):
            self.fat_set(x, 0)
            self.next_free = min(self.next_free, x)

    def alloc(self, n):
        """Allocate n clusters as one chain; returns the first (0 if n == 0)."""
        if n == 0:
            return 0
        got = []
        for first, last in ((self.next_free, self.nclusters + 2), (2, self.next_free)):
            for c in range(first, last):
                if self.fat_get(c) == 0:
                    got.append(c)
                    if len(got) == n: break
            if len(got) == n: break
        if len(got) < n:
            raise SystemExit("FAT32 volume full (%d clusters needed)" % n)
        for i, c in enumerate(got):
            self.fat_set(c, got[i + 1] if i + 1 < n else EOC)
            self.wr(self.cl_off(c), bytes(self.cl_bytes))
        self.next_free = got[-1] + 1
        return got[0]

    def cl_off(self, c):
        return (self.data_lba + (c - 2) * self.spc) * SECTOR

    def read_chain(self, c, size):
        if size == 0: return b''
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
        bad = set(' ".*+,/:;<=>?[\\]|')
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

    def named_entries(self, dirc):
        parts, slots, expected, checksum = {}, [], 0, 0
        for c, off, entry in self.entries(dirc):
            if entry[0] == 0: break
            if entry[0] == 0xE5:
                parts, slots, expected = {}, [], 0
                continue
            if entry[11] == 0x0F:
                order = entry[0] & 31
                if entry[0] & 64 and 1 <= order <= 20:
                    parts, slots, expected, checksum = {}, [], order, entry[13]
                if not expected or order != expected or entry[13] != checksum or entry[12] or entry[26:28] != b'\0\0':
                    parts, slots, expected = {}, [], 0
                    continue
                parts[order] = entry[1:11] + entry[14:26] + entry[28:32]
                slots.append((c, off)); expected -= 1
                continue
            if not entry[11] & 8:
                name = self.from83(entry)
                if parts and not expected and checksum == self.name_checksum(entry[:11]):
                    raw = b''.join(parts[i] for i in sorted(parts))
                    units = [raw[i:i+2] for i in range(0, len(raw), 2)]
                    units = units[:next((i for i, x in enumerate(units) if x in (b'\0\0', b'\xff\xff')), len(units))]
                    try: name = b''.join(units).decode('utf-16-le')
                    except UnicodeDecodeError: pass
                yield name, c, off, entry, slots if not expected else []
            parts, slots, expected = {}, [], 0

    def find_name(self, dirc, name):
        for longname, c, off, entry, _ in self.named_entries(dirc):
            if name.casefold() in (longname.casefold(), self.from83(entry).casefold()):
                return c, off, entry
        return None

    @staticmethod
    def name_checksum(name):
        value = 0
        for b in name: value = (((value & 1) << 7) + (value >> 1) + b) & 255
        return value

    def short_name(self, dirc, name):
        if not name or name[-1] in '. ' or any(ord(c) < 32 or c in '"*/:<>?\\|' for c in name):
            raise ValueError('invalid FAT filename')
        if len(name.encode('utf-8')) > 255: raise ValueError('filename exceeds 255 UTF-8 bytes')
        try:
            short = self.to83(name)
            if name.isascii() and self.from83(short).casefold() == name.casefold(): return short
        except (SystemExit, UnicodeEncodeError):
            pass
        extension = name.rsplit('.', 1)[1] if '.' in name else ''
        extension = ''.join(c.upper() if c.isascii() and c.isalnum() else '_' for c in extension[:3])
        for n in range(1, 65537):
            short = ('N%07d' % n).encode() + extension.ljust(3).encode()
            if self.find(dirc, short) is None: return short
        raise ValueError('directory is full')

    def free_slots(self, dirc, count):
        available, after_end, last = [], False, None
        for c, off, entry in self.entries(dirc):
            last = c
            if entry[0] == 0: after_end = True
            if after_end or entry[0] == 0xE5: available.append((c, off))
            else: available = []
            if len(available) == count:
                # Preserve the end marker when the next slot already exists.
                if after_end:
                    if off + 32 < self.cl_bytes: self.wr(self.cl_off(c) + off + 32, b'\0')
                    else:
                        nxt = self.fat_get(c)
                        if 2 <= nxt < self.nclusters + 2: self.wr(self.cl_off(nxt), b'\0')
                return available
        while len(available) < count:
            new = self.alloc(1)
            self.fat_set(last, new); last = new
            for off in range(0, self.cl_bytes, 32):
                available.append((new, off))
                if len(available) == count: return available

    def create_entry(self, dirc, name, attr, first, size, metadata=None):
        short = metadata[:11] if metadata else self.short_name(dirc, name)
        wide = name.encode('utf-16-le')
        use_long = name != self.from83(short)
        count = (len(wide) // 2 + 12) // 13 if use_long else 0
        slots = self.free_slots(dirc, count + 1)
        wide = (wide + b'\0\0').ljust(count * 26, b'\xff')
        for i in range(count):
            order = count - i
            entry = bytearray(32)
            entry[0] = order | (64 if i == 0 else 0); entry[11] = 15
            entry[13] = self.name_checksum(short)
            chunk = wide[(order-1)*26:order*26]
            entry[1:11], entry[14:26], entry[28:32] = chunk[:10], chunk[10:22], chunk[22:26]
            self.put_entry(*slots[i], entry)
        entry = bytearray(metadata if metadata else self.make_entry(short, attr, first, size))
        entry[:11] = short; entry[11] = attr
        struct.pack_into('<H', entry, 20, first >> 16)
        struct.pack_into('<H', entry, 26, first & 65535)
        struct.pack_into('<I', entry, 28, size)
        self.put_entry(*slots[-1], entry)
        return slots[-1]

    def lookup_dir(self, parts, create=False):
        """Walk directory components from the root; returns the directory's first cluster."""
        cur = self.rootc
        for p in parts:
            hit = self.find_name(cur, p)
            if hit is None:
                if not create:
                    raise SystemExit("no such directory: %s" % p)
                cur = self.mkdir_name(cur, p)
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
        return self.mkdir_name(parent, self.from83(name83))

    def mkdir_name(self, parent, name, metadata=None):
        nc = self.alloc(1)
        dot = self.make_entry(b'.          ', 0x10, nc, 0)
        dotdot = self.make_entry(b'..         ', 0x10, 0 if parent == self.rootc else parent, 0)
        self.wr(self.cl_off(nc), dot + dotdot)
        self.create_entry(parent, name, 0x10, nc, 0, metadata)
        return nc

    # ---- commands -----------------------------------------------------------------
    def split(self, fat_path):
        parts = [p for p in fat_path.replace('\\', '/').split('/') if p and p != '.']
        if not parts:
            raise SystemExit("empty path")
        return parts[:-1], parts[-1]

    def put(self, host_file, fat_path):
        size = os.path.getsize(host_file)
        if size > 0xFFFFFFFF: raise ValueError('file exceeds FAT32 maximum size')
        dirs, leaf = self.split(fat_path)
        dirc = self.lookup_dir(dirs, create=True)
        hit = self.find_name(dirc, leaf)
        nclu = (size + self.cl_bytes - 1) // self.cl_bytes
        if hit is not None:
            c, o, e = hit
            if e[11] & 0x10:
                raise SystemExit("%s is a directory" % fat_path)
            old = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            if old >= 2:
                self.free_chain(old)
        first = self.alloc(nclu)
        with open(host_file, 'rb') as source:
            remaining = size
            for cluster in self.chain(first) if first else ():
                n = min(remaining, self.cl_bytes)
                data = source.read(n)
                if len(data) != n: raise ValueError('source file changed during copy')
                self.wr(self.cl_off(cluster), data)
                remaining -= n
        if hit:
            self.put_entry(c, o, self.make_entry(e[:11], 0x20, first, size))
        else:
            self.create_entry(dirc, leaf, 0x20, first, size)
        self.flush_fat()
        print("  put %-28s -> /disk/%-20s %7d bytes (%d clusters)" % (os.path.basename(host_file), '/'.join(dirs + [leaf]), size, nclu))

    def mkdir(self, fat_path):
        dirs, leaf = self.split(fat_path)
        self.lookup_dir(dirs + [leaf], create=True)
        self.flush_fat()
        print("  mkdir /disk/%s" % '/'.join(dirs + [leaf]).lower())

    def rm(self, fat_path):
        dirs, leaf = self.split(fat_path)
        dirc = self.lookup_dir(dirs)
        hit = self.find_name(dirc, leaf)
        if hit is None:
            raise SystemExit("not found: %s" % fat_path)
        c, o, e = hit
        if e[11] & 0x10:
            raise ValueError("rm accepts files only")
        first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        if first >= 2:
            self.free_chain(first)
        for _, lc, lo, _, slots in self.named_entries(dirc):
            if (lc, lo) == (c, o):
                for sc, so in slots: self.put_entry(sc, so, b'\xe5' + bytes(31))
                break
        self.put_entry(c, o, b'\xE5' + e[1:])
        self.flush_fat()
        print("  rm /disk/%s" % fat_path.lower())

    def rmdir(self, fat_path):
        """Remove an EMPTY directory (only '.', '..' and deleted slots left), like rmdir(1)."""
        dirs, leaf = self.split(fat_path)
        dirc = self.lookup_dir(dirs)
        hit = self.find_name(dirc, leaf)
        if hit is None:
            raise SystemExit("not found: %s" % fat_path)
        c, o, e = hit
        if not e[11] & 0x10:
            raise ValueError("rmdir accepts directories only: %s" % fat_path)
        first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        if first < 2 or first == self.rootc:
            raise ValueError("refusing to remove %s (cluster %d)" % (fat_path, first))
        for name, *_ in self.named_entries(first):
            if name not in ('.', '..'):
                raise SystemExit("directory not empty: %s" % fat_path)
        self.free_chain(first)
        for _, lc, lo, _, slots in self.named_entries(dirc):
            if (lc, lo) == (c, o):
                for sc, so in slots: self.put_entry(sc, so, b'\xe5' + bytes(31))
                break
        self.put_entry(c, o, b'\xE5' + e[1:])
        self.flush_fat()
        print("  rmdir /disk/%s" % fat_path.lower())

    def ls(self, fat_path='', indent=0, dirc=None):
        if dirc is None:
            dirs = [p for p in fat_path.replace('\\', '/').split('/') if p]
            dirc = self.lookup_dir(dirs)
        for name, c, o, e, _ in self.named_entries(dirc):
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x08):
                continue
            first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            if e[11] & 0x10:
                print('  ' * indent + "%-14s <DIR>" % (name.lower() + '/'))
                if name not in ('.', '..'):
                    self.ls(indent=indent + 1, dirc=first)
            else:
                print('  ' * indent + "%-14s %8d" % (name.lower(), size))

    def close(self):
        if not self.f.closed:
            try:
                if not getattr(self, 'readonly', False):
                    self.f.flush()
                    os.fsync(self.f.fileno())
            finally:
                self.f.close()


def edit(argv):
    if len(argv) < 3:
        print(__doc__); return 1
    fs = Fat32(argv[1], create=argv[2] != 'ls')
    try:
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
            elif cmd == 'rmdir':
                fs.rmdir(argv[i + 1]); i += 2
            else:
                raise SystemExit("unknown command %s" % cmd)
    finally:
        fs.close()
    free = sum(1 for c in range(2, fs.nclusters + 2) if fs.fat_get(c) == 0)
    print("[fatput] done: %d of %d clusters free (%d KiB)" % (free, fs.nclusters, free * fs.cl_bytes // 1024))
    return 0


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 1
    if argv[2] == 'ls':
        return edit(argv)
    disk = os.path.abspath(argv[1])
    with open(disk, 'rb') as f:
        original = hashlib.file_digest(f, 'sha256').digest()
        f.seek(0); gpt = is_gpt(f.read(SECTOR))  # GPT disks have no legacy NPFS window to preserve
        f.seek(LIMIT); tail = hashlib.sha256(f.read(DATA_LBA * SECTOR - LIMIT)).digest()
    fd, temp = tempfile.mkstemp(prefix='.fatput-', suffix='.img', dir=os.path.dirname(disk))
    os.close(fd)
    try:
        shutil.copyfile(disk, temp)
        edit([argv[0], temp, *argv[2:]])
        with open(temp, 'rb') as f:
            f.seek(LIMIT)
            if not gpt and hashlib.sha256(f.read(DATA_LBA * SECTOR - LIMIT)).digest() != tail:
                raise ValueError('NPFS/persistence region changed')
        with open(disk, 'rb') as f:
            if hashlib.file_digest(f, 'sha256').digest() != original:
                raise ValueError('Disk changed during installation; retry with QEMU stopped')
        os.replace(temp, disk)
        return 0
    finally:
        if os.path.exists(temp): os.remove(temp)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
