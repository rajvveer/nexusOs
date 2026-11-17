"""Debian's disk and filesystem tools for NexusOS storage work and tests, pinned and verified.

    python -I arch/x86_64/storage_tools.py lock [--allow-older]           signed indices -> arch/x86_64/storage-lock.json
    python -I arch/x86_64/storage_tools.py fetch [--cache DIR]            every .deb of the lock (size + SHA-256 checked)
    python -I arch/x86_64/storage_tools.py image OUT.img [--cache DIR] [--size MIB] [--put HOST=GUEST[:MODE]]...

Tools (Debian 13 "trixie", amd64): util-linux (lsblk, blkid, wipefs, mount, umount, losetup), fdisk (fdisk, sfdisk),
parted, e2fsprogs (mkfs.ext4, e2fsck), dosfstools (mkfs.vfat, fsck.vfat), exfatprogs (mkfs.exfat, fsck.exfat),
fuse3 (fusermount3), fuse2fs, ntfs-3g (+ mkntfs), sshfs with openssh-sftp-server, cryptsetup, dmsetup, xorriso (ISO
images with Rock Ridge and Joliet), keyutils (keyctl), plus dash, coreutils, grep, sed, findutils and diffutils for scripts.

Verification is arch/x86_64/wine_install.py's: every InRelease must carry a good signature by a Debian archive key
(gpgv, the debian-archive-keyring .deb pinned in wine-lock.json), Packages.xz must match its signed SHA-256, the
dependency closure is resolved over trixie, trixie-updates and trixie-security, and every .deb is pinned (URL, size,
SHA-256) in storage-lock.json. Downloads only come from deb.debian.org (snapshot.debian.org once a version left the
archive). `fetch` also records each package's licenses (the License: fields of its debian/copyright) in the lock.
`image` builds an ext4 NexusOS test disk: out/apps64.img converted by userland/fat2ext4.py (default 768 MiB), with
every locked package's files (owners, modes and set-user-ID bits from the .deb; docs/man/locales left out except the
copyright files) and the extra --put files. Maintainer scripts are not run. Nothing here is part of the kernel.
"""
from pathlib import Path, PurePosixPath
import os
import argparse
import importlib.util
import json
import re
import shutil
import sys
import tarfile
import time
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
LOCK = HERE / 'storage-lock.json'
CACHE = nexus_paths.cache('storage')
SHARED_CACHE = nexus_paths.cache('apps')   # read only: .debs other rounds verified (re-checked against this lock)
PACKAGES = ['dash', 'coreutils', 'grep', 'sed', 'diffutils', 'util-linux', 'mount', 'fdisk', 'parted', 'e2fsprogs', 'dosfstools',
            'exfatprogs', 'fuse3', 'fuse2fs', 'ntfs-3g', 'sshfs', 'openssh-sftp-server', 'cryptsetup-bin', 'dmsetup',
            'xorriso', 'keyutils', 'findutils', 'lvm2', 'thin-provisioning-tools', 'mdadm']
SKIP = {
    'libpam-modules': 'PAM modules for su/login (util-linux): not used by the storage tools',
    'libpam-modules-bin': 'PAM helpers: not used', 'libpam-runtime': 'PAM configuration: not used',
    'debconf': 'package configuration: maintainer scripts are not run', 'sensible-utils': 'maintainer scripts only',
    'libdebconfclient0': 'debconf client: not used', 'lsb-base': 'init scripts', 'init-system-helpers': 'init scripts',
    'adduser': 'maintainer scripts only', 'passwd': 'maintainer scripts only', 'login': 'login programs: not used',
    'ucf': 'maintainer scripts only', 'procps': 'maintainer scripts only', 'runit-helper': 'init scripts',
    'debconf-2.0': 'virtual (debconf): maintainer scripts are not run',
}
POSTINST = [('usr/bin/fusermount3', 0o4755, 'fuse3 postinst: fusermount3 is set-user-ID root')]   # maintainer-script effects emulated
SKIP_PATHS = re.compile(r'^(usr/share/(doc|man|info|locale|lintian|bug|bash-completion|zsh|fish)/|etc/(init\.d|cron|logrotate)|lib/systemd|usr/lib/systemd)')


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


wi = module('storage_wine_install', HERE / 'wine_install.py')
ai = wi.ai


def make_lock(allow_older=False):
    wine_lock = json.loads((HERE / 'wine-lock.json').read_text())
    previous = json.loads(LOCK.read_text()) if LOCK.exists() else {}
    keyring = previous.get('keyring') or wine_lock['keyring']
    wi.MIRRORS = {suite: url for suite, url in wi.MIRRORS.items() if suite != 'sid'}
    packages, meta, keyring = wi.verified_indices(keyring, previous.get('suites'), allow_older)
    packages['sid'] = {}
    wi.GROUPS = {'disk-tools': {'trixie': PACKAGES}}; wi.SKIP = dict(SKIP)
    selected, skipped, _ = wi.resolve(packages, {})
    old = {p['name']: p for p in previous.get('packages', [])}
    for p in selected.values():
        if old.get(p['name'], {}).get('sha256') == p['sha256'] and old[p['name']].get('licenses'): p['licenses'] = old[p['name']]['licenses']
    lock = {'format': 1, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'suites': meta, 'keyring': keyring,
            'roots': PACKAGES, 'skipped': dict(sorted(skipped.items())), 'packages': sorted(selected.values(), key=lambda p: p['name'])}
    LOCK.write_text(json.dumps(lock, indent=1) + '\n')
    print('[storage-tools] wrote', LOCK, len(selected), 'packages,', len(skipped), 'skipped', flush=True)


def deb(entry, cache):
    """The .deb from the shared cache when it matches the lock, else downloaded into our cache."""
    name = PurePosixPath(entry['filename']).name
    shared = SHARED_CACHE / 'debs' / name
    if shared.is_file() and shared.stat().st_size == entry['bytes'] and ai.sha256_file(shared) == entry['sha256']: return shared
    return wi.fetch_package(entry, cache)


def licenses_of(path, package):
    with ai.open_payload({'kind': 'deb'}, path) as archive:
        for member in archive:
            if ai.safe_member(member.name) == f'usr/share/doc/{package}/copyright' and member.isfile():
                text = archive.extractfile(member).read().decode('utf-8', 'replace')
                found = sorted({line.split(':', 1)[1].strip() for line in text.splitlines() if line.startswith('License:') and line.split(':', 1)[1].strip()})
                return found or ['see copyright (unstructured)']
    return ['copyright file not found in the package']


def fetch(cache):
    lock = json.loads(LOCK.read_text()); changed = False
    for entry in lock['packages']:
        path = deb(entry, cache)
        if not entry.get('licenses'): entry['licenses'] = licenses_of(path, entry['name']); changed = True
        print(f"  {entry['name']:28} {entry['version']:28} {', '.join(entry['licenses'])[:70]}", flush=True)
    if changed: LOCK.write_text(json.dumps(lock, indent=1) + '\n')
    print('[storage-tools] all', len(lock['packages']), 'packages verified', flush=True)


def install(fs, path, report):
    links = []
    with ai.open_payload({'kind': 'deb'}, path) as archive:
        for member in archive:
            name = ai.safe_member(member.name)
            if not name or name == '.' or SKIP_PATHS.match(name) and not name.endswith('/copyright'): continue
            parts = [p for p in name.split('/') if p]
            mode = member.mode & 0o7777
            if member.isdir(): fs.ensure_dir(parts, mode, member.uid, member.gid); continue
            fs.ensure_dir(parts[:-1])
            if member.issym():
                try: fs.remove(name)
                except Exception: pass
                fs.symlink(member.linkname, name, member.uid, member.gid); report['symlinks'] += 1
            elif member.islnk(): links.append((ai.safe_member(member.linkname), name))
            elif member.isfile():
                fs.put_bytes(archive.extractfile(member).read(), name, mode, member.uid, member.gid); report['files'] += 1
                if mode & 0o6000: report['setid'].append('/' + name)
    for existing, name in links:
        try: fs.remove(name)
        except Exception: pass
        fs.link(existing, name)


def image(output, cache, size, puts):
    lock = json.loads(LOCK.read_text())
    fat2ext4 = module('storage_fat2ext4', ROOT / 'userland/fat2ext4.py')
    output = Path(output)
    if output.exists(): output.unlink()
    fat2ext4.convert(str(OUT / 'apps64.img'), str(output), size_mib=size)
    fs = fat2ext4.ext4lib.Writer(str(output))
    report = {'files': 0, 'symlinks': 0, 'setid': [], 'packages': []}
    try:
        for entry in lock['packages']:
            install(fs, deb(entry, cache), report); report['packages'].append(entry['name'] + ' ' + entry['version'])
        for path, mode, why in POSTINST:
            try: fs.setattr(path, mode=mode)
            except Exception as error: print('[storage-tools] postinst emulation failed for', path, error)
        try: fs.symlink('/usr/bin/dash', 'bin/sh', 0, 0)   # usrmerge: /bin/sh is dash (popen, system(), sh -c)
        except Exception: pass                             # the root already has one
        try: fs.symlink('../proc/self/mounts', 'etc/mtab', 0, 0)   # as Debian systems have it (fusermount3 -u reads it)
        except Exception: pass
        for tool in ('mount', 'umount'):   # usrmerge: /bin/mount (libfuse runs it to record a mount when /etc/mtab exists)
            try: fs.symlink('/usr/bin/' + tool, 'bin/' + tool, 0, 0)
            except Exception: pass
        for spec in puts:
            host, guest = spec.split('=', 1); mode = 0o755
            if ':' in guest[1:]: guest, mode_text = guest.rsplit(':', 1); mode = int(mode_text, 8)
            parts = [p for p in guest.split('/') if p]; fs.ensure_dir(parts[:-1])
            fs.put_bytes(Path(host).read_bytes(), '/'.join(parts), mode, 0, 0)
    finally:
        fs.close()
    print(f"[storage-tools] {output}: {len(report['packages'])} packages, {report['files']} files, {report['symlinks']} symlinks, "
          f"set-ID: {' '.join(report['setid'])}", flush=True)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=['lock', 'fetch', 'image'])
    parser.add_argument('output', nargs='?', type=Path)
    parser.add_argument('--cache', type=Path, default=CACHE)
    parser.add_argument('--size', type=int, default=768)
    parser.add_argument('--put', action='append', default=[])
    parser.add_argument('--allow-older', action='store_true')
    args = parser.parse_args(argv)
    if args.action == 'lock': make_lock(args.allow_older)
    elif args.action == 'fetch': fetch(args.cache)
    else:
        if not args.output: parser.error('image needs OUT.img')
        image(args.output, args.cache, args.size, args.put)
    return 0


if __name__ == '__main__':
    sys.exit(main())
