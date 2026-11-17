"""User space from 64 KiB and RAM above 4 GiB (the address layout Wine needs).

Boots the native kernel with /nexus/test-memlayout on private copies of apps64.img. The kernel runs
/bin/memlayout64 (arch/x86_64/memlayout_user.c), which checks: MAP_FIXED at 0x10000, 0x400000 and
0x7ffe0000, the Wine preloader's reservations (a 1.6 GiB PROT_NONE MAP_NORESERVE one at 0x110000), EPERM
below 0x10000 and NULL faults, MAP_32BIT below 2 GiB, low mprotect/madvise/mremap/munmap, fork/COW and
/proc/self/maps on low mappings, signal frames on a low alternate stack, and non-PIE ET_EXEC programs at
0x400000 (glibc) and 0x10000 (static); with mode 'a' it also touches 5.5 GiB in one process. Then the
kernel checks that every page came back.

Runs:  small     TCG,  256 MiB, pc + IDE,     2 CPUs: layout only (no RAM above 4 GiB)
       big-tcg   TCG,  8 GiB,   pc + IDE,     2 CPUs: layout + 5.5 GiB
       big-whpx  WHPX, 8 GiB,   q35 + NVMe,   2 CPUs: layout + 5.5 GiB (64-bit DMA from high memory)
       self-8g   WHPX, 8 GiB,   q35 + AHCI,   2 CPUs: the whole boot/disk self-test suite with RAM above 4 GiB, plus
                 DMA32 devices (e1000e, xHCI + USB HID, ICH9 HD Audio) initialised next to it
Usage: python -I tests/test_x64_memlayout.py [WORKDIR] [--tcg-only | --whpx-only] [--only TAG]
"""
from pathlib import Path
import importlib.util
import re
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('native64-memlayout')
WORK.mkdir(parents=True, exist_ok=True)
ONLY = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
TCG = ['-cpu', 'max']
WHPX = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+xsave,+avx,+avx2']

RUNS = [  # tag, accelerator, memory MiB, storage, marker mode (None: plain self-tests), timeout s
    ('small', TCG, 256, 'ide', '0', 300),
    ('big-tcg', TCG, 8192, 'ide', 'a', 900),
    ('big-whpx', WHPX, 8192, 'nvme', 'a', 600),
    ('self-8g', WHPX, 8192, 'ahci', None, 600),
]
if '--tcg-only' in sys.argv: RUNS = [r for r in RUNS if r[1] is TCG]
if '--whpx-only' in sys.argv: RUNS = [r for r in RUNS if r[1] is WHPX]
if ONLY: RUNS = [r for r in RUNS if r[0] == ONLY]


DEVICES = ['-netdev', 'user,id=network', '-device', 'e1000e,netdev=network', '-device', 'qemu-xhci,id=xhci',
           '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0', '-audiodev', 'none,id=snd0',
           '-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0']


def storage(kind, disk):
    if kind == 'ide': return ['-drive', f'file={disk},format=raw,if=ide']
    if kind == 'nvme': return ['-machine', 'q35', '-drive', f'if=none,id=n0,file={disk},format=raw', '-device', 'nvme,serial=nx0,drive=n0']
    return ['-machine', 'q35', '-drive', f'if=none,id=d0,file={disk},format=raw', '-device', 'ahci,id=ahci', '-device', 'ide-hd,drive=d0,bus=ahci.0']


for tag, accel, memory, kind, mode, timeout in RUNS:
    disk = WORK / (tag + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    if mode:
        marker = WORK / 'marker'; marker.write_bytes(mode.encode())
        fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-memlayout'])
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, *accel, '-m', str(memory), '-smp', '2',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', *storage(kind, disk),
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
               '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', *(DEVICES if not mode else [])]
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output = process.communicate(timeout=timeout)[0]
    except subprocess.TimeoutExpired:
        process.kill(); output = process.communicate()[0]
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors='replace')[-4000:])
    log = serial.read_text(errors='replace')
    disk.unlink(missing_ok=True)  # keep only the log
    for line in log.splitlines():
        if any(key in line for key in ('E820', 'MEMLAYOUT', 'FAIL', 'KERNEL_FAULT', 'ALL TESTS PASSED', 'ROOT ', 'native DMA', 'USB boot enumeration', 'HDA native')): print(' ', line)
    assert process.returncode == 33, (tag, process.returncode, output, log[-4000:])
    for forbidden in ('[64] FAIL', 'KERNEL_FAULT', 'FILE_WRITEBACK failed', 'PAGE_IO', 'FILE_CACHE read failed'):
        assert forbidden not in log, (tag, forbidden, log[-4000:])
    zone = re.search(r'\[64\] E820 managed physical pages=(\d+) zones=(\d+) above_4g_pages=(\d+)', log)
    assert zone, (tag, 'no zone report', log[:4000])
    managed, above = int(zone.group(1)), int(zone.group(3))
    if memory > 4096:  # RAM above 4 GiB is managed (QEMU puts at least 4 GiB of an 8 GiB guest there)
        assert above >= (memory - 4096) * 256 and managed * 4096 >= (memory - 512) << 20, (tag, managed, above)
    else:
        assert above == 0, (tag, above)
    if mode:
        expected = ['[64] MEMLAYOUT ALL PASSED:', '[64] MEMLAYOUT EXEC PASS: non-PIE ET_EXEC at 0x40', '[64] MEMLAYOUT LOW PASS: static ET_EXEC at 0x10000',
                    '[64] PASS memory layout campaign pages reclaimed', '[64] MEMLAYOUT CAMPAIGN PASSED']
        if mode == 'a': expected.append('[64] MEMLAYOUT BIG PASS: one process touched 5632 MiB')
        for text in expected: assert text in log, (tag, text, log[-4000:])
        # The deliberate faults: NULL write and the read-only low page in fork children, two handled SIGSEGVs.
        faults = re.findall(r'USER_FAULT vector=0x000000000000000E error=0x[0-9A-F]+ rip=0x[0-9A-F]+ cr2=0x([0-9A-F]+)', log)
        assert sorted(int(a, 16) for a in faults) == [0, 0x8000, 0x400000, 0x700000], (tag, faults)
    else:
        for text in ('[64] ALL TESTS PASSED', '[64] DISK TESTS PASSED', '[64] ROOT sata0p1 (MBR FAT32, FAT32) on AHCI', '[64] BLOCK SELFTEST sata0 PASS',
                     'Intel 82574L (e1000e) native DMA/IRQ ready', '[64] USB boot enumeration complete, devices=2',
                     '[64] HDA native PCM ready: NexusOS HD Audio 8086:293E'):
            assert text in log, (tag, text, log[-4000:])
    print(f'PASS {tag} ({memory} MiB, above 4 GiB pages={above}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS memory layout campaign: user space from 64 KiB, RAM above 4 GiB')
