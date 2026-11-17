"""Kernel lockdown campaign (arch/x86_64/lockdown.c).

Boots the native kernel three ways and checks kernel_lockdown_level() and the enforcement invariants (the in-kernel
lockdown_selftest, reached through /nexus/test-lockdown):
  no option            -> level 0 (none): nothing is blocked
  lockdown=integrity   -> level 1: hibernation + unsigned kernel images blocked, kptr restricted; a signed image allowed
  lockdown=confidentiality -> level 2: the integrity band plus kernel-memory reads blocked
The self-test also proves the ratchet (the level cannot be lowered) and the /sys/kernel/security/lockdown text format.
Runs on 1 and 2 CPUs. Usage: python -I tests/test_x64_lockdown.py [WORKDIR]"""
from pathlib import Path
import importlib.util, shutil, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'C:\nexustest\native64-lockdown')
WORK.mkdir(parents=True, exist_ok=True)
fatput = importlib.util.module_from_spec(importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py'))
importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py').loader.exec_module(fatput)

CASES = [('none', '', 0, 'none'), ('integrity', 'lockdown=integrity', 1, '[integrity]'),
         ('confidentiality', 'lockdown=confidentiality', 2, '[confidentiality]')]
for tag, cmdline, level, bracket in CASES:
    for smp in (1, 2):
        label = f'{tag}-smp{smp}'
        disk = WORK / (label + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
        marker = WORK / 'marker'; marker.write_bytes(b'1')
        fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-lockdown'])
        serial = WORK / (label + '.log'); serial.unlink(missing_ok=True)
        command = [QEMU, '-cpu', 'max', '-m', '512', '-smp', str(smp),
                   '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
                   '-drive', f'file={disk},format=raw,if=ide',
                   '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
                   '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
        if cmdline:
            command += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
        started = time.monotonic()
        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            out = p.communicate(timeout=120)[0]
        except subprocess.TimeoutExpired:
            p.kill(); out = p.communicate()[0]
            raise AssertionError(f'{label}: timed out\n' + serial.read_text(errors="replace")[-3000:])
        log = serial.read_text(errors='replace'); disk.unlink(missing_ok=True)
        for line in log.splitlines():
            if 'LOCKDOWN' in line or 'Lockdown' in line or 'FAIL' in line or 'FAULT' in line: print(' ', line)
        assert p.returncode == 33, (label, p.returncode, out, log[-3000:])
        for bad in ('[64] FAIL', 'KERNEL_FAULT', '[64] LOCKDOWN FAIL', 'uncontained'):
            assert bad not in log, (label, bad, log[-3000:])
        assert f'[64] LOCKDOWN level={level} sys=' in log, (label, 'wrong level', log[-3000:])
        assert bracket in log, (label, 'sys text missing ' + bracket, log[-3000:])
        assert '[64] LOCKDOWN SELFTEST PASSED' in log and '[64] LOCKDOWN CAMPAIGN PASSED' in log, (label, log[-3000:])
        assert '[64] SECFS USER PASS' in log, (label, 'securityfs /sys/kernel/security/lockdown check', log[-3000:])
        print(f'PASS {label} (level={level}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS kernel lockdown: level from Secure Boot / boot option, integrity+confidentiality bands, one-way ratchet')
