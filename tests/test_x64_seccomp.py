"""seccomp conformance campaign.

Boots the native x86-64 kernel with /nexus/test-seccomp on private copies of apps64.img. The kernel runs
/bin/seccomp64 (arch/x86_64/seccomp_user.c), which exercises, in forked children/threads so the driver stays
clean: strict mode (read/write/exit allowed, anything else SIGKILL), the classic-BPF validator (rejects a
zero-length / non-returning / out-of-range-jump / unaligned-load / bad-opcode program with EINVAL) and
interpreter, every return action (ERRNO, TRAP -> SIGSYS with si_syscall/si_arch/si_call_addr/si_errno,
USER_NOTIF, KILL_THREAD, KILL_PROCESS), stacked-filter precedence, the NO_NEW_PRIVS requirement (EACCES),
TSYNC across threads, USER_NOTIF (NEW_LISTENER/RECV/SEND/ID_VALID), GET_ACTION_AVAIL / GET_NOTIF_SIZES, the
int-0x80 (AUDIT_ARCH_I386) path, fork inheritance, and /proc/self/status (Seccomp / Seccomp_filters).

Runs on 1 and on 2 CPUs (TSYNC and USER_NOTIF cross-thread paths matter under SMP).
Usage: python -I tests/test_x64_seccomp.py [WORKDIR]
"""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-seccomp')
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)

EXPECT = [
    '[64] SECCOMP ALL PASSED:',
    '[64] PASS seccomp: strict, BPF validate/interpret, actions, precedence, TSYNC, USER_NOTIF',
    '[64] PASS seccomp campaign pages reclaimed',
    '[64] SECCOMP CAMPAIGN PASSED',
]
FORBIDDEN = ('[64] FAIL', 'KERNEL_FAULT', '[64] SECCOMP FAIL', 'uncontained kernel exception')

for tag, smp in (('smp1', 1), ('smp2', 2)):
    disk = WORK / (tag + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / 'marker'; marker.write_bytes(b'1')
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-seccomp'])
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, '-cpu', 'max', '-m', '512', '-smp', str(smp),
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output = process.communicate(timeout=120)[0]
    except subprocess.TimeoutExpired:
        process.kill(); output = process.communicate()[0]
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors='replace')[-4000:])
    log = serial.read_text(errors='replace')
    disk.unlink(missing_ok=True)
    for line in log.splitlines():
        if 'SECCOMP' in line or 'FAIL' in line or 'FAULT' in line:
            print(' ', line)
    assert process.returncode == 33, (tag, process.returncode, output, log[-4000:])
    for bad in FORBIDDEN:
        assert bad not in log, (tag, bad, log[-4000:])
    for text in EXPECT:
        assert text in log, (tag, 'missing: ' + text, log[-4000:])
    print(f'PASS {tag} (smp={smp}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS seccomp campaign: strict + classic-BPF filter + USER_NOTIF, Linux-exact')
