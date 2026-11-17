"""Compile and run production native input code with simulated ports."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-input-') as directory:
    executable = Path(directory) / 'input.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc',
                    '-target', 'x86_64-windows-gnu', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'tests/input_test.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
