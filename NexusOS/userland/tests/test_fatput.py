"""Host checks: installation is atomic and never changes the persistence window."""
from pathlib import Path
import importlib.util
import hashlib
import struct
import tempfile

spec = importlib.util.spec_from_file_location('fatput', Path(__file__).resolve().parents[1] / 'fatput.py')
fatput = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fatput)


def digest(p):
    return hashlib.sha256(p.read_bytes()).digest()


with tempfile.TemporaryDirectory(prefix='nexus-fat-') as td:
    work = Path(td)
    disk = work / 'disk.img'
    tail = b'PERSIST-DO-NOT-TOUCH' * 1024
    disk.write_bytes(bytes(fatput.LIMIT) + tail)
    source = work / 'app.elf'
    source.write_bytes(bytes(range(256)) * 37)
    fatput.main(['fatput', str(disk), 'put', str(source), 'bin/demo'])
    assert disk.read_bytes()[fatput.LIMIT:] == tail
    fs = fatput.Fat32(disk)
    directory = fs.lookup_dir(['bin'])
    entry = fs.find(directory, fs.to83('demo'))[2]
    first = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
    assert fs.read_chain(first, struct.unpack_from('<I', entry, 28)[0]) == source.read_bytes()
    fs.close()
    before = digest(disk)
    source.write_bytes(bytes(12 * 1024 * 1024))
    try:
        fatput.main(['fatput', str(disk), 'put', str(source), 'bin/demo'])
        raise AssertionError('full volume accepted')
    except SystemExit:
        pass
    assert digest(disk) == before, 'failed update damaged original disk'
    with disk.open('r+b') as f:
        f.write(b'broken!')
        f.seek(510); f.write(b'xx')
    before = digest(disk)
    try:
        fatput.main(['fatput', str(disk), 'put', str(source), 'bin/demo'])
        raise AssertionError('corrupt nonempty filesystem accepted')
    except ValueError:
        pass
    assert digest(disk) == before, 'corrupt filesystem was reformatted'
    # rmdir: empty directories only, clusters freed, the volume stays consistent
    disk2 = work / 'rmdir.img'; disk2.write_bytes(bytes(fatput.LIMIT) + tail)
    small = work / 'small.txt'; small.write_bytes(b'x' * 100)
    fatput.main(['fatput', str(disk2), 'mkdir', 'home/nexus/.fccache-build', 'put', str(small), 'home/nexus/.fccache-build/a long name.txt'])
    fs = fatput.Fat32(disk2); free_full = sum(1 for c in range(2, fs.nclusters + 2) if fs.fat_get(c) == 0); fs.close()
    before = digest(disk2)
    try:
        fatput.main(['fatput', str(disk2), 'rmdir', 'home/nexus/.fccache-build'])
        raise AssertionError('rmdir of a non-empty directory accepted')
    except SystemExit:
        pass
    assert digest(disk2) == before, 'failed rmdir changed the disk'
    try:
        fatput.main(['fatput', str(disk2), 'rmdir', 'home/nexus/.fccache-build/a long name.txt'])
        raise AssertionError('rmdir of a file accepted')
    except ValueError:
        pass
    fatput.main(['fatput', str(disk2), 'rm', 'home/nexus/.fccache-build/a long name.txt', 'rmdir', 'home/nexus/.fccache-build'])
    fs = fatput.Fat32(disk2)
    assert fs.find_name(fs.lookup_dir(['home', 'nexus']), '.fccache-build') is None
    assert sum(1 for c in range(2, fs.nclusters + 2) if fs.fat_get(c) == 0) == free_full + 2, 'directory and file clusters not freed'
    fs.close()
    spec = importlib.util.spec_from_file_location('fatfsck', Path(__file__).resolve().parents[1] / 'fatfsck.py')
    fatfsck = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatfsck)
    status, report = fatfsck.check(disk2)
    assert status == 0, report.problems
print('PASS: FAT file contents, preserved tail, atomic disk-full failure, corrupt-volume refusal, rmdir (empty only, consistent)')
