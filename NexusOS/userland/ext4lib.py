#!/usr/bin/env python3
"""ext4lib.py -- host-side ext4 (read/write/check) for NexusOS disk images.

Used by mkext4.py, ext4put.py, ext4check.py and fat2ext4.py. Implements the subset of ext4 the
NexusOS kernel driver (arch/x86_64/ext4.c) supports, with the same on-disk rules:
  4 KiB blocks; features filetype, extents, 64bit, flex_bg, sparse_super, large_file, huge_file,
  dir_nlink, extra_isize, dir_index (htree), has_journal (jbd2, csum v3) and metadata_csum (crc32c).
Images are either a whole-disk ext4 volume or an MBR disk whose Linux (0x83) partition starts at
LBA >= 32768 (the NexusOS layout: the first 16 MiB hold the boot/legacy/NPFS regions).
No mkfs.ext4/e2fsck is needed on the host.
"""
import os
import struct
import time
import uuid as uuidmod

BLOCK = 4096
SECTOR = 512
PART_LBA = 32768
EXT4_MAGIC = 0xEF53
JBD2_MAGIC = 0xC03B3998

COMPAT_HAS_JOURNAL, COMPAT_EXT_ATTR, COMPAT_RESIZE_INODE, COMPAT_DIR_INDEX = 0x4, 0x8, 0x10, 0x20
COMPAT_ORPHAN_FILE, RO_COMPAT_ORPHAN_PRESENT = 0x1000, 0x10000  # mke2fs >= 1.47 default (Linux orphan file)
INCOMPAT_FILETYPE, INCOMPAT_RECOVER, INCOMPAT_EXTENTS, INCOMPAT_64BIT, INCOMPAT_FLEX_BG = 0x2, 0x4, 0x40, 0x80, 0x200
INCOMPAT_CSUM_SEED = 0x2000
RO_SPARSE_SUPER, RO_LARGE_FILE, RO_HUGE_FILE, RO_DIR_NLINK, RO_EXTRA_ISIZE, RO_METADATA_CSUM = 0x1, 0x2, 0x8, 0x20, 0x40, 0x400
SUPPORTED_INCOMPAT = INCOMPAT_FILETYPE | INCOMPAT_RECOVER | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG | INCOMPAT_CSUM_SEED
SUPPORTED_RO = RO_SPARSE_SUPER | RO_LARGE_FILE | RO_HUGE_FILE | RO_DIR_NLINK | RO_EXTRA_ISIZE | RO_METADATA_CSUM
STATE_VALID, STATE_ERROR, STATE_ORPHAN = 1, 2, 4

EXTENTS_FL, INDEX_FL, HUGE_FILE_FL, INLINE_DATA_FL = 0x80000, 0x1000, 0x40000, 0x10000000
S_IFMT, S_IFSOCK, S_IFLNK, S_IFREG, S_IFBLK, S_IFDIR, S_IFCHR, S_IFIFO = 0o170000, 0o140000, 0o120000, 0o100000, 0o60000, 0o40000, 0o20000, 0o10000
FT = {S_IFREG: 1, S_IFDIR: 2, S_IFCHR: 3, S_IFBLK: 4, S_IFIFO: 5, S_IFSOCK: 6, S_IFLNK: 7}
EXT_MAGIC = 0xF30A
EXT_INIT_MAX = 32768
ROOT_INO, JOURNAL_INO, FIRST_INO, LOST_FOUND_INO = 2, 8, 11, 11
INODE_SIZE = 256
EXTRA_ISIZE = 32


# ---------------------------------------------------------------- crc32c (raw, Linux crc32c_le)
def _tables():
    t0 = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        t0.append(c)
    tabs = [t0]
    for _ in range(7):
        p = tabs[-1]
        tabs.append([(p[i] >> 8) ^ t0[p[i] & 0xFF] for i in range(256)])
    return tabs


_CT = _tables()


def crc32c(crc, data):
    """crc32c without pre/post inversion (seed ~0 gives the ext4/jbd2 convention)."""
    t0, t1, t2, t3, t4, t5, t6, t7 = _CT
    data = bytes(data)
    n8 = len(data) // 8
    if n8:
        for lo, hi in struct.iter_unpack('<II', data[:n8 * 8]):
            lo ^= crc
            crc = (t7[lo & 0xFF] ^ t6[(lo >> 8) & 0xFF] ^ t5[(lo >> 16) & 0xFF] ^ t4[lo >> 24] ^
                   t3[hi & 0xFF] ^ t2[(hi >> 8) & 0xFF] ^ t1[(hi >> 16) & 0xFF] ^ t0[hi >> 24])
    for b in data[n8 * 8:]:
        crc = t0[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


assert crc32c(0xFFFFFFFF, b'123456789') ^ 0xFFFFFFFF == 0xE3069283


# ---------------------------------------------------------------- directory hashes
def _u32(x):
    return x & 0xFFFFFFFF


def _str2hashbuf(name, length, num, signed):
    pad = _u32(length | (length << 8))
    pad = _u32(pad | (pad << 16))
    val = pad
    out = []
    if length > num * 4:
        length = num * 4
    for i in range(length):
        c = name[i]
        if signed and c >= 128:
            c -= 256
        val = _u32(c + (val << 8))
        if i % 4 == 3:
            out.append(val)
            val = pad
            num -= 1
    num -= 1
    if num >= 0:
        out.append(val)
    while True:
        num -= 1
        if num < 0:
            break
        out.append(pad)
    return out


def _rol(x, s):
    x &= 0xFFFFFFFF
    return ((x << s) | (x >> (32 - s))) & 0xFFFFFFFF


def _half_md4(buf, inp):
    a, b, c, d = buf
    F = lambda x, y, z: z ^ (x & (y ^ z))
    G = lambda x, y, z: _u32((x & y) + ((x ^ y) & z))
    H = lambda x, y, z: x ^ y ^ z
    K2, K3 = 0x5A827999, 0x6ED9EBA1

    def R(f, a, b, c, d, x, s):
        return _rol(_u32(a + f(b, c, d) + x), s)
    a = R(F, a, b, c, d, inp[0], 3); d = R(F, d, a, b, c, inp[1], 7); c = R(F, c, d, a, b, inp[2], 11); b = R(F, b, c, d, a, inp[3], 19)
    a = R(F, a, b, c, d, inp[4], 3); d = R(F, d, a, b, c, inp[5], 7); c = R(F, c, d, a, b, inp[6], 11); b = R(F, b, c, d, a, inp[7], 19)
    a = R(G, a, b, c, d, _u32(inp[1] + K2), 3); d = R(G, d, a, b, c, _u32(inp[3] + K2), 5)
    c = R(G, c, d, a, b, _u32(inp[5] + K2), 9); b = R(G, b, c, d, a, _u32(inp[7] + K2), 13)
    a = R(G, a, b, c, d, _u32(inp[0] + K2), 3); d = R(G, d, a, b, c, _u32(inp[2] + K2), 5)
    c = R(G, c, d, a, b, _u32(inp[4] + K2), 9); b = R(G, b, c, d, a, _u32(inp[6] + K2), 13)
    a = R(H, a, b, c, d, _u32(inp[3] + K3), 3); d = R(H, d, a, b, c, _u32(inp[7] + K3), 9)
    c = R(H, c, d, a, b, _u32(inp[2] + K3), 11); b = R(H, b, c, d, a, _u32(inp[6] + K3), 15)
    a = R(H, a, b, c, d, _u32(inp[1] + K3), 3); d = R(H, d, a, b, c, _u32(inp[5] + K3), 9)
    c = R(H, c, d, a, b, _u32(inp[0] + K3), 11); b = R(H, b, c, d, a, _u32(inp[4] + K3), 15)
    return [_u32(buf[0] + a), _u32(buf[1] + b), _u32(buf[2] + c), _u32(buf[3] + d)]


def _tea(buf, inp):
    s, b0, b1 = 0, buf[0], buf[1]
    a, b, c, d = inp[:4]
    for _ in range(16):
        s = _u32(s + 0x9E3779B9)
        b0 = _u32(b0 + (_u32((b1 << 4) + a) ^ _u32(b1 + s) ^ _u32((b1 >> 5) + b)))
        b1 = _u32(b1 + (_u32((b0 << 4) + c) ^ _u32(b0 + s) ^ _u32((b0 >> 5) + d)))
    return [_u32(buf[0] + b0), _u32(buf[1] + b1), buf[2], buf[3]]


def _legacy(name, signed):
    h0, h1 = 0x12a3fe2d, 0x37abe8f9
    for c in name:
        if signed and c >= 128:
            c -= 256
        h = _u32(h1 + (h0 ^ _u32(c * 7152373)))
        if h & 0x80000000:
            h = _u32(h - 0x7fffffff)
        h1, h0 = h0, h
    return _u32(h0 << 1)


def dirhash(name, version, seed=None):
    """ext4fs_dirhash: version 0..5 (legacy, half_md4, tea, + unsigned variants). Returns (major, minor)."""
    buf = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    if seed and any(seed):
        buf = list(seed)
    signed = version < 3
    v = version % 3
    minor = 0
    if v == 0:
        h = _legacy(name, signed)
    elif v == 1:
        p, n = 0, len(name)
        while n > 0:
            buf = _half_md4(buf, _str2hashbuf(name[p:], n, 8, signed))
            n -= 32; p += 32
        h, minor = buf[1], buf[2]
    else:
        p, n = 0, len(name)
        while n > 0:
            buf = _tea(buf, _str2hashbuf(name[p:], n, 4, signed))
            n -= 16; p += 16
        h, minor = buf[0], buf[1]
    h &= ~1 & 0xFFFFFFFF
    if h == (0x7fffffff << 1):
        h = (0x7fffffff - 1) << 1
    return h, minor


# ---------------------------------------------------------------- raw records
class Rec:
    """Little-endian field accessors over a bytearray slice."""
    FIELDS = {}

    def __init__(self, raw):
        self.raw = bytearray(raw)

    def __getattr__(self, name):
        f = type(self).FIELDS.get(name)
        if f is None:
            raise AttributeError(name)
        if isinstance(f, tuple) and isinstance(f[0], tuple):
            (fl, ol), (fh, oh) = f
            lo = struct.unpack_from('<' + fl, self.raw, ol)[0]
            hi = struct.unpack_from('<' + fh, self.raw, oh)[0] if oh < len(self.raw) else 0
            return lo | (hi << (8 * struct.calcsize(fl)))
        fmt, off = f
        v = struct.unpack_from('<' + fmt, self.raw, off)
        return v[0]

    def __setattr__(self, name, value):
        if name == 'raw':
            return object.__setattr__(self, name, value)
        f = type(self).FIELDS.get(name)
        if f is None:
            return object.__setattr__(self, name, value)
        if isinstance(f, tuple) and isinstance(f[0], tuple):
            (fl, ol), (fh, oh) = f
            bits = 8 * struct.calcsize(fl)
            struct.pack_into('<' + fl, self.raw, ol, value & ((1 << bits) - 1))
            if oh < len(self.raw):
                struct.pack_into('<' + fh, self.raw, oh, value >> bits)
            elif value >> bits:
                raise ValueError('field overflow ' + name)
            return
        fmt, off = f
        struct.pack_into('<' + fmt, self.raw, off, value)


class Super(Rec):
    FIELDS = {
        'inodes_count': ('I', 0x0), 'blocks_count': (('I', 0x4), ('I', 0x150)),
        'r_blocks_count': (('I', 0x8), ('I', 0x154)), 'free_blocks': (('I', 0xC), ('I', 0x158)),
        'free_inodes': ('I', 0x10), 'first_data_block': ('I', 0x14), 'log_block_size': ('I', 0x18),
        'log_cluster_size': ('I', 0x1C), 'blocks_per_group': ('I', 0x20), 'clusters_per_group': ('I', 0x24),
        'inodes_per_group': ('I', 0x28), 'mtime': ('I', 0x2C), 'wtime': ('I', 0x30), 'mnt_count': ('H', 0x34),
        'max_mnt_count': ('H', 0x36), 'magic': ('H', 0x38), 'state': ('H', 0x3A), 'errors': ('H', 0x3C),
        'minor_rev': ('H', 0x3E), 'lastcheck': ('I', 0x40), 'checkinterval': ('I', 0x44), 'creator_os': ('I', 0x48),
        'rev_level': ('I', 0x4C), 'def_resuid': ('H', 0x50), 'def_resgid': ('H', 0x52), 'first_ino': ('I', 0x54),
        'inode_size': ('H', 0x58), 'block_group_nr': ('H', 0x5A), 'compat': ('I', 0x5C), 'incompat': ('I', 0x60),
        'ro_compat': ('I', 0x64), 'uuid': ('16s', 0x68), 'volume_name': ('16s', 0x78), 'last_mounted': ('64s', 0x88),
        'reserved_gdt_blocks': ('H', 0xCE), 'journal_uuid': ('16s', 0xD0), 'journal_inum': ('I', 0xE0),
        'journal_dev': ('I', 0xE4), 'last_orphan': ('I', 0xE8), 'hash_seed': ('16s', 0xEC),
        'def_hash_version': ('B', 0xFC), 'jnl_backup_type': ('B', 0xFD), 'desc_size': ('H', 0xFE),
        'default_mount_opts': ('I', 0x100), 'first_meta_bg': ('I', 0x104), 'mkfs_time': ('I', 0x108),
        'jnl_blocks': ('68s', 0x10C), 'min_extra_isize': ('H', 0x15C), 'want_extra_isize': ('H', 0x15E),
        'flags': ('I', 0x160), 'log_groups_per_flex': ('B', 0x174), 'checksum_type': ('B', 0x175),
        'kbytes_written': ('Q', 0x178), 'error_count': ('I', 0x194), 'lpf_ino': ('I', 0x268),
        'checksum_seed': ('I', 0x270), 'orphan_file_inum': ('I', 0x280), 'checksum': ('I', 0x3FC),
    }


class Desc(Rec):
    FIELDS = {
        'block_bitmap': (('I', 0x0), ('I', 0x20)), 'inode_bitmap': (('I', 0x4), ('I', 0x24)),
        'inode_table': (('I', 0x8), ('I', 0x28)), 'free_blocks': (('H', 0xC), ('H', 0x2C)),
        'free_inodes': (('H', 0xE), ('H', 0x2E)), 'used_dirs': (('H', 0x10), ('H', 0x30)), 'flags': ('H', 0x12),
        'exclude_bitmap': (('I', 0x14), ('I', 0x34)), 'block_bitmap_csum': (('H', 0x18), ('H', 0x38)),
        'inode_bitmap_csum': (('H', 0x1A), ('H', 0x3A)), 'itable_unused': (('H', 0x1C), ('H', 0x32)),
        'checksum': ('H', 0x1E),
    }


class Inode(Rec):
    FIELDS = {
        'mode': ('H', 0x0), 'uid': (('H', 0x2), ('H', 0x78)), 'size': (('I', 0x4), ('I', 0x6C)),
        'atime': ('I', 0x8), 'ctime': ('I', 0xC), 'mtime': ('I', 0x10), 'dtime': ('I', 0x14),
        'gid': (('H', 0x18), ('H', 0x7A)), 'links': ('H', 0x1A), 'blocks': (('I', 0x1C), ('H', 0x74)),
        'flags': ('I', 0x20), 'version': ('I', 0x24), 'block': ('60s', 0x28), 'generation': ('I', 0x64),
        'file_acl': (('I', 0x68), ('H', 0x76)), 'checksum_lo': ('H', 0x7C), 'extra_isize': ('H', 0x80),
        'checksum_hi': ('H', 0x82), 'ctime_extra': ('I', 0x84), 'mtime_extra': ('I', 0x88),
        'atime_extra': ('I', 0x8C), 'crtime': ('I', 0x90), 'crtime_extra': ('I', 0x94),
    }

    def set_time(self, which, seconds_ns):
        sec, ns = divmod(int(seconds_ns), 1_000_000_000)
        low = sec & 0xFFFFFFFF
        signed_low = low - (1 << 32) if low & 0x80000000 else low
        epoch = ((sec - signed_low) >> 32) & 3
        setattr(self, which, low)
        setattr(self, which + '_extra', (ns << 2) | epoch)

    def get_time(self, which):
        sec = getattr(self, which)
        if sec & 0x80000000:
            sec -= 1 << 32
        extra = getattr(self, which + '_extra')
        return (sec + ((extra & 3) << 32)) * 1_000_000_000 + (extra >> 2)


# ---------------------------------------------------------------- image access
def find_partition(f, disk_size):
    """(byte offset, byte length) of the ext4 volume: whole disk, or the MBR Linux partition."""
    f.seek(1024)
    if struct.unpack('<H', f.read(0x3A)[0x38:0x3A])[0] == EXT4_MAGIC:
        return 0, disk_size
    f.seek(0)
    mbr = f.read(512)
    if mbr[510:512] == b'\x55\xaa':
        for off in range(446, 510, 16):
            kind = mbr[off + 4]
            start, count = struct.unpack_from('<II', mbr, off + 8)
            if kind == 0x83 and start >= PART_LBA and count and (start + count) * SECTOR <= disk_size:
                return start * SECTOR, count * SECTOR
    raise ValueError('no ext4 volume (whole disk or MBR type 0x83 partition at LBA >= %d)' % PART_LBA)


def has_super(group):
    if group <= 1:
        return True
    for base in (3, 5, 7):
        g = group
        while g % base == 0:
            g //= base
        if g == 1:
            return True
    return False


class Ext4:
    """An ext4 volume inside an image file. Metadata edits go straight to the image (offline use);
    bitmaps/descriptors/superblock counts are flushed by close()."""

    def __init__(self, path, writable=False, overlay=None, base=None, length=None):
        self.path = path
        self.writable = writable
        self.f = open(path, 'r+b' if writable else 'rb')
        self.f.seek(0, 2)
        size = self.f.tell()
        if base is None:
            base, length = find_partition(self.f, size)
        self.base, self.length = base, length
        self.overlay = overlay if overlay is not None else {}
        # a replayed journal may carry block 0: the superblock must come from the same view as the rest
        raw = bytes(self.overlay[0][1024:2048]) if 0 in self.overlay else self._read_raw(1024, 1024)
        self.sb = Super(raw)
        sb = self.sb
        if sb.magic != EXT4_MAGIC:
            raise ValueError('bad ext4 magic')
        self.bs = 1024 << sb.log_block_size
        if self.bs != BLOCK:
            raise ValueError('only 4 KiB blocks are supported')
        self.metadata_csum = bool(sb.ro_compat & RO_METADATA_CSUM)
        if sb.incompat & INCOMPAT_CSUM_SEED:
            self.seed = sb.checksum_seed
        else:
            self.seed = crc32c(0xFFFFFFFF, sb.uuid)
        self.desc_size = sb.desc_size if sb.incompat & INCOMPAT_64BIT else 32
        self.ipg, self.bpg = sb.inodes_per_group, sb.blocks_per_group
        self.isize = sb.inode_size
        self.blocks = sb.blocks_count
        self.ngroups = (self.blocks - sb.first_data_block + self.bpg - 1) // self.bpg
        self.gdt_blocks = (self.ngroups * self.desc_size + BLOCK - 1) // BLOCK
        gdt = b''.join(self.rb(sb.first_data_block + 1 + i) for i in range(self.gdt_blocks))
        self.gds = [Desc(gdt[i * self.desc_size:(i + 1) * self.desc_size]) for i in range(self.ngroups)]
        self.bitmaps = {}
        self.ibitmaps = {}
        self.dirty_groups = set()
        self.hash_seed = struct.unpack('<4I', sb.hash_seed)
        self.hash_unsigned = bool(sb.flags & 2)

    # raw I/O ---------------------------------------------------------------
    def _read_raw(self, off, n):
        self.f.seek(self.base + off)
        d = self.f.read(n)
        if len(d) != n:
            d += bytes(n - len(d))
        return d

    def rb(self, n):
        if n in self.overlay:
            return bytes(self.overlay[n])
        if n >= self.blocks:
            raise ValueError('block %d beyond the volume' % n)
        return self._read_raw(n * BLOCK, BLOCK)

    def wb(self, n, data):
        assert self.writable and len(data) == BLOCK
        if n >= self.blocks:
            raise ValueError('write beyond the volume')
        if n in self.overlay:
            self.overlay[n] = bytes(data)
        self.f.seek(self.base + n * BLOCK)
        self.f.write(data)

    def read_run(self, block, count):
        if any(b in self.overlay for b in range(block, block + count)):
            return b''.join(self.rb(b) for b in range(block, block + count))
        return self._read_raw(block * BLOCK, count * BLOCK)

    def write_run(self, block, data):
        assert self.writable and len(data) % BLOCK == 0
        if block + len(data) // BLOCK > self.blocks:
            raise ValueError('write beyond the volume')
        self.f.seek(self.base + block * BLOCK)
        self.f.write(data)

    # checksums -------------------------------------------------------------
    def sb_csum(self, raw):
        return crc32c(0xFFFFFFFF, raw[:0x3FC])

    def gd_csum(self, group, raw):
        c = crc32c(self.seed, struct.pack('<I', group))
        c = crc32c(c, raw[:0x1E])
        c = crc32c(c, b'\0\0')
        c = crc32c(c, raw[0x20:self.desc_size])
        return c & 0xFFFF

    def inode_seed(self, ino, generation):
        return crc32c(crc32c(self.seed, struct.pack('<I', ino)), struct.pack('<I', generation))

    def inode_csum(self, ino, raw):
        inode = Inode(raw)
        r = bytearray(raw)
        r[0x7C:0x7E] = b'\0\0'
        has_hi = self.isize > 128 and 128 + inode.extra_isize >= 0x84
        if has_hi:
            r[0x82:0x84] = b'\0\0'
        c = crc32c(self.inode_seed(ino, inode.generation), bytes(r[:self.isize]))
        return c if has_hi else c & 0xFFFF

    def bitmap_csum(self, data, nbytes):
        return crc32c(self.seed, data[:nbytes])

    def extent_csum(self, ino, generation, block):
        emax = struct.unpack_from('<H', block, 4)[0]
        return crc32c(self.inode_seed(ino, generation), block[:12 + 12 * emax])

    def dirent_csum(self, ino, generation, block):
        return crc32c(self.inode_seed(ino, generation), block[:BLOCK - 12])

    def dx_csum(self, ino, generation, block, count_offset):
        limit, count = struct.unpack_from('<HH', block, count_offset)
        tail = count_offset + limit * 8
        c = crc32c(self.inode_seed(ino, generation), block[:count_offset + count * 8])
        return crc32c(c, bytes(block[tail:tail + 4]) + bytes(4)), tail  # tail hashed with dt_checksum = 0

    # groups ----------------------------------------------------------------
    def group_first(self, g):
        return self.sb.first_data_block + g * self.bpg

    def group_blocks(self, g):
        return min(self.bpg, self.blocks - self.group_first(g))

    def block_bitmap(self, g):
        if g not in self.bitmaps:
            self.bitmaps[g] = bytearray(self.rb(self.gds[g].block_bitmap))
        return self.bitmaps[g]

    def inode_bitmap(self, g):
        if g not in self.ibitmaps:
            self.ibitmaps[g] = bytearray(self.rb(self.gds[g].inode_bitmap))
        return self.ibitmaps[g]

    # inodes ----------------------------------------------------------------
    def inode_loc(self, ino):
        g, i = divmod(ino - 1, self.ipg)
        off = i * self.isize
        return self.gds[g].inode_table + off // BLOCK, off % BLOCK

    def read_inode(self, ino):
        blk, off = self.inode_loc(ino)
        return Inode(self.rb(blk)[off:off + self.isize])

    def write_inode(self, ino, inode):
        if self.metadata_csum:
            c = self.inode_csum(ino, inode.raw)
            inode.checksum_lo = c & 0xFFFF
            if self.isize > 128 and 128 + inode.extra_isize >= 0x84:
                inode.checksum_hi = c >> 16
        blk, off = self.inode_loc(ino)
        data = bytearray(self.rb(blk))
        data[off:off + self.isize] = inode.raw[:self.isize]
        self.wb(blk, bytes(data))

    # extents ---------------------------------------------------------------
    def extents(self, inode, tree_blocks=None):
        """[(logical, physical, length, uninit)] of an extent-mapped inode; tree_blocks collects index/leaf blocks."""
        out = []

        def walk(node, depth_expected=None):
            magic, entries, emax, depth = struct.unpack_from('<HHHH', node, 0)
            if magic != EXT_MAGIC:
                raise ValueError('bad extent header')
            for i in range(entries):
                o = 12 + 12 * i
                if depth == 0:
                    lblk, ln, hi, lo = struct.unpack_from('<IHHI', node, o)
                    uninit = ln > EXT_INIT_MAX
                    if uninit:
                        ln -= EXT_INIT_MAX
                    out.append((lblk, (hi << 32) | lo, ln, uninit))
                else:
                    lblk, lo, hi = struct.unpack_from('<IIH', node, o)
                    child = (hi << 32) | lo
                    if tree_blocks is not None:
                        tree_blocks.append(child)
                    walk(self.rb(child))
        walk(inode.block)
        return out

    def file_blocks(self, inode):
        """Map logical->physical for a file; returns sorted extents."""
        if inode.flags & EXTENTS_FL:
            return self.extents(inode)
        if inode.flags & INLINE_DATA_FL:
            raise ValueError('inline data is not supported')
        # Legacy block map (read only).
        out = []
        ptrs = struct.unpack('<15I', inode.block)
        nblocks = (inode.size + BLOCK - 1) // BLOCK
        per = BLOCK // 4

        def ind(blk, level, base):
            if not blk:
                return
            vals = struct.unpack('<%dI' % per, self.rb(blk))
            span = per ** (level - 1)
            for i, v in enumerate(vals):
                if level == 1:
                    if v:
                        out.append((base + i, v, 1, False))
                else:
                    ind(v, level - 1, base + i * span)
        for i in range(12):
            if ptrs[i] and i < nblocks:
                out.append((i, ptrs[i], 1, False))
        ind(ptrs[12], 1, 12)
        ind(ptrs[13], 2, 12 + per)
        ind(ptrs[14], 3, 12 + per + per * per)
        return out

    def read_file(self, inode, limit=None):
        size = inode.size if limit is None else min(limit, inode.size)
        if (inode.mode & S_IFMT) == S_IFLNK and self.is_fast_symlink(inode):
            return bytes(inode.block[:inode.size])
        data = bytearray(size)
        for lblk, pblk, ln, uninit in self.file_blocks(inode):
            start = lblk * BLOCK
            if start >= size or uninit:
                continue
            n = min(ln * BLOCK, size - start)
            chunk = self.read_run(pblk, (n + BLOCK - 1) // BLOCK)[:n]
            data[start:start + n] = chunk
        return bytes(data)

    def is_fast_symlink(self, inode):
        ea = 8 if inode.file_acl else 0
        return (inode.mode & S_IFMT) == S_IFLNK and not (inode.flags & INLINE_DATA_FL) and inode.blocks - ea == 0

    # directories -----------------------------------------------------------
    def dir_entries(self, ino, inode=None):
        """Linear walk: [(name bytes, ino, ftype)] including . and .."""
        inode = inode or self.read_inode(ino)
        out = []
        for lblk, pblk, ln, uninit in self.file_blocks(inode):
            for k in range(ln):
                blk = self.rb(pblk + k)
                off = 0
                while off < BLOCK - 8:
                    dino, rec, nl, ft = struct.unpack_from('<IHBB', blk, off)
                    if rec < 8 or off + rec > BLOCK:
                        raise ValueError('corrupt directory %d' % ino)
                    if dino and not (nl == 0 and ft == 0xDE):
                        out.append((bytes(blk[off + 8:off + 8 + nl]), dino, ft))
                    off += rec
        return out

    def lookup(self, path):
        ino = ROOT_INO
        for part in [p for p in path.split('/') if p and p != '.']:
            inode = self.read_inode(ino)
            if (inode.mode & S_IFMT) != S_IFDIR:
                return None
            name = part.encode()
            found = None
            for n, d, ft in self.dir_entries(ino, inode):
                if n == name:
                    found = d
                    break
            if found is None:
                return None
            ino = found
        return ino

    def bitmap_test(self, bm, i):
        return (bm[i >> 3] >> (i & 7)) & 1

    def close(self):
        self.f.close()


# ---------------------------------------------------------------- writer
class Writer(Ext4):
    """Offline editor: allocation, files, directories (linear or htree), symlinks, device nodes."""

    def __init__(self, path, base=None, length=None):
        super().__init__(path, writable=True, base=base, length=length)
        sb = self.sb
        if sb.incompat & ~SUPPORTED_INCOMPAT or sb.ro_compat & ~SUPPORTED_RO:
            raise ValueError('unsupported ext4 features for writing')
        if sb.incompat & INCOMPAT_RECOVER:
            js = self.journal_super()
            if js and js['start']:
                raise ValueError('journal needs recovery: boot NexusOS or run "ext4check.py --replay" first')
        self.next_goal = 0
        self.gen = int.from_bytes(os.urandom(4), 'little') or 1
        # Mark "in use" so an interrupted edit is visible to the checker / kernel.
        sb.state = sb.state & ~STATE_VALID
        self.flush_super()

    # allocation ------------------------------------------------------------
    def alloc_blocks(self, want, goal=None):
        """Allocate a contiguous run of up to `want` blocks near `goal`. Returns (start, count)."""
        if goal is None:
            goal = self.next_goal
        g0 = max(0, min(self.ngroups - 1, (goal - self.sb.first_data_block) // self.bpg))
        for pass_ in range(2):
            for k in range(self.ngroups):
                g = (g0 + k) % self.ngroups
                if not self.gds[g].free_blocks:
                    continue
                bm = self.block_bitmap(g)
                n = self.group_blocks(g)
                start_i = max(0, goal - self.group_first(g)) if g == g0 and pass_ == 0 else 0
                i = self._find_zero(bm, start_i, n)
                if i < 0 and start_i:
                    i = self._find_zero(bm, 0, n)
                if i < 0:
                    continue
                j = i
                while j < n and j - i < want and not self.bitmap_test(bm, j):
                    j += 1
                for b in range(i, j):
                    bm[b >> 3] |= 1 << (b & 7)
                cnt = j - i
                self.gds[g].free_blocks -= cnt
                self.sb.free_blocks = self.sb.free_blocks - cnt
                self.dirty_groups.add(g)
                start = self.group_first(g) + i
                self.next_goal = start + cnt
                return start, cnt
        raise OSError(28, 'ext4 volume full')

    @staticmethod
    def _find_zero(bm, start, n):
        i = start
        while i < n:
            byte = bm[i >> 3]
            if byte == 0xFF and (i & 7) == 0:
                i += 8
                continue
            if not (byte >> (i & 7)) & 1:
                return i
            i += 1
        return -1

    def free_blocks(self, start, count):
        while count:
            g = (start - self.sb.first_data_block) // self.bpg
            i = start - self.group_first(g)
            n = min(count, self.group_blocks(g) - i)
            bm = self.block_bitmap(g)
            for b in range(i, i + n):
                if not self.bitmap_test(bm, b):
                    raise ValueError('double free of block %d' % (self.group_first(g) + b))
                bm[b >> 3] &= ~(1 << (b & 7))
            self.gds[g].free_blocks += n
            self.sb.free_blocks = self.sb.free_blocks + n
            self.dirty_groups.add(g)
            start += n
            count -= n

    def alloc_inode(self, is_dir, parent=ROOT_INO, group=None):
        pg = (parent - 1) // self.ipg
        order = list(range(pg, self.ngroups)) + list(range(0, pg))
        if group is not None:
            order = [group]
        elif is_dir:  # spread directories: least used group with free inodes
            order.sort(key=lambda g: (self.gds[g].used_dirs, -self.gds[g].free_inodes))
        for g in order:
            if not self.gds[g].free_inodes:
                continue
            bm = self.inode_bitmap(g)
            first = self.sb.first_ino if g == 0 else 0
            i = self._find_zero(bm, 0, self.ipg)
            while 0 <= i < first - (1 if g == 0 else 0):
                i = self._find_zero(bm, i + 1, self.ipg)
            if i < 0:
                continue
            bm[i >> 3] |= 1 << (i & 7)
            self.gds[g].free_inodes -= 1
            if is_dir:
                self.gds[g].used_dirs += 1
            self.sb.free_inodes -= 1
            self.dirty_groups.add(g)
            return g * self.ipg + i + 1
        raise OSError(28, 'no free inodes')

    def free_inode(self, ino, is_dir):
        g, i = divmod(ino - 1, self.ipg)
        bm = self.inode_bitmap(g)
        if not self.bitmap_test(bm, i):
            raise ValueError('double free of inode %d' % ino)
        bm[i >> 3] &= ~(1 << (i & 7))
        self.gds[g].free_inodes += 1
        if is_dir:
            self.gds[g].used_dirs -= 1
        self.sb.free_inodes += 1
        self.dirty_groups.add(g)

    # inode creation ----------------------------------------------------------
    def new_inode(self, mode, uid=0, gid=0, times_ns=None):
        inode = Inode(bytes(self.isize))
        inode.mode = mode
        inode.uid = uid
        inode.gid = gid
        inode.extra_isize = EXTRA_ISIZE
        self.gen = (self.gen * 1103515245 + 12345) & 0xFFFFFFFF
        inode.generation = self.gen
        t = times_ns if times_ns is not None else time.time_ns()
        for w in ('atime', 'ctime', 'mtime', 'crtime'):
            inode.set_time(w, t)
        return inode

    def build_extent_tree(self, ino, inode, extents):
        """Write `extents` [(lblk, pblk, len)] into inode (depth 0..2); returns number of tree blocks."""
        if len(extents) <= 4:
            inode.block = self._extent_node(extents, 4, 0, leaf=True)[:60]
            return 0
        per = (BLOCK - 12) // 12
        level = [(e[0], e) for e in extents]
        depth, tree = 0, 0
        nodes = [('leaf', extents)]
        entries = extents
        leafy = True
        while len(entries) > 4:
            parents = []
            for k in range(0, len(entries), per):
                chunk = entries[k:k + per]
                blk, _ = self.alloc_blocks(1, self.next_goal)
                tree += 1
                node = bytearray(self._extent_node(chunk, per, depth, leaf=leafy))
                if self.metadata_csum:
                    struct.pack_into('<I', node, 12 + 12 * per, self.extent_csum(ino, inode.generation, node))
                self.wb(blk, bytes(node))
                parents.append((chunk[0][0], blk))
            entries = parents
            leafy = False
            depth += 1
        inode.block = self._extent_node(entries, 4, depth, leaf=False)[:60]
        return tree

    @staticmethod
    def _extent_node(entries, emax, depth, leaf):
        size = 12 + 12 * emax + (4 if emax != 4 else 0)
        node = bytearray(max(size, 60) if emax == 4 else BLOCK)
        struct.pack_into('<HHHHI', node, 0, EXT_MAGIC, len(entries), emax, depth, 0)
        for i, e in enumerate(entries):
            o = 12 + 12 * i
            if leaf:
                lblk, pblk, ln = e[:3]
                uninit = len(e) > 3 and e[3]
                struct.pack_into('<IHHI', node, o, lblk, ln + (EXT_INIT_MAX if uninit else 0), pblk >> 32, pblk & 0xFFFFFFFF)
            else:
                lblk, child = e
                struct.pack_into('<IIHH', node, o, lblk, child & 0xFFFFFFFF, child >> 32, 0)
        return bytes(node)

    def write_data(self, chunks, size):
        """Allocate and write file content (all blocks written: freed blocks may hold stale data).
        Returns the extent list [(logical, physical, length)]."""
        extents = []
        nblocks = (size + BLOCK - 1) // BLOCK
        buf = bytearray()
        it = iter(chunks)
        eof = False
        lblk = 0
        while lblk < nblocks:
            start, cnt = self.alloc_blocks(min(nblocks - lblk, EXT_INIT_MAX))
            done = 0
            while done < cnt:
                want = min(cnt - done, 2048) * BLOCK
                while len(buf) < want and not eof:
                    try:
                        buf += next(it)
                    except StopIteration:
                        eof = True
                piece = bytes(buf[:want])
                del buf[:want]
                self.write_run(start + done, piece.ljust(want, b'\0'))
                done += want // BLOCK
            if extents and extents[-1][1] + extents[-1][2] == start and extents[-1][2] + cnt <= EXT_INIT_MAX:
                extents[-1] = (extents[-1][0], extents[-1][1], extents[-1][2] + cnt)
            else:
                extents.append((lblk, start, cnt))
            lblk += cnt
        return extents

    def make_file(self, data_or_iter, size, mode, uid=0, gid=0, times_ns=None, parent=ROOT_INO):
        ino = self.alloc_inode(False, parent)
        inode = self.new_inode(S_IFREG | (mode & 0o7777), uid, gid, times_ns)
        if isinstance(data_or_iter, (bytes, bytearray)):
            data_or_iter = [bytes(data_or_iter)]
        ext = self.write_data(data_or_iter, size)
        tree = self.build_extent_tree(ino, inode, ext)
        inode.flags = EXTENTS_FL
        inode.size = size
        inode.links = 1
        inode.blocks = (sum(e[2] for e in ext) + tree) * (BLOCK // 512)
        self.write_inode(ino, inode)
        return ino

    def make_symlink(self, target, uid=0, gid=0, times_ns=None, parent=ROOT_INO):
        target = target.encode() if isinstance(target, str) else target
        ino = self.alloc_inode(False, parent)
        inode = self.new_inode(S_IFLNK | 0o777, uid, gid, times_ns)
        inode.links = 1
        inode.size = len(target)
        if len(target) < 60:
            inode.block = target.ljust(60, b'\0')
        else:
            ext = self.write_data([target], len(target))
            self.build_extent_tree(ino, inode, ext)
            inode.flags = EXTENTS_FL
            inode.blocks = sum(e[2] for e in ext) * 8
        self.write_inode(ino, inode)
        return ino

    def make_node(self, mode, rdev=0, uid=0, gid=0, times_ns=None, parent=ROOT_INO):
        ino = self.alloc_inode(False, parent)
        inode = self.new_inode(mode, uid, gid, times_ns)
        inode.links = 1
        if (mode & S_IFMT) in (S_IFCHR, S_IFBLK):
            major, minor = rdev >> 8, rdev & 0xFF
            if major < 256 and minor < 256:
                inode.block = struct.pack('<I', (major << 8) | minor).ljust(60, b'\0')
            else:
                inode.block = struct.pack('<II', 0, (minor & 0xFF) | (major << 8) | ((minor & ~0xFF) << 12)).ljust(60, b'\0')
        self.write_inode(ino, inode)
        return ino

    # directories -------------------------------------------------------------
    @staticmethod
    def rec_len(name_len):
        return (8 + name_len + 3) & ~3

    def _leaf_block(self, dir_ino, gen, entries):
        """Pack entries [(name, ino, ft)] into one leaf block (entries must fit)."""
        blk = bytearray(BLOCK)
        end = BLOCK - (12 if self.metadata_csum else 0)
        off = 0
        for i, (name, ino, ft) in enumerate(entries):
            rl = self.rec_len(len(name))
            if i == len(entries) - 1:
                rl = end - off
            struct.pack_into('<IHBB', blk, off, ino, rl, len(name), ft)
            blk[off + 8:off + 8 + len(name)] = name
            off += rl
        if not entries:
            struct.pack_into('<IHBB', blk, 0, 0, end, 0, 0)
        if self.metadata_csum:
            struct.pack_into('<IHBB', blk, BLOCK - 12, 0, 12, 0, 0xDE)
            struct.pack_into('<I', blk, BLOCK - 4, self.dirent_csum(dir_ino, gen, blk))
        return bytes(blk)

    def _hash(self, name):
        return dirhash(name, 1 + (3 if self.hash_unsigned else 0), self.hash_seed)

    def build_directory(self, dir_ino, inode, parent_ino, entries):
        """Lay out a directory from scratch. entries excludes '.'/'..'. Returns block list written."""
        dots = [(b'.', dir_ino, 2), (b'..', parent_ino, 2)]
        end = BLOCK - (12 if self.metadata_csum else 0)
        total = sum(self.rec_len(len(n)) for n, _, _ in dots + entries)
        gen = inode.generation
        if total <= end:
            blocks = [self._leaf_block(dir_ino, gen, dots + entries)]
            inode.flags = EXTENTS_FL
        else:
            hashed = sorted(((self._hash(n)[0], self._hash(n)[1], n, i, f) for n, i, f in entries), key=lambda x: (x[0], x[1]))
            leaves, cur, used, starts = [], [], 0, []
            fill = end * 3 // 4  # leave room for later inserts
            for h, mh, n, i, f in hashed:
                rl = self.rec_len(len(n))
                if cur and used + rl > fill and h != cur[-1][0]:
                    leaves.append(cur); cur, used = [], 0
                elif cur and used + rl > end:  # same-hash run overflowing: continuation leaf
                    leaves.append(cur); cur, used = [], 0
                cur.append((h, n, i, f)); used += rl
            if cur:
                leaves.append(cur)
            for k, leaf in enumerate(leaves):
                h = leaf[0][0]
                if k and leaves[k - 1][-1][0] == h:
                    h |= 1
                starts.append(h)
            nleaves = len(leaves)
            root_limit = (BLOCK - 32) // 8 - (1 if self.metadata_csum else 0)
            node_limit = (BLOCK - 8) // 8 - (1 if self.metadata_csum else 0)
            if nleaves <= root_limit:
                levels = 0
            elif nleaves <= root_limit * node_limit:
                levels = 1
            else:
                raise ValueError('directory too large')
            # logical layout: 0 root, then index nodes (if any), then leaves
            n_index = 0 if levels == 0 else (nleaves + node_limit - 1) // node_limit
            first_leaf = 1 + n_index
            blocks = [None] * (first_leaf + nleaves)
            for k, leaf in enumerate(leaves):
                blocks[first_leaf + k] = self._leaf_block(dir_ino, gen, [(n, i, f) for _, n, i, f in leaf])
            leaf_entries = [(starts[k], first_leaf + k) for k in range(nleaves)]
            if levels:
                idx_entries = []
                for j in range(n_index):
                    chunk = leaf_entries[j * node_limit:(j + 1) * node_limit]
                    node = bytearray(BLOCK)
                    struct.pack_into('<IHBB', node, 0, 0, BLOCK, 0, 0)
                    self._dx_entries(node, 8, node_limit, chunk)
                    if self.metadata_csum:
                        c, tail = self.dx_csum(dir_ino, gen, node, 8)
                        struct.pack_into('<II', node, tail, 0, c)
                    blocks[1 + j] = bytes(node)
                    idx_entries.append((chunk[0][0], 1 + j))
                top = idx_entries
            else:
                top = leaf_entries
            root = bytearray(BLOCK)
            struct.pack_into('<IHBB4s', root, 0, dir_ino, 12, 1, 2, b'.\0\0\0')
            struct.pack_into('<IHBB4s', root, 12, parent_ino, BLOCK - 12, 2, 2, b'..\0\0')
            struct.pack_into('<IBBBB', root, 24, 0, 1 + (3 if self.hash_unsigned else 0), 8, levels, 0)
            self._dx_entries(root, 32, root_limit, top)
            if self.metadata_csum:
                c, tail = self.dx_csum(dir_ino, gen, root, 32)
                struct.pack_into('<II', root, tail, 0, c)
            blocks[0] = bytes(root)
            inode.flags = EXTENTS_FL | INDEX_FL
        # allocate and write
        ext = []
        k = 0
        while k < len(blocks):
            start, cnt = self.alloc_blocks(len(blocks) - k)
            for j in range(cnt):
                self.wb(start + j, blocks[k + j])
            if ext and ext[-1][1] + ext[-1][2] == start:
                ext[-1] = (ext[-1][0], ext[-1][1], ext[-1][2] + cnt)
            else:
                ext.append((k, start, cnt))
            k += cnt
        tree = self.build_extent_tree(dir_ino, inode, ext)
        inode.size = len(blocks) * BLOCK
        inode.blocks = (len(blocks) + tree) * (BLOCK // 512)
        return ext

    @staticmethod
    def _dx_entries(node, at, limit, entries):
        struct.pack_into('<HHI', node, at, limit, len(entries), entries[0][1])
        for k, (h, b) in enumerate(entries[1:], 1):
            struct.pack_into('<II', node, at + 8 * k, h, b)

    def release_blocks(self, inode):
        """Free all data and tree blocks of an extent/fast inode."""
        if not (inode.flags & EXTENTS_FL):
            return
        tree = []
        for lblk, pblk, ln, _ in self.extents(inode, tree):
            self.free_blocks(pblk, ln)
        for b in tree:
            self.free_blocks(b, 1)

    def rewrite_directory(self, dir_ino, entries=None):
        inode = self.read_inode(dir_ino)
        cur = self.dir_entries(dir_ino, inode)
        parent = next(i for n, i, f in cur if n == b'..')
        if entries is None:
            entries = [(n, i, f) for n, i, f in cur if n not in (b'.', b'..')]
        self.release_blocks(inode)
        self.build_directory(dir_ino, inode, parent, entries)
        self.write_inode(dir_ino, inode)

    # high level ----------------------------------------------------------------
    def split(self, path):
        parts = [p for p in path.replace('\\', '/').split('/') if p and p != '.']
        if not parts or any(p == '..' for p in parts):
            raise ValueError('bad path ' + path)
        return parts

    def ensure_dir(self, parts, mode=0o755, uid=0, gid=0, times_ns=None):
        ino = ROOT_INO
        for p in parts:
            ents = self.dir_entries(ino)
            hit = next((e for e in ents if e[0] == p.encode()), None)
            if hit:
                if hit[2] != 2:
                    raise ValueError('%s is not a directory' % p)
                ino = hit[1]
            else:
                ino = self.mkdir_in(ino, p.encode(), mode, uid, gid, times_ns)
        return ino

    def begin_dir(self, parent, mode=0o755, uid=0, gid=0, times_ns=None):
        """Allocate a directory inode whose blocks are written later by finish_dir (bulk builds)."""
        ino = self.alloc_inode(True, parent)
        inode = self.new_inode(S_IFDIR | (mode & 0o7777), uid, gid, times_ns)
        return ino, inode

    def finish_dir(self, ino, inode, parent, entries):
        """entries: [(name bytes, ino, ftype)] without dots; sets the link count from subdirectories."""
        subdirs = sum(1 for _, _, ft in entries if ft == 2)
        inode.links = 2 + subdirs if 2 + subdirs < 65000 else 1
        self.build_directory(ino, inode, parent, entries)
        self.write_inode(ino, inode)

    def mkdir_in(self, parent, name, mode=0o755, uid=0, gid=0, times_ns=None):
        ino = self.alloc_inode(True, parent)
        inode = self.new_inode(S_IFDIR | (mode & 0o7777), uid, gid, times_ns)
        inode.links = 2
        self.build_directory(ino, inode, parent, [])
        self.write_inode(ino, inode)
        self.add_entry(parent, name, ino, 2)
        pinode = self.read_inode(parent)
        if pinode.links < 65000:
            pinode.links += 1
        self.write_inode(parent, pinode)
        return ino

    def add_entry(self, dir_ino, name, ino, ft):
        ents = [(n, i, f) for n, i, f in self.dir_entries(dir_ino) if n not in (b'.', b'..')]
        if any(n == name for n, _, _ in ents):
            raise FileExistsError(name.decode(errors='replace'))
        if len(name) > 255:
            raise ValueError('name too long')
        ents.append((name, ino, ft))
        self.rewrite_directory(dir_ino, ents)

    def remove_entry(self, dir_ino, name):
        ents = [(n, i, f) for n, i, f in self.dir_entries(dir_ino) if n not in (b'.', b'..')]
        hit = next((e for e in ents if e[0] == name), None)
        if not hit:
            raise FileNotFoundError(name.decode(errors='replace'))
        ents.remove(hit)
        self.rewrite_directory(dir_ino, ents)
        return hit

    def unlink_inode(self, ino):
        inode = self.read_inode(ino)
        is_dir = (inode.mode & S_IFMT) == S_IFDIR
        inode.links = max(0, inode.links - (2 if is_dir else 1))
        if inode.links:
            inode.set_time('ctime', time.time_ns())
            self.write_inode(ino, inode)
            return
        if not ((inode.mode & S_IFMT) == S_IFLNK and self.is_fast_symlink(inode)):
            self.release_blocks(inode)
        inode.dtime = int(time.time())
        inode.blocks = 0
        inode.size = 0
        inode.block = bytes(60)
        inode.flags = 0
        self.write_inode(ino, inode)
        self.free_inode(ino, is_dir)

    def remove(self, path):
        parts = self.split(path)
        parent = self.ensure_existing(parts[:-1])
        name, ino, ft = self.remove_entry(parent, parts[-1].encode())
        if ft == 2:
            if [e for e in self.dir_entries(ino) if e[0] not in (b'.', b'..')]:
                raise OSError(39, 'directory not empty')
            pinode = self.read_inode(parent)
            if pinode.links > 2:
                pinode.links -= 1
            self.write_inode(parent, pinode)
        self.unlink_inode(ino)

    def ensure_existing(self, parts):
        ino = ROOT_INO
        for p in parts:
            hit = next((e for e in self.dir_entries(ino) if e[0] == p.encode()), None)
            if not hit or hit[2] != 2:
                raise FileNotFoundError('/'.join(parts))
            ino = hit[1]
        return ino

    def put_file(self, host_path, guest, mode=None, uid=0, gid=0, times_ns=None, replace=True):
        parts = self.split(guest)
        parent = self.ensure_dir(parts[:-1])
        st = os.stat(host_path)
        if mode is None:
            mode = st.st_mode & 0o777
        if times_ns is None:
            times_ns = st.st_mtime_ns
        size = st.st_size
        name = parts[-1].encode()
        existing = next((e for e in self.dir_entries(parent) if e[0] == name), None)
        if existing:
            if not replace or existing[2] == 2:
                raise FileExistsError(guest)
            self.remove_entry(parent, name)
            self.unlink_inode(existing[1])

        def chunks():
            with open(host_path, 'rb') as src:
                while True:
                    b = src.read(8 << 20)
                    if not b:
                        return
                    yield b
        ino = self.make_file(chunks(), size, mode, uid, gid, times_ns, parent)
        self.add_entry(parent, name, ino, 1)
        return ino

    def put_bytes(self, data, guest, mode=0o644, uid=0, gid=0, times_ns=None):
        parts = self.split(guest)
        parent = self.ensure_dir(parts[:-1])
        name = parts[-1].encode()
        existing = next((e for e in self.dir_entries(parent) if e[0] == name), None)
        if existing:
            self.remove_entry(parent, name)
            self.unlink_inode(existing[1])
        ino = self.make_file(data, len(data), mode, uid, gid, times_ns, parent)
        self.add_entry(parent, name, ino, 1)
        return ino

    def symlink(self, target, guest, uid=0, gid=0, times_ns=None):
        parts = self.split(guest)
        parent = self.ensure_dir(parts[:-1])
        ino = self.make_symlink(target, uid, gid, times_ns, parent)
        self.add_entry(parent, parts[-1].encode(), ino, 7)
        return ino

    def mknod(self, guest, mode, rdev=0, uid=0, gid=0):
        parts = self.split(guest)
        parent = self.ensure_dir(parts[:-1])
        ino = self.make_node(mode, rdev, uid, gid, None, parent)
        self.add_entry(parent, parts[-1].encode(), ino, FT[mode & S_IFMT])
        return ino

    def link(self, existing, guest):
        ino = self.lookup(existing)
        if not ino:
            raise FileNotFoundError(existing)
        inode = self.read_inode(ino)
        if (inode.mode & S_IFMT) == S_IFDIR:
            raise IsADirectoryError(existing)
        parts = self.split(guest)
        parent = self.ensure_dir(parts[:-1])
        self.add_entry(parent, parts[-1].encode(), ino, FT[inode.mode & S_IFMT])
        inode.links += 1
        self.write_inode(ino, inode)

    def setattr(self, guest, mode=None, uid=None, gid=None, mtime_ns=None):
        ino = self.lookup(guest)
        if not ino:
            raise FileNotFoundError(guest)
        inode = self.read_inode(ino)
        if mode is not None:
            inode.mode = (inode.mode & S_IFMT) | (mode & 0o7777)
        if uid is not None:
            inode.uid = uid
        if gid is not None:
            inode.gid = gid
        if mtime_ns is not None:
            inode.set_time('mtime', mtime_ns)
            inode.set_time('atime', mtime_ns)
        self.write_inode(ino, inode)

    # flush -----------------------------------------------------------------------
    def flush_groups(self):
        for g in sorted(self.dirty_groups):
            d = self.gds[g]
            if g in self.bitmaps:
                bm = self.bitmaps[g]
                if self.metadata_csum:
                    d.block_bitmap_csum = self.bitmap_csum(bm, self.bpg // 8)
                self.wb(d.block_bitmap, bytes(bm))
            if g in self.ibitmaps:
                bm = self.ibitmaps[g]
                if self.metadata_csum:
                    d.inode_bitmap_csum = self.bitmap_csum(bm, self.ipg // 8)
                self.wb(d.inode_bitmap, bytes(bm))
        self.dirty_groups.clear()
        self.write_gdt()

    def write_gdt(self, group_copy=0):
        raw = bytearray()
        for g, d in enumerate(self.gds):
            if self.metadata_csum:
                d.checksum = self.gd_csum(g, d.raw)
            raw += d.raw[:self.desc_size]
        raw += bytes(self.gdt_blocks * BLOCK - len(raw))
        first = self.group_first(group_copy) + 1
        for i in range(self.gdt_blocks):
            self.wb(first + i, bytes(raw[i * BLOCK:(i + 1) * BLOCK]))

    def flush_super(self, group_copy=0):
        sb = self.sb
        sb.block_group_nr = group_copy
        if self.metadata_csum:
            sb.checksum = self.sb_csum(sb.raw)
        off = self.group_first(group_copy) * BLOCK + (1024 if group_copy == 0 else 0)
        self.f.seek(self.base + off)
        self.f.write(sb.raw)
        sb.block_group_nr = 0

    def close(self, backups=False):
        self.flush_groups()
        self.sb.state = self.sb.state | STATE_VALID
        self.sb.wtime = int(time.time())
        if backups:
            for g in range(1, self.ngroups):
                if has_super(g):
                    self.flush_super(g)
                    self.write_gdt(g)
        if self.metadata_csum:
            self.sb.checksum = self.sb_csum(self.sb.raw)
        self.flush_super(0)
        self.f.flush()
        self.f.close()

    # journal ---------------------------------------------------------------------
    def journal_super(self):
        return journal_super(self)


# ---------------------------------------------------------------- jbd2
def journal_map(fs):
    """Logical journal block -> physical block list of the journal inode."""
    jino = fs.sb.journal_inum
    if not (fs.sb.compat & COMPAT_HAS_JOURNAL) or not jino:
        return None
    inode = fs.read_inode(jino)
    out = {}
    for lblk, pblk, ln, uninit in fs.file_blocks(inode):
        for k in range(ln):
            out[lblk + k] = pblk + k
    n = inode.size // BLOCK
    return [out.get(i) for i in range(n)]


def journal_super(fs):
    jm = journal_map(fs)
    if not jm or jm[0] is None:
        return None
    raw = fs.rb(jm[0])
    magic, btype, seq = struct.unpack_from('>III', raw, 0)
    if magic != JBD2_MAGIC or btype not in (3, 4):
        raise ValueError('bad journal superblock')
    bsize, maxlen, first, sequence, start, errno = struct.unpack_from('>IIIIIi', raw, 12)
    fc, fi, fr = struct.unpack_from('>III', raw, 0x24)
    return {'raw': raw, 'blocksize': bsize, 'maxlen': maxlen, 'first': first, 'sequence': sequence,
            'start': start, 'errno': errno, 'compat': fc, 'incompat': fi, 'ro_compat': fr,
            'uuid': raw[0x30:0x40], 'csum_type': raw[0x50], 'checksum': struct.unpack_from('>I', raw, 0xFC)[0],
            'map': jm}


JBD2_CSUM_V2, JBD2_CSUM_V3, JBD2_64BIT, JBD2_REVOKE = 0x8, 0x10, 0x2, 0x1


def journal_scan(fs, js, verbose=False):
    """Recovery scan. Returns (transactions, revoked, end_sequence); transactions = [(seq, [(home, jblock, flags)])]."""
    jm, first, maxlen = js['map'], js['first'], js['maxlen']
    csum = js['incompat'] & (JBD2_CSUM_V2 | JBD2_CSUM_V3)
    v3 = js['incompat'] & JBD2_CSUM_V3
    seed = crc32c(0xFFFFFFFF, js['uuid'])
    tag_bytes = 16
    if not v3:
        tag_bytes = (12 if js['incompat'] & JBD2_64BIT else 8) + (2 if js['incompat'] & JBD2_CSUM_V2 else 0)
    seq, blk = js['sequence'], js['start']
    txns, revoked, cur = [], {}, []
    steps = 0

    def nxt(b):
        b += 1
        return first if b >= maxlen else b
    while steps < maxlen:
        raw = fs.rb(jm[blk])
        magic, btype, bseq = struct.unpack_from('>III', raw, 0)
        if magic != JBD2_MAGIC or bseq != seq:
            break
        if btype == 1:  # descriptor
            if csum and struct.unpack_from('>I', raw, BLOCK - 4)[0] != crc32c(seed, raw[:BLOCK - 4] + b'\0\0\0\0'):
                break
            off, tags = 12, []
            limit = BLOCK - (4 if csum else 0)
            while off + tag_bytes <= limit:
                if v3:
                    lo, flags, hi, tcs = struct.unpack_from('>IIII', raw, off)
                else:
                    lo, tcs, flags = struct.unpack_from('>IHH', raw, off)
                    hi = struct.unpack_from('>I', raw, off + 8)[0] if js['incompat'] & JBD2_64BIT else 0
                off += tag_bytes
                if not (flags & 2):
                    off += 16
                tags.append(((hi << 32) | lo, flags, tcs))
                if flags & 8:
                    break
            for home, flags, tcs in tags:
                blk = nxt(blk)
                cur.append((home, blk, flags, tcs))
            blk = nxt(blk)
            steps += 1 + len(tags)
            continue
        if btype == 2:  # commit
            if csum and v3:
                stored = struct.unpack_from('>I', raw, 16)[0]
                r = bytearray(raw)
                r[16:20] = b'\0\0\0\0'
                if stored != crc32c(seed, bytes(r)):
                    break
            txns.append((seq, cur))
            cur = []
            seq = (seq + 1) & 0xFFFFFFFF
            blk = nxt(blk)
            steps += 1
            continue
        if btype == 5:  # revoke
            count = struct.unpack_from('>I', raw, 12)[0]
            width = 8 if js['incompat'] & JBD2_64BIT else 4
            off = 16
            while off + width <= count:
                b = struct.unpack_from('>Q' if width == 8 else '>I', raw, off)[0]
                revoked[b] = max(revoked.get(b, 0), seq)
                off += width
            blk = nxt(blk)
            steps += 1
            continue
        break
    return txns, revoked, seq, seed, v3


def journal_replay(fs, apply_to_overlay=True):
    """Replay committed transactions. With apply_to_overlay the image is not modified (fs.overlay gets the blocks)."""
    js = journal_super(fs)
    if not js or not js['start']:
        return 0
    txns, revoked, end_seq, seed, v3 = journal_scan(fs, js)
    count = 0
    for seq, tags in txns:
        for home, jblk, flags, tcs in tags:
            if home in revoked and revoked[home] >= seq:
                continue
            data = bytearray(fs.rb(js['map'][jblk]))
            if v3:
                if crc32c(crc32c(seed, struct.pack('>I', seq)), bytes(data)) != tcs:
                    raise ValueError('journal block checksum mismatch (seq %d home %d)' % (seq, home))
            if flags & 1:
                struct.pack_into('>I', data, 0, JBD2_MAGIC)
            if apply_to_overlay:
                fs.overlay[home] = bytes(data)
            else:
                fs.wb(home, bytes(data))
            count += 1
    jraw = bytearray(js['raw'])
    struct.pack_into('>I', jraw, 0x18, end_seq)
    struct.pack_into('>I', jraw, 0x1C, 0)
    if js['csum_type'] == 4 or js['incompat'] & (JBD2_CSUM_V2 | JBD2_CSUM_V3):
        jraw[0xFC:0x100] = b'\0\0\0\0'
        struct.pack_into('>I', jraw, 0xFC, crc32c(0xFFFFFFFF, bytes(jraw[:1024])))
    if apply_to_overlay:
        fs.overlay[js['map'][0]] = bytes(jraw)
    else:
        fs.wb(js['map'][0], bytes(jraw))
    # superblock may have been replayed: reload descriptors from the new view
    return count


def create_journal_super(fs_uuid, nblocks):
    raw = bytearray(BLOCK)
    struct.pack_into('>III', raw, 0, JBD2_MAGIC, 4, 0)
    struct.pack_into('>IIIIIi', raw, 12, BLOCK, nblocks, 1, 1, 0, 0)
    struct.pack_into('>III', raw, 0x24, 0, JBD2_64BIT | JBD2_CSUM_V3, 0)
    raw[0x30:0x40] = fs_uuid
    struct.pack_into('>I', raw, 0x40, 1)
    raw[0x50] = 4  # crc32c
    struct.pack_into('>I', raw, 0xFC, crc32c(0xFFFFFFFF, bytes(raw[:1024])))
    return bytes(raw)


# ---------------------------------------------------------------- mkfs
def default_journal_blocks(blocks):
    if blocks < 2048:
        return 0
    if blocks < 32768:
        return 1024
    if blocks < 256 * 1024:
        return 4096
    if blocks < 512 * 1024:
        return 8192
    if blocks < 4096 * 1024:
        return 16384
    return 32768


def sparse_file(f):
    if os.name == 'nt':
        import ctypes
        import msvcrt
        returned = ctypes.c_ulong()
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                           ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p]
        kernel.DeviceIoControl(msvcrt.get_osfhandle(f.fileno()), 0x900C4, None, 0, None, 0, ctypes.byref(returned), None)


def mkfs(path, size_bytes=None, base=None, label=b'nexus', inode_ratio=16384, journal_blocks=None,
         uuid=None, hash_seed=None, now=None, root_mode=0o755):
    """Format an ext4 volume. If base is None and the image has an MBR partition table a Linux
    partition is created at LBA 32768 (NexusOS layout), else the whole image is used."""
    now = int(now if now is not None else time.time())
    exists = os.path.exists(path)
    with open(path, 'r+b' if exists else 'w+b') as f:
        sparse_file(f)
        if size_bytes and (not exists or os.path.getsize(path) < size_bytes):
            f.truncate(size_bytes)
        f.seek(0, 2)
        disk = f.tell()
        if base is None:
            base = PART_LBA * SECTOR
        length = disk - base
        if base:
            f.seek(0)
            mbr = bytearray(f.read(512).ljust(512, b'\0'))
            for off in range(446, 510, 16):
                mbr[off:off + 16] = bytes(16)
            mbr[446 + 4] = 0x83
            struct.pack_into('<II', mbr, 446 + 8, base // SECTOR, length // SECTOR)
            mbr[510:512] = b'\x55\xaa'
            f.seek(0)
            f.write(mbr)
    blocks = length // BLOCK
    if blocks < 1024:
        raise ValueError('volume too small')
    bpg = 32768
    ngroups = (blocks + bpg - 1) // bpg
    if blocks - (ngroups - 1) * bpg < 256 and ngroups > 1:  # drop a tiny last group
        blocks = (ngroups - 1) * bpg
        ngroups -= 1
    ipg = max(16, min(bpg, (bpg * BLOCK // inode_ratio) if ngroups > 1 else blocks * BLOCK // inode_ratio))
    ipg = (ipg + 15) // 16 * 16
    itb = ipg * INODE_SIZE // BLOCK
    desc = 64
    gdt_blocks = (ngroups * desc + BLOCK - 1) // BLOCK
    flex = 16
    jblocks = default_journal_blocks(blocks) if journal_blocks is None else journal_blocks
    uuid = uuid or uuidmod.uuid4().bytes
    hash_seed = hash_seed or uuidmod.uuid4().bytes
    sb = Super(bytes(1024))
    sb.inodes_count = ipg * ngroups
    sb.blocks_count = blocks
    sb.r_blocks_count = 0
    sb.first_data_block = 0
    sb.log_block_size = 2
    sb.log_cluster_size = 2
    sb.blocks_per_group = bpg
    sb.clusters_per_group = bpg
    sb.inodes_per_group = ipg
    sb.wtime = now
    sb.max_mnt_count = 0xFFFF
    sb.magic = EXT4_MAGIC
    sb.state = STATE_VALID
    sb.errors = 1
    sb.lastcheck = now
    sb.rev_level = 1
    sb.first_ino = FIRST_INO
    sb.inode_size = INODE_SIZE
    sb.compat = COMPAT_DIR_INDEX | (COMPAT_HAS_JOURNAL if jblocks else 0)
    sb.incompat = INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG
    sb.ro_compat = RO_SPARSE_SUPER | RO_LARGE_FILE | RO_HUGE_FILE | RO_DIR_NLINK | RO_EXTRA_ISIZE | RO_METADATA_CSUM
    sb.uuid = uuid
    sb.volume_name = label[:16].ljust(16, b'\0')
    sb.journal_inum = JOURNAL_INO if jblocks else 0
    sb.hash_seed = hash_seed
    sb.def_hash_version = 1
    sb.desc_size = desc
    sb.mkfs_time = now
    sb.min_extra_isize = EXTRA_ISIZE
    sb.want_extra_isize = EXTRA_ISIZE
    sb.flags = 1  # signed directory hash
    sb.log_groups_per_flex = 4
    sb.checksum_type = 1
    sb.lpf_ino = LOST_FOUND_INO
    seed = crc32c(0xFFFFFFFF, uuid)
    # group layout
    used = [set() for _ in range(ngroups)]
    gds = [Desc(bytes(desc)) for _ in range(ngroups)]
    meta_end = {}
    for g in range(ngroups):
        first = g * bpg
        n = 0
        if has_super(g):
            n = 1 + gdt_blocks
        meta_end[g] = first + n
        for b in range(first, first + n):
            used[g].add(b)
    for fg in range(0, ngroups, flex):
        members = list(range(fg, min(fg + flex, ngroups)))
        cursor = meta_end[fg]
        for g in members:
            gds[g].block_bitmap = cursor; cursor += 1
        for g in members:
            gds[g].inode_bitmap = cursor; cursor += 1
        for g in members:
            gds[g].inode_table = cursor; cursor += itb
        if cursor > min((fg + 1) * bpg, blocks) and fg + 1 < ngroups:
            raise ValueError('flex metadata does not fit in one group')
        for b in range(meta_end[fg], cursor):
            used[b // bpg].add(b)
        meta_end[fg] = cursor
    for g in range(ngroups):
        gds[g].flags = 0x4  # INODE_ZEROED
    with open(path, 'r+b') as f:
        # zero the metadata area of the volume that is not sparse already: bitmaps/tables are written below
        f.seek(base)
        f.write(bytes(BLOCK))  # block 0 (sb written later)
    # write bitmaps through a temporary Ext4-like writer
    with open(path, 'r+b') as f:
        def wblk(b, data):
            f.seek(base + b * BLOCK)
            f.write(data)
        for g in range(ngroups):
            bm = bytearray(BLOCK)
            first = g * bpg
            nb = min(bpg, blocks - first)
            for b in used[g]:
                i = b - first
                bm[i >> 3] |= 1 << (i & 7)
            for i in range(nb, BLOCK * 8):
                bm[i >> 3] |= 1 << (i & 7)
            wblk(gds[g].block_bitmap, bytes(bm))
            gds[g].free_blocks = nb - len(used[g])
            ibm = bytearray(BLOCK)
            for i in range(ipg, BLOCK * 8):
                ibm[i >> 3] |= 1 << (i & 7)
            if g == 0:
                for i in range(FIRST_INO - 1):
                    ibm[i >> 3] |= 1 << (i & 7)
            wblk(gds[g].inode_bitmap, bytes(ibm))
            gds[g].free_inodes = ipg - (FIRST_INO - 1 if g == 0 else 0)
            gds[g].itable_unused = 0
        sb.free_blocks = sum(d.free_blocks for d in gds)
        sb.free_inodes = sum(d.free_inodes for d in gds)
        f.seek(base + 1024)
        f.write(sb.raw)
        gdt = bytearray()
        for g, d in enumerate(gds):
            gdt += d.raw
        gdt += bytes(gdt_blocks * BLOCK - len(gdt))
        wblk(1, bytes(gdt))
    fs = Writer(path, base=base, length=length)
    # bitmap checksums are applied by flush_groups for dirty groups: mark all
    fs.dirty_groups = set(range(ngroups))
    for g in range(ngroups):
        fs.block_bitmap(g)
        fs.inode_bitmap(g)
    # reserved inode 1 (bad blocks): empty, with times
    bad = fs.new_inode(0, 0, 0, now * 10**9)
    bad.generation = 0
    fs.write_inode(1, bad)
    # root directory
    root = fs.new_inode(S_IFDIR | root_mode, 0, 0, now * 10**9)
    root.generation = 0
    root.links = 3
    fs.gds[0].used_dirs += 1
    fs.next_goal = meta_end[0]
    fs.build_directory(ROOT_INO, root, ROOT_INO, [])
    fs.write_inode(ROOT_INO, root)
    # journal
    if jblocks:
        start, cnt = fs.alloc_blocks(jblocks, meta_end[0])
        if cnt != jblocks:
            raise ValueError('journal needs %d contiguous blocks' % jblocks)
        j = fs.new_inode(S_IFREG | 0o600, 0, 0, now * 10**9)
        j.generation = 0
        j.links = 1
        j.flags = EXTENTS_FL
        j.size = jblocks * BLOCK
        ext = []
        b, left = start, jblocks
        while left:
            n = min(left, EXT_INIT_MAX)
            ext.append((b - start, b, n))
            b += n; left -= n
        tree = fs.build_extent_tree(JOURNAL_INO, j, ext)
        j.blocks = (jblocks + tree) * 8
        fs.write_inode(JOURNAL_INO, j)
        fs.wb(start, create_journal_super(uuid, jblocks))
        sbj = bytearray(68)
        sbj[0:60] = j.block
        struct.pack_into('<II', sbj, 60, j.size >> 32, j.size & 0xFFFFFFFF)
        fs.sb.jnl_blocks = bytes(sbj)
        fs.sb.jnl_backup_type = 1
    # lost+found with 4 blocks so fsck can relink without allocating
    lf_ino = fs.alloc_inode(True, ROOT_INO, group=0)
    assert lf_ino == LOST_FOUND_INO, lf_ino
    lf = fs.new_inode(S_IFDIR | 0o700, 0, 0, now * 10**9)
    lf.links = 2
    fs.build_directory(lf_ino, lf, ROOT_INO, [])
    # extend lost+found to 4 empty leaf blocks
    fs.release_blocks(lf)
    start, cnt = fs.alloc_blocks(4)
    assert cnt == 4
    dots = [(b'.', lf_ino, 2), (b'..', ROOT_INO, 2)]
    fs.wb(start, fs._leaf_block(lf_ino, lf.generation, dots))
    for k in range(1, 4):
        fs.wb(start + k, fs._leaf_block(lf_ino, lf.generation, []))
    fs.build_extent_tree(lf_ino, lf, [(0, start, 4)])
    lf.flags = EXTENTS_FL
    lf.size = 4 * BLOCK
    lf.blocks = 4 * 8
    fs.write_inode(lf_ino, lf)
    rootents = [(b'lost+found', lf_ino, 2)]
    fs.rewrite_directory(ROOT_INO, rootents)
    fs.close(backups=True)
    return base, length


# ---------------------------------------------------------------- checker
class Checker:
    """e2fsck-style read-only verification. errors -> list of strings."""

    def __init__(self, path, replay=True, verbose=False, base=None):
        self.errors = []
        self.warnings = []
        self.verbose = verbose
        self.fs = Ext4(path, base=base)
        self.replayed = 0
        js = None
        if self.fs.sb.compat & COMPAT_HAS_JOURNAL:
            try:
                js = journal_super(self.fs)
            except ValueError as e:
                self.err('journal: %s' % e)
        if js and js['start']:
            if replay:
                self.replayed = journal_replay(self.fs, apply_to_overlay=True)
                # reopen with the replayed overlay
                ov = self.fs.overlay
                self.fs.close()
                self.fs = Ext4(path, overlay=ov, base=base)
            else:
                self.err('journal needs recovery (start=%d)' % js['start'])
        self.journal = js

    def err(self, msg):
        self.errors.append(msg)

    def run(self):
        fs = self.fs
        sb = fs.sb
        if fs.metadata_csum and sb.checksum != fs.sb_csum(sb.raw):
            self.err('superblock checksum')
        if sb.incompat & ~SUPPORTED_INCOMPAT:
            self.err('unsupported incompat features %#x' % (sb.incompat & ~SUPPORTED_INCOMPAT))
        if sb.state & STATE_ERROR:
            self.err('filesystem marked with errors')
        bused = [bytearray(BLOCK) for _ in range(fs.ngroups)]
        iused = [bytearray(BLOCK) for _ in range(fs.ngroups)]
        owner = {}

        def mark_block(b, who):
            if b >= fs.blocks or b < sb.first_data_block:
                self.err('%s: block %d out of range' % (who, b)); return
            g, i = divmod(b - sb.first_data_block, fs.bpg)
            if (bused[g][i >> 3] >> (i & 7)) & 1:
                self.err('%s: block %d multiply claimed (also %s)' % (who, b, owner.get(b, '?')))
            bused[g][i >> 3] |= 1 << (i & 7)
            if len(owner) < 2000000:
                owner[b] = who
        # group metadata
        for g in range(fs.ngroups):
            d = fs.gds[g]
            if fs.metadata_csum and d.checksum != fs.gd_csum(g, d.raw):
                self.err('group %d descriptor checksum' % g)
            first = fs.group_first(g)
            if has_super(g) or not (sb.ro_compat & RO_SPARSE_SUPER):
                for b in range(first, first + 1 + fs.gdt_blocks + sb.reserved_gdt_blocks):
                    mark_block(b, 'sb/gdt %d' % g)
            if g == 0 and sb.compat & COMPAT_RESIZE_INODE:  # resize inode 7: its double-indirect block
                dind = struct.unpack_from('<I', fs.read_inode(7).raw, 0x28 + 13 * 4)[0]
                if dind:
                    mark_block(dind, 'resize inode')
            mark_block(d.block_bitmap, 'block bitmap %d' % g)
            mark_block(d.inode_bitmap, 'inode bitmap %d' % g)
            itb = (fs.ipg * fs.isize + BLOCK - 1) // BLOCK
            for b in range(d.inode_table, d.inode_table + itb):
                mark_block(b, 'inode table %d' % g)
            if fs.metadata_csum:
                bm = fs.rb(d.block_bitmap)
                if d.block_bitmap_csum != fs.bitmap_csum(bm, fs.bpg // 8):
                    self.err('group %d block bitmap checksum' % g)
                ibm = fs.rb(d.inode_bitmap)
                if d.inode_bitmap_csum != fs.bitmap_csum(ibm, fs.ipg // 8) & (0xFFFFFFFF if fs.desc_size >= 64 else 0xFFFF):
                    self.err('group %d inode bitmap checksum' % g)
        # inodes
        links = {}
        dir_parent = {}
        children = {}
        types = {}
        inodes = {}
        orphans = set()
        o = sb.last_orphan
        seen_orphan = 0
        while o and seen_orphan < 100000:
            orphans.add(o)
            seen_orphan += 1
            if o > sb.inodes_count:
                self.err('orphan list points past inode count'); break
            o = fs.read_inode(o).dtime
        # Linux orphan file (inode s_orphan_file_inum, linked nowhere): each block holds inode numbers,
        # then an 8-byte tail; entries are only meaningful while ORPHAN_PRESENT is set
        orphan_file = sb.orphan_file_inum if sb.compat & COMPAT_ORPHAN_FILE else 0
        if orphan_file and sb.ro_compat & RO_COMPAT_ORPHAN_PRESENT:
            data = fs.read_file(fs.read_inode(orphan_file))
            for at in range(0, len(data) - BLOCK + 1, BLOCK):
                for (entry,) in struct.iter_unpack('<I', data[at:at + BLOCK - 8]):
                    if entry:
                        orphans.add(entry)
        for g in range(fs.ngroups):
            d = fs.gds[g]
            ibm = fs.rb(d.inode_bitmap)
            itb = (fs.ipg * fs.isize) // BLOCK
            table = fs.read_run(d.inode_table, itb)
            for i in range(fs.ipg):
                ino = g * fs.ipg + i + 1
                allocated = (ibm[i >> 3] >> (i & 7)) & 1
                raw = table[i * fs.isize:(i + 1) * fs.isize]
                inode = Inode(raw)
                if allocated:
                    iused[g][i >> 3] |= 1 << (i & 7)
                if ino < sb.first_ino and ino != ROOT_INO and ino != sb.journal_inum and ino != 1:
                    continue
                if not allocated:
                    if inode.mode and inode.links and inode.dtime == 0 and ino >= sb.first_ino:
                        self.err('inode %d in use but not marked in bitmap' % ino)
                    continue
                if ino >= sb.first_ino and not inode.mode:
                    self.err('inode %d allocated with zero mode' % ino)
                    continue
                if not inode.mode and ino < sb.first_ino:
                    continue
                if fs.metadata_csum:
                    c = fs.inode_csum(ino, raw)
                    stored = inode.checksum_lo | ((inode.checksum_hi << 16) if fs.isize > 128 and 128 + inode.extra_isize >= 0x84 else 0)
                    if c != stored:
                        self.err('inode %d checksum' % ino)
                if inode.links == 0 and ino not in orphans and ino >= sb.first_ino:
                    self.err('inode %d has zero links and is not on the orphan list' % ino)
                inodes[ino] = inode
                self.check_inode_blocks(ino, inode, mark_block)
                if (inode.mode & S_IFMT) == S_IFDIR:
                    self.check_dir(ino, inode, children, dir_parent, types)
        # namespace reachability and link counts
        if ROOT_INO not in inodes:
            self.err('root inode missing')
        reach = {ROOT_INO}
        stack = [ROOT_INO]
        while stack:
            d = stack.pop()
            for name, child, ft in children.get(d, []):
                links[child] = links.get(child, 0) + 1
                ci = inodes.get(child)
                if ci is None:
                    self.err('dir %d entry %r -> unallocated inode %d' % (d, name, child)); continue
                exp = {1: S_IFREG, 2: S_IFDIR, 3: S_IFCHR, 4: S_IFBLK, 5: S_IFIFO, 6: S_IFSOCK, 7: S_IFLNK}.get(ft)
                if exp != (ci.mode & S_IFMT):
                    self.err('dir %d entry %r file type %d does not match inode mode %o' % (d, name, ft, ci.mode))
                if (ci.mode & S_IFMT) == S_IFDIR:
                    if child in reach:
                        self.err('directory %d reachable twice' % child); continue
                    if dir_parent.get(child) != d:
                        self.err('directory %d .. is %s, expected %d' % (child, dir_parent.get(child), d))
                    reach.add(child)
                    stack.append(child)
                else:
                    reach.add(child)
        for ino, inode in inodes.items():
            if (ino < sb.first_ino and ino != ROOT_INO) or ino == orphan_file:
                continue
            is_dir = (inode.mode & S_IFMT) == S_IFDIR
            if ino not in reach and ino not in orphans:
                self.err('inode %d (mode %o) unreachable' % (ino, inode.mode))
            if is_dir:
                subdirs = sum(1 for n, c, ft in children.get(ino, []) if ft == 2)
                expected = 2 + subdirs
                if not (inode.links == expected or (inode.links == 1 and expected >= 65000)):
                    if ino not in orphans:
                        self.err('directory %d link count %d, expected %d' % (ino, inode.links, expected))
            else:
                if inode.links != links.get(ino, 0) and not (ino in orphans and links.get(ino, 0) == 0):
                    self.err('inode %d link count %d, counted %d' % (ino, inode.links, links.get(ino, 0)))
        # bitmaps
        free_b = free_i = 0
        for g in range(fs.ngroups):
            d = fs.gds[g]
            bm = fs.rb(d.block_bitmap)
            nb = fs.group_blocks(g)
            for i in range(nb):
                a = (bm[i >> 3] >> (i & 7)) & 1
                u = (bused[g][i >> 3] >> (i & 7)) & 1
                if a != u:
                    self.err('block %d bitmap=%d actual=%d' % (fs.group_first(g) + i, a, u))
                    if len(self.errors) > 200:
                        return self.errors
            for i in range(nb, fs.bpg):
                if not (bm[i >> 3] >> (i & 7)) & 1:
                    self.err('group %d block bitmap padding not set' % g); break
            fb = nb - sum(bin(x).count('1') for x in bused[g][:(nb + 7) // 8])
            if fb != d.free_blocks:
                self.err('group %d free blocks %d, counted %d' % (g, d.free_blocks, fb))
            free_b += fb
            ibm = fs.rb(d.inode_bitmap)
            fi = fs.ipg - sum(bin(x).count('1') for x in ibm[:fs.ipg // 8])
            if fi != d.free_inodes:
                self.err('group %d free inodes %d, counted %d' % (g, d.free_inodes, fi))
            free_i += fi
            dirs = sum(1 for ino, inode in inodes.items() if (ino - 1) // fs.ipg == g and (inode.mode & S_IFMT) == S_IFDIR)
            if dirs != d.used_dirs:
                self.err('group %d used dirs %d, counted %d' % (g, d.used_dirs, dirs))
            if d.itable_unused > d.free_inodes:
                self.err('group %d itable_unused beyond free inodes' % g)
        if free_b != sb.free_blocks:
            self.err('superblock free blocks %d, counted %d' % (sb.free_blocks, free_b))
        if free_i != sb.free_inodes:
            self.err('superblock free inodes %d, counted %d' % (sb.free_inodes, free_i))
        # journal superblock
        if self.journal:
            js = journal_super(fs)
            if js['incompat'] & (JBD2_CSUM_V2 | JBD2_CSUM_V3):
                r = bytearray(js['raw'][:1024])
                r[0xFC:0x100] = b'\0\0\0\0'
                if crc32c(0xFFFFFFFF, bytes(r)) != js['checksum']:
                    self.err('journal superblock checksum')
            if js['maxlen'] != len(js['map']):
                self.err('journal length %d, inode has %d blocks' % (js['maxlen'], len(js['map'])))
        self.stats = {'inodes': len(inodes), 'free_blocks': free_b, 'free_inodes': free_i, 'replayed': self.replayed,
                      'orphans': len(orphans)}
        return self.errors

    def check_inode_blocks(self, ino, inode, mark_block):
        fs = self.fs
        kind = inode.mode & S_IFMT
        if kind in (S_IFCHR, S_IFBLK, S_IFIFO, S_IFSOCK):
            if inode.blocks:
                self.err('inode %d device/fifo/socket with blocks' % ino)
            return
        if kind == S_IFLNK and fs.is_fast_symlink(inode):
            if inode.size >= 60:
                self.err('fast symlink %d size %d' % (ino, inode.size))
            return
        if not (inode.flags & EXTENTS_FL):
            if inode.size and not inode.flags & INLINE_DATA_FL:
                for lblk, pblk, ln, _ in fs.file_blocks(inode):
                    for k in range(ln):
                        mark_block(pblk + k, 'inode %d' % ino)
            return
        total = 0
        last_end = -1
        max_end = 0

        def walk(node, depth_hint, lo, hi, is_root):
            nonlocal total, last_end, max_end
            magic, entries, emax, depth, _ = struct.unpack_from('<HHHHI', node, 0)
            if magic != EXT_MAGIC:
                self.err('inode %d bad extent magic' % ino); return
            if entries > emax:
                self.err('inode %d extent node overfull' % ino); return
            if depth_hint is not None and depth != depth_hint:
                self.err('inode %d extent depth mismatch' % ino)
            prev = None
            for i in range(entries):
                o = 12 + 12 * i
                if depth == 0:
                    lblk, ln, h, l = struct.unpack_from('<IHHI', node, o)
                    if ln > EXT_INIT_MAX:
                        ln -= EXT_INIT_MAX
                    if ln == 0:
                        self.err('inode %d zero-length extent' % ino)
                    p = (h << 32) | l
                    if lblk < lo or (hi is not None and lblk + ln > hi):
                        self.err('inode %d extent %d outside index range' % (ino, lblk))
                    if lblk <= last_end:
                        self.err('inode %d overlapping/unsorted extents at %d' % (ino, lblk))
                    last_end = lblk + ln - 1
                    max_end = max(max_end, lblk + ln)
                    for k in range(ln):
                        mark_block(p + k, 'inode %d' % ino)
                    total += ln
                else:
                    lblk, l, h, _ = struct.unpack_from('<IIHH', node, o)
                    child = (h << 32) | l
                    nxt = struct.unpack_from('<I', node, o + 12)[0] if i + 1 < entries else hi
                    if prev is not None and lblk <= prev:
                        self.err('inode %d unsorted index' % ino)
                    prev = lblk
                    mark_block(child, 'inode %d extent tree' % ino)
                    total += 1
                    cb = fs.rb(child)
                    if fs.metadata_csum:
                        cemax = struct.unpack_from('<H', cb, 4)[0]
                        if 12 + 12 * cemax + 4 > BLOCK:
                            self.err('inode %d extent block eh_max too big' % ino)
                        elif struct.unpack_from('<I', cb, 12 + 12 * cemax)[0] != fs.extent_csum(ino, inode.generation, cb):
                            self.err('inode %d extent block %d checksum' % (ino, child))
                    walk(cb, depth - 1, lblk, nxt, False)
        walk(inode.block, None, 0, None, True)
        blocks512 = inode.blocks
        if inode.flags & HUGE_FILE_FL:
            blocks512 *= 8
        ea = 8 if inode.file_acl else 0
        if blocks512 - ea != total * 8:
            self.err('inode %d i_blocks %d, counted %d' % (ino, blocks512, total * 8))
        if kind == S_IFREG or kind == S_IFLNK:
            # blocks past EOF are only legal when uninitialized (fallocate KEEP_SIZE)
            pass
        if kind == S_IFDIR and inode.size != max_end * BLOCK:
            self.err('directory %d size %d, extents end at block %d' % (ino, inode.size, max_end))

    def check_dir(self, ino, inode, children, dir_parent, types):
        fs = self.fs
        ents = []
        indexed = bool(inode.flags & INDEX_FL)
        ext = fs.file_blocks(inode)
        blocks = {}
        for lblk, pblk, ln, _ in ext:
            for k in range(ln):
                blocks[lblk + k] = pblk + k
        nblocks = inode.size // BLOCK
        dx_blocks = set()
        if indexed:
            dx_blocks = self.check_htree(ino, inode, blocks)
        for lb in range(nblocks):
            if lb not in blocks:
                self.err('directory %d hole at block %d' % (ino, lb)); continue
            blk = fs.rb(blocks[lb])
            if lb in dx_blocks:
                if lb == 0:
                    d0, r0, n0, f0 = struct.unpack_from('<IHBB', blk, 0)
                    d1, r1, n1, f1 = struct.unpack_from('<IHBB', blk, 12)
                    if blk[8:9] != b'.' or blk[20:22] != b'..' or d0 != ino:
                        self.err('htree root of %d has bad dots' % ino)
                    ents.append((b'.', d0, f0)); ents.append((b'..', d1, f1))
                continue
            off = 0
            end = BLOCK
            if fs.metadata_csum:
                tino, trec, tnl, tft = struct.unpack_from('<IHBB', blk, BLOCK - 12)
                if tino or trec != 12 or tnl or tft != 0xDE:
                    self.err('directory %d block %d missing checksum tail' % (ino, lb))
                elif struct.unpack_from('<I', blk, BLOCK - 4)[0] != fs.dirent_csum(ino, inode.generation, blk):
                    self.err('directory %d block %d checksum' % (ino, lb))
                end = BLOCK - 12
            while off < end:
                if off + 8 > end:
                    self.err('directory %d block %d truncated entry' % (ino, lb)); break
                dino, rec, nl, ft = struct.unpack_from('<IHBB', blk, off)
                if rec < 8 or rec % 4 or off + rec > end or (dino and 8 + nl > rec):
                    self.err('directory %d block %d bad rec_len %d at %d' % (ino, lb, rec, off)); break
                if dino:
                    name = bytes(blk[off + 8:off + 8 + nl])
                    if not nl or b'/' in name or b'\0' in name:
                        self.err('directory %d bad name %r' % (ino, name))
                    if dino > fs.sb.inodes_count:
                        self.err('directory %d entry %r inode out of range' % (ino, name))
                    if indexed and lb not in dx_blocks:
                        h = fs and dirhash(name, self.hash_version(ino, blocks), fs.hash_seed)[0]
                        rng = self.leaf_ranges.get(lb)
                        if rng and not (rng[0] <= h and (rng[1] is None or h < rng[1] or (h == rng[1] & ~1))):
                            self.err('directory %d entry %r hash %#x outside leaf range %r' % (ino, name, h, rng))
                    ents.append((name, dino, ft))
                off += rec
        if not ents or ents[0][0] != b'.' or ents[0][1] != ino:
            self.err('directory %d missing "."' % ino)
        if len(ents) < 2 or ents[1][0] != b'..':
            self.err('directory %d missing ".."' % ino)
        else:
            dir_parent[ino] = ents[1][1]
        names = set()
        kids = []
        for name, child, ft in ents[2:]:
            if name in names:
                self.err('directory %d duplicate name %r' % (ino, name))
            names.add(name)
            kids.append((name, child, ft))
        children[ino] = kids

    def hash_version(self, ino, blocks):
        return self._hv.get(ino, 1)

    def check_htree(self, ino, inode, blocks):
        fs = self.fs
        self.leaf_ranges = {}
        if not hasattr(self, '_hv'):
            self._hv = {}
        if 0 not in blocks:
            self.err('htree %d has no root' % ino); return set()
        root = fs.rb(blocks[0])
        reserved, hv, info_len, levels, flags = struct.unpack_from('<IBBBB', root, 24)
        if info_len != 8 or levels > 2:
            self.err('htree %d bad root info' % ino); return {0}
        if hv <= 2 and fs.hash_unsigned:
            hv += 3
        self._hv[ino] = hv
        dx = {0}

        def node(lb, blk, at, depth, lo, hi):
            limit, count = struct.unpack_from('<HH', blk, at)
            exp_limit = (BLOCK - at) // 8 - (1 if fs.metadata_csum else 0)
            if limit != exp_limit or count > limit or count == 0:
                self.err('htree %d node %d bad count/limit %d/%d' % (ino, lb, count, limit)); return
            if fs.metadata_csum:
                c, tail = fs.dx_csum(ino, inode.generation, blk, at)
                if struct.unpack_from('<I', blk, tail + 4)[0] != c:
                    self.err('htree %d node %d checksum' % (ino, lb))
            ents = [(lo, struct.unpack_from('<I', blk, at + 4)[0])]
            for k in range(1, count):
                h, b = struct.unpack_from('<II', blk, at + 8 * k)
                ents.append((h, b))
            for k in range(1, len(ents)):
                if ents[k][0] < ents[k - 1][0]:
                    self.err('htree %d node %d unsorted hashes' % (ino, lb))
            for k, (h, b) in enumerate(ents):
                nhi = ents[k + 1][0] if k + 1 < len(ents) else hi
                if b not in blocks:
                    self.err('htree %d points to missing block %d' % (ino, b)); continue
                if depth:
                    dx.add(b)
                    cb = fs.rb(blocks[b])
                    fd, fr, fn, ff = struct.unpack_from('<IHBB', cb, 0)
                    if fd or fr != BLOCK:
                        self.err('htree %d index node %d bad fake dirent' % (ino, b))
                    node(b, cb, 8, depth - 1, h & ~1, nhi)
                else:
                    self.leaf_ranges[b] = (h & ~1, nhi)
        node(0, root, 32, levels, 0, None)
        return dx


def check_image(path, replay=True, verbose=False):
    c = Checker(path, replay=replay, verbose=verbose)
    errors = c.run()
    return errors, getattr(c, 'stats', {}), c


# ---------------------------------------------------------------- tree listing
def walk(fs, ino=ROOT_INO, prefix=''):
    """Yield (path, ino, inode) for every entry below a directory (depth first)."""
    for name, child, ft in fs.dir_entries(ino):
        if name in (b'.', b'..'):
            continue
        path = prefix + '/' + name.decode('utf-8', 'surrogateescape')
        inode = fs.read_inode(child)
        yield path, child, inode
        if (inode.mode & S_IFMT) == S_IFDIR:
            yield from walk(fs, child, path)
