"""The Software app (nexus-software) on an installed NexusOS, driven through the desktop like a user.

    python -I tests/test_x64_software.py WORK --base BASE [--accel auto|whpx|tcg] [--qmp-port 4677] [--proxy-cache DIR]

BASE: an installed system (the overlay a finished tests/test_x64_services.py --keep-disk run kept). WORK/stage.qcow2 over
it gets the worktree's services and an Aurora desktop session. The host proxies deb.debian.org (cached) and serves a
signed test repository with nexus-greeting 1.0 (installed by the guest hook before the session is driven), later 2.0.

Through the GUI (QMP pointer + keyboard, Aurora's window lines, the app's "[64] SOFTWARE" lines, screenshots):
  Spotlight starts Software; Explore: search "cowsay", select it, Install, a wrong password is refused (nothing
  installed), the right one installs it with progress; Installed: filter "cowsay", Remove; Updates: Check for updates
  (apt-get update through the helper) lists nexus-greeting 1.0 -> 2.0, Update all upgrades it; Close.
The guest then checks the results with dpkg and the journal (sudo log, wrong password attempt). Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import json
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import pkg_vm  # noqa: E402
import debrepo  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4677)
ap.add_argument('--proxy-cache', type=Path, default=ROOT / 'arch/x86_64/out/apt-proxy')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'software', 'checks': [], 'passed': False}
PASSWORD = 'nexus-test-pw'   # the installer password of the services test base (user nexus, group sudo)


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def greeting(version):
    program = ('#!/bin/sh\necho "nexus-greeting %s"\n' % version).encode()
    return debrepo.build_deb('nexus-greeting', version, {'/usr/bin/nexus-greeting': (program, 0o755),
                             '/usr/share/doc/nexus-greeting/copyright': (b'Public domain test package.\n', 0o644)},
                             description='NexusOS Software app test package %s' % version)


repo = debrepo.Repo(WORK / 'repo')


def repo_files():
    return {'repo/' + p.relative_to(repo.root).as_posix(): p.read_bytes() for p in repo.root.rglob('*') if p.is_file()}


HOOK = '''PATH=/usr/sbin:/usr/bin:/sbin:/bin; export DEBIAN_FRONTEND=noninteractive
H=@HOST@
ok() { echo "SW $1 PASS ${2:-}"; }
bad() { echo "SW $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/sw.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/sw.out | tr '\\n' ' ')"; else bad "$name" "$(tail -c 600 /tmp/sw.out | tr '\\n' ' ')"; fi; }
event() { wget -q -O /dev/null --post-data=x "$H/event/$1" || true; }
gate() { i=0; while ! wget -q -O /dev/null "$H/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt 5400 ] && return 1; sleep 1; done; }
echo 'Acquire::http::Proxy "@HOST@/";' > /etc/apt/apt.conf.d/99nexus-test-proxy
for f in /etc/apt/sources.list.d/google-chrome*; do [ -e "$f" ] && mv "$f" "/tmp/$(basename "$f").aside"; done
mkdir -p /etc/apt/keyrings; wget -q -O /etc/apt/keyrings/nexus-test.gpg $H/files/key.gpg
printf 'Types: deb\\nURIs: @HOST@/files/repo\\nSuites: nexus-test\\nComponents: main\\nSigned-By: /etc/apt/keyrings/nexus-test.gpg\\n' > /etc/apt/sources.list.d/nexus-test.sources
check setup sh -c 'apt-get update && apt-get -y install nexus-greeting && [ "$(nexus-greeting)" = "nexus-greeting 1.0" ] && ! dpkg -l cowsay 2>/dev/null | grep -q "^ii"'
event ready
gate verify
check cowsay-removed sh -c '! dpkg -l cowsay 2>/dev/null | grep -q "^ii"; dpkg -l cowsay 2>/dev/null | tail -1; ! dpkg -l cowsay 2>/dev/null | grep -q "^ii"'
check cowsay-was-installed sh -c 'grep -E " (install|remove) cowsay:all " /var/log/dpkg.log | tail -2; grep -q " install cowsay:all " /var/log/dpkg.log && grep -q " remove cowsay:all " /var/log/dpkg.log'
check greeting-upgraded sh -c '[ "$(nexus-greeting)" = "nexus-greeting 2.0" ] && dpkg -s nexus-greeting | grep -q "Version: 2.0"'
check sudo-logged sh -c 'nexusctl logs -g "COMMAND=/usr/lib/nexus/nexus-pkg-helper" | tail -3; nexusctl logs -g "COMMAND=/usr/lib/nexus/nexus-pkg-helper install cowsay" | grep -q sudo'
check wrong-password-logged sh -c '{ nexusctl logs -g "incorrect password"; nexusctl logs -g "authentication failure"; } > /tmp/sw-auth.txt; grep -i sudo /tmp/sw-auth.txt | tail -2; grep -qi sudo /tmp/sw-auth.txt'
check dpkg-consistent sh -c '[ -z "$(dpkg --audit)" ] && apt-get check'
rm -f /etc/apt/apt.conf.d/99nexus-test-proxy /etc/apt/sources.list.d/nexus-test.sources
for f in /tmp/google-chrome*.aside; do [ -e "$f" ] && mv "$f" "/etc/apt/sources.list.d/$(basename "$f" .aside)"; done
echo "SW DONE"
'''


class Ui:
    def __init__(self, vm): self.vm = vm; self.mark = 0
    def log(self): return self.vm.text()

    def wait(self, pattern, seconds=60, after=None):
        start = self.mark if after is None else after; end = time.time() + seconds
        while time.time() < end:
            m = None
            for m in re.finditer(pattern, self.log()[start:]): pass
            if m: return m
            if self.vm.proc.poll() is not None: return None
            time.sleep(0.3)
        return None

    def here(self): self.mark = len(self.log())

    def ui(self, seconds=20, condition=''):
        """The latest UI line (after the mark) matching condition: {field: value}."""
        m = self.wait(r'\[64\] SOFTWARE UI tab=(\d+) items=(\d+) selected=(\S+) asking=(\d) primary=(\d+),(\d+) search=(\d+),(\d+) row0=(\d+),(\d+) '
                      r'tab1=(\d+),(\d+) tab2=(\d+),(\d+) check=(\d+),(\d+) auth=(\d+),(\d+)' + condition, seconds)
        if not m: return None
        g = m.groups()
        return {'tab': int(g[0]), 'items': int(g[1]), 'selected': g[2], 'asking': g[3] == '1', 'primary': (int(g[4]), int(g[5])),
                'search': (int(g[6]), int(g[7])), 'row0': (int(g[8]), int(g[9])), 'tab1': (int(g[10]), int(g[11])),
                'tab2': (int(g[12]), int(g[13])), 'check': (int(g[14]), int(g[15])), 'auth': (int(g[16]), int(g[17]))}

    def click(self, local, window):
        # nexus-software asks for no server-side decoration: its surface starts at the window position Aurora reports
        self.vm.click(window[0] + local[0], window[1] + local[1]); time.sleep(0.8)


def ctrl(vm, key): vm.key('ctrl', True); vm.tap(key); vm.key('ctrl', False)


started = time.time()
server = vm = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    repo.publish([greeting('1.0')])
    def stage(fs):
        pkg_vm.refresh_services(fs)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage), flush=True)
    server = pkg_vm.HookServer(HOOK, {'key.gpg': repo.key.public_packets(), **repo_files()}, WORK / 'uploads', args.proxy_cache)
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='software', accel=args.accel, smp=2, memory='2048', hook_port=server.port)
    u = Ui(vm)
    logged_in, detail = pkg_vm.greeter_login(vm, PASSWORD)   # the base boots to the greeter (password, no automatic login)
    check('logged in through the greeter', logged_in, detail)
    ready_deadline = time.time() + 1800
    while time.time() < ready_deadline and not any(name == 'ready' for _, name, _ in server.events) and vm.proc.poll() is None: time.sleep(1)
    check('guest setup done (test repository, nexus-greeting 1.0 installed)', re.search(r'^SW setup PASS', vm.text(), re.M), re.findall(r'^SW setup.*$', vm.text(), re.M))
    repo.publish([greeting('2.0')]); server.files.update(repo_files())   # 2.0 appears at the next apt-get update
    time.sleep(5)
    # ---- Spotlight starts Software
    u.here(); vm.point(960, 540); ctrl(vm, 'spc'); time.sleep(1.5); vm.type('software'); time.sleep(1.5); vm.shot('spotlight'); vm.tap('ret')
    started_line = u.wait(r'\[64\] SOFTWARE STARTED', 60)
    win_m = u.wait(r'AURORA (?:WINDOW|TITLE) app=(\S*) title=Software x=(-?\d+) y=(-?\d+)', 60)
    check('Spotlight starts the Software app (its window appears)', started_line and win_m, re.findall(r'AURORA [^\n]*', vm.text()[u.mark:])[-6:])
    if not win_m: raise SystemExit('no Software window')
    win = (int(win_m.group(2)), int(win_m.group(3)))
    installed = u.wait(r'\[64\] SOFTWARE INSTALLED count=(\d+)', 60)
    check('the installed list is read (dpkg-query)', installed and int(installed.group(1)) > 300, installed and installed.group(0))
    first = u.ui(30)
    if not first: raise SystemExit('no UI line')
    time.sleep(2); picture = vm.shot('software-start')
    px, py = win[0] + first['primary'][0], win[1] + first['primary'][1]
    colour = picture.getpixel((px - 60, py))
    check('the window is drawn where Aurora put it (primary button colour at its place)', all(abs(a - b) < 40 for a, b in zip(colour, (0xa4, 0xb4, 0xcb))) or
          all(abs(a - b) < 40 for a, b in zip(colour, (0x2f, 0x9d, 0x6a))), colour)
    # ---- Explore: search, select, install with a wrong password, then the right one
    u.here(); u.click(first['search'], win); vm.type('cowsay'); vm.tap('ret')
    found = u.wait(r'\[64\] SOFTWARE SEARCH query=cowsay results=(\d+)', 120)
    check('search "cowsay" finds packages (apt-cache)', found and int(found.group(1)) >= 1, found and found.group(0))
    ui = u.ui(20) or first
    u.here(); u.click(ui['row0'], win)
    ui = u.ui(20, '')
    check('a result row can be selected', ui and ui['selected'].startswith('cowsay'), ui)
    u.here(); u.click(ui['primary'], win)
    ask = u.wait(r'\[64\] SOFTWARE ASK op=install', 20); time.sleep(1); vm.shot('software-password')
    check('Install asks for the password', ask)
    vm.type('wrong-password'); vm.tap('ret')
    done = u.wait(r'\[64\] SOFTWARE JOB done op=install status=(\d+) auth=(\d) denied=(\d)', 180)
    check('a wrong password is refused (sudo), nothing installed', done and done.group(1) != '0' and done.group(2) == '1', done and done.group(0))
    time.sleep(2); vm.shot('software-wrong-password')
    ui = u.ui(20)
    u.here(); u.click(ui['primary'], win); u.wait(r'\[64\] SOFTWARE ASK op=install', 20); vm.type(PASSWORD)
    ui = u.ui(10) or ui
    u.click(ui['auth'], win)   # the Authenticate button this time
    progress = u.wait(r'\[64\] SOFTWARE PROGRESS (\d+) pmstatus', 300)
    done = u.wait(r'\[64\] SOFTWARE JOB done op=install status=(\d+)', 600)
    check('installing shows apt progress', progress, '')
    check('the right password installs cowsay (helper exit 0)', done and done.group(1) == '0', done and done.group(0))
    time.sleep(3); vm.shot('software-installed')
    # ---- Installed: filter, remove
    ui = u.ui(20)
    u.here(); u.click(ui['tab1'], win); time.sleep(1)
    ui = u.ui(20, '')
    u.click(ui['search'], win)
    for _ in range(12): vm.tap('backspace')   # the search text is shared with Explore: replace it
    vm.type('cowsay'); time.sleep(2)
    ui = u.ui(20, '')
    check('Installed tab lists cowsay when filtered', ui and ui['tab'] == 1 and ui['items'] >= 1, ui)
    u.here(); u.click(ui['row0'], win); ui = u.ui(10) or ui
    u.here(); u.click(ui['primary'], win); u.wait(r'\[64\] SOFTWARE ASK op=remove', 20); vm.type(PASSWORD); vm.tap('ret')
    done = u.wait(r'\[64\] SOFTWARE JOB done op=remove status=(\d+)', 600)
    check('Remove takes cowsay away (helper exit 0)', done and done.group(1) == '0', done and done.group(0))
    time.sleep(3); vm.shot('software-removed')
    # ---- Updates: check, update all
    ui = u.ui(20)
    u.here(); u.click(ui['tab2'], win)
    listed = u.wait(r'\[64\] SOFTWARE UPDATES count=(\d+)', 120)
    ui = u.ui(20) or ui
    u.here(); u.click(ui['check'], win); u.wait(r'\[64\] SOFTWARE ASK op=update', 20); vm.type(PASSWORD); vm.tap('ret')
    done = u.wait(r'\[64\] SOFTWARE JOB done op=update status=(\d+)', 600)
    check('Check for updates runs apt-get update (helper exit 0)', done and done.group(1) == '0', done and done.group(0))
    listed = u.wait(r'\[64\] SOFTWARE UPDATES count=(\d+)', 180)
    check('the update to nexus-greeting 2.0 is listed', listed and int(listed.group(1)) >= 1, listed and listed.group(0))
    time.sleep(2); vm.shot('software-updates')
    ui = u.ui(20)
    u.here(); u.click(ui['primary'], win); u.wait(r'\[64\] SOFTWARE ASK op=upgrade', 20); vm.type(PASSWORD); vm.tap('ret')
    done = u.wait(r'\[64\] SOFTWARE JOB done op=upgrade status=(\d+)', 900)
    check('Update all installs the updates (helper exit 0)', done and done.group(1) == '0', done and done.group(0))
    listed = u.wait(r'\[64\] SOFTWARE UPDATES count=(\d+)', 180)
    check('afterwards no update is listed', listed and listed.group(1) == '0', listed and listed.group(0))
    time.sleep(2); vm.shot('software-updated')
    u.here(); vm.tap('esc')
    check('Esc closes the app', u.wait(r'\[64\] SOFTWARE CLOSED', 30))
    # ---- the guest's own checks
    server.open_gate('verify')
    end = time.time() + 600
    while time.time() < end and 'SW DONE' not in vm.text() and vm.proc.poll() is None: time.sleep(1)
    log = vm.text()
    for m in re.finditer(r'^SW (\S+) (PASS|FAIL) ?(.*)$', log, re.M):
        if m.group(1) != 'setup': check('guest: ' + m.group(1), m.group(2) == 'PASS', m.group(3))
    check('guest checks finished', 'SW DONE' in log)
    check('no kernel faults', 'KERNEL_FAULT' not in log, re.findall(r'.*KERNEL_FAULT.*', log)[:3])
    check('the archive was reached only through deb.debian.org', not server.refused, server.refused[:5])
finally:
    if vm: vm.close()
    if server: server.close()
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    (WORK / 'stage.qcow2').unlink(missing_ok=True)
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
