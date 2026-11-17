"""ext4 crash consistency: kill the VM during an fsync-heavy workload, recover, verify, repeat.

Usage: python -I tests/test_x64_ext4_crash.py [WORK] [--iterations 20] [--commit-crashes 1,2,3,5,8]
                                              [--accel tcg|whpx] [--linux] [--seed N]

Random kills (one image, state carried across iterations): /bin/crash64 appends checksummed records,
atomically replaces a generation file (write, fsync, rename, directory fsync), creates/unlinks churn files
and resizes a scratch file, printing "CRASH FSYNCED n" after each durable step. The host kills QEMU at a
random moment; then (1) ext4check replays the journal in memory and checks the whole volume, (2) with
--linux, Debian's e2fsck -fy replays the jbd2 journal itself (on a QEMU snapshot) and e2fsck -fn must then
be clean, (3) the next boot's kernel recovers the journal/orphans and the workload verifies that every
fsync'd record and generation survived ("CRASH VERIFY records>=n") with no stale bytes in unsynced files.
Commit crashes (a fresh image per N): the kernel powers off right after the N-th commit block is durable
and before checkpointing, so recovery must replay the journal; checked the same way.
The final boot of the random-kill image ends cleanly and the volume must check clean without replay.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import random
import re
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('native64-ext4-crash'))
ap.add_argument('--iterations', type=int, default=20)
ap.add_argument('--commit-crashes', default='1,2,3,5,8')
ap.add_argument('--accel', default='tcg', choices=['tcg', 'whpx'])
ap.add_argument('--linux', action='store_true')
ap.add_argument('--linux-every', type=int, default=1)
ap.add_argument('--seed', type=int, default=None)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
seed = args.seed if args.seed is not None else int(time.time())
rng = random.Random(seed)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


ext4lib = module('ext4lib', ROOT / 'userland/ext4lib.py')
lx = module('ext4_linux_validator', ROOT / 'tests/ext4_linux_validator.py') if args.linux else None
ACCEL = {'tcg': ['-cpu', 'max'], 'whpx': ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt']}
SOURCE = OUT / 'apps64-ext4.img'
assert SOURCE.exists(), 'run arch/x86_64/build.py first'
hostqos = nexus_paths   # unthrottle(pid): build.py's Windows power-throttling opt-out


def prepare(name, marker):
    disk = WORK / (name + '.img'); disk.unlink(missing_ok=True)
    shutil.copyfile(SOURCE, disk)
    fs = ext4lib.Writer(str(disk)); fs.put_bytes(marker.encode(), '/nexus/test-crash', 0o644); fs.close()
    return disk


def start(disk, tag):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    cmd = [QEMU, *ACCEL[args.accel], '-m', '128', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
           '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
           '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if hostqos:
        try: hostqos.unthrottle(p.pid)
        except Exception: pass
    return p, serial


def text(serial):
    return serial.read_text(encoding='latin1') if serial.exists() else ''


def run(disk, tag, kill_after=None, timeout=240):
    """Boot once. kill_after: seconds after 'CRASH WRITING' to kill the VM (None: let it finish)."""
    p, serial = start(disk, tag)
    deadline = time.time() + timeout
    killed = False
    while p.poll() is None:
        if time.time() > deadline:
            p.kill(); p.wait(); raise AssertionError('%s: timed out\n%s' % (tag, text(serial)[-3000:]))
        log = text(serial)
        if kill_after is not None and '[64] CRASH WRITING' in log:
            time.sleep(kill_after)
            if p.poll() is None:
                p.kill(); killed = True
            p.wait(); break
        time.sleep(0.05)
    log = text(serial)
    assert 'KERNEL_FAULT' not in log and '[64] FAIL' not in log and '[64] CRASH FAIL' not in log, (tag, log[-3000:])
    assert '[EXT4] ERROR' not in log, (tag, log[-3000:])
    verify = re.search(r'\[64\] CRASH VERIFY records=(\d+) generation=(\d+) churn=(\d+)', log)
    assert verify, (tag, log[-3000:])
    fsynced = [int(x) for x in re.findall(r'\[64\] CRASH FSYNCED (\d+)', log)]
    return {'rc': p.returncode, 'killed': killed, 'records': int(verify.group(1)), 'generation': int(verify.group(2)),
            'last_fsynced': max(fsynced) if fsynced else int(verify.group(1)), 'replayed': '[EXT4] JOURNAL REPLAY' in log,
            'orphans': '[EXT4] orphan inodes processed' in log, 'poweroff': 'TEST POWER-OFF after journal commit' in log, 'log': log}


def host_check(disk, tag):
    c = ext4lib.Checker(str(disk)); errors = c.run(); replayed = c.replayed; c.fs.close()
    assert not errors, (tag, errors[:20])
    note = 'ext4check clean (replayed %d journal blocks in memory)' % replayed
    if lx:
        out, _ = lx.run(str(disk), str(WORK / ('linux-' + tag)), lx.SCRIPTS['replay'], args.accel == 'whpx')
        rcs = lx.verdict(out)
        assert len(rcs) == 2 and rcs[0] in (0, 1) and rcs[1] == 0 and 'LXV DONE' in out, (tag, out[-4000:])
        recovered = 'recovering journal' in out
        note += '; Linux e2fsck -fy rc=%d%s then -fn clean' % (rcs[0], ' (replayed the journal)' if recovered else '')
    return replayed, note


summary = {'seed': seed, 'random': [], 'commit': []}
print('crash test seed', seed, 'accel', args.accel, flush=True)
# ---- random kills ----------------------------------------------------------------------------------
disk = prepare('random', '8')
expected_records = 0
for i in range(args.iterations):
    delay = rng.uniform(0.05, 4.0)
    r = run(disk, 'random-%02d' % i, kill_after=delay)
    assert r['records'] >= expected_records and r['generation'] >= expected_records, ('lost fsync\'d data', i, r['records'], r['generation'], expected_records)
    replayed, note = host_check(disk, 'random-%02d' % i)
    line = 'iteration %2d: kill %.2fs after start -> next boot saw records=%d gen=%d (needed >= %d); boot replay=%s orphans=%s; %s' % (
        i, delay, r['records'], r['generation'], expected_records, r['replayed'], r['orphans'], note)
    print(line, flush=True); summary['random'].append(line)
    expected_records = r['last_fsynced']
assert any('orphans=True' in line for line in summary['random']), 'no crash left an unlinked-but-open inode to recover'
final = run(disk, 'random-final')
assert final['rc'] == 33 and final['records'] >= expected_records and '[64] CRASH CAMPAIGN PASSED' in final['log'], final['log'][-3000:]
c = ext4lib.Checker(str(disk)); errors = c.run()
assert not errors and not c.replayed and not (c.fs.sb.incompat & ext4lib.INCOMPAT_RECOVER) and c.fs.sb.state & ext4lib.STATE_VALID, errors[:10]
c.fs.close()
print('final boot: records=%d (>= %d), clean shutdown, volume clean without replay' % (final['records'], expected_records), flush=True)
disk.unlink()
# ---- deterministic power-off right after a journal commit -----------------------------------------------
for n in [int(x) for x in args.commit_crashes.split(',') if x]:
    disk = prepare('commit-%d' % n, 'commit:%d' % n)
    expected = 0
    for round_ in range(2):
        r = run(disk, 'commit-%d-%d' % (n, round_))
        assert r['rc'] == 35 and r['poweroff'], (n, r['rc'], r['log'][-2000:])
        assert r['records'] >= expected, ('lost fsync\'d data after commit crash', n, r['records'], expected)
        replayed, note = host_check(disk, 'commit-%d-%d' % (n, round_))
        assert replayed, 'power-off after a commit must leave a journal to replay'
        line = 'commit crash N=%d round %d: VERIFY records=%d (>= %d), boot replay=%s; %s' % (n, round_, r['records'], expected, r['replayed'], note)
        print(line, flush=True); summary['commit'].append(line)
        expected = r['last_fsynced']
    disk.unlink()
print('PASS ext4 crash consistency: %d random kills + %d commit-boundary power-offs, seed %d' % (
    len(summary['random']), len(summary['commit']), seed), flush=True)
