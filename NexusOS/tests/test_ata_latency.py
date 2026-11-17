"""A slow, healthy ATA disk must not fail a CPU-iteration timeout during mount."""
from pathlib import Path
import subprocess
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = nexus_paths.runs('native64-ata')
WORK.mkdir(parents=True, exist_ok=True)
serial = WORK / 'latency.log'
serial.unlink(missing_ok=True)
process = subprocess.Popen([
    r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '128',
    '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
    '-drive', f'file={OUT / "apps64.img"},format=raw,if=ide,snapshot=on,iops_rd=10',
    '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
    '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'
], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        log = serial.read_text() if serial.exists() else ''
        if '[64] PASS native ATA/FAT32 mount' in log:
            assert '[ATA]' not in log and '[64] FAIL:' not in log and 'KERNEL_FAULT' not in log, log
            print('PASS FAT mount on a real QEMU disk throttled to 10 read IOPS', flush=True)
            break
        if process.poll() is not None:
            raise AssertionError((process.returncode, process.communicate()[0], log))
        time.sleep(.05)
    else:
        raise AssertionError('Throttled disk did not mount within deadline: ' + log)
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
