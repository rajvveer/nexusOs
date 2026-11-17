"""Aurora Network settings on the real desktop: the saved settings are applied at boot and the Network app opens.

    python -I tests/test_aurora_network.py WORK [--smp 2] [--tcg] [--keep-disk]

Prepares one sparse copy of the fresh desktop disk (NEXUS_TEST_SOURCE, arch/x86_64/nexus_paths.py; never written)
in WORK with this tree's out/desktop.elf as /sbin/init, out/aurora_app.elf as /bin/aurora-app and an
/etc/nexus/network.conf (DHCP off, a static IPv4 address and gateway, static DNS, firewall on, TCP 22 allowed), boots
it through a qcow2 overlay on the deterministic fixture network of test_x64_netwire.py (this process is the whole
network) and checks:
  - the desktop runs `aurora-app network --apply` once: "[64] NETWORK SETTINGS APPLIED ... errors=0"
  - the kernel gave up its DHCP lease and the static address is live (it answers on the wire below)
  - the boot firewall (nf_tables "inet nexus" built over nfnetlink, no nft binary on this disk) is enforced: a SYN to
    the allowed port 22 gets the kernel's RST, a SYN to port 23 and an ICMP echo get nothing
  - Spotlight "Network" opens the Network app (AURORA APP OPEN app=network); screenshots are kept as evidence
  - Aurora shuts down cleanly; no kernel or user fault. The prepared copy is deleted unless --keep-disk.
Never touches the user's real disk. /home/nexus/.config and .cache are never read."""
from pathlib import Path
import argparse
import importlib.util
import json
import socket
import struct
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
BASE_DISK = nexus_paths.source_disk()
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'
ap = argparse.ArgumentParser(); ap.add_argument('work', type=Path); ap.add_argument('--smp', default='2')
ap.add_argument('--tcg', action='store_true'); ap.add_argument('--keep-disk', action='store_true')
args = ap.parse_args(); args.work.mkdir(parents=True, exist_ok=True)

def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
nw = load_module('netwire', ROOT / 'tests/test_x64_netwire.py')
fatput = load_module('aurora_net_fatput', ROOT / 'userland/fatput.py')

CONFIG = ('# test_aurora_network.py\ndhcp=0\nstatic4=192.168.50.77/24 192.168.50.1\n'
          'dns=2001:db8:53::99,search=corp.test\nfirewall=1\nallow_tcp=22\n')
STATIC = bytes([192, 168, 50, 77])


class Qmp:
    def __init__(self, port):
        deadline = time.time() + 60
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=60); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(.3)
        self.f = self.s.makefile('rwb'); self.read(); self.cmd('qmp_capabilities')
    def read(self):
        while True:
            message = json.loads(self.f.readline())
            if 'event' not in message: return message
    def cmd(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None: request['arguments'] = arguments
        self.f.write(json.dumps(request).encode() + b'\n'); self.f.flush()
        reply = self.read()
        if 'error' in reply: raise RuntimeError(reply['error'])
        return reply.get('return')
    def key(self, name, down):
        self.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]}); time.sleep(.06)
    def tap(self, *names):
        for n in names: self.key(n, True)
        for n in reversed(names): self.key(n, False)
        time.sleep(.08)
    def type(self, text):
        for ch in text:
            code = {' ': 'spc'}.get(ch, ch.lower()); shift = ch.isupper()
            if shift: self.key('shift', True)
            self.key(code, True); self.key(code, False)
            if shift: self.key('shift', False)
            time.sleep(.03)


def tcp_syn(src, dst, sport, dport):
    h = struct.pack('!HHIIHHHH', sport, dport, 1234, 0, (5 << 12) | 0x02, 65535, 0, 0)
    c = nw.checksum(src + dst + struct.pack('!BBH', 0, 6, len(h)) + h)
    return h[:16] + struct.pack('!H', c) + h[18:]


def main():
    results = []
    def note(text): results.append(text); print('  ' + text, flush=True)
    disk = args.work / 'desktop64.img'
    if not disk.exists():
        load_module('aurora_net_pkg_vm', ROOT / 'tests/pkg_vm.py').sparse_copy(BASE_DISK, disk)
    conf = args.work / 'network.conf'; conf.write_text(CONFIG, newline='\n')
    fatput.main(['fatput', str(disk), 'put', str(OUT / 'desktop.elf'), 'sbin/init', 'put', str(OUT / 'aurora_app.elf'), 'bin/aurora-app',
                 'put', str(conf), 'etc/nexus/network.conf'])
    overlay = args.work / 'overlay.qcow2'; overlay.unlink(missing_ok=True)
    subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(disk.resolve()), '-F', 'raw', str(overlay)], check=True)
    serial = args.work / 'serial.log'; serial.unlink(missing_ok=True)
    net = nw.Network(); seen = []
    net.ip4_hook = lambda proto, src, dst, payload: seen.append((time.monotonic(), proto, src, dst, payload))
    accel = ['-accel', 'tcg', '-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
    command = [QEMU, *accel, '-smp', args.smp, '-m', '3072',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={overlay},format=qcow2,if=ide', '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga', '-display', 'none',
               '-netdev', f'dgram,id=network,local.type=inet,local.host=127.0.0.1,local.port={nw.QEMU_PORT},remote.type=inet,remote.host=127.0.0.1,remote.port={nw.FIXTURE_PORT}',
               '-device', 'e1000e,netdev=network,mac=52:54:00:12:34:56', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
               '-serial', f'file:{serial}', '-qmp', f'tcp:127.0.0.1:{nw.QMP_PORT + 1},server=on,wait=off', '-no-reboot']
    proc = subprocess.Popen(command, stdout=(args.work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    def text(): return serial.read_bytes().decode('latin1') if serial.exists() else ''
    def until(predicate, seconds, what):
        end = time.time() + seconds
        while time.time() < end:
            value = predicate()
            if value: return value
            assert proc.poll() is None, 'VM exited while waiting for ' + what
            time.sleep(.25)
        raise TimeoutError(what)
    q = None
    try:
        until(lambda: '[64] AURORA READY' in text(), 600, 'Aurora')
        line = until(lambda: next((l for l in text().splitlines() if 'NETWORK SETTINGS APPLIED' in l), None), 120, 'network settings applied')
        assert 'errors=0' in line and 'dhcp=0' in line and 'firewall=1' in line and '192.168.50.77/24' in line, line
        note('boot: ' + line.split('] ', 1)[1])
        assert 'DHCP disabled: 192.168.50.10 removed' in text(), 'the DHCP lease was not given up'
        release = until(lambda: [e for e in net.dhcplog if e.get('type') == 7], 10, 'DHCPRELEASE on the wire')[0]
        assert release['ciaddr'] == '192.168.50.10' and release['server'] == '192.168.50.1', release
        note('kernel DHCP client released its lease (DHCPRELEASE reached the server after ARP) and stopped; '
             'static 192.168.50.77/24 + gateway configured over rtnetlink')
        # firewall enforcement from the wire
        start = time.monotonic()
        net.send4(nw.ip4(nw.ROUTER4, STATIC, 6, tcp_syn(nw.ROUTER4, STATIC, 40022, 22)))
        net.send4(nw.ip4(nw.ROUTER4, STATIC, 6, tcp_syn(nw.ROUTER4, STATIC, 40023, 23)))
        echo = struct.pack('!BBHHH', 8, 0, 0, 0x6464, 1) + b'firewall'; echo = echo[:2] + struct.pack('!H', nw.checksum(echo)) + echo[4:]
        net.send4(nw.ip4(nw.ROUTER4, STATIC, 1, echo))
        time.sleep(3)
        after = [e for e in seen if e[0] >= start and e[2] == STATIC]
        rst22 = [e for e in after if e[1] == 6 and struct.unpack('!H', e[4][0:2])[0] == 22 and e[4][13] & 0x04]
        any23 = [e for e in after if e[1] == 6 and struct.unpack('!H', e[4][0:2])[0] == 23]
        echo_reply = [e for e in after if e[1] == 1 and e[4][0] == 0]
        assert rst22 and not any23 and not echo_reply, ('firewall', len(rst22), len(any23), len(echo_reply))
        note('boot firewall enforced: SYN to allowed TCP 22 -> RST, SYN to 23 and ICMP echo dropped')
        # the Network app from Spotlight
        q = Qmp(nw.QMP_PORT + 1)
        before = len(text()); q.tap('ctrl', 'spc'); time.sleep(1.5); q.type('Network'); time.sleep(1.5); q.tap('ret')
        until(lambda: 'AURORA APP OPEN app=network' in text()[before:], 120, 'Network app')
        time.sleep(4)
        shot = args.work / 'network-app.ppm'; q.cmd('screendump', {'filename': str(shot)})
        data = shot.read_bytes(); header_end = data.index(b'255\n') + 4; pixels = data[header_end:]
        dark = pixels.count(bytes([0x1E, 0x1E, 0x20])); green = pixels.count(bytes([0x32, 0xD7, 0x4B]))
        assert dark > 20000 and green > 50, ('the Network window did not render', dark, green)
        try:
            from PIL import Image
            with Image.open(shot) as image: image.save(args.work / 'network-app.png')
            shot.unlink()
        except ImportError: pass
        note(f'Spotlight "Network" opened the Network app; screenshot {dark} window-background pixels, {green} toggle-on pixels')
        # clean shutdown
        before = len(text()); q.tap('ctrl', 'spc'); time.sleep(1); q.type('shut down'); time.sleep(1)
        try: q.tap('ret')
        except (ConnectionError, EOFError, OSError): pass   # QEMU may exit (debug-exit) before the key release
        q = None
        end = time.time() + 240
        while proc.poll() is None and time.time() < end and 'INIT SESSION RETURNED' not in text()[before:]: time.sleep(1)
        assert 'INIT SESSION RETURNED' in text()[before:], 'no clean shutdown'
        log = text()
        assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, 'fault in the log'
        assert not net.errors, net.errors
        note('Aurora shut down cleanly; no kernel or user fault')
    except BaseException:
        print(text()[-5000:]); raise
    finally:
        net.stop = True
        if proc.poll() is None:
            try:
                if q: q.cmd('quit')
            except Exception: pass
            try: proc.wait(30)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        net.sock.close(); overlay.unlink(missing_ok=True)
        for leftover in args.work.glob('*.ppm'): leftover.unlink()
        if not args.keep_disk: disk.unlink(missing_ok=True)
    print('PASS Aurora Network settings: ' + '; '.join(results))


if __name__ == '__main__':
    main()
