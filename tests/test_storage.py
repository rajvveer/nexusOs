"""Host regression checks for disk migration and long-name preservation."""
from pathlib import Path
import hashlib
import importlib.util
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('storage', ROOT / 'storage.py')
storage = importlib.util.module_from_spec(spec); spec.loader.exec_module(storage)
fatput = storage.fatput


def get(fs, path):
    parent, name = path.rsplit('/', 1)
    entry = fs.find_name(fs.lookup_dir(parent.split('/')), name)[2]
    first = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
    return fs.read_chain(first, struct.unpack_from('<I', entry, 28)[0])


with tempfile.TemporaryDirectory(prefix='nexus-storage-') as directory:
    disk = Path(directory) / 'disk.img'
    protected = bytes(range(256)) * ((storage.BASE-fatput.LIMIT) // 256)
    disk.write_bytes(bytes(fatput.LIMIT) + protected)
    source = Path(directory) / 'input.bin'; body = bytes(range(256)) * 300
    source.write_bytes(body)
    for name in ['simple.bin', 'a b.zip', 'archive.tar.gz', 'Long Name - caf\u00e9.iso']:
        fatput.main(['fatput', str(disk), 'put', str(source), 'Downloads/' + name])
    for size in (64, 128):
        storage.grow(disk, size)
        fs = fatput.Fat32(disk)
        try:
            assert fs.base == storage.BASE and fs.total * 512 + fs.base == disk.stat().st_size
            for name in ['simple.bin', 'a b.zip', 'archive.tar.gz', 'Long Name - caf\u00e9.iso']:
                assert get(fs, 'Downloads/' + name) == body
            assert fs.rd(fs.rsv * 512, fs.fatsz * 512) == fs.rd((fs.rsv + fs.fatsz) * 512, fs.fatsz * 512)
        finally:
            fs.close()
        with disk.open('rb') as file:
            file.seek(fatput.LIMIT); assert file.read(len(protected)) == protected
        print('PASS grow', size, 'MiB: contents, long/UTF-8 names, mirrored FATs, protected region')
    before = storage.digest(disk)
    try:
        storage.grow(disk, 64)
        raise AssertionError('shrink accepted')
    except ValueError:
        pass
    assert storage.digest(disk) == before
    print('PASS shrink refused without changing the image')
