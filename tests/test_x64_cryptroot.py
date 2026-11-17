"""Encrypted root end to end: Debian's cryptsetup formats LUKS volumes (in a NexusOS guest with the Debian storage
tools) and copies a root filesystem into them; NexusOS then boots each volume as its root: the boot-time prompt
(cryptroot.c) takes the passphrase from the keyboard (QMP key events: PS/2, and a USB keyboard on xHCI), refuses a
wrong one and asks again, opens the volume (luks.c), and the root's /sbin/init (arch/x86_64/cryptroot_user.c) checks
that / is the dm-crypt mapping, writes through it and powers off; the host checks that the written text is not
on the raw disk.

Cases (volume made by cryptsetup 2.7 from Debian trixie; root filesystem):
  argon2id   LUKS2 with cryptsetup's defaults (argon2id, memory/time benchmarked by cryptsetup)    ext4   PS/2, wrong first
             (its disk also has a hibernation partition: writing "disk" to /sys/power/state must be refused)
  pbkdf2     LUKS2 --pbkdf pbkdf2                                                                     ext4   USB keyboard
  sector4k   LUKS2 argon2id 64 MiB --sector-size 4096                                                 ext4   cryptroot=/dev/nvme0n1p1
  argon2i    LUKS2 argon2i, 4 lanes                                                                   ext4   no cryptroot= (GPT LUKS type)
  luks1      LUKS1 aes-xts-plain64 (pbkdf2-sha256)                                                     ext4   rd.luks.uuid=
  luks1cbc   LUKS1 aes-cbc-essiv:sha256                                                                FAT32  -
  lockout    (luks1's volume) three wrong passphrases: the machine powers off ("CRYPTROOT POWER OFF")
  hostile    copies of the argon2i/luks1 volumes with hostile headers (HOSTILE: bad checksums, offsets, sizes, stripes,
             4 GiB Argon2, deep JSON nesting): one bad copy falls back to the other, the rest are refused, nothing crashes
Every boot case runs on 1 and 2 CPUs unless --smp is given; one screendump checks the graphical dialog.

Usage: test_x64_cryptroot.py WORK [--cases argon2id,pbkdf2,...] [--smp 1,2] [--qmp-port 4733] [--keep]"""
from pathlib import Path
import argparse, ctypes, hashlib, importlib.util, json, msvcrt, os, re, shutil, socket, struct, subprocess, sys, time, uuid, zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from qmpkeys import Qmp, type_text   # noqa: E402
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
PASSPHRASE = 'Correct horse battery 42'
LUKS_TYPE = uuid.UUID('ca7d7ccb-63ed-4c53-861c-1742536059cc')
CASES = {   # cryptsetup options, root filesystem
    'argon2id': ('--type luks2', 'ext4'),
    'pbkdf2': ('--type luks2 --pbkdf pbkdf2 --pbkdf-force-iterations 50000', 'ext4'),
    'sector4k': ('--type luks2 --pbkdf argon2id --pbkdf-memory 65536 --pbkdf-force-iterations 4 --sector-size 4096', 'ext4'),
    'argon2i': ('--type luks2 --pbkdf argon2i --pbkdf-memory 32768 --pbkdf-force-iterations 4 --pbkdf-parallel 4', 'ext4'),
    'luks1': ('--type luks1 --pbkdf-force-iterations 50000', 'ext4'),
    'luks1cbc': ('--type luks1 --cipher aes-cbc-essiv:sha256 --key-size 256 --pbkdf-force-iterations 50000', 'fat'),
}
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='argon2id,pbkdf2,sector4k,argon2i,luks1,luks1cbc,lockout,hostile')
ap.add_argument('--smp', default='1,2')
ap.add_argument('--qmp-port', type=int, default=4733)
ap.add_argument('--keep', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
wanted = [c for c in args.cases.split(',') if c]
results = []


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def check(ok, what, detail=''):
    results.append(ok); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-4000:]), flush=True)
    return ok


def punch(path):   # deallocate all-zero 64 KiB runs (NTFS sparse; plain truncate/seek does not on this host)
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    def ioctl(f, code, data):
        out = ctypes.c_uint32(0); buf = ctypes.create_string_buffer(data)
        if not k32.DeviceIoControl(msvcrt.get_osfhandle(f.fileno()), code, buf, len(data), None, 0, ctypes.byref(out), None): raise OSError(ctypes.get_last_error())
    with open(path, 'r+b') as f:
        ioctl(f, 0x900C4, b'\x01'); size = os.fstat(f.fileno()).st_size; start = None; at = 0; zero = bytes(65536)
        while chunk := f.read(65536):
            if chunk == zero[:len(chunk)]:
                if start is None: start = at
            elif start is not None: ioctl(f, 0x980C8, start.to_bytes(8, 'little') + at.to_bytes(8, 'little')); start = None
            at += len(chunk)
        if start is not None: ioctl(f, 0x980C8, start.to_bytes(8, 'little') + size.to_bytes(8, 'little'))


HIB_TYPE = bytes([0xB7, 0xA0, 0xB1, 0xC0, 0x48, 0xA5, 0x4E, 0x4E, 0x9F, 0x1D, 0x4E, 0x58, 0x48, 0x49, 0x42, 0x45])   # hibernate.c HIB_GUID
def gpt_disk(path, mib, part_guid, hib_mib=0):
    """Partition 1 (LUKS type) fills `mib` MiB; hib_mib adds a hibernation partition (hibernate.c) after it."""
    total = (mib + hib_mib) * 2048; first = 2048; last = first + (mib * 2048 - 34 - first) // 8 * 8 - 1   # whole 4 KiB units (--sector-size 4096)
    entry = LUKS_TYPE.bytes_le + part_guid.bytes_le + struct.pack('<QQQ', first, last, 0) + 'cryptroot'.encode('utf-16-le').ljust(72, b'\0')
    if hib_mib: entry += HIB_TYPE + uuid.uuid4().bytes_le + struct.pack('<QQQ', mib * 2048, total - 35, 0) + 'hibernate'.encode('utf-16-le').ljust(72, b'\0')
    entries = entry.ljust(128 * 128, b'\0'); disk_guid = uuid.uuid4()
    def header(mine, other, table):
        h = bytearray(struct.pack('<8sIII4xQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, mine, other, 34, total - 34, disk_guid.bytes_le, table, 128, 128, zlib.crc32(entries)))
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:92]))); return bytes(h).ljust(512, b'\0')
    mbr = bytearray(512); struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\x02\0', 0xEE, b'\xff\xff\xff', 1, min(total - 1, 0xFFFFFFFF)); mbr[510:] = b'\x55\xaa'
    path.unlink(missing_ok=True)
    with open(path, 'wb') as f:
        f.truncate(total * 512); f.write(mbr); f.write(header(1, total - 1, 2)); f.write(entries)
        f.seek((total - 33) * 512); f.write(entries); f.write(header(total - 1, 1, total - 33))
    punch(path)


def build_programs():
    init = WORK / 'cryptroot_user.elf'; fstools = WORK / 'fstools.elf'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-musl', '-static', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/cryptroot_user.c'), '-o', str(init)], check=True)
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/fstools_user.c'), '-o', str(fstools)], check=True)
    return init, fstools


def root_images(init):
    """The two root filesystems (partition contents only): apps64's ext4 and FAT32 volumes with the test init."""
    ext4lib = load('cr_ext4lib', ROOT / 'userland/ext4lib.py'); fatput = load('cr_fatput', ROOT / 'userland/fatput.py')
    staged = WORK / 'ext4-staged.img'; shutil.copyfile(OUT / 'apps64-ext4.img', staged)
    fs = ext4lib.Writer(str(staged))
    try:
        fs.put_bytes(init.read_bytes(), 'sbin/init', 0o755, 0, 0); fs.put_bytes(b'cryptroot test\n', 'nexus/init-as-root', 0o644, 0, 0)
    finally: fs.close()
    fat = WORK / 'fat-staged.img'; shutil.copyfile(OUT / 'apps64.img', fat)
    marker = WORK / 'init-as-root'; marker.write_text('cryptroot test\n', newline='\n')
    fatput.edit(['fatput', str(fat), 'put', str(init), 'sbin/init', 'put', str(marker), 'nexus/init-as-root'])
    out = {}
    for kind, image in (('ext4', staged), ('fat', fat)):
        raw = WORK / f'root-{kind}.raw'
        with open(image, 'rb') as a, open(raw, 'wb') as b: a.seek(32768 * 512); b.write(a.read(98304 * 512))
        out[kind] = raw; unlink(image)
    return out


def unlink(path):
    import gc
    for _ in range(40):
        try: Path(path).unlink(missing_ok=True); return
        except PermissionError: gc.collect(); time.sleep(0.5)


def tools_base():
    tools = load('cr_storage_tools', ROOT / 'arch/x86_64/storage_tools.py')
    base, stamp_file = WORK / 'tools-base.img', WORK / 'tools-base.stamp'
    stamp = (ROOT / 'arch/x86_64/storage-lock.json').read_bytes()[:4096].hex()[:16] + str((OUT / 'apps64.img').stat().st_mtime_ns)
    if base.exists() and stamp_file.exists() and stamp_file.read_text() == stamp: return base
    unlink(base); tools.image(base, tools.CACHE, 640, []); punch(base); stamp_file.write_text(stamp)
    return base


def prepare(cases, roots, fstools):
    """One boot of the Debian tools root: cryptsetup luksFormat + open + dd of the root image for every case."""
    ext4lib = load('cr_ext4lib2', ROOT / 'userland/ext4lib.py')
    disk = WORK / 'prep-root.img'; shutil.copyfile(tools_base(), disk); punch(disk)
    lines = ['#!/bin/dash', 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1', 'fail() { echo "[64] FSTEST FAIL $*"; exit 1; }',
             'mkdir -p /run/cryptsetup', f"printf '%s' '{PASSPHRASE}' > /tmp/pass"]
    if len(cases) > 6: raise SystemExit('at most 6 volumes per preparation boot (AHCI ports)')
    for i, case in enumerate(cases):
        options, kind = CASES[case]
        dev = f'/dev/sd{chr(97 + i)}1'
        lines += [f'[ -b {dev} ] || fail "no {dev}"',
                  f'cryptsetup luksFormat -q {options} --key-file /tmp/pass {dev} || fail "luksFormat {case}"',
                  f'cryptsetup luksDump {dev} | grep -E "Version|Cipher|PBKDF|Memory|Time|Threads|Iterations|sector|Hash|Key:" | sed "s/^/[64] DUMP {case}: /"',
                  f'cryptsetup open --key-file /tmp/pass {dev} c{i} || fail "open {case}"',
                  f'echo "[64] DUMP {case}: table $(dmsetup table c{i} | sed "s/ [0-9a-f]\\{{32,\\}} / KEY /; s/:[0-9]*:logon:[^ ]* / KEY /")"',
                  f'dd if=/nexus/root-{kind}.raw of=/dev/mapper/c{i} bs=1M conv=fsync status=none || fail "dd {case}"',
                  f'cryptsetup close c{i} || fail "close {case}"',
                  f'echo "[64] FSTEST PASS prepared {case} $(blkid -o value -s UUID {dev})"']
    lines += ['sync', 'echo "[64] FSTEST ALL PASSED"']
    fs = ext4lib.Writer(str(disk)); data = fstools.read_bytes()
    try:
        fs.put_bytes(data, 'sbin/init', 0o755, 0, 0); fs.put_bytes(data, 'bin/fstools-root', 0o4755, 0, 0)
        fs.put_bytes(('\n'.join(lines) + '\n').encode(), 'nexus/fs-cryptroot.sh', 0o644, 0, 0)
        fs.put_bytes(b'script /nexus/fs-cryptroot.sh\n', 'nexus/fstools-mode', 0o644, 0, 0)
        fs.put_bytes(b'root:x:0:0:root:/root:/bin/sh\n', 'etc/passwd', 0o644, 0, 0)
        for kind, raw in roots.items(): fs.put_bytes(raw.read_bytes(), f'nexus/root-{kind}.raw', 0o644, 0, 0)   # the root images to copy in
    finally: fs.close()
    command = [QEMU, '-machine', 'q35', '-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU, '-smp', '2', '-m', '2048', '-display', 'none', '-vga', 'none',
               '-device', 'virtio-vga', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
               '-drive', f'if=none,id=d0,format=raw,file={disk}', '-device', 'nvme,serial=prep0,drive=d0', '-device', 'ahci,id=ahci']
    for i, c in enumerate(cases):   # the volumes on AHCI ports 0..5: /dev/sda1 ...
        command += ['-drive', f'if=none,id=t{i},format=raw,file={WORK / f"target-{c}.img"}', '-device', f'ide-hd,drive=t{i},bus=ahci.{i}']
    serial = WORK / 'prepare.log'; serial.unlink(missing_ok=True)
    command += ['-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot']
    try: subprocess.run(command, capture_output=True, timeout=1800)
    except subprocess.TimeoutExpired: pass
    log = serial.read_text(errors='replace'); unlink(disk)
    for line in log.splitlines():
        if '[64] DUMP' in line or 'FSTEST' in line: print('    ' + line.strip()[:160])
    uuids = {m.group(1): m.group(2) for m in re.finditer(r'FSTEST PASS prepared (\w+) ([0-9a-f-]+)', log)}
    return check('FSTEST ALL PASSED' in log and len(uuids) == len(cases), f'cryptsetup formatted and filled {len(cases)} volumes', log[-3000:]), uuids


def vm_command(target, cmdline, cpus, serial, usb=False):
    return [QEMU, '-machine', 'q35,i8042=off' if usb else 'q35', '-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU, '-smp', str(cpus), '-m', '2048', '-display', 'none',
            '-vga', 'none', '-device', 'virtio-vga', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
            '-drive', f'if=none,id=root,format=raw,file={target}', '-device', 'nvme,serial=cryptroot,drive=root',
            *(['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,id=kbd0,bus=xhci.0'] if usb else []),
            '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot',
            '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', *(['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}'] if cmdline else [])]


def boot_case(case, target, cmdline, cpus, wrong_first=1, usb=False, shot=False, expect_fs='ext4'):
    serial = WORK / f'{case}-smp{cpus}.log'; serial.unlink(missing_ok=True)
    vm = subprocess.Popen(vm_command(target, cmdline, cpus, serial, usb), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = ''; ok = True; notes = []
    def wait(pattern, seconds=240, start=0):
        nonlocal log
        end = time.time() + seconds
        while time.time() < end:
            log = serial.read_text(errors='replace') if serial.exists() else ''
            if re.search(pattern, log[start:]): return True
            if vm.poll() is not None: log = serial.read_text(errors='replace') if serial.exists() else ''; return bool(re.search(pattern, log[start:]))
            time.sleep(.2)
        return False
    def wait_count(text, n, seconds=300):   # the n-th occurrence of `text`
        nonlocal log
        end = time.time() + seconds
        while time.time() < end:
            log = serial.read_text(errors='replace') if serial.exists() else ''
            if log.count(text) >= n: return True
            if vm.poll() is not None: return log.count(text) >= n
            time.sleep(.2)
        return False
    try:
        q = Qmp(args.qmp_port); device = None   # USB case: no i8042 on the machine, so QEMU's only keyboard is usb-kbd
        for attempt in range(wrong_first):
            ok &= wait_count('CRYPTROOT PROMPT', attempt + 1)
            if not ok: notes.append('no prompt'); break
            if shot and not attempt:
                ppm = WORK / f'{case}-prompt.ppm'; time.sleep(1); q.cmd('screendump', {'filename': str(ppm)})
                data = ppm.read_bytes(); ppm.unlink()
                header = data.split(b'\n', 3); w, h = map(int, header[1].split()); pixels = header[3]
                def rgb(x, y): o = (y * w + x) * 3; return tuple(pixels[o:o + 3])
                k = 3 if w >= 2400 else 2 if w >= 1600 else 1   # cryptroot.c draw(): a 640x300 panel (times k), centred
                panel = rgb((w - 640 * k) // 2 + 10 * k, (h - 300 * k) // 2 + 250 * k)
                accent = sum(1 for x in range(0, w, 4) if rgb(x, (h - 300 * k) // 2 + 2) == (0x5B, 0x8C, 0xFF))
                ok &= check(panel == (0x1B, 0x21, 0x30) and accent > 20, f'{case} smp{cpus}: graphical unlock dialog on the framebuffer', (panel, accent))
            type_text(q, 'not the passphrase\n', device)
            ok &= wait_count('Wrong passphrase', attempt + 1)
            if not ok: notes.append('wrong passphrase not refused'); break
        if case == 'lockout':
            ok &= wait(r'CRYPTROOT POWER OFF', seconds=120); notes.append('lockout')
        elif ok:
            ok &= wait_count('CRYPTROOT PROMPT', wrong_first + 1)
            type_text(q, PASSPHRASE + '\n', device)
            ok &= wait(r'CRYPTROOT UNLOCKED', seconds=600) and wait(r'CRYPTROOT ROOT (OK|FAIL)', seconds=240)
            ok &= f'CRYPTROOT ROOT OK fs={expect_fs}' in log
        vm.wait(120)
    except Exception as error:
        ok = False; notes.append(repr(error))
    finally:
        if vm.poll() is None: vm.kill(); vm.wait()
    bad = [w for w in ('KERNEL_FAULT', '[64] FAIL:', 'USER_FAULT') if w in log]
    took = re.search(r'UNLOCKED keyslot in (\d+) ms', log)
    root = re.search(r'CRYPTROOT ROOT OK ([^\n]*)', log)
    return ok and not bad, (took.group(1) + ' ms' if took else '-'), (root.group(1) if root else ''), notes + bad, log


PART = 2048 * 512   # gpt_disk: partition 1 starts at LBA 2048
def luks2_edit(image, change, copies=(0, 1), fix_checksum=True):
    """Rewrite the LUKS2 JSON of the chosen header copies (0 primary, 1 secondary) with change(dict) -> dict or str."""
    with open(image, 'r+b') as f:
        f.seek(PART); size = struct.unpack('>Q', f.read(16)[8:16])[0]
        for copy in copies:
            f.seek(PART + copy * size); area = bytearray(f.read(size))
            text = bytes(area[4096:]).split(b'\0', 1)[0].decode()
            new = change(json.loads(text)); new = (new if isinstance(new, str) else json.dumps(new)).encode()
            if len(new) >= size - 4096: raise SystemExit('hostile JSON too large for the header')
            area[4096:] = new.ljust(size - 4096, b'\0')
            if fix_checksum:
                area[448:512] = bytes(64); area[448:480] = hashlib.sha256(bytes(area)).digest()
            f.seek(PART + copy * size); f.write(area)


def luks1_edit(image, offset, data):
    with open(image, 'r+b') as f: f.seek(PART + offset); f.write(data)


def slots(change):   # apply change(slot) to every LUKS2 keyslot
    def edit(meta):
        for slot in meta['keyslots'].values(): change(slot)
        return meta
    return edit


def flip_json(meta):   # a valid-looking JSON whose checksum no longer matches (fix_checksum=False)
    meta['config']['json_size'] = str(int(meta['config']['json_size']) + 1); return meta


# name: (base volume, how to damage it, expected outcome)  outcome: unlock | damaged | unsupported | memory
HOSTILE = {
    'primary-checksum': ('argon2i', lambda t: luks2_edit(t, flip_json, copies=(0,), fix_checksum=False), 'unlock'),
    'both-checksums': ('argon2i', lambda t: luks2_edit(t, flip_json, fix_checksum=False), 'damaged'),
    'area-offset': ('argon2i', lambda t: luks2_edit(t, slots(lambda s: s['area'].update(offset=str(10 ** 15)))), 'damaged'),
    'key-size': ('argon2i', lambda t: luks2_edit(t, slots(lambda s: s.update(key_size=4096))), 'damaged'),
    'stripes': ('argon2i', lambda t: luks2_edit(t, slots(lambda s: s['af'].update(stripes=4000000))), 'unsupported'),
    'argon2-memory': ('argon2i', lambda t: luks2_edit(t, slots(lambda s: s['kdf'].update(memory=4194304))), 'memory'),
    'segment-offset': ('argon2i', lambda t: luks2_edit(t, lambda m: (m['segments']['0'].update(offset=str(2 ** 62)), m)[1]), 'damaged'),
    'nesting': ('argon2i', lambda t: luks2_edit(t, lambda m: '{"keyslots":' + '[' * 4000 + ']' * 4000 + '}'), 'damaged'),
    'luks1-stripes': ('luks1', lambda t: luks1_edit(t, 208 + 44, struct.pack('>I', 0x7FFFFFFF)), 'damaged'),
}
OUTCOME = {'damaged': 'The LUKS metadata is damaged', 'unsupported': 'cipher, hash or feature NexusOS cannot open',
           'memory': 'Not enough memory for this keyslot'}


def hostile_case(name, base, damage, expect, guid, cpus):
    """A copy of a cryptsetup-made volume with a hostile header: the boot must neither crash nor trust it."""
    target = WORK / 'target-hostile.img'; shutil.copyfile(base, target); damage(target)
    serial = WORK / f'hostile-{name}-smp{cpus}.log'; serial.unlink(missing_ok=True)
    vm = subprocess.Popen(vm_command(target, f'cryptroot=PARTUUID={guid}', cpus, serial), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = ''; ok = False
    def wait(pattern, seconds):
        nonlocal log
        end = time.time() + seconds
        while time.time() < end:
            log = serial.read_text(errors='replace') if serial.exists() else ''
            if pattern in log: return True
            if vm.poll() is not None: log = serial.read_text(errors='replace') if serial.exists() else ''; return pattern in log
            time.sleep(.2)
        return False
    try:
        q = Qmp(args.qmp_port)
        if wait('CRYPTROOT PROMPT', 240):
            type_text(q, PASSPHRASE + '\n')
            if expect == 'unlock': ok = wait('CRYPTROOT ROOT OK', 600)
            else: ok = wait(OUTCOME[expect], 600) and wait('CRYPTROOT POWER OFF', 60) and 'Wrong passphrase' not in log
        if expect != 'unlock' and ok: vm.wait(60)
    except Exception as error: log += f'\n{error!r}'
    finally:
        if vm.poll() is None: vm.kill(); vm.wait()
        unlink(target)
    bad = [w for w in ('KERNEL_FAULT', '[64] FAIL:', 'USER_FAULT', 'PANIC') if w in log]
    detail = '\n'.join(l for l in log.splitlines() if 'CRYPTROOT' in l or 'FAULT' in l or 'PANIC' in l)[-2000:]
    return check(ok and not bad, f'hostile {name} smp{cpus}: {expect}', (bad, detail))


init, fstools = build_programs()
boot_cases = [c for c in wanted if c in CASES]
if 'lockout' in wanted and 'luks1' not in boot_cases: boot_cases.append('luks1')
if 'hostile' in wanted: boot_cases += [c for c in ('argon2i', 'luks1') if c not in boot_cases]
guids = {c: uuid.uuid4() for c in boot_cases}
for c in boot_cases: gpt_disk(WORK / f'target-{c}.img', 96, guids[c], hib_mib=72 if c == 'argon2id' else 0)
roots = root_images(init)
ok, uuids = prepare(boot_cases, roots, fstools)
for raw in roots.values(): unlink(raw)
if ok:
    for cpus in [int(x) for x in args.smp.split(',')]:
        for i, case in enumerate(wanted):
            if case == 'hostile':
                for name, (base, damage, expect) in HOSTILE.items(): hostile_case(name, WORK / f'target-{base}.img', damage, expect, guids[base], cpus)
                continue
            target = WORK / f'target-{case if case != "lockout" else "luks1"}.img'
            cmdline = {'argon2id': f'cryptroot=PARTUUID={guids.get("argon2id")} root=/dev/mapper/cryptroot',
                       'pbkdf2': f'cryptroot=PARTUUID={guids.get("pbkdf2")} cryptroot.name=system',
                       'sector4k': 'cryptroot=/dev/nvme0n1p1',
                       'argon2i': '',
                       'luks1': f'rd.luks.uuid={uuids.get("luks1")}',
                       'luks1cbc': f'cryptroot=UUID={uuids.get("luks1cbc")}',
                       'lockout': f'cryptroot=PARTUUID={guids.get("luks1")}'}[case]
            passed, took, root, notes, log = boot_case(case, target, cmdline, cpus, wrong_first=3 if case == 'lockout' else 1, usb=case == 'pbkdf2',
                                                       shot=case == 'argon2id' and cpus == 2, expect_fs=CASES.get(case, ('', 'ext4'))[1] if case != 'luks1cbc' else 'vfat')
            what = 'three wrong passphrases power the machine off' if case == 'lockout' else f'wrong passphrase refused, unlocked in {took}, root: {root}'
            check(passed, f'{case} smp{cpus}: {what}', (notes, '\n'.join(l for l in log.splitlines() if 'CRYPTROOT' in l or 'FAIL' in l)[-2500:]))
            if case == 'argon2id':   # its disk has a hibernation partition: refused while the encrypted root is open
                check('CRYPTROOT HIBERNATE refused' in log and 'PM: hibernation refused: an encrypted volume is open' in log,
                      f'{case} smp{cpus}: hibernation refused while the encrypted root is open',
                      '\n'.join(l for l in log.splitlines() if 'HIBERNATE' in l or 'PM:' in l)[-1500:])
            if passed and case != 'lockout':
                with open(target, 'rb') as f:
                    clean = True
                    while chunk := f.read(1 << 20):
                        if b'NEXUS-CRYPTROOT-PLAINTEXT-MARK' in chunk: clean = False; break
                check(clean, f'{case} smp{cpus}: the text written through the mapping is not on the raw disk')
if not args.keep:
    for c in boot_cases: unlink(WORK / f'target-{c}.img')
print(f'SUMMARY cryptroot: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
