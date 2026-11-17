"""Desktop VM harness for the multi-monitor / KMS / Aurora desktop tests (desk agent).

- DeskVm boots nexus64.img with a disk through a throw-away qcow2 overlay (the given image is never written), a
  multi-head virtio-vga (max_outputs=N, EDID on), PS/2 or USB input, QMP over TCP and the serial log in WORK.
- Heads other than 0 start disabled in QEMU without a UI; each gets its own VNC server (display=video0,head=N) whose
  RFB SetDesktopSize request is QEMU's "UI resized" path (dpy_set_ui_info): a size enables the head (hot-plug, with
  that preferred size and EDID), 0x0 disables it (unplug). Vnc implements just enough RFB 3.8 for that.
- shot(label, head) takes a QMP screendump of one head.
"""
from pathlib import Path
import json
import os
import re
import socket
import struct
import subprocess
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'
QMP_PORTS = range(4660, 4670)   # desk agent port ranges (AGENT_COMMON_PROD.md)
VNC_PORTS = range(8960, 8970)
if os.environ.get('NEXUS_DESK_PORTS'):   # "QMP-FIRST,VNC-FIRST": a range of 10 each (several agents on one host)
    _qmp, _vnc = (int(v) for v in os.environ['NEXUS_DESK_PORTS'].split(','))
    QMP_PORTS, VNC_PORTS = range(_qmp, _qmp + 10), range(_vnc, _vnc + 10)


def module(name, path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def free_port(ports):
    for port in ports:
        with socket.socket() as s:
            try: s.bind(('127.0.0.1', port)); return port
            except OSError: continue
    raise RuntimeError('no free port in ' + str(ports))


class Vnc:
    """Minimal RFB 3.8 client (security None) for SetDesktopSize only."""
    def __init__(self, port, timeout=30):
        deadline = time.time() + timeout
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=10); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(.2)
        version = self._read(12)
        assert version.startswith(b'RFB 003.'), version
        self.s.sendall(b'RFB 003.008\n')
        count = self._read(1)[0]
        types = self._read(count) if count else b''
        assert 1 in types, ('VNC security types', types)
        self.s.sendall(b'\x01')
        assert struct.unpack('>I', self._read(4))[0] == 0, 'VNC security failed'
        self.s.sendall(b'\x01')   # ClientInit: shared
        width, height = struct.unpack('>HH', self._read(4)); self._read(16); name = self._read(struct.unpack('>I', self._read(4))[0])
        self.size = (width, height); self.name = name
        self.s.sendall(struct.pack('>BBHi', 2, 0, 1, -308))   # SetEncodings: ExtendedDesktopSize

    def _read(self, n):
        data = b''
        while len(data) < n:
            block = self.s.recv(n - len(data))
            if not block: raise EOFError('VNC closed')
            data += block
        return data

    def set_desktop_size(self, width, height):
        """RFB SetDesktopSize (251) with one screen; QEMU forwards it as the head's UI size (0x0: disabled)."""
        self.s.sendall(struct.pack('>BBHHBB', 251, 0, width, height, 1, 0) + struct.pack('>IHHHHI', 0, 0, 0, width, height, 0))

    def close(self):
        try: self.s.close()
        except OSError: pass


class Qmp:
    def __init__(self, port, timeout=60):
        deadline = time.time() + timeout
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


class DeskVm:
    def __init__(self, work, disk, heads=2, accel='tcg', smp=2, memory='2048', machine='pc', usb=False, extra=(), vnc_heads=None,
                 device='virtio-vga', raw_disk=False, xres=None, yres=None, keep_overlay=False, plug=None, overlay=None):
        """keep_overlay: reuse the overlay from an earlier boot (guest writes persist between boots) and keep it; overlay: its
        path (default WORK/overlay.qcow2). plug: {head: (w, h)} enabled over VNC right after QEMU starts (before the guest
        probes its displays)."""
        self.work = Path(work); self.work.mkdir(parents=True, exist_ok=True)
        for old in self.work.iterdir():
            if old.is_file() and old.suffix in ('.log', '.png', '.ppm', '.json') or (old.suffix == '.qcow2' and not keep_overlay): old.unlink()
        self.serial = self.work / 'serial.log'
        self.overlay = None; self.keep_overlay = keep_overlay
        if raw_disk: drive = ['-drive', f'file={Path(disk).resolve()},format=raw,if=ide']
        else:
            self.overlay = Path(overlay) if overlay else self.work / 'overlay.qcow2'
            if not self.overlay.exists():
                subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(Path(disk).resolve()), '-F', 'raw', str(self.overlay)], check=True)
            drive = ['-drive', f'file={self.overlay},format=qcow2,if=ide']
        acceleration = ['-accel', 'tcg', '-cpu', 'max'] if accel == 'tcg' else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
        self.qmp_port = free_port(QMP_PORTS)
        gpu = f'{device},id=video0,max_outputs={heads},edid=on' + (f',xres={xres},yres={yres}' if xres else '')
        self.vnc_ports = {}
        vnc = []
        for head in (vnc_heads if vnc_heads is not None else range(1, heads)):
            port = free_port([p for p in VNC_PORTS if p not in self.vnc_ports.values()])
            self.vnc_ports[head] = port
            vnc += ['-vnc', f'127.0.0.1:{port - 5900},id=head{head},display=video0,head={head}']
        command = [QEMU, *acceleration, '-machine', machine, '-smp', str(smp), '-m', memory,
                   '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', *drive, '-boot', 'a',
                   '-vga', 'none', '-device', gpu, '-display', 'none', *vnc,
                   *(['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0,display=video0'] if usb else []),
                   '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{self.serial}',
                   '-qmp', f'tcp:127.0.0.1:{self.qmp_port},server=on,wait=off', '-no-reboot', *extra]
        (self.work / 'cmd.json').write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, stdout=(self.work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
        print('VM', self.proc.pid, accel, 'heads', heads, 'qmp', self.qmp_port, 'vnc', self.vnc_ports, flush=True)
        try: nexus_paths.unthrottle(self.proc.pid)
        except Exception: pass
        self.vnc = {}
        for head, (w, h) in (plug or {}).items(): self.head_size(head, w, h)
        self.q = Qmp(self.qmp_port)
        self.pos = [960, 540]; self.usb = usb

    def text(self): return self.serial.read_bytes().decode('latin1') if self.serial.exists() else ''

    def wait(self, pattern, after=0, seconds=300, fail=r'\[64\] (KMS|DESK|WPTEST|XTEST) FAIL[^\n]*'):
        end = time.time() + seconds
        while time.time() < end:
            log = self.text(); match = re.search(pattern, log[after:])
            if match: return match
            failed = re.search(fail, log) if fail else None
            if failed: raise AssertionError(failed.group(0))
            if 'KERNEL_FAULT' in log or 'USER_FAULT' in log: raise AssertionError('fault: ' + log[-3000:])
            if self.proc.poll() is not None: raise AssertionError('VM exited while waiting for ' + pattern + '\n' + log[-3000:])
            time.sleep(.15)
        raise TimeoutError(pattern)

    def head_size(self, head, width, height):
        """Hot-plug (width x height) or unplug (0x0) a head through its VNC server."""
        if head not in self.vnc: self.vnc[head] = Vnc(self.vnc_ports[head])
        self.vnc[head].set_desktop_size(width, height)

    def key(self, name, down):
        self.q.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]}); time.sleep(.06)

    def tap(self, *names):
        for name in names: self.key(name, True)
        for name in reversed(names): self.key(name, False)
        time.sleep(.08)

    def move(self, dx, dy, step=40, delay=.03):
        while dx or dy:
            sx = max(-step, min(step, dx)); sy = max(-step, min(step, dy)); events = []
            if sx: events.append({'type': 'rel', 'data': {'axis': 'x', 'value': sx}})
            if sy: events.append({'type': 'rel', 'data': {'axis': 'y', 'value': sy}})
            self.q.cmd('input-send-event', {'events': events}); dx -= sx; dy -= sy; time.sleep(delay)

    def move_to(self, x, y):
        self.move(x - self.pos[0], y - self.pos[1]); self.pos = [x, y]

    head0 = (0, 0, 1920, 1080)   # desktop rectangle of head 0 (the USB tablet's console), kept up to date by the test

    def home(self, x, y):
        """Absolute (USB tablet) move to desktop point x,y, which must lie on head 0: resynchronises the tracked pointer."""
        assert self.usb, 'absolute positioning needs the USB tablet (usb=True)'
        ox, oy, w, h = self.head0
        assert ox <= x < ox + w and oy <= y < oy + h, ('not on head 0', x, y, self.head0)
        ax, ay = round((x - ox) * 32767 / (w - 1)), round((y - oy) * 32767 / (h - 1))
        # The tablet reports only axes that changed: a nudge first makes the guest see both axes of the target.
        for vx, vy in ((ax + 64 if ax < 32000 else ax - 64, ay + 64 if ay < 32000 else ay - 64), (ax, ay)):
            events = [{'type': 'abs', 'data': {'axis': 'x', 'value': vx}}, {'type': 'abs', 'data': {'axis': 'y', 'value': vy}}]
            self.q.cmd('input-send-event', {'device': 'video0', 'head': 0, 'events': events}); time.sleep(.08)   # the tablet is bound to head 0
        time.sleep(.1); self.pos = [x, y]

    def goto(self, x, y):
        """Desktop point x,y anywhere: an absolute jump to the nearest point of head 0, then relative motion across."""
        ox, oy, w, h = self.head0
        self.home(min(max(x, ox), ox + w - 1), min(max(y, oy), oy + h - 1))
        self.move_to(x, y)

    def drag(self, x0, y0, x1, y1, home=True):
        """Press at desktop x0,y0 (absolute when home: on head 0), move relatively to x1,y1, release."""
        if home: self.goto(x0, y0)
        else: self.move_to(x0, y0)
        self.button('left', True); self.move_to(x1, y1); time.sleep(.2); self.button('left', False)

    def button(self, name, down):
        self.q.cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'button': name, 'down': down}}]}); time.sleep(.12)

    def click(self, name='left'):
        self.button(name, True); self.button(name, False)

    def type(self, text):
        shifted = {':': 'semicolon', '_': 'minus', '>': 'dot', '"': 'apostrophe', '|': 'backslash', '!': '1', '&': '7', '?': 'slash', '@': '2', '+': 'equal'}
        plain = {' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash', '=': 'equal', ';': 'semicolon', "'": 'apostrophe', ',': 'comma'}
        for ch in text:
            if ch in shifted: self.key('shift', True); self.tap(shifted[ch]); self.key('shift', False)
            elif ch.isupper(): self.key('shift', True); self.tap(ch.lower()); self.key('shift', False)
            else: self.tap(plain.get(ch, ch))

    def shot(self, label, head=0):
        from PIL import Image
        ppm = self.work / f'{label}.ppm'
        arguments = {'filename': str(ppm)}
        if head: arguments.update({'device': 'video0', 'head': head})
        self.q.cmd('screendump', arguments)
        with Image.open(ppm) as image: picture = image.convert('RGB')
        picture.save(self.work / f'{label}.png'); ppm.unlink(); return picture

    def frame(self, label, predicate, head=0, seconds=60):
        end = time.time() + seconds; picture = None
        while time.time() < end:
            picture = self.shot(label, head)
            if predicate(picture): return picture
            if self.proc.poll() is not None: raise AssertionError('VM exited before ' + label)
            time.sleep(.3)
        raise AssertionError(f'screen condition not met: {label} (last size {picture.size if picture else None})')

    def close(self):
        for v in self.vnc.values(): v.close()
        if self.proc.poll() is None:
            try: self.q.cmd('quit')
            except Exception: pass
            try: self.proc.wait(30)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()
        if self.overlay and not self.keep_overlay: self.overlay.unlink(missing_ok=True)
