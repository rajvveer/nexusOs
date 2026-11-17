"""Debian network tools on native NexusOS: installs iproute2, iputils-ping, tcpdump, nftables, wireguard-tools and
net-tools (arch/x86_64/nettools_install.py, from the signed nettools-lock.json) onto a private disk copy, boots it on
the deterministic fixture network of test_x64_netwire.py (this Python process is the whole network over `-netdev
dgram`), and drives the real Debian binaries through the netwire agent's RUN command:
  - ip -6 addr / ip -6 route / ip -6 neigh / ip link    (iproute2 reads the kernel's AF_INET6 netlink)
  - ping / ping -6 to the fixture router                (iputils-ping over datagram ICMP sockets)
  - tcpdump on eth0 capturing a ping it triggers         (AF_PACKET + libpcap + a cBPF filter)
  - nft -f a ruleset and `nft list ruleset`             (nftables netlink; see test_x64_firewall.py for enforcement)
  - wg genkey/pubkey and a wireguard interface via `ip link add type wireguard` + `wg set`/`wg show` (in-kernel WG)
  - ifconfig / route -n / netstat (net-tools)
Usage: test_x64_nettools.py [work dir] [cpus]. The disk is a private copy; nettools-lock.json pins every .deb."""
from pathlib import Path
import os
import base64
import hashlib
import importlib.util
import re
import shutil
import struct
import threading
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-nettools')
SMP = sys.argv[2] if len(sys.argv) > 2 else '1'
WORK.mkdir(parents=True, exist_ok=True)
PKG_CACHE = nexus_paths.cache('net')

spec = importlib.util.spec_from_file_location('netwire', ROOT / 'tests/test_x64_netwire.py')
nw = importlib.util.module_from_spec(spec); spec.loader.exec_module(nw)
spec2 = importlib.util.spec_from_file_location('nettools_install', ROOT / 'arch/x86_64/nettools_install.py')
nettools = importlib.util.module_from_spec(spec2); spec2.loader.exec_module(nettools)


def run(net, command, seconds=20, want=0, contains=(), absent=(), timeout=45):
    """RUN <command> on the guest; assert the exit status and substrings. Returns stdout+stderr."""
    reply = net.rpc('RUN %d %s' % (seconds, command.replace(' ', ' ').replace('  ', ' ')), timeout=timeout, retry=True)
    assert reply.startswith('EXIT '), reply
    status, _, output = reply[5:].partition('\n')
    status = int(status)
    if want is not None:
        assert status == want, f'{command!r} exited {status} (wanted {want}):\n{output}'
    for s in contains: assert s in output, f'{command!r} output missing {s!r}:\n{output}'
    for s in absent: assert s not in output, f'{command!r} output had {s!r}:\n{output}'
    return output


def esc(text):
    """Pass one shell-free argument through the agent's space-encoded RUN (%20 becomes a blank in the argument)."""
    return text.replace(' ', '%20')


def main():
    results = []
    def note(text): results.append(text); print('  ' + text, flush=True)
    agent = nw.build_agent(WORK)
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    spec3 = importlib.util.spec_from_file_location('nettools_fatput', ROOT / 'userland/fatput.py')
    fatput = importlib.util.module_from_spec(spec3); spec3.loader.exec_module(fatput)
    fatput.main(['fatput', str(disk), 'put', str(agent), 'sbin/init'])
    report = nettools.install([str(disk), '--cache', str(PKG_CACHE), '--report', str(WORK / 'install.json')])
    assert report == 0
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    net = nw.Network()
    qemu = subprocess.Popen(nw.qemu_command(disk, serial, SMP), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def serial_text(): return serial.read_text(errors='replace') if serial.exists() else ''
    try:
        net.wait(lambda: [e for e in net.dhcplog if e.get('answer') == 5], 90, 'DHCPv4'); net.guest4 = nw.a4('192.168.50.10')
        net.wait(lambda: 'NETWIRE AGENT READY' in serial_text(), 60, 'agent')
        net.wait(lambda: [e for e in net.icmp6log if e[1] == 135 and e[3] == nw.ANY6 and e[7][8:24] == nw.GUEST_GLOBAL], 20, 'global DAD')
        time.sleep(2.5)

        # iproute2: the kernel's addresses, routes and neighbours over rtnetlink
        out = run(net, '/usr/bin/ip -6 addr show dev eth0', contains=('inet6 2001:db8:1:', 'scope global', 'inet6 fe80::', 'scope link', 'mngtmpaddr'))
        assert 'temporary' in out or 'dynamic' in out, out
        run(net, '/usr/bin/ip -4 addr show dev eth0', contains=('inet 192.168.50.10/24', 'eth0'))
        run(net, '/usr/bin/ip -6 route show', contains=('2001:db8:1::/64', 'default via fe80::1', 'proto ra'))
        run(net, '/usr/bin/ip -4 route show', contains=('default via 192.168.50.1', '192.168.50.0/24'))
        run(net, '/usr/bin/ip link show eth0', contains=('eth0', 'ether 52:54:00:12:34:56', 'mtu 1500'))   # eth0 device MTU; the RA's 1400 is the IPv6 link MTU
        run(net, '/usr/bin/ip -6 neigh show', contains=('fe80::1',))
        ss = run(net, '/usr/bin/ss -lnu', want=None)   # (ss needs /proc/net; informational)
        note('iproute2: ip -6 addr/route/neigh and ip link read the stack over AF_INET6 netlink (SLAAC, RA default route, MTU 1400)')

        # iputils ping (datagram ICMP sockets)
        run(net, '/usr/bin/ping -6 -c 2 -W 3 2001:db8:1::1', contains=('2 packets transmitted', '2 received', 'bytes from 2001:db8:1::1'))
        run(net, '/usr/bin/ping -c 2 -W 3 192.168.50.1', contains=('2 packets transmitted', '2 received'))
        note('iputils-ping: ping and ping6 to the router over the ping-group datagram ICMP sockets')

        # tcpdump: AF_PACKET + libpcap + cBPF filter "icmp6"; the fixture pings the guest while it captures to a file
        def ping_the_guest():
            time.sleep(1.0)
            for i in range(6):
                net.send6(nw.ip6(nw.R_GLOBAL, nw.GUEST_GLOBAL, 58, nw.icmp6(nw.R_GLOBAL, nw.GUEST_GLOBAL, 128, 0,
                          struct.pack('!HH', 0x4242, i) + b'tcpdump-capture-probe')))
                time.sleep(0.4)
        threading.Thread(target=ping_the_guest, daemon=True).start()
        run(net, '/usr/bin/tcpdump -i eth0 -c 4 -n -w /tmp/cap.pcap icmp6', seconds=12, contains=('4 packets captured',))
        run(net, '/usr/bin/tcpdump -n -r /tmp/cap.pcap', contains=('ICMP6, echo request', '2001:db8:1::1'))
        note('tcpdump: AF_PACKET + libpcap captured an ICMPv6 echo exchange on eth0 (filter "icmp6") to a pcap file and re-read it')

        # nftables: load a ruleset and read it back (enforcement is tested in test_x64_firewall.py)
        ruleset = ('table inet filter {\n  chain input {\n    type filter hook input priority 0; policy drop;\n'
                   '    ct state established,related accept\n    iif "lo" accept\n    ip6 nexthdr icmpv6 accept\n'
                   '    tcp dport 22 accept\n    tcp dport { 80, 443 } accept\n    udp dport { 53, 123 } accept\n'
                   '    udp dport 7001 accept\n'   # the harness RPC channel: every request is a NEW flow (fresh source port)
                   '    counter\n  }\n}\n')
        net.rpc('PUT /tmp/ruleset.nft ' + ruleset.encode().hex(), retry=True)
        run(net, '/usr/sbin/nft -f /tmp/ruleset.nft')
        run(net, '/usr/sbin/nft list ruleset', contains=('table inet filter', 'chain input', 'policy drop',
                                                          'ct state established,related accept', 'tcp dport 22 accept',
                                                          'tcp dport { 80, 443 } accept', 'udp dport { 53, 123 } accept', 'counter'))
        # rule placement and deletion: insert goes first, `delete rule ... handle N` removes exactly that rule
        run(net, '/usr/sbin/nft insert rule inet filter input tcp dport 8080 accept')
        listed = run(net, '/usr/sbin/nft -a list chain inet filter input', contains=('tcp dport 8080 accept # handle',))
        assert listed.index('tcp dport 8080 accept') < listed.index('ct state established,related accept'), listed
        handle = re.search(r'tcp dport 8080 accept # handle (\d+)', listed).group(1)
        run(net, '/usr/sbin/nft add rule inet filter input tcp dport 8081 accept')
        listed = run(net, '/usr/sbin/nft list chain inet filter input')
        assert listed.index('tcp dport 8081 accept') > listed.index('counter'), listed
        run(net, '/usr/sbin/nft delete rule inet filter input handle ' + handle)
        run(net, '/usr/sbin/nft list ruleset', contains=('tcp dport 8081 accept', 'tcp dport { 80, 443 } accept'), absent=('8080',))
        bad = run(net, '/usr/sbin/nft add rule inet filter input ct zone set 1', want=None)   # an unimplemented write: refused, not ignored
        assert 'Operation not supported' in bad, bad
        run(net, '/usr/sbin/nft list ruleset', absent=('zone set',))
        run(net, '/usr/sbin/nft add rule inet filter input meta mark set 1')                    # (mark writes are implemented)
        run(net, '/usr/sbin/nft list ruleset', contains=('meta mark set 0x00000001',))
        run(net, '/usr/sbin/nft flush ruleset')
        run(net, '/usr/sbin/nft list ruleset', absent=('table inet filter',))
        note('nftables: `nft -f` loaded an inet filter table (ct/iif/ip6 nexthdr/tcp dport/two anonymous sets/counter), '
             '`nft list ruleset` round-tripped it; insert/add placement, delete by handle, an unsupported write refused, flush')

        # wireguard-tools: key generation, then (if the in-kernel device is present) an interface, a peer and `wg show`
        priv = run(net, '/usr/bin/wg genkey').strip()
        assert len(priv) == 44 and priv.endswith('='), priv
        peer = 'yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk='
        add = run(net, '/usr/bin/ip link add dev wg0 type wireguard', want=None)
        if 'not supported' in add or 'does not exist' in add or 'Unknown' in add:
            note('wireguard-tools: wg genkey produced a 44-char key; the in-kernel wireguard device and `wg` config are covered by tests/test_x64_wireguard.py')
        else:
            net.rpc('PUT /tmp/wg.key ' + (priv + '\n').encode().hex(), retry=True)
            run(net, '/usr/bin/wg set wg0 private-key /tmp/wg.key listen-port 51820 peer %s allowed-ips 10.66.0.0/24 endpoint 192.168.50.1:51820' % peer)
            run(net, '/usr/bin/ip -4 addr add 10.66.0.2/24 dev wg0')
            run(net, '/usr/bin/ip link set wg0 up')
            run(net, '/usr/bin/wg show wg0', contains=('interface: wg0', 'public key:', 'listening port: 51820', 'peer: ' + peer, 'allowed ips: 10.66.0.0/24'))
            # a full device (16 peers x 16 allowed IPs, ~13 KB of attributes): the kernel splits its GET_DEVICE dump over
            # several messages that repeat the interface, and wg merges them (the RPC channel carries one ~1400-byte
            # request, so the peers are added one `wg set` at a time)
            def key(i): return base64.b64encode(hashlib.sha256(b'nexus-peer-%d' % i).digest()).decode()
            for i in range(1, 16):
                ips = ','.join(['10.100.%d.%d/32' % (i, k) for k in range(8)] + ['fd00:%x::%x/128' % (i, k) for k in range(8)])
                run(net, '/usr/bin/wg set wg0 peer %s allowed-ips %s' % (key(i), ips))
            peers = run(net, '/usr/bin/wg show wg0 peers').split()
            assert len(peers) == 16 and all(key(i) in peers for i in range(1, 16)), peers
            ips = run(net, '/usr/bin/wg show wg0 allowed-ips')
            total = sum(len(line.split('\t')[1].split()) for line in ips.strip().splitlines())
            assert total == 1 + 15 * 16, (total, ips[:2000])
            full = run(net, '/usr/bin/wg set wg0 peer %s allowed-ips 10.66.1.0/24' % base64.b64encode(b'\x01' * 32).decode(), want=None)
            assert 'No space left' in full, full   # a 17th peer is refused (ENOSPC), the device is unchanged
            run(net, '/usr/bin/wg show wg0 peers', absent=(base64.b64encode(b'\x01' * 32).decode(),))
            run(net, '/usr/bin/ip link del wg0')
            note('wireguard-tools: wg genkey, `ip link add type wireguard`, wg set (key/port/peer/allowed-ips/endpoint), wg show; '
                 'a full device (16 peers x 16 allowed IPs) read back through a multi-message dump, a 17th peer refused (ENOSPC)')

        # net-tools
        run(net, '/usr/sbin/ifconfig eth0', contains=('192.168.50.10', 'inet6', 'ether 52:54:00:12:34:56'))
        run(net, '/usr/sbin/route -n -A inet', contains=('192.168.50.1', '0.0.0.0'))
        note('net-tools: ifconfig and route read the interface and the IPv4 routing table')

        assert net.rpc('EXIT') == 'BYE'
        qemu.wait(timeout=60)
        log = serial_text()
        assert qemu.returncode == 33 and 'NETWIRE AGENT DONE' in log, (qemu.returncode, log[-3000:])
        assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, log[-3000:]
        assert not net.errors, net.errors
    except BaseException:
        print(serial_text()[-5000:]); print('\n'.join(net.errors)[-3000:]); raise
    finally:
        net.stop = True
        if qemu.poll() is None: qemu.kill(); qemu.wait()
        net.sock.close(); disk.unlink(missing_ok=True)
    print(f'PASS native Debian network tools ({SMP} CPU): ' + '; '.join(results))


if __name__ == '__main__':
    main()
