"""Host-check the actual shared Aurora renderer without booting or touching disk images."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
temporary_root=Path(tempfile.gettempdir()).resolve()
with tempfile.TemporaryDirectory(prefix='nexus-aurora-graphics-',dir=temporary_root) as directory:
    work=Path(directory).resolve();assert work.parent==temporary_root and not work.is_symlink()
    executable=work/'graphics.exe'
    sources=[ROOT/'tests/aurora_graphics_test.c',ROOT/'arch/x86_64/aurora_graphics.c']
    sources += [ROOT/'kernel'/f'{name}.c' for name in ('gui','font','gfx','theme','appui')]
    subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-windows-gnu',
        '-std=c11','-O2','-DNEXUS_HOSTED=1','-DNEXUS_AURORA=1','-Wall','-Wextra','-Werror',
        '-fsanitize=undefined','-fno-sanitize-recover=all',*map(str,sources),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True,timeout=10)
    for invalid in ('negative','overflow','alias','stride','position','scene-canvas','scene-shadow','scene-video',
                    'scene-null','scene-stride','scene-overflow','scene-range','scene-detached','rect-alias','rect-stride','rect-detached'):
        result=subprocess.run([str(executable),invalid],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
        assert result.returncode not in (0,1),(invalid,result.returncode,result.stdout)
print('PASS invalid canvas dimensions, cursor position, video stride and alias rejected; temporary host artifacts removed')
