"""Linux ABI completeness campaign (kabi, wave 2).

Boots the native x86-64 kernel with /nexus/test-kabi on private copies of apps64.img (FAT root) and apps64-ext4.img
(ext4 root). The kernel runs /bin/kabi64 (arch/x86_64/kabi_user.c): POSIX timers (every clock, SIGEV_SIGNAL/NONE/
THREAD/THREAD_ID, overruns, TIMER_ABSTIME, CPU-time timers, RLIMIT_SIGPENDING limit, fork/exec cleanup, hostile
values), ITIMER_VIRTUAL/PROF, timerfd (ticks, epoll/poll, ABSTIME, CANCEL_ON_SET + clock steps, SET_TICKS, fork),
signalfd/signalfd4 (records, readiness, masks, SIGCHLD and timer records), rt_sigtimedwait, rt_sig(tg)queueinfo,
pidfd_open/send_signal/getfd, waitid(P_PIDFD), clone(CLONE_PIDFD), clone3 (CLONE_PIDFD, exit_signal, stack,
CLONE_CLEAR_SIGHAND, argument errors; glibc 2.41 pthread_create/posix_spawn go through it), quotactl/quotactl_fd,
getcpu and the rseq fallback; then /bin/kabi2 (arch/x86_64/kabi2_user.c, as the user and as root): RLIMIT_SIGPENDING for
every queued signal, /proc/PID/fdinfo, System V semaphores/message queues/shared memory with /proc/sysvipc, the IPC
sysctls and IPC namespaces, Landlock ABI v4 (with seccomp, over fork and exec), the new mount API and an _ALARM timer
waking suspend-to-idle; then checks that every timer, IPC object, Landlock domain and page was reclaimed.
Runs on 1, 2 and 4 CPUs (FAT root) and on 2 CPUs with the ext4 root (quotactl ESRCH path).
Usage: python -I tests/test_x64_kabi.py [WORKDIR] [--only smp1,smp2,smp4,ext4]
"""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
args = sys.argv[1:]
only = None
if '--only' in args: i = args.index('--only'); only = set(args[i + 1].split(',')); del args[i:i + 2]
WORK = Path(args[0]) if args else Path(r'C:\nexustest\native64-kabi')
WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


fatput = module('fatput', ROOT / 'userland/fatput.py')
ext4lib = module('ext4lib', ROOT / 'userland/ext4lib.py')
EXPECT = [
    '[64] KABI POSIX TIMERS PASS:', '[64] KABI ITIMER PASS:', '[64] KABI TIMERFD PASS:', '[64] KABI SIGNALFD PASS:',
    '[64] KABI PIDFD PASS:', '[64] KABI MISC PASS:', '[64] KABI MOUNTS PASS:', '[64] KABI ALL PASSED:',
    '[64] PASS kabi: POSIX timers, timerfd, itimers, signalfd, sigtimedwait, pidfd, clone3, quotactl',
    '[64] KABI ROOT PASS:', '[64] PASS kabi as root: clock steps, CANCEL_ON_SET, _ALARM clocks',
    '[64] PASS kabi campaign timers and pages reclaimed', '[64] KABI CAMPAIGN PASSED',
    '[64] KABI2 PASS: RLIMIT_SIGPENDING', '[64] KABI2 PASS: preadv/pwritev', '[64] KABI2 PASS: PR_SET_PDEATHSIG', '[64] KABI2 PASS: /proc/PID/fdinfo', '[64] KABI2 PASS: System V semaphores',
    '[64] KABI2 PASS: System V message queues', '[64] KABI2 PASS: System V shared memory', '[64] KABI2 PASS: Landlock ABI v4',
    '[64] KABI2 PASS: new mount API: EPERM', '[64] KABI2 PASS: new mount API: fsopen', '[64] KABI2 PASS: IPC sysctls',
    '[64] KABI2 PASS: CLOCK_BOOTTIME_ALARM', '[64] KABI2 ALL PASSED (user)', '[64] KABI2 ALL PASSED (root)',
    '[64] PASS kabi2: System V IPC, Landlock', '[64] PASS kabi2 as root: System V IPC',
    '[64] PASS kabi campaign System V IPC objects and Landlock domains released',
]
FORBIDDEN = ('[64] FAIL', 'KERNEL_FAULT', 'KABI FAIL', 'uncontained kernel exception', 'ENOSYS syscall=', 'Unsupported clone flags')
CASES = [('smp1', 1, 'fat'), ('smp2', 2, 'fat'), ('smp4', 4, 'fat'), ('ext4', 2, 'ext4')]
for tag, smp, root in CASES:
    if only and tag not in only: continue
    disk = WORK / (tag + '-disk.img')
    if root == 'fat':
        shutil.copyfile(OUT / 'apps64.img', disk)
        marker = WORK / 'marker'; marker.write_bytes(b'1')
        fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-kabi'])
    else:
        shutil.copyfile(OUT / 'apps64-ext4.img', disk)
        fs = ext4lib.Writer(str(disk)); fs.ensure_dir(['nexus'], 0o755, 0, 0); fs.put_bytes(b'1', 'nexus/test-kabi', 0o644, 0, 0); fs.close()
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, '-cpu', 'max', '-m', '512', '-smp', str(smp), '-global', 'PIIX4_PM.disable_s3=0',   # S3: the _ALARM wake case
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output = process.communicate(timeout=600)[0]
    except subprocess.TimeoutExpired:
        process.kill(); output = process.communicate()[0]
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors='replace')[-4000:])
    log = serial.read_text(errors='replace')
    disk.unlink(missing_ok=True)
    for line in log.splitlines():
        if 'KABI' in line or 'FAIL' in line or 'FAULT' in line or 'ENOSYS' in line:
            print(' ', line[:300])
    assert process.returncode == 33, (tag, process.returncode, output, log[-4000:])
    for bad in FORBIDDEN:
        assert bad not in log, (tag, bad, log[-4000:])
    for text in EXPECT:
        assert text in log, (tag, 'missing: ' + text, log[-4000:])
    if root == 'ext4': assert '[64] PASS native ATA/ext4 root mount' in log, (tag, log[-3000:])
    print(f'PASS {tag} (smp={smp}, {root} root) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS kabi campaign: POSIX timers, timerfd, itimers, signalfd, pidfd, clone3, quotactl - Linux-exact')
