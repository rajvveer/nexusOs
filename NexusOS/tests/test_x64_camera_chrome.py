"""getUserMedia (video) on native NexusOS: real Google Chrome or Firefox shows NexusOS's virtual camera (V4L2, vcam.c).

    python -I tests/test_x64_camera_chrome.py [--browser chrome|firefox] [--work DIR] [--disk DESKTOP64.img] [--port 9152] [--qmp-port 4752]

Firefox (from Spotlight, /nexus/apps/firefox.args) gets a fresh profile whose user.js grants the camera without the
prompt (media.navigator.permission.disabled) and allows getUserMedia on the http host page; it captures through
libwebrtc's V4L2 backend (PipeWire camera portal off: no portal here).

--disk is a desktop disk with Chrome, used read only (default: the integrator's D:\\nexustest\\prod\\integ_desk_disk\\desktop64.img);
through tests/overlay_stage.py this tree's desktop.elf (/sbin/init), aurora_app.elf
(/bin/aurora-app) and a Chrome argument file (fresh profile in /tmp, --use-fake-ui-for-media-stream to grant the camera
without the prompt, the host page treated as secure) are staged into it, then it boots through a qcow2 overlay with
the kernel option vcam. Chrome is opened from the Aurora dock on tests/fixtures/camera/getusermedia_video.html (served
from the host), which captures 640x480 at 30 fps and reads every presented frame back from a canvas.
Checks: secure context, the camera listed with its V4L2 card name, a live 640x480 track, the eight colour bars in the
right colours, the frame counter increasing (at most 3 repeated presentations) with at least 15 distinct frames per second over 4 s (Chrome
renders in software here), and the kernel log shows the V4L2 device. The overlay is deleted at the end.
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
ap.add_argument('--port', type=int, default=9152); ap.add_argument('--qmp-port', type=int, default=4752)
ap.add_argument('--browser', default='chrome', choices=('chrome', 'firefox'))
args = ap.parse_args()
WORK = (args.work or Path(r'D:\nexustest\wave2\ws_hw2_runs') / ('camera_' + args.browser)).resolve(); WORK.mkdir(parents=True, exist_ok=True)
for old in WORK.iterdir():
    if old.is_file() and old.suffix in ('.log', '.qcow2', '.png', '.ppm', '.json'): old.unlink()
assert 'New folder' not in str(args.disk), 'never boot the real desktop disk'
disk = args.disk
PAGE = (ROOT / 'tests/fixtures/camera/getusermedia_video.html').read_bytes()
chrome_args = '\n'.join([
    '/home/nexus/Applications/chrome/opt/google/chrome/chrome', '--ozone-platform=wayland', '--disable-gpu', '--no-first-run',
    '--ipc-connection-timeout=120', '--user-data-dir=/tmp/nx-camera-profile', '--use-fake-ui-for-media-stream',
    f'--unsafely-treat-insecure-origin-as-secure=http://10.0.2.2:{args.port}', '--autoplay-policy=no-user-gesture-required',
    f'http://10.0.2.2:{args.port}/camera', ''])
firefox_args = '\n'.join(['MOZ_ENABLE_WAYLAND=1', 'MOZ_CRASHREPORTER_DISABLE=1', '/opt/firefox/firefox-bin', '--profile', '/home/nexus/ff-camera-profile',
                          '--no-remote', f'http://10.0.2.2:{args.port}/camera', ''])
firefox_prefs = (ROOT / 'arch/x86_64/apps-config/firefox-user.js').read_text() + ''.join(f'user_pref("{k}", {v});\n' for k, v in (
    ('media.navigator.permission.disabled', 'true'), ('media.devices.insecure.enabled', 'true'), ('media.getusermedia.insecure.enabled', 'true'),
    ('media.webrtc.camera.allow-pipewire', 'false'), ('media.navigator.streams.fake', 'false')))
QCODE = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', ':': 'semicolon'}

state = {'results': [], 'done': None, 'gets': []}
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        state['gets'].append(self.path)
        if self.path.split('?')[0] == '/camera':
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
    files = {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n',
             'nexus/chrome-desktop.args': chrome_args}
    if args.browser == 'firefox': files.update({'nexus/apps/firefox.args': firefox_args, 'home/nexus/ff-camera-profile/user.js': firefox_prefs})
    overlay_stage.stage(disk, overlay, files)   # the desktop disk itself is never written
    accel = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', aurora_vm.CPU] if aurora_vm.whpx_usable() else ['-accel', 'tcg', '-cpu', 'max']
    command = [aurora_vm.QEMU, *accel, '-smp', '2', '-m', '2048',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={overlay},format=qcow2,if=ide', '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga',
               '-display', 'none', '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network',
               '-fw_cfg', 'name=opt/nexus/cmdline,string=vcam',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{serial}',
               '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-no-reboot']
    (WORK / 'cmd.json').write_text(json.dumps(command, indent=1))
    process = subprocess.Popen(command, stdout=(WORK / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    print('VM', process.pid, flush=True)
    try: aurora_vm.module('camera_build', ROOT / 'arch/x86_64/build.py').unthrottle(process.pid)
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
    assert '[64] V4L2 /dev/video0: NexusOS Virtual Camera' in text(), 'no camera'
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
    def key(name, down): q.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]})
    def tap(*names):
        for name in names: key(name, True)
        for name in reversed(names): key(name, False)
        time.sleep(.08)
    before = len(text())
    if args.browser == 'chrome':
        move_to(864, 1040); click(); wait(r'AURORA WINDOW app=google-chrome', 240, before)
    else:
        tap('ctrl', 'spc'); time.sleep(1.5)
        for ch in 'firefox': tap(QCODE.get(ch, ch)); time.sleep(.03)
        time.sleep(1); tap('ret'); wait(r'AURORA WINDOW app=\S*firefox', 600, before)
    print(args.browser, 'window open', flush=True)
    end = time.time() + 300
    while state['done'] is None and time.time() < end:
        assert process.poll() is None, 'VM exited'
        time.sleep(.5)
    time.sleep(1)
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
    if 'V4L2' in line or 'VCAM' in line: print(line)
done = state['done']
assert done and done.get('ok'), ('page failed', done, state['gets'])
frames = done['frames']
summary = {k: done[k] for k in ('secure', 'track', 'devices')}
print(json.dumps(summary, indent=1), len(frames), 'frames sampled')
assert done['secure'] or args.browser == 'firefox', 'page not a secure context'   # Firefox: insecure getUserMedia allowed by the profile
assert done['track']['readyState'] == 'live' and 'NexusOS Virtual Camera' in done['track']['label'], done['track']
assert any('NexusOS Virtual Camera' in d for d in done['devices']), done['devices']
settings = done['track']['settings']
assert settings.get('width') == 640 and settings.get('height') == 480, settings
# BT.601 limited-range bars -> RGB: white, yellow, cyan, green, magenta, red, blue, black (100 % bars)
expected = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
good = [f for f in frames if all(max(abs(a - b) for a, b in zip(bar, want)) <= 48 for bar, want in zip(f['bars'], expected))]
counters = [f['counter'] for f in good]
if done.get('callback') == 'requestAnimationFrame': counters = [c for i, c in enumerate(counters) if not i or c != counters[i - 1]]   # rAF samples repeat frames
increasing = all(b >= a for a, b in zip(counters, counters[1:])) and sum(b == a for a, b in zip(counters, counters[1:])) <= 3   # a repeated presentation now and then
span_s = (good[-1]['t'] - good[0]['t']) / 1000 if len(good) > 1 else 0
distinct_rate = (len(set(counters)) - 1) / span_s if span_s else 0
print('frames with correct bars', len(good), 'of', len(frames), 'counters', counters[:5], '...', counters[-3:], 'distinct/s', round(distinct_rate, 1))
assert len(good) >= 0.9 * len(frames) and len(counters) >= 40, ('bar colours / frames', frames[:3], len(counters))
assert increasing, ('frame counter must increase (at most 3 repeats)', counters)
assert distinct_rate >= 15, ('frame rate', distinct_rate)
assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log
print('PASS', args.browser, 'getUserMedia video from /dev/video0:', len(good), 'frames,', round(distinct_rate, 1), 'distinct frames/s, track', settings)
