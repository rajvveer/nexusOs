"""x2APIC mode on native NexusOS (arch/x86_64/apic.c): the kernel's full self-test campaign with 4 CPUs whose local
APICs run in x2APIC mode (kernel option "x2apic"): MSR register access, 64-bit ICR IPIs (INIT/SIPI bring-up, wake-up,
TLB shootdowns, preemption ticks), EOI by MSR, IOAPIC and MSI/MSI-X destinations from x2APIC ids.

Usage: python -I tests/test_x64_x2apic.py [WORK] [--only CASE ...] [--no-whpx]
  tcg        TCG (-cpu max), q35, 4 CPUs, x2apic: the self-test campaign on the apps disk (IDE)
  devices    TCG, q35, 4 CPUs, x2apic, NVMe root + e1000e + xHCI (keyboard, tablet, hub + mouse): MSI/MSI-X interrupts
  whpx       the campaign under WHPX (kernel-irqchip=off), 4 CPUs, +x2apic: x2APIC mode, or - where the hypervisor
             refuses the IA32_APIC_BASE switch with #GP - the logged fallback to xAPIC
  default    TCG, 4 CPUs, no option: the local APICs stay in xAPIC mode (the firmware did not enable x2APIC)
  off        "x2apic nox2apic": nox2apic wins
S3 resume with x2APIC: tests/test_x64_suspend.py --cmdline x2apic --smp 4 --only s3 (re-enters x2APIC mode on every CPU).
"""
from pathlib import Path
import argparse
import importlib.util
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CASES = ['tcg', 'devices', 'whpx', 'default', 'off']
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\native64-x2apic'))
parser.add_argument('--only', nargs='+', choices=CASES, default=CASES)
parser.add_argument('--no-whpx', action='store_true')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('nexus_build', HERE / 'build.py'); build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)


def run(case, cpu, cmdline, devices=False, accel=()):
    disk = WORK / (case + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    serial = WORK / (case + '.log'); serial.unlink(missing_ok=True)
    args = [QEMU, *accel, '-cpu', cpu, '-m', '512', '-smp', '4', '-machine', 'q35',
            '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
            '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
            '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    if cmdline: args += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
    if devices:
        args += ['-drive', f'if=none,id=n0,file={disk},format=raw', '-device', 'nvme,serial=nx0,drive=n0',
                 '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network', '-device', 'qemu-xhci,id=xhci',
                 '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0', '-device', 'usb-hub,bus=xhci.0,port=3', '-device', 'usb-mouse,bus=xhci.0,port=3.1']
    else: args += ['-drive', f'file={disk},format=raw,if=ide']
    process = subprocess.Popen(args, stdout=(WORK / (case + '.qemu.txt')).open('wb'), stderr=subprocess.STDOUT)
    build.unthrottle(process.pid)
    print(case, 'VM', process.pid, flush=True)
    try: code = process.wait(600)
    except subprocess.TimeoutExpired: process.kill(); process.wait(); code = None
    disk.unlink(missing_ok=True)
    log = serial.read_text(errors='replace')
    for line in log.splitlines():
        if 'APIC' in line or 'SMP' in line or 'IRQ mode' in line or 'MSI' in line: print('  ' + line[:200])
    assert 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log, (case, log[-4000:])
    assert code == 33 and '[64] ALL TESTS PASSED' in log, (case, code, log[-3000:])
    assert '[64] SMP: 4 CPUs online' in log, (case, 'SMP')
    switches = [line for line in log.splitlines() if line.startswith('[64] SMP switches')]
    assert switches and switches[-1].count('=0x') == 4 and '=0x0000000000000000' not in switches[-1], ('every CPU ran tasks', switches[-1:])
    return log


results = {}
for case in options.only:
    if case == 'whpx' and (options.no_whpx or not build.whpx_available()): print('SKIP whpx (no WHPX)'); continue
    if case == 'tcg':
        log = run(case, 'max,+x2apic', 'x2apic')
    elif case == 'devices':
        log = run(case, 'max,+x2apic', 'x2apic', devices=True)
        assert '[64] ROOT nvme0n1p1' in log and 'Intel 82574L (e1000e) native DMA/IRQ ready' in log and '[64] USB boot enumeration complete, devices=4' in log, log[-3000:]
    elif case == 'whpx':
        log = run(case, 'qemu64,+rdrand' + build.host_cpu_flags() + ',+x2apic', 'x2apic', accel=('-accel', 'whpx,kernel-irqchip=off'))
    elif case == 'default':
        log = run(case, 'max,+x2apic', None)
        assert 'x2APIC mode' not in log and 'CPUs online (big kernel lock, LAPIC IPIs' in log, 'xAPIC unless asked'
    else:
        log = run(case, 'max,+x2apic', 'x2apic nox2apic')
        assert 'x2APIC mode' not in log and 'CPUs online (big kernel lock, LAPIC IPIs' in log, 'nox2apic wins'
    if case == 'whpx' and 'the platform refused x2APIC mode; xAPIC' in log:
        assert 'CPUs online (big kernel lock, LAPIC IPIs' in log, 'fallback to xAPIC on every CPU'
        print('  WHPX (kernel-irqchip=off) refuses the IA32_APIC_BASE x2APIC switch: graceful xAPIC fallback verified', flush=True)
    elif case in ('tcg', 'devices', 'whpx'):
        assert re.search(r'\[64\] APIC: x2APIC mode \(MSR interface, kernel command line\), BSP x2APIC id 0', log), 'x2APIC entered'
        assert 'CPUs online (big kernel lock, x2APIC MSR IPIs' in log, 'APs started and running in x2APIC mode'
    results[case] = 'PASS'
    print('PASS', case, flush=True)
print('PASS x2APIC:', ' '.join(results))
