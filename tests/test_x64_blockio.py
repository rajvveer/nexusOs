"""Disk waits without the big kernel lock: a syscall-heavy CPU-bound process keeps its rate while another process does
synchronous disk I/O (arch/x86_64/blockio_user.c booted as /sbin/init, as root).

Each controller (NVMe on q35, AHCI on q35, IDE on pc) runs twice on the same kernel: with the kernel option
blockio.spin (disk waits keep the big lock, the previous behaviour: "before") and without it ("after"). The test asserts
that "after" keeps at least MIN_RATIO (0.2) of the syscall rate measured alone and 20 points more than "before" (which
is ~0%), and that the disk I/O keeps at least half its "before" throughput (it must not starve either). What "after"
still loses is the I/O task's CPU time under the big lock (copies, FAT, page cache), not its disk waits: measured 28-98%,
the lower values with fast host disks (2026-10-29: ~240 MiB/s, 28-46%).

Usage: test_x64_blockio.py [work dir] [out dir] [--smp N] [--bus nvme,ahci,ide] [--whpx] [--min-ratio R] [--repeat N]"""
from pathlib import Path
import argparse
import importlib.util
import re
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
parser = argparse.ArgumentParser()
parser.add_argument('work', nargs='?', default=str(nexus_paths.runs('native64-blockio')))
parser.add_argument('out', nargs='?', default=str(ROOT / 'arch/x86_64/out'))
parser.add_argument('--smp', type=int, default=2)
parser.add_argument('--bus', default='nvme,ahci,ide')
parser.add_argument('--whpx', action='store_true')
parser.add_argument('--min-ratio', type=float, default=0.2)   # TCG: 28-98% measured (lower with fast host disks: see below)
parser.add_argument('--repeat', type=int, default=1)
options = parser.parse_args()
WORK = Path(options.work); OUT = Path(options.out); WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('blockio_fatput', ROOT / 'userland/fatput.py'); fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
app = WORK / 'blockio_user.elf'
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/blockio_user.c'), '-o', str(app)], check=True)
base = WORK / 'base.img'
if base.exists(): base.unlink()
shutil.copyfile(OUT / 'apps64.img', base)
marker = WORK / 'init-as-root'; marker.write_text('blockio test runs init as root (drop_caches)\n')
fatput.main(['fatput', str(base), 'put', str(app), 'sbin/init', 'put', str(marker), 'nexus/init-as-root'])
subprocess.run([sys.executable, '-I', str(ROOT / 'storage.py'), '512', str(base)], check=True, capture_output=True)


def run(bus, spin, attempt):
    tag = f'{bus}-{"before" if spin else "after"}-{attempt}'
    disk = WORK / (tag + '.img'); serial = WORK / (tag + '.log')
    shutil.copyfile(base, disk); serial.unlink(missing_ok=True)
    drive = {'nvme': ['-machine', 'q35', '-drive', f'if=none,id=n0,file={disk},format=raw', '-device', 'nvme,serial=nx0,drive=n0'],
             'ahci': ['-machine', 'q35', '-drive', f'if=none,id=d0,file={disk},format=raw', '-device', 'ahci,id=ahci', '-device', 'ide-hd,drive=d0,bus=ahci.0'],
             'ide': ['-drive', f'file={disk},format=raw,if=ide']}[bus]
    accel = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt'] if options.whpx else ['-accel', 'tcg,thread=multi', '-cpu', 'max']
    command = [str(QEMU), *accel, '-m', '512', '-smp', str(options.smp), '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               *drive, '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
               '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', *(['-fw_cfg', 'name=opt/nexus/cmdline,string=blockio.spin'] if spin else [])]
    try: code = subprocess.run(command, capture_output=True, timeout=600).returncode
    except subprocess.TimeoutExpired: code = 'timeout'
    log = serial.read_text(errors='replace') if serial.exists() else ''
    disk.unlink(missing_ok=True)
    m = re.search(r'BLOCKIO RESULT ratio=([0-9.]+) io_mib_s=([0-9.]+)', log)
    bad = [w for w in ('BLOCKIO FAIL', 'KERNEL_FAULT', '[64] FAIL', 'uncontained', 'BLOCKIO request from a task-less') if w in log]
    if code != 33 or not m or bad or (spin and 'keep the big lock' not in log):
        print('FAIL', tag, 'exit', code, bad); print(log[-5000:]); return None
    for line in log.splitlines():
        if 'BLOCKIO syscall rate' in line: print('   ', tag, line.split('BLOCKIO ', 1)[1])
    return float(m.group(1)), float(m.group(2))


failures = 0
for bus in options.bus.split(','):
    for attempt in range(options.repeat):
        before, after = run(bus, True, attempt), run(bus, False, attempt)
        if not before or not after: failures += 1; continue
        ok = after[0] >= options.min_ratio and after[0] >= before[0] + 0.2 and after[1] >= before[1] * 0.5   # the disk keeps its pace too
        print('PASS' if ok else 'FAIL', f'{bus}: syscall rate during disk I/O {before[0]:.0%} of alone with the big lock held in disk waits, '
              f'{after[0]:.0%} without; disk I/O {before[1]:.1f} -> {after[1]:.1f} MiB/s', flush=True)
        if not ok: failures += 1
base.unlink(missing_ok=True)
print('BLOCKIO TESTS', 'PASSED' if not failures else f'FAILED ({failures})')
sys.exit(1 if failures else 0)
