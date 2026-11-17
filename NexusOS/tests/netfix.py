"""netfix: this Python process is a QEMU guest's whole network, for NexusOS network tests (QEMU `-netdev dgram`).

A Fixture owns Links; a Link is one Ethernet segment = one UDP socket paired with one guest NIC's dgram netdev:
    -netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port=QEMU_PORT,
            remote.type=inet,remote.host=127.0.0.1,remote.port=FIXTURE_PORT
On a link the fixture plays every other host, all behind one MAC per link:
  * IPv6 router: RA (prefixes, MTU, M/O flags, router lifetime, RDNSS, DNSSL) on RS and periodically, NA for its
    on-link addresses, stateless DHCPv6 (DNS servers + search list), ICMPv6 echo;
  * IPv4: ARP, DHCPv4 server (one lease), ICMP echo;
  * DNS server (UDP) with configurable A/AAAA records (NXDOMAIN / NODATA otherwise);
  * a small user-space TCP for both families: passive and active open, in-order delivery, go-back-N retransmission,
    FIN/RST; an HTTP/1.x server on top (one request per connection, Connection: close);
  * UDP services (echo, callbacks), multicast receive hooks (mDNS tests);
  * blackholed addresses (every packet silently dropped: a dead path for happy-eyeballs tests); other unknown
    destinations get ICMP/ICMPv6 destination unreachable from the router.
Every event is logged (fx.events, dicts with 't' and 'kind') for assertions; fx.wait() blocks on a predicate."""
import random
import socket
import struct
import threading
import time
import traceback


def a6(text): return socket.inet_pton(socket.AF_INET6, text)
def t6(raw): return socket.inet_ntop(socket.AF_INET6, raw)
def a4(text): return socket.inet_aton(text)
def t4(raw): return socket.inet_ntoa(raw)
def ta(raw): return t4(raw) if len(raw) == 4 else t6(raw)
def addr(text): return a6(text) if ':' in text else a4(text)


def checksum(data):
    if len(data) & 1: data += b'\0'
    total = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while total >> 16: total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def pseudo(src, dst, proto, length):
    if len(src) == 16: return src + dst + struct.pack('!I3xB', length, proto)
    return src + dst + struct.pack('!BBH', 0, proto, length)


def eth(dst, src, kind, payload): return dst + src + struct.pack('!H', kind) + payload


def ip6(src, dst, nh, payload, hlim=64):
    return struct.pack('!IHBB', 0x60000000, len(payload), nh, hlim) + src + dst + payload


def ip4(src, dst, proto, payload, ttl=64, ident=0, tos=0, df=True):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, tos, 20 + len(payload), ident, 0x4000 if df else 0, ttl, proto, 0, src, dst)
    return header[:10] + struct.pack('!H', checksum(header)) + header[12:] + payload


def udp(src, dst, sport, dport, data):
    body = struct.pack('!HHHH', sport, dport, 8 + len(data), 0) + data
    c = checksum(pseudo(src, dst, 17, len(body)) + body) or 0xFFFF
    return body[:6] + struct.pack('!H', c) + body[8:]


def icmp6(src, dst, kind, code, body):
    m = struct.pack('!BBH', kind, code, 0) + body
    return m[:2] + struct.pack('!H', checksum(pseudo(src, dst, 58, len(m)) + m)) + m[4:]


def icmp4(kind, code, body):
    m = struct.pack('!BBH', kind, code, 0) + body
    return m[:2] + struct.pack('!H', checksum(m)) + m[4:]


def tcp_segment(src, dst, sport, dport, seq, ack, flags, window=65535, options=b'', data=b''):
    header = struct.pack('!HHIIBBHHH', sport, dport, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF, ((20 + len(options)) // 4) << 4, flags,
                         window, 0, 0) + options
    segment = header + data
    c = checksum(pseudo(src, dst, 6, len(segment)) + segment)
    return segment[:16] + struct.pack('!H', c) + segment[18:]


def mc_mac6(dst): return b'\x33\x33' + dst[12:]
def mc_mac4(dst): return bytes([1, 0, 0x5E, dst[1] & 0x7F, dst[2], dst[3]])
def dns_name(name): return b''.join(bytes([len(label)]) + label.encode() for label in name.strip('.').split('.') if label) + b'\0'


def dns_read_name(data, at, depth=0):
    """(name, next offset) of a possibly compressed DNS name; raises ValueError on garbage."""
    labels, jumped, end = [], False, None
    for _ in range(128):
        if at >= len(data): raise ValueError('name past the end')
        length = data[at]
        if length == 0:
            return '.'.join(labels), (end if jumped else at + 1)
        if length & 0xC0 == 0xC0:
            if at + 1 >= len(data): raise ValueError('pointer past the end')
            if not jumped: end = at + 2
            at, jumped = ((length & 0x3F) << 8) | data[at + 1], True
            continue
        labels.append(data[at + 1:at + 1 + length].decode('utf-8', 'replace')); at += 1 + length
    raise ValueError('name loop')


ETH_IP4, ETH_ARP, ETH_IP6 = 0x0800, 0x0806, 0x86DD
SYN, ACK, FIN, RST, PSH = 0x02, 0x10, 0x01, 0x04, 0x08
ALL_NODES = a6('ff02::1')
BCAST = b'\xff' * 6
M32 = 0xFFFFFFFF


def seq_lt(a, b): return ((a - b) & M32) > 0x7FFFFFFF
def seq_le(a, b): return a == b or seq_lt(a, b)


class Tcp:
    """One user-space TCP connection of the fixture (local = the fixture's end)."""
    def __init__(self, fx, link, local, lport, remote, rport, app):
        self.fx, self.link, self.local, self.lport, self.remote, self.rport, self.app = fx, link, local, lport, remote, rport, app
        self.family = 6 if len(local) == 16 else 4
        self.iss = random.getrandbits(32); self.snd_una = self.snd_nxt = self.snd_max = (self.iss + 1) & M32
        self.buffer = b''; self.close_wanted = False; self.fin_seq = None; self.fin_acked = False
        self.rcv_nxt = 0; self.peer_fin = False; self.peer_window = 65535; self.peer_mss = 536
        self.state = 'CLOSED'; self.rto = 0.5; self.timer = None; self.retries = 0; self.created = time.monotonic()
        self.received = 0; self.sent_bytes = 0

    @property
    def key(self): return (self.link.name, self.local, self.lport, self.remote, self.rport)

    def mss(self): return max(64, min(self.peer_mss, self.link.mtu - (60 if self.family == 6 else 40)))

    def segment(self, seq, flags, data=b'', options=b''):
        ack = self.rcv_nxt if flags & ACK else 0
        packet = tcp_segment(self.local, self.remote, self.lport, self.rport, seq, ack, flags, 65535, options, data)
        self.fx.send_ip(self.link, self.local, self.remote, 6, packet)

    def arm(self):
        self.timer = time.monotonic() + self.rto

    # ---- the application side ----
    def send(self, data):
        if self.close_wanted: raise RuntimeError('send after close')
        self.buffer += data; self.output()

    def close(self):
        self.close_wanted = True; self.output()

    def abort(self):
        if self.state != 'CLOSED': self.segment(self.snd_nxt, RST | ACK)
        self.finish('aborted')

    def finish(self, why):
        if self.state == 'CLOSED' and self.key not in self.fx.tcp: return
        self.state = 'CLOSED'; self.fx.tcp.pop(self.key, None)
        self.fx.log('tcp-close', link=self.link.name, family=self.family, local=ta(self.local), lport=self.lport,
                    remote=ta(self.remote), rport=self.rport, why=why, received=self.received, sent=self.sent_bytes)
        closed = getattr(self.app, 'on_closed', None)
        if closed: closed(self, why)

    # ---- transmission ----
    def output(self):
        if self.state != 'ESTABLISHED': return
        flight = (self.snd_nxt - self.snd_una) & M32
        offset = min(flight, len(self.buffer))
        window = max(self.peer_window, 1 if not flight else 0)   # a zero window gets one-byte probes (retransmit timer)
        sent = False
        while offset < len(self.buffer) and flight < window:
            n = min(self.mss(), len(self.buffer) - offset, window - flight)
            if n <= 0: break
            last = offset + n == len(self.buffer)
            self.segment((self.snd_una + offset) & M32, ACK | (PSH if last else 0), self.buffer[offset:offset + n])
            offset += n; flight += n; self.snd_nxt = (self.snd_una + offset) & M32; sent = True
        if self.close_wanted and self.fin_seq is None and offset == len(self.buffer):
            self.fin_seq = self.snd_nxt; self.segment(self.snd_nxt, FIN | ACK); self.snd_nxt = (self.snd_nxt + 1) & M32; sent = True
        if seq_lt(self.snd_max, self.snd_nxt): self.snd_max = self.snd_nxt
        if sent and self.timer is None: self.arm()

    def tick(self, now):
        if self.timer is None or now < self.timer: return
        self.retries += 1
        if self.retries > 12: self.finish('timeout'); return
        self.rto = min(self.rto * 2, 4.0); self.arm()
        if self.state == 'SYN_RECEIVED':
            self.segment(self.iss, SYN | ACK, options=struct.pack('!BBH', 2, 4, self.link.mtu - (60 if self.family == 6 else 40)))
        elif self.state == 'SYN_SENT':
            self.segment(self.iss, SYN, options=struct.pack('!BBH', 2, 4, self.link.mtu - (60 if self.family == 6 else 40)))
        elif self.state == 'ESTABLISHED':
            self.fx.log('tcp-retransmit', remote=ta(self.remote), rport=self.rport, una=self.snd_una, nxt=self.snd_nxt)
            self.snd_nxt = self.snd_una                                # go back N
            if self.fin_seq is not None and not self.fin_acked: self.fin_seq = None
            if not self.buffer and not (self.close_wanted and not self.fin_acked): self.timer = None; return
            self.output()

    # ---- reception ----
    def options(self, segment, size):
        at = 20
        while at < size:
            kind = segment[at]
            if kind == 0: break
            if kind == 1: at += 1; continue
            if at + 1 >= size: break
            length = segment[at + 1]
            if kind == 2 and length == 4: self.peer_mss = struct.unpack('!H', segment[at + 2:at + 4])[0]
            at += max(length, 2)

    def input(self, seq, ack, flags, window, segment, size):
        data = segment[size:]
        if flags & RST:
            self.finish('reset'); return
        if self.state == 'SYN_SENT':
            if flags & SYN and flags & ACK and ack == (self.iss + 1) & M32:
                self.options(segment, size); self.rcv_nxt = (seq + 1) & M32; self.snd_una = ack; self.peer_window = window
                self.state = 'ESTABLISHED'; self.timer = None; self.retries = 0; self.rto = 0.5
                self.segment(self.snd_nxt, ACK)
                self.fx.log('tcp-connected', link=self.link.name, family=self.family, remote=ta(self.remote), rport=self.rport)
                opened = getattr(self.app, 'on_open', None)
                if opened: opened(self)
                self.output()
            return
        if flags & SYN:
            if self.state == 'SYN_RECEIVED' and seq == (self.rcv_nxt - 1) & M32:   # the SYN again: SYN-ACK again
                self.segment(self.iss, SYN | ACK, options=struct.pack('!BBH', 2, 4, self.link.mtu - (60 if self.family == 6 else 40)))
            else:
                self.segment(self.snd_nxt, ACK)
            return
        if not flags & ACK: return
        if self.state == 'SYN_RECEIVED':
            if ack != (self.iss + 1) & M32: self.segment(ack, RST); return
            self.state = 'ESTABLISHED'; self.timer = None; self.retries = 0; self.rto = 0.5
            opened = getattr(self.app, 'on_open', None)
            if opened: opened(self)
        # acknowledgement of our data / FIN
        if seq_lt(self.snd_una, ack) and seq_le(ack, self.snd_max):
            acked = (ack - self.snd_una) & M32
            self.buffer = self.buffer[min(acked, len(self.buffer)):]
            self.snd_una = ack
            if seq_lt(self.snd_nxt, ack): self.snd_nxt = ack
            if self.fin_seq is not None and ack == (self.fin_seq + 1) & M32: self.fin_acked = True
            self.retries = 0; self.rto = 0.5
            self.timer = None if self.snd_una == self.snd_max else time.monotonic() + self.rto
        self.peer_window = window
        # data, in order only (the guest retransmits what we drop)
        need_ack = False
        if data:
            offset = (self.rcv_nxt - seq) & M32
            if offset < len(data) and not seq_lt(self.rcv_nxt, seq):
                fresh = data[offset:]; self.rcv_nxt = (self.rcv_nxt + len(fresh)) & M32; self.received += len(fresh)
                on_data = getattr(self.app, 'on_data', None)
                if on_data: on_data(self, fresh)
            need_ack = True
        if flags & FIN and (seq + len(data)) & M32 == self.rcv_nxt and not self.peer_fin:
            self.peer_fin = True; self.rcv_nxt = (self.rcv_nxt + 1) & M32; need_ack = True
            on_eof = getattr(self.app, 'on_eof', None)
            if on_eof: on_eof(self)
            else: self.close_wanted = True
        elif flags & FIN: need_ack = True
        if self.state == 'CLOSED': return
        self.output()
        if need_ack: self.segment(self.snd_nxt, ACK)
        if self.peer_fin and self.fin_acked: self.finish('closed')


class HttpRequest:
    def __init__(self, conn, method, target, version, headers, body):
        self.conn, self.method, self.target, self.version, self.headers, self.body = conn, method, target, version, headers, body
        self.path = target.split('?', 1)[0]; self.query = target.split('?', 1)[1] if '?' in target else ''
        self.host = headers.get('host', '')
        self.family = conn.family; self.client = ta(conn.remote); self.server = ta(conn.local)


class HttpApp:
    """One request per connection; handler(request) -> (status, {header: value}, body bytes)."""
    def __init__(self, fx, handler):
        self.fx, self.handler, self.data = fx, handler, b''

    def on_data(self, conn, data):
        self.data += data
        end = self.data.find(b'\r\n\r\n')
        if end < 0:
            if len(self.data) > 65536: conn.abort()
            return
        lines = self.data[:end].decode('latin1').split('\r\n')
        try: method, target, version = lines[0].split(' ', 2)
        except ValueError: conn.abort(); return
        headers = {}
        for line in lines[1:]:
            if ':' in line: k, v = line.split(':', 1); headers[k.strip().lower()] = v.strip()
        length = int(headers.get('content-length', '0') or 0)
        if len(self.data) < end + 4 + length: return
        body = self.data[end + 4:end + 4 + length]; self.data = b''
        request = HttpRequest(conn, method, target, version, headers, body)
        try: status, extra, payload = self.handler(request)
        except Exception:
            self.fx.errors.append(traceback.format_exc()); status, extra, payload = 500, {}, b'fixture error'
        self.fx.log('http', family=conn.family, client=request.client, server=request.server, port=conn.lport, method=method,
                    host=request.host, path=request.path, query=request.query, agent=headers.get('user-agent', ''), status=status)
        head = 'HTTP/1.1 %d %s\r\n' % (status, {200: 'OK', 204: 'No Content', 404: 'Not Found', 500: 'Error'}.get(status, 'Status'))
        extra = dict(extra); extra.setdefault('Content-Type', 'text/html; charset=utf-8')
        extra['Content-Length'] = str(len(payload)); extra['Connection'] = 'close'; extra.setdefault('Cache-Control', 'no-store')
        head += ''.join('%s: %s\r\n' % kv for kv in extra.items()) + '\r\n'
        conn.send(head.encode('latin1') + (payload if method != 'HEAD' else b'')); conn.sent_bytes += len(payload)
        conn.close()

    def on_eof(self, conn):
        if not self.data: conn.close()


class Relay:
    """A fixture TCP connection relayed to a real socket on this host (e.g. an HTTP or IPP server on 127.0.0.1)."""
    def __init__(self, fx, conn, target):
        self.fx, self.conn = fx, conn
        self.sock = socket.create_connection(target, timeout=10); self.sock.settimeout(None)
        threading.Thread(target=self.pump, daemon=True).start()
    def pump(self):
        while True:
            try: data = self.sock.recv(65536)
            except OSError: data = b''
            with self.fx.lock:
                if self.conn.state == 'CLOSED': return
                if not data:
                    if not self.conn.close_wanted: self.conn.close()
                    return
                self.conn.send(data)
    def on_data(self, conn, data):
        try: self.sock.sendall(data)
        except OSError: conn.abort()
    def on_eof(self, conn):
        try: self.sock.shutdown(socket.SHUT_WR)
        except OSError: pass
    def on_closed(self, conn, why):
        try: self.sock.close()
        except OSError: pass


def dns_records(message):
    """(flags, questions [(name, type, unicast)], records [(name, type, cache_flush, ttl, value)]) of a DNS message;
    values: A/AAAA text, PTR/CNAME names, SRV (priority, weight, port, target), TXT [strings], else raw bytes."""
    flags, qd, an, ns, ar = struct.unpack('!HHHHH', message[2:12]); at = 12; questions, records = [], []
    for _ in range(qd):
        name, at = dns_read_name(message, at); qtype, qclass = struct.unpack('!HH', message[at:at + 4]); at += 4
        questions.append((name.lower(), qtype, bool(qclass & 0x8000)))
    for _ in range(an + ns + ar):
        name, at = dns_read_name(message, at); rtype, rclass, ttl, length = struct.unpack('!HHIH', message[at:at + 10]); at += 10
        data = message[at:at + length]
        if rtype == 1 and length == 4: value = t4(data)
        elif rtype == 28 and length == 16: value = t6(data)
        elif rtype in (12, 5): value = dns_read_name(message, at)[0]
        elif rtype == 33: value = struct.unpack('!HHH', data[:6]) + (dns_read_name(message, at + 6)[0],)
        elif rtype == 16:
            value, i = [], 0
            while i < len(data): value.append(data[i + 1:i + 1 + data[i]].decode('utf-8', 'replace')); i += 1 + data[i]
        else: value = data
        records.append((name.lower(), rtype, bool(rclass & 0x8000), ttl, value)); at += length
    return flags, questions, records


class Mdns:
    """An mDNS (RFC 6762) / DNS-SD (RFC 6763) responder and querier on one link of the fixture: answers questions for
    its own records (A/AAAA of hosts, PTR/SRV/TXT of services), logs every mDNS message ('mdns' events with the parsed
    questions and records) and can ask the network itself."""
    def __init__(self, fx, link, v4='224.0.0.251', v6='ff02::fb'):
        self.fx, self.link, self.group4, self.group6 = fx, link, a4(v4), a6(v6)
        self.records = []          # (name, type, rdata bytes, unique)
        fx.multicast_hooks.append(self.receive)
        fx.udp_services.setdefault(5353, lambda link_, src, dst, sport, data: self.receive(link_, 6 if len(src) == 16 else 4, src, dst, sport, 5353, data, 255) or None)
    def host(self, name, addresses):
        for a in addresses:
            self.records.append((name.lower(), 28 if ':' in a else 1, addr(a), True))
            raw = addr(a)   # reverse (PTR) names: in-addr.arpa / ip6.arpa nibbles
            reverse = ('.'.join(str(b) for b in reversed(raw)) + '.in-addr.arpa') if len(raw) == 4 else \
                      ('.'.join(c for c in reversed(raw.hex())) + '.ip6.arpa')
            self.records.append((reverse, 12, dns_name(name), True))
    def service(self, instance, kind, target, port, txt):
        full = instance + '.' + kind
        self.records.append((kind.lower(), 12, dns_name(full), False))
        self.records.append(('_services._dns-sd._udp.local', 12, dns_name(kind), False))
        self.records.append((full.lower(), 33, struct.pack('!HHH', 0, 0, port) + dns_name(target), True))
        self.records.append((full.lower(), 16, b''.join(bytes([len(s)]) + s.encode() for s in txt) or b'\0', True))
    def message(self, answers, questions=(), ident=0, response=True):
        out = struct.pack('!HHHHHH', ident, 0x8400 if response else 0, len(questions), len(answers), 0, 0)
        for name, qtype in questions: out += dns_name(name) + struct.pack('!HH', qtype, 1)
        for name, rtype, data, unique in answers: out += dns_name(name) + struct.pack('!HHIH', rtype, 0x8001 if unique else 1, 120, len(data)) + data
        return out
    def send(self, payload, family=4, dst=None, dport=5353):
        if family == 4:
            src = next(iter(self.link.onlink4)); self.fx.send_udp(self.link, src, dst or self.group4, 5353, dport, payload, hlim=255)
        else:
            src = next(iter(self.link.routers6)); self.fx.send_udp(self.link, src, dst or self.group6, 5353, dport, payload, hlim=255)
    def query(self, name, qtype, family=4):
        self.send(self.message([], [(name, qtype)], response=False), family)
    def receive(self, link, family, src, dst, sport, dport, data, ttl):
        if link is not self.link or dport != 5353 or len(data) < 12: return
        try: flags, questions, records = dns_records(data)
        except (ValueError, struct.error, IndexError):
            self.fx.log('mdns-bad', family=family, src=ta(src), length=len(data)); return
        self.fx.log('mdns', family=family, src=ta(src), dst=ta(dst), sport=sport, ttl=ttl, response=bool(flags & 0x8000),
                    questions=questions, records=records)
        if flags & 0x8000: return
        answers = []
        for name, qtype, unicast in questions:
            for record in self.records:
                if record[0] == name and (qtype in (255, record[1])) and record not in answers: answers.append(record)
        if not answers: return
        legacy = sport != 5353
        reply = self.message(answers, ident=struct.unpack('!H', data[:2])[0] if legacy else 0,
                             questions=[(n, q) for n, q, _ in questions] if legacy else ())
        if legacy or any(u for _, _, u in questions): self.send(reply, family, dst=src, dport=sport)
        else: self.send(reply, family)


class Link:
    def __init__(self, fx, name, fixture_port, qemu_port, guest_mac=None, mac=None, mtu=1500):
        self.fx, self.name, self.qemu_port, self.mtu = fx, name, qemu_port, mtu
        self.mac = mac or bytes([2, 0, 0, 0, 0, len(fx.links) + 1])
        self.guest_mac = guest_mac
        self.onlink4, self.onlink6 = set(), set()          # fixture addresses answered by ARP / NS on this link
        self.routers6 = set()
        self.guest4 = None
        self.neigh4 = {}                                     # guest IPv4 -> MAC
        self.ra = None; self.dhcp4 = None; self.dhcp6 = None; self.next_ra = 0
        self.sock = self.thread = None
        if fixture_port is None: return                      # (TunnelLink: no socket of its own)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        self.sock.bind(('127.0.0.1', fixture_port))
        self.thread = threading.Thread(target=self.run, daemon=True, name='netfix-' + name)

    def send(self, frame):
        try: self.sock.sendto(frame, ('127.0.0.1', self.qemu_port))
        except OSError: pass

    def run(self):
        self.sock.settimeout(0.2)
        while not self.fx.stopping:
            try: frame, _ = self.sock.recvfrom(65536)
            except socket.timeout: continue
            except ConnectionResetError: continue   # Windows: ICMP port unreachable before QEMU listens
            except OSError: break
            try:
                with self.fx.lock: self.fx.frame(self, frame)
            except Exception:
                self.fx.errors.append(traceback.format_exc())

    # ---- configuration ----
    def router6(self, ll='fe80::1', prefixes=(('2001:db8:1::', 64, 0xC0, 86400, 14400),), rdnss=(), dnssl=(), mtu=None,
                lifetime=1800, managed=False, other=False, interval=20, hlim=64):
        """Router advertisements from link-local `ll` (prefix flags: 0x80 on-link, 0x40 autonomous)."""
        ll = a6(ll); self.onlink6.add(ll); self.routers6.add(ll)
        self.ra = dict(ll=ll, prefixes=[(a6(p), n, f, v, pr) for p, n, f, v, pr in prefixes], rdnss=[a6(x) for x in rdnss],
                       dnssl=list(dnssl), mtu=mtu, lifetime=lifetime, flags=(0x80 if managed else 0) | (0x40 if other else 0),
                       interval=interval, hlim=hlim)
        self.next_ra = 0

    def server4(self, router='192.168.50.1', lease_ip='192.168.50.10', mask='255.255.255.0', dns=('192.168.50.1',),
                domain='lan.test', lease=86400):
        self.onlink4.add(a4(router))
        self.dhcp4 = dict(router=a4(router), ip=a4(lease_ip), mask=a4(mask), dns=[a4(x) for x in dns], domain=domain, lease=lease)

    def server6(self, dns=(), domains=()):
        self.dhcp6 = dict(dns=[a6(x) for x in dns], domains=list(domains))

    def send_ra(self, dst=ALL_NODES):
        r = self.ra
        body = struct.pack('!BBHII', r['hlim'], r['flags'], r['lifetime'], 0, 0) + bytes([1, 1]) + self.mac
        if r['mtu']: body += struct.pack('!BBHI', 5, 1, 0, r['mtu'])
        for prefix, length, flags, valid, preferred in r['prefixes']:
            body += struct.pack('!BBBBIII', 3, 4, length, flags, valid, preferred, 0) + prefix
        if r['rdnss']: body += struct.pack('!BBHI', 25, 1 + 2 * len(r['rdnss']), 0, 600) + b''.join(r['rdnss'])
        if r['dnssl']:
            names = b''.join(dns_name(n) for n in r['dnssl']); names += bytes(-len(names) % 8)
            body += struct.pack('!BBHI', 31, 1 + len(names) // 8, 0, 600) + names
        packet = ip6(r['ll'], dst, 58, icmp6(r['ll'], dst, 134, 0, body), hlim=255)
        self.send(eth(mc_mac6(dst) if dst[0] == 0xFF else (self.guest_mac or BCAST), self.mac, ETH_IP6, packet))
        self.fx.log('ra', link=self.name)


class TunnelLink(Link):
    def __init__(self, fx, name, carrier, local, remote, port, mac=None, mtu=1500):
        super().__init__(fx, name, None, None, None, mac, mtu)
        self.carrier, self.local, self.remote, self.port = carrier, addr(local), addr(remote), port
        fx.udp_services[port] = self.input
    def input(self, link, src, dst, sport, data):
        if link is self.carrier and src == self.remote and sport == self.port: self.fx.frame(self, data)
        return None
    def send(self, frame):
        self.fx.send_udp(self.carrier, self.local, self.remote, self.port, self.port, frame)


class Fixture:
    def __init__(self):
        self.lock = threading.RLock(); self.cond = threading.Condition(self.lock)
        self.links = []; self.events = []; self.errors = []; self.stopping = False
        self.hosts = set()            # addresses (4 or 16 bytes) the fixture answers for (servers behind the router too)
        self.blackholes = set()       # addresses whose packets are dropped silently
        self.dns_servers = set()
        self.records = {}             # (name, qtype) -> [rdata]
        self.listeners = {}           # port -> app factory(conn)
        self.udp_services = {}        # port -> f(link, src, dst, sport, data) -> reply bytes or None
        self.multicast_hooks = []     # f(link, family, src, dst, sport, dport, data)
        self.tcp = {}; self.fragments = {}
        self.ip4_hook = None          # optional f(link, packet) -> True when consumed
        self.ip6_hook = None
        self.udp_services[7] = lambda link, src, dst, sport, data: data   # echo
        self.timer = threading.Thread(target=self.ticker, daemon=True, name='netfix-timer')

    # ---- set-up ----
    def link(self, name, fixture_port, qemu_port, guest_mac=None, mac=None, mtu=1500):
        link = Link(self, name, fixture_port, qemu_port, guest_mac, mac, mtu); self.links.append(link); return link

    def tunnel(self, name, carrier, local, remote, port, mac=None, mtu=1500):
        """A second Ethernet segment carried in UDP over `carrier` to a guest relay (arch/x86_64/taprelay_user.c):
        frames arrive as UDP datagrams to `local`:`port` from the guest's `remote`:`port`."""
        link = TunnelLink(self, name, carrier, local, remote, port, mac, mtu); self.links.append(link); return link

    def start(self):
        for link in self.links:
            if link.thread: link.thread.start()
        self.timer.start(); return self

    def stop(self):
        self.stopping = True
        for link in self.links:
            try:
                if link.sock: link.sock.close()
            except OSError: pass

    def host(self, *texts):
        for text in texts: self.hosts.add(addr(text))

    def blackhole(self, *texts):
        for text in texts: self.blackholes.add(addr(text))

    def dns_server(self, *texts):
        for text in texts: self.dns_servers.add(addr(text)); self.hosts.add(addr(text))

    def name(self, name, a=(), aaaa=()):
        name = name.lower().strip('.')
        self.records[(name, 1)] = [a4(x) for x in a]; self.records[(name, 28)] = [a6(x) for x in aaaa]

    def http(self, port, handler):
        self.listeners[port] = lambda conn: HttpApp(self, handler)

    def listen(self, port, factory): self.listeners[port] = factory

    def relay(self, port, target_port, target_host='127.0.0.1'):
        """TCP to the fixture's `port` (any served address) is relayed to target_host:target_port on this host."""
        self.listeners[port] = lambda conn: Relay(self, conn, (target_host, target_port))

    def connect(self, link, local, remote, port, app, lport=None):
        """Active open from fixture address `local` to the guest's `remote`:`port`."""
        with self.lock:
            c = Tcp(self, link, addr(local), lport or random.randint(20000, 60000), addr(remote), port, app)
            c.state = 'SYN_SENT'; self.tcp[c.key] = c
            c.segment(c.iss, SYN, options=struct.pack('!BBH', 2, 4, link.mtu - (60 if c.family == 6 else 40))); c.arm()
            return c

    def rpc(self, link, src, dst, text, timeout=20, port=7001, resend=4.0, expect=True):
        """UDP request/response with a guest agent (arch/x86_64/netwire_user.c protocol): `text` from fixture address
        `src` to the guest's `dst`:`port`; the first datagram back is the answer (resent every `resend` s)."""
        src, dst, data = addr(src), addr(dst), text.encode()
        with self.lock:
            self.rpc_port = getattr(self, 'rpc_port', 41000) + 1; sport = self.rpc_port; box = []
            def answer(link_, s, d, sp, payload):
                box.append(payload); self.cond.notify_all(); return None
            self.udp_services[sport] = answer
        try:
            deadline = time.monotonic() + timeout; next_send = 0
            with self.cond:
                while not box:
                    now = time.monotonic()
                    if now >= deadline:
                        if expect: raise AssertionError('no answer to %r' % text[:80])
                        return None
                    if now >= next_send: self.send_udp(link, src, dst, sport, port, data); next_send = now + resend
                    self.cond.wait(0.2)
                return box[0].decode('utf-8', 'replace')
        finally:
            with self.lock: self.udp_services.pop(sport, None)

    # ---- logging ----
    def log(self, kind, **fields):
        fields['t'] = time.monotonic(); fields['kind'] = kind
        with self.cond: self.events.append(fields); self.cond.notify_all()

    def find(self, kind, since=0, **match):
        return [e for e in self.events if e['kind'] == kind and e['t'] >= since and all(e.get(k) == v for k, v in match.items())]

    def wait(self, predicate, timeout, what):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                value = predicate()
                if value: return value
                left = deadline - time.monotonic()
                if left <= 0: raise AssertionError('timed out waiting for ' + what)
                self.cond.wait(min(left, 0.25))

    # ---- output ----
    def send_ip(self, link, src, dst, proto, payload, hlim=64, l2dst=None):
        """One packet; larger than the link MTU it is fragmented (IPv6 fragment header / IPv4 MF + offsets)."""
        if len(src) == 16:
            frame_dst = l2dst or (mc_mac6(dst) if dst[0] == 0xFF else link.guest_mac)
            if not frame_dst: return
            if 40 + len(payload) <= link.mtu:
                link.send(eth(frame_dst, link.mac, ETH_IP6, ip6(src, dst, proto, payload, hlim))); return
            ident, piece, at = random.getrandbits(32), (link.mtu - 48) & ~7, 0
            while at < len(payload):
                part = payload[at:at + piece]; more = at + len(part) < len(payload)
                header = struct.pack('!BBHI', proto, 0, at | (1 if more else 0), ident)
                link.send(eth(frame_dst, link.mac, ETH_IP6, ip6(src, dst, 44, header + part, hlim))); at += len(part)
        else:
            frame_dst = l2dst or (mc_mac4(dst) if dst[0] >> 4 == 14 else BCAST if dst == b'\xff' * 4 else link.neigh4.get(dst, link.guest_mac))
            if not frame_dst: return
            if 20 + len(payload) <= link.mtu:
                link.send(eth(frame_dst, link.mac, ETH_IP4, ip4(src, dst, proto, payload, ttl=hlim))); return
            ident, piece, at = random.getrandbits(16), (link.mtu - 20) & ~7, 0
            while at < len(payload):
                part = payload[at:at + piece]; more = at + len(part) < len(payload)
                header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(part), ident, (0x2000 if more else 0) | (at // 8), hlim, proto, 0, src, dst)
                header = header[:10] + struct.pack('!H', checksum(header)) + header[12:]
                link.send(eth(frame_dst, link.mac, ETH_IP4, header + part)); at += len(part)

    def send_udp(self, link, src, dst, sport, dport, data, hlim=64):
        self.send_ip(link, src, dst, 17, udp(src, dst, sport, dport, data), hlim)

    def unreachable(self, link, packet, family):
        if family == 6:
            router = next(iter(link.routers6), None)
            if not router: return
            body = b'\0\0\0\0' + packet[:1232 - 48]
            self.send_ip(link, router, packet[8:24], 58, icmp6(router, packet[8:24], 1, 3, body))
        else:
            router = next(iter(link.onlink4), None)
            if not router: return
            self.send_ip(link, router, packet[12:16], 1, icmp4(3, 1, b'\0\0\0\0' + packet[:28]))

    # ---- input ----
    def ticker(self):
        while not self.stopping:
            time.sleep(0.1); now = time.monotonic()
            try:
                with self.lock:
                    for c in list(self.tcp.values()): c.tick(now)
                    for link in self.links:
                        if link.ra and link.ra['interval'] and now >= link.next_ra:
                            link.next_ra = now + link.ra['interval']; link.send_ra()
            except Exception:
                self.errors.append(traceback.format_exc())

    def frame(self, link, frame):
        if len(frame) < 14: return
        dst, src, kind = frame[:6], frame[6:12], struct.unpack('!H', frame[12:14])[0]
        if link.guest_mac is None and not src[0] & 1: link.guest_mac = src
        if src == link.mac: return
        body = frame[14:]
        if kind == ETH_ARP: self.arp(link, src, body)
        elif kind == ETH_IP4: self.ipv4(link, src, dst, body)
        elif kind == ETH_IP6: self.ipv6(link, src, dst, body)
        else: self.log('l2', link=link.name, ethertype=kind, length=len(body))

    def arp(self, link, mac, b):
        if len(b) < 28 or b[0:4] != b'\0\1\x08\0': return
        op, sha, spa, tpa = struct.unpack('!H', b[6:8])[0], b[8:14], b[14:18], b[24:28]
        if spa != bytes(4): link.neigh4[spa] = sha
        self.log('arp', link=link.name, op=op, sender=t4(spa), target=t4(tpa))
        if op == 1 and tpa in link.onlink4:
            link.send(eth(sha, link.mac, ETH_ARP, struct.pack('!HHBBH', 1, 0x0800, 6, 4, 2) + link.mac + tpa + sha + spa))

    def ipv4(self, link, mac, l2dst, b):
        if len(b) < 20 or b[0] >> 4 != 4: return
        ihl = (b[0] & 15) * 4; total = struct.unpack('!H', b[2:4])[0]
        if total < ihl or total > len(b): return
        b = b[:total]; proto, src, dst = b[9], b[12:16], b[16:20]; payload = b[ihl:]
        if src != bytes(4) and src[0] >> 4 < 14: link.neigh4[src] = mac
        frag = struct.unpack('!H', b[6:8])[0]
        if frag & 0x3FFF:
            self.log('frag4', link=link.name, src=t4(src), dst=t4(dst), offset=(frag & 0x1FFF) * 8, more=bool(frag & 0x2000))
            whole = self.reassemble((4, src, dst, b[4:6], proto), (frag & 0x1FFF) * 8, bool(frag & 0x2000), payload)
            if whole is None: return
            payload = whole; b = b[:ihl] + whole
        if self.ip4_hook and self.ip4_hook(link, b): return
        if proto == 1 and len(payload) >= 4:
            self.log('icmp4', link=link.name, src=t4(src), dst=t4(dst), type=payload[0], code=payload[1], message=payload, ttl=b[8])
        multicast = dst[0] >> 4 == 14 or dst == b'\xff' * 4
        if proto == 17 and len(payload) >= 8:
            sport, dport, length = struct.unpack('!HHH', payload[:6]); data = payload[8:length]
            self.log('udp', link=link.name, family=4, src=t4(src), dst=t4(dst), sport=sport, dport=dport, length=len(data), ttl=b[8])
            if dport == 67 and link.dhcp4: self.dhcp4(link, data); return
            if multicast:
                for hook in self.multicast_hooks: hook(link, 4, src, dst, sport, dport, data, b[8])
                return
        if dst in self.blackholes: self.log('blackhole', family=4, dst=t4(dst), proto=proto); return
        if multicast:
            if proto == 2: self.log('igmp', link=link.name, src=t4(src), dst=t4(dst), message=payload, ttl=b[8], options=b[20:ihl])
            return
        if dst not in self.hosts and dst not in link.onlink4:
            self.log('unknown-dst', family=4, dst=t4(dst), proto=proto); self.unreachable(link, b, 4); return
        if proto == 1 and payload[:1] == b'\x08':
            self.send_ip(link, dst, src, 1, icmp4(0, 0, payload[4:]))
        elif proto == 17 and len(payload) >= 8: self.udp_in(link, src, dst, payload)
        elif proto == 6 and len(payload) >= 20: self.tcp_in(link, src, dst, payload)

    def ipv6(self, link, mac, l2dst, b):
        if len(b) < 40 or b[0] >> 4 != 6: return
        b = b[:40 + struct.unpack('!H', b[4:6])[0]]
        nh, hlim, src, dst, at = b[6], b[7], b[8:24], b[24:40], 40
        alert = False
        while nh in (0, 43, 60) and at + 8 <= len(b):
            if nh == 0 and b'\x05\x02\x00\x00' in b[at + 2:at + (b[at + 1] + 1) * 8]: alert = True   # Router Alert (MLD)
            nh, at = b[at], at + (b[at + 1] + 1) * 8
        payload = b[at:]
        if nh == 44 and len(payload) >= 8:
            field = struct.unpack('!H', payload[2:4])[0]
            self.log('frag6', link=link.name, src=t6(src), dst=t6(dst), offset=field & 0xFFF8, more=bool(field & 1), length=len(b))
            whole = self.reassemble((6, src, dst, payload[4:8]), field & 0xFFF8, bool(field & 1), payload[8:])
            if whole is None: return
            nh, payload = payload[0], whole
        if self.ip6_hook and self.ip6_hook(link, b, nh, payload): return
        if nh == 58 and len(payload) >= 4:
            kind = payload[0]
            self.log('icmp6', link=link.name, type=kind, code=payload[1], src=t6(src), dst=t6(dst), hlim=hlim, message=payload, alert=alert)
            if kind == 133 and link.ra: link.send_ra(); return
            if kind == 135 and len(payload) >= 24:
                target = payload[8:24]
                if target in link.onlink6 and src != bytes(16):
                    flags = 0x60000000 | (0x80000000 if target in link.routers6 else 0)
                    na = icmp6(target, src, 136, 0, struct.pack('!I', flags) + target + bytes([2, 1]) + link.mac)
                    link.send(eth(mac, link.mac, ETH_IP6, ip6(target, src, 58, na, hlim=255)))
                return
            if kind in (136, 143, 130, 131, 132, 134): return
        if dst[0] == 0xFF:
            if nh == 17 and len(payload) >= 8:
                sport, dport, length = struct.unpack('!HHH', payload[:6]); data = payload[8:length]
                self.log('udp', link=link.name, family=6, src=t6(src), dst=t6(dst), sport=sport, dport=dport, length=len(data), ttl=hlim)
                if dport == 547 and link.dhcp6: self.dhcp6(link, src, data); return
                for hook in self.multicast_hooks: hook(link, 6, src, dst, sport, dport, data, hlim)
            return
        if dst in self.blackholes: self.log('blackhole', family=6, dst=t6(dst), proto=nh); return
        if dst not in self.hosts and dst not in link.onlink6:
            self.log('unknown-dst', family=6, dst=t6(dst), proto=nh); self.unreachable(link, b, 6); return
        if nh == 58 and payload[0] == 128:
            self.send_ip(link, dst, src, 58, icmp6(dst, src, 129, 0, payload[4:]))
        elif nh == 17 and len(payload) >= 8:
            if struct.unpack('!H', payload[2:4])[0] == 547 and link.dhcp6: self.dhcp6(link, src, payload[8:]); return
            self.udp_in(link, src, dst, payload)
        elif nh == 6 and len(payload) >= 20: self.tcp_in(link, src, dst, payload)

    def reassemble(self, key, offset, more, data):
        """Fragment reassembly (no overlap handling: the guest is trusted here); the whole payload when complete."""
        parts = self.fragments.setdefault(key, {}); parts[offset] = (data, more)
        if all(m for _, m in parts.values()): return None
        whole, position = b'', 0
        for at in sorted(parts):
            piece, m = parts[at]
            if at != position: return None
            whole += piece; position += len(piece)
            if not m: del self.fragments[key]; return whole
        return None

    # ---- UDP / DNS / DHCP ----
    def udp_in(self, link, src, dst, payload):
        sport, dport, length = struct.unpack('!HHH', payload[:6]); data = payload[8:length]
        family = 6 if len(src) == 16 else 4
        self.log('udp', link=link.name, family=family, src=ta(src), dst=ta(dst), sport=sport, dport=dport, length=len(data))
        if dport == 53 and dst in self.dns_servers: reply = self.dns(family, src, dst, data)
        elif dport in self.udp_services: reply = self.udp_services[dport](link, src, dst, sport, data)
        else:
            if family == 6: self.send_ip(link, dst, src, 58, icmp6(dst, src, 1, 4, b'\0\0\0\0' + ip6(src, dst, 17, payload)[:1232 - 48]))
            else: self.send_ip(link, dst, src, 1, icmp4(3, 3, b'\0\0\0\0' + ip4(src, dst, 17, payload)[:28]))
            return
        if reply is not None: self.send_udp(link, dst, src, dport, sport, reply)

    def dns(self, family, src, dst, data):
        if len(data) < 17: return None
        try: name, at = dns_read_name(data, 12)
        except ValueError: return None
        if at + 4 > len(data): return None
        qtype, qclass = struct.unpack('!HH', data[at:at + 4]); name = name.lower().strip('.')
        question = data[12:at + 4]
        self.log('dns', family=family, server=ta(dst), client=ta(src), name=name, qtype=qtype)
        known = any(n == name for n, _ in self.records)
        answers = self.records.get((name, qtype), []) if qclass == 1 else []
        rr = b''.join(b'\xc0\x0c' + struct.pack('!HHIH', qtype, 1, 30, len(r)) + r for r in answers)
        flags = 0x8400 | (0x0100 & struct.unpack('!H', data[2:4])[0]) | 0x80 | (0 if known else 3)
        return data[:2] + struct.pack('!HHHHH', flags, 1, len(answers), 0, 0) + question + rr

    def dhcp4(self, link, data):
        if len(data) < 240 or data[0] != 1 or data[236:240] != b'\x63\x82\x53\x63': return
        xid, ciaddr, chaddr, options, at = data[4:8], data[12:16], data[28:34], {}, 240
        while at < len(data) and data[at] != 255:
            if data[at] == 0: at += 1; continue
            if at + 1 >= len(data): break
            options[data[at]] = data[at + 2:at + 2 + data[at + 1]]; at += 2 + data[at + 1]
        kind = options.get(53, b'\0')[0]; cfg = link.dhcp4
        self.log('dhcp4', link=link.name, type=kind, requested=t4(options[50]) if len(options.get(50, b'')) == 4 else None)
        if kind == 1: answer = 2
        elif kind == 3:
            wanted = options.get(50) or ciaddr
            answer = 5 if wanted == cfg['ip'] else 6
        else: return
        message = struct.pack('!BBBB4sHH4s4s4s4s16s64s128s', 2, 1, 6, 0, xid, 0, 0x8000, ciaddr, cfg['ip'] if answer != 6 else bytes(4),
                              cfg['router'], bytes(4), chaddr + bytes(10), b'', b'')
        message += b'\x63\x82\x53\x63' + bytes([53, 1, answer, 54, 4]) + cfg['router']
        if answer != 6:
            message += bytes([1, 4]) + cfg['mask'] + bytes([3, 4]) + cfg['router']
            if cfg['dns']: message += bytes([6, 4 * len(cfg['dns'])]) + b''.join(cfg['dns'])
            if cfg['domain']: message += bytes([15, len(cfg['domain'])]) + cfg['domain'].encode()
            message += bytes([51, 4]) + struct.pack('!I', cfg['lease'])
        message = (message + b'\xff').ljust(300, b'\0')
        if answer == 5: link.guest4 = cfg['ip']; link.neigh4[cfg['ip']] = chaddr
        packet = ip4(cfg['router'], b'\xff' * 4, 17, udp(cfg['router'], b'\xff' * 4, 67, 68, message))
        link.send(eth(BCAST, link.mac, ETH_IP4, packet))

    def dhcp6(self, link, src, data):
        if len(data) < 4: return
        kind, xid, options, at = data[0], data[1:4], {}, 4
        while at + 4 <= len(data):
            code, length = struct.unpack('!HH', data[at:at + 4]); options[code] = data[at + 4:at + 4 + length]; at += 4 + length
        self.log('dhcp6', link=link.name, type=kind, src=t6(src), options=sorted(options), oro=options.get(6, b''))
        if kind != 11: return
        cfg = link.dhcp6; router = next(iter(link.routers6), a6('fe80::1'))
        duid = struct.pack('!HH', 3, 1) + link.mac
        reply = bytes([7]) + xid + (struct.pack('!HH', 1, len(options[1])) + options[1] if 1 in options else b'')
        reply += struct.pack('!HH', 2, len(duid)) + duid
        if cfg['dns']: reply += struct.pack('!HH', 23, 16 * len(cfg['dns'])) + b''.join(cfg['dns'])
        if cfg['domains']:
            names = b''.join(dns_name(n) for n in cfg['domains']); reply += struct.pack('!HH', 24, len(names)) + names
        reply += struct.pack('!HHI', 32, 4, 3600)
        self.send_ip(link, router, src, 17, udp(router, src, 547, 546, reply))

    # ---- TCP ----
    def tcp_in(self, link, src, dst, segment):
        sport, dport, seq, ack, offset, flags, window = struct.unpack('!HHIIBBH', segment[:16])
        size = (offset >> 4) * 4
        if size < 20 or size > len(segment): return
        if checksum(pseudo(src, dst, 6, len(segment)) + segment):
            self.log('tcp-badsum', src=ta(src), dst=ta(dst)); return
        key = (link.name, dst, dport, src, sport)
        c = self.tcp.get(key)
        if flags & SYN and not flags & ACK:
            mss, at = None, 20
            while at < size:
                if segment[at] == 0: break
                if segment[at] == 1: at += 1; continue
                if at + 1 >= size: break
                if segment[at] == 2 and segment[at + 1] == 4: mss = struct.unpack('!H', segment[at + 2:at + 4])[0]
                at += max(segment[at + 1], 2)
            self.log('tcp-syn', link=link.name, family=6 if len(dst) == 16 else 4, src=ta(src), dst=ta(dst), sport=sport, dport=dport, mss=mss)
        if c is None:
            if flags & RST: return
            if flags & SYN and not flags & ACK and dport in self.listeners:
                c = Tcp(self, link, dst, dport, src, sport, None); c.app = self.listeners[dport](c)
                c.options(segment, size); c.rcv_nxt = (seq + 1) & M32; c.peer_window = window; c.state = 'SYN_RECEIVED'
                self.tcp[key] = c
                c.segment(c.iss, SYN | ACK, options=struct.pack('!BBH', 2, 4, link.mtu - (60 if c.family == 6 else 40))); c.arm()
                self.log('tcp-accept', link=link.name, family=c.family, local=ta(dst), lport=dport, remote=ta(src), rport=sport)
                return
            # no connection, no listener: RST (refused)
            data_len = len(segment) - size + (1 if flags & SYN else 0) + (1 if flags & FIN else 0)
            if flags & ACK: reply = tcp_segment(dst, src, dport, sport, ack, 0, RST)
            else: reply = tcp_segment(dst, src, dport, sport, 0, seq + data_len, RST | ACK)
            self.log('tcp-refused', family=6 if len(dst) == 16 else 4, dst=ta(dst), dport=dport)
            self.send_ip(link, dst, src, 6, reply)
            return
        c.input(seq, ack, flags, window, segment, size)
