"""V4L2 capture on native NexusOS: /dev/video0 from the virtual test camera (arch/x86_64/vcam.c, boot option vcam) through
the V4L2 core (arch/x86_64/v4l2.c), exercised by arch/x86_64/camera_user.c (built against the real linux/videodev2.h)
in the kernel's /nexus/test-hw campaign, which afterwards checks that every buffer page came back.

Usage: python -I tests/test_x64_camera.py [WORK] [--tcg] [--smp N] [--qmp-port N] [--only CASE ...]
  vcam     the full campaign: discovery, formats, controls, YUYV/NV12 streaming with pattern + counter checks at
           30/15 fps, slow reader, hostile requests, close while streaming
  smp      the same on 2 CPUs (the producer runs on the BSP's scheduler loop, readers on any CPU)
  nocam    without vcam: no /dev/video* at all
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
CASES = ['vcam', 'smp', 'nocam']
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\native64-camera'))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4751)
parser.add_argument('--only', nargs='+', choices=CASES, default=CASES)
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland' / 'fatput.py')
program = WORK / 'camera_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'camera_user.c'), '-o', str(program)], check=True)
base = WORK / 'base.img'; shutil.copyfile(OUT / 'apps64.img', base)
fatput.main(['fatput', str(base), 'put', str(program), 'bin/hw64'])


def boot(case, mode, smp=1, cmdline='vcam'):
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
    build.unthrottle(process.pid)      # frame timing: as the desktop launcher does
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
    lines = [line for line in text.splitlines() if 'CAMERA' in line or 'V4L2' in line or 'VCAM' in line]
    print('\n'.join(lines), flush=True)
    assert 'KERNEL_FAULT' not in text and 'USER_FAULT' not in text and '[64] FAIL:' not in text, (case, 'failure (see serial.log)')
    assert '[64] CAMERA TEST PASSED' in text and '[64] HW CAMPAIGN PASSED' in text, (case, 'guest test failed')
    return text


results = {}
try:
    for case in options.only:
        if case in ('vcam', 'smp'):
            text = boot(case, 'v', smp=2 if case == 'smp' else 1)
            assert '[64] V4L2 /dev/video0: NexusOS Virtual Camera' in text
            results[case] = re.findall(r'\[64\] CAMERA stream .*', text)
        else:
            text = boot(case, 'n', cmdline=None)
            assert 'V4L2' not in text
        print('PASS', case, flush=True)
finally:
    base.unlink(missing_ok=True)
(WORK / 'result.json').write_text(json.dumps(results, indent=1))
print('PASS V4L2 capture:', ' '.join(options.only))
