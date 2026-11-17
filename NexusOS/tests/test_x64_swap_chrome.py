"""Chrome with many heavy tabs in a 2 GiB guest: with the swap file the tabs survive (pages go to swap), without it the
OOM killer has to take renderers -- and the kernel and the browser keep running either way.

--desktop-disk (a disposable desktop disk copy, never the real one) is edited in place -- out/desktop.elf as /sbin/init,
a /swapfile in /etc/fstab when it has none (arch/x86_64/swap_tools.py swapfile --size SWAP_MIB, default auto: half the
free space, at most 2 GiB; it stays for later runs), a /nexus/chrome-desktop.args that opens --tabs copies of a
host-served page -- and booted with snapshot=on; the original chrome-desktop.args is put back afterwards, so no second
image is ever made. The guest boots straight into Chrome (fw_cfg opt/nexus/session=chrome; --no-swap adds fw_cfg
opt/nexus/swap=off, so the same disk boots without swap). Every tab fills
--tab-mb MiB of typed arrays with a per-tab pattern, then verifies every word every 20 s and reports to the host server.
The kernel's "[64] MEMSTAT" lines (every 5 s) give free memory, swap use, pswpin/pswpout and OOM kills.

PASS (swap): every tab filled and verified >= 2 times after the last tab finished filling, all with the session id it
started with (no reload or discard), no verify error, no OOM kill, swap used >= 64 MiB, no kernel fault. The browser's
NULL atomic increment in its histogram code under pressure (2026-10-29) was page reclaim dropping unlinked files' only
copies (Chrome's /tmp shared memory and its deleted BrowserMetrics .pma): fixed, 208 MiB 5/5 consecutive passes.
Kernel option mm.audit (--kernel-options mm.audit) checksums every swapped and dropped page (see docs/DEVELOPMENT_NOTES.md).
PASS (--no-swap): the kernel and the session survive (MEMSTAT keeps coming, no fault, the VM still runs) and memory ran
out (an OOM kill or a lost tab) -- otherwise the run did not overcommit RAM and proves nothing (the OOM killer takes a
renderer, oom_score_adj 300; the browser keeps running).

Usage: test_x64_swap_chrome.py WORK --desktop-disk IMG [--tabs 8] [--tab-mb 208] [--minutes 3] [--no-swap] [--smp 2]
       [--memory 2048] [--swap-mib auto|MIB] [--tcg] [--port 8920] [--qmp-port 4620]"""
from pathlib import Path
import argparse
import http.server
import importlib.util
import json
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
CHROME = '/home/nexus/Applications/chrome/opt/google/chrome/chrome'
parser = argparse.ArgumentParser()
parser.add_argument('work'); parser.add_argument('--desktop-disk', required=True)
parser.add_argument('--tabs', type=int, default=8); parser.add_argument('--tab-mb', type=int, default=208)
parser.add_argument('--minutes', type=float, default=3); parser.add_argument('--no-swap', action='store_true')
parser.add_argument('--smp', type=int, default=2); parser.add_argument('--memory', type=int, default=2048)
parser.add_argument('--swap-mib', default='auto'); parser.add_argument('--tcg', action='store_true')
parser.add_argument('--port', type=int, default=8920); parser.add_argument('--qmp-port', type=int, default=4620)
parser.add_argument('--timeout', type=int, default=1800)
parser.add_argument('--extra-url', action='append', default=[], help='diagnosis: another tab (e.g. chrome://kill)')
parser.add_argument('--kernel-options', default=None, help='fw_cfg opt/nexus/cmdline (e.g. blockio.spin)')
parser.add_argument('--chrome-flag', action='append', default=[], help='diagnosis: another Chrome switch')
options = parser.parse_args()
WORK = Path(options.work).resolve(); WORK.mkdir(parents=True, exist_ok=True)
disk = Path(options.desktop_disk).resolve()
if 'New folder' in str(disk) or not disk.is_file(): raise SystemExit('give a disposable desktop disk copy (never the real disk)')
spec = importlib.util.spec_from_file_location('chrome_fatput', ROOT / 'userland/fatput.py'); fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
spec = importlib.util.spec_from_file_location('chrome_build', ROOT / 'arch/x86_64/build.py'); build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
mode = 'noswap' if options.no_swap else 'swap'
serial = WORK / f'chrome-{mode}.log'; serial.unlink(missing_ok=True)

PAGE = r'''<!doctype html><meta charset=utf-8><title>tab</title><body style="font:20px monospace"><pre id=o>start</pre><script>
const q = new URLSearchParams(location.search), tab = +q.get('tab'), mb = +q.get('mb');
const sid = Math.random().toString(36).slice(2) + Date.now().toString(36), o = document.getElementById('o');
document.title = 'tab ' + tab;
function post(kind, data) { fetch('/r', {method: 'POST', body: JSON.stringify({kind, tab, sid, data})}).catch(() => {}); }
const CH = 16 << 20, chunks = [];
function pattern(k) { return (Math.imul(tab + 1, 0x9E3779B1) ^ Math.imul(k + 1, 0x85EBCA77)) | 0; }
function fill(k) { const u = new Int32Array(CH / 4), b = pattern(k); for (let i = 0; i < u.length; i++) u[i] = b + i; return u; }
function check(u, k) { const b = pattern(k); let bad = 0; for (let i = 0; i < u.length; i++) if (u[i] !== ((b + i) | 0)) bad++; return bad; }
post('start', {mb});
let k = 0, round = 0;
function step() {
  try { const t = performance.now(); for (let n = 0; n < 4 && k * 16 < mb; n++, k++) chunks.push(fill(k));
        o.textContent = 'tab ' + tab + ' filled ' + k * 16 + ' MiB'; post('progress', {mib: k * 16, ms: performance.now() - t}); }
  catch (e) { post('error', {stage: 'fill', mib: k * 16, error: String(e)}); return; }
  if (k * 16 < mb) setTimeout(step, 50); else { post('filled', {mib: k * 16}); setTimeout(verify, 2000); }
}
function verify() {
  const t = performance.now(); let bad = 0;
  for (let i = 0; i < chunks.length; i++) bad += check(chunks[i], i);
  round++; o.textContent = 'tab ' + tab + ' verify ' + round + ' errors ' + bad;
  post('verify', {round, errors: bad, mib: chunks.length * 16, ms: Math.round(performance.now() - t)});
  setTimeout(verify, 20000);
}
setTimeout(step, 500);
</script>'''

# ---- the disk ------------------------------------------------------------------------------------------------------
def fat_read(path):   # (bytes or None) of a small file on the disk
    fs = fatput.Fat32(str(disk), readonly=True)
    try:
        dirs, leaf = fs.split(path)
        try: hit = fs.find_name(fs.lookup_dir(dirs), leaf)
        except SystemExit: return None
        if hit is None: return None
        e = hit[2]; first = (int.from_bytes(e[20:22], 'little') << 16) | int.from_bytes(e[26:28], 'little')
        return fs.read_chain(first, int.from_bytes(e[28:32], 'little'))
    finally: fs.f.close()
tools = str(ROOT / 'arch/x86_64/swap_tools.py')
status = json.loads(subprocess.run([sys.executable, '-I', tools, 'status', str(disk)], check=True, capture_output=True, text=True).stdout)
if not status['fstab_swap']:
    subprocess.run([sys.executable, '-I', tools, 'swapfile', str(disk), '--size', str(options.swap_mib)], check=True)
    status = json.loads(subprocess.run([sys.executable, '-I', tools, 'status', str(disk)], check=True, capture_output=True, text=True).stdout)
print('[chrome] disk', disk, 'fstab swap', status['fstab_swap'], flush=True)
original_args = fat_read('nexus/chrome-desktop.args')
urls = [f'http://10.0.2.2:{options.port}/tab.html?tab={i}&mb={options.tab_mb}' for i in range(options.tabs)] + options.extra_url
args = WORK / 'chrome-desktop.args'
args.write_text('\n'.join([CHROME, '--ozone-platform=wayland', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
                           '--ipc-connection-timeout=120', '--user-data-dir=/tmp/nx-swap-profile', '--disable-background-timer-throttling',
                           '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', *options.chrome_flag, *urls]) + '\n', newline='\n')
root_marker = ['rm', 'nexus/init-as-root'] if fat_read('nexus/init-as-root') is not None else []   # Chrome must not run as root
assert fatput.edit(['fatput', str(disk), 'put', str(OUT / 'desktop.elf'), 'sbin/init', 'put', str(args), 'nexus/chrome-desktop.args',
                    *root_marker]) in (0, None)

# ---- host page server ----------------------------------------------------------------------------------------------
lock = threading.Lock(); tabs = {}; events = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = PAGE.encode() if self.path.startswith('/tab.html') else b'not found'
        self.send_response(200 if self.path.startswith('/tab.html') else 404)
        self.send_header('content-type', 'text/html'); self.send_header('cache-control', 'no-store'); self.send_header('content-length', str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        data = self.rfile.read(int(self.headers.get('content-length', 0)))
        try: m = json.loads(data)
        except ValueError: m = {'kind': 'bad'}
        m['at'] = time.time()
        with lock:
            events.append(m); t = tabs.setdefault(m.get('tab'), {'sids': [], 'filled': None, 'verify': [], 'errors': []})
            if m.get('sid') not in t['sids']: t['sids'].append(m.get('sid'))
            if m['kind'] == 'filled': t['filled'] = m['at']
            if m['kind'] == 'verify': t['verify'].append((m['at'], m['data']['errors'], m['data']['ms']))
            if m['kind'] == 'error': t['errors'].append(m['data'])
        self.send_response(204); self.send_header('access-control-allow-origin', '*'); self.end_headers()
server = http.server.ThreadingHTTPServer(('127.0.0.1', options.port), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()

# ---- the VM --------------------------------------------------------------------------------------------------------
cpu = build.desktop_cpu_options(force_tcg=options.tcg)
if options.tcg or '-accel' not in cpu: cpu = ['-accel', 'tcg,thread=multi', '-cpu', 'max']
command = [str(QEMU), *cpu, '-smp', str(options.smp), '-m', str(options.memory), '-machine', 'q35',
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
           '-drive', f'if=none,id=root,file={disk},format=raw,snapshot=on', '-device', 'nvme,serial=nexus-root,drive=root',
           '-vga', 'none', '-device', 'virtio-vga,id=video0', '-display', 'none',
           '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network',
           '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,display=video0', '-device', 'usb-tablet,bus=xhci.0,display=video0',
           '-fw_cfg', 'name=opt/nexus/session,string=chrome', *(['-fw_cfg', 'name=opt/nexus/cmdline,string=' + options.kernel_options] if options.kernel_options else []), *(['-fw_cfg', 'name=opt/nexus/swap,string=off'] if options.no_swap else []),
           '-serial', f'file:{serial}', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off',
           '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
(WORK / f'chrome-{mode}.cmd.json').write_text(json.dumps(command, indent=1))
vm = subprocess.Popen(command, stdout=(WORK / f'chrome-{mode}.qemu.log').open('wb'), stderr=subprocess.STDOUT)
print('[chrome] VM pid', vm.pid, 'unthrottled', build.unthrottle(vm.pid), mode, f'{options.tabs} tabs x {options.tab_mb} MiB', flush=True)

def log_text(): return serial.read_text(errors='replace') if serial.exists() else ''
def memstats(text):
    rows = []
    for line in text.splitlines():
        if line.startswith('[64] MEMSTAT'): rows.append({k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)})
    return rows
def screendump(name):
    try:
        with socket.create_connection(('127.0.0.1', options.qmp_port), timeout=10) as s:
            f = s.makefile('rwb'); f.readline()
            for cmd in ({'execute': 'qmp_capabilities'}, {'execute': 'screendump', 'arguments': {'filename': str(WORK / (name + '.ppm'))}}):
                f.write(json.dumps(cmd).encode() + b'\n'); f.flush()
                while 'event' in json.loads(f.readline()): pass
        from PIL import Image
        Image.open(WORK / (name + '.ppm')).save(WORK / (name + '.png')); (WORK / (name + '.ppm')).unlink()
    except Exception as error: print('[chrome] screendump failed:', error)

started = time.time(); reason = ''; ran_out = None
all_filled_at = None; last_report = 0
try:
    while time.time() - started < options.timeout:
        time.sleep(2)
        text = log_text(); now = time.time()
        if vm.poll() is not None: reason = f'VM exited with {vm.returncode}'; break
        if any(w in text for w in ('KERNEL_FAULT', 'uncontained', '[64] PANIC', 'panic(')): reason = 'kernel fault'; break
        with lock:
            filled = [t for t in tabs.values() if t['filled']]
            if all_filled_at is None and len(filled) == options.tabs: all_filled_at = max(t['filled'] for t in filled); print('[chrome] all tabs filled after', round(all_filled_at - started), 's', flush=True)
            snapshot = {k: (len(v['sids']), bool(v['filled']), len(v['verify'])) for k, v in tabs.items()}
        stats = memstats(text)
        if now - last_report > 30:
            last_report = now
            print('[chrome] t=%ds tabs=%s %s' % (now - started, snapshot, ' '.join('%s=%s' % kv for kv in (stats[-1].items() if stats else []))), flush=True)
            if all_filled_at is None: screendump(f'chrome-{mode}-progress')
        kills = text.count('OOM Out of memory: Killed process')
        if options.no_swap:
            lost = [k for k, v in tabs.items() if v['filled'] and v['verify'] and now - v['verify'][-1][0] > 90]
            if (kills or lost) and ran_out is None: ran_out = now; reason = 'memory ran out'
            if ran_out is not None and now - ran_out > 120: break   # memory ran out: the system is watched for two more minutes
            if all_filled_at and now - all_filled_at > options.minutes * 60: break
        else:
            if kills: reason = 'OOM kill with swap enabled'; break
            if all_filled_at and now - all_filled_at > options.minutes * 60: break
    else:
        reason = reason or 'timeout'
    screendump(f'chrome-{mode}-final')
finally:
    text_before_quit = log_text()
    try:
        with socket.create_connection(('127.0.0.1', options.qmp_port), timeout=10) as s:
            f = s.makefile('rwb'); f.readline()
            for cmd in ({'execute': 'qmp_capabilities'}, {'execute': 'quit'}):
                f.write(json.dumps(cmd).encode() + b'\n'); f.flush(); f.readline()
    except Exception: pass
    try: vm.wait(30)
    except subprocess.TimeoutExpired: vm.kill(); vm.wait()
    server.shutdown()
    if original_args is not None:   # the disk as it was (snapshot=on kept the guest's writes off it)
        saved = WORK / 'chrome-desktop.args.original'; saved.write_bytes(original_args)
        fatput.edit(['fatput', str(disk), 'put', str(saved), 'nexus/chrome-desktop.args']); saved.unlink()

text = text_before_quit; stats = memstats(text)
kills = [l for l in text.splitlines() if 'OOM Out of memory: Killed process' in l]
peak_swap = max((s.get('swap_used_kb', 0) for s in stats), default=0) >> 10
summary = {'mode': mode, 'tabs': options.tabs, 'tab_mb': options.tab_mb, 'memory_mb': options.memory, 'smp': options.smp,
           'seconds': round(time.time() - started), 'reason': reason, 'all_filled_after_s': round(all_filled_at - started) if all_filled_at else None,
           'peak_swap_used_mib': peak_swap, 'min_free_mib': min((s.get('free_kb', 0) for s in stats), default=0) >> 10,
           'pswpin': stats[-1].get('pswpin') if stats else None, 'pswpout': stats[-1].get('pswpout') if stats else None,
           'oom_kills': len(kills), 'oom_lines': kills[:10], 'memstat_lines': len(stats),
           'tabs_detail': {str(k): {'sessions': len(v['sids']), 'filled': bool(v['filled']), 'verifies': len(v['verify']),
                                    'verify_errors': sum(e for _, e, _ in v['verify']), 'last_verify_ms': v['verify'][-1][2] if v['verify'] else None,
                                    'errors': v['errors'][:3]} for k, v in sorted(tabs.items(), key=lambda kv: str(kv[0]))}}
(WORK / f'chrome-{mode}.json').write_text(json.dumps(summary, indent=1))
print(json.dumps(summary, indent=1))
problems = []
if any(w in text for w in ('KERNEL_FAULT', 'uncontained', '[64] PANIC')): problems.append('kernel fault')
if not stats or len(stats) < 3: problems.append('no MEMSTAT trace (kernel not alive?)')
if not options.no_swap:
    if reason: problems.append(reason)
    if len(tabs) != options.tabs: problems.append(f'{len(tabs)} of {options.tabs} tabs reported')
    for k, v in tabs.items():
        late = [x for x in v['verify'] if all_filled_at and x[0] > all_filled_at]
        if len(v['sids']) != 1: problems.append(f'tab {k} reloaded ({len(v["sids"])} sessions)')
        if not v['filled'] or len(late) < 2: problems.append(f'tab {k}: filled={bool(v["filled"])} verifies after all filled={len(late)}')
        if any(e for _, e, _ in v['verify']) or v['errors']: problems.append(f'tab {k}: data errors {v["errors"][:2]}')
    if peak_swap < 64: problems.append(f'swap barely used ({peak_swap} MiB): the tabs fit in RAM')
else:
    lost = [k for k, v in tabs.items() if len(v['sids']) > 1 or v['errors'] or not v['verify'] or (stats and v['verify'][-1][0] < time.time() - 150)]
    if not kills and not lost: problems.append('no OOM kill and no lost tab: RAM was not overcommitted (raise --tab-mb)')
    if stats and peak_swap: problems.append('swap used although disabled')
    if reason.startswith('VM exited'):   # the OOM killer may take renderers, the browser and the session must stay
        problems.append(reason + ': the session ended (browser crash after the OOM kill?)')
print('CHROME SWAP TEST', mode, 'PASSED' if not problems else 'FAILED: ' + '; '.join(problems))
sys.exit(1 if problems else 0)
