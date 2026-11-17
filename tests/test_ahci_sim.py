"""Compile and run the native AHCI driver (arch/x86_64/ahci.c) against a simulated HBA (tests/ahci_sim_test.c): BIOS
handoff, link debounce, COMRESET, link power management, IDENTIFY/DMA/FLUSH, error and timeout recovery, 32-bit
HBAs, polling, ATAPI and RAID-mode controllers. No QEMU needed."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-ahci-') as directory:
    executable = Path(directory) / 'ahci.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O1',
                    '-DNEXUS_NATIVE64=1', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(ROOT / 'tests/ahci_sim_test.c'), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=300)
    print(result.stdout[-3000:], result.stderr[-3000:])
    assert result.returncode == 0 and 'AHCI SIMULATOR TESTS PASSED' in result.stdout, result.returncode
print('PASS AHCI driver against the simulated HBA')
