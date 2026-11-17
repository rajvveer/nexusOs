#!/usr/bin/env python3
"""Install VS Code, Firefox, LibreOffice (Writer + Calc, GTK3) and a shell userland onto a NexusOS disk image.

    python -I arch/x86_64/apps_install.py <disk.img> [--grow MIB] [--cache DIR]
                                          [--only vscode,firefox,libreoffice,shell] [--report PATH]

Artifacts come from arch/x86_64/apps-lock.json; each one is fetched into the cache when missing and is
always verified (size + sha256) before use. Downloads go only to deb.debian.org, Mozilla and the
VS Code update/download hosts. Archives are treated as untrusted data: nothing from them is executed,
member paths are validated, and no maintainer script runs: the few update-alternatives links and
"install from default" copies the apps rely on (e.g. /etc/libreoffice/registry/main.xcd) are recreated
from tables (ALTERNATIVES, CONFIG_COPIES); caches built by triggers (glib schemas, mime, icon, font,
pixbuf loader caches) are listed in the manifest as not generated here: after a GUI group the command line runs
arch/x86_64/apps_postinst.py itself (it builds them inside NexusOS; --no-postinst to skip, e.g. to run it later).

Layout: /opt/vscode (VSCode-linux-x64/*), /usr/bin/code -> /opt/vscode/bin/code; /opt/firefox (firefox/*),
/usr/bin/firefox -> /opt/firefox/firefox-bin; Debian payloads at their Debian paths without documentation
(each package's copyright goes to /share/licenses/<pkg>.txt); merged-usr emulation by /bin/<n>, /sbin/<n>,
/lib/<n> -> /usr/... symlinks; /etc/passwd, /etc/group, /etc/shells when absent; a manifest at
/nexus/apps-installed.json; Aurora launch files /nexus/apps/{vscode,firefox,writer,calc}.args and first-run
defaults from apps-config/ (only when absent). Symlinks are NexusOS FAT symlinks (NT byte 0xA2, content = target).

Selection (--only): vscode and libreoffice also install the shell group (their launchers are shell
scripts); every GUI app also installs the 'common' desktop support packages. Default: everything.

Safety: the image is analysed read-only first (collisions, conflicts, free space); nothing is written when
the analysis finds no change. Otherwise all writes go to a sparse temporary copy next to the image (grown
first with --grow) that replaces the original only after the reserved prefix (bytes 10..16 MiB) has been
verified unchanged. Existing disk files are never overwritten: identical files are skipped, differing ones
are reported as conflicts and kept. The FAT is flushed once. /home/nexus/.config and /home/nexus/.cache
are never read. Stop QEMU before running.
"""
from pathlib import Path, PurePosixPath
import argparse
import array
import calendar
import ctypes
import hashlib
import importlib.util
import io
import json
import os
import posixpath
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = HERE / 'apps-lock.json'
CHROME_LOCK = HERE / 'chrome-lock.json'
DEFAULT_CACHE = nexus_paths.cache('apps')
VERSION = 1

_spec = importlib.util.spec_from_file_location('apps_fatput', ROOT / 'userland' / 'fatput.py')
fatput = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fatput)
Fat32 = fatput.Fat32

SECTOR = 512
BASE = fatput.DATA_LBA * SECTOR          # FAT partition starts after the reserved 16 MiB
NPFS = fatput.LIMIT                      # reserved NPFS/persistence region: 10 MiB .. 16 MiB
EOC = 0x0FFFFFFF
NT_SYMLINK = 0xA2
PATH_MAX = 512
DIR_MAX_SLOTS = 65536                    # FAT caps a directory at 65536 entries (2 MiB)

ALLOWED_HOSTS = {'deb.debian.org', 'archive.mozilla.org', 'ftp.mozilla.org', 'download-installer.cdn.mozilla.net',
                 'product-details.mozilla.org', 'update.code.visualstudio.com', 'vscode.download.prss.microsoft.com'}
GROUPS = {'vscode': {'vscode', 'shell', 'common'}, 'libreoffice': {'libreoffice', 'shell', 'common'},
          'firefox': {'firefox', 'common'}, 'shell': {'shell'}}
APP_TREES = {  # app -> (tarball root, install dir, launcher link, launcher target)
    'vscode': ('VSCode-linux-x64/', 'opt/vscode', 'usr/bin/code', '/opt/vscode/bin/code'),
    # The 'firefox' stub is a non-PIE (ET_EXEC) launcher mapped below 4 GiB, outside NexusOS user space;
    # firefox-bin is the position-independent browser binary it would exec.
    'firefox': ('firefox/', 'opt/firefox', 'usr/bin/firefox', '/opt/firefox/firefox-bin'),
}
DOC_PREFIXES = ('usr/share/doc/', 'usr/share/doc-base/', 'usr/share/man/', 'usr/share/info/', 'usr/share/lintian/',
                'usr/share/bug/', 'usr/share/locale/', 'usr/share/gtk-doc/', 'usr/share/help/', 'usr/share/devhelp/',
                'usr/share/omf/', 'usr/share/reportbug/')
VIRTUAL_TOPS = ('dev', 'proc', 'sys')            # kernel-synthesised trees; an on-disk copy would shadow them
MERGED_TOPS = {'bin': 'usr/bin', 'lib': 'usr/lib', 'lib64': 'usr/lib64', 'sbin': 'usr/sbin'}
TOLERATED_COLLISIONS = ('usr/share/terminfo/',)  # obscure terminal descriptions: first one wins
# update-alternatives links created by maintainer scripts that the apps rely on (package, link, target)
ALTERNATIVES = [
    ('mawk', 'usr/bin/awk', 'mawk'), ('mawk', 'usr/bin/nawk', 'mawk'),
    ('debianutils', 'usr/bin/which', 'which.debianutils'),
    ('pinentry-curses', 'usr/bin/pinentry', 'pinentry-curses'),
    ('libblas3', 'usr/lib/x86_64-linux-gnu/libblas.so.3', 'blas/libblas.so.3'),
    ('liblapack3', 'usr/lib/x86_64-linux-gnu/liblapack.so.3', 'lapack/liblapack.so.3'),
    ('adwaita-icon-theme', 'usr/share/icons/default/index.theme', '/usr/share/icons/Adwaita/cursor.theme'),
]
# files that maintainer scripts copy from a shipped default when absent (package, source, destination)
CONFIG_COPIES = [
    ('libreoffice-common', 'usr/lib/libreoffice/share/.registry/main.xcd', 'etc/libreoffice/registry/main.xcd'),  # ucf
    ('base-files', 'usr/share/base-files/profile', 'etc/profile'),
    ('base-files', 'usr/share/base-files/motd', 'etc/motd'),
    ('base-files', 'usr/share/base-files/dot.profile', 'root/.profile'),
    ('base-files', 'usr/share/base-files/dot.bashrc', 'root/.bashrc'),
    ('readline-common', 'usr/share/readline/inputrc', 'etc/inputrc'),
]
FORBIDDEN = (('home', 'nexus', '.config'), ('home', 'nexus', '.cache'))
# Packages installed only partly: the C.UTF-8 locale data of libc-bin (glibc's setlocale needs it for LANG=C.UTF-8).
ONLY_PATHS = {'libc-bin': ('usr/lib/locale/C.utf8/',)}
# Aurora launcher files (/nexus/apps/<name>.args: KEY=VALUE lines, program, arguments) and first-run defaults,
# copied from arch/x86_64/apps-config/ and written only when absent so user changes survive reinstalls.
# Profiles live outside ~/.config and ~/.cache. (group, guest path, file in apps-config/)
CONFIG_DIR = HERE / 'apps-config'
LAUNCH_FILES = [
    ('vscode', 'nexus/apps/vscode.args', 'vscode.args'),
    ('vscode', 'home/nexus/vscode-data/User/settings.json', 'vscode-settings.json'),
    ('vscode', 'home/nexus/project/README.md', 'project-README.md'),
    ('firefox', 'nexus/apps/firefox.args', 'firefox.args'),
    ('firefox', 'opt/firefox/distribution/policies.json', 'firefox-policies.json'),
    ('firefox', 'home/nexus/firefox-profile/user.js', 'firefox-user.js'),
    ('libreoffice', 'nexus/apps/writer.args', 'writer.args'),
    ('libreoffice', 'nexus/apps/calc.args', 'calc.args'),
    ('libreoffice', 'home/nexus/libreoffice-profile/user/registrymodifications.xcu', 'libreoffice-registrymodifications.xcu'),
]
PASSWD = b'root:x:0:0:root:/root:/bin/bash\nnexus:x:1000:1000:nexus:/home/nexus:/bin/bash\n'
GROUP = b'root:x:0:\ntty:x:5:\nnexus:x:1000:\n'
SHELLS = b'# /etc/shells: valid login shells\n/bin/sh\n/usr/bin/sh\n/bin/bash\n/usr/bin/bash\n/bin/rbash\n/usr/bin/rbash\n/bin/dash\n/usr/bin/dash\n'
_LOWER = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


class InstallError(Exception):
    pass


def fold(name):
    """Case folding exactly like the kernel's name_equal (ASCII A-Z only)."""
    return name.translate(_LOWER)


def log(*args):
    print('[apps]', *args, flush=True)


def fat_stamp(mtime):
    t = time.gmtime(max(315532800, min(int(mtime), 4354819198)))
    return (((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday,
            (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2))


def safe_member(name):
    while name.startswith('./'): name = name[2:]
    name = name.rstrip('/')
    if name in ('', '.'): return ''
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or '\\' in name or '\0' in name:
        raise InstallError('unsafe archive path: %r' % name)
    return p.as_posix()


def name_problem(leaf):
    if not leaf or leaf in ('.', '..'): return 'empty or dot name'
    if leaf[-1] in '. ': return 'ends with dot or space'
    if any(c in '"*/:<>?\\|' or ord(c) < 32 or ord(c) == 127 for c in leaf): return 'character not allowed on FAT'
    if len(leaf.encode('utf-8')) > 255: return 'longer than 255 UTF-8 bytes'
    try: leaf.encode('utf-16-le')
    except UnicodeEncodeError: return 'not encodable as UTF-16'
    return None


# ------------------------------------------------------------------------------------------------
# fetching / verification
# ------------------------------------------------------------------------------------------------
def check_url(url):
    u = urllib.parse.urlsplit(url)
    if u.scheme != 'https' or u.hostname not in ALLOWED_HOSTS:
        raise InstallError('download origin not allowed: ' + url)
    return url


class _Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        check_url(urllib.parse.urljoin(req.full_url, newurl))
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def sha256_file(path):
    with open(path, 'rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()


def fetch(entry, cache):
    path = cache / entry['filename']
    if path.exists() and path.stat().st_size == entry['bytes'] and sha256_file(path) == entry['sha256']:
        return path, False
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_name(path.name + '.part')
    opener = urllib.request.build_opener(_Redirects())
    opener.addheaders = [('User-Agent', 'nexusos-apps-install/%d' % VERSION)]
    digest, received = hashlib.sha256(), 0
    with opener.open(check_url(entry['url']), timeout=120) as response, open(part, 'wb') as out:
        check_url(response.geturl())
        while block := response.read(1 << 20):
            received += len(block)
            if received > entry['bytes']: raise InstallError('oversized download: ' + entry['name'])
            digest.update(block); out.write(block)
    if received != entry['bytes'] or digest.hexdigest() != entry['sha256']:
        part.unlink()
        raise InstallError('verification failed for downloaded ' + entry['name'])
    part.replace(path)
    if sha256_file(path) != entry['sha256']: raise InstallError('cache verification failed: ' + entry['name'])
    return path, True


def deb_data(path):
    """Return (member name, bytes) of a .deb's data.tar.* payload."""
    with open(path, 'rb') as f:
        if f.read(8) != b'!<arch>\n': raise InstallError('not a Debian package: %s' % path)
        while header := f.read(60):
            if len(header) != 60 or header[58:60] != b'`\n': raise InstallError('bad ar member in %s' % path)
            size = int(header[48:58]); name = header[:16].decode('ascii').strip().rstrip('/')
            if name.startswith('data.tar'):
                data = f.read(size)
                if len(data) != size: raise InstallError('truncated payload in %s' % path)
                return name, data
            f.seek(size + (size & 1), 1)
    raise InstallError('no data.tar in %s' % path)


def open_payload(entry, path):
    if entry['kind'] == 'tar':
        mode = 'r|gz' if path.name.endswith('.gz') else 'r|xz' if path.name.endswith('.xz') else \
               'r|bz2' if path.name.endswith('.bz2') else 'r|*'
        return tarfile.open(str(path), mode)
    name, data = deb_data(path)
    ext = name.rsplit('.', 1)[-1]
    mode = {'xz': 'r|xz', 'gz': 'r|gz', 'zst': 'r|zst', 'bz2': 'r|bz2', 'tar': 'r|'}.get(ext)
    if mode is None: raise InstallError('unsupported payload compression %s in %s' % (name, path))
    return tarfile.open(fileobj=io.BytesIO(data), mode=mode)


# ------------------------------------------------------------------------------------------------
# FAT32 volume engine (batched: in-memory FAT + directory images, single flush)
# ------------------------------------------------------------------------------------------------
class Entry:
    __slots__ = ('name', 'attr', 'first', 'size', 'nt', 'slot', 'target')

    def __init__(self, name, attr, first, size, nt, slot, target=None):
        self.name, self.attr, self.first, self.size, self.nt, self.slot, self.target = name, attr, first, size, nt, slot, target

    @property
    def is_dir(self): return bool(self.attr & 0x10)

    @property
    def is_link(self): return not self.attr & 0x10 and self.nt == NT_SYMLINK

    @property
    def is_special(self): return not self.attr & 0x10 and (self.nt & 0xE0) == 0xA0 and self.nt != NT_SYMLINK


class Dir:
    def __init__(self, first, clusters, data, path):
        self.first, self.clusters, self.data, self.path = first, clusters, data, path
        self.names, self.shorts, self.end, self.dirty, self.next_short = {}, set(), 0, False, 1


class Volume:
    def __init__(self, path, writable):
        self.path, self.writable = Path(path), writable
        self.f = open(path, 'r+b' if writable else 'rb')
        self.f.seek(0, 2); self.disk_size = self.f.tell()
        self.f.seek(0); mbr = self.f.read(SECTOR)
        if mbr[510:512] != b'\x55\xaa' or mbr[446 + 4] not in (0x0B, 0x0C):
            raise InstallError('disk has no MBR FAT32 partition (expanded NexusOS layout required)')
        start, sectors = struct.unpack_from('<II', mbr, 446 + 8)
        if start * SECTOR != BASE: raise InstallError('FAT partition must start at LBA %d' % fatput.DATA_LBA)
        self.base, self.limit = BASE, sectors * SECTOR
        if self.base + self.limit > self.disk_size: raise InstallError('partition extends beyond the disk')
        bpb = self.rd(0, SECTOR)
        if not Fat32.valid_bpb(bpb): raise InstallError('invalid FAT32 boot sector')
        self.spc = bpb[13]; self.rsv = struct.unpack_from('<H', bpb, 14)[0]; self.nfats = bpb[16]
        self.total = struct.unpack_from('<I', bpb, 32)[0]; self.fatsz = struct.unpack_from('<I', bpb, 36)[0]
        self.rootc = struct.unpack_from('<I', bpb, 44)[0]; self.fsinfo = struct.unpack_from('<H', bpb, 48)[0]
        self.data_lba = self.rsv + self.nfats * self.fatsz
        self.cl = self.spc * SECTOR
        self.nclusters = (self.total - self.data_lba) // self.spc
        if (self.nfats not in (1, 2) or self.rsv < 2 or self.data_lba >= self.total or self.total * SECTOR > self.limit
                or not 2 <= self.rootc < self.nclusters + 2 or (self.nclusters + 2) * 4 > self.fatsz * SECTOR):
            raise InstallError('invalid FAT32 geometry')
        self.fat = bytearray(self.rd(self.rsv * SECTOR, self.fatsz * SECTOR))
        entries = array.array('I'); entries.frombytes(bytes(self.fat[:(self.nclusters + 2) * 4]))
        self.free = sum(1 for x in entries[2:] if not x & 0x0FFFFFFF)
        self.free_start = self.free
        self.cursor = 2
        self.fat_dirty = False
        self.dirs = {}
        self.virtual = -1          # dry-run cluster ids are negative
        self.allocated = 0         # clusters allocated (real or virtual)
        self.root = self.load_dir(self.rootc, '/')

    # ---- raw I/O ----------------------------------------------------------------------------------
    def rd(self, off, n):
        if off < 0 or n < 0 or off + n > self.limit: raise InstallError('read outside the FAT partition')
        self.f.seek(self.base + off); data = self.f.read(n)
        if len(data) != n: raise InstallError('short read')
        return data

    def wr(self, off, data):
        if not self.writable: raise InstallError('internal error: write in analysis mode')
        if off < 0 or off + len(data) > self.limit: raise InstallError('write outside the FAT partition')
        self.f.seek(self.base + off); self.f.write(data)

    def cl_off(self, c):
        return (self.data_lba + (c - 2) * self.spc) * SECTOR

    def fat_get(self, c):
        return struct.unpack_from('<I', self.fat, c * 4)[0] & 0x0FFFFFFF

    def fat_set(self, c, v):
        old = struct.unpack_from('<I', self.fat, c * 4)[0] & 0xF0000000
        struct.pack_into('<I', self.fat, c * 4, old | (v & 0x0FFFFFFF)); self.fat_dirty = True

    def chain(self, c):
        out, seen = [], set()
        while 2 <= c < 0x0FFFFFF8:
            if c >= self.nclusters + 2 or c in seen: raise InstallError('invalid or cyclic FAT chain')
            seen.add(c); out.append(c); c = self.fat_get(c)
        return out

    def runs(self, clusters):
        out = []
        for c in clusters:
            if out and out[-1][0] + out[-1][1] == c: out[-1][1] += 1
            else: out.append([c, 1])
        return out

    def read_file(self, first, size):
        """Yield the content of a chain in large chunks."""
        if size == 0 or first < 2: return
        remaining = size
        for c, n in self.runs(self.chain(first)):
            while n and remaining > 0:
                k = min(n, 2048)
                data = self.rd(self.cl_off(c), k * self.cl)
                yield data[:remaining]
                remaining -= len(data); c += k; n -= k
            if remaining <= 0: break
        if remaining > 0: raise InstallError('cluster chain shorter than file size')

    # ---- allocation --------------------------------------------------------------------------------
    def alloc(self, n):
        """Allocate an n-cluster chain; returns the cluster list (negative ids in analysis mode)."""
        if n <= 0: return []
        if self.writable and n > self.free: raise InstallError('FAT32 volume full (%d clusters needed, %d free)' % (n, self.free))
        self.free -= n; self.allocated += n
        if not self.writable:
            out = list(range(self.virtual, self.virtual - n, -1)); self.virtual -= n; return out
        limit = (self.nclusters + 2) * 4
        zero = bytes(4 * n)
        for start in (self.cursor, 2):
            pos = self.fat.find(zero, start * 4, limit)
            while pos != -1 and pos % 4:
                pos = self.fat.find(zero, pos + 4 - pos % 4, limit)
            if pos != -1:
                c = pos // 4
                links = array.array('I', range(c + 1, c + n + 1)); links[-1] = EOC
                self.fat[pos:pos + 4 * n] = links.tobytes(); self.fat_dirty = True
                self.cursor = c + n
                return list(range(c, c + n))
        got, c = [], 2                              # fragmented fallback
        while len(got) < n:
            if c >= self.nclusters + 2: raise InstallError('FAT free count inconsistent')
            if not self.fat_get(c): got.append(c)
            c += 1
        for i, x in enumerate(got): self.fat_set(x, got[i + 1] if i + 1 < n else EOC)
        return got

    # ---- directories -------------------------------------------------------------------------------
    def load_dir(self, first, path):
        if first in self.dirs: return self.dirs[first]
        parts = tuple(fold(p) for p in path.strip('/').split('/') if p)
        if any(parts[:len(f)] == f for f in FORBIDDEN): raise InstallError('refusing to read ' + path)
        clusters = self.chain(first)
        if len(clusters) * self.cl > DIR_MAX_SLOTS * 32: raise InstallError('oversized directory ' + path)
        data = bytearray()
        for c, n in self.runs(clusters): data += self.rd(self.cl_off(c), n * self.cl)
        d = Dir(first, clusters, data, path)
        parts_lfn, expected, checksum = {}, 0, 0
        nslots = len(data) // 32
        d.end = nslots
        for i in range(nslots):
            e = data[i * 32:(i + 1) * 32]
            if e[0] == 0: d.end = i; break
            if e[0] == 0xE5: parts_lfn, expected = {}, 0; continue
            if e[11] == 0x0F:
                order = e[0] & 31
                if e[0] & 64 and 1 <= order <= 20: parts_lfn, expected, checksum = {}, order, e[13]
                if not expected or order != expected or e[13] != checksum or e[12] or e[26:28] != b'\0\0':
                    parts_lfn, expected = {}, 0; continue
                parts_lfn[order] = e[1:11] + e[14:26] + e[28:32]; expected -= 1
                continue
            d.shorts.add(bytes(e[:11]))
            if e[11] & 0x08: parts_lfn, expected = {}, 0; continue
            name = Fat32.from83(e)
            if parts_lfn and not expected and checksum == Fat32.name_checksum(e[:11]):
                raw = b''.join(parts_lfn[k] for k in sorted(parts_lfn))
                units = [raw[k:k + 2] for k in range(0, len(raw), 2)]
                units = units[:next((k for k, x in enumerate(units) if x in (b'\0\0', b'\xff\xff')), len(units))]
                try: name = b''.join(units).decode('utf-16-le')
                except UnicodeDecodeError: pass
            parts_lfn, expected = {}, 0
            if name in ('.', '..'): continue
            first_c = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            d.names.setdefault(fold(name), Entry(name, e[11], first_c, struct.unpack_from('<I', e, 28)[0], e[12], i))
        self.dirs[first] = d
        return d

    def short_name(self, d, name):
        try:
            short = Fat32.to83(name)
            if name.isascii() and Fat32.from83(short).casefold() == name.casefold() and short not in d.shorts:
                return short
        except (SystemExit, UnicodeEncodeError):
            pass
        ext = name.rsplit('.', 1)[1] if '.' in name else ''
        ext = ''.join(c.upper() if c.isascii() and c.isalnum() else '_' for c in ext[:3]).ljust(3).encode()
        while True:
            short = ('N%07d' % d.next_short).encode() + ext; d.next_short += 1
            if short not in d.shorts: return short

    def append_slots(self, d, raw_entries):
        need = len(raw_entries)
        total = len(d.data) // 32
        if d.end + need > DIR_MAX_SLOTS: raise InstallError('directory %s would exceed 65536 entries' % d.path)
        if d.end + need > total:
            extra = ((d.end + need - total) * 32 + self.cl - 1) // self.cl
            new = self.alloc(extra)
            if self.writable:
                self.fat_set(d.clusters[-1], new[0])
            d.clusters += new
            d.data += bytes(extra * self.cl)
        for k, raw in enumerate(raw_entries):
            d.data[(d.end + k) * 32:(d.end + k + 1) * 32] = raw
        d.end += need
        if d.end < len(d.data) // 32: d.data[d.end * 32] = 0
        d.dirty = True
        return d.end - 1

    def add_entry(self, d, name, attr, first, size, nt, mtime, target=None):
        short = self.short_name(d, name)
        dd, tt = fat_stamp(mtime)
        wide = name.encode('utf-16-le')
        count = (len(wide) // 2 + 12) // 13 if name != Fat32.from83(short) else 0
        wide = (wide + b'\0\0').ljust(count * 26, b'\xff')
        raws = []
        checksum = Fat32.name_checksum(short)
        for i in range(count):
            order = count - i
            e = bytearray(32)
            e[0] = order | (64 if i == 0 else 0); e[11] = 15; e[13] = checksum
            chunk = wide[(order - 1) * 26:order * 26]
            e[1:11], e[14:26], e[28:32] = chunk[:10], chunk[10:22], chunk[22:26]
            raws.append(bytes(e))
        e = bytearray(32)
        e[0:11] = short; e[11] = attr; e[12] = nt
        fc = max(first, 0)
        struct.pack_into('<HHHHHHHI', e, 14, tt, dd, dd, (fc >> 16) & 0xFFFF, tt, dd, fc & 0xFFFF, size)
        raws.append(bytes(e))
        slot = self.append_slots(d, raws)
        d.shorts.add(bytes(short))
        ent = Entry(name, attr, first, size, nt, slot, target)
        d.names[fold(name)] = ent
        return ent

    def set_entry_data(self, d, ent, first, size, mtime):
        o = ent.slot * 32
        dd, tt = fat_stamp(mtime)
        fc = max(first, 0)
        struct.pack_into('<HH', d.data, o + 20, (fc >> 16) & 0xFFFF, tt)
        struct.pack_into('<HHI', d.data, o + 24, dd, fc & 0xFFFF, size)
        ent.first, ent.size = first, size; d.dirty = True

    def mkdir(self, parent, name, mtime):
        cl = self.alloc(1)
        nc = cl[0]
        data = bytearray(self.cl)
        dd, tt = fat_stamp(mtime)
        for k, (nm, target) in enumerate(((b'.          ', nc), (b'..         ', 0 if parent.first == self.rootc else parent.first))):
            e = bytearray(32); e[0:11] = nm; e[11] = 0x10
            t = max(target, 0)
            struct.pack_into('<HHHHHHHI', e, 14, tt, dd, dd, (t >> 16) & 0xFFFF, tt, dd, t & 0xFFFF, 0)
            data[k * 32:(k + 1) * 32] = e
        child = Dir(nc, cl, data, posixpath.join(parent.path, name))
        child.end = 2; child.dirty = True
        child.shorts.update((b'.          ', b'..         '))
        self.dirs[nc] = child
        self.add_entry(parent, name, 0x10, nc, 0, 0, mtime)
        return child

    def write_file(self, d, name, size, chunks, mtime, nt=0, target=None):
        n = (size + self.cl - 1) // self.cl
        clusters = self.alloc(n)
        written = 0
        if self.writable:
            pending = bytearray()
            runs = iter(self.runs(clusters))
            run = None
            pos = 0     # cluster index into the current run
            for block in chunks:
                pending += block
                while len(pending) >= self.cl or (written + len(pending) == size and pending):
                    if run is None or pos >= run[1]:
                        run = next(runs, None); pos = 0
                        if run is None: raise InstallError('more data than declared for %s/%s' % (d.path, name))
                    k = min(run[1] - pos, max(len(pending) // self.cl, 1))
                    piece = bytes(pending[:k * self.cl])
                    del pending[:k * self.cl]
                    self.wr(self.cl_off(run[0] + pos), piece.ljust(k * self.cl, b'\0'))
                    written += len(piece); pos += k
            if written != size: raise InstallError('size mismatch while writing %s/%s' % (d.path, name))
        else:
            for block in chunks: written += len(block)
            if written != size: raise InstallError('size mismatch while reading %s/%s' % (d.path, name))
        return self.add_entry(d, name, 0x20, clusters[0] if clusters else 0, size, nt, mtime, target)

    def flush(self):
        if not self.writable: return
        for d in self.dirs.values():
            if not d.dirty: continue
            k = 0
            for c, n in self.runs(d.clusters):
                self.wr(self.cl_off(c), bytes(d.data[k * self.cl:(k + n) * self.cl])); k += n
            d.dirty = False
        if self.fat_dirty:
            for i in range(self.nfats): self.wr((self.rsv + i * self.fatsz) * SECTOR, bytes(self.fat))
            self.fat_dirty = False
        fsi = bytearray(self.rd(self.fsinfo * SECTOR, SECTOR)) if 0 < self.fsinfo < self.rsv else None
        if fsi and struct.unpack_from('<I', fsi, 0)[0] == 0x41615252 and struct.unpack_from('<I', fsi, 488)[0] != 0xFFFFFFFF:
            struct.pack_into('<II', fsi, 488, self.free, self.cursor)
            self.wr(self.fsinfo * SECTOR, bytes(fsi))
        self.f.flush(); os.fsync(self.f.fileno())

    def close(self):
        if not self.f.closed: self.f.close()


# ------------------------------------------------------------------------------------------------
# growing (block-level: keeps every cluster number, so no directory is walked or rewritten)
# ------------------------------------------------------------------------------------------------
def set_sparse(f):
    if os.name != 'nt': return
    import msvcrt
    returned = ctypes.c_ulong()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                       ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p]
    if not kernel.DeviceIoControl(msvcrt.get_osfhandle(f.fileno()), 0x900C4, None, 0, None, 0,
                                  ctypes.byref(returned), None):
        raise ctypes.WinError(ctypes.get_last_error())


def set_size(f, size):
    """Set the end of file without writing zeros (Python's truncate zero-fills on Windows)."""
    f.flush()
    if os.name != 'nt':
        f.truncate(size); return
    import msvcrt
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.SetFilePointerEx.argtypes = [ctypes.c_void_p, ctypes.c_longlong, ctypes.c_void_p, ctypes.c_ulong]
    kernel.SetEndOfFile.argtypes = [ctypes.c_void_p]
    handle = msvcrt.get_osfhandle(f.fileno())
    if not kernel.SetFilePointerEx(handle, size, None, 0) or not kernel.SetEndOfFile(handle):
        raise ctypes.WinError(ctypes.get_last_error())
    f.seek(0)


def sparse_copy(src, dst, length=None):
    zero = bytes(1 << 20)
    with open(src, 'rb') as a, open(dst, 'w+b') as b:
        set_sparse(b)
        size = os.path.getsize(src) if length is None else length
        set_size(b, os.path.getsize(src))
        done = 0
        while done < size:
            block = a.read(min(1 << 20, size - done))
            if not block: raise InstallError('short read while copying the image')
            if (block != zero) if len(block) == len(zero) else any(block):
                b.seek(done); b.write(block)
            done += len(block)


def storage_mbr(size):
    mbr = bytearray(512)
    mbr[446 + 4] = 0x0C
    struct.pack_into('<II', mbr, 446 + 8, fatput.DATA_LBA, (size - BASE) // SECTOR)
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr)


def format_geometry(total, spc):
    """Same layout as fatput.Fat32.format(total) but with the cluster size given."""
    rsv, nfats = 32, 2
    clusters = (total - rsv) // spc
    fatsz = (clusters + 127) // 128
    clusters = (total - rsv - nfats * fatsz) // spc
    return rsv, nfats, fatsz, clusters


def format_structures(total, spc, base_lba):
    """Boot sector, FSInfo, first FAT sector and root label exactly as fatput.Fat32.format writes them."""
    rsv, nfats, fatsz, clusters = format_geometry(total, spc)
    bpb = bytearray(SECTOR)
    bpb[0:3] = b'\xEB\x58\x90'; bpb[3:11] = b'NEXUSOS '
    struct.pack_into('<HBHBHHBHHHII', bpb, 11, 512, spc, rsv, nfats, 0, 0, 0xF8, 0, 63, 16, 0, total)
    struct.pack_into('<I', bpb, 28, base_lba)
    struct.pack_into('<IHHIHH', bpb, 36, fatsz, 0, 0, 2, 1, 6)
    bpb[64] = 0x80; bpb[66] = 0x29
    struct.pack_into('<I', bpb, 67, 0x4E585553)
    bpb[71:82] = b'NEXUSOS    '; bpb[82:90] = b'FAT32   '
    bpb[510] = 0x55; bpb[511] = 0xAA
    fsi = bytearray(SECTOR)
    struct.pack_into('<I', fsi, 0, 0x41615252); struct.pack_into('<I', fsi, 484, 0x61417272)
    struct.pack_into('<I', fsi, 488, 0xFFFFFFFF); struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)
    fsi[510] = 0x55; fsi[511] = 0xAA
    return bytes(bpb), bytes(fsi), (rsv, nfats, fatsz, clusters)


def grow(src_path, dst_path, size_mib):
    size = size_mib * 1024 * 1024
    if size_mib < 64 or size > 128 * 1024 ** 3: raise InstallError('choose 64..131072 MiB')
    disk_size = os.path.getsize(src_path)
    if size <= disk_size: raise InstallError('--grow %d MiB is not larger than the disk (%d MiB)' % (size_mib, disk_size >> 20))
    src = Volume(src_path, writable=False)
    try:
        with open(src_path, 'rb') as f: mbr = f.read(SECTOR)
        if src.base + src.total * SECTOR != disk_size or any(mbr[462:510]):
            raise InstallError('unsupported partition layout; refusing to grow')
        if mbr != storage_mbr(disk_size):
            raise InstallError('MBR differs from the storage.py layout; refusing to grow')
        total = (size - BASE) // SECTOR
        bpb, fsi, (rsv, nfats, fatsz, clusters) = format_structures(total, src.spc, fatput.DATA_LBA)
        if clusters < src.nclusters: raise InstallError('grown volume would be smaller')
        data_lba = rsv + nfats * fatsz
        sparse_copy(src_path, dst_path, length=BASE)
        with open(dst_path, 'r+b') as out:
            set_size(out, size)
            out.seek(0); out.write(storage_mbr(size))
            # reserved region + FATs + root cluster start zeroed (like Fat32.format), then structures
            out.seek(BASE); out.write(bytes(rsv * SECTOR))
            out.seek(BASE); out.write(bpb)
            out.seek(BASE + SECTOR); out.write(fsi)
            out.seek(BASE + 6 * SECTOR); out.write(bpb)
            if src.rootc != 2:
                out.seek(BASE + 44); out.write(struct.pack('<I', src.rootc))
                out.seek(BASE + 6 * SECTOR + 44); out.write(struct.pack('<I', src.rootc))
            fat = bytearray(fatsz * SECTOR)
            struct.pack_into('<III', fat, 0, 0x0FFFFFF8, EOC, EOC)
            fat[8:(src.nclusters + 2) * 4] = src.fat[8:(src.nclusters + 2) * 4]
            for k in range(nfats):
                out.seek(BASE + (rsv + k * fatsz) * SECTOR); out.write(fat)
            new_off = lambda c: BASE + (data_lba + (c - 2) * src.spc) * SECTOR
            entries = array.array('I'); entries.frombytes(bytes(src.fat[:(src.nclusters + 2) * 4]))
            c, end, moved = 2, src.nclusters + 2, 0
            while c < end:
                if not entries[c] & 0x0FFFFFFF: c += 1; continue
                start = c
                while c < end and entries[c] & 0x0FFFFFFF and c - start < 2048: c += 1
                block = src.rd(src.cl_off(start), (c - start) * src.cl)
                out.seek(new_off(start)); out.write(block); moved += len(block)
            out.flush(); os.fsync(out.fileno())
        return {'from_mib': disk_size >> 20, 'to_mib': size_mib, 'moved_bytes': moved,
                'clusters_before': src.nclusters, 'clusters_after': clusters, 'cluster_bytes': src.cl}
    finally:
        src.close()


# ------------------------------------------------------------------------------------------------
# installer
# ------------------------------------------------------------------------------------------------
class Installer:
    def __init__(self, vol, artifacts, paths, groups):
        self.vol, self.artifacts, self.paths, self.groups = vol, artifacts, paths, groups
        self.by_name = {a['name']: a for a in artifacts}
        self.stats = {}              # bucket (app, or 'preinstalled') -> counters
        self.conflicts, self.collisions, self.fatal, self.invalid = [], [], [], []
        self.skipped, self.skipped_samples = {}, {}
        self.special, self.hardlinks, self.merged_dirs = [], [], []
        self.usr_names = {}          # (top, name) -> bucket, for merged-usr emulation
        self.managed_links = []      # installer-made symlinks present with the expected target
        self.links_added, self.created = [], []
        self.new_links = []          # (path, target, package) of every symlink written in this run
        self.config_copies = []
        self.licenses, self.doc_links = {}, {}
        self.changes = 0
        self.dirs_created = 0
        self.manifest = None
        self.lock_sha256 = None

    @staticmethod
    def bucket(art):
        return 'preinstalled' if art.get('preinstalled') else art['app']

    def stat(self, bucket):
        return self.stats.setdefault(bucket, {k: 0 for k in (
            'files_added', 'bytes_added', 'files_identical', 'bytes_identical', 'symlinks_added',
            'symlinks_identical', 'dirs_created', 'conflicts', 'preinstalled_present', 'skipped')})

    def skip(self, reason, path, bucket=None):
        self.skipped[reason] = self.skipped.get(reason, 0) + 1
        s = self.skipped_samples.setdefault(reason, [])
        if len(s) < 25: s.append(path)
        if bucket: self.stat(bucket)['skipped'] += 1

    # ---- path helpers -------------------------------------------------------------------------
    def read_link(self, ent):
        if ent.target is not None: return ent.target
        return b''.join(self.vol.read_file(ent.first, ent.size)).decode('utf-8', 'replace')

    def walk(self, path, create, mtime=None, bucket=None, depth=0):
        """Return the Dir for `path` (relative to /), following directory symlinks; None if absent."""
        d, cur = self.vol.root, []
        for part in [p for p in path.split('/') if p]:
            if part in ('.', '..'): raise InstallError('unexpected path component in ' + path)
            ent = d.names.get(fold(part))
            if ent is None:
                if not create: return None
                cur.append(part)
                full = '/' + '/'.join(cur)
                if self.forbidden(full): raise InstallError('refusing to create ' + full)
                if len(full.encode()) >= PATH_MAX: raise InstallError('path too long: ' + full)
                prob = name_problem(part)
                if prob: raise InstallError('invalid directory name %s: %s' % (full, prob))
                d = self.vol.mkdir(d, part, mtime if mtime is not None else time.time())
                self.changes += 1; self.dirs_created += 1
                if bucket: self.stat(bucket)['dirs_created'] += 1
                continue
            if ent.name != part and ent.is_dir:
                rec = '/%s -> existing %s' % ('/'.join(cur + [part]), ent.name)
                if rec not in self.merged_dirs: self.merged_dirs.append(rec)
            cur.append(ent.name)
            if ent.is_dir:
                d = self.vol.load_dir(ent.first, '/' + '/'.join(cur)); continue
            if ent.is_link:
                if depth > 40: raise InstallError('symlink loop at /' + '/'.join(cur))
                target = self.read_link(ent)
                real = posixpath.normpath(target if target.startswith('/') else '/' + '/'.join(cur[:-1]) + '/' + target).lstrip('/')
                d = self.walk(real, False, depth=depth + 1)
                if d is None:
                    if create: raise InstallError('directory symlink /%s -> %s is dangling' % ('/'.join(cur), target))
                    return None
                cur = [p for p in real.split('/') if p]
                continue
            if create: raise InstallError('/%s is not a directory' % '/'.join(cur))
            return None
        return d

    def resolve(self, path, depth=0):
        """Resolve an absolute path to its final Entry (following symlinks); None if missing/dangling."""
        parent, leaf = posixpath.split(path.strip('/'))
        d = self.walk(parent, False, depth=depth) if parent else self.vol.root
        if d is None: return None
        ent = d.names.get(fold(leaf))
        if ent is None or not ent.is_link: return ent
        if depth > 40: return None
        target = self.read_link(ent)
        return self.resolve(posixpath.normpath(target if target.startswith('/') else '/' + parent + '/' + target), depth + 1)

    @staticmethod
    def forbidden(path):
        parts = tuple(fold(p) for p in path.split('/') if p)
        return any(parts[:len(f)] == f for f in FORBIDDEN)

    # ---- placing entries ----------------------------------------------------------------------
    def existing(self, path, kind, art, bucket, mtime):
        """Returns (Dir, leaf, Entry or None), or None when the member is skipped."""
        if self.forbidden(path): raise InstallError('archive member targets a protected path: /' + path)
        if len(('/' + path).encode()) >= PATH_MAX:
            self.bad(path, 'path longer than 511 bytes', art); return None
        parent, leaf = posixpath.split(path)
        prob = name_problem(leaf)
        if prob: self.bad(path, prob, art); return None
        d = self.walk(parent, True, mtime, bucket)
        ent = d.names.get(fold(leaf))
        if ent is not None and ent.name != leaf:
            if ent.is_dir and kind == 'dir':
                rec = '/%s -> existing %s' % (path, ent.name)
                if rec not in self.merged_dirs: self.merged_dirs.append(rec)
                return d, leaf, ent
            rec = {'path': '/' + path, 'existing': posixpath.join(d.path, ent.name), 'package': art['name']}
            if path.startswith(TOLERATED_COLLISIONS):
                rec['resolution'] = 'existing entry kept (first wins); member skipped'
                self.collisions.append(rec); self.skip('case collision (tolerated area)', '/' + path, bucket)
            else:
                rec['resolution'] = 'FATAL'
                self.collisions.append(rec); self.fatal.append('case collision: /%s vs %s' % (path, rec['existing']))
            return None
        return d, leaf, ent

    def bad(self, path, why, art):
        self.invalid.append({'path': '/' + path, 'problem': why, 'package': art['name']})
        self.fatal.append('invalid name /%s: %s' % (path, why))

    def conflict(self, path, why, art, bucket):
        self.conflicts.append({'path': '/' + path, 'reason': why, 'package': art['name']})
        self.stat(bucket)['conflicts'] += 1

    def place_dir(self, path, art, bucket, mtime):
        r = self.existing(path, 'dir', art, bucket, mtime)
        if r is None: return
        d, leaf, ent = r
        if ent is None:
            self.vol.mkdir(d, leaf, mtime); self.changes += 1; self.dirs_created += 1
            self.stat(bucket)['dirs_created'] += 1
        elif not ent.is_dir and not ent.is_link and not self.by_name.get(art['name'], {}).get('preinstalled'):
            self.conflict(path, 'directory expected, file exists', art, bucket)

    def place_file(self, path, size, chunks, art, bucket, mtime):
        r = self.existing(path, 'file', art, bucket, mtime)
        if r is None:
            for _ in chunks: pass
            return
        d, leaf, ent = r
        st = self.stat(bucket)
        if ent is None:
            self.vol.write_file(d, leaf, size, chunks, mtime)
            self.changes += 1; st['files_added'] += 1; st['bytes_added'] += size
            return
        if art.get('preinstalled'):          # Chrome's package set: only add what is missing
            for _ in chunks: pass
            st['preinstalled_present'] += 1; return
        if ent.is_dir or ent.is_link or ent.is_special or ent.size != size:
            for _ in chunks: pass
            self.conflict(path, 'existing %s (%d bytes) differs from package file (%d bytes)' % (
                'directory' if ent.is_dir else 'symlink' if ent.is_link else 'file', ent.size, size), art, bucket)
            return
        h, g = hashlib.sha256(), hashlib.sha256()
        for block in chunks: h.update(block)
        for block in self.vol.read_file(ent.first, ent.size): g.update(block)
        if h.digest() == g.digest():
            st['files_identical'] += 1; st['bytes_identical'] += size
        else:
            self.conflict(path, 'existing file content differs', art, bucket)

    def place_link(self, path, target, art, bucket, mtime, managed=False):
        if not target or '\0' in target or len(target.encode()) >= PATH_MAX:
            self.bad(path, 'unsupported symlink target %r' % target, art); return False
        r = self.existing(path, 'link', art, bucket, mtime)
        if r is None: return False
        d, leaf, ent = r
        st = self.stat(bucket)
        if ent is None:
            data = target.encode('utf-8')
            self.vol.write_file(d, leaf, len(data), [data], mtime, nt=NT_SYMLINK, target=target)
            self.changes += 1; st['symlinks_added'] += 1
            self.new_links.append((path, target, art['name']))
            if managed: self.links_added.append('/%s -> %s' % (path, target))
            return True
        if art.get('preinstalled'):
            st['preinstalled_present'] += 1; return False
        if ent.is_link and self.read_link(ent) == target:
            st['symlinks_identical'] += 1; return True
        if managed and ent.is_link:   # installer-owned link from an older layout: point it at the new target
            data = target.encode('utf-8')
            if self.vol.writable and ent.first >= 2:
                old_chain = self.vol.chain(ent.first)
                for c in old_chain: self.vol.fat_set(c, 0)
                self.vol.free += len(old_chain)
            clusters = self.vol.alloc((len(data) + self.vol.cl - 1) // self.vol.cl)
            if self.vol.writable:
                off = 0
                for c, k in self.vol.runs(clusters):
                    self.vol.wr(self.vol.cl_off(c), data[off:off + k * self.vol.cl].ljust(k * self.vol.cl, b'\0')); off += k * self.vol.cl
            self.vol.set_entry_data(d, ent, clusters[0] if clusters else 0, len(data), mtime)
            self.changes += 1; st['symlinks_added'] += 1; self.links_added.append('/%s -> %s (retargeted)' % (path, target))
            return True
        if not ent.is_link and not ent.is_dir and ent.first >= 2:
            # a materialized copy of the link target with identical content is equivalent
            want = self.resolve(posixpath.normpath(target if target.startswith('/') else '/' + posixpath.dirname(path) + '/' + target))
            if want is not None and not want.is_dir and want.size == ent.size and want.first >= 2:
                a, b = hashlib.sha256(), hashlib.sha256()
                for x in self.vol.read_file(ent.first, ent.size): a.update(x)
                for x in self.vol.read_file(want.first, want.size): b.update(x)
                if a.digest() == b.digest():
                    st['files_identical'] += 1; st['bytes_identical'] += ent.size; return False
        self.conflict(path, 'existing %s where symlink -> %s expected' % (
            'directory' if ent.is_dir else ('symlink -> ' + self.read_link(ent)) if ent.is_link else 'file', target), art, bucket)
        return False

    # ---- archives -----------------------------------------------------------------------------
    def install_artifact(self, art):
        bucket = self.bucket(art)
        tree = APP_TREES.get(art['name']) if art['kind'] == 'tar' else None
        hard = {}
        with open_payload(art, self.paths[art['name']]) as tar:
            for m in tar:
                name = safe_member(m.name)
                if tree:
                    if name == tree[0].rstrip('/'): continue
                    if not name.startswith(tree[0]): raise InstallError('unexpected member %s in %s' % (name, art['name']))
                    name = tree[1] + '/' + name[len(tree[0]):]
                if not name: continue
                only = ONLY_PATHS.get(art['name'])
                if only and not any(name.startswith(prefix) or prefix.startswith(name.rstrip('/') + '/') for prefix in only):
                    self.skip('outside the parts installed from this package', '/' + name, bucket); continue
                if not tree:
                    top = name.split('/', 1)[0]
                    if top in VIRTUAL_TOPS:
                        self.skip('kernel virtual filesystem path', '/' + name, bucket); continue
                    if name in MERGED_TOPS and m.issym():
                        self.skip('merged-usr top-level link (real directory kept)', '/' + name, bucket); continue
                    if name.startswith('usr/share/doc/'):
                        rest = name[len('usr/share/doc/'):]
                        if m.issym() and '/' not in rest: self.doc_links[art['name']] = m.linkname
                        if rest.count('/') == 1 and rest.endswith('/copyright') and m.isreg() and art['name'] not in self.licenses:
                            self.licenses[art['name']] = 'file'
                            f = tar.extractfile(m)
                            self.place_file('share/licenses/%s.txt' % art['name'], m.size,
                                            iter(lambda: f.read(1 << 20), b''), art, bucket, m.mtime)
                            continue
                    if name.startswith(DOC_PREFIXES):
                        self.skip('documentation/locale', '/' + name, bucket); continue
                    for top_link, base in (('bin', 'usr/bin/'), ('sbin', 'usr/sbin/'), ('lib', 'usr/lib/')):
                        if name.startswith(base):
                            leaf = name[len(base):].split('/', 1)[0]
                            if leaf and not (top_link == 'lib' and leaf == 'x86_64-linux-gnu'):
                                self.usr_names.setdefault((top_link, leaf), art['app'])
                if m.isdir():
                    self.place_dir(name, art, bucket, m.mtime)
                elif m.isreg():
                    f = tar.extractfile(m)
                    self.place_file(name, m.size, iter(lambda: f.read(1 << 20), b''), art, bucket, m.mtime)
                    hard[safe_member(m.name)] = name
                elif m.issym():
                    self.place_link(name, m.linkname, art, bucket, m.mtime)
                elif m.islnk():
                    src = hard.get(safe_member(m.linkname))
                    ent = self.resolve('/' + src) if src else None
                    if ent is None or ent.is_dir:
                        self.special.append({'path': '/' + name, 'type': 'hardlink', 'package': art['name'], 'note': 'target not found'})
                        self.skip('unresolvable hardlink', '/' + name, bucket); continue
                    self.hardlinks.append('/%s (copy of /%s)' % (name, src))
                    chunks = (iter([bytes(ent.size)]) if ent.size else iter([])) if ent.first < 0 else self.vol.read_file(ent.first, ent.size)
                    self.place_file(name, ent.size, chunks, art, bucket, m.mtime)
                else:
                    self.special.append({'path': '/' + name, 'type': m.type.decode('latin1'), 'package': art['name']})
                    self.skip('special file (device/fifo)', '/' + name, bucket)

    # ---- post-install steps ---------------------------------------------------------------------
    def managed_link(self, path, target, bucket, mtime):
        ok = self.place_link(path, target, {'name': 'apps_install', 'app': bucket}, bucket, mtime, managed=True)
        if ok: self.managed_links.append('/%s -> %s' % (path, target))

    def finish(self, versions):
        now = time.time()
        names = set(self.by_name)
        for app, (root, dest, link, target) in APP_TREES.items():
            if app in self.groups:
                self.managed_link(link, target, app, now)
                self.usr_names.setdefault(('bin', link.rsplit('/', 1)[1]), app)
        for pkg, link, target in ALTERNATIVES:      # update-alternatives emulation
            if pkg not in names: continue
            app = self.by_name[pkg]['app']
            self.managed_link(link, target, app, now)
            for top, base in (('bin', 'usr/bin/'), ('sbin', 'usr/sbin/')):
                if link.startswith(base): self.usr_names.setdefault((top, link[len(base):]), app)
        for (top, n), app in sorted(self.usr_names.items()):     # merged-usr emulation
            target = '/usr/%s/%s' % (top, n)
            if top == 'bin' and n == 'sh' and 'dash' in names: target = '/usr/bin/dash'
            d = self.walk(top, False)
            ent = d.names.get(fold(n)) if d else None
            if ent is not None and not (ent.is_link and ent.name == n and self.read_link(ent) == target):
                self.skip('merged-usr link: existing entry kept', '/%s/%s' % (top, n)); continue
            self.managed_link('%s/%s' % (top, n), target, app, now)
        for pkg, target in sorted(self.doc_links.items()):       # licenses of doc-dir-symlink packages
            if pkg in self.licenses: continue
            other = posixpath.basename(target.rstrip('/'))
            if self.licenses.get(other) == 'file':
                art = self.by_name[pkg]
                self.place_link('share/licenses/%s.txt' % pkg, '%s.txt' % other, art, self.bucket(art), now)
                self.licenses[pkg] = 'link:' + other
        for pkg, a in self.by_name.items():
            if a['kind'] == 'deb' and pkg not in self.licenses: self.licenses[pkg] = 'none'
        for pkg, src, dst in CONFIG_COPIES:                       # maintainer-script default copies
            if pkg not in names: continue
            art = self.by_name[pkg]
            source = self.resolve('/' + src)
            if source is None or source.is_dir:
                self.config_copies.append('/%s: source /%s missing' % (dst, src)); continue
            r = self.existing(dst, 'file', art, self.bucket(art), now)
            if r is None: continue
            d, leaf, ent = r
            if ent is not None:
                self.config_copies.append('/%s: present (kept)' % dst); continue
            chunks = self.vol.read_file(source.first, source.size) if source.first >= 2 else \
                (iter([bytes(source.size)]) if source.size else iter([]))
            self.vol.write_file(d, leaf, source.size, chunks, now)
            self.changes += 1; self.created.append('/%s (copy of /%s)' % (dst, src))
            self.config_copies.append('/%s: created from /%s' % (dst, src))
        for app, path, source in LAUNCH_FILES:                    # Aurora launchers and first-run defaults
            if app not in self.groups: continue
            data = (CONFIG_DIR / source).read_bytes().replace(b'\r\n', b'\n')   # guest files stay LF after a CRLF checkout
            r = self.existing(path, 'file', {'name': 'apps_install', 'app': app}, app, now)
            if r is None: continue
            d, leaf, ent = r
            if ent is not None:
                self.config_copies.append('/%s: present (kept)' % path); continue
            self.vol.write_file(d, leaf, len(data), [data], now)
            self.changes += 1; self.created.append('/' + path); self.config_copies.append('/%s: created' % path)
        d = self.walk('etc', True, now, 'shell')                  # accounts, only when absent
        for name, data in (('passwd', PASSWD), ('group', GROUP), ('shells', SHELLS)):
            if fold(name) not in d.names:
                self.vol.write_file(d, name, len(data), [data], now)
                self.changes += 1; self.created.append('/etc/' + name)
        self.write_manifest(versions, now)

    def write_manifest(self, versions, now):
        d = self.walk('nexus', True, now, 'common')
        ent = d.names.get(fold('apps-installed.json'))
        old = {}
        if ent is not None:
            if ent.is_dir or ent.is_link or ent.is_special: raise InstallError('/nexus/apps-installed.json is not a regular file')
            try: old = json.loads(b''.join(self.vol.read_file(ent.first, ent.size)) or b'{}')
            except ValueError: old = {}
        apps = dict(old.get('apps', {}))
        for app, info in versions.items():
            st = self.stats.get(app, {})
            apps[app] = dict(info, files=st.get('files_added', 0) + st.get('files_identical', 0),
                             bytes=st.get('bytes_added', 0) + st.get('bytes_identical', 0),
                             symlinks=st.get('symlinks_added', 0) + st.get('symlinks_identical', 0))
        packages = {p['name']: p for p in old.get('debian_packages', [])}
        for a in self.artifacts:
            if a['kind'] == 'deb':
                packages[a['name']] = {'name': a['name'], 'version': a['version'], 'app': a['app'],
                                       'preinstalled': bool(a.get('preinstalled'))}
        manifest = {'format': VERSION, 'installer': 'arch/x86_64/apps_install.py', 'lock_sha256': self.lock_sha256,
                    'apps': dict(sorted(apps.items())), 'debian_packages': [packages[k] for k in sorted(packages)],
                    'symlinks_created': sorted(set(old.get('symlinks_created', [])) | set(self.managed_links)),
                    'post_install_not_run': ['glib-compile-schemas /usr/share/glib-2.0/schemas',
                                             'update-mime-database /usr/share/mime',
                                             'gdk-pixbuf-query-loaders --update-cache',
                                             'gtk-update-icon-cache -f /usr/share/icons/hicolor /usr/share/icons/Adwaita',
                                             'fc-cache']}
        data = (json.dumps(manifest, indent=1) + '\n').encode()
        self.manifest = manifest
        if ent is None:
            self.vol.write_file(d, 'apps-installed.json', len(data), [data], now); self.changes += 1
            return
        if b''.join(self.vol.read_file(ent.first, ent.size)) == data: return
        if self.vol.writable and ent.first >= 2:          # replace the installer-owned manifest
            old_chain = self.vol.chain(ent.first)
            for c in old_chain: self.vol.fat_set(c, 0)
            self.vol.free += len(old_chain)
        clusters = self.vol.alloc((len(data) + self.vol.cl - 1) // self.vol.cl)
        if self.vol.writable:
            off = 0
            for c, k in self.vol.runs(clusters):
                self.vol.wr(self.vol.cl_off(c), data[off:off + k * self.vol.cl].ljust(k * self.vol.cl, b'\0')); off += k * self.vol.cl
        self.vol.set_entry_data(d, ent, clusters[0] if clusters else 0, len(data), now)
        self.changes += 1


def check_links(inst, paths):
    out = {}
    for p in paths:
        ent = inst.resolve(p)
        out[p] = 'missing or dangling' if ent is None else 'directory' if ent.is_dir else 'file %d bytes' % ent.size
    return out


def run(inst, artifacts, versions):
    for art in artifacts:
        inst.install_artifact(art)
    inst.finish(versions)


def prefix_digest(path, start, end, skip=None):
    with open(path, 'rb') as f:
        f.seek(start); data = bytearray(f.read(end - start))
    if skip: data[skip[0] - start:skip[1] - start] = bytes(skip[1] - skip[0])
    return hashlib.sha256(data).hexdigest()


def write_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(report, indent=1) + '\n').encode())


def summary(inst):
    keys = ('files_added', 'bytes_added', 'files_identical', 'bytes_identical', 'symlinks_added',
            'symlinks_identical', 'dirs_created', 'conflicts', 'preinstalled_present', 'skipped')
    return {k: sum(s.get(k, 0) for s in inst.stats.values()) for k in keys}


def details(inst):
    return {'apps': inst.stats, 'totals': summary(inst), 'dirs_created': inst.dirs_created,
            'conflicts': inst.conflicts, 'collisions': inst.collisions, 'invalid_names': inst.invalid,
            'skipped': inst.skipped, 'skipped_samples': inst.skipped_samples, 'special_files': inst.special,
            'hardlinks_materialized': inst.hardlinks, 'merged_case_dirs': inst.merged_dirs,
            'licenses': inst.licenses, 'managed_symlinks_added': inst.links_added, 'files_created': inst.created,
            'config_copies': inst.config_copies,
            'dangling_symlinks': getattr(inst, 'dangling', [])}


def dangling_links(inst):
    """Symlinks written in this run whose target does not resolve (e.g. into skipped documentation)."""
    out = []
    for path, target, pkg in inst.new_links:
        if inst.resolve('/' + path) is None:
            out.append({'path': '/' + path, 'target': target, 'package': pkg,
                        'why': 'points into skipped documentation' if '/share/doc/' in posixpath.normpath(
                            posixpath.join('/' + posixpath.dirname(path), target)) else 'target not installed'})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('disk')
    ap.add_argument('--grow', type=int, metavar='MIB', help='grow the disk to MIB MiB before installing')
    ap.add_argument('--cache', default=str(DEFAULT_CACHE), help='artifact cache (default %(default)s)')
    ap.add_argument('--only', default='vscode,firefox,libreoffice,shell', help='comma list of vscode,firefox,libreoffice,shell')
    ap.add_argument('--report', default=None, help='host-side JSON report (default <cache>/install-report.json)')
    ap.add_argument('--no-postinst', action='store_true', help='do not run apps_postinst.py after a GUI group (command line only)')
    args = ap.parse_args(argv)
    t0 = time.time()
    timings = {}
    disk = Path(args.disk).resolve()
    if not disk.is_file(): raise SystemExit('no such disk image: %s' % disk)
    cache = Path(args.cache).resolve()
    report_path = Path(args.report).resolve() if args.report else cache / 'install-report.json'
    wanted = [x.strip() for x in args.only.split(',') if x.strip()]
    for x in wanted:
        if x not in GROUPS: raise SystemExit('unknown --only item %r (choose from vscode,firefox,libreoffice,shell)' % x)
    if not wanted: raise SystemExit('--only selects nothing')
    groups = set().union(*(GROUPS[x] for x in wanted))
    lock_bytes = LOCK.read_bytes()
    lock_sha = hashlib.sha256(lock_bytes).hexdigest()
    lock = json.loads(lock_bytes)
    chrome = {p['name'] for p in json.loads(CHROME_LOCK.read_bytes())} if CHROME_LOCK.exists() else set()
    for e in lock:
        if e['kind'] == 'deb' and e['name'] in chrome: e['preinstalled'] = True
    by_name = {e['name']: e for e in lock}
    debs = sorted((dict(e) for e in lock if e['kind'] == 'deb' and set(e['needed_by']) & groups), key=lambda e: e['name'])
    tars = sorted((dict(e) for e in lock if e['kind'] == 'tar' and e['app'] in groups), key=lambda e: e['name'])
    artifacts = debs + tars
    log('selected %d artifacts (%d debs, %d app trees) for groups %s' % (len(artifacts), len(debs), len(tars), ','.join(sorted(groups))))

    # 1. fetch + verify every artifact (size + sha256 from the lock file)
    paths, fetched = {}, 0
    for e in artifacts:
        p, new = fetch(e, cache); paths[e['name']] = p; fetched += new
    timings['fetch_verify_s'] = round(time.time() - t0, 1)
    log('artifacts verified (%d downloaded, %.0f MiB) in %.1fs' % (fetched, sum(e['bytes'] for e in artifacts) / 2**20, timings['fetch_verify_s']))
    versions = {}
    for app, pkg in (('vscode', 'vscode'), ('firefox', 'firefox'), ('libreoffice', 'libreoffice-core')):
        if app in groups: versions[app] = {'version': by_name[pkg]['version']}
    if 'shell' in groups:
        versions['shell'] = {'version': ', '.join('%s %s' % (n, by_name[n]['version']) for n in ('bash', 'dash', 'coreutils'))}
    if 'common' in groups: versions['common'] = {'version': 'Debian trixie desktop support packages'}
    report = {'disk': str(disk), 'only': wanted, 'groups': sorted(groups), 'lock_sha256': lock_sha,
              'artifacts': [{'name': e['name'], 'version': e['version'], 'app': e['app'], 'kind': e['kind'],
                             'preinstalled': bool(e.get('preinstalled'))} for e in artifacts]}

    # 2. read-only analysis of the image as it is now
    st0 = disk.stat()
    head0 = prefix_digest(disk, 0, BASE)
    t1 = time.time()
    vol = Volume(disk, writable=False)
    try:
        free_before = vol.free_start * vol.cl
        ana = Installer(vol, artifacts, paths, groups); ana.lock_sha256 = lock_sha
        run(ana, artifacts, versions)
        needed, free_clusters, cl, spc, ncl = vol.allocated, vol.free_start, vol.cl, vol.spc, vol.nclusters
    finally:
        vol.close()
    timings['analysis_s'] = round(time.time() - t1, 1)
    log('analysis: %d changes, %.1f MiB to allocate, %.1f MiB free, %d conflicts, %d collisions, %d fatal (%.1fs)' % (
        ana.changes, needed * cl / 2**20, free_clusters * cl / 2**20, len(ana.conflicts), len(ana.collisions),
        len(ana.fatal), timings['analysis_s']))
    if ana.fatal:
        report.update(result='aborted', fatal=ana.fatal, **details(ana))
        write_report(report_path, report)
        for x in ana.fatal[:50]: log('FATAL', x)
        raise SystemExit('installation aborted before writing: %d fatal problems (see %s)' % (len(ana.fatal), report_path))
    do_grow, grow_clusters = False, 0
    if args.grow:
        new_size = args.grow * 1024 * 1024
        if new_size < st0.st_size: raise SystemExit('--grow %d MiB is smaller than the disk (%d MiB)' % (args.grow, st0.st_size >> 20))
        do_grow = new_size > st0.st_size
        if do_grow: grow_clusters = format_geometry((new_size - BASE) // SECTOR, spc)[3] - ncl
    if needed > free_clusters + grow_clusters:
        report.update(result='aborted: not enough space', needed_bytes=needed * cl, free_bytes=(free_clusters + grow_clusters) * cl)
        write_report(report_path, report)
        raise SystemExit('not enough space: need %.1f MiB, have %.1f MiB%s' % (
            needed * cl / 2**20, (free_clusters + grow_clusters) * cl / 2**20, '' if do_grow else ' (use --grow MIB)'))
    if ana.changes == 0 and not do_grow:
        timings['total_s'] = round(time.time() - t0, 1)
        report.update(result='no changes', changes=0, timings=timings, free_bytes_before=free_before,
                      free_bytes_after=free_before, manifest=ana.manifest, **details(ana))
        write_report(report_path, report)
        log('nothing to do: everything is already installed; image left untouched (%.1fs); report %s' % (timings['total_s'], report_path))
        return 0

    # 3. temporary copy (grown, or a sparse copy), apply, verify, replace
    temp = disk.with_name('.apps-install-%d.img' % os.getpid())
    grow_info = None
    try:
        t2 = time.time()
        if do_grow:
            grow_info = grow(disk, temp, args.grow)
            timings['grow_s'] = round(time.time() - t2, 1)
            log('grown %d -> %d MiB in %.1fs (%.0f MiB of allocated clusters moved, cluster numbers kept)' % (
                grow_info['from_mib'], grow_info['to_mib'], timings['grow_s'], grow_info['moved_bytes'] / 2**20))
        else:
            sparse_copy(disk, temp)
            timings['copy_s'] = round(time.time() - t2, 1)
            log('sparse working copy made in %.1fs' % timings['copy_s'])
        t3 = time.time()
        vol = Volume(temp, writable=True)
        try:
            if do_grow and fold('Downloads') not in vol.root.names:     # as storage.grow does
                vol.mkdir(vol.root, 'Downloads', time.time())
            inst = Installer(vol, artifacts, paths, groups); inst.lock_sha256 = lock_sha
            run(inst, artifacts, versions)
            if 'libreoffice' in groups: inst.walk('home/nexus/Documents', True)  # LibreOffice's default save folder
            if inst.fatal: raise InstallError('fatal problems while writing: %r' % inst.fatal[:5])
            vol.flush()
            entries = array.array('I'); entries.frombytes(bytes(vol.fat[:(vol.nclusters + 2) * 4]))
            recount = sum(1 for x in entries[2:] if not x & 0x0FFFFFFF)
            if recount != vol.free: raise InstallError('FAT free count mismatch after install (%d vs %d)' % (recount, vol.free))
            free_after = vol.free * vol.cl
            links = check_links(inst, ['/bin/sh', '/usr/bin/sh', '/bin/bash', '/usr/bin/bash', '/usr/bin/env', '/usr/bin/code',
                                       '/usr/bin/firefox', '/usr/bin/libreoffice', '/usr/lib/x86_64-linux-gnu/liblapack.so.3'])
            inst.dangling = dangling_links(inst)
        finally:
            vol.close()
        timings['apply_s'] = round(time.time() - t3, 1)
        log('applied %d changes in %.1fs' % (inst.changes, timings['apply_s']))
        # protected regions: 10..16 MiB byte-identical; the 16 MiB prefix identical except the MBR sector count (grow)
        skip = (446 + 12, 446 + 16) if do_grow else None
        if prefix_digest(temp, NPFS, BASE) != prefix_digest(disk, NPFS, BASE):
            raise InstallError('NPFS/persistence region (10..16 MiB) changed')
        if prefix_digest(temp, 0, BASE, skip) != prefix_digest(disk, 0, BASE, skip):
            raise InstallError('reserved 16 MiB prefix changed')
        st1 = disk.stat()
        if (st1.st_size, st1.st_mtime_ns) != (st0.st_size, st0.st_mtime_ns) or prefix_digest(disk, 0, BASE) != head0:
            raise InstallError('disk changed during installation; stop QEMU and retry')
        os.replace(temp, disk)
    finally:
        if temp.exists(): temp.unlink()
    timings['total_s'] = round(time.time() - t0, 1)
    report.update(result='installed', changes=inst.changes, timings=timings, grow=grow_info,
                  free_bytes_before=free_before, free_bytes_after=free_after, link_checks=links,
                  manifest=inst.manifest, **details(inst))
    write_report(report_path, report)
    tot = report['totals']
    log('installed: %d files (%.1f MiB) added, %d identical, %d symlinks added, %d dirs, %d conflicts, %d collisions' % (
        tot['files_added'], tot['bytes_added'] / 2**20, tot['files_identical'], tot['symlinks_added'], inst.dirs_created,
        len(inst.conflicts), len(inst.collisions)))
    log('free space %.1f MiB -> %.1f MiB; total %.1fs; report %s' % (free_before / 2**20, free_after / 2**20, timings['total_s'], report_path))
    return 0


def postinst_after(argv):
    """Command line only: the GTK/glib/mime/font caches the GUI groups need (Debian's triggers), built by apps_postinst.py."""
    ap = argparse.ArgumentParser(add_help=False); ap.add_argument('disk'); ap.add_argument('--only', default='vscode,firefox,libreoffice,shell')
    ap.add_argument('--no-postinst', action='store_true')
    args, _ = ap.parse_known_args(argv)
    wanted = [x.strip() for x in args.only.split(',') if x.strip() in GROUPS]
    if args.no_postinst or 'common' not in set().union(*(GROUPS[x] for x in wanted)): return 0
    log('GUI group installed: running apps_postinst.py (GTK, glib, mime and font caches)')
    return subprocess.run([sys.executable, '-I', str(HERE / 'apps_postinst.py'), str(Path(args.disk).resolve())]).returncode


if __name__ == '__main__':
    try:
        rc = main()
        if rc == 0: rc = postinst_after(sys.argv[1:])
        sys.exit(rc)
    except InstallError as e:
        raise SystemExit('apps_install: ' + str(e))
