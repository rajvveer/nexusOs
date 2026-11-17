"""nftables enforcement on native NexusOS: proves the nf_hook evaluator (arch/x86_64/nft.c) and connection tracking
(conntrack.c) actually filter traffic, not just that `nft` can load a ruleset. Installs the Debian network tools,
boots on the test_x64_netwire.py fixture network (this process is the whole network over `-netdev dgram`), loads
inet filter rule sets with `nft -f` and observes the effect from the wire:
  - default-accept: the fixture's ICMPv6 echo to the guest is answered.
  - stateful inbound deny (policy drop; ct established,related accept; ND accepted; echo NOT accepted): the guest's
    own outbound TCP connect still completes (the SYN/ACK is ESTABLISHED, accepted), but the fixture's unsolicited
    echo gets no reply (dropped at input), while NDP keeps working.
  - full inbound drop (only iif lo accept): the guest's outbound TCP connect times out (the SYN/ACK is dropped),
    showing the established rule was what let it through; a counter in the chain advances.
  - reject: `tcp dport 2323 reject with tcp reset` answers an IPv6 and an IPv4 SYN with an RST, `tcp dport 2324
    reject` with ICMPv6 port unreachable, an unlisted port gets nothing (policy drop).
  - `nft list ruleset` shows live counter values (packets > 0 after traffic was dropped).
  - flush: connectivity returns.
Usage: test_x64_firewall.py [work dir] [cpus]. The disk is a private copy."""
from pathlib import Path
import os
import importlib.util
import re
import shutil
import struct
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-firewall')
SMP = sys.argv[2] if len(sys.argv) > 2 else '1'
WORK.mkdir(parents=True, exist_ok=True)
PKG_CACHE = nexus_paths.cache('net')

def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
nw = load_module('netwire', ROOT / 'tests/test_x64_netwire.py')
nettools = load_module('nettools_install', ROOT / 'arch/x86_64/nettools_install.py')
ntt = load_module('nettoolstest', ROOT / 'tests/test_x64_nettools.py')
run, esc = ntt.run, ntt.esc

ND = ('    icmpv6 type nd-neighbor-solicit accept\n    icmpv6 type nd-neighbor-advert accept\n'
      '    icmpv6 type nd-router-advert accept\n    icmpv6 type nd-router-solicit accept\n'
      '    udp dport 7001 accept\n')   # keep the test harness RPC channel open while input is otherwise denied
ACCEPT_ESTABLISHED = (
    'table inet filter {\n'
    '  chain input {\n'
    '    type filter hook input priority 0; policy drop;\n'
    '    ct state established,related accept\n'
    '    iif "lo" accept\n' + ND +
    '    tcp dport 2323 reject with tcp reset\n'
    '    tcp dport 2324 reject\n'
    '    counter\n'
    '  }\n}\n')
DROP_ALL = (
    'table inet filter {\n'
    '  chain input {\n'
    '    type filter hook input priority 0; policy drop;\n'
    '    iif "lo" accept\n'
    '    icmpv6 type nd-neighbor-solicit accept\n    icmpv6 type nd-neighbor-advert accept\n'
    '    udp dport 7001 accept\n'   # the harness RPC channel (its reply is OUTPUT, accepted)
    '    counter\n'
    '  }\n}\n')


def tcp4_syn(src, dst, sport, dport):
    h = struct.pack('!HHIIHHHH', sport, dport, 1234, 0, (5 << 12) | 0x02, 65535, 0, 0)
    c = nw.checksum(src + dst + struct.pack('!BBH', 0, 6, len(h)) + h)
    return h[:16] + struct.pack('!H', c) + h[18:]


def load_ruleset(net, text):
    net.rpc('PUT /tmp/rules.nft ' + text.encode().hex(), retry=True)
    run(net, '/usr/sbin/nft flush ruleset', want=None)
    run(net, '/usr/sbin/nft -f /tmp/rules.nft')


_echo_id = [0x3000]
def echo_answered(net):
    """Send ICMPv6 echo requests (a fresh id each call, so conntrack never counts them as established) and return
    how many the guest answered within ~2 s."""
    _echo_id[0] += 1; ident = _echo_id[0]
    before = len([e for e in net.icmp6log if e[1] == 129])
    for i in range(4):
        net.send6(nw.ip6(nw.R_GLOBAL, nw.GUEST_GLOBAL, 58, nw.icmp6(nw.R_GLOBAL, nw.GUEST_GLOBAL, 128, 0, struct.pack('!HH', ident, i) + b'fw')))
        time.sleep(0.1)
    time.sleep(1.5)
    return len([e for e in net.icmp6log if e[1] == 129]) - before


def main():
    results = []
    def note(text): results.append(text); print('  ' + text, flush=True)
    agent = nw.build_agent(WORK)
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    fatput = importlib.util.module_from_spec(importlib.util.spec_from_file_location('fp', ROOT / 'userland/fatput.py'))
    importlib.util.spec_from_file_location('fp', ROOT / 'userland/fatput.py').loader.exec_module(fatput)
    fatput.main(['fatput', str(disk), 'put', str(agent), 'sbin/init'])
    assert nettools.install([str(disk), '--cache', str(PKG_CACHE), '--report', str(WORK / 'install.json')]) == 0
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    net = nw.Network()
    qemu = subprocess.Popen(nw.qemu_command(disk, serial, SMP), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def serial_text(): return serial.read_text(errors='replace') if serial.exists() else ''
    try:
        net.wait(lambda: [e for e in net.dhcplog if e.get('answer') == 5], 90, 'DHCPv4'); net.guest4 = nw.a4('192.168.50.10')
        net.wait(lambda: 'NETWIRE AGENT READY' in serial_text(), 60, 'agent')
        net.wait(lambda: [e for e in net.icmp6log if e[1] == 135 and e[3] == nw.ANY6 and e[7][8:24] == nw.GUEST_GLOBAL], 20, 'global DAD')
        time.sleep(2.5)

        assert echo_answered(net) >= 1, 'baseline: the guest should answer pings before any firewall'
        note('baseline: unsolicited ICMPv6 echo to the guest is answered (no ruleset)')

        load_ruleset(net, ACCEPT_ESTABLISHED)
        assert run(net, '/usr/bin/ping -6 -c 1 -W 3 2001:db8:1::1', want=None, contains=('1 received',)) is not None
        assert run(net, '/usr/sbin/nft list ruleset', contains=('ct state established,related accept', 'counter packets'))
        assert net.rpc('TCP 2001:db8:5::80 80') == 'RESULT 0', 'outbound TCP connect: the SYN/ACK is ESTABLISHED and accepted'
        assert echo_answered(net) == 0, 'unsolicited echo must be dropped (input policy drop, echo not allowed)'
        note('stateful deny: guest keeps NDP + its own outbound TCP (established accepted), unsolicited echo dropped')

        seen4 = []; net.ip4_hook = lambda proto, src, dst, payload: seen4.append((proto, src, dst, payload))
        start = time.monotonic()
        for port in (2323, 2324, 2325):
            net.send6(nw.ip6(nw.R_GLOBAL, nw.GUEST_GLOBAL, 6, nw.tcp6(nw.R_GLOBAL, nw.GUEST_GLOBAL, 40000 + port, port, 1000, 0, 0x02)))
        net.send4(nw.ip4(nw.ROUTER4, net.guest4, 6, tcp4_syn(nw.ROUTER4, net.guest4, 41323, 2323)))
        net.send4(nw.ip4(nw.ROUTER4, net.guest4, 6, tcp4_syn(nw.ROUTER4, net.guest4, 41325, 2325)))
        time.sleep(3)
        rst6 = sorted(e[2] for e in net.tcplog if e[0] >= start and e[1] == nw.R_GLOBAL and e[3] & 0x04)
        unreach = [e for e in net.icmp6log if e[0] >= start and e[1] == 1]
        quoted = [(e[2], struct.unpack('!H', e[7][8 + 40 + 2:8 + 40 + 4])[0]) for e in unreach if len(e[7]) >= 8 + 40 + 4]
        rst4 = sorted(struct.unpack('!H', p[0:2])[0] for proto, s, d, p in seen4 if proto == 6 and d == nw.ROUTER4 and p[13] & 0x04)
        net.ip4_hook = None
        assert rst6 == [42323], ('IPv6: only the SYN to 2323 is answered with an RST', rst6)
        assert quoted == [(4, 2324)], ('IPv6: 2324 gets ICMPv6 port unreachable (code 4), 2325 nothing', quoted)
        assert rst4 == [2323], ('IPv4: the SYN to 2323 is answered with an RST, 2325 dropped', rst4)
        note('reject: `reject with tcp reset` answered IPv6 and IPv4 SYNs to 2323 with an RST, `reject` sent ICMPv6 port '
             'unreachable for 2324, the unlisted 2325 was silently dropped')

        load_ruleset(net, DROP_ALL)
        t = net.rpc('TCP 2001:db8:5::80 80', timeout=20)   # the SYN/ACK comes back but no established rule accepts it
        assert t.split()[1] in ('110', '115'), ('full drop: the SYN/ACK is dropped -> connect times out', t)
        assert echo_answered(net) == 0
        counters = run(net, '/usr/sbin/nft list ruleset', contains=('counter packets',))
        live = [int(n) for n in re.findall(r'counter packets (\d+)', counters)]
        assert live and max(live) > 0, ('the chain counter shows the dropped traffic', counters)
        note(f'full inbound drop: outbound TCP connect times out (SYN/ACK dropped); `nft list` shows the live counter ({max(live)} packets)')

        run(net, '/usr/sbin/nft flush ruleset')
        assert echo_answered(net) >= 1, 'after flush the guest answers again'
        note('flush ruleset: inbound connectivity restored')

        assert net.rpc('EXIT') == 'BYE'
        qemu.wait(timeout=60); log = serial_text()
        assert qemu.returncode == 33 and 'NETWIRE AGENT DONE' in log, (qemu.returncode, log[-3000:])
        assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, log[-3000:]
        assert not net.errors, net.errors
    except BaseException:
        print(serial_text()[-5000:]); print('\n'.join(net.errors)[-3000:]); raise
    finally:
        net.stop = True
        if qemu.poll() is None: qemu.kill(); qemu.wait()
        net.sock.close(); disk.unlink(missing_ok=True)
    print(f'PASS native nftables enforcement ({SMP} CPU): ' + '; '.join(results))


if __name__ == '__main__':
    main()
