"""Multicast on the native kernel, checked on the wire: socket API (RFC 3678), IGMPv3 with IGMPv1/v2 compatibility,
MLDv2 with MLDv1 compatibility, delivery filters, multicast loopback, hostile options and queries.

    python -I tests/test_x64_multicast.py WORK [cpus] [--port 9124] [--qmp-port 4723]

tests/netfix.py is the whole network (QEMU -netdev dgram): router, DHCPv4, multicast sources and querier.
arch/x86_64/mcast_user.c runs as /sbin/init on a copy of out/apps64.img and executes socket commands sent over UDP;
the host builds every option structure (ip_mreqn, ip_mreq_source, ip_msfilter, group_req, group_source_req,
group_filter) itself, so the kernel parses exactly what programs pass, hostile variants included.
IPv4: any-source join (TO_EX{} report: TTL 1, Router Alert, to 224.0.0.22, robustness 2), delivery with packet info,
IP_MULTICAST_ALL, IP_BLOCK/UNBLOCK_SOURCE (BLOCK/ALLOW records), source-specific join (ALLOW), IP_MSFILTER get/set
(TO_EX), leave (TO_IN{}), MCAST_* group_req forms, general / group / group-and-source queries (IS_EX, IS_IN answers),
IP_MULTICAST_TTL/IF/LOOP (wire TTL, local copy on and off), /proc/net/igmp and /proc/net/mcfilter, then an IGMPv2
querier (v2 reports to the group, Leave to 224.0.0.2, report suppression) and an IGMPv1 querier (v1 reports).
IPv6: IPV6_JOIN_GROUP (MLDv2 TO_EX, hop limit 1 + Router Alert), MCAST_BLOCK_SOURCE, MCAST_JOIN_SOURCE_GROUP,
MCAST_MSFILTER, IPV6_MULTICAST_ALL, IPV6_MULTICAST_HOPS/LOOP, general and group-and-source queries, MLDv1 fallback
(reports, Done to ff02::2). Hostile: short/garbage structures, wrong families, unicast groups, multicast sources,
limits (20 memberships, 10/64 sources), msfilter size lies, truncated and over-long IGMP/MLD queries; the kernel
stays up and memberships are released when sockets close."""
from pathlib import Path
import importlib.util
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import netfix  # noqa: E402
from netfix import a4, a6, t4, t6  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
args = sys.argv[1:]
def option(name, default):   # removes the option and its value from args
    if name not in args: return default
    i = args.index(name); value = int(args[i + 1]); del args[i:i + 2]; return value
PORT, QMP_PORT = option('--port', 9124), option('--qmp-port', 4723)
WORK = Path(args[0]) if args else Path(r'D:\nexustest\wave2\ws_net2_runs\multicast')
SMP = args[1] if len(args) > 1 and args[1].isdigit() else '1'
WORK.mkdir(parents=True, exist_ok=True)
GUEST4, GUEST6 = '192.168.50.10', '2001:db8:1::5054:ff:fe12:3456'
RPC_SRC = '2001:db8:5::100'
SOL_IP, IPV6 = 0, 41
EINVAL, EADDRINUSE, EADDRNOTAVAIL, ENOBUFS, EAFNOSUPPORT, ENOPROTOOPT = 22, 98, 99, 105, 97, 92
results = []


def note(text): results.append(text); print('  ' + text, flush=True)
def settle(): time.sleep(2.3)   # the robustness retransmissions of a state change are over (2 x <= 1 s)


# ---- option structures (Linux x86-64 layouts) ----
def mreqn(group, address='0.0.0.0', ifindex=0): return a4(group) + a4(address) + struct.pack('<i', ifindex)
def mreq_source(group, interface, source): return a4(group) + a4(interface) + a4(source)
def msfilter(group, interface, mode, sources, numsrc=None):
    return a4(group) + a4(interface) + struct.pack('<II', mode, len(sources) if numsrc is None else numsrc) + b''.join(a4(s) for s in sources)
def storage(address, family=None):
    if family == 1: return struct.pack('<H', 1) + bytes(126)
    if ':' in address: return (struct.pack('<H', 10) + bytes(6) + a6(address) + bytes(4)).ljust(128, b'\0')
    return (struct.pack('<H', 2) + bytes(2) + a4(address)).ljust(128, b'\0')
def group_req(ifindex, group): return struct.pack('<I4x', ifindex) + storage(group)
def group_source_req(ifindex, group, source): return struct.pack('<I4x', ifindex) + storage(group) + storage(source)
def group_filter(ifindex, group, mode, sources, numsrc=None):
    return struct.pack('<I4x', ifindex) + storage(group) + struct.pack('<II', mode, len(sources) if numsrc is None else numsrc) + b''.join(storage(s) for s in sources)


class Guest:
    def __init__(self, fx, lan): self.fx, self.lan = fx, lan
    def rpc(self, text, timeout=20): return self.fx.rpc(self.lan, RPC_SRC, GUEST6, text, timeout=timeout)
    def sock(self, family, port):
        r = self.rpc(f'SOCK {family} {port}'); assert r.startswith('OK '), r; return int(r.split()[1])
    def setopt(self, sid, level, name, value): return int(self.rpc(f'SETOPT {sid} {level} {name} {value.hex() or "-"}').split()[1])
    def getopt(self, sid, level, name, size, value=b''):
        parts = self.rpc(f'GETOPT {sid} {level} {name} {size}:{value.hex()}').split()
        return int(parts[1]), bytes.fromhex(parts[3]) if len(parts) > 3 else b''
    def send(self, sid, address, port, text): return self.rpc(f'SEND {sid} {address} {port} {text}').split()[1:]
    def recv(self, sid, ms=700):
        lines = self.rpc(f'RECV {sid} {ms}', timeout=20 + ms / 1000).splitlines()[1:]
        return [dict(zip(('src', 'port', 'if', 'dst', 'data'), l.split(' ', 4))) for l in lines if l.strip()]
    def data(self, sid, ms=700): return sorted(r['data'] for r in self.recv(sid, ms))


# ---- wire: IGMP / MLD records ----
def igmp_records(message):
    """[(record type, group, [sources])] of an IGMPv3 report"""
    count, at, out = struct.unpack('!H', message[6:8])[0], 8, []
    for _ in range(count):
        kind, aux, n = message[at], message[at + 1], struct.unpack('!H', message[at + 2:at + 4])[0]
        group = t4(message[at + 4:at + 8]); sources = [t4(message[at + 8 + 4 * i:at + 12 + 4 * i]) for i in range(n)]
        out.append((kind, group, sources)); at += 8 + 4 * n + 4 * aux
    return out
def mld_records(message):
    count, at, out = struct.unpack('!H', message[6:8])[0], 8, []
    for _ in range(count):
        kind, aux, n = message[at], message[at + 1], struct.unpack('!H', message[at + 2:at + 4])[0]
        group = t6(message[at + 4:at + 20]); sources = [t6(message[at + 20 + 16 * i:at + 36 + 16 * i]) for i in range(n)]
        out.append((kind, group, sources)); at += 20 + 16 * n + 4 * aux
    return out
def v3_reports(fx, since):
    return [(e, igmp_records(e['message'])) for e in fx.find('igmp', since) if e['message'][:1] == b'\x22']
def wait_record(fx, since, want, timeout=6, what=''):
    def match():
        for e, records in v3_reports(fx, since):
            if want in records: return e
    return fx.wait(match, timeout, what or f'IGMPv3 record {want}')
def mld_reports(fx, since):
    return [(e, mld_records(e['message'])) for e in fx.find('icmp6', since) if e['type'] == 143]
def wait_mld(fx, since, want, timeout=6, what=''):
    def match():
        for e, records in mld_reports(fx, since):
            if want in records: return e
    return fx.wait(match, timeout, what or f'MLDv2 record {want}')


def igmp_packet(dst, message, src='192.168.50.1'):
    """IPv4 with the Router Alert option, TTL 1"""
    header = struct.pack('!BBHHHBBH4s4s', 0x46, 0xC0, 24 + len(message), 0, 0, 1, 2, 0, a4(src), a4(dst)) + b'\x94\x04\x00\x00'
    header = header[:10] + struct.pack('!H', netfix.checksum(header)) + header[12:]
    return header + message
def igmp_message(kind, code, group, extra=b''):
    m = struct.pack('!BBH4s', kind, code, 0, a4(group)) + extra
    return m[:2] + struct.pack('!H', netfix.checksum(m)) + m[4:]
def send_igmp(fx, lan, dst, message, src='192.168.50.1'):
    lan.send(netfix.eth(netfix.mc_mac4(a4(dst)), lan.mac, 0x0800, igmp_packet(dst, message, src)))
def v3_query(group='0.0.0.0', sources=(), code=10, qrv=2, qqic=125):
    return igmp_message(0x11, code, group, struct.pack('!BBH', qrv, qqic, len(sources)) + b''.join(a4(s) for s in sources))
def send_mld(fx, lan, dst, kind, body, src='fe80::1', hlim=1):
    hbh = bytes([58, 0, 5, 2, 0, 0, 1, 0])
    m = netfix.icmp6(a6(src), a6(dst), kind, 0, body)
    lan.send(netfix.eth(netfix.mc_mac6(a6(dst)), lan.mac, 0x86DD, netfix.ip6(a6(src), a6(dst), 0, hbh + m, hlim=hlim)))
def mld2_query(group='::', sources=(), max_ms=1000, qrv=2, qqic=125):
    return struct.pack('!HH', max_ms, 0) + a6(group) + struct.pack('!BBH', qrv, qqic, len(sources)) + b''.join(a6(s) for s in sources)


def main():
    zig = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
    subprocess.run([str(zig), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'arch/x86_64/mcast_user.c'), '-o', str(WORK / 'mcast-user.elf')], check=True)
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    spec = importlib.util.spec_from_file_location('mc_fatput', ROOT / 'userland/fatput.py')
    fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
    fatput.main(['fatput', str(disk), 'put', str(WORK / 'mcast-user.elf'), 'sbin/init'])
    fx = netfix.Fixture(); lan = fx.link('lan', PORT, PORT + 1, guest_mac=bytes.fromhex('525400123456'))
    lan.router6(); lan.server4(dns=())
    fx.host(RPC_SRC, '2001:db8:5::7', '2001:db8:5::8', '2001:db8:5::9', '192.0.2.7', '192.0.2.8', '192.0.2.9')
    fx.start()
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    qemu = subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-accel', 'whpx,kernel-irqchip=off',
        '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt', '-m', '512', '-smp', SMP,
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', f'dgram,id=n0,local.type=inet,local.host=127.0.0.1,local.port={PORT + 1},remote.type=inet,remote.host=127.0.0.1,remote.port={PORT}',
        '-device', 'e1000e,netdev=n0,mac=52:54:00:12:34:56', '-qmp', f'tcp:127.0.0.1:{QMP_PORT},server=on,wait=off',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def text(): return serial.read_text(errors='replace') if serial.exists() else ''
    g = Guest(fx, lan)
    try:
        fx.wait(lambda: 'MCAST AGENT READY' in text(), 120, 'agent')
        fx.wait(lambda: fx.find('dhcp4', type=3), 60, 'DHCP')
        fx.wait(lambda: [e for e in fx.find('icmp6') if e['type'] == 135 and e['src'] == '::' and e['message'][8:24] == a6(GUEST6)], 30, 'SLAAC DAD')
        time.sleep(3)
        eth0 = int(g.rpc('IFINDEX eth0'))

        # ---- IPv4: any-source join, delivery, IP_MULTICAST_ALL ----
        A, B, C, D = g.sock(4, 5000), g.sock(4, 5000), g.sock(4, 5001), g.sock(4, 0)
        t = time.monotonic()
        assert g.setopt(A, SOL_IP, 35, mreqn('239.1.1.1', ifindex=eth0)) == 0
        e = wait_record(fx, t, (4, '239.1.1.1', []), what='TO_EX{} for 239.1.1.1')
        assert e['ttl'] == 1 and e['options'][:2] == b'\x94\x04' and e['dst'] == '224.0.0.22', e
        fx.wait(lambda: len([1 for _, r in v3_reports(fx, t) if (4, '239.1.1.1', []) in r]) >= 2, 4, 'the robustness retransmission')
        assert g.setopt(A, SOL_IP, 35, mreqn('239.1.1.1', ifindex=eth0)) == EADDRINUSE
        fx.send_udp(lan, a4('192.0.2.7'), a4('239.1.1.1'), 4000, 5000, b'm1')
        got = g.recv(A); assert [r['data'] for r in got] == ['m1'] and got[0]['if'] == f'if={eth0}' and got[0]['dst'] == 'dst=239.1.1.1' and got[0]['src'] == '192.0.2.7', got
        assert g.data(B, 300) == ['m1'] and g.data(C, 300) == []
        assert g.setopt(B, SOL_IP, 49, struct.pack('<i', 0)) == 0
        fx.send_udp(lan, a4('192.0.2.7'), a4('239.1.1.1'), 4000, 5000, b'm2')
        assert g.data(A) == ['m2'] and g.data(B, 300) == []
        note('IPv4 join: IGMPv3 TO_EX{} (TTL 1, Router Alert, 224.0.0.22, 2x); delivery with IP_PKTINFO; duplicate join EADDRINUSE; '
             'a non-member socket gets copies only with IP_MULTICAST_ALL')

        # ---- source filters ----
        t = time.monotonic()
        assert g.setopt(A, SOL_IP, 38, mreq_source('239.1.1.1', GUEST4, '192.0.2.7')) == 0
        wait_record(fx, t, (6, '239.1.1.1', ['192.0.2.7']), what='BLOCK{192.0.2.7}')
        fx.send_udp(lan, a4('192.0.2.7'), a4('239.1.1.1'), 4000, 5000, b'm3'); fx.send_udp(lan, a4('192.0.2.8'), a4('239.1.1.1'), 4000, 5000, b'm4')
        assert g.data(A) == ['m4'], 'blocked source delivered'
        settle(); mcf = g.rpc('FILE /proc/net/mcfilter')
        assert '0xef010101 0xc0000207' in mcf and mcf.strip().splitlines()[-1].split()[-2:] == ['0', '1'], mcf
        t = time.monotonic()
        assert g.setopt(A, SOL_IP, 37, mreq_source('239.1.1.1', GUEST4, '192.0.2.7')) == 0
        wait_record(fx, t, (5, '239.1.1.1', ['192.0.2.7']), what='ALLOW{192.0.2.7}')
        assert g.setopt(A, SOL_IP, 37, mreq_source('239.1.1.1', GUEST4, '192.0.2.7')) == EADDRNOTAVAIL
        settle(); t = time.monotonic()
        assert g.setopt(C, SOL_IP, 39, mreq_source('232.1.1.1', GUEST4, '192.0.2.9')) == 0
        wait_record(fx, t, (5, '232.1.1.1', ['192.0.2.9']), what='SSM ALLOW{192.0.2.9}')
        fx.send_udp(lan, a4('192.0.2.9'), a4('232.1.1.1'), 4000, 5001, b's9'); fx.send_udp(lan, a4('192.0.2.8'), a4('232.1.1.1'), 4000, 5001, b's8')
        assert g.data(C) == ['s9'], 'source-specific membership'
        assert g.setopt(C, SOL_IP, 38, mreq_source('232.1.1.1', GUEST4, '192.0.2.8')) == EINVAL, 'block on an INCLUDE filter'
        settle(); r, value = g.getopt(C, SOL_IP, 41, 16 + 4 * 4, msfilter('232.1.1.1', GUEST4, 0, [], numsrc=4))
        assert r == 0 and struct.unpack('<II', value[8:16]) == (1, 1) and value[16:20] == a4('192.0.2.9'), (r, value.hex())
        t = time.monotonic()
        assert g.setopt(C, SOL_IP, 41, msfilter('232.1.1.1', GUEST4, 0, ['192.0.2.8'])) == 0
        wait_record(fx, t, (4, '232.1.1.1', ['192.0.2.8']), what='TO_EX{192.0.2.8}')
        fx.send_udp(lan, a4('192.0.2.9'), a4('232.1.1.1'), 4000, 5001, b'x9'); fx.send_udp(lan, a4('192.0.2.8'), a4('232.1.1.1'), 4000, 5001, b'x8')
        assert g.data(C) == ['x9']
        settle(); t = time.monotonic()
        assert g.setopt(C, SOL_IP, 36, mreqn('232.1.1.1', GUEST4)) == 0
        wait_record(fx, t, (3, '232.1.1.1', []), what='TO_IN{} (leave)')
        assert g.setopt(C, SOL_IP, 36, mreqn('232.1.1.1', GUEST4)) == EADDRNOTAVAIL
        settle(); t = time.monotonic()
        assert g.setopt(C, SOL_IP, 46, group_source_req(eth0, '232.2.2.2', '192.0.2.9')) == 0
        wait_record(fx, t, (5, '232.2.2.2', ['192.0.2.9']), what='MCAST_JOIN_SOURCE_GROUP ALLOW')
        settle(); t = time.monotonic()
        assert g.setopt(C, SOL_IP, 47, group_source_req(eth0, '232.2.2.2', '192.0.2.9')) == 0
        wait_record(fx, t, (6, '232.2.2.2', ['192.0.2.9']), what='last source left: BLOCK')
        t = time.monotonic()
        assert g.setopt(C, SOL_IP, 46, group_source_req(eth0, '232.3.3.3', '192.0.2.9')) == 0
        wait_record(fx, t, (5, '232.3.3.3', ['192.0.2.9']), what='ALLOW for 232.3.3.3')
        assert g.setopt(C, SOL_IP, 47, group_source_req(eth0, '232.3.3.3', '192.0.2.9')) == 0   # inside the join's burst
        wait_record(fx, t, (3, '232.3.3.3', []), what='a change inside a burst: the full state TO_IN{}')
        note('source filters: IP_BLOCK/UNBLOCK_SOURCE (BLOCK/ALLOW records, /proc/net/mcfilter), IP_ADD_SOURCE_MEMBERSHIP (ALLOW, '
             'only that source delivered), IP_MSFILTER get/set (TO_EX), leave (TO_IN{}), MCAST_JOIN/LEAVE_SOURCE_GROUP')

        # ---- queries ----
        t = time.monotonic(); send_igmp(fx, lan, '224.0.0.1', v3_query())
        e = wait_record(fx, t, (2, '239.1.1.1', []), timeout=4, what='IS_EX{} answer to a general query')
        assert all(r[1] != '224.0.0.1' for r in igmp_records(e['message'])), 'all-hosts reported'
        assert g.setopt(A, SOL_IP, 38, mreq_source('239.1.1.1', GUEST4, '192.0.2.7')) == 0
        time.sleep(2.5)
        t = time.monotonic(); send_igmp(fx, lan, '239.1.1.1', v3_query('239.1.1.1'))
        wait_record(fx, t, (2, '239.1.1.1', ['192.0.2.7']), timeout=4, what='group-specific answer IS_EX{.7}')
        t = time.monotonic(); send_igmp(fx, lan, '239.1.1.1', v3_query('239.1.1.1', ['192.0.2.7', '192.0.2.8']))
        wait_record(fx, t, (1, '239.1.1.1', ['192.0.2.8']), timeout=4, what='group-and-source answer IS_IN{.8}')
        good = v3_query(); t = time.monotonic(); send_igmp(fx, lan, '224.0.0.1', good[:2] + bytes([good[2] ^ 0x5A]) + good[3:])   # bad checksum
        bad = v3_query(sources=['192.0.2.1'] * 3); send_igmp(fx, lan, '224.0.0.1', bad[:-4])   # source count lies
        trunc = igmp_message(0x11, 10, '0.0.0.0', b'\x02')                                     # 9 bytes
        send_igmp(fx, lan, '224.0.0.1', trunc); time.sleep(2)
        assert not v3_reports(fx, t), 'a malformed query was answered'
        note('queries: general (IS_EX, no 224.0.0.1), group-specific (IS_EX{.7}), group-and-source (IS_IN{.8}); bad checksum, '
             'lying source count and 9-byte queries ignored')

        # ---- multicast TTL / IF / LOOP ----
        assert g.setopt(D, SOL_IP, 33, b'\x05') == 0 and g.setopt(D, SOL_IP, 32, a4(GUEST4)) == 0
        t = time.monotonic(); assert g.send(D, '239.1.1.1', 5000, 'loop1') == ['0', '5']
        e = fx.wait(lambda: [x for x in fx.find('udp', t) if x['dst'] == '239.1.1.1' and x['dport'] == 5000], 4, 'multicast on the wire')[0]
        assert e['ttl'] == 5, e
        got = g.recv(A); assert [r['data'] for r in got] == ['loop1'] and got[0]['if'] == f'if={eth0}', got
        assert g.setopt(D, SOL_IP, 34, b'\x00') == 0
        t = time.monotonic(); g.send(D, '239.1.1.1', 5000, 'loop2')
        fx.wait(lambda: [x for x in fx.find('udp', t) if x['dst'] == '239.1.1.1' and x['length'] == 5], 4, 'loop2 on the wire')
        assert g.data(A, 400) == [], 'IP_MULTICAST_LOOP=0 still looped'
        note('IP_MULTICAST_TTL 5 on the wire, IP_MULTICAST_IF by address, IP_MULTICAST_LOOP copy delivered locally (on eth0) and not when off')

        # ---- hostile options ----
        bad = [(g.setopt(A, SOL_IP, 35, a4('239.9.9.9')), EINVAL, 'short ip_mreq'),
               (g.setopt(A, SOL_IP, 35, mreqn('10.1.1.1')), EINVAL, 'unicast group'),
               (g.setopt(A, SOL_IP, 39, mreq_source('239.9.9.9', GUEST4, '239.1.1.2')), EINVAL, 'multicast source'),
               (g.setopt(A, SOL_IP, 39, mreq_source('239.9.9.9', GUEST4, '0.0.0.0')), EINVAL, 'zero source'),
               (g.setopt(A, SOL_IP, 38, mreq_source('239.8.8.8', GUEST4, '192.0.2.7')), EINVAL, 'block without a join'),
               (g.setopt(A, SOL_IP, 41, msfilter('239.1.1.1', GUEST4, 0, [], numsrc=1000)), ENOBUFS, 'msfilter numsrc 1000'),
               (g.setopt(A, SOL_IP, 41, msfilter('239.1.1.1', GUEST4, 0, ['192.0.2.1'], numsrc=5)), EINVAL, 'msfilter longer than optlen'),
               (g.setopt(A, SOL_IP, 41, msfilter('239.1.1.1', GUEST4, 7, [])), EINVAL, 'msfilter bad mode'),
               (g.setopt(A, SOL_IP, 41, b'\1\2\3'), EINVAL, 'msfilter 3 bytes'),
               (g.setopt(A, SOL_IP, 48, group_filter(eth0, '239.1.1.1', 0, [])[:100]), EINVAL, 'group_filter short'),
               (g.setopt(A, SOL_IP, 42, struct.pack('<I4x', eth0) + storage('', 1)), EAFNOSUPPORT, 'group_req AF_UNIX'),
               (g.setopt(A, SOL_IP, 42, group_req(eth0, 'ff15::5')), EAFNOSUPPORT, 'IPv6 group on an IPv4 socket'),
               (g.setopt(A, SOL_IP, 42, group_req(999, '239.9.9.9')), 19, 'unknown interface'),
               (g.setopt(A, SOL_IP, 46, group_source_req(eth0, '239.9.9.9', '192.0.2.1')[:200]), EINVAL, 'group_source_req short')]
        for got, want, what in bad: assert got == want, (what, got, want)
        assert g.setopt(A, SOL_IP, 38, mreq_source('239.1.1.1', GUEST4, '192.0.2.7')) == 0   # already blocked: no error, no change
        for i in range(10): assert g.setopt(C, SOL_IP, 39, mreq_source('232.5.5.5', GUEST4, f'192.0.2.{20 + i}')) == 0
        assert g.setopt(C, SOL_IP, 39, mreq_source('232.5.5.5', GUEST4, '192.0.2.40')) == ENOBUFS, 'igmp_max_msf'
        many = g.sock(4, 5100); codes = [g.setopt(many, SOL_IP, 35, mreqn(f'239.20.0.{i}', ifindex=eth0)) for i in range(21)]
        assert codes == [0] * 20 + [ENOBUFS], codes
        r, value = g.getopt(A, SOL_IP, 41, 8, msfilter('239.1.1.1', GUEST4, 0, []))
        assert r == EINVAL, ('msfilter get with a short buffer', r)
        note(f'hostile options: {len(bad)} malformed/invalid structures refused with Linux errnos; 10-source and 20-membership limits (ENOBUFS)')
        assert g.rpc(f'CLOSE {many}') == 'OK'
        t = time.monotonic()
        fx.wait(lambda: len({r[1] for _, rs in v3_reports(fx, t) for r in rs if r[0] == 3 and r[1].startswith('239.20.0.')}) >= 20, 8, 'leaves of the closed socket')

        # ---- IPv6 / MLD ----
        E, F, G = g.sock(6, 6000), g.sock(6, 6001), g.sock(6, 0)
        t = time.monotonic()
        assert g.setopt(E, IPV6, 20, a6('ff15::1:2') + struct.pack('<i', eth0)) == 0
        e = wait_mld(fx, t, (4, 'ff15::1:2', []), what='MLDv2 TO_EX{}')
        assert e['hlim'] == 1 and e['alert'] and e['dst'] == 'ff02::16', e
        fx.send_udp(lan, a6('2001:db8:5::7'), a6('ff15::1:2'), 4000, 6000, b'v1')
        got = g.recv(E); assert [r['data'] for r in got] == ['v1'] and got[0]['dst'] == 'dst=ff15::1:2', got
        settle(); t = time.monotonic()
        assert g.setopt(E, IPV6, 43, group_source_req(eth0, 'ff15::1:2', '2001:db8:5::7')) == 0
        wait_mld(fx, t, (6, 'ff15::1:2', ['2001:db8:5::7']), what='MLDv2 BLOCK')
        fx.send_udp(lan, a6('2001:db8:5::7'), a6('ff15::1:2'), 4000, 6000, b'b7'); fx.send_udp(lan, a6('2001:db8:5::8'), a6('ff15::1:2'), 4000, 6000, b'b8')
        assert g.data(E) == ['b8']
        t = time.monotonic()
        assert g.setopt(F, IPV6, 46, group_source_req(eth0, 'ff35::3', '2001:db8:5::9')) == 0
        wait_mld(fx, t, (5, 'ff35::3', ['2001:db8:5::9']), what='MLDv2 SSM ALLOW')
        fx.send_udp(lan, a6('2001:db8:5::9'), a6('ff35::3'), 4000, 6001, b'q9'); fx.send_udp(lan, a6('2001:db8:5::8'), a6('ff35::3'), 4000, 6001, b'q8')
        assert g.data(F) == ['q9']
        r, value = g.getopt(F, IPV6, 48, 144 + 128 * 2, group_filter(eth0, 'ff35::3', 0, [], numsrc=2))
        assert r == 0 and struct.unpack('<II', value[136:144]) == (1, 1) and value[144 + 8:144 + 24] == a6('2001:db8:5::9'), (r, value[:160].hex())
        settle(); sixty_five = [f'2001:db8:6::{i:x}' for i in range(1, 66)]
        assert g.setopt(F, IPV6, 48, group_filter(eth0, 'ff35::3', 1, sixty_five)) == ENOBUFS, 'mld_max_msf'
        t = time.monotonic()
        assert g.setopt(F, IPV6, 48, group_filter(eth0, 'ff35::3', 1, sixty_five[:64])) == 0, '64 sources (fragmented command)'
        fx.wait(lambda: [1 for _, rs in mld_reports(fx, t) for r in rs if r[1] == 'ff35::3' and r[0] == 5 and len(r[2]) >= 30], 6, 'ALLOW with the new sources')
        assert g.setopt(E, IPV6, 29, struct.pack('<i', 0)) == 0 and g.setopt(G, IPV6, 18, struct.pack('<i', 3)) == 0
        t = time.monotonic(); g.send(G, 'ff15::1:2', 6000, 'six')
        e = fx.wait(lambda: [x for x in fx.find('udp', t) if x['dst'] == 'ff15::1:2'], 4, 'IPv6 multicast on the wire')[0]
        assert e['ttl'] == 3, e
        got = g.recv(E); assert [r['data'] for r in got] == ['six'], ('IPv6 multicast loop', got)
        t = time.monotonic(); send_mld(fx, lan, 'ff02::1', 130, mld2_query())
        wait_mld(fx, t, (2, 'ff15::1:2', ['2001:db8:5::7']), timeout=4, what='MLDv2 general query answer')
        t = time.monotonic(); send_mld(fx, lan, 'ff15::1:2', 130, mld2_query('ff15::1:2', ['2001:db8:5::7', '2001:db8:5::8']))
        wait_mld(fx, t, (1, 'ff15::1:2', ['2001:db8:5::8']), timeout=4, what='MLDv2 group-and-source answer')
        t = time.monotonic(); send_mld(fx, lan, 'ff02::1', 130, mld2_query(sources=['::1'] * 4)[:-20])   # source count lies
        send_mld(fx, lan, 'ff02::1', 130, mld2_query(), src='2001:db8:1::1')                          # not link-local
        time.sleep(2); assert not mld_reports(fx, t), 'a bad MLD query was answered'
        note('IPv6: IPV6_JOIN_GROUP (MLDv2 TO_EX, hop limit 1 + Router Alert), MCAST_BLOCK_SOURCE, MCAST_JOIN_SOURCE_GROUP, '
             'MCAST_MSFILTER get and 64/65 sources, IPV6_MULTICAST_ALL, IPV6_MULTICAST_HOPS + loop, general and G&S answers, bad queries ignored')

        # ---- IGMPv2 / IGMPv1 / MLDv1 compatibility ----
        t = time.monotonic(); send_igmp(fx, lan, '224.0.0.1', igmp_message(0x11, 10, '0.0.0.0'))
        fx.wait(lambda: [e for e in fx.find('igmp', t) if e['message'][0] == 0x16 and e['dst'] == '239.1.1.1'], 4, 'IGMPv2 report')
        assert ' V2' in g.rpc('FILE /proc/net/igmp')
        t = time.monotonic(); assert g.setopt(B, SOL_IP, 35, mreqn('239.3.3.3', ifindex=eth0)) == 0
        fx.wait(lambda: [e for e in fx.find('igmp', t) if e['message'][0] == 0x16 and e['dst'] == '239.3.3.3'], 4, 'v2 join report')
        t = time.monotonic(); assert g.setopt(B, SOL_IP, 36, mreqn('239.3.3.3', ifindex=eth0)) == 0
        fx.wait(lambda: [e for e in fx.find('igmp', t) if e['message'][0] == 0x17 and e['dst'] == '224.0.0.2' and e['message'][4:8] == a4('239.3.3.3')], 4, 'v2 leave')
        t = time.monotonic()
        send_igmp(fx, lan, '239.1.1.1', igmp_message(0x11, 50, '239.1.1.1'))
        lan.send(netfix.eth(netfix.mc_mac4(a4('239.1.1.1')), bytes.fromhex('020000000099'), 0x0800,
                            igmp_packet('239.1.1.1', igmp_message(0x16, 0, '239.1.1.1'), src='192.168.50.99')))
        time.sleep(6)
        # (only 239.1.1.1: other groups may still be answering the general query of a moment ago)
        assert not [e for e in fx.find('igmp', t) if e['src'] == GUEST4 and e['message'][0] == 0x16 and e['dst'] == '239.1.1.1'], 'report not suppressed'
        t = time.monotonic(); send_igmp(fx, lan, '224.0.0.1', igmp_message(0x11, 0, '0.0.0.0'))
        fx.wait(lambda: [e for e in fx.find('igmp', t) if e['message'][0] == 0x12 and e['dst'] == '239.1.1.1'], 12, 'IGMPv1 report')
        assert ' V1' in g.rpc('FILE /proc/net/igmp')
        t = time.monotonic(); send_mld(fx, lan, 'ff02::1', 130, struct.pack('!HH', 1000, 0) + bytes(16))
        fx.wait(lambda: [e for e in fx.find('icmp6', t) if e['type'] == 131 and e['dst'] == 'ff15::1:2'], 4, 'MLDv1 report')
        t = time.monotonic(); assert g.rpc(f'CLOSE {E}') == 'OK'
        fx.wait(lambda: [e for e in fx.find('icmp6', t) if e['type'] == 132 and e['dst'] == 'ff02::2'], 4, 'MLDv1 Done')
        note('compatibility: IGMPv2 querier -> v2 reports to the group, v2 Leave to 224.0.0.2, report suppressed by another '
             'member; IGMPv1 querier -> v1 reports; MLDv1 querier -> v1 report, Done to ff02::2; /proc/net/igmp shows V2/V1')

        # ---- cleanup: every membership released ----
        for sid in (A, B, C, D, F, G): assert g.rpc(f'CLOSE {sid}') == 'OK'
        time.sleep(3)
        igmp = g.rpc('FILE /proc/net/igmp'); mcf = g.rpc('FILE /proc/net/mcfilter') + g.rpc('FILE /proc/net/mcfilter6')
        groups = [l.split()[0] for l in igmp.splitlines() if l.startswith('\t\t')]
        assert groups and all(x == '010000E0' for x in groups), igmp
        assert len(mcf.strip().splitlines()) == 1, mcf
        log = text(); assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, log[-2000:]
        assert not fx.errors, fx.errors
        note('closing every socket released all groups (only 224.0.0.1 left, no source filters); no fault')
        g.rpc('EXIT')
    except BaseException:
        print(text()[-4000:]); print('\n'.join(fx.errors)[-3000:]); raise
    finally:
        qemu.kill(); qemu.wait(); fx.stop()
        disk.unlink(missing_ok=True)
    print(f'PASS multicast ({SMP} CPU): ' + '; '.join(results))


if __name__ == '__main__':
    main()
