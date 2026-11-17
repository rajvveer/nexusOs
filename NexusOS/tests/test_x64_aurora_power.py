"""Aurora's power UI and system sleep on the native NexusOS desktop with Chrome (q35, S3 enabled, test SSDT).

    python -I tests/test_x64_aurora_power.py WORK --disk DESKTOP.img [--scenarios sleep,timeout,double,critical,profile]
           [--port 8931] [--qmp-port 4632] [--smp 2] [--tcg] [--cycles 3]

DESKTOP.img is a desktop64 image with Chrome whose /sbin/init is arch/x86_64/out/desktop.elf (checked first; a sparse
copy of the fresh desktop disk staged with `tests/stage_desktop_disk.py DISK arch/x86_64/out/desktop.elf sbin/init`). It is
never written: every scenario boots its own qcow2 overlay in WORK/<scenario>/ (deleted afterwards; logs and
screenshots stay). The firmware gets tests/acpi_test_ssdt.py's table (AC adapter, battery, backlight, buttons, lid,
thermal zone; state in CMOS 0x60.., events through a GPE block on pc-testdev), so the battery item, the brightness
slider and the battery notifications run against the kernel's real ACPI path.
  sleep    the menu-bar battery item (percent of the SSDT battery) and its panel, the brightness slider
           (/sys/class/backlight -> _BCM), the low-battery notification, the power-button dialog (Cancel), then Chrome
           on tests/fixtures/power_heartbeat.html and --cycles sleeps with it open - dialog "Sleep" (S key), the sleep
           key (KEY_SLEEP), Apple menu "Sleep" - each woken by system_wakeup: Aurora saw suspend-prepare, acknowledged,
           saw post-suspend; Chrome's timers and network came back (beats POSTed after the wake), Date.now() moved by
           the sleep and performance.now() (CLOCK_MONOTONIC) did not; Apple menu "Hibernate" without a hibernation
           area is refused with a notice; finally dialog "Restart": Chrome closes, Aurora ends and the kernel restarts
           the machine (QEMU -no-reboot exits).
  timeout  one power-button press and no answer: 60 s later the shutdown starts and the machine powers off.
  lid      two displays (virtio-gpu head 1 enabled through a VNC client's SetDesktopSize, as plugging in a monitor):
           closing the lid keeps the machine up (lid_switch_docked) and turns the built-in panel (head 0) dark while
           the desktop keeps running; opening it brings the panel back with the desktop on it.
  profile  the battery panel's power mode (Low Power / Balanced / Performance: keys 3 and 1, a click on Balanced) ->
           /sys/power/policy/profile -> the cpufreq governor, seen at the SSDT's _PCT register (P0 for performance,
           the lowest P-state for power-saver); battery saver: the battery Low on DC turns Balanced into Low Power
           (lowest P-state), connecting power brings Balanced back.
  double   Chrome open, power button pressed twice: shut down at once (Chrome asked to close), power off.
  critical battery critical while on battery: Aurora warns, the kernel's policy (hibernate; unavailable -> power off)
           ends the session through Aurora (PM_EVENT=shutdown-prepare), power off.
  hibernate (needs /nexus/hiber.img on the disk, e.g. tests/stage_desktop_disk.py DISK --reserve nexus/hiber.img 512;
           the fresh desktop disk has ~1 GiB free and Chrome's profile needs room too):
           Chrome on the heartbeat page, Apple menu "Hibernate": the image is written and QEMU powers off; the same
           overlay boots again, the image is restored, Chrome's timers and network continue (Date.now() advanced by the
           time off, performance.now() did not), then power-button double press shuts down.
Every scenario fails on a kernel fault or a user-mode fault. WHPX is used unless --tcg.
"""
from pathlib import Path
import argparse, ctypes, hashlib, http.server, importlib.util, json, re, socket, struct, subprocess, sys, threading, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
PAGE = ROOT / 'tests/fixtures/power_heartbeat.html'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', type=Path); ap.add_argument('--disk', type=Path, required=True)
ap.add_argument('--scenarios', default='sleep,timeout,double,critical,profile')
ap.add_argument('--port', type=int, default=8931); ap.add_argument('--qmp-port', type=int, default=4632)
ap.add_argument('--smp', type=int, default=2); ap.add_argument('--memory', default='2048')
ap.add_argument('--cycles', type=int, default=3); ap.add_argument('--tcg', action='store_true')
ap.add_argument('--vnc-port', type=int, default=9167, help='lid: the VNC server of the second display head')
args = ap.parse_args()
scenarios = [name for name in args.scenarios.split(',') if name]
assert all(name in ('sleep', 'timeout', 'double', 'critical', 'hibernate', 'profile', 'lid') for name in scenarios), scenarios


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


ssdt = module('acpi_test_ssdt', ROOT / 'tests/acpi_test_ssdt.py')
postinst = module('aurora_power_postinst', ROOT / 'arch/x86_64/apps_postinst.py')   # FAT reader
fs = postinst.fatput.Fat32(str(args.disk.resolve()), readonly=True)
try:
    def guest_file(path):
        parts = path.strip('/').split('/'); directory = fs.lookup_dir(parts[:-1]); entry = fs.find_name(directory, parts[-1])
        if not entry: return None
        record = entry[2]; first = int.from_bytes(record[20:22], 'little') << 16 | int.from_bytes(record[26:28], 'little')
        return fs.read_chain(first, int.from_bytes(record[28:32], 'little'))
    nexus = fs.lookup_dir(['nexus']); HIBER = fs.find_name(nexus, 'hiber.img') is not None   # hibernation area (hibernate.c)
    init = guest_file('/sbin/init')
    assert init and hashlib.sha256(init).digest() == hashlib.sha256((OUT / 'desktop.elf').read_bytes()).digest(), \
        '/sbin/init on the disk is not arch/x86_64/out/desktop.elf'
finally:
    fs.close()


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


# ---------- host HTTP server: the heartbeat page and its beats ----------
beats = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.split('?')[0] != '/power_heartbeat.html': self.send_error(404); return
        body = PAGE.read_bytes()
        self.send_response(200); self.send_header('content-type', 'text/html; charset=utf-8')
        self.send_header('content-length', str(len(body))); self.send_header('cache-control', 'no-store'); self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('content-length', 0)))
        try: message = json.loads(body)
        except ValueError: message = {'raw': body.decode('latin1')}
        message['host'] = time.time(); beats.append(message)
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
    def hmp(self, line): return self.cmd('human-monitor-command', {'command-line': line})


QCODE = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', ',': 'comma', '.': 'dot', '/': 'slash', ':': 'semicolon', '_': 'minus'}


def vnc_set_desktop_size(port, width=1280, height=800, timeout=20):
    """Minimal RFB 3.8 client: SetDesktopSize on the VNC server bound to a virtio-gpu head makes QEMU report that head
    enabled, like a monitor being plugged in (as tests/test_x64_acpi.py). Returns the open connection."""
    deadline = time.time() + timeout
    while True:
        try: link = socket.create_connection(('127.0.0.1', port), timeout=10); break
        except OSError:
            if time.time() > deadline: raise
            time.sleep(0.2)
    def read(n):
        data = b''
        while len(data) < n:
            chunk = link.recv(n - len(data))
            if not chunk: raise AssertionError('VNC server closed the connection')
            data += chunk
        return data
    read(12); link.sendall(b'RFB 003.008\n')
    types = read(read(1)[0]); assert 1 in types, f'VNC security types {list(types)}'
    link.sendall(b'\x01'); assert struct.unpack('>I', read(4))[0] == 0, 'VNC security result'
    link.sendall(b'\x01'); read(20); read(struct.unpack('>I', read(4))[0])
    link.sendall(struct.pack('>BBHi', 2, 0, 1, -308))
    link.sendall(struct.pack('>BBHHBB', 251, 0, width, height, 1, 0) + struct.pack('>IHHHHI', 0, 0, 0, width, height, 0))
    return link


class Run:
    def __init__(self, name, state, again=False, heads=1):
        """again: boot the same overlay once more (hibernation restore); its log is serial-again.log."""
        self.name, self.work = name, args.work / name
        self.work.mkdir(parents=True, exist_ok=True)
        if not again:
            for old in self.work.iterdir():
                if old.is_file(): old.unlink()
        log = 'serial-again.log' if again else 'serial.log'
        self.serial, self.overlay, self.t0, self.pos = self.work / log, self.work / 'overlay.qcow2', time.time(), [0, 0]
        if not again:
            (self.work / 'ssdt.aml').write_bytes(ssdt.build(args.smp))
            subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(args.disk.resolve()), '-F', 'raw', str(self.overlay)], check=True)
        accel = ['-accel', 'tcg', '-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
        command = [QEMU, *accel, '-smp', str(args.smp), '-m', args.memory, '-machine', 'q35', '-global', 'ICH9-LPC.disable_s3=0',
                   '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
                   '-drive', 'if=none,id=root,file=overlay.qcow2,format=qcow2', '-device', 'nvme,serial=nexus-root,drive=root',
                   '-vga', 'none', '-device', 'virtio-vga' if heads == 1 else f'virtio-vga,id=gpu0,max_outputs={heads}', '-display', 'none',
                   *([] if heads == 1 else ['-vnc', f'127.0.0.1:{args.vnc_port - 5900},display=gpu0,head=1']),
                   '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network',
                   '-acpitable', 'file=ssdt.aml', '-device', 'pc-testdev',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', 'file:' + log,
                   '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-no-reboot', '-S']
        (self.work / 'cmd.json').write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, cwd=self.work, stdout=(self.work / ('qemu-again.log' if again else 'qemu.log')).open('wb'), stderr=subprocess.STDOUT)
        self.step('vm', pid=self.proc.pid, unthrottled=unthrottle(self.proc.pid))
        self.q = Qmp(args.qmp_port)
        self.state(**state)
        self.links = [vnc_set_desktop_size(args.vnc_port)] if heads > 1 else []   # head 1 connected before the firmware runs
        self.q.cmd('cont')

    def step(self, label, **data):
        print(' ', self.name, {'t': round(time.time() - self.t0, 1), 'step': label, **data}, flush=True)
    def text(self): return self.serial.read_bytes().decode('latin1') if self.serial.exists() else ''
    def wait_serial(self, pattern, after=0, seconds=300):
        end = time.time() + seconds
        while time.time() < end:
            match = re.search(pattern, self.text()[after:])
            if match: return match
            assert self.proc.poll() is None, 'VM exited while waiting for ' + pattern
            time.sleep(.25)
        raise TimeoutError(pattern)
    def state(self, **values):
        for line in ssdt.state_commands(ssdt.state_bytes(**values)): self.q.hmp(line)
    def acpi_event(self, **values):
        self.state(**values)
        for line in ssdt.event_commands(): self.q.hmp(line)
    def key(self, name, down):
        self.q.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]})
        time.sleep(.06)
    def tap(self, *names):
        for name in names: self.key(name, True)
        for name in reversed(names): self.key(name, False)
        time.sleep(.1)
    def type(self, text):
        for ch in text:
            shift = ch in ':_'
            if shift: self.key('shift', True)
            code = QCODE.get(ch, ch); self.key(code, True); self.key(code, False)
            if shift: self.key('shift', False)
    def move_to(self, x, y):
        while self.pos != [x, y]:
            dx = max(-90, min(90, x - self.pos[0])); dy = max(-90, min(90, y - self.pos[1])); events = []
            if dx: events.append({'type': 'rel', 'data': {'axis': 'x', 'value': dx}})
            if dy: events.append({'type': 'rel', 'data': {'axis': 'y', 'value': dy}})
            self.pos[0] += dx; self.pos[1] += dy; self.q.cmd('input-send-event', {'events': events}); time.sleep(.03)
    def click(self, x, y):
        self.move_to(x, y); time.sleep(.2)
        for down in (True, False):
            self.q.cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': down}}]}); time.sleep(.15)
    def shot(self, label):
        ppm = self.work / f'{label}.ppm'
        try:
            self.q.cmd('screendump', {'filename': str(ppm)})
            data = ppm.read_bytes(); header = data[:32].split(); size = (int(header[1]), int(header[2]))
            try:
                from PIL import Image
                with Image.open(ppm) as image: image.save(self.work / f'{label}.png')
                ppm.unlink()
            except ImportError: pass
            return size
        except Exception as error: print('  screendump failed', error); return None
    def status(self): return self.q.cmd('query-status')['status']
    def wait_status(self, wanted, seconds=60):
        end = time.time() + seconds
        while time.time() < end:
            if self.status() == wanted: return
            assert self.proc.poll() is None, 'VM exited while waiting for status ' + wanted
            time.sleep(.2)
        raise TimeoutError('status ' + wanted)
    def ready(self):
        self.wait_serial(r'AURORA READY persistent desktop', 0, 420)
        line = self.wait_serial(r'AURORA POWER ready uevents=(\w+) batteries=(\d+) ac=(\d+) backlight=(\S+) keys=(\w+)', 0, 60)
        self.step('aurora', power=line.group(0))
        assert line.groups() == ('yes', '1', '1', 'acpi_video0', 'interactive'), line.group(0)
        item = int(self.wait_serial(r'AURORA POWER status item x=(-?\d+)', 0, 60).group(1))
        size = self.shot('desktop'); assert size, 'no screendump'
        self.size = size; self.pos = [size[0] // 2, size[1] // 2]   # Aurora starts the pointer in the middle
        return item
    def open_chrome(self):
        before = len(self.text()); self.click(864 * self.size[0] // 1920, self.size[1] - 40)
        self.wait_serial(r'AURORA WINDOW app=google-chrome', before, 300); self.step('chrome window')
        time.sleep(12)
        url = f'http://10.0.2.2:{args.port}/power_heartbeat.html'
        for attempt in range(4):
            count = len(beats)
            self.tap('ctrl', 'l'); time.sleep(.5); self.type(url); time.sleep(.3); self.tap('ret')
            end = time.time() + 60
            while time.time() < end and len(beats) < count + 3: time.sleep(.3)
            if len(beats) >= count + 3: break
            self.step('navigation retry', attempt=attempt)
        assert len(beats) >= 3, 'heartbeat page never reported'
        self.step('heartbeat', beats=len(beats))
    def power_off(self, before, restart=False, seconds=300):
        end = time.time() + seconds
        while self.proc.poll() is None and time.time() < end: time.sleep(.5)
        assert self.proc.poll() is not None, 'machine did not stop'
        log = self.text()[before:]
        if restart: assert '[64] AURORA POWER restarting' in log and 'REBOOT syscall: restart' in log, log[-3000:]
        else: assert 'INIT SESSION RETURNED' in log and self.proc.returncode == 33, (self.proc.returncode, log[-3000:])
        self.step('stopped', code=self.proc.returncode, restart=restart)
    def check_log(self):
        log = self.text()
        assert 'KERNEL_FAULT' not in log, 'kernel fault'
        assert 'USER_FAULT' not in log, 'user-mode fault: ' + re.search(r'[^\n]*USER_FAULT[^\n]*', log).group(0)
        assert '[64] FAIL' not in log, re.search(r'[^\n]*\[64\] FAIL[^\n]*', log).group(0)
    def close(self, keep=False):
        if self.proc.poll() is None:
            try: self.q.cmd('quit')
            except Exception: pass
            try: self.proc.wait(30)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()
        for link in getattr(self, 'links', []): link.close()
        if not keep: self.overlay.unlink(missing_ok=True)


# Apple-menu rows (kernel/menubar.c M_NEXUS: items 24 px, separators 10 px): Sleep and Hibernate follow
# About | Applications | System Settings..., Lock Screen |
MENU_SLEEP_Y, MENU_HIBERNATE_Y = 116 + 58, 140 + 58
CHARGING = dict(ac=True, battery_state=2, remaining=40000, rate=5000)


def panel_geometry(run, before):
    """The battery panel's layout as Aurora prints it when the panel opens (its height depends on the power-mode row)."""
    line = run.wait_serial(r'AURORA POWER panel geometry ([^\n]*)', before, 30).group(1)
    values = dict(pair.split('=', 1) for pair in line.split())
    return {key: ([int(v) for v in value.split(',')] if key == 'modes' else value if key == 'mode' else int(value)) for key, value in values.items()}


def read_pstate(run):   # tests/acpi_test_ssdt.py's _PCT control register: 0 = P0 (2400 MHz) ... 2 = the lowest P-state
    return int(run.q.hmp(f'i /b {ssdt.PSTATE_PORT:#x}').strip().split('=')[-1], 16)


def sleep_cycle(run, how, cycle):
    """Put the machine to sleep with Chrome open, wake it with system_wakeup and check what came back."""
    before = len(run.text()); count = len(beats); last = beats[-1]
    if how == 'dialog':
        run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog open', before, 60); time.sleep(.5); run.tap('s')
    elif how == 'key': run.tap('sleep')
    else:   # Apple menu -> Sleep (merged menu: About | Applications | System Settings..., Lock Screen | Sleep, ...)
        run.click(20, 14); time.sleep(.6); run.click(48, MENU_SLEEP_Y)
    run.wait_serial(r'AURORA POWER request mem', before, 60)
    run.wait_serial(r'AURORA POWER pm-event suspend-prepare', before, 60)
    ack = run.wait_serial(r'AURORA POWER sleep acknowledged after (\d+) ms', before, 60)
    run.wait_status('suspended', 90); asleep = time.time(); time.sleep(6)
    count_asleep = len(beats); run.q.cmd('system_wakeup'); woke = time.time()
    run.wait_serial(r'\[64\] PM: woke from S3', before, 90)
    run.wait_serial(r'AURORA POWER pm-event post-suspend', before, 90)
    run.wait_serial(r'AURORA POWER mem request returned 0', before, 90)
    end = time.time() + 90
    while time.time() < end and len(beats) < count_asleep + 3: time.sleep(.3)
    assert len(beats) >= count_asleep + 3, f'Chrome heartbeat did not resume after wake {cycle} ({len(beats) - count_asleep} beats)'
    assert count_asleep - count <= 2, 'beats while suspended'
    first = beats[count_asleep]
    date_step, perf_step = first['date'] - last['date'], first['perf'] - last['perf']
    slept = (woke - asleep) * 1000
    assert date_step >= slept - 1500, f'Date.now() moved {date_step:.0f} ms over a {slept:.0f} ms sleep'
    assert perf_step < date_step - slept * 0.6, f'performance.now() moved {perf_step:.0f} ms (Date {date_step:.0f} ms, slept {slept:.0f} ms)'
    assert first['ticks'] > last['ticks'], 'Chrome timers did not run after the wake'
    run.shot(f'after_sleep_{cycle}')
    run.step('sleep', cycle=cycle, how=how, ack_ms=int(ack.group(1)), slept_ms=round(slept), date_ms=round(date_step), perf_ms=round(perf_step),
             beats_after=len(beats) - count_asleep, fetch_failures=beats[-1].get('failures'))


def scenario_sleep():
    run = Run('sleep', CHARGING)
    try:
        item = run.ready(); assert item > 0, 'no battery item in the menu bar'
        # battery panel + brightness slider
        before = len(run.text()); run.click(item + 20, 14); run.wait_serial(r'AURORA POWER panel open', before, 30)
        geometry = panel_geometry(run, before); run.shot('battery_panel')
        run.click(geometry['slider_x'] + geometry['slider_w'] * 3 // 9, geometry['slider_y'] + 4)
        level = run.wait_serial(r'AURORA POWER brightness (\d+)/(\d+) error=(-?\d+)', before, 30)
        assert level.groups() == ('3', '9', '0'), level.group(0)
        run.tap('esc'); run.wait_serial(r'AURORA POWER panel closed', before, 30)
        run.step('brightness', line=level.group(0))
        # low battery on battery power, then back to the charger
        before = len(run.text()); run.acpi_event(ac=False, battery_state=1, remaining=4000, rate=12000)
        low = run.wait_serial(r'AURORA POWER battery low (\d+)%', before, 60); run.shot('battery_low')
        run.acpi_event(**CHARGING); run.wait_serial(r'POWER AC \S+ online', before, 60)
        run.step('battery low', percent=int(low.group(1)))
        # power button -> dialog -> Cancel
        before = len(run.text()); run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog open', before, 60)
        time.sleep(1.5); run.shot('power_dialog'); run.tap('esc'); run.wait_serial(r'AURORA POWER dialog choice Cancel', before, 30)
        assert 'POWER ORDERLY' not in run.text()[before:], 'kernel acted on the power button although Aurora owns it'
        run.open_chrome()
        for cycle in range(1, args.cycles + 1): sleep_cycle(run, ('dialog', 'key', 'menu')[(cycle - 1) % 3], cycle)
        # Hibernate without a hibernation area: refused, the session goes on (the hibernate scenario covers a disk with one)
        before = len(run.text())
        if not HIBER:
            run.click(20, 14); time.sleep(.6); run.click(48, MENU_HIBERNATE_Y)
            refused = run.wait_serial(r'AURORA POWER disk request returned (\d+)', before, 60)
            assert refused.group(1) != '0', 'hibernation unexpectedly succeeded'
            count = len(beats); end = time.time() + 30
            while time.time() < end and len(beats) < count + 2: time.sleep(.3)
            assert len(beats) >= count + 2, 'session stopped after the refused hibernation'
            run.step('hibernate refused', code=int(refused.group(1)))
        # Restart from the dialog: Chrome closes, Aurora ends, the kernel resets the machine
        before = len(run.text()); run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog open', before, 60)
        time.sleep(.5); run.tap('r'); run.wait_serial(r'AURORA POWER dialog choice Restart', before, 30)
        run.power_off(before, restart=True)
        run.check_log()
    finally: run.close()


def scenario_profile():
    run = Run('profile', CHARGING)
    try:
        item = run.ready(); assert item > 0, 'no battery item in the menu bar'
        before = len(run.text()); run.click(item + 20, 14); run.wait_serial(r'AURORA POWER panel open', before, 30)
        geometry = panel_geometry(run, before); run.shot('power_mode_panel')
        assert geometry['mode'] == 'balanced' and min(geometry['modes']) > 0, geometry
        run.tap('3'); run.wait_serial(r'AURORA POWER profile performance error=0', before, 30); time.sleep(2); performance = read_pstate(run)
        run.tap('1'); run.wait_serial(r'AURORA POWER profile power-saver error=0', before, 30); time.sleep(2); saver = read_pstate(run)
        run.shot('power_mode_low')
        run.click(geometry['modes'][1], geometry['mode_y']); run.wait_serial(r'AURORA POWER profile balanced error=0', before, 30)
        run.shot('power_mode_balanced'); run.tap('esc'); run.wait_serial(r'AURORA POWER panel closed', before, 30)
        assert performance == 0 and saver == 2, f'_PCT after performance {performance}, after power-saver {saver}'
        # battery saver: Low on battery power turns Balanced into Low Power; power back restores Balanced
        before = len(run.text()); run.acpi_event(ac=False, battery_state=1, remaining=4000, rate=12000)
        run.wait_serial(r'AURORA POWER profile power-saver \(battery saver\) error=0', before, 60)
        run.wait_serial(r'AURORA POWER battery low \d+% battery-saver', before, 60); time.sleep(2); on_battery = read_pstate(run)
        run.shot('battery_saver')
        run.acpi_event(**CHARGING); run.wait_serial(r'AURORA POWER profile balanced \(battery saver\) error=0', before, 60)
        assert on_battery == 2, f'_PCT in battery saver {on_battery}'
        run.check_log()
        run.step('profile', performance_pstate=performance, power_saver_pstate=saver, battery_saver_pstate=on_battery)
    finally: run.close()


def head_brightness(run, label):
    """Mean brightness (0-255) of head 0's screendump (QMP screendump device gpu0 head 0)."""
    ppm = run.work / f'{label}.ppm'
    run.q.cmd('screendump', {'filename': str(ppm), 'device': 'gpu0', 'head': 0})
    data = ppm.read_bytes(); parts = data.split(maxsplit=4); pixels = parts[4]
    mean = sum(pixels[::97]) / len(pixels[::97])
    try:
        from PIL import Image
        with Image.open(ppm) as image: image.save(run.work / f'{label}.png')
        ppm.unlink()
    except ImportError: pass
    return mean


def scenario_lid():
    run = Run('lid', CHARGING, heads=2)
    try:
        item = run.ready()
        assert re.search(r'\[64\] KMS ready heads=2 connected=2', run.text()), 'the second display is not connected'
        before_level = head_brightness(run, 'panel_open')
        before = len(run.text()); run.acpi_event(lid=False)
        run.wait_serial(r'\[64\] POWER: lid closed with an external display: ignore', before, 60)
        run.wait_serial(r'\[64\] KMS built-in panel off \(lid closed\)\n', before, 60); time.sleep(2)
        closed_level = head_brightness(run, 'panel_closed')
        assert run.status() == 'running' and 'PM: entering' not in run.text()[before:], 'the machine slept with a second display'
        run.click(item + 20, 14); run.wait_serial(r'AURORA POWER panel open', before, 30); run.tap('esc')   # the desktop still runs
        run.wait_serial(r'AURORA POWER panel closed', before, 30)
        before = len(run.text()); run.acpi_event(lid=True)
        run.wait_serial(r'\[64\] KMS built-in panel on \(lid open\)\n', before, 60); time.sleep(2)
        open_level = head_brightness(run, 'panel_reopened')
        run.step('lid', open=round(before_level, 1), closed=round(closed_level, 1), reopened=round(open_level, 1))
        assert before_level > 15 and open_level > 15 and closed_level < before_level / 3, (before_level, closed_level, open_level)
        run.check_log()
    finally: run.close()


def scenario_timeout():
    run = Run('timeout', CHARGING)
    try:
        run.ready(); before = len(run.text()); run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog open', before, 60)
        opened = time.time(); time.sleep(20); run.shot('countdown')
        run.wait_serial(r'AURORA POWER dialog timed out', before, 90); waited = time.time() - opened
        assert 55 <= waited <= 75, f'dialog timed out after {waited:.0f} s'
        run.power_off(before); run.check_log(); run.step('timeout', seconds=round(waited))
    finally: run.close()


def scenario_double():
    run = Run('double', CHARGING)
    try:
        run.ready(); run.open_chrome()
        before = len(run.text()); run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog open', before, 60)
        time.sleep(1); run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog choice Shut Down', before, 30)
        run.power_off(before); run.check_log()
        assert re.search(r'AURORA CHILD EXIT', run.text()[before:]), 'Chrome was not ended'
    finally: run.close()


def scenario_critical():
    run = Run('critical', CHARGING)
    try:
        run.ready(); before = len(run.text())
        run.acpi_event(ac=False, battery_state=1 | 4, remaining=2000, rate=12000)
        run.wait_serial(r'AURORA POWER battery critical (\d+)%', before, 60); run.shot('battery_critical')
        run.wait_serial(r'POWER: battery critical; \w+ in 10 s', before, 60)
        if HIBER:   # the policy's first choice: hibernate (power off with the image saved)
            run.wait_serial(r'AURORA POWER pm-event hibernation-prepare', before, 60)
            run.wait_serial(r'PM: hibernation image saved', before, 900); run.proc.wait(180); run.check_log(); return
        run.wait_serial(r'AURORA POWER pm-event shutdown-prepare', before, 60)
        run.power_off(before); run.check_log()
    finally: run.close()


def scenario_hibernate():
    """Apple menu Hibernate with Chrome open; QEMU powers off; booting the same disk restores the session."""
    run = Run('hibernate', CHARGING)
    try:
        run.ready(); run.open_chrome()
        before = len(run.text()); run.click(20, 14); time.sleep(.6); run.click(48, MENU_HIBERNATE_Y)
        run.wait_serial(r'AURORA POWER request disk', before, 60)
        run.wait_serial(r'AURORA POWER pm-event hibernation-prepare', before, 60)
        written = run.wait_serial(r'hibernation image written: (\d+) pages \((\d+) non-zero, (\d+) MiB\) -> (\d+) MiB in (\d+) ms', before, 1200)
        run.wait_serial(r'PM: hibernation image saved', before, 120)
        code = run.proc.wait(180); off = time.time(); count = len(beats); last = beats[-1]; pos = list(run.pos); size = run.size
        run.step('hibernated', pages=int(written.group(1)), raw_mib=int(written.group(3)), image_mib=int(written.group(4)), ms=int(written.group(5)), qemu_exit=code)
    finally: run.close(keep=True)
    time.sleep(3)
    run = Run('hibernate', CHARGING, again=True); run.pos, run.size = pos, size
    try:
        loaded = run.wait_serial(r'hibernation image loaded: (\d+) pages \((\d+) in place, (\d+) through spare frames\) in (\d+) ms', 0, 900)
        run.wait_serial(r'\[64\] PM: restored from the hibernation image', 0, 300); back = time.time()
        run.wait_serial(r'AURORA POWER pm-event post-hibernation', 0, 300)
        run.wait_serial(r'AURORA POWER disk request returned 0', 0, 300)
        end = time.time() + 120
        while time.time() < end and len(beats) < count + 3: time.sleep(.3)
        assert len(beats) >= count + 3, 'Chrome heartbeat did not resume after the restore'
        first = beats[count]; date_step, perf_step = first['date'] - last['date'], first['perf'] - last['perf']
        assert first['ticks'] > last['ticks'] and date_step >= (back - off) * 1000 - 2000, (date_step, back - off)
        assert perf_step < date_step - (back - off) * 1000 * 0.5, f'performance.now() moved {perf_step:.0f} ms across the hibernation'
        run.shot('after_restore')
        run.step('restored', pages=int(loaded.group(1)), in_place=int(loaded.group(2)), copied=int(loaded.group(3)), load_ms=int(loaded.group(4)),
                 off_s=round(back - off, 1), date_ms=round(date_step), perf_ms=round(perf_step), fetch_failures=beats[-1].get('failures'))
        before = len(run.text()); run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog open', before, 60)
        time.sleep(1); run.q.cmd('system_powerdown'); run.wait_serial(r'AURORA POWER dialog choice Shut Down', before, 30)
        run.power_off(before); run.check_log()
    finally: run.close()


for name in scenarios:
    print('SCENARIO', name, flush=True); beats.clear()
    {'sleep': scenario_sleep, 'timeout': scenario_timeout, 'double': scenario_double, 'critical': scenario_critical, 'hibernate': scenario_hibernate,
     'profile': scenario_profile, 'lid': scenario_lid}[name]()
    print('PASS', name, flush=True)
server.shutdown()
print('PASS Aurora power:', ', '.join(scenarios), flush=True)
