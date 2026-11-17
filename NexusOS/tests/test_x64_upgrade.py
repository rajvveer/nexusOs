"""System Upgrade end to end: a FAT32 desktop disk becomes the full system and goes back, the way a user does it
(System Settings > System Upgrade in NexusOS, arch/x86_64/migrate_disk.py run by the launcher between two sessions).

    python -I tests/test_x64_upgrade.py WORK --source DESKTOP.img [--cache DIR] [--accel auto|whpx|tcg] [--qmp-port 4773]
           [--no-keep-cycle]

--source: a FAT32 desktop disk with Aurora, Chrome, Firefox, Wine (e.g. the integrator's disk; only read: the test makes
its own sparse copy WORK/disk/desktop64.img, never the user's disk). The copy gets this tree's Aurora (out/desktop.elf,
out/aurora_app.elf), System Settings' System Upgrade page as the session's program, and a synthetic user data set
(a Chromium-style profile with 8.3 names, documents, a file in the Wine prefix). The test leaves the copy's
/home/nexus/.config and .cache out of the upgrade (--skip, never read); a real upgrade takes them too.
  boot 1 (FAT32)   the page: mismatching passwords refused (no request); then the password, "Upgrade at Next Start"
                   (request.json written), "Shut Down Now" (the system powers off)
  host             hostile first: the disk open by another program, too little free space (failed.json, the request set
                   aside), a forged request (a shell command as the host name) - each refused, nothing converted; then
                   the upgrade as the launcher runs it: every file copied and compared (0 differences), the old disk
                   kept byte for byte (SHA-256), the new disk consistent, the report in it, the account's password set
  boot 2 (ext4)    the first start installs the Debian base (the page shows it), the greeter takes the password from
                   boot 1, the page shows the report; "Go Back to the Old Disk", "Shut Down Now"
  host             the launcher's step goes back: the FAT32 disk again with every file, the full system kept beside it;
                   `finish` deletes it. Then (unless --no-keep-cycle) a second upgrade and "Keep the Full System" (a
                   finish-request): the launcher's step deletes the old disk.
Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import random
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--source', type=Path, required=True)
ap.add_argument('--cache', type=Path, help='debian_base.py cache (default NEXUS_BASE_CACHE)')
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4773); ap.add_argument('--no-keep-cycle', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
OUT = ROOT / 'arch/x86_64/out'
results = {'test': 'upgrade', 'checks': [], 'passed': False}
md = pkg_vm.module('upgrade_migrate_disk', ROOT / 'arch/x86_64/migrate_disk.py')
fatput = md.fatput
nexus_paths = pkg_vm.nexus_paths
SKIP = ('/home/nexus/.config', '/home/nexus/.cache')   # the copy's profile folders: never read here
# Left out of the test's upgrades only to keep its disks small (4 GB cap on the test host): Wine, the apps, downloads.
# The checks cover every other file; the product (the page, launcher_step) never leaves anything out.
BIG = ('/usr/lib/x86_64-linux-gnu/wine', '/home/nexus/Applications', '/opt', '/home/nexus/Downloads', '/home/nexus/chrome-user-profile')
PW = 'Upgrade-Pass-61'
H = 'home/nexus/'
rng = random.Random(11)
FIXTURE = {   # guest path as the FAT driver writes it (8.3 names upper case) -> content
    H + 'chrome-upgrade-profile/Local State': b'{"profile":{"info_cache":{"Default":{"name":"Upgrade test"}}}}',
    H + 'chrome-upgrade-profile/DEFAULT/HISTORY': b'SQLite format 3\0' + rng.randbytes(30000),
    H + 'chrome-upgrade-profile/DEFAULT/Local Storage/leveldb/CURRENT': b'MANIFEST-000001\n',
    H + 'chrome-upgrade-profile/DEFAULT/Local Storage/leveldb/MANIFEST-000001': rng.randbytes(200),
    H + 'Documents/Upgrade notes (keep).txt': 'every file stays नमस्ते\n'.encode(),
    H + 'Documents/upgrade-big.bin': rng.randbytes(9 << 20),
    H + '.wine/drive_c/users/nexus/Documents/upgrade-marker.txt': b'a file in the Wine prefix\n',
}
EXPECT = {'/home/nexus/chrome-upgrade-profile/Default/History': H + 'chrome-upgrade-profile/DEFAULT/HISTORY',
          '/home/nexus/chrome-upgrade-profile/Default/Local Storage/leveldb/CURRENT': H + 'chrome-upgrade-profile/DEFAULT/Local Storage/leveldb/CURRENT',
          '/home/nexus/Documents/Upgrade notes (keep).txt': H + 'Documents/Upgrade notes (keep).txt',
          '/home/nexus/Documents/upgrade-big.bin': H + 'Documents/upgrade-big.bin',
          '/home/nexus/.wine/drive_c/users/nexus/Documents/upgrade-marker.txt': H + '.wine/drive_c/users/nexus/Documents/upgrade-marker.txt'}


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(16 << 20): digest.update(block)
    return digest.hexdigest()


def fat_edit(disk, puts=(), removes=()):
    staging = WORK / 'staging'; staging.mkdir(exist_ok=True)
    fs = fatput.Fat32(str(disk))
    try:
        for guest in removes:
            try: fs.rm(guest)
            except SystemExit: pass
        for i, (guest, data) in enumerate(puts):
            host = staging / ('%03d.bin' % i); host.write_bytes(data)
            try: fs.rm(guest)
            except SystemExit: pass
            fs.put(str(host), guest)
    finally: fs.close()
    for p in staging.glob('*.bin'): p.unlink()


def refused(name, call, expect):
    try: call(); return check(name, False, 'accepted')
    except md.Refused as error: return check(name, expect in str(error), str(error))


def boot(disk, tag):
    """One session on the disk (through an overlay that is committed into the disk afterwards, as if booted directly)."""
    return pkg_vm.Vm(WORK, disk, args.qmp_port, tag=tag, accel=args.accel, smp=2, memory='3072', overlay_name='session.qcow2')


def commit(vm):
    subprocess.run([pkg_vm.QEMU_IMG, 'commit', '-q', str(vm.overlay)], check=True); vm.overlay.unlink(missing_ok=True)


def page(vm, mark=0, seconds=300):
    win = vm.wait(r'AURORA WINDOW app=nexus.settings title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds)
    return int(win.group(1)), int(win.group(2))


def click_off(vm, x, y):
    """A click that powers the machine off: QEMU may be gone before the button is released."""
    try: vm.click(x, y)
    except (OSError, EOFError, ConnectionError): pass


def controls(vm, mark, latest=False):
    """Window-local centres of the page's controls: the first report after mark, or (latest) the last one so far (the
    layout changes when the first start ends)."""
    m = vm.wait(r'\[64\] UPGRADE CONTROLS ([^\n]*)', mark, 60)
    text = re.findall(r'\[64\] UPGRADE CONTROLS ([^\n]*)', vm.text()[mark:])[-1] if latest else m.group(1)
    return {k: tuple(map(int, v.split(','))) for k, v in re.findall(r'(\w+)=(-?\d+,-?\d+)', text)}


FAILING_STAGE2 = b'#!/bin/sh\necho "[stage2] forced failure (test_x64_upgrade)" >&2\nexit 1\n'


def settle(vm, quiet=40, limit=150):
    """Wait until the guest has written nothing to its disk for `quiet` s (at most `limit` s), going by the kernel's
    5-second `[64] IOSTAT ... wr=N` deltas: ext4 checkpoints 30 s after the last commit, and that checkpoint is a write
    too, so `quiet` s without any write means the log is empty. Otherwise a checkpoint forced by a later commit (full
    checkpoint list or cache, ext4_journal.c) can write the request home before the power fails - seen on the
    integrator's disk, whose user 1000 brings a user manager and more writes. (The serial log itself never goes quiet:
    the page polls its state every few seconds.) The limit stays well under Aurora's idle lock (lock_after=300 s)."""
    start = time.time(); end = start + limit; seen = len(vm.text()); last_write = start
    while time.time() < end:
        time.sleep(1); text = vm.text()
        done = text.rfind('\n') + 1   # whole lines only
        if any(int(w, 16) for w in re.findall(r'\[64\] IOSTAT [^\n]*? wr=(?:0x)?([0-9A-Fa-f]+)', text[seen:done])): last_write = time.time()
        seen = max(seen, done)
        if time.time() - last_write >= quiet: return True
    return False


def edge_cases():
    """On overlays over the freshly upgraded D (never committed): (1) the first start fails (stage2 replaced by a
    failing script): the page says so and offers only the way back; (2) "Go Back" right before a power failure: the
    request is committed to the ext4 journal (fsync of the file and its folder) but the volume is not checked out,
    and the launcher's ext4_read still finds it (journal replayed in memory)."""
    fail = WORK / 'edge-fail.qcow2'; session = WORK / 'edge-session.qcow2'
    def edit(fs):
        path = '/usr/lib/nexus/bootstrap/stage2'
        if fs.lookup(path): fs.remove(path)
        fs.put_bytes(FAILING_STAGE2, path, 0o755, 0, 0)
    pkg_vm.edit_overlay(D, fail, edit)
    vm = pkg_vm.Vm(WORK, fail, args.qmp_port, tag='edge', accel=args.accel, smp=2, memory='3072', overlay_name=session.name)
    try:
        logged, detail = pkg_vm.greeter_login(vm, password=PW, seconds=900)
        m = vm.wait(r'\[nexusd\] nexus-firstboot\.service (?:finished|failed) \(result: ([\w-]+)\)', 0, 900)
        check('edge: the first start fails (stage 2 replaced by a failing script)', m.group(1) != 'success' and logged, (m.group(0), detail))
        st = vm.wait(r'\[64\] UPGRADE STATE upgraded used=\d+ firstboot=2 packages=\d+', 0, 180)
        sx, sy = page(vm); time.sleep(3); c = controls(vm, 0, latest=True); vm.shot('edge-failed')
        check('edge: the page shows the failed first start and offers only the way back', c.get('keep') == (0, 0) and c.get('back', (0, 0)) != (0, 0), (st.group(0), c))
        quiet = settle(vm); print('edge: no disk writes for 40 s before "Go Back":', quiet, flush=True)
        mark = len(vm.text()); vm.click(sx + c['back'][0], sy + c['back'][1])
        vm.wait(r'\[64\] UPGRADE ROLLBACK requested', mark, 60)
        vm.proc.kill(); vm.proc.wait(30)   # power failure right after the request
    finally:
        vm.close(keep_overlay=True)
    original = md.ext4lib.__dict__.get('open')
    import builtins
    md.ext4lib.open = lambda path, mode='rb', *a, **k: pkg_vm.Qcow2View(str(session)) if str(path) == str(D) else builtins.open(path, mode, *a, **k)
    try:
        fs = md.ext4lib.Ext4(str(D))   # what a reader without journal replay sees
        try: ino = fs.lookup(md.UPDIR + '/rollback-request')
        except (KeyError, ValueError, FileNotFoundError): ino = None
        js = md.ext4lib.journal_super(fs); fs.f.close()
        seen = md.ext4_read(D, md.UPDIR + '/rollback-request'); pending = md.pending(D)
    finally:
        if original is None: del md.ext4lib.open
        else: md.ext4lib.open = original
    check('edge: after the power failure the request is only in the ext4 journal', not ino and js and js['start'], 'ino=%s journal start=%s' % (ino, js and js['start']))
    check('edge: the launcher still finds it (journal replayed in memory): pending rollback', seen is not None and pending == 'rollback', (seen, pending))
    for p in (session, fail): p.unlink(missing_ok=True)


started = time.time()
base = pkg_vm.compressed_dir(WORK / 'disk'); D = base / 'desktop64.img'
leftovers = [D, md.side(D, '-fat.img'), md.side(D, '-full.img'), md.side(D, '.upgrading'), md.side(D, '.upgrade.json'), md.side(D, '.migration.json')]
vm = None
finished = False
try:
    for p in leftovers: p.unlink(missing_ok=True)
    nexus_paths.refuse_user_disk(args.source, '--source')
    print('working copy: %.2f GB of used clusters' % (pkg_vm.fat_used_copy(args.source, D) / 1e9), flush=True)
    fat_edit(D, [('sbin/init', (OUT / 'desktop.elf').read_bytes()), ('bin/aurora-app', (OUT / 'aurora_app.elf').read_bytes()),
                 ('nexus/desktop.args', b'--aurora\n/bin/aurora-app\nsettings\nupgrade\n')] + list(FIXTURE.items()),
             removes=[md.FAT_UPDIR + '/request.json', md.FAT_UPDIR + '/failed.json', md.FAT_UPDIR + '/request.failed.json'])
    check('the copy is a NexusOS FAT32 desktop disk', md.kind_of(D) == 'fat')
    # ------------------------------------------------------------------ boot 1: the request from the page
    vm = boot(D, 'fat')
    vm.wait(r'\[64\] UPGRADE STATE fat used=\d+', 0, 600); sx, sy = page(vm)
    c = controls(vm, 0); time.sleep(2)
    vm.click(sx + c['field0'][0], sy + c['field0'][1]); vm.type(PW); vm.click(sx + c['field1'][0], sy + c['field1'][1]); vm.type(PW + 'x')
    mark = len(vm.text()); vm.click(sx + c['go'][0], sy + c['go'][1]); time.sleep(4)
    check('boot1: mismatching passwords: no request', '[64] UPGRADE REQUEST' not in vm.text()[mark:])
    vm.click(sx + c['field1'][0], sy + c['field1'][1])
    for _ in range(len(PW) + 2): vm.tap('backspace')
    vm.type(PW); mark = len(vm.text()); vm.click(sx + c['go'][0], sy + c['go'][1])
    m = vm.wait(r'\[64\] UPGRADE REQUEST written user=(\S+) host=(\S+)', mark, 60)
    check('boot1: the page writes the request', m.group(1) == 'nexus', m.group(0))
    vm.wait(r'\[64\] UPGRADE STATE scheduled', mark, 30); c = controls(vm, mark); vm.shot('scheduled')
    click_off(vm, sx + c['shutdown'][0], sy + c['shutdown'][1])
    code = vm.exited(300)
    check('boot1: "Shut Down Now" powers the system off', code is not None and '[64] UPGRADE SHUTDOWN requested' in vm.text(), 'exit=%s' % code)
    check('boot1: no kernel or user fault', not vm.faults(), vm.faults()[:3])
    vm.close(keep_overlay=True); commit(vm); vm = None
    raw = md.fat_read(D, md.REQUEST); request = json.loads(raw) if raw else {}
    check('the request: user, a password hash (never the password), host name', request.get('user') == 'nexus' and md.HASH_RE.fullmatch(request.get('password_hash', ''))
          and PW.encode() not in raw and 'hostname' in request, {k: v for k, v in request.items() if k != 'password_hash'})
    check('the launcher sees it: pending upgrade', md.pending(D) == 'upgrade')
    # ------------------------------------------------------------------ host: hostile and failure cases first
    with open(D, 'rb'):
        refused('refused while another program has the disk open', lambda: md.run(D, yes=True, cache=args.cache, skips=SKIP, request_required=True), 'in use')
    check('... nothing changed', md.pending(D) == 'upgrade' and not md.side(D, '-fat.img').exists() and not md.side(D, '.upgrading').exists())
    os.environ['NEXUS_UPGRADE_FREE_BYTES'] = str(1 << 20)
    try: refused('refused when the PC has too little free space', lambda: md.run(D, yes=True, cache=args.cache, skips=SKIP, request_required=True), 'not enough free space')
    finally: del os.environ['NEXUS_UPGRADE_FREE_BYTES']
    failed = json.loads(md.fat_read(D, md.UPDIR + '/failed.json') or b'{}')
    check('... the reason is left for the page, the request set aside (no retry at every start)', 'free space' in failed.get('reason', '') and
          md.fat_read(D, md.REQUEST) is None and md.fat_read(D, md.UPDIR + '/request.failed.json') == raw and md.pending(D) is None, failed)
    forged = json.dumps({**request, 'hostname': 'evil;reboot'}).encode()
    fat_edit(D, [(md.FAT_UPDIR + '/request.json', forged)], removes=[md.FAT_UPDIR + '/failed.json', md.FAT_UPDIR + '/request.failed.json'])
    refused('a forged request (a shell command as host name) is refused', lambda: md.run(D, yes=True, cache=args.cache, skips=SKIP, request_required=True), 'bad hostname')
    junk = base / 'junk.img'; junk.write_bytes(os.urandom(1 << 20))
    refused('a file that is not a NexusOS disk is refused', lambda: md.run(junk, yes=True), 'not a NexusOS'); junk.unlink()
    fat_edit(D, [(md.FAT_UPDIR + '/request.json', raw)], removes=[md.FAT_UPDIR + '/failed.json', md.FAT_UPDIR + '/request.failed.json'])   # the user schedules it again
    check('scheduled again: pending upgrade', md.pending(D) == 'upgrade')
    # ------------------------------------------------------------------ host: the upgrade
    before = sha256(D); t0 = time.time()
    try: result = md.run(D, yes=True, cache=args.cache, skips=SKIP + BIG, request_required=True); ok = True
    except md.Refused as error: result, ok = {}, False; print('refused:', error)
    check('the upgrade runs (%.0f s)' % (time.time() - t0), ok and md.kind_of(D) == 'ext4', result)
    check('every file copied and compared: 0 differences, 0 missing', ok and result.get('differences') == 0 and result.get('missing') == 0 and (result.get('files') or 0) > 4000, result)
    check('only the test\'s skipped folders were left out', sorted(result.get('skipped', [])) == sorted(SKIP + BIG), result.get('skipped'))
    check('the old disk is kept byte for byte (SHA-256)', md.side(D, '-fat.img').is_file() and sha256(md.side(D, '-fat.img')) == before)
    state = md.state_of(D); check('state: upgraded, with the report', state.get('state') == 'upgraded' and md.side(D, '.migration.json').is_file(), state.get('state'))
    fs = pkg_vm.ext4lib.Ext4(str(D))
    for path, key in EXPECT.items():
        data = pkg_vm.guest_read(fs, path)
        check('kept (case restored): %s' % path, data == FIXTURE[key], 'missing' if data is None else '%d bytes' % len(data))
    report = json.loads(pkg_vm.guest_read(fs, md.RESULT) or b'{}')
    check('the report is on the new disk for the page; the request is not', report.get('differences') == 0 and pkg_vm.guest_read(fs, md.REQUEST) is None, report)
    up = fs.read_inode(fs.lookup(md.UPDIR)); check('the upgrade folder belongs to the user (the page writes there)', (up.uid, up.mode & 0o777) == (1000, 0o755), (up.uid, oct(up.mode)))
    config = pkg_vm.guest_read(fs, '/var/lib/nexus/bootstrap/config') or b''
    check('the account gets the request\'s password hash at the first start', ("PASSWORD_HASH='%s'" % request.get('password_hash', '-')).encode() in config, len(config))
    fs.f.close()
    checked = pkg_vm.ext4lib.check_image(str(D)); errors = checked[0]; checked[2].fs.f.close(); del checked
    check('the new disk passes the ext4 consistency check', not errors, errors[:5])
    refused('a second run is refused (already the full system)', lambda: md.run(D, yes=True), 'already runs the full system')
    # ------------------------------------------------------------------ boot 2: the full system
    vm = boot(D, 'full')
    logged, detail = pkg_vm.greeter_login(vm, password=PW, seconds=900)   # the first start: no password yet (set at its end)
    check('boot2: the session starts', logged, detail)
    vm.wait(r'\[64\] UPGRADE STATE upgraded', 0, 600)
    m = vm.wait(r'\[nexusd\] nexus-firstboot\.service (?:finished|failed) \(result: ([\w-]+)\)', 0, 2400)
    check('boot2: the first start installs the Debian base (dpkg)', m.group(1) == 'success', m.group(0))
    check('boot2: the page showed the first start running', re.search(r'\[64\] UPGRADE STATE upgraded used=\d+ firstboot=1', vm.text()) is not None)
    mark = len(vm.text()); m = vm.wait(r'\[64\] UPGRADE STATE upgraded used=\d+ firstboot=0 packages=(\d+)', mark, 120)   # after the first start (the page may open before it begins)
    check('boot2: after it, the page counts the installed packages', int(m.group(1)) >= 300, m.group(0))
    sx, sy = page(vm); time.sleep(3); c = controls(vm, 0, latest=True); vm.shot('upgraded')
    mark = len(vm.text()); vm.click(sx + c['back'][0], sy + c['back'][1])
    vm.wait(r'\[64\] UPGRADE ROLLBACK requested', mark, 60); c = controls(vm, mark)
    check('boot2: "Go Back to the Old Disk" is asked for', True)
    click_off(vm, sx + c['shutdown'][0], sy + c['shutdown'][1]); code = vm.exited(600)
    check('boot2: shut down', code is not None, 'exit=%s' % code)
    check('boot2: no kernel fault', 'KERNEL_FAULT' not in vm.text(), re.findall(r'.*KERNEL_FAULT.*', vm.text())[:3])
    vm.close(keep_overlay=True); commit(vm); vm = None
    fs = pkg_vm.ext4lib.Ext4(str(D)); shadow = pkg_vm.guest_read(fs, '/etc/shadow') or b''; group = pkg_vm.guest_read(fs, '/etc/group') or b''; fs.f.close()
    check('boot2: the account has the password chosen on the page', ('nexus:%s:' % request.get('password_hash', '-')).encode() in shadow)
    groups = {line.split(':')[0]: line.split(':')[3].split(',') for line in group.decode('utf-8', 'replace').splitlines() if line.count(':') == 3}
    check('boot2: the old accounts got the Debian system groups, and the user the admin ones', 'mail' in groups and 'nexus' in groups.get('sudo', [])
          and 'nexus' in groups and 'nexus' in groups.get('lpadmin', []), {k: v for k, v in groups.items() if k in ('mail', 'sudo', 'lpadmin', 'nexus')})
    check('the launcher sees it: pending rollback', md.pending(D) == 'rollback')
    # ------------------------------------------------------------------ host: back, and finish
    check('the launcher\'s step goes back', md.launcher_step(D) and md.kind_of(D) == 'fat' and md.side(D, '-full.img').is_file() and md.state_of(D).get('state') == 'rolled-back')
    vol = fatput.Fat32(str(D), readonly=True); vol.close()
    kept_ok = all(md.fat_read(D, '/' + key) == FIXTURE[key] for key in FIXTURE if len(FIXTURE[key]) < (1 << 20))
    check('the FAT32 disk again: every test file, and no request (no new upgrade at the next start)', kept_ok and md.fat_read(D, md.REQUEST) is None and md.pending(D) is None)
    refused('rollback twice is refused', lambda: md.rollback(D, yes=True), 'nothing to go back to')
    md.finish(D, yes=True); check('finish deletes the full system kept beside it', not md.side(D, '-full.img').exists() and md.state_of(D).get('state') == 'finished')
    if not args.no_keep_cycle:
        fat_edit(D, [(md.FAT_UPDIR + '/request.json', raw)])
        try: md.run(D, yes=True, cache=args.cache, skips=SKIP + BIG, request_required=True); ok = True
        except md.Refused as error: ok = False; print('refused:', error)
        check('a second upgrade', ok and md.kind_of(D) == 'ext4')
        edge_cases()   # a failed first start, and a request still only in the ext4 journal (overlays: D is not changed)
        w = pkg_vm.ext4lib.Writer(str(D)); w.put_bytes(b'requested\n', md.UPDIR + '/finish-request', 0o644, 1000, 1000); w.close(backups=True)   # "Keep the Full System"
        check('"Keep": the launcher\'s step deletes the old disk', md.pending(D) == 'finish' and md.launcher_step(D) and not md.side(D, '-fat.img').exists()
              and md.kind_of(D) == 'ext4' and md.state_of(D).get('state') == 'finished' and md.pending(D) is None)
    finished = True
except (Exception, SystemExit) as error:   # noqa: BLE001
    import traceback; traceback.print_exc()
    check('test ran to the end', False, repr(error))
    if vm:
        try: vm.shot('failure')
        except Exception: pass
finally:
    if vm: vm.close()
    if not finished and not any(c['check'] == 'test ran to the end' for c in results['checks']): check('test ran to the end', False, 'stopped early')
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    for p in leftovers + [WORK / 'session.qcow2', WORK / 'edge-fail.qcow2', WORK / 'edge-session.qcow2']:
        try: p.unlink(missing_ok=True)
        except OSError: pass
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
