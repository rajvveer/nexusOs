"""IPv6 in real applications on NexusOS: Chrome, Firefox and Wine (winsock) on the Aurora desktop.

    python -I tests/test_x64_ipv6_apps.py WORK [--disk DESKTOP64.img] [--variants dual,v6only,slirp]
           [--apps chrome,firefox,wine] [--smp 2] [--qmp-port 4721] [--port 9120]

The desktop disk (default: the shared read-only integ desk disk with Chrome, Firefox and Wine) is never written: one
qcow2 overlay per boot gets this tree's Aurora (out/desktop.elf as /sbin/init, out/aurora_app.elf), the script runner
(out/wine_runner_user.elf as the session program) and tests/wine/winsock6.c built here for x86_64-windows-gnu. The
overlay is deleted after the boot.

Variants (one boot each; Chrome, Firefox and Wine run in it one after the other):
  dual    tests/netfix.py is the whole network (QEMU -netdev dgram): DHCPv4 WITHOUT a DNS server option, RA with prefix
          2001:db8:1::/64 + RDNSS 2001:db8:1::53, so every name is resolved over IPv6 transport. Names:
          dual (A + AAAA), v6only (AAAA), brokenv6 (AAAA = a blackholed address, A works: happy-eyeballs fallback to
          IPv4), brokenv4 (A blackholed, AAAA works), and the literal [2001:db8:5::80].
  v6only  the same fixture without any IPv4 (no DHCPv4 server): RA with the O flag and no RDNSS, DNS from stateless
          DHCPv6 (2001:db8:53::53) only.
  slirp   QEMU's user network with ipv4=off,ipv6=on (SLAAC fec0::/64, slirp's RDNSS fec0::3): pages from this
          process's HTTP server on [::1]:PORT+2 reached as http://[fec0::2]:PORT+2/.
Checks per browser: the start page, an AAAA-only name, both broken-path names and the literal come over the expected
family (the blackholed address must have seen the SYN first: the fallback really happened), the page's script
reports every image loaded. Wine: tests/wine/winsock6.c (adapters with an IPv6 address and DNS server, getaddrinfo
order, HTTP over v6/v4, connect-loop fallback, IPV6_V6ONLY default, dual-stack listener reached over v6 and v4,
UDP echo). resolv.conf is checked against the variant's DNS source. No kernel fault anywhere."""
from pathlib import Path
import argparse
import http.server
import json
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import netfix  # noqa: E402
import x64_overlay  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
CHROME = '/home/nexus/Applications/chrome/opt/google/chrome/chrome'
FIREFOX = '/opt/firefox/firefox-bin'

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--disk', type=Path, default=Path(r'D:\nexustest\prod\integ_desk_disk\desktop64.img'))
ap.add_argument('--variants', default='dual,v6only,slirp')
ap.add_argument('--apps', default='chrome,firefox,wine')
ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4721)
ap.add_argument('--port', type=int, default=9120, help='fixture UDP port; +1 QEMU dgram; +2 slirp-variant HTTP server')
ap.add_argument('--tcg', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
VARIANTS = [v for v in args.variants.split(',') if v]
APPS = [a for a in args.apps.split(',') if a]
assert set(VARIANTS) <= {'dual', 'v6only', 'slirp'} and set(APPS) <= {'chrome', 'firefox', 'wine'}, (VARIANTS, APPS)
FIXTURE_PORT, QEMU_PORT, SLIRP_HTTP = args.port, args.port + 1, args.port + 2
GUEST_MAC = bytes.fromhex('525400123456')
SERVER6, SERVER4, DEAD6, DEAD4 = '2001:db8:5::80', '192.0.2.80', '2001:db8:5::dead', '192.0.2.99'
results = []


def note(ok, what, detail=''):
    results.append((bool(ok), what, str(detail)[:600]))
    print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '  (%s)' % str(detail)[:600]), flush=True)
    return bool(ok)


# ---------------------------------------------------------------- the test web site (fixture and slirp variants)
GIF = bytes.fromhex('47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b')
IMAGES = {'dual': ['v6only', 'brokenv6', 'brokenv4', 'literal6'], 'v6only': ['v6only', 'brokenv4', 'literal6', 'dual'], 'slirp': ['literal6']}


def start_page(variant, app, base):
    if variant == 'slirp': sources = {'literal6': base + '/img/literal6.gif'}
    else: sources = {name: ('http://[%s]/img/literal6.gif' % SERVER6 if name == 'literal6' else 'http://%s.nexus.test/img/%s.gif' % (name, name))
                     for name in IMAGES[variant]}
    images = ''.join('<img id="%s" src="%s?app=%s" width=40 height=40>' % (n, s, app) for n, s in sources.items())
    script = ('<script>var left=%d,r=[];function done(n,ok){r.push(n+":"+(ok?1:0));if(--left==0)fetch("/done?app=%s&r="+r.join(","));}'
              '%s</script>' % (len(sources), app, ''.join('document.getElementById("%s").onload=function(){done("%s",1)};'
                                                            'document.getElementById("%s").onerror=function(){done("%s",0)};' % (n, n, n, n) for n in sources)))
    return ('<!doctype html><html><head><title>NexusOS IPv6 %s</title></head><body style="background:#2a7">'
            '<h1>IPv6 %s %s</h1>%s%s</body></html>' % (variant, variant, app, images, script)).encode()


class Site:
    """Request handling shared by the fixture HTTP server and the slirp variant's host server."""
    def __init__(self, variant, base):
        self.variant, self.base, self.done, self.gates, self.lock = variant, base, {}, set(), threading.Lock()
        self.fx = self.link = None

    def handle(self, path, query, family, client, host):
        params = dict(p.split('=', 1) for p in query.split('&') if '=' in p)
        if path == '/start': return 200, {}, start_page(self.variant, params.get('app', '?'), self.base)
        if path.startswith('/img/'): return 200, {'Content-Type': 'image/gif'}, GIF
        if path == '/done':
            with self.lock: self.done[params.get('app', '?')] = params.get('r', ''); self.gates.add(params.get('app', '?') + '-done')
            return 204, {}, b''
        if path.startswith('/gate/'):
            with self.lock: return (200, {}, b'open') if path[6:] in self.gates else (404, {}, b'wait')
        if path.startswith('/ws6/connect-back'):
            port = int(params.get('port', '7090'))
            if self.fx:
                self.fx.connect(self.link, SERVER6, client, port, Hello(b'HELLO6'))
                if self.link.guest4: self.fx.connect(self.link, SERVER4, netfix.t4(self.link.guest4), port, Hello(b'HELLO4'))
            return 200, {}, b'WS6-OK connect-back'
        if path.startswith('/ws6/'): return 200, {}, ('WS6-OK family=%d path=%s' % (family, path)).encode()
        return 404, {}, b'not here'


class Hello:
    def __init__(self, text): self.text, self.echo = text, b''
    def on_open(self, conn): conn.send(self.text)
    def on_data(self, conn, data):
        self.echo += data
        if self.echo == self.text: conn.close()
    def on_eof(self, conn): conn.close()


def fixture(variant, site):
    fx = netfix.Fixture()
    lan = fx.link('lan', FIXTURE_PORT, QEMU_PORT, guest_mac=GUEST_MAC)
    if variant == 'dual':
        lan.server4(dns=())                                  # IPv4 works, but no IPv4 DNS server: names go over IPv6
        lan.router6(rdnss=['2001:db8:1::53'], dnssl=['nexus.test'])
        fx.dns_server('2001:db8:1::53'); lan.onlink6.add(netfix.a6('2001:db8:1::53'))
    else:
        lan.router6(other=True)                              # O flag: DNS from stateless DHCPv6 only
        lan.server6(dns=['2001:db8:53::53'], domains=['nexus.test'])
        fx.dns_server('2001:db8:53::53')
    fx.host(SERVER6, SERVER4)
    fx.blackhole(DEAD6, DEAD4)
    fx.name('dual.nexus.test', a=[SERVER4], aaaa=[SERVER6])
    fx.name('v6only.nexus.test', aaaa=[SERVER6])
    fx.name('brokenv6.nexus.test', a=[SERVER4], aaaa=[DEAD6])
    fx.name('brokenv4.nexus.test', a=[DEAD4], aaaa=[SERVER6])
    site.fx, site.link = fx, lan
    fx.http(80, lambda r: site.handle(r.path, r.query, r.family, r.client, r.host))
    return fx.start(), lan


class SlirpServer:
    """The slirp variant's host side: HTTP on [::1] (slirp maps fec0::2 to the host's loopback)."""
    def __init__(self, site, port):
        hits = self.hits = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a): pass
            def do_GET(self):
                path, _, query = self.path.partition('?')
                family = 4 if self.client_address[0].startswith('::ffff:') or '.' in self.client_address[0] else 6
                hits.append({'t': time.monotonic(), 'path': path, 'query': query, 'family': family, 'client': self.client_address[0],
                             'agent': self.headers.get('user-agent', ''), 'host': self.headers.get('host', '')})
                status, extra, body = site.handle(path, query, family, self.client_address[0], self.headers.get('host', ''))
                self.send_response(status)
                for k, v in extra.items(): self.send_header(k, v)
                self.send_header('Content-Length', str(len(body))); self.send_header('Connection', 'close'); self.end_headers()
                self.wfile.write(body)
        class Server(http.server.ThreadingHTTPServer): address_family = socket.AF_INET6
        self.server = Server(('::1', port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self): self.server.shutdown(); self.server.server_close()


# ---------------------------------------------------------------- guest script
def q(word):
    text = str(word).replace('\\', '\\\\').replace('"', '\\"')
    return '"%s"' % text if not text or any(c in text for c in ' \t\'#') else text


def guest_script(variant, chrome_flags):
    lines = []
    def cmd(*words): lines.append(' '.join(map(q, words)))
    for name, value in (('HOME', '/home/nexus'), ('USER', 'nexus'), ('LOGNAME', 'nexus'), ('PATH', '/usr/bin:/bin'),
                        ('LANG', 'C.UTF-8'), ('WINEDEBUG', 'err+all,fixme-all')):
        cmd('env', '%s=%s' % (name, value))
    if variant == 'slirp': cmd('host', str(SLIRP_HTTP), 'fec0::2'); base = 'http://[fec0::2]:%d' % SLIRP_HTTP
    else: cmd('host', '80', SERVER6); base = 'http://dual.nexus.test'
    cmd('cd', '/home/nexus')
    cmd('sleep', '8000')                                     # SLAAC, DAD and DHCPv6 settle
    cmd('mark', 'PHASE net'); cmd('cat', '/etc/resolv.conf'); cmd('cat', '/proc/net/if_inet6'); cmd('cat', '/proc/net/ipv6_route')
    if 'chrome' in APPS:
        cmd('mark', 'PHASE chrome')
        cmd('bg', 'chrome', CHROME, *chrome_flags, '--user-data-dir=/tmp/nx-v6-chrome', base + '/start?app=chrome')
        cmd('gate', 'chrome-done', '300'); cmd('kill', 'chrome'); cmd('procwait', '/opt/google/chrome/chrome', '60')
    if 'firefox' in APPS:
        cmd('mark', 'PHASE firefox')
        cmd('env', 'MOZ_ENABLE_WAYLAND=1'); cmd('env', 'MOZ_CRASHREPORTER_DISABLE=1')
        cmd('bg', 'firefox', FIREFOX, '--profile', '/home/nexus/nx-ff6', '--no-remote', base + '/start?app=firefox')
        cmd('gate', 'firefox-done', '360'); cmd('kill', 'firefox'); cmd('procwait', 'firefox-bin', '60')
    if 'wine' in APPS:
        cmd('mark', 'PHASE wine')
        cmd('run', 'wineboot', '1800', '/usr/bin/wineboot', '--init')
        if variant == 'slirp': cmd('run', 'ws6', '300', '/usr/bin/wine', '/home/nexus/wt/winsock6.exe', 'slirp', 'fec0::2', str(SLIRP_HTTP))
        else: cmd('run', 'ws6', '300', '/usr/bin/wine', '/home/nexus/wt/winsock6.exe', variant, 'dual.nexus.test', '80')
    return '\n'.join(lines) + '\n'


# Firefox profile: no first-run pages and no connectivity/captive-portal probes (they need the internet, which this
# isolated network does not have; Firefox would otherwise mark IPv6 as unavailable). IPv6 settings stay default.
FIREFOX_PREFS = '\n'.join('user_pref("%s", %s);' % kv for kv in (
    ('browser.shell.checkDefaultBrowser', 'false'), ('browser.aboutwelcome.enabled', 'false'),
    ('datareporting.policy.dataSubmissionEnabled', 'false'), ('toolkit.telemetry.reportingpolicy.firstRun', 'false'),
    ('browser.startup.homepage_override.mstone', '"ignore"'), ('network.captive-portal-service.enabled', 'false'),
    ('network.connectivity-service.enabled', 'false'), ('app.normandy.enabled', 'false'),
    ('browser.safebrowsing.update.enabled', 'false'), ('extensions.update.enabled', 'false'))) + '\n'


def boot(variant, overlay, serial):
    accel = ['-accel', 'tcg', '-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
    if variant == 'slirp': net = ['-netdev', 'user,id=n0,ipv4=off,ipv6=on']
    else: net = ['-netdev', 'dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=%d,remote.type=inet,remote.host=127.0.0.1,'
                              'remote.port=%d' % (QEMU_PORT, FIXTURE_PORT)]
    command = [QEMU, *accel, '-smp', str(args.smp), '-m', '3072', '-machine', 'q35',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'if=none,id=root,file={overlay},format=qcow2', '-device', 'nvme,serial=nexus-root,drive=root',
               '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga', '-display', 'none',
               *net, '-device', 'e1000e,netdev=n0,mac=52:54:00:12:34:56',
               '-serial', f'file:{serial}', '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-no-reboot',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    proc = subprocess.Popen(command, stdout=(WORK / f'{variant}-qemu.log').open('wb'), stderr=subprocess.STDOUT)
    try:
        hostqos = x64_overlay._load('ipv6apps_hostqos', Path(r'C:\nexustest\fix\core_harness\hostqos.py'))
        hostqos.unthrottle(proc.pid)
    except Exception: pass
    print(f'[{variant}] VM pid {proc.pid}', flush=True)
    return proc


def stop(proc):
    if proc.poll() is None:
        try:
            with socket.create_connection(('127.0.0.1', args.qmp_port), timeout=10) as s:
                f = s.makefile('rw'); f.readline()
                for request in ({'execute': 'qmp_capabilities'}, {'execute': 'quit'}):
                    f.write(json.dumps(request) + '\n'); f.flush(); f.readline()
        except Exception: pass
        try: proc.wait(30)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()


def runner_lines(log, tag):
    return [m.group(1) for m in re.finditer(r'\[64\] WR %s\| ([^\n]*)' % re.escape(tag), log)]


# ---------------------------------------------------------------- checks
def check_browser(variant, app, hits, events, done):
    """hits: http requests [{'family', 'host', 'path', 'query'}] of this app; events: fixture events (blackhole SYNs)."""
    mine = [h for h in hits if 'app=' + app in h.get('query', '') or (h['path'] == '/start' and 'app=' + app in h.get('query', ''))]
    def fam(host_part, path_part):
        found = [h for h in mine if host_part in h['host'] and path_part in h['path']]
        return found[0]['family'] if found else None
    if variant == 'slirp':
        note(fam('fec0::2', '/start') == 6, f'{variant} {app}: start page http://[fec0::2]:{SLIRP_HTTP}/ over IPv6 (QEMU user net, v6-only)',
             [(h['host'], h['path'], h['family']) for h in mine])
        note(fam('fec0::2', 'literal6') == 6, f'{variant} {app}: image from the IPv6 literal', [(h['host'], h['path']) for h in mine])
    else:
        note(fam('dual.nexus.test', '/start') == 6, f'{variant} {app}: dual-stack name -> IPv6 preferred (start page)',
             [(h['host'], h['path'], h['family']) for h in mine])
        note(fam('v6only.nexus.test', 'v6only') == 6, f'{variant} {app}: AAAA-only name over IPv6', [(h['host'], h['family']) for h in mine])
        note(fam('brokenv4.nexus.test', 'brokenv4') == 6, f'{variant} {app}: name with a dead IPv4 path loads over IPv6',
             [(h['host'], h['family']) for h in mine])
        note(fam('2001:db8:5::80', 'literal6') == 6, f'{variant} {app}: [2001:db8:5::80] literal over IPv6', [(h['host'], h['family']) for h in mine])
        if variant == 'dual':
            got = [h for h in mine if 'brokenv6.nexus.test' in h['host']]
            begin = min([h['t'] for h in mine if h['path'] == '/start'] or [0])   # this app's attempts only
            dead = [e for e in events if e['kind'] == 'blackhole' and e['dst'] == DEAD6 and e['proto'] == 6 and e['t'] >= begin]
            ok = got and got[0]['family'] == 4 and dead and dead[0]['t'] <= got[0]['t']
            note(ok, f'{variant} {app}: happy eyeballs - dead IPv6 path tried, then IPv4 '
                     f'({(got[0]["t"] - dead[0]["t"]) * 1000:.0f} ms after the first dead SYN)' if ok else
                     f'{variant} {app}: happy eyeballs - dead IPv6 path tried, then IPv4', (got and got[0]['family'], len(dead)))
        if variant == 'v6only':
            note(fam('dual.nexus.test', 'dual') == 6, f'{variant} {app}: dual-stack name on an IPv6-only network over IPv6',
                 [(h['host'], h['family']) for h in mine])
    report = done.get(app, '')
    loaded = dict(x.split(':') for x in report.split(',') if ':' in x)
    note(loaded and all(v == '1' for v in loaded.values()) and set(loaded) == set(IMAGES[variant]),
         f'{variant} {app}: the page script saw every image load', report)


def run_variant(variant, programs, chrome_flags):
    site = Site(variant, 'http://[fec0::2]:%d' % SLIRP_HTTP if variant == 'slirp' else 'http://dual.nexus.test')
    fx = server = proc = None
    overlay = WORK / f'{variant}-overlay.qcow2'; serial = WORK / f'{variant}-serial.log'; serial.unlink(missing_ok=True)
    script = guest_script(variant, chrome_flags); (WORK / f'{variant}.script').write_text(script)
    staging = WORK / 'staging'; staging.mkdir(exist_ok=True)
    (staging / 'net6.script').write_text(script); (staging / 'desktop.args').write_text('--aurora\n/usr/bin/nexus-wine-runner\n/nexus/net6.script\n')
    (staging / 'user.js').write_text(FIREFOX_PREFS)
    puts = [(OUT / 'desktop.elf', 'sbin/init'), (OUT / 'aurora_app.elf', 'bin/aurora-app'), (OUT / 'wine_runner_user.elf', 'usr/bin/nexus-wine-runner'),
            (staging / 'net6.script', 'nexus/net6.script'), (staging / 'desktop.args', 'nexus/desktop.args'),
            (staging / 'user.js', 'home/nexus/nx-ff6/user.js'), (programs['winsock6.exe'], 'home/nexus/wt/winsock6.exe')]
    t0 = time.monotonic()
    try:
        x64_overlay.stage(args.disk, overlay, puts)
        if variant == 'slirp': server = SlirpServer(site, SLIRP_HTTP)
        else: fx, lan = fixture(variant, site)
        proc = boot(variant, overlay, serial)
        def text(): return serial.read_text('latin1') if serial.exists() else ''
        deadline = time.monotonic() + 3600
        while time.monotonic() < deadline:
            log = text()
            if re.search(r'\[64\] WR DONE failures=\d+', log) or proc.poll() is not None: break
            time.sleep(2)
        log = text()
    finally:
        if proc: stop(proc)
        if server: server.stop()
        if fx:
            fx.stop()
            with fx.lock:   # evidence: every fixture event (without raw packet bytes)
                (WORK / f'{variant}-events.json').write_text(json.dumps([{k: v for k, v in e.items() if not isinstance(v, bytes)} for e in fx.events], indent=0))
        overlay.unlink(missing_ok=True)
    took = time.monotonic() - t0
    print(f'[{variant}] boot finished after {took:.0f} s', flush=True)
    note('KERNEL_FAULT' not in log and 'uncontained kernel exception' not in log, f'{variant}: no kernel fault',
         [l for l in log.splitlines() if 'KERNEL_FAULT' in l][:3])
    note(re.search(r'\[64\] WR DONE failures=\d+', log), f'{variant}: guest script finished', log[-1500:])
    resolv = [l for l in runner_lines(log, 'cat') if l.startswith(('nameserver', 'search'))]
    servers = [l.split()[1] for l in resolv if l.startswith('nameserver')]
    want = {'dual': ['2001:db8:1::53'], 'v6only': ['2001:db8:53::53'], 'slirp': ['fec0::3']}[variant]
    note(servers == want, f'{variant}: resolv.conf nameservers {want} ({"RA RDNSS" if variant != "v6only" else "DHCPv6"})', resolv)
    if variant != 'slirp':
        hits = [e for e in fx.events if e['kind'] == 'http']
        dns = [e for e in fx.events if e['kind'] == 'dns']
        aaaa = [e for e in dns if e['name'] == 'dual.nexus.test' and e['qtype'] == 28]
        note(aaaa and all(e['family'] == 6 for e in dns), f'{variant}: DNS (AAAA and A) queried over IPv6 transport ({len(dns)} queries)',
             sorted({(e['family'], e['server']) for e in dns}))
        events = fx.events
        if fx.errors: note(False, f'{variant}: fixture errors', fx.errors[:2])
    else:
        hits = server.hits; events = []
    for app in ('chrome', 'firefox'):
        if app in APPS:
            gate = re.search(r'\[64\] WR GATE %s-done (\w+)' % app, log)
            note(gate and gate.group(1) == 'open', f'{variant} {app}: page finished (gate)', gate and gate.group(0))
            check_browser(variant, app, hits, events, site.done)
    if 'wine' in APPS:
        ws6 = runner_lines(log, 'ws6')
        summary = [l for l in ws6 if l.startswith('WS6 SUMMARY')]
        for line in ws6:
            if line.startswith(('WS6 FAIL', 'WS6 INFO')): print('     wine:', line[:300], flush=True)
        passed = re.search(r'pass=(\d+) fail=(\d+)', summary[0]) if summary else None
        note(passed and passed.group(2) == '0' and int(passed.group(1)) >= 8, f'{variant} wine: winsock6.exe {summary[0] if summary else "no summary"}',
             [l for l in ws6 if 'FAIL' in l][:6] or log[-800:])
    faults = sorted({m.group(0)[:120] for m in re.finditer(r'USER_FAULT[^\n]*', log)})
    if faults: print(f'[{variant}] user faults (informational):', faults[:6], flush=True)


def main():
    for need in ('desktop.elf', 'aurora_app.elf', 'wine_runner_user.elf'):
        assert (OUT / need).exists(), f'build first: {OUT / need} (build_desktop.py)'
    (WORK / 'bin').mkdir(exist_ok=True)
    exe = WORK / 'bin' / 'winsock6.exe'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/wine/winsock6.c'),
                    '-lws2_32', '-liphlpapi', '-o', str(exe)], check=True)
    chrome_args = (x64_overlay.read_file(args.disk, 'nexus/chrome-desktop.args') or b'').decode('utf-8', 'replace').splitlines()
    chrome_flags = [l for l in chrome_args[1:] if l.startswith('--') and not l.startswith(('--user-data-dir', '--enable-logging', '--v='))]
    for variant in VARIANTS: run_variant(variant, {'winsock6.exe': exe}, chrome_flags)
    failed = [r for r in results if not r[0]]
    print('SUMMARY ipv6 apps: %d/%d checks passed (variants %s, apps %s, %d CPUs)' % (len(results) - len(failed), len(results),
          ','.join(VARIANTS), ','.join(APPS), args.smp), flush=True)
    for ok, what, detail in failed: print('  FAILED', what, detail[:300])
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
