"""UPower and power profiles (nexus-upowerd) on an installed NexusOS: ext4 root, Debian base, system D-Bus.

    python -I tests/test_x64_upower.py WORK --base BASE [--accel auto|whpx|tcg] [--smp 2] [--qmp-port 4766]

BASE is an installed system (a tests/test_x64_services.py --keep-installed qcow2); nothing below it is written:
WORK/stage.qcow2 over it gets the worktree's services (sysinstall.py services --aurora --tests: nexus-upowerd, its unit,
D-Bus policy and activation files) and /usr/lib/nexus/tests/upower-test (arch/x86_64/upower_test_user.c). The machine
also gets the power test SSDT (tests/acpi_test_ssdt.py: AC adapter, battery, lid, processors with _PSS -> acpi-cpufreq)
on pc-testdev; its state block lives in CMOS, rewritten here through the QEMU monitor followed by the SSDT's GPE.

Guest side (tests/upower_guest.sh, root, from test-hook.service): the unit is active and owns the names; upower-test
check (objects, every property against sysfs, EnumerateDevices/GetDisplayDevice/GetCriticalAction, Set/HoldProfile/
ReleaseProfile, a holder leaving the bus, the legacy net.hadess name, hostile calls: unknown object/interface/property/
method, wrong types, read-only Set, 200-call burst); `nobody` is refused a profile change, the session user (nexus) may;
the API follows a direct write of /sys/power/policy/profile (what Aurora's battery menu does); Chrome headless (the
desktop's Chrome, a throw-away profile) reports navigator.getBattery() and its change events to the host; then the host
walks the battery through initial (AC, charging 83%) -> ac-off (discharging) -> low (7%) -> ac-on -> removed -> inserted
and checks after every step UPower's OnBattery / display device / warning level, the PropertiesChanged signals and what
Chrome saw. Results: WORK/result.json.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'tests'
OUT = ROOT / 'arch/x86_64/out'
sys.path.insert(0, str(HERE))
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4766)
ap.add_argument('--timeout', type=int, default=1500)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'upower', 'checks': [], 'passed': False}


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


ssdt = module('acpi_test_ssdt', HERE / 'acpi_test_ssdt.py')
(WORK / 'ssdt.aml').write_bytes(ssdt.build(args.smp))
client = OUT / 'upower_test_user.elf'
if not client.exists() or not (OUT / 'nexus_upowerd.elf').exists(): raise SystemExit('run arch/x86_64/build.py build first (upower_test_user.elf, nexus_upowerd.elf)')

STEPS = {   # simulated hardware per step, and what UPower must then report (display device)
    'initial':  (dict(ac=True, battery=True, battery_state=2, remaining=40000, rate=5000), dict(on_battery=0, present=1, state=1, percentage=83, warning=1)),
    'ac-off':   (dict(ac=False, battery=True, battery_state=1, remaining=40000, rate=5000), dict(on_battery=1, present=1, state=2, percentage=83, warning=1)),
    'low':      (dict(ac=False, battery=True, battery_state=1, remaining=3500, rate=5000), dict(on_battery=1, present=1, state=2, percentage=7, warning=3)),
    'ac-on':    (dict(ac=True, battery=True, battery_state=2, remaining=3500, rate=5000), dict(on_battery=0, present=1, state=1, percentage=7, warning=1)),
    'removed':  (dict(ac=True, battery=False, battery_state=0, remaining=0, rate=0), dict(on_battery=0, present=0)),
    'inserted': (dict(ac=True, battery=True, battery_state=2, remaining=20000, rate=4000), dict(on_battery=0, present=1, state=1, percentage=42, warning=1)),
}
sequence = 0


def hardware(vm, values):
    """Store the state block in CMOS and raise the SSDT's GPE: the kernel's real ACPI notify path runs."""
    global sequence
    sequence += 1
    state = dict(lid=True, sleep_presses=0, brightness_up=0, temperature=3082, brightness_down=0, power_presses=0, info=0, ppc=0)
    state.update(values); state['sequence'] = sequence & 255
    for line in ssdt.state_commands(ssdt.state_bytes(**state)) + ssdt.event_commands():
        out = vm.q.cmd('human-monitor-command', {'command-line': line})
        if out and out.strip(): raise RuntimeError('monitor: %s: %s' % (line, out))


HOOK = '#!/bin/sh\nwget -q -O /tmp/upower_guest.sh @HOST@/files/upower_guest.sh && sh /tmp/upower_guest.sh\n'   # @HOST@: HookServer.text()
started = time.time()
server = vm = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    def session(fs):
        pkg_vm.refresh_services(fs)   # the worktree's services (nexus-upowerd, unit, D-Bus files) over the installed base
        pkg_vm.put(fs, '/usr/lib/nexus/tests/upower-test', client.read_bytes(), 0o755)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
    print('stage overlay: %d blocks changed' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', session), flush=True)
    files = {'upower_guest.sh': (HERE / 'upower_guest.sh').read_bytes().replace(b'\r\n', b'\n')}
    for old in (WORK / 'uploads').glob('*') if (WORK / 'uploads').exists() else []: old.unlink()
    server = pkg_vm.HookServer(HOOK, files, WORK / 'uploads')
    os.chdir(WORK)   # -acpitable file= splits on ':' (a drive letter): QEMU gets a relative name
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='upower', accel=args.accel, smp=args.smp, memory='3072',
                   extra=['-acpitable', 'file=ssdt.aml', '-device', 'pc-testdev', '-S'], hook_port=server.port)
    hardware(vm, STEPS['initial'][0])   # before the firmware runs: the battery is there from the start
    vm.q.cmd('cont')
    logged_in, detail = pkg_vm.greeter_login(vm)
    check('logged in through the greeter', logged_in, detail)
    handled = set(); deadline = time.time() + args.timeout
    while time.time() < deadline and vm.proc.poll() is None:
        log = vm.text()
        if 'UPW DONE' in log or 'KERNEL_FAULT' in log: break
        for when, name, _ in list(server.events):
            if name in handled or not name.startswith('step-'): continue
            handled.add(name); step = name[5:]
            print('-- step', step, time.strftime('%H:%M:%S'), flush=True)
            hardware(vm, STEPS[step][0]); time.sleep(2)
            if step == 'initial':   # Chrome's first report must come before AC goes off (it took 12 s to start on a loaded host)
                until = time.time() + 90
                while time.time() < until and not any(n in ('chrome-initial', 'chrome-error') for _, n, _ in server.events): time.sleep(1)
                print('-- chrome initial report', 'seen' if time.time() < until else 'missing (90 s)', time.strftime('%H:%M:%S'), flush=True)
            server.open_gate(step)
        time.sleep(1)
    log = vm.text()
    for m in re.finditer(r'^UPW (\S+) (PASS|FAIL) ?(.*)$', log, re.M): check(m.group(1), m.group(2) == 'PASS', m.group(3))
    for m in re.finditer(r'^UPTEST FAIL: (.*)$', log, re.M): check('upower-test: ' + m.group(1)[:80], False, m.group(1))
    check('guest checks ran to the end', 'UPW DONE' in log, log[-400:])
    # UPower's view after every hardware step
    for step, (_, want) in STEPS.items():
        m = re.search(r'^UPW STATE %s UPTEST STATE (.*)$' % re.escape(step), log, re.M)
        if not check('state after %s reported' % step, m, ''): continue
        got = {k: v for k, v in re.findall(r'(\w+)=(\S+)', m.group(1))}
        bad = []
        for key, value in want.items():
            have = got.get(key)
            if key == 'percentage': ok = have is not None and abs(float(have) - value) <= 1.5
            else: ok = have is not None and int(float(have)) == value
            if not ok: bad.append('%s=%s (want %s)' % (key, have, value))
        if step == 'ac-off' and got.get('time_to_empty') and not (27000 <= int(got['time_to_empty']) <= 30600): bad.append('time_to_empty=%s (want ~28800)' % got['time_to_empty'])
        check('UPower after %s: %s' % (step, ' '.join('%s=%s' % kv for kv in want.items())), not bad, ' '.join(bad) + ' | ' + m.group(1))
    # signals: PropertiesChanged for the battery and the manager
    watch = (WORK / 'uploads' / 'watch.log').read_text('utf-8', 'replace') if (WORK / 'uploads' / 'watch.log').exists() else log
    signals = re.findall(r'UPTEST SIGNAL (\S+) (\S+) (.*)', watch)
    results['signals'] = [' '.join(s)[:200] for s in signals[:80]]
    check('PropertiesChanged OnBattery=1 on the manager', any(p == '/org/freedesktop/UPower' and 'OnBattery=1' in d for p, _, d in signals), results['signals'][:10])
    check('PropertiesChanged OnBattery=0 on the manager', any(p == '/org/freedesktop/UPower' and 'OnBattery=0' in d for p, _, d in signals), results['signals'][:10])
    check('PropertiesChanged WarningLevel=3 on the display device', any(p.endswith('/DisplayDevice') and 'WarningLevel=3' in d for p, _, d in signals), results['signals'][:10])
    check('PropertiesChanged IsPresent=0 on the battery', any('battery_' in p and 'IsPresent=0' in d for p, _, d in signals), results['signals'][:10])
    check('PropertiesChanged Percentage on the battery', any('battery_' in p and 'Percentage=' in d for p, _, d in signals), results['signals'][:10])
    # Chrome: navigator.getBattery() and its events
    chrome = [(name[7:], json.loads(body or b'{}')) for _, name, body in server.events if name.startswith('chrome-') and name != 'chrome-error']
    errors = [body for _, name, body in server.events if name == 'chrome-error']
    results['chrome'] = [[n, b] for n, b in chrome]
    check('Chrome: navigator.getBattery() resolved', chrome and not errors, errors[:3] or chrome[:3])
    initial = next((b for n, b in chrome if n == 'initial'), None)
    check('Chrome: initial level 0.83 charging', initial and abs(initial.get('level', 0) - 0.83) <= 0.02 and initial.get('charging') is True, initial)
    check('Chrome: charging became false (AC off)', any(n == 'chargingchange' and b.get('charging') is False for n, b in chrome), chrome[:8])
    check('Chrome: level fell to ~0.07 (low battery)', any(n == 'levelchange' and abs(b.get('level', 1) - 0.07) <= 0.02 for n, b in chrome), chrome[:8])
    check('Chrome: dischargingTime ~28800 s on battery', any(isinstance(b.get('dischargingTime'), (int, float)) and 27000 <= b['dischargingTime'] <= 30600 for n, b in chrome), chrome[:8])
    check('no kernel faults', 'KERNEL_FAULT' not in log, re.findall(r'.*KERNEL_FAULT.*', log)[:3])
    results['user_faults'] = re.findall(r'.*USER_FAULT.*', log)[:10]
    check('nexus-upowerd never faulted', not any('upower' in f for f in results['user_faults']), results['user_faults'])
finally:
    if vm: vm.close()
    if server: server.close()
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    (WORK / 'stage.qcow2').unlink(missing_ok=True)
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
