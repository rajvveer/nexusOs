"""WoW64 CPU-mode campaign: Linux GDT selectors, LDT/TLS segments, int $0x80, sigreturn selector checks and
32-bit compatibility-mode execution (Wine's WoW64 needs all of them).

Boots the native kernel with /nexus/test-wow32 on a private copy of apps64.img; the kernel runs its segment
self-test, then /bin/wow32 (arch/x86_64/wow32_user.c), and checks that every page it used was reclaimed.
Without user memory below 4 GiB the 32-bit execution part reports SKIP (allowed unless --require-low).
Usage: python -I tests/test_x64_wow32.py [WORKDIR] [--smp N] [--whpx] [--require-low]
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
argv = sys.argv[1:]
SMP = 2
if '--smp' in argv: i = argv.index('--smp'); SMP = int(argv[i + 1]); del argv[i:i + 2]
args = [a for a in argv if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('native64-wow32')
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-wow32'])
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
accel = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand,+smep,+smap'] if '--whpx' in sys.argv else ['-cpu', 'max']
command = [r'C:\Program Files\qemu\qemu-system-x86_64.exe', *accel, '-m', '256', '-smp', str(SMP),
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
           '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none',
           '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
           '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
started = time.monotonic()
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    output = process.communicate(timeout=300)[0]
except subprocess.TimeoutExpired:
    process.kill(); output = process.communicate()[0]
    raise AssertionError('wow32 campaign timed out\n' + serial.read_text(errors='replace')[-6000:])
log = serial.read_text(errors='replace')
disk.unlink(missing_ok=True)  # keep only the log
for line in log.splitlines():
    if 'WOW32' in line or 'KERNEL_FAULT' in line or 'CAMPAIGN' in line or 'Invalid signal frame' in line: print(line)
assert process.returncode == 33, (process.returncode, output, log[-6000:])
for expected in ('[64] PASS WOW32 SELFTEST:', '[64] WOW32 ALL PASSED:', '[64] WOW32 CAMPAIGN PASSED',
                 '[64] PASS LDTs, threads and descriptors reclaimed'):
    assert expected in log, (expected, log[-6000:])
for forbidden in ('WOW32 FAIL', 'GROUP FAILED', 'KERNEL_FAULT', 'FILE_WRITEBACK failed', 'PAGE_IO', 'uncontained'):
    assert forbidden not in log, (forbidden, log[-6000:])
groups64 = ['Linux user selectors', 'arch_prctl:', 'modify_ldt: read', '64-bit selector loads', "this CPU's DS/ES",
            'on the other CPUs', 'deleted LDT stack segment', 'sigreturn accepts', 'fork copies the LDT', 'threads share one LDT',
            'execve drops', 'int $0x80 from 64-bit code']
groups32 = ['set_thread_area/get_thread_area', 'far transfers', 'faults in 32-bit code', 'int $0x80 from 32-bit code',
            '32-bit loops preempted', 'deleted LDT code segment']
passes = [line for line in log.splitlines() if line.startswith('[64] WOW32 PASS:')]
for name in groups64:
    assert any(name in line for line in passes), ('missing 64-bit group', name, log[-6000:])
assert log.count('Invalid signal frame;') == 9, ('nine forged sigreturn frames must be refused', log.count('Invalid signal frame;'))
skipped = '[64] WOW32 SKIP:' in log
if skipped:
    assert not any(name in line for line in passes for name in groups32), log[-6000:]
    assert '--require-low' not in sys.argv, 'user memory below 4 GiB is required (--require-low) but the kernel has none'
    print('NOTE 32-bit compatibility-mode groups SKIPPED: no user memory below 4 GiB in this kernel')
else:
    for name in groups32:
        assert any(name in line for line in passes), ('missing 32-bit group', name, log[-6000:])
print('PASS wow32 campaign (%d CPUs%s, %s) in %.1f s' % (SMP, ', WHPX' if '--whpx' in sys.argv else ', TCG',
      '64-bit groups only' if skipped else 'all groups', time.monotonic() - started))
