"""Boot NexusOS from a USB stick only (BIOS and UEFI) and install it onto the internal NVMe disk.

    python -I tests/test_x64_osinstall.py WORK [--cases usbboot,install,encrypt] [--accel auto|whpx|tcg] [--qmp-port 4653]

The stick is `build.py usb-image` (arch/x86_64/usbimage.py) made from out/apps64.img, with
arch/x86_64/osinstall_test_user.c as /sbin/init; QEMU has no floppy and no other boot disk.
  usbboot   a stick with the normal apps64 root boots on SeaBIOS (WHPX or TCG) and on OVMF (TCG) from
            usb-storage on xHCI; the kernel takes root=PARTUUID from stage 2 / CMDLINE.TXT and runs the whole
            boot campaign ("[64] ALL TESTS PASSED");
  install   1. live boot (BIOS): init checks nexus-os-install's refusals (its own stick, a 64 MiB NVMe disk), writes a
               file, requests nvme0n1 and restarts;
            2. the kernel finds the request, asks on the console; the test types "no" -> cancelled, the live
               system starts again, requests again and restarts;
            3. the kernel asks again; the test types YES, the system is copied, Enter restarts;
            4. NVMe only, SeaBIOS: the installed system boots (root nvme0n1p3), the live file is there, the request
               is gone, the root is writable; 5. the same with OVMF (TCG);
            6. stick + installed disk, firmware boots the stick: the live system keeps its own root;
            7. stick + installed disk, firmware boots the disk: the installed root is chosen (root=PARTUUID).
  desktop   (not in the default list; needs ~5 GB of scratch space) the Aurora desktop of --desktop-source on a stick:
            SeaBIOS boots it from USB only, `nexus-os-install nvme0n1 --yes --reboot` is typed into Aurora's Terminal,
            YES on the console copies it, and the internal disk alone starts Aurora (SeaBIOS, then OVMF/TCG).
Images are sparse in WORK and deleted at the end; logs stay.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import re
import socket
import struct
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='usbboot,install,encrypt')
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4653)
ap.add_argument('--desktop-source', type=Path, default=nexus_paths.source_disk(),
                help='desktop: a published desktop64 image (read only) whose root goes onto the stick')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


build = module('osinstall_build', ROOT / 'arch/x86_64/build.py')
usbimage = module('osinstall_usbimage', ROOT / 'arch/x86_64/usbimage.py')
fatput = module('osinstall_fatput', ROOT / 'userland/fatput.py')
accel = ('whpx' if build.whpx_available() else 'tcg') if args.accel == 'auto' else args.accel
results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), str(detail)[-1500:])); print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ' -- ' + str(detail)[-1500:]), flush=True)
    return ok


class Qmp:
    def __init__(self, port, proc):
        for _ in range(300):
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=60); break
            except OSError:
                if proc.poll() is not None: raise AssertionError('QEMU exited before QMP connected')
                time.sleep(.1)
        else: raise TimeoutError('QMP')
        self.f = self.s.makefile('rwb'); self.f.readline(); self.cmd('qmp_capabilities')

    def cmd(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None: request['arguments'] = arguments
        self.f.write(json.dumps(request).encode() + b'\n'); self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if 'event' in reply: continue
            if 'error' in reply: raise RuntimeError(reply['error'])
            return reply.get('return')

    def type(self, text):
        names = {' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash', '=': 'equal'}
        for ch in text:
            keys = [{'type': 'qcode', 'data': 'shift'}] if ch.isupper() else []
            keys.append({'type': 'qcode', 'data': names.get(ch, ch.lower())})
            self.cmd('send-key', {'keys': keys}); time.sleep(.15)


def boot(tag, disks, uefi=False, keys=(), timeout=900, memory='1024', stop=None):
    """disks: [(path, 'usb'|'nvme', bootindex or None, serial)]; keys: [(pattern, text or callable(q))], each acted on when
    its pattern shows up after the previous action; stop: a pattern after which QEMU is quit (returncode None)."""
    serial = WORK / f'{tag}.log'; serial.unlink(missing_ok=True)
    tcg = uefi or accel == 'tcg'
    cpu = ['-accel', 'tcg', '-cpu', 'max'] if tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
    command = [QEMU, '-machine', 'q35', *cpu, '-smp', '2', '-m', memory, '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-device', 'qemu-xhci,id=xhci', '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
               '-no-reboot', '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off']
    if uefi: command += ['-bios', str(build.ovmf_unified())]
    for i, (path, kind, index, serial_number) in enumerate(disks):
        command += ['-drive', f'if=none,id=d{i},format=raw,file={path}']
        boot_index = f',bootindex={index}' if index is not None else ''
        command += ['-device', f'usb-storage,bus=xhci.0,drive=d{i}{boot_index}' if kind == 'usb' else f'nvme,serial={serial_number},drive=d{i}{boot_index}']
    (WORK / f'{tag}.cmd').write_text(' '.join(map(str, command)))
    proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    q = Qmp(args.qmp_port, proc); pending = list(keys); start = time.time(); mark = 0; stopped = False
    try:
        while proc.poll() is None and time.time() - start < timeout:
            log = serial.read_bytes().decode('latin1') if serial.exists() else ''
            if pending and re.search(pending[0][0], log[mark:]):
                time.sleep(1); action = pending.pop(0)[1]; mark = len(log)
                action(q) if callable(action) else q.type(action)
            elif not pending and stop and re.search(stop, log[mark:]):
                stopped = True; time.sleep(2)
                try: q.cmd('quit')
                except Exception: pass
                proc.wait(60); break
            time.sleep(.3)
        if proc.poll() is None: proc.kill(); proc.wait(); raise TimeoutError(tag)
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
    log = serial.read_bytes().decode('latin1')
    return (None if stopped else proc.returncode), log


def faultless(log): return 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and '[64] FAIL:' not in log


def raw(path, size):
    path.unlink(missing_ok=True); path.touch()
    subprocess.run(['fsutil', 'sparse', 'setflag', str(path)], check=True, stdout=subprocess.DEVNULL)
    with path.open('r+b') as f: f.truncate(size)


def root_copy_faithful(stick, disk):
    """The installed FAT32 root against the stick's: every target sector equals the source sector, or is zero (a free
    cluster is not copied), or is the first sector of a FAT whose only change is the clean bit (FAT[1] bit 27), or the
    FSInfo sector with other free-count/next-free hints (the installer writes the copy's; the stick's changes at shutdown)."""
    with open(disk, 'rb') as d, open(stick, 'rb') as s:
        d.seek(512); header = d.read(512); d.seek(512 * int.from_bytes(header[72:80], 'little')); entries = d.read(128 * 128)
        start = end = None
        for i in range(128):
            e = entries[i * 128:(i + 1) * 128]
            if e[:16] == bytes(16): continue
            start, end = int.from_bytes(e[32:40], 'little'), int.from_bytes(e[40:48], 'little')   # the last entry: the root
        s.seek(512 * start); boot = s.read(512)
        reserved, fats, fat_size = int.from_bytes(boot[14:16], 'little'), boot[16], int.from_bytes(boot[36:40], 'little')
        fat_starts = {reserved + k * fat_size for k in range(fats)}; fs_info = int.from_bytes(boot[48:50], 'little')
        sector = 0; total = end - start + 1; bad = []
        while sector < total and len(bad) < 5:
            n = min(2048, total - sector)
            d.seek(512 * (start + sector)); s.seek(512 * (start + sector)); a, b = d.read(512 * n), s.read(512 * n)
            if a != b:
                for k in range(n):
                    x, y = a[512 * k:512 * (k + 1)], b[512 * k:512 * (k + 1)]
                    if x == y or x == bytes(512): continue
                    if sector + k in fat_starts and x[:4] == y[:4] and x[8:] == y[8:] and \
                            (int.from_bytes(x[4:8], 'little') & ~0x08000000) == (int.from_bytes(y[4:8], 'little') & ~0x08000000): continue
                    if sector + k == fs_info and x[:488] == y[:488] and x[496:] == y[496:]: continue   # FSInfo hints: free count, next free
                    bad.append(sector + k)
            sector += n
        return not bad, f'root sectors differing from the stick: {bad}'


def case_usbboot():
    stick = WORK / 'apps-usb.img'
    info = usbimage.build_image(stick, None, OUT / 'apps64.img')
    try:
        with open(stick, 'rb') as f: mbr = f.read(512)
        parts = [struct.unpack_from('<B3sB3sII', mbr, 446 + 16 * i) for i in range(4)]
        hybrid = [(p[0], p[2], p[4]) for p in parts if p[2]]
        check('usb image: hybrid MBR (0xEE over the GPT at LBA 1, ESP 0xEF, FAT32 root 0x0C active) next to the GPT',
              hybrid == [(0, 0xEE, 1), (0, 0xEF, usbimage.ESP_START), (0x80, 0x0C, usbimage.ROOT_START)] and mbr[510:] == b'\x55\xaa', hybrid)
        for tag, uefi in (('usbboot-bios', False), ('usbboot-uefi', True)):
            rc, log = boot(tag, [(stick, 'usb', 0, None)], uefi=uefi, timeout=1500 if uefi else 900)
            source = 'UEFI' if uefi else 'BIOS'
            check(f'{tag}: firmware booted the USB stick ({source}) with root=PARTUUID', f'BOOT source={source}' in log and
                  'cmdline="' + info['cmdline'] in log and re.search(r'\[64\] ROOT sda3 \(root= on the command line, FAT32\) on USB', log), log[-2500:])
            # the campaign provokes user-mode faults on purpose; kernel faults and failed checks count
            check(f'{tag}: boot campaign passed from the stick', rc == 33 and '[64] ALL TESTS PASSED' in log and 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log, log[-2500:])
    finally:
        stick.unlink(missing_ok=True)


def case_install():
    root_img = WORK / 'root.img'
    stick, nvme, small = WORK / 'live-usb.img', WORK / 'nvme.img', WORK / 'small.img'
    try:
        root_img.unlink(missing_ok=True)
        with (OUT / 'apps64.img').open('rb') as a, root_img.open('wb') as b: b.write(a.read())
        elf = WORK / 'osinstall_test.elf'
        build.run(build.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                  ROOT / 'arch/x86_64/osinstall_test_user.c', '-o', elf)
        assert fatput.edit(['fatput', str(root_img), 'put', str(elf), 'sbin/init', 'mkdir', 'home/nexus']) == 0
        info = usbimage.build_image(stick, None, root_img)
        root_img.unlink()
        raw(nvme, 1 << 30); raw(small, 64 << 20)
        live = [(stick, 'usb', 0, None), (nvme, 'nvme', None, 'NXINSTALL01'), (small, 'nvme', None, 'NXSMALL01')]
        rc, log = boot('install-1-request', live)
        check('live boot from the stick: refusals checked', '[64] OSTEST REFUSALS PASSED' in log and faultless(log), log[-2500:])
        check('live boot: request written, reboot(2) restarted', 'OSINSTALL REQUEST target=nvme0n1 serial=NXINSTALL01' in log and
              'REBOOT syscall: restart' in log, log[-2000:])
        rc, log = boot('install-2-cancel', live, keys=[(r'Type YES and press Enter', 'no\n')])
        check('kernel asks on the console; "no" cancels and the live system starts', 'INSTALL requested: target nvme0n1' in log and
              'INSTALL cancelled' in log and 'INSTALL COMPLETE' not in log and 'OSINSTALL REQUEST target=nvme0n1' in log and faultless(log), log[-2500:])
        rc, log = boot('install-3-copy', live, keys=[(r'Type YES and press Enter', 'YES\n'), (r'press Enter to restart', '\n')], timeout=1500)
        m = re.search(r'INSTALL COMPLETE target=nvme0n1 root=PARTUUID=(\S+) fat32_sectors=(\d+) ms=(\d+)', log)
        check('YES installs: GPT, boot partitions, root copied and verified, Enter restarts', m and faultless(log), log[-3000:])
        partuuid = m.group(1) if m else 'none'
        faithful, detail = root_copy_faithful(stick, nvme)
        check('the installed FAT32 root is the stick root, sector for sector (FAT clean bit set)', faithful, detail)
        installed = [(nvme, 'nvme', 0, 'NXINSTALL01')]
        rc, log = boot('install-4-nvme-bios', installed)
        check('installed disk boots alone (SeaBIOS)', rc == 33 and f'cmdline="root=PARTUUID={partuuid}' in log and
              '[64] OSTEST INSTALLED SYSTEM PASSED' in log and 'ROOT nvme0n1p3' in log and faultless(log), log[-2500:])
        rc, log = boot('install-5-nvme-uefi', installed, uefi=True, timeout=1500)
        check('installed disk boots alone (OVMF, TCG)', rc == 33 and 'BOOT source=UEFI' in log and '[64] OSTEST INSTALLED SYSTEM PASSED' in log and faultless(log), log[-2500:])
        rc, log = boot('install-6-both-stick', [(stick, 'usb', 0, None), (nvme, 'nvme', None, 'NXINSTALL01')])
        check('stick + installed disk, stick booted: live root kept', rc == 33 and 'OSTEST LIVE with an installed disk present' in log and
              'ROOT sda3' in log and faultless(log), log[-2500:])
        rc, log = boot('install-7-both-disk', [(stick, 'usb', None, None), (nvme, 'nvme', 0, 'NXINSTALL01')])
        check('stick + installed disk, disk booted: installed root chosen', rc == 33 and 'ROOT nvme0n1p3' in log and
              '[64] OSTEST INSTALLED SYSTEM PASSED' in log and faultless(log), log[-2500:])
    finally:
        for path in (root_img, stick, nvme, small): path.unlink(missing_ok=True)


ENCRYPT_PASS = 'Correct horse-99 yes'   # '-' and 'y' sit on other keys of a German keyboard
def case_encrypt():
    """--encrypt: the live system requests an encrypted install; the kernel asks YES and the passphrase (a too short one
    and a mismatch are refused), formats LUKS2 and copies the root through dm-crypt; the internal disk alone then asks
    for the passphrase at boot (cryptroot.c), refuses a wrong one, and starts the installed system from /dev/mapper/cryptroot."""
    root_img, stick, nvme, small = WORK / 'root-enc.img', WORK / 'live-enc.img', WORK / 'nvme-enc.img', WORK / 'small-enc.img'
    try:
        root_img.unlink(missing_ok=True)
        with (OUT / 'apps64.img').open('rb') as a, root_img.open('wb') as b: b.write(a.read())
        elf = WORK / 'osinstall_test.elf'
        build.run(build.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                  ROOT / 'arch/x86_64/osinstall_test_user.c', '-o', elf)
        marker = WORK / 'encrypt-marker'; marker.write_text('encrypt\n', newline='\n')
        assert fatput.edit(['fatput', str(root_img), 'put', str(elf), 'sbin/init', 'mkdir', 'home/nexus', 'put', str(marker), 'nexus/osinstall-encrypt']) == 0
        usbimage.build_image(stick, None, root_img); root_img.unlink()
        raw(nvme, 1 << 30); raw(small, 64 << 20)
        live = [(stick, 'usb', 0, None), (nvme, 'nvme', None, 'NXCRYPT01'), (small, 'nvme', None, 'NXSMALL02')]
        rc, log = boot('encrypt-1-request', live)
        check('encrypt: live system requests an encrypted install and restarts', 'OSINSTALL REQUEST target=nvme0n1' in log and
              'REBOOT syscall: restart' in log and faultless(log), log[-2500:])
        keys = [(r'Type YES and press Enter', 'YES\n'), (r'Choose the disk passphrase', 'short1\n'),
                (r'Choose the disk passphrase', ENCRYPT_PASS + '\n'), (r'Type the passphrase again', 'Correct horse 98\n'),
                (r'Choose the disk passphrase', ENCRYPT_PASS + '\n'), (r'Type the passphrase again', ENCRYPT_PASS + '\n'),
                (r'press Enter to restart', '\n')]
        rc, log = boot('encrypt-2-copy', live, keys=keys, timeout=1800)
        m = re.search(r'INSTALL COMPLETE target=nvme0n1 cryptroot=PARTUUID=(\S+) fat32_sectors=(\d+) ms=(\d+)', log)
        check('encrypt: passphrase chosen (too short and mismatch refused), LUKS2 formatted, root copied through dm-crypt',
              m and 'passphrase too short' in log and 'two passphrases differ' in log and 'INSTALL encrypting the root' in log and faultless(log), log[-3000:])
        with open(nvme, 'rb') as f:
            clean = True
            while chunk := f.read(1 << 20):
                if b'written in the live session' in chunk: clean = False; break
        check('encrypt: the live-session file is not readable on the raw internal disk', clean)
        # Debian's cryptsetup reads the volume NexusOS formatted: header, keyslot (argon2id), the root through dm-crypt
        sys.path.insert(0, str(ROOT / 'tests')); import debian_tools_vm
        script = '\n'.join(['#!/bin/dash', 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1', 'fail() { echo "[64] FSTEST FAIL $*"; exit 1; }',
                            'mkdir -p /run/cryptsetup', f"printf '%s' '{ENCRYPT_PASS}' > /tmp/pass", "printf 'wrong pass' > /tmp/bad",
                            'cryptsetup isLuks /dev/sda3 || fail "isLuks"',
                            'cryptsetup luksDump /dev/sda3 | sed "s/^/[64] DUMP: /" | grep -Ev "Salt|Digest:|^\\[64\\] DUMP: *[0-9a-f ]+$"',
                            'cryptsetup open --test-passphrase --key-file /tmp/bad /dev/sda3 2>/dev/null && fail "wrong passphrase accepted"',
                            'cryptsetup open --key-file /tmp/pass /dev/sda3 inst || fail "open with the installer passphrase"',
                            'fsck.vfat -n /dev/mapper/inst > /tmp/fsck.out 2>&1 || { cat /tmp/fsck.out; fail "fsck.vfat"; }',
                            'mkdir -p /mnt/inst && mount -o ro /dev/mapper/inst /mnt/inst || fail "mount"',
                            'grep -q "written in the live session" /mnt/inst/home/nexus/live-proof.txt || fail "live file"',
                            'umount /mnt/inst; cryptsetup close inst || fail "close"', 'echo "[64] FSTEST ALL PASSED"', ''])
        ok, dlog = debian_tools_vm.run_script(WORK, script, [nvme], tag='encrypt-cryptsetup')
        dump = '\n'.join(l for l in dlog.splitlines() if '[64] DUMP' in l or 'FSTEST' in l)
        check('encrypt: Debian cryptsetup opens the NexusOS-formatted LUKS2 volume (wrong passphrase refused), fsck.vfat clean, live file there',
              ok and 'argon2id' in dump and 'aes-xts-plain64' in dump, dump[-3000:] or dlog[-3000:])
        rc, log = boot('encrypt-3-boot', [(nvme, 'nvme', 0, 'NXCRYPT01')], keys=[(r'CRYPTROOT PROMPT', 'wrong passphrase\n'),
                       (r'Wrong passphrase', ENCRYPT_PASS + '\n')], timeout=1200)   # the prompt is back at once (boot() waits 1 s)
        check('encrypt: the internal disk alone asks for the passphrase, refuses a wrong one, boots the installed system',
              'CRYPTROOT UNLOCKED' in log and '[64] OSTEST INSTALLED SYSTEM PASSED: root /dev/mapper/cryptroot' in log and faultless(log), log[-3000:])
        # the installed session picked a German keyboard (Aurora's keyboard.conf): the shutdown wrote vconsole.keymap=de
        # into the disk's boot command line, so the next prompt reads German keys (the US key sequence is now wrong)
        check('encrypt: the keyboard chosen in the session reaches the boot command line at shutdown',
              'OSTEST KEYBOARD set to de' in log and '[64] KEYMAP boot prompt layout us -> de: boot command line updated' in log,
              '\n'.join(l for l in log.splitlines() if 'KEYMAP' in l or 'KEYBOARD' in l))
        sys.path.insert(0, str(ROOT / 'tests')); import qmpkeys
        rc, log = boot('encrypt-4-keymap', [(nvme, 'nvme', 0, 'NXCRYPT01')], keys=[(r'CRYPTROOT PROMPT', ENCRYPT_PASS + '\n'),
                       (r'Wrong passphrase', lambda q: qmpkeys.type_layout(q, ENCRYPT_PASS + '\n', 'de'))], timeout=1200)
        check('encrypt: the next boot prompt uses the German layout (US keys refused, German keys open the root)',
              '[64] KEYMAP de (German, from vconsole.keymap)' in log and 'Wrong passphrase' in log and 'CRYPTROOT UNLOCKED' in log and
              'OSTEST KEYBOARD de configured' in log and '[64] OSTEST INSTALLED SYSTEM PASSED: root /dev/mapper/cryptroot' in log and faultless(log), log[-3000:])
    finally:
        for path in (root_img, stick, nvme, small): path.unlink(missing_ok=True)


def spotlight(command):
    def act(q):
        q.cmd('send-key', {'keys': [{'type': 'qcode', 'data': 'ctrl'}, {'type': 'qcode', 'data': 'spc'}]}); time.sleep(1.5)
        q.type(command); time.sleep(1); q.type('\n')
    return act


def case_desktop():
    """The Aurora desktop from a stick: SeaBIOS boot from USB only, the installer typed in Aurora's Terminal, the copy,
    then the internal disk alone (SeaBIOS and OVMF)."""
    stick, nvme = WORK / 'desktop-usb.img', WORK / 'desktop-nvme.img'
    try:
        info = usbimage.build_image(stick, None, args.desktop_source)
        assert fatput.edit(['fatput', str(stick), 'put', str(OUT / 'desktop.elf'), 'sbin/init', 'put', str(OUT / 'aurora_app.elf'), 'bin/aurora-app']) == 0
        raw(nvme, info['root_start'] * 512 + info['root_sectors'] * 512 + (64 << 20))
        live = [(stick, 'usb', 0, None), (nvme, 'nvme', None, 'NXDESKTOP01')]
        rc, log = boot('desktop-1-live', live, memory='2048', timeout=1200, keys=[
            (r'AURORA READY', lambda q: time.sleep(4)), (r'.', spotlight('terminal')),
            (r'AURORA WINDOW app=\S+ title=Terminal', lambda q: (time.sleep(3), q.type('nexus-os-install nvme0n1 --yes --reboot\n')))])
        check('desktop from the stick only (SeaBIOS): Aurora ready on root sda3', 'AURORA READY' in log and
              re.search(r'\[64\] ROOT sda3 \(root= on the command line, FAT32\) on USB', log), log[-3000:])
        check('Aurora Terminal: nexus-os-install requested nvme0n1 and restarted', 'AURORA COMMAND pid=' in log and
              'REBOOT syscall: restart' in log and 'KERNEL_FAULT' not in log, log[-3000:])
        rc, log = boot('desktop-2-copy', live, memory='2048', timeout=2400,
                       keys=[(r'Type YES and press Enter', 'YES\n'), (r'press Enter to restart', '\n')])
        m = re.search(r'INSTALL COMPLETE target=nvme0n1 root=PARTUUID=(\S+) fat32_sectors=(\d+) ms=(\d+)', log)
        check('kernel copied the desktop system to nvme0n1', m and 'KERNEL_FAULT' not in log, log[-3000:])
        if m: print(f'  copied {int(m.group(2)) * 512 >> 20} MiB in {int(m.group(3)) / 1000:.1f} s', flush=True)
        for tag, uefi in (('desktop-3-nvme-bios', False), ('desktop-4-nvme-uefi', True)):
            rc, log = boot(tag, [(nvme, 'nvme', 0, 'NXDESKTOP01')], uefi=uefi, memory='2048', timeout=3000 if uefi else 900,
                           keys=[(r'AURORA READY', lambda q: time.sleep(3)), (r'.', spotlight('files'))], stop=r'AURORA WINDOW app=\S+ title=Files')
            check(f'{tag}: the installed desktop starts from the internal disk and opens Files', rc is None and 'ROOT nvme0n1p3' in log and
                  'AURORA READY' in log and 'KERNEL_FAULT' not in log and '[FAT32] unclean' not in log, log[-3000:])
    finally:
        for path in (stick, nvme): path.unlink(missing_ok=True)


cases = [c for c in args.cases.split(',') if c]
for case in cases:
    try: {'usbboot': case_usbboot, 'install': case_install, 'encrypt': case_encrypt, 'desktop': case_desktop}[case]()
    except Exception as error: check(case + ' completed', False, repr(error))
failed = [r for r in results if not r[1]]
(WORK / 'results.json').write_text(json.dumps([{'name': n, 'ok': o, 'detail': d} for n, o, d in results], indent=1))
print(('PASS' if not failed else 'FAIL') + f' USB boot / OS install: {len(results)} checks, {len(failed)} failed', flush=True)
sys.exit(1 if failed else 0)
