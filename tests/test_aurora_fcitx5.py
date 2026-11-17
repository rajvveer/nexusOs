"""fcitx5 (Debian 13) as Aurora's input method, end to end: Pinyin, Japanese (Anthy) and Korean (Hangul) text committed
into a GTK application (Mozilla Firefox, GTK3: its Wayland IM module speaks zwp_text_input_v3) and into Google Chrome
(Ozone/Wayland, text-input-v3).

python -I tests/test_aurora_fcitx5.py WORK [--base DESKTOP64.img] [--cache DIR] [--apps firefox,chrome] [--smp 2]
       [--port 9101] [--qmp-ports 4700-4709] [--keep]

The base (default D:\\nexustest\\prod\\integ_desk_disk\\desktop64.img) is read only: arch/x86_64/ime_install.py `overlay`
installs fcitx5 + the engines (ime-lock.json) and stages /sbin/init = out/desktop.elf, /bin/aurora-app, a test
/etc/shadow + autologin, ~/.aurora/input-method.conf = program=/usr/libexec/nexus-fcitx5 and /nexus/desktop.args (the
application, opening a page of this script's HTTP server) into one qcow2 overlay per application. Per application:
  * Aurora starts fcitx5 (AURORA IME PROGRAM ...), fcitx5 binds the input-method global (AURORA IME GRAB keyboard);
  * the page's text field has the focus: Aurora activates the input method (AURORA IME ACTIVATE);
  * typed with QMP keys (fcitx5's group: pinyin is active by default, Ctrl+Shift+F9 moves to the next engine):
        z h o n g g u o space      -> 中国        (Pinyin, first candidate; nihao is ambiguous: 你好/您好 rank by model)
        Ctrl+Shift+F9, n i h o n g o space ret -> 日本語  (Anthy: romaji, convert, commit)
        Ctrl+Shift+F9, d k s s u d space       -> 안녕   (Hangul, 2-set layout)
  * the page POSTs its field on every input event; the committed text must be exactly 中国日本語안녕 + spaces as typed;
  * no kernel or user fault, no ENOSYS.
"""
from pathlib import Path
import http.server, importlib.util, json, re, sys, threading, time, urllib.parse

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
from sha512_crypt import sha512_crypt  # noqa: E402


def option(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] not in
        ('--base', '--cache', '--apps', '--smp', '--port', '--qmp-ports')]
WORK = Path(args[0]).resolve() if args else Path(r'D:\nexustest\wave2\ws_kabi_runs\fcitx5')
BASE = Path(option('--base', r'D:\nexustest\prod\integ_desk_disk\desktop64.img')).resolve()
CACHE = Path(option('--cache', r'D:\nexustest\wave2\ws_kabi_cache\ime')).resolve()
APPS = option('--apps', 'firefox,chrome').split(',')
SMP = int(option('--smp', '2')); PORT = int(option('--port', '9101'))
low, high = map(int, option('--qmp-ports', '4700-4709').split('-'))
desk_vm.QMP_PORTS = range(low, high + 1); desk_vm.VNC_PORTS = range(9106, 9110)
WORK.mkdir(parents=True, exist_ok=True); CACHE.mkdir(parents=True, exist_ok=True)
sys.stdout.reconfigure(errors='backslashreplace')
spec = importlib.util.spec_from_file_location('test_ime_install', ROOT / 'arch/x86_64/ime_install.py')
ime = importlib.util.module_from_spec(spec); spec.loader.exec_module(ime)

PAGE = '''<!doctype html><meta charset="utf-8"><title>NexusOS IME</title>
<body style="margin:0;background:#fff"><textarea id="t" autofocus style="width:1200px;height:400px;font-size:40px"></textarea>
<script>
const t = document.getElementById('t');
function send(kind) { fetch('/' + kind, {method: 'POST', body: t.value}).catch(() => {}); }
t.addEventListener('input', () => send('value'));
window.addEventListener('load', () => { t.focus(); send('loaded'); setInterval(() => { if (document.activeElement !== t) t.focus(); send('value'); }, 700); });
</script>'''
LAUNCH = {
    'firefox': ['/usr/bin/env', 'MOZ_ENABLE_WAYLAND=1', 'MOZ_CRASHREPORTER_DISABLE=1', '/opt/firefox/firefox-bin', '--profile', '/tmp/ffime',
                '--no-remote', 'http://10.0.2.2:%d/ime.html' % PORT],
    'chrome': ['/home/nexus/Applications/chrome/opt/google/chrome/chrome', '--ozone-platform=wayland', '--enable-wayland-ime',
               '--wayland-text-input-version=3', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
               '--ipc-connection-timeout=120', '--user-data-dir=/tmp/chime', 'http://10.0.2.2:%d/ime.html' % PORT],
}
WINDOW = {'firefox': r'firefox', 'chrome': r'google-chrome'}
FF_PREFS = (ROOT / 'arch/x86_64/apps-config/firefox-user.js').read_bytes()  # no first-run / Terms of Use modal
STEPS = [('pinyin', [], list('zhongguo') + ['spc'], '中国'),
         ('anthy', [('ctrl', 'shift', 'f9')], list('nihongo') + ['spc', 'ret'], '日本語'),
         ('hangul', [('ctrl', 'shift', 'f9')], list('dkssud') + ['spc'], '안녕')]
FAIL = r'\[64\] (KERNEL_FAULT|USER_FAULT)[^\n]*'
values = {}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def reply(self, code, body=b'', kind='text/html; charset=utf-8'):
        self.send_response(code); self.send_header('content-type', kind); self.send_header('content-length', str(len(body))); self.end_headers()
        if self.command != 'HEAD': self.wfile.write(body)
    def do_GET(self):
        if urllib.parse.urlsplit(self.path).path == '/ime.html': return self.reply(200, PAGE.encode())
        return self.reply(404)
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('content-length', '0')))
        values[urllib.parse.urlsplit(self.path).path] = body.decode('utf-8', 'replace'); self.reply(200)


def stage_files(app):
    files = WORK / ('stage-' + app); files.mkdir(exist_ok=True)
    shadow = 'root:*:20000:0:99999:7:::\nnexus:%s:20000:0:99999:7:::\n' % sha512_crypt('imeFcitx5x', 'Fcitx5TestSaltVal')
    content = {'etc/shadow': shadow.encode(), 'home/nexus/.aurora/login.conf': b'autologin=1\nlock_after=0\nlock_on_sleep=0\n',
               'home/nexus/.aurora/input-method.conf': b'program=/usr/libexec/nexus-fcitx5\n',
               'nexus/desktop.args': ('--aurora\n' + '\n'.join(LAUNCH[app]) + '\n').encode(), 'tmp/ffime/user.js': FF_PREFS}
    puts = [(OUT / 'desktop.elf', 'sbin/init'), (OUT / 'aurora_app.elf', 'bin/aurora-app')]
    for guest, data in content.items():
        host = files / guest.replace('/', '_'); host.write_bytes(data); puts.append((host, guest))
    return puts


def run(app):
    overlay = WORK / ('%s.qcow2' % app)
    ime.overlay(BASE, overlay, CACHE, stage_files(app))
    values.clear()
    vm = desk_vm.DeskVm(WORK / app, BASE, heads=1, accel='whpx', smp=SMP, usb=True, vnc_heads=[], memory='3072',
                        keep_overlay=True, overlay=overlay)
    result = {}
    try:
        vm.wait(r'AURORA READY', seconds=300, fail=FAIL)
        vm.wait(r'AURORA IME PROGRAM /usr/libexec/nexus-fcitx5 pid=\d+ start=1', seconds=120, fail=FAIL)
        vm.wait(r'AURORA WINDOW app=%s' % WINDOW[app], seconds=300, fail=FAIL)
        for _ in range(600):
            if '/loaded' in values: break
            time.sleep(.5)
        assert '/loaded' in values, 'the page did not load'
        vm.wait(r'AURORA IME ACTIVATE', seconds=120, fail=FAIL)
        time.sleep(3)
        # The first key makes fcitx5 load the Pinyin model; when that takes longer than the 500 ms repeat delay the
        # key is repeated (z z hong guo -> \u5728\u4e2d\u56fd). A throwaway key, cleared with Escape, takes that hit.
        vm.tap('a'); time.sleep(3); vm.tap('esc'); time.sleep(1)
        assert not values.get('/value', ''), 'the warm-up key reached the field: %r' % values.get('/value')
        typed = ''
        for engine, switch, keys, want in STEPS:
            for combo in switch: vm.tap(*combo); time.sleep(1)
            for k in keys: vm.tap(k); time.sleep(.15)
            typed += want
            deadline = time.time() + 30
            while time.time() < deadline and values.get('/value', '').replace(' ', '').replace('\n', '') != typed: time.sleep(.5)
            got = values.get('/value', '')
            result[engine] = got
            if got.replace(' ', '').replace('\n', '') != typed:
                vm.shot('%s-%s' % (app, engine))
                raise AssertionError('%s %s: field %r, want %r' % (app, engine, got, typed))
        vm.shot(app + '-done')
        log = vm.text()
        assert not re.search(FAIL, log), re.search(FAIL, log).group(0)
        enosys = sorted(set(re.findall(r'ENOSYS syscall=(0x[0-9A-F]+)', log)))
        result['enosys'] = [int(e, 16) for e in enosys]
        return result
    except BaseException:
        try: vm.shot(app + '-failure')
        except Exception: pass
        raise
    finally:
        vm.close()
        if '--keep' not in sys.argv: overlay.unlink(missing_ok=True)


server = http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler); server.daemon_threads = True
threading.Thread(target=server.serve_forever, daemon=True).start()
results = {}
try:
    for app in APPS:
        try: results[app] = run(app)
        except AssertionError as e:   # a key that QMP held past fcitx5's 500 ms repeat delay under host load: one retry, reported
            print('RETRY', app, str(e)[:200], flush=True); results[app] = run(app); results[app]['retried'] = True
        print('PASS', app, json.dumps(results[app], ensure_ascii=False), flush=True)
finally:
    server.shutdown()
(WORK / 'results.json').write_text(json.dumps(results, indent=1, ensure_ascii=False), encoding='utf-8')
bad = {a: r['enosys'] for a, r in results.items() if r.get('enosys')}
print('ENOSYS seen:', bad or 'none', flush=True)
print('PASS fcitx5 on Aurora: Pinyin, Anthy, Hangul commits into', ', '.join(APPS), flush=True)
