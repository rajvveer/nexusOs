"""Chrome getUserMedia (audio) on native NexusOS: real Google Chrome records 2 s from the ALSA capture device.

    python -I tests/test_x64_capture_chrome.py [--work DIR] [--source DESKTOP64.img] [--device hda|ac97]
                                               [--port 8981] [--qmp-port 4684] [--keep-disk]

A private sparse copy of the fixed desktop64 base gets this tree's desktop.elf as /sbin/init and a Chrome argument
file (fresh profile in /tmp, --use-fake-ui-for-media-stream to grant the microphone without the prompt,
--unsafely-treat-insecure-origin-as-secure for the host page, autoplay without a gesture); it boots through a qcow2
overlay with intel-hda + hda-duplex (or AC'97) on QEMU's "none" audiodev and the kernel option snd.test-capture=tone,
so the microphone delivers a known 1 kHz sine. Chrome is opened from the Aurora dock on
tests/fixtures/capture/getusermedia.html (served from the host), which runs getUserMedia + MediaRecorder (Opus/WebM)
for 2 s, analyses the live input (AnalyserNode) and the decoded recording, uploads the recording and posts the result.
Checks: secure context, an audio track from the ALSA device, live spectrum peak at 1 kHz, the decoded 2 s recording at
1 kHz with the expected level, and the kernel log shows the capture stream opened and closed. The prepared disk and the
overlay are deleted at the end (unless --keep-disk).
"""
from pathlib import Path
import argparse
import http.server
import json
import re
import shutil
import subprocess
import sys
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('--work', type=Path, default=nexus_paths.runs('chrome_capture'))
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--device', default='hda', choices=('hda', 'ac97'))
ap.add_argument('--port', type=int, default=8981); ap.add_argument('--qmp-port', type=int, default=4684)
ap.add_argument('--keep-disk', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
for old in WORK.iterdir():
    if old.is_file() and old.suffix in ('.log', '.qcow2', '.png', '.ppm', '.json', '.webm'): old.unlink()
assert 'New folder' not in str(args.source), 'never boot the real desktop disk'
disk = WORK / 'desktop64.img'
PAGE = (ROOT / 'tests/fixtures/capture/getusermedia.html').read_bytes()
chrome_args = '\n'.join([
    '/home/nexus/Applications/chrome/opt/google/chrome/chrome', '--ozone-platform=wayland', '--disable-gpu', '--no-first-run',
    '--ipc-connection-timeout=120', '--user-data-dir=/tmp/nx-capture-profile', '--use-fake-ui-for-media-stream',
    f'--unsafely-treat-insecure-origin-as-secure=http://10.0.2.2:{args.port}', '--autoplay-policy=no-user-gesture-required',
    f'http://10.0.2.2:{args.port}/capture', ''])

state = {'results': [], 'done': None, 'gets': []}
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        state['gets'].append(self.path)
        if self.path.split('?')[0] == '/capture':
            self.send_response(200); self.send_header('content-type', 'text/html'); self.send_header('content-length', str(len(PAGE))); self.end_headers()
            self.wfile.write(PAGE)
        else: self.send_response(404); self.end_headers()
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('content-length', 0)))
        if self.path == '/upload': (WORK / 'recording.webm').write_bytes(body[:4 << 20])
        else:
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
    if not disk.exists():
        print('sparse copy of', args.source, flush=True); aurora_vm.sparse_copy(args.source, disk)
    aurora_vm.stage(disk, {'sbin/init': OUT / 'desktop.elf', 'nexus/desktop.args': '--aurora\n', 'nexus/chrome-desktop.args': chrome_args})
    shutil.rmtree(WORK / 'staging', ignore_errors=True)
    overlay.unlink(missing_ok=True)
    subprocess.run([aurora_vm.QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(disk), '-F', 'raw', str(overlay)], check=True)
    sound = (['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'] if args.device == 'hda' else ['-device', 'AC97,audiodev=snd0'])
    accel = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', aurora_vm.CPU] if aurora_vm.whpx_usable() else ['-accel', 'tcg', '-cpu', 'max']
    command = [aurora_vm.QEMU, *accel, '-smp', '2', '-m', '2048',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={overlay},format=qcow2,if=ide', '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga',
               '-display', 'none', '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network',
               '-audiodev', 'none,id=snd0', *sound, '-fw_cfg', 'name=opt/nexus/cmdline,string=snd.test-capture=tone',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{serial}',
               '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-no-reboot']
    (WORK / 'cmd.json').write_text(json.dumps(command, indent=1))
    process = subprocess.Popen(command, stdout=(WORK / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    print('VM', process.pid, flush=True)
    try: nexus_paths.unthrottle(process.pid)
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
    wait(r'AURORA READY persistent desktop', 240)
    assert '[64] SOUND capture device ready (test pattern)' in text(), 'no capture device'
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
    before = len(text()); move_to(864, 1040); click()
    wait(r'AURORA WINDOW app=google-chrome', 240, before)
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
    if not args.keep_disk: disk.unlink(missing_ok=True)
    shutil.rmtree(WORK / 'staging', ignore_errors=True)
log = text()
(WORK / 'results.json').write_text(json.dumps(state['results'], indent=1))
for line in log.splitlines():
    if any(k in line for k in ('SOUND', 'HDA', 'AC97', 'ALSA')): print(line)
done = state['done']
print(json.dumps(done, indent=1))
assert done and done.get('ok'), ('page failed', done, state['gets'])
assert done['secure'], 'page not a secure context'
settings = done['track']['settings']
assert done['track']['readyState'] == 'live' and done['track']['label'], done['track']
assert abs(done['live']['peak_hz'] - 1000) < 15 and done['live']['peak_db'] > -60, ('live spectrum', done['live'])   # AnalyserNode: -40 dB here
assert any('Capture' in d for d in done['devices']), ('ALSA capture devices not listed', done['devices'])
decoded = done['decoded']
assert 1.7 <= decoded['duration_s'] <= 2.4 and abs(decoded['frequency'] - 1000) < 20, ('recording', decoded)
assert 0.12 < decoded['rms'] < 0.25 and 0.18 < decoded['peak'] < 0.4, ('level (sine at 8192/32768: rms 0.177)', decoded)
assert done['recording']['bytes'] > 2000 and 'webm' in done['recording']['mimeType'], done['recording']
assert (WORK / 'recording.webm').exists() and (WORK / 'recording.webm').stat().st_size == done['recording']['bytes']
assert 'SOUND close capture' in log, 'capture stream not closed'
assert 'KERNEL_FAULT' not in log
print('PASS Chrome getUserMedia + MediaRecorder recorded', round(decoded['duration_s'], 2), 's at', round(decoded['frequency'], 1), 'Hz,',
      'rate', settings.get('sampleRate'), 'channels', settings.get('channelCount'), 'via', args.device)
