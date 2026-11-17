"""Game controller input on native NexusOS: evdev + joydev (arch/x86_64/evdev.c), uinput (uinput.c), the input sysfs
objects (hwsysfs.c) and the virtual Xbox 360 pad of the "vpad" boot option (vpad.c), exercised by
arch/x86_64/gamepad_user.c (built against the real linux/input.h, uinput.h and joystick.h) in the kernel's
/nexus/test-hw campaign, which afterwards checks that every page came back.
USB HID gamepads (gamepad.c) and Xbox controllers (xpad.c) are covered by tests/test_usbclass_sim.py gamepad: QEMU has
no USB game controller to boot with.

Usage: python -I tests/test_x64_gamepad.py [WORK] [--tcg] [--qmp-port N] [--only CASE ...]
  uinput   uinput gamepad -> evdev/joydev/sysfs, filtering, overflow, grab, joydev maps and correction, force feedback
           through the uinput upload/erase protocol with a second process, console merging, permissions, ioctl fuzz
  smp      the same on 2 CPUs
  vpad     the virtual Xbox 360 pad: discovery, its input pattern through evdev and joydev, rumble (kernel log)
  none     no vpad: no joystick nodes, /dev/uinput present
  udev     Debian's libudev.so.1 (copied read-only from --udev-disk, a desktop disk) enumerates the vpad the way Chrome
           and SDL do (ID_INPUT_JOYSTICK, devnode, parent input device's name and id/vendor/product) and its monitor
           sees a uinput pad come and go (kernel uevents, no udevd)
"""
from pathlib import Path
import argparse
import importlib.util
import json
import re
import shutil
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CASES = ['uinput', 'smp', 'vpad', 'none', 'udev']
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\native64-gamepad'))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4753)
parser.add_argument('--only', nargs='+', choices=CASES, default=CASES)
parser.add_argument('--udev-debug', action='store_true')
parser.add_argument('--udev-disk', type=Path, default=Path(r'D:\nexustest\prod\integ_desk_disk\desktop64.img'))   # read only
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland' / 'fatput.py')
program = WORK / 'gamepad_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'gamepad_user.c'), '-o', str(program)], check=True)
base = WORK / 'base.img'; shutil.copyfile(OUT / 'apps64.img', base)
fatput.main(['fatput', str(base), 'put', str(program), 'bin/hw64'])


def stage_udev():
    """libudev.so.1 and its libcap.so.2 from a desktop disk's /usr/lib/x86_64-linux-gnu (read only) -> /nexus/udev/."""
    assert 'New folder' not in str(options.udev_disk), 'never read the real desktop disk'
    import struct
    fs = fatput.Fat32(str(options.udev_disk), readonly=True)
    directory = fs.lookup_dir(['usr', 'lib', 'x86_64-linux-gnu']); wanted = {}
    for name, _c, _o, entry, _s in fs.named_entries(directory):
        for prefix, target in (('libudev.so.1.', 'libudev.so.1'), ('libcap.so.2.', 'libcap.so.2')):
            if name.startswith(prefix) and name[len(prefix):].replace('.', '').isdigit():
                first = struct.unpack_from('<H', entry, 26)[0] | struct.unpack_from('<H', entry, 20)[0] << 16
                wanted[target] = fs.read_chain(first, struct.unpack_from('<I', entry, 28)[0])
    assert set(wanted) == {'libudev.so.1', 'libcap.so.2'}, sorted(wanted)
    if options.udev_debug: wanted['debug'] = b'1'                       # SYSTEMD_LOG_LEVEL=debug in the guest program
    for target, data in wanted.items():
        local = WORK / target; local.write_bytes(data)
        fatput.main(['fatput', str(base), 'put', str(local), 'nexus/udev/' + target]); local.unlink()


def boot(case, mode, smp=1, cmdline=None):
    work = WORK / case; work.mkdir(exist_ok=True)
    disk = work / 'disk.img'; shutil.copyfile(base, disk)
    marker = work / 'marker'; marker.write_bytes(mode.encode())
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-hw'])
    serial = work / 'serial.log'; serial.unlink(missing_ok=True)
    command = [QEMU, *build.desktop_cpu_options(options.tcg), '-m', '1024', '-smp', str(smp), '-machine', 'q35',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               *(['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}'] if cmdline else []),
               '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    process = subprocess.Popen([str(c) for c in command], stdout=(work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    build.unthrottle(process.pid)      # timers: as the desktop launcher does
    print(case, 'VM', process.pid, flush=True)
    try:
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            text = serial.read_bytes().decode('latin1') if serial.exists() else ''
            if re.search(r'\[64\] HW CAMPAIGN PASSED|\[64\] FAIL:|KERNEL_FAULT', text) or process.poll() is not None: break
            time.sleep(.2)
        time.sleep(.5)
    finally:
        try:
            with socket.create_connection(('127.0.0.1', options.qmp_port), timeout=5) as link:
                stream = link.makefile('rwb'); stream.readline()
                for request in ({'execute': 'qmp_capabilities'}, {'execute': 'quit'}):
                    stream.write(json.dumps(request).encode() + b'\n'); stream.flush(); stream.readline()
        except OSError: pass
        try: process.wait(30)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        disk.unlink(missing_ok=True)
    text = serial.read_bytes().decode('latin1') if serial.exists() else ''
    lines = [line for line in text.splitlines() if 'GAMEPAD' in line or 'INPUT' in line or 'VPAD' in line]
    print('\n'.join(lines[-60:]), flush=True)
    assert 'KERNEL_FAULT' not in text and 'USER_FAULT' not in text and '[64] FAIL:' not in text, (case, 'failure (see serial.log)')
    assert '[64] GAMEPAD TEST PASSED' in text and '[64] HW CAMPAIGN PASSED' in text, (case, 'guest test failed')
    return text


results = {}
try:
    for case in options.only:
        if case in ('uinput', 'smp'):
            text = boot(case, 'u', smp=2 if case == 'smp' else 1)
            assert '[64] INPUT /dev/input/event2 + /dev/input/js0: NexusOS Test Pad' in text, 'the uinput pad is event2 + js0'
            assert 'INPUT device removed: NexusOS Test Pad' in text
            results[case] = re.findall(r'\[64\] GAMEPAD (?:ff|uinput).*', text)
        elif case == 'udev':
            stage_udev()
            text = boot(case, 'd', cmdline='vpad')
            results[case] = re.findall(r'\[64\] GAMEPAD udev: .*', text)
        elif case == 'vpad':
            text = boot(case, 'p', cmdline='vpad')
            assert '[64] INPUT /dev/input/event2 + /dev/input/js0: Microsoft X-Box 360 pad' in text
            rumble = re.findall(r'\[64\] VPAD rumble strong=(\d+) weak=(\d+)', text)
            assert rumble[:2] == [('49152', '12288'), ('0', '0')], ('rumble reached the pad, stopped after its replay length', rumble)
            results[case] = rumble
        else:
            text = boot(case, 'n')
            assert 'VPAD' not in text and '/dev/input/js' not in text
        print('PASS', case, flush=True)
finally:
    base.unlink(missing_ok=True)
(WORK / 'result.json').write_text(json.dumps(results, indent=1))
print('PASS game controllers:', ' '.join(options.only))
