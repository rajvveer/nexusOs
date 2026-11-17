"""ACPI interpreter (uACPI) and ACPI device drivers end to end on QEMU.

Usage: python -I tests/test_x64_acpi.py [WORK] [--smp N] [--whpx] [--only NAME] [--qmp-port 4630]

QEMU has no battery, lid or thermal hardware, so tests/acpi_test_ssdt.py adds them (-acpitable) with methods that read
a state block in CMOS RAM; the harness rewrites it through the QEMU monitor and then raises the GPE of the table's
GPE block device (pc-testdev's scratch register + ISA IRQ 3), whose _E00 method Notify()s the changed devices.
/bin/power64 (power_user.c) checks the Linux interfaces and waits for each event the harness causes. Runs:
  devices    q35, --smp CPUs: sysfs/uevent/input for AC, battery (low, critical, removal), lid, buttons (control method
             and the fixed power button through system_powerdown), backlight hotkeys/stores, thermal active/passive,
             hostile stores; ends with the power button -> orderly shutdown (SIGTERM to the program).
  lid-policy lid_switch_external_power=ignore: closing the lid on AC does nothing, on battery lid_switch suspends (S3);
             then a second display (virtio-gpu head 1 enabled by a VNC client's SetDesktopSize, as plugging in a monitor):
             closing the lid on battery is ignored (lid_switch_docked default).
  battery    critical battery on DC: the kernel's critical action (no hibernation area: power-off) after 10 s.
  thermal    temperature above _CRT: orderly critical shutdown.
  fallback   acpi.testfail and acpi=off: table-only mode, the boot self-tests still pass (1 CPU, i440fx).
"""
from pathlib import Path
import argparse
import importlib.util
import json
import re
import shutil
import socket
import struct
import subprocess
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('acpi'))
parser.add_argument('--smp', type=int, default=2)
parser.add_argument('--whpx', action='store_true')
parser.add_argument('--only', action='append')
parser.add_argument('--qmp-port', type=int, default=4630)
parser.add_argument('--vnc-port', type=int, default=9161, help='lid-policy: VNC server of the second display head')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland/fatput.py')
ssdt = module('acpi_test_ssdt', ROOT / 'tests/acpi_test_ssdt.py')
(WORK / 'ssdt.aml').write_bytes(ssdt.build(options.smp))
program = OUT / 'power_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'power_user.c'), '-o', str(program)], check=True)


class Qmp:
    def __init__(self, port):
        deadline = time.time() + 20
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=60); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(0.2)
        self.f = self.s.makefile('rwb')
        json.loads(self.f.readline()); self.command('qmp_capabilities')

    def command(self, name, **arguments):
        self.f.write(json.dumps({'execute': name, 'arguments': arguments}).encode() + b'\n'); self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if 'return' in reply: return reply['return']
            if 'error' in reply: raise RuntimeError(f'{name}: {reply["error"]}')

    def hmp(self, *lines):
        for line in lines:
            out = self.command('human-monitor-command', **{'command-line': line})
            if out.strip(): raise RuntimeError(f'monitor: {line}: {out}')

    def close(self):
        try: self.s.close()
        except OSError: pass


class State:
    fields = dict(ac=True, lid=True, battery=True, sleep_presses=0, brightness_up=0, battery_state=2, remaining=40000, rate=5000,
                  temperature=3082, brightness_down=0, power_presses=0, info=0, sequence=0, ppc=0)

    def __init__(self): self.v = dict(self.fields)
    def commands(self): return ssdt.state_commands(ssdt.state_bytes(**self.v))


def make_disk(tag, mode):
    disk = WORK / (tag + '.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / (tag + '.marker'); marker.write_bytes(mode.encode())
    fatput.main(['fatput', str(disk), 'put', str(program), 'bin/power64', 'put', str(marker), 'nexus/test-power'])
    return disk


def boot(tag, disk=None, smp=1, machine='q35', cmdline='', devices=True, s3=False, gpu='virtio-vga', extra=(), prepare=None):
    """Start paused; with `devices` the initial CMOS state is stored before the firmware runs. Returns (vm, serial, qmp)."""
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    args = [QEMU, '-machine', machine, '-m', '512', '-smp', str(smp), '-display', 'none', '-vga', 'none', '-device', gpu, *extra,
            '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a', '-serial', f'file:{serial}',
            '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-S',
            *(['-global', 'ICH9-LPC.disable_s3=0'] if s3 else []),
            '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    args += ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand'] if options.whpx else ['-cpu', 'max']
    if devices: args += ['-acpitable', 'file=ssdt.aml', '-device', 'pc-testdev']   # relative: QEMU splits file= on ':'
    if disk:
        if machine == 'q35': args += ['-drive', f'if=none,id=root,file={disk},format=raw', '-device', 'nvme,serial=nexus-root,drive=root']
        else: args += ['-drive', f'file={disk},format=raw,if=ide']
    if cmdline: args += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
    vm = subprocess.Popen(args, cwd=WORK)
    qmp = Qmp(options.qmp_port)
    if devices: qmp.hmp(*State().commands())
    if prepare: prepare()
    qmp.command('cont')
    return vm, serial, qmp


def log_of(serial): return serial.read_text(errors='replace') if serial.exists() else ''


def wait_for(vm, serial, pattern, timeout, start=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = log_of(serial)
        if re.search(pattern, text[start:]): return text
        if '[64] POWER FAIL' in text[start:] or 'KERNEL_FAULT' in text or '[64] FAIL' in text:
            raise AssertionError('failure in ' + str(serial) + ':\n' + text[-4000:])
        if vm.poll() is not None: raise AssertionError(f'VM exited {vm.returncode} waiting for {pattern}:\n' + text[-4000:])
        time.sleep(0.1)
    raise AssertionError(f'timeout waiting for {pattern} in {serial}:\n' + log_of(serial)[-4000:])


def finish(vm, timeout=60):
    try: return vm.wait(timeout)
    except subprocess.TimeoutExpired: vm.kill(); vm.wait(); raise AssertionError('VM did not exit')


def check_clean(text):
    for bad in ('KERNEL_FAULT', '[64] FAIL', 'POWER FAIL', 'ACPI error', 'ACPI heap: invalid'):
        assert bad not in text, (bad, text[-3000:])


def raise_event(qmp, state):
    state.v['sequence'] += 1
    qmp.hmp(*state.commands(), *ssdt.event_commands())


STEPS = {
    'ac-offline': dict(ac=False, battery_state=1), 'battery-low': dict(remaining=3840),
    'battery-critical': dict(remaining=960, battery_state=5), 'ac-online': dict(ac=True, battery_state=2, remaining=1200),
    'lid-close': dict(lid=False), 'lid-open': dict(lid=True), 'sleep-button': 'sleep_presses', 'power-button-method': 'power_presses',
    'power-button-fixed': 'powerdown', 'brightness-up': 'brightness_up', 'brightness-down': 'brightness_down',
    'thermal-active': dict(temperature=2732 + 720), 'thermal-passive': dict(temperature=2732 + 820),
    'thermal-normal': dict(temperature=2732 + 400), 'battery-removed': dict(battery=False), 'battery-inserted': dict(battery=True),
    'cpufreq-performance': dict(ppc=1), 'cpufreq-powersave': dict(ppc=0), 'cpufreq-schedutil-busy': 'sample', 'cpufreq-schedutil-idle': 'sample',
}
# acpi-cpufreq: the test SSDT's _PCT register (pc-testdev byte 0xE2) before and after each step: 0 = 2400, 1 = 1800, 2 = 1200 MHz
PSTATE_BEFORE = {'cpufreq-performance': 0, 'cpufreq-powersave': 2}
PSTATE_AFTER = {'cpufreq-performance': 1, 'cpufreq-powersave': 2}
PSTATE_SEEN = {'cpufreq-schedutil-busy': 0, 'cpufreq-schedutil-idle': 2}


def read_pstate(qmp):
    out = qmp.command('human-monitor-command', **{'command-line': f'i /b {ssdt.PSTATE_PORT:#x}'})
    return int(out.strip().split('=')[-1], 16)


def run_devices():
    disk = make_disk('devices', 'a')
    vm, serial, qmp = boot('devices', disk, smp=options.smp)
    try:
        text = wait_for(vm, serial, r'\[64\] POWER WAIT ', 120)
        state = State(); done = 0
        assert '[64] ACPI AML ready' in text and 'ACPI battery BAT0' in text and 'ACPI thermal zone' in text and 'ACPI GPE block' in text, text[-3000:]
        assert '[64] POWER initial sysfs state checked' in text and '[64] POWER hostile stores refused' in text, text[-3000:]
        for name, change in STEPS.items():
            start = len(log_of(serial))
            wait_for(vm, serial, r'\[64\] POWER WAIT ' + re.escape(name), 60)
            time.sleep(0.2)
            if name in PSTATE_BEFORE: assert read_pstate(qmp) == PSTATE_BEFORE[name], (name, read_pstate(qmp))
            if change == 'powerdown': qmp.command('system_powerdown')
            elif change == 'sample':   # schedutil follows the load: the wanted state must show up within 2 s
                seen = []; end = time.time() + 2
                while time.time() < end and PSTATE_SEEN[name] not in seen: seen.append(read_pstate(qmp)); time.sleep(0.05)
                assert PSTATE_SEEN[name] in seen, (name, seen[-20:])
            else:
                if isinstance(change, str): state.v[change] += 1
                else: state.v.update(change)
                raise_event(qmp, state)
            wait_for(vm, serial, r'\[64\] POWER STEP ' + re.escape(name) + ' PASS', 60, start)
            if name in PSTATE_AFTER: assert read_pstate(qmp) == PSTATE_AFTER[name], (name, read_pstate(qmp))
            done += 1
        text = wait_for(vm, serial, r'\[64\] POWER WAIT final-power-button', 60)
        assert '[64] POWER ACPI DEVICES ALL PASSED' in text, text[-3000:]
        qmp.command('system_powerdown')
        text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 60)
        code = finish(vm); text = log_of(serial)
        assert code == 33, code
        assert '[64] POWER ORDERLY SHUTDOWN: power button' in text and 'SIGTERM received after the power button' in text, text[-3000:]
        assert 'POWER CAMPAIGN STATUS=0x000000000000008F' in text, text[-3000:]
        assert 'battery BAT0 low' in text and 'battery critical' in text and 'critical battery action cancelled' in text, text[-3000:]
        assert 'active cooling level 0' in text and 'passive cooling cap' in text, text[-3000:]
        check_clean(text)
        print(f'PASS devices ({done} hardware steps, {options.smp} CPUs)', flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()


def run_terminal(tag, change, expect):
    disk = make_disk(tag, 'w')
    vm, serial, qmp = boot(tag, disk, smp=options.smp)
    try:
        wait_for(vm, serial, r'\[64\] POWER READY', 120)
        state = State(); state.v.update(change); raise_event(qmp, state)
        text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 90)
        code = finish(vm); text = log_of(serial)
        assert code == 33, code
        assert expect in text and 'SIGTERM received, exiting' in text and 'POWER CAMPAIGN STATUS=0x000000000000008F' in text, text[-3000:]
        check_clean(text)
        print('PASS', tag, flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()


def read_cmos(qmp, index):
    qmp.hmp(f'o /b 0x70 {index:#x}')
    out = qmp.command('human-monitor-command', **{'command-line': 'i /b 0x71'})
    return int(out.strip().split('=')[-1], 16)


def run_lid():
    """Lid closed -> the default policy suspends (S3); the lid's _PRW wake GPE was set up and _DSW(1, 3) armed it;
    system_wakeup resumes, _DSW(0, 0) disarms it; the lid opens; the power button ends the run."""
    disk = make_disk('lid', 'w')
    vm, serial, qmp = boot('lid', disk, smp=options.smp, s3=True)
    try:
        text = wait_for(vm, serial, r'\[64\] POWER READY', 120)
        assert re.search(r'ACPI wake device \\_SB_\.NXLD GPE 0x0+ S3 \(enabled\)', text), text[-3000:]
        start = len(text); state = State(); state.v.update(lid=False); raise_event(qmp, state)
        wait_for(vm, serial, r'\[64\] PM: entering S3', 60, start)
        deadline = time.time() + 60
        while qmp.command('query-status')['status'] != 'suspended':
            assert time.time() < deadline, 'not suspended after the lid closed'
            time.sleep(0.1)
        armed = read_cmos(qmp, ssdt.CMOS_BASE + 17)
        assert armed == 0x31, f'_DSW while asleep: {armed:#x} (0x31 = enabled for S3)'
        qmp.command('system_wakeup')
        wait_for(vm, serial, r'\[64\] PM: woke from S3', 60, start)
        time.sleep(1)
        disarmed = read_cmos(qmp, ssdt.CMOS_BASE + 17)
        assert disarmed == 0, f'_DSW after the wake: {disarmed:#x}'
        state.v.update(lid=True); raise_event(qmp, state)
        wait_for(vm, serial, r'\[64\] LID opened', 30, start)
        time.sleep(2)   # a press within 1.5 s of the resume is the waking press (KEY_WAKEUP only, pm.c pm_waking)
        qmp.command('system_powerdown')
        text = wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 90)
        code = finish(vm); text = log_of(serial)
        assert code == 33 and 'LID closed' in text and 'POWER CAMPAIGN STATUS=0x000000000000008F' in text, (code, text[-3000:])
        check_clean(text)
        print(f'PASS lid (closed -> S3, _DSW armed {armed:#x} / disarmed {disarmed:#x}, {options.smp} CPUs)', flush=True)
    finally:
        qmp.close()
        if vm.poll() is None: vm.kill(); vm.wait()


def vnc_set_desktop_size(port, width=1280, height=800, timeout=20):
    """Minimal RFB 3.8 client: SetDesktopSize on the VNC server bound to a virtio-gpu head makes QEMU report that head
    enabled (GET_DISPLAY_INFO), like a monitor being plugged in. Returns the open connection."""
    deadline = time.time() + timeout
    while True:
        try: link = socket.create_connection(('127.0.0.1', port), timeout=10); break
        except OSError:
            if time.time() > deadline: raise
            time.sleep(0.2)
    def read(n):
        data = b''
        while len(data) < n:
            chunk = link.recv(n - len(data))
            if not chunk: raise AssertionError('VNC server closed the connection')
            data += chunk
        return data
    read(12); link.sendall(b'RFB 003.008\n')
    types = read(read(1)[0]); assert 1 in types, f'VNC security types {list(types)}'
    link.sendall(b'\x01'); assert struct.unpack('>I', read(4))[0] == 0, 'VNC security result'
    link.sendall(b'\x01'); read(20); read(struct.unpack('>I', read(4))[0])   # ClientInit (shared); ServerInit + name
    link.sendall(struct.pack('>BBHi', 2, 0, 1, -308))                           # SetEncodings: ExtendedDesktopSize
    link.sendall(struct.pack('>BBHHBB', 251, 0, width, height, 1, 0) + struct.pack('>IHHHHI', 0, 0, 0, width, height, 0))
    return link


def run_lid_policy():
    for docked in (False, True):
        tag = 'lidpolicy-docked' if docked else 'lidpolicy'
        disk = make_disk(tag, 'L'); links = []
        extra = ['-vnc', f'127.0.0.1:{options.vnc_port - 5900},display=gpu0,head=1'] if docked else []
        vm, serial, qmp = boot(tag, disk, smp=options.smp, s3=True, gpu='virtio-vga,id=gpu0,max_outputs=2', extra=extra,
                               prepare=(lambda: links.append(vnc_set_desktop_size(options.vnc_port))) if docked else None)
        try:
            text = wait_for(vm, serial, r'\[64\] POWER LID POLICY READY', 120)
            heads = re.search(r'\[64\] KMS ready heads=(\d+) connected=(\d+)', text)
            assert heads and heads.group(2) == ('2' if docked else '1'), (heads and heads.group(0), text[-3000:])
            state = State()
            if not docked:   # on AC (the initial state): lid_switch_external_power=ignore
                start = len(text); state.v.update(lid=False); raise_event(qmp, state)
                wait_for(vm, serial, r'\[64\] POWER: lid closed on external power: ignore', 30, start)
                wait_for(vm, serial, r'\[64\] KMS built-in panel off \(lid closed\)', 30, start)
                time.sleep(3)
                assert 'PM: entering' not in log_of(serial)[start:] and qmp.command('query-status')['status'] == 'running', 'suspended on AC'
                state.v.update(lid=True); raise_event(qmp, state); wait_for(vm, serial, r'\[64\] KMS built-in panel on \(lid open\)', 30, start)
            start = len(log_of(serial)); state.v.update(ac=False, battery_state=1); raise_event(qmp, state)
            wait_for(vm, serial, r'\[64\] POWER AC \S+ offline', 30, start)
            start = len(log_of(serial)); state.v.update(lid=False); raise_event(qmp, state)
            if docked:   # a second display: lid_switch_docked=ignore even on battery
                wait_for(vm, serial, r'\[64\] POWER: lid closed with an external display: ignore', 30, start)
                wait_for(vm, serial, r'\[64\] KMS built-in panel off \(lid closed\)', 30, start)
                time.sleep(3)
                assert 'PM: entering' not in log_of(serial)[start:] and qmp.command('query-status')['status'] == 'running', 'suspended while docked'
            else:        # on battery: lid_switch (suspend)
                wait_for(vm, serial, r'\[64\] PM: entering S3', 60, start)
                deadline = time.time() + 60
                while qmp.command('query-status')['status'] != 'suspended':
                    assert time.time() < deadline, 'not suspended after the lid closed on battery'
                    time.sleep(0.1)
                qmp.command('system_wakeup'); wait_for(vm, serial, r'\[64\] PM: woke from S3', 60, start)
            state.v.update(lid=True); raise_event(qmp, state); wait_for(vm, serial, r'\[64\] LID opened', 30, start)
            time.sleep(2)   # a press within 1.5 s of a resume is the waking press
            qmp.command('system_powerdown')
            wait_for(vm, serial, r'POWER CAMPAIGN STATUS=', 90)
            code = finish(vm); text = log_of(serial)
            assert code == 33 and 'POWER CAMPAIGN STATUS=0x000000000000008F' in text, (code, text[-3000:])
            check_clean(text)
            print(f'PASS {tag} ({heads.group(0)}; ' + ('lid closed on battery with 2 displays ignored' if docked else
                  'lid closed on AC ignored, on battery suspended + resumed') + f', {options.smp} CPUs)', flush=True)
        finally:
            for link in links: link.close()
            qmp.close()
            if vm.poll() is None: vm.kill(); vm.wait()


def run_fallback():
    for tag, cmdline, expect in [('fallback-fail', 'acpi.testfail', 'AML interpreter unavailable (acpi.testfail); continuing in table-only mode'),
                                 ('fallback-off', 'acpi=off', 'AML interpreter disabled on the command line')]:
        vm, serial, qmp = boot(tag, None, smp=1, machine='pc', cmdline=cmdline, devices=False)
        try:
            code = finish(vm, 120); text = log_of(serial)
        finally: qmp.close()
        assert code == 33 and '[64] ALL TESTS PASSED' in text and expect in text and '[64] ACPI MADT' in text, (code, text[-3000:])
        assert '[64] IRQ mode: IOAPIC' in text and 'KERNEL_FAULT' not in text, text[-3000:]
        print('PASS', tag, flush=True)


runs = {'devices': run_devices, 'lid': run_lid, 'lid-policy': run_lid_policy,
        'battery': lambda: run_terminal('battery', dict(ac=False, battery_state=5, remaining=480), 'POWER ORDERLY SHUTDOWN: critical battery'),
        'thermal': lambda: run_terminal('thermal', dict(temperature=2732 + 1020), 'POWER ORDERLY SHUTDOWN: critical temperature'),
        'fallback': run_fallback}
def remove_images():   # disk hygiene: no test disk outlives its run (also after a failure)
    for leftover in WORK.glob('*.img'):
        try: leftover.unlink()
        except OSError: pass


try:
    for name, function in runs.items():
        if options.only and name not in options.only: continue
        function()
        remove_images()
finally:
    remove_images()
print('PASS ACPI devices, events and fallback', flush=True)
