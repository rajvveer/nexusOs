"""Compile and run the USB class drivers against simulated devices on the host (QEMU has no UAC2, capture-capable
UAC, UVC or game-controller models): tests/uaudio_sim_test.c (arch/x86_64/uaudio.c), tests/uvc_sim_test.c (uvc.c),
tests/gamepad_sim_test.c (gamepad.c, xpad.c) when present. UBSan is on; every program must print its PASSED line.
Usage: python -I tests/test_usbclass_sim.py [NAME ...]"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
CASES = [('uaudio_sim_test', 'UAUDIO SIMULATOR TESTS PASSED'), ('uvc_sim_test', 'UVC SIMULATOR TESTS PASSED'),
         ('gamepad_sim_test', 'GAMEPAD SIMULATOR TESTS PASSED'), ('btusb_sim_test', 'BTUSB SIMULATOR TESTS PASSED')]
wanted = set(sys.argv[1:])
failed = 0
with tempfile.TemporaryDirectory(prefix='nexus-classsim-') as directory:
    for name, marker in CASES:
        source = ROOT / 'tests' / (name + '.c')
        if not source.exists() or (wanted and name not in wanted and name.replace('_sim_test', '') not in wanted): continue
        executable = Path(directory) / (name + '.exe')
        subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O1', '-g', '-DNEXUS_NATIVE64=1',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(source), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=900)
        ok = result.returncode == 0 and marker in result.stdout
        print(f"{'PASS' if ok else 'FAIL'} {name} (exit {result.returncode})")
        for line in result.stdout.splitlines():
            if 'PASSED' in line or line.startswith('hostile'): print('  ' + line)
        if not ok:
            print(result.stdout[-4000:], result.stderr[-4000:]); failed += 1
sys.exit(1 if failed else 0)
