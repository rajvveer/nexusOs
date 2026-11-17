"""Native Intel NIC and xHCI USB drivers, on TCG and WHPX.

Usage: python -I tests/test_x64_usbnet.py [WORK] [--accel tcg|whpx|both] [--cases NAME,...]
Cases:
  net-e1000e, net-e1000, net-igb  the exact 2 MiB TCP stream campaign (/nexus/test-network) over that NIC
  net-order                       e1000e and RTL8139 both present: the Intel NIC must be chosen
  usb-input                       qemu-xhci + usb-kbd + usb-tablet + usb-mouse, no i8042: QMP input-send-event
                                  routed to the USB devices reaches /dev/input (bin usbinput test program)
  usb-nec                         the same through nec-usb-xhci (USB3 ports first)
  usb-hub                         usb-hub (port power switched) with a keyboard behind it, then hot-unplug and
                                  hot-plug of keyboards/tablets through QMP device_del/device_add
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import socket
import struct
import subprocess
import threading
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
CASES = ['net-e1000e', 'net-e1000', 'net-igb', 'net-order', 'usb-input', 'usb-nec', 'usb-hub']
parser = argparse.ArgumentParser()
parser.add_argument('work', nargs='?', default=str(nexus_paths.runs('native64-usbnet')))
parser.add_argument('--accel', default='both', choices=['tcg', 'whpx', 'both'])
parser.add_argument('--cases', default=','.join(CASES))
options = parser.parse_args()
WORK = Path(options.work); WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('usbnet_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
spec = importlib.util.spec_from_file_location('usbnet_build', ROOT / 'arch/x86_64/build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)


def cpu_args(accel):
    if accel == 'whpx':
        return ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand' + build.host_cpu_flags()]
    return ['-cpu', 'max']


def base_args(serial, disk, memory=256):
    return [QEMU, '-m', str(memory), '-smp', '2', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
            '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none', '-vga', 'none',
            '-device', 'virtio-vga,id=video0', '-serial', f'file:{serial}', '-no-reboot',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']


def check_clean(log):
    assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and '[64] FAIL' not in log and 'PANIC' not in log.upper().replace('PANIC_', ''), log[-4000:]


def network_case(accel, devices, expect, served=0):
    work = WORK / f'{accel}-{"-".join(devices)}'; work.mkdir(exist_ok=True)
    server = socket.socket(); server.bind(('127.0.0.1', 0)); server.listen(1); server.settimeout(150)
    port = server.getsockname()[1]; errors, finished = [], []

    def serve():
        try:
            with server:
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(150); request = b''
                    while not request.endswith(b'\n') and len(request) < 64:
                        data = connection.recv(64 - len(request))
                        if not data: raise AssertionError('guest closed before request')
                        request += data
                    assert request == b'NEXUS64-NET\n', request
                    block = bytes((i * 37 + 11) & 255 for i in range(65536))
                    for _ in range(32): connection.sendall(block)
                    connection.shutdown(socket.SHUT_WR)
                    assert connection.recv(1) == b'', 'guest sent unexpected trailing data'
                    finished.append(True)
        except BaseException as error: errors.append(repr(error))
    thread = threading.Thread(target=serve, daemon=True); thread.start()
    disk = work / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    marker = work / 'port'; marker.write_bytes(struct.pack('<H', port))
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-network'])
    before = hashlib.sha256(disk.read_bytes()).digest()
    serial = work / 'serial.log'; serial.unlink(missing_ok=True)
    args = base_args(serial, disk, 128) + cpu_args(accel)
    for index, device in enumerate(devices):  # PCI slot order = list order; each NIC has its own user network
        args += ['-netdev', f'user,id=net{index}', '-device', f'{device},netdev=net{index}',
                 '-object', f'filter-dump,id=capture{index},netdev=net{index},file={work / f"network{index}.pcap"}']
    started = time.time()
    try:
        result = subprocess.run(args, capture_output=True, timeout=170)
        log = serial.read_text(errors='replace'); thread.join(timeout=5)
        assert result.returncode == 33 and '[64] NETWORK CAMPAIGN PASSED' in log, (result.stdout, result.stderr, log[-4000:])
        assert '[64] NATIVE NETWORK 2 MIB STREAM PASSED' in log and expect in log, log[-4000:]
        check_clean(log)
        assert finished and not errors, (finished, errors)
        assert hashlib.sha256(disk.read_bytes()).digest() == before, 'network DMA/test altered application disk'
        captured = [(work / f'network{index}.pcap').stat().st_size for index in range(len(devices))]
        assert captured[served] > 2 * 1024 * 1024 and all(size < 65536 for index, size in enumerate(captured) if index != served), captured
        if 'Intel' in expect: assert 'RTL8139 native DMA/IRQ ready' not in log, log[-4000:]
    finally:
        server.close()
        disk.unlink(missing_ok=True)  # keep only logs: each case's disk copy goes as soon as it is checked
        for index in range(len(devices)): (work / f'network{index}.pcap').unlink(missing_ok=True)
    print(f'PASS {accel} network campaign over {" + ".join(devices)} ({expect!r}) in {time.time() - started:.1f}s', flush=True)


NETWORK = {'net-e1000e': (['e1000e'], 'Intel 82574L (e1000e) native DMA/IRQ ready'),
           'net-e1000': (['e1000'], 'Intel 82540EM (e1000) native DMA/IRQ ready'),
           'net-igb': (['igb'], 'Intel 82576 (igb) native DMA/IRQ ready'),
           'net-order': (['rtl8139', 'e1000e'], 'Intel 82574L (e1000e) native DMA/IRQ ready')}

class Qmp:
    def __init__(self, port, proc):
        deadline = time.time() + 30
        while True:
            try: self.socket = socket.create_connection(('127.0.0.1', port), timeout=20); break
            except OSError:
                if time.time() > deadline or proc.poll() is not None: raise
                time.sleep(.2)
        self.file = self.socket.makefile('rwb'); self.read(); self.cmd('qmp_capabilities')

    def read(self):
        while True:
            line = self.file.readline()
            if not line: raise EOFError('QMP closed')
            message = json.loads(line)
            if 'event' not in message: return message

    def cmd(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None: request['arguments'] = arguments
        self.file.write(json.dumps(request).encode() + b'\n'); self.file.flush()
        reply = self.read()
        if 'error' in reply: raise RuntimeError(f'{name}: {reply["error"]}')
        return reply.get('return')

    def send(self, *events):
        # 'device' routes the events to the input handlers bound to that display (the USB HID devices below
        # use display=video0), never to the PS/2 devices.
        self.cmd('input-send-event', {'device': 'video0', 'events': list(events)}); time.sleep(.05)

    def key(self, name, down): self.send({'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}})

    def tap(self, *names):
        for name in names: self.key(name, True)
        for name in reversed(names): self.key(name, False)

    def close(self):
        try: self.socket.close()
        except OSError: pass


def usbinput_elf():
    elf = WORK / 'usbinput.elf'
    if not elf.exists() or elf.stat().st_mtime < (ROOT / 'arch/x86_64/usbinput_user.c').stat().st_mtime:
        subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'arch/x86_64/usbinput_user.c'), '-o', str(elf)], check=True)
    return elf


class Session:
    """QEMU running the /dev/input probe as /sbin/init; serial-log expectations with a moving cursor."""
    def __init__(self, accel, tag, devices, qmp_port):
        self.work = WORK / f'{accel}-{tag}'; self.work.mkdir(exist_ok=True)
        disk = self.disk = self.work / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
        fatput.main(['fatput', str(disk), 'put', str(usbinput_elf()), 'sbin/init'])
        self.serial = self.work / 'serial.log'; self.serial.unlink(missing_ok=True)
        # i8042=off: no PS/2 controller at all, so every key and pointer event must come through USB.
        args = base_args(self.serial, disk) + cpu_args(accel) + ['-machine', 'pc,i8042=off', '-net', 'none', *devices,
                                                                  '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off']
        (self.work / 'cmd.json').write_text(json.dumps(args, indent=1))
        self.proc = subprocess.Popen(args, stdout=(self.work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
        self.cursor = 0; self.timeout = 240 if accel == 'tcg' else 120
        self.expect(r'\[64\] USBINPUT READY')
        self.qmp = Qmp(qmp_port, self.proc)

    def log(self): return self.serial.read_bytes().decode('latin1') if self.serial.exists() else ''

    def expect(self, pattern, seconds=None, advance=True):
        deadline = time.time() + (seconds or self.timeout)
        while time.time() < deadline:
            match = re.search(pattern, self.log()[self.cursor:])
            if match:
                if advance: self.cursor += match.end()
                return match
            if self.proc.poll() is not None: break
            time.sleep(.1)
        raise AssertionError(f'{self.work.name}: missing {pattern!r} after offset {self.cursor}\n{self.log()[-5000:]}')

    def event(self, node, kind, code, value, seconds=None):
        return self.expect(rf'\[64\] USBINPUT {node} type={kind} code={code} value={value}\n', seconds)

    def finish(self):
        self.qmp.tap('esc')
        self.expect(r'\[64\] USBINPUT DONE')
        self.expect(r'\[64\] INIT SESSION RETURNED')
        self.qmp.close()
        assert self.proc.wait(timeout=60) == 33, self.proc.returncode
        check_clean(self.log())

    def pool(self, pages):
        used = int(self.expect(r'\[64\] USB pool pages in use=(\d+)').group(1))
        assert used == pages, (used, pages)

    def kill(self):
        if self.proc.poll() is None: self.proc.kill(); self.proc.wait()
        self.disk.unlink(missing_ok=True)


def usb_input_case(accel, controller):
    nec = controller == 'nec-usb-xhci'
    session = Session(accel, 'usb-nec' if nec else 'usb-input', [
        '-device', f'{controller},id=xhci',
        '-device', 'usb-kbd,bus=xhci.0,display=video0,id=kbd',
        '-device', 'usb-tablet,bus=xhci.0,display=video0,id=tablet',
        # usb-mouse has no display property: QMP REL events reach it as the only unbound REL handler
        '-device', 'usb-mouse,bus=xhci.0,id=mouse'], 4531 if nec else 4530)
    try:
        log = session.log()
        assert 'PS/2 keyboard ready' not in log and 'PS/2 mouse ready' not in log, log
        assert ('(NEC) ready' in log) == nec and 'usb2=4' in log and 'usb3 ports=4' in log, log
        assert 'keyboard ready (boot protocol)' in log and 'absolute pointer ready' in log and 'relative pointer ready' in log, log
        assert re.search(r'USBINPUT CAPS keyboard name="Nexus USB Keyboard" ev=0x03 keys=\d{3}', log), log
        assert re.search(r'USBINPUT CAPS pointer name="Nexus USB Pointer" ev=0x0f keys=8 btn_left=1 rel=0x0143 abs=0x03', log), log
        assert 'USBINPUT ABS pointer x=0..32767 y=0..32767' in log, log
        q = session.qmp
        q.tap('a'); session.event('keyboard', 1, 30, 1); session.event('keyboard', 1, 30, 0)
        q.tap('shift', 'b')
        for code, value in ((42, 1), (48, 1), (48, 0), (42, 0)): session.event('keyboard', 1, code, value)
        q.tap('ctrl', 'alt', 'delete')  # modifiers and an extended key in one report
        for code, value in ((29, 1), (56, 1), (111, 1), (111, 0), (56, 0), (29, 0)): session.event('keyboard', 1, code, value)
        q.key('c', True); time.sleep(1.2); q.key('c', False)  # held: kernel software autorepeat after 250 ms
        session.event('keyboard', 1, 46, 1); session.event('keyboard', 1, 46, 2); session.event('keyboard', 1, 46, 0)
        q.send({'type': 'abs', 'data': {'axis': 'x', 'value': 16384}}, {'type': 'abs', 'data': {'axis': 'y', 'value': 8192}})
        session.event('pointer', 3, 0, 16384); session.event('pointer', 3, 1, 8192)
        q.send({'type': 'btn', 'data': {'down': True, 'button': 'left'}}); q.send({'type': 'btn', 'data': {'down': False, 'button': 'left'}})
        session.event('pointer', 1, 272, 1); session.event('pointer', 1, 272, 0)
        q.send({'type': 'btn', 'data': {'down': True, 'button': 'right'}}); q.send({'type': 'btn', 'data': {'down': False, 'button': 'right'}})
        session.event('pointer', 1, 273, 1); session.event('pointer', 1, 273, 0)
        q.send({'type': 'rel', 'data': {'axis': 'x', 'value': 7}}, {'type': 'rel', 'data': {'axis': 'y', 'value': -4}})
        session.event('pointer', 2, 0, 7); session.event('pointer', 2, 1, -4)
        q.send({'type': 'btn', 'data': {'down': True, 'button': 'wheel-up'}}); q.send({'type': 'btn', 'data': {'down': False, 'button': 'wheel-up'}})
        session.event('pointer', 2, 8, '-?1')
        session.finish()
    finally:
        session.kill()
    print(f'PASS {accel} {controller}: USB keyboard (boot protocol, modifiers, repeat), tablet (absolute), mouse (relative, wheel) '
          'through /dev/input with the i8042 removed', flush=True)


def usb_hub_case(accel):
    # Kernel logs name xHCI port numbers; QEMU's port=N counts physical USB ports (USB3 ports come first in xHCI).
    session = Session(accel, 'usb-hub', [
        '-device', 'qemu-xhci,id=xhci',
        '-device', 'usb-hub,bus=xhci.0,port=1,id=hub,port-power=on',
        '-device', 'usb-kbd,bus=xhci.0,port=1.1,display=video0,id=kbd1',
        '-device', 'usb-tablet,bus=xhci.0,port=2,display=video0,id=tablet'], 4532)
    try:
        log = session.log(); q = session.qmp
        assert 'USB2 hub' in log and 'USB hub ports=8' in log, log
        assert re.search(r'xhci0 port \d+ route 0x00001 full-speed 0627:0001 slot \d+: keyboard ready', log), log
        q.tap('a'); session.event('keyboard', 1, 30, 1); session.event('keyboard', 1, 30, 0)
        # hot-unplug the keyboard behind the hub, hot-plug one on another hub port
        q.cmd('device_del', {'id': 'kbd1'})
        session.expect(r'route 0x00001 full-speed 0627:0001 slot \d+: detached')
        # xHCI pool pages: 6 controller + hub 3 (context, EP0 ring, status pipe) + keyboard/tablet 4 each (+ HID state)
        session.pool(6 + 3 + 4)
        q.cmd('device_add', {'driver': 'usb-kbd', 'bus': 'xhci.0', 'port': '1.3', 'id': 'kbd2', 'display': 'video0'})
        session.expect(r'route 0x00003 full-speed 0627:0001 slot \d+: keyboard ready')
        q.tap('b'); session.event('keyboard', 1, 48, 1); session.event('keyboard', 1, 48, 0)
        # hot-plug on a root port (newest keyboard receives the routed events)
        q.cmd('device_add', {'driver': 'usb-kbd', 'bus': 'xhci.0', 'port': '3', 'id': 'kbd3', 'display': 'video0'})
        session.expect(r'xhci0 port \d+ high-speed 0627:0001 slot \d+: keyboard ready')
        q.tap('d'); session.event('keyboard', 1, 32, 1); session.event('keyboard', 1, 32, 0)
        # replace the tablet
        q.send({'type': 'abs', 'data': {'axis': 'x', 'value': 3000}}); session.event('pointer', 3, 0, 3000)
        q.cmd('device_del', {'id': 'tablet'})
        session.expect(r'xhci0 port \d+ high-speed 0627:0001 slot \d+: detached')
        session.pool(6 + 3 + 4 + 4)
        q.cmd('device_add', {'driver': 'usb-tablet', 'bus': 'xhci.0', 'port': '2', 'id': 'tablet2', 'display': 'video0'})
        session.expect(r'xhci0 port \d+ high-speed 0627:0001 slot \d+: absolute pointer ready')
        q.send({'type': 'abs', 'data': {'axis': 'x', 'value': 1000}}); session.event('pointer', 3, 0, 1000)
        # unplug the whole hub: its child goes first, then the hub
        q.cmd('device_del', {'id': 'hub'})
        session.expect(r'route 0x00003 full-speed 0627:0001 slot \d+: detached')
        session.pool(6 + 3 + 4 + 4)
        session.expect(r'xhci0 port \d+ full-speed 0409:55aa slot \d+: detached')
        session.pool(6 + 4 + 4)  # every page of the unplugged hub subtree returned
        # and plug a hub back in with a keyboard already behind it
        q.cmd('device_add', {'driver': 'usb-hub', 'bus': 'xhci.0', 'port': '1', 'id': 'hub2'})
        q.cmd('device_add', {'driver': 'usb-kbd', 'bus': 'xhci.0', 'port': '1.2', 'id': 'kbd4', 'display': 'video0'})
        session.expect(r'route 0x00002 full-speed 0627:0001 slot \d+: keyboard ready')
        q.tap('e'); session.event('keyboard', 1, 18, 1); session.event('keyboard', 1, 18, 0)
        session.finish()
    finally:
        session.kill()
    print(f'PASS {accel} USB hub (port power switching) + hot-unplug/hot-plug of keyboards, tablet and a whole hub', flush=True)


accels = ['tcg', 'whpx'] if options.accel == 'both' else [options.accel]
if 'whpx' in accels and not build.whpx_available():
    print('WHPX unavailable: running TCG only'); accels = ['tcg']
for case in options.cases.split(','):
    for accel in accels:
        if case in NETWORK:
            devices, expect = NETWORK[case]
            # net-order: the RTL8139 sits in the lower PCI slot; the stream must still run through the e1000e.
            network_case(accel, devices, expect, served=len(devices) - 1)
        elif case == 'usb-input': usb_input_case(accel, 'qemu-xhci')
        elif case == 'usb-nec': usb_input_case(accel, 'nec-usb-xhci')
        elif case == 'usb-hub': usb_hub_case(accel)
        else:
            raise SystemExit('unknown case ' + case)
print('PASS test_x64_usbnet:', options.cases, accels)
