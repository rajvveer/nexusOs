"""In-kernel WireGuard end to end (arch/x86_64/wireguard.c) against an independent peer: this Python process is the
guest's whole network (test_x64_netwire.py fixture over `-netdev dgram`) and also a WireGuard peer written from the
protocol description with the `cryptography` package (X25519, ChaCha20-Poly1305) and hashlib's BLAKE2s, at
192.168.50.1:51820, inner address 10.66.0.1, with a tiny IPv4 ICMP/TCP/HTTP stack behind it. The guest is configured
only with Debian's tools (`ip link add type wireguard`, `wg set`, `ip addr/route/rule`):
  - guest-initiated handshake (Noise IKpsk2, mac1 checked by the peer) -> ping and an HTTP GET through the tunnel
  - `wg show` reports the handshake, transfer counters, endpoint and allowed IPs
  - peer-initiated handshake (the guest as responder, its next keypair confirmed by the peer's first data)
  - cryptokey routing: an inner packet whose source is outside the peer's allowed IPs is dropped
  - wg-quick-style policy routing: fwmark on the tunnel, default route through wg0 in table 51820, `ip rule not
    fwmark`: an HTTP GET to a "public" address goes through the tunnel while the encapsulated UDP itself stays on eth0
  - hostile input on the listen port (bad mac1, truncated, unknown type, replayed data) never breaks the tunnel
Usage: test_x64_wireguard.py [work dir] [cpus]. The disk is a private copy."""
from pathlib import Path
import hashlib
import hmac
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
import base64

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives import serialization
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-wireguard')
SMP = sys.argv[2] if len(sys.argv) > 2 else '1'
WORK.mkdir(parents=True, exist_ok=True)
PKG_CACHE = nexus_paths.cache('net')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
nw = load_module('netwire', ROOT / 'tests/test_x64_netwire.py')
nettools = load_module('nettools_install', ROOT / 'arch/x86_64/nettools_install.py')
ntt = load_module('nettoolstest', ROOT / 'tests/test_x64_nettools.py')
run = ntt.run

# ---------------------------------------------------------------- WireGuard primitives (independent implementation)
CONSTRUCTION = b'Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s'
IDENTIFIER = b'WireGuard v1 zx2c4 Jason@zx2c4.com'
def HASH(*parts):
    h = hashlib.blake2s()
    for p in parts: h.update(p)
    return h.digest()
def HMAC(key, data): return hmac.new(key, data, hashlib.blake2s).digest()
def KDF(n, key, data):
    t0, out, prev = HMAC(key, data), [], b''
    for i in range(1, n + 1): prev = HMAC(t0, prev + bytes([i])); out.append(prev)
    return out
def MAC(key, data): return hashlib.blake2s(data, digest_size=16, key=key).digest()
def nonce(counter): return b'\0\0\0\0' + struct.pack('<Q', counter)
def AEAD(key, counter, plain, ad): return ChaCha20Poly1305(key).encrypt(nonce(counter), plain, ad)
def DEAEAD(key, counter, data, ad): return ChaCha20Poly1305(key).decrypt(nonce(counter), data, ad)
def raw_public(private): return private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
def raw_private(private): return private.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
def DH(private_raw, public_raw): return X25519PrivateKey.from_private_bytes(private_raw).exchange(X25519PublicKey.from_public_bytes(public_raw))
def tai64n():
    now = time.time_ns(); return struct.pack('>QI', 0x400000000000000A + now // 10**9, now % 10**9)
def csum(data):
    if len(data) & 1: data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF
def ip4(src, dst, proto, payload, ttl=64):
    h = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 0, 0x4000, ttl, proto, 0, src, dst)
    return h[:10] + struct.pack('!H', csum(h)) + h[12:] + payload
def tcp(src, dst, sport, dport, seq, ack, flags, data=b'', options=b''):
    off = (20 + len(options)) // 4
    h = struct.pack('!HHIIHHHH', sport, dport, seq, ack, off << 12 | flags, 65535, 0, 0) + options + data
    c = csum(src + dst + struct.pack('!BBH', 0, 6, len(h)) + h)
    return h[:16] + struct.pack('!H', c) + h[18:]
A4 = lambda text: bytes(int(x) for x in text.split('.'))

PAGE = b'<html><body><h1>NexusOS through WireGuard</h1></body></html>'


class Peer:
    """A WireGuard peer + an IPv4 host stack (ICMP echo, a one-shot HTTP server) behind the tunnel."""
    def __init__(self, net):
        self.net, self.static = net, X25519PrivateKey.generate()
        self.priv, self.pub = raw_private(self.static), raw_public(self.static)
        self.guest_pub = None; self.psk = bytes(32)
        self.keys = None            # the keyset we send with: dict(send, recv, local, remote, counter)
        self.keysets = {}           # every keyset by our local index (the guest may still use the previous one)
        self.pending = None         # our initiation state
        self.events, self.inner, self.lock = [], [], threading.Lock()
        self.tcp = {}
        self.guest_endpoint = None
        self.last_data_message = None
        net.udp4_hook = self.udp

    # outer transport
    def send(self, message):
        src, dst = nw.ROUTER4, self.guest_endpoint[0]
        u = struct.pack('!HHHH', 51820, self.guest_endpoint[1], 8 + len(message), 0) + message
        c = csum(src + dst + struct.pack('!BBH', 0, 17, len(u)) + u) or 0xFFFF
        u = u[:6] + struct.pack('!H', c) + u[8:]
        self.net.send4(nw.ip4(src, dst, 17, u))
    def udp(self, src, dst, sport, dport, data):
        if dport != 51820 or dst != nw.ROUTER4: return False
        self.guest_endpoint = (src, sport)
        try: self.message(data)
        except Exception as error: self.events.append(('error', repr(error)))
        return True
    def message(self, m):
        kind = m[0]
        if kind == 1: self.on_initiation(m)
        elif kind == 2: self.on_response(m)
        elif kind == 4: self.on_data(m)
        self.events.append(('message', kind, len(m)))

    # guest initiates: we answer as responder
    def on_initiation(self, m):
        assert len(m) == 148 and MAC(HASH(b'mac1----', self.pub), m[:116]) == m[116:132], 'initiation mac1'
        C = HASH(CONSTRUCTION); H = HASH(C, IDENTIFIER); H = HASH(H, self.pub)
        e_i = m[8:40]; C = KDF(1, C, e_i)[0]; H = HASH(H, e_i)
        C, k = KDF(2, C, DH(self.priv, e_i)); static = DEAEAD(k, 0, m[40:88], H); H = HASH(H, m[40:88])
        assert static == self.guest_pub, 'initiation from an unknown static key'
        C, k = KDF(2, C, DH(self.priv, static)); stamp = DEAEAD(k, 0, m[88:116], H); H = HASH(H, m[88:116])
        e = X25519PrivateKey.generate(); e_priv, e_pub = raw_private(e), raw_public(e)
        local = os.urandom(4)
        C = KDF(1, C, e_pub)[0]; H = HASH(H, e_pub)
        C = KDF(1, C, DH(e_priv, e_i))[0]; C = KDF(1, C, DH(e_priv, static))[0]
        C, tau, k = KDF(3, C, self.psk); H = HASH(H, tau); empty = AEAD(k, 0, b'', H); H = HASH(H, empty)
        reply = b'\x02\0\0\0' + local + m[4:8] + e_pub + empty
        reply += MAC(HASH(b'mac1----', static), reply) + bytes(16)
        t1, t2 = KDF(2, C, b'')
        self.keys = {'send': t2, 'recv': t1, 'local': local, 'remote': m[4:8], 'counter': 0, 'role': 'responder', 'stamp': stamp}
        self.keysets[local] = self.keys
        self.events.append(('handshake', 'guest-initiated'))
        self.send(reply)
    # we initiate: the guest answers as responder
    def initiate(self):
        C = HASH(CONSTRUCTION); H = HASH(C, IDENTIFIER); H = HASH(H, self.guest_pub)
        e = X25519PrivateKey.generate(); e_priv, e_pub = raw_private(e), raw_public(e)
        local = os.urandom(4)
        C = KDF(1, C, e_pub)[0]; H = HASH(H, e_pub)
        C, k = KDF(2, C, DH(e_priv, self.guest_pub)); static = AEAD(k, 0, self.pub, H); H = HASH(H, static)
        C, k = KDF(2, C, DH(self.priv, self.guest_pub)); stamp = AEAD(k, 0, tai64n(), H); H = HASH(H, stamp)
        m = b'\x01\0\0\0' + local + e_pub + static + stamp
        m += MAC(HASH(b'mac1----', self.guest_pub), m) + bytes(16)
        self.pending = {'C': C, 'H': H, 'e_priv': e_priv, 'local': local}
        self.send(m)
    def on_response(self, m):
        assert self.pending and len(m) == 92 and m[8:12] == self.pending['local'], 'unexpected response'
        assert MAC(HASH(b'mac1----', self.pub), m[:60]) == m[60:76], 'response mac1'
        C, H, e_priv = self.pending['C'], self.pending['H'], self.pending['e_priv']
        e_r = m[12:44]; C = KDF(1, C, e_r)[0]; H = HASH(H, e_r)
        C = KDF(1, C, DH(e_priv, e_r))[0]; C = KDF(1, C, DH(self.priv, e_r))[0]
        C, tau, k = KDF(3, C, self.psk); H = HASH(H, tau); DEAEAD(k, 0, m[44:60], H)
        t1, t2 = KDF(2, C, b'')
        self.keys = {'send': t1, 'recv': t2, 'local': self.pending['local'], 'remote': m[4:8], 'counter': 0, 'role': 'initiator'}
        self.keysets[self.keys['local']] = self.keys
        self.pending = None
        self.events.append(('handshake', 'peer-initiated'))
    # transport
    def on_data(self, m):
        keys = self.keysets.get(m[4:8]); assert keys, 'data for an unknown index'
        counter = struct.unpack('<Q', m[8:16])[0]
        plain = DEAEAD(keys['recv'], counter, m[16:], b'')
        self.last_data_message = m
        if not plain: self.events.append(('keepalive',)); return
        length = struct.unpack('!H', plain[2:4])[0]; packet = plain[:length]
        with self.lock: self.inner.append(packet)
        self.ip(packet)
    def tunnel(self, packet):
        k = self.keys; padded = packet + bytes((-len(packet)) % 16)
        msg = b'\x04\0\0\0' + k['remote'] + struct.pack('<Q', k['counter']) + AEAD(k['send'], k['counter'], padded, b'')
        k['counter'] += 1; self.send(msg); return msg
    # the inner IPv4 host
    def ip(self, p):
        src, dst, proto, ihl = p[12:16], p[16:20], p[9], (p[0] & 15) * 4; body = p[ihl:]
        if proto == 1 and body[0] == 8:
            reply = b'\0\0\0\0' + body[4:]; reply = reply[:2] + struct.pack('!H', csum(reply)) + reply[4:]
            self.tunnel(ip4(dst, src, 1, reply))
        elif proto == 6: self.tcp_in(src, dst, body)
    def tcp_in(self, src, dst, seg):
        sport, dport, seq, ack, field = struct.unpack('!HHIIH', seg[:14]); flags = field & 0x3F; data = seg[(field >> 12) * 4:]
        key = (src, sport, dst, dport); c = self.tcp.get(key)
        if flags & 0x02:
            c = self.tcp[key] = {'iss': 9000, 'rcv': seq + 1, 'buffer': b'', 'sent': False}
            self.tunnel(ip4(dst, src, 6, tcp(dst, src, dport, sport, 9000, seq + 1, 0x12, options=struct.pack('!BBH', 2, 4, 1360))))
            return
        if not c: return
        if data:
            c['buffer'] += data; c['rcv'] = seq + len(data)
            if b'\r\n\r\n' in c['buffer'] and not c['sent']:
                response = b'HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n' % len(PAGE) + PAGE
                self.tunnel(ip4(dst, src, 6, tcp(dst, src, dport, sport, c['iss'] + 1, c['rcv'], 0x18, response)))
                self.tunnel(ip4(dst, src, 6, tcp(dst, src, dport, sport, c['iss'] + 1 + len(response), c['rcv'], 0x11)))
                c['sent'] = True; c['fin_seq'] = c['iss'] + 2 + len(response)
            else: self.tunnel(ip4(dst, src, 6, tcp(dst, src, dport, sport, c['iss'] + 1, c['rcv'], 0x10)))
        if flags & 0x01:
            self.tunnel(ip4(dst, src, 6, tcp(dst, src, dport, sport, c.get('fin_seq', c['iss'] + 1), seq + len(data) + 1, 0x10)))
            self.tcp.pop(key, None)


def main():
    results = []
    def note(text): results.append(text); print('  ' + text, flush=True)
    agent = nw.build_agent(WORK)
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    fatput = load_module('fp', ROOT / 'userland/fatput.py')
    fatput.main(['fatput', str(disk), 'put', str(agent), 'sbin/init'])
    assert nettools.install([str(disk), '--cache', str(PKG_CACHE), '--report', str(WORK / 'install.json')]) == 0
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    net = nw.Network(); peer = Peer(net)
    qemu = subprocess.Popen(nw.qemu_command(disk, serial, SMP), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def serial_text(): return serial.read_text(errors='replace') if serial.exists() else ''
    try:
        net.wait(lambda: [e for e in net.dhcplog if e.get('answer') == 5], 90, 'DHCPv4'); net.guest4 = nw.a4('192.168.50.10')
        net.wait(lambda: 'NETWIRE AGENT READY' in serial_text(), 60, 'agent')
        net.wait(lambda: [e for e in net.icmp6log if e[1] == 135 and e[3] == nw.ANY6 and e[7][8:24] == nw.GUEST_GLOBAL], 20, 'global DAD')
        time.sleep(2.5)

        # keys: the guest's from `wg genkey`, its public key computed here independently
        guest_priv = run(net, '/usr/bin/wg genkey').strip()
        peer.guest_pub = raw_public(X25519PrivateKey.from_private_bytes(base64.b64decode(guest_priv)))
        guest_pub_text, peer_pub_text = base64.b64encode(peer.guest_pub).decode(), base64.b64encode(peer.pub).decode()
        net.rpc('PUT /tmp/wg.key ' + (guest_priv + '\n').encode().hex(), retry=True)
        run(net, '/usr/bin/ip link add dev wg0 type wireguard')
        run(net, f'/usr/bin/wg set wg0 private-key /tmp/wg.key listen-port 51820 peer {peer_pub_text} allowed-ips 10.66.0.0/24 endpoint 192.168.50.1:51820')
        run(net, '/usr/bin/ip -4 addr add 10.66.0.2/24 dev wg0')
        run(net, '/usr/bin/ip link set wg0 up')
        run(net, '/usr/bin/wg show wg0', contains=('interface: wg0', 'public key: ' + guest_pub_text, 'listening port: 51820',
                                                   'peer: ' + peer_pub_text, 'endpoint: 192.168.50.1:51820', 'allowed ips: 10.66.0.0/24'))
        run(net, '/usr/bin/ip link show wg0', contains=('wg0', 'mtu 1420'))
        note('setup: `ip link add type wireguard` + `wg set` + `ip addr`; `wg show` reports the public key the peer computed independently')

        first = run(net, '/usr/bin/ping -c 3 -W 3 10.66.0.1', want=None)
        if '3 received' not in first:   # keep the evidence of a lost tunnel reply (wg0 counters, the IPv4/ICMP MIBs)
            for command in ('/usr/bin/ip -s link show wg0', '/usr/bin/wg show wg0'):
                print(command, run(net, command, want=None), sep='\n')
            print('/proc/net/snmp', net.rpc('FILE /proc/net/snmp', expect=False), sep='\n')
            raise AssertionError(('the first ping through the tunnel lost replies', first, peer.events[-20:]))
        assert ('handshake', 'guest-initiated') in peer.events, peer.events
        reply = net.rpc('HTTP 10.66.0.1 80 /', timeout=30)
        assert reply.startswith('RESULT 0 ') and '200 OK' in reply and 'NexusOS through WireGuard' in reply, reply
        show = run(net, '/usr/bin/wg show wg0', contains=('latest handshake:', 'transfer:'))
        note('guest-initiated Noise IKpsk2 handshake (mac1 + static key verified by the peer), ping x3 and an HTTP GET through the tunnel; '
             'wg show: latest handshake + transfer')

        # the guest as responder: the peer starts a new handshake, then sends inner traffic with the new keys
        peer.initiate()
        net.wait(lambda: ('handshake', 'peer-initiated') in peer.events, 10, 'response from the guest')
        before = len([p for p in peer.inner if p[9] == 1 and p[20] == 0])
        for i in range(3):
            echo = struct.pack('!BBHHH', 8, 0, 0, 0x5151, i) + b'responder-path'
            echo = echo[:2] + struct.pack('!H', csum(echo)) + echo[4:]
            peer.tunnel(ip4(A4('10.66.0.1'), A4('10.66.0.2'), 1, echo)); time.sleep(0.3)
        net.wait(lambda: len([p for p in peer.inner if p[9] == 1 and p[20] == 0]) >= before + 3, 10, 'echo replies with the new keys')
        note('peer-initiated handshake: the guest responded, confirmed its next keypair on the first data and answered 3 echoes with it')

        # cryptokey routing: an inner source outside the peer's allowed IPs is dropped
        before = len([p for p in peer.inner if p[9] == 1 and p[20] == 0])
        echo = struct.pack('!BBHHH', 8, 0, 0, 0x5252, 1) + b'spoofed'; echo = echo[:2] + struct.pack('!H', csum(echo)) + echo[4:]
        peer.tunnel(ip4(A4('10.77.0.5'), A4('10.66.0.2'), 1, echo)); time.sleep(2)
        assert len([p for p in peer.inner if p[9] == 1 and p[20] == 0]) == before, 'a spoofed inner source was accepted'
        note('cryptokey routing: an inner packet from 10.77.0.5 (not in the peer\'s allowed IPs) was dropped')

        # hostile input on the listen port, then a replay of a captured data message
        for junk in (b'\x01\0\0\0' + os.urandom(144), b'\x02\0\0\0' + os.urandom(88), b'\x04\0\0\0' + os.urandom(40), b'\x09' + os.urandom(30), b'\x01', b''):
            peer.send(junk)
        replayed = peer.tunnel(ip4(A4('10.66.0.1'), A4('10.66.0.2'), 1,
                               (lambda e: e[:2] + struct.pack('!H', csum(e)) + e[4:])(struct.pack('!BBHHH', 8, 0, 0, 0x5353, 7) + b'once')))
        time.sleep(1); count = len([p for p in peer.inner if p[9] == 1 and p[20] == 0 and p[24:26] == b'\x53\x53'])
        peer.send(replayed); time.sleep(1.5)
        assert len([p for p in peer.inner if p[9] == 1 and p[20] == 0 and p[24:26] == b'\x53\x53']) == count == 1, 'replayed data message was accepted'
        run(net, '/usr/bin/ping -c 1 -W 3 10.66.0.1', contains=('1 received',))
        note('hostile input (bad mac1, random response/data, unknown type, empty) ignored; a replayed data message dropped; tunnel still up')

        # wg-quick style: everything through the tunnel except the tunnel itself
        run(net, f'/usr/bin/wg set wg0 fwmark 51820 peer {peer_pub_text} allowed-ips 0.0.0.0/0')
        run(net, '/usr/bin/ip -4 route add default dev wg0 table 51820')
        run(net, '/usr/bin/ip -4 rule add not fwmark 51820 table 51820')
        run(net, '/usr/bin/ip -4 rule add table main suppress_prefixlength 0')
        run(net, '/usr/bin/ip -4 rule show', contains=('not from all fwmark 0xca6c lookup 51820', 'suppress_prefixlength 0'))
        outer_before = len([e for e in peer.events if e[0] == 'message'])
        reply = net.rpc('HTTP 198.18.0.80 80 /', timeout=30)
        assert reply.startswith('RESULT 0 ') and 'NexusOS through WireGuard' in reply, reply
        assert any(p[9] == 6 and p[16:20] == A4('198.18.0.80') for p in peer.inner), 'the public-address connection did not use the tunnel'
        assert len([e for e in peer.events if e[0] == 'message']) > outer_before, 'no encapsulated UDP reached the peer over eth0'
        run(net, '/usr/bin/ping -c 1 -W 3 192.168.50.1', contains=('1 received',))   # the LAN stays on eth0 (main table)
        run(net, '/usr/bin/ip -4 route get 198.18.0.80', contains=('dev wg0', 'table 51820'))
        note('policy routing: fwmark + `ip rule not fwmark 51820 table 51820` + default via wg0 -> HTTP to 198.18.0.80 went through '
             'the tunnel, the encapsulated UDP stayed on eth0, the LAN stayed direct')

        run(net, '/usr/bin/ip -4 rule del not fwmark 51820 table 51820'); run(net, '/usr/bin/ip -4 rule del table main suppress_prefixlength 0')
        run(net, '/usr/bin/ip link del wg0')
        run(net, '/usr/bin/wg show wg0', want=None, absent=('interface: wg0',))
        assert not [e for e in peer.events if e[0] == 'error'], [e for e in peer.events if e[0] == 'error']
        note('teardown: rules removed, `ip link del wg0`')

        assert net.rpc('EXIT') == 'BYE'
        qemu.wait(timeout=60); log = serial_text()
        assert qemu.returncode == 33 and 'NETWIRE AGENT DONE' in log, (qemu.returncode, log[-3000:])
        assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, log[-3000:]
        assert not net.errors, net.errors
    except BaseException:
        print(serial_text()[-5000:]); print('\n'.join(net.errors)[-3000:]); print(peer.events[-30:]); raise
    finally:
        net.stop = True
        if qemu.poll() is None: qemu.kill(); qemu.wait()
        net.sock.close(); disk.unlink(missing_ok=True)
    print(f'PASS native WireGuard ({SMP} CPU): ' + '; '.join(results))


if __name__ == '__main__':
    main()
