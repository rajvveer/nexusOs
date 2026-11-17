"""FAT32 desktop disk -> ext4 root migration (arch/x86_64/sysinstall.py migrate) and the new-install path, host side.

    python -I tests/test_x64_migrate.py WORK [--source FAT_DESKTOP.img] [--cache DIR] [--keep]

A copy of --source (the fresh desktop disk; never the user's disk) gets a synthetic user data set written the way NexusOS's
FAT32 driver stores it (8.3-compatible names as bare short entries, i.e. without their case): a Chrome user data directory
(~/chrome-user-profile: Local State, Default/History, Default/Network/Cookies, a LevelDB folder with MANIFEST/CURRENT/
LOCK/LOG/LOG.old), XDG folders, Unicode and long names, an empty file, a 24 MiB file, deep folders. The test passes
--skip /home/nexus/.config --skip /home/nexus/.cache: those folders of the test disk are never read (a real migration
takes them too). Then:
  - migrate: every file is copied (sysinstall's own comparison: content SHA-256, links, folders) and the test checks
    again with its own walk; the 8.3 names get their case back (Default, History, Network/Cookies, CURRENT, LOCK, LOG,
    LOG.old, Pictures); the profile is owned by the user; the old disk is byte-identical afterwards (SHA-256);
  - the new disk has the Debian base staged and the services installed (first boot does the rest; the boot itself is
    covered by tests/test_x64_services.py, which builds a new install the same way);
  - hostile cases: an existing target, the source as target, a source that is not a NexusOS disk -> refused, nothing
    written; a damaged FAT chain in a copy -> the migration stops with an error.
Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import gc
import hashlib
import json
import os
import random
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # the fixture has Hindi file names (fatput prints them)
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--cache', type=Path, help='debian_base.py cache')
ap.add_argument('--keep', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'migrate', 'checks': [], 'passed': False}
SYSINSTALL = ROOT / 'arch/x86_64/sysinstall.py'


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(8 << 20): digest.update(block)
    return digest.hexdigest()


def remove(path):
    """Delete a file; Windows may hold a freshly written image open for a moment (indexer, antivirus): retry."""
    for attempt in range(30):
        try: Path(path).unlink(missing_ok=True); return
        except PermissionError: gc.collect(); time.sleep(2)
    Path(path).unlink(missing_ok=True)


def sysinstall(*argv):
    return subprocess.run([sys.executable, '-I', '-X', 'utf8', str(SYSINSTALL), *map(str, argv)], capture_output=True, text=True, encoding='utf-8', errors='replace')


fatput = pkg_vm.module('migrate_fatput', ROOT / 'userland/fatput.py')
H = 'home/nexus/'
rng = random.Random(7)
# guest path (as the FAT driver writes it: 8.3-compatible names in upper case) -> content
FIXTURE = {
    H + 'chrome-user-profile/Local State': b'{"profile":{"info_cache":{"Default":{"name":"Person 1"}}}}',
    H + 'chrome-user-profile/DEFAULT/HISTORY': b'SQLite format 3\0' + rng.randbytes(40000),
    H + 'chrome-user-profile/DEFAULT/NETWORK/COOKIES': b'SQLite format 3\0' + rng.randbytes(20000),
    H + 'chrome-user-profile/DEFAULT/Preferences': b'{"browser":{"has_seen_welcome_page":true}}',
    H + 'chrome-user-profile/DEFAULT/Local Storage/leveldb/MANIFEST-000001': rng.randbytes(300),
    H + 'chrome-user-profile/DEFAULT/Local Storage/leveldb/CURRENT': b'MANIFEST-000001\n',
    H + 'chrome-user-profile/DEFAULT/Local Storage/leveldb/LOCK': b'',
    H + 'chrome-user-profile/DEFAULT/Local Storage/leveldb/LOG': b'leveldb log\n',
    H + 'chrome-user-profile/DEFAULT/Local Storage/leveldb/LOG.OLD': b'old log\n',
    H + 'chrome-user-profile/DEFAULT/Local Storage/leveldb/000003.LOG': rng.randbytes(5000),
    H + 'vscode-data/USER/settings.json': b'{"telemetry.telemetryLevel": "off"}',
    H + 'PICTURES/holiday.png': b'\x89PNG\r\n\x1a\n' + rng.randbytes(3000),
    H + 'Documents/\u0928\u092e\u0938\u094d\u0924\u0947 \u0926\u0941\u0928\u093f\u092f\u093e.txt': '\u0928\u092e\u0938\u094d\u0924\u0947\n'.encode(),
    H + 'Documents/Quarterly report for the board (final version).odt': rng.randbytes(70000),
    H + 'Documents/empty.txt': b'',
    H + 'Documents/big.bin': rng.randbytes(24 << 20),
    H + 'Documents/a/b/c/d/e/f/g/deep.txt': b'deep\n',
}
EXPECT = {   # ext4 path -> FIXTURE key (case restored)
    '/home/nexus/chrome-user-profile/Default/History': 'chrome-user-profile/DEFAULT/HISTORY',
    '/home/nexus/chrome-user-profile/Default/Network/Cookies': 'chrome-user-profile/DEFAULT/NETWORK/COOKIES',
    '/home/nexus/chrome-user-profile/Default/Local Storage/leveldb/CURRENT': 'chrome-user-profile/DEFAULT/Local Storage/leveldb/CURRENT',
    '/home/nexus/chrome-user-profile/Default/Local Storage/leveldb/LOCK': 'chrome-user-profile/DEFAULT/Local Storage/leveldb/LOCK',
    '/home/nexus/chrome-user-profile/Default/Local Storage/leveldb/LOG': 'chrome-user-profile/DEFAULT/Local Storage/leveldb/LOG',
    '/home/nexus/chrome-user-profile/Default/Local Storage/leveldb/LOG.old': 'chrome-user-profile/DEFAULT/Local Storage/leveldb/LOG.OLD',
    '/home/nexus/chrome-user-profile/Default/Local Storage/leveldb/000003.log': 'chrome-user-profile/DEFAULT/Local Storage/leveldb/000003.LOG',
    '/home/nexus/vscode-data/User/settings.json': 'vscode-data/USER/settings.json',
    '/home/nexus/Pictures/holiday.png': 'PICTURES/holiday.png',
    '/home/nexus/Documents/a/b/c/d/e/f/g/deep.txt': 'Documents/a/b/c/d/e/f/g/deep.txt',
}

started = time.time()
base = pkg_vm.compressed_dir(WORK / 'base')
old, new = base / 'old.img', base / 'new.img'
try:
    for p in (old, new, Path(str(new) + '.migration.json')): p.unlink(missing_ok=True)
    pkg_vm.sparse_copy(args.source, old)
    staging = WORK / 'fixture'; staging.mkdir(exist_ok=True)
    edit = ['fatput', str(old)]
    for i, (guest, data) in enumerate(FIXTURE.items()):
        host = staging / ('%02d.bin' % i); host.write_bytes(data); edit += ['put', str(host), guest]
    fatput.edit(edit)
    old_hash = sha256(old)
    # ---- hostile: nothing is written
    SKIP = ('--skip', '/home/nexus/.config', '--skip', '/home/nexus/.cache')   # the test disk's profile folders: never read
    new.write_bytes(b'occupied'); r = sysinstall('migrate', old, new, *SKIP)
    check('an existing target is refused', r.returncode != 0 and 'exists' in (r.stderr + r.stdout) and new.read_bytes() == b'occupied', r.stderr[-300:])
    new.unlink()
    r = sysinstall('migrate', old, old, *SKIP)
    check('the old disk as target is refused', r.returncode != 0 and sha256(old) == old_hash, r.stderr[-300:])
    junk = base / 'junk.img'; junk.write_bytes(os.urandom(1 << 20) + bytes(64 << 20))
    r = sysinstall('migrate', junk, base / 'junk-new.img', *SKIP)
    check('a source that is not a NexusOS disk is refused', r.returncode != 0 and not (base / 'junk-new.img').exists(), r.stderr[-300:])
    junk.unlink(); (base / 'junk-new.img').unlink(missing_ok=True)
    # ---- the migration
    t0 = time.time()
    r = sysinstall('migrate', old, new, *SKIP, *(['--cache', args.cache] if args.cache else []))
    (WORK / 'migrate.log').write_text(r.stdout + r.stderr)
    check('sysinstall migrate succeeds (%.0f s)' % (time.time() - t0), r.returncode == 0, (r.stdout + r.stderr)[-600:])
    report = json.loads(Path(str(new) + '.migration.json').read_text()) if Path(str(new) + '.migration.json').exists() else {}
    comparison = report.get('comparison', {})
    check('its comparison: every file, link and folder identical (%s files, %s MiB)' % (comparison.get('files'), (comparison.get('bytes') or 0) >> 20),
          comparison and not comparison.get('differences') and not comparison.get('missing') and comparison.get('files', 0) > 4000, comparison.get('differences', [])[:5])
    check('only the folders the test excludes were left out', sorted(report.get('converted', {}).get('skipped', [])) == ['/home/nexus/.cache', '/home/nexus/.config'],
          report.get('converted', {}).get('skipped'))
    check('the old disk is unchanged (SHA-256)', sha256(old) == old_hash)
    # ---- independent checks on the new disk
    fs = pkg_vm.ext4lib.Ext4(str(new))
    for path, key in EXPECT.items():
        data = pkg_vm.guest_read(fs, path)
        check('case restored and content identical: %s' % path, data == FIXTURE[H + key], 'missing' if data is None else '%d bytes' % len(data))
    for path in FIXTURE:
        if '/DEFAULT/' in path or 'PICTURES' in path or '/USER/' in path: continue   # checked above with their restored case
        data = pkg_vm.guest_read(fs, '/' + path)
        check('content identical: /%s' % path, data == FIXTURE[path], 'missing' if data is None else '%d bytes' % len(data))
    inode = fs.read_inode(fs.lookup('/home/nexus/chrome-user-profile/Default/History'))
    check('migrated profile files belong to the user (1000:1000, 0644)', (inode.uid, inode.gid, inode.mode & 0o7777) == (1000, 1000, 0o644), (inode.uid, inode.gid, oct(inode.mode)))
    check('no lower-case "default" left beside "Default"', fs.lookup('/home/nexus/chrome-user-profile/default') is None)
    staged = pkg_vm.guest_read(fs, '/usr/lib/nexus/bootstrap/stage2')
    check('the Debian base is staged for the first boot', staged is not None and b'dpkg' in staged)
    check('the service manager is installed set-user-ID root', (lambda i: i and (fs.read_inode(i).mode & 0o7777) == 0o4755 and fs.read_inode(i).uid == 0)(fs.lookup('/usr/lib/nexus/nexusd')))
    check('Aurora is the init program', pkg_vm.guest_read(fs, '/usr/sbin/init') is not None or pkg_vm.guest_read(fs, '/sbin/init') is not None)
    checked = pkg_vm.ext4lib.check_image(str(new)); errors = checked[0]; checked[2].fs.f.close(); del checked   # the checker keeps the image open
    check('the new ext4 volume passes the consistency check', not errors, errors[:5])
    fs.close()
    results['home_lowercased'] = report.get('home_lowercased', [])[:50]
    # ---- a damaged source stops the migration
    remove(new); remove(str(new) + '.migration.json')
    damaged = base / 'damaged.img'; damaged.unlink(missing_ok=True); pkg_vm.sparse_copy(old, damaged)
    vol = fatput.Fat32(str(damaged)); hit = vol.find_name(vol.lookup_dir(['home', 'nexus', 'Documents']), 'big.bin')
    first = (int.from_bytes(hit[2][20:22], 'little') << 16) | int.from_bytes(hit[2][26:28], 'little')
    vol.fat_set(first, 0x0FFFFFF7); vol.flush_fat(); vol.close()   # bad-cluster mark inside the 24 MiB file's chain
    r = sysinstall('migrate', damaged, new, *SKIP)
    check('a damaged FAT chain stops the migration with an error', r.returncode != 0, (r.stdout + r.stderr)[-300:])
    remove(damaged)
finally:
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    if not args.keep:
        for p in (old, new, base / 'damaged.img', Path(str(new) + '.migration.json')): remove(p)
        for p in (WORK / 'fixture').glob('*.bin'): p.unlink()
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
