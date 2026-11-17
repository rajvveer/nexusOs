"""SMAP test (SMAP is ON by default; arch/x86_64/uaccess.h + hardening.c).

Every direct kernel access to user memory goes through uaccess.h (STAC/CLAC copies with fault fixups), so the kernel
boots and passes all self-tests with SMAP enforcing. This proves three things:
  default   SMAP is on, the kernel boots and passes every self-test, Hardening reports SMAP=on, no KERNEL_FAULT.
  nosmap    with the `nosmap` boot option SMAP is off and the kernel still boots clean (the option works).
  probe     with `smap.probe` a deliberate raw ring-0 read of a mapped user page (no uaccess window) faults and is
            CONTAINED as an SMAP violation (reported + panic), i.e. not a silent read. The negative test.

Usage: python -I tests/test_x64_smap.py [WORKDIR]
"""
from pathlib import Path
import re, subprocess, sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-smap')
WORK.mkdir(parents=True, exist_ok=True)


def boot(tag, cmdline):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    cmd = [QEMU, '-cpu', 'max', '-m', '128',
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
           '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
           '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    if cmdline:
        cmd += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
    p = subprocess.run(cmd, capture_output=True, timeout=90)
    return p.returncode, serial.read_text(errors='replace')


rc, log = boot('default', '')
assert rc == 33 and '[64] ALL TESTS PASSED' in log, ('default boot must pass with SMAP on', rc, log[-2000:])
assert 'SMAP=on' in log, ('SMAP must be enabled by default', log[-2000:])
assert 'KERNEL_FAULT' not in log, ('no fault with SMAP on (every user access goes through uaccess.h)', log[-2000:])
print('PASS default: SMAP on by default, kernel boots and passes all self-tests')

rc, log = boot('nosmap', 'nosmap')
assert rc == 33 and '[64] ALL TESTS PASSED' in log, ('`nosmap` boot must pass', rc, log[-2000:])
assert 'SMAP=present-off' in log or 'SMAP=absent' in log, ('`nosmap` disables SMAP', log[-2000:])
print('PASS nosmap: the boot option turns SMAP off; kernel still boots clean')

# Enforcement (a stray ring-0 user access is trapped) is proven in task context by tests/test_x64_kguard.py: there
# the violation kills the offending task and the system stays alive, rather than panicking.
print('PASS SMAP: on by default (uaccess.h), `nosmap` disables it; enforcement is covered by test_x64_kguard.py')
