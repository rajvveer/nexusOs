"""Kernel-pointer hygiene (item 5): /proc/kallsyms with gated addresses, writable kptr_restrict/dmesg_restrict.

The /nexus/test-kptr campaign runs /bin/kptr64 (arch/x86_64/kptr_user.c) as root and checks:
  - /proc/kallsyms lists kernel symbols (a known name present) with every address zero while kptr_restrict=1 (default),
    even though the reader has CAP_SYSLOG (NexusOS reveals only when kptr_restrict=0 AND CAP_SYSLOG);
  - after `echo 0 > /proc/sys/kernel/kptr_restrict` the addresses become non-zero;
  - /proc/sys/kernel/{kptr_restrict,dmesg_restrict} accept writes and read back the written value.
Runs on 1 and 2 CPUs. Usage: python -I tests/test_x64_kptr.py [WORKDIR]"""
from pathlib import Path
import importlib.util, shutil, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'C:\nexustest\native64-kptr')
WORK.mkdir(parents=True, exist_ok=True)
fatput = importlib.util.module_from_spec(importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py'))
importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py').loader.exec_module(fatput)

EXPECT = ['[64] KPTR USER PASS', '[64] KPTR CAMPAIGN PASSED']
FORBIDDEN = ('[64] FAIL', 'KERNEL_FAULT', 'uncontained')
for tag, smp in (('smp1', 1), ('smp2', 2)):
    disk = WORK / (tag + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / 'marker'; marker.write_bytes(b'1')
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-kptr'])
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
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors='replace')[-3000:])
    log = serial.read_text(errors='replace'); disk.unlink(missing_ok=True)
    for line in log.splitlines():
        if 'KPTR' in line or 'FAIL' in line or 'FAULT' in line: print(' ', line)
    assert p.returncode == 33, (tag, p.returncode, out, log[-3000:])
    for bad in FORBIDDEN:
        assert bad not in log, (tag, bad, log[-3000:])
    for text in EXPECT:
        assert text in log, (tag, 'missing: ' + text, log[-3000:])
    print(f'PASS {tag} (smp={smp}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS kptr: /proc/kallsyms address gating by kptr_restrict + CAP_SYSLOG; kptr_restrict/dmesg_restrict writable')
