"""Wire-level IPv6/IPv4 conformance and robustness of the native x86_64 network stack. This Python process is the
guest's whole network (QEMU `-netdev dgram`): IPv6 router (RA with prefix, MTU, RDNSS, DNSSL), neighbour, DHCPv4
server (short leases: renew, rebind, expiry, NAK, INIT-REBOOT, release), stateless DHCPv6 server, DNS server, remote
hosts (TCP, ICMP errors, Packet Too Big) and attacker (malformed extension headers, fragments, bogus RAs/redirects,
DAD collisions). arch/x86_64/netwire_user.c runs as /sbin/init and answers commands over UDP port 7001; QMP set_link
toggles the carrier. Usage: test_x64_netwire.py [work dir] [cpus]. The disk is a private copy."""
from pathlib import Path
import importlib.util
import json
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-netwire')
SMP = sys.argv[2] if len(sys.argv) > 2 else '1'
WORK.mkdir(parents=True, exist_ok=True)
FIXTURE_PORT, QEMU_PORT, QMP_PORT = 8943, 8944, 4640

def a6(text): return socket.inet_pton(socket.AF_INET6, text)
def t6(raw): return socket.inet_ntop(socket.AF_INET6, raw)
def a4(text): return socket.inet_aton(text)
def t4(raw): return socket.inet_ntoa(raw)
GUEST_MAC, R_MAC, R2_MAC, BCAST = bytes.fromhex('525400123456'), bytes.fromhex('020000000001'), bytes.fromhex('020000000002'), b'\xff' * 6
GUEST_LL, GUEST_GLOBAL = a6('fe80::5054:ff:fe12:3456'), a6('2001:db8:1::5054:ff:fe12:3456')
R_LL, R2_LL, R_GLOBAL, ONLINK_RPC = a6('fe80::1'), a6('fe80::2'), a6('2001:db8:1::1'), a6('2001:db8:1::100')
PREFIX, PREFIX2 = a6('2001:db8:1::'), a6('2001:db8:2::')
RPC6, RPC4, ROUTER4 = a6('2001:db8:5::100'), a4('198.51.100.100'), a4('192.168.50.1')
ALL_NODES, ANY6 = a6('ff02::1'), bytes(16)
TCP_OPEN, TCP_REFUSED, PTB_HOST, REDIRECTED = a6('2001:db8:5::80'), a6('2001:db8:5::81'), a6('2001:db8:5::7'), a6('2001:db8:5::9')
UNREACHABLE, DEFENDED, NODAD = a6('2001:db8:1::99'), a6('2001:db8:1::abcd'), a6('2001:db8:1::abce')

def checksum(data):
    if len(data) & 1: data += b'\0'
    total = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while total >> 16: total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF
def pseudo6(src, dst, nh, data): return checksum(src + dst + struct.pack('!I3xB', len(data), nh) + data)
def eth(dst, src, kind, payload): return dst + src + struct.pack('!H', kind) + payload
def ip6(src, dst, nh, payload, hlim=64, plen=None):
    return struct.pack('!IHBB', 0x60000000, len(payload) if plen is None else plen, nh, hlim) + src + dst + payload
def udp6(src, dst, sport, dport, data):
    c = pseudo6(src, dst, 17, struct.pack('!HHHH', sport, dport, 8 + len(data), 0) + data) or 0xFFFF
    return struct.pack('!HHHH', sport, dport, 8 + len(data), c) + data
def icmp6(src, dst, kind, code, body):
    return struct.pack('!BBH', kind, code, pseudo6(src, dst, 58, struct.pack('!BBH', kind, code, 0) + body)) + body
def tcp6(src, dst, sport, dport, seq, ack, flags, options=b''):
    header = struct.pack('!HHIIHHHH', sport, dport, seq, ack, ((20 + len(options)) // 4) << 12 | flags, 65535, 0, 0) + options
    return header[:16] + struct.pack('!H', pseudo6(src, dst, 6, header)) + header[18:]
def ip4(src, dst, proto, payload, ttl=64, ident=0, frag=0):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), ident, frag, ttl, proto, 0, src, dst)
    return header[:10] + struct.pack('!H', checksum(header)) + header[12:] + payload
def udp4(src, dst, sport, dport, data):
    c = checksum(src + dst + struct.pack('!BBH', 0, 17, 8 + len(data)) + struct.pack('!HHHH', sport, dport, 8 + len(data), 0) + data) or 0xFFFF
    return struct.pack('!HHHH', sport, dport, 8 + len(data), c) + data
def mc_mac(dst): return b'\x33\x33' + dst[12:]
def fragment_header(nh, offset, more, ident): return struct.pack('!BBHI', nh, 0, offset | (1 if more else 0), ident)
def reassemble(parts):
    if all(more for _, more in parts.values()): return None
    out, position = b'', 0
    for offset in sorted(parts):
        data, more = parts[offset]
        if offset != position: return None
        out += data; position += len(data)
        if not more: return out
    return None
def dns_name(name): return b''.join(bytes([len(label)]) + label.encode() for label in name.split('.')) + b'\0'

class Network:
    """The guest's whole network. Every frame from the guest is logged; handlers answer like real hosts."""
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        self.sock.bind(('127.0.0.1', FIXTURE_PORT))
        self.cond = threading.Condition(); self.errors = []; self.stop = False
        self.ip6log, self.icmp6log, self.udp6log, self.tcplog, self.frag6log = [], [], [], [], []
        self.dhcplog, self.dhcp6log, self.dnslog, self.replies = [], [], [], {}
        self.frag6, self.frag4 = {}, {}
        self.neighbours = {R_LL: R_MAC, R2_LL: R2_MAC, R_GLOBAL: R_MAC, ONLINK_RPC: R_MAC}
        self.defend = set(); self.ra_on_rs = True; self.ptb_mtu = 1280
        self.dhcp_mode = 'normal'; self.dhcp_ip = '192.168.50.10'; self.lease = (40, 15, 30)
        self.port = 40000; self.guest4 = None
        self.udp4_hook = None   # optional f(src, dst, sport, dport, data) -> True when consumed (test_x64_wireguard.py)
        self.ip4_hook = None    # optional f(proto, src, dst, payload): every IPv4 packet from the guest (test_aurora_network.py)
        self.thread = threading.Thread(target=self.run, daemon=True); self.thread.start()
    def send(self, frame): self.sock.sendto(frame, ('127.0.0.1', QEMU_PORT))
    def send6(self, packet, l2dst=GUEST_MAC, l2src=R_MAC): self.send(eth(l2dst, l2src, 0x86DD, packet))
    def send4(self, packet, l2dst=GUEST_MAC): self.send(eth(l2dst, R_MAC, 0x0800, packet))
    def run(self):
        self.sock.settimeout(0.2)
        while not self.stop:
            try: frame, _ = self.sock.recvfrom(65536)
            except socket.timeout: continue
            except ConnectionResetError: continue   # (Windows: ICMP port unreachable before QEMU listens)
            except OSError: break
            try: self.frame(frame)
            except Exception: self.errors.append(traceback.format_exc())
    def log(self, where, item):
        with self.cond: where.append(item); self.cond.notify_all()
    def wait(self, predicate, timeout, what):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                value = predicate()
                if value: return value
                left = deadline - time.monotonic()
                if left <= 0: raise AssertionError(f'timed out waiting for {what}')
                self.cond.wait(min(left, 0.25))
    def frame(self, frame):
        if len(frame) < 14 or frame[6:12] != GUEST_MAC: return
        l2dst, kind, body = frame[:6], struct.unpack('!H', frame[12:14])[0], frame[14:]
        if kind == 0x0806 and len(body) >= 28 and body[6:8] == b'\0\1' and body[24:28] == ROUTER4:
            self.send(eth(body[8:14], R_MAC, 0x0806, struct.pack('!HHBBH', 1, 0x0800, 6, 4, 2) + R_MAC + ROUTER4 + body[8:14] + body[14:18]))
        elif kind == 0x0800: self.ipv4(l2dst, body)
        elif kind == 0x86DD: self.ipv6(l2dst, body)

    # ---- IPv4: DHCP server, DNS, RPC replies, echo ----
    def ipv4(self, l2dst, b):
        if len(b) < 20: return
        ihl = (b[0] & 15) * 4; b = b[:struct.unpack('!H', b[2:4])[0]]
        ident, frag = struct.unpack('!HH', b[4:8]); proto, src, dst = b[9], b[12:16], b[16:20]; payload = b[ihl:]
        if frag & 0x3FFF:
            parts = self.frag4.setdefault((src, dst, ident, proto), {}); parts[(frag & 0x1FFF) * 8] = (payload, bool(frag & 0x2000))
            whole = reassemble(parts)
            if whole is None: return
            del self.frag4[(src, dst, ident, proto)]; payload = whole
        if self.ip4_hook: self.ip4_hook(proto, src, dst, payload)
        if proto == 17 and len(payload) >= 8:
            sport, dport, length = struct.unpack('!HHH', payload[:6]); data = payload[8:length]
            if self.udp4_hook and self.udp4_hook(src, dst, sport, dport, data): return
            if dport == 67: self.dhcp(l2dst, src, dst, data)
            elif dport == 53: self.dns(4, src, dst, sport, data)
            elif sport == 7001 and dst == RPC4: self.log(self.replies.setdefault(dport, []), data)
        elif proto == 1 and payload[:1] == b'\x08':
            body = b'\0\0\0\0' + payload[4:]; body = b'\0\0' + struct.pack('!H', checksum(body)) + payload[4:]
            self.send4(ip4(dst, src, 1, body))
    def dhcp(self, l2dst, src, dst, data):
        if len(data) < 240 or data[0] != 1: return
        xid, ciaddr, chaddr, options, at = data[4:8], data[12:16], data[28:34], {}, 240
        while at < len(data) and data[at] != 255:
            if data[at] == 0: at += 1; continue
            options[data[at]] = data[at + 2:at + 2 + data[at + 1]]; at += 2 + data[at + 1]
        kind = options.get(53, b'\0')[0]
        entry = {'t': time.monotonic(), 'type': kind, 'ciaddr': t4(ciaddr), 'requested': t4(options[50]) if 50 in options else None,
                 'server': t4(options[54]) if 54 in options else None, 'dst': t4(dst), 'src': t4(src), 'broadcast': l2dst == BCAST}
        mode = self.dhcp_mode
        if mode == 'silent' or kind == 7: self.log(self.dhcplog, entry); return
        if kind == 1: entry['answer'] = 2; self.dhcp_reply(xid, chaddr, 2, a4(self.dhcp_ip))
        elif kind == 3:
            wanted = options.get(50) or ciaddr
            if entry['server'] and entry['server'] != '192.168.50.1': entry['answer'] = None
            elif mode == 'nak-reboot' and not entry['server'] and ciaddr == bytes(4):
                entry['answer'] = 6; self.dhcp_mode = 'normal'; self.dhcp_reply(xid, chaddr, 6, bytes(4))
            elif wanted != a4(self.dhcp_ip): entry['answer'] = 6; self.dhcp_reply(xid, chaddr, 6, bytes(4))
            else: entry['answer'] = 5; self.dhcp_reply(xid, chaddr, 5, wanted, unicast=ciaddr if ciaddr != bytes(4) else None, ciaddr=ciaddr)
        self.log(self.dhcplog, entry)
    def dhcp_reply(self, xid, chaddr, kind, yiaddr, unicast=None, ciaddr=bytes(4)):
        lease, t1, t2 = self.lease
        message = struct.pack('!BBBB4sHH4s4s4s4s16s64s128s', 2, 1, 6, 0, xid, 0, 0, ciaddr, yiaddr, ROUTER4, bytes(4), chaddr + bytes(10), b'', b'')
        message += b'\x63\x82\x53\x63' + bytes([53, 1, kind, 54, 4]) + ROUTER4
        if kind != 6:
            message += bytes([1, 4]) + a4('255.255.255.0') + bytes([3, 4]) + ROUTER4 + bytes([6, 4]) + ROUTER4 + bytes([15, 8]) + b'lan.test'
            message += bytes([51, 4]) + struct.pack('!I', lease) + bytes([58, 4]) + struct.pack('!I', t1) + bytes([59, 4]) + struct.pack('!I', t2)
        message = (message + b'\xff').ljust(300, b'\0')
        target = unicast or b'\xff' * 4
        self.send4(ip4(ROUTER4, target, 17, udp4(ROUTER4, target, 67, 68, message)), GUEST_MAC if unicast else BCAST)
    def dns(self, family, src, dst, sport, data):
        if len(data) < 12: return
        at, labels = 12, []
        while at < len(data) and data[at]: labels.append(data[at + 1:at + 1 + data[at]].decode(errors='replace')); at += 1 + data[at]
        qtype = struct.unpack('!H', data[at + 1:at + 3])[0]; question = data[12:at + 5]; name = '.'.join(labels).lower()
        self.log(self.dnslog, (family, t4(dst) if family == 4 else t6(dst), name, qtype))
        records = {('host6.example.test', 28): a6('2001:db8:5::80'), ('host6.example.test', 1): a4('192.0.2.80')}
        record = records.get((name, qtype)); known = any(n == name for n, _ in records)
        answer = b'\xc0\x0c' + struct.pack('!HHIH', qtype, 1, 60, len(record)) + record if record else b''
        response = data[:2] + struct.pack('!HHHHH', 0x8180 | (0 if known else 3), 1, 1 if record else 0, 0, 0) + question + answer
        if family == 4: self.send4(ip4(dst, src, 17, udp4(dst, src, 53, sport, response)))
        else: self.send6(ip6(dst, src, 17, udp6(dst, src, 53, sport, response)))

    # ---- IPv6: router, neighbours, DHCPv6, DNS, TCP peers, error sources ----
    def ipv6(self, l2dst, b):
        if len(b) < 40: return
        b = b[:40 + struct.unpack('!H', b[4:6])[0]]
        nh, hlim, src, dst, at, alert, payload = b[6], b[7], b[8:24], b[24:40], 40, False, None
        while payload is None and nh in (0, 43, 60, 44):
            if nh == 44:
                field = struct.unpack('!H', b[at + 2:at + 4])[0]; key = (src, dst, b[at + 4:at + 8])
                self.log(self.frag6log, (time.monotonic(), dst, field & 0xFFF8, field & 1, len(b)))
                parts = self.frag6.setdefault(key, {}); parts[field & 0xFFF8] = (b[at + 8:], bool(field & 1))
                whole = reassemble(parts)
                if whole is None: return
                del self.frag6[key]; nh, payload = b[at], whole
                break
            if nh == 0 and b[at + 2:at + 4] == b'\x05\x02': alert = True
            nh, at = b[at], at + (b[at + 1] + 1) * 8
        if payload is None: payload = b[at:]
        self.log(self.ip6log, (time.monotonic(), l2dst, src, dst, nh, len(b)))
        if nh == 58 and payload: self.icmp6_in(l2dst, src, dst, hlim, payload, alert, b)
        elif nh == 17 and len(payload) >= 8:
            sport, dport, length = struct.unpack('!HHH', payload[:6]); data = payload[8:length]
            self.log(self.udp6log, (time.monotonic(), l2dst, src, dst, sport, dport, len(data)))
            if dport == 547: self.dhcp6(src, data)
            elif dport == 53: self.dns(6, src, dst, sport, data)
            elif sport == 7001: self.log(self.replies.setdefault(dport, []), data)
            elif dst == PTB_HOST and len(b) > self.ptb_mtu and b[6] != 44:
                self.send6(ip6(R_GLOBAL, src, 58, icmp6(R_GLOBAL, src, 2, 0, struct.pack('!I', self.ptb_mtu) + b[:1232 - 48])))
        elif nh == 6 and len(payload) >= 20: self.tcp_in(src, dst, payload, b)
    def icmp6_in(self, l2dst, src, dst, hlim, m, alert, packet):
        kind = m[0]
        self.log(self.icmp6log, (time.monotonic(), kind, m[1], src, dst, hlim, alert, m, l2dst))
        if kind == 133 and self.ra_on_rs: self.ra()
        elif kind == 135 and len(m) >= 24:
            target = m[8:24]
            if src == ANY6:
                if target in self.defend:   # someone already uses it: defend (RFC 4862 5.4.3)
                    self.na(target, ALL_NODES, target, R2_MAC, 0x20000000)
                return
            mac = self.neighbours.get(target)
            if mac: self.na(target, src, target, mac, (0xE0000000 if target in (R_LL, R2_LL) else 0x60000000))
        elif kind == 128 and dst != GUEST_GLOBAL:
            self.send6(ip6(dst, src, 58, icmp6(dst, src, 129, 0, m[4:])))
    def na(self, src, dst, target, mac, flags):
        packet = ip6(src, dst, 58, icmp6(src, dst, 136, 0, struct.pack('!I', flags) + target + bytes([2, 1]) + mac), hlim=255)
        self.send6(packet, mc_mac(dst) if dst[0] == 0xFF else GUEST_MAC, mac)
    def ra(self, src=R_LL, hlim=255, lifetime=1800, prefixes=((PREFIX, 64, 0xC0, 600, 300),), mtu=1400, dns=True, flags=0x40, dst=ALL_NODES):
        body = struct.pack('!BBHII', 64, flags, lifetime, 0, 0) + bytes([1, 1]) + (R2_MAC if src == R2_LL else R_MAC)
        if mtu: body += struct.pack('!BBHI', 5, 1, 0, mtu)
        for prefix, length, pflags, valid, preferred in prefixes: body += struct.pack('!BBBBIII', 3, 4, length, pflags, valid, preferred, 0) + prefix
        if dns: body += struct.pack('!BBHI', 25, 3, 0, 600) + a6('2001:db8:1::53') + struct.pack('!BBHI', 31, 3, 0, 600) + dns_name('example.test').ljust(16, b'\0')
        self.send6(ip6(src, dst, 58, icmp6(src, dst, 134, 0, body), hlim=hlim), mc_mac(dst) if dst[0] == 0xFF else GUEST_MAC)
    def dhcp6(self, src, data):
        if len(data) < 4: return
        kind, xid, options, at = data[0], data[1:4], {}, 4
        while at + 4 <= len(data):
            code, length = struct.unpack('!HH', data[at:at + 4]); options[code] = data[at + 4:at + 4 + length]; at += 4 + length
        self.log(self.dhcp6log, (time.monotonic(), kind, src, sorted(options), options.get(6, b'')))
        if kind != 11: return
        duid = struct.pack('!HH', 3, 1) + R_MAC; domain = dns_name('corp6.test')
        reply = bytes([7]) + xid + (struct.pack('!HH', 1, len(options[1])) + options[1] if 1 in options else b'')
        reply += struct.pack('!HH', 2, len(duid)) + duid + struct.pack('!HH', 23, 16) + a6('2001:db8:53::53')
        reply += struct.pack('!HH', 24, len(domain)) + domain + struct.pack('!HHI', 32, 4, 3600)
        self.send6(ip6(R_LL, src, 17, udp6(R_LL, src, 547, 546, reply)))
    def tcp_in(self, src, dst, segment, packet):
        sport, dport, seq, ack, field = struct.unpack('!HHIIH', segment[:14]); flags = field & 0x3F; size = (field >> 12) * 4
        mss, at = None, 20
        while at < size:
            if segment[at] == 0: break
            if segment[at] == 1: at += 1; continue
            if segment[at] == 2: mss = struct.unpack('!H', segment[at + 2:at + 4])[0]
            at += max(segment[at + 1], 2)
        self.log(self.tcplog, (time.monotonic(), dst, dport, flags, mss, pseudo6(src, dst, 6, segment) == 0, len(segment) - size))
        if flags & 0x04: return
        if dst == TCP_OPEN and flags & 0x02 and not flags & 0x10:
            self.send6(ip6(dst, src, 6, tcp6(dst, src, dport, sport, 7000, seq + 1, 0x12, struct.pack('!BBH', 2, 4, 1440))))
        elif dst == TCP_REFUSED and flags & 0x02:
            self.send6(ip6(R_GLOBAL, src, 58, icmp6(R_GLOBAL, src, 1, 4, b'\0\0\0\0' + packet[:1232 - 48])))
        elif dst == TCP_OPEN and flags & 0x01:
            self.send6(ip6(dst, src, 6, tcp6(dst, src, dport, sport, ack, 0, 0x04)))

    # ---- the agent's RPC ----
    def rpc(self, text, timeout=20, v4=False, source=RPC6, wrap=None, expect=True, retry=False):
        self.port += 1; port = self.port; data = text.encode()
        def transmit():
            if v4: self.send4(ip4(RPC4, self.guest4, 17, udp4(RPC4, self.guest4, port, 7001, data)))
            elif wrap: wrap(source, port, data)
            else: self.send6(ip6(source, GUEST_GLOBAL, 17, udp6(source, GUEST_GLOBAL, port, 7001, data)))
        transmit(); deadline = time.monotonic() + timeout; resend = time.monotonic() + 4
        with self.cond:
            while True:
                if self.replies.get(port): return self.replies[port][0].decode(errors='replace')
                now = time.monotonic()
                if now >= deadline:
                    if expect: raise AssertionError(f'no reply to {text[:60]!r}')
                    return None
                if retry and now >= resend:
                    resend = now + 4; self.cond.release()
                    try: transmit()
                    finally: self.cond.acquire()
                self.cond.wait(0.2)
    def since(self, log, t): return [e for e in log if (e['t'] if isinstance(e, dict) else e[0]) >= t]

def until(function, timeout, what):
    deadline = time.monotonic() + timeout
    while True:
        value = function()
        if value: return value
        if time.monotonic() >= deadline: raise AssertionError(f'timed out waiting for {what}')
        time.sleep(0.5)
def qmp(command, arguments=None):
    with socket.create_connection(('127.0.0.1', QMP_PORT), timeout=10) as s:
        f = s.makefile('rw')
        json.loads(f.readline())
        for request in ({'execute': 'qmp_capabilities'}, {'execute': command, 'arguments': arguments or {}}):
            f.write(json.dumps(request) + '\n'); f.flush()
            while True:
                answer = json.loads(f.readline())
                if 'return' in answer or 'error' in answer: break
        assert 'return' in answer, answer
def canon(text):
    """One spelling per address (glibc's inet_ntop leaves a single zero group uncompressed, RFC 5952 4.2.2)"""
    address, slash, plen = text.partition('/')
    return (t6(a6(address)) if ':' in address else address) + slash + plen
def addrs(net, family='6'):
    lines = net.rpc(f'ADDRS {family}', retry=True).strip().splitlines(); result = {}
    for line in lines:
        address, index, flags, preferred, valid = line.split()
        result[canon(address)] = (int(index), int(flags, 16), int(preferred), int(valid))
    return result
def snmp6(net):
    return {k: int(v) for k, v in (line.split() for line in net.rpc('FILE /proc/net/snmp6', retry=True).strip().splitlines() if len(line.split()) == 2)}
def param_problem(net, since, source, code, pointer, timeout=4):
    def match():
        for e in net.since(net.icmp6log, since):
            if e[1] == 4 and e[3] == GUEST_GLOBAL and e[4] == source: return e
    e = net.wait(match, timeout, f'parameter problem for {t6(source)}')
    got = (e[2], struct.unpack('!I', e[7][4:8])[0])
    assert got == (code, pointer), ('parameter problem', t6(source), got, (code, pointer))
    return got

def qemu_command(disk, serial, smp):
    """The guest on the fixture network (shared with tests/test_x64_nettools.py)."""
    return [r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '512', '-smp', smp,
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', f'dgram,id=network,local.type=inet,local.host=127.0.0.1,local.port={QEMU_PORT},remote.type=inet,remote.host=127.0.0.1,remote.port={FIXTURE_PORT}',
        '-device', 'e1000e,netdev=network,mac=52:54:00:12:34:56', '-qmp', f'tcp:127.0.0.1:{QMP_PORT},server=on,wait=off',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
def build_agent(work):
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                    '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/netwire_user.c'), '-o', str(work / 'netwire-user.elf')], check=True)
    return work / 'netwire-user.elf'
def main():
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                    '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/netwire_user.c'), '-o', str(WORK / 'netwire-user.elf')], check=True)
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    spec = importlib.util.spec_from_file_location('netwire_fatput', ROOT / 'userland/fatput.py')
    fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
    fatput.main(['fatput', str(disk), 'put', str(WORK / 'netwire-user.elf'), 'sbin/init'])
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    net = Network(); results = []
    qemu = subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '256', '-smp', SMP,
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', f'dgram,id=network,local.type=inet,local.host=127.0.0.1,local.port={QEMU_PORT},remote.type=inet,remote.host=127.0.0.1,remote.port={FIXTURE_PORT}',
        '-device', 'e1000e,netdev=network,mac=52:54:00:12:34:56', '-qmp', f'tcp:127.0.0.1:{QMP_PORT},server=on,wait=off',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def note(text): results.append(text); print('  ' + text, flush=True)
    def serial_text(): return serial.read_text(errors='replace') if serial.exists() else ''
    try:
        # ---- boot: DHCPv4, DAD, RS -> RA, SLAAC + temporary address, DHCPv6 information request ----
        net.wait(lambda: [e for e in net.dhcplog if e.get('answer') == 5], 90, 'DHCPv4 ACK'); net.guest4 = a4('192.168.50.10')
        boot = net.wait(lambda: [e for e in net.icmp6log if e[1] == 135 and e[3] == ANY6 and e[7][8:24] == GUEST_LL], 30, 'link-local DAD')[0]
        net.wait(lambda: [e for e in net.icmp6log if e[1] == 133], 15, 'router solicitation')
        net.wait(lambda: 'NETWIRE AGENT READY' in serial_text(), 60, 'agent')
        net.wait(lambda: [e for e in net.icmp6log if e[1] == 135 and e[3] == ANY6 and e[7][8:24] == GUEST_GLOBAL], 15, 'global DAD')
        def settled():   # every address past DAD (the temporary one starts its own after the public one)
            table = addrs(net)
            return table if len(table) >= 4 and not [v for v in table.values() if v[1] & 0x40] else None
        table = until(settled, 15, 'addresses past DAD')
        ll, glob = table.get(canon('fe80::5054:ff:fe12:3456/64')), table.get(canon('2001:db8:1::5054:ff:fe12:3456/64'))
        temporary = [a for a, v in table.items() if a.startswith('2001:db8:1:') and v[1] & 0x01]
        assert ll and glob and glob[1] == 0x100 and 290 <= glob[2] <= 300 and 590 <= glob[3] <= 600, table   # mngtmpaddr (dynamic)
        assert len(temporary) == 1 and table[temporary[0]][1] == 0x01 and table[temporary[0]][2] <= 300, table
        dad_probes = len([e for e in net.icmp6log if e[1] == 135 and e[3] == ANY6])
        mld = [e for e in net.icmp6log if e[1] == 143]
        assert mld and all(e[6] and e[5] == 1 for e in mld), 'MLDv2 reports need hop limit 1 and Router Alert'
        net.wait(lambda: [e for e in net.dhcp6log if e[1] == 11], 20, 'DHCPv6 information request')
        info = [e for e in net.dhcp6log if e[1] == 11][0]
        assert 1 in info[3] and 8 in info[3] and 6 in info[3] and 3 not in info[3] and info[2] == GUEST_LL, info
        note(f'boot: DHCPv4 lease, link-local + SLAAC {glob} + temporary {temporary[0]}, {dad_probes} DAD probes, RS, MLDv2, DHCPv6 info-request')

        # ---- resolver configuration and glibc resolution (A + AAAA) ----
        resolv = until(lambda: (lambda r: r if '2001:db8:53::53' in r else None)(net.rpc('FILE /etc/resolv.conf', retry=True)), 20, 'DHCPv6 DNS in resolv.conf')
        servers = [line.split()[1] for line in resolv.splitlines() if line.startswith('nameserver')]
        search = [line.split()[1:] for line in resolv.splitlines() if line.startswith('search')]
        assert servers == ['192.168.50.1', '2001:db8:1::53', '2001:db8:53::53'], resolv
        assert search and {'lan.test', 'example.test', 'corp6.test'} <= set(search[0]), resolv
        answer = net.rpc('RESOLVE host6.example.test', retry=True).split()
        assert answer[:2] == ['RESULT', '0'] and '2001:db8:5::80' in answer and '192.0.2.80' in answer, answer
        assert {q[3] for q in net.dnslog if q[2] == 'host6.example.test'} >= {1, 28}, net.dnslog
        assert net.rpc('UDP4 192.0.2.80 80 10') == 'RESULT 0 10 0' and net.rpc('UDP 192.0.2.80 80 10') == 'RESULT 10 0', 'IPv4 default route'
        assert net.rpc('WRITE /proc/sys/net/nexus/dns 2001:db8:53::99,search=corp.test') == 'OK'
        resolv = net.rpc('FILE /etc/resolv.conf', retry=True)
        assert [l for l in resolv.splitlines() if l.startswith(('nameserver', 'search'))] == ['nameserver 2001:db8:53::99', 'search corp.test'], resolv
        before = len(net.dnslog); answer = net.rpc('RESOLVE host6.example.test', retry=True).split()
        assert answer[:2] == ['RESULT', '0'] and '2001:db8:5::80' in answer, answer
        assert any(q[0] == 6 and q[1] == '2001:db8:53::99' for q in net.dnslog[before:]), net.dnslog[before:]
        assert net.rpc('WRITE /proc/sys/net/nexus/dns EMPTY') == 'OK' and '192.168.50.1' in net.rpc('FILE /etc/resolv.conf', retry=True)
        assert net.rpc('WRITE /proc/sys/net/nexus/dns 224.0.0.1').startswith('ERR')
        note(f'DNS: resolv.conf {servers} search {search[0]}; glibc A+AAAA over IPv4 and (static server) over IPv6')

        # ---- ping, TCP, ICMP errors, PMTU, NUD failure ----
        assert net.rpc('PING 2001:db8:1::1', retry=True) == 'RESULT 64 129'
        assert net.rpc('TCP 2001:db8:5::80 80') == 'RESULT 0'
        syn = [e for e in net.tcplog if e[1] == TCP_OPEN and e[3] & 0x02][0]
        assert syn[5] and syn[4] == 1340, ('SYN checksum / MSS from the RA MTU 1400', syn)
        assert net.rpc('TCP 2001:db8:5::81 80') == 'RESULT 111'
        reply = net.rpc('UDPERR 2001:db8:5::7 9 1300').split()
        assert reply == ['RESULT', '90', '2', '0', '1280', '1280'], reply
        t = time.monotonic(); assert net.rpc('UDP 2001:db8:5::7 9 1300') == 'RESULT 1300 0'
        train = net.wait(lambda: [e for e in net.since(net.frag6log, t) if e[1] == PTB_HOST], 5, 'fragments')
        net.wait(lambda: [e for e in net.since(net.frag6log, t) if e[1] == PTB_HOST and not e[3]], 5, 'last fragment')
        train = [e for e in net.since(net.frag6log, t) if e[1] == PTB_HOST]
        assert len(train) == 2 and all(e[4] <= 1280 for e in train), train
        t = time.monotonic(); reply = net.rpc('UDPERR 2001:db8:1::99 9 100').split()
        assert reply[:4] == ['RESULT', '113', '1', '3'], reply
        probes = [e for e in net.since(net.icmp6log, t) if e[1] == 135 and e[7][8:24] == UNREACHABLE]
        assert len(probes) == 3 and all(e[4][:13] == a6('ff02::1:ff00:0')[:13] for e in probes), probes
        neigh = {canon(k): v for k, v in (line.split(' ', 1) for line in net.rpc('NEIGH', retry=True).strip().splitlines())}
        assert int(neigh['2001:db8:1::99'].split()[0], 16) == 0x20 and int(neigh['fe80::1'].split()[1], 16) & 0x80, neigh
        assert net.rpc('TCP 2001:db8:1::99 80').split()[1] in ('113', '110'), 'TCP to a failed neighbour'
        note('ping6, TCP connect (checksum, MSS 1340), ICMPv6 port unreachable -> ECONNREFUSED, PTB -> EMSGSIZE/IPV6_MTU 1280 + fragmentation, NUD failure -> EHOSTUNREACH after 3 NS')

        # ---- redirects: bogus ones ignored, the router's honoured ----
        def to_redirected():
            t = time.monotonic(); assert net.rpc('UDP 2001:db8:5::9 9 20') == 'RESULT 20 0'
            return net.wait(lambda: [e for e in net.since(net.udp6log, t) if e[3] == REDIRECTED], 5, 'redirected datagram')[0][1]
        def redirect(src, hlim=255):
            body = b'\0\0\0\0' + R2_LL + REDIRECTED + bytes([2, 1]) + R2_MAC
            net.send6(ip6(src, GUEST_LL, 58, icmp6(src, GUEST_LL, 137, 0, body), hlim=hlim))
        assert to_redirected() == R_MAC
        redirect(R2_LL); redirect(R_LL, hlim=64); time.sleep(0.5)
        assert to_redirected() == R_MAC, 'a redirect from a non-router or with hop limit 64 was accepted'
        redirect(R_LL); time.sleep(0.5)
        assert to_redirected() == R2_MAC, 'the router redirect was ignored'
        note('redirect: from the router honoured (next hop fe80::2), from a non-router / hop limit 64 ignored')

        # ---- MLD: state change report, v2 general query, query validation, MLDv1 compatibility ----
        def reports(t, kind):
            out = []
            for e in net.since(net.icmp6log, t):
                if e[1] == kind == 143:
                    m, at = e[7], 8
                    for _ in range(struct.unpack('!H', e[7][6:8])[0]):
                        out.append((m[at], t6(m[at + 4:at + 20]))); at += 20 + 16 * struct.unpack('!H', m[at + 2:at + 4])[0] + 4 * m[at + 1]
                elif e[1] == kind == 131: out.append((131, t6(e[7][8:24])))
            return out
        t = time.monotonic(); assert net.rpc('JOIN ff05::1234') == 'RESULT 0'
        net.wait(lambda: (4, 'ff05::1234') in reports(t, 143), 4, 'MLDv2 state change report')
        time.sleep(3)
        def query(src, v1=False, hlim=1):
            body = struct.pack('!HH', 1000, 0) + ANY6 + (b'' if v1 else struct.pack('!BBH', 2, 125, 0))
            hbh = bytes([58, 0, 5, 2, 0, 0, 1, 0])
            net.send6(ip6(src, ALL_NODES, 0, hbh + icmp6(src, ALL_NODES, 130, 0, body), hlim=hlim), mc_mac(ALL_NODES))
        t = time.monotonic(); query(R_GLOBAL); time.sleep(2.5)
        assert not reports(t, 143), 'a query from a global source was answered'
        t = time.monotonic(); query(R_LL)
        net.wait(lambda: (2, 'ff05::1234') in reports(t, 143), 4, 'MLDv2 report to a general query')
        groups = {g for kind, g in reports(t, 143)}
        assert 'ff02::1:ff12:3456' in groups and 'ff02::1' not in groups, groups
        time.sleep(1.5); t = time.monotonic(); query(R_LL, v1=True)
        try: net.wait(lambda: (131, 'ff05::1234') in reports(t, 131), 4, 'MLDv1 report')
        except AssertionError:
            raise AssertionError(('MLDv1', [(round(e[0] - t, 2), e[1], t6(e[4])) for e in net.since(net.icmp6log, t - 3)], reports(t - 3, 143)))
        note(f'MLD: state change + general query reports ({sorted(groups)}), global-source query ignored, MLDv1 fallback')

        # ---- hostile IPv6: extension headers, options, fragments ----
        def hostile(n): return a6(f'2001:db8:66::{n:x}')
        def udp_rpc(source, port, data): return udp6(source, GUEST_GLOBAL, port, 7001, data)
        t = time.monotonic()
        s = hostile(1); net.send6(ip6(s, GUEST_GLOBAL, 253, bytes(8))); param_problem(net, t, s, 1, 6)
        s = hostile(2); net.send6(ip6(s, GUEST_GLOBAL, 60, bytes([0, 0, 1, 4, 0, 0, 0, 0]) + bytes([17, 0, 1, 4, 0, 0, 0, 0]) + udp_rpc(s, 9, b'x')))
        param_problem(net, t, s, 1, 40)
        s = hostile(3); net.send6(ip6(s, GUEST_GLOBAL, 43, bytes([17, 2, 0, 1, 0, 0, 0, 0]) + a6('2001:db8::1') + udp_rpc(s, 9, b'x'))); param_problem(net, t, s, 0, 42)
        s = hostile(4); net.send6(ip6(s, GUEST_GLOBAL, 60, bytes([17, 0, 0x80, 4, 0, 0, 0, 0]) + udp_rpc(s, 9, b'x'))); param_problem(net, t, s, 2, 42)
        def opts(header):
            return lambda source, port, data: net.send6(ip6(source, GUEST_GLOBAL, 60, header + udp_rpc(source, port, data)))
        assert net.rpc('SLEEP 1', wrap=opts(bytes([17, 0, 0x1E, 4, 0, 0, 0, 0])), source=hostile(5)) == 'OK', 'unknown skippable option'
        assert net.rpc('SLEEP 1', wrap=opts(bytes([17, 1, 1, 12]) + bytes(12)), source=hostile(6), timeout=3, expect=False) is None, 'excess padding'
        assert net.rpc('SLEEP 1', wrap=opts(bytes([17, 0, 1, 4, 0, 1, 0, 0])), source=hostile(6), timeout=3, expect=False) is None, 'nonzero PadN'
        def chain(count):
            def send(source, port, data):
                body = udp_rpc(source, port, data)
                for i in range(count): body = bytes([17 if i == 0 else 60, 0, 1, 4, 0, 0, 0, 0]) + body
                net.send6(ip6(source, GUEST_GLOBAL, 60, body))
            return send
        assert net.rpc('SLEEP 1', wrap=chain(12), source=hostile(7)) == 'OK', 'a 12-header chain'
        assert net.rpc('SLEEP 1', wrap=chain(20), source=hostile(7), timeout=3, expect=False) is None, '20 headers accepted'
        s = hostile(8); net.send6(ip6(s, GUEST_GLOBAL, 17, udp_rpc(s, 9, b'x' * 40)[:20], plen=100))   # truncated
        net.send6(ip6(s, GUEST_GLOBAL, 0, bytes([17, 0, 0xC2, 4, 0, 1, 0, 0]) + udp_rpc(s, 9, b'x')))     # jumbo option, payload length set
        net.send6(ip6(a6('ff02::1'), GUEST_GLOBAL, 17, udp_rpc(a6('ff02::1'), 9, b'x')))                  # multicast source
        net.send6(ip6(GUEST_GLOBAL, GUEST_GLOBAL, 17, udp_rpc(GUEST_GLOBAL, 9, b'x')))                     # our own source
        net.send6(ip6(s, a6('2001:db8:1::77'), 17, udp6(s, a6('2001:db8:1::77'), 1, 9, b'x')))             # not ours
        net.send6(ip6(s, GUEST_GLOBAL, 17, udp_rpc(s, 9, b'x')[:6] + b'\x12\x34' + b'x'))                 # bad checksum
        def fragments(pieces, ident):
            def send(source, port, data):
                whole = udp_rpc(source, port, data)
                for offset, length, more in pieces(len(whole)):
                    net.send6(ip6(source, GUEST_GLOBAL, 44, fragment_header(17, offset, more, ident) + whole[offset:offset + length]))
            return send
        padded = 'SLEEP 1' + ' ' * 2000
        in_order = lambda n: [(0, 1000, True), (1000, 1000, True), (2000, n - 2000, False)]
        assert net.rpc(padded, wrap=fragments(lambda n: list(reversed(in_order(n))), 0x1001), source=hostile(9)) == 'OK', 'out-of-order fragments'
        overlap = lambda n: [(0, 1000, True), (992, 1008, True), (2000, n - 2000, False)]
        assert net.rpc(padded, wrap=fragments(overlap, 0x1002), source=hostile(9), timeout=3, expect=False) is None, 'overlapping fragments accepted'
        assert net.rpc(padded, wrap=fragments(in_order, 0x1003), source=hostile(9)) == 'OK'
        s = hostile(10); whole = udp_rpc(s, 9, b'y' * 100)
        net.send6(ip6(s, GUEST_GLOBAL, 44, fragment_header(17, 0, True, 0x2001) + whole[:13])); param_problem(net, t, s, 0, 4)
        s = hostile(11); net.send6(ip6(s, GUEST_GLOBAL, 44, fragment_header(60, 0, True, 0x2002) + bytes([17, 2, 1, 4, 0, 0, 0, 0])))
        param_problem(net, t, s, 3, 0)
        s = hostile(12); net.send6(ip6(s, GUEST_GLOBAL, 44, fragment_header(17, 65528, False, 0x2003) + bytes(16))); param_problem(net, t, s, 0, 42)
        assert net.rpc('WRITE /proc/sys/net/ipv6/ip6frag_time 2') == 'OK'
        s = hostile(13); t13 = time.monotonic(); net.send6(ip6(s, GUEST_GLOBAL, 44, fragment_header(17, 0, True, 0x2004) + whole[:16]))
        e = net.wait(lambda: [e for e in net.since(net.icmp6log, t13) if e[1] == 3 and e[4] == s], 8, 'reassembly time exceeded')[0]
        assert e[2] == 1 and e[0] - t13 >= 1.5, ('time exceeded', e[2], e[0] - t13)
        assert net.rpc('WRITE /proc/sys/net/ipv6/ip6frag_time 60') == 'OK'
        s = hostile(14)
        for i in range(200): net.send6(ip6(s, GUEST_GLOBAL, 44, fragment_header(17, 0, True, 0x3000 + i) + whole[:96]))
        assert net.rpc(padded, wrap=fragments(in_order, 0x1004), source=hostile(9)) == 'OK', 'reassembly blocked by a fragment flood'
        # IPv4: overlapping fragments dropped, proper ones reassembled
        def frag4(pieces, ident):
            def send(source, port, data):
                whole = udp4(RPC4, net.guest4, port, 7001, data)
                for offset, length, more in pieces(len(whole)):
                    net.send4(ip4(RPC4, net.guest4, 17, whole[offset:offset + length], ident=ident, frag=(offset // 8) | (0x2000 if more else 0)))
            return send
        assert net.rpc(padded, wrap=frag4(overlap, 77), timeout=3, expect=False) is None, 'overlapping IPv4 fragments accepted'
        assert net.rpc(padded, wrap=frag4(lambda n: list(reversed(in_order(n))), 78)) == 'OK', 'IPv4 fragments'
        assert net.rpc('PING 2001:db8:1::1', retry=True) == 'RESULT 64 129'
        counters = snmp6(net)
        assert counters['Ip6ReasmOKs'] >= 3 and counters['Ip6ReasmFails'] >= 5 and counters['Ip6ReasmTimeout'] >= 1 and counters['Ip6InHdrErrors'] >= 8, counters
        note(f'hostile IPv6: 6 parameter problems with Linux codes/pointers, skippable option delivered, padding/chain/jumbo/truncated/'
             f'martian packets dropped, out-of-order/overlap/tiny/oversize fragments, reassembly timeout, 200-fragment flood; '
             f'IPv4 overlap; Ip6ReasmOKs {counters["Ip6ReasmOKs"]} Fails {counters["Ip6ReasmFails"]} InHdrErrors {counters["Ip6InHdrErrors"]}')

        # ---- router advertisements: validation, a second prefix, default router lifetime 0 ----
        bad = lambda tag: ((a6(f'2001:db8:{tag}::'), 64, 0xC0, 600, 300),)
        net.ra(hlim=64, prefixes=bad('bad1')); net.ra(src=R_GLOBAL, prefixes=bad('bad2'))
        net.ra(prefixes=((a6('2001:db8:bad3::'), 48, 0xC0, 600, 300),)); net.ra(prefixes=((a6('fe80::'), 64, 0xC0, 600, 300),))
        net.ra(prefixes=((a6('2001:db8:bad4::'), 64, 0xC0, 600, 900),))   # preferred > valid
        net.ra(prefixes=((PREFIX2, 64, 0xC0, 600, 300),), mtu=1000)
        time.sleep(3)
        table = addrs(net)
        assert not [a for a in table if 'bad' in a or (a.startswith('fe80') and a != canon('fe80::5054:ff:fe12:3456/64'))], table
        assert canon('2001:db8:2::5054:ff:fe12:3456/64') in table, table
        net.ra(lifetime=0, prefixes=(), dns=False); time.sleep(0.5)
        assert net.rpc('TCP 2001:db8:5::80 80', source=ONLINK_RPC) == 'RESULT 101', 'default router lifetime 0'
        net.ra(); time.sleep(0.5)
        assert net.rpc('TCP 2001:db8:5::80 80') == 'RESULT 0'
        note('RA: hop limit 64 / global source / /48 / link-local / preferred>valid prefixes ignored, second prefix configured, lifetime 0 removes the default route')

        # ---- DAD: a defended address fails, NODAD skips it ----
        net.defend.add(DEFENDED); t = time.monotonic()
        assert net.rpc('ADDR 2001:db8:1::abcd/64 eth0') == 'RESULT 0'
        assert net.rpc('ADDR 2001:db8:1::abce/64 eth0 0x02') == 'RESULT 0'
        time.sleep(2.5); table = addrs(net)
        assert table[canon('2001:db8:1::abcd/64')][1] & 0x48 == 0x48, table   # tentative + dadfailed
        assert not table[canon('2001:db8:1::abce/64')][1] & 0x48, table
        assert not [e for e in net.since(net.icmp6log, t) if e[1] == 135 and e[3] == ANY6 and e[7][8:24] == NODAD]
        assert net.rpc('ADDR 2001:db8:1::abce/64 eth0') == 'RESULT -17'
        assert net.rpc('DELADDR 2001:db8:1::abcd/64 eth0') == 'RESULT 0' and net.rpc('DELADDR 2001:db8:1::abce/64 eth0') == 'RESULT 0'
        note('DAD: defended address -> tentative+dadfailed, IFA_F_NODAD skips probing, duplicate add -> EEXIST')

        # ---- carrier: QMP set_link off/on -> netlink events, DAD + RS again, DHCP INIT-REBOOT (NAKed) -> new lease ----
        net.rpc('EVENTS', retry=True)
        net.dhcp_mode = 'nak-reboot'; net.dhcp_ip = '192.168.50.11'
        qmp('set_link', {'name': 'network', 'up': False}); time.sleep(3)
        t = time.monotonic(); qmp('set_link', {'name': 'network', 'up': True})
        reboot = net.wait(lambda: [e for e in net.since(net.dhcplog, t) if e['type'] == 3 and e['requested'] == '192.168.50.10' and not e['server']], 20, 'INIT-REBOOT')[0]
        assert reboot['ciaddr'] == '0.0.0.0' and reboot.get('answer') == 6, reboot
        net.wait(lambda: [e for e in net.since(net.dhcplog, t) if e.get('answer') == 5], 30, 'new lease after NAK')
        ack_time = [e for e in net.since(net.dhcplog, t) if e.get('answer') == 5][-1]['t']
        net.wait(lambda: [e for e in net.since(net.icmp6log, t) if e[1] == 135 and e[3] == ANY6 and e[7][8:24] == GUEST_LL], 10, 'DAD after link up')
        net.wait(lambda: [e for e in net.since(net.icmp6log, t) if e[1] == 133], 10, 'RS after link up')
        net.guest4 = a4('192.168.50.11'); time.sleep(3)
        events = net.rpc('EVENTS', retry=True).splitlines()[1:]
        links = [l.split() for l in events if l.startswith('LINK 2 ')]
        assert any(not int(l[2], 16) & 0x10000 and l[3] == '2' for l in links) and int(links[-1][2], 16) & 0x10000 and links[-1][3] == '6', links
        assert 'DELADDR 192.168.50.10/24 2' in events and 'NEWADDR 192.168.50.11/24 2' in events, events
        assert '192.168.50.11/24' in addrs(net, '4')
        assert net.rpc('SLEEP 10', v4=True) == 'OK'
        note(f'carrier: set_link off/on -> RTM_NEWLINK (lower_up, operstate), DAD + RS rerun, INIT-REBOOT NAKed -> new lease 192.168.50.11; {len(events)} events')

        # ---- DHCPv4 lifecycle: renew (unicast), rebind (broadcast), expiry, rediscovery; release ----
        renewed = [e for e in net.dhcplog if e['type'] == 3 and e['ciaddr'] != '0.0.0.0' and not e['broadcast'] and e.get('answer') == 5]
        assert renewed and renewed[0]['dst'] == '192.168.50.1' and not renewed[0]['requested'] and not renewed[0]['server'], renewed
        net.dhcp_mode = 'silent'; net.rpc('EVENTS', retry=True)
        ack_time = max(e['t'] for e in net.dhcplog if e.get('answer') == 5)
        renew = net.wait(lambda: [e for e in net.dhcplog if e['t'] > ack_time and e['type'] == 3], 25, 'renew')[0]
        rebind = net.wait(lambda: [e for e in net.dhcplog if e['t'] > ack_time and e['type'] == 3 and e['dst'] == '255.255.255.255'], 25, 'rebind')[0]
        discover = net.wait(lambda: [e for e in net.dhcplog if e['t'] > ack_time and e['type'] == 1], 25, 'rediscovery')[0]
        assert renew['dst'] == '192.168.50.1' and renew['ciaddr'] == '192.168.50.11' and 13 <= renew['t'] - ack_time <= 17, (renew, renew['t'] - ack_time)
        assert rebind['ciaddr'] == '192.168.50.11' and rebind['broadcast'] and 28 <= rebind['t'] - ack_time <= 32, (rebind, rebind['t'] - ack_time)
        assert 39 <= discover['t'] - ack_time <= 44, discover['t'] - ack_time
        events = net.rpc('EVENTS', retry=True)
        assert 'DELADDR 192.168.50.11/24 2' in events and not [a for a in addrs(net, '4') if a.startswith('192.168.50.')], events
        assert '192.168.50.1' not in net.rpc('FILE /etc/resolv.conf', retry=True)
        net.dhcp_mode = 'normal'
        net.wait(lambda: [e for e in net.dhcplog if e['t'] > discover['t'] and e.get('answer') == 5], 40, 'lease after expiry')
        time.sleep(1); assert '192.168.50.11/24' in addrs(net, '4')
        t = time.monotonic(); assert net.rpc('WRITE /proc/sys/net/nexus/dhcp 0') == 'OK'
        release = net.wait(lambda: [e for e in net.since(net.dhcplog, t) if e['type'] == 7], 5, 'DHCPRELEASE')[0]
        assert release['ciaddr'] == '192.168.50.11' and release['server'] == '192.168.50.1' and not [a for a in addrs(net, '4') if a.startswith('192.168.50.')]
        assert net.rpc('FILE /proc/sys/net/nexus/dhcp').strip() == '0'
        t = time.monotonic(); assert net.rpc('WRITE /proc/sys/net/nexus/dhcp 1') == 'OK'
        net.wait(lambda: [e for e in net.since(net.dhcplog, t) if e.get('answer') == 5], 20, 'lease after re-enable')
        time.sleep(1); assert '192.168.50.11/24' in addrs(net, '4')
        note(f'DHCPv4: renew unicast at {renew["t"] - ack_time:.1f} s, rebind broadcast at {rebind["t"] - ack_time:.1f} s, expiry -> address removed, '
             f'rediscover at {discover["t"] - ack_time:.1f} s; release on disable, lease again on enable; {len(renewed)} successful renewals')

        assert net.rpc('EXIT') == 'BYE'
        qemu.wait(timeout=60)
        log = serial_text()
        assert qemu.returncode == 33 and '[64] INIT SESSION RETURNED' in log, (qemu.returncode, log[-4000:])
        assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and 'NETWIRE AGENT DONE' in log, log[-4000:]
        assert not net.errors, net.errors
    except BaseException:
        print(serial_text()[-6000:]); print('\n'.join(net.errors)[-4000:])
        now = time.monotonic()
        for e in net.ip6log[-25:]: print(f'  ip6 {e[0] - now:7.2f} {e[1].hex()} {t6(e[2])} -> {t6(e[3])} nh {e[4]} len {e[5]}')
        print('  alive:', net.rpc('PING 2001:db8:1::1', timeout=5, expect=False))
        raise
    finally:
        net.stop = True
        if qemu.poll() is None: qemu.kill(); qemu.wait()
        net.sock.close(); disk.unlink(missing_ok=True)
    print(f'PASS native wire-level IPv6/IPv4 ({SMP} CPU): ' + '; '.join(results))

if __name__ == '__main__': main()
