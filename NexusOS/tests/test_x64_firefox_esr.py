"""Debian firefox-esr from apt runs on an ext4 install (kabi, wave 2).

python -I tests/test_x64_firefox_esr.py WORK --base INSTALLED.qcow2 [--port 9105] [--qmp-port 4705]
       [--proxy-cache DIR] [--write-cache DIR] [--smp 2]
INSTALLED.qcow2: the installed Debian base that tests/test_x64_services.py --keep-installed leaves (pkg group).
Boots it on a fresh overlay (the base is never written), installs firefox-esr through a deb.debian.org-only caching
proxy (reads --proxy-cache, default the shared D:\\nexustest\\pkgs\\pkg-cache\\apt-proxy, read only; new downloads go to
--write-cache), then as the desktop user runs `firefox-esr --headless --screenshot` of a local page and checks the PNG
(size, PNG signature, the page's colour), and `firefox-esr --version`. Fails on any USER_FAULT of firefox, on
KERNEL_FAULT, and lists every `ENOSYS syscall=` seen.
Kernel trace records (kmsg.c): the boot's serial log carries the process traces (EXEC_ARGS, PROCESS_EXIT, ...), but
neither dmesg nor the persistent journal (nexusctl logs -k) has one; a second boot with the `kmsg.trace` boot option
(QEMU fw_cfg opt/nexus/cmdline) finds them in dmesg as KERN_DEBUG (<7>) records.
pkg-style: run it under C:\\nexustest\\wave2\\pkgvm_lock.py; the refreshed test hook fetches this run's --port
(nexus.testhook=PORT, pkg_vm.Vm(hook_port=...); kernel options go through Vm(cmdline=...))."""
from pathlib import Path
import argparse, http.server, re, struct, sys, threading, time, urllib.error, urllib.parse, urllib.request, zlib

parser = argparse.ArgumentParser()
parser.add_argument('work'); parser.add_argument('--base', required=True)
parser.add_argument('--port', type=int, default=9105); parser.add_argument('--qmp-port', type=int, default=4705)
parser.add_argument('--proxy-cache', default=r'D:\nexustest\pkgs\pkg-cache\apt-proxy')
parser.add_argument('--write-cache', default=r'D:\nexustest\wave2\ws_kabi_cache\apt-proxy')
parser.add_argument('--smp', type=int, default=2); parser.add_argument('--memory', default='2048')
options = parser.parse_args()
ROOT = Path(__file__).resolve().parents[1]
work, base = Path(options.work).resolve(), Path(options.base).resolve()
read_cache, write_cache = Path(options.proxy_cache), Path(options.write_cache)
sys.path.insert(0, str(ROOT / 'tests'))
import pkg_vm  # noqa: E402

work.mkdir(parents=True, exist_ok=True)
PAGE = b'<!doctype html><html><body style="margin:0;background:#1f7a3a"><h1 style="color:#fff">NexusOS firefox-esr</h1></body></html>'
GUEST = r'''PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH DEBIAN_FRONTEND=noninteractive
echo 'Acquire::http::Proxy "http://10.0.2.2:@PORT@/";' > /etc/apt/apt.conf.d/99probe-proxy
for f in /etc/apt/sources.list.d/google-chrome*; do [ -e "$f" ] && mv "$f" "/tmp/$(basename "$f").aside"; done
apt-get update >/tmp/u.out 2>&1; echo "PROBE apt-update rc=$?"
apt-get install -y --no-install-recommends firefox-esr >/tmp/i.out 2>&1
echo "PROBE install rc=$? $(tail -c 300 /tmp/i.out | tr '\n' ' ')"
rm -rf /tmp/ffp; mkdir -p /tmp/ffp; chown nexus /tmp/ffp
wget -q -O /tmp/ffp/page.html http://10.0.2.2:@PORT@/files/page.html; chmod 644 /tmp/ffp/page.html
su nexus -c 'HOME=/tmp/ffp timeout 120 firefox-esr --version' >/tmp/ffv.out 2>&1; echo "PROBE version rc=$? $(head -c 200 /tmp/ffv.out | tr '\n' ' ')"
su nexus -c 'cd /tmp/ffp && HOME=/tmp/ffp MOZ_CRASHREPORTER_DISABLE=1 timeout 240 firefox-esr --headless --window-size=800,600 --screenshot /tmp/ffp/shot.png file:///tmp/ffp/page.html' >/tmp/ff.out 2>&1
rc=$?; echo "PROBE firefox-esr rc=$rc shot=$([ -s /tmp/ffp/shot.png ] && stat -c %s /tmp/ffp/shot.png || echo none) $(tail -c 600 /tmp/ff.out | tr '\n' ' ')"
[ -s /tmp/ffp/shot.png ] && wget -q -O /dev/null --post-file=/tmp/ffp/shot.png http://10.0.2.2:@PORT@/upload/shot.png
for f in /tmp/google-chrome*.aside; do [ -e "$f" ] && mv "$f" "/etc/apt/sources.list.d/$(basename "$f" .aside)"; done
echo "PROBE kmsg-traces dmesg=$(dmesg | grep -cE 'EXEC_ARGS|PROCESS_EXIT|INTERPRETER') journal=$(nexusctl logs -k -b | grep -cE 'EXEC_ARGS|PROCESS_EXIT|INTERPRETER') kernel=$(nexusctl logs -k -b | grep -cF '[64]')"
echo "APT DONE"
touch /run/apttest.done
'''.replace('@PORT@', str(options.port))
TRACE_GUEST = r'''PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH
echo "PROBE kmsg-trace-boot debug=$(dmesg -r | grep -cE '^<7>.*(EXEC_ARGS|PROCESS_EXIT) ') cmdline=$(cat /proc/cmdline)"
echo "APT DONE"
touch /run/apttest.done
'''
HOOK = ('set -e\nmkdir -p /usr/lib/nexus/tests\nwget -q -O /usr/lib/nexus/tests/probe.sh http://10.0.2.2:%d/files/probe.sh\n'
        'sh /usr/lib/nexus/tests/probe.sh\n' % options.port).encode()
FILES = {'/hook.sh': HOOK, '/files/probe.sh': GUEST.encode(), '/files/page.html': PAGE}
refused, uploads = [], {}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def reply(self, code, body=b'', kind='application/octet-stream'):
        self.send_response(code); self.send_header('content-type', kind); self.send_header('content-length', str(len(body))); self.end_headers()
        if self.command != 'HEAD': self.wfile.write(body)
    def do_HEAD(self): self.do_GET()
    def do_CONNECT(self): refused.append(self.path); self.reply(403)
    def do_POST(self):
        length = int(self.headers.get('content-length', '0')); body = self.rfile.read(length)
        uploads[urllib.parse.urlsplit(self.path).path] = body; self.reply(200)
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


def png_pixel(data, x, y):
    """RGB of pixel (x, y) of an 8-bit RGB/RGBA non-interlaced PNG."""
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat, width = 8, b'', 0
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8]); chunk = data[pos + 8:pos + 8 + length]; pos += 12 + length
        if kind == b'IHDR': width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', chunk); assert depth == 8 and colour in (2, 6) and not interlace
        elif kind == b'IDAT': idat += chunk
    raw = zlib.decompress(idat); bpp = 4 if colour == 6 else 3; stride = width * bpp
    rows, previous = [], bytearray(stride)
    for row in range(y + 1):
        f = raw[row * (stride + 1)]; line = bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0; b = previous[i]; c = previous[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        previous = line
    return tuple(previous[x * bpp:x * bpp + 3]), width, height


servers = [http.server.ThreadingHTTPServer(('127.0.0.1', p), Handler) for p in (options.port,)]   # the hook's port: Vm(hook_port=)
for s in servers: s.daemon_threads = True; threading.Thread(target=s.serve_forever, daemon=True).start()
failures = []
try:
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, work / 'stage.qcow2', pkg_vm.refresh_services), flush=True)
    vm = pkg_vm.Vm(work, work / 'stage.qcow2', options.qmp_port, tag='ffesr', accel='auto', smp=options.smp, memory=options.memory,
                    hook_port=options.port)
    try:
        vm.wait(r'APT DONE', seconds=3600, fail=r'KERNEL_FAULT|\[64\] FAIL')
        vm.exited(240); log = vm.text()
    finally:
        vm.close()
    FILES['/files/probe.sh'] = TRACE_GUEST.encode()   # second boot: the kernel traces reach /dev/kmsg on request
    vm = pkg_vm.Vm(work, work / 'stage.qcow2', options.qmp_port, tag='trace', accel='auto', smp=options.smp, memory=options.memory,
                   hook_port=options.port, cmdline='kmsg.trace')
    try:
        vm.wait(r'APT DONE', seconds=900, fail=r'KERNEL_FAULT|\[64\] FAIL')
        vm.exited(240); trace_log = vm.text()
    finally:
        vm.close()
    probes = dict(re.findall(r'^PROBE (\S+) (.*)$', log, re.M))
    probes.update(re.findall(r'^PROBE (\S+) (.*)$', trace_log, re.M))
    for name, line in probes.items(): print('PROBE', name, line[:900], flush=True)
    serial_traces = len(re.findall(r'^\[64\] (?:EXEC_ARGS|PROCESS_EXIT|INTERPRETER) ', log, re.M))
    print('serial process traces:', serial_traces, flush=True)
    m = re.match(r'dmesg=(\d+) journal=(\d+) kernel=(\d+)', probes.get('kmsg-traces', ''))
    if not m or serial_traces < 100 or int(m.group(1)) or int(m.group(2)) or not int(m.group(3)):
        failures.append('kernel traces: serial %d, %s (want: serial only, other kernel records kept)' % (serial_traces, probes.get('kmsg-traces')))
    m = re.match(r'debug=(\d+) cmdline=.*kmsg\.trace', probes.get('kmsg-trace-boot', ''))
    if not m or int(m.group(1)) < 5: failures.append('kmsg.trace boot: ' + probes.get('kmsg-trace-boot', 'missing'))
    enosys = sorted(set(re.findall(r'ENOSYS syscall=(\S+)', log)))
    faults = re.findall(r'.*USER_FAULT.*\n(?:.*(?:FAULT_IMAGE|CALLER_IMAGE).*\n)*', log)
    ff_faults = [f for f in faults if 'firefox' in f or 'libxul' in f or 'mozilla' in f]
    print('ENOSYS:', ' '.join(int(e, 16).__str__() if e.startswith('0x') else e for e in enosys) or 'none', flush=True)
    for f in faults[:8]: print('FAULT:', f.strip()[:1500], flush=True)
    if 'KERNEL_FAULT' in log: failures.append('KERNEL_FAULT')
    if not probes.get('install', '').startswith('rc=0'): failures.append('apt install firefox-esr: ' + probes.get('install', 'missing')[:200])
    if not re.match(r'rc=0 Mozilla Firefox \d+', probes.get('version', '')): failures.append('firefox-esr --version: ' + probes.get('version', 'missing')[:200])
    if not probes.get('firefox-esr', '').startswith('rc=0 shot='): failures.append('firefox-esr --screenshot: ' + probes.get('firefox-esr', 'missing')[:300])
    if ff_faults: failures.append('firefox USER_FAULT: ' + ff_faults[0][:300])
    shot = uploads.get('/upload/shot.png')
    if not shot: failures.append('no screenshot uploaded')
    else:
        (work / 'shot.png').write_bytes(shot)
        rgb, w, h = png_pixel(shot, 400, 300)
        print('screenshot %dx%d pixel(400,300)=%s' % (w, h, rgb), flush=True)
        if (w, h) != (800, 600) or max(abs(a - b) for a, b in zip(rgb, (0x1f, 0x7a, 0x3a))) > 6: failures.append('screenshot content %dx%d %s' % (w, h, rgb))
    print('refused:', refused[:5], flush=True)
finally:
    for s in servers: s.shutdown()
    for name in ('overlay.qcow2', 'stage.qcow2'): (work / name).unlink(missing_ok=True)
for f in failures: print('FAIL', f, flush=True)
print('RESULT', 'PASS' if not failures else 'FAIL', flush=True)
sys.exit(1 if failures else 0)
