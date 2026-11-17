"""Host-side FAT12/16/32 images for the storage tests: build (VFAT long names, nested directories) and check
(fsck-style: every chain valid, no cross-links, no lost clusters, LFN checksums, FSInfo free count, clean bit),
and read files back. No external tools.

    img = FatImage.create(size_bytes, fat_type, label, sector=512, cluster_sectors=None)
    img.mkdir('SUB'); img.put('SUB/DATA.BIN', data); img.put('Long name.txt', data)
    raw = img.finish()                      # bytes of the volume
    v = FatVolume(raw); v.read('/SUB/DATA.BIN'); errors = v.check()
"""
import struct

ATTR_RO, ATTR_HIDDEN, ATTR_SYSTEM, ATTR_VOLUME, ATTR_DIR, ATTR_ARCHIVE, ATTR_LFN = 1, 2, 4, 8, 16, 32, 15
VALID_SHORT = set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~')
DATE = ((2026 - 1980) << 9) | (10 << 5) | 18
TIME = (12 << 11) | (34 << 5) | 28


def lfn_checksum(name11):
    s = 0
    for b in name11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def short_for(name, taken):
    up = name.upper()
    if '.' in up[1:]:
        base, ext = up.rsplit('.', 1)
    else:
        base, ext = up, ''
    def clean(text):
        return bytes(c if c in VALID_SHORT else ord('_') for c in text.encode('ascii', 'replace') if c not in b' .')
    b, e = clean(base), clean(ext)[:3]
    exact = name == up and len(base) <= 8 and len(ext) <= 3 and b == base.encode() and e == ext.encode() and name.count('.') <= 1
    if exact:
        cand = b.ljust(8) + e.ljust(3)
        if cand not in taken:
            return cand, False
    b = b or b'_'
    for n in range(1, 1000000):
        tail = b'~%d' % n
        cand = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if cand not in taken:
            return cand, True
    raise ValueError('no short name')


class FatImage:
    def __init__(self, size, fat_type, label='NEXUSTEST', sector=512, cluster_sectors=None):
        self.ss, self.type = sector, fat_type
        self.total = size // sector
        if cluster_sectors is None:
            cluster_sectors = 1 if fat_type != 32 else 8
        self.spc = cluster_sectors
        self.reserved = 32 if fat_type == 32 else 1
        self.fats = 2
        self.root_entries = 0 if fat_type == 32 else 512
        root_sectors = self.root_entries * 32 // sector
        fat_sectors = 1
        while True:
            data = self.total - self.reserved - self.fats * fat_sectors - root_sectors
            clusters = data // self.spc
            entry_bits = {12: 12, 16: 16, 32: 32}[fat_type]
            need = ((clusters + 2) * entry_bits + 8 * sector - 1) // (8 * sector)
            if need <= fat_sectors:
                break
            fat_sectors = need
        kind = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
        if kind != fat_type:
            raise ValueError(f'{size} bytes with {cluster_sectors} sectors/cluster make FAT{kind}, not FAT{fat_type}')
        self.fat_sectors, self.clusters, self.root_sectors = fat_sectors, clusters, root_sectors
        self.data_start = self.reserved + self.fats * fat_sectors + root_sectors
        self.fat = [0] * (clusters + 2)
        self.fat[0] = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[fat_type]
        self.fat[1] = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[fat_type]
        self.eoc = self.fat[1]
        self.image = bytearray(self.total * sector)
        self.next = 2
        self.label = label.upper().ljust(11)[:11].encode()
        self.dirs = {(): []}
        self.dir_cluster = {(): self.alloc(1) if fat_type == 32 else 0}
        self.dirs[()].append(self.entry(self.label, ATTR_VOLUME, 0, 0))

    def alloc(self, count):
        first = self.next
        if first + count > self.clusters + 2:
            raise ValueError('volume full')
        for c in range(first, first + count):
            self.fat[c] = c + 1 if c + 1 < first + count else self.eoc
        self.next += count
        return first

    def cluster_offset(self, c):
        return (self.data_start + (c - 2) * self.spc) * self.ss

    def entry(self, name11, attr, cluster, size):
        return struct.pack('<11sBBBHHHHHHHI', name11, attr, 0, 0, TIME, DATE, DATE, cluster >> 16, TIME, DATE, cluster & 0xFFFF, size)

    def names(self, parts, name, attr, cluster, size):
        taken = {e[:11] for e in self.dirs[parts] if e[11] != ATTR_LFN}
        short, need_lfn = short_for(name, taken)
        entries = []
        if need_lfn:
            units = list(name.encode('utf-16-le'))
            units = [units[i] | units[i + 1] << 8 for i in range(0, len(units), 2)]
            count = (len(units) + 12) // 13
            padded = units + [0] + [0xFFFF] * (count * 13 - len(units) - 1) if len(units) % 13 else units
            sum_ = lfn_checksum(short)
            for ord_ in range(count, 0, -1):
                chunk = padded[(ord_ - 1) * 13:ord_ * 13]
                e = bytearray(32)
                e[0] = ord_ | (0x40 if ord_ == count else 0); e[11] = ATTR_LFN; e[13] = sum_
                for i, at in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
                    struct.pack_into('<H', e, at, chunk[i])
                entries.append(bytes(e))
        entries.append(self.entry(short, attr, cluster, size))
        return entries

    def mkdir(self, path):
        parts = tuple(p for p in path.split('/') if p)
        for i in range(1, len(parts) + 1):
            sub = parts[:i]
            if sub in self.dirs:
                continue
            self.dirs[sub] = []
            self.dir_cluster[sub] = self.alloc(4)
            self.dirs[sub[:-1]] += self.names(sub[:-1], sub[-1], ATTR_DIR, self.dir_cluster[sub], 0)

    def put(self, path, data):
        parts = tuple(p for p in path.split('/') if p)
        self.mkdir('/'.join(parts[:-1]))
        cluster_bytes = self.spc * self.ss
        cluster = self.alloc((len(data) + cluster_bytes - 1) // cluster_bytes) if data else 0
        if data:
            at = self.cluster_offset(cluster)
            self.image[at:at + len(data)] = data
        self.dirs[parts[:-1]] += self.names(parts[:-1], parts[-1], ATTR_ARCHIVE, cluster, len(data))

    def finish(self, clean=True):
        cluster_bytes = self.spc * self.ss
        for path, entries in self.dirs.items():
            if path:
                parent = self.dir_cluster[path[:-1]] if path[:-1] and (self.type != 32 or True) else 0
                if not path[:-1]:
                    parent = 0
                blob = self.entry(b'.          ', ATTR_DIR, self.dir_cluster[path], 0) + self.entry(b'..         ', ATTR_DIR, parent, 0) + b''.join(entries)
                limit = 4 * cluster_bytes
                at = self.cluster_offset(self.dir_cluster[path])
            elif self.type == 32:
                blob = b''.join(entries); limit = cluster_bytes; at = self.cluster_offset(self.dir_cluster[()])
            else:
                blob = b''.join(entries); limit = self.root_entries * 32; at = (self.reserved + self.fats * self.fat_sectors) * self.ss
            if len(blob) > limit:
                raise ValueError('directory too large')
            self.image[at:at + len(blob)] = blob
        b = bytearray(self.ss)
        b[0:3] = b'\xEB\x58\x90' if self.type == 32 else b'\xEB\x3C\x90'
        b[3:11] = b'MSWIN4.1'
        total16 = self.total if self.total < 0x10000 and self.type != 32 else 0
        struct.pack_into('<HBHBHHBHHHII', b, 11, self.ss, self.spc, self.reserved, self.fats, self.root_entries, total16, 0xF8,
                         0 if self.type == 32 else self.fat_sectors, 63, 255, 0, 0 if total16 else self.total)
        kind = b'FAT%-5d' % self.type
        if self.type == 32:
            struct.pack_into('<IHHIHH', b, 36, self.fat_sectors, 0, 0, self.dir_cluster[()], 1, 6)
            struct.pack_into('<BBBI11s8s', b, 64, 0x80, 0, 0x29, 0x1234ABCD, self.label, kind)
        else:
            struct.pack_into('<BBBI11s8s', b, 36, 0x80, 0, 0x29, 0x1234ABCD, self.label, kind)
        b[510:512] = b'\x55\xaa'
        self.image[0:self.ss] = b
        if self.type == 32:
            info = bytearray(self.ss)
            struct.pack_into('<I', info, 0, 0x41615252)
            struct.pack_into('<III', info, 484, 0x61417272, self.clusters + 2 - self.next, self.next)
            info[510:512] = b'\x55\xaa'
            self.image[self.ss:2 * self.ss] = info
            self.image[6 * self.ss:7 * self.ss] = b
            self.image[7 * self.ss:8 * self.ss] = info
        if not clean:
            self.fat[1] &= ~(0x8000 if self.type == 16 else 0x08000000)
        fat = bytearray()
        if self.type == 12:
            for i in range(0, len(self.fat), 2):
                a = self.fat[i]; c = self.fat[i + 1] if i + 1 < len(self.fat) else 0
                fat += bytes([a & 0xFF, ((a >> 8) & 0x0F) | ((c & 0x0F) << 4), (c >> 4) & 0xFF])
        else:
            fat = struct.pack('<%d%s' % (len(self.fat), 'H' if self.type == 16 else 'I'), *self.fat)
        for copy in range(self.fats):
            at = (self.reserved + copy * self.fat_sectors) * self.ss
            self.image[at:at + len(fat)] = fat
        return bytes(self.image)

    @staticmethod
    def create(size, fat_type, label='NEXUSTEST', sector=512, cluster_sectors=None):
        return FatImage(size, fat_type, label, sector, cluster_sectors)


class FatVolume:
    """Read-only view of a FAT volume for verification."""
    def __init__(self, raw):
        self.raw = raw
        b = raw[:512]
        self.ss, self.spc, self.reserved, self.fats, self.root_entries = struct.unpack_from('<HBHBH', b, 11)
        total16 = struct.unpack_from('<H', b, 19)[0]; fat16 = struct.unpack_from('<H', b, 22)[0]
        self.total = total16 or struct.unpack_from('<I', b, 32)[0]
        self.fat_sectors = fat16 or struct.unpack_from('<I', b, 36)[0]
        self.root_sectors = (self.root_entries * 32 + self.ss - 1) // self.ss
        self.data_start = self.reserved + self.fats * self.fat_sectors + self.root_sectors
        self.clusters = (self.total - self.data_start) // self.spc
        self.type = 32 if not fat16 else 12 if self.clusters < 4085 else 16
        if self.type == 32:
            entries = self.fat_sectors * self.ss // 4
        elif self.type == 16:
            entries = self.fat_sectors * self.ss // 2
        else:
            entries = self.fat_sectors * self.ss * 2 // 3
        self.clusters = min(self.clusters, entries - 2)
        self.root_cluster = struct.unpack_from('<I', b, 44)[0] if self.type == 32 else 0
        self.eoc_min = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.type]

    def fat_entry(self, c, copy=0):
        base = (self.reserved + copy * self.fat_sectors) * self.ss
        if self.type == 32:
            return struct.unpack_from('<I', self.raw, base + c * 4)[0] & 0x0FFFFFFF
        if self.type == 16:
            return struct.unpack_from('<H', self.raw, base + c * 2)[0]
        pair = struct.unpack_from('<H', self.raw, base + c + c // 2)[0]
        return pair >> 4 if c & 1 else pair & 0xFFF

    def chain(self, first):
        out = []; c = first; seen = set()
        while 2 <= c < self.clusters + 2:
            if c in seen:
                raise ValueError('chain loop at %d' % c)
            seen.add(c); out.append(c)
            n = self.fat_entry(c)
            if n >= self.eoc_min:
                return out
            if n < 2 or n >= self.clusters + 2:
                raise ValueError('chain from %d broken at %d -> %#x' % (first, c, n))
            c = n
        if first:
            raise ValueError('bad first cluster %d' % first)
        return out

    def cluster_data(self, chain):
        cb = self.spc * self.ss
        return b''.join(self.raw[(self.data_start + (c - 2) * self.spc) * self.ss:(self.data_start + (c - 2) * self.spc) * self.ss + cb] for c in chain)

    def entries(self, first):
        if first == 0 and self.type != 32:
            at = (self.reserved + self.fats * self.fat_sectors) * self.ss
            data = self.raw[at:at + self.root_entries * 32]
        else:
            data = self.cluster_data(self.chain(first or self.root_cluster))
        out = []; lfn = {}; lfn_sum = None
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = {}; continue
            if e[11] == ATTR_LFN:
                if e[0] & 0x40:
                    lfn = {}; lfn_sum = e[13]
                if e[13] != lfn_sum:
                    raise ValueError('LFN checksum mismatch inside one name')
                units = [struct.unpack_from('<H', e, at)[0] for at in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)]
                lfn[e[0] & 0x1F] = units
                continue
            if e[11] & ATTR_VOLUME:
                lfn = {}; continue
            name = None
            if lfn:
                if lfn_checksum(e[:11]) != lfn_sum:
                    raise ValueError('orphan LFN entries before %r' % e[:11])
                units = []
                for k in sorted(lfn):
                    units += lfn[k]
                units = units[:units.index(0)] if 0 in units else [u for u in units if u != 0xFFFF]
                name = struct.pack('<%dH' % len(units), *units).decode('utf-16-le')
            if name is None:
                base = e[:8].rstrip(b' ').decode('latin1'); ext = e[8:11].rstrip(b' ').decode('latin1')
                if e[12] & 0x08: base = base.lower()
                if e[12] & 0x10: ext = ext.lower()
                name = base + ('.' + ext if ext else '')
            cluster = (struct.unpack_from('<H', e, 20)[0] << 16 if self.type == 32 else 0) | struct.unpack_from('<H', e, 26)[0]
            out.append((name, e[11], cluster, struct.unpack_from('<I', e, 28)[0], e))
            lfn = {}
        return out

    def find(self, path):
        parts = [p for p in path.split('/') if p]
        first = 0; entry = None
        for p in parts:
            hits = [e for e in self.entries(first) if e[0].lower() == p.lower() and e[0] not in ('.', '..')]
            if not hits:
                return None
            entry = hits[0]; first = entry[2]
        return entry

    def read(self, path):
        e = self.find(path)
        if e is None:
            return None
        if not e[2]:
            return b''
        return self.cluster_data(self.chain(e[2]))[:e[3]]

    def check(self):
        errors = []; owner = {}
        def visit(first, path):
            for name, attr, cluster, size, raw in self.entries(first):
                if name in ('.', '..'):
                    continue
                full = path + '/' + name
                try:
                    chain = self.chain(cluster) if cluster else []
                except ValueError as error:
                    errors.append(f'{full}: {error}'); continue
                for c in chain:
                    if c in owner:
                        errors.append(f'{full}: cluster {c} cross-linked with {owner[c]}')
                    owner[c] = full
                if attr & ATTR_DIR:
                    visit(cluster, full)
                else:
                    cb = self.spc * self.ss
                    if len(chain) != (size + cb - 1) // cb:
                        errors.append(f'{full}: {len(chain)} clusters for {size} bytes')
        try:
            if self.type == 32:
                for c in self.chain(self.root_cluster):
                    owner[c] = '/'
            visit(0, '')
        except ValueError as error:
            errors.append(str(error))
        used = sum(1 for c in range(2, self.clusters + 2) if self.fat_entry(c) != 0)
        lost = [c for c in range(2, self.clusters + 2) if self.fat_entry(c) != 0 and c not in owner]
        if lost:
            errors.append(f'{len(lost)} lost clusters, first {lost[:8]}')
        for c in range(self.clusters + 2):
            if self.fat_entry(c, 0) != self.fat_entry(c, 1):
                errors.append(f'FAT copies differ at {c}'); break
        if self.type != 12:
            clean = self.fat_entry(1) & (0x8000 if self.type == 16 else 0x08000000)
            if not clean:
                errors.append('clean bit not set (volume left dirty)')
        if self.type == 32:
            info = self.raw[self.ss:2 * self.ss]
            free = struct.unpack_from('<I', info, 488)[0]
            if free != 0xFFFFFFFF and free != self.clusters - used:
                errors.append(f'FSInfo free count {free} != {self.clusters - used}')
        return errors
