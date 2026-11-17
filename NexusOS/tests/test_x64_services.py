"""System services on an ext4 NexusOS root: Debian base installed by dpkg at first boot, the nexusd service manager and
journal, /dev/kmsg + dmesg, D-Bus system/session buses and desktop notifications, NTP (chrony) + timedatectl, locales,
host name, sudo/PAM, resource limits.

    python -I tests/test_x64_services.py WORK [--source FAT_DESKTOP.img] [--disk PREPARED.img] [--keep-disk]
           [--accel auto|whpx|tcg] [--smp 2] [--qmp-port 4672] [--ntp-port 8972]

Disk: by default a new ext4 root is built in WORK from --source (default NEXUS_TEST_SOURCE, arch/x86_64/nexus_paths.py;
only read): userland/fat2ext4.py, arch/x86_64/debian_base.py stage (base-lock.json: required + admin + services + Chrome's
runtime; user nexus / password nexus-test-pw, host nexus-svctest, Asia/Kolkata, en_US.UTF-8 + hi_IN), arch/x86_64/
sysinstall.py services --aurora --tests, plus the test units (t-*.service) and tests/services_guest.sh as svctest.service.
--disk takes such a prepared image instead (it is never written: every boot runs on WORK/overlay.qcow2).

Boot 1 runs the first-boot installation (nexus-firstboot.service) and then the guest checks; the session's main program
waits for them, so the desktop (Aurora in session mode) stops nexusd and the machine powers off cleanly. Boot 2 (same
overlay) checks persistence. The host serves NTP (UDP --ntp-port, guest 10.0.2.2) for chrony and checks the serial log
(SVC lines, Aurora's notification), the journal files on the overlay and that no kernel or user fault happened.
Result: WORK/result.json.
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
import sys
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'tests'
OUT = ROOT / 'arch/x86_64/out'
sys.path.insert(0, str(HERE))
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--disk', type=Path)
ap.add_argument('--keep-disk', action='store_true')
ap.add_argument('--keep-installed', type=Path, help='after a passing run, save the installed system as ONE compressed qcow2 here (the base for the apt/printing/software/update tests); the raw disk and overlay are deleted')
ap.add_argument('--base-cache', type=Path, help='debian_base.py --cache (verified .debs + signed indices; default: its own)')
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4672)
ap.add_argument('--ntp-port', type=int, default=8972)
ap.add_argument('--boot-timeout', type=int, default=2400)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'services', 'checks': [], 'passed': False}


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


UNITS = {
    't-a.service': '[Unit]\nDescription=ordering test A\n[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/bin/sh -c "date +%%s%%N > /run/t-a.time; sleep 1"\n',
    't-b.service': '[Unit]\nDescription=ordering test B\nRequires=t-a.service\nAfter=t-a.service\n[Service]\nType=oneshot\nExecStart=/bin/sh -c "date +%%s%%N > /run/t-b.time"\n',
    't-notify.service': '[Unit]\nDescription=readiness by sd_notify\n[Service]\nType=notify\nExecStart=/usr/lib/nexus/tests/nexus-svc-test notify 1500\n',
    't-ready.service': '[Unit]\nDescription=readiness by ReadyPath\n[Service]\nReadyPath=/run/t-ready.flag\nExecStartPre=/bin/rm -f /run/t-ready.flag\n'
                       'ExecStart=/bin/sh -c "sleep 2; touch /run/t-ready.flag; exec sleep 100000"\n[Install]\nWantedBy=multi-user\n',
    't-crash.service': '[Unit]\nDescription=crash loop\n[Service]\nExecStart=/usr/lib/nexus/tests/nexus-svc-test crash /run/t-crash.log\n'
                       'Restart=on-failure\nRestartSec=200ms\nStartLimitBurst=5\nStartLimitIntervalSec=60\n',
    't-flaky.service': '[Unit]\nDescription=fails twice then runs\n[Service]\nExecStart=/usr/lib/nexus/tests/nexus-svc-test flaky /var/lib/t-flaky.count 2\n'
                       'Restart=on-failure\nRestartSec=100ms\n',
    't-user.service': '[Unit]\nDescription=identity and environment\n[Service]\nType=oneshot\nUser=nobody\nGroup=nogroup\n'
                      'Environment="FOO=bar baz"\nWorkingDirectory=/tmp\nExecStart=/usr/lib/nexus/tests/nexus-svc-test env /tmp/t-user.env\n',
    't-badexec.service': '[Unit]\nDescription=missing program\n[Service]\nExecStart=/nonexistent/program --flag\n',
    't-cond.service': '[Unit]\nDescription=condition not met\nConditionPathExists=/nonexistent/flag\n[Service]\nExecStart=/bin/sleep 100\n',
    't-junk.service': '[Unit]\nDescription=unit with junk lines\nBogusKey=1\nthis line has no equals sign\n[Service]\nFrobnicate=yes\n'
                      'Type=oneshot\nExecStart=/bin/true\n[Weird]\nKey=value\n',
    't-log.service': '[Unit]\nDescription=log volume\n[Service]\nType=oneshot\nExecStart=/bin/sh -c \'for i in $(seq 1 200); do echo "t-log line $i '
                     'padding padding padding padding padding padding padding padding"; done; echo "<3>t-log error line"\'\n',
    't-stop.service': '[Unit]\nDescription=ExecStop\n[Service]\nExecStart=/bin/sleep 100000\nExecStop=/bin/sh -c "echo stopping > /run/t-stop.flag"\n',
    't-own@.service': '[Unit]\nDescription=user instance %i\n[Service]\nUser=%i\nExecStart=/bin/sleep 100000\nAllowUserInstance=yes\n',
    'svctest.service': '[Unit]\nDescription=services test (guest side)\nAfter=nexus-firstboot.service dbus.service cron.service chrony.service cups.service nexus-printers.service\n'
                       '[Service]\nType=oneshot\nExecStart=/bin/sh /usr/lib/nexus/tests/services_guest.sh\nStandardOutput=console\nTimeoutStartSec=1800\n'
                       '[Install]\nWantedBy=multi-user\nDefaultEnabled=yes\n',
}
# trust: the guest also reaches Debian's NTP pool through slirp; when the host clock is off by a second or two the pool
# majority would mark the test server a falseticker (seen: +2083 ms, ^x) and the selected-source check would fail.
CHRONY_TEST = 'server 10.0.2.2 port %d iburst prefer trust minpoll 0 maxpoll 2\n'


def run(*command):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run([str(c) for c in command], check=True)


def prepare():
    disk = pkg_vm.compressed_dir(WORK / 'base') / 'services.img'; disk.unlink(missing_ok=True)
    if args.disk:
        pkg_vm.sparse_copy(args.disk, disk); return disk
    run(sys.executable, '-I', ROOT / 'userland/fat2ext4.py', args.source, disk, '--size', '8192')
    config = ['USERNAME=nexus', 'FULLNAME=NexusOS User', 'PASSWORD=nexus-test-pw', 'HOSTNAME=nexus-svctest', 'TIMEZONE=Asia/Kolkata',
              'LOCALES=en_US.UTF-8 hi_IN', 'LANG_DEFAULT=en_US.UTF-8']
    run(sys.executable, '-I', ROOT / 'arch/x86_64/debian_base.py', 'stage', disk, *(['--cache', args.base_cache] if args.base_cache else []),
        *[x for c in config for x in ('--config', c)])
    stage = WORK / 'stage'; stage.mkdir(exist_ok=True)
    units = []
    for name, text in UNITS.items():
        (stage / name).write_text(text); units += ['--test-unit', '%s=%s' % (name, stage / name)]   # '%%' is the unit-file escape of '%'
    run(sys.executable, '-I', ROOT / 'arch/x86_64/sysinstall.py', 'services', disk, '--aurora', '--tests', *units)
    ext4lib = pkg_vm.ext4lib; fs = ext4lib.Writer(str(disk))
    def put(guest, data, mode):
        parts = fs.split(guest); fs.ensure_dir(parts[:-1])
        try:
            if fs.lookup(guest): fs.remove(guest)
        except (KeyError, ValueError, FileNotFoundError): pass
        fs.put_bytes(data, guest, mode)
    put('/usr/lib/nexus/tests/services_guest.sh', (HERE / 'services_guest.sh').read_bytes().replace(b'\r\n', b'\n'), 0o755)
    put('/usr/lib/nexus/tests/authpre.so', (OUT / 'auth_preload.so').read_bytes(), 0o755)   # LD_PRELOAD probe (AT_SECURE check)
    put('/etc/chrony/conf.d/svctest.conf', (CHRONY_TEST % args.ntp_port).encode(), 0o644)
    put('/nexus/desktop.args', b'/usr/lib/nexus/tests/svctest-wait\n', 0o644)
    fs.close(backups=True)
    errors, _, checker = ext4lib.check_image(str(disk)); checker.fs.f.close()   # the checker keeps the image open
    if errors: raise SystemExit('prepared image fails the ext4 check: %r' % errors[:5])
    return disk


class Ntp(threading.Thread):
    """Minimal SNTP server (RFC 4330, stratum 1) for chrony in the guest."""
    def __init__(self, port):
        super().__init__(daemon=True); self.requests = 0
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self.s.bind(('127.0.0.1', port)); self.start()
    def run(self):
        while True:
            try: data, peer = self.s.recvfrom(512)
            except OSError: return
            if len(data) < 48 or (data[0] & 7) != 3: continue
            self.requests += 1
            def stamp(t): sec = int(t) + 2208988800; return struct.pack('!II', sec, int((t - int(t)) * 2**32))
            receive = time.time()
            reply = bytes([(0 << 6) | (4 << 3) | 4, 1, data[2], 0xEC]) + struct.pack('!II', 0, 0x10) + b'NXTS' + stamp(receive) + data[40:48] + stamp(receive) + stamp(time.time())
            self.s.sendto(reply, peer)


def svc_lines(text):
    return {m.group(1): (m.group(2) == 'PASS', m.group(3)) for m in re.finditer(r'^SVC (\S+) (PASS|FAIL) ?(.*)$', text, re.M)}


def boot(disk, tag, first):
    vm = pkg_vm.Vm(WORK, disk, args.qmp_port, tag=tag, accel=args.accel, smp=args.smp, memory='2048', fresh_overlay=first)
    try:
        vm.wait(r'SVC DONE boot=', seconds=args.boot_timeout, fail=r'KERNEL_FAULT|\[64\] FAIL')
        code = vm.exited(240)
        text = vm.text()
        check('%s: clean power-off after the session' % tag, code is not None and 'INIT SESSION RETURNED' in text and 'nexusd] service manager stopped' in text,
              'exit=%s' % code)
        return text
    finally:
        vm.close(keep_overlay=True)


started = time.time()
disk = None
try:
    if not (OUT / 'desktop.elf').exists() or not (OUT / 'nexusd.elf').exists(): raise SystemExit('build first: arch/x86_64/build.py build + desktop.py')
    disk = prepare()
    ntp = Ntp(args.ntp_port)
    log1 = boot(disk, 'boot1', True)
    lines = svc_lines(log1)
    for name, (ok, detail) in sorted(lines.items()): check('boot1 ' + name, ok, detail)
    check('boot1: guest checks ran', len(lines) >= 60, '%d checks' % len(lines))
    check('boot1: Aurora showed the D-Bus notification', '[64] AURORA NOTIFY app=nexus-notify-test summary=svctest-notification' in log1)
    check('boot1: chrony queried the host NTP server', ntp.requests > 0, '%d requests' % ntp.requests)
    check('boot1: no user/kernel faults', not re.search(r'KERNEL_FAULT|USER_FAULT', log1), re.findall(r'.*(?:KERNEL|USER)_FAULT.*', log1)[:3])
    fs = pkg_vm.guest_ext4(disk, WORK / 'overlay.qcow2')
    journal = (pkg_vm.guest_read(fs, '/var/log/journal/system.journal') or b'').decode('utf-8', 'replace')
    rows = [l.split(' ', 7) for l in journal.splitlines() if l]
    check('journal: record format', rows and all(len(r) == 8 and r[0].isdigit() and r[1].isdigit() and len(r[2]) == 16 and r[3] in '01234567' for r in rows),
          rows[:1])
    status = (pkg_vm.guest_read(fs, '/var/lib/dpkg/status') or b'').decode('utf-8', 'replace')
    installed = status.count('Status: install ok installed')
    states = re.findall(r'^Status: (.*)$', status, re.M)
    check('dpkg database: every package installed', installed >= 300 and all(s == 'install ok installed' for s in states), '%d installed, other: %s' % (installed, sorted(set(s for s in states if s != 'install ok installed'))))
    fs.f.close()
    log2 = boot(disk, 'boot2', False)
    lines2 = svc_lines(log2)
    for name, (ok, detail) in sorted(lines2.items()): check('boot2 ' + name, ok, detail)
    check('boot2: guest checks ran', len(lines2) >= 7, '%d checks' % len(lines2))
    check('boot2: no first-boot installation again', 'Starting nexus-firstboot.service' not in log2)
finally:
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    if disk and args.keep_installed and results['passed'] and (WORK / 'overlay.qcow2').exists():
        args.keep_installed.parent.mkdir(parents=True, exist_ok=True); args.keep_installed.unlink(missing_ok=True)
        subprocess.run([pkg_vm.QEMU_IMG, 'convert', '-c', '-O', 'qcow2', str(WORK / 'overlay.qcow2'), str(args.keep_installed)], check=True)
        print('installed system saved as', args.keep_installed, flush=True)
    if disk and not args.keep_disk:
        (WORK / 'overlay.qcow2').unlink(missing_ok=True); disk.unlink(missing_ok=True)
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
