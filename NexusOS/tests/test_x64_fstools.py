"""Filesystems and block devices with Debian's own tools on native NexusOS: loop devices, exFAT, vfat, ISO 9660.

    python -I tests/test_x64_fstools.py WORK [--cases basic,smp1] [--accel tcg|whpx] [--keep-base]

The root is an ext4 disk built by arch/x86_64/storage_tools.py image (apps64 + the packages pinned in
arch/x86_64/storage-lock.json: util-linux, mount, fdisk, exfatprogs, dosfstools, xorriso, ...), cached in
WORK/tools-base.img for the lock's hash. /sbin/init is arch/x86_64/fstools_user.c (root through a set-user-ID
copy), which runs tests/storage_scripts/fs_basic.sh with dash and then fuzzes the exFAT and ISO images the script
made (damaged copies through LOOP_CONFIGURE, mounted, walked, written, unmounted).
  basic  2 CPUs: fs_basic.sh ("[64] FSTEST ALL PASSED") + exFAT fuzz (80 rounds) + ISO fuzz (60 rounds)
  fuse   2 CPUs: fs_fuse.sh (sshfs, fuse2fs, ntfs-3g fuse/fuseblk, fusermount3 user mount, daemon death)
  crypt  2 CPUs: fs_crypt.sh (dmsetup linear/zero/error, dm-crypt IEEE 1619 answers, keyctl, cryptsetup LUKS2/LUKS1/plain)
  cryptsw the same with the boot option noaesni: dm-crypt on the constant-time software AES
  smp1   the three scripts and a shorter exFAT fuzz on one CPU
Fails on a kernel fault or a failed check; user-mode faults of the tools are reported.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='basic,fuse,crypt,cryptsw,smp1')
ap.add_argument('--accel', default='whpx', choices=('tcg', 'whpx'))
ap.add_argument('--keep-base', action='store_true', help='keep WORK/tools-base.img for the next run')
ap.add_argument('--cmdline', default='', help='extra kernel options (fw_cfg), e.g. fuse.trace')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


tools = module('fstools_storage_tools', ROOT / 'arch/x86_64/storage_tools.py')
build = module('fstools_build', ROOT / 'arch/x86_64/build.py')
ext4lib = module('fstools_ext4lib', ROOT / 'userland/ext4lib.py')
MODES = {'basic': b'script /nexus/fs-basic.sh\nfuzz exfat /tmp/ex.img 80\nfuzz iso9660 /tmp/test.iso 60\n',
         'fuse': b'script /nexus/fs-fuse.sh\n',
         'crypt': b'script /nexus/fs-crypt.sh\n',
         'cryptsw': b'script /nexus/fs-crypt.sh\n',
         'smp1': b'script /nexus/fs-basic.sh\nscript /nexus/fs-fuse.sh\nscript /nexus/fs-crypt.sh\nfuzz exfat /tmp/ex.img 30\n'}


def sparse_copy(source, destination):
    """Copy a disk image leaving all-zero MiB unallocated (the host disk must not fill up)."""
    destination = Path(destination); destination.unlink(missing_ok=True); destination.touch()
    subprocess.run(['fsutil', 'sparse', 'setflag', str(destination)], check=True, stdout=subprocess.DEVNULL)
    with open(source, 'rb') as a, destination.open('r+b') as b:
        b.truncate(Path(source).stat().st_size)
        while block := a.read(1 << 20):
            if block.strip(bytes(1)): b.write(block)
            else: b.seek(len(block), 1)


def base_image():
    lock = (ROOT / 'arch/x86_64/storage-lock.json').read_bytes()
    stamp = hashlib.sha256(lock + (ROOT / 'arch/x86_64/storage_tools.py').read_bytes() + (OUT / 'apps64.img').read_bytes()[:1 << 20]).hexdigest()[:16]
    base, marker = WORK / 'tools-base.img', WORK / 'tools-base.stamp'
    if base.exists() and marker.exists() and marker.read_text() == stamp: return base
    base.unlink(missing_ok=True)
    dense = WORK / 'tools-base.dense'
    tools.image(dense, tools.CACHE, 640, [])
    sparse_copy(dense, base); dense.unlink()
    marker.write_text(stamp); return base


def prepare(disk, case):
    elf = WORK / 'fstools.elf'
    build.run(build.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
              ROOT / 'arch/x86_64/fstools_user.c', '-o', elf)
    sparse_copy(base_image(), disk)
    fs = ext4lib.Writer(str(disk)); data = elf.read_bytes()
    try:
        fs.put_bytes(data, 'sbin/init', 0o755, 0, 0)
        fs.put_bytes(data, 'bin/fstools-root', 0o4755, 0, 0)
        fs.put_bytes((ROOT / 'tests/storage_scripts/fs_basic.sh').read_bytes(), 'nexus/fs-basic.sh', 0o644, 0, 0)
        fs.put_bytes((ROOT / 'tests/storage_scripts/fs_fuse.sh').read_bytes(), 'nexus/fs-fuse.sh', 0o644, 0, 0)
        fs.put_bytes((ROOT / 'tests/storage_scripts/fs_crypt.sh').read_bytes(), 'nexus/fs-crypt.sh', 0o644, 0, 0)
        fs.put_bytes(MODES[case], 'nexus/fstools-mode', 0o644, 0, 0)
        fs.ensure_dir(['srv'], 0o755, 0, 0)
        fs.ensure_dir(['mnt'], 0o755, 0, 0); fs.ensure_dir(['root'], 0o700, 0, 0)
        fs.put_bytes(b'root:x:0:0:root:/root:/bin/sh\nnexus:x:1000:1000:NexusOS:/home/nexus:/bin/sh\n', 'etc/passwd', 0o644, 0, 0)   # sftp-server needs its user
        fs.put_bytes(b'root:x:0:\ndisk:x:6:\nnexus:x:1000:\n', 'etc/group', 0o644, 0, 0)
    finally:
        fs.close()


CMDLINE = {'cryptsw': 'noaesni'}   # per-case kernel options
AES_LINE = {'crypt': 'AES-NI matches the constant-time software path', 'cryptsw': 'constant-time software AES (no AES-NI)'}


def boot(tag, disk, smp):
    cmdline = ' '.join(filter(None, [args.cmdline, CMDLINE.get(tag, '')]))
    serial = WORK / f'{tag}.log'; serial.unlink(missing_ok=True)
    cpu = ['-accel', 'tcg', '-cpu', 'max'] if args.accel == 'tcg' else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
    command = [QEMU, '-machine', 'q35', *cpu, '-smp', str(smp), '-m', '2048', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
               '-drive', f'if=none,id=root,format=raw,file={disk}', '-device', 'nvme,serial=nexus-root,drive=root',
               '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot',
               *(['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}'] if cmdline else [])]
    (WORK / f'{tag}.cmd').write_text(' '.join(map(str, command)))
    started = time.time()
    try: rc = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, timeout=900).returncode
    except subprocess.TimeoutExpired: rc = 'timeout'
    log = serial.read_bytes().decode('utf-8', 'replace')
    return rc, log, time.time() - started


failures = []
for case in [c for c in args.cases.split(',') if c]:
    disk = WORK / f'{case}.img'
    try:
        prepare(disk, case)
        rc, log, seconds = boot(case, disk, 1 if case == 'smp1' else 2)
        passes = re.findall(r'\[64\] FSTEST PASS ([^\n]*)', log)
        fuzz = re.findall(r'\[64\] FSTOOLS FUZZ PASSED ([^\n]*)', log)
        user_faults = len(re.findall(r'USER_FAULT', log))
        wanted_fuzz = MODES[case].count(b'fuzz '); wanted_scripts = MODES[case].count(b'script ')
        ok = rc == 33 and log.count('[64] FSTEST ALL PASSED') == wanted_scripts and len(fuzz) == wanted_fuzz and '[64] FSTOOLS ALL PASSED' in log and \
            'KERNEL_FAULT' not in log and '[64] FAIL:' not in log and \
            (case not in AES_LINE or ('[64] PASS AES/XTS known answers' in log and AES_LINE[case] in log))
        for line in passes: print('  ' + line[:150])
        for line in fuzz: print('  fuzz ' + line)
        for line in re.findall(r'\[64\] FSTEST INFO ([^\n]*)', log): print('  info ' + line[:200])
        if case == 'crypt' and ok:   # AES-NI: dm-crypt over a zero target must run at hundreds of MB/s at least
            speeds = []
            for value, unit in re.findall(r'INFO dm-crypt [^\n]*copied, [0-9.,]+ s, ([0-9.,]+) ([kMG]?B)/s', log):
                speeds.append(float(value.replace(',', '.')) * {'B': 1e-6, 'kB': 1e-3, 'MB': 1.0, 'GB': 1000.0}[unit])
            ok = len(speeds) == 2 and min(speeds) >= 100
            print(f'  dm-crypt throughput MB/s: {speeds} (need >= 100 each)')
        print(('PASS ' if ok else 'FAIL ') + f'{case} ({seconds:.0f} s, user-mode faults {user_faults})', flush=True)
        if not ok:
            failures.append(case)
            tail = [l for l in log.splitlines() if 'FSTEST' in l or 'FSTOOLS' in l or 'FAULT' in l or 'FAIL' in l][-20:]
            print('\n'.join(tail) or log[-3000:])
    finally:
        disk.unlink(missing_ok=True)
if not args.keep_base:
    (WORK / 'tools-base.img').unlink(missing_ok=True); (WORK / 'tools-base.stamp').unlink(missing_ok=True)
print(('FAIL' if failures else 'PASS') + ' fstools: ' + (', '.join(failures) if failures else 'all cases'), flush=True)
sys.exit(1 if failures else 0)
