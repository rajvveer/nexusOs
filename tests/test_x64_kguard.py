"""kguard: kernel-oops containment (arch/x86_64 kguard path in native_dispatch + process.c task_kguard_*).

A recoverable kernel-mode exception caused by a task in a syscall must kill that task (like a Linux SIGKILL oops)
and keep the system alive, instead of panicking. The /nexus/test-kguard campaign boots once per CPU count and,
through /bin/kguard64 and a private syscall (arch/x86_64/kguard.c, armed by the `kguard.probe` boot option):
  - mode 0: the kernel does a stray ring-0 read of a mapped user address with no uaccess window (an SMAP violation);
  - mode 1: the kernel recurses until its stack overflows the guard page (delivered as #PF or #DF on IST).
Each must log `kguard: killed ... - system alive`, the task's pages must be fully reclaimed, and the kernel must go
on to run another user program. A panic ("uncontained kernel exception") fails the run. Runs on 1 and 2 CPUs.
Usage: python -I tests/test_x64_kguard.py [WORKDIR]"""
from pathlib import Path
import importlib.util, shutil, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'C:\nexustest\native64-kguard')
WORK.mkdir(parents=True, exist_ok=True)
fatput = importlib.util.module_from_spec(importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py'))
importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py').loader.exec_module(fatput)

EXPECT = ['[64] kguard: killed', '(SMAP violation) - system alive', '(kernel stack overflow) - system alive',
          '[64] KGUARD CAMPAIGN PASSED']
FORBIDDEN = ('uncontained kernel exception', '[64] FAIL', 'KGUARD PROBE read returned', 'KGUARD PROBE recursion returned',
             'process allocation ownership leak')

for tag, smp in (('smp1', 1), ('smp2', 2)):
    disk = WORK / (tag + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / 'marker'; marker.write_bytes(b'1')
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-kguard'])
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, '-cpu', 'max', '-m', '512', '-smp', str(smp),
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
               '-fw_cfg', 'name=opt/nexus/cmdline,string=kguard.probe']
    started = time.monotonic()
    p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out = p.communicate(timeout=120)[0]
    except subprocess.TimeoutExpired:
        p.kill(); out = p.communicate()[0]
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors='replace')[-3000:])
    log = serial.read_text(errors='replace'); disk.unlink(missing_ok=True)
    for line in log.splitlines():
        if 'kguard' in line or 'KGUARD' in line or 'PASS' in line or 'FAIL' in line or 'panic' in line.lower(): print(' ', line)
    assert p.returncode == 33, (tag, p.returncode, out, log[-3000:])
    for bad in FORBIDDEN:
        assert bad not in log, (tag, 'forbidden: ' + bad, log[-3000:])
    for text in EXPECT:
        assert text in log, (tag, 'missing: ' + text, log[-3000:])
    print(f'PASS {tag} (smp={smp}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS kguard: stray SMAP access and kernel-stack overflow each kill the task and leave the system alive')
