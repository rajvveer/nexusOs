"""Boot a FAT image interrupted during a namespace mutation with the boot check skipped (fsck.mode=skip): it must stay
read-only. (By default the boot check repairs such a volume: tests/test_x64_fatcheck.py.)"""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import struct
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-readonly')
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-readonly',
             'put', str(marker), 'home/nexus/readonly.dat'])
fs = fatput.Fat32(disk)
try:
    # Even one dirty mirror must prevent writes after a reset.
    offset = (fs.rsv + (fs.nfats - 1) * fs.fatsz) * 512 + 4
    state = struct.unpack('<I', fs.rd(offset, 4))[0]
    fs.wr(offset, struct.pack('<I', state & ~0x08000000))
finally:
    fs.close()
before = hashlib.sha256(disk.read_bytes()).digest()
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
process = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '128',
    '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
    '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none',
    '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
    '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-fw_cfg', 'name=opt/nexus/cmdline,string=fsck.mode=skip'], capture_output=True, timeout=60)
log = serial.read_text()
assert process.returncode == 33, (process.returncode, process.stderr, log[-4000:])
assert '[64] STATFS READONLY PASSED:' in log and '[64] READONLY CAMPAIGN PASSED' in log, log
assert '[FAT32] unclean metadata; mounted read-only' in log, log
assert 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log, log
assert hashlib.sha256(disk.read_bytes()).digest() == before
print('PASS dirty FAT mirror: read-only mount/statfs, EROFS mutations, readable data and unchanged disk')
