"""Gamepad API on native NexusOS: real Google Chrome sees NexusOS's virtual Xbox 360 pad (vpad.c: evdev + joydev +
the input sysfs objects that Chrome's libudev enumeration reads) and drives its rumble motors.

    python -I tests/test_x64_gamepad_chrome.py [--work DIR] [--disk DESKTOP64.img] [--port 9153] [--qmp-port 4754]

--disk is a desktop disk with Chrome, used read only (default: the integrator's D:\\nexustest\\prod\\integ_desk_disk\\desktop64.img);
through tests/overlay_stage.py this tree's desktop.elf (/sbin/init), aurora_app.elf
(/bin/aurora-app) and a Chrome argument file (fresh profile in /tmp, the host page treated as secure) are staged into
it, then it boots through a qcow2 overlay with the kernel option vpad. Chrome is opened from the Aurora dock on
tests/fixtures/gamepad/gamepad.html (served from the host), which polls navigator.getGamepads().
Checks: one gamepad with Chrome's Xbox 360 id (vendor 045e, product 028e) and the "standard" mapping, 17 buttons and
4 axes, every phase of the pad's 4-second pattern seen through the standard layout (A, B, left trigger, D-pad up,
left stick right/left/down), vibrationActuator.playEffect('dual-rumble') resolving "complete",
and the kernel log showing the rumble reaching the pad (strong ~0.75, weak ~0.25 of full scale, then off).
The overlay is deleted at the end.
"""
from pathlib import Path
import argparse
import http.server
import json
import re
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402
import overlay_stage  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('--work', type=Path, default=None)
ap.add_argument('--disk', type=Path, default=Path(r'D:\nexustest\prod\integ_desk_disk\desktop64.img'))   # read only
ap.add_argument('--port', type=int, default=9153); ap.add_argument('--qmp-port', type=int, default=4754)
args = ap.parse_args()
WORK = (args.work or Path(r'D:\nexustest\wave2\ws_hw2_runs') / 'gamepad_chrome').resolve(); WORK.mkdir(parents=True, exist_ok=True)
for old in WORK.iterdir():
    if old.is_file() and old.suffix in ('.log', '.qcow2', '.png', '.ppm', '.json'): old.unlink()
assert 'New folder' not in str(args.disk), 'never boot the real desktop disk'
disk = args.disk
PAGE = (ROOT / 'tests/fixtures/gamepad/gamepad.html').read_bytes()
chrome_args = '\n'.join([
    '/home/nexus/Applications/chrome/opt/google/chrome/chrome', '--ozone-platform=wayland', '--disable-gpu', '--no-first-run',
    '--ipc-connection-timeout=120', '--user-data-dir=/tmp/nx-gamepad-profile',
    f'--unsafely-treat-insecure-origin-as-secure=http://10.0.2.2:{args.port}', f'http://10.0.2.2:{args.port}/gamepad', ''])

state = {'results': [], 'done': None, 'gets': []}
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        state['gets'].append(self.path)
        if self.path.split('?')[0] == '/gamepad':
            self.send_response(200); self.send_header('content-type', 'text/html'); self.send_header('content-length', str(len(PAGE))); self.end_headers()
            self.wfile.write(PAGE)
        else: self.send_response(404); self.end_headers()
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('content-length', 0)))
        try: message = json.loads(body)
        except ValueError: message = {'raw': body.decode('latin1')}
        state['results'].append(message)
        if message.get('kind') == 'done': state['done'] = message['data']
        self.send_response(204); self.end_headers()
server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()

overlay = WORK / 'overlay.qcow2'; serial = WORK / 'serial.log'
text = lambda: serial.read_bytes().decode('latin1') if serial.exists() else ''
try:
    overlay_stage.stage(disk, overlay, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf',
                                        'nexus/desktop.args': b'--aurora\n', 'nexus/chrome-desktop.args': chrome_args.encode()})   # disk never written
    accel = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', aurora_vm.CPU] if aurora_vm.whpx_usable() else ['-accel', 'tcg', '-cpu', 'max']
    command = [aurora_vm.QEMU, *accel, '-smp', '2', '-m', '2048',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={overlay},format=qcow2,if=ide', '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga',
               '-display', 'none', '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network',
               '-fw_cfg', 'name=opt/nexus/cmdline,string=vpad',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{serial}',
               '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-no-reboot']
    (WORK / 'cmd.json').write_text(json.dumps(command, indent=1))
    process = subprocess.Popen(command, stdout=(WORK / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    print('VM', process.pid, flush=True)
    try: aurora_vm.module('gamepad_build', ROOT / 'arch/x86_64/build.py').unthrottle(process.pid)
    except Exception: pass
    q = aurora_vm.Qmp(args.qmp_port)
    def wait(pattern, seconds, after=0):
        end = time.time() + seconds
        while time.time() < end:
            if re.search(pattern, text()[after:]): return
            assert process.poll() is None, 'VM exited'
            assert 'KERNEL_FAULT' not in text(), text()[-3000:]
            time.sleep(.25)
        raise TimeoutError(pattern)
    wait(r'AURORA READY persistent desktop', 300)
    assert '[64] INPUT /dev/input/event2 + /dev/input/js0: Microsoft X-Box 360 pad' in text(), 'no virtual pad'
    time.sleep(3)
    pos = [960, 540]
    def move_to(x, y):
        while pos != [x, y]:
            dx = max(-90, min(90, x - pos[0])); dy = max(-90, min(90, y - pos[1])); events = []
            if dx: events.append({'type': 'rel', 'data': {'axis': 'x', 'value': dx}})
            if dy: events.append({'type': 'rel', 'data': {'axis': 'y', 'value': dy}})
            pos[0] += dx; pos[1] += dy; q.cmd('input-send-event', {'events': events}); time.sleep(.03)
    def click():
        for down in (True, False): q.cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': down}}]}); time.sleep(.15)
    before = len(text())
    move_to(864, 1040); click(); wait(r'AURORA WINDOW app=google-chrome', 240, before)
    print('chrome window open', flush=True)
    end = time.time() + 300
    while state['done'] is None and time.time() < end:
        assert process.poll() is None, 'VM exited'
        time.sleep(.5)
    time.sleep(2)
    try:
        from PIL import Image
        ppm = WORK / 'final.ppm'; q.cmd('screendump', {'filename': str(ppm)})
        with Image.open(ppm) as image: image.convert('RGB').save(WORK / 'final.png')
        ppm.unlink()
    except Exception as error: print('screendump failed', error)
    try: q.cmd('quit')
    except Exception: pass
    try: process.wait(30)
    except subprocess.TimeoutExpired: process.kill(); process.wait()
finally:
    server.shutdown()
    overlay.unlink(missing_ok=True)
log = text()
(WORK / 'results.json').write_text(json.dumps(state['results'], indent=1))
for line in log.splitlines():
    if 'VPAD' in line or '[64] INPUT' in line: print(line)
done = state['done']
assert done, ('page did not finish', state['results'][-3:], state['gets'])
print(json.dumps({k: done[k] for k in ('secure', 'connected', 'id', 'mapping', 'axes', 'buttons', 'seen', 'samples', 'rumble', 'actuator', 'events')}, indent=1))
assert done['secure'], 'page not a secure context'
assert 'Vendor: 045e' in done['id'] and 'Product: 028e' in done['id'], done['id']
assert done['mapping'] == 'standard' and done['buttons'] == 17 and done['axes'] == 4, (done['mapping'], done['buttons'], done['axes'])
assert all(done['seen'].get(k) for k in ('a', 'b', 'lt', 'up', 'right', 'left', 'down')), done['seen']
assert done['rumble'] == 'complete', done['rumble']
rumble = [(int(s), int(w)) for s, w in re.findall(r'\[64\] VPAD rumble strong=(\d+) weak=(\d+)', log)]
assert rumble and any(abs(s - 49151) < 700 and abs(w - 16383) < 700 for s, w in rumble) and rumble[-1] == (0, 0), ('rumble at the pad', rumble)
assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log
print('PASS Chrome Gamepad API:', done['id'], '| standard mapping, pattern seen, dual-rumble', rumble)
