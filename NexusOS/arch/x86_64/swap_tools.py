#!/usr/bin/env python3
"""Swap on a NexusOS desktop disk: Debian's swap tools and the default swap file.

    python -I arch/x86_64/swap_tools.py lock [--allow-older]          re-resolve swap-tools-lock.json from signed indices
    python -I arch/x86_64/swap_tools.py install <disk.img> [--cache DIR] [--report PATH]
    python -I arch/x86_64/swap_tools.py swapfile <disk.img> [--size MIB|auto] [--grow MIB] [--path /swapfile] [--priority N] [--copy]
    python -I arch/x86_64/swap_tools.py swapfile <disk.img> --remove
    python -I arch/x86_64/swap_tools.py status <disk.img>

install   Debian 13 (trixie) mount (swapon, swapoff, mount, losetup), util-linux (mkswap, ...) and procps (free, vmstat, ...)
          with the libraries they need that the desktop disk does not have yet (the packages of chrome-lock.json are on every
          desktop disk). Every .deb is pinned in swap-tools-lock.json by URL, size and SHA-256; `lock` takes them only from
          InRelease files with a good signature by a Debian archive key (wine_install.py's verified_indices: gpgv, the
          pinned debian-archive-keyring, Valid-Until, Packages.xz checked against the signed hash). Files go through
          apps_install.py's FAT32 engine: read-only analysis, then a sparse working copy that replaces the image only after
          the reserved prefix was verified unchanged; existing files are never overwritten. No maintainer script runs.
swapfile  A swap file of SIZE MiB (default 2048; auto: 2048 or half the free space if less, at least 256) at PATH
          (default /swapfile) on the FAT32 root, after growing the disk to --grow MIB if given: one contiguous cluster run
          when the volume has one (any run of whole clusters works: the kernel maps the chain at swapon), a mkswap v1
          header (version 1, last_page, random UUID, label "nexusswap") in its first page, the rest zero (holes in the
          sparse host image: no host space is used until the guest swaps), and the line "PATH none swap sw 0 0" in
          /etc/fstab. The kernel activates /etc/fstab swap entries before the init session (swap.c swap_boot); the
          desktop launcher's --no-swap (fw_cfg opt/nexus/swap=off) or the kernel option `noswap` skip them for one boot.
          --remove deletes the file and its /etc/fstab line and records /nexus/swap-declined, so that the desktop
          launcher (build.py desktop/chrome, ensure_default) does not create it again; `swapfile` without --remove
          clears that record. The image is edited in place (checks first, FAT and directories written last) unless it
          grows or --copy asks for a transactional copy, which needs the image's allocated size again on the host.
Stop QEMU before running any of these on a disk."""
from pathlib import Path, PurePosixPath
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
import struct
import sys
import time
import uuid
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = HERE / 'swap-tools-lock.json'
DEFAULT_CACHE = nexus_paths.cache('swap-tools')
_spec = importlib.util.spec_from_file_location('swap_wine_install', HERE / 'wine_install.py')
wi = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(wi)
ai = wi.ai
InstallError = ai.InstallError
GROUPS = {'swap-tools': {'trixie': ['util-linux', 'mount', 'procps']}}   # Debian ships swapon/swapoff in mount, mkswap in util-linux
SKIP = {   # dependencies left out (name -> reason): nothing swapon/swapoff/mkswap/free loads needs them
    'debianutils': 'run-parts/which/tempfile helpers for maintainer scripts',
    'login': 'su/login programs (util-linux only recommends using them)',
    'libpam-modules': 'PAM modules: only su/runuser authenticate',
    'libpam-runtime': 'PAM configuration framework (maintainer scripts)',
    'libpam-modules-bin': 'PAM helper binaries',
    'init-system-helpers': 'service management helpers for maintainer scripts',
}
PAGE = 4096
SECTOR = 512


def log(*args): print('[swap]', *args, flush=True)


# ---- lock ------------------------------------------------------------------------------------------
def make_lock(allow_older=False):
    previous = json.loads(LOCK.read_bytes()) if LOCK.exists() else {}
    keyring = previous.get('keyring') or json.loads((HERE / 'wine-lock.json').read_bytes())['keyring']
    wi.MIRRORS = {s: wi.MIRRORS[s] for s in wi.TRIXIE}   # trixie only: no sid index is needed
    wi.GROUPS, wi.SKIP = GROUPS, SKIP
    packages, meta, keyring = wi.verified_indices(keyring, previous.get('suites'), allow_older, False)
    packages['sid'] = {}
    present = {p['name']: p['version'] for p in json.loads((HERE / 'chrome-lock.json').read_text())}
    selected, skipped, satisfied = wi.resolve(packages, present)
    lock = {'format': 1, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'suites': meta, 'keyring': keyring,
            'present_from_chrome_lock': dict(sorted(satisfied.items())), 'skipped': dict(sorted(skipped.items())),
            'packages': sorted(selected.values(), key=lambda p: p['name'])}
    LOCK.write_bytes((json.dumps(lock, indent=1) + '\n').encode())
    log('wrote', LOCK, len(selected), 'packages,', len(skipped), 'skipped')
    for p in lock['packages']: log('  %-28s %-26s %s' % (p['name'], p['version'], p['suite']))


# ---- install ---------------------------------------------------------------------------------------
class ToolsInstaller(ai.Installer):
    def existing(self, path, kind, art, bucket, mtime):
        if path == 'usr/include' or path.startswith('usr/include/'):
            self.skip('development headers', '/' + path, bucket); return None
        return super().existing(path, kind, art, bucket, mtime)

    def finish(self, versions):
        now = time.time()
        for (top, n), app in sorted(self.usr_names.items()):     # merged-usr emulation: /sbin/swapon -> /usr/sbin/swapon
            target = '/usr/%s/%s' % (top, n)
            d = self.walk(top, False)
            ent = d.names.get(ai.fold(n)) if d else None
            if ent is not None and not (ent.is_link and ent.name == n and self.read_link(ent) == target):
                self.skip('merged-usr link: existing entry kept', '/%s/%s' % (top, n)); continue
            self.managed_link('%s/%s' % (top, n), target, app, now)
        for pkg in self.by_name:
            if pkg not in self.licenses: self.licenses[pkg] = 'none'
        manifest = {'format': 1, 'installer': 'arch/x86_64/swap_tools.py', 'lock_sha256': self.lock_sha256,
                    'packages': [{'name': a['name'], 'version': a['version']} for a in self.artifacts],
                    'symlinks_created': sorted(self.managed_links)}
        d = self.walk('nexus', True, now, 'swap-tools')
        ent = d.names.get(ai.fold('swap-tools-installed.json'))
        old = {}
        if ent is not None and not (ent.is_dir or ent.is_link):
            try: old = json.loads(b''.join(self.vol.read_file(ent.first, ent.size)) if ent.size else b'{}')
            except ValueError: old = {}
        manifest['symlinks_created'] = sorted(set(manifest['symlinks_created']) | set(old.get('symlinks_created') or []))
        self.manifest = manifest
        data = (json.dumps(manifest, indent=1) + '\n').encode()
        if ent is None:
            self.vol.write_file(d, 'swap-tools-installed.json', len(data), [data], now); self.changes += 1
        elif ent.is_dir or ent.is_link:
            self.skip('manifest path is not a regular file', '/nexus/swap-tools-installed.json')
        elif old != manifest:   # a newer lock or package set: rewrite the manifest in place
            self.changes += 1
            if self.vol.writable: write_text(self, '/nexus/swap-tools-installed.json', data.decode(), now)


def install(disk, cache, report_path):
    lock_bytes = LOCK.read_bytes(); lock = json.loads(lock_bytes)
    artifacts = [dict(p, preinstalled=False) for p in lock['packages']]
    paths = {}
    for a in artifacts: paths[a['name']] = wi.fetch_package(a, cache)
    log('%d packages verified (size + SHA-256)' % len(artifacts))
    vol = ai.Volume(disk, writable=False)
    try:
        ana = ToolsInstaller(vol, artifacts, paths, {'swap-tools'}); ana.lock_sha256 = hashlib.sha256(lock_bytes).hexdigest()
        ai.run(ana, artifacts, {})
        needed, free = vol.allocated, vol.free_start
    finally:
        vol.close()
    if ana.fatal: raise SystemExit('aborted before writing: %r' % ana.fatal[:10])
    if needed > free: raise SystemExit('not enough space on the FAT volume')
    if not ana.changes: log('nothing to do: already installed'); return
    temp = disk.with_name('.swap-tools-%d.img' % os.getpid())
    head = ai.prefix_digest(disk, ai.NPFS, ai.BASE)
    try:
        ai.sparse_copy(disk, temp)
        vol = ai.Volume(temp, writable=True)
        try:
            inst = ToolsInstaller(vol, artifacts, paths, {'swap-tools'}); inst.lock_sha256 = ana.lock_sha256
            ai.run(inst, artifacts, {})
            if inst.fatal: raise InstallError('fatal problems while writing: %r' % inst.fatal[:5])
            vol.flush()
        finally:
            vol.close()
        if ai.prefix_digest(temp, ai.NPFS, ai.BASE) != head: raise InstallError('reserved prefix changed')
        os.replace(temp, disk)
    finally:
        if temp.exists(): temp.unlink()
    report = {'disk': str(disk), 'packages': [a['name'] + ' ' + a['version'] for a in artifacts], 'summary': ai.summary(inst),
              'conflicts': inst.conflicts[:200], 'symlinks': inst.managed_links}
    ai.write_report(report_path, report)
    log('installed; summary', ai.summary(inst), 'report', report_path)


# ---- swap file -------------------------------------------------------------------------------------
def zero_range(f, offset, length):
    """FSCTL_SET_ZERO_DATA on Windows (deallocates in a sparse file), else plain zeros."""
    if not length: return
    if os.name == 'nt':
        import msvcrt
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                           ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p]
        f.flush()
        info = struct.pack('<qq', offset, offset + length); returned = ctypes.c_ulong()
        if kernel.DeviceIoControl(msvcrt.get_osfhandle(f.fileno()), 0x980C8, info, len(info), None, 0, ctypes.byref(returned), None): return
    block = bytes(1 << 20)
    f.seek(offset)
    while length:
        n = min(length, len(block)); f.write(block[:n]); length -= n


def swap_header(pages, label=b'nexusswap'):
    page = bytearray(PAGE)
    struct.pack_into('<III', page, 1024, 1, pages - 1, 0)
    page[1036:1052] = uuid.uuid4().bytes
    page[1052:1052 + len(label)] = label
    page[PAGE - 10:] = b'SWAPSPACE2'
    return bytes(page)


def fstab_lines(inst):
    ent = inst.resolve('/etc/fstab')
    if ent is None: return None, []
    if ent.is_dir or ent.is_link: raise InstallError('/etc/fstab is not a regular file')
    data = b''.join(inst.vol.read_file(ent.first, ent.size)) if ent.size else b''
    return ent, data.decode('utf-8', 'replace').splitlines()


def write_text(inst, path, text, now):
    """Create or replace an installer-owned small file (FAT chain rewritten)."""
    top, leaf = path.rsplit('/', 1)
    d = inst.walk(top.lstrip('/'), True, now, 'swap')
    data = text.encode()
    ent = d.names.get(ai.fold(leaf))
    vol = inst.vol
    if ent is not None:
        if ent.first >= 2:
            chain = vol.chain(ent.first)
            for c in chain: vol.fat_set(c, 0)
            vol.free += len(chain)
        clusters = vol.alloc((len(data) + vol.cl - 1) // vol.cl)
        off = 0
        for c, k in vol.runs(clusters):
            vol.wr(vol.cl_off(c), data[off:off + k * vol.cl].ljust(k * vol.cl, b'\0')); off += k * vol.cl
        vol.set_entry_data(d, ent, clusters[0] if clusters else 0, len(data), now)
    else:
        vol.write_file(d, leaf, len(data), [data], now)


def swapfile(disk, size_mib, path, priority, remove, grow_mib=None, copy=False):
    """Create (or remove) a swap file and its /etc/fstab line. Edits the image in place unless it grows or `copy` asks
    for a transactional copy (a whole second image on the host). In place, every check runs before the first write and
    the FAT and directories are written last, so an interruption leaves at most unreferenced zeroed clusters."""
    if not path.startswith('/') or path.count('/') < 1 or ' ' in path: raise SystemExit('give an absolute path without spaces')
    head = ai.prefix_digest(disk, ai.NPFS, ai.BASE)
    grow = bool(grow_mib and grow_mib * 1024 * 1024 > disk.stat().st_size)
    temp = disk.with_name('.swap-file-%d.img' % os.getpid())
    target = temp if grow or copy else disk
    try:
        if grow: ai.grow(disk, temp, grow_mib)
        elif copy: ai.sparse_copy(disk, temp)
        vol = ai.Volume(target, writable=True)
        try:
            inst = ToolsInstaller(vol, [], {}, set()); now = time.time()
            if size_mib == 'auto':
                free_mib = vol.free * vol.cl >> 20
                size_mib = min(2048, max(256, free_mib // 2))
                if size_mib > free_mib - 64: raise InstallError('only %d MiB free: grow the disk (--grow MIB)' % free_mib)
            elif (size_mib << 20) > vol.free * vol.cl:
                raise InstallError('%d MiB do not fit: %d MiB free (use --grow MIB or a smaller --size)' % (size_mib, vol.free * vol.cl >> 20))
            fstab, lines = fstab_lines(inst)
            kept = [l for l in lines if not (l.split()[:1] == [path] and len(l.split()) > 2 and l.split()[2] == 'swap')]
            top, leaf = path.rsplit('/', 1)
            d = inst.walk(top.lstrip('/'), not remove, now, 'swap') if top else vol.root
            ent = d.names.get(ai.fold(leaf)) if d else None
            if ent is not None:
                if ent.is_dir or ent.is_link: raise InstallError(path + ' exists and is not a regular file')
                if ent.first >= 2:
                    chain = vol.chain(ent.first)
                    for c in chain: vol.fat_set(c, 0)
                    vol.free += len(chain)
                delete_entry(d, ent); d.names.pop(ai.fold(leaf))
            if not remove:
                size = size_mib << 20
                clusters = vol.alloc((size + vol.cl - 1) // vol.cl)
                runs = vol.runs(clusters)
                header = swap_header(size // PAGE)
                with open(target, 'r+b') as f:
                    for c, k in runs: zero_range(f, vol.base + vol.cl_off(c), k * vol.cl)
                    pos = 0
                    for c, k in runs:   # the header page may span clusters smaller than a page
                        if pos >= PAGE: break
                        n = min(k * vol.cl, PAGE - pos)
                        f.seek(vol.base + vol.cl_off(c)); f.write(header[pos:pos + n]); pos += n
                vol.add_entry(d, leaf, 0x20, clusters[0], size, 0, now)
                kept.append('%s none swap sw%s 0 0' % (path, ',pri=%d' % priority if priority is not None else ''))
                log('%s: %d MiB in %d cluster run(s) of %d bytes' % (path, size_mib, len(runs), vol.cl))
            write_text(inst, '/etc/fstab', '\n'.join(kept) + ('\n' if kept else ''), now)
            nexus = inst.walk('nexus', True, now, 'swap'); declined = nexus.names.get(ai.fold('swap-declined'))
            if remove and declined is None: vol.write_file(nexus, 'swap-declined', 26, [b'swap file removed by user\n'], now)
            if not remove and declined is not None:
                if declined.first >= 2:
                    chain = vol.chain(declined.first)
                    for c in chain: vol.fat_set(c, 0)
                    vol.free += len(chain)
                delete_entry(nexus, declined); nexus.names.pop(ai.fold('swap-declined'))
            vol.flush()
        finally:
            vol.close()
        if ai.prefix_digest(target, ai.NPFS, ai.BASE) != head: raise InstallError('reserved prefix changed')
        if target == temp: os.replace(temp, disk)
    finally:
        if temp.exists(): temp.unlink()
    log('removed' if remove else 'created', path, '; /etc/fstab updated', '(in place)' if target == disk else '')


def ensure_default(disk):
    """Desktop launcher: the default swap file, once (not after `swapfile --remove`, nor when fstab already has swap).
    Returns True when it created one. Never raises: the desktop starts without swap when anything is in the way."""
    try:
        vol = ai.Volume(disk, writable=False)
        try:
            inst = ToolsInstaller(vol, [], {}, set())
            fstab, lines = fstab_lines(inst)
            declined = inst.resolve('/nexus/swap-declined') is not None
        finally:
            vol.close()
        if declined or any(len(l.split()) > 2 and l.split()[2] == 'swap' and not l.lstrip().startswith('#') for l in lines): return False
        swapfile(disk, 'auto', '/swapfile', None, False); return True
    except Exception as error:   # unreadable or foreign disk layout, no space, a file lock: boot without swap
        log('no swap file created:', error); return False


def delete_entry(d, ent):
    """Mark the entry's short slot and the long-name slots before it deleted (0xE5)."""
    i = ent.slot
    d.shorts.discard(bytes(d.data[i * 32:i * 32 + 11]))
    d.data[i * 32] = 0xE5
    j = i - 1
    while j >= 0 and d.data[j * 32 + 11] == 0x0F and d.data[j * 32] != 0xE5:
        d.data[j * 32] = 0xE5; j -= 1
    d.dirty = True


def status(disk):
    vol = ai.Volume(disk, writable=False)
    try:
        inst = ToolsInstaller(vol, [], {}, set())
        fstab, lines = fstab_lines(inst)
        swaps = [l for l in lines if len(l.split()) > 2 and l.split()[2] == 'swap']
        print(json.dumps({'fstab_swap': swaps, 'tools': {p: inst.resolve(p) is not None for p in
              ('/usr/sbin/swapon', '/usr/sbin/swapoff', '/usr/sbin/mkswap', '/usr/bin/free')}}, indent=1))
    finally:
        vol.close()


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='action', required=True)
    p = sub.add_parser('lock'); p.add_argument('--allow-older', action='store_true')
    p = sub.add_parser('install'); p.add_argument('disk'); p.add_argument('--cache', default=str(DEFAULT_CACHE)); p.add_argument('--report')
    p = sub.add_parser('swapfile'); p.add_argument('disk'); p.add_argument('--size', default='2048'); p.add_argument('--grow', type=int, metavar='MIB')
    p.add_argument('--path', default='/swapfile'); p.add_argument('--priority', type=int); p.add_argument('--remove', action='store_true')
    p.add_argument('--copy', action='store_true', help='write a transactional copy and replace the image (needs its size again)')
    p = sub.add_parser('status'); p.add_argument('disk')
    a = ap.parse_args(argv)
    if a.action == 'lock': make_lock(a.allow_older); return 0
    disk = Path(a.disk).resolve()
    if not disk.is_file(): raise SystemExit('no such disk image: %s' % disk)
    if a.action == 'install':
        cache = Path(a.cache).resolve(); cache.mkdir(parents=True, exist_ok=True)
        install(disk, cache, Path(a.report).resolve() if a.report else cache / 'swap-tools-report.json')
    elif a.action == 'swapfile':
        size = a.size if a.size == 'auto' else int(a.size)
        if size != 'auto' and (size < 1 or size > 64 * 1024): raise SystemExit('--size takes 1..65536 MiB or auto')
        swapfile(disk, size, a.path, a.priority, a.remove, a.grow, a.copy)
    else: status(disk)
    return 0


if __name__ == '__main__':
    sys.exit(main())
