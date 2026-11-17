"""Compile and run the native Ethernet drivers against register-level simulators on the host (no QEMU model exists
for these chips): tests/rtl8169_sim_test.c (Realtek RTL8111/8168 g+, RTL8125/8126 with the pinned linux-firmware
files in arch/x86_64/firmware) and tests/e1000_sim_test.c (Intel I219 PCH LAN, I225/I226) when present.
UBSan is on; every scenario must print its PASSED line."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
CASES = [('rtl8169_sim_test', [str(ROOT / 'arch/x86_64/firmware/rtl_nic')], 'RTL8169 SIMULATOR TESTS PASSED'),
         ('e1000_sim_test', [], 'E1000 SIMULATOR TESTS PASSED')]
failed = 0
with tempfile.TemporaryDirectory(prefix='nexus-nicsim-') as directory:
    for name, arguments, marker in CASES:
        source = ROOT / 'tests' / (name + '.c')
        if not source.exists():
            continue
        executable = Path(directory) / (name + '.exe')
        subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2', '-g', '-DNEXUS_NATIVE64=1',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(source), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable), *arguments], capture_output=True, text=True, timeout=300)
        tail = result.stdout[-3000:]
        ok = result.returncode == 0 and marker in result.stdout
        print(f"{'PASS' if ok else 'FAIL'} {name} (exit {result.returncode})")
        if not ok:
            print(tail, result.stderr[-3000:])
            failed += 1
        else:
            for line in result.stdout.splitlines():
                if line.endswith('passed') or line.endswith('PASSED'):
                    print('  ' + line)
sys.exit(1 if failed else 0)
