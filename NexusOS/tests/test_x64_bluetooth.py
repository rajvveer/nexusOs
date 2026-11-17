"""Bluetooth HCI layer on native NexusOS (arch/x86_64/bluetooth.c): /dev/vhci virtual controllers and AF_BLUETOOTH
HCI sockets (raw with HCI_FILTER, user channel, monitor, mgmt basics), exercised by arch/x86_64/bluetooth_user.c
(two controllers played by a forked child; see its header) in the kernel's /nexus/test-hw campaign as root
(/nexus/init-as-root on the test disk), which afterwards checks that every page came back.
The rest of the stack (mgmt for bluetoothd, L2CAP, SMP, RFCOMM with Debian's BlueZ, BlueZ's kernel testers, hostile
peers) is tests/test_x64_bluez.py; btusb is tests/test_usbclass_sim.py btusb.

Usage: python -I tests/test_x64_bluetooth.py [WORK] [--tcg] [--qmp-port N] [--only CASE ...]
  hci   1 CPU      smp   2 CPUs
"""
from pathlib import Path
import argparse, importlib.util, json, re, shutil, socket, subprocess, time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CASES = ['hci', 'smp']
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\native64-bluetooth'))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4756)
parser.add_argument('--only', nargs='+', choices=CASES, default=CASES)
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland' / 'fatput.py')
program = WORK / 'bluetooth_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'bluetooth_user.c'), '-o', str(program)], check=True)
base = WORK / 'base.img'; shutil.copyfile(OUT / 'apps64.img', base)
marker = WORK / 'marker'; marker.write_bytes(b'b')
fatput.main(['fatput', str(base), 'put', str(program), 'bin/hw64', 'put', str(marker), 'nexus/test-hw', 'put', str(marker), 'nexus/init-as-root'])


def boot(case, smp):
    work = WORK / case; work.mkdir(exist_ok=True)
    disk = work / 'disk.img'; shutil.copyfile(base, disk)
    serial = work / 'serial.log'; serial.unlink(missing_ok=True)
    command = [QEMU, *build.desktop_cpu_options(options.tcg), '-m', '1024', '-smp', str(smp), '-machine', 'q35',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    process = subprocess.Popen([str(c) for c in command], stdout=(work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    build.unthrottle(process.pid)
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
    print('\n'.join(line for line in text.splitlines() if 'BT' in line or 'BLUETOOTH' in line), flush=True)
    assert 'KERNEL_FAULT' not in text and 'USER_FAULT' not in text and '[64] FAIL:' not in text, (case, 'failure (see serial.log)')
    assert '[64] BT TEST PASSED' in text and '[64] HW CAMPAIGN PASSED' in text, (case, 'guest test failed')
    assert text.count('[64] BLUETOOTH hci') >= 3 and '[64] BLUETOOTH hci0 removed' in text
    return text


try:
    for case in options.only:
        boot(case, 2 if case == 'smp' else 1)
        print('PASS', case, flush=True)
finally:
    base.unlink(missing_ok=True)
print('PASS Bluetooth HCI groundwork:', ' '.join(options.only))
