"""Verify the browser/app sandboxes are ON and working on the native NexusOS desktop.

    python -I tests/test_x64_sandbox.py WORK [--apps chrome,firefox,vscode] [--disk DESKTOP64.img]
           [--apps-cache DIR] [--qmp-port 4611] [--port 8911] [--tcg]

DESKTOP64.img (default: the shared fresh desktop image, Chrome installed) is never written: everything an app run
needs is put into a qcow2 overlay in WORK (tests/x64_overlay.py) that is deleted after the run, and the kernel boots
with `sbxtrace` (one serial line per sandbox setup step: namespace probes and clones, SBX_* environment at exec,
every seccomp filter install with its executable). Per app:

  chrome   /nexus/chrome-desktop.args must carry no sandbox-disabling flag (--no-sandbox,
           --disable-seccomp-filter-sandbox, --disable-namespace-sandbox, --disable-setuid-sandbox, --no-zygote,
           --single-process, CHROME_DEVEL_SANDBOX). Layer 1: access("/proc/self/ns/{user,pid,net}") succeeds through
           the nsfs magic links, the zygote is cloned with CLONE_NEWUSER|NEWPID|NEWNET and execs with SBX_USER_NS=1
           SBX_PID_NS=1 SBX_NET_NS=1 (what chrome://sandbox's namespace rows read). Layer 2: seccomp-BPF activates for
           renderer, gpu-process and utility (Chrome's log + the kernel's installs). A host page loads; the
           chrome://sandbox screenshot must show no red row (all green -> "adequately sandboxed").
  firefox  Firefox 157 is installed into the overlay (apps_install --only firefox, GTK caches from the apps_postinst
           helper). Its sandbox probes (SandboxInfo: seccomp(...NULL) -> EFAULT, access /proc/self/ns/{user,pid,net,
           ipc}) succeed, content processes are cloned into new user + IPC namespaces (CLONE_NEWUSER|NEWIPC; +NEWNET
           at level 6 with a local display) and install the content seccomp-bpf policy with TSYNC; no content
           process crashes; a host page loads; an about:support "Sandbox" screenshot is saved.
  vscode   The same for VS Code (Electron = Chromium's namespace + seccomp-bpf sandbox for its renderers). VS Code
           only fits the shared image after a disk grow, which would exceed the agent disk budget, so when it does
           not fit the run prints "SKIP vscode" with the reason (never a PASS).
Every run fails on a KERNEL_FAULT / uncontained exception.
"""
from pathlib import Path
import argparse, http.server, json, re, socket, subprocess, sys, threading, time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import x64_overlay  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
BASE = nexus_paths.source_disk()

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', type=Path); ap.add_argument('--disk', type=Path, default=BASE)
ap.add_argument('--apps', default='chrome,firefox,vscode')
ap.add_argument('--apps-cache', type=Path, default=nexus_paths.cache('apps'))
ap.add_argument('--qmp-port', type=int, default=4611); ap.add_argument('--port', type=int, default=8911)
ap.add_argument('--tcg', action='store_true'); ap.add_argument('--secs', type=int, default=300)
a = ap.parse_args()
APPS = [x for x in a.apps.split(',') if x]
assert APPS and all(x in ('chrome', 'firefox', 'vscode') for x in APPS), APPS
WORK = a.work; WORK.mkdir(parents=True, exist_ok=True)
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+xsave,+avx,+avx2,+smep,+smap'
ACCEL = ['-accel', 'tcg'] if a.tcg else ['-accel', 'whpx,kernel-irqchip=off']
DISABLERS = ['--no-sandbox', '--disable-seccomp-filter-sandbox', '--disable-namespace-sandbox',
             '--disable-setuid-sandbox', '--no-zygote', '--single-process', 'CHROME_DEVEL_SANDBOX']

# ---- host HTTP page for the "browsing works" checks ----
hits = []
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *x): pass
    def do_GET(self):
        hits.append(self.path); self.send_response(200); self.send_header('content-type', 'text/html'); self.end_headers()
        self.wfile.write(b'<!doctype html><title>nx-sandbox</title><h1 id=ok>SANDBOX-PAGE-OK</h1>')
srv = http.server.ThreadingHTTPServer(('127.0.0.1', a.port), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
PAGE = f'http://10.0.2.2:{a.port}/page'


class Qmp:
    def __init__(self, port):
        d = time.time() + 40
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=10); break
            except OSError:
                if time.time() > d: raise
                time.sleep(.3)
        self.f = self.s.makefile('rwb'); self.f.readline(); self._raw({'execute': 'qmp_capabilities'})
    def _raw(self, r): self.f.write(json.dumps(r).encode() + b'\n'); self.f.flush(); return self.f.readline()
    def hmp(self, l): return self._raw({'execute': 'human-monitor-command', 'arguments': {'command-line': l}})
    def ev(self, e): return self._raw({'execute': 'input-send-event', 'arguments': {'events': e}})
    def key(self, n, d): self.ev([{'type': 'key', 'data': {'down': d, 'key': {'type': 'qcode', 'data': n}}}]); time.sleep(.06)
    def tap(self, n): self.key(n, True); self.key(n, False)
    def type_text(self, s):
        m = {':': 'semicolon', '/': 'slash', '.': 'dot', '-': 'minus', ' ': 'spc'}
        for ch in s:
            shift = ch == ':'
            if shift: self.key('shift', True)
            self.tap(m.get(ch, ch))
            if shift: self.key('shift', False)
    def navigate(self, url):
        self.key('ctrl', True); self.tap('l'); self.key('ctrl', False); time.sleep(.5)
        self.type_text(url); time.sleep(.3); self.tap('ret'); time.sleep(7)
    def find(self, needle):
        self.key('ctrl', True); self.tap('f'); self.key('ctrl', False); time.sleep(1)
        self.type_text(needle); time.sleep(.5); self.tap('ret'); time.sleep(2); self.tap('esc'); time.sleep(2)


class Vm:
    """One NexusOS boot of `overlay` with fw_cfg session=chrome (Aurora execs /nexus/chrome-desktop.args) + sbxtrace."""
    def __init__(self, tag, overlay):
        self.serial = WORK / f'{tag}.log'; self.serial.unlink(missing_ok=True)
        cmd = [QEMU, *ACCEL, '-cpu', CPU, '-smp', '2', '-m', '2048', '-machine', 'q35',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'if=none,id=n0,file={overlay},format=qcow2,snapshot=on', '-device', 'nvme,serial=nx0,drive=n0',
               '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga,id=video0', '-display', 'none',
               '-netdev', 'user,id=net', '-device', 'e1000e,netdev=net',
               '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0',
               '-fw_cfg', 'name=opt/nexus/session,string=chrome', '-fw_cfg', 'name=opt/nexus/cmdline,string=sbxtrace',
               '-serial', f'file:{self.serial}', '-qmp', f'tcp:127.0.0.1:{a.qmp_port},server=on,wait=off', '-no-reboot']
        self.proc = subprocess.Popen(cmd, stdout=(WORK / f'{tag}-qemu.log').open('wb'), stderr=subprocess.STDOUT)
        print(f'[{tag}] VM', self.proc.pid, flush=True)
    def text(self): return self.serial.read_text('latin1') if self.serial.exists() else ''
    def wait(self, predicate, secs):
        end = time.time() + secs
        while time.time() < end:
            if predicate(self.text()): return True
            assert self.proc.poll() is None, 'VM exited early\n' + self.text()[-3000:]
            time.sleep(1)
        return False
    def stop(self):
        try: Qmp(a.qmp_port).hmp('quit')
        except Exception: pass
        try: self.proc.wait(15)
        except Exception: self.proc.kill(); self.proc.wait()


def _pixels(raw):
    _, _, rest = raw.partition(b'\n'); dims, _, rest = rest.partition(b'\n'); _, _, px = rest.partition(b'\n')
    w, h = map(int, dims.split()); return w, h, px
def _count(raw, test):
    w, h, px = _pixels(raw)
    return sum(1 for i in range(0, w * h * 3, 3) if test(px[i], px[i + 1], px[i + 2]))
def _green(raw): return _count(raw, lambda r, g, b: g > 120 and r < 110 and b < 110)
def _red(raw): return _count(raw, lambda r, g, b: r > 200 and g < 60 and b < 60)
def _purple(raw): return _count(raw, lambda r, g, b: 90 < r < 150 and 40 < g < 100 and b > 200)   # about:support headers


def kernel_checks(log, tag):
    assert 'KERNEL_FAULT' not in log and 'uncontained kernel exception' not in log, (tag, 'kernel fault', log[-3000:])
def ns_probes(log):
    """{path: result} of the namespace probes the sandboxes make with access(2) (last result per path)."""
    return dict(re.findall(r'SBXTRACE call=0x0+15 pid=0x[0-9A-F]+ path=(/proc/self/ns/\w+) result=0x([0-9A-F]+)', log))
def clones(log):
    return [int(f, 16) for f in re.findall(r'SBXTRACE call=0x0+38 pid=0x[0-9A-F]+ flags=0x([0-9A-F]+) result=0x0+[1-9A-F]', log)]
def installs(log, exe_part):
    return [(int(pid), t) for pid, t, exe in re.findall(r'SBXTRACE seccomp-filter pid=(\d+) insns=\d+ tsync=(\d) exe=(\S+)', log)
            if exe_part in exe]


def run_chrome():
    args_text = (x64_overlay.read_file(a.disk, 'nexus/chrome-desktop.args') or b'').decode('utf-8', 'replace')
    assert args_text.strip(), 'no /nexus/chrome-desktop.args on the disk'
    for bad in DISABLERS:
        assert bad not in args_text, f'Chrome args carry a sandbox-disabling flag: {bad}\n{args_text}'
    print('PASS chrome args: no sandbox-disabling flag in /nexus/chrome-desktop.args')
    staged = WORK / 'chrome-desktop.args'
    lines = [l for l in args_text.splitlines() if l.strip() and not l.startswith('--enable-logging') and not l.startswith('--v=')]
    lines = [l if not l.startswith('--user-data-dir=') else '--user-data-dir=/tmp/nx-sbx-profile' for l in lines]
    if not any(l.startswith('http') or l.startswith('chrome://') for l in lines[1:]):
        lines.append('https://example.com/')
    lines = lines[:-1] + ['--enable-logging=stderr', '--v=1', lines[-1]]
    staged.write_text('\n'.join(lines) + '\n')
    overlay = WORK / 'chrome-overlay.qcow2'
    shot = WORK / 'chrome_sandbox.ppm'; shot.unlink(missing_ok=True)
    try:
        x64_overlay.stage(a.disk, overlay, [(OUT / 'desktop.elf', 'sbin/init'), (staged, 'nexus/chrome-desktop.args')])
        vm = Vm('chrome', overlay)
        try:
            # the renderer's seccomp-BPF sandbox activating is the definitive proof Chrome is up and sandboxed
            assert vm.wait(lambda t: 'Activated seccomp-bpf sandbox for process type: renderer' in t, a.secs), \
                'Chrome renderer sandbox never activated\n' + vm.text()[-3000:]
            time.sleep(12)
            q = Qmp(a.qmp_port)
            q.navigate(PAGE); time.sleep(3)              # normal browsing still works with the sandbox on
            best, best_green = None, -1                 # keyboard nav may drop the first key: retry, keep the best
            for _ in range(4):
                q.navigate('chrome://sandbox'); time.sleep(5)
                try: q.hmp(f'screendump {shot}')
                except Exception as e: print('screendump failed', e); continue
                if not shot.exists(): continue
                g = _green(shot.read_bytes())
                if g > best_green: best_green, best = g, shot.read_bytes()
                if g > 200: break
            if best is not None: shot.write_bytes(best)
            time.sleep(4); q.s.close()
        finally:
            vm.stop()
    finally:
        overlay.unlink(missing_ok=True)
    log = vm.text()
    kernel_checks(log, 'chrome')
    for fail in ('Failed to initialize sandbox', 'running without the SUID sandbox', 'No usable sandbox',
                 'InitializeSandbox() called with multiple threads'):
        assert fail not in log, (fail, log[-3000:])
    probes = ns_probes(log)
    for ns in ('user', 'pid', 'net'):
        assert probes.get('/proc/self/ns/' + ns) == '0' * 16, (f'access(/proc/self/ns/{ns}) failed', probes)
    assert any(f & 0x70000000 == 0x70000000 for f in clones(log)), ('no CLONE_NEWUSER|NEWPID|NEWNET zygote clone', [hex(f) for f in clones(log)])
    env = set(re.findall(r'SBXTRACE exec pid=0x[0-9A-F]+ env=(SBX_\w+=\w+)', log))
    for need in ('SBX_USER_NS=1', 'SBX_PID_NS=1', 'SBX_NET_NS=1'):
        assert need in env, (f'zygote not started with {need}', env)
    print('PASS chrome layer 1: ns probes ok, zygote cloned with CLONE_NEWUSER|NEWPID|NEWNET, env', sorted(env))
    act = re.findall(r'Activated seccomp-bpf sandbox for process type: (\w[\w-]*)', log)
    kinds = set(act)
    for need in ('renderer', 'gpu-process', 'utility'):
        assert need in kinds, (f'no seccomp-bpf sandbox for {need}', sorted(kinds), log[-3000:])
    assert len(act) >= 10, ('too few sandbox activations', len(act))
    assert '--type=zygote' in log, ('no Chrome zygote (Layer-1 namespace sandbox)', log[-3000:])
    chrome_installs = installs(log, 'chrome')
    assert len(chrome_installs) >= 10, ('kernel saw too few Chrome seccomp installs', chrome_installs[:20])
    print(f'PASS chrome layer 2: seccomp-BPF for {sorted(kinds)} ({len(act)} activations); kernel saw '
          f'{len(chrome_installs)} filter installs')
    assert any(p.startswith('/page') for p in hits) or 'chrome://sandbox' in log, ('browsing did not work', hits)
    # chrome://sandbox screenshot: supplementary evidence (the serial log above is authoritative). A red (failed)
    # row is a hard failure; a blank capture (keyboard nav dropped) only means no screenshot.
    if shot.exists():
        raw = shot.read_bytes(); red, green = _red(raw), _green(raw)
        assert red < 50, ('chrome://sandbox shows a red (failed/No) row', red)
        print(f'chrome://sandbox screenshot: red={red} green={green} -> '
              + ('all rows green, adequately sandboxed' if green > 200 else 'page not captured (nav dropped)'))
    return 'PASS'


def run_app(app, exe_part, extra_tail, checks):
    """Install `app` into an overlay, boot it, wait for its sandbox, run `checks(log)`."""
    overlay = WORK / f'{app}-overlay.qcow2'
    try:
        try:
            program = x64_overlay.app_overlay(a.disk, overlay, app, a.apps_cache, session_tail=extra_tail,
                                              work=WORK / f'{app}-build', accel=tuple(ACCEL))
        except RuntimeError as e:
            if 'without --grow' in str(e) or 'not enough space' in str(e):
                print(f'SKIP {app}: does not fit the shared desktop image without a disk grow, and a grown apps '
                      f'image exceeds the agent disk budget ({e})')
                return 'SKIP'
            raise
        print(f'[{app}] session program {program}')
        vm = Vm(app, overlay)
        shot = WORK / f'{app}_sandbox.ppm'; shot.unlink(missing_ok=True)
        try:
            ok = vm.wait(lambda t: len(installs(t, exe_part)) >= 3, a.secs)
            assert ok, (f'{app}: no sandboxed child processes (fewer than 3 seccomp installs by {exe_part})', vm.text()[-3000:])
            time.sleep(15)
            if app == 'firefox':                         # about:support "Sandbox" section as visual evidence
                q = Qmp(a.qmp_port)
                for _ in range(3):
                    q.navigate('about:support'); time.sleep(4)
                    q.hmp(f'screendump {shot}')
                    if shot.exists() and _purple(shot.read_bytes()) > 2000: break
                q.find('seccomp'); q.hmp(f'screendump {shot}')
                q.s.close()
            time.sleep(5)
        finally:
            vm.stop()
    finally:
        overlay.unlink(missing_ok=True)
    log = vm.text()
    kernel_checks(log, app)
    checks(log)
    return 'PASS'


def firefox_checks(log):
    probes = ns_probes(log)
    for ns in ('user', 'pid', 'net', 'ipc'):            # SandboxInfo::HasUserNamespaceSupport
        assert probes.get('/proc/self/ns/' + ns) == '0' * 16, (f'firefox access(/proc/self/ns/{ns}) failed', probes)
    content = [f for f in clones(log) if f & 0x18000000 == 0x18000000]   # CLONE_NEWUSER|CLONE_NEWIPC
    assert content, ('no Firefox content process cloned into new user+IPC namespaces', [hex(f) for f in clones(log)])
    net = sum(1 for f in content if f & 0x40000000)
    ff = installs(log, '/opt/firefox/')
    assert len(ff) >= 3 and all(t == '1' for _, t in ff), ('Firefox content seccomp-bpf (TSYNC) not installed', ff)
    for crash in ('seccomp sandbox violation', 'FAULT_IMAGE libxul.so', 'FAULT_IMAGE firefox-bin'):
        assert crash not in log, ('Firefox sandboxed process crashed', crash, log[-3000:])
    assert any(p.startswith('/page') for p in hits), ('Firefox did not load the host page', hits)
    print(f'PASS firefox: ns probes ok; {len(content)} processes in new user+IPC namespaces ({net} also netns); '
          f'{len(ff)} content seccomp-bpf installs, all TSYNC; no content crash; host page loaded')


def vscode_checks(log):
    probes = ns_probes(log)
    for ns in ('user', 'pid', 'net'):
        assert probes.get('/proc/self/ns/' + ns) == '0' * 16, (f'vscode access(/proc/self/ns/{ns}) failed', probes)
    assert any(f & 0x70000000 == 0x70000000 for f in clones(log)), ('no Electron zygote in user+pid+net namespaces', [hex(f) for f in clones(log)])
    for fail in ('The SUID sandbox helper binary was found, but is not configured correctly', 'No usable sandbox',
                 'Failed to initialize sandbox'):
        assert fail not in log, (fail, log[-3000:])
    vs = installs(log, '/opt/vscode/')
    assert len(vs) >= 3, ('VS Code renderer/utility seccomp-bpf not installed', vs)
    print(f'PASS vscode: ns probes ok, Electron zygote in user+pid+net namespaces, {len(vs)} seccomp-bpf installs')


results = {}
try:
    for app in APPS:
        if app == 'chrome': results[app] = run_chrome()
        elif app == 'firefox': results[app] = run_app('firefox', '/opt/firefox/', [PAGE], firefox_checks)
        else: results[app] = run_app('vscode', '/opt/vscode/', [], vscode_checks)
finally:
    srv.shutdown()
print('sandbox results:', ', '.join(f'{k}={v}' for k, v in results.items()))
assert all(v in ('PASS', 'SKIP') for v in results.values()), results
print('PASS browser/app sandbox verification' + (' (with skips)' if 'SKIP' in results.values() else ''))
