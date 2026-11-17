"""Large FAT32 directories on the root volume: the Wine i386-windows lookup bug (812 long-named entries over fragmented
4 KiB clusters, zlib1.dll the last entry, reported "not found"). arch/x86_64/fatdir_user.c (static) is /sbin/init on a
fresh FAT32 disk with 4 KiB clusters (MBR partition at LBA 32768):
  build   N entries with long names, filler files between the creates so the directory's chain is fragmented, then check
  check   (cold caches, every --smp) negative lookups first, then stat/open/read/upper-case lookup of every name,
          getdents64 with three buffer sizes, a case-insensitive readdir scan
  race    (2 CPUs) the check with parallel lookups, allocation churn, processes dying with open-unlinked files, listings
Host side: userland/fatfsck.py -n clean after every boot; the directory's chain must be fragmented.

Usage: test_x64_fatdir.py WORK [--entries 850] [--smp 1,2] [--race-seconds 30] [--tcg] [--bus ide|nvme] [--qmp-port N]"""
from pathlib import Path
import argparse, importlib.util, struct, subprocess, sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
WHPX_CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
fatput = load('fd_fatput', ROOT / 'userland/fatput.py')
fatfsck = load('fd_fatfsck', ROOT / 'userland/fatfsck.py')
fatimage = load('fd_fatimage', ROOT / 'tests/fatimage.py')
ap = argparse.ArgumentParser()
ap.add_argument('work', type=Path)
ap.add_argument('--entries', type=int, default=850)
ap.add_argument('--smp', default='1,2')
ap.add_argument('--race-seconds', type=int, default=30)
ap.add_argument('--tcg', action='store_true')
ap.add_argument('--bus', default='nvme', choices=('ide', 'nvme'))
ap.add_argument('--qmp-port', type=int, default=4731)
ap.add_argument('--keep', action='store_true')
args = ap.parse_args()
WORK = args.work; WORK.mkdir(parents=True, exist_ok=True)
elf = WORK / 'fatdir_user.elf'
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-musl', '-static', '-O2',
                '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/fatdir_user.c'), '-o', str(elf)], check=True)
results = []


def check(ok, what, detail=''):
    results.append(ok); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-4000:]), flush=True)
    return ok


def make_disk(path):
    vol = fatimage.FatImage.create(320 << 20, 32, 'FATDIR', cluster_sectors=8)   # > 65524 clusters of 4 KiB: FAT32
    vol.mkdir('home/nexus'); vol.mkdir('nexus'); vol.mkdir('tmp')
    vol.put('sbin/init', elf.read_bytes()); vol.put('nexus/init-as-root', b'fatdir runs init as root\n')
    raw = vol.finish()
    mbr = bytearray(512); struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\x02\0', 0x0C, b'\xfe\xff\xff', 32768, len(raw) // 512)
    mbr[510:512] = b'\x55\xaa'
    path.touch(); subprocess.run(['fsutil', 'sparse', 'setflag', str(path)], check=True, stdout=subprocess.DEVNULL)
    with open(path, 'r+b') as f:   # sparse: only the non-zero MiB are written
        f.truncate(32768 * 512 + len(raw)); f.write(mbr)
        for at in range(0, len(raw), 1 << 20):
            block = raw[at:at + (1 << 20)]
            if block.strip(b'\0'): f.seek(32768 * 512 + at); f.write(block)


def conf(disk, text):
    c = WORK / 'fatdir.conf'; c.write_text(text + '\n', newline='\n')
    fatput.edit(['fatput', str(disk), 'put', str(c), 'nexus/fatdir.conf'])


def boot(disk, tag, cpus, timeout=900):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    drive = ['-drive', f'file={disk},format=raw,if=ide'] if args.bus == 'ide' else \
            ['-drive', f'file={disk},format=raw,if=none,id=d0', '-device', 'nvme,drive=d0,serial=fatdir']
    accel = ['-accel', 'tcg,thread=multi' if cpus > 1 else 'tcg', '-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', WHPX_CPU]
    command = [str(QEMU), *accel, '-m', '512', '-smp', str(cpus), '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               *drive, '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    try: code = subprocess.run(command, capture_output=True, timeout=timeout).returncode
    except subprocess.TimeoutExpired: code = 'timeout'
    return code, serial.read_text(errors='replace') if serial.exists() else ''


def bad(log): return [w for w in ('[64] FATDIR FAIL', 'KERNEL_FAULT', '[64] FAIL', 'USER_FAULT', 'panic', 'stale entry') if w in log]


def host_check(disk, tag):
    status, r = fatfsck.check(disk)
    return check(status == 0, f'{tag}: userland/fatfsck.py -n clean', r.problems)


def fragments(disk):
    fs = fatput.Fat32(str(disk), readonly=True)
    try:
        chain = fs.chain(fs.lookup_dir(['home', 'nexus', 'big']))
        return len(chain), sum(1 for a, b in zip(chain, chain[1:]) if b != a + 1)
    finally: fs.f.close()


disk = WORK / 'fatdir.img'; disk.unlink(missing_ok=True); make_disk(disk)
n = args.entries
conf(disk, f'build {n}')
code, log = boot(disk, 'build', 2)
ok = check(code in (0, 33) and f'ALL PASSED build' in log and not bad(log), f'build: {n} long-named entries created and checked (2 CPUs)', log[-4000:])
if ok:
    host_check(disk, 'build')
    clusters, breaks = fragments(disk)
    check(breaks >= clusters // 2, f'build: the directory spans {clusters} clusters with {breaks} discontinuities')
    for cpus in [int(x) for x in args.smp.split(',')]:
        conf(disk, f'check {n}')
        code, log = boot(disk, f'check-smp{cpus}', cpus)
        check(code in (0, 33) and 'ALL PASSED check' in log and not bad(log), f'check smp{cpus}: cold-cache lookups, listings and the case-insensitive scan', log[-4000:])
        host_check(disk, f'check smp{cpus}')
    conf(disk, f'race {n} {args.race_seconds}')
    code, log = boot(disk, 'race-smp2', 2)
    check(code in (0, 33) and 'ALL PASSED race' in log and not bad(log), f'race smp2: lookups during churn, signal deaths with orphans and listings ({args.race_seconds} s)', log[-4000:])
    host_check(disk, 'race smp2')
if not args.keep: disk.unlink(missing_ok=True)
print(f'SUMMARY fatdir: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
