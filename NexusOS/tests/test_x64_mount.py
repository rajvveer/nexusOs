"""mount(2) and the vfat driver on USB media: privileged and desktop-user mounts, file operations, persistence,
fsck-clean results, corrupted images (in-guest fuzzing) and surprise removal of a mounted stick.

python -I tests/test_x64_mount.py <work> [--cases root,fuzz,removal,fatroot,smp1] [--accel tcg|whpx] [--qmp-port 4651]

  root     ext4 root (NVMe); /sbin/init = mount64 (arch/x86_64/mount_user.c), /bin/mount64-root set-user-ID root:
           privileged vfat campaign then the desktop-user /media/nexus mount; the host checks the FAT16 volume
           afterwards (no lost/cross-linked clusters, clean bit set, the files the guest wrote)
  fuzz     120 mutated FAT12 images written to a second stick and mounted/walked/written in the guest
  removal  QMP device_del of a stick while the guest reads and writes a file on it
  fatroot  the user phase on the FAT32 desktop-style root (no root user at all)
  smp1     the root campaign on one CPU
"""
from pathlib import Path
import argparse, importlib.util, json, shutil, socket, struct, subprocess, sys, time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
ALL = ('root', 'fuzz', 'removal', 'fatroot', 'smp1')
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('mount'))
ap.add_argument('--cases', default=','.join(ALL))
ap.add_argument('--accel', default='tcg', choices=['tcg', 'whpx'])
ap.add_argument('--qmp-port', type=int, default=4651)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
ext4lib = module('mount_ext4lib', ROOT / 'userland/ext4lib.py')
fatimage = module('mount_fatimage', ROOT / 'tests/fatimage.py')
fatput = module('mount_fatput', ROOT / 'userland/fatput.py')
usbtest = None


def pat(i, seed):
    return (i * 131 + seed * 17 + (i >> 11)) & 255


def pattern(length, seed):
    return bytes(pat(i, seed) for i in range(length))


def mbr_disk(path, size, partitions):
    """partitions: [(type, start_lba, data bytes or sector count)]"""
    with open(path, 'wb') as f:
        f.truncate(size)
        mbr = bytearray(512)
        for i, (kind, start, payload) in enumerate(partitions):
            count = payload if isinstance(payload, int) else len(payload) // 512
            struct.pack_into('<B3sB3sII', mbr, 446 + 16 * i, 0, b'\x00\x02\x00', kind, b'\xff\xff\xff', start, count)
            if not isinstance(payload, int):
                f.seek(start * 512); f.write(payload)
        mbr[440:444] = b'\x4e\x58\x55\x53'
        mbr[510:512] = b'\x55\xaa'
        f.seek(0); f.write(mbr)


def stick_a(path):
    img = fatimage.FatImage.create(16 * 1024 * 1024, 16, 'NEXUSSTICK')
    img.put('HELLO.TXT', b'hello, stick\n')
    img.put('SUB/DATA.BIN', pattern(300000, 1))
    mbr_disk(path, 64 * 1024 * 1024, [(0x0E, 2048, img.finish()), (0x83, 2048 + 32768, 64 * 1024 * 1024 // 512 - 2048 - 32768)])


def stick_b(path):
    img = fatimage.FatImage.create(2 * 1024 * 1024, 12, 'FUZZ')
    img.put('README.TXT', b'fuzz template\n')
    for i in range(12):
        img.put(f'dir{i % 3}/A long file name number {i}.dat', pattern(1000 + i * 777, i))
    raw = img.finish()
    mbr_disk(path, 16 * 1024 * 1024, [(0x01, 2048, 4096), (0x01, 6144, raw)])


def build_program():
    elf = WORK / 'mount64.elf'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'arch/x86_64/mount_user.c'), '-o', str(elf)], check=True)
    return elf


def ext4_root(path, program, mode):
    shutil.copyfile(OUT / 'apps64-ext4.img', path)
    fs = ext4lib.Writer(str(path)); data = Path(program).read_bytes()
    fs.put_bytes(data, 'sbin/init', 0o755, 0, 0)
    fs.put_bytes(data, 'bin/mount64-root', 0o4755, 0, 0)
    fs.put_bytes(mode.encode(), 'nexus/mount-mode', 0o644, 0, 0)
    fs.ensure_dir(['media', 'nexus'], 0o755, 0, 0)
    fs.close()


def fat_root(path, program, mode):
    shutil.copyfile(OUT / 'apps64.img', path)
    flag = WORK / 'mode.txt'; flag.write_bytes(mode.encode())
    assert fatput.edit(['fatput', str(path), 'put', str(program), 'sbin/init', 'put', str(flag), 'nexus/mount-mode']) == 0   # no /media: the kernel creates /media/nexus/<name> (as on desktop64)


class Qmp:
    def __init__(self, port, proc):
        for _ in range(300):
            try: self.socket = socket.create_connection(('127.0.0.1', port), timeout=120); break
            except OSError:
                if proc.poll() is not None: raise AssertionError('QEMU exited before QMP connected')
                time.sleep(.1)
        else: raise TimeoutError('QMP')
        self.file = self.socket.makefile('rwb'); self.file.readline(); self.command('qmp_capabilities')
    def command(self, name, arguments=None):
        self.file.write(json.dumps({'execute': name, **({'arguments': arguments} if arguments else {})}).encode() + b'\n'); self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if 'event' not in reply:
                assert 'error' not in reply, reply
                return reply.get('return')
    def close(self): self.socket.close()


def wait_serial(proc, serial, marker, seconds=300):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        text = serial.read_text(errors='replace') if serial.exists() else ''
        if marker in text: return text
        if '[64] MOUNTTEST FAIL' in text or 'KERNEL_FAULT' in text: raise AssertionError(text[-4000:])
        if proc.poll() is not None: raise AssertionError(text[-4000:])
        time.sleep(.05)
    raise TimeoutError(marker)


def boot(tag, root, sticks, smp=2, remove=None, timeout=900):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    cpu = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt'] if args.accel == 'whpx' else ['-cpu', 'max']
    usb = ['-device', 'qemu-xhci,id=xhci']
    for i, s in enumerate(sticks):
        usb += ['-drive', f'if=none,id=stick{i},file={s},format=raw', '-device', f'usb-storage,id=usb{i},bus=xhci.0,drive=stick{i},removable=on']
    cmd = [QEMU, '-machine', 'q35', *cpu, '-smp', str(smp), '-m', '512',
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
           '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
           '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
           '-drive', f'if=none,id=root,file={root},format=raw', '-device', 'nvme,serial=root,drive=root', *usb,
           '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off']
    (WORK / (tag + '.cmd')).write_text(' '.join(map(str, cmd)))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        if remove:
            wait_serial(proc, serial, '[64] MOUNT WAITING REMOVE')
            time.sleep(1.5)
            qmp = Qmp(args.qmp_port, proc); qmp.command('device_del', {'id': remove}); qmp.close()
        out = proc.communicate(timeout=timeout)[0].decode(errors='replace')
    except BaseException:
        proc.kill(); proc.communicate(); raise
    return proc.returncode, serial.read_text(errors='replace'), out


def base_checks(tag, rc, log):
    assert 'KERNEL_FAULT' not in log and '[64] MOUNTTEST FAIL' not in log, (tag, log[-4000:])
    assert rc == 33 and '[64] MOUNT ALL PASSED' in log, (tag, rc, log[-4000:])


def partition(path, start, sectors):
    with open(path, 'rb') as f:
        f.seek(start * 512); return f.read(sectors * 512)


results = {}
program = build_program()
for case in cases:
    root, a, b = WORK / f'{case}-root.img', WORK / f'{case}-a.img', WORK / f'{case}-b.img'
    try:
        stick_a(a); stick_b(b)
        if case in ('root', 'smp1'):
            ext4_root(root, program, 'root')
            rc, log, out = boot(case, root, [a, b], smp=1 if case == 'smp1' else 2)
            base_checks(case, rc, log)
            assert '[64] MOUNT ROOT PASSED' in log and '[64] MOUNT USER PASSED' in log, log[-3000:]
            vol = fatimage.FatVolume(partition(a, 2048, 32768))
            errors = vol.check(); assert not errors, errors
            assert vol.read('/Long File Name with spaces and été.txt') == pattern(100000, 2)
            assert vol.read('/map.bin') == pattern(65536, 6)
            assert vol.read('/after-remount.txt') == b'rw again\n' and vol.read('/from-user.txt') == b'desktop\n'
            assert vol.read('/SUB/DATA.BIN') == pattern(300000, 1) and vol.find('/orphan.bin') is None and vol.find('/STICK') is None
            assert vol.read('/big.bin')[:1 << 20] == pattern(5 << 20, 5)[:1 << 20] and vol.read('/big.bin')[1 << 20:] == bytes(2 << 20)
        elif case == 'fuzz':
            ext4_root(root, program, 'fuzz')
            rc, log, out = boot(case, root, [a, b])
            base_checks(case, rc, log)
            assert '[64] MOUNT FUZZ PASSED rounds=120' in log, log[-3000:]
        elif case == 'removal':
            ext4_root(root, program, 'removal')
            rc, log, out = boot(case, root, [a, b], remove='usb1')
            base_checks(case, rc, log)
            assert '[64] MOUNT REMOVAL PASSED' in log and '[64] BLOCK sdb removed' in log, log[-3000:]
        elif case == 'fatroot':
            fat_root(root, program, 'user')
            rc, log, out = boot(case, root, [a, b])
            base_checks(case, rc, log)
            assert '[64] MOUNT USER PASSED' in log, log[-3000:]
            vol = fatimage.FatVolume(partition(a, 2048, 32768)); errors = vol.check(); assert not errors, errors
            assert vol.read('/from-user.txt') == b'desktop\n'
        results[case] = 'PASS'; print(f'PASS {case}', flush=True)
    finally:
        for p in (root, a, b): p.unlink(missing_ok=True)
(WORK / 'results.json').write_text(json.dumps(results, indent=1) + '\n')
print('PASS mount/vfat: ' + ', '.join(results), flush=True)
