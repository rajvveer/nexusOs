"""Host-check the Aurora compositor damage helpers (arch/x86_64/aurora_damage.h) without booting."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
temporary_root=Path(tempfile.gettempdir()).resolve()
with tempfile.TemporaryDirectory(prefix='nexus-aurora-damage-',dir=temporary_root) as directory:
    work=Path(directory).resolve();assert work.parent==temporary_root and not work.is_symlink()
    executable=work/'damage.exe'
    subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-windows-gnu',
        '-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-fsanitize=undefined','-fno-sanitize-recover=all',str(ROOT/'tests/aurora_damage_test.c'),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True,timeout=120)
print('PASS Aurora damage helpers; temporary host artifacts removed')
