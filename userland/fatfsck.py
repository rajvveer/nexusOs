#!/usr/bin/env python3
"""fatfsck.py -- check and repair the FAT32 volume of a disk image (NexusOS desktop disks, USB images, plain volumes).

    python -I userland/fatfsck.py IMAGE [-n | -a] [--recover] [--verbose] [--json FILE]

  -n         check only; the image is opened read-only (the default)
  -a         repair automatically, like dosfstools' fsck.fat -a (and the NexusOS kernel's boot check)
  --recover  with -a: keep lost cluster chains as files /FSCK0000.REC, /FSCK0001.REC ... instead of freeing them
  --verbose  list every problem (default: the first 40)
  --json F   write the counts and actions as JSON to F

What it checks (and how -a repairs it):
  * FAT copies that differ: the intact copy (fewest invalid entries; the first on a tie) is written to all copies
  * FAT entries outside the volume or reserved values: set to end-of-chain
  * the directory tree from the root: chains that leave the volume, run into a free cluster, loop or are shared by two
    entries (cross-links; the later entry is truncated at the shared cluster), file sizes that do not match the chain
    (a longer chain is truncated, a shorter one shortens the size), directories with a size or without clusters,
    '.'/'..' pointing elsewhere, orphaned long-name slots, NexusOS staged entries that were never published (a crash
    while a symlink or download was being created: T#######.TMP with the final name's long-name slots; removed)
  * lost clusters: allocated, but no directory entry reaches them (freed, or saved with --recover)
  * FSInfo free count / next-free hint, the dirty flags (FAT[1] clean-shutdown and hard-error bits, the boot sector's
    Linux dirty bit), the backup boot sector (reported only, as fsck.fat does)
Entries after a directory's end marker are invisible to NexusOS and Windows; their clusters are never freed (they are
reported). Names below /home/*/.config and /home/*/.cache are never printed (the path is cut at that folder) and no file
contents are read: only the FAT and directory clusters.
Exit status as fsck(8): 0 clean, 1 errors corrected, 4 errors left uncorrected, 8 operational error.
"""
import argparse, importlib.util, json, re, struct, sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location('fatfsck_fatput', Path(__file__).resolve().parent / 'fatput.py')
fatput = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(fatput)

SECTOR = 512
EOC = 0x0FFFFFFF
BAD = 0x0FFFFFF7
CLEAN_BIT, HARD_ERROR_BIT = 0x08000000, 0x04000000
PENDING_SHORT = re.compile(rb'T[0-9]{7}TMP')


def shown(path):
    parts = path.split('/')
    for i, p in enumerate(parts):
        if p in ('.config', '.cache') and i >= 3 and parts[1] == 'home': return '/'.join(parts[:i + 1]) + '/...'
    return path


class Report:
    def __init__(self):
        self.problems, self.actions, self.warnings = [], [], []
        self.counts = {k: 0 for k in ('files', 'dirs', 'referenced', 'allocated', 'lost_clusters', 'lost_chains', 'cross_links',
                                      'bad_chains', 'size_mismatch', 'fat_invalid', 'fat_copies_differ', 'dir_fixes', 'orphan_lfn',
                                      'pending_entries', 'hidden_entries', 'duplicates', 'dirty', 'fsinfo_fixes', 'free')}

    def problem(self, kind, text):
        self.counts[kind] += 1; self.problems.append(text)


class Checker:
    def __init__(self, image, repair=False, recover=False):
        self.fs = fatput.Fat32(str(image), readonly=not repair)
        self.repair, self.recover = repair, recover
        self.r = Report()
        fs = self.fs
        self.n = fs.nclusters + 2                      # valid clusters: 2 .. n-1
        self.cs = fs.cl_bytes
        self.boot = bytearray(fs.rd(0, SECTOR))
        self.dir_writes = {}                           # cluster -> bytearray (edited directory clusters)
        self.fat_changed = False

    # ---- FAT -----------------------------------------------------------------------------------------------------
    def entry_valid(self, v):
        return v == 0 or v == BAD or v >= 0x0FFFFFF8 or 2 <= v < self.n

    def check_fat_copies(self):
        fs, r = self.fs, self.r
        copies = [fs.fat] + [bytearray(fs.rd((fs.rsv + k * fs.fatsz) * SECTOR, fs.fatsz * SECTOR)) for k in range(1, fs.nfats)]
        if all(c == copies[0] for c in copies[1:]): return
        def invalid(copy):
            view = memoryview(copy).cast('I')
            return sum(1 for c in range(2, self.n) if not self.entry_valid(view[c] & 0x0FFFFFFF))
        scores = [invalid(c) for c in copies]
        best = min(range(len(copies)), key=lambda i: (scores[i], i))
        r.problem('fat_copies_differ', 'FAT copies differ (invalid entries per copy: %s); using copy %d' % (scores, best + 1))
        if best: fs.fat[:] = copies[best]
        self.fat_changed = True                        # every copy is rewritten from the chosen one

    def check_fat_entries(self):
        fat, r = memoryview(self.fs.fat).cast('I'), self.r
        for c in range(2, self.n):
            v = fat[c] & 0x0FFFFFFF
            if not self.entry_valid(v):
                r.problem('fat_invalid', 'FAT entry of cluster %d is 0x%07X (outside the volume); set to end of chain' % (c, v))
                fat[c] = (fat[c] & 0xF0000000) | EOC; self.fat_changed = True
        self.fatv = fat

    def get(self, c): return self.fatv[c] & 0x0FFFFFFF

    def set(self, c, v):
        self.fatv[c] = (self.fatv[c] & 0xF0000000) | (v & 0x0FFFFFFF); self.fat_changed = True

    # ---- directory clusters --------------------------------------------------------------------------------------
    def cluster(self, c):
        if c in self.dir_writes: return self.dir_writes[c]
        return self.fs.rd(self.fs.cl_off(c), self.cs)

    def edit(self, c, off, data):
        buf = self.dir_writes.get(c)
        if buf is None: buf = self.dir_writes[c] = bytearray(self.fs.rd(self.fs.cl_off(c), self.cs))
        buf[off:off + len(data)] = data

    def claim(self, first, path, owner_dir):
        """Claim the chain from `first`; truncate it at the first link that is invalid, free or already claimed.
        Returns the claimed cluster list (empty if `first` itself is unusable)."""
        r, claimed = self.r, self.claimed
        if not 2 <= first < self.n: return None, 'starts outside the volume'
        if self.get(first) == 0: return None, 'starts at a free cluster'
        if self.get(first) == BAD: return None, 'starts at a bad cluster'
        if claimed[first]:
            return None, 'duplicate' if first in self.heads else 'starts at a cluster another entry uses (cross-link)'
        chain = [first]; claimed[first] = 1
        while True:
            c = chain[-1]; v = self.get(c)
            if v >= 0x0FFFFFF8: break
            why = None
            if v == 0: why = 'runs into a free cluster'
            elif v == BAD: why = 'runs into a bad cluster'
            elif not 2 <= v < self.n: why = 'leaves the volume'
            elif claimed[v]:
                why = 'loops' if v in set(chain) else 'is cross-linked with another entry at cluster %d' % v
            if why:
                r.problem('cross_links' if 'cross' in why else 'bad_chains',
                          '%s: cluster chain %s after %d clusters; truncated' % (shown(path), why, len(chain)))
                self.set(c, EOC); break
            claimed[v] = 1; chain.append(v)
        return chain, None

    def free_chain(self, chain):
        for c in chain: self.set(c, 0); self.claimed[c] = 0

    # ---- the tree --------------------------------------------------------------------------------------------------
    def walk(self):
        fs, r = self.fs, self.r
        self.claimed = bytearray(self.n); self.heads = set()
        root, why = self.claim(fs.rootc, '/', None)
        if root is None: raise SystemExit('root directory chain is unusable (%s): not repairable here' % why)
        stack = [(fs.rootc, root, '', 0)]
        self.hidden = []
        while stack:
            first, chain, path, parent = stack.pop()
            self.scan_dir(first, chain, path, parent, stack)
        for x, path in self.hidden:   # after every visible entry: they only keep clusters nobody else uses
            if self.claimed[x] or self.get(x) in (0, BAD): continue
            r.counts['hidden_entries'] += 1
            r.warnings.append('%s: an entry after the end-of-directory marker keeps its clusters' % shown(path or '/'))
            while 2 <= x < self.n and not self.claimed[x] and self.get(x) not in (0, BAD):
                self.claimed[x] = 2; x = self.get(x)
        r.counts['referenced'] = sum(1 for c in range(2, self.n) if self.claimed[c])

    def entries(self, chain):
        for c in chain:
            data = self.cluster(c)
            for off in range(0, self.cs, 32): yield c, off, bytes(data[off:off + 32])

    def delete_slots(self, slots):
        for c, off in slots: self.edit(c, off, b'\xe5')

    def scan_dir(self, first, chain, path, parent, stack):
        r, fs = self.r, self.fs
        is_root = first == fs.rootc
        parts, lfn_slots, expected, checksum = {}, [], 0, 0
        names, index, ended = {}, -1, False
        def orphan_lfn():
            nonlocal parts, lfn_slots, expected
            if lfn_slots:
                r.problem('orphan_lfn', '%s: %d orphaned long-name slot(s); removed' % (shown(path or '/'), len(lfn_slots)))
                self.delete_slots(lfn_slots)
            parts, lfn_slots, expected = {}, [], 0
        for c, off, e in self.entries(chain):
            index += 1
            if ended:   # invisible here and on Windows; Linux and dosfstools still read it: never free its clusters
                if e[0] not in (0, 0xE5) and e[11] != 0x0F and not e[11] & 0x08:
                    hidden = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
                    if 2 <= hidden < self.n: self.hidden.append((hidden, path))
                continue
            if e[0] == 0:
                orphan_lfn(); ended = True; continue
            if e[0] == 0xE5:
                orphan_lfn(); continue
            if e[11] == 0x0F:
                order = e[0] & 0x1F
                if e[0] & 0x40:
                    orphan_lfn()
                    if 1 <= order <= 20: parts, lfn_slots, expected, checksum = {}, [], order, e[13]
                if not expected or order != expected or e[13] != checksum or e[12] or e[26:28] != b'\0\0':
                    lfn_slots.append((c, off)); expected = 0; parts = {}
                    continue
                parts[order] = e[1:11] + e[14:26] + e[28:32]; lfn_slots.append((c, off)); expected -= 1
                continue
            if e[11] & 0x08:                            # volume label
                orphan_lfn(); continue
            # a short entry: does the long name belong to it?
            name = fs.from83(e)
            long_ok = bool(parts) and expected == 0 and checksum == fs.name_checksum(e[:11])
            if long_ok:
                raw = b''.join(parts[i] for i in sorted(parts))
                units = [raw[i:i + 2] for i in range(0, len(raw), 2)]
                units = units[:next((i for i, x in enumerate(units) if x in (b'\0\0', b'\xff\xff')), len(units))]
                try: name = b''.join(units).decode('utf-16-le')
                except UnicodeDecodeError: pass
            my_lfn = list(lfn_slots) if long_ok else []
            if lfn_slots and not long_ok:
                if PENDING_SHORT.fullmatch(e[:11]) and expected == 0 and parts:
                    # NexusOS staged entry (fat32_begin_download): the long name was written for the final alias
                    r.problem('pending_entries', '%s: unpublished staged entry (interrupted symlink or download); removed'
                              % shown(path + '/' + fs.from83(e)))
                    self.delete_slots(lfn_slots + [(c, off)])   # its clusters are unreachable now: lost, freed below
                    parts, lfn_slots, expected = {}, [], 0
                    continue
                orphan_lfn()
            parts, lfn_slots, expected = {}, [], 0
            attr = e[11]
            first_c = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            full = path + '/' + name
            if e[:11] in (b'.          ', b'..         '):
                if is_root:
                    r.problem('dir_fixes', '/: a "%s" entry in the root directory; removed' % name)
                    self.delete_slots([(c, off)]); continue
                want = first if e[:11] == b'.          ' else (0 if parent == fs.rootc else parent)
                if index > 1 or not attr & 0x10:
                    r.problem('dir_fixes', '%s: misplaced "%s" entry; removed' % (shown(path), name))
                    self.delete_slots([(c, off)]); continue
                if first_c != want:
                    r.problem('dir_fixes', '%s: "%s" points to cluster %d instead of %d; fixed' % (shown(path), name, first_c, want))
                    fixed = bytearray(e); struct.pack_into('<H', fixed, 20, want >> 16); struct.pack_into('<H', fixed, 26, want & 0xFFFF)
                    self.edit(c, off, fixed)
                continue
            key = name.casefold()
            if key in names:
                r.counts['duplicates'] += 1; r.warnings.append('%s: duplicate name in the directory' % shown(full))
            names[key] = True
            if attr & 0x10:
                r.counts['dirs'] += 1
                if size:
                    r.problem('dir_fixes', '%s: directory with size %d; set to 0' % (shown(full), size))
                    fixed = bytearray(e); struct.pack_into('<I', fixed, 28, 0); self.edit(c, off, fixed)
                sub, why = (None, 'has no clusters') if first_c < 2 else self.claim(first_c, full, first)
                if sub is None:
                    r.problem('bad_chains', '%s: directory %s; entry removed' % (shown(full), why))
                    self.delete_slots(my_lfn + [(c, off)]); continue
                stack.append((first_c, sub, full, first))
                continue
            r.counts['files'] += 1
            need = -(-size // self.cs)
            if first_c == 0:
                if size:
                    r.problem('size_mismatch', '%s: size %d but no clusters; set to 0' % (shown(full), size))
                    fixed = bytearray(e); struct.pack_into('<I', fixed, 28, 0); self.edit(c, off, fixed)
                continue
            got, why = self.claim(first_c, full, first)
            if got is None and why == 'duplicate':   # an interrupted rename wrote the new name before erasing the old
                r.problem('duplicates', '%s: second entry of the same file (interrupted rename); removed' % shown(full))
                self.delete_slots(my_lfn + [(c, off)]); continue
            if got is None:
                r.problem('cross_links' if 'cross' in why else 'bad_chains', '%s: file %s; truncated to 0 bytes' % (shown(full), why))
                fixed = bytearray(e); struct.pack_into('<H', fixed, 20, 0); struct.pack_into('<H', fixed, 26, 0)
                struct.pack_into('<I', fixed, 28, 0); self.edit(c, off, fixed); continue
            self.heads.add(first_c)
            if len(got) > need:   # the tail becomes unreachable: freed later unless another entry reaches it
                r.problem('size_mismatch', '%s: %d bytes but %d clusters; chain truncated to %d' % (shown(full), size, len(got), need))
                if need: self.set(got[need - 1], EOC)
                else:
                    self.heads.discard(first_c)
                    fixed = bytearray(e); struct.pack_into('<H', fixed, 20, 0); struct.pack_into('<H', fixed, 26, 0); self.edit(c, off, fixed)
                for x in got[need:]: self.claimed[x] = 0
            elif len(got) < need:
                r.problem('size_mismatch', '%s: %d bytes but only %d clusters; size set to %d' % (shown(full), size, len(got), len(got) * self.cs))
                fixed = bytearray(e); struct.pack_into('<I', fixed, 28, len(got) * self.cs); self.edit(c, off, fixed)
        orphan_lfn()

    # ---- lost clusters ----------------------------------------------------------------------------------------------
    def lost(self):
        r, claimed = self.r, self.claimed
        lost = [c for c in range(2, self.n) if not claimed[c] and self.get(c) not in (0, BAD)]
        r.counts['lost_clusters'] = len(lost)
        if not lost: return
        lostset = bytearray(self.n)
        for c in lost: lostset[c] = 1
        pointed = bytearray(self.n)
        for c in lost:
            v = self.get(c)
            if 2 <= v < self.n and lostset[v]: pointed[v] = 1
        chains, done = [], bytearray(self.n)
        for start in [c for c in lost if not pointed[c]] + lost:     # heads first, then whatever is left (cycles)
            if done[start]: continue
            chain, x = [], start
            while 2 <= x < self.n and lostset[x] and not done[x]:
                done[x] = 1; chain.append(x); x = self.get(x)
            if chain: chains.append(chain)
        r.counts['lost_chains'] = len(chains); self.lost_chains = chains
        r.problems.append('%d lost clusters (%d KiB) in %d chains; %s' % (
            len(lost), len(lost) * self.cs // 1024, len(chains), 'saved as /FSCK*.REC' if self.recover else 'freed'))
        if self.recover:
            for i, chain in enumerate(chains):
                self.set(chain[-1], EOC)
                for c in chain: claimed[c] = 1
                self.recovered.append((chain[0], len(chain)))
        else:
            for chain in chains:
                for c in chain: self.set(c, 0)

    # ---- run -------------------------------------------------------------------------------------------------------
    def run(self):
        fs, r = self.fs, self.r
        self.recovered = []
        fat1 = struct.unpack_from('<I', fs.fat, 4)[0]
        dirty_fat = not fat1 & CLEAN_BIT; hard_error = not fat1 & HARD_ERROR_BIT; dirty_boot = bool(self.boot[0x41] & 1)
        if dirty_fat or dirty_boot or hard_error:
            r.counts['dirty'] = 1
            r.problems.append('volume was not properly unmounted (%s)' % ', '.join(
                x for x, on in (('FAT[1] clean bit clear', dirty_fat), ('FAT[1] hard-error bit clear', hard_error),
                                ('boot sector dirty bit set', dirty_boot)) if on))
        backup = struct.unpack_from('<H', self.boot, 50)[0]
        if 0 < backup < fs.rsv and fs.rd(backup * SECTOR, 90) != bytes(self.boot[:90]):
            r.warnings.append('the backup boot sector differs from the boot sector (not changed)')
        view = memoryview(fs.fat).cast('I')
        free = sum(1 for c in range(2, self.n) if not view[c] & 0x0FFFFFFF)   # as the first FAT says (FSInfo must agree)
        view.release()
        self.check_fat_copies()
        self.check_fat_entries()
        self.walk()
        r.counts['allocated'] = sum(1 for c in range(2, self.n) if self.get(c) not in (0, BAD))
        self.lost()
        r.counts['free'] = sum(1 for c in range(2, self.n) if self.get(c) == 0)   # after the repair
        info_sector = struct.unpack_from('<H', self.boot, 48)[0]
        info = bytearray(fs.rd(info_sector * SECTOR, SECTOR)) if 0 < info_sector < fs.rsv else None
        if info is not None and struct.unpack_from('<I', info, 0)[0] == 0x41615252 and struct.unpack_from('<I', info, 484)[0] == 0x61417272:
            stored = struct.unpack_from('<I', info, 488)[0]
            hint = struct.unpack_from('<I', info, 492)[0]
            if stored != 0xFFFFFFFF and stored != free:
                r.problem('fsinfo_fixes', 'FSInfo free count %d, really %d; fixed' % (stored, free))
                struct.pack_into('<I', info, 488, free)
            if hint != 0xFFFFFFFF and not 2 <= hint < self.n:
                r.problem('fsinfo_fixes', 'FSInfo next-free hint %d outside the volume; reset' % hint)
                struct.pack_into('<I', info, 492, 0xFFFFFFFF)
        else:
            info = None
            r.warnings.append('no valid FSInfo sector')
        errors = len(r.problems)
        if not self.repair: return 4 if errors else 0
        if not errors: return 0
        # Write order: directory sectors, the recovered files' entries, every FAT copy, FSInfo, then the clean flags last
        # (an interruption leaves the volume dirty, and running the check again finishes the repair).
        for c, data in self.dir_writes.items(): fs.wr(fs.cl_off(c), bytes(data))
        if self.recovered: self.write_recovered()
        if self.fat_changed: fs.flush_fat()
        if info is not None:   # the recovered files' directory may have taken a cluster: count again
            struct.pack_into('<I', info, 488, sum(1 for c in range(2, self.n) if self.get(c) == 0))
            fs.wr(info_sector * SECTOR, bytes(info))
        fat1 = struct.unpack_from('<I', fs.fat, 4)[0] | CLEAN_BIT | HARD_ERROR_BIT
        struct.pack_into('<I', fs.fat, 4, fat1)
        for k in range(fs.nfats): fs.wr((fs.rsv + k * fs.fatsz) * SECTOR + 4, struct.pack('<I', fat1))
        if self.boot[0x41] & 1:
            self.boot[0x41] &= 0xFE; fs.wr(0, bytes(self.boot))
        r.actions.append('repaired %d problem(s)' % errors)
        return 1

    def write_recovered(self):
        fs = self.fs
        for i, (head, count) in enumerate(self.recovered):
            for n in range(i, 10000):
                name = 'FSCK%04d.REC' % n
                if fs.find(fs.rootc, fs.to83(name)) is None: break
            fs.create_entry(fs.rootc, name, 0x20, head, min(count * self.cs, 0xFFFFFFFF))
            self.r.actions.append('saved a lost chain of %d clusters as /%s' % (count, name))

    def close(self):
        self.fs.close()


def check(image, repair=False, recover=False):
    """Returns (exit status, Report)."""
    checker = Checker(image, repair, recover)
    try: status = checker.run()
    finally: checker.close()
    return status, checker.r


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('image')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('-n', action='store_true', help='check only (default)')
    mode.add_argument('-a', action='store_true', help='repair automatically')
    ap.add_argument('--recover', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--json')
    a = ap.parse_args(argv)
    if a.recover and not a.a: ap.error('--recover needs -a')
    try:
        status, r = check(a.image, a.a, a.recover)
    except (ValueError, OSError, SystemExit) as error:
        print('fatfsck: %s: %s' % (a.image, error)); return 8
    c = r.counts
    print('%s: %d files, %d dirs, %d clusters referenced, %d lost (%d chains), %d free; %d problem(s), %d warning(s)'
          % (a.image, c['files'], c['dirs'], c['referenced'], c['lost_clusters'], c['lost_chains'], c['free'], len(r.problems), len(r.warnings)))
    limit = None if a.verbose else 40
    for p in r.problems[:limit]: print('  ', p)
    if limit and len(r.problems) > limit: print('   ... %d more' % (len(r.problems) - limit))
    for w in r.warnings[:limit]: print('   warning:', w)
    for x in r.actions: print('   action:', x)
    print('FSCK', {0: 'CLEAN', 1: 'REPAIRED', 4: 'ERRORS'}[status])
    if a.json:
        Path(a.json).write_text(json.dumps({'status': status, 'counts': c, 'problems': r.problems, 'warnings': r.warnings,
                                            'actions': r.actions}, indent=1), newline='\n')
    return status


if __name__ == '__main__':
    sys.exit(main())
