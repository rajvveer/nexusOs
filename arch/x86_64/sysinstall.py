#!/usr/bin/env python3
"""Install NexusOS system components onto an ext4 NexusOS disk image (host side; the image must not be in use).

    python -I arch/x86_64/sysinstall.py services <disk.img> [--aurora] [--test-unit NAME=FILE ...]
    python -I arch/x86_64/sysinstall.py install <new.img> [--source FAT_DESKTOP.img] [--size MiB] [--cache DIR] [--config K=V ...]
    python -I arch/x86_64/sysinstall.py migrate <old-fat.img> <new.img> [--size MiB] [--cache DIR] [--config K=V ...]

services   the service manager and journal: /usr/lib/nexus/nexusd (set-user-ID root: Aurora, the desktop user's pid 1,
           starts it and it becomes root), /usr/bin/nexusctl, vendor units /usr/lib/nexus/services/*.service,
           /usr/sbin/policy-rc.d (Debian maintainer scripts hand daemon start/stop to nexusd), /etc/nexus/journald.conf
           (only when absent), /etc/nexus/{enabled,disabled}/. --aurora also installs out/desktop.elf as /usr/sbin/init,
           out/aurora_app.elf as /usr/bin/aurora-app and the account helper out/nexus-auth.elf as /usr/libexec/nexus-auth
           (set-user-ID root: Aurora's greeter, lock screen and Settings > Users check and change passwords through it).
           --test-unit installs an extra unit into /etc/nexus/services (tests only).
install    a new NexusOS installation on an ext4 root (the default file system for new installs): the NexusOS userland of
           --source (a FAT32 desktop disk, only read; its .config/.cache are not taken) converted by userland/fat2ext4.py,
           the Debian 13 base staged by debian_base.py (installed by dpkg at the first boot), then `services --aurora`.
           --config takes the installer settings (USERNAME, PASSWORD or PASSWORD_HASH, FULLNAME, HOSTNAME, TIMEZONE,
           LOCALES, LANG_DEFAULT; see debian_base.py).
migrate    the same from the user's own FAT32 disk, keeping every file (browser profiles, .config, .cache included). The
           old disk is only read. Every file is then compared with its copy (content SHA-256, symbolic link targets,
           folder contents); the report (<new>.migration.json) lists any difference and the home-folder names whose
           case could not be restored (8.3 names, see userland/fat2ext4-case.txt). Boot the new disk instead of the
           old one when the report shows no difference; keep the old disk until then.
"""
from pathlib import Path
import argparse
import importlib.util
import sys
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
# test disks: the host test's hook runs last; the session's main program waits for it (tests/test_x64_*.py)
TEST_HOOK_UNIT = b'''[Unit]
Description=Test hook (host test script, test disks only)
After=nexus-firstboot.service dbus.service cron.service chrony.service cups.service nexus-printers.service svctest.service nexus-update-check.service

[Service]
Type=oneshot
ExecStart=/usr/lib/nexus/tests/test-hook
StandardOutput=console
TimeoutStartSec=infinity

[Install]
WantedBy=multi-user
DefaultEnabled=yes
'''
TEST_WAIT = b'''#!/bin/sh
# test session main program: wait for the test hook (the last boot unit), then end the session (nexusd stops, power off).
# /run is on the root file system: the mark carries this boot's kernel boot_id, so one left from a previous boot is ignored.
boot=$(cat /proc/sys/kernel/random/boot_id)
i=0; while [ "$(cat /run/test-hook.done 2>/dev/null)" != "$boot" ] && [ $i -lt 7200 ]; do sleep 1; i=$((i + 1)); done
sleep 2
'''
UPDATE_CONF = b'''# NexusOS core updates (nexus-update; sourced by /bin/sh as root).
# Signature check of a bundle's MANIFEST: COMMAND SIGNATURE MANIFEST, exit status 0 = valid (update_bundle.py).
#VERIFY_COMMAND="sqv --keyring /usr/share/keyrings/nexusos-update.pgp"
# Trial boots of a new slot before the UEFI loader rolls back to the previous one (1-9).
#TRIES=3
'''
OFF_BY_DEFAULT = {}
# Disabled-markers earlier installers wrote for units that work now: removed (a user's own `nexusctl disable` stays)
LEGACY_OFF = {'avahi-daemon.service': b'# disabled by NexusOS: needs IPv4 multicast (see the unit); nexusctl enable avahi-daemon turns it on\n'}
NOTIFY_ACTIVATION = b'''[D-BUS Service]
Name=org.freedesktop.Notifications
Exec=/usr/lib/nexus/nexus-notifyd
'''
# UPower + power-profiles-daemon APIs (nexus-upowerd, started by nexus-upower.service; D-Bus activation as a fallback)
UPOWER_NAMES = ('org.freedesktop.UPower', 'org.freedesktop.UPower.PowerProfiles', 'net.hadess.PowerProfiles')
UPOWER_POLICY = b'''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<!-- nexus-upowerd (NexusOS): UPower and power profiles. Anyone may read; the daemon itself allows profile changes to
     root and the local desktop session's user (power-profiles-daemon's polkit default). -->
<busconfig>
  <policy user="root">
''' + b''.join(b'    <allow own="%s"/>\n' % n.encode() for n in UPOWER_NAMES) + b'''  </policy>
  <policy context="default">
''' + b''.join(b'    <allow send_destination="%s"/>\n    <allow receive_sender="%s"/>\n' % (n.encode(), n.encode()) for n in UPOWER_NAMES) + b'''  </policy>
</busconfig>
'''
LOGIND_ACTIVATION = b'''[D-BUS Service]
Name=org.freedesktop.login1
Exec=/usr/bin/nexusctl start nexus-logind.service
User=root
'''
JOURNALD_CONF = b'''# NexusOS journal (nexusd). Changes apply at the next boot or on SIGHUP to nexusd.
[Journal]
# rotate /var/log/journal/system.journal at this size and keep this many files
SystemMaxFileSize=4M
SystemMaxFiles=8
# copy records up to MaxLevelConsole (0 emerg .. 7 debug) to the console (the boot log)
ForwardToConsole=no
MaxLevelConsole=3
# per source (unit, syslog identifier, kernel): at most RateLimitBurst records per RateLimitIntervalSec, the rest is
# dropped and counted ("Suppressed N messages from SOURCE"); 0 turns the limit off
RateLimitIntervalSec=30s
RateLimitBurst=10000
'''


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


ext4lib = module('sysinstall_ext4lib', ROOT / 'userland/ext4lib.py')


class Image:
    def __init__(self, path, fs=None):
        """path: a raw ext4 disk image; or fs: an open ext4lib.Writer the caller closes (e.g. over a qcow2 overlay)."""
        self.owned = fs is None
        self.fs = ext4lib.Writer(str(path)) if fs is None else fs; self.path = path

    def exists(self, guest):
        try: return self.fs.lookup(guest) is not None
        except (KeyError, ValueError, FileNotFoundError): return False

    def resolve(self, guest):
        """Follow the merged-/usr links of the first component (/bin, /sbin, /lib, /lib64 -> usr/...)."""
        parts = [p for p in guest.split('/') if p]
        if parts and parts[0] in ('bin', 'sbin', 'lib', 'lib64'):
            try:
                ino = self.fs.lookup('/' + parts[0])
                if ino and (self.fs.read_inode(ino).mode & 0o170000) == 0o120000: parts = ['usr'] + parts
            except (KeyError, ValueError, FileNotFoundError): pass
        return '/' + '/'.join(parts)

    def put(self, guest, data, mode, uid=0, gid=0):
        guest = self.resolve(guest)
        parts = self.fs.split(guest); self.fs.ensure_dir(parts[:-1], 0o755, 0, 0)
        if self.exists(guest): self.fs.remove(guest)
        self.fs.put_bytes(data, guest, mode, uid, gid)

    def mkdir(self, guest, mode=0o755):
        self.fs.ensure_dir(self.fs.split(self.resolve(guest)), mode, 0, 0)

    def close(self):
        if not self.owned: return
        self.fs.close(backups=True)
        errors, _, checker = ext4lib.check_image(str(self.path)); checker.fs.f.close()   # else open until a GC (Windows: no rename)
        if errors: raise SystemExit('ext4 check failed: ' + '; '.join(errors[:10]))


def install_services(disk, aurora=False, test_units=(), tests=False, fs=None):
    for need in ('nexusd.elf', 'nexusctl.elf', 'nexus_notifyd.elf', 'nexus_software.elf'):
        if not (OUT / need).exists(): raise SystemExit('missing out/%s: run arch/x86_64/build.py build' % need)
    img = Image(disk, fs)
    img.put('/usr/lib/nexus/nexusd', (OUT / 'nexusd.elf').read_bytes(), 0o4755)
    img.put('/usr/bin/nexusctl', (OUT / 'nexusctl.elf').read_bytes(), 0o755)
    for unit in sorted(p for kind in ('service', 'socket', 'timer', 'path') for p in (HERE / 'services').glob('*.' + kind)):
        img.put('/usr/lib/nexus/services/' + unit.name, unit.read_bytes().replace(b'\r\n', b'\n'), 0o644)
    # per-user managers (user@UID.service): vendor and admin user units; timer stamps (Persistent=)
    img.mkdir('/usr/lib/nexus/user'); img.mkdir('/etc/nexus/user'); img.mkdir('/var/lib/nexus/timers')
    img.put('/usr/sbin/policy-rc.d', (HERE / 'services' / 'policy-rc.d').read_bytes().replace(b'\r\n', b'\n'), 0o755)
    for script, guest in (('timedatectl', '/usr/bin/timedatectl'), ('setup-printers', '/usr/lib/nexus/setup-printers'),
                          ('nexus-update', '/usr/sbin/nexus-update')):
        img.put(guest, (HERE / 'services' / script).read_bytes().replace(b'\r\n', b'\n'), 0o755)
    # Software app: apt front end (reads as the user, changes through sudo + the helper)
    img.put('/usr/bin/nexus-software', (OUT / 'nexus_software.elf').read_bytes(), 0o755)
    img.put('/usr/lib/nexus/nexus-pkg-helper', (HERE / 'services' / 'nexus-pkg-helper').read_bytes().replace(b'\r\n', b'\n'), 0o755)
    img.put('/nexus/apps/software.args', b'/usr/bin/nexus-software\n', 0o644)
    # desktop notifications: org.freedesktop.Notifications, started on demand by the session bus (D-Bus activation)
    img.put('/usr/lib/nexus/nexus-notifyd', (OUT / 'nexus_notifyd.elf').read_bytes(), 0o755)
    img.put('/usr/share/dbus-1/services/org.freedesktop.Notifications.service', NOTIFY_ACTIVATION, 0o644)
    # UPower / power profiles on the system bus (unit services/nexus-upower.service starts it with the bus)
    if (OUT / 'nexus_upowerd.elf').exists():
        img.put('/usr/libexec/nexus-upowerd', (OUT / 'nexus_upowerd.elf').read_bytes(), 0o755)
        img.put('/usr/share/dbus-1/system.d/nexus-upower.conf', UPOWER_POLICY, 0o644)
        for name in UPOWER_NAMES:
            img.put('/usr/share/dbus-1/system-services/%s.service' % name, b'[D-BUS Service]\nName=%s\nExec=/usr/libexec/nexus-upowerd\nUser=root\n' % name.encode(), 0o644)
    # org.freedesktop.login1 (nexus_logind.c): sessions/seat from Aurora, inhibitors, power; its bus policy and activation
    if (OUT / 'nexus_logind.elf').exists():
        img.put('/usr/lib/nexus/nexus-logind', (OUT / 'nexus_logind.elf').read_bytes(), 0o755)
        img.put('/usr/share/dbus-1/system.d/org.freedesktop.login1.conf', (HERE / 'services' / 'org.freedesktop.login1.conf').read_bytes().replace(b'\r\n', b'\n'), 0o644)
        img.put('/usr/share/dbus-1/system-services/org.freedesktop.login1.service', LOGIND_ACTIVATION, 0o644)
    # the desktop portal's Aurora backend (nexus_portal.c; the frontend is Debian's xdg-desktop-portal: apt install
    # xdg-desktop-portal) and Aurora's applications as MIME handlers (OpenURI, xdg-open)
    if (OUT / 'nexus_portal.elf').exists():
        img.put('/usr/libexec/xdg-desktop-portal-aurora', (OUT / 'nexus_portal.elf').read_bytes(), 0o755)
        portal = HERE / 'portal'
        img.put('/usr/share/xdg-desktop-portal/portals/aurora.portal', (portal / 'aurora.portal').read_bytes().replace(b'\r\n', b'\n'), 0o644)
        img.put('/usr/share/xdg-desktop-portal/nexusos-portals.conf', (portal / 'nexusos-portals.conf').read_bytes().replace(b'\r\n', b'\n'), 0o644)
        img.put('/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.aurora.service', (portal / 'org.freedesktop.impl.portal.desktop.aurora.service').read_bytes().replace(b'\r\n', b'\n'), 0o644)
        for entry in sorted((portal / 'applications').glob('*.desktop')):
            img.put('/usr/share/applications/' + entry.name, entry.read_bytes().replace(b'\r\n', b'\n'), 0o644)
        img.put('/usr/share/applications/mimeapps.list', (portal / 'mimeapps.list').read_bytes().replace(b'\r\n', b'\n'), 0o644)   # their types
    # the screen reader (aurora_reader.h): the Aurora shell as an AT-SPI2 application for Orca (apt install orca), and
    # Orca's start without an X display
    if (OUT / 'aurora_a11y_bridge.elf').exists():
        img.put('/usr/libexec/aurora-a11y-bridge', (OUT / 'aurora_a11y_bridge.elf').read_bytes(), 0o755)
        img.put('/usr/lib/nexus/orca-wayland', (HERE / 'a11y' / 'orca-wayland').read_bytes().replace(b'\r\n', b'\n'), 0o755)
    if tests:
        img.put('/usr/lib/nexus/tests/test-hook', (HERE / 'services' / 'test-hook').read_bytes().replace(b'\r\n', b'\n'), 0o755)
        img.put('/etc/nexus/services/test-hook.service', TEST_HOOK_UNIT, 0o644)
        img.put('/usr/lib/nexus/tests/svctest-wait', TEST_WAIT, 0o755)
        img.put('/usr/lib/nexus/tests/nexus-notify-test', (OUT / 'notify_test_user.elf').read_bytes(), 0o755)
        img.put('/usr/lib/nexus/tests/nexus-svc-test', (OUT / 'svc_test_user.elf').read_bytes(), 0o755)
        if (OUT / 'logind_test_user.elf').exists(): img.put('/usr/lib/nexus/tests/logind-test', (OUT / 'logind_test_user.elf').read_bytes(), 0o755)
        if (OUT / 'portal_test_user.elf').exists(): img.put('/usr/lib/nexus/tests/portal-test', (OUT / 'portal_test_user.elf').read_bytes(), 0o755)
        img.put('/usr/lib/nexus/tests/atspi-watch', (HERE / 'a11y' / 'atspi-watch').read_bytes().replace(b'\r\n', b'\n'), 0o755)   # tests/test_aurora_orca.py
    if not img.exists('/etc/nexus/journald.conf'): img.put('/etc/nexus/journald.conf', JOURNALD_CONF, 0o644)
    # core updates: the release signing key's public half (arch/x86_64/keys/, provided by the release process)
    if (HERE / 'keys' / 'nexusos-update.pgp').is_file():
        img.put('/usr/share/keyrings/nexusos-update.pgp', (HERE / 'keys' / 'nexusos-update.pgp').read_bytes(), 0o644)
    if not img.exists('/etc/nexus/update.conf'): img.put('/etc/nexus/update.conf', UPDATE_CONF, 0o644)
    # Secure Boot: the kernel signing key's certificate (nexus-update checks a bundle's kernel.sig before installing it)
    img.put('/usr/share/nexus/secureboot/kernel.crt', (HERE / 'uefi' / 'keys' / 'kernel.crt').read_bytes().replace(b'\r\n', b'\n'), 0o644)
    img.mkdir('/etc/nexus/services'); img.mkdir('/etc/nexus/enabled'); img.mkdir('/etc/nexus/disabled')
    # Debian enables these through deb-systemd-helper's wants links at first boot; they cannot work here yet
    for unit, why in OFF_BY_DEFAULT.items():
        if not img.exists('/etc/nexus/enabled/' + unit): img.put('/etc/nexus/disabled/' + unit, why, 0o644)
    for unit, marker in LEGACY_OFF.items():
        path = '/etc/nexus/disabled/' + unit
        if img.exists(path):
            inode = img.fs.read_inode(img.fs.lookup(path))
            if (inode.mode & 0o170000) == 0o100000 and img.fs.read_file(inode) == marker: img.fs.remove(path)
    if aurora:
        if not (OUT / 'desktop.elf').exists(): raise SystemExit('missing out/desktop.elf')
        img.put('/sbin/init', (OUT / 'desktop.elf').read_bytes(), 0o755)
        # Aurora's applications (System Settings, Notes, ...) of this desktop, and the account helper (greeter and lock
        # screen password checks, Settings > Users), set-user-ID root as nexusd
        for need in ('aurora_app.elf', 'nexus-auth.elf', 'nexus-session.elf', 'pkexec.elf', 'nexus-polkitd.elf'):
            if not (OUT / need).exists(): raise SystemExit('missing out/%s: run arch/x86_64/desktop.py and aurora.py' % need)
        img.put('/bin/aurora-app', (OUT / 'aurora_app.elf').read_bytes(), 0o755)
        img.put('/usr/libexec/nexus-auth', (OUT / 'nexus-auth.elf').read_bytes(), 0o4755)
        img.put('/usr/libexec/nexus-session', (OUT / 'nexus-session.elf').read_bytes(), 0o4755)   # other accounts' sessions (aurora_session.h)
        # the authorization broker (nexus_polkitd.c, unit services/nexus-polkitd.service) and pkexec (pkexec_user.c)
        img.put('/usr/libexec/nexus-polkitd', (OUT / 'nexus-polkitd.elf').read_bytes(), 0o755)
        img.put('/usr/bin/pkexec', (OUT / 'pkexec.elf').read_bytes(), 0o4755)
    for spec in test_units:
        name, _, source = spec.partition('=')
        img.put('/etc/nexus/services/' + name, Path(source).read_bytes().replace(b'\r\n', b'\n'), 0o644)
    img.close()
    print('[sysinstall] services installed into', disk or 'the open volume', flush=True)


def compare_trees(fat_image, ext4_image, home='/home/nexus', uid=1000, skips=(), progress=None):
    """Every FAT entry against its ext4 copy (names matched without case, as the FAT32 driver does): regular files by
    size + SHA-256, symbolic links by target, folders by their entries. Returns the report dict."""
    import hashlib
    f2e = module('sysinstall_fat2ext4', ROOT / 'userland/fat2ext4.py')
    ext4lib = f2e.ext4lib
    fat = f2e.fatput.Fat32(str(fat_image), readonly=True)
    fs = ext4lib.Ext4(str(ext4_image))
    conv = f2e.Converter(fat, None, uid, uid, home, [], False, None)
    report = {'files': 0, 'bytes': 0, 'dirs': 0, 'symlinks': 0, 'sockets': 0, 'differences': [], 'missing': [], 'extra': []}
    skipped = {s.rstrip('/').lower() for s in skips}   # never read (the converter left them out too)

    def ext4_digest(inode):
        digest, size, done = hashlib.sha256(), inode.size, 0
        for lblk, pblk, ln, uninit in fs.file_blocks(inode):
            start = lblk * ext4lib.BLOCK
            if start >= size: continue
            if start > done: digest.update(bytes(start - done)); done = start
            n = min(ln * ext4lib.BLOCK, size - start)
            for off in range(0, n, 8 << 20):
                part = min(8 << 20, n - off)
                digest.update(bytes(part) if uninit else fs.read_run(pblk + off // ext4lib.BLOCK, (part + ext4lib.BLOCK - 1) // ext4lib.BLOCK)[:part])
            done = start + n
        if done < size: digest.update(bytes(size - done))
        return digest.hexdigest()

    def walk(dirc, ino, path):
        children = {}
        for name, i, ft in fs.dir_entries(ino):
            if name not in (b'.', b'..', b'lost+found'): children[name.decode('utf-8', 'surrogateescape').lower()] = (name, i, ft)
        seen = set()
        for name, is_dir, first, size, kind, _ in conv.entries(dirc, path):
            child = path.rstrip('/') + '/' + name
            if child.lower() in skipped: seen.add(name.lower()); continue
            match = children.get(name.lower())
            if not match: report['missing'].append(child); continue
            seen.add(name.lower()); inode = fs.read_inode(match[1]); fmt = inode.mode & 0o170000
            if is_dir:
                if fmt != 0o040000: report['differences'].append('%s: not a folder' % child); continue
                report['dirs'] += 1; walk(first if first else fat.rootc, match[1], child)
            elif kind == 2:
                target = conv.read_small(first, size)
                if fmt != 0o120000 or fs.read_file(inode) != target: report['differences'].append('%s: symbolic link target' % child)
                report['symlinks'] += 1
            elif kind == 1:
                if fmt != 0o140000: report['differences'].append('%s: not a socket' % child)
                report['sockets'] += 1
            else:
                digest = hashlib.sha256()
                for piece in conv.stream(first, size): digest.update(piece)
                if fmt != 0o100000 or inode.size != size or ext4_digest(inode) != digest.hexdigest():
                    report['differences'].append('%s: content (%d bytes on FAT, %d on ext4)' % (child, size, inode.size))
                report['files'] += 1; report['bytes'] += size
                if progress: progress(report['files'], report['bytes'])
        for key, (name, _, _) in children.items():
            if key not in seen: report['extra'].append(path.rstrip('/') + '/' + name.decode('utf-8', 'replace'))
    try:
        walk(fat.rootc, ext4lib.ROOT_INO, '/')
    finally:
        fat.close(); fs.close()
    return report


def new_system(target, source, size_mib, config, cache, migrate=False, groups=None, skips=(), progress=None):
    """progress(phase, files, bytes): 'copy' and 'verify' after every file, 'base' and 'services' once."""
    """install / migrate: FAT32 desktop disk -> ext4 root with the Debian base staged and the services installed."""
    import json
    import time
    target, source = Path(target).resolve(), Path(source).resolve()
    if target.exists(): raise SystemExit('the new disk exists already: %s' % target)
    if target == source: raise SystemExit('the new disk must be another file than the old one')
    t0 = time.time()
    f2e = module('sysinstall_fat2ext4', ROOT / 'userland/fat2ext4.py')
    skips = list(skips) if migrate else list(f2e.DEFAULT_SKIP) + list(skips)
    stats = f2e.convert(str(source), str(target), size_mib, skips=skips, progress=(lambda f, b: progress('copy', f, b)) if progress else None)
    report = {'source': str(source), 'target': str(target), 'mode': 'migrate' if migrate else 'install',
              'converted': {k: v for k, v in stats.items() if k != 'home_lowercased'}, 'home_lowercased': stats.get('home_lowercased', [])}
    if migrate:
        check = compare_trees(source, target, skips=skips, progress=(lambda f, b: progress('verify', f, b)) if progress else None)
        report['comparison'] = {k: v if not isinstance(v, list) else v[:500] for k, v in check.items()}
        print('[sysinstall] compared %d files (%d MiB), %d folders, %d links: %d differences, %d missing' % (
            check['files'], check['bytes'] >> 20, check['dirs'], check['symlinks'], len(check['differences']), len(check['missing'])), flush=True)
        if check['differences'] or check['missing']:
            Path(str(target) + '.migration.json').write_text(json.dumps(report, indent=1))
            raise SystemExit('migration check failed (see %s.migration.json); the old disk is unchanged' % target)
    base = module('sysinstall_debian_base', HERE / 'debian_base.py')
    if progress: progress('base', 0, 0)
    staged = base.stage(target, groups or base.DEFAULT_GROUPS, cache or base.DEFAULT_CACHE, None, config)
    report['staged'] = {k: v for k, v in staged.items() if isinstance(v, (int, str))} if isinstance(staged, dict) else None
    if progress: progress('services', 0, 0)
    install_services(target, aurora=True)
    report['seconds'] = round(time.time() - t0, 1)
    Path(str(target) + ('.migration.json' if migrate else '.install.json')).write_text(json.dumps(report, indent=1))
    print('[sysinstall] %s ready: %s (first boot installs the Debian base with dpkg)' % (report['mode'], target), flush=True)
    return report


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='action', required=True)
    s = sub.add_parser('services'); s.add_argument('disk', type=Path); s.add_argument('--aurora', action='store_true')
    s.add_argument('--test-unit', action='append', default=[]); s.add_argument('--tests', action='store_true', help='also the test clients')
    for name in ('install', 'migrate'):
        p = sub.add_parser(name)
        if name == 'migrate': p.add_argument('old', type=Path)
        p.add_argument('new', type=Path)
        if name == 'install': p.add_argument('--source', type=Path, default=None, help='FAT32 desktop disk (default NEXUS_TEST_SOURCE, arch/x86_64/nexus_paths.py)')
        p.add_argument('--size', type=int, default=8192, help='MiB (default 8192)')
        p.add_argument('--cache', type=Path, help='debian_base.py cache (verified .debs + signed indices)')
        p.add_argument('--config', action='append', default=[], help='installer setting KEY=VALUE (debian_base.py stage)')
        p.add_argument('--skip', action='append', default=[], help='a path left out (never read), e.g. a profile folder')
    a = ap.parse_args(argv)
    if a.action == 'services': install_services(a.disk, a.aurora, a.test_unit, a.tests)
    elif a.action in ('install', 'migrate'):
        config = dict(item.split('=', 1) for item in a.config)
        new_system(a.new, a.old if a.action == 'migrate' else (a.source or nexus_paths.source_disk()), a.size, config, a.cache, a.action == 'migrate', skips=a.skip)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
