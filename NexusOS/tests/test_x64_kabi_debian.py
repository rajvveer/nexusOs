"""Debian programs on the new kernel interfaces (kabi, wave 2): System V IPC users and util-linux's libmount on the new
mount API, on an installed Debian base.

python -I tests/test_x64_kabi_debian.py WORK --base INSTALLED.qcow2 [--port 9107] [--qmp-port 4707]
       [--proxy-cache DIR] [--write-cache DIR] [--smp 2] [--no-postgres]
INSTALLED.qcow2: the installed Debian base that tests/test_x64_services.py --keep-installed leaves (pkg group).
Boots it on a fresh overlay (the base is never written), apt-installs dmsetup, dosfstools and postgresql through a
deb.debian.org-only caching proxy (reads --proxy-cache, the shared D:\\nexustest\\pkgs\\pkg-cache\\apt-proxy, read only;
new downloads go to --write-cache), then:
  * util-linux ipcmk / ipcs (-s/-q/-m/-l/-i, /proc/sysvipc) / ipcrm;
  * libdevmapper's udev synchronisation (dmsetup udevcreatecookie / udevcookies / udevcomplete / udevreleasecookie: a
    System V semaphore per cookie, released by semop; /run/udev/control stands in for a running udevd meanwhile);
  * PostgreSQL 17 (initdb, pg_ctl start, psql): its data-directory interlock is a System V shared memory segment
    (shmget/shmat/shmctl IPC_STAT nattch);
  * mount(8) through libmount (LIBMOUNT_DEBUG shows open_tree/move_mount/fsopen/fsmount/mount_setattr): a bind mount,
    remount,bind,ro (writes fail), umount; a vfat image on a loop device (fsopen("vfat") ... move_mount).
Fails on KERNEL_FAULT and on any expectation; lists every `ENOSYS syscall=`.
pkg-style: run it under C:\\nexustest\\wave2\\pkgvm_lock.py; the refreshed test hook fetches this run's --port
(nexus.testhook=PORT, pkg_vm.Vm(hook_port=...))."""
from pathlib import Path
import argparse, http.server, re, sys, threading, time, urllib.error, urllib.parse, urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('work'); parser.add_argument('--base', required=True)
parser.add_argument('--port', type=int, default=9107); parser.add_argument('--qmp-port', type=int, default=4707)
parser.add_argument('--proxy-cache', default=r'D:\nexustest\pkgs\pkg-cache\apt-proxy')
parser.add_argument('--write-cache', default=r'D:\nexustest\wave2\ws_kabi_cache\apt-proxy')
parser.add_argument('--smp', type=int, default=2); parser.add_argument('--memory', default='2048')
parser.add_argument('--no-postgres', action='store_true')
options = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
work, base = Path(options.work).resolve(), Path(options.base).resolve()
read_cache, write_cache = Path(options.proxy_cache), Path(options.write_cache)
sys.path.insert(0, str(ROOT / 'tests'))
import pkg_vm  # noqa: E402

work.mkdir(parents=True, exist_ok=True)
PACKAGES = 'dmsetup dosfstools' + ('' if options.no_postgres else ' postgresql')
GUEST = r'''PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH DEBIAN_FRONTEND=noninteractive
echo 'Acquire::http::Proxy "http://10.0.2.2:@PORT@/";' > /etc/apt/apt.conf.d/99probe-proxy
for f in /etc/apt/sources.list.d/google-chrome*; do [ -e "$f" ] && mv "$f" "/tmp/$(basename "$f").aside"; done
apt-get update >/tmp/u.out 2>&1; echo "PROBE apt-update rc=$?"
apt-get install -y --no-install-recommends @PACKAGES@ >/tmp/i.out 2>&1
echo "PROBE install rc=$? $(tail -c 200 /tmp/i.out | tr '\n' ' ')"
for f in /tmp/google-chrome*.aside; do [ -e "$f" ] && mv "$f" "/etc/apt/sources.list.d/$(basename "$f" .aside)"; done
# ---- util-linux ipcmk / ipcs / ipcrm
S=$(ipcmk -S 3 | awk '{print $NF}'); Q=$(ipcmk -Q | awk '{print $NF}'); M=$(ipcmk -M 65536 -p 0640 | awk '{print $NF}')
echo "PROBE ipcmk sem=$S queue=$Q shm=$M"
echo "PROBE ipcs-listed sem=$(ipcs -s | grep -cw "$S") queue=$(ipcs -q | grep -cw "$Q") shm=$(ipcs -m | grep -cw "$M")"
echo "PROBE ipcs-sem-detail $(ipcs -s -i "$S" | tr -s ' \n' ' ' | head -c 400)"
echo "PROBE ipcs-limits $(ipcs -l | tr -s ' \n' ' ' | head -c 3000)"
echo "PROBE proc-sysvipc sem=$(grep -cw "$S" /proc/sysvipc/sem) shm=$(grep -cw "$M" /proc/sysvipc/shm) msg=$(grep -cw "$Q" /proc/sysvipc/msg)"
ipcrm -s "$S" -q "$Q" -m "$M"; echo "PROBE ipcrm rc=$? left=$(ipcs | grep -c '^0x')"
# ---- libdevmapper udev synchronisation: one semaphore per cookie
mkdir -p /run/udev; touch /run/udev/control
C=$(dmsetup udevcreatecookie 2>/tmp/dm.err); echo "PROBE dm-cookie rc=$? cookie=$C err=$(head -c 200 /tmp/dm.err | tr '\n' ' ')"
SEMID=$(dmsetup udevcookies 2>/dev/null | awk 'NR==2 {print $2}')
# a pending uevent, as a DM ioctl with this cookie would leave it (semop +1 through perl's syscall())
perl -e '$b = pack("Sss", 0, 1, 0); syscall(65, $ARGV[0] + 0, $b, 1) == 0 or die "semop: $!"' "$SEMID"; echo "PROBE dm-uevent rc=$? semid=$SEMID"
echo "PROBE dm-cookies $(dmsetup udevcookies 2>&1 | tr -s ' \n' ' ' | head -c 400)"
(timeout 60 dmsetup udevreleasecookie "$C" >/tmp/rel.err 2>&1; echo "rc=$?" > /tmp/rel.out) &
sleep 2; echo "PROBE dm-waiting $([ -s /tmp/rel.out ] && echo done || echo waiting)"
dmsetup udevcomplete "$C"; echo "PROBE dm-complete rc=$?"
wait; echo "PROBE dm-release $(cat /tmp/rel.out) left=$(dmsetup udevcookies 2>/dev/null | grep -c '^0x')"
rm -f /run/udev/control
# ---- PostgreSQL: System V shared memory interlock
if [ -x /usr/lib/postgresql/17/bin/initdb ]; then
  rm -rf /tmp/pg; mkdir -p /tmp/pg; chown postgres /tmp/pg
  su postgres -s /bin/sh -c '/usr/lib/postgresql/17/bin/initdb -D /tmp/pg/data -A trust' >/tmp/pg.init 2>&1; echo "PROBE pg-initdb rc=$? $(tail -c 200 /tmp/pg.init | tr '\n' ' ')"
  su postgres -s /bin/sh -c '/usr/lib/postgresql/17/bin/pg_ctl -D /tmp/pg/data -l /tmp/pg/log -o "-k /tmp/pg -c listen_addresses=" -w -t 120 start' >/tmp/pg.start 2>&1; echo "PROBE pg-start rc=$? $(tail -c 200 /tmp/pg.start | tr '\n' ' ')"
  echo "PROBE pg-log $(grep -E 'FATAL|PANIC|ERROR|could not' /tmp/pg/log 2>/dev/null | head -c 600 | tr '\n' ' ')"
  echo "PROBE pg-sysv-shm $(ipcs -m | grep -c postgres)"
  su postgres -s /bin/sh -c "psql -h /tmp/pg -At -c 'select 6*7'" >/tmp/pg.q 2>&1; echo "PROBE pg-query rc=$? $(head -c 120 /tmp/pg.q | tr '\n' ' ')"
  su postgres -s /bin/sh -c '/usr/lib/postgresql/17/bin/pg_ctl -D /tmp/pg/data -m fast -w stop' >/tmp/pg.stop 2>&1; echo "PROBE pg-stop rc=$? left=$(ipcs -m | grep -c postgres) $(tail -c 300 /tmp/pg/log | tr '\n' ' ')"
fi
# ---- libmount on the new mount API
mkdir -p /tmp/ma /tmp/mb; echo bound > /tmp/ma/f
LIBMOUNT_DEBUG=all mount --bind /tmp/ma /tmp/mb 2>/tmp/m1.dbg; rc=$?
echo "PROBE mount-bind rc=$rc content=$(cat /tmp/mb/f 2>&1) api=$(grep -oE 'open_tree|move_mount|mount_setattr|fsopen|fsmount' /tmp/m1.dbg | sort -u | tr '\n' ',')"
echo "PROBE findmnt $(findmnt -n -o TARGET,SOURCE /tmp/mb 2>&1 | tr -s ' \n' ' ')"
LIBMOUNT_DEBUG=all mount -o remount,bind,ro /tmp/mb 2>/tmp/m2.dbg; rc=$?
touch /tmp/mb/g 2>/dev/null; w=$?
echo "PROBE mount-remount-ro rc=$rc write=$w api=$(grep -oE 'open_tree|move_mount|mount_setattr|fspick|fsconfig' /tmp/m2.dbg | sort -u | tr '\n' ',')"
umount /tmp/mb; echo "PROBE umount rc=$? still=$(grep -c ' /tmp/mb ' /proc/self/mountinfo)"
dd if=/dev/zero of=/tmp/v.img bs=1M count=64 2>/dev/null; mkfs.vfat -F 32 /tmp/v.img >/tmp/mkfs.out 2>&1; echo "PROBE mkfs rc=$?"
mkdir -p /tmp/mv; LIBMOUNT_FORCE_MOUNT2=always mount -o loop /tmp/v.img /tmp/mv 2>/tmp/m0.err; rc=$?
echo "PROBE mount2-vfat rc=$rc type=$(findmnt -n -o FSTYPE /tmp/mv) $(head -c 200 /tmp/m0.err | tr '\n' ' ')"; umount /tmp/mv 2>/dev/null
mkdir -p /tmp/mv; LIBMOUNT_DEBUG=all mount -o loop /tmp/v.img /tmp/mv 2>/tmp/m3.dbg; rc=$?
echo data > /tmp/mv/x 2>/dev/null; w=$?
echo "PROBE mount-vfat rc=$rc write=$w type=$(findmnt -n -o FSTYPE /tmp/mv) api=$(grep -oE 'fsopen|fsconfig|fsmount|move_mount' /tmp/m3.dbg | sort -u | tr '\n' ',') $(grep -iE 'error|failed' /tmp/m3.dbg | head -c 300 | tr '\n' ' ')"
umount /tmp/mv; echo "PROBE vfat-umount rc=$?"
echo "APT DONE"
touch /run/apttest.done
'''.replace('@PORT@', str(options.port)).replace('@PACKAGES@', PACKAGES)
HOOK = ('set -e\nmkdir -p /usr/lib/nexus/tests\nwget -q -O /usr/lib/nexus/tests/probe.sh http://10.0.2.2:%d/files/probe.sh\n'
        'sh /usr/lib/nexus/tests/probe.sh\n' % options.port).encode()
FILES = {'/hook.sh': HOOK, '/files/probe.sh': GUEST.encode()}
refused = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def reply(self, code, body=b'', kind='application/octet-stream'):
        self.send_response(code); self.send_header('content-type', kind); self.send_header('content-length', str(len(body))); self.end_headers()
        if self.command != 'HEAD': self.wfile.write(body)
    def do_HEAD(self): self.do_GET()
    def do_CONNECT(self): refused.append(self.path); self.reply(403)
    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        host = url.hostname if url.scheme == 'http' else None
        path = url.path if host is None or host.startswith('10.0.2.2') else None
        if path is not None:
            body = FILES.get(path); return self.reply(200, body, 'text/plain') if body is not None else self.reply(404)
        if host != 'deb.debian.org': refused.append(self.path); return self.reply(403)
        volatile = '/dists/' in url.path
        for root in (write_cache, read_cache):   # own downloads first, then the shared cache (never written)
            cache = root / host / url.path.lstrip('/')
            if cache.is_file() and (not volatile or time.time() - cache.stat().st_mtime < 6 * 3600): return self.reply(200, cache.read_bytes())
        try:
            with urllib.request.urlopen(urllib.request.Request('https://%s%s' % (host, url.path), headers={'User-Agent': 'NexusOS-apt-test-proxy'}), timeout=120) as r:
                body = r.read()
        except urllib.error.HTTPError as e:
            return self.reply(e.code)
        except OSError:
            return self.reply(502)
        cache = write_cache / host / url.path.lstrip('/'); cache.parent.mkdir(parents=True, exist_ok=True); cache.write_bytes(body)
        return self.reply(200, body)


servers = [http.server.ThreadingHTTPServer(('127.0.0.1', p), Handler) for p in (options.port,)]   # the hook's port: Vm(hook_port=)
for s in servers: s.daemon_threads = True; threading.Thread(target=s.serve_forever, daemon=True).start()
failures = []
try:
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, work / 'stage.qcow2', pkg_vm.refresh_services), flush=True)
    vm = pkg_vm.Vm(work, work / 'stage.qcow2', options.qmp_port, tag='kabideb', accel='auto', smp=options.smp, memory=options.memory,
                    hook_port=options.port)
    try:
        vm.wait(r'APT DONE', seconds=3600, fail=r'KERNEL_FAULT|\[64\] FAIL')
        vm.exited(240); log = vm.text()
    finally:
        vm.close()
    probes = dict(re.findall(r'^PROBE (\S+) (.*)$', log, re.M))
    for name, line in probes.items(): print('PROBE', name, line[:900], flush=True)
    def want(name, pattern, what):
        if not re.search(pattern, probes.get(name, '')): failures.append('%s: %s (%s)' % (name, probes.get(name, 'missing')[:300], what))
    want('install', r'^rc=0', 'apt-get install ' + PACKAGES)
    want('ipcmk', r'^sem=\d+ queue=\d+ shm=\d+$', 'ipcmk -S/-Q/-M print the ids')
    want('ipcs-listed', r'^sem=1 queue=1 shm=1$', 'ipcs lists each object once')
    want('ipcs-sem-detail', r'nsems = 3', 'ipcs -s -i: the set and its 3 semaphores')
    for limit in ('max number of arrays = 32000', 'max semaphores per array = 32000', 'max queues system wide = 32000',
                  'max number of segments = 4096', 'max ops per semop call = 500'):
        want('ipcs-limits', re.escape(limit), 'ipcs -l: ' + limit)
    want('proc-sysvipc', r'^sem=1 shm=1 msg=1$', '/proc/sysvipc rows')
    want('ipcrm', r'^rc=0 left=0$', 'ipcrm removes them')
    want('dm-cookie', r'^rc=0 cookie=0x[0-9A-Fa-f]+', 'dmsetup udevcreatecookie: a semaphore-backed cookie (udev sync on)')
    want('dm-uevent', r'^rc=0 semid=\d+', 'semop +1 on the cookie semaphore')
    want('dm-cookies', r'0x[0-9A-Fa-f]+ +\d+ +2 ', 'dmsetup udevcookies: the cookie, its semid, value 2')
    want('dm-waiting', r'^waiting$', 'udevreleasecookie waits in semop')
    want('dm-complete', r'^rc=0$', 'dmsetup udevcomplete')
    want('dm-release', r'^rc=0 left=0$', 'udevreleasecookie returns once the semaphore is 0, the set removed')
    if not options.no_postgres:
        want('pg-initdb', r'^rc=0', 'initdb')
        want('pg-start', r'^rc=0', 'pg_ctl start')
        want('pg-sysv-shm', r'^[1-9]', 'the postmaster\'s System V segment')
        want('pg-query', r'^rc=0 42', 'psql select 6*7')
        want('pg-stop', r'^rc=0 left=0', 'pg_ctl stop removes the segment')
    want('mount-bind', r'^rc=0 content=bound api=(?=.*open_tree)(?=.*move_mount)', 'mount --bind through open_tree/move_mount')
    want('findmnt', r'/tmp/mb', 'findmnt sees the bind mount')
    want('mount-remount-ro', r'^rc=0 write=[1-9]', 'remount,bind,ro: writes fail')
    want('umount', r'^rc=0 still=0$', 'umount')
    want('mkfs', r'^rc=0$', 'mkfs.vfat')
    if probes.get('mount2-vfat', '').startswith('rc=0 type=vfat'):   # the same mount works through mount(2): the API must too
        want('mount-vfat', r'^rc=0 write=0 type=vfat api=(?=.*fsopen)(?=.*fsconfig)(?=.*fsmount)(?=.*move_mount)', 'mount -o loop vfat through fsopen/fsconfig/fsmount/move_mount')
        want('vfat-umount', r'^rc=0$', 'umount vfat')
    else: print('NOTE loop vfat fails through mount(2) as well: ' + probes.get('mount2-vfat', 'missing')[:200], flush=True)
    enosys = sorted(set(re.findall(r'ENOSYS syscall=(\S+)', log)))
    print('ENOSYS:', ' '.join(str(int(e, 16)) if e.startswith('0x') else e for e in enosys) or 'none', flush=True)
    faults = re.findall(r'.*USER_FAULT.*\n(?:.*(?:FAULT_IMAGE|CALLER_IMAGE).*\n)*', log)
    for f in faults[:8]: print('FAULT:', f.strip()[:800], flush=True)
    if 'KERNEL_FAULT' in log: failures.append('KERNEL_FAULT')
    print('refused:', refused[:5], flush=True)
finally:
    for s in servers: s.shutdown()
    for name in ('overlay.qcow2', 'stage.qcow2'): (work / name).unlink(missing_ok=True)
for f in failures: print('FAIL', f, flush=True)
print('RESULT', 'PASS' if not failures else 'FAIL', flush=True)
sys.exit(1 if failures else 0)
