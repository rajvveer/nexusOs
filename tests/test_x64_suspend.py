"""System sleep and hibernation end to end on QEMU q35 (-global ICH9-LPC.disable_s3=0), SeaBIOS or OVMF (--uefi).

Usage: python -I tests/test_x64_suspend.py [WORK] [--smp N] [--whpx] [--uefi] [--cycles N] [--only NAME]... [--qmp-port 4631] [--cmdline OPTS]
       [--cache DIR]   (downloads of arch/x86_64/power-lock.json artifacts: util-linux for rtcwake)

/bin/power64 in mode 's' (power_user.c sleep_cycles) writes "mem"/"freeze"/"disk" to /sys/power/state N times; around
every cycle it writes and fsyncs a file, arms a 300 ms ITIMER_REAL, checks CLOCK_MONOTONIC/BOOTTIME/REALTIME, the PM
uevents and (rtc runs) the RTC wake alarm. The harness waits until QEMU reports the guest suspended (query-status), keeps
it asleep, wakes it (system_wakeup or the RTC alarm), and finally checks on the host that every file written before and
after each sleep reached the disk image. Runs:
  s3        S3 cycles woken by system_wakeup (default 3; the leak/fault soak uses --cycles 20)
  rtc       S3 woken by the RTC alarm (/sys/class/rtc/rtc0/wakealarm, +3 s)
  button    a power-button press during the resume (system_powerdown) is the waking press: KEY_WAKEUP, no shutdown
  freeze    suspend-to-idle woken by the power button (system_powerdown -> fixed event SCI)
  devices   e1000e TCP connection to a host echo server, xHCI USB keyboard (re-enumerated), AC97 tones after resume
  hda / e1000 / rtl8139   the same with Intel HDA audio, the 82540EM NIC, the RTL8139 NIC
  ahci      root disk on the ICH9 AHCI controller (pc machine: PIIX IDE)
  gpu       virtio-vga-gl (virgl, hidden SDL GL window): S3 refused (EBUSY) while a virgl resource exists, /bin/gpu64
            renders after the resume
  rtcwake   Debian's util-linux rtcwake -m mem -s 3 (/dev/rtc0 RTC_WKALM_SET), woken by the RTC alarm
  hibernate the image goes to a second disk's MBR partition of type 0xA0, QEMU powers off, the next boot restores it and
            the test program continues; -swsusp: reboot(2) SW_SUSPEND with /sys/power/disk "reboot"; -corrupt (image
            bytes flipped), -kernel (another kernel boots), -disk (root file system changed): discarded, normal boot
"""
from pathlib import Path
import os
import argparse
import importlib.util
import json
import re
import shutil
import socket
import struct
import subprocess
import threading
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('suspend'))
parser.add_argument('--smp', type=int, default=2)
parser.add_argument('--whpx', action='store_true')
parser.add_argument('--cycles', type=int, default=3)
parser.add_argument('--only', action='append')
parser.add_argument('--qmp-port', type=int, default=4631)
parser.add_argument('--machine', default='q35')
parser.add_argument('--cmdline', help='kernel command line (fw_cfg opt/nexus/cmdline), e.g. x2apic')
parser.add_argument('--uefi', action='store_true', help='boot through OVMF (nexus64-uefi.img) instead of SeaBIOS + floppy')
parser.add_argument('--cache', type=Path, default=nexus_paths.cache('power'), help='downloads (power-lock.json artifacts)')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
if options.uefi:   # copies: QEMU option parsing must not see the "Program Files" space
    OVMF_CODE, OVMF_VARS = WORK / 'edk2-x86_64-code.fd', WORK / 'edk2-vars-template.fd'
    shutil.copyfile(Path(QEMU).parent / 'share/edk2-x86_64-code.fd', OVMF_CODE); shutil.copyfile(Path(QEMU).parent / 'share/edk2-i386-vars.fd', OVMF_VARS)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland/fatput.py')
wav_check = module('wav_check', HERE / 'wav_check.py')
program = OUT / 'power_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'power_user.c'), '-o', str(program)], check=True)


class Qmp:
    def __init__(self, port):
        deadline = time.time() + 20
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=60); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(0.2)
        self.f = self.s.makefile('rwb'); self.events = []
        json.loads(self.f.readline()); self.command('qmp_capabilities')

    def command(self, name, **arguments):
        self.f.write(json.dumps({'execute': name, 'arguments': arguments}).encode() + b'\n'); self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if 'event' in reply: self.events.append(reply); continue
            if 'return' in reply: return reply['return']
            if 'error' in reply: raise RuntimeError(f'{name}: {reply["error"]}')

    def close(self):
        try: self.s.close()
        except OSError: pass


def log_of(serial): return serial.read_text(errors='replace') if serial.exists() else ''


def wait_for(vm, serial, pattern, timeout, start=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = log_of(serial)
        if re.search(pattern, text[start:]): return text
        if '[64] POWER FAIL' in text[start:] or 'KERNEL_FAULT' in text or '[64] FAIL' in text:
            raise AssertionError('failure in ' + str(serial) + ':\n' + text[-5000:])
        if vm.poll() is not None: raise AssertionError(f'VM exited {vm.returncode} waiting for {pattern}:\n' + text[-5000:])
        time.sleep(0.1)
    raise AssertionError(f'timeout waiting for {pattern} in {serial}:\n' + log_of(serial)[-5000:])


def wait_status(qmp, status, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if qmp.command('query-status')['status'] == status: return
        time.sleep(0.1)
    raise AssertionError('guest never reached ' + status)


def boot(tag, config, devices=False, root='nvme', extra=(), again=False, floppy=None, files=(), gpu=False):
    """again: boot the same disk once more (hibernation restore), logging to <tag>-again.log; floppy: other boot image."""
    disk = WORK / (tag + '.img'); capture = WORK / (tag + '.wav')
    if not again:
        shutil.copyfile(OUT / 'apps64.img', disk)
        marker = WORK / (tag + '.marker'); marker.write_bytes(config.encode())
        install = ['fatput', str(disk), 'put', str(program), 'bin/power64', 'put', str(marker), 'nexus/test-power']
        capture.unlink(missing_ok=True)
        if devices and audio_program:   # tests/test_x64_audio.py's ALSA client, played after the last resume
            install += ['put', str(audio_program), 'bin/audio64', 'put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2']   # no /nexus/test-audio: that is the kernel's own campaign
            for source in sorted((alsa_root / 'usr/share/alsa').rglob('*')):
                if source.is_file(): install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(alsa_root / 'usr/share/alsa').as_posix()]
        for host, guest in files: install += ['put', str(host), guest]
        fatput.main(install)
    serial = WORK / (tag + ('-again' if again else '') + '.log'); serial.unlink(missing_ok=True)
    machine = options.machine + (',i8042=off' if devices and (not isinstance(devices, dict) or devices.get('usb', True)) else '')   # devices: the USB keyboard is the only keyboard (gets the injected keys)
    if options.uefi:
        firmware = WORK / (tag + '-vars.fd'); shutil.copyfile(OVMF_VARS, firmware)
        loader = ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={firmware}',
                  '-drive', f'if=none,id=esp,format=raw,snapshot=on,file={floppy or OUT / "nexus64-uefi.img"}',
                  '-device', 'ide-hd,drive=esp,bus=' + ('ide.2' if options.machine == 'q35' else 'ide.1') + ',bootindex=0']
    else: loader = ['-drive', f'file={floppy or OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a']
    # gpu: virtio-vga-gl with virgl through a hidden SDL GL window (as tests/test_x64_gpu.py)
    args = [QEMU, '-machine', machine, '-m', '512', '-smp', str(options.smp), '-display', 'sdl,gl=on' if gpu else 'none', '-vga', 'none',
            '-device', 'virtio-vga-gl' if gpu else 'virtio-vga',
            *loader, '-serial', f'file:{serial}',
            '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
            '-global', 'ICH9-LPC.disable_s3=0' if options.machine == 'q35' else 'PIIX4_PM.disable_s3=0',
            '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    if root == 'nvme': args += ['-drive', f'if=none,id=root,file={disk},format=raw', '-device', 'nvme,serial=nexus-root,drive=root']
    else: args += ['-drive', f'if=none,id=root,file={disk},format=raw', '-device', 'ide-hd,drive=root,bus=ide.0']   # q35: ICH9 AHCI
    if devices:   # True: e1000e + xHCI keyboard/tablet + AC97; a dict picks nic / audio ('AC97', 'hda', None) / usb
        chosen = {'nic': 'e1000e', 'audio': 'AC97', 'usb': True, **(devices if isinstance(devices, dict) else {})}
        args += ['-netdev', 'user,id=network', '-device', f'{chosen["nic"]},netdev=network']
        if chosen['usb']: args += ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,id=usbkbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0']
        if chosen['audio']:
            args += ['-audiodev', f'wav,id=snd0,path={capture.name},out.frequency=48000']
            args += ['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'] if chosen['audio'] == 'hda' else ['-device', 'AC97,audiodev=snd0']
    else: args += ['-nic', 'none']
    args += list(extra)
    if options.cmdline: args += ['-fw_cfg', f'name=opt/nexus/cmdline,string={options.cmdline}']
    # the audio client checks real-time playback rates: devices runs use WHPX when the host has it
    whpx = options.whpx or (devices and build.whpx_available())
    args += ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand' + build.host_cpu_flags()] if whpx else ['-cpu', 'max']
    hidden = None
    if gpu: hidden = subprocess.STARTUPINFO(); hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW; hidden.wShowWindow = 0
    vm = subprocess.Popen(args, cwd=WORK, stdout=(WORK / (tag + ('-again' if again else '') + '.qemu.txt')).open('wb'), stderr=subprocess.STDOUT, startupinfo=hidden)
    return vm, serial, Qmp(options.qmp_port), disk, capture


def check_disk(disk, cycles):
    fs = fatput.Fat32(str(disk))
    try:
        directory = fs.lookup_dir(['home', 'nexus'])
        for cycle in range(1, cycles + 1):
            for kind in ('before', 'after'):
                entry = fs.find_name(directory, f'pm-{kind}-{cycle}')
                assert entry is not None, f'pm-{kind}-{cycle} missing on disk'
                record = entry[2]
                first = int.from_bytes(record[20:22], 'little') << 16 | int.from_bytes(record[26:28], 'little')
                size = int.from_bytes(record[28:32], 'little')
                assert fs.read_chain(first, size) == f'cycle {cycle}\n'.encode(), (kind, cycle)
    finally: fs.close()


class EchoServer(threading.Thread):
    """Line echo on 127.0.0.1:8930 (the guest reaches it as 10.0.2.2:8930); records what it saw."""
    def __init__(self):
        super().__init__(daemon=True); self.lines = []
        self.listener = socket.create_server(('127.0.0.1', 8930)); self.listener.settimeout(1)
        self.stop = False

    def run(self):
        while not self.stop:
            try: connection, _ = self.listener.accept()
            except OSError: continue
            with connection:
                connection.settimeout(1); buffered = b''
                while not self.stop:
                    try: data = connection.recv(4096)
                    except socket.timeout: continue
                    except OSError: break
                    if not data: break
                    buffered += data
                    while b'\n' in buffered:
                        line, buffered = buffered.split(b'\n', 1); self.lines.append(line.decode(errors='replace'))
                        connection.sendall(line + b'\n')

    def close(self): self.stop = True; self.listener.close()


def press_a(qmp):
    for down in (True, False):
        qmp.command('input-send-event', events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': 'a'}}}])
        time.sleep(0.05)


def run(tag, config, cycles, wake, devices=False, root='nvme', files=(), gpu=False):
    echo = EchoServer() if devices else None
    if echo: echo.start()
    vm, serial, qmp, disk, capture = boot(tag, config, devices, root, files=files, gpu=gpu)
    try:
        asleep = []; start = 0
        for cycle in range(1, cycles + 1):
            text = wait_for(vm, serial, rf'\[64\] POWER WAIT sleep-{cycle}\b', 180, start)
            start = text.index(f'[64] POWER WAIT sleep-{cycle}', start)
            if wake == 'powerdown':   # suspend-to-idle: the guest keeps running; press the power button once it idles
                wait_for(vm, serial, r'\[64\] PM: entering suspend-to-idle', 60, start)
                time.sleep(1.5); qmp.command('system_powerdown')
            else:
                wait_status(qmp, 'suspended', 60)
                began = time.time()
                if wake == 'qmp': time.sleep(1.5); qmp.command('system_wakeup')
                elif wake == 'button':
                    # QEMU neither wakes for the power button nor keeps PWRBTN_STS across its wake reset, so the press
                    # comes while the kernel resumes (as the waking press's status bit does on a PC): the fixed event
                    # is enabled again once "PM: resumed after" is printed, and the press must count as the wake.
                    time.sleep(1.5); qmp.command('system_wakeup')
                    wait_for(vm, serial, r'\[64\] PM: resumed after', 30, start); qmp.command('system_powerdown')
                else: wait_status(qmp, 'running', 30)   # the RTC alarm wakes it
                asleep.append(time.time() - began)
            if 'usb' in config:
                wait_for(vm, serial, rf'\[64\] POWER WAIT usb-key-{cycle}\b', 120, start)
                for attempt in range(10):
                    press_a(qmp)
                    if re.search(rf'cycle {cycle}: USB keyboard works', log_of(serial)[start:]): break
                    time.sleep(1)
            wait_for(vm, serial, rf'\[64\] POWER SLEEP CYCLE {cycle} PASS', 180, start)
        text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 240)
        code = vm.wait(60); text = log_of(serial)
        assert code == 33 and 'POWER CAMPAIGN STATUS=0x0000000000000000' in text and f'SLEEP ALL PASSED {cycles} cycles' in text, (code, text[-4000:])
        for bad in ('KERNEL_FAULT', '[64] FAIL', 'POWER FAIL', 'ACPI error', 'did not come back', 'failed to initialize', 'did not restart', 'did not leave reset'):
            assert bad not in text, (bad, text[-4000:])
        if wake == 'button':   # the waking press is KEY_WAKEUP only (Linux acpi_button), never a shutdown
            assert text.count('POWER BUTTON pressed (wake)') >= cycles and 'POWER ORDERLY' not in text, text[-4000:]
        if wake != 'powerdown':
            assert text.count('[64] PM: woke from S3') == cycles, text[-4000:]
            assert text.count(f'CPUs online after resume') == (cycles if options.smp > 1 else 0), text[-4000:]
        check_disk(disk, cycles)
        extra = ''
        if echo:
            assert echo.lines[:1] == ['hello'] and all(f'cycle {c}' in echo.lines for c in range(1, cycles + 1)), echo.lines
            extra += f', TCP echo {len(echo.lines)} lines'
        if 'audio' in config:
            assert '[64] AUDIO TEST PASSED' in text and 'audio played after resume' in text, text[-4000:]
            report = wav_check.analyze(capture)
            tones = [round(s['frequency_hz'] / 10) * 10 for s in report['segments'] if s['duration_s'] >= 0.3]
            assert tones[:4] == [440, 880, 660, 330], tones
            extra += ', audio tones ' + '/'.join(str(t) for t in tones[:4]) + ' Hz'
        if 'gpu' in config:
            assert 'S3 refused while a virgl resource exists (EBUSY)' in text and '3D objects in use' in text, text[-4000:]
            assert 'virgl rendering works after resume' in text and '[64] GPU ALL PASSED' in text, text[-4000:]
            extra += ', S3 refused with a live virgl resource, virgl renders after resume'
        print(f'PASS {tag}: {cycles} cycles, {options.smp} CPUs' + (f', asleep {min(asleep):.1f}-{max(asleep):.1f} s' if asleep else '') + extra, flush=True)
    finally:
        qmp.close()
        if echo: echo.close()
        if vm.poll() is None: vm.kill(); vm.wait()
        disk.unlink(missing_ok=True); capture.unlink(missing_ok=True)   # disk hygiene: the audio capture too


AREA_LBA = 2048


def make_area(path, megabytes=320):
    """A second disk whose MBR partition 1 (type 0xA0, from LBA 2048) is the hibernation area."""
    size = megabytes << 20
    with open(path, 'wb') as f:
        f.truncate(size)
        mbr = bytearray(512)
        mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\0\0\0', 0xA0, b'\0\0\0', AREA_LBA, size // 512 - AREA_LBA)
        mbr[510:512] = b'\x55\xaa'; f.write(mbr)


def area_header(path):
    with open(path, 'rb') as f:
        f.seek(AREA_LBA * 512); h = f.read(4096)
    names = ('pfn_limit', 'pages', 'stored_pages', 'blocks', 'bitmap_offset', 'bitmap_bytes', 'data_offset', 'data_bytes')
    return {'magic': h[:8], 'state': struct.unpack_from('<I', h, 12)[0], **dict(zip(names, struct.unpack_from('<8Q', h, 120)))}


def hibernate(tag, variant, config='s disk 1'):
    """Hibernate /bin/power64 mid-test (QEMU powers off), then boot again: 'restore' must continue the test where it
    stopped; 'corrupt' (image bytes flipped), 'kernel' (another kernel boots) and 'disk' (root file system changed
    meanwhile) must discard the image and boot normally."""
    area = WORK / (tag + '-area.img'); make_area(area)
    extra = ['-drive', f'if=none,id=hib,file={area.name},format=raw', '-device', 'nvme,serial=nexus-hiber,drive=hib']
    vm, serial, qmp, disk, _ = boot(tag, config, extra=extra)
    floppy = None
    try:
        text = wait_for(vm, serial, r'\[64\] POWER WAIT sleep-1\b', 180)
        assert 'disk' in re.search(r"/sys/power/state='([^']*)'", text).group(1), text[-3000:]
        text = wait_for(vm, serial, r'\[64\] PM: hibernation image saved', 600)
        code = vm.wait(120)
        if 'swsusp' in config: assert 'hibernation image saved, restarting' in text, text[-3000:]   # /sys/power/disk "reboot"
        header = area_header(area)
        written = re.search(r'hibernation image written: (\d+) pages \((\d+) non-zero, (\d+) MiB\) -> (\d+) MiB in (\d+) ms', text)
        assert header['magic'] == b'NXHIBER1' and header['state'] == 0x56414C49 and written, (header, text[-3000:])
        detail = f'{written.group(1)} pages, {written.group(3)} MiB -> {written.group(4)} MiB in {written.group(5)} ms, QEMU exit {code}'
        print(f'  {tag}: image {detail}', flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()
    if variant == 'corrupt':
        with open(area, 'r+b') as f:
            at = AREA_LBA * 512 + header['data_offset'] + header['data_bytes'] // 2
            f.seek(at); data = bytearray(f.read(16)); f.seek(at); f.write(bytes(b ^ 0x5A for b in data))
    elif variant == 'kernel':
        floppy = WORK / (tag + '-other.img'); image = bytearray((OUT / ('nexus64-uefi.img' if options.uefi else 'nexus64.img')).read_bytes())
        at = image.index(b'custom kernel development target'); image[at] ^= 0x20; floppy.write_bytes(bytes(image))
    elif variant == 'disk':
        changed = WORK / (tag + '.changed'); changed.write_bytes(b'changed while hibernated\n')
        fatput.main(['fatput', str(disk), 'put', str(changed), 'home/nexus/changed-while-hibernated'])
    vm, serial, qmp, _, _ = boot(tag, None, extra=extra, again=True, floppy=floppy)
    try:
        if variant == 'restore':
            text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 600)
            code = vm.wait(60); text = log_of(serial)
            loaded = re.search(r'hibernation image loaded: (\d+) pages \((\d+) in place, (\d+) through spare frames\) in (\d+) ms', text)
            assert loaded and '[64] PM: restored from the hibernation image' in text and 'SLEEP CYCLE 1 PASS' in text, text[-4000:]
            assert code == 33 and 'POWER CAMPAIGN STATUS=0x0000000000000000' in text and 'SLEEP ALL PASSED 1 cycles' in text, (code, text[-4000:])
            assert 'PM: hibernation image discarded: restoring' in text and area_header(area)['state'] != 0x56414C49, 'image not invalidated'
            assert re.search(r'cycle 1 resumed: monotonic \+[0-9.]+ s, boottime \+[0-9.]+ s', text), text[-3000:]
            if options.smp > 1: assert 'CPUs online after resume' in text, text[-3000:]
            for bad in ('KERNEL_FAULT', '[64] FAIL', 'POWER FAIL', 'USER_FAULT'): assert bad not in text, (bad, text[-4000:])
            check_disk(disk, 1)
            resumed = re.search(r'cycle 1 resumed: [^\n]*', text).group(0)
            print(f'PASS {tag}: restored {loaded.group(1)} pages ({loaded.group(2)} in place, {loaded.group(3)} copied) in {loaded.group(4)} ms; {resumed}', flush=True)
        else:
            reason = {'corrupt': 'unreadable, corrupt', 'kernel': 'written by a different kernel', 'disk': 'the disk changed'}[variant]
            text = wait_for(vm, serial, r'\[64\] PM: hibernation image discarded: ' + re.escape(reason), 300)
            assert area_header(area)['state'] != 0x56414C49, 'stale image not invalidated'
            text = wait_for(vm, serial, r'\[64\] POWER WAIT sleep-1\b', 300)   # a normal boot reached the test program again
            assert '[64] PM: restored from the hibernation image' not in text and 'KERNEL_FAULT' not in text, text[-4000:]
            print(f'PASS {tag}: stale image discarded ({reason}), normal boot', flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()
        for path in (disk, area, floppy):
            if path: path.unlink(missing_ok=True)


SWAP_LBA = 2048


def make_swap(path, megabytes):
    """A second disk whose MBR partition 1 (type 0x82 Linux swap, from LBA 2048) holds a mkswap'd area (version 1)."""
    size = megabytes << 20; pages = (size // 512 - SWAP_LBA) // 8
    with open(path, 'wb') as f:
        f.truncate(size)
        mbr = bytearray(512)
        mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\0\0\0', 0x82, b'\0\0\0', SWAP_LBA, size // 512 - SWAP_LBA)
        mbr[510:512] = b'\x55\xaa'; f.write(mbr)
        header = bytearray(4096)
        struct.pack_into('<III', header, 1024, 1, pages - 1, 0)          # version, last page, no bad pages
        header[1036:1052] = bytes(range(0x10, 0x20)); header[1052:1058] = b'nxswap'; header[4086:4096] = b'SWAPSPACE2'
        f.seek(SWAP_LBA * 512); f.write(header)


def swap_signature(path):
    with open(path, 'rb') as f: f.seek(SWAP_LBA * 512 + 4086); return f.read(10)


def hibernate_swap(tag, variant):
    """Hibernation into a swap partition with memory under pressure (part of it swapped out first): 'restore' continues
    the test with every page intact and SWAPSPACE2 back; 'corrupt' flips image bytes: discarded, SWAPSPACE2 back, a normal
    boot; 'full' (a swap area too small for the image): refused with ENOSPC, the system and its pages run on; 'lockdown'
    (lockdown=integrity): no "disk" in /sys/power/state, the request gets EPERM. pm.test-nvs: the EBDA tail is an ACPI NVS
    stand-in whose content every boot changes; the restore must put the hibernated value back."""
    sizes = {'restore': 1024, 'corrupt': 1024, 'full': 160, 'lockdown': 1024, 'squeeze': 1536}
    area = WORK / (tag + '-swap.img'); make_swap(area, sizes[variant])
    # full: 300 MiB of random (incompressible) pages: the image cannot fit the 160 MiB area -> ENOSPC while writing;
    # squeeze: 1 GiB of pages in 512 MiB: RAM is full, the kernel must swap out before the image fits the restoring kernel
    config = 's disk 1 ' + ('pressure-random=300 expect-error=28' if variant == 'full' else 'pressure=1024' if variant == 'squeeze' else 'pressure=640')
    restore = variant in ('restore', 'squeeze')
    if variant == 'lockdown': config += ' expect-error=1'
    cmdline = 'pm.test-nvs=0x9ffc0' + (' lockdown=integrity' if variant == 'lockdown' else '')
    extra = ['-drive', f'if=none,id=swp,file={area.name},format=raw', '-device', 'nvme,serial=nexus-swap,drive=swp',
             '-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
    fstab = WORK / (tag + '.fstab'); fstab.write_bytes(b'# swap for the hibernation test (activated by the kernel before the campaign)\nUUID=10111213-1415-1617-1819-1a1b1c1d1e1f none swap sw 0 0\n')
    vm, serial, qmp, disk, _ = boot(tag, config, extra=extra, files=[(fstab, 'etc/fstab')])
    try:
        if variant in ('full', 'lockdown'):
            text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 900); code = vm.wait(60); text = log_of(serial)
            assert code == 33 and 'POWER CAMPAIGN STATUS=0x0000000000000000' in text and 'refused as expected' in text, (code, text[-4000:])
            assert 'pages intact after the refused hibernation' in text and swap_signature(area) == b'SWAPSPACE2', text[-3000:]
            states = re.search(r"/sys/power/state='([^']*)'", text).group(1)
            if variant == 'lockdown': assert 'disk' not in states and 'kernel is locked down' in text, (states, text[-3000:])
            else: assert 'no room for the hibernation image' in text or 'compressed image must fit' in text or 'not possible' in text, text[-3000:]
            for bad in ('KERNEL_FAULT', '[64] FAIL', 'POWER FAIL', 'USER_FAULT'): assert bad not in text, (bad, text[-4000:])
            print(f'PASS {tag}: refused ({variant}), the system ran on with its swapped pages intact', flush=True)
            return
        text = wait_for(vm, serial, r'\[64\] PM: hibernation image saved', 900)
        code = vm.wait(120); saved = log_of(serial)
        assert swap_signature(area) == b'NXSUSPEND\0', swap_signature(area)
        written = re.search(r'hibernation image written: (\d+) pages \((\d+) non-zero, (\d+) MiB\) -> (\d+) MiB in (\d+) ms', text)
        assert written and 'pressure: ' in text and re.search(r'/dev/nvme1n1p1\s+partition\s+\d+\s+[1-9]\d*', text), text[-4000:]   # pages were in swap
        print(f'  {tag}: image {written.group(1)} pages -> {written.group(4)} MiB into swap, QEMU exit {code}', flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()
    if variant == 'corrupt':   # flip bytes in the middle of the data: the extent table says where (find the first block)
        with open(area, 'r+b') as f:
            f.seek(SWAP_LBA * 512 + 4096 - 40); _, first, count = struct.unpack('<IQQ', f.read(20))
            f.seek((SWAP_LBA + first) * 512 + 4096 * 4); data = bytearray(f.read(4096))
            f.seek((SWAP_LBA + first) * 512 + 4096 * 4 + 64 * 1024); f.write(bytes(b ^ 0x5A for b in data))
    vm, serial, qmp, _, _ = boot(tag, None, extra=extra, again=True)
    try:
        if restore:
            text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 900)
            code = vm.wait(60); text = log_of(serial)
            assert re.search(r'hibernation image found on nvme1n1p1 \(swap, \d+ extent', text) and '[64] PM: restored from the hibernation image' in text, text[-4000:]
            assert code == 33 and 'POWER CAMPAIGN STATUS=0x0000000000000000' in text and 'pages intact after the resume' in text, (code, text[-4000:])
            assert '[64] PM: NVS test marker restored' in text and 'changed by the firmware' in text, text[-3000:]
            assert swap_signature(area) == b'SWAPSPACE2', 'the swap signature was not restored'
            for bad in ('KERNEL_FAULT', '[64] FAIL', 'POWER FAIL', 'USER_FAULT', 'NVS test marker LOST'): assert bad not in text, (bad, text[-4000:])
            check_disk(disk, 1)
            if variant == 'squeeze': assert re.search(r'PM: freeing \d+ pages for the image', saved) and re.search(r'PM: \d+ pages freed', saved), saved[-3000:]
            print(f'PASS {tag}: restored from the swap partition, swapped-out and in-memory pages intact, ACPI NVS restored, SWAPSPACE2 back'
                  + (' (memory freed into swap first)' if variant == 'squeeze' else ''), flush=True)
        else:
            text = wait_for(vm, serial, r'\[64\] PM: hibernation image discarded', 300)
            text = wait_for(vm, serial, r'\[64\] POWER WAIT sleep-1\b', 300)
            assert swap_signature(area) == b'SWAPSPACE2' and '[64] PM: restored from the hibernation image' not in text and 'KERNEL_FAULT' not in text, text[-4000:]
            assert re.search(r'\[64\] SWAP Adding \d+k swap on /dev/nvme1n1p1', text), text[-3000:]   # the area is usable again
            print(f'PASS {tag}: corrupt image in swap discarded, SWAPSPACE2 back, swapon works, normal boot', flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()
        for path in (disk, area): path.unlink(missing_ok=True)


def hibernate_swapfile(tag, by_uuid=False):
    """Hibernation into a swap file on the FAT32 root (/swapfile in /etc/fstab), found again through resume=/dev/nvme0n1p1
    and resume_offset= (its header page within the partition, in 4 KiB pages; a pad file puts it on a page boundary).
    The image must be restored and the file's header must read SWAPSPACE2 afterwards."""
    pages = 28 * 256; swapfile = WORK / (tag + '.swapfile')
    header = bytearray(4096); struct.pack_into('<III', header, 1024, 1, pages - 1, 0); header[4086:4096] = b'SWAPSPACE2'
    swapfile.write_bytes(bytes(header) + bytes((pages - 1) * 4096))
    disk = WORK / (tag + '.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / (tag + '.marker'); marker.write_bytes(b's disk 1')
    fstab = WORK / (tag + '.fstab'); fstab.write_bytes(b'/swapfile none swap sw 0 0\n')
    fatput.main(['fatput', str(disk), 'put', str(program), 'bin/power64', 'put', str(marker), 'nexus/test-power', 'put', str(fstab), 'etc/fstab'])
    start = part_start(disk)
    with open(disk, 'rb') as f: f.seek(start * 512); boot_sector = f.read(512)
    spc, reserved, fats, fatsz = boot_sector[13], struct.unpack_from('<H', boot_sector, 14)[0], boot_sector[16], struct.unpack_from('<I', boot_sector, 36)[0]
    data = reserved + fats * fatsz
    fs = fatput.Fat32(str(disk))
    try: free = next(c for c in range(2, fs.nclusters + 2) if fs.fat_get(c) == 0)
    finally: fs.close()
    pad = next(k for k in range(64) if (data + (free + k - 2) * spc) % 8 == 0)   # clusters before /swapfile's first one
    if pad:
        padding = WORK / (tag + '.pad'); padding.write_bytes(b'\x01' * 512 * spc * pad)
        fatput.main(['fatput', str(disk), 'put', str(padding), 'nexus/swapfile.pad'])
    fatput.main(['fatput', str(disk), 'put', str(swapfile), 'swapfile'])
    fs = fatput.Fat32(str(disk))
    try:
        record = fs.find_name(fs.lookup_dir([]), 'swapfile')[2]
        first = int.from_bytes(record[20:22], 'little') << 16 | int.from_bytes(record[26:28], 'little')
        chain = fs.chain(first)
        assert chain == list(range(first, first + len(chain))), 'swap file not contiguous'
    finally: fs.close()
    sector = data + (first - 2) * spc
    assert sector % 8 == 0, ('swap file header not page aligned', sector)
    serial = struct.unpack_from('<I', boot_sector, 0x43)[0]   # by_uuid: resume=UUID=<the FAT volume serial> (Linux's swap-file form)
    device = f'UUID={serial >> 16:04X}-{serial & 0xFFFF:04X}' if by_uuid else '/dev/nvme0n1p1'
    extra = ['-fw_cfg', f'name=opt/nexus/cmdline,string=resume={device} resume_offset={sector // 8}']
    vm, serial, qmp, _, _ = boot(tag, None, extra=extra, again=True)
    try:
        text = wait_for(vm, serial, r'\[64\] PM: hibernation image saved', 900); code = vm.wait(120)
        assert re.search(r'\[64\] SWAP Adding \d+k swap on /swapfile', text) and f'offset {sector // 8}' in text, text[-3000:]
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()
    with open(disk, 'rb') as f: f.seek((start + sector) * 512 + 4086); signature = f.read(10)
    assert signature == b'NXSUSPEND\0', signature
    print(f'  {tag}: image written into /swapfile (resume_offset={sector // 8}), QEMU exit {code}', flush=True)
    (WORK / (tag + '-again.log')).replace(WORK / (tag + '-first.log'))
    vm, serial, qmp, _, _ = boot(tag, None, extra=extra, again=True)
    try:
        text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 900); code = vm.wait(60); text = log_of(serial)
        assert re.search(r'hibernation image found on nvme0n1p1 \(swap', text) and '[64] PM: restored from the hibernation image' in text, text[-4000:]
        assert code == 33 and 'POWER CAMPAIGN STATUS=0x0000000000000000' in text and 'SLEEP ALL PASSED 1 cycles' in text, (code, text[-4000:])
        for bad in ('KERNEL_FAULT', '[64] FAIL', 'POWER FAIL', 'USER_FAULT'): assert bad not in text, (bad, text[-4000:])
        with open(disk, 'rb') as f: f.seek((start + sector) * 512 + 4086); signature = f.read(10)
        assert signature == b'SWAPSPACE2', signature
        print(f'PASS {tag}: restored from /swapfile on the FAT32 root (resume={device} resume_offset={sector // 8}), SWAPSPACE2 back', flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()
        for path in (disk, swapfile): path.unlink(missing_ok=True)


def part_start(disk):
    with open(disk, 'rb') as f: return struct.unpack_from('<I', f.read(512), 446 + 8)[0]


def rtcwake_binary():
    """util-linux's /usr/sbin/rtcwake from the Debian package pinned in arch/x86_64/power-lock.json (downloaded once)."""
    import hashlib, io, lzma, tarfile, urllib.request
    entry = json.loads((HERE / 'power-lock.json').read_text())['util-linux']
    options.cache.mkdir(parents=True, exist_ok=True)
    deb = options.cache / entry['url'].rsplit('/', 1)[1]
    if not deb.exists():
        assert entry['url'].startswith('https://deb.debian.org/'), entry['url']
        deb.write_bytes(urllib.request.urlopen(entry['url'], timeout=300).read())
    data = deb.read_bytes()
    assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256'], 'util-linux package does not match power-lock.json'
    position, members = 8, {}
    while position < len(data):
        name, size = data[position:position + 16].decode().strip(), int(data[position + 48:position + 58])
        members[name] = data[position + 60:position + 60 + size]; position += 60 + size + (size & 1)
    with tarfile.open(fileobj=io.BytesIO(lzma.decompress(members['data.tar.xz']))) as archive:
        binary = archive.extractfile(entry['member']).read()
    assert hashlib.sha256(binary).hexdigest() == entry['member_sha256']
    out = WORK / 'rtcwake'; out.write_bytes(binary); return out


alsa_root = OUT / 'chrome-root'
libasound = alsa_root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
audio_program = None
if libasound.exists() and (not options.only or 'devices' in options.only or 'hda' in options.only):
    audio_program = OUT / 'audio_user.elf'
    subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(HERE / 'audio_user.c'), str(libasound), '-lm', '-o', str(audio_program)], check=True)
runs = {'s3': lambda: run('s3', f's mem {options.cycles}', options.cycles, 'qmp'),
        'rtc': lambda: run('rtc', 's mem 1 3', 1, 'rtc'),
        'button': lambda: run('button', 's mem 2 button', 2, 'button'),
        'freeze': lambda: run('freeze', 's freeze 2', 2, 'powerdown'),
        'devices': lambda: run('devices', 's mem 2 0 net usb' + (' audio' if audio_program else ''), 2, 'qmp', devices=True),
        'hda': lambda: run('hda', 's mem 2 0 net' + (' audio' if audio_program else ''), 2, 'qmp', devices={'audio': 'hda', 'usb': False}),
        'e1000': lambda: run('e1000', 's mem 2 0 net', 2, 'qmp', devices={'nic': 'e1000', 'audio': None, 'usb': False}),
        'rtl8139': lambda: run('rtl8139', 's mem 2 0 net', 2, 'qmp', devices={'nic': 'rtl8139', 'audio': None, 'usb': False}),
        'ahci': lambda: run('ahci', 's mem 2', 2, 'qmp', root='ahci'),
        'gpu': lambda: run('gpu', 's mem 1 0 gpu', 1, 'qmp', files=[(alsa_root / 'usr/lib/x86_64-linux-gnu/libdrm.so.2.124.0', 'lib/x86_64-linux-gnu/libdrm.so.2')], gpu=True),
        'rtcwake': lambda: run('rtcwake', 's rtcwake 1 3', 1, 'rtc', files=[(rtcwake_binary(), 'usr/sbin/rtcwake')]),
        'hibernate': lambda: hibernate('hibernate', 'restore'),
        'hibernate-corrupt': lambda: hibernate('hibernate-corrupt', 'corrupt'),
        'hibernate-kernel': lambda: hibernate('hibernate-kernel', 'kernel'),
        'hibernate-disk': lambda: hibernate('hibernate-disk', 'disk'),
        'hibernate-swsusp': lambda: hibernate('hibernate-swsusp', 'restore', 's swsusp 1'),
        'hibernate-swap': lambda: hibernate_swap('hibernate-swap', 'restore'),
        'hibernate-swap-corrupt': lambda: hibernate_swap('hibernate-swap-corrupt', 'corrupt'),
        'hibernate-swap-full': lambda: hibernate_swap('hibernate-swap-full', 'full'),
        'hibernate-lockdown': lambda: hibernate_swap('hibernate-lockdown', 'lockdown'),
        'hibernate-swap-squeeze': lambda: hibernate_swap('hibernate-swap-squeeze', 'squeeze'),
        'hibernate-swapfile': lambda: hibernate_swapfile('hibernate-swapfile'),
        'hibernate-swapfile-uuid': lambda: hibernate_swapfile('hibernate-swapfile-uuid', by_uuid=True)}
for name, function in runs.items():
    if options.only and name not in options.only: continue
    function()
print('PASS system sleep', flush=True)
