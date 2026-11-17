"""Compile and run arch/x86_64/cpufreq.c against simulated processors (tests/cpufreq_host_test.c): Intel HWP (hybrid),
AMD CPPC (Ryzen 7 9700X figures), acpi-cpufreq on FFixedHW and SystemIO, schedutil, _PPC, thermal cap, _CST/MWAIT."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-cpufreq-') as directory:
    executable = Path(directory) / 'cpufreq.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-I', str(ROOT / 'tests'), str(ROOT / 'tests/cpufreq_host_test.c'), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
    print(result.stdout[-4000:], result.stderr[-4000:])
    assert result.returncode == 0 and 'CPUFREQ HOST TESTS PASSED' in result.stdout, result.returncode
print('PASS cpufreq drivers against simulated processors')
