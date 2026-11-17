"""nexusd socket, timer and path units, per-user managers and journal rate limiting on an installed ext4 NexusOS root.

    python -I tests/test_x64_units.py WORK --base BASE [--accel auto|whpx|tcg] [--smp 2] [--qmp-port 4771]

BASE: an installed system (tests/test_x64_services.py --keep-installed X.qcow2). WORK/stage.qcow2 over it gets the
tree's services (sysinstall.py services --aurora --tests), the test units below in /etc/nexus/services (system) and
/etc/nexus/user (every user's manager), and a journald.conf with a small rate limit (2000 records per 10 s). The test
hook (tests/units_guest.sh, served on this run's port) checks, as root after every boot unit:
  sockets  Accept=no: lazy start on the first connection with LISTEN_FDS/LISTEN_PID/LISTEN_FDNAMES (unix stream +
           datagram, TCP), no polling while the service runs, re-activation after it stops, the fds also for a service
           started directly; Accept=yes: one instance per connection (inetd stdin/stdout, fd 3), instances removed,
           MaxConnections; hostile: a path that is not a socket (left untouched), address in use, invalid address,
           a service that never accepts (trigger limit), Accept=yes without a template
  timers   OnBootSec, OnActiveSec + OnUnitActiveSec (interval), OnCalendar (*:*:0/5, the trigger second), an elapse while
           the unit still runs is skipped, list-timers, stop, an invalid calendar, a missing unit, Persistent= catch-up
           of a missed elapse (stamp 3 days old) vs a non-persistent twin
  paths    PathExists, MakeDirectory, PathChanged, PathModified vs PathChanged (write without close), DirectoryNotEmpty,
           PathExistsGlob, the trigger limit of a condition that stays true
  users    user@1000.service (nexusd --user) with the session, its units at login (/etc/nexus/user), on-demand start,
           its own journal, timers and sockets as the user, ~/.config/nexus/user + daemon-reload, enable, another user
           and nobody refused, session-end / session
  journal  rate limit per source: 2000 of 30000 records kept, the "Suppressed N messages" record, another source
           unaffected
  parity   sockets: a plain PORT is dual stack (IPv4 and IPv6 clients, IPv4-mapped peer), [::]:PORT with
           BindIPv6Only=ipv6-only refuses IPv4, BindIPv6Only=both takes IPv4 datagrams; ListenSequentialPacket;
           ListenFIFO (made with SocketMode/DirectoryMode, fd 3 read by the service, a second write while it runs,
           RemoveOnStop); ListenNetlink=route 16 (an address added: RTM_NEWADDR); refused: Accept=yes on a FIFO, an
           unknown netlink family; the kernel's named pipes (ENXIO, blocking open, O_RDWR, on tmpfs and ext4).
           Debian links: the system's timers.target.wants timers (apt-daily, apt-daily-upgrade, logrotate,
           dpkg-db-backup, fstrim) are active, and with stamps 2 days old (and RandomizedDelaySec=0 drop-ins) apt-daily,
           apt-daily-upgrade, logrotate and dpkg-db-backup catch up at boot and succeed; a vendor
           multi-user.target.wants link starts its service, a /dev/null mask wins. User managers: /etc/systemd/user
           default.target.wants and /usr/lib/systemd/user sockets.target.wants links, NAME.service.wants/ links, masks,
           a Debian dbus.socket is never loaded (the session bus is dbus-session@UID), and gnupg's real gpg-agent.socket
           (deb-systemd-helper --user link) socket-activates gpg-agent for gpg-connect-agent.
Boot 2 (same overlay): sockets again, the persistent timer does not repeat, the user's enabled unit starts at login,
the Debian timers do not run again.
Both boots: clean power-off, no kernel or user fault. Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import json
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'tests'
sys.path.insert(0, str(HERE))
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--smp', type=int, default=2); ap.add_argument('--qmp-port', type=int, default=4771)
ap.add_argument('--timeout', type=int, default=1500)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'units', 'smp': args.smp, 'checks': [], 'passed': False}
T = '/usr/lib/nexus/tests/nexus-svc-test'
INSTALL = '\n[Install]\nWantedBy=%s\nDefaultEnabled=yes\n'
SYSTEM = {
    # sockets
    't-sock.socket': '[Unit]\nDescription=socket activation (Accept=no)\n[Socket]\nListenStream=/run/t-sock.sock\nSocketMode=0666\n' + INSTALL % 'sockets',
    't-sock.service': '[Unit]\nDescription=socket-activated service\nRequires=t-sock.socket\n[Service]\nUser=nobody\nExecStart=%s listen\n' % T,
    't-multi.socket': '[Socket]\nListenStream=/run/t-multi.sock\nListenDatagram=/run/t-multi.dgram\nFileDescriptorName=multi\nService=t-multi-svc.service\n' + INSTALL % 'sockets',
    't-multi-svc.service': '[Service]\nExecStart=%s listen\n' % T,
    't-tcp.socket': '[Socket]\nListenStream=127.0.0.1:9123\n' + INSTALL % 'sockets',
    't-tcp.service': '[Service]\nExecStart=%s listen\n' % T,
    't-echo.socket': '[Socket]\nListenStream=/run/t-echo.sock\nAccept=yes\nMaxConnections=3\n' + INSTALL % 'sockets',
    't-echo@.service': '[Unit]\nDescription=inetd-style instance %%i\n[Service]\nUser=nobody\nStandardInput=socket\nExecStart=%s inetd\n' % T,
    't-conn.socket': '[Socket]\nListenStream=9124\nAccept=yes\n' + INSTALL % 'sockets',
    't-conn@.service': '[Service]\nExecStart=%s conn\n' % T,
    't-badsock.socket': '[Socket]\nListenStream=/etc/hostname\nService=t-tcp.service\n',
    't-busy.socket': '[Socket]\nListenStream=127.0.0.1:9123\nService=t-tcp.service\n',
    't-badaddr.socket': '[Socket]\nListenStream=99999\nService=t-tcp.service\n',
    't-quit.socket': '[Socket]\nListenStream=/run/t-quit.sock\nTriggerLimitBurst=4\nTriggerLimitIntervalSec=10s\n',
    't-quit.service': '[Service]\nType=oneshot\nExecStart=/bin/true\n',
    't-notpl.socket': '[Socket]\nListenStream=/run/t-notpl.sock\nAccept=yes\n',
    # timers
    't-boot.timer': '[Timer]\nOnBootSec=1s\n' + INSTALL % 'timers',
    't-boot.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "date +%%s >> /run/t-boot.ran"\n',
    't-rep.timer': '[Timer]\nOnActiveSec=1s\nOnUnitActiveSec=2s\n',
    't-rep.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "date +%%s%%N >> /run/t-rep.ran"\n',
    't-cal.timer': '[Timer]\nOnCalendar=*:*:0/5\n',
    't-cal.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "date +%%S >> /run/t-cal.ran"\n',
    't-slow.timer': '[Timer]\nOnActiveSec=1s\nOnUnitActiveSec=1s\n',
    't-slow.service': '[Service]\nExecStart=/bin/sh -c "echo started >> /run/t-slow.ran; exec sleep 100000"\n',
    't-pers.timer': '[Timer]\nOnCalendar=daily\nPersistent=yes\n' + INSTALL % 'timers',
    't-pers.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "date >> /var/lib/t-pers.ran"\n',
    't-npers.timer': '[Timer]\nOnCalendar=daily\n' + INSTALL % 'timers',
    't-npers.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "date >> /var/lib/t-npers.ran"\n',
    't-badcal.timer': '[Timer]\nOnCalendar=*-*-* 25:00:00\nUnit=t-boot.service\n',
    't-nounit.timer': '[Timer]\nOnActiveSec=1s\nUnit=t-missing.service\n',
    # paths
    't-pe.path': '[Path]\nPathExists=/run/t-flag\n' + INSTALL % 'paths',
    't-pe.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo ran >> /run/t-pe.ran; rm -f /run/t-flag"\n',
    't-pc.path': '[Path]\nPathChanged=/tmp/t-watch/file\nMakeDirectory=yes\n' + INSTALL % 'paths',
    't-pc.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo ran >> /run/t-pc.ran"\n',
    't-pc2.path': '[Path]\nPathChanged=/tmp/t-watch/mod\n' + INSTALL % 'paths',
    't-pc2.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo ran >> /run/t-pc2.ran"\n',
    't-pm.path': '[Path]\nPathModified=/tmp/t-watch/mod\n' + INSTALL % 'paths',
    't-pm.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo ran >> /run/t-pm.ran"\n',
    't-pdir.path': '[Path]\nDirectoryNotEmpty=/run/t-spool\nMakeDirectory=yes\n' + INSTALL % 'paths',
    't-pdir.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "ls /run/t-spool >> /run/t-pdir.ran; rm -f /run/t-spool/*"\n',
    't-pglob.path': '[Path]\nPathExistsGlob=/run/t-glob/*.job\n' + INSTALL % 'paths',
    't-pglob.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo ran >> /run/t-pglob.ran; rm -f /run/t-glob/*.job"\n',
    't-ploop.path': '[Path]\nPathExists=/run/t-loop\nTriggerLimitBurst=4\nTriggerLimitIntervalSec=20s\n' + INSTALL % 'paths',
    't-ploop.service': '[Service]\nType=oneshot\nExecStart=/bin/true\n',
    # journal
    't-spam.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "yes spam-line-padding | head -n 30000"\n',
    # parity: IPv6/dual stack, FIFO, netlink, sequential packets
    't-dual.socket': '[Socket]\nListenStream=9125\n' + INSTALL % 'sockets',
    't-dual.service': '[Service]\nExecStart=%s listen\n' % T,
    't-v6only.socket': '[Socket]\nListenStream=[::]:9126\nBindIPv6Only=ipv6-only\n' + INSTALL % 'sockets',
    't-v6only.service': '[Service]\nExecStart=%s listen\n' % T,
    't-v6both.socket': '[Socket]\nListenDatagram=[::]:9127\nBindIPv6Only=both\n' + INSTALL % 'sockets',
    't-v6both.service': '[Service]\nExecStart=%s listen\n' % T,
    't-seqp.socket': '[Socket]\nListenSequentialPacket=/run/t-seqp.sock\nSocketMode=0666\n' + INSTALL % 'sockets',
    't-seqp.service': '[Service]\nExecStart=%s listen\n' % T,
    't-fifo.socket': '[Socket]\nListenFIFO=/run/t-fifo/pipe\nSocketMode=0620\nDirectoryMode=0750\nRemoveOnStop=yes\n' + INSTALL % 'sockets',
    't-fifo.service': '[Service]\nExecStart=%s listen\n' % T,
    't-nl.socket': '[Socket]\nListenNetlink=route 16\n' + INSTALL % 'sockets',
    't-nl.service': '[Service]\nExecStart=%s listen\n' % T,
    't-fifoacc.socket': '[Socket]\nListenFIFO=/run/t-fifoacc\nAccept=yes\n',
    't-fifoacc@.service': '[Service]\nExecStart=/bin/true\n',
    't-badnl.socket': '[Socket]\nListenNetlink=nosuchfamily 1\nService=t-nl.service\n',
}
# Debian-style units in the systemd directories, enabled by links as deb-systemd-helper leaves them
DEBIAN_SYSTEM = {
    '/usr/lib/systemd/system/t-vendorlink.service': '[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/bin/sh -c "echo ran > /run/t-vendorlink.ran"\n[Install]\nWantedBy=multi-user.target\n',
    '/usr/lib/systemd/system/t-masked.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo ran > /run/t-masked.ran"\n[Install]\nWantedBy=multi-user.target\n',
}
DEBIAN_USER = {
    '/usr/lib/systemd/user/u-deb.service': '[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/bin/sh -c "echo deb > %h/u-deb.ran"\n[Install]\nWantedBy=default.target\n',
    '/usr/lib/systemd/user/u-debdep.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo dep > %h/u-debdep.ran"\n',
    '/usr/lib/systemd/user/u-debsock.socket': '[Socket]\nListenStream=%t/u-debsock.sock\n[Install]\nWantedBy=sockets.target\n',
    '/usr/lib/systemd/user/u-debsock.service': '[Service]\nExecStart=%s listen\n' % T,
    '/usr/lib/systemd/user/u-masked.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo ran > %h/u-masked.ran"\n[Install]\nWantedBy=default.target\n',
    '/usr/lib/systemd/user/dbus.socket': '[Socket]\nListenStream=%t/bus\n[Install]\nWantedBy=sockets.target\n',
}
LINKS = {   # link -> target
    '/usr/lib/systemd/system/multi-user.target.wants/t-vendorlink.service': '../t-vendorlink.service',
    '/etc/systemd/system/multi-user.target.wants/t-masked.service': '/usr/lib/systemd/system/t-masked.service',
    '/etc/systemd/system/t-masked.service': '/dev/null',
    '/etc/systemd/user/default.target.wants/u-deb.service': '/usr/lib/systemd/user/u-deb.service',
    '/etc/systemd/user/u-deb.service.wants/u-debdep.service': '/usr/lib/systemd/user/u-debdep.service',
    '/usr/lib/systemd/user/sockets.target.wants/u-debsock.socket': '../u-debsock.socket',
    '/etc/systemd/user/default.target.wants/u-masked.service': '/usr/lib/systemd/user/u-masked.service',
    '/etc/systemd/user/u-masked.service': '/dev/null',
    '/usr/lib/systemd/user/sockets.target.wants/dbus.socket': '../dbus.socket',
}
DEBIAN_TIMERS = ('apt-daily.timer', 'apt-daily-upgrade.timer', 'logrotate.timer', 'dpkg-db-backup.timer')   # catch up at boot 1
USER = {
    'u-hello.service': '[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=/bin/sh -c "id -u > %h/u-hello.ran"\n'
                       'ExecStop=/bin/sh -c "echo stopped >> %h/u-hello.stop"\n' + INSTALL % 'default',
    'u-ondemand.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "echo on-demand-user-output; id -un"\n[Install]\nWantedBy=default\n',
    'u-tick.timer': '[Timer]\nOnActiveSec=1s\n' + INSTALL % 'timers',
    'u-tick.service': '[Service]\nType=oneshot\nExecStart=/bin/sh -c "date >> %h/u-tick.ran"\n',
    'u-sock.socket': '[Socket]\nListenStream=%t/u-sock.sock\n' + INSTALL % 'sockets',
    'u-sock.service': '[Service]\nExecStart=%s listen\n' % T,
}
JOURNALD = ('[Journal]\nSystemMaxFileSize=4M\nSystemMaxFiles=8\nForwardToConsole=no\nMaxLevelConsole=3\n'
            'RateLimitIntervalSec=10s\nRateLimitBurst=2000\n')


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def stage(fs):
    pkg_vm.refresh_services(fs)   # the tree's nexusd/nexusctl/svc-test, vendor units (user@.service), test-hook
    for name, text in SYSTEM.items(): pkg_vm.put(fs, '/etc/nexus/services/' + name, text)
    for name, text in USER.items(): pkg_vm.put(fs, '/etc/nexus/user/' + name, text)
    for path, text in {**DEBIAN_SYSTEM, **DEBIAN_USER}.items(): pkg_vm.put(fs, path, text)
    for link, target in LINKS.items():
        try:
            if fs.lookup(link): fs.remove(link)
        except (KeyError, ValueError, FileNotFoundError): pass
        fs.symlink(target, link)
    old = int(time.time()) - 2 * 86400   # the Debian timers last ran 2 days ago: Persistent= catches up at boot
    for timer in DEBIAN_TIMERS:
        pkg_vm.put(fs, '/var/lib/nexus/timers/stamp-' + timer, '%d\n' % old)
        pkg_vm.put(fs, '/etc/nexus/services/%s.d/test-no-delay.conf' % timer, '[Timer]\nRandomizedDelaySec=0\n')   # fire at once
    pkg_vm.put(fs, '/etc/nexus/journald.conf', JOURNALD)
    for stale in ('/var/lib/units-test.boot', '/var/lib/t-pers.ran', '/var/lib/t-npers.ran'):
        try:
            if fs.lookup(stale): fs.remove(stale)
        except (KeyError, ValueError, FileNotFoundError): pass


def boot(server, tag, first):
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag=tag, accel=args.accel, smp=args.smp, memory='2048',
                   fresh_overlay=first, hook_port=server.port)
    try:
        vm.wait(r'UNITS DONE boot=', seconds=args.timeout, fail=r'KERNEL_FAULT|\[64\] FAIL')
        code = vm.exited(240)
        log = vm.text()
    finally:
        vm.close(keep_overlay=True)
    lines = re.findall(r'^UNT (\S+) (PASS|FAIL) ?(.*)$', log, re.M)
    for name, verdict, detail in lines: check('%s %s' % (tag, name), verdict == 'PASS', detail)
    check('%s: clean power-off' % tag, code is not None and 'INIT SESSION RETURNED' in log and 'nexusd] service manager stopped' in log, 'exit=%s' % code)
    check('%s: no kernel or user fault' % tag, not re.search(r'KERNEL_FAULT|USER_FAULT', log), re.findall(r'.*(?:KERNEL|USER)_FAULT.*', log)[:3])
    return lines


server = None
started = time.time()
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage), flush=True)
    hook = (HERE / 'units_guest.sh').read_bytes().replace(b'\r\n', b'\n')
    server = pkg_vm.HookServer(hook, {}, WORK / 'uploads')
    lines1 = boot(server, 'boot1', True)
    check('boot1: every guest check ran', len(lines1) >= 80, '%d checks' % len(lines1))
    lines2 = boot(server, 'boot2', False)
    check('boot2: every guest check ran', len(lines2) >= 6, '%d checks' % len(lines2))
finally:
    if server: server.close()
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    for name in ('overlay.qcow2', 'stage.qcow2'): (WORK / name).unlink(missing_ok=True)
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
