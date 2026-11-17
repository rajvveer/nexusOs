"""Regression campaign for the net fix group: boots arch/x86_64/inet_fix_user.c as init, with a NIC and without one.
usage: test_x64_inet_fix.py <workdir> [modes: n (rtl8139 NIC) l (no NIC) b (bulk/burst only)] [--kernel <nexus64.img>]"""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
args = sys.argv[1:]
kernel = OUT / 'nexus64.img'
smp = ['-smp', '2'] if '--smp2' in args else []
args = [a for a in args if a != '--smp2']
if '--kernel' in args:
    i = args.index('--kernel'); kernel = Path(args[i + 1]); del args[i:i + 2]
WORK = Path(args[0]) if args else nexus_paths.runs('native64-inet-fix')
modes = args[1] if len(args) > 1 else 'nl'
WORK.mkdir(parents=True, exist_ok=True)
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36',
    '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
    str(ROOT / 'arch/x86_64/inet_fix_user.c'), '-o', str(WORK / 'inet-fix.elf')], check=True)
spec = importlib.util.spec_from_file_location('fix_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
failed = []
for mode in modes:
    disk = WORK / f'disk-{mode}.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    (WORK / 'mode').write_text({'n': 'nic', 'l': 'lo', 'b': 'bulk', 'r': 'brst'}[mode])
    fatput.main(['fatput', str(disk), 'put', str(WORK / 'mode'), 'nexus/inet-fix-mode', 'put', str(WORK / 'inet-fix.elf'), 'sbin/init'])
    serial = WORK / f'serial-{mode}.log'; serial.unlink(missing_ok=True)
    net = ['-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network'] if mode not in 'l' else ['-nic', 'none']
    try:
        result = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', *smp, '-m', '256',
            '-drive', f'file={kernel},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
            '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', *net,
            '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], capture_output=True, timeout=300)
        log = serial.read_text(errors='replace')
        print(f'--- mode {mode}: qemu exit {result.returncode}')
        for line in log.splitlines():
            if 'FIX' in line or 'KERNEL' in line or 'panic' in line.lower() or 'FAIL' in line or 'loopback only' in line: print('   ', line)
        ok = result.returncode == 33 and '[64] INIT SESSION RETURNED' in log and 'KERNEL_FAULT' not in log and \
             'USER_FAULT' not in log and '[64] FAIL:' not in log and ('INET FIX CAMPAIGN PASSED' in log)
        if not ok: failed.append(mode)
    except subprocess.TimeoutExpired:
        print(f'--- mode {mode}: TIMEOUT'); failed.append(mode)
        print('\n'.join(l for l in serial.read_text(errors='replace').splitlines() if 'FIX' in l))
    finally:
        disk.unlink(missing_ok=True)
print('FAIL modes: ' + ''.join(failed) if failed else 'PASS inet fix campaign: ' + modes)
sys.exit(1 if failed else 0)
