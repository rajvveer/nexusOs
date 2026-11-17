"""dpkg + apt on an ext4 NexusOS root against the real Debian 13 archive (apt-secure) and a signed test repository.

    python -I tests/test_x64_apt.py WORK --base BASE [--accel auto|whpx|tcg] [--port 0] [--qmp-port 4673]
           [--proxy-cache DIR]

BASE is an installed system: the overlay a finished tests/test_x64_services.py run kept (--keep-disk:
WORK/overlay.qcow2 over services.img), or a raw ext4 disk whose first boot is done. Nothing below BASE is written: the
boot runs on its own qcow2 overlay in WORK.

Host side: one HTTP server on --port (0 = a free one; guest 10.0.2.2; the guest's test-hook learns it from the kernel
command line, nexus.testhook=PORT) that serves the hook and is apt's proxy (Acquire::http::Proxy) for deb.debian.org
(cached in --proxy-cache; nothing is altered, apt verifies Debian's InRelease signatures itself) and serves a test
repository signed by a per-run OpenPGP key (tests/debrepo.py): nexus-hello 1.0/2.0 with maintainer scripts (log of
every call), a conffile, an update-alternatives link, a system user made by adduser, and a daemon (init script + systemd
unit) that nexusd starts through policy-rc.d. Guest side (tests/apt_guest.sh): update, install (test + Debian packages),
run, upgrade (maintainer script order, conffile kept, daemon restarted), remove, purge, autoremove; a repository signed by
an unknown key, an unsigned one and one with a tampered .deb must all be refused. Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import hashlib
import http.server
import json
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'tests'
sys.path.insert(0, str(HERE))
import pkg_vm  # noqa: E402
import debrepo  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--port', type=int, default=0); ap.add_argument('--qmp-port', type=int, default=4673)
ap.add_argument('--proxy-cache', type=Path, default=nexus_paths.cache('apt-proxy'))
ap.add_argument('--timeout', type=int, default=3600)
ap.add_argument('--smp', type=int, default=2)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'apt', 'checks': [], 'passed': False}


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


# ------------------------------------------------------------------ the test package
def hello(version):
    log = 'echo "%s $*" >> /var/log/nexus-hello-maint.log\n'
    program = ('#!/bin/sh\nif [ "$1" = --serve ]; then\n  echo "nexus-hello %s serving"\n  while :; do sleep 60; done\nfi\necho "nexus-hello %s"\n' % (version, version)).encode()
    init = ('#!/bin/sh\n### BEGIN INIT INFO\n# Provides:          nexus-hello\n# Required-Start:    $remote_fs\n# Required-Stop:     $remote_fs\n'
            '# Default-Start:     2 3 4 5\n# Default-Stop:      0 1 6\n# Short-Description: NexusOS test daemon\n### END INIT INFO\n'
            'case "$1" in start) /usr/bin/nexus-hello --serve & ;; stop) pkill -f "nexus-hello --serve" ;; restart|force-reload) $0 stop; $0 start ;; esac\nexit 0\n').encode()
    unit = b'[Unit]\nDescription=NexusOS test daemon\n[Service]\nExecStart=/usr/bin/nexus-hello --serve\nRestart=on-failure\n[Install]\nWantedBy=multi-user.target\n'
    files = {'/usr/bin/nexus-hello': (program, 0o755), '/etc/nexus-hello.conf': (('# nexus-hello configuration\nversion=%s\n' % version[0]).encode(), 0o644),
             '/etc/init.d/nexus-hello': (init, 0o755), '/usr/lib/systemd/system/nexus-hello.service': (unit, 0o644),
             '/usr/share/doc/nexus-hello/copyright': (b'Public domain test package.\n', 0o644)}
    scripts = {
        'preinst': '#!/bin/sh\nset -e\n' + log % 'preinst',
        'postinst': '#!/bin/sh\nset -e\n' + log % 'postinst' +
                    'if [ "$1" = configure ]; then\n'
                    '  adduser --system --group --no-create-home --home /nonexistent --quiet nexushello\n'
                    '  update-alternatives --install /usr/bin/nexus-greet nexus-greet /usr/bin/nexus-hello 50\n'
                    '  if deb-systemd-helper --quiet was-enabled nexus-hello.service; then deb-systemd-helper enable nexus-hello.service >/dev/null || true;'
                    ' else deb-systemd-helper update-state nexus-hello.service >/dev/null || true; fi\n'
                    '  update-rc.d nexus-hello defaults >/dev/null\n'
                    '  if [ -n "$2" ]; then invoke-rc.d nexus-hello restart || true; else invoke-rc.d nexus-hello start || true; fi\n'
                    'fi\n',
        'prerm': '#!/bin/sh\nset -e\n' + log % 'prerm' +
                 'if [ "$1" = remove ]; then invoke-rc.d nexus-hello stop || true; update-alternatives --remove nexus-greet /usr/bin/nexus-hello; fi\n',
        'postrm': '#!/bin/sh\nset -e\n' + log % 'postrm' +
                  'if [ "$1" = purge ]; then update-rc.d nexus-hello remove >/dev/null; deb-systemd-helper purge nexus-hello.service >/dev/null || true;'
                  ' deluser --quiet --system nexushello >/dev/null || true; fi\n'}
    return debrepo.build_deb('nexus-hello', version, files, scripts, depends='adduser', conffiles=['/etc/nexus-hello.conf'],
                             description='NexusOS apt test daemon %s' % version)


repo = debrepo.Repo(WORK / 'repo')
VARIANTS = {'v1': ([hello('1.0')], 'good'), 'v2': ([hello('2.0')], 'good'), 'badsig': ([hello('3.0')], 'badsig'),
            'unsigned': ([hello('3.0')], 'unsigned'), 'badhash': ([hello('3.0')], 'badhash')}
state = {'variant': None, 'proxied': 0, 'cached': 0, 'refused': []}
ALLOWED = ('deb.debian.org',)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def reply(self, code, body=b'', kind='application/octet-stream'):
        self.send_response(code); self.send_header('content-type', kind); self.send_header('content-length', str(len(body))); self.end_headers()
        if self.command != 'HEAD': self.wfile.write(body)
    def do_HEAD(self): self.do_GET()
    def do_CONNECT(self): state['refused'].append('CONNECT ' + self.path); self.reply(403)   # no tunnels (https) through this proxy
    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        host = url.hostname if url.scheme == 'http' else None
        path = url.path if host is None or host.startswith('10.0.2.2') else None
        if path is not None and path.startswith('/control/repo'):
            variant = urllib.parse.parse_qs(url.query).get('variant', [''])[0]
            if variant not in VARIANTS: return self.reply(400)
            debs, kind = VARIANTS[variant]; repo.publish(debs, kind); state['variant'] = variant
            print('-- repo', variant, flush=True); return self.reply(200, b'ok\n', 'text/plain')
        if path is not None and (path == '/hook.sh' or path.startswith('/files/')):
            body = HOOK_FILES.get(path)
            return self.reply(200, body, 'text/plain') if body is not None else self.reply(404)
        if path is not None and path.startswith('/nexus-test/'):
            target = (repo.root / path[len('/nexus-test/'):]).resolve()
            if repo.root.resolve() not in target.parents or not target.is_file(): return self.reply(404)
            return self.reply(200, target.read_bytes())
        if host not in ALLOWED: state['refused'].append(self.path); return self.reply(403)
        cache = args.proxy_cache / host / url.path.lstrip('/')
        volatile = '/dists/' in url.path   # indices change; packages in pool/ never do
        if cache.is_file() and (not volatile or time.time() - cache.stat().st_mtime < 6 * 3600):
            state['cached'] += 1; return self.reply(200, cache.read_bytes())
        try:
            with urllib.request.urlopen(urllib.request.Request('https://%s%s' % (host, url.path), headers={'User-Agent': 'NexusOS-apt-test-proxy'}), timeout=120) as r:
                body = r.read()
        except urllib.error.HTTPError as e:
            return self.reply(e.code)
        except OSError:
            return self.reply(502)
        state['proxied'] += 1
        cache.parent.mkdir(parents=True, exist_ok=True); cache.write_bytes(body)
        return self.reply(200, body)


server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler); server.daemon_threads = True
args.port = server.server_address[1]   # this run's port (also the hook's: Vm(hook_port=...))
threading.Thread(target=server.serve_forever, daemon=True).start()
# The base image's test-hook.service (sysinstall.py --tests) fetches http://10.0.2.2:PORT/hook.sh after every boot unit.
HOOK = '''set -e
H=http://10.0.2.2:%d/files
mkdir -p /etc/apt/keyrings /usr/lib/nexus/tests
wget -q -O /usr/lib/nexus/tests/apt_guest.sh $H/apt_guest.sh
wget -q -O /etc/apt/apt.conf.d/99nexus-test-proxy $H/proxy.conf
wget -q -O /etc/apt/keyrings/nexus-test.gpg $H/key.gpg
wget -q -O /etc/apt/sources.list.d/nexus-test.sources $H/nexus-test.sources
sh /usr/lib/nexus/tests/apt_guest.sh
''' % args.port
HOOK_FILES = {
    '/hook.sh': HOOK.encode(),
    '/files/apt_guest.sh': (HERE / 'apt_guest.sh').read_text().replace('@PORT@', str(args.port)).encode(),
    '/files/proxy.conf': ('Acquire::http::Proxy "http://10.0.2.2:%d/";\n' % args.port).encode(),
    '/files/key.gpg': repo.key.public_packets(),
    '/files/nexus-test.sources': ('Types: deb\nURIs: http://10.0.2.2:%d/nexus-test\nSuites: nexus-test\nComponents: main\n'
                                  'Signed-By: /etc/apt/keyrings/nexus-test.gpg\n' % args.port).encode(),
}
started = time.time()
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    # the worktree's services (policy-rc.d, nexusd, ...) over the installed base, in an overlay of this run
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', pkg_vm.refresh_services), flush=True)
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='apt', accel=args.accel, smp=args.smp, memory='2048', hook_port=args.port)
    try:
        vm.wait(r'APT DONE', seconds=args.timeout, fail=r'KERNEL_FAULT|\[64\] FAIL')
        code = vm.exited(240)
        log = vm.text()
    finally:
        vm.close()
    for m in re.finditer(r'^APT (\S+) (PASS|FAIL) ?(.*)$', log, re.M): check(m.group(1), m.group(2) == 'PASS', m.group(3))
    check('guest checks ran', len(re.findall(r'^APT \S+ (?:PASS|FAIL)', log, re.M)) >= 30)
    check('clean power-off', code is not None and 'INIT SESSION RETURNED' in log, code)
    check('no user/kernel faults', not re.search(r'KERNEL_FAULT|USER_FAULT', log), re.findall(r'.*(?:KERNEL|USER)_FAULT.*', log)[:3])
    check('apt reached the archive only through deb.debian.org', not state['refused'], state['refused'][:5])
    print('proxy: %d fetched, %d from cache' % (state['proxied'], state['cached']), flush=True)
finally:
    server.shutdown()
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    (WORK / 'overlay.qcow2').unlink(missing_ok=True); (WORK / 'stage.qcow2').unlink(missing_ok=True)
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
