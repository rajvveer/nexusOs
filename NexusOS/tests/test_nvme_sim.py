"""Compile and run the native NVMe driver (arch/x86_64/nvme.c) against a simulated controller (tests/nvme_sim_test.c):
queues, PRP lists, APST policy, timeouts/fatal status/error completions with reset and retry, surprise removal,
MDTS splitting, bounce buffers and the shutdown notification. No QEMU needed."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-nvme-') as directory:
    executable = Path(directory) / 'nvme.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O1',
                    '-DNEXUS_NATIVE64=1', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(ROOT / 'tests/nvme_sim_test.c'), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=300)
    print(result.stdout[-3000:], result.stderr[-3000:])
    assert result.returncode == 0 and 'NVME SIMULATOR TESTS PASSED' in result.stdout, result.returncode
print('PASS NVMe driver against the simulated controller')
