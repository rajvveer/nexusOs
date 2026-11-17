"""USB mass storage: Bulk-Only and UAS transports, SCSI disks, /dev + /sys + uevents, hot-plug and surprise removal.

python -I tests/test_x64_usbstorage.py <work> [--cases bot,uas,uas-hs,bot-4k,multilun,smp1] [--accel tcg|whpx] [--qmp-port 4650]

Root: out/apps64-ext4.img (NVMe) with /sbin/init = storage64 (arch/x86_64/usbstor_user.c) and /bin/storage64-root
(set-user-ID root). The stick is a 64 MiB MBR disk (p1 0x0C empty, p2 0x83 filled with a position pattern).
  bot       usb-storage at SuperSpeed (BOT), full campaign: basic/root phases, then QMP device_add of a second
            usb-storage stick (uevent add, I/O) and device_del while the guest reads it (EIO, uevent remove, node gone)
  uas       usb-uas + scsi-hd at SuperSpeed (UAS with streams), basic/root phases
  uas-hs    usb-uas on a USB2-only xHCI (UAS without streams: READ/WRITE READY IUs), basic/root phases
  bot-4k    usb-storage with 4096-byte logical blocks (READ CAPACITY(16), 4Kn partition I/O), basic/root phases
  multilun  usb-bot with two scsi-hd LUNs: sda and sdb from one device, basic phase on sda, LUN 1 visible
  smp1      the bot campaign on one CPU
Every case checks the stick on the host afterwards (the unaligned write landed, nothing else changed) and that the
log has no kernel fault. Image copies are deleted after each case; logs stay in <work>.
"""
from pathlib import Path
import argparse, hashlib, importlib.util, json, os, shutil, socket, struct, subprocess, sys, time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
ALL = ('bot', 'uas', 'uas-hs', 'bot-4k', 'multilun', 'smp1')
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('usbstorage'))
ap.add_argument('--cases', default=','.join(ALL))
ap.add_argument('--accel', default='tcg', choices=['tcg', 'whpx'])
ap.add_argument('--qmp-port', type=int, default=4650)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
for c in cases: assert c in ALL, c


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
ext4lib = module('usbstor_ext4lib', ROOT / 'userland/ext4lib.py')

P2_START = 34816
SIZE = 64 * 1024 * 1024
WRITE_AT, WRITE_LEN = 1048576 + 333, 200000


def pattern_bytes(offset, length):
    """pattern(o) = ((o >> 9) * 7 + (o & 511) * 13 + 1) & 255 over a sector-aligned range."""
    assert offset % 512 == 0 and length % 512 == 0
    inner = bytes(((i * 13) + 1) & 255 for i in range(512))
    out = bytearray(length)
    for s in range(length // 512):
        k = (((offset // 512) + s) * 7) & 255
        out[s * 512:(s + 1) * 512] = bytes((b + k) & 255 for b in inner)
    return bytes(out)


def make_stick(path, sector=512):
    """64 MiB MBR disk: p1 type 0x0C at 2048 (16 MiB, zeros), p2 type 0x83 at 34816 to the end (pattern)."""
    with open(path, 'wb') as f:
        f.truncate(SIZE)
        mbr = bytearray(512)
        scale = sector // 512
        for i, (kind, start, count) in enumerate(((0x0C, 2048, 32768), (0x83, P2_START, SIZE // 512 - P2_START))):
            struct.pack_into('<B3sB3sII', mbr, 446 + 16 * i, 0, b'\x00\x02\x00', kind, b'\xff\xff\xff', start // scale, count // scale)
        mbr[510:512] = b'\x55\xaa'
        f.seek(0); f.write(mbr)
        f.seek(P2_START * 512)
        chunk = 1 << 20; at = 0; total = SIZE - P2_START * 512
        while at < total:
            n = min(chunk, total - at); f.write(pattern_bytes(at, n)); at += n


def verify_stick(path, wrote):
    with open(path, 'rb') as f:
        f.seek(P2_START * 512); data = f.read(SIZE - P2_START * 512)
    expected = bytearray(pattern_bytes(0, len(data)))
    if wrote:
        expected[WRITE_AT:WRITE_AT + WRITE_LEN] = bytes(((WRITE_AT + i) * 29 + 101) & 255 for i in range(WRITE_LEN))
    if data != bytes(expected):
        bad = next(i for i in range(len(data)) if data[i] != expected[i])
        raise AssertionError(f'{path}: partition 2 differs at offset {bad}')


def build_program():
    elf = WORK / 'storage64.elf'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'arch/x86_64/usbstor_user.c'), '-o', str(elf)], check=True)
    return elf


def make_root(path, program):
    shutil.copyfile(OUT / 'apps64-ext4.img', path)
    fs = ext4lib.Writer(str(path))
    data = Path(program).read_bytes()
    fs.put_bytes(data, 'sbin/init', 0o755, 0, 0)
    fs.put_bytes(data, 'bin/storage64-root', 0o4755, 0, 0)
    fs.close()


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


def wait_serial(proc, serial, marker, seconds=240):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        text = serial.read_text(errors='replace') if serial.exists() else ''
        if marker in text: return text
        if '[64] USBSTOR FAIL' in text or 'KERNEL_FAULT' in text: raise AssertionError(text[-4000:])
        if proc.poll() is not None: raise AssertionError(text[-4000:])
        time.sleep(.05)
    raise TimeoutError(marker)


def boot(tag, root, usb, hotplug=None, smp=2, timeout=600):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    cpu = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt'] if args.accel == 'whpx' else ['-cpu', 'max']
    port = args.qmp_port
    cmd = [QEMU, '-machine', 'q35', *cpu, '-smp', str(smp), '-m', '512',
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
           '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
           '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
           '-drive', f'if=none,id=root,file={root},format=raw', '-device', 'nvme,serial=root,drive=root',
           *usb, '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off']
    (WORK / (tag + '.cmd')).write_text(' '.join(map(str, cmd)))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        if hotplug:
            wait_serial(proc, serial, '[64] USBSTOR WAITING ADD')
            qmp = Qmp(port, proc)
            qmp.command('device_add', hotplug)
            wait_serial(proc, serial, '[64] USBSTOR WAITING REMOVE')
            time.sleep(2.0)                      # the guest is reading the stick now
            qmp.command('device_del', {'id': hotplug['id']})
            qmp.close()
        output = proc.communicate(timeout=timeout)[0].decode(errors='replace')
    except BaseException:
        proc.kill(); proc.communicate(); raise
    log = serial.read_text(errors='replace')
    return proc.returncode, log, output


def check(log, rc, tag, hotplug):
    assert rc == 33, (tag, rc, log[-4000:])
    assert '[64] USBSTOR ALL PASSED' in log and '[64] USBSTOR BASIC PASSED' in log, (tag, log[-4000:])
    assert 'KERNEL_FAULT' not in log and '[64] USBSTOR FAIL' not in log and 'panic' not in log.lower(), (tag, log[-4000:])
    assert '[64] USBSTOR ROOT PASSED' in log, (tag, log[-4000:])
    if hotplug: assert '[64] USBSTOR HOTPLUG REMOVE PASSED' in log and '[64] BLOCK sdb removed' in log, (tag, log[-4000:])


results = {}
program = build_program()
for case in cases:
    root, stick, stick2 = WORK / f'{case}-root.img', WORK / f'{case}-stick.img', WORK / f'{case}-stick2.img'
    try:
        make_root(root, program)
        sector = 4096 if case == 'bot-4k' else 512
        make_stick(stick, sector)
        hot = None; smp = 1 if case == 'smp1' else 2
        if case in ('bot', 'smp1'):
            make_stick(stick2)
            usb = ['-device', 'qemu-xhci,id=xhci', '-drive', f'if=none,id=stick,file={stick},format=raw',
                   '-device', 'usb-storage,bus=xhci.0,drive=stick,removable=on', '-drive', f'if=none,id=stick2,file={stick2},format=raw']
            hot = {'driver': 'usb-storage', 'id': 'hot1', 'bus': 'xhci.0', 'drive': 'stick2'}
        elif case == 'bot-4k':
            usb = ['-device', 'qemu-xhci,id=xhci', '-drive', f'if=none,id=stick,file={stick},format=raw',
                   '-device', 'usb-storage,bus=xhci.0,drive=stick,logical_block_size=4096,physical_block_size=4096']
        elif case in ('uas', 'uas-hs'):
            xhci = 'qemu-xhci,id=xhci' + (',p3=0' if case == 'uas-hs' else '')
            usb = ['-device', xhci, '-drive', f'if=none,id=stick,file={stick},format=raw',
                   '-device', 'usb-uas,id=uas,bus=xhci.0', '-device', 'scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=stick']
        else:  # multilun
            make_stick(stick2)
            usb = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-bot,id=bot,bus=xhci.0',
                   '-drive', f'if=none,id=stick,file={stick},format=raw', '-device', 'scsi-hd,bus=bot.0,lun=0,drive=stick',
                   '-drive', f'if=none,id=stick2,file={stick2},format=raw', '-device', 'scsi-hd,bus=bot.0,lun=1,drive=stick2']
        if not hot:
            fs = ext4lib.Writer(str(root)); fs.put_bytes(b'nohotplug', 'nexus/usbstor-mode', 0o644, 0, 0); fs.close()
        rc, log, out = boot(case, root, usb, hot, smp)
        if not hot and '[64] USBSTOR WAITING ADD' in log:
            raise AssertionError('hot-plug phase started without a device to add')
        check(log, rc, case, hot is not None)
        verify_stick(stick, True)
        if case == 'multilun': assert '[64] BLOCK sdb USB' in log, log[-3000:]
        if case == 'uas': assert 'mass storage UAS (streams)' in log, log[-3000:]
        if case == 'uas-hs': assert 'mass storage UAS' in log and 'UAS (streams)' not in log and 'high-speed' in log, log[-3000:]
        if case == 'bot-4k': assert ' bs=4096 ' in log and 'logical=4096' in log, log[-3000:]
        results[case] = 'PASS'
        print(f'PASS {case}', flush=True)
    finally:
        for p in (root, stick, stick2): p.unlink(missing_ok=True)
(WORK / 'results.json').write_text(json.dumps(results, indent=1) + '\n')
print('PASS USB mass storage: ' + ', '.join(results), flush=True)
