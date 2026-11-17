"""Run userland workflows on image copies and verify saved output bytes.

Usage: python -I userland/tests/test_vm.py [image-directory] [work-directory]
"""
from pathlib import Path
import importlib.util
import socket
import shutil
import struct
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tests')); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
project = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT
work = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else nexus_paths.runs('dailyuse_runtime')
expected = {'check.ok': b'SYSCHECK ALL PASSED\n', 'lua.ok': b'LUA ALL PASSED\n',
            'pipe.txt': b'1 2 12\n', 'game.ok': b'GAMES ALL PASSED\n', 'edit.txt': b'Edited in NexusOS\n'}
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fatput)
source = work / 'source'
source.mkdir(parents=True, exist_ok=True)
for name in ('nexus.img', 'disk.img'):
    shutil.copyfile(project / name, source / name)
fs = fatput.Fat32(source / 'disk.img')
try:
    for name in expected:
        if fs.find(fs.rootc, fs.to83(name)):
            fs.rm(name)
finally:
    fs.close()
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
steps = ['sleep:9', 'type:root', 'ret', 'sleep:1', 'type:root', 'ret', 'sleep:3', 'key:f12', 'sleep:2',
         'cmd:syscheck', 'sleep:8', 'shot:syscheck',
         'cmd:lua /disk/tests/check.lua', 'sleep:3', 'shot:lua',
         'cmd:sh -c "echo hello nexus | wc > /disk/pipe.txt"', 'sleep:3', 'shot:pipe',
         'cmd:cat /disk/pipe.txt', 'sleep:1', 'shot:pipefile',
         'cmd:2048 --test', 'sleep:2', 'shot:games',
         'cmd:kilo /disk/edit.txt', 'sleep:3', 'type:Edited in NexusOS', 'key:ctrl-s',
         'sleep:2', 'shot:kilo', 'key:ctrl-q', 'sleep:2', 'key:ret',
         'cmd:cat /disk/edit.txt', 'sleep:1', 'shot:saved', 'serial']
subprocess.run([sys.executable, '-u', '-I', str(ROOT / 'handoff/ws_tls/harness57.py'),
                str(source), str(work), str(port), 'apps', 'virtio', ';'.join(steps)], check=True)
serial = (work / 'serial_apps.log').read_bytes()
assert b'!EX:' not in serial and b'!KR:' not in serial
fs = fatput.Fat32(work / 'apps_disk.img')
try:
    for name, data in expected.items():
        hit = fs.find(fs.rootc, fs.to83(name))
        assert hit, f'missing output: {name}'
        e = hit[2]
        first = (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]
        actual = fs.read_chain(first, struct.unpack_from('<I', e, 28)[0])
        assert actual == data, f'{name}: {actual!r} != {data!r}'
        print('PASS', name, flush=True)
finally:
    fs.close()
print('PASS all userland workflows; no kernel faults')
