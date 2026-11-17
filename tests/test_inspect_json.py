"""Check the production native DevTools reader against malformed/ambiguous input."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-cdp-json-') as directory:
    executable=Path(directory)/'inspect-json.exe'
    subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-windows-gnu',
        '-std=c11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/inspect_json_test.c'),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True,timeout=10)
