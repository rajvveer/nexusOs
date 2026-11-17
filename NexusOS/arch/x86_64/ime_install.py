#!/usr/bin/env python3
"""fcitx5 input methods from Debian 13 (trixie) for an Aurora desktop disk: Pinyin (fcitx5-chinese-addons), Japanese
(fcitx5-anthy), Korean (fcitx5-hangul), with fcitx5 running as Aurora's zwp_input_method_v2 client.

    python -I arch/x86_64/ime_install.py lock [--update-keyring] [--allow-older]   # re-resolve ime-lock.json (signed indices)
    python -I arch/x86_64/ime_install.py install <disk.img> [--cache DIR] [--report PATH]
    python -I arch/x86_64/ime_install.py overlay <base.img> <overlay.qcow2> [--cache DIR] [--extra HOST=GUEST ...]
                                                             # the same into a qcow2 overlay over a read-only raw base

ime-lock.json pins every .deb (URL, snapshot URL, size, SHA-256) with the signed InRelease of its suite; `lock` checks the
InRelease signatures against Debian's archive keys (the keyring of wine-lock.json) and resolves the dependency closure of
fcitx5 + the three engines. Packages of chrome-lock.json (on every desktop disk) and desk-lock.json (the fonts group:
Noto CJK draws the candidates) count as present. Installing reuses desk_install.py / apps_install.py's FAT32 engine
(existing files are never overwritten); `overlay` runs it on a copy-on-write view of the base (tests/x64_overlay.py) and
writes only the changed clusters into a qcow2 overlay, so no multi-GB copy is made.

Besides the packages the installer writes:
  /etc/xdg/fcitx5/profile            the system default input-method group: keyboard-us, pinyin, anthy, hangul
  /etc/xdg/fcitx5/config             active by default (pinyin); Ctrl+Shift+F9 cycles pinyin/anthy/hangul, Ctrl+Shift+F10
                                     toggles to plain keyboard (Ctrl+Space is Aurora's Spotlight)
  /etc/xdg/fcitx5/conf/{pinyin,anthy}.conf  predictable candidate choice (no cloud pinyin, no prediction)
  /usr/libexec/nexus-fcitx5          the program Aurora starts (~/.aurora/input-method.conf): fcitx5 with only the
                                     addons a Wayland input method needs (no X11, D-Bus, notification or XIM frontends)
  /usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0/immodules.cache  GTK 3's IM module list (every module present: im-wayland,
                                     im-fcitx5, ...), if absent: GTK 3 applications then speak text-input-v3
  /usr/lib/x86_64-linux-gnu/gtk-4.0/4.0.0/immodules/giomodule.cache  with GTK 4 and its IM modules, if absent
  /nexus/ime-installed.json          what was installed (lock digest, packages)
No file under the user's ~/.config is written: fcitx5 reads the system defaults from /etc/xdg/fcitx5."""
from pathlib import Path, PurePosixPath
import argparse
import builtins
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = HERE / 'ime-lock.json'
OUT = HERE / 'out'
DEFAULT_CACHE = Path(os.environ.get('NEXUS_IME_CACHE', str(OUT / 'ime-packages')))
VERSION = 1


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module); return module


desk = _load('ime_desk_install', HERE / 'desk_install.py')
wi, ai = desk.wi, desk.ai
InstallError = wi.InstallError
GROUPS = {'ime': {'trixie': ['fcitx5', 'fcitx5-chinese-addons', 'fcitx5-anthy', 'fcitx5-hangul']}}
SKIP = {
    'libpam0g': 'not needed by the libraries fcitx5 loads',
    'dbus': 'no D-Bus daemon: nexus-fcitx5 runs fcitx5 without its D-Bus addons',
    'dbus-x11': 'no X11 session bus launcher', 'dbus-user-session': 'no user session bus',
    'default-dbus-session-bus': 'no user session bus', 'dbus-session-bus': 'no user session bus',
    'fcitx5-frontend-qt5': 'Qt applications are not installed', 'fcitx5-frontend-qt6': 'Qt applications are not installed',
    'fcitx5-config-qt': 'Qt configuration tool', 'kde-config-fcitx5': 'KDE configuration tool',
    'im-config': 'Debian session integration (X11 / systemd user sessions)',
    'fcitx5-frontend-gtk2': 'GTK applications use Wayland text-input-v3', 'fcitx5-frontend-gtk3': 'GTK applications use Wayland text-input-v3',
    'fcitx5-frontend-gtk4': 'GTK applications use Wayland text-input-v3',
    'fcitx5-frontend-all': 'toolkit modules: applications use Wayland text-input-v3',
    'qtwayland5': 'Qt', 'qt6-wayland': 'Qt',
    'libtext-iconv-perl': "dictionaries-common's maintainer scripts only (Perl); no runtime use",
}
PROFILE = b'''[Groups/0]
# Group Name
Name=Default
# Layout
Default Layout=us
# Default Input Method
DefaultIM=pinyin

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=pinyin
Layout=

[Groups/0/Items/2]
Name=anthy
Layout=

[Groups/0/Items/3]
Name=hangul
Layout=

[GroupOrder]
0=Default
'''
CONFIG = b'''[Hotkey]
EnumerateWithTriggerKeys=False
EnumerateSkipFirst=True

[Hotkey/TriggerKeys]
0=Control+Shift+F10

[Hotkey/EnumerateForwardKeys]
0=Control+Shift+F9

[Hotkey/ActivateKeys]
0=Control+Shift+F12

[Hotkey/DeactivateKeys]
0=Control+Shift+F11

[Behavior]
ActiveByDefault=True
ShareInputState=All
PreeditEnabledByDefault=True
ShowInputMethodInformation=False
showInputMethodInformationWhenFocusIn=False
CompactInputMethodInformation=False
ShowFirstInputMethodInformation=False
DefaultPageSize=5
PreloadInputMethod=True
'''
PINYIN_CONF = b'''# NexusOS: deterministic candidates (no cloud pinyin, no prediction)
CloudPinyinEnabled=False
Prediction=False
PageSize=5
SpellEnabled=False
SymbolsEnabled=False
ChaiziEnabled=False
ExtBEnabled=False
'''
ANTHY_CONF = b'''[General]
# NexusOS: predictable conversion
PageSize=5
'''
WRAPPER = b'''#!/bin/sh
# NexusOS: Aurora starts this from ~/.aurora/input-method.conf with WAYLAND_SOCKET set (arch/x86_64/ime_install.py).
# fcitx5 as a Wayland input method only: no X11, no D-Bus (no session bus runs), no tray / notifications, no XIM.
export XDG_CONFIG_DIRS="/etc/xdg${XDG_CONFIG_DIRS:+:$XDG_CONFIG_DIRS}"
unset DISPLAY
exec /usr/bin/fcitx5 --disable=dbus,dbusfrontend,ibusfrontend,xim,xcb,fcitx4frontend,notificationitem,notifications,kimpanel,cloudpinyin "$@"
'''
# GTK IM module caches. GTK 3 loads IM modules only through immodules.cache, which Debian's libgtk-3-0t64 postinst
# writes with gtk-query-immodules-3.0 (the desk/chrome installers run no maintainer scripts). Without it every GTK 3
# application (Firefox included) uses the built-in simple context and never binds zwp_text_input_v3. The installer
# writes the cache when it is missing, with one entry per module in the immodules directory (im-wayland from
# libgtk-3-0t64, im-fcitx5 from fcitx5-frontend-gtk3, im-ibus, ... if present): the GtkIMContextInfo records are read
# from each module's ELF file (five relocated string pointers whose fourth is LOCALEDIR), which is what
# gtk-query-immodules-3.0 prints. GTK 4 has the Wayland context built in; its loadable IM modules (gtk-4.0/4.0.0/
# immodules, GIO extension point gtk-im-module) get a giomodule.cache when GTK 4 is installed. Existing caches are kept.
GTK3_DIR = 'usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0'
GTK3_CACHE = GTK3_DIR + '/immodules.cache'
GTK4_LIBRARY = 'usr/lib/x86_64-linux-gnu/libgtk-4.so.1'
GTK4_MODULES = 'usr/lib/x86_64-linux-gnu/gtk-4.0/4.0.0/immodules'
GTK3_WAYLAND_ENTRY = ('wayland', 'Wayland', 'gtk30', '/usr/share/locale', '')   # gtk-query-immodules-3.0, GTK 3.24


def elf_im_infos(data):
    """GtkIMContextInfo records of a GTK 3 IM module: [(id, name, domain, localedir, default locales)]."""
    import struct
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1: return []
    phoff, shoff = struct.unpack_from('<QQ', data, 0x20); phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', data, 0x36)
    loads = []
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if kind == 1: loads.append((vaddr, offset, filesz))
    def file_offset(vaddr):
        for start, offset, size in loads:
            if start <= vaddr < start + size: return offset + vaddr - start
        return None
    def string(vaddr):
        o = file_offset(vaddr)
        if o is None: return None
        end = data.find(b'\0', o)
        if end < 0 or end - o > 256: return None
        try: return data[o:end].decode('utf-8')
        except UnicodeDecodeError: return None
    relocated = {}
    for i in range(shnum):
        _, kind, _, _, offset, size, _, _, _, entsize = struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)
        if kind != 4 or entsize != 24: continue   # SHT_RELA
        for k in range(size // 24):
            r_offset, r_info, r_addend = struct.unpack_from('<QQq', data, offset + k * 24)
            if r_info & 0xFFFFFFFF == 8: relocated[r_offset] = r_addend   # R_X86_64_RELATIVE
    infos = []
    for at in sorted(relocated):
        if not all(at + 8 * n in relocated for n in range(5)): continue
        fields = [string(relocated[at + 8 * n]) for n in range(5)]
        if None in fields or not fields[3].startswith('/') or not fields[0] or fields in infos: continue
        if not all(ch.isalnum() or ch in '-_.' for ch in fields[0]): continue
        infos.append(fields)
    return [tuple(f) for f in infos]


def gtk3_cache_text(modules):
    """modules: [(guest path, ELF bytes)] -> immodules.cache content (gtk-query-immodules-3.0 layout)."""
    def q(text): return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'
    lines = ['# GTK+ Input Method Modules file', '# Automatically generated file, do not edit',
             '# Created by NexusOS arch/x86_64/ime_install.py (the records gtk-query-immodules-3.0 prints)', '#',
             '# ModulesPath = /' + GTK3_DIR + '/immodules', '#']
    for path, data in sorted(modules):
        infos = elf_im_infos(data)
        if not infos and path.endswith('/im-wayland.so'): infos = [GTK3_WAYLAND_ENTRY]
        if not infos: continue
        lines.append(q(path) + ' ')
        lines.extend(' '.join(q(f) for f in info) + ' ' for info in infos)
        lines.append('')
    return ('\n'.join(lines) + '\n').encode()


def write_gtk_caches(inst, now, bucket='ime'):
    """Writes the missing GTK IM caches into the disk of installer `inst` (desk_install.DeskInstaller)."""
    def files_in(directory, suffix):
        d = inst.walk(directory, False)
        if d is None: return []
        out = []
        for ent in d.names.values():
            if ent.is_dir or not ent.name.endswith(suffix): continue
            data = b''.join(inst.vol.read_file(ent.first, ent.size)) if not ent.is_link and ent.first >= 2 else b''
            out.append(('/' + directory + '/' + ent.name, data))
        return out
    modules = files_in(GTK3_DIR + '/immodules', '.so')
    if modules and inst.resolve('/' + GTK3_CACHE) is None: inst.put_bytes(GTK3_CACHE, gtk3_cache_text(modules), bucket, now)
    elif not modules: log('GTK 3 IM modules not installed: no immodules.cache written')
    gtk4 = files_in(GTK4_MODULES, '.so') if inst.resolve('/' + GTK4_LIBRARY) is not None else []
    if gtk4 and inst.resolve('/' + GTK4_MODULES + '/giomodule.cache') is None:
        text = ''.join('%s: gtk-im-module\n' % path.rsplit('/', 1)[1] for path, _ in sorted(gtk4))
        inst.put_bytes(GTK4_MODULES + '/giomodule.cache', text.encode(), bucket, now)


def gtk_cache_selftest(deb):
    """The parser against Debian's libgtk-3-0t64 (python -I ime_install.py gtk-selftest <libgtk-3-0t64.deb>)."""
    import io, tarfile
    data = Path(deb).read_bytes(); pos = 8; modules = []
    while pos < len(data):
        name = data[pos:pos + 16].decode().strip(); size = int(data[pos + 48:pos + 58]); body = data[pos + 60:pos + 60 + size]
        if name.startswith('data.tar'):
            with tarfile.open(fileobj=io.BytesIO(body), mode='r:*') as t:
                for m in t.getmembers():
                    if m.isfile() and '/immodules/' in m.name and m.name.endswith('.so'):
                        modules.append(('/' + m.name.lstrip('./'), t.extractfile(m).read()))
        pos += 60 + size + (size & 1)
    text = gtk3_cache_text(modules).decode()
    print(text)
    wayland = [m for m in modules if m[0].endswith('/im-wayland.so')]
    assert wayland and elf_im_infos(wayland[0][1]) == [GTK3_WAYLAND_ENTRY], elf_im_infos(wayland[0][1]) if wayland else 'no im-wayland.so'
    assert all(elf_im_infos(d) for _, d in modules), [p for p, d in modules if not elf_im_infos(d)]
    print('GTK CACHE SELFTEST PASS: %d modules, im-wayland = %r' % (len(modules), GTK3_WAYLAND_ENTRY))


ENTRY_POINTS = ['/usr/bin/fcitx5', '/usr/lib/x86_64-linux-gnu/fcitx5/libwaylandim.so', '/usr/lib/x86_64-linux-gnu/fcitx5/libpinyin.so',
                '/usr/lib/x86_64-linux-gnu/fcitx5/libanthy.so', '/usr/lib/x86_64-linux-gnu/fcitx5/libhangul.so',
                '/usr/lib/x86_64-linux-gnu/fcitx5/libclassicui.so', '/usr/libexec/nexus-fcitx5']


def log(*args): print('[ime]', *args, flush=True)


def present_packages():
    present = {p['name']: p['version'] for p in json.loads((HERE / 'chrome-lock.json').read_text())}
    for p in json.loads((HERE / 'desk-lock.json').read_text())['packages']: present.setdefault(p['name'], p['version'])
    return present


def make_lock(update_keyring=False, allow_older=False):
    previous = json.loads(LOCK.read_bytes()) if LOCK.exists() else {}
    keyring = previous.get('keyring') or json.loads((HERE / 'wine-lock.json').read_bytes())['keyring']
    packages, meta, keyring = wi.verified_indices(keyring, previous.get('suites'), allow_older, update_keyring)
    wi.GROUPS, wi.SKIP = GROUPS, SKIP
    selected, skipped, satisfied = wi.resolve(packages, present_packages())
    for p in selected.values():
        p['snapshot'] = wi.snapshot_lookup(p['name'], p['version'], PurePosixPath(p['filename']).name, p['bytes']) or \
            wi.snapshot_url(p['suite'], meta[p['suite']]['date'], p['url'][len(wi.MIRRORS[p['suite']]):])
    lock = {'format': VERSION, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'suites': meta, 'keyring': keyring,
            'present_from_chrome_and_desk_locks': dict(sorted(satisfied.items())), 'skipped': dict(sorted(skipped.items())),
            'packages': sorted(selected.values(), key=lambda p: p['name'])}
    LOCK.write_bytes((json.dumps(lock, indent=1) + '\n').encode())
    log('wrote', LOCK, len(selected), 'packages, %.1f MiB' % (sum(p['bytes'] for p in selected.values()) / 2**20))
    for p in lock['packages']: log('  %-34s %-26s %8.1f KiB' % (p['name'], p['version'], p['bytes'] / 1024))


class ImeInstaller(desk.DeskInstaller):
    def finish(self, versions):
        now = time.time()
        for path, data in (('etc/xdg/fcitx5/profile', PROFILE), ('etc/xdg/fcitx5/config', CONFIG),
                           ('etc/xdg/fcitx5/conf/pinyin.conf', PINYIN_CONF), ('etc/xdg/fcitx5/conf/anthy.conf', ANTHY_CONF),
                           ('usr/libexec/nexus-fcitx5', WRAPPER)):
            self.put_bytes(path, data, 'ime', now)
        write_gtk_caches(self, now)   # GTK 3 immodules.cache / GTK 4 giomodule.cache when missing
        for (top, n), app in sorted(self.usr_names.items()):           # merged-usr emulation (as desk_install)
            target = '/usr/%s/%s' % (top, n)
            d = self.walk(top, False)
            ent = d.names.get(desk.fold(n)) if d else None
            if ent is not None and not (ent.is_link and ent.name == n and self.read_link(ent) == target):
                self.skip('merged-usr link: existing entry kept', '/%s/%s' % (top, n)); continue
            self.managed_link('%s/%s' % (top, n), target, app, now)
        for pkg in self.by_name:
            if pkg not in self.licenses: self.licenses[pkg] = 'none'
        self.write_manifest(versions, now)

    def write_manifest(self, versions, now):
        d = self.walk('nexus', True, now, 'ime')
        manifest = {'format': VERSION, 'installer': 'arch/x86_64/ime_install.py', 'lock_sha256': self.lock_sha256,
                    'packages': [{'name': a['name'], 'version': a['version']} for a in sorted(self.artifacts, key=lambda a: a['name'])]}
        self.manifest = manifest
        data = (json.dumps(manifest, indent=1) + '\n').encode()
        if d.names.get(desk.fold('ime-installed.json')) is None:
            self.vol.write_file(d, 'ime-installed.json', len(data), [data], now); self.changes += 1


def run_install(disk, cache, report_path, temp_hook=None):
    """Install into `disk` (desk_install.install's flow: analysis, then a working copy that replaces the disk)."""
    t0 = time.time()
    lock_bytes = LOCK.read_bytes(); lock = json.loads(lock_bytes)
    artifacts = [dict(p, app='ime') for p in lock['packages']]
    paths = {a['name']: wi.fetch_package(a, cache) for a in artifacts}
    log('%d packages verified (%.0f MiB)' % (len(artifacts), sum(a['bytes'] for a in artifacts) / 2**20))
    versions = {'ime': {'packages': sorted('%s %s' % (a['name'], a['version']) for a in artifacts)}}
    digest = hashlib.sha256(lock_bytes).hexdigest()
    vol = ai.Volume(disk, writable=False)
    try:
        ana = ImeInstaller(vol, artifacts, paths, {'ime'}); ana.lock_sha256 = digest
        for art in artifacts: ana.install_artifact(art)
        ana.finish(versions)
        needed, free_clusters, cl = vol.allocated, vol.free_start, vol.cl
    finally:
        vol.close()
    log('analysis: %d changes, %.1f MiB to allocate, %.1f MiB free, %d conflicts, %d fatal' % (
        ana.changes, needed * cl / 2**20, free_clusters * cl / 2**20, len(ana.conflicts), len(ana.fatal)))
    if ana.fatal: raise InstallError('fatal problems: %r' % ana.fatal[:5])
    if needed > free_clusters: raise InstallError('not enough space on the disk (%.1f MiB needed)' % (needed * cl / 2**20))
    temp = disk.with_name('.ime-install-%d.img' % os.getpid())
    try:
        ai.sparse_copy(disk, temp)
        vol = ai.Volume(temp, writable=True)
        try:
            inst = ImeInstaller(vol, artifacts, paths, {'ime'}); inst.lock_sha256 = digest
            for art in artifacts: inst.install_artifact(art)
            inst.finish(versions)
            if inst.fatal: raise InstallError('fatal problems while writing: %r' % inst.fatal[:5])
            vol.flush()
            links = ai.check_links(inst, ENTRY_POINTS)
            broken = [p for p, state in links.items() if state == 'missing or dangling']
            if broken: raise InstallError('entry points do not resolve after the install: ' + ', '.join(broken))
        finally:
            vol.close()
        if ai.prefix_digest(temp, 0, ai.BASE) != ai.prefix_digest(disk, 0, ai.BASE): raise InstallError('reserved prefix changed')
        os.replace(temp, disk)
    finally:
        if temp.exists(): temp.unlink()
    report = {'disk': str(disk), 'lock_sha256': digest, 'result': 'installed', 'changes': inst.changes,
              'seconds': round(time.time() - t0, 1), 'link_checks': links, **ai.details(inst)}
    ai.write_report(report_path, report)
    log('installed: %d changes in %.0f s; report %s' % (inst.changes, time.time() - t0, report_path))
    return inst


def overlay(base, target, cache, extra=()):
    """Install into a copy-on-write view of the read-only raw `base` and write the changed clusters as qcow2 `target`
    (with optional extra files `extra` = [(host path, guest path)])."""
    ov = _load('ime_x64_overlay', ROOT / 'tests/x64_overlay.py')
    base, target = Path(base).resolve(), Path(target)
    work = Path(tempfile.mkdtemp(prefix='nx-ime-', dir=target.parent))
    view = ov.CowFile(base, work / 'fsync.tmp'); view_close = view.close; view.close = lambda: None
    temp = {'path': None}
    try:
        def ai_open(path, mode='r', *a, **k):
            p = str(path)
            if any(c in mode for c in '+wa') and (Path(p).resolve() == base or p == temp['path']): return view
            if p == temp['path']: return builtins.open(base, mode, *a, **k)
            return builtins.open(path, mode, *a, **k)
        ai.open = ai_open; ai.os = ov._OsProxy()
        ai.sparse_copy = lambda src, dst, length=None: temp.__setitem__('path', str(dst))
        ai.prefix_digest = lambda *a, **k: b''
        global os
        saved_os = os
        try:
            os = ov._OsProxy()
            run_install(base, cache, cache / 'ime-overlay-report.json')
        finally:
            os = saved_os
        if extra:
            fat = ov._fatput(); fat.open = ov._view_open(view, base)
            args = ['fatput', str(base)]
            for host, guest in extra: args += ['put', str(host), guest]
            fat.edit(args)
        view_close(); closed = True
        return ov.snapshot(view, base, target)
    finally:
        if not locals().get('closed'): view_close()
        for p in work.iterdir(): p.unlink()
        work.rmdir()


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['lock']:
        lp = argparse.ArgumentParser(prog='ime_install.py lock'); lp.add_argument('--update-keyring', action='store_true'); lp.add_argument('--allow-older', action='store_true')
        a = lp.parse_args(argv[1:]); make_lock(a.update_keyring, a.allow_older); return 0
    if argv[:1] == ['install']:
        ap = argparse.ArgumentParser(prog='ime_install.py install'); ap.add_argument('disk'); ap.add_argument('--cache', default=str(DEFAULT_CACHE)); ap.add_argument('--report')
        a = ap.parse_args(argv[1:]); disk = Path(a.disk).resolve()
        if 'new folder' in str(disk).lower(): raise InstallError('refusing the real desktop tree')
        cache = Path(a.cache).resolve(); cache.mkdir(parents=True, exist_ok=True)
        run_install(disk, cache, Path(a.report).resolve() if a.report else cache / 'ime-install-report.json'); return 0
    if argv[:1] == ['overlay']:
        ap = argparse.ArgumentParser(prog='ime_install.py overlay'); ap.add_argument('base'); ap.add_argument('target')
        ap.add_argument('--cache', default=str(DEFAULT_CACHE)); ap.add_argument('--extra', nargs='*', default=[])
        a = ap.parse_args(argv[1:]); cache = Path(a.cache).resolve(); cache.mkdir(parents=True, exist_ok=True)
        if 'new folder' in str(Path(a.base).resolve()).lower(): raise InstallError('refusing the real desktop tree')
        overlay(a.base, a.target, cache, [tuple(x.split('=', 1)) for x in a.extra]); return 0
    if argv[:1] == ['gtk-selftest']: gtk_cache_selftest(argv[1]); return 0
    raise SystemExit(__doc__)


if __name__ == '__main__':
    try: sys.exit(main())
    except InstallError as e: raise SystemExit('ime_install: ' + str(e))
