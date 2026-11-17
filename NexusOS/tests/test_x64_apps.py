"""Apps platform campaign: pseudo-terminals/job control, inotify and the extended file/VM syscalls.

Boots the native kernel with /nexus/test-apps on a private copy of apps64.img; the kernel runs
/bin/pty64, /bin/inotify64 and /bin/sysext64 and checks that every page they used was reclaimed.
Usage: python -I tests/test_x64_apps.py [WORKDIR] [--smp N]
"""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('native64-apps')
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-apps'])
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
command = [r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '256', '-smp', str(SMP),
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
           '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none',
           '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
           '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
started = time.monotonic()
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    output = process.communicate(timeout=240)[0]
except subprocess.TimeoutExpired:
    process.kill(); output = process.communicate()[0]
    raise AssertionError('apps campaign timed out\n' + serial.read_text(errors='replace'))
log = serial.read_text(errors='replace')
disk.unlink(missing_ok=True)  # keep only the log
for line in log.splitlines():
    if any(key in line for key in ('PASS', 'FAIL', 'ENOSYS', 'FAULT', 'CAMPAIGN')): print(line)
assert process.returncode == 33, (process.returncode, output, log[-4000:])
for expected in ('[64] PTY ALL PASSED:', '[64] INOTIFY ALL PASSED:', '[64] SYSEXT ALL PASSED:', '[64] APPS CAMPAIGN PASSED'):
    assert expected in log, (expected, log[-4000:])
for forbidden in ('FAIL', 'KERNEL_FAULT', 'USER_FAULT', 'FILE_WRITEBACK failed', 'PAGE_IO'):
    assert forbidden not in log, (forbidden, log[-4000:])
# rseq (334) stays unsupported: glibc falls back without it. Everything else the programs call must exist.
assert all(line.endswith('=0x000000000000014E') for line in log.splitlines() if 'ENOSYS syscall=' in line), log[-4000:]
print('PASS apps campaign (%d CPUs) in %.1f s' % (SMP, time.monotonic() - started))
