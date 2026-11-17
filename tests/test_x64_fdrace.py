"""Open-file description lifetime under concurrency (struct open_file use-after-free, found in the UPower runs).

Usage: python -I tests/test_x64_fdrace.py [WORK] [--smp 1,2,4] [--roots fat,ext4] [--modes inject,natural]
       [--seconds 20] [--whpx] [--qmp-port 4764]

The kernel's power campaign runs arch/x86_64/fdrace_user.c as /bin/power64 (/nexus/test-power "f <seconds> <dir>"):
threads and forked processes open / mmap / write / close and rename files in an inotify-watched directory on the root
file system while others create, pass (SCM_RIGHTS) and close descriptions with I/O-free system calls and recycle
anonymous pages (see the program's header). Modes:
  inject   boot option test.fd-release-sleep: every file close sleeps 1 ms, big lock released, where fd_release can
           sleep in a disk wait (inotify_event's parent lookup) before it unlinks the page-cache views - the window in
           which the description used to be reused (descriptor_new) or freed (descriptors_prune). Deterministic.
  natural  the same load without the injected sleep.
Each run must end with "[64] FDRACE DONE failures=0", campaign status 0, and no kernel or user fault.
Roots: fat = out/apps64.img, ext4 = out/apps64-ext4.img (NVMe, q35)."""
from pathlib import Path
import argparse, importlib.util, re, shutil, subprocess, time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\wave2\ws_power2_runs\fdrace'))
parser.add_argument('--smp', default='1,2,4')
parser.add_argument('--roots', default='fat,ext4')
parser.add_argument('--modes', default='inject,natural')
parser.add_argument('--seconds', type=int, default=20)
parser.add_argument('--whpx', action='store_true')
parser.add_argument('--qmp-port', type=int, default=4764)
parser.add_argument('--expect-fail', action='store_true', help='report instead of asserting (a kernel without the fix)')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
tools = {'fat': module('fatput', ROOT / 'userland/fatput.py'), 'ext4': module('ext4put', ROOT / 'userland/ext4put.py')}
images = {'fat': OUT / 'apps64.img', 'ext4': OUT / 'apps64-ext4.img'}
program = WORK / 'fdrace_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'fdrace_user.c'), '-o', str(program)], check=True)
DIRECTORY = '/home/nexus/fdrace'   # the campaign user's; ext4put gives home/nexus to 1000:1000


def run(tag, root, smp, mode):
    disk = WORK / f'{tag}.img'; serial = WORK / f'{tag}.log'; marker = WORK / f'{tag}.marker'
    shutil.copyfile(images[root], disk); marker.write_bytes(f'f {options.seconds} {DIRECTORY}'.encode())
    tools[root].main(['put', str(disk), 'mkdir', DIRECTORY.strip('/'), 'put', str(program), 'bin/power64', 'put', str(marker), 'nexus/test-power'])
    serial.unlink(missing_ok=True)
    accel = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand' + build.host_cpu_flags()] if options.whpx else ['-accel', 'tcg', '-cpu', 'max']
    args = [QEMU, *accel, '-machine', 'q35', '-m', '1024', '-smp', str(smp), '-display', 'none', '-vga', 'none',
            '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
            '-drive', f'if=none,id=root,file={disk},format=raw', '-device', 'nvme,serial=nexus-root,drive=root',
            '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
            '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    if mode == 'inject': args += ['-fw_cfg', 'name=opt/nexus/cmdline,string=test.fd-release-sleep']
    vm = subprocess.Popen(args, cwd=WORK, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    build.unthrottle(vm.pid)
    text = ''; deadline = time.time() + 600 + 4 * options.seconds; hung = False
    try:
        while True:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if '[64] POWER CAMPAIGN STATUS=' in text or 'KERNEL_FAULT' in text or '[64] FAIL' in text or 'PANIC' in text or vm.poll() is not None: break
            if time.time() > deadline: hung = True; break
            time.sleep(0.5)
        time.sleep(1); text = serial.read_text(errors='replace') if serial.exists() else text
    finally:
        if vm.poll() is None: vm.kill()
        vm.wait()
        for f in (disk, marker): f.unlink(missing_ok=True)
    done = re.search(r'\[64\] FDRACE DONE failures=(\d+)[^\n]*', text)
    problems = [p for p in ('KERNEL_FAULT', 'USER_FAULT', '[64] FAIL', 'PANIC', 'FDRACE FAIL') if p in text]
    if hung: problems.append('hung (no campaign end in time)')
    if not done: problems.append('no DONE line')
    elif done.group(1) != '0': problems.append('failures=' + done.group(1))
    if '[64] POWER CAMPAIGN STATUS=0x0000000000000000' not in text: problems.append('campaign status')
    if mode == 'inject' and '[64] TEST fd-release-sleep active' not in text: problems.append('the injected sleep never ran')
    detail = done.group(0) if done else ''
    fault = re.search(r'\[64\] KERNEL_FAULT[^\n]*', text)
    return problems, detail, fault.group(0) if fault else ''


results = []; bad = 0
for root in options.roots.split(','):
    for smp in [int(n) for n in options.smp.split(',')]:
        for mode in options.modes.split(','):
            tag = f'fdrace-{root}-smp{smp}-{mode}'; start = time.time()
            problems, detail, fault = run(tag, root, smp, mode)
            line = f'{"PASS" if not problems else "FAIL"} {tag} ({time.time() - start:.0f} s): {detail or "-"}' + (f' {problems} {fault}' if problems else '')
            print(line, flush=True); results.append(line); bad += bool(problems)
print(f'FDRACE SUMMARY {len(results) - bad}/{len(results)} passed', flush=True)
if bad and not options.expect_fail: raise SystemExit(1)
