"""Swap, page reclaim and the OOM killer on the native x86-64 kernel.

arch/x86_64/swap_user.c is booted as /sbin/init on a copy of apps64.img (FAT32) or apps64-ext4.img; its mode comes from
/nexus/swaptest.conf (see the comment at the top of swap_user.c):
  api     swapon/swapoff semantics, headers, priorities, /proc/swaps, meminfo, sysctls, hostile cases     (256 MiB guest)
  stress  2x RAM anonymous + shared + fork COW + dirty MAP_PRIVATE + memfd + /dev/shm, mlock, swapoff     (256 MiB guest)
  oom     no swap: OOM victim selection (badness, oom_score_adj), sentinel survives                       (256 MiB guest)
  ext4    swap file on an ext4 root: holes refused, extents used                                          (256 MiB guest)
  debian  Debian's mkswap/swapon/swapoff/free, the /etc/fstab swap file activated at boot (needs --desktop-disk: a disposable
          desktop disk copy on which arch/x86_64/swap_tools.py install ran; the case edits it in place -- a 256 MiB
          /swapfile when /etc/fstab has none, the campaign as /sbin/init --, boots it with snapshot=on and puts
          out/desktop.elf back as /sbin/init afterwards: no second image on the host)
Every case runs on 1 CPU and on 2 CPUs (MTTCG) unless --smp is given; --repeat N repeats each case.

Usage: test_x64_swap.py [work dir] [out dir] [--only api,stress,oom,ext4,debian] [--smp 1,2,4] [--repeat N] [--memory MiB]
       [--rounds N] [--whpx] [--desktop-disk IMG]"""
from pathlib import Path
import argparse
import gc
import importlib.util
import json
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
parser = argparse.ArgumentParser()
parser.add_argument('work', nargs='?', default=str(nexus_paths.runs('native64-swap')))
parser.add_argument('out', nargs='?', default=str(ROOT / 'arch/x86_64/out'))
parser.add_argument('--only', default='api,stress,oom,ext4')
parser.add_argument('--smp', default='1,2')
parser.add_argument('--repeat', type=int, default=1)
parser.add_argument('--memory', type=int, default=256)
parser.add_argument('--rounds', type=int, default=1)
parser.add_argument('--whpx', action='store_true')
parser.add_argument('--desktop-disk', help='desktop disk copy with the Debian swap tools (debian case; never the real one)')
options = parser.parse_args()
WORK = Path(options.work); OUT = Path(options.out)
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('swap_fatput', ROOT / 'userland/fatput.py'); fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
spec = importlib.util.spec_from_file_location('swap_fat2ext4', ROOT / 'userland/fat2ext4.py'); fat2ext4 = importlib.util.module_from_spec(spec); spec.loader.exec_module(fat2ext4)
app = WORK / 'swap_user.elf'
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/swap_user.c'), '-o', str(app)], check=True)
GROW = {'api': None, 'stress': 2048, 'oom': None, 'ext4': None, 'debian': None}
TIMEOUT = {'api': 300, 'stress': 1800, 'oom': 600, 'ext4': 900, 'debian': 900}


def fat_has(disk, path):
    fs = fatput.Fat32(str(disk), readonly=True)
    try:
        dirs, leaf = fs.split(path)
        try: return fs.find_name(fs.lookup_dir(dirs), leaf) is not None
        except SystemExit: return False
    finally: fs.f.close()


def prepare_debian(files):
    """--desktop-disk in place: a 256 MiB /swapfile in /etc/fstab when it has no swap line, the campaign as /sbin/init."""
    if not options.desktop_disk: raise SystemExit('the debian case needs --desktop-disk')
    disk = Path(options.desktop_disk).resolve()
    if 'New folder' in str(disk) or not disk.is_file(): raise SystemExit('give a disposable desktop disk copy (never the real disk)')
    tools = str(ROOT / 'arch/x86_64/swap_tools.py')
    status = subprocess.run([sys.executable, '-I', tools, 'status', str(disk)], check=True, capture_output=True, text=True)
    if not json.loads(status.stdout)['fstab_swap']:
        subprocess.run([sys.executable, '-I', tools, 'swapfile', str(disk), '--size', '256'], check=True)
    fatput.edit(['fatput', str(disk), *files])
    return disk


def restore_debian(disk):
    """The disk as the desktop boots it again: Aurora as /sbin/init, no campaign files."""
    extra = [item for path in ('nexus/swaptest.conf', 'nexus/init-as-root') if fat_has(disk, path) for item in ('rm', path)]
    fatput.edit(['fatput', str(disk), 'put', str(OUT / 'desktop.elf'), 'sbin/init', *extra])


def prepare(case, disk):
    conf = WORK / 'swaptest.conf'
    conf.write_text(case if case != 'stress' or options.rounds == 1 else f'stress:{options.rounds}', newline='\n')
    marker = WORK / 'init-as-root'; marker.write_text('swap tests run init as root\n')   # swapon needs CAP_SYS_ADMIN
    files = ['put', str(app), 'sbin/init', 'put', str(conf), 'nexus/swaptest.conf', 'put', str(marker), 'nexus/init-as-root']
    if case == 'debian': return prepare_debian(files)
    if case == 'ext4':   # a 2 GiB ext4 root converted from apps64.img with the campaign files
        staging = WORK / 'ext4-staging.img'; staging.unlink(missing_ok=True)
        shutil.copyfile(OUT / 'apps64.img', staging)
        fatput.main(['fatput', str(staging), *files])
        fat2ext4.convert(str(staging), str(disk), size_mib=2048)
        staging.unlink()
        return disk
    shutil.copyfile(OUT / 'apps64.img', disk)
    fatput.main(['fatput', str(disk), *files])
    if GROW[case]:
        subprocess.run([sys.executable, '-I', str(ROOT / 'storage.py'), str(GROW[case]), str(disk)], check=True, capture_output=True)
    return disk


def run(case, cpus, attempt):
    tag = f'{case}-smp{cpus}-{attempt}'
    disk = WORK / (tag + '.img'); serial = WORK / (tag + '.log')
    disk.unlink(missing_ok=True); serial.unlink(missing_ok=True)
    disk = prepare(case, disk)
    accel = ['-accel', 'whpx,kernel-irqchip=off'] if options.whpx else ['-accel', 'tcg,thread=multi' if cpus > 1 else 'tcg']
    command = [str(QEMU), *accel, '-cpu', 'max' if not options.whpx else 'qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt', '-m', str(options.memory),
               '-smp', str(cpus), '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={disk},format=raw,if=ide' + (',snapshot=on' if case == 'debian' else ''), '-boot', 'a', '-display', 'none', '-vga', 'none',
               '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    started = time.time()
    try:
        result = subprocess.run(command, capture_output=True, timeout=TIMEOUT[case] * (2 if cpus > 1 else 1))
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
    elapsed = time.time() - started
    log = serial.read_text(errors='replace') if serial.exists() else ''
    marker = {'api': 'SWAPTEST API ALL PASSED', 'stress': 'SWAPTEST STRESS ALL PASSED', 'oom': 'SWAPTEST OOM ALL PASSED', 'ext4': 'SWAPTEST EXT4 ALL PASSED',
              'debian': 'SWAPTEST DEBIAN ALL PASSED'}[case]
    bad = [w for w in ('SWAPTEST FAIL', 'KERNEL_FAULT', '[64] FAIL', 'uncontained', 'panic') if w in log]
    ok = code == 33 and marker in log and not bad
    if case == 'oom' and ok:
        kills = [line for line in log.splitlines() if 'OOM Out of memory: Killed process' in line]
        order = [line for line in log.splitlines() if 'victims in order' in line]
        ok = len(kills) >= 2 and bool(order)
        if ok:   # the adj-1000 hog is the first victim after the order line
            a = order[0].split('a=')[1].split()[0]
            after = log[log.index(order[0]):]
            first = [line for line in after.splitlines() if 'Killed process' in line][0]
            ok = f'Killed process {a} ' in first
            if not ok: print('wrong first victim:', first, 'expected', a)
    print(('PASS' if ok else 'FAIL'), tag, f'{elapsed:.0f}s', flush=True)
    for line in log.splitlines():
        if 'SWAPTEST' in line and ('PASS' in line or 'stress:' in line or 'oom:' in line or 'debian: $' in line): print('   ', line.strip())
    if not ok:
        print('exit', code, 'markers', bad); print(log[-6000:])
    if case == 'debian': restore_debian(disk); return ok   # the caller's disk copy: restored, never deleted
    for attempt in range(20):   # fat2ext4 may still hold its handle until collected
        try: disk.unlink(missing_ok=True); break
        except PermissionError: gc.collect(); time.sleep(0.5)
    return ok


failures = 0
for case in options.only.split(','):
    for cpus in [int(c) for c in options.smp.split(',')]:
        for attempt in range(options.repeat):
            if not run(case, cpus, attempt): failures += 1
print('SWAP TESTS', 'PASSED' if not failures else f'FAILED ({failures})')
sys.exit(1 if failures else 0)
