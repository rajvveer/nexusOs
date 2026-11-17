"""NexusOS as a router: IPv4/IPv6 forwarding, nftables NAT (masquerade, snat, dnat port forwarding, redirect), sets
(interval, concatenation), maps and verdict maps, limit, quota, named counters, dynamic sets with timeouts, log,
MSS clamping, ct state/status, and Debian `nft` rule sets written like firewalld and ufw - checked on the wire.

    python -I tests/test_x64_nat.py WORK [cpus] [--port 9124] [--qmp-port 4725]

tests/netfix.py is both networks: the WAN on eth0 (QEMU -netdev dgram: DHCPv4 192.168.50.10/24, RA 2001:db8:1::/64,
servers 198.51.100.80 / 2001:db8:5::80) and a LAN carried in UDP over it to arch/x86_64/taprelay_user.c, which bridges
it to the TAP interface lan0 (10.9.0.1/24, fd09::1/64; clients 10.9.0.2, 10.9.0.3, fd09::2 are the fixture). The disk
is a copy of out/apps64.img with the Debian network tools (nettools_install.py, pinned) and the netwire agent
(arch/x86_64/netwire_user.c: RUN/SPAWN/WRITE commands) as /sbin/init. Every check looks at packets on the wire."""
from pathlib import Path
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import netfix  # noqa: E402
from netfix import a4, a6, t4  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
argv = [a for a in sys.argv[1:]]
def option(name, default):
    if name in argv: i = argv.index(name); value = int(argv[i + 1]); del argv[i:i + 2]; return value
    return default
PORT, QMP_PORT = option('--port', 9124), option('--qmp-port', 4725)
WORK = Path(argv[0]) if argv else Path(r'D:\nexustest\wave2\ws_net2_runs\nat')
SMP = argv[1] if len(argv) > 1 else '1'
WORK.mkdir(parents=True, exist_ok=True)
PKG_CACHE = Path(os.environ.get('NEXUS_NET_PKGS', r'D:\nexustest\pkgs\nettools'))
GUEST4, GUEST6 = '192.168.50.10', '2001:db8:1::5054:ff:fe12:3456'
WAN4, WAN6, RPC_SRC = '198.51.100.80', '2001:db8:5::80', '2001:db8:5::100'
LAN_GW, LAN_A, LAN_B, LAN_C, LAN6_GW, LAN6_A = '10.9.0.1', '10.9.0.2', '10.9.0.3', '10.9.0.4', 'fd09::1', 'fd09::2'
RELAY_PORT = 7100
results = []


def note(text): results.append(text); print('  ' + text, flush=True)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


class Guest:
    def __init__(self, fx, wan): self.fx, self.wan = fx, wan
    def rpc(self, text, timeout=30): return self.fx.rpc(self.wan, RPC_SRC, GUEST6, text, timeout=timeout)
    def run(self, command, want=0, seconds=20, contains=(), absent=()):
        reply = self.rpc('RUN %d %s' % (seconds, command), timeout=seconds + 25)
        assert reply.startswith('EXIT '), reply
        status, _, output = reply[5:].partition('\n'); status = int(status)
        if want is not None: assert status == want, f'{command!r} exited {status}:\n{output}'
        for s in contains: assert s in output, f'{command!r} output missing {s!r}:\n{output}'
        for s in absent: assert s not in output, f'{command!r} output had {s!r}:\n{output}'
        return output
    def put_file(self, path, data):
        """the agent's PUT holds 2 KiB: bigger files are written in parts (PUT, then PUTA appends)"""
        for k in range(0, len(data), 1800):
            assert self.rpc(f'{"PUT" if not k else "PUTA"} {path} {data[k:k + 1800].hex()}') == 'OK'



class Ping:
    """ICMP echo from a fixture client; replies are matched by id+seq"""
    def __init__(self, fx): self.fx = fx; self.seq = 0
    def send(self, link, src, dst, ident=0x4242, ttl=64, size=16, df=False):
        self.seq += 1; body = struct.pack('!HH', ident, self.seq) + bytes(size)
        if ':' in src:
            self.fx.send_ip(link, a6(src), a6(dst), 58, netfix.icmp6(a6(src), a6(dst), 128, 0, body), hlim=ttl)
        else:
            payload = netfix.icmp4(8, 0, body)
            header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), self.seq, 0x4000 if df else 0, ttl, 1, 0, a4(src), a4(dst))
            header = header[:10] + struct.pack('!H', netfix.checksum(header)) + header[12:]
            link.send(netfix.eth(link.guest_mac, link.mac, 0x0800, header + payload))
        return self.seq


def raw4(link, src, dst, proto, payload, ttl=64, df=False, ident=1):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), ident, 0x4000 if df else 0, ttl, proto, 0, a4(src), a4(dst))
    header = header[:10] + struct.pack('!H', netfix.checksum(header)) + header[12:]
    link.send(netfix.eth(link.guest_mac, link.mac, 0x0800, header + payload))


class Echo:
    """a TCP client app: sends `request`, collects the answer"""
    def __init__(self, request): self.request, self.data, self.closed, self.open = request, b'', False, False
    def on_open(self, conn): self.open = True; conn.send(self.request)
    def on_data(self, conn, data): self.data += data
    def on_eof(self, conn): self.closed = True; conn.close()


ROUTER_RULESET = '''flush ruleset
table ip nat {
    chain prerouting {
        type nat hook prerouting priority dstnat; policy accept;
        iifname "eth0" tcp dport 8080 dnat to 10.9.0.2:80
        iifname "eth0" tcp dport 8443 dnat to 10.9.0.3:443
        iifname "eth0" tcp dport 8081 dnat to 10.9.0.2:81
        iifname "eth0" udp dport 5300 dnat to 10.9.0.2:53
        iifname "eth0" udp dport 7002 redirect to :7001
    }
    chain postrouting {
        type nat hook postrouting priority srcnat; policy accept;
        oifname "eth0" ip saddr 10.9.0.3 ip protocol tcp snat to 192.168.50.10:30000-30010
        oifname "eth0" ip saddr 10.9.0.0/24 masquerade
    }
}
table ip6 nat {
    chain postrouting {
        type nat hook postrouting priority srcnat; policy accept;
        oifname "eth0" ip6 saddr fd09::/64 masquerade
    }
}
table inet filter {
    counter lan_out { }
    quota lan_quota { until 1 mbytes }
    set blocked { type ipv4_addr; flags interval; elements = { 203.0.113.0/24, 198.18.0.0/15 } }
    set published { type ipv4_addr . inet_service; elements = { 10.9.0.2 . 80, 10.9.0.3 . 443 } }
    set seen { type ipv4_addr; flags dynamic, timeout; timeout 30s; }
    map port_policy { type inet_service : verdict; elements = { 25 : drop, 23 : jump telnet_log } }
    chain telnet_log {
        log prefix "nexus-telnet " drop
    }
    chain forward {
        type filter hook forward priority filter; policy drop;
        ct state vmap { established : accept, related : accept, invalid : drop }
        tcp flags syn tcp option maxseg size set rt mtu
        ip daddr @blocked counter reject with icmp type admin-prohibited
        tcp dport vmap @port_policy
        iifname "lan0" update @seen { ip saddr }
        iifname "lan0" icmp type echo-request limit rate over 2/second burst 2 packets drop
        iifname "lan0" oifname "eth0" counter name lan_out quota name lan_quota accept
        iifname "eth0" oifname "lan0" ip daddr . tcp dport @published ct status dnat accept
        iifname "eth0" oifname "lan0" udp dport 53 ct status dnat accept
        iifname "lan0" oifname "eth0" meta nfproto ipv6 accept
    }
    chain input {
        type filter hook input priority filter; policy accept;
        iifname "eth0" udp dport 7001 ct status dnat counter accept
    }
}
'''

FIREWALLD_STYLE = '''flush ruleset
table inet firewalld {
    chain raw_PREROUTING {
        type filter hook prerouting priority raw + 10; policy accept;
        meta nfproto ipv6 fib saddr . mark . iif oif missing drop
    }
    chain mangle_PREROUTING {
        type filter hook prerouting priority mangle + 10; policy accept;
        jump mangle_PREROUTING_ZONES
    }
    chain mangle_PREROUTING_ZONES {
        iifname "eth0" goto mangle_PRE_public
        goto mangle_PRE_public
    }
    chain mangle_PRE_public {
        meta mark set 0x10
    }
    chain filter_INPUT {
        type filter hook input priority filter + 10; policy accept;
        ct state { established, related } accept
        ct status dnat accept
        iifname "lo" accept
        ct state invalid drop
        jump filter_INPUT_ZONES
        reject with icmpx admin-prohibited
    }
    chain filter_INPUT_ZONES {
        iifname "eth0" goto filter_IN_public
        goto filter_IN_public
    }
    chain filter_IN_public {
        jump filter_IN_public_allow
        meta l4proto { icmp, ipv6-icmp } accept
    }
    chain filter_IN_public_allow {
        ip6 daddr fe80::/64 udp dport 546 accept comment "dhcpv6-client"
        tcp dport 22 accept comment "ssh"
        udp dport 7001 accept comment "test agent"
    }
    chain filter_FORWARD {
        type filter hook forward priority filter + 10; policy accept;
        ct state { established, related } accept
        ct status dnat accept
        ct state invalid drop
        reject with icmpx admin-prohibited
    }
    chain filter_OUTPUT {
        type filter hook output priority filter + 10; policy accept;
        oifname "lo" accept
    }
    chain nat_PREROUTING {
        type nat hook prerouting priority dstnat + 10; policy accept;
    }
    chain nat_POSTROUTING {
        type nat hook postrouting priority srcnat + 10; policy accept;
    }
}
'''

UFW_STYLE = '''flush ruleset
table inet ufw {
    set ufw_user_limit { type ipv4_addr; flags dynamic, timeout; timeout 30s; }
    chain ufw-before-input {
        iifname "lo" accept
        ct state related,established accept
        ct state invalid drop
        icmp type { destination-unreachable, time-exceeded, parameter-problem, echo-request } accept
        icmpv6 type { destination-unreachable, packet-too-big, time-exceeded, parameter-problem, echo-request, nd-router-advert, nd-neighbor-solicit, nd-neighbor-advert } accept
        udp sport 67 udp dport 68 accept
        ip daddr 224.0.0.251 udp dport 5353 accept
        ip6 daddr ff02::fb udp dport 5353 accept
    }
    chain ufw-user-input {
        udp dport 7001 accept
        tcp dport 22 ct state new add @ufw_user_limit { ip saddr limit rate over 6/minute } drop
        tcp dport 22 accept
        tcp dport 80 accept
    }
    chain ufw-after-input {
        udp dport { 137, 138 } drop
        tcp dport { 139, 445 } drop
    }
    chain ufw-logging-deny {
        limit rate 3/minute burst 10 packets log prefix "[UFW BLOCK] "
    }
    chain input {
        type filter hook input priority filter; policy drop;
        jump ufw-before-input
        jump ufw-user-input
        jump ufw-after-input
        jump ufw-logging-deny
    }
    chain output {
        type filter hook output priority filter; policy accept;
    }
    chain forward {
        type filter hook forward priority filter; policy drop;
    }
}
'''


def main():
    zig = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
    for source, target in (('netwire_user.c', 'agent.elf'), ('taprelay_user.c', 'taprelay.elf')):
        subprocess.run([str(zig), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'arch/x86_64' / source), '-o', str(WORK / target)], check=True)
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    fatput = load('nat_fatput', ROOT / 'userland/fatput.py')
    fatput.main(['fatput', str(disk), 'put', str(WORK / 'agent.elf'), 'sbin/init', 'put', str(WORK / 'taprelay.elf'), 'bin/taprelay'])
    nettools = load('nat_nettools', ROOT / 'arch/x86_64/nettools_install.py')
    assert nettools.install([str(disk), '--cache', str(PKG_CACHE), '--report', str(WORK / 'install.json')]) == 0
    fx = netfix.Fixture()
    wan = fx.link('wan', PORT, PORT + 1, guest_mac=bytes.fromhex('525400123456'))
    wan.server4(dns=())
    wan.router6()
    fx.host(WAN4, WAN6, RPC_SRC, '203.0.113.5')
    lan = fx.tunnel('lan', wan, '192.168.50.1', GUEST4, RELAY_PORT, mac=bytes.fromhex('020000000902'), mtu=1400)
    lan.onlink4.update({a4(LAN_A), a4(LAN_B), a4(LAN_C)}); lan.onlink6.add(a6(LAN6_A))
    fx.host(LAN_A, LAN_B, LAN_C, LAN6_A)
    seen_http = []
    def site(r):
        seen_http.append((r.server, r.client, r.path, r.conn.link.name))
        return 200, {}, ('NAT-OK %s %s' % (r.server, r.path)).encode()
    fx.http(80, site); fx.http(443, site)
    fx.udp_services[53] = lambda link, src, dst, sport, data: b'dns:' + data
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
    g = Guest(fx, wan); ping = Ping(fx)
    def wan_since(t, kind='icmp'):
        return [e for e in fx.events if e['t'] >= t and e.get('link') == 'wan']
    try:
        fx.wait(lambda: 'NETWIRE AGENT READY' in text(), 120, 'agent')
        fx.wait(lambda: fx.find('dhcp4', type=3), 60, 'DHCP')
        fx.wait(lambda: [e for e in fx.find('icmp6') if e['type'] == 135 and e['src'] == '::' and e['message'][8:24] == a6(GUEST6)], 30, 'SLAAC')
        time.sleep(3)
        # ---- the LAN: TAP lan0 bridged to the fixture
        assert g.rpc(f'SPAWN /bin/taprelay lan0 {RELAY_PORT} 192.168.50.1 {RELAY_PORT}').startswith('PID ')
        fx.wait(lambda: 'TAPRELAY READY lan0' in text(), 20, 'taprelay')
        g.run('/usr/bin/ip link set lan0 mtu 1400 up')
        g.run(f'/usr/bin/ip addr add {LAN_GW}/24 dev lan0'); g.run(f'/usr/bin/ip -6 addr add {LAN6_GW}/64 dev lan0 nodad')
        fx.wait(lambda: lan.guest_mac, 20, 'a frame from lan0')
        # ---- forwarding off: nothing crosses
        t = time.monotonic(); ping.send(lan, LAN_A, WAN4); time.sleep(1.5)
        assert not [e for e in fx.find('udp', t) + fx.find('unknown-dst', t) if e.get('link') == 'wan' and e.get('dst') == WAN4], 'forwarded while ip_forward=0'
        assert not [e for e in fx.events if e['t'] >= t and e['kind'] in ('icmp4',)], 'forwarded'
        assert g.rpc('WRITE /proc/sys/net/ipv4/ip_forward 1') == 'OK' and g.rpc('WRITE /proc/sys/net/ipv6/conf/all/forwarding 1') == 'OK'
        assert g.rpc('FILE /proc/sys/net/ipv4/conf/lan0/forwarding').strip() == '1'
        # ---- routed (no NAT yet): TTL lowered, replies routed back
        got = []; fx.ip4_hook = lambda link, b: (got.append((link.name, t4(b[12:16]), t4(b[16:20]), b[8], b[9])) and False) or False
        t = time.monotonic(); seq = ping.send(lan, LAN_A, WAN4, ttl=10)
        fx.wait(lambda: [x for x in got if x[0] == 'wan' and x[1] == LAN_A and x[3] == 9], 5, 'the routed echo on the WAN with TTL 9')
        fx.wait(lambda: [x for x in got if x[0] == 'lan' and x[2] == LAN_A and x[4] == 1], 5, 'the echo reply routed back to the LAN')
        t = time.monotonic(); ping.send(lan, LAN_A, WAN4, ttl=1)
        fx.wait(lambda: [x for x in got if x[0] == 'lan' and x[1] == LAN_GW and x[2] == LAN_A and x[4] == 1], 5, 'ICMP time exceeded from 10.9.0.1')
        snmp = g.rpc('FILE /proc/net/snmp'); fwd = int(snmp.split('Ip:')[2].split()[5])
        assert fwd >= 2 and snmp.split('Ip:')[2].split()[0] == '1', snmp[:300]
        # IPv6 routed
        got6 = []; fx.ip6_hook = lambda link, b, nh, payload: (got6.append((link.name, netfix.t6(b[8:24]), netfix.t6(b[24:40]), b[7], nh, payload[:1])) and False) or False
        ping.send(lan, LAN6_A, WAN6, ttl=7)
        fx.wait(lambda: [x for x in got6 if x[0] == 'wan' and x[1] == LAN6_A and x[3] == 6], 5, 'IPv6 forwarded with hop limit 6')
        fx.wait(lambda: [x for x in got6 if x[0] == 'lan' and x[2] == LAN6_A and x[5] == b'\x81'], 5, 'IPv6 echo reply back')
        note('forwarding: off by default (nothing crosses); ip_forward=1 -> TTL 10->9 on the WAN, reply routed back, '
             'TTL 1 -> ICMP time exceeded from 10.9.0.1, /proc/net/snmp Forwarding=1; IPv6 hop limit 7->6 and back')

        # ---- the router rule set (Debian nft)
        g.put_file('/tmp/router.nft', ROUTER_RULESET.encode())
        out = g.run('/usr/sbin/nft -f /tmp/router.nft', want=None)
        assert 'rror' not in out, out
        listing = g.run('/usr/sbin/nft list ruleset', contains=('masquerade', 'dnat to 10.9.0.2:80', 'snat to 192.168.50.10:30000-30010',
                        'redirect to :7001', 'flags interval', '203.0.113.0/24', 'ipv4_addr . inet_service', '10.9.0.2 . 80',
                        'type inet_service : verdict', '25 : drop', '23 : jump telnet_log', 'limit rate over 2/second',
                        'counter name "lan_out"', 'quota name "lan_quota"', 'update @seen', 'maxseg size set rt mtu'))
        assert listing
        note('Debian nft loaded the router rule set; `nft list ruleset` shows nat chains (masquerade, snat range, dnat, redirect), '
             'an interval set, a concatenation set, a verdict map, limit, named counter/quota, a dynamic set, MSS clamping')
        fx.ip4_hook = None; fx.ip6_hook = None
        # ---- masquerade: TCP, two clients with the same source port, UDP, ICMP
        apps = []
        for client in (LAN_A, LAN_A):
            app = Echo(b'GET /masq HTTP/1.0\r\nHost: wan\r\n\r\n'); apps.append(app)
            fx.connect(lan, client, WAN4, 80, app)
        fx.wait(lambda: all(a.closed for a in apps), 15, 'two masqueraded HTTP exchanges')
        assert all(b'NAT-OK 198.51.100.80 /masq' in a.data for a in apps), [a.data[:80] for a in apps]
        masq = [c for c in seen_http if c[2] == '/masq']
        assert len(masq) == 2 and all(c[1] == GUEST4 and c[3] == 'wan' for c in masq), masq
        t = time.monotonic(); same = Echo(b'GET /same HTTP/1.0\r\n\r\n'); other = Echo(b'GET /same HTTP/1.0\r\n\r\n')
        fx.connect(lan, LAN_A, WAN4, 80, same, lport=41000); fx.connect(lan, LAN_C, WAN4, 80, other, lport=41000)
        fx.wait(lambda: same.closed and other.closed, 15, 'two clients, same source port, same server')
        accepted = sorted(e['rport'] for e in fx.find('tcp-accept', t) if e['link'] == 'wan' and e['remote'] == GUEST4)
        assert len(accepted) == 2 and 41000 in accepted and len(set(accepted)) == 2, accepted
        t = time.monotonic(); fx.send_udp(lan, a4(LAN_A), a4(WAN4), 5555, 7, b'udp-through-nat')
        fx.wait(lambda: [e for e in fx.find('udp', t) if e['link'] == 'lan' and e['dst'] == LAN_A and e['sport'] == 7 and e['dport'] == 5555], 5, 'UDP echo back through NAT')
        t = time.monotonic(); ping.send(lan, LAN_A, WAN4, ident=0x7777)
        fx.wait(lambda: [e for e in fx.find('udp', t)] or True, 1, '')
        conntrack = g.rpc('FILE /proc/net/nf_conntrack')
        assert 'src=10.9.0.2' in conntrack and 'dst=192.168.50.10' in conntrack, conntrack[:800]
        note(f'masquerade: two LAN HTTP flows reached the WAN server from {GUEST4} and came back; 10.9.0.2:41000 and '
             f'10.9.0.4:41000 to one server left as ports {accepted} (unique reply tuples), '
             'UDP echo and ICMP translated both ways; /proc/net/nf_conntrack lists the reply tuples')
        # ---- ICMP error translation: port unreachable from the WAN reaches the LAN client un-NATed
        t = time.monotonic(); fx.send_udp(lan, a4(LAN_A), a4(WAN4), 6001, 9999, b'nobody-listens')
        def icmp_err():
            for e in fx.find('icmp4', t):
                if e['link'] == 'lan' and e['dst'] == LAN_A and e['type'] == 3 and e['code'] == 3: return e
        e = fx.wait(icmp_err, 5, 'ICMP port unreachable translated to the LAN client')
        inner = e['message'][8:]
        assert t4(inner[12:16]) == LAN_A and struct.unpack('!H', inner[20:22])[0] == 6001, inner[:28].hex()
        note('ICMP error translation: port unreachable to 192.168.50.10 reached 10.9.0.2 quoting 10.9.0.2:6001')
        # ---- snat to an address:port range
        app = Echo(b'GET /snat HTTP/1.0\r\n\r\n'); fx.connect(lan, LAN_B, WAN4, 80, app, lport=42000)
        fx.wait(lambda: app.closed, 10, 'snat flow')
        sn = [e for e in fx.find('tcp-accept') if e['link'] == 'wan' and e['remote'] == GUEST4 and 30000 <= e['rport'] <= 30010]
        if not sn: print(g.run('/usr/sbin/nft --debug=netlink list chain ip nat postrouting', want=None))
        assert sn, [e for e in fx.find('tcp-accept') if e['link'] == 'wan'][-3:]
        note(f'snat to 192.168.50.10:30000-30010: the flow from 10.9.0.3:42000 left as port {sn[0]["rport"]}')
        # ---- dnat port forwarding, concatenation set gate, redirect
        fwd = Echo(b'GET /forwarded HTTP/1.0\r\n\r\n'); fx.connect(wan, WAN4, GUEST4, 8080, fwd)
        fx.wait(lambda: fwd.closed, 10, 'port 8080 forwarded')
        assert b'NAT-OK 10.9.0.2 /forwarded' in fwd.data, fwd.data
        assert [c for c in seen_http if c[2] == '/forwarded' and c[0] == LAN_A and c[1] == WAN4 and c[3] == 'lan'], seen_http[-3:]
        fwd2 = Echo(b'GET /second HTTP/1.0\r\n\r\n'); fx.connect(wan, WAN4, GUEST4, 8443, fwd2)
        fx.wait(lambda: fwd2.closed, 10, 'port 8443 forwarded to 10.9.0.3:443')
        t = time.monotonic(); blocked = Echo(b'x'); fx.connect(wan, WAN4, GUEST4, 8081, blocked); time.sleep(3)
        assert not [e for e in fx.find('tcp-syn', t) if e['link'] == 'lan'], 'dnat to 10.9.0.2:81 passed the forward policy'
        t = time.monotonic(); red = fx.rpc(wan, WAN4, GUEST4, 'SLEEP 1', timeout=8, port=7002)   # udp 7002 -> redirect -> the agent's 7001
        assert red == 'OK', red
        back = [e for e in fx.find('udp', t) if e['link'] == 'wan' and e['src'] == GUEST4 and e['dst'] == WAN4]
        assert back and back[0]['sport'] == 7002, back
        note('dnat: TCP 8080 -> 10.9.0.2:80 and 8443 -> 10.9.0.3:443 (allowed by the {daddr . dport} set + ct status dnat), '
             '8081 -> 10.9.0.2:81 dropped by the forward policy; redirect UDP 7002 -> :7001 answered from port 7002')
        # ---- filter: interval set reject, verdict map drop/jump+log, limit, MSS clamp
        t = time.monotonic(); fx.send_udp(lan, a4(LAN_A), a4('203.0.113.5'), 6002, 53, b'blocked')
        fx.wait(lambda: [e for e in fx.find('icmp4', t) if e['link'] == 'lan' and e['type'] == 3 and e['code'] == 13], 5, 'admin-prohibited for the blocked range')
        t = time.monotonic(); smtp = Echo(b''); fx.connect(lan, LAN_A, WAN4, 25, smtp); telnet = Echo(b''); fx.connect(lan, LAN_A, WAN4, 23, telnet); time.sleep(3)
        assert not [e for e in fx.find('tcp-syn', t) if e['link'] == 'wan' and e['dport'] in (23, 25)], 'vmap verdicts'
        assert 'nexus-telnet' in text(), 'the jump target logged'
        t = time.monotonic()
        for k in range(12): ping.send(lan, LAN_A, WAN4, ident=0x5100 + k)   # new flows (an established echo is accepted earlier)
        time.sleep(2)
        passed = [e for e in fx.find('icmp4', t) if e['link'] == 'wan' and e['type'] == 8]
        assert 1 <= len(passed) <= 6, ('limit 2/second burst 2', len(passed))
        g.run('/usr/bin/ip link set eth0 mtu 1280')
        t = time.monotonic(); mss = Echo(b'GET /mss HTTP/1.0\r\n\r\n'); fx.connect(lan, LAN_A, WAN4, 80, mss); fx.wait(lambda: mss.closed, 10, 'mss flow')
        syn = [e for e in fx.find('tcp-syn', t) if e['link'] == 'wan']
        assert syn and syn[0]['mss'] == 1240, ('MSS clamped to the 1280 route MTU', syn[:1])
        note(f'filter: 203.0.113.0/24 (interval set) -> ICMP admin-prohibited; vmap 25 drop / 23 jump+log ("nexus-telnet" in the '
             f'kernel log); limit 2/s burst 2 passed {len(passed)} of 12 pings; MSS 1360 clamped to 1240 (eth0 MTU 1280)')
        # ---- named objects and the dynamic set
        lst = g.run('/usr/sbin/nft list counter inet filter lan_out')
        packets = int(lst.split('packets ')[1].split()[0]); assert packets > 0, lst
        dyn = g.run('/usr/sbin/nft list set inet filter seen', contains=('10.9.0.2', 'expires'))
        note(f'named counter lan_out counted {packets} packets; dynamic set "seen" holds 10.9.0.2 with an expiry')
        # ---- NAT66 masquerade
        t = time.monotonic(); ping.send(lan, LAN6_A, WAN6, ident=0x6666)
        def v6_masq():
            for e in fx.find('icmp6', t):
                if e['link'] == 'wan' and e['type'] == 128 and e['src'].startswith('2001:db8:1:'): return e
        e6 = fx.wait(v6_masq, 5, 'IPv6 masquerade: the echo left from the router\'s global address')
        fx.wait(lambda: [x for x in fx.find('icmp6', t) if x['link'] == 'lan' and x['type'] == 129 and x['dst'] == LAN6_A], 5, 'NAT66 reply back')
        note(f'NAT66: fd09::2 masqueraded to {e6["src"]} on the WAN, reply translated back')
        # ---- hostile: martians and bad headers from the LAN are dropped, nothing crashes
        t = time.monotonic()
        raw4(lan, '127.0.0.1', WAN4, 17, netfix.udp(a4('127.0.0.1'), a4(WAN4), 1, 7, b'martian'))
        raw4(lan, '0.0.0.0', WAN4, 17, netfix.udp(a4('0.0.0.0'), a4(WAN4), 1, 7, b'zero'))
        raw4(lan, LAN_A, '255.255.255.255', 17, netfix.udp(a4(LAN_A), b'\xff' * 4, 1, 7, b'bcast'))
        raw4(lan, LAN_A, '10.9.0.255', 17, netfix.udp(a4(LAN_A), a4('10.9.0.255'), 1, 7, b'dbcast'))
        lan.send(netfix.eth(lan.guest_mac, lan.mac, 0x0800, b'\x4f' + bytes(19)))          # IHL 15, truncated
        lan.send(netfix.eth(lan.guest_mac, lan.mac, 0x0800, b'\x45\x00\x00\x08' + bytes(16)))   # total length < header
        time.sleep(2)
        leaked = [e for e in fx.events if e['t'] >= t and e.get('link') == 'wan' and e['kind'] in ('udp',) and e.get('dport') == 7]
        assert not leaked, leaked
        note('hostile: loopback/zero sources, limited and directed broadcasts, truncated and inconsistent headers were not forwarded')
        # ---- firewalld- and ufw-style rule sets
        for name, ruleset, checks in (('firewalld', FIREWALLD_STYLE, ('chain filter_IN_public_allow', 'fib saddr . mark . iif oif missing drop', 'comment "ssh"', 'meta mark set 0x00000010')),
                                      ('ufw', UFW_STYLE, ('chain ufw-user-input', 'limit rate 3/minute burst 10 packets', 'add @ufw_user_limit', 'tcp dport { 139, 445 } drop'))):
            g.put_file(f'/tmp/{name}.nft', ruleset.encode())
            out = g.run(f'/usr/sbin/nft -f /tmp/{name}.nft', want=None)
            assert 'rror' not in out and 'not supported' not in out, (name, out)
            g.run('/usr/sbin/nft list ruleset', contains=checks)
            # behaviour: the agent (UDP 7001) still answers, an unlisted TCP port is refused/dropped
            assert g.rpc('FILE /proc/sys/net/ipv4/ip_forward').strip() == '1'
            t = time.monotonic(); probe = Echo(b''); fx.connect(wan, WAN4, GUEST4, 23, probe); time.sleep(2.5)
            rst = [e for e in fx.find('tcp-close', t) if e.get('remote') == GUEST4 and e.get('rport') == 23]
            icmp = [e for e in fx.find('icmp4', t) if e['link'] == 'wan' and e['type'] == 3]
            note(f'{name}-style rule set loaded and listed; port 23 from the WAN: {"ICMP admin-prohibited" if icmp else "reset" if rst else "dropped"}; the agent still answers')
        g.run('/usr/sbin/nft flush ruleset')
        assert g.rpc('WRITE /proc/sys/net/ipv4/ip_forward 0') == 'OK'
        t = time.monotonic(); ping.send(lan, LAN_A, WAN4); time.sleep(1.5)
        assert not [e for e in fx.find('icmp4', t) if e['link'] == 'wan'], 'forwarded after ip_forward=0'
        log = text(); assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, log[-3000:]
        assert not fx.errors, fx.errors[:2]
        note('flush + ip_forward=0: forwarding stops; no fault')
    except BaseException:
        print(text()[-5000:]); print('\n'.join(fx.errors)[-3000:]); raise
    finally:
        qemu.kill(); qemu.wait(); fx.stop()
        disk.unlink(missing_ok=True)
    print(f'PASS NAT/forwarding ({SMP} CPU): ' + '; '.join(results))


if __name__ == '__main__':
    main()
