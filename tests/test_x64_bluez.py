"""BlueZ end to end on native NexusOS: Debian 13's bluetoothd/bluetoothctl/btmon (arch/x86_64/bluez_install.py, pinned
bluez-lock.json) over the kernel's Bluetooth stack (bluetooth.c + bt_*.c: mgmt, HCI, L2CAP, SMP, RFCOMM), with BlueZ's
own controller emulator btvirt (bluez-test-tools) providing two virtual controllers on /dev/vhci that see each other.
The guest driver arch/x86_64/bluez_user.c (/bin/hw64 in the /nexus/test-hw campaign, root) runs a script per case and
the campaign checks afterwards that every page came back.

Usage: python -I tests/test_x64_bluez.py [WORK] [--tcg] [--smp N] [--qmp-port N] [--only CASE ...] [--script FILE]
Cases: see CASES below. --script runs a host file as the guest script (exploration).
signalfd/timerfd: BlueZ's mainloop uses the kernel's own (signalfd.c, posix_timers.c; the LD_PRELOAD emulation
tests/bt_shim.c of the hw2 worktree was dropped at the wave-2 merge)."""
from pathlib import Path
import argparse, importlib.util, json, os, re, shutil, socket, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'

DBUS_CONF = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=/tmp/dbus/system_bus_socket</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <deny own="*"/>
    <deny send_type="method_call"/>
    <allow send_type="signal"/>
    <allow send_requested_reply="true" send_type="method_return"/>
    <allow send_requested_reply="true" send_type="error"/>
    <allow receive_type="method_call"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
    <allow receive_type="signal"/>
    <allow send_destination="org.freedesktop.DBus" send_interface="org.freedesktop.DBus"/>
    <allow send_destination="org.freedesktop.DBus" send_interface="org.freedesktop.DBus.Introspectable"/>
    <allow send_destination="org.freedesktop.DBus" send_interface="org.freedesktop.DBus.Properties"/>
  </policy>
  <policy user="root">
    <allow send_destination="*" eavesdrop="true"/>
  </policy>
  <includedir>/usr/share/dbus-1/system.d</includedir>
</busconfig>
"""
SETUP = r"""
env PATH=/usr/bin:/usr/sbin:/bin:/sbin:/usr/libexec/bluetooth
env DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/dbus/system_bus_socket
env STATE_DIRECTORY=/tmp/bt/state
env HOME=/root
mkdir /tmp/bt/state
mkdir /tmp/dbus
spawn dbus /tmp/bt/dbus.log dbus-daemon --config-file=/etc/nexus-bt/system.conf --nofork --nopidfile
sleep 1500
"""
DAEMONS = r"""
spawn btmon /tmp/bt/btmon.txt btmon
sleep 300
spawn btvirt /tmp/bt/btvirt.log btvirt -l2
sleep 1500
spawn bluetoothd /tmp/bt/bluetoothd.log bluetoothd -n -d
sleep 3000
run 8 btmgmt -i 1 info
expect addr 00:AA:01:01:00:01
session ctl /tmp/bt/ctl.txt bluetoothctl --agent NoInputNoOutput
sleep 2000
"""
TEARDOWN = r"""
send ctl quit
sleep 1000
cat /tmp/bt/ctl.txt
kill bluetoothd
sleep 500
kill btvirt
sleep 500
kill btmon
fail-if /tmp/bt/bluetoothd.log Unknown Command
fail-if /tmp/bt/bluetoothd.log Not Supported
cat /tmp/bt/bluetoothd.log 60
cat /tmp/bt/btmon.txt 120
"""
# hci0 = 00:AA:01:00:00:00 (the "host", bluetoothctl), hci1 = 00:AA:01:01:00:01 (the peer); both btvirt controllers
BREDR = SETUP + DAEMONS + r"""
send ctl select 00:AA:01:01:00:01
send ctl system-alias NexusPeer
send ctl pairable on
send ctl discoverable on
waitlog /tmp/bt/ctl.txt 10 Discoverable: yes
send ctl select 00:AA:01:00:00:00
send ctl power on
send ctl scan on
waitlog /tmp/bt/ctl.txt 30 Device 00:AA:01:01:00:01 NexusPeer
send ctl scan off
sleep 1000
send ctl pair 00:AA:01:01:00:01
waitlog /tmp/bt/ctl.txt 40 Pairing successful
waitlog /tmp/bt/ctl.txt 10 ServicesResolved: yes
spawn l2srv /tmp/bt/l2srv.txt /bin/hw64 l2-server 0x1001 bredr 2
sleep 700
run 40 /bin/hw64 l2-client 00:AA:01:01:00:01 0x1001 bredr 40 2
expect L2 ECHO OK messages=40
expect security=2
spawn rcsrv /tmp/bt/rcsrv.txt /bin/hw64 rc-server 5 2
sleep 700
run 60 /bin/hw64 rc-client 00:AA:01:01:00:01 5 60000 2
expect RC ECHO OK bytes=60000
run 20 l2ping -c 3 00:AA:01:01:00:01
expect 3 sent, 3 received
send ctl info 00:AA:01:01:00:01
waitlog /tmp/bt/ctl.txt 10 Bonded: yes
waitlog /tmp/bt/ctl.txt 10 Paired: yes
send ctl remove 00:AA:01:01:00:01
waitlog /tmp/bt/ctl.txt 20 has been removed
cat /tmp/bt/l2srv.txt
cat /tmp/bt/rcsrv.txt
""" + TEARDOWN
LE = SETUP + DAEMONS + r"""
send ctl select 00:AA:01:01:00:01
send ctl system-alias NexusPeerLE
send ctl pairable on
send ctl advertise.name on
send ctl advertise on
waitlog /tmp/bt/ctl.txt 10 Advertising object registered
send ctl select 00:AA:01:00:00:00
send ctl scan le
waitlog /tmp/bt/ctl.txt 30 Device 00:AA:01:01:00:01 NexusPeerLE
send ctl scan off
sleep 1000
send ctl pair 00:AA:01:01:00:01
waitlog /tmp/bt/ctl.txt 40 Pairing successful
waitlog /tmp/bt/ctl.txt 10 ServicesResolved: yes
send ctl info 00:AA:01:01:00:01
waitlog /tmp/bt/ctl.txt 10 Bonded: yes
spawn l2srv /tmp/bt/l2srv.txt /bin/hw64 l2-server 0x0081 le 2
sleep 700
run 40 /bin/hw64 l2-client 00:AA:01:01:00:01 0x0081 le 30 2
expect L2 ECHO OK messages=30
send ctl disconnect 00:AA:01:01:00:01
waitlog /tmp/bt/ctl.txt 10 Connected: no
send ctl remove 00:AA:01:01:00:01
waitlog /tmp/bt/ctl.txt 20 has been removed
waitlog /tmp/bt/btmon.txt 5 Authentication requirement: Bonding, No MITM, SC
cat /tmp/bt/l2srv.txt
""" + TEARDOWN
HOSTILE = r"""
env PATH=/usr/bin:/usr/sbin:/bin:/sbin
run 180 /bin/hw64 hostile
expect HOSTILE PASSED
"""
TESTERS = r"""
env PATH=/usr/bin:/usr/sbin:/bin:/sbin
env SUITE_DETAIL=1
run 1600 /bin/hw64 suite l2cap-tester 20
run 900 /bin/hw64 suite smp-tester 20
run 900 /bin/hw64 suite rfcomm-tester 20
"""
CASES = {'bredr': BREDR, 'le': LE, 'hostile': HOSTILE, 'bredr_smp': BREDR, 'le_smp': LE, 'hostile_smp': HOSTILE, 'testers': TESTERS}
DEFAULT_CASES = ['bredr', 'le', 'hostile', 'bredr_smp', 'le_smp', 'hostile_smp']   # testers: run with --only testers (BlueZ's conformance suites)

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\wave2\ws_hw2_runs\bluez'))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4757)
parser.add_argument('--smp', type=int, default=0, help='CPUs (default: each case its own)')
parser.add_argument('--only', nargs='+', default=None); parser.add_argument('--script', type=Path)
parser.add_argument('--cache', default=os.environ.get('NEXUS_BLUEZ_PKGS', r'D:\nexustest\wave2\ws_hw2_pkgs'))
parser.add_argument('--timeout', type=int, default=600)
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland' / 'fatput.py')
bluez = module('bluez_install', HERE / 'bluez_install.py')
CC = [str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-Wall', '-Wextra', '-Werror']


def prepare():
    driver = WORK / 'bluez_user.elf'
    subprocess.run(CC + ['-fPIE', '-pie', str(HERE / 'bluez_user.c'), '-o', str(driver)], check=True)
    base = WORK / 'base.img'; shutil.copyfile(OUT / 'apps64.img', base)
    assert bluez.install([str(base), '--groups', 'bluez,bluez-test', '--grow', '192', '--cache', options.cache, '--report', str(WORK / 'bluez-install.json')]) == 0
    conf = WORK / 'system.conf'; conf.write_text(DBUS_CONF, newline='\n')
    marker = WORK / 'marker'; marker.write_bytes(b'b')
    files = {'passwd': 'root:x:0:0:root:/root:/bin/sh\nmessagebus:x:100:101::/nonexistent:/usr/sbin/nologin\nnobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n',
             'group': 'root:x:0:\nmessagebus:x:101:\nbluetooth:x:102:\nnogroup:x:65534:\n', 'machine-id': '4e657875734f53426c7565746f6f7468\n'}
    for name, text in files.items(): (WORK / name).write_text(text, newline='\n')
    fatput.main(['fatput', str(base), 'put', str(driver), 'bin/hw64',
                 'put', str(conf), 'etc/nexus-bt/system.conf', 'put', str(marker), 'nexus/test-hw', 'put', str(marker), 'nexus/init-as-root',
                 'put', str(WORK / 'passwd'), 'etc/passwd', 'put', str(WORK / 'group'), 'etc/group', 'put', str(WORK / 'machine-id'), 'etc/machine-id',
                 'mkdir', 'run/dbus', 'mkdir', 'var/lib/bluetooth', 'mkdir', 'root'])
    return base


def boot(base, case, text, smp):
    work = WORK / case; work.mkdir(exist_ok=True)
    disk = work / 'disk.img'; shutil.copyfile(base, disk)
    script = work / 'script.txt'; script.write_text(text, newline='\n')
    fatput.main(['fatput', str(disk), 'put', str(script), 'nexus/bt-script'])
    serial = work / 'serial.log'; serial.unlink(missing_ok=True)
    command = [QEMU, *build.desktop_cpu_options(options.tcg), '-m', '1536', '-smp', str(smp), '-machine', 'q35',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    process = subprocess.Popen([str(c) for c in command], stdout=(work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    build.unthrottle(process.pid)
    print(case, 'VM', process.pid, 'smp', smp, flush=True)
    try:
        deadline = time.monotonic() + options.timeout
        while time.monotonic() < deadline:
            out = serial.read_bytes().decode('latin1') if serial.exists() else ''
            if re.search(r'\[64\] HW CAMPAIGN PASSED|\[64\] FAIL:|KERNEL_FAULT|PANIC', out) or process.poll() is not None: break
            time.sleep(.5)
        time.sleep(.5)
    finally:
        try:
            with socket.create_connection(('127.0.0.1', options.qmp_port), timeout=5) as link:
                stream = link.makefile('rwb'); stream.readline()
                for request in ({'execute': 'qmp_capabilities'}, {'execute': 'quit'}):
                    stream.write(json.dumps(request).encode() + b'\n'); stream.flush(); stream.readline()
        except OSError: pass
        try: process.wait(30)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        disk.unlink(missing_ok=True)
    out = serial.read_bytes().decode('latin1') if serial.exists() else ''
    return out


def check(case, out):
    problems = [p for p in ('KERNEL_FAULT', '[64] FAIL:', 'PANIC') if p in out]
    if '[64] BZ TEST PASSED' not in out: problems.append('guest script failed')
    if '[64] HW CAMPAIGN PASSED' not in out: problems.append('campaign did not pass (pages or exit status)')
    return problems


def main():
    cases = {'script': options.script.read_text()} if options.script else {c: CASES[c] for c in (options.only or DEFAULT_CASES)}
    base = prepare()
    failed = []
    try:
        for case, text in cases.items():
            smp = options.smp or (2 if case.endswith('_smp') else 1)
            out = boot(base, case, text, smp)
            for line in out.splitlines():
                if 'BZ ' in line or 'BLUETOOTH' in line or 'ENOSYS' in line or 'USER_FAULT' in line or 'KERNEL_FAULT' in line: print(line[:300], flush=True)
            problems = check(case, out)
            print(('FAIL ' + case + ': ' + '; '.join(problems)) if problems else 'PASS ' + case, flush=True)
            if problems: failed.append(case)
    finally:
        base.unlink(missing_ok=True)
    print('SUMMARY bluez', len(cases) - len(failed), '/', len(cases), 'passed' + ('' if not failed else ' (failed: ' + ' '.join(failed) + ')'), flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
