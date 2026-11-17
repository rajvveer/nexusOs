"""ext4 root filesystem: every on-disk kernel campaign on out/apps64-ext4.img, under TCG and WHPX.

Usage: python -I tests/test_x64_ext4.py [WORK] [--accel tcg|whpx|both] [--linux] [--no-scripts]

  disk      the default campaign (ELF loader, glibc, IPC, threads, fs, shmem, limits, locks, statfs, unix...);
            the read-only campaign must leave the image byte-identical (no superblock touch on mount)
  writes    /nexus/test-writes + /nexus/test-posix, booted twice: mutation APIs, POSIX permissions as an
            unprivileged user and as root after set-user-ID execve, links, symlinks, rename, holes, a 5 GiB
            sparse file, statx/utimensat, fallocate, mknod, fsync; second boot checks persistence. The image
            must be clean (ext4check --expect-clean) after each boot and its contents are verified on the host.
  linuxmade (--linux) Debian's mke2fs formats a volume with Linux defaults and the Linux kernel copies the apps
            tree into it, adds a 1500-entry htree and fast/slow symlinks; NexusOS then runs the writes campaign
            on it twice (plus lookups/unlinks/insertions in the Linux-built index) and e2fsck -fn must be clean
  readonly  a volume in the 'errors' state mounts read-only; data reads work, every mutation gets EROFS
  scripts   test_x64_network/display/audio/writeback_errors.py with NEXUS_ROOT_FS=ext4
--linux additionally runs Debian's e2fsck (tests/ext4_linux_validator.py) on the image written by the kernel.
Image copies are deleted as soon as each campaign's result is recorded.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('native64-ext4'))
ap.add_argument('--accel', default='both', choices=['tcg', 'whpx', 'both'])
ap.add_argument('--linux', action='store_true')
ap.add_argument('--no-scripts', action='store_true')
ap.add_argument('--only', choices=['disk', 'writes', 'linuxmade', 'readonly', 'scripts'])
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


ext4lib = module('ext4lib', ROOT / 'userland/ext4lib.py')
SOURCE = OUT / 'apps64-ext4.img'
assert SOURCE.exists(), 'run arch/x86_64/build.py first (it builds apps64-ext4.img)'
source_hash = hashlib.sha256(SOURCE.read_bytes()).digest()
ACCEL = {'tcg': ['-cpu', 'max'], 'whpx': ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt']}
results = []


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).digest()


def boot(disk, accel, tag, timeout=240, memory=128, extra=()):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    cmd = [QEMU, *ACCEL[accel], '-m', str(memory), '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
           '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
           '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', *extra]
    t = time.time()
    p = subprocess.run(cmd, timeout=timeout, capture_output=True)
    log = serial.read_text(encoding='latin1')
    return p.returncode, log, time.time() - t


def copy(tag):
    disk = WORK / (tag + '.img'); disk.unlink(missing_ok=True)
    shutil.copyfile(SOURCE, disk); return disk


def markers(disk, files):
    fs = ext4lib.Writer(str(disk))
    for path, data, uid in files:
        fs.ensure_dir(fs.split(path)[:-1], 0o755, uid, uid)
        fs.put_bytes(data, path, 0o644, uid, uid)
    fs.close()


def check_image(disk, clean=True):
    c = ext4lib.Checker(str(disk)); errors = c.run()
    if clean:
        if c.replayed: errors.append('journal needed recovery after a clean shutdown')
        if c.fs.sb.incompat & ext4lib.INCOMPAT_RECOVER: errors.append('needs_recovery left set')
        if not c.fs.sb.state & ext4lib.STATE_VALID: errors.append('volume not marked clean')
    c.fs.close()
    assert not errors, errors[:20]
    return c.stats


def common(log, tag):
    assert '[64] ALL TESTS PASSED' in log and '[64] PASS native ATA/ext4 root mount' in log, (tag, log[-3000:])
    assert '[64] FAIL' not in log and 'KERNEL_FAULT' not in log, (tag, log[-3000:])
    assert 'FILE_WRITEBACK failed' not in log and 'FILE_CACHE read failed' not in log and 'PAGE_IO' not in log, (tag, log[-3000:])
    assert '[EXT4] ERROR' not in log, (tag, log[-3000:])


DISK_MARKERS = ['[64] DISK TESTS PASSED', '[64] DISK USER PASS:', '[64] FILE API PASS:', '[64] DYNAMIC sparse 1 TiB reservation PASS',
                'file mmap past EOF terminates with SIGBUS', '[64] SIGNAL ALL PASSED:', '[64] IPC ALL PASSED:', '[64] PROCESS ALL PASSED:',
                '[64] EXEC IMAGE ISOLATION PASSED:', '[64] THREAD ALL PASSED:', '[64] PIPE ALL PASSED:', '[64] EVENT ALL PASSED:',
                '[64] PID ALL PASSED:', '[64] FS ALL PASSED:', '[64] SHMEM ALL PASSED:', '[64] LIMIT ALL PASSED:',
                '[64] FILEMAP READONLY PASSED:', '[64] JIT ALL PASSED:', '[64] LOCK ALL PASSED:', '[64] STATFS API PASSED:',
                '[64] NAMESPACE ALL PASSED:', '[64] DEVICE PASS:', '[64] UNIX ALL PASSED:']
WRITE_MARKERS = ['[64] DIRECTORY API PASSED', '[64] FILE WRITE API PASSED', '[64] UNIX NAMED PASSED:', '[64] FS MUTATIONS PASSED:',
                 '[64] FILEMAP WRITE PASSED:', '[64] FD EXEC PASSED:', '[64] RENAME ALL PASSED', '[64] STATFS COUNTS PASSED:',
                 '[64] POSIX PERMISSIONS PASSED', '[64] POSIX LINKS PASSED', '[64] POSIX SYMLINKS PASSED', '[64] POSIX RENAME PASSED',
                 '[64] POSIX HOLES PASSED', '[64] POSIX 5GiB SPARSE PASSED', '[64] POSIX STATX/TIMES PASSED', '[64] POSIX USER ALL PASSED',
                 '[64] POSIX ROOT PASSED', '[64] POSIX ROOT ALL PASSED', '[64] FSFIX EXT4 PASSED', '[64] POSIX PTRACE PASSED',
                 '[64] FSFIX XATTR PASSED']


def campaign_disk(accel):
    disk = copy('disk-' + accel)
    before = digest(disk)
    rc, log, sec = boot(disk, accel, 'disk-' + accel)
    assert rc == 33, (rc, log[-3000:])
    common(log, 'disk')
    for m in DISK_MARKERS: assert m in log, (m, log[-3000:])
    assert log.count('[64] DYNAMIC ALL PASSED') == 3, 'dynamic campaign count'
    assert digest(disk) == before, 'the read-only campaign modified the ext4 image'
    disk.unlink()
    return 'disk campaign %.0fs, image byte-identical' % sec


def verify_writes(disk):
    fs = ext4lib.Ext4(str(disk))
    try:
        def node(path):
            ino = fs.lookup(path); assert ino, path
            return ino, fs.read_inode(ino)
        ino, data = node('/home/nexus/native64-persist/data.bin')
        expected = bytes(90 if i == 1 else ((i * 19 + 7) & 255) if i < 4099 else 0 for i in range(8193))
        assert fs.read_file(data) == expected and data.uid == 1000 and data.mode & 0o777 == 0o600
        _, mapped = node('/home/nexus/native64-persist/mapped.bin')
        want = bytearray(8193)
        for offset, value in ((0, 65), (1, 68), (2, 69), (3, 72), (4096, 73), (8192, 74)): want[offset] = value
        assert fs.read_file(mapped) == bytes(want)
        _, sock = node('/home/nexus/native64-persist/stale.sock'); assert sock.mode & 0o170000 == ext4lib.S_IFSOCK
        _, link = node('/home/nexus/native64-persist/data-link')
        assert link.mode & 0o170000 == ext4lib.S_IFLNK and fs.read_file(link) == b'data.bin'
        for gone in ('/home/nexus/native64-dir', '/home/nexus/filemap-exec', '/tmp/native64-unix', '/tmp/native64-fs',
                     '/tmp/native64-locks', '/tmp/native64-space', '/tmp/native64-rename', '/etc/forbidden', '/bin/forbidden',
                     '/tmp/posix64', '/etc/posix64-root', '/etc/posix64-forbidden'):
            assert not fs.lookup(gone), gone + ' left behind'
        _, persist = node('/home/nexus/posix-persist/data')
        assert fs.read_file(persist) == b'persisted ext4 data' and persist.mode & 0o7777 == 0o640 and persist.uid == 1000 and persist.links == 2
        assert persist.get_time('mtime') == 1300000000 * 10**9 + 777777777
        _, sparse = node('/home/nexus/posix-persist/sparse')
        assert sparse.size == 4 * 2**30 + 10 and sparse.blocks == 8, (sparse.size, sparse.blocks)
        dino, index = node('/home/nexus/posix-persist/index')
        assert index.flags & ext4lib.INDEX_FL, 'large directory was not indexed'
        assert len([n for n, _, _ in fs.dir_entries(dino) if n not in (b'.', b'..')]) == 600
    finally:
        fs.close()


def campaign_writes(accel):
    disk = copy('writes-' + accel)
    markers(disk, [('/nexus/test-writes', b'1', 0), ('/nexus/test-posix', b'1', 0)])
    notes = []
    for round_ in range(2):
        rc, log, sec = boot(disk, accel, 'writes-%s-%d' % (accel, round_), timeout=400)
        assert rc == 33, (rc, log[-4000:])
        common(log, 'writes')
        for m in WRITE_MARKERS: assert m in log, (m, log[-4000:])
        assert log.count('[64] LOCK ALL PASSED:') == 2
        if round_:
            for m in ('[64] PERSISTED FILE READ PASSED', '[64] FILEMAP persisted bytes survived reboot PASS',
                      '[64] UNIX persistent socket type survived reboot PASS', '[64] UNIX persistent symlink survived reboot PASS',
                      '[64] POSIX PERSISTED PASSED'):
                assert m in log, (m, log[-4000:])
        stats = check_image(disk)
        verify_writes(disk)
        notes.append('boot %d %.0fs clean (%d inodes)' % (round_ + 1, sec, stats['inodes']))
    if args.linux:
        lx = module('ext4_linux_validator', ROOT / 'tests/ext4_linux_validator.py')
        text, _ = lx.run(str(disk), str(WORK / ('linux-' + accel)), lx.SCRIPTS['check'], accel == 'whpx')
        rcs = lx.verdict(text)
        assert rcs == [0] and 'LXV DONE' in text, text[-4000:]
        notes.append('Linux e2fsck -fn clean')
    disk.unlink()
    return 'writes+posix: ' + ', '.join(notes)


def campaign_linuxmade(accel):
    lx = module('ext4_linux_validator', ROOT / 'tests/ext4_linux_validator.py')
    disk = copy('linuxmade-' + accel)
    text, sec = lx.run(str(SOURCE), str(WORK / ('linuxmade-' + accel)), lx.SCRIPTS['linuxmade'], accel == 'whpx', second=str(disk))
    for m in ('LXV COPIED', 'LXV REMOUNTED', 'LXV LINUX MADE', 'LXV DONE'): assert m in text, (m, text[-4000:])
    assert lx.verdict(text) == [0], text[-4000:]
    features = re.search(r'Filesystem features:\s*(.*)', text).group(1).strip()
    inode_size = re.search(r'Inode size:\s*(\d+)', text).group(1)
    notes = ['made by Linux in %.0fs (features: %s; inode size %s)' % (sec, features, inode_size)]
    for round_ in range(2):
        rc, log, sec = boot(disk, accel, 'linuxmade-%s-%d' % (accel, round_), timeout=400)
        assert rc == 33, (rc, log[-4000:])
        common(log, 'linuxmade')
        for m in WRITE_MARKERS: assert m in log, (m, log[-4000:])
        assert ('[64] POSIX LINUX INDEX VERIFIED' if round_ else '[64] POSIX LINUX INDEX PASSED') in log, log[-4000:]
        assert not round_ or '[64] POSIX PERSISTED PASSED' in log, log[-4000:]
        stats = check_image(disk)
        notes.append('NexusOS boot %d %.0fs clean (%d inodes)' % (round_ + 1, sec, stats['inodes']))
    text, _ = lx.run(str(disk), str(WORK / ('linuxmade-check-' + accel)), lx.SCRIPTS['check'], accel == 'whpx')
    assert lx.verdict(text) == [0] and 'LXV DONE' in text, text[-4000:]
    notes.append('Linux e2fsck -fn clean')
    disk.unlink()
    return 'Linux-made volume: ' + ', '.join(notes)


def campaign_readonly(accel):
    disk = copy('readonly-' + accel)
    markers(disk, [('/nexus/test-readonly', b'1', 0), ('/home/nexus/readonly.dat', b'1', 1000)])
    fs = ext4lib.Writer(str(disk)); fs.sb.state = fs.sb.state | ext4lib.STATE_ERROR; fs.close()
    before = digest(disk)
    rc, log, sec = boot(disk, accel, 'readonly-' + accel)
    assert rc == 33 and '[64] READONLY CAMPAIGN PASSED' in log and '[64] STATFS READONLY PASSED' in log, (rc, log[-3000:])
    assert 'volume has errors: mounted read-only' in log and 'KERNEL_FAULT' not in log and '[64] FAIL' not in log
    assert digest(disk) == before, 'a read-only ext4 volume was modified'
    disk.unlink()
    return 'error-state volume mounted read-only, EROFS for every mutation, image unchanged'


def scripts(accel):
    env = dict(os.environ, NEXUS_ROOT_FS='ext4', NEXUS_ACCEL=accel)
    done = []
    for script, extra in (('test_x64_network.py', []), ('test_x64_display.py', []),
                          ('test_x64_audio.py', ['--qmp-port', '4552'] + (['--tcg'] if accel == 'tcg' else [])),
                          ('test_x64_writeback_errors.py', ['--accel', accel])):
        work = WORK / ('%s-%s' % (script[:-3], accel))
        if work.exists(): shutil.rmtree(work)
        p = subprocess.run([sys.executable, '-I', str(ROOT / 'tests' / script), str(work), *extra], env=env,
                           capture_output=True, text=True, timeout=900)
        for leftover in work.glob('*.img'): leftover.unlink()
        for leftover in work.glob('*.ppm'): leftover.unlink()
        if 'SKIP:' in p.stdout + p.stderr: done.append(script + ' skipped'); continue
        assert p.returncode == 0 and 'PASS' in p.stdout, (script, p.stdout[-3000:], p.stderr[-3000:])
        done.append(script.replace('test_x64_', '').replace('.py', ''))
    return 'campaign scripts on ext4: ' + ', '.join(done)


accels = ['tcg', 'whpx'] if args.accel == 'both' else [args.accel]
for accel in accels:
    for name, fn in (('disk', campaign_disk), ('writes', campaign_writes), ('linuxmade', campaign_linuxmade),
                     ('readonly', campaign_readonly), ('scripts', scripts)):
        if args.only and name != args.only: continue
        if name == 'linuxmade' and not args.linux: continue
        if name == 'scripts' and args.no_scripts: continue
        line = 'PASS ext4 %s [%s]: %s' % (name, accel, fn(accel))
        results.append(line); print(line, flush=True)
assert digest(SOURCE) == source_hash, 'the source image changed'
print('PASS ext4 root campaigns: %d checks under %s' % (len(results), '+'.join(accels)), flush=True)
