#!/usr/bin/env python3
"""Install Debian's BlueZ onto a NexusOS disk image (FAT root, e.g. a copy of out/apps64.img).

    python -I arch/x86_64/bluez_install.py <disk.img> [--groups bluez,bluez-test] [--grow MIB] [--cache DIR] [--report PATH]
    python -I arch/x86_64/bluez_install.py lock [--allow-older]   # re-resolve bluez-lock.json from signed indices

Groups (Debian 13 trixie + updates/security, dependency closure each; the disk's glibc runtime stands for libc6):
  bluez       bluez 5.82 (bluetoothd, bluetoothctl, btmon, btmgmt, l2test, rctest, l2ping, hcitool...) and the D-Bus
              system bus (dbus-daemon, dbus-send) bluetoothd needs.
  bluez-test  bluez-test-tools (btvirt: BlueZ's controller emulator on /dev/vhci; the kernel conformance testers
              l2cap-tester, smp-tester, rfcomm-tester, mgmt-tester...). Only tests/test_x64_bluez.py installs it.
They talk to the kernel's Bluetooth stack (arch/x86_64/bluetooth.c + bt_*.c): mgmt, HCI, L2CAP, SMP, RFCOMM sockets.

The pipeline is arch/x86_64/nettools_install.py's (signed InRelease -> Packages.xz -> closure -> every .deb pinned by
URL + size + SHA-256 + snapshot.debian.org fallback in bluez-lock.json; installing only uses the lock, verifies every
.deb, runs no maintainer script) with its own lock, roots, skip list and manifest (/nexus/bluez-installed.json).
Licenses: BlueZ GPL-2.0-or-later and LGPL-2.1-or-later (libbluetooth), D-Bus AFL-2.1 OR GPL-2.0-or-later, GLib
LGPL-2.1-or-later, readline GPL-3, alsa-lib LGPL-2.1, elfutils libdw LGPL-3/GPL-2 - all user space; nothing enters the
kernel (the kernel stack is NexusOS' own code).
"""
from pathlib import Path, PurePosixPath
import argparse
import array
import importlib.util
import json
import os
import sys
import time

HERE = Path(__file__).resolve().parent
LOCK = HERE / 'bluez-lock.json'
DEFAULT_CACHE = Path(os.environ.get('NEXUS_BLUEZ_PKGS', r'D:\nexustest\wave2\ws_hw2_pkgs'))
VERSION = 1
NAME = 'bluez'

_spec = importlib.util.spec_from_file_location('bluez_wine_install', HERE / 'wine_install.py')
wi = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(wi)
ai, InstallError, fold = wi.ai, wi.InstallError, wi.fold

GROUPS = {'bluez': {'trixie': ['bluez', 'dbus-daemon', 'dbus-bin']},
          'bluez-test': {'trixie': ['bluez-test-tools']}}
PRESENT = {'libc6': '2.41-12+deb13u4'}   # the glibc runtime on every NexusOS disk (arch/x86_64/runtime.py)
SKIP = {
    'debconf': 'maintainer-script configuration only',
    'debconf-2.0': 'virtual package of debconf (maintainer-script configuration only)',
    'adduser': 'maintainer scripts create the messagebus user; NexusOS runs the bus with its own configuration',
    'systemd-sysusers': 'maintainer scripts create the messagebus user (alternative of adduser)',
    'init-system-helpers': 'service management helpers (no systemd on NexusOS)',
    'kmod': 'modprobe of hidp/bnep/uhid for the input and network plugins (NexusOS has no loadable modules)',
    'udev': 'udev rules for hid2hci (no udevd on NexusOS)',
    'default-dbus-system-bus': 'virtual package: the bus comes from dbus-daemon (a root of this lock)',
    'dbus-system-bus': 'virtual package: the bus comes from dbus-daemon (a root of this lock)',
    'libcap2-bin': 'setcap/getcap for maintainer scripts',
    'perl-base': 'maintainer scripts',
    'lsb-base': 'init-script helpers (no SysV init scripts run on NexusOS)',
    'sysvinit-utils': 'init-script helpers (no SysV init scripts run on NexusOS)',
}
ENTRY_POINTS = {'bluez': ['/usr/libexec/bluetooth/bluetoothd', '/usr/bin/bluetoothctl', '/usr/bin/btmon', '/usr/bin/btmgmt',
                          '/usr/bin/l2test', '/usr/bin/rctest', '/usr/bin/dbus-daemon', '/usr/bin/dbus-send'],
                'bluez-test': ['/usr/bin/btvirt', '/usr/bin/l2cap-tester', '/usr/bin/smp-tester', '/usr/bin/rfcomm-tester',
                               '/usr/bin/mgmt-tester']}


def log(*args): print('[%s]' % NAME, *args, flush=True)


def make_lock(allow_older=False):
    """Signed indices -> dependency closure -> bluez-lock.json (wine_install.py's verified pipeline, trixie only)."""
    previous = json.loads(LOCK.read_bytes()) if LOCK.exists() else {}
    keyring = previous.get('keyring') or json.loads((HERE / 'wine-lock.json').read_bytes())['keyring']
    wi.MIRRORS = {s: wi.MIRRORS[s] for s in wi.TRIXIE}
    packages, meta, keyring = wi.verified_indices(keyring, previous.get('suites'), allow_older, False)
    packages['sid'] = {}
    wi.GROUPS, wi.SKIP = GROUPS, SKIP
    selected, skipped, satisfied = wi.resolve(packages, dict(PRESENT))
    for p in selected.values():
        p['snapshot'] = wi.snapshot_lookup(p['name'], p['version'], PurePosixPath(p['filename']).name, p['bytes'])
        if not p['snapshot']: p['snapshot'] = wi.snapshot_url(p['suite'], meta[p['suite']]['date'], p['url'][len(wi.MIRRORS[p['suite']]):])
    lock = {'format': VERSION, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'suites': meta, 'keyring': keyring,
            'present': dict(sorted(satisfied.items())), 'skipped': dict(sorted(skipped.items())),
            'packages': sorted(selected.values(), key=lambda p: p['name'])}
    LOCK.write_bytes((json.dumps(lock, indent=1) + '\n').encode())
    log('wrote', LOCK, len(selected), 'packages,', len(skipped), 'skipped')
    for p in lock['packages']: log('  %-30s %-30s %-16s %8d %s' % (p['name'], p['version'], p['suite'], p['bytes'], ','.join(p['needed_by'])))


class BluezInstaller(ai.Installer):
    def existing(self, path, kind, art, bucket, mtime):
        if path == 'usr/include' or path.startswith('usr/include/'):
            self.skip('development headers', '/' + path, bucket); return None
        return super().existing(path, kind, art, bucket, mtime)

    def finish(self, versions):
        now = time.time()
        for (top, n), app in sorted(self.usr_names.items()):           # merged-usr emulation
            target = '/usr/%s/%s' % (top, n)
            d = self.walk(top, False)
            ent = d.names.get(fold(n)) if d else None
            if ent is not None and not (ent.is_link and ent.name == n and self.read_link(ent) == target):
                self.skip('merged-usr link: existing entry kept', '/%s/%s' % (top, n)); continue
            self.managed_link('%s/%s' % (top, n), target, app, now)
        for pkg in self.by_name:
            if pkg not in self.licenses: self.licenses[pkg] = 'none'
        manifest = {'format': VERSION, 'installer': 'arch/x86_64/bluez_install.py', 'lock_sha256': self.lock_sha256,
                    'groups': sorted(self.groups_installed),
                    'packages': [{'name': a['name'], 'version': a['version'], 'suite': a['suite']} for a in self.artifacts],
                    'symlinks_created': sorted(set(self.managed_links)), 'maintainer_scripts_not_run': sorted(self.by_name)}
        self.manifest = manifest
        data = (json.dumps(manifest, indent=1) + '\n').encode()
        d = self.walk('nexus', True, now, NAME); mname = NAME + '-installed.json'
        ent = d.names.get(fold(mname))
        if ent is None: self.vol.write_file(d, mname, len(data), [data], now); self.changes += 1
        elif not (ent.first >= 2 and b''.join(self.vol.read_file(ent.first, ent.size)) == data):
            self.conflict('nexus/' + mname, 'existing manifest differs (kept)', {'name': NAME + '_install'}, NAME)


def install(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('disk'); ap.add_argument('--grow', type=int, metavar='MIB')
    ap.add_argument('--groups', default='bluez'); ap.add_argument('--cache', default=str(DEFAULT_CACHE)); ap.add_argument('--report', default=None)
    args = ap.parse_args(argv)
    t0 = time.time(); disk = Path(args.disk).resolve()
    if not disk.is_file(): raise SystemExit('no such disk image: %s' % disk)
    groups = set(args.groups.split(','))
    if not groups or groups - set(GROUPS): raise SystemExit('--groups: choose from ' + ','.join(GROUPS))
    cache = Path(args.cache).resolve(); cache.mkdir(parents=True, exist_ok=True)
    report_path = Path(args.report).resolve() if args.report else cache / (NAME + '-install-report.json')
    lock_bytes = LOCK.read_bytes(); lock = json.loads(lock_bytes)
    artifacts = [dict(p, app=NAME) for p in lock['packages'] if groups & set(p['needed_by'])]
    paths = {a['name']: wi.fetch_package(a, cache) for a in artifacts}
    log('%d packages verified (%.1f MiB) in %.1fs' % (len(artifacts), sum(a['bytes'] for a in artifacts) / 2**20, time.time() - t0))
    entry_points = [p for g in sorted(groups) for p in ENTRY_POINTS[g]]
    report = {'disk': str(disk), 'lock_sha256': wi.sha256(lock_bytes), 'groups': sorted(groups)}
    st0 = disk.stat(); head0 = ai.prefix_digest(disk, 0, ai.BASE)
    vol = ai.Volume(disk, writable=False)
    try:
        ana = BluezInstaller(vol, artifacts, paths, {NAME}); ana.lock_sha256 = report['lock_sha256']; ana.groups_installed = groups
        for art in artifacts: ana.install_artifact(art)
        ana.finish({})
        needed, free_clusters, cl, spc, ncl = vol.allocated, vol.free_start, vol.cl, vol.spc, vol.nclusters
    finally:
        vol.close()
    if ana.fatal:
        report.update(result='aborted', fatal=ana.fatal, **ai.details(ana)); ai.write_report(report_path, report)
        raise SystemExit('installation aborted before writing: %r' % ana.fatal[:10])
    grow_clusters, do_grow = 0, False
    if args.grow:
        do_grow = args.grow * 1024 * 1024 > st0.st_size
        if do_grow: grow_clusters = ai.format_geometry((args.grow * 1024 * 1024 - ai.BASE) // ai.SECTOR, spc)[3] - ncl
    if needed > free_clusters + grow_clusters:
        raise SystemExit('not enough space: need %.1f MiB, have %.1f MiB (use --grow MIB)' % (needed * cl / 2**20, (free_clusters + grow_clusters) * cl / 2**20))
    if ana.changes == 0 and not do_grow: log('nothing to do; image untouched'); return 0
    temp = disk.with_name('.%s-install-%d.img' % (NAME, os.getpid()))
    try:
        if do_grow: ai.grow(disk, temp, args.grow)
        else: ai.sparse_copy(disk, temp)
        vol = ai.Volume(temp, writable=True)
        try:
            inst = BluezInstaller(vol, artifacts, paths, {NAME}); inst.lock_sha256 = report['lock_sha256']; inst.groups_installed = groups
            for art in artifacts: inst.install_artifact(art)
            inst.finish({})
            if inst.fatal: raise InstallError('fatal problems while writing: %r' % inst.fatal[:5])
            vol.flush()
            entries = array.array('I'); entries.frombytes(bytes(vol.fat[:(vol.nclusters + 2) * 4]))
            if sum(1 for x in entries[2:] if not x & 0x0FFFFFFF) != vol.free: raise InstallError('FAT free count mismatch after install')
            links = ai.check_links(inst, entry_points)
            broken = [p for p, state in links.items() if state == 'missing or dangling']
            if broken: raise InstallError('entry points do not resolve after the install: ' + ', '.join(broken))
        finally:
            vol.close()
        skip = (446 + 12, 446 + 16) if do_grow else None
        if ai.prefix_digest(temp, 0, ai.BASE, skip) != ai.prefix_digest(disk, 0, ai.BASE, skip): raise InstallError('reserved prefix changed')
        st1 = disk.stat()
        if (st1.st_size, st1.st_mtime_ns) != (st0.st_size, st0.st_mtime_ns) or ai.prefix_digest(disk, 0, ai.BASE) != head0:
            raise InstallError('disk changed during installation; stop QEMU and retry')
        os.replace(temp, disk)
    finally:
        if temp.exists(): temp.unlink()
    report.update(result='installed', changes=inst.changes, seconds=round(time.time() - t0, 1), link_checks=links, **ai.details(inst))
    ai.write_report(report_path, report)
    log('installed %d packages, %d changes, %d conflicts in %.1fs' % (len(artifacts), inst.changes, len(inst.conflicts), time.time() - t0))
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['lock']:
        lp = argparse.ArgumentParser(prog='bluez_install.py lock'); lp.add_argument('--allow-older', action='store_true')
        make_lock(lp.parse_args(argv[1:]).allow_older); return 0
    return install(argv)


if __name__ == '__main__':
    try: sys.exit(main())
    except InstallError as e: raise SystemExit('bluez_install: ' + str(e))
