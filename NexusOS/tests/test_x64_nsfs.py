"""nsfs campaign: /proc/PID/ns/* and /proc/PID/fd/* magic-link semantics (access/stat/open/readlink THROUGH the link,
as Chromium's and Firefox's sandbox probes need), CLONE_NEWNET/CLONE_NEWIPC/unshare and setns(2) for net/ipc/user/pid
with the Linux capability rules (arch/x86_64/namespaces.c, proc.c, /bin/nsfs64 = nsfs_user.c). Runs on 1 and 2 CPUs.
Usage: python -I tests/test_x64_nsfs.py [WORKDIR]"""
from pathlib import Path
import importlib.util, shutil, subprocess, sys, time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-nsfs')
WORK.mkdir(parents=True, exist_ok=True)
fatput = importlib.util.module_from_spec(importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py'))
importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py').loader.exec_module(fatput)

EXPECT = ['[64] NSFS PASS: /proc/self/ns/{user,ipc,net,pid}', '[64] NSFS PASS: /proc/self/fd/N magic links',
          '[64] NSFS PASS: unprivileged', '[64] NSFS PASS: CLONE_NEWUSER|NEWNET|NEWIPC isolates',
          '[64] NSFS PASS: setns(CLONE_NEWUSER)', '[64] NSFS PASS: setns(CLONE_NEWPID)',
          '[64] NSFS ALL PASSED:', '[64] NSFS CAMPAIGN PASSED']
for tag, smp in (('smp1', 1), ('smp2', 2)):
    disk = WORK / (tag + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / 'marker'; marker.write_bytes(b'1')
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-nsfs'])
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, '-cpu', 'max', '-m', '512', '-smp', str(smp),
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
               '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    started = time.monotonic()
    p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out = p.communicate(timeout=180)[0]
    except subprocess.TimeoutExpired:
        p.kill(); out = p.communicate()[0]
        disk.unlink(missing_ok=True)
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors="replace")[-3000:])
    log = serial.read_text(errors='replace'); disk.unlink(missing_ok=True); marker.unlink(missing_ok=True)
    for line in log.splitlines():
        if 'NSFS' in line or 'FAIL' in line or 'FAULT' in line: print(' ', line)
    assert p.returncode == 33, (tag, p.returncode, out, log[-3000:])
    for bad in ('[64] FAIL', 'KERNEL_FAULT', 'NSFS FAIL', 'USER_FAULT'):
        assert bad not in log, (tag, bad, log[-3000:])
    for t in EXPECT:
        assert t in log, (tag, 'missing: ' + t, log[-3000:])
    print(f'PASS {tag} (smp={smp}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS nsfs campaign: ns/fd magic links, CLONE_NEWNET/NEWIPC, unshare, setns(net/ipc/user/pid)')
