"""Wine Mono (.NET Framework) and Wine Gecko (MSHTML) on NexusOS: a new prefix gets both from /usr/share/wine without a
download prompt, and real programs use them.

    python -I tests/test_wine_dotnet.py WORK [--disk DESKTOP64.img] [--addons-cache DIR] [--apps-cache DIR]
           [--accel auto|whpx|tcg] [--qmp-port 4673] [--http-port 8973]

Disk: --disk (default: --source) is a FAT32 desktop disk that has Wine 10.0 (wine_install.py); it is never written and
never copied. Everything the test needs - the addons (wine-addons-lock.json: Wine Mono 9.4.0, Wine Gecko 2.47.4 x86 +
x86_64, into /usr/share/wine as wine_install.py --only addons puts them), this tree's winewayland.so and HTA host
(winewayland_build.py), Aurora and the programs - goes into a qcow2 overlay (tests/x64_overlay.py) that is deleted at
the end. Programs: tests/wine/dotnet-lock.json (NuGet 6.11.1 nuget.exe, KeePass 2.57 portable), downloaded once from
the publishers and verified by size + SHA-256 every time.

One boot runs a script with nexus-wine-runner (as tests/test_wine.py does); at each gate the test drives the desktop:
  prefix   wineboot --init installs Mono and Gecko from the shared directory (C:\\windows\\mono, system32 and syswow64
           gecko) and no "Wine Mono/Gecko Installer" window appears
  dotnet   nuget.exe help (.NET Framework 4.7.2 console program) and nuget locals all -list; KeePass 2.57 (WinForms): its
           main window appears and is drawn, closes with Alt+F4, the process exits 0 and saved its configuration (.NET XML)
  mshtml   Gecko is installed from /usr/share/wine/gecko at the first MSHTML use; hh.exe shows KeePass.chm (HTML Help
           renders with MSHTML); iexplore loads a page from the host whose JavaScript (DOM, JSON, layout, XMLHttpRequest)
           reports to the host; the 32-bit hh.exe (syswow64, a 32-bit MSHTML program) shows KeePass.chm too and
           installs the x86 Gecko into syswow64
  hta      NexusOS's HTA host (arch/x86_64/wine_mshta.c; Wine 10.0's own mshta is a stub), 64-bit and 32-bit: the HTA's
           JScript builds the DOM, reports over XMLHttpRequest (with navigator.platform: Win64 / Win32) and closes itself
           with window.close(); mshta then exits 0. Its attempt at Scripting.FileSystemObject is recorded only (Wine's
           mshtml refuses objects that are not safe for scripting, also in an HTA)
Result: WORK/result.json, screenshots WORK/*.png, guest log WORK/serial.log. Ports: QMP --qmp-port, HTTP --http-port.
"""
from collections import Counter
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import time
import urllib.request
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402
import pkg_vm  # noqa: E402
import wine_host  # noqa: E402
import x64_overlay  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--disk', type=Path, help='a desktop disk with Wine (read only; default --source)')
ap.add_argument('--addons-cache', type=Path, default=nexus_paths.file('NEXUS_WINE_ADDONS_CACHE', ROOT / 'arch/x86_64/out/wine-addons'))
ap.add_argument('--apps-cache', type=Path, default=nexus_paths.file('NEXUS_WINE_DOTNET_CACHE', ROOT / 'arch/x86_64/out/wine-dotnet'))
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4673); ap.add_argument('--http-port', type=int, default=8973)
ap.add_argument('--hta-only', action='store_true', help='only the prefix and the HTA phase (a quick run while working on the HTA host)')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
OUT = aurora_vm.OUT
LOCK = ROOT / 'tests/wine/dotnet-lock.json'
ORIGINS = ('https://dist.nuget.org/', 'https://downloads.sourceforge.net/project/keepass/')
WINE = '/usr/bin/wine'
APPS = '/home/nexus/apps'
started = time.time(); checks = []
result = {'test': 'wine-dotnet', 'passed': False, 'checks': checks}


def check(phase, name, ok, detail=''):
    checks.append({'phase': phase, 'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %-7s %s%s' % ('PASS' if ok else 'FAIL', phase, name, '' if ok else '  (%s)' % str(detail)[:300]), flush=True)
    return bool(ok)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(1 << 20): digest.update(block)
    return digest.hexdigest()


def fetch_programs(cache):
    """{name: path} of the pinned programs; a download must start at the publisher and match the pinned size + SHA-256
    (SourceForge redirects to a mirror: the digest is what makes the file trusted)."""
    cache.mkdir(parents=True, exist_ok=True); found = {}
    for entry in json.loads(LOCK.read_text())['apps']:
        path = cache / entry['name']
        if not (path.exists() and path.stat().st_size == entry['bytes'] and sha256(path) == entry['sha256']):
            if not entry['url'].startswith(ORIGINS): raise SystemExit('download origin not allowed: ' + entry['url'])
            part = path.with_name(path.name + '.part'); received = 0
            request = urllib.request.Request(entry['url'], headers={'User-Agent': 'NexusOS-wine-test/1'})
            with urllib.request.urlopen(request, timeout=300) as response, open(part, 'wb') as out:
                while block := response.read(1 << 20):
                    received += len(block)
                    if received > entry['bytes']: raise SystemExit('oversized download: ' + entry['name'])
                    out.write(block)
            if part.stat().st_size != entry['bytes'] or sha256(part) != entry['sha256']:
                part.unlink(); raise SystemExit('verification failed for ' + entry['name'])
            part.replace(path)
        found[entry['name']] = path
    return found


def unzip(archive, target):
    """Members of a zip as {relative posix path: host path}; names are validated (no absolute paths, no '..')."""
    files = {}
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            parts = PurePosixPath(name).parts
            if info.is_dir(): continue
            if not parts or name.startswith('/') or any(p in ('', '.', '..') or ':' in p for p in parts) or info.file_size > 64 << 20:
                raise SystemExit('unsafe member in %s: %r' % (archive.name, info.filename))
            host = target.joinpath(*parts); host.parent.mkdir(parents=True, exist_ok=True)
            host.write_bytes(z.read(info)); files['/'.join(parts)] = host
    return files


# KeePass: no first-start question about update checks (it would open a second window titled KeePass)
KEEPASS_CONFIG = '''<?xml version="1.0" encoding="utf-8"?>
<Configuration xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
\t<Application>
\t\t<Start>
\t\t\t<CheckForUpdate>false</CheckForUpdate>
\t\t\t<CheckForUpdateConfigured>true</CheckForUpdateConfigured>
\t\t</Start>
\t</Application>
</Configuration>
'''.replace('\n', '\r\n')

# An HTA run by the 64-bit (system32) and the 32-bit (syswow64) mshta: its script builds the DOM, reports to the host
# with XMLHttpRequest (standards mode) and closes the application; a FileSystemObject report is tried too (evidence).
HTA = '''<!DOCTYPE html>
<html><head><meta http-equiv="X-UA-Compatible" content="IE=edge"><title>NexusOS HTA test @BITS@</title>
<hta:application id="nexus" applicationname="NexusOSTest@BITS@" showintaskbar="yes" />
<script type="text/javascript">
function run() {
  document.title = 'NexusOS HTA test @BITS@ (onload)';
  var n = 0; for (var i = 1; i <= 100; i++) n += i;
  var p = document.createElement('p'); p.id = 'made'; p.appendChild(document.createTextNode('made by script')); document.body.appendChild(p);
  var text = 'engine=hta bits=@BITS@ sum=' + n + ' dom=' + document.getElementById('made').innerHTML + ' mode=' + document.documentMode;
  text += ' platform=' + navigator.platform + ' ua=' + navigator.userAgent;
  try {
    var f = new ActiveXObject('Scripting.FileSystemObject').CreateTextFile('C:\\\\users\\\\nexus\\\\hta-report-@BITS@.txt', true);
    f.WriteLine(text); f.Close();
    text += ' fso=yes';
  } catch (e) { text += ' fso=refused'; }
  try {
    var x = window.XMLHttpRequest ? new XMLHttpRequest() : new ActiveXObject('Microsoft.XMLHTTP');
    x.open('POST', 'http://10.0.2.2:@PORT@/upload/mshtml-hta-@BITS@.txt', false); x.send(text);
  } catch (e) { document.title = 'xhr failed ' + e.message; }
  window.close();
}
</script></head><body onload="run()"><p>NexusOS HTA @BITS@</p>
<script type="text/javascript">document.title = 'NexusOS HTA test @BITS@ (scripts run)';</script></body></html>
'''.replace('\n', '\r\n')


def hta(bits): return HTA.replace('@BITS@', bits).replace('@PORT@', str(args.http_port))


def addon_files(cache):
    """[(guest path, host file)] of Wine Mono/Gecko as wine_install.py --only addons puts them (verified in cache)."""
    spec = importlib.util.spec_from_file_location('dotnet_wine_install', ROOT / 'arch/x86_64/wine_install.py')
    wi = importlib.util.module_from_spec(spec); spec.loader.exec_module(wi)
    out = []
    for guest, data in wi.fetch_addons(cache): out.append((guest, cache / PurePosixPath(guest).name))
    return out


PAGE = '''<!DOCTYPE html>
<html><head><meta http-equiv="X-UA-Compatible" content="IE=edge"><meta charset="utf-8"><title>NexusOS MSHTML test</title>
<style>body { font: 24px sans-serif; background: #ffffff; } #box { width: 300px; height: 120px; background: #1a66e6; color: #ffffff; }</style>
</head><body><h1>NexusOS MSHTML</h1><div id="box">waiting</div><ul id="list"></ul>
<script>
var list = document.getElementById('list');
for (var i = 1; i <= 5; i++) { var li = document.createElement('li'); li.appendChild(document.createTextNode('item ' + i)); list.appendChild(li); }
var items = document.querySelectorAll('#list li').length;
var squares = []; for (var j = 0; j < 10; j++) squares.push(j * j);
var report = { engine: 'iexplore', mode: document.documentMode, items: items, json: JSON.parse(JSON.stringify({ v: squares })).v.join(','),
               width: document.getElementById('box').offsetWidth, ua: navigator.userAgent };
var x = new XMLHttpRequest();
x.open('POST', '/upload/mshtml-ie.txt', false); x.setRequestHeader('Content-Type', 'application/json'); x.send(JSON.stringify(report));
document.getElementById('box').innerHTML = 'reported ' + x.status;
document.title = 'NexusOS MSHTML done';
</script></body></html>
'''


# ------------------------------------------------------------------ guest script (nexus-wine-runner syntax, see test_wine.py)
def q(argument):
    text = str(argument).replace('\\', '\\\\').replace('"', '\\"')
    return '"%s"' % text if not text or any(ch in text for ch in ' \t\'#') else text


def z(path): return 'Z:' + path.replace('/', '\\')


def hta_phase(cmd):
    """the HTA host, 64-bit (system32) then 32-bit (syswow64): each HTA reports to the host and closes itself"""
    cmd('mark', 'PHASE hta')
    cmd('bg', 'hta', WINE, 'mshta.exe', z(APPS + '/nexus-test64.hta')); cmd('gate', 'hta', '900'); cmd('wait', 'hta', '120')
    cmd('bg', 'hta32', WINE, r'C:\windows\syswow64\mshta.exe', z(APPS + '/nexus-test32.hta')); cmd('gate', 'hta32', '900'); cmd('wait', 'hta32', '120')
    cmd('cat', '/home/nexus/.wine/drive_c/users/nexus/hta-report-64.txt'); cmd('cat', '/home/nexus/.wine/drive_c/users/nexus/hta-report-32.txt')


def guest_script():
    lines = []
    def cmd(*words): lines.append(' '.join([words[0], *map(q, words[1:])]))
    for name, value in (('HOME', '/home/nexus'), ('USER', 'nexus'), ('LOGNAME', 'nexus'), ('PATH', '/usr/bin:/bin'), ('LANG', 'C.UTF-8'),
                        ('WINEDEBUG', 'err+all,fixme-all')):
        cmd('env', '%s=%s' % (name, value))
    cmd('host', str(args.http_port)); cmd('cd', '/home/nexus')
    cmd('mark', 'PHASE prefix')
    cmd('run', 'boot', '2400', '/usr/bin/wineboot', '--init')
    cmd('tree', '/home/nexus/.wine/drive_c/windows/mono', '2')
    cmd('exists', '/home/nexus/.wine/drive_c/windows/mono/mono-2.0/lib/mono/4.5/mscorlib.dll')
    cmd('tree', '/home/nexus/.wine/drive_c/windows/system32/gecko', '3'); cmd('tree', '/home/nexus/.wine/drive_c/windows/syswow64/gecko', '3')
    cmd('run', 'nocrashdlg', '300', WINE, 'reg', 'add', r'HKCU\Software\Wine\WineDbg', '/v', 'ShowCrashDialog', '/t', 'REG_DWORD', '/d', '0', '/f')
    cmd('run', 'netreg', '300', WINE, 'reg', 'query', r'HKLM\Software\Microsoft\NET Framework Setup\NDP\v4\Full')
    if args.hta_only:
        hta_phase(cmd); cmd('mark', 'PHASE cleanup'); cmd('procwait', '.exe', '120'); cmd('procwait', 'wineserver', '60'); cmd('sync')
        return '\n'.join(lines) + '\n'
    cmd('mark', 'PHASE dotnet')
    cmd('run', 'nuget', '900', WINE, APPS + '/nuget.exe', 'help')
    cmd('run', 'nugetlocals', '900', WINE, APPS + '/nuget.exe', 'locals', 'all', '-list')
    cmd('bg', 'keepass', WINE, APPS + '/KeePass/KeePass.exe'); cmd('gate', 'keepass', '1200'); cmd('wait', 'keepass', '180')
    cmd('exists', '/home/nexus/.wine/drive_c/users/nexus/AppData/Roaming/KeePass/KeePass.config.xml')
    cmd('mark', 'PHASE mshtml')
    cmd('bg', 'chm', WINE, 'hh.exe', z(APPS + '/KeePass/KeePass.chm')); cmd('gate', 'chm', '900'); cmd('wait', 'chm', '120')
    # a 32-bit MSHTML program: hh.exe from syswow64 (its first MSHTML use installs the x86 Gecko into syswow64)
    cmd('bg', 'chm32', WINE, r'C:\windows\syswow64\hh.exe', z(APPS + '/KeePass/KeePass.chm')); cmd('gate', 'chm32', '900'); cmd('wait', 'chm32', '120')
    hta_phase(cmd)
    cmd('bg', 'iexplore', WINE, 'iexplore.exe', 'http://10.0.2.2:%d/file/mshtml.html' % args.http_port)
    cmd('gate', 'iexplore', '900'); cmd('wait', 'iexplore', '120')
    # Gecko is installed into the prefix from /usr/share/wine/gecko when MSHTML is first used (no prompt)
    cmd('tree', '/home/nexus/.wine/drive_c/windows/system32/gecko', '2'); cmd('tree', '/home/nexus/.wine/drive_c/windows/syswow64/gecko', '2')
    cmd('ls', APPS + '/KeePass')
    cmd('mark', 'PHASE cleanup')
    cmd('procwait', '.exe', '120'); cmd('procwait', 'wineserver', '60'); cmd('procs', 'wine'); cmd('sync')
    return '\n'.join(lines) + '\n'


# ------------------------------------------------------------------ VM and conductor
class DesktopVm(aurora_vm.Vm):
    """aurora_vm.Vm's input/screendump helpers on the launcher's machine (q35, NVMe root, e1000e)."""
    def __init__(self, work, disk, qmp_port, accel='auto', smp=2, memory='3072', width=1920, height=1080):
        self.work = Path(work); self.serial, self.overlay = self.work / 'serial.log', self.work / 'overlay.qcow2'
        for old in self.work.iterdir():
            if old.is_file() and old.suffix in ('.log', '.qcow2', '.png', '.ppm') and old.name != 'staged.qcow2': old.unlink()
        subprocess.run([aurora_vm.QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(Path(disk).resolve()), '-F', 'qcow2' if Path(disk).suffix == '.qcow2' else 'raw',
                        str(self.overlay)], check=True)
        self.accel = ('whpx' if aurora_vm.whpx_usable() else 'tcg') if accel == 'auto' else accel
        acceleration = ['-accel', 'tcg', '-cpu', 'max'] if self.accel == 'tcg' else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', aurora_vm.CPU]
        command = [aurora_vm.QEMU, *acceleration, '-machine', 'q35', '-smp', str(smp), '-m', memory,
                   '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
                   '-drive', f'if=none,id=root,file={self.overlay},format=qcow2', '-device', 'nvme,serial=nexus-root,drive=root',
                   '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network', '-vga', 'none', '-device', 'virtio-vga', '-display', 'none',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{self.serial}',
                   '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off', '-no-reboot']
        (self.work / 'cmd.json').write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, stdout=(self.work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
        print('VM', self.proc.pid, self.accel, flush=True)
        try: nexus_paths.unthrottle(self.proc.pid)
        except Exception: pass
        self.q = aurora_vm.Qmp(qmp_port); self.pos = [width // 2, height // 2]

    def home(self):
        self.move(-2600, -1600, step=80, delay=0.01); self.pos = [0, 0]; time.sleep(0.3)

    def alt(self, key): self.key('alt', True); self.tap(key); self.key('alt', False)


class Conductor:
    def __init__(self, vm): self.vm = vm; self.mark = 0
    def log(self): return self.vm.text()

    def wait(self, pattern, seconds=60, after=None):
        start = self.mark if after is None else after; end = time.time() + seconds
        while True:
            m = re.search(pattern, self.log()[start:])
            if m or time.time() > end or self.vm.proc.poll() is not None: return m
            time.sleep(0.25)

    def window(self, title, seconds=120, after=None):
        m = self.wait(r'AURORA (?:WINDOW|TITLE) app=(\S*) title=(%s) x=(-?\d+) y=(-?\d+)\r?\n' % title, seconds, after)
        return (m.group(1), m.group(2), int(m.group(3)), int(m.group(4))) if m else None

    def shot(self, name):
        try: return self.vm.shot(name)
        except Exception as error: print('screenshot failed', name, error, flush=True); return None

    def close(self, win, settle=0.8):
        self.vm.move_to(win[2] + 140, win[3] + 12); time.sleep(0.3); self.vm.click(); time.sleep(settle); self.vm.alt('f4')


def client_area(picture, win, top=56, w=392, h=244, step=6):
    x, y = win[2] + 8, win[3] + top
    samples = [picture.getpixel((xx, yy)) for yy in range(max(0, y), min(picture.height, y + h), step)
               for xx in range(max(0, x), min(picture.width, x + w), step)]
    if not samples: return 0, None, 0.0
    background, count = Counter(samples).most_common(1)[0]
    return len(set(samples)), background, 1 - count / len(samples)


def drawn(phase, name, picture, win):
    colours, background, share = client_area(picture, win)
    return check(phase, '%s: the program draws its window (%d colours, %.0f%% off its background)' % (name, colours, 100 * share),
                 colours >= 4 and share >= 0.02 and background is not None and min(background) >= 160,
                 'colours=%d background=%s drawn=%.3f' % (colours, background, share))


def gate_keepass(c):
    win = c.window('KeePass', 600)
    if not check('dotnet', 'KeePass 2.57 (.NET WinForms on Wine Mono): main window appears', win, re.findall(r'AURORA (?:WINDOW|TITLE)[^\n]*', c.log()[c.mark:])[:6]): return
    c.vm.home(); time.sleep(6); picture = c.shot('keepass')
    if picture: drawn('dotnet', 'KeePass', picture, win)
    c.close(win)


def gate_chm(c, bits=''):
    win = c.window(r'[^\n]*KeePass[^\n]*', 300)
    if not check('mshtml', 'hh.exe%s opens KeePass.chm (HTML Help window)' % bits, win, re.findall(r'AURORA (?:WINDOW|TITLE)[^\n]*', c.log()[c.mark:])[:6]): return
    c.vm.home(); time.sleep(8); picture = c.shot('chm' + bits.strip(' ()-bit'))
    if picture:
        # the topic pane (right of the contents tree) is an MSHTML document: dark text on white, many anti-aliased colours
        colours, background, share = client_area(picture, win, top=90, w=700, h=400, step=4)
        check('mshtml', 'the help topic%s is rendered by MSHTML (%d colours, %.0f%% text/graphics)' % (bits, colours, 100 * share),
              colours >= 12 and share >= 0.02 and background is not None and min(background) >= 200, 'colours=%d background=%s drawn=%.3f' % (colours, background, share))
    c.close(win)


def gate_iexplore(c, uploads):
    target = uploads / 'mshtml-ie.txt'; end = time.time() + 300
    while not target.exists() and time.time() < end and c.vm.proc.poll() is None: time.sleep(0.5)
    begin = c.log().rfind('WR START tag=iexplore')   # the window can map (and retitle) before the gate line
    # Wine's iexplore keeps its frame title ("Wine Internet Explorer"); the page's own title is not shown there
    win = c.window(r'Wine Internet Explorer[^\n]*|[^\n]*MSHTML[^\n]*', 120, after=max(0, begin))
    check('mshtml', 'iexplore opens its window', win, re.findall(r'AURORA (?:WINDOW|TITLE)[^\n]*', c.log()[max(0, begin):])[:4])
    if win:
        c.vm.home(); time.sleep(3); picture = c.shot('iexplore')
        if picture:
            blue = sum(1 for yy in range(win[3], min(picture.height, win[3] + 700), 4) for xx in range(win[2], min(picture.width, win[2] + 900), 4)
                       if all(abs(a - b) <= 30 for a, b in zip(picture.getpixel((xx, yy)), (26, 102, 230))))
            check('mshtml', 'the page\'s CSS box is painted (%d blue samples)' % blue, blue >= 100, blue)
        c.close(win)


def gate_hta(c, uploads, bits):
    """The HTA window appears, its report reaches the host and it closes itself; a screenshot either way. A window
    still open after 90 s is closed (the check on mshta's exit code then fails)."""
    tag = 'hta' if bits == '64' else 'hta32'; begin = max(0, c.log().rfind('WR START tag=%s ' % tag))
    win = c.window(r'nexus-test%s\.hta|NexusOS HTA test %s|xhr failed[^\n]*' % (bits, bits), 240, after=begin)
    check('hta', 'mshta (%s-bit) opens the HTA in a window' % bits, win, re.findall(r'AURORA (?:WINDOW|TITLE)[^\n]*', c.log()[begin:])[:4])
    target = uploads / ('mshtml-hta-%s.txt' % bits); end = time.time() + 90
    while time.time() < end and not target.exists() and c.vm.proc.poll() is None: time.sleep(0.5)
    time.sleep(2); c.shot('hta' + bits)
    end = time.time() + 30
    while time.time() < end and not re.search(r'WR END tag=%s status' % tag, c.log()[begin:]): time.sleep(0.5)
    if win and not re.search(r'WR END tag=%s status' % tag, c.log()[begin:]): c.close(win)


# ------------------------------------------------------------------ main
disk = None; servers = vm = None
try:
    t0 = time.time()
    programs = fetch_programs(args.apps_cache)
    keepass = unzip(programs['KeePass-2.57.zip'], WORK / 'keepass')
    base = (args.disk or args.source).resolve()
    nexus_paths.refuse_user_disk(base)
    if x64_overlay.read_file(base, 'usr/bin/wine') is None: raise SystemExit('%s has no Wine (wine_install.py on a copy first)' % base)
    disk = WORK / 'staged.qcow2'   # the overlay over the read-only base: everything below goes in there
    script = guest_script(); (WORK / 'dotnet-test.script').write_text(script)
    files = {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'bin/xdg-open': OUT / 'nexus-installer-ui.elf',
             'usr/bin/nexus-wine-runner': OUT / 'wine_runner_user.elf', 'nexus/dotnet-test.script': script,
             'nexus/desktop.args': '--aurora\n/usr/bin/nexus-wine-runner\n/nexus/dotnet-test.script\n',
             'home/nexus/apps/nuget.exe': programs['nuget.exe'], 'home/nexus/apps/nexus-test64.hta': hta('64'), 'home/nexus/apps/nexus-test32.hta': hta('32'),
             'home/nexus/apps/KeePass/KeePass.config.xml': KEEPASS_CONFIG}
    for name, path in keepass.items(): files['home/nexus/apps/KeePass/' + name] = path
    for guest, host in addon_files(args.addons_cache): files[guest] = host
    spec = importlib.util.spec_from_file_location('dotnet_winewayland', ROOT / 'arch/x86_64/winewayland_build.py')
    ww = importlib.util.module_from_spec(spec); spec.loader.exec_module(ww)
    for host, guest in ww.overlay_files(): files[guest] = host   # this tree's winewayland.so and HTA host (mshta.exe)
    texts = WORK / 'staged-files'; texts.mkdir(exist_ok=True); puts = []
    for i, (guest, value) in enumerate(sorted(files.items())):
        if isinstance(value, (str, bytes)):
            host = texts / ('%03d-%s' % (i, PurePosixPath(guest).name)); host.write_bytes(value.encode() if isinstance(value, str) else value); value = host
        puts.append((value, guest))
    x64_overlay.stage(base, disk, puts)
    result['prepare_seconds'] = round(time.time() - t0, 1)
    www = WORK / 'www'; www.mkdir(exist_ok=True); (www / 'mshtml.html').write_text(PAGE)
    uploads = WORK / 'uploads'
    for old in uploads.glob('*'): old.unlink()
    servers = wine_host.HostServers(args.http_port, args.http_port + 1, www, uploads)
    vm = DesktopVm(WORK, disk, args.qmp_port, args.accel, smp=args.smp)
    result['accel'] = vm.accel
    c = Conductor(vm)
    gates = {'keepass': lambda: gate_keepass(c), 'chm': lambda: gate_chm(c), 'chm32': lambda: gate_chm(c, ' (32-bit)'),
             'iexplore': lambda: gate_iexplore(c, uploads), 'hta': lambda: gate_hta(c, uploads, '64'), 'hta32': lambda: gate_hta(c, uploads, '32')}
    handled = set(); deadline = time.time() + 3600
    vm.wait(r'AURORA READY', seconds=300)
    while time.time() < deadline:
        log = vm.text()
        for m in re.finditer(r'\[64\] WR GATE WAIT (\S+)', log):
            name = m.group(1)
            if name in handled: continue
            handled.add(name); c.mark = m.end(); print('-- gate', name, flush=True)
            try: gates[name]()
            except Exception as error: check('gate', 'gate %s' % name, False, repr(error))
            servers.open_gate(name)
        if re.search(r'\[64\] WR DONE failures=\d+', log) or vm.proc.poll() is not None: break
        time.sleep(0.5)
    log = vm.text()
    done = re.search(r'\[64\] WR DONE failures=(\d+)', log)
    check('run', 'guest script finished', done, log[-400:])

    def end(tag):
        m = re.search(r'\[64\] WR END tag=%s status=(\S+) ms=(\d+)' % re.escape(tag), log); return (m.group(1), int(m.group(2))) if m else (None, None)

    def output(tag): return '\n'.join(re.findall(r'\[64\] WR %s\| ([^\n]*)' % re.escape(tag), log))
    status, ms = end('boot'); result['wineboot_seconds'] = round((ms or 0) / 1000, 1)
    check('prefix', 'wineboot --init creates the prefix (%.0f s)' % ((ms or 0) / 1000), status == '0', status)
    check('prefix', 'Wine Mono is installed into the new prefix (C:\\windows\\mono\\mono-2.0, mscorlib 4.5)',
          'EXISTS path=/home/nexus/.wine/drive_c/windows/mono/mono-2.0/lib/mono/4.5/mscorlib.dll yes' in log, re.findall(r'WR ls\| [^\n]*mono[^\n]*', log)[:5])
    for folder in ('system32', 'syswow64'):   # system32: hh.exe and iexplore (64-bit); syswow64: the 32-bit hh.exe
        check('mshtml', 'Wine Gecko 2.47.4 was installed for %s from /usr/share/wine/gecko at the first MSHTML use' % folder,
              re.search(r'WR ls\| D +\d+ /home/nexus/\.wine/drive_c/windows/%s/gecko/2\.47\.4' % folder, log),
              re.findall(r'WR ls\| [^\n]*gecko[^\n]*', log)[:6])
    titles = re.findall(r'AURORA (?:WINDOW|TITLE) app=\S* title=([^\n]*?) x=', log)
    check('prefix', 'no Wine Mono/Gecko download prompt appeared', not any('Installer' in t and ('Mono' in t or 'Gecko' in t) for t in titles), titles[:12])
    result['netfx_registry'] = output('netreg')[-600:]
    check('dotnet', 'nuget.exe help runs on Wine Mono (NuGet 6.11.1)', end('nuget')[0] == '0' and re.search(r'NuGet Version: 6\.11\.1', output('nuget')),
          output('nuget')[-400:])
    check('dotnet', 'nuget locals all -list prints its cache folders', end('nugetlocals')[0] == '0' and re.search(r'global-packages: ', output('nugetlocals')),
          output('nugetlocals')[-400:])
    check('dotnet', 'KeePass exits 0 after Alt+F4', end('keepass')[0] == '0', end('keepass'))
    # KeePass writes its configuration on exit (.NET XmlSerializer): to %APPDATA%\KeePass, or, portable, over the global
    # KeePass.config.xml next to KeePass.exe when that file is writable (the staged one is small)
    saved = re.search(r'WR ls\| F +(\d+) /home/nexus/apps/KeePass/KeePass\.config\.xml', log)
    check('dotnet', 'KeePass saved its configuration on exit (.NET XML serializer)',
          'EXISTS path=/home/nexus/.wine/drive_c/users/nexus/AppData/Roaming/KeePass/KeePass.config.xml yes' in log or
          bool(saved and int(saved.group(1)) > len(KEEPASS_CONFIG) + 1000), saved and saved.group(0))
    check('mshtml', 'hh.exe exits 0 after its window is closed', end('chm')[0] == '0', end('chm'))
    check('mshtml', 'hh.exe (32-bit) exits 0 after its window is closed', end('chm32')[0] == '0', end('chm32'))
    reports = re.findall(r'\[64\] WR cat\| (engine=hta [^\n]*)', log); result['mshta'] = {}
    for bits, tag, arch in (('64', 'hta', 'AMD64'), ('32', 'hta32', 'x86')):
        report = next((r.strip() for r in reports if ' bits=%s ' % bits in r), '')
        upload = uploads / ('mshtml-hta-%s.txt' % bits)
        result['mshta'][bits] = {'status': end(tag), 'report': report, 'uploaded': upload.read_text('utf-8', 'replace') if upload.exists() else None}
        uploaded = result['mshta'][bits]['uploaded'] or ''
        result['mshta'][bits].update(stub='mshta.exe is a stub!' in output(tag), fso='fso=yes' in uploaded)
        platform = 'Win64' if bits == '64' else 'Win32'
        check('hta', 'mshta (%s-bit) runs an HTA: its JScript builds the DOM, reports over XMLHttpRequest (platform %s) and closes it with window.close()' % (bits, platform),
              end(tag)[0] == '0' and 'sum=5050 dom=made by script' in uploaded and ' platform=%s ' % platform in uploaded and 'mshta.exe is a stub!' not in output(tag),
              (end(tag), uploaded[:200], output(tag)[-200:]))
    ie = (uploads / 'mshtml-ie.txt').read_text('utf-8', 'replace') if (uploads / 'mshtml-ie.txt').exists() else ''
    result['iexplore_report'] = ie
    try: report = json.loads(ie)
    except ValueError: report = {}
    check('mshtml', 'iexplore: the page\'s JavaScript ran (querySelectorAll, JSON, layout, XMLHttpRequest) in standards mode',
          report.get('items') == 5 and report.get('json') == '0,1,4,9,16,25,36,49,64,81' and report.get('width') == 300 and (report.get('mode') or 0) >= 8, ie[:300])
    check('mshtml', 'iexplore exits 0 after its window is closed', end('iexplore')[0] == '0', end('iexplore'))
    m = re.search(r'WR PROCWAIT text=\.exe remaining=(\d+)', log); n = re.search(r'WR PROCWAIT text=wineserver remaining=(\d+)', log)
    check('cleanup', 'no Windows process and no wineserver remain', m and n and m.group(1) == '0' and n.group(1) == '0', re.findall(r'WR procs\| [^\n]*', log)[:10])
    kernel = [line for line in log.splitlines() if 'KERNEL_FAULT' in line]
    check('run', 'no kernel fault', not kernel, kernel[:5])
    result['user_faults'] = len(re.findall(r'USER_FAULT', log))
    result['passed'] = all(item['ok'] for item in checks)
finally:
    if vm: vm.close()
    if servers: servers.close()
    result['seconds'] = round(time.time() - started, 1)
    result['failed'] = [item for item in checks if not item['ok']]
    (WORK / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    if disk: disk.unlink(missing_ok=True)   # the overlay; the base was never written
    for old in (WORK / 'staged-files').glob('*') if (WORK / 'staged-files').exists() else (): old.unlink()
print('PASS Wine Mono/Gecko (%d checks)' % len(checks) if result['passed'] else 'FAIL Wine Mono/Gecko: %d of %d checks failed' % (len(result['failed']), len(checks)))
sys.exit(0 if result['passed'] else 1)
