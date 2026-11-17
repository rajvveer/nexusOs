#!/usr/bin/env python3
"""Aurora desktop extras from Debian: international fonts, the password hashing library, input methods, build headers.

    python -I arch/x86_64/desk_install.py lock [--update-keyring] [--allow-older]   # re-resolve desk-lock.json (signed indices)
    python -I arch/x86_64/desk_install.py sdk                                       # headers for the build -> out/desk-sdk
    python -I arch/x86_64/desk_install.py install <disk.img> [--only fonts,auth,ime] [--grow MIB] [--cache DIR] [--report PATH]
    python -I arch/x86_64/desk_install.py fccache <disk.img>   # prebuilt fontconfig cache (boots the disk copy once in QEMU)

Groups:
  fonts  Noto from Debian 13 (trixie), Sans Regular + Bold only: Latin/Greek/Cyrillic, Arabic, Hebrew, Devanagari, Bengali,
         Tamil, Telugu, Gujarati, Gurmukhi, Thai, Armenian, Georgian, Ethiopic (fonts-noto-core), CJK SC/TC/JP/KR in one
         collection (fonts-noto-cjk: NotoSansCJK-{Regular,Bold}.ttc) and Noto Color Emoji; the Debian fontconfig defaults
         (conf.d links fontconfig-config's postinst would make) plus /etc/fonts/conf.d/60-nexus-noto.conf (generic family
         preferences) and the Wine font replacements file /usr/share/nexus/wine-fonts.reg (desktop launchers import it).
  auth   libcrypt1 (crypt_r: yescrypt / SHA-512 password hashes; the Aurora lock screen and greeter dlopen it).
  ime    (reserved: input methods)

Verification and installing reuse arch/x86_64/wine_install.py: desk-lock.json pins every .deb (URL + size + SHA-256) with the
signed InRelease of its suite; `lock` verifies the InRelease signatures against Debian's archive keys and resolves the
dependency closure (packages of chrome-lock.json count as present: they are on every desktop disk; app groups are
optional, so the fonts group carries fontconfig itself). The
image is changed through apps_install.py's FAT32 engine (analysis first, sparse working copy, reserved prefix checked);
existing files are never overwritten. /nexus/desk-installed.json records what was installed.
"""
from pathlib import Path, PurePosixPath
import argparse
import functools
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = HERE / 'desk-lock.json'
OUT = HERE / 'out'
SDK = OUT / 'desk-sdk'
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations
DEFAULT_CACHE = nexus_paths.file('NEXUS_DESK_CACHE', OUT / 'desk-packages')
VERSION = 1

_spec = importlib.util.spec_from_file_location('desk_wine_install', HERE / 'wine_install.py')
wi = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(wi)
ai = wi.ai
InstallError = wi.InstallError
fold = ai.fold

GROUPS = {
    'fonts': {'trixie': ['fonts-noto-core', 'fonts-noto-cjk', 'fonts-noto-color-emoji', 'fontconfig']},
    'auth': {'trixie': ['libcrypt1']},
}
# Build-time headers (not installed on disks): exactly these packages, no dependency closure.
SDK_PACKAGES = ['libfreetype-dev', 'libharfbuzz-dev', 'libfontconfig-dev', 'libegl-dev', 'libgl-dev', 'libxkbregistry-dev']   # + EGL/xkbregistry: winewayland_build.py
SDK_PATHS = ('usr/include/freetype2/', 'usr/include/harfbuzz/', 'usr/include/fontconfig/', 'usr/include/EGL/', 'usr/include/KHR/', 'usr/include/xkbcommon/xkbregistry.h')
SKIP = {'libpam0g': 'not needed by the libraries Aurora loads'}
# Font files kept from the font packages (the rest of fonts-noto-core is ~400 files / 250 MB): Sans, Regular + Bold.
SCRIPTS = ('', 'Arabic', 'Hebrew', 'Devanagari', 'Bengali', 'Tamil', 'Telugu', 'Gujarati', 'Gurmukhi', 'Thai', 'Armenian',
           'Georgian', 'Ethiopic', 'Mono')
KEEP_FONTS = re.compile(r'(NotoSans(%s)-(Regular|Bold)\.ttf|NotoSansCJK-(Regular|Bold)\.ttc|NotoColorEmoji\.ttf)$' % '|'.join(SCRIPTS))
FONT_PACKAGES = ('fonts-noto-core', 'fonts-noto-cjk', 'fonts-noto-color-emoji')
# fontconfig-config's postinst links these from /usr/share/fontconfig/conf.avail into /etc/fonts/conf.d (Debian defaults).
CONF_DEFAULTS = ['10-hinting-slight.conf', '10-scale-bitmap-fonts.conf', '11-lcdfilter-default.conf', '20-unhint-small-vera.conf',
                 '30-metric-aliases.conf', '40-nonlatin.conf', '45-generic.conf', '45-latin.conf', '48-spacing.conf',
                 '49-sansserif.conf', '50-user.conf', '51-local.conf', '60-generic.conf', '60-latin.conf', '65-fonts-persian.conf',
                 '65-nonlatin.conf', '69-unifont.conf', '80-delicious.conf', '90-synthetic.conf']
NEXUS_FONTS_CONF = b'''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<!-- NexusOS (arch/x86_64/desk_install.py): Noto first for every script, Liberation/DejaVu for Latin metrics. -->
<fontconfig>
  <alias binding="same"><family>sans-serif</family><prefer><family>Noto Sans</family><family>Noto Sans CJK SC</family>
    <family>Noto Sans Arabic</family><family>Noto Sans Hebrew</family><family>Noto Sans Devanagari</family>
    <family>Noto Sans Bengali</family><family>Noto Sans Tamil</family><family>Noto Sans Telugu</family>
    <family>Noto Sans Gujarati</family><family>Noto Sans Gurmukhi</family><family>Noto Sans Thai</family>
    <family>Noto Sans Armenian</family><family>Noto Sans Georgian</family><family>Noto Sans Ethiopic</family>
    <family>Noto Color Emoji</family></prefer></alias>
  <alias binding="same"><family>serif</family><prefer><family>Liberation Serif</family><family>Noto Sans</family>
    <family>Noto Sans CJK SC</family><family>Noto Color Emoji</family></prefer></alias>
  <alias binding="same"><family>monospace</family><prefer><family>Noto Sans Mono</family><family>DejaVu Sans Mono</family>
    <family>Liberation Mono</family><family>Noto Sans CJK SC</family><family>Noto Color Emoji</family></prefer></alias>
  <alias binding="same"><family>emoji</family><prefer><family>Noto Color Emoji</family></prefer></alias>
</fontconfig>
'''
# Wine: Windows font names Wine's GDI asks for, mapped to the Noto families (HKCU\\Software\\Wine\\Fonts\\Replacements).
WINE_FONTS_REG = '''Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\\Software\\Wine\\Fonts\\Replacements]
"MS UI Gothic"="Noto Sans CJK JP"
"MS Gothic"="Noto Sans CJK JP"
"MS PGothic"="Noto Sans CJK JP"
"Meiryo"="Noto Sans CJK JP"
"Yu Gothic"="Noto Sans CJK JP"
"SimSun"="Noto Sans CJK SC"
"NSimSun"="Noto Sans CJK SC"
"SimHei"="Noto Sans CJK SC"
"Microsoft YaHei"="Noto Sans CJK SC"
"PMingLiU"="Noto Sans CJK TC"
"MingLiU"="Noto Sans CJK TC"
"Microsoft JhengHei"="Noto Sans CJK TC"
"Gulim"="Noto Sans CJK KR"
"Dotum"="Noto Sans CJK KR"
"Batang"="Noto Sans CJK KR"
"Malgun Gothic"="Noto Sans CJK KR"
"Mangal"="Noto Sans Devanagari"
"Nirmala UI"="Noto Sans Devanagari"
"Vrinda"="Noto Sans Bengali"
"Latha"="Noto Sans Tamil"
"Gautami"="Noto Sans Telugu"
"Shruti"="Noto Sans Gujarati"
"Raavi"="Noto Sans Gurmukhi"
"Leelawadee UI"="Noto Sans Thai"
"Tahoma"="Noto Sans"
"Segoe UI"="Noto Sans"
"Segoe UI Emoji"="Noto Color Emoji"
'''.replace('\n', '\r\n').encode('utf-16')


def log(*args): print('[desk]', *args, flush=True)


def present_packages():
    # chrome-lock.json is on every desktop disk; apps-lock.json packages are not: apps_install.py installs each one only
    # with the groups that need it (fontconfig comes with vscode/libreoffice only).
    return {p['name']: p['version'] for p in json.loads((HERE / 'chrome-lock.json').read_text())}


def make_lock(update_keyring=False, allow_older=False):
    previous = json.loads(LOCK.read_bytes()) if LOCK.exists() else {}
    keyring = previous.get('keyring') or json.loads((HERE / 'wine-lock.json').read_bytes())['keyring']
    packages, meta, keyring = wi.verified_indices(keyring, previous.get('suites'), allow_older, update_keyring)
    wi.GROUPS, wi.SKIP = GROUPS, SKIP
    selected, skipped, satisfied = wi.resolve(packages, present_packages())
    sdk = []
    for name in SDK_PACKAGES:
        candidates = [s for suite in wi.TRIXIE for s in packages[suite].get(name, [])]
        if not candidates: raise InstallError('no %s in trixie' % name)
        top = max(candidates, key=functools.cmp_to_key(lambda a, b: wi.version_compare(a['Version'], b['Version'])))
        if not wi.POOL_PATH.fullmatch(top['Filename']): raise InstallError('bad Filename for ' + name)
        sdk.append({'name': name, 'version': top['Version'], 'url': wi.MIRRORS['trixie'] + top['Filename'], 'sha256': top['SHA256'],
                    'bytes': int(top['Size']), 'kind': 'deb', 'suite': 'trixie', 'filename': 'debs/' + PurePosixPath(top['Filename']).name})
    for p in list(selected.values()) + sdk:
        p['snapshot'] = wi.snapshot_lookup(p['name'], p['version'], PurePosixPath(p['filename']).name, p['bytes']) or \
            wi.snapshot_url(p['suite'], meta[p['suite']]['date'], p['url'][len(wi.MIRRORS[p['suite']]):])
    lock = {'format': VERSION, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'suites': meta, 'keyring': keyring,
            'present_from_chrome_and_apps_locks': dict(sorted(satisfied.items())), 'skipped': dict(sorted(skipped.items())),
            'packages': sorted(selected.values(), key=lambda p: p['name']), 'sdk': sdk}
    LOCK.write_bytes((json.dumps(lock, indent=1) + '\n').encode())
    log('wrote', LOCK, len(selected), 'packages,', len(sdk), 'sdk packages')
    for p in lock['packages'] + sdk: log('  %-30s %-26s %8.1f KiB %s' % (p['name'], p['version'], p['bytes'] / 1024, ','.join(p.get('needed_by', ['sdk']))))


def prepare_sdk(cache=DEFAULT_CACHE):
    """Headers of SDK_PACKAGES (pinned in desk-lock.json) under out/desk-sdk/usr/include; rebuilt when the lock changes."""
    lock = json.loads(LOCK.read_bytes()); stamp = SDK / 'lock.sha256'
    want = hashlib.sha256(json.dumps(lock['sdk'], sort_keys=True).encode()).hexdigest()
    if stamp.exists() and stamp.read_text().strip() == want: return SDK / 'usr/include'
    if SDK.exists(): shutil.rmtree(SDK)
    for entry in lock['sdk']:
        deb = wi.fetch_package(entry, cache)
        with ai.open_payload(entry, deb) as archive:
            for member in archive:
                name = ai.safe_member(member.name)
                if not member.isfile() or not name.startswith(SDK_PATHS): continue
                target = SDK.joinpath(*PurePosixPath(name).parts)
                target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(archive.extractfile(member).read())
    stamp.write_text(want + '\n')
    log('SDK headers in', SDK / 'usr/include')
    return SDK / 'usr/include'


class DeskInstaller(ai.Installer):
    def existing(self, path, kind, art, bucket, mtime):
        if path == 'usr/include' or path.startswith('usr/include/'):
            self.skip('development headers', '/' + path, bucket); return None
        if art.get('name') in FONT_PACKAGES and kind != 'dir' and path.startswith('usr/share/fonts/') and not KEEP_FONTS.search(path):
            self.skip('font not in the NexusOS selection', '/' + path, bucket); return None
        return super().existing(path, kind, art, bucket, mtime)

    def put_bytes(self, path, data, bucket, now):
        r = self.existing(path, 'file', {'name': 'desk_install', 'app': bucket}, bucket, now)
        if r is None: return
        d, leaf, ent = r
        if ent is not None:
            if not ent.is_dir and not ent.is_link and ent.size == len(data) and ent.first >= 2 and b''.join(self.vol.read_file(ent.first, ent.size)) == data:
                self.stat(bucket)['files_identical'] += 1; return
            self.conflict(path, 'existing file differs (kept)', {'name': 'desk_install'}, bucket); return
        self.vol.write_file(d, leaf, len(data), [data], now); self.changes += 1; self.created.append('/' + path)

    def finish(self, versions):
        now = time.time()
        if 'fonts' in self.groups:
            for conf in CONF_DEFAULTS:
                if self.resolve('/usr/share/fontconfig/conf.avail/' + conf) is not None or not self.vol.writable:
                    self.managed_link('etc/fonts/conf.d/' + conf, '/usr/share/fontconfig/conf.avail/' + conf, 'fonts', now)
            self.put_bytes('etc/fonts/conf.d/60-nexus-noto.conf', NEXUS_FONTS_CONF, 'fonts', now)
            self.put_bytes('usr/share/nexus/wine-fonts.reg', WINE_FONTS_REG, 'fonts', now)
        for (top, n), app in sorted(self.usr_names.items()):           # merged-usr emulation
            target = '/usr/%s/%s' % (top, n)
            d = self.walk(top, False)
            ent = d.names.get(fold(n)) if d else None
            if ent is not None and not (ent.is_link and ent.name == n and self.read_link(ent) == target):
                self.skip('merged-usr link: existing entry kept', '/%s/%s' % (top, n)); continue
            self.managed_link('%s/%s' % (top, n), target, app, now)
        for pkg in self.by_name:
            if pkg not in self.licenses: self.licenses[pkg] = 'none'
        self.write_manifest(versions, now)

    def write_manifest(self, versions, now):
        d = self.walk('nexus', True, now, 'fonts')
        ent = d.names.get(fold('desk-installed.json'))
        old = {}
        if ent is not None and not ent.is_dir and ent.first >= 2 and ent.size:
            try: old = json.loads(b''.join(self.vol.read_file(ent.first, ent.size)))
            except ValueError: old = {}
        packages = {p['name']: p for p in old.get('packages', []) if isinstance(p, dict) and 'name' in p}
        for a in self.artifacts: packages[a['name']] = {'name': a['name'], 'version': a['version'], 'group': a['app']}
        manifest = {'format': VERSION, 'installer': 'arch/x86_64/desk_install.py', 'lock_sha256': self.lock_sha256,
                    'groups': sorted(set(old.get('groups', [])) | set(self.groups)), 'packages': [packages[k] for k in sorted(packages)],
                    'symlinks_created': sorted(set(old.get('symlinks_created', [])) | set(self.managed_links))}
        self.manifest = manifest
        data = (json.dumps(manifest, indent=1) + '\n').encode()
        if ent is None: self.vol.write_file(d, 'desk-installed.json', len(data), [data], now); self.changes += 1; return
        if ent.first >= 2 and b''.join(self.vol.read_file(ent.first, ent.size)) == data: return
        if self.vol.writable and ent.first >= 2:
            old_chain = self.vol.chain(ent.first)
            for c in old_chain: self.vol.fat_set(c, 0)
            self.vol.free += len(old_chain)
        clusters = self.vol.alloc((len(data) + self.vol.cl - 1) // self.vol.cl)
        if self.vol.writable:
            off = 0
            for c, k in self.vol.runs(clusters):
                self.vol.wr(self.vol.cl_off(c), data[off:off + k * self.vol.cl].ljust(k * self.vol.cl, b'\0')); off += k * self.vol.cl
        self.vol.set_entry_data(d, ent, clusters[0] if clusters else 0, len(data), now); self.changes += 1


def install(disk, groups, grow, cache, report_path):
    t0 = time.time()
    lock_bytes = LOCK.read_bytes(); lock = json.loads(lock_bytes)
    artifacts = [dict(p) for p in lock['packages'] if set(p['needed_by']) & groups]
    for a in artifacts: a['app'] = sorted(set(a['needed_by']) & groups)[0]
    paths = {a['name']: wi.fetch_package(a, cache) for a in artifacts}
    log('%d packages verified (%.0f MiB) for %s' % (len(artifacts), sum(a['bytes'] for a in artifacts) / 2**20, ','.join(sorted(groups))))
    versions = {g: {'packages': sorted('%s %s' % (a['name'], a['version']) for a in artifacts if g in a['needed_by'])} for g in sorted(groups)}
    report = {'disk': str(disk), 'groups': sorted(groups), 'lock_sha256': hashlib.sha256(lock_bytes).hexdigest()}
    st0 = disk.stat(); head0 = ai.prefix_digest(disk, 0, ai.BASE)
    vol = ai.Volume(disk, writable=False)
    try:
        ana = DeskInstaller(vol, artifacts, paths, groups); ana.lock_sha256 = report['lock_sha256']
        for art in artifacts: ana.install_artifact(art)
        ana.finish(versions)
        needed, free_clusters, cl, spc, ncl = vol.allocated, vol.free_start, vol.cl, vol.spc, vol.nclusters
    finally:
        vol.close()
    log('analysis: %d changes, %.1f MiB to allocate, %.1f MiB free, %d conflicts, %d fatal' % (
        ana.changes, needed * cl / 2**20, free_clusters * cl / 2**20, len(ana.conflicts), len(ana.fatal)))
    if ana.fatal:
        report.update(result='aborted', fatal=ana.fatal, **ai.details(ana)); ai.write_report(report_path, report)
        raise SystemExit('installation aborted before writing (see %s)' % report_path)
    do_grow, grow_clusters = False, 0
    if grow:
        new_size = grow * 1024 * 1024
        if new_size < st0.st_size: raise SystemExit('--grow is smaller than the disk')
        do_grow = new_size > st0.st_size
        if do_grow: grow_clusters = ai.format_geometry((new_size - ai.BASE) // ai.SECTOR, spc)[3] - ncl
    if needed > free_clusters + grow_clusters:
        raise SystemExit('not enough space: need %.1f MiB, have %.1f MiB' % (needed * cl / 2**20, (free_clusters + grow_clusters) * cl / 2**20))
    if ana.changes == 0 and not do_grow:
        report.update(result='no changes', **ai.details(ana)); ai.write_report(report_path, report); log('nothing to do'); return 0
    temp = disk.with_name('.desk-install-%d.img' % os.getpid())
    try:
        if do_grow: ai.grow(disk, temp, grow)
        else: ai.sparse_copy(disk, temp)
        vol = ai.Volume(temp, writable=True)
        try:
            inst = DeskInstaller(vol, artifacts, paths, groups); inst.lock_sha256 = report['lock_sha256']
            for art in artifacts: inst.install_artifact(art)
            inst.finish(versions)
            if inst.fatal: raise InstallError('fatal problems while writing: %r' % inst.fatal[:5])
            vol.flush()
            entry_points = {'fonts': ['/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc', '/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf',
                                      '/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf', '/etc/fonts/conf.d/60-nexus-noto.conf', '/usr/bin/fc-cache'],
                            'auth': ['/usr/lib/x86_64-linux-gnu/libcrypt.so.1']}
            links = ai.check_links(inst, sorted({p for g in groups for p in entry_points.get(g, [])}))
            broken = [p for p, state in links.items() if state == 'missing or dangling']
            if broken: raise InstallError('entry points do not resolve after the install: ' + ', '.join(broken))
            inst.dangling = ai.dangling_links(inst)
        finally:
            vol.close()
        skip = (446 + 12, 446 + 16) if do_grow else None
        if ai.prefix_digest(temp, ai.NPFS, ai.BASE) != ai.prefix_digest(disk, ai.NPFS, ai.BASE): raise InstallError('NPFS region changed')
        if ai.prefix_digest(temp, 0, ai.BASE, skip) != ai.prefix_digest(disk, 0, ai.BASE, skip): raise InstallError('reserved prefix changed')
        st1 = disk.stat()
        if (st1.st_size, st1.st_mtime_ns) != (st0.st_size, st0.st_mtime_ns) or ai.prefix_digest(disk, 0, ai.BASE) != head0:
            raise InstallError('disk changed during installation; stop QEMU and retry')
        os.replace(temp, disk)
    finally:
        if temp.exists(): temp.unlink()
    report.update(result='installed', changes=inst.changes, seconds=round(time.time() - t0, 1), link_checks=links, **ai.details(inst))
    ai.write_report(report_path, report)
    tot = report['totals']
    log('installed: %d files (%.1f MiB), %d identical, %d symlinks, %d conflicts; report %s' % (
        tot['files_added'], tot['bytes_added'] / 2**20, tot['files_identical'], tot['symlinks_added'], len(inst.conflicts), report_path))
    return 0


def fccache(disk, work, in_place=False):
    """Build the system fontconfig cache: boot the disk once with out-of-tree init fccache_user.c (it runs fc-cache with
    the cache directory redirected to /home/nexus/.fccache-build, the only writable place on a FAT root), then copy the
    cache files into /var/cache/fontconfig (the font directories, and so the caches' validity, are not touched).
    Default: on a sparse copy of the disk (needs as much free host space as the disk holds). in_place: a scratch disk
    of your own is booted itself; its /sbin/init is restored and the build folder removed afterwards."""
    b = importlib.util.module_from_spec(spec := importlib.util.spec_from_file_location('desk_build', HERE / 'build.py')); spec.loader.exec_module(b)
    fatput = importlib.util.module_from_spec(spec := importlib.util.spec_from_file_location('desk_fatput', ROOT / 'userland/fatput.py')); spec.loader.exec_module(fatput)
    if 'new folder' in str(disk).lower(): raise InstallError('refusing the real desktop tree')
    work.mkdir(parents=True, exist_ok=True)
    init = work / 'fccache_init.elf'
    b.run(b.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror', HERE / 'fccache_user.c', '-o', init)
    def read_file(image, parts):
        fs = fatput.Fat32(str(image), readonly=True)
        try:
            d = fs.lookup_dir(parts[:-1]); found = fs.find_name(d, parts[-1])
            if not found: return None
            rec = found[2]; return fs.read_chain(int.from_bytes(rec[20:22], 'little') << 16 | int.from_bytes(rec[26:28], 'little'), int.from_bytes(rec[28:32], 'little'))
        finally: fs.close()
    target = disk if in_place else work / 'fccache-disk.img'
    original_init = read_file(disk, ['sbin', 'init'])
    if original_init is None: raise InstallError('the disk has no /sbin/init')
    (work / 'init.saved').write_bytes(original_init)
    if not in_place: target.unlink(missing_ok=True); ai.sparse_copy(disk, target)
    try:
        if fatput.edit(['fatput', str(target), 'put', str(init), 'sbin/init']) != 0: raise InstallError('staging the cache builder failed')
        serial = work / 'fccache-serial.log'; serial.unlink(missing_ok=True)
        p = subprocess.run([str(b.QEMU), '-cpu', 'max', '-m', '1024', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
                            '-drive', f'file={target},format=raw,if=ide', '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
                            '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], timeout=900)
        log_text = serial.read_text(errors='replace')
        if '[64] FCCACHE DONE' not in log_text: raise InstallError('fc-cache run failed (exit %s):\n%s' % (p.returncode, log_text[-3000:]))
        names = re.findall(r'\[64\] FCCACHE FILE (\S+)', log_text); files = {}
        for name in names:
            if not re.fullmatch(r'[0-9a-f]{32}-le64\.cache-\d+|CACHEDIR\.TAG', name): raise InstallError('unexpected cache file ' + name)
            data = read_file(target, ['home', 'nexus', '.fccache-build', name])
            if data is None: raise InstallError('missing cache file ' + name)
            files[name] = data
        if not any('.cache-' in n for n in files): raise InstallError('no cache files produced')
        staged = work / 'cache'; shutil.rmtree(staged, ignore_errors=True); staged.mkdir()
        args = ['fatput', str(disk)]
        if in_place:   # the scratch disk itself: its init back, the build folder away
            args += ['put', str(work / 'init.saved'), 'sbin/init']
            for name in names + ['fonts.conf']: args += ['rm', 'home/nexus/.fccache-build/' + name]
        for name, data in sorted(files.items()):
            (staged / name).write_bytes(data); args += ['put', str(staged / name), 'var/cache/fontconfig/' + name]
        if in_place: args += ['rmdir', 'home/nexus/.fccache-build']   # last: the emptied build folder (fatput rmdir refuses a non-empty one)
        if fatput.edit(args) != 0: raise InstallError('writing the cache into the image failed')
        log('fontconfig cache: %d files into /var/cache/fontconfig (%d KiB)' % (len(files), sum(len(d) for d in files.values()) // 1024))
    except BaseException:
        if in_place: fatput.edit(['fatput', str(disk), 'put', str(work / 'init.saved'), 'sbin/init'])
        raise
    finally:
        if not in_place: target.unlink(missing_ok=True)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['lock']:
        lp = argparse.ArgumentParser(prog='desk_install.py lock'); lp.add_argument('--update-keyring', action='store_true'); lp.add_argument('--allow-older', action='store_true')
        a = lp.parse_args(argv[1:]); make_lock(a.update_keyring, a.allow_older); return 0
    if argv[:1] == ['sdk']: prepare_sdk(); return 0
    if argv[:1] == ['fccache']:
        fp = argparse.ArgumentParser(prog='desk_install.py fccache'); fp.add_argument('disk'); fp.add_argument('--work', default=str(OUT / 'fccache-work')); fp.add_argument('--in-place', action='store_true')
        a = fp.parse_args(argv[1:]); fccache(Path(a.disk).resolve(), Path(a.work).resolve(), a.in_place); return 0
    if argv[:1] != ['install']: raise SystemExit(__doc__)
    ap = argparse.ArgumentParser(prog='desk_install.py install'); ap.add_argument('disk'); ap.add_argument('--grow', type=int, metavar='MIB')
    ap.add_argument('--cache', default=str(DEFAULT_CACHE)); ap.add_argument('--only', default='fonts,auth'); ap.add_argument('--report')
    args = ap.parse_args(argv[1:])
    disk = Path(args.disk).resolve()
    if not disk.is_file(): raise SystemExit('no such disk image: %s' % disk)
    groups = {x.strip() for x in args.only.split(',') if x.strip()}
    if not groups or groups - set(GROUPS): raise SystemExit('--only takes a comma list of ' + ','.join(GROUPS))
    cache = Path(args.cache).resolve(); cache.mkdir(parents=True, exist_ok=True)
    return install(disk, groups, args.grow, cache, Path(args.report).resolve() if args.report else cache / 'desk-install-report.json')


if __name__ == '__main__':
    try: sys.exit(main())
    except InstallError as e: raise SystemExit('desk_install: ' + str(e))
