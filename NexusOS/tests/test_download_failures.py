"""Disk-full and cancellation regression on a deliberately small private disk."""
from pathlib import Path
import importlib.util
import http.server
import shutil
import sys
import threading
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('campaign', ROOT / 'tests/test_downloads.py')
campaign = importlib.util.module_from_spec(spec); spec.loader.exec_module(campaign)
campaign.WORK = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else nexus_paths.runs('failures')
campaign.WORK.mkdir(parents=True, exist_ok=True)
fatput = campaign.fatput
source = campaign.WORK / 'source'; source.mkdir(exist_ok=True)
shutil.copyfile(ROOT / 'nexus.img', source / 'nexus.img')
protected = b'protected-persistence-region' * 4096
with (source / 'disk.img').open('wb') as disk:
    disk.truncate(16 * 1024**2)
    disk.seek(fatput.LIMIT); disk.write(protected)
sentinel = source / 'sentinel.bin'; sentinel.write_bytes(b'keep-existing-file')
fatput.main(['fatput', str(source / 'disk.img'), 'put', str(sentinel), 'Downloads/existing.bin'])


def free_clusters(path):
    fs = fatput.Fat32(path)
    try: return sum(fs.fat_get(c) == 0 for c in range(2, fs.nclusters + 2))
    finally: fs.close()


before = free_clusters(source / 'disk.img')
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), campaign.Server)
server.daemon_threads = True
worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
base = 'http://10.0.2.2:%d' % server.server_address[1]
login = ['sleep:9', 'type:root', 'ret', 'sleep:1', 'type:root', 'ret', 'sleep:3']
try:
    steps = login + ['key:f12', 'sleep:2',
             'cmd:wget ' + base + '/file?n=16777216&name=full.bin', 'waitser:[NET] wget url=', 'shot:full',
             'cmd:wget ' + base + '/slow?n=2097152&name=cancelled.bin', 'sleep:2', 'key:esc', 'sleep:2', 'shot:cancelled',
             'cmd:wget ' + base + '/file?n=1024 "/disk/Downloads/existing.bin"', 'waitser:[NET] wget url=',
             'cmd:wget ' + base + '/broken?name=broken.bin', 'waitser:[NET] wget url=', 'serial']
    disk, log = campaign.run(source, 'errors', steps)
    assert 'err=Disk is full' in log and 'err=Cancelled' in log and 'err=Destination already exists' in log, log[-2000:]
    campaign.verify(disk, 'Downloads/existing.bin', len(b'keep-existing-file'), b'keep-existing-file')
    assert free_clusters(disk) == before, 'failed transfers leaked data clusters'
    fs = fatput.Fat32(disk)
    names = [name.lower() for name, *_ in fs.named_entries(fs.lookup_dir(['Downloads']))]
    fs.close()
    assert sorted(names) == ['.', '..', 'existing.bin'], names
    with open(disk, 'rb') as image:
        image.seek(fatput.LIMIT); assert image.read(len(protected)) == protected
    print('PASS disk full, cancellation, interrupted body, existing file, no leaked clusters, protected bytes', flush=True)
    shutil.copyfile(disk, source / 'disk.img')
    steps = login + ['key:ctrl-spc', 'sleep:1', 'type:browser', 'ret', 'sleep:2',
             'key:ctrl-l', 'type:' + base + '/slow?n=2097152&name=browser-cancelled.bin', 'ret',
             'sleep:3', 'key:esc', 'sleep:2', 'shot:browser_cancel', 'serial']
    disk, _ = campaign.run(source, 'browser_cancel', steps)
    assert free_clusters(disk) == before
    fs = fatput.Fat32(disk)
    names = [name.lower() for name, *_ in fs.named_entries(fs.lookup_dir(['Downloads']))]
    fs.close()
    assert sorted(names) == ['.', '..', 'existing.bin'], names
    print('PASS browser cancellation cleans up temporary file and clusters', flush=True)
finally:
    server.shutdown(); server.server_close(); worker.join()
