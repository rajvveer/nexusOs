"""Shared helpers for Aurora protocol/Xwayland VM tests (test_aurora_wine_protocols.py, test_xwayland.py).

A desktop64 disk prepared by the caller (sparse copy + staged files) is booted through a throw-away qcow2 overlay, so
the prepared image itself is never written by the guest. QMP runs over TCP; the guest serial log is a file in WORK.
"""
from pathlib import Path
import importlib.util
import json
import re
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def sparse_copy(source, destination):
    """Sparse copy of a raw disk image (destination must not exist)."""
    source, destination = Path(source), Path(destination)
    if destination.exists(): raise FileExistsError(destination)
    destination.parent.mkdir(parents=True, exist_ok=True); destination.touch()
    subprocess.run(['fsutil', 'sparse', 'setflag', str(destination)], check=True, stdout=subprocess.DEVNULL)
    with source.open('rb') as a, destination.open('r+b') as b:
        b.truncate(source.stat().st_size)
        while block := a.read(1 << 20):
            if block.strip(b'\0'): b.write(block)
            else: b.seek(len(block), 1)


def stage(disk, files):
    """Write host files into the FAT volume of disk in place: files = {guest path: host path or bytes}."""
    fatput = module('aurora_vm_fatput', ROOT / 'userland/fatput.py')
    work = Path(disk).parent / 'staging'; work.mkdir(exist_ok=True)
    args = ['fatput', str(disk)]
    for guest, host in files.items():
        if isinstance(host, (bytes, str)) and not isinstance(host, Path):
            data = host.encode() if isinstance(host, str) else host
            host = work / guest.replace('/', '_'); host.write_bytes(data)
        args += ['put', str(host), guest.strip('/')]
    fatput.edit(args)


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


def whpx_usable():
    build = module('aurora_vm_build', ROOT / 'arch/x86_64/build.py')
    return build.whpx_available()


class Vm:
    """One boot of disk (through an overlay in work). Pointer position is tracked from Aurora's start (screen centre)."""
    def __init__(self, work, disk, qmp_port, accel='auto', smp=2, memory='2048', width=1920, height=1080):
        self.work = Path(work); self.work.mkdir(parents=True, exist_ok=True)
        for old in self.work.iterdir():
            if old.is_file() and old.suffix in ('.log', '.qcow2', '.png', '.ppm', '.json'): old.unlink()
        self.serial, self.overlay = self.work / 'serial.log', self.work / 'overlay.qcow2'
        subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(Path(disk).resolve()), '-F', 'raw', str(self.overlay)], check=True)
        if accel == 'auto': accel = 'whpx' if whpx_usable() else 'tcg'
        self.accel = accel
        acceleration = ['-accel', 'tcg', '-cpu', 'max'] if accel == 'tcg' else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
        command = [QEMU, *acceleration, '-smp', str(smp), '-m', memory,
                   '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
                   '-drive', f'file={self.overlay},format=qcow2,if=ide', '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga',
                   '-display', 'none', '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{self.serial}',
                   '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off', '-no-reboot']
        (self.work / 'cmd.json').write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, stdout=(self.work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
        print('VM', self.proc.pid, accel, flush=True)
        try: module('aurora_vm_nexus_paths', ROOT / 'arch/x86_64/nexus_paths.py').unthrottle(self.proc.pid)
        except Exception: pass
        self.q = Qmp(qmp_port); self.pos = [width // 2, height // 2]

    def text(self): return self.serial.read_bytes().decode('latin1') if self.serial.exists() else ''

    def wait(self, pattern, after=0, seconds=300):
        end = time.time() + seconds
        while time.time() < end:
            log = self.text(); match = re.search(pattern, log[after:])
            if match: return match
            failed = re.search(r'\[64\] (WPTEST|XTEST) FAIL[^\n]*', log)
            if failed: raise AssertionError(failed.group(0))
            if self.proc.poll() is not None: raise AssertionError('VM exited while waiting for ' + pattern)
            time.sleep(.2)
        raise TimeoutError(pattern)

    def move(self, dx, dy, step=40, delay=.03):
        while dx or dy:
            sx = max(-step, min(step, dx)); sy = max(-step, min(step, dy)); events = []
            if sx: events.append({'type': 'rel', 'data': {'axis': 'x', 'value': sx}})
            if sy: events.append({'type': 'rel', 'data': {'axis': 'y', 'value': sy}})
            self.q.cmd('input-send-event', {'events': events}); dx -= sx; dy -= sy; time.sleep(delay)

    def move_to(self, x, y):
        self.move(x - self.pos[0], y - self.pos[1]); self.pos = [x, y]

    def button(self, name, down):
        self.q.cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'button': name, 'down': down}}]}); time.sleep(.12)

    def click(self, name='left'):
        self.button(name, True); self.button(name, False)

    def key(self, name, down):
        self.q.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]}); time.sleep(.06)

    def tap(self, *names):
        for name in names: self.key(name, True)
        for name in reversed(names): self.key(name, False)
        time.sleep(.08)

    def type(self, text):
        shifted = {':': 'semicolon', '_': 'minus', '>': 'dot', '"': 'apostrophe', '|': 'backslash', '!': '1', '&': '7'}
        plain = {' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash', '=': 'equal', ';': 'semicolon', "'": 'apostrophe', ',': 'comma'}
        for ch in text:
            if ch in shifted: self.key('shift', True); self.tap(shifted[ch]); self.key('shift', False)
            elif ch.isupper(): self.key('shift', True); self.tap(ch.lower()); self.key('shift', False)
            else: self.tap(plain.get(ch, ch))

    def shot(self, label):
        """Screen dump as PNG (PIL) and RGB image object."""
        from PIL import Image
        ppm = self.work / f'{label}.ppm'
        self.q.cmd('screendump', {'filename': str(ppm)})
        with Image.open(ppm) as image: picture = image.convert('RGB')
        picture.save(self.work / f'{label}.png'); ppm.unlink(); return picture

    def faults(self):
        log = self.text()
        return [line for line in log.splitlines() if 'KERNEL_FAULT' in line or 'USER_FAULT' in line]

    def close(self):
        if self.proc.poll() is None:
            try: self.q.cmd('quit')
            except Exception: pass
            try: self.proc.wait(30)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()
        self.overlay.unlink(missing_ok=True)
