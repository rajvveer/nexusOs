"""Verify native filesystem mutations on a private image, including reboot persistence."""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-files')
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-writes'])
with disk.open('rb') as f: protected = f.read(16 * 1024 * 1024)
fs = fatput.Fat32(disk)
before_free = sum(fs.fat_get(c) == 0 for c in range(2, fs.nclusters + 2)); fs.close()
for boot in range(2):
    serial = WORK / f'boot-{boot}.log'; serial.unlink(missing_ok=True)
    p = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '128',
                        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
                        '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none',
                        '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
                        '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], timeout=60)
    log = serial.read_text()
    assert p.returncode == 33 and '[64] DIRECTORY API PASSED' in log and '[64] ALL TESTS PASSED' in log, log[-3500:]
    assert 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log
    assert '[64] FILE WRITE API PASSED' in log
    assert '[64] EXEC IMAGE ISOLATION PASSED:' in log
    assert '[64] UNIX NAMED PASSED:' in log
    assert '[64] PID ALL PASSED:' in log
    assert '[64] FS ALL PASSED:' in log and '[64] FS MUTATIONS PASSED:' in log
    assert '[64] FSFIX FAT PASSED:' in log and '[64] FSFIX XATTR PASSED:' in log
    assert '[64] SHMEM ALL PASSED:' in log
    assert '[64] LIMIT ALL PASSED:' in log and '[64] LIMIT MEMLOCK PASSED:' in log
    assert '[64] FILEMAP WRITE PASSED:' in log
    assert '[64] FD EXEC PASSED:' in log
    assert '[64] JIT ALL PASSED:' in log
    assert log.count('[64] LOCK ALL PASSED:') == 2
    assert '[64] RENAME ALL PASSED' in log
    assert '[64] STATFS COUNTS PASSED:' in log
    if boot: assert '[64] FILEMAP persisted bytes survived reboot PASS' in log
    if boot: assert '[64] UNIX persistent socket type survived reboot PASS' in log
    if boot: assert '[64] UNIX persistent symlink survived reboot PASS' in log
    if boot: assert '[64] PERSISTED FILE READ PASSED' in log
    fs = fatput.Fat32(disk)
    try:
        home = fs.lookup_dir(['home', 'nexus'])
        assert fs.find_name(home, 'native64-persist'), 'directory not persisted'
        assert not fs.find_name(home, 'native64-dir'), 'rmdir left the test directory'
        directory = fs.lookup_dir(['home', 'nexus', 'native64-persist'])
        entry = fs.find_name(directory, 'data.bin')[2]
        size = struct.unpack_from('<I', entry, 28)[0]
        first = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
        expected = bytes(90 if i == 1 else ((i * 19 + 7) & 255) if i < 4099 else 0 for i in range(8193))
        assert size == len(expected) and fs.read_chain(first, size) == expected
        mapped = fs.find_name(directory, 'mapped.bin')[2]
        mapped_size = struct.unpack_from('<I', mapped, 28)[0]
        mapped_first = struct.unpack_from('<H', mapped, 20)[0] << 16 | struct.unpack_from('<H', mapped, 26)[0]
        mapped_expected = bytearray(8193)
        for offset, value in ((0,65),(1,68),(2,69),(3,72),(4096,73),(8192,74)): mapped_expected[offset] = value
        assert mapped_size == len(mapped_expected) and fs.read_chain(mapped_first, mapped_size) == mapped_expected
        assert not fs.find_name(home, 'filemap-exec'), 'executable mapping fixture not removed'
        socket_entry = fs.find_name(directory, 'stale.sock')[2]
        assert socket_entry[12] == 0xA1 and struct.unpack_from('<I', socket_entry, 28)[0] == 0, 'socket inode type not persisted'
        link_entry = fs.find_name(directory, 'data-link')[2]
        link_first = struct.unpack_from('<H', link_entry, 20)[0] << 16 | struct.unpack_from('<H', link_entry, 26)[0]
        assert link_entry[12] == 0xA2 and struct.unpack_from('<I', link_entry, 28)[0] == 8 and fs.read_chain(link_first, 8) == b'data.bin', 'symlink target not persisted'
        assert not fs.find_name(fs.lookup_dir(['tmp']), 'native64-unix'), 'Unix test directory not removed'
        assert not fs.find_name(fs.lookup_dir(['tmp']), 'native64-fs'), 'filesystem context test directory not removed'
        assert not fs.find_name(fs.lookup_dir(['tmp']), 'native64-locks'), 'lock fixture not removed'
        assert not fs.find_name(fs.lookup_dir(['tmp']), 'native64-space'), 'volume-stat fixture not removed'
        assert not fs.find_name(fs.lookup_dir(['tmp']), 'native64-rename'), 'rename fixture not removed'
        for parent in ('etc', 'bin'):
            assert not fs.find_name(fs.lookup_dir([parent]), 'forbidden'), 'path traversal modified system files'
        after_free = sum(fs.fat_get(c) == 0 for c in range(2, fs.nclusters + 2))
        allocated = 3 + (len(expected) + fs.cl_bytes - 1) // fs.cl_bytes + (len(mapped_expected) + fs.cl_bytes - 1) // fs.cl_bytes
        assert after_free == before_free - allocated, ('unexpected allocation/leak', before_free, after_free, allocated)
        statistics = fs.find_name(directory, 'stats.bin')[2]
        statistics_first = struct.unpack_from('<H', statistics, 20)[0] << 16 | struct.unpack_from('<H', statistics, 26)[0]
        assert struct.unpack_from('<I', statistics, 28)[0] == 24
        assert struct.unpack('<QQQ', fs.read_chain(statistics_first, 24)) == (fs.cl_bytes, fs.nclusters, after_free)
    finally: fs.close()
    with disk.open('rb') as f: assert f.read(len(protected)) == protected
    print('PASS native directory mutations/persistence, boot', boot, flush=True)
print('PASS private FAT mutations and protected region unchanged')
