"""Aurora Network app: static IPv6 + DNS settings, applied at boot, changed in the app, persisted across a reboot.

    python -I tests/test_aurora_network6.py WORK [--disk DESKTOP64.img] [--smp 2] [--qmp-port 4722] [--port 9123] [--tcg]

The desktop disk (default: the shared read-only integ desk disk) is never written: one qcow2 overlay gets this tree's
Aurora (out/desktop.elf, out/aurora_app.elf), the script runner as the session program and /etc/nexus/network.conf =
  dhcp=1, slaac=0 (manual IPv6), static6=2001:db8:1::a/64 fe80::1, dns=2001:db8:53::99,search=corp6.test
and boots twice on tests/netfix.py (RA with prefix 2001:db8:1::/64 + RDNSS, DHCPv4 with a DNS server):
  boot 1  the settings are applied at boot: the static address answers ICMPv6 echo on the wire, the SLAAC address does
          not exist, the default route is the static one (not an RA route), resolv.conf has only the static server
          and search domain (the RA's RDNSS and the DHCP server are not used); then the test opens Network from
          Spotlight and types a new static IPv6 setting (2001:db8:7::b/64 2001:db8:7::1) + Enter: the old address and
          route are gone, the new ones work from the wire, the new setting is saved (on this FAT32 disk the session
          may only write its home folder: /home/nexus/.aurora/network.conf, which overrides /etc/nexus/network.conf)
  boot 2  (same overlay = the same disk) the new setting is applied at boot
No kernel or user fault; the overlay is deleted at the end."""
from pathlib import Path
import argparse
import json
import re
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import netfix  # noqa: E402
import x64_overlay  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--disk', type=Path, default=Path(r'D:\nexustest\prod\integ_desk_disk\desktop64.img'))
ap.add_argument('--smp', default='2'); ap.add_argument('--qmp-port', type=int, default=4722)
ap.add_argument('--port', type=int, default=9123, help='fixture UDP port; +1 = QEMU dgram port')
ap.add_argument('--tcg', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
CONF = ('# test_aurora_network6.py\ndhcp=1\nstatic4=\ndns=2001:db8:53::99,search=corp6.test\nfirewall=0\nallow_tcp=22\n'
        'slaac=0\nstatic6=2001:db8:1::a/64 fe80::1\n')
A, B, B_GW, SLAAC = '2001:db8:1::a', '2001:db8:7::b', '2001:db8:7::1', '2001:db8:1::5054:ff:fe12:3456'
NEW_SETTING = '2001:db8:7::b/64 2001:db8:7::1'
results = []


def note(ok, what, detail=''):
    results.append((bool(ok), what))
    print(('PASS ' if ok else 'FAIL ') + what + ('' if ok else '  (%s)' % str(detail)[:500]), flush=True)
    return bool(ok)


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
        self.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]}); time.sleep(.05)
    def tap(self, *names):
        for n in names: self.key(n, True)
        for n in reversed(names): self.key(n, False)
        time.sleep(.06)
    def type(self, text):
        plain = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', ',': 'comma', '=': 'equal'}
        for ch in text:
            if ch == ':': self.key('shift', True); self.tap('semicolon'); self.key('shift', False)
            elif ch.isupper(): self.key('shift', True); self.tap(ch.lower()); self.key('shift', False)
            else: self.tap(plain.get(ch, ch))


def script():
    lines = ['env HOME=/home/nexus', 'env PATH=/usr/bin:/bin', 'host 80 192.0.2.80', 'sleep 6000',
             'mark CHECK boot', 'cat /etc/resolv.conf', 'cat /proc/net/if_inet6', 'cat /proc/net/ipv6_route',
             'gate ui 400', 'sleep 3000',
             'mark CHECK changed', 'cat /etc/nexus/network.conf', 'cat /home/nexus/.aurora/network.conf', 'cat /etc/resolv.conf',
             'cat /proc/net/if_inet6', 'cat /proc/net/ipv6_route',
             'gate end 600']
    return '\n'.join(lines) + '\n'


def section(log, name):
    """The runner's cat lines between "MARK CHECK name" and the next MARK."""
    m = re.search(r'\[64\] WR MARK CHECK %s\n(.*?)(?:\[64\] WR MARK|\[64\] WR GATE WAIT|\Z)' % name, log, re.S)
    return [l[len('[64] WR cat| '):] for l in (m.group(1).splitlines() if m else []) if l.startswith('[64] WR cat| ')]


def addresses(lines):
    out = set()
    for l in lines:
        parts = l.split()
        if len(parts) == 6 and len(parts[0]) == 32 and parts[5] == 'eth0':
            out.add(netfix.t6(bytes.fromhex(parts[0])))
    return out


def defaults(lines):
    """(gateway, flags) of eth0's IPv6 default routes"""
    out = []
    for l in lines:
        parts = l.split()
        if len(parts) == 10 and parts[0] == '0' * 32 and parts[1] == '00' and parts[9] == 'eth0':
            out.append((netfix.t6(bytes.fromhex(parts[4])), int(parts[8], 16)))
    return out


def ping(fx, lan, target, tries=3):
    for attempt in range(tries):
        t = time.monotonic(); ident = 0x6e00 + attempt
        body = struct.pack('!HH', ident, 1) + b'nexus-static6'
        fx.send_ip(lan, netfix.a6('2001:db8:5::80'), netfix.a6(target), 58, netfix.icmp6(netfix.a6('2001:db8:5::80'), netfix.a6(target), 128, 0, body))
        try:
            fx.wait(lambda: [e for e in fx.find('icmp6', since=t) if e['type'] == 129 and e['src'] == netfix.t6(netfix.a6(target))], 3, 'echo reply')
            return True
        except AssertionError: pass
    return False


def boot(overlay, serial, tag):
    accel = ['-accel', 'tcg', '-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
    command = [QEMU, *accel, '-smp', args.smp, '-m', '2048', '-machine', 'q35',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'if=none,id=root,file={overlay},format=qcow2', '-device', 'nvme,serial=nexus-root,drive=root',
               '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga', '-display', 'none',
               '-netdev', f'dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port={args.port + 1},remote.type=inet,remote.host=127.0.0.1,remote.port={args.port}',
               '-device', 'e1000e,netdev=n0,mac=52:54:00:12:34:56', '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
               '-serial', f'file:{serial}', '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-no-reboot',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    proc = subprocess.Popen(command, stdout=(WORK / f'{tag}-qemu.log').open('wb'), stderr=subprocess.STDOUT)
    print(f'[{tag}] VM pid {proc.pid}', flush=True)
    return proc


def stop(proc, q):
    if proc.poll() is None:
        try: (q or Qmp(args.qmp_port)).cmd('quit')
        except Exception: pass
        try: proc.wait(30)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()


def main():
    gates = set()
    def site(r):
        if r.path.startswith('/gate/'): return (200, {}, b'open') if r.path[6:] in gates else (404, {}, b'wait')
        return 404, {}, b''
    fx = netfix.Fixture()
    lan = fx.link('lan', args.port, args.port + 1, guest_mac=bytes.fromhex('525400123456'))
    lan.router6(rdnss=['2001:db8:1::53'], dnssl=['ra.test'])
    lan.server4(dns=['192.168.50.1'], domain='lan.test')
    lan.onlink6.add(netfix.a6(B_GW)); lan.routers6.add(netfix.a6(B_GW))
    fx.host('192.0.2.80', '2001:db8:5::80'); fx.http(80, site); fx.start()
    staging = WORK / 'staging'; staging.mkdir(exist_ok=True)
    (staging / 'network.conf').write_text(CONF, newline='\n'); (staging / 'net6.script').write_text(script(), newline='\n')
    (staging / 'desktop.args').write_text('--aurora\n/usr/bin/nexus-wine-runner\n/nexus/net6.script\n', newline='\n')
    overlay = WORK / 'overlay.qcow2'
    puts = [(OUT / 'desktop.elf', 'sbin/init'), (OUT / 'aurora_app.elf', 'bin/aurora-app'), (OUT / 'wine_runner_user.elf', 'usr/bin/nexus-wine-runner'),
            (staging / 'net6.script', 'nexus/net6.script'), (staging / 'desktop.args', 'nexus/desktop.args'), (staging / 'network.conf', 'etc/nexus/network.conf')]
    proc = q = None
    try:
        x64_overlay.stage(args.disk, overlay, puts)
        # ---------------- boot 1
        serial = WORK / 'boot1-serial.log'; serial.unlink(missing_ok=True)
        proc = boot(overlay, serial, 'boot1')
        def text(): return serial.read_text('latin1') if serial.exists() else ''
        fx.wait(lambda: '[64] WR GATE WAIT ui' in text() or proc.poll() is not None, 400, 'the first checks')
        log = text()
        applied = re.search(r'NETWORK SETTINGS APPLIED [^\n]*', log)
        note(applied and 'errors=0' in applied.group(0) and 'slaac=0' in applied.group(0), 'boot 1: network.conf applied at boot', applied and applied.group(0))
        first = section(log, 'boot')
        have = addresses(first); routes = defaults(first)
        resolv = [l for l in first if l.startswith(('nameserver', 'search'))]
        note(A in have and SLAAC not in have, 'boot 1: static IPv6 2001:db8:1::a on eth0, no SLAAC address (manual IPv6)', sorted(have))
        note(routes and all(f & 0x40000 == 0 for _, f in routes) and ('fe80::1', 0x3) in [(g, f & 0xFFFF) for g, f in routes],
             'boot 1: default route via fe80::1 is the static one (no router-advertisement route)', routes)
        note(resolv == ['nameserver 2001:db8:53::99', 'search corp6.test'], 'boot 1: resolv.conf = the static IPv6 DNS server + search domain only', resolv)
        note(ping(fx, lan, A), 'boot 1: the static address answers ICMPv6 echo on the wire')
        note(not ping(fx, lan, SLAAC, tries=1), 'boot 1: the SLAAC address does not answer')
        # the Network app: change the static IPv6 setting
        q = Qmp(args.qmp_port); before = len(text())
        q.tap('ctrl', 'spc'); time.sleep(1.5); q.type('Network'); time.sleep(1.5); q.tap('ret')
        fx.wait(lambda: 'AURORA APP OPEN app=network' in text()[before:], 120, 'the Network app')
        time.sleep(3)
        q.tap('tab'); q.tap('tab')                        # focus: static IPv4, then static IPv6
        for _ in range(40): q.tap('backspace')
        q.type(NEW_SETTING); time.sleep(0.5); q.tap('ret')
        time.sleep(4)
        shot = WORK / 'network-app.ppm'
        try:
            q.cmd('screendump', {'filename': str(shot)})
            from PIL import Image
            with Image.open(shot) as image: image.convert('RGB').save(WORK / 'network-app.png')
        except Exception as e: print('screenshot:', e)
        finally: shot.unlink(missing_ok=True)
        gates.add('ui')
        fx.wait(lambda: '[64] WR GATE WAIT end' in text() or proc.poll() is not None, 120, 'the second checks')
        log = text(); second = section(log, 'changed')
        conf = [l for l in second if l.startswith(('static6=', 'slaac='))]
        have = addresses(second); routes = defaults(second)
        note('static6=' + NEW_SETTING in conf, 'Network app: the typed setting is saved (FAT32 disk: /home/nexus/.aurora/network.conf)', conf)
        note(B in have and A not in have, 'Network app: new static address live, the replaced one removed', sorted(have))
        note([g for g, _ in routes] == [B_GW], 'Network app: default route replaced (via 2001:db8:7::1 only)', routes)
        note(ping(fx, lan, B) and not ping(fx, lan, A, tries=1), 'Network app: the wire reaches the new address, not the old one')
        gates.add('end'); time.sleep(2)
        stop(proc, q); q = None; proc = None
        # ---------------- boot 2: the same disk
        serial = WORK / 'boot2-serial.log'; serial.unlink(missing_ok=True)
        proc = boot(overlay, serial, 'boot2')
        fx.wait(lambda: '[64] WR GATE WAIT ui' in text() or proc.poll() is not None, 400, 'boot 2 checks')
        log = text(); first = section(log, 'boot')
        have = addresses(first); routes = defaults(first)
        note(B in have and A not in have and SLAAC not in have, 'boot 2: the changed setting persisted and is applied at boot', sorted(have))
        note([g for g, _ in routes] == [B_GW], 'boot 2: default route via the persisted gateway', routes)
        note(ping(fx, lan, B), 'boot 2: the persisted static address answers on the wire')
        gates.update({'ui', 'end'})
        fx.wait(lambda: '[64] WR DONE' in text() or proc.poll() is not None, 120, 'script end')
        for name, path in (('boot1', WORK / 'boot1-serial.log'), ('boot2', serial)):
            data = path.read_text('latin1') if path.exists() else ''
            faults = [l for l in data.splitlines() if 'KERNEL_FAULT' in l or 'USER_FAULT' in l]
            note(not faults, f'{name}: no kernel or user fault', faults[:3])
        note(not fx.errors, 'fixture without errors', fx.errors[:2])
    finally:
        if proc: stop(proc, q)
        fx.stop(); overlay.unlink(missing_ok=True)
    failed = [w for ok, w in results if not ok]
    print('SUMMARY aurora network6: %d/%d checks passed' % (len(results) - len(failed), len(results)), flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
