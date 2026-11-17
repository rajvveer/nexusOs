"""Compile and run the ACPI embedded-controller protocol (arch/x86_64/acpi_ec.c) against a simulated EC
(tests/acpi_ec_host_test.c): byte read/write, burst framing, declined burst, query queue, stuck controller."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-ec-') as directory:
    executable = Path(directory) / 'ec.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(ROOT / 'tests/acpi_ec_host_test.c'), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
    print(result.stdout[-4000:], result.stderr[-4000:])
    assert result.returncode == 0 and 'EC HOST TESTS PASSED' in result.stdout, result.returncode
print('PASS ACPI embedded controller protocol against a simulated controller')
