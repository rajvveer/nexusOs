"""Drive VS Code, Firefox and LibreOffice Writer + Calc on the native NexusOS desktop and check what they produce.

    python -I tests/test_x64_desktop_apps.py WORK --disk APPS.img [--scenarios vscode,firefox,writer,calc]
           [--port 8860] [--qmp-port 4560] [--smp 2] [--tcg] [--display none]

APPS.img is a desktop64 image prepared with arch/x86_64/apps_install.py and arch/x86_64/apps_postinst.py whose
/sbin/init is arch/x86_64/out/desktop.elf (checked first). The image is never written: every scenario boots its own
qcow2 overlay in WORK/<scenario>/, drives the application with QMP keyboard input through Aurora Spotlight, shuts
Aurora down cleanly, reads the files the application saved out of the overlay on the host and deletes the overlay.
  vscode   opens /home/nexus/project, creates nexus.txt through the command palette, types two lines, saves with
           Ctrl+S, then runs `echo hello > /tmp/x; cat /tmp/x` in the integrated terminal (bash on a NexusOS pty).
           Checks the bytes of nexus.txt and /tmp/x.
  firefox  opens tests/fixtures/apps_browser_check.html from this script's HTTP server (guest address 10.0.2.2);
           the page POSTs JavaScript (sieve, fib, strings, JSON) and 120-frame canvas results, all must be ok.
  writer   types two paragraphs and saves /home/nexus/Documents/nexus-writer.odt (Save As); the host unzips it and
           checks the mimetype and both paragraphs.
  calc     enters 12, 30 and =A1*A2+1 and saves nexus-calc.ods; the host checks the formula and its value 361.
Every scenario also fails on a kernel fault, a user-mode fault or an ENOSYS outside ALLOWED_ENOSYS.
Logs and screenshots stay in WORK/<scenario>/. WHPX is used unless --tcg (the applications need minutes under TCG).
"""
from pathlib import Path
import argparse
import ctypes
import hashlib
import http.server
import importlib.util
import json
import re
import socket
import subprocess
import sys
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
PAGE = ROOT / 'tests/fixtures/apps_browser_check.html'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'
# Calls the applications probe and handle without (each has a userspace fallback).
ALLOWED_ENOSYS = {179: 'quotactl', 252: 'ioprio_get', 317: 'seccomp', 334: 'rseq', 425: 'io_uring_setup',
                  435: 'clone3', 444: 'landlock_create_ruleset'}
VSCODE_TEXT = b'Hello from VS Code on NexusOS\nline two\n'
WRITER_LINES = ('Hello from LibreOffice Writer on NexusOS.', 'Second paragraph.')

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', type=Path); ap.add_argument('--disk', type=Path, required=True)
ap.add_argument('--scenarios', default='vscode,firefox,writer,calc')
ap.add_argument('--port', type=int, default=8860); ap.add_argument('--qmp-port', type=int, default=4560)
ap.add_argument('--smp', type=int, default=2); ap.add_argument('--memory', default='3072')
ap.add_argument('--tcg', action='store_true'); ap.add_argument('--display', default='none')
args = ap.parse_args()
scenarios = [name for name in args.scenarios.split(',') if name]
assert all(name in ('vscode', 'firefox', 'writer', 'calc') for name in scenarios), scenarios


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


postinst = module('desktop_apps_postinst', ROOT / 'arch/x86_64/apps_postinst.py')   # Qcow2 overlay reader + FAT lookup


def guest_files(image, overlay, paths):
    """Read guest files from the raw image, or from a qcow2 overlay on top of it."""
    view = postinst.Qcow2(str(overlay), str(image)) if overlay else None
    postinst.fatput.open = (lambda path, mode='rb': view) if view else (lambda path, mode='rb': open(path, 'rb'))
    try:
        fs = postinst.fatput.Fat32(str(overlay or image))
        found = {}
        for path in paths:
            entry = postinst.fat_entry(fs, path.strip('/'))
            found[path] = fs.read_chain(entry[0], entry[1]) if entry and not entry[2] else None
        fs.f.close(); return found
    finally:
        del postinst.fatput.open
        if view: view.close()


init = guest_files(args.disk.resolve(), None, ['/sbin/init'])['/sbin/init']
assert init and hashlib.sha256(init).digest() == hashlib.sha256((OUT / 'desktop.elf').read_bytes()).digest(), \
    '/sbin/init on the disk is not arch/x86_64/out/desktop.elf; put it there with userland/fatput.py first'


def unthrottle(pid):
    """Windows 11 ignores 1 ms timer requests of windowless processes; opt QEMU out of power throttling."""
    class State(ctypes.Structure):
        _fields_ = [('Version', ctypes.c_ulong), ('ControlMask', ctypes.c_ulong), ('StateMask', ctypes.c_ulong)]
    try:
        kernel32 = ctypes.WinDLL('kernel32'); kernel32.OpenProcess.restype = ctypes.c_void_p
        handle = kernel32.OpenProcess(0x0200, False, pid)
        if not handle: return False
        state = State(1, 0x1 | 0x4, 0)
        ok = kernel32.SetProcessInformation(ctypes.c_void_p(handle), 4, ctypes.byref(state), ctypes.sizeof(state))
        kernel32.CloseHandle(ctypes.c_void_p(handle)); return bool(ok)
    except (AttributeError, OSError):
        return False


# ---------- host HTTP server: the browser check page and its result POSTs ----------
posted = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.split('?')[0] != '/apps_browser_check.html':
            self.send_error(404); return
        body = PAGE.read_bytes()
        self.send_response(200); self.send_header('content-type', 'text/html; charset=utf-8')
        self.send_header('content-length', str(len(body))); self.send_header('cache-control', 'no-store'); self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('content-length', 0)))
        try: message = json.loads(body)
        except ValueError: message = {'raw': body.decode('latin1')}
        posted.append(message)
        self.send_response(204); self.end_headers()
server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()


class Qmp:
    def __init__(self, port):
        deadline = time.time() + 60
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=120); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(.3)
        self.f = self.s.makefile('rwb'); self.read(); self.cmd('qmp_capabilities')
    def read(self):
        while True:
            line = self.f.readline()
            if not line: raise EOFError('QMP closed')
            message = json.loads(line)
            if 'event' not in message: return message
    def cmd(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None: request['arguments'] = arguments
        self.f.write(json.dumps(request).encode() + b'\n'); self.f.flush()
        reply = self.read()
        if 'error' in reply: raise RuntimeError(reply['error'])
        return reply.get('return')


QCODE = {' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
         ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', '\\': 'backslash', ',': 'comma', '.': 'dot', '/': 'slash'}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0', '_': 'minus',
           '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent',
           '|': 'backslash', '<': 'comma', '>': 'dot', '?': 'slash'}


class Run:
    def __init__(self, name):
        self.name, self.work = name, args.work / name
        self.work.mkdir(parents=True, exist_ok=True)
        for old in self.work.iterdir():
            if old.is_file(): old.unlink()
        self.serial, self.overlay, self.steps, self.q, self.t0 = self.work / 'serial.log', self.work / 'overlay.qcow2', [], None, time.time()
        subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(args.disk.resolve()), '-F', 'raw', str(self.overlay)], check=True)
        accel = ['-accel', 'tcg', '-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
        command = [QEMU, *accel, '-smp', str(args.smp), '-m', args.memory,
                   '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
                   '-drive', f'file={self.overlay},format=qcow2,if=ide', '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga',
                   '-display', args.display, '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{self.serial}',
                   '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-no-reboot']
        (self.work / 'cmd.json').write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, stdout=(self.work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
        self.step('vm', pid=self.proc.pid, unthrottled=unthrottle(self.proc.pid))

    def step(self, label, **data):
        entry = {'t': round(time.time() - self.t0, 1), 'step': label, **data}; self.steps.append(entry)
        print(' ', self.name, entry, flush=True)
    def text(self): return self.serial.read_bytes().decode('latin1') if self.serial.exists() else ''
    def wait_serial(self, pattern, after=0, seconds=300):
        end = time.time() + seconds
        while time.time() < end:
            match = re.search(pattern, self.text()[after:])
            if match: return match
            assert self.proc.poll() is None, 'VM exited while waiting for ' + pattern
            time.sleep(.25)
        raise TimeoutError(pattern)
    def key(self, name, down):
        self.q.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]})
        time.sleep(.06)
    def tap(self, *names):
        for name in names: self.key(name, True)
        for name in reversed(names): self.key(name, False)
        time.sleep(.08)
    def type(self, text, delay=.02):
        for ch in text:
            shift = ch.isupper() or ch in SHIFTED
            code = SHIFTED.get(ch) or QCODE.get(ch) or ch.lower()
            if shift: self.key('shift', True)
            self.key(code, True); self.key(code, False)
            if shift: self.key('shift', False)
            time.sleep(delay)
    def shot(self, label):
        ppm = self.work / f'{label}.ppm'
        try:
            self.q.cmd('screendump', {'filename': str(ppm)})
            from PIL import Image
            with Image.open(ppm) as image: image.save(self.work / f'{label}.png')
            ppm.unlink()
        except ImportError: pass                                  # keep the PPM
        except Exception as error: print('  screendump failed', error)
    def launch(self, query, app_id):
        before = len(self.text())
        self.tap('ctrl', 'spc'); time.sleep(1); self.type(query); time.sleep(1); self.tap('ret')
        match = self.wait_serial(r'AURORA WINDOW app=' + re.escape(app_id) + r' title=([^\n]*)', before, 900)
        self.step('window', app=app_id, title=match.group(1).strip())
    def shutdown(self):
        """Spotlight 'shut down': Aurora closes every window, init returns and the kernel writes everything back."""
        before = len(self.text()); self.tap('ctrl', 'spc'); time.sleep(1); self.type('shut down'); time.sleep(1); self.tap('ret')
        end = time.time() + 240
        while self.proc.poll() is None and time.time() < end and 'INIT SESSION RETURNED' not in self.text()[before:]: time.sleep(1)
        clean = 'INIT SESSION RETURNED' in self.text()[before:]
        self.step('shutdown', clean=clean)
        try: self.proc.wait(30)
        except subprocess.TimeoutExpired: pass
        return clean
    def close(self):
        if self.proc.poll() is None:
            try: self.q.cmd('quit')
            except Exception: pass
            try: self.proc.wait(30)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()
    def check_log(self):
        log = self.text()
        enosys = sorted({int(value, 16) for value in re.findall(r'ENOSYS syscall=(0x[0-9A-Fa-f]+)', log)})
        self.step('log', enosys=[ALLOWED_ENOSYS.get(n, n) for n in enosys])
        assert 'KERNEL_FAULT' not in log, 'kernel fault'
        assert 'USER_FAULT' not in log, 'user-mode fault: ' + re.search(r'[^\n]*USER_FAULT[^\n]*', log).group(0)
        unexpected = [n for n in enosys if n not in ALLOWED_ENOSYS]
        assert not unexpected, 'unexpected ENOSYS %s' % unexpected


def scenario_vscode(run):
    run.launch('visual studio', 'code'); time.sleep(45); run.shot('vscode_open')
    run.tap('ctrl', 'shift', 'p'); time.sleep(3); run.type('File: New File...'); time.sleep(3); run.tap('ret'); time.sleep(4)
    run.type('nexus.txt'); time.sleep(2); run.tap('ret'); time.sleep(8); run.shot('vscode_newfile')
    run.type(VSCODE_TEXT.decode()); time.sleep(3)
    run.tap('ctrl', 's'); time.sleep(8); run.shot('vscode_saved')
    run.tap('ctrl', 'grave_accent'); time.sleep(25); run.shot('vscode_terminal_open')
    run.type('echo hello > /tmp/x; cat /tmp/x; cat /home/nexus/project/nexus.txt\n'); time.sleep(10); run.shot('vscode_terminal')
    return {'/home/nexus/project/nexus.txt': lambda data: data == VSCODE_TEXT, '/tmp/x': lambda data: data == b'hello\n'}


def scenario_firefox(run):
    run.launch('firefox', 'firefox'); time.sleep(40); run.shot('firefox_open')
    url = f'http://10.0.2.2:{args.port}/apps_browser_check.html'
    for attempt in range(3):
        count = len(posted)
        run.tap('ctrl', 'l'); time.sleep(1); run.type(url); time.sleep(.5); run.tap('ret')
        end = time.time() + 90
        while time.time() < end and not any(m.get('kind') == 'phase' for m in posted[count:]): time.sleep(.5)
        if any(m.get('kind') == 'phase' for m in posted[count:]): break
        run.step('navigation retry', attempt=attempt)
    end = time.time() + 600
    while time.time() < end and not any(m.get('kind') == 'done' for m in posted): time.sleep(1)
    run.shot('firefox_page')
    done = next((m['data'] for m in posted if m.get('kind') == 'done'), None)
    assert done, 'the browser check page posted no result'
    (run.work / 'browser_result.json').write_text(json.dumps(done, indent=1))
    run.step('page result', ua=done.get('ua'), js={k: v['ok'] for k, v in done['js'].items()},
             canvas_fps=done['canvas']['fps'], canvas_ok=done['canvas']['ok'])
    assert 'Firefox/' in done.get('ua', ''), done.get('ua')
    assert all(v['ok'] for v in done['js'].values()) and done['canvas']['ok'] and done['canvas']['frames'] == 120, done
    return {}


def odf(mimetype, *needles):
    def check(data):
        with zipfile.ZipFile(__import__('io').BytesIO(data)) as archive:
            content = archive.read('content.xml').decode('utf-8')
            return archive.read('mimetype') == mimetype and all(needle in content for needle in needles)
    return check


def scenario_office(run, app):
    query, app_id = ('libreoffice w', 'libreoffice-writer') if app == 'writer' else ('libreoffice c', 'libreoffice-calc')
    run.launch(query, app_id); time.sleep(40); run.shot(app + '_open')
    if app == 'writer':
        for line in WRITER_LINES: run.type(line + '\n')
    else:
        run.type('12\n'); run.type('30\n'); run.type('=A1*A2+1\n')
    time.sleep(5); run.shot(app + '_typed')
    run.tap('ctrl', 'shift', 's'); time.sleep(10); run.shot(app + '_saveas')
    path = '/home/nexus/Documents/nexus-' + app + ('.odt' if app == 'writer' else '.ods')
    run.tap('ctrl', 'a'); run.type(path); time.sleep(1); run.tap('ret'); time.sleep(12); run.shot(app + '_saved')
    if app == 'writer':
        return {path: odf(b'application/vnd.oasis.opendocument.text', *WRITER_LINES)}
    return {path: odf(b'application/vnd.oasis.opendocument.spreadsheet', 'of:=[.A1]*[.A2]+1', 'office:value="361"')}


failures = []
started = time.time()
try:
    for name in scenarios:
        run = Run(name); checks = {}; result = {'name': name}
        try:
            run.wait_serial(r'AURORA READY persistent desktop', seconds=300)
            run.q = Qmp(args.qmp_port); time.sleep(5); run.step('desktop ready'); run.shot('desktop')
            checks = scenario_office(run, name) if name in ('writer', 'calc') else globals()['scenario_' + name](run)
            assert run.shutdown(), 'Aurora did not shut down cleanly'
            run.close()
            files = guest_files(args.disk.resolve(), run.overlay, list(checks))
            for path, check in checks.items():
                data = files[path]
                assert data is not None, path + ' was not saved'
                (run.work / ('saved_' + path.strip('/').replace('/', '_'))).write_bytes(data)
                assert check(data), path + ' has unexpected content (%d bytes)' % len(data)
                run.step('verified', path=path, bytes=len(data))
            run.check_log()
            result['passed'] = True
        except Exception as error:
            result['error'] = repr(error); failures.append((name, repr(error))); print('  FAIL', name, repr(error), flush=True)
            try: run.shot('error')
            except Exception: pass
        finally:
            run.close()
            run.overlay.unlink(missing_ok=True)
            result['steps'] = run.steps
            (run.work / 'summary.json').write_text(json.dumps(result, indent=1))
        if result.get('passed'): print('PASS', name, 'in %.0f s' % (time.time() - run.t0), flush=True)
finally:
    server.shutdown()
assert not failures, failures
print('PASS desktop applications (%s) in %.0f s' % (', '.join(scenarios), time.time() - started))
