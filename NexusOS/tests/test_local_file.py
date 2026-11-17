from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-local-file-') as directory:
    exe=Path(directory)/'test.exe'
    subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-windows-gnu','-O2','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',str(ROOT/'tests/local_file_test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
