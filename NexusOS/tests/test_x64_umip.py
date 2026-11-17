"""UMIP emulation campaign: with CR4.UMIP set, SGDT/SIDT/SLDT/SMSW/STR from ring 3 #GP and the kernel emulates
them (arch/x86_64/umip.c) with Linux-compatible dummy values. Boots /bin/umip64 and also asserts the boot report
shows UMIP=on. Runs on 1 and 2 CPUs. Usage: python -I tests/test_x64_umip.py [WORKDIR]"""
from pathlib import Path
import importlib.util, shutil, subprocess, sys, time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-umip')
WORK.mkdir(parents=True, exist_ok=True)
fatput = importlib.util.module_from_spec(importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py'))
importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py').loader.exec_module(fatput)

EXPECT = ['[64] UMIP ALL PASSED:', '[64] PASS UMIP:', '[64] UMIP CAMPAIGN PASSED', '[64] Hardening: SMEP=on']
for tag, smp in (('smp1', 1), ('smp2', 2)):
    disk = WORK / (tag + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / 'marker'; marker.write_bytes(b'1')
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-umip'])
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, '-cpu', 'max', '-m', '512', '-smp', str(smp),
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    started = time.monotonic()
    p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out = p.communicate(timeout=120)[0]
    except subprocess.TimeoutExpired:
        p.kill(); out = p.communicate()[0]
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors="replace")[-3000:])
    log = serial.read_text(errors='replace'); disk.unlink(missing_ok=True)
    for line in log.splitlines():
        if 'UMIP' in line or 'Hardening' in line or 'FAIL' in line or 'FAULT' in line: print(' ', line)
    assert p.returncode == 33, (tag, p.returncode, out, log[-3000:])
    for bad in ('[64] FAIL', 'KERNEL_FAULT', '[64] UMIP FAIL', 'USER_FAULT'):
        assert bad not in log, (tag, bad, log[-3000:])
    assert 'UMIP=on' in log, (tag, 'UMIP not enabled', log[-3000:])
    for t in EXPECT:
        assert t in log, (tag, 'missing: ' + t, log[-3000:])
    print(f'PASS {tag} (smp={smp}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS UMIP campaign: SGDT/SIDT/SLDT/SMSW/STR emulated with Linux dummy values')
