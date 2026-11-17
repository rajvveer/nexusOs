#!/usr/bin/env python3
"""Install Debian's network tools onto a NexusOS disk image (FAT root, e.g. a copy of out/apps64.img).

    python -I arch/x86_64/nettools_install.py <disk.img> [--grow MIB] [--cache DIR] [--report PATH]
    python -I arch/x86_64/nettools_install.py lock [--allow-older]   # re-resolve nettools-lock.json from signed indices

Packages (Debian 13 trixie, with trixie-updates and trixie-security): iproute2 (ip, ss, bridge, tc), iputils-ping (ping),
tcpdump (+ libpcap), nftables (nft), wireguard-tools (wg, wg-quick), net-tools (ifconfig, route, netstat) and netbase
(/etc/protocols, /etc/services) with their dependency closure; the disk's glibc runtime stands for libc6.

Verification and installation reuse arch/x86_64/wine_install.py (and through it apps_install.py's FAT32 engine) unchanged:
`lock` verifies each InRelease with gpgv against the Debian archive keys (ARCHIVE_KEYS, keyring .deb pinned by
wine-lock.json), checks Packages.xz against the signed SHA-256, resolves the closure and pins every .deb (URL, size,
SHA-256, snapshot.debian.org fallback) in nettools-lock.json; installing only uses the lock and verifies every .deb.
No maintainer script runs (the packages here need none for the binaries to work: no users are created, no
capabilities are set, NexusOS grants the desktop owner CAP_NET_RAW/CAP_NET_ADMIN by policy). Merged-usr links
/bin/<n>, /sbin/<n> -> /usr/... are made as wine_install.py does. /nexus/nettools-installed.json records the result.
"""
from pathlib import Path, PurePosixPath
import argparse
import array
import importlib.util
import json
import os
import sys
import time
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
LOCK = HERE / 'nettools-lock.json'
DEFAULT_CACHE = nexus_paths.cache('net')
VERSION = 1
NAME = 'nettools'   # manifest/report/group name (wifi_install.py reuses this module with NAME = 'wifi')

_spec = importlib.util.spec_from_file_location('nettools_wine_install', HERE / 'wine_install.py')
wi = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(wi)
ai, InstallError, fold = wi.ai, wi.InstallError, wi.fold

ROOTS = ['iproute2', 'iputils-ping', 'tcpdump', 'nftables', 'wireguard-tools', 'net-tools', 'netbase']
GROUPS = {'nettools': {'trixie': ROOTS}}
# Present on every NexusOS disk: the glibc runtime (arch/x86_64/runtime.py extracts it from this exact libc6).
PRESENT = {'libc6': '2.41-12+deb13u4'}
SKIP = {
    'debconf': 'maintainer-script configuration only',
    'debconf-2.0': 'virtual package of debconf (maintainer-script configuration only)',
    'adduser': 'tcpdump postinst creates the tcpdump user; tcpdump drops privileges only when started as root',
    'init-system-helpers': 'service management helpers (no systemd on NexusOS)',
    'libcap2-bin': 'setcap/getcap for maintainer scripts; NexusOS grants capabilities by policy',
    'perl-base': 'maintainer scripts',
}
ENTRY_POINTS = ['/usr/bin/ip', '/usr/bin/ss', '/usr/bin/ping', '/usr/bin/tcpdump', '/usr/sbin/nft', '/usr/bin/wg',
                '/usr/sbin/ifconfig', '/etc/protocols', '/etc/services']


def log(*args): print('[%s]' % NAME, *args, flush=True)


def make_lock(allow_older=False):
    """Signed indices -> dependency closure -> nettools-lock.json (wine_install.py's verified pipeline, trixie only)."""
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
    for p in lock['packages']: log('  %-30s %-30s %s %d' % (p['name'], p['version'], p['suite'], p['bytes']))


class NetInstaller(ai.Installer):
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
        manifest = {'format': VERSION, 'installer': 'arch/x86_64/%s_install.py' % NAME, 'lock_sha256': self.lock_sha256,
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
    ap.add_argument('--cache', default=str(DEFAULT_CACHE)); ap.add_argument('--report', default=None)
    args = ap.parse_args(argv)
    t0 = time.time(); disk = Path(args.disk).resolve()
    if not disk.is_file(): raise SystemExit('no such disk image: %s' % disk)
    cache = Path(args.cache).resolve(); cache.mkdir(parents=True, exist_ok=True)
    report_path = Path(args.report).resolve() if args.report else cache / (NAME + '-install-report.json')
    lock_bytes = LOCK.read_bytes(); lock = json.loads(lock_bytes)
    artifacts = [dict(p, app=NAME) for p in lock['packages']]
    paths = {a['name']: wi.fetch_package(a, cache) for a in artifacts}
    log('%d packages verified (%.1f MiB) in %.1fs' % (len(artifacts), sum(a['bytes'] for a in artifacts) / 2**20, time.time() - t0))
    groups = {NAME}; report = {'disk': str(disk), 'lock_sha256': wi.sha256(lock_bytes)}
    st0 = disk.stat(); head0 = ai.prefix_digest(disk, 0, ai.BASE)
    vol = ai.Volume(disk, writable=False)
    try:
        ana = NetInstaller(vol, artifacts, paths, groups); ana.lock_sha256 = report['lock_sha256']
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
            inst = NetInstaller(vol, artifacts, paths, groups); inst.lock_sha256 = report['lock_sha256']
            for art in artifacts: inst.install_artifact(art)
            inst.finish({})
            if inst.fatal: raise InstallError('fatal problems while writing: %r' % inst.fatal[:5])
            vol.flush()
            entries = array.array('I'); entries.frombytes(bytes(vol.fat[:(vol.nclusters + 2) * 4]))
            if sum(1 for x in entries[2:] if not x & 0x0FFFFFFF) != vol.free: raise InstallError('FAT free count mismatch after install')
            links = ai.check_links(inst, ENTRY_POINTS)
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
        lp = argparse.ArgumentParser(prog='nettools_install.py lock'); lp.add_argument('--allow-older', action='store_true')
        make_lock(lp.parse_args(argv[1:]).allow_older); return 0
    return install(argv)


if __name__ == '__main__':
    try: sys.exit(main())
    except InstallError as e: raise SystemExit('nettools_install: ' + str(e))
