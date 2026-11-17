#!/usr/bin/env python3
"""fat2ext4.py -- build an ext4 NexusOS disk from a FAT32 NexusOS disk (the source is only read).

    python -I fat2ext4.py <fat.img> <ext4.img> [--size MiB] [--skip /path ...] [--journal-blocks N]
                          [--user UID:GID] [--home /home/nexus]

The new image keeps the NexusOS disk layout: the first 16 MiB (boot, legacy and NPFS regions) are
copied, and the MBR's single partition becomes a Linux (type 0x83) ext4 volume at the same LBA.
Ownership and modes follow a normal Linux system: everything is root:root, directories 0755,
ELF executables / '#!' scripts / the dynamic loader 0755 and other files 0644, /tmp 1777, and the
user's home and runtime directories (and everything below them) belong to --user (1000:1000).
FAT "native kind" markers become real symbolic links and socket nodes; timestamps come from the
FAT write time. /home/nexus/.config and /home/nexus/.cache are skipped without being read
(they can hold a real browser profile); --skip adds more paths. --migrate converts everything (the user's
own disk: profiles included); sysinstall.py migrate uses it and then checks every file.

8.3 names: an 8.3-compatible name is stored without its case; the true case comes from the Debian package
manifests (--case-source), from path lists (--case-list, default userland/fat2ext4-case.txt: the names
Chrome, VS Code and the XDG folders use) and, in the user's home, from LevelDB's fixed names (LOCK, CURRENT,
LOG, LOG.old next to a MANIFEST-*). Home entries left in lower case are listed in the result ('home_lowercased').
"""
import argparse
import importlib.util
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, file))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


ext4lib = load('ext4lib', 'ext4lib.py')
fatput = load('fatput', 'fatput.py')
ext4put = load('ext4put', 'ext4put.py')

DEFAULT_SKIP = ('/home/nexus/.config', '/home/nexus/.cache')
ROOT = os.path.dirname(HERE)
DEFAULT_CASE_SOURCES = (os.path.join(ROOT, 'tools', 'chrome-packages'), os.path.join(ROOT, 'arch', 'x86_64', 'out', 'chrome-root'))
DEFAULT_CASE_LISTS = (os.path.join(HERE, 'fat2ext4-case.txt'),)
LEVELDB_NAMES = {'lock': 'LOCK', 'current': 'CURRENT', 'log': 'LOG', 'log.old': 'LOG.old'}
CHROMIUM_TOP = {'default': 'Default', 'crashpad': 'Crashpad', 'user': 'User', 'cache': 'Cache', 'gpucache': 'GPUCache',
                'backups': 'Backups', 'network': 'Network', 'cookies': 'Cookies'}


class CaseMap:
    """True-case names for FAT 8.3 entries. NexusOS stores an 8.3-compatible name ("X11", "Makefile",
    "lib") as a bare short entry, so the original case is lost (the kernel lists such names in lower
    case). The desktop's system files came from Debian packages: their manifests (and the extracted
    reference tree) give the exact spelling. Unknown names stay lower case."""

    def __init__(self, sources, lists=()):
        import io
        import tarfile
        self.paths, self.names, self.list_paths = {}, {}, {}
        for listing in lists:   # path lists: matched as path suffixes of two or more parts, never by bare name
            if listing and os.path.isfile(listing):
                with open(listing, encoding='utf-8') as f:
                    for line in f:
                        line = line.strip()
                        if line and not line.startswith('#'):
                            parts = [p for p in line.split('/') if p and p != '.']
                            for i in range(1, len(parts)): self.list_paths.setdefault('/'.join(parts[i - 1:i + 1]).lower(), parts[i])
                            if len(parts) > 2: self.list_paths.setdefault('/'.join(parts).lower(), parts[-1])
        for src in sources:
            if not src or not os.path.exists(src):
                continue
            if os.path.isdir(src) and not any(f.endswith('.deb') for f in os.listdir(src)):
                for root, dirs, files in os.walk(src):
                    rel = os.path.relpath(root, src).replace(os.sep, '/')
                    for n in dirs + files:
                        self.add(n if rel == '.' else rel + '/' + n)
                continue
            debs = [os.path.join(src, f) for f in sorted(os.listdir(src)) if f.endswith('.deb')] if os.path.isdir(src) else [src]
            for deb in debs:
                with open(deb, 'rb') as f:
                    data = f.read()
                off = 8
                while off + 60 <= len(data):
                    name = data[off:off + 16].decode('latin1').strip().rstrip('/')
                    size = int(data[off + 48:off + 58].decode().strip())
                    if name.startswith('data.tar'):
                        with tarfile.open(fileobj=io.BytesIO(data[off + 60:off + 60 + size]), mode='r|*') as t:
                            for m in t:
                                self.add(m.name)
                    off += 60 + size + (size & 1)

    def add(self, path):
        parts = [p for p in path.replace(os.sep, '/').split('/') if p and p != '.']
        for i in range(len(parts)):
            self.names.setdefault(parts[i].lower(), set()).add(parts[i])
            self.paths['/'.join(parts[:i + 1]).lower()] = parts[i]

    def restore(self, parent, short):
        """parent: converted path of the directory; short: the 8.3 name as stored."""
        parts = [p for p in (parent + '/' + short).split('/') if p]
        for k in range(len(parts)):  # exact path, then the same tree installed under a prefix
            hit = self.paths.get('/'.join(parts[k:]).lower())
            if hit and hit.lower() == short.lower():
                return hit
        for k in range(len(parts) - 1):  # path lists: the longest suffix of at least parent/name
            hit = self.list_paths.get('/'.join(parts[k:]).lower())
            if hit and hit.lower() == short.lower():
                return hit
        variants = self.names.get(short.lower(), ())
        if len(variants) == 1:
            return next(iter(variants))
        return short.lower()


def fat_time_ns(date, tm):
    if not date:
        return 0
    y, m, d = 1980 + (date >> 9), (date >> 5) & 15, date & 31
    m = m if 1 <= m <= 12 else 1
    d = d if d >= 1 else 1
    hh, mm, ss = tm >> 11, (tm >> 5) & 63, (tm & 31) * 2
    import calendar
    try:
        return calendar.timegm((y, m, d, hh, mm, ss, 0, 0, 0)) * 10**9
    except (ValueError, OverflowError):
        return 0


class Converter:
    def __init__(self, src, dst, uid, gid, home, skips, verbose=False, case=None):
        self.case = case
        self.restored = 0
        self.fat = src
        self.fs = dst
        self.uid, self.gid = uid, gid
        self.home = home.rstrip('/')
        self.skips = {s.rstrip('/').lower() for s in skips}
        self.verbose = verbose
        self.stats = {'files': 0, 'dirs': 0, 'symlinks': 0, 'sockets': 0, 'bytes': 0, 'skipped': [], 'exec': 0, 'home_lowercased': [], 'home_lowercased_count': 0}
        self.progress = None   # progress(files, bytes) after every file (migrate_disk.py)

    def owned(self, path):
        p = path.lower()
        for base in (self.home.lower(), '/run/user/%d' % self.uid):
            if p == base or p.startswith(base + '/'):
                return True
        return False

    def entries(self, dirc, path='/'):
        out = []
        for name, c, off, e, slots in self.fat.named_entries(dirc):
            if name in ('.', '..'):
                continue
            if not slots:  # 8.3-only: case unknown (NexusOS lists it lower-case)
                restored = self.case.restore(path, name) if self.case else name.lower()
                self.restored += restored != name.lower()
                name = restored
            first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            kind = e[12] & 7 if (e[12] & 0xE0) == 0xA0 and not (e[11] & 0x10) else 0
            t = fat_time_ns(struct.unpack_from('<H', e, 24)[0], struct.unpack_from('<H', e, 22)[0])
            out.append((name, bool(e[11] & 0x10), first, size, kind, t, not slots))
        if self.owned(path):   # fixed names of the user's application data (the caller lists what stays lower case)
            names = {o[0] for o in out}
            fixed = dict(LEVELDB_NAMES) if any(n.startswith('MANIFEST-') for n in names) else {}
            if 'Local State' in names: fixed.update(CHROMIUM_TOP)   # a Chromium/Electron user data directory
            if len(path.rstrip('/').rsplit('/', 1)[-1]) == 32 and all('a' <= c <= 'p' for c in path.rstrip('/').rsplit('/', 1)[-1]):
                fixed['icons'] = 'Icons'   # a Chromium extension/web app folder (32 letters a-p)
            out = [((fixed.get(o[0], o[0]) if o[6] and o[0] == o[0].lower() else o[0]),) + o[1:] for o in out]
            for o in out:
                if o[6] and o[0] == o[0].lower() and any(c.isalpha() for c in o[0]):
                    self.stats['home_lowercased_count'] += 1
                    if len(self.stats['home_lowercased']) < 2000: self.stats['home_lowercased'].append(path.rstrip('/') + '/' + o[0])
        return [o[:6] for o in out]

    def stream(self, first, size):
        """Yield file content following the FAT chain, merging physically contiguous clusters."""
        if not size:
            return
        chain = self.fat.chain(first)
        left = size
        i = 0
        cl = self.fat.cl_bytes
        while left > 0 and i < len(chain):
            j = i + 1
            while j < len(chain) and chain[j] == chain[j - 1] + 1 and (j - i) * cl < (8 << 20):
                j += 1
            n = min(left, (j - i) * cl)
            yield self.fat.rd(self.fat.cl_off(chain[i]), n)
            left -= n
            i = j
        if left > 0:
            raise ValueError('truncated FAT chain')

    def read_small(self, first, size):
        return b''.join(self.stream(first, size))

    def convert_dir(self, dirc, ino, inode, parent_ino, path):
        ents = []
        for name, is_dir, first, size, kind, t in self.entries(dirc, path):
            child_path = (path.rstrip('/') + '/' + name)
            if child_path.lower() in self.skips:
                self.stats['skipped'].append(child_path)
                continue
            raw = name.encode('utf-8', 'surrogateescape')
            uid, gid = (self.uid, self.gid) if self.owned(child_path) else (0, 0)
            if is_dir:
                mode = 0o1777 if child_path.lower() == '/tmp' else 0o755
                if child_path.lower() in ('/run/user/%d' % self.uid, self.home.lower()):   # private: other accounts cannot read the user's files
                    mode = 0o700
                cino, cinode = self.fs.begin_dir(ino, mode, uid, gid, t or None)
                self.convert_dir(first if first else self.fat.rootc, cino, cinode, ino, child_path)
                ents.append((raw, cino, 2))
                self.stats['dirs'] += 1
            elif kind == 2:  # symbolic link: the content is the target
                target = self.read_small(first, size)
                cino = self.fs.make_symlink(target, uid, gid, t or None, ino)
                ents.append((raw, cino, 7))
                self.stats['symlinks'] += 1
            elif kind == 1:  # unix socket node
                cino = self.fs.make_node(ext4lib.S_IFSOCK | 0o755, 0, uid, gid, t or None, ino)
                ents.append((raw, cino, 6))
                self.stats['sockets'] += 1
            else:
                head = self.read_small(first, min(size, 4096)) if size else b''
                exe = ext4put.is_executable_image(head) or name.startswith('ld-linux')
                mode = 0o755 if exe else 0o644
                cino = self.fs.make_file(self.stream(first, size), size, mode, uid, gid, t or None, ino)
                ents.append((raw, cino, 1))
                self.stats['files'] += 1
                self.stats['bytes'] += size
                if self.progress: self.progress(self.stats['files'], self.stats['bytes'])
                self.stats['exec'] += exe
                if self.verbose and size > (16 << 20):
                    print('  %s (%d MiB)' % (child_path, size >> 20), flush=True)
        self.fs.finish_dir(ino, inode, parent_ino, ents)


def convert(src_path, dst_path, size_mib=None, skips=DEFAULT_SKIP, journal_blocks=None, uid=1000, gid=1000,
            home='/home/nexus', verbose=False, case_sources=DEFAULT_CASE_SOURCES, case_lists=DEFAULT_CASE_LISTS, progress=None):
    t0 = time.time()
    fat = fatput.Fat32(src_path, readonly=True)
    if os.path.exists(dst_path):
        raise SystemExit('destination exists: ' + dst_path)
    base = fat.base
    if not base:
        fat.close()
        raise SystemExit('source must use the partitioned NexusOS layout (FAT partition at LBA 32768)')
    src_size = os.path.getsize(src_path)
    size = size_mib * 1024 * 1024 if size_mib else src_size
    # copy the reserved prefix (MBR boot code, legacy and NPFS regions) sparsely
    with open(src_path, 'rb') as a, open(dst_path, 'w+b') as b:
        ext4lib.sparse_file(b)
        b.truncate(size)
        pos = 0
        while pos < base:
            chunk = a.read(min(1 << 20, base - pos))
            if not chunk:
                break
            if chunk.strip(b'\0'):
                b.seek(pos)
                b.write(chunk)
            pos += len(chunk)
    ext4lib.mkfs(dst_path, None, base=base, journal_blocks=journal_blocks, label=b'nexus-root')
    fs = ext4lib.Writer(dst_path)
    # root directory is rebuilt from the FAT root; drop the mkfs placeholder contents
    root = fs.read_inode(ext4lib.ROOT_INO)
    lost = [(n, i, f) for n, i, f in fs.dir_entries(ext4lib.ROOT_INO) if n == b'lost+found']
    fs.release_blocks(root)
    case = CaseMap(case_sources, case_lists) if case_sources or case_lists else None
    conv = Converter(fat, fs, uid, gid, home, list(skips), verbose, case); conv.progress = progress
    # convert the children of the FAT root into a fresh root directory that keeps lost+found
    orig_finish = fs.finish_dir

    def finish_root(ino, inode, parent, entries):
        if ino == ext4lib.ROOT_INO:
            entries = lost + [e for e in entries if e[0] != b'lost+found']
        orig_finish(ino, inode, parent, entries)
    fs.finish_dir = finish_root
    conv.convert_dir(fat.rootc, ext4lib.ROOT_INO, root, ext4lib.ROOT_INO, '/')
    fs.close(backups=True)
    fat.close()
    errors, stats, checker = ext4lib.check_image(dst_path); checker.fs.f.close()   # the checker keeps the image open (a cycle)
    if errors:
        print('\n'.join(errors[:40]))
        raise SystemExit('fat2ext4: converted image failed the consistency check')
    s = conv.stats
    print('[fat2ext4] %s -> %s: %d files (%d executables, %d MiB), %d dirs, %d symlinks, %d sockets, %d 8.3 names re-cased in %.1fs; skipped %s' % (
        src_path, dst_path, s['files'], s['exec'], s['bytes'] >> 20, s['dirs'], s['symlinks'], s['sockets'], conv.restored,
        time.time() - t0, ', '.join(s['skipped']) or 'nothing'), flush=True)
    if s['home_lowercased_count']: print('[fat2ext4] %d 8.3 names in the home folder kept in lower case (case unknown), e.g. %s' % (
        s['home_lowercased_count'], ', '.join(s['home_lowercased'][:5])), flush=True)
    return s


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source')
    ap.add_argument('destination')
    ap.add_argument('--size', type=int)
    ap.add_argument('--skip', action='append', default=[])
    ap.add_argument('--journal-blocks', type=int)
    ap.add_argument('--user', default='1000:1000')
    ap.add_argument('--home', default='/home/nexus')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--migrate', action='store_true', help="convert everything, the profiles in .config/.cache included (the user's own disk)")
    ap.add_argument('--case-list', action='append', help='path lists giving the true case of 8.3 names (default: userland/fat2ext4-case.txt)')
    ap.add_argument('--case-source', action='append', help='.deb files/directories or reference trees giving the true case of 8.3 names (default: tools/chrome-packages and out/chrome-root)')
    a = ap.parse_args(argv)
    uid, _, gid = a.user.partition(':')
    convert(a.source, a.destination, a.size, ([] if a.migrate else list(DEFAULT_SKIP)) + a.skip, a.journal_blocks, int(uid), int(gid or uid),
            a.home, a.verbose, a.case_source or DEFAULT_CASE_SOURCES, a.case_list or DEFAULT_CASE_LISTS)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
