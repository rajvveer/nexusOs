#!/usr/bin/env python3
"""Debian 13 (trixie) base system for an ext4 NexusOS root, installed by the real dpkg/apt.

    python -I arch/x86_64/debian_base.py lock [--update-keyring] [--allow-older]   # base-lock.json from signed indices
    python -I arch/x86_64/debian_base.py fetch [--cache DIR] [--groups base,services,desktop]
    python -I arch/x86_64/debian_base.py stage <disk.img> [--cache DIR] [--groups ...]    # debootstrap-style first stage

Groups (base-lock.json):
  base      every Priority: required package of trixie plus the administration set a desktop OS needs (apt + apt-secure
            keys, sudo/PAM, locales, tzdata, CA certificates, procps, nano, ...). No systemd: NexusOS's service manager
            (nexusd) is the init system; daemons are kept from starting inside package scripts by /usr/sbin/policy-rc.d,
            which hands start/stop/restart requests to nexusd instead.
  services  D-Bus, chrony (NTP), CUPS + cups-pdf + IPP tools, avahi (mDNS).
  desktop   the Debian runtime of Google Chrome (the package names of chrome-lock.json, at the lock's trixie versions) and
            google-chrome-stable itself (pinned by chrome-lock.json), so dpkg knows every file of the desktop.

Verification (`lock`): each suite's InRelease (trixie, trixie-updates, trixie-security) must be one clearsigned block with
a good signature by a Debian archive key (wine_install.ARCHIVE_KEYS, gpgv status lines, keyring from the pinned
debian-archive-keyring .deb), name its suite and be within Valid-Until; Packages.xz must match its signed SHA-256. The lock
pins every .deb (URL, snapshot.debian.org URL, size, SHA-256) and the exact InRelease/Packages.xz bytes (by SHA-256, kept
in the cache so the disk can carry them as an offline, still signature-checked, apt source). `fetch`/`stage` only trust
the lock.

`stage` is debootstrap's first stage on the host: the target must be an ext4 NexusOS image (userland/ext4lib.py). It
normalises the root to merged /usr (/bin, /sbin, /lib, /lib64 become symlinks into /usr; files already in those
directories move into /usr), extracts the Priority: required packages (owners, modes, symlinks, hard links), copies every
selected .deb plus the signed indices into /var/lib/nexus/bootstrap, and writes the second stage
(/usr/lib/nexus/bootstrap/stage2), which nexusd runs once as root at first boot: dpkg installs the essential packages in
debootstrap's order, unpacks and configures the required set, then installs the rest through apt from the signed offline
repository (apt-secure checks Debian's signature there too).
"""
from pathlib import Path, PurePosixPath
import argparse
import functools
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
import time
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = HERE / 'base-lock.json'
DEFAULT_CACHE = nexus_paths.file('NEXUS_BASE_CACHE', HERE / 'out' / 'debian-cache')   # verified .debs + signed indices
VERSION = 1


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


wi = module('base_wine_install', HERE / 'wine_install.py')     # signed-index verification, Debian version rules, fetch
ai = wi.ai
InstallError = wi.InstallError

SUITES = ('trixie', 'trixie-updates', 'trixie-security')
MIRRORS = {s: wi.MIRRORS[s] for s in SUITES}
EXCLUDE = {  # never installed: NexusOS has its own init/service manager
    'systemd': 'nexusd is the service manager', 'systemd-sysv': 'nexusd is the init system', 'init': 'nexusd is the init system',
    'libpam-systemd': 'no systemd-logind', 'udev': 'the kernel provides /dev', 'systemd-timesyncd': 'chrony keeps the time',
    'sysvinit-core': 'nexusd is the init system', 'runit-init': 'nexusd is the init system', 'systemd-standalone-sysusers': 'adduser is used',
    'systemd-resolved': 'the kernel serves /etc/resolv.conf from DHCP',
}
# virtual package -> provider (NexusOS starts the per-user session bus itself; no systemd user instance)
PREFER = {'dbus-session-bus': 'dbus-x11', 'default-dbus-session-bus': 'dbus-x11', 'default-dbus-system-bus': 'dbus-daemon',
          'dbus-system-bus': 'dbus-daemon', 'default-logind': None, 'logind': None}
GROUPS = {
    'base': ['@required', 'apt', 'apt-utils', 'debian-archive-keyring', 'gpgv', 'ca-certificates', 'adduser', 'sudo',
             'libpam-modules', 'libpam-runtime', 'locales', 'tzdata', 'procps', 'less', 'nano', 'xz-utils', 'zstd',
             'netbase', 'psmisc', 'file', 'bash-completion', 'cron', 'logrotate', 'ucf', 'readline-common', 'libnss-myhostname'],
    'services': ['dbus', 'dbus-bin', 'dbus-daemon', 'dbus-system-bus-common', 'dbus-session-bus-common', 'chrony',
                 'cups', 'cups-client', 'cups-bsd', 'cups-filters', 'printer-driver-cups-pdf', 'cups-ipp-utils',
                 'avahi-daemon', 'avahi-utils', 'libnss-mdns', 'poppler-utils', 'ghostscript'],
    'desktop': ['@chrome-lock'],
    'sdk': ['libdbus-1-dev'],     # build-time headers only (nexus-notifyd); never installed on a disk
}
DEFAULT_GROUPS = ('base', 'services', 'desktop')
APT_SOURCES = '''# Debian 13 "trixie" (apt-secure: InRelease signatures checked against debian-archive-keyring)
Types: deb
URIs: http://deb.debian.org/debian
Suites: trixie trixie-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: http://deb.debian.org/debian-security
Suites: trixie-security
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
'''
# NexusOS programs at paths a Debian package also uses (xdg-utils' xdg-open): dpkg-diverted in stage 2
DIVERSIONS = ['/usr/bin/xdg-open']
# Debian's essential bootstrap order (debootstrap scripts/debian-common, second_stage_install)
CORE_ORDER = ['base-passwd', 'base-files', 'dpkg', 'libc6', 'perl-base', 'mawk', 'debconf']


def log(*args): print('[base]', *args, flush=True)
def sha256(data): return hashlib.sha256(data).hexdigest()


# ------------------------------------------------------------------------------------------------ lock
def verified_indices(keyring_entry, pinned, allow_older, update_keyring, cache):
    work = Path(tempfile.mkdtemp(prefix='base-lock-'))
    try:
        keyring = wi.keyring_file(keyring_entry, cache)
        packages, meta = {}, {}
        for suite in SUITES:
            raw = wi.fetch_url(MIRRORS[suite] + f'dists/{suite}/InRelease', 1 << 24)
            fields, signers = wi.verify_release(suite, raw, keyring, work, (pinned or {}).get(suite, {}).get('date'), allow_older)
            listing = [line.split() for line in fields.get('SHA256', '').split('\n')]
            listing = [x[:2] for x in listing if len(x) == 3 and x[2] == 'main/binary-amd64/Packages.xz']
            if len(listing) != 1: raise InstallError(suite + ': InRelease does not list main/binary-amd64/Packages.xz exactly once')
            index = wi.fetch_url(MIRRORS[suite] + f'dists/{suite}/main/binary-amd64/Packages.xz', 1 << 27)
            if [sha256(index), str(len(index))] != listing[0]: raise InstallError(suite + ': Packages.xz does not match InRelease')
            store = cache / 'indices'; store.mkdir(parents=True, exist_ok=True)
            (store / (sha256(raw) + '.InRelease')).write_bytes(raw); (store / (sha256(index) + '.Packages.xz')).write_bytes(index)
            meta[suite] = {'inrelease_sha256': sha256(raw), 'inrelease_bytes': len(raw), 'packages_xz_sha256': sha256(index),
                           'packages_xz_bytes': len(index), 'date': fields['Date'], 'signed_by': signers,
                           'inrelease_snapshot': wi.snapshot_url(suite, fields['Date'], f'dists/{suite}/InRelease'),
                           'packages_snapshot': wi.snapshot_url(suite, fields['Date'], f'dists/{suite}/main/binary-amd64/Packages.xz')}
            table = {}
            for stanza in wi.parse_packages(wi.unxz(index).decode('utf-8')):
                if stanza.get('Architecture') in ('amd64', 'all'): table.setdefault(stanza['Package'], []).append(stanza)
            packages[suite] = table
        newest = max(packages['trixie'].get('debian-archive-keyring', []), default=None,
                     key=functools.cmp_to_key(lambda a, b: wi.version_compare(a['Version'], b['Version'])))
        if newest and newest['SHA256'] != keyring_entry['sha256']:
            if update_keyring:
                keyring_entry = {'name': 'debian-archive-keyring', 'version': newest['Version'], 'sha256': newest['SHA256'],
                                 'url': MIRRORS['trixie'] + newest['Filename'], 'bytes': int(newest['Size'])}
                keyring = wi.keyring_file(keyring_entry, cache)
                for suite in SUITES:
                    wi.verify_release(suite, (cache / 'indices' / (meta[suite]['inrelease_sha256'] + '.InRelease')).read_bytes(), keyring, work)
            else:
                log('note: trixie has debian-archive-keyring %s; the lock keeps %s' % (newest['Version'], keyring_entry['version']))
        return packages, meta, keyring_entry
    finally:
        shutil.rmtree(work, ignore_errors=True)


class Resolver:
    """Dependency closure over the newest candidates of the three trixie suites (Pre-Depends + Depends; virtual names
    through Provides; versioned relations checked at the end). A relation is met by the first alternative that is
    already chosen, else the first real (or provided) alternative that is not excluded."""
    def __init__(self, packages):
        self.best, self.provides = {}, {}
        for suite in SUITES:
            for name, stanzas in packages[suite].items():
                for s in stanzas:
                    current = self.best.get(name)
                    if current is None or wi.version_compare(s['Version'], current[1]['Version']) > 0: self.best[name] = (suite, s)
        for name, (suite, s) in self.best.items():
            for group in wi.relations(s.get('Provides')):
                for alt, op, version in group:
                    self.provides.setdefault(alt, []).append((name, version))
        self.selected, self.why = {}, {}

    def candidate(self, name, op, wanted):
        if name in EXCLUDE: return None
        if name in PREFER:
            return PREFER[name] if PREFER[name] and PREFER[name] not in EXCLUDE else None
        if name in self.best:
            if wi.satisfies(self.best[name][1]['Version'], op, wanted): return name
            return None
        for provider, version in sorted(self.provides.get(name, [])):
            if provider in EXCLUDE: continue
            if op is None or (version and wi.satisfies(version, op, wanted)): return provider
        return None

    def met(self, name, op, wanted):
        return self.meeting(name, op, wanted) is not None

    def meeting(self, name, op, wanted):
        """The selected package that meets the relation (itself or a provider), or None."""
        if name in self.selected and wi.satisfies(self.selected[name][1]['Version'], op, wanted): return name
        for provider, version in self.provides.get(name, []):
            if provider in self.selected and (op is None or (version and wi.satisfies(version, op, wanted))): return provider
        return None

    def dependencies(self, name):
        """Names of the selected packages that meet name's Pre-Depends/Depends (the first meeting alternative)."""
        s = self.selected[name][1]; out = set()
        for relation in wi.relations(s.get('Pre-Depends')) + wi.relations(s.get('Depends')):
            hit = next((m for m in (self.meeting(*alt) for alt in relation) if m), None)
            if hit and hit != name: out.add(hit)
        return sorted(out)

    def add(self, roots, group):
        queue = [(name, 'root:' + group) for name in roots]
        while queue:
            name, parent = queue.pop(0)
            if name in self.selected:
                self.why.setdefault(name, set()).add(group); continue
            if name in EXCLUDE: raise InstallError('%s is excluded (%s) but needed by %s' % (name, EXCLUDE[name], parent))
            if name not in self.best: raise InstallError('%s (needed by %s) is not in trixie' % (name, parent))
            self.selected[name] = self.best[name]; self.why.setdefault(name, set()).add(group)
            stanza = self.best[name][1]
            for relation in wi.relations(stanza.get('Pre-Depends')) + wi.relations(stanza.get('Depends')):
                if any(self.met(alt, op, wanted) for alt, op, wanted in relation): continue
                pending = [alt for alt, _, _ in relation if alt in {n for n, _ in queue}]
                if pending: continue
                choice = next((c for c in (self.candidate(*alt) for alt in relation) if c), None)
                if not choice:
                    raise InstallError('%s: no installable alternative for %s' % (name, ' | '.join(
                        a + (' (%s %s)' % (o, w) if o else '') for a, o, w in relation)))
                queue.append((choice, name))

    def check(self):
        for name, (suite, s) in self.selected.items():
            for relation in wi.relations(s.get('Pre-Depends')) + wi.relations(s.get('Depends')):
                if not any(self.met(alt, op, wanted) for alt, op, wanted in relation):
                    raise InstallError('%s %s: %s is not met' % (name, s['Version'], ' | '.join(
                        a + (' (%s %s)' % (o, w) if o else '') for a, o, w in relation)))
            for relation in wi.relations(s.get('Conflicts')) + wi.relations(s.get('Breaks')):
                for alt, op, wanted in relation:
                    if alt != name and alt in self.selected and wi.satisfies(self.selected[alt][1]['Version'], op, wanted):
                        raise InstallError('%s conflicts with/breaks %s' % (name, alt))


def make_lock(update_keyring, allow_older, cache):
    previous = json.loads(LOCK.read_bytes()) if LOCK.exists() else {}
    keyring = previous.get('keyring') or json.loads((HERE / 'wine-lock.json').read_bytes())['keyring']
    packages, meta, keyring = verified_indices(keyring, previous.get('suites'), allow_older, update_keyring, cache)
    resolver = Resolver(packages)
    chrome = json.loads((HERE / 'chrome-lock.json').read_text())
    google = [p for p in chrome if p['name'] == 'google-chrome-stable']
    google_controls = []
    for g in google:   # google-chrome-stable's own Depends (it is not in Debian's index)
        entry = {'suite': 'google', 'filename': 'debs/' + PurePosixPath(g['url']).name, 'url': g['url'], 'bytes': g['bytes'], 'sha256': g['sha256'], 'version': g['version']}
        google_controls.append(control_fields(fetch_one(entry, cache)))
    for group, roots in GROUPS.items():
        names = []
        for root in roots:
            if root == '@required':
                names += sorted(n for n, (suite, s) in resolver.best.items() if s.get('Priority') == 'required' and n not in EXCLUDE)
            elif root == '@chrome-lock':
                names += [p['name'] for p in chrome if p['name'] != 'google-chrome-stable']
                for control in google_controls:
                    for relation in wi.relations(control.get('Pre-Depends')) + wi.relations(control.get('Depends')):
                        if not any(resolver.met(*alt) or alt[0] in names for alt in relation):
                            choice = next((c for c in (resolver.candidate(*alt) for alt in relation) if c), None)
                            if not choice: raise InstallError('google-chrome-stable: cannot meet %r' % (relation,))
                            names.append(choice)
            else: names.append(root)
        resolver.add(names, group)
    resolver.check()
    for control in google_controls:   # every relation of the Google package must be met by the lock
        for relation in wi.relations(control.get('Pre-Depends')) + wi.relations(control.get('Depends')):
            if not any(resolver.met(*alt) for alt in relation): raise InstallError('google-chrome-stable: %r is not met' % (relation,))
    entries = []
    for name, (suite, s) in sorted(resolver.selected.items()):
        if not wi.POOL_PATH.fullmatch(s['Filename']): raise InstallError('bad Filename %r' % s['Filename'])
        entry = {'name': name, 'version': s['Version'], 'suite': suite, 'url': MIRRORS[suite] + s['Filename'],
                 'pool': s['Filename'], 'sha256': s['SHA256'], 'bytes': int(s['Size']), 'filename': 'debs/' + PurePosixPath(s['Filename']).name,
                 'priority': s.get('Priority', ''), 'essential': s.get('Essential') == 'yes', 'groups': sorted(resolver.why[name]),
                 'source': (s.get('Source') or name).split()[0], 'depends': resolver.dependencies(name)}
        entry['snapshot'] = wi.snapshot_url(suite, meta[suite]['date'], s['Filename'])
        entries.append(entry)
    for g in google:   # Google's own repository; pinned by chrome-lock.json (verified there)
        entries.append({'name': g['name'], 'version': g['version'], 'suite': 'google', 'url': g['url'], 'pool': None,
                        'sha256': g['sha256'], 'bytes': g['bytes'], 'filename': 'debs/' + PurePosixPath(g['url']).name,
                        'priority': 'optional', 'essential': False, 'groups': ['desktop'], 'source': g['name'], 'snapshot': None,
                        'depends': []})
    lock = {'format': VERSION, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'suites': meta, 'keyring': keyring,
            'exclude': EXCLUDE, 'groups': GROUPS, 'core_order': CORE_ORDER, 'packages': entries}
    LOCK.write_bytes((json.dumps(lock, indent=1) + '\n').encode())
    by_group = {g: sum(1 for e in entries if g in e['groups']) for g in GROUPS}
    log('wrote %s: %d packages %s, %.1f MiB' % (LOCK, len(entries), by_group, sum(e['bytes'] for e in entries) / 1048576))


# ------------------------------------------------------------------------------------------------ fetch
def load_lock():
    if not LOCK.exists(): raise InstallError('%s is missing: run `lock` first' % LOCK)
    return json.loads(LOCK.read_bytes())


def selected(lock, groups):
    return [p for p in lock['packages'] if set(p['groups']) & set(groups)]


def fetch_one(entry, cache):
    if entry['suite'] == 'google':
        path = cache / entry['filename']
        if not (path.exists() and path.stat().st_size == entry['bytes'] and ai.sha256_file(path) == entry['sha256']):
            source = next((p for p in (ROOT / 'tools/chrome-packages' / PurePosixPath(entry['url']).name,
                                       nexus_paths.file('NEXUS_CHROME_DEB', nexus_paths.cache('chrome') / 'google-chrome-stable_current_amd64.deb')) if p.exists()), None)
            if not source or ai.sha256_file(source) != entry['sha256']: raise InstallError('google-chrome-stable %s is not cached' % entry['version'])
            path.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source, path)
        return path
    return wi.fetch_package(entry, cache)


def fetch_index(lock, suite, kind, cache):
    meta = lock['suites'][suite]
    digest, size = meta[kind + '_sha256'], meta[kind + '_bytes']
    name = digest + ('.InRelease' if kind == 'inrelease' else '.Packages.xz')
    path = cache / 'indices' / name
    if path.exists() and path.stat().st_size == size and ai.sha256_file(path) == digest: return path
    data = wi.fetch_url(meta[('inrelease' if kind == 'inrelease' else 'packages') + '_snapshot'], 1 << 27, (wi.SNAPSHOT,))
    if len(data) != size or sha256(data) != digest: raise InstallError('%s %s from snapshot.debian.org does not match the lock' % (suite, kind))
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data); return path


def fetch_all(lock, groups, cache):
    from concurrent.futures import ThreadPoolExecutor
    entries = selected(lock, groups)
    with ThreadPoolExecutor(8) as pool:
        paths = list(pool.map(lambda e: fetch_one(e, cache), entries))
    for suite in SUITES:
        fetch_index(lock, suite, 'inrelease', cache); fetch_index(lock, suite, 'packages_xz', cache)
    log('fetched and verified %d packages (%.1f MiB)' % (len(paths), sum(e['bytes'] for e in entries) / 1048576))
    return dict(zip((e['name'] for e in entries), paths))


# ------------------------------------------------------------------------------------------------ first stage
def deb_members(path):
    """{member name: bytes} of a .deb (ar archive)."""
    out = {}
    with open(path, 'rb') as f:
        if f.read(8) != b'!<arch>\n': raise InstallError('not a Debian package: %s' % path)
        while header := f.read(60):
            if len(header) != 60 or header[58:60] != b'`\n': raise InstallError('bad ar member in %s' % path)
            size = int(header[48:58]); name = header[:16].decode('ascii').strip().rstrip('/')
            data = f.read(size)
            if len(data) != size: raise InstallError('truncated member in %s' % path)
            out[name] = data
            if size & 1: f.read(1)
    return out


def control_fields(deb):
    """Fields of a .deb's control file."""
    with tar_of(deb_members(deb), 'control.tar', deb) as archive:
        for m in archive:
            if ai.safe_member(m.name) == 'control' and m.isreg():
                return next(wi.parse_packages(archive.extractfile(m).read().decode('utf-8')))
    raise InstallError('no control file in %s' % deb)


def tar_of(members, prefix, path):
    for name, data in members.items():
        if name.startswith(prefix):
            ext = name.rsplit('.', 1)[-1] if name != prefix + '.tar' else 'tar'
            mode = {'xz': 'r|xz', 'gz': 'r|gz', 'zst': 'r|zst', 'bz2': 'r|bz2', 'tar': 'r|'}.get(ext)
            if not mode: raise InstallError('unsupported compression %s in %s' % (name, path))
            return tarfile.open(fileobj=io.BytesIO(data), mode=mode)
    raise InstallError('no %s in %s' % (prefix, path))


class Target:
    """The ext4 root being prepared (ext4lib.Writer) with path helpers that never follow links out of the image."""
    def __init__(self, image):
        self.ext4lib = module('base_ext4lib', ROOT / 'userland/ext4lib.py')
        self.fs = self.ext4lib.Writer(str(image))
        self.e = self.ext4lib

    def node(self, path):
        try: ino = self.fs.lookup(path)
        except (KeyError, ValueError, FileNotFoundError): return None
        return ino

    def kind(self, path):
        ino = self.node(path)
        if ino is None: return None, None
        mode = self.fs.read_inode(ino).mode & 0o170000
        return {0o040000: 'dir', 0o100000: 'file', 0o120000: 'link'}.get(mode, 'other'), ino

    def put(self, path, data, mode=0o644, uid=0, gid=0, mtime_ns=None):
        parts = self.fs.split(path)
        self.fs.ensure_dir(parts[:-1], 0o755, 0, 0)
        if self.node(path) is not None: self.fs.remove(path)
        self.fs.put_bytes(data, path, mode, uid, gid, mtime_ns)

    def close(self): self.fs.close(backups=True)


def merge_usr(t, report):
    """/bin, /sbin, /lib, /lib64 -> usr/... symlinks (Debian's merged /usr). Entries of an existing real directory move
    into /usr/<dir>; a name that exists in both keeps the /usr copy when the moved entry is a symlink to it (the FAT-era
    per-file links), otherwise the root copy wins (it is what ran so far) and the /usr copy is reported."""
    fs = t.fs

    def entries(ino):
        return [(n, i, f) for n, i, f in fs.dir_entries(ino) if n not in (b'.', b'..')]

    def link_target(ino):
        inode = fs.read_inode(ino)
        if (inode.mode & 0o170000) != 0o120000: return None
        return bytes(inode.block[:inode.size]) if fs.is_fast_symlink(inode) else fs.read_file(inode)

    def write_dir(ino, parent, ents):
        inode = fs.read_inode(ino); fs.release_blocks(inode)
        inode.links = 2 + sum(1 for _, _, f in ents if f == 2)
        fs.build_directory(ino, inode, parent, ents); fs.write_inode(ino, inode)

    def merge(src, dst, src_path, dst_path):
        """Move every entry of directory src into directory dst (both rewritten once); returns entries moved."""
        moved, src_ents, dst_ents = 0, entries(src), {n: (i, f) for n, i, f in entries(dst)}
        for name, child, ft in src_ents:
            here, there = src_path + '/' + name.decode('utf-8', 'replace'), dst_path + '/' + name.decode('utf-8', 'replace')
            if name in dst_ents:
                other, other_ft = dst_ents[name]
                if ft == 2 and other_ft == 2:
                    moved += merge(child, other, here, there)
                    fs.unlink_inode(child); continue      # emptied: the parent rewrite below drops its entry
                target = link_target(child) if ft == 7 else None
                if target is not None and target.rstrip(b'/') in (there.encode(), ('../' + there.lstrip('/')).encode()):
                    fs.unlink_inode(child); continue      # FAT-era per-file merged-/usr link
                if ft != 2 and other_ft != 2:
                    a, b = fs.read_inode(child), fs.read_inode(other)
                    if a.mode != b.mode or a.size != b.size or fs.read_file(a) != fs.read_file(b):
                        report.setdefault('merge_conflicts', []).append('%s replaced %s' % (here, there))
                    fs.unlink_inode(other); dst_ents[name] = (child, ft); moved += 1; continue
                raise InstallError('cannot merge %s into %s (directory and non-directory)' % (here, there))
            dst_ents[name] = (child, ft); moved += 1
            if ft == 2:   # a moved directory's '..' names its new parent
                write_dir(child, dst, entries(child))
        write_dir(dst, next(i for n, i, f in fs.dir_entries(dst) if n == b'..'), [(n, i, f) for n, (i, f) in dst_ents.items()])
        write_dir(src, next(i for n, i, f in fs.dir_entries(src) if n == b'..'), [])
        return moved

    for top in ('bin', 'sbin', 'lib', 'lib64'):
        kind, ino = t.kind('/' + top)
        fs.ensure_dir(['usr', top], 0o755, 0, 0)
        if kind == 'link':
            if link_target(ino).rstrip(b'/') not in (b'usr/' + top.encode(), b'/usr/' + top.encode()):
                raise InstallError('/%s is a symbolic link to %r' % (top, link_target(ino)))
            continue
        if kind == 'dir':
            moved = merge(ino, fs.lookup('/usr/' + top), '/' + top, '/usr/' + top)
            fs.remove('/' + top)
            report.setdefault('merged', []).append('/%s (%d entries moved)' % (top, moved))
        elif kind is not None:
            raise InstallError('/%s is neither a directory nor a link' % top)
        fs.symlink(('usr/' + top).encode(), '/' + top)


def extract(t, deb, report, hardlinks):
    """Unpack a .deb's data.tar into the target like dpkg would (no maintainer scripts): owners, modes, links."""
    members = deb_members(deb)
    count = 0
    with tar_of(members, 'data.tar', deb) as archive:
        for m in archive:
            name = ai.safe_member(m.name)
            if not name: continue
            if name in ('bin', 'sbin', 'lib', 'lib64') and m.issym():
                if m.linkname.rstrip('/') != 'usr/' + name: raise InstallError('%s: unexpected /%s -> %s' % (deb.name, name, m.linkname))
                continue                                   # the merged-/usr link itself (made by merge_usr)
            for top in ('bin/', 'sbin/', 'lib/', 'lib64/'):
                if name.startswith(top) or name == top[:-1]: name = 'usr/' + name   # merged /usr
            path = '/' + name
            parts = t.fs.split(path)
            mtime = int(m.mtime) * 1_000_000_000
            if m.isdir():
                kind, ino = t.kind(path)
                if kind is None:
                    t.fs.ensure_dir(parts[:-1], 0o755, 0, 0)
                    t.fs.ensure_dir(parts, m.mode & 0o7777, m.uid, m.gid, mtime)
                elif kind == 'dir':
                    t.fs.setattr(path, mode=m.mode & 0o7777, uid=m.uid, gid=m.gid)
                continue
            t.fs.ensure_dir(parts[:-1], 0o755, 0, 0)
            kind, ino = t.kind(path)
            if kind == 'dir': raise InstallError('%s: %s is a directory on the target' % (deb.name, path))
            if kind is not None: t.fs.remove(path)
            if m.issym():
                t.fs.symlink(m.linkname.encode('utf-8', 'surrogateescape'), path, m.uid, m.gid, mtime)
            elif m.islnk():
                target = '/' + ai.safe_member(m.linkname)
                for top in ('bin/', 'sbin/', 'lib/', 'lib64/'):
                    if target.startswith('/' + top): target = '/usr' + target
                t.fs.link(target, path)
            elif m.isreg():
                data = archive.extractfile(m).read()
                t.fs.put_bytes(data, path, m.mode & 0o7777, m.uid, m.gid, mtime)
            else:
                report.setdefault('special_skipped', []).append(path)
                continue
            count += 1
    return count


STAGE2 = r'''#!/bin/sh
# NexusOS first boot: the second stage of the Debian base installation (debootstrap's second_stage_install, adapted).
# Run once by nexusd (nexus-firstboot.service) as root; output goes to the system journal.
set -e
BOOT=/var/lib/nexus/bootstrap
export PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true LC_ALL=C.UTF-8 LANG=C.UTF-8
deb() { sed -n "s|^$1 |$BOOT/debs/|p" $BOOT/debs.list; }
debs() { for p in "$@"; do deb "$p"; done; }
step() { echo "[stage2] $*"; }
[ -e $BOOT/done ] && { step "already complete"; exit 0; }
mkdir -p /var/lib/dpkg/info /var/lib/dpkg/updates /var/lib/dpkg/triggers /var/lib/dpkg/alternatives /var/cache/apt/archives/partial
[ -e /var/lib/dpkg/status ] || : > /var/lib/dpkg/status
[ -e /var/lib/dpkg/available ] || : > /var/lib/dpkg/available
[ -e /var/lib/dpkg/arch ] || echo amd64 > /var/lib/dpkg/arch
/usr/sbin/ldconfig || true
ln -sf mawk /usr/bin/awk
# A configuration file that is already there (a disk upgraded from FAT32 brings its settings) is kept; the package's
# version goes beside it as .dpkg-dist (no question: there is no terminal)
DPKG="dpkg --force-unsafe-io --force-confdef --force-confold"
core() { step "core: $*"; $DPKG --force-depends --install $(debs "$@"); }
if [ ! -e $BOOT/core.done ]; then
  had_accounts=; [ -e /etc/group ] && had_accounts=1
  core base-passwd
  # base-passwd only creates missing account files: an upgraded disk keeps its users and gets Debian's system users and
  # groups (root:mail for base-files, ...) merged in
  if [ -n "$had_accounts" ]; then step "adding Debian's system users and groups to the existing accounts"; update-passwd --verbose || true; fi
  core base-files
  core dpkg
  [ -e /etc/localtime ] || ln -sf /usr/share/zoneinfo/Etc/UTC /etc/localtime
  core libc6
  core perl-base
  rm -f /usr/bin/awk
  core mawk
  core debconf
  : > $BOOT/core.done
fi
# NexusOS files that Debian packages also ship stay NexusOS's: the package's copy goes beside it (dpkg-divert)
if [ -e $BOOT/diversions ]; then
  while read -r path; do
    [ -n "$path" ] || continue
    dpkg-divert --list "$path" | grep -q . || dpkg-divert --local --no-rename --divert "$path.debian" --add "$path"
  done < $BOOT/diversions
fi
if [ ! -e $BOOT/required.done ]; then
  step "unpacking required packages"
  $DPKG --force-depends --unpack $(debs $(cat $BOOT/required.list))
  step "configuring required packages"
  $DPKG --configure --pending --force-configure-any --force-depends
  : > $BOOT/required.done
fi
if [ ! -e $BOOT/rest.done ]; then
  step "installing the remaining packages"
  $DPKG --force-depends --unpack $(debs $(cat $BOOT/rest.list))
  $DPKG --configure -a
  : > $BOOT/rest.done
fi
step "dpkg audit"
dpkg --audit
# /etc/mtab -> ../proc/self/mounts as on Debian systems (libfuse, fusermount3 and mount(8) read it)
[ -e /etc/mtab ] || [ -L /etc/mtab ] || ln -s ../proc/self/mounts /etc/mtab
# System configuration from the installer ($BOOT/config: KEY=VALUE lines; a plain-text PASSWORD is hashed by chpasswd
# with the system default method and the file is then removed)
if [ -e $BOOT/config ]; then
  HOSTNAME=nexus TIMEZONE=Etc/UTC LOCALES="C.UTF-8" LANG_DEFAULT=C.UTF-8 USERNAME= USERID=1000 FULLNAME= PASSWORD= PASSWORD_HASH= GROUPS_EXTRA="sudo adm cdrom audio video plugdev netdev lpadmin users dip"
  . $BOOT/config
  step "hostname $HOSTNAME, time zone $TIMEZONE, locales $LOCALES"
  echo "$HOSTNAME" > /etc/hostname
  hostname "$HOSTNAME" || true
  grep -q "127.0.1.1" /etc/hosts 2>/dev/null || printf '127.0.1.1\t%s\n' "$HOSTNAME" >> /etc/hosts
  if [ -e "/usr/share/zoneinfo/$TIMEZONE" ]; then
    ln -sf "/usr/share/zoneinfo/$TIMEZONE" /etc/localtime; echo "$TIMEZONE" > /etc/timezone
    dpkg-reconfigure -f noninteractive tzdata >/dev/null 2>&1 || true
  fi
  if [ -e /etc/locale.gen ]; then
    for l in $LOCALES; do
      case "$l" in C.UTF-8|C.utf8) continue ;; esac
      charset=UTF-8; case "$l" in *.*) ;; *) l="$l.UTF-8" ;; esac
      grep -q "^$l " /etc/locale.gen || { sed -i "s/^# *$l /$l /" /etc/locale.gen; grep -q "^$l " /etc/locale.gen || echo "$l $charset" >> /etc/locale.gen; }
    done
    locale-gen
    update-locale "LANG=$LANG_DEFAULT"
  fi
  if [ -n "$USERNAME" ]; then
    created=
    if ! getent passwd "$USERNAME" >/dev/null; then
      step "user $USERNAME ($USERID)"; created=1
      getent group "$USERID" >/dev/null || groupadd --gid "$USERID" "$USERNAME"
      useradd --uid "$USERID" --gid "$USERID" --no-create-home --home-dir "/home/$USERNAME" --shell /bin/bash --comment "$FULLNAME" "$USERNAME"
    else
      step "user $USERNAME (kept from the old system: its files keep their owners)"
    fi
    for g in $GROUPS_EXTRA; do getent group "$g" >/dev/null && usermod -aG "$g" "$USERNAME"; done
    if [ ! -d "/home/$USERNAME" ]; then mkdir -p "/home/$USERNAME"; cp -rT /etc/skel "/home/$USERNAME"; fi
    for f in .bashrc .profile .bash_logout; do [ -e "/home/$USERNAME/$f" ] || cp "/etc/skel/$f" "/home/$USERNAME/$f" 2>/dev/null || true; done
    if [ -n "$created" ]; then chown -R "$USERID:$USERID" "/home/$USERNAME"
    else for f in .bashrc .profile .bash_logout; do chown "$USERID:$USERID" "/home/$USERNAME/$f" 2>/dev/null || true; done; fi
    if [ -n "$PASSWORD_HASH" ]; then usermod -p "$PASSWORD_HASH" "$USERNAME"
    elif [ -n "$PASSWORD" ]; then printf '%s:%s\n' "$USERNAME" "$PASSWORD" | chpasswd
    elif [ -n "$created" ]; then passwd -l "$USERNAME" >/dev/null; fi
    passwd -l root >/dev/null 2>&1 || true
  fi
  rm -f $BOOT/config
fi
date -u +%Y-%m-%dT%H:%M:%SZ > $BOOT/done
step "complete: $(dpkg-query -f '${binary:Package}\n' -W | wc -l) packages"
'''


def stage(image, groups, cache, report_path=None, config=None):
    lock = load_lock()
    paths = fetch_all(lock, groups, cache)
    entries = selected(lock, groups)
    by_name = {e['name']: e for e in entries}
    # debootstrap's "required" set: Priority: required and everything it depends on (extracted on the host)
    required, queue = set(), [e['name'] for e in entries if e['priority'] == 'required']
    while queue:
        name = queue.pop()
        if name in required: continue
        required.add(name); queue += [d for d in by_name[name].get('depends', []) if d in by_name]
    required = sorted(required)
    t = Target(image); report = {'groups': list(groups), 'packages': len(entries)}
    try:
        merge_usr(t, report)
        hardlinks = {}
        extracted = 0
        for name in required:
            extracted += extract(t, paths[name], report, hardlinks)
        report['extracted_files'] = extracted
        boot = '/var/lib/nexus/bootstrap'
        lines = []
        for e in entries:
            data = paths[e['name']].read_bytes()
            if sha256(data) != e['sha256']: raise InstallError('cache changed under us: ' + e['name'])
            leaf = PurePosixPath(e['filename']).name
            t.put(boot + '/debs/' + leaf, data)
            lines.append('%s %s' % (e['name'], leaf))
        t.put(boot + '/debs.list', ('\n'.join(lines) + '\n').encode())
        core = set(CORE_ORDER)
        t.put(boot + '/required.list', ('\n'.join(n for n in required if n not in core) + '\n').encode())
        t.put(boot + '/rest.list', ('\n'.join(e['name'] for e in entries if e['name'] not in required and e['name'] not in core) + '\n').encode())
        t.put('/usr/lib/nexus/bootstrap/stage2', STAGE2.encode(), 0o755)
        t.put(boot + '/diversions', ('\n'.join(DIVERSIONS) + '\n').encode())
        if t.node('/etc/apt/sources.list.d/debian.sources') is None:
            t.put('/etc/apt/sources.list.d/debian.sources', APT_SOURCES.encode())
        if config:   # installer settings for stage 2 (sh syntax; single-quoted values)
            lines = []
            for key, value in config.items():
                if not re.fullmatch(r'[A-Z_][A-Z0-9_]*', key) or '\n' in value or '\0' in value: raise InstallError('bad config entry %r' % key)
                lines.append("%s='%s'" % (key, value.replace("'", "'\\''")))
            t.put(boot + '/config', ('\n'.join(lines) + '\n').encode(), 0o600)
        for suite in SUITES:
            t.put(boot + '/indices/%s.InRelease' % suite, fetch_index(lock, suite, 'inrelease', cache).read_bytes())
            t.put(boot + '/indices/%s.Packages.xz' % suite, fetch_index(lock, suite, 'packages_xz', cache).read_bytes())
        report['lock_generated'] = lock['generated']
    finally:
        t.close()
    errors, stats, checker = t.ext4lib.check_image(str(image)); checker.fs.f.close()   # the checker keeps the image open (a cycle)
    if errors: raise InstallError('ext4 check failed after staging: ' + '; '.join(errors[:10]))
    log('staged %d packages (%d required, %d files extracted) into %s' % (len(entries), len(required), report['extracted_files'], image))
    if report_path: Path(report_path).write_text(json.dumps(report, indent=1))
    return report


def extract_sdk(out, cache):
    """C headers of the 'sdk' group (build-time only) into out/usr/... (no scripts, no links outside out)."""
    lock = load_lock(); paths = fetch_all(lock, ['sdk'], cache); count = 0
    for e in selected(lock, ['sdk']):
        with tar_of(deb_members(paths[e['name']]), 'data.tar', paths[e['name']]) as archive:
            for m in archive:
                name = ai.safe_member(m.name)
                if not m.isreg() or not ('/include/' in '/' + name) or not name.endswith('.h'): continue
                target = out.joinpath(*PurePosixPath(name).parts); target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(m).read()); count += 1
    log('sdk: %d headers in %s' % (count, out))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', choices=['lock', 'fetch', 'stage', 'sdk'])
    ap.add_argument('disk', nargs='?')
    ap.add_argument('--cache', type=Path, default=DEFAULT_CACHE)
    ap.add_argument('--groups', default=','.join(DEFAULT_GROUPS))
    ap.add_argument('--update-keyring', action='store_true'); ap.add_argument('--allow-older', action='store_true')
    ap.add_argument('--report')
    ap.add_argument('--config', action='append', default=[], help='KEY=VALUE for stage 2 (USERNAME, USERID, FULLNAME, PASSWORD, PASSWORD_HASH, HOSTNAME, TIMEZONE, LOCALES, LANG_DEFAULT)')
    a = ap.parse_args(argv)
    groups = [g for g in a.groups.split(',') if g]
    if any(g not in GROUPS for g in groups): ap.error('unknown group in ' + a.groups)
    a.cache.mkdir(parents=True, exist_ok=True)
    try:
        if a.action == 'lock': make_lock(a.update_keyring, a.allow_older, a.cache)
        elif a.action == 'fetch': fetch_all(load_lock(), groups, a.cache)
        elif a.action == 'sdk':
            if not a.disk: ap.error('sdk needs an output directory')
            extract_sdk(Path(a.disk), a.cache)
        else:
            if not a.disk: ap.error('stage needs a disk image')
            stage(Path(a.disk), groups, a.cache, a.report, dict(item.split('=', 1) for item in a.config))
    except InstallError as error:
        print('debian_base: error:', error, file=sys.stderr); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
