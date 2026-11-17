"""Compile and run the UEFI loader's display selection (arch/x86_64/uefi/bootx64.c) against simulated firmware
(tests/uefi_gop_test.c): EDID-native modes, ConSplitter consoles, video= options, VM EDIDs, broken EDIDs, pixel
layouts, padded scan lines, SetMode failures. No QEMU: OVMF has no EDID protocol, so this is where that path runs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-gop-') as directory:
    executable = Path(directory) / 'gop.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O1',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fno-builtin', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(ROOT / 'tests/uefi_gop_test.c'), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
    print(result.stdout[-3000:], result.stderr[-3000:])
    assert result.returncode == 0 and 'UEFI GOP TESTS PASSED' in result.stdout, result.returncode
print('PASS UEFI loader display selection against simulated firmware')
