"""Compile and run the production HD Audio driver (arch/x86_64/hda.c) against simulated controllers and codecs, including
the corpus of real codec dumps in tests/fixtures/hda (converted to C by tests/hda_dump_to_c.py)."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-hda-') as directory:
    header = Path(directory) / 'hda_dumps.h'
    subprocess.run([sys.executable, '-I', str(ROOT / 'tests/hda_dump_to_c.py'), str(header),
                    *sorted(str(p) for p in (ROOT / 'tests/fixtures/hda').glob('*.txt'))], check=True)
    executable = Path(directory) / 'hda.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2',
                    '-DNEXUS_NATIVE64=1', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-I', directory, str(ROOT / 'tests/hda_codec_test.c'), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
    print(result.stdout[-4000:], result.stderr[-4000:])
    assert result.returncode == 0 and 'HDA CODEC TESTS PASSED' in result.stdout, result.returncode
print('PASS HDA controller/codec driver against simulated hardware and the real-codec corpus')
