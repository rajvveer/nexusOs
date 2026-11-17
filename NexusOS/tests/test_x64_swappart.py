"""Swap partitions from /etc/fstab (swap.c swap_boot with mem's swap areas): an MBR type 0x82 partition named by UUID=
with pri=10 and a GPT "Linux swap" partition on a second NVMe disk named by LABEL= with pri=5 (plus a missing device
with nofail), mkswap v1 headers written here. arch/x86_64/swappart_user.c (static, as root) checks them at boot, fills
memory beyond the 256 MiB of RAM (the higher priority fills first, every page reads back), swapoff/swapon with
SWAP_FLAG_PREFER, then powers off. Every case on 1 and 2 CPUs unless --smp is given.

Usage: test_x64_swappart.py WORK [--smp 1,2] [--qmp-port N (unused; kept for w2_suite)]"""
from pathlib import Path
import argparse, importlib.util, shutil, struct, subprocess, sys, uuid, zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
SWAP_TYPE = uuid.UUID('0657fd6d-a4ab-43c4-84e5-0933c84b4f4f')
ap = argparse.ArgumentParser()
ap.add_argument('work', type=Path)
ap.add_argument('--smp', default='1,2')
ap.add_argument('--qmp-port', type=int, default=4736)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
fatput = load('sp_fatput', ROOT / 'userland/fatput.py')
dtv = load('sp_dtv', ROOT / 'tests/debian_tools_vm.py')
results = []


def check(ok, what, detail=''):
    results.append(ok); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True)


def swap_header(pages, label, ident):
    page = bytearray(4096); struct.pack_into('<III', page, 1024, 1, pages - 1, 0)
    page[1036:1052] = ident.bytes; page[1052:1052 + len(label)] = label; page[4086:] = b'SWAPSPACE2'
    return bytes(page)


def disks():
    a, b = WORK / 'swap-a.img', WORK / 'swap-b.img'
    for d in (a, b): d.unlink(missing_ok=True)
    shutil.copyfile(OUT / 'apps64.img', a)
    ua, ub = uuid.uuid4(), uuid.uuid4()
    with open(a, 'r+b') as f:   # partition 2: MBR 0x82, 48 MiB after the FAT32 volume
        mbr = bytearray(f.read(512)); start, count = 131072, 98304
        struct.pack_into('<B3sB3sII', mbr, 446 + 16, 0, b'\xfe\xff\xff', 0x82, b'\xfe\xff\xff', start, count)
        f.seek(0); f.write(mbr); f.truncate((start + count) * 512)
        f.seek(start * 512); f.write(swap_header(count // 8, b'swapA', ua))
    total = 96 * 2048; first = 2048; last = total - 34
    entry = SWAP_TYPE.bytes_le + uuid.uuid4().bytes_le + struct.pack('<QQQ', first, last, 0) + 'swap'.encode('utf-16-le').ljust(72, b'\0')
    entries = entry.ljust(128 * 128, b'\0'); disk_guid = uuid.uuid4()
    def header(mine, other, table):
        h = bytearray(struct.pack('<8sIII4xQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, mine, other, 34, total - 34, disk_guid.bytes_le, table, 128, 128, zlib.crc32(entries)))
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:92]))); return bytes(h).ljust(512, b'\0')
    mbr = bytearray(512); struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\x02\0', 0xEE, b'\xff\xff\xff', 1, total - 1); mbr[510:] = b'\x55\xaa'
    with open(b, 'wb') as f:
        f.truncate(total * 512); f.write(mbr); f.write(header(1, total - 1, 2)); f.write(entries)
        f.seek(first * 512); f.write(swap_header((last - first + 1) // 8, b'swapB', ub))
        f.seek((total - 33) * 512); f.write(entries); f.write(header(total - 1, 1, total - 33))
    for d in (a, b): dtv.punch(d)
    init = WORK / 'swappart_user.elf'
    subprocess.run([str(dtv.ZIG), 'cc', '-target', 'x86_64-linux-musl', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'arch/x86_64/swappart_user.c'), '-o', str(init)], check=True)
    fstab = WORK / 'fstab'; fstab.write_text(f'UUID={ua} none swap sw,pri=10 0 0\nLABEL=swapB none swap pri=5,nofail 0 0\n'
                                             '/dev/nvme7n1p9 none swap nofail 0 0\n', newline='\n')
    conf = WORK / 'swappart.conf'; conf.write_text('/dev/nvme0n1p2 /dev/nvme1n1p1\n', newline='\n')
    marker = WORK / 'init-as-root'; marker.write_text('swap partitions\n', newline='\n')
    fatput.edit(['fatput', str(a), 'put', str(init), 'sbin/init', 'put', str(fstab), 'etc/fstab', 'put', str(conf), 'nexus/swappart.conf',
                 'put', str(marker), 'nexus/init-as-root'])
    return a, b


for cpus in [int(x) for x in args.smp.split(',')]:
    a, b = disks()
    serial = WORK / f'swappart-smp{cpus}.log'; serial.unlink(missing_ok=True)
    command = [QEMU, '-machine', 'q35', '-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU, '-smp', str(cpus), '-m', '256', '-display', 'none',
               '-vga', 'none', '-device', 'virtio-vga', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
               '-drive', f'if=none,id=a,format=raw,file={a}', '-device', 'nvme,serial=swapa,drive=a',
               '-drive', f'if=none,id=b,format=raw,file={b}', '-device', 'nvme,serial=swapb,drive=b',
               '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot']
    try: subprocess.run(command, capture_output=True, timeout=900)
    except subprocess.TimeoutExpired: pass
    log = serial.read_text(errors='replace') if serial.exists() else ''
    lines = '\n'.join(l for l in log.splitlines() if 'SWAPPART' in l or 'SWAP' in l)
    print(lines[-1500:])
    check('SWAPPART ALL PASSED' in log and 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log and 'OOM' not in log,
          f'swap partitions smp{cpus}: fstab UUID=/LABEL= with priorities, fill order, data intact, swapoff/swapon', log[-3000:])
    for d in (a, b): dtv.unlink(d)
print(f'SUMMARY swappart: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
