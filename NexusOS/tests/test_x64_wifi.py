"""Wi-Fi without hardware on native NexusOS: Debian's hostapd and wpa_supplicant (arch/x86_64/wifi_install.py, from the
signed wifi-lock.json) over the kernel's nl80211 (arch/x86_64/nl80211.c) and the virtual radio medium of the softmac
(arch/x86_64/wifi.c, /proc/sys/net/nexus/wifi_sim: radios on one channel hear each other, per-radio signal level and
range switch, raw frame injection for hostile input). One guest, three radios: wlan0 is the client, wlan1 and wlan2
are access points.
  - kernel crypto self-test (AES-128 FIPS-197, AES-CMAC RFC 4493, CCMP IEEE 802.11 M.6.4); `iw list` / `iw dev`
  - WPA2-PSK: hostapd on wlan1 (channel 1), `iw dev wlan0 scan` sees it with its signal, wpa_supplicant connects
    (4-way handshake, CCMP), `wpa_cli status`/`signal_poll`, `iw dev wlan0 link`, `iw dev wlan1 station dump`;
    ARP over the link from a packet socket on wlan0 (round trips through the encrypted data path, the medium counters
    show protected data frames only); the signal follows `signal wlan1 -67`
  - roaming: a second AP with the same SSID on wlan2 (channel 6): `wpa_cli roam` reassociates to it (PREV_BSSID),
    data still flows; wlan2 out of range -> beacon loss -> wpa_supplicant reconnects to wlan1
  - WPA3-SAE with PMF required on wlan2: SAE commit/confirm through NL80211_CMD_AUTHENTICATE, BIP/IGTK installed;
    hostapd_cli deauthenticate (CCMP-protected deauth) -> disconnect + reconnect; a spoofed unprotected deauth is
    ignored (UNPROT_DEAUTHENTICATE), the link stays up
  - hostile: wrong WPA2 passphrase (4-way handshake fails, no connection), injected garbage (truncated/oversized/
    bogus management and data frames, unknown BSSIDs, bad CCMP), malformed nl80211 requests via iw; no fault
Usage: test_x64_wifi.py [work dir] [cpus] [--serve]  (--serve: keep the guest for manual RPCs on 127.0.0.1:9128)
Ports: fixture 9126, QEMU 9127, QMP 4726 (net2's range)."""
from pathlib import Path
import importlib.util
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
SERVE = '--serve' in sys.argv
WORK = Path(args[0]) if args else Path(r'D:\nexustest\wave2\ws_net2_runs\wifi')
SMP = args[1] if len(args) > 1 else '2'
WORK.mkdir(parents=True, exist_ok=True)
NET_CACHE = Path(os.environ.get('NEXUS_NET_PKGS', r'D:\nexustest\pkgs\nettools'))
WIFI_CACHE = Path(os.environ.get('NEXUS_WIFI_PKGS', r'D:\nexustest\wave2\ws_net2_pkgs'))

spec = importlib.util.spec_from_file_location('netwire', ROOT / 'tests/test_x64_netwire.py')
nw = importlib.util.module_from_spec(spec); spec.loader.exec_module(nw)
nw.FIXTURE_PORT, nw.QEMU_PORT, nw.QMP_PORT = 9126, 9127, 4726
def load(name, path):
    s = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(s); s.loader.exec_module(m); return m

SIM = '/proc/sys/net/nexus/wifi_sim'
AP1, AP2, STA, EVIL = '02:00:00:00:00:01', '02:00:00:00:00:02', '02:00:00:00:00:00', '02:00:00:00:00:03'
WPA2_AP = """interface={ifname}
driver=nl80211
ctrl_interface=/tmp/hostapd
ssid={ssid}
hw_mode=g
channel={channel}
wpa=2
wpa_passphrase=correct horse battery
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
"""
SAE_AP = """interface={ifname}
driver=nl80211
ctrl_interface=/tmp/hostapd
ssid={ssid}
hw_mode=g
channel={channel}
wpa=2
wpa_key_mgmt=SAE
rsn_pairwise=CCMP
ieee80211w=2
sae_password=nexus sae secret
sae_pwe=2
"""
WPA_CONF = """ctrl_interface=/tmp/wpa
update_config=0
"""


class Guest:
    def __init__(self, net, serial): self.net, self.serial = net, serial; self.logs = []
    def rpc(self, text, timeout=45, retry=True): return self.net.rpc(text, timeout=timeout, retry=retry)
    # (a resent RUN is executed again after the first one: commands that take seconds are sent once)
    def run(self, command, seconds=15, want=0, contains=(), absent=(), timeout=60):
        reply = self.rpc('RUN %d %s' % (seconds, command), timeout=max(timeout, seconds + 15), retry=seconds <= 15)
        assert reply.startswith('EXIT '), reply
        status, _, output = reply[5:].partition('\n'); status = int(status)
        if want is not None: assert status == want, f'{command!r} exited {status} (wanted {want}):\n{output}'
        for s in contains: assert s in output, f'{command!r} output missing {s!r}:\n{output}'
        for s in absent: assert s not in output, f'{command!r} output had {s!r}:\n{output}'
        return output
    def put(self, path, text): assert self.rpc('PUT %s %s' % (path, text.encode().hex())) == 'OK'
    def sim(self, command):
        reply = self.rpc('WRITE %s %s' % (SIM, command.replace(' ', ',')))
        assert reply == 'OK', (command, reply)
    def sim_text(self): return self.rpc('FILE ' + SIM)
    def file(self, path): return self.rpc('FILE ' + path)
    def grep(self, path, text): return self.rpc('GREP %s %s' % (path, text.replace(' ', '%20')))
    def spawn(self, command):
        reply = self.rpc('SPAWN ' + command); assert reply.startswith('PID '), reply; return int(reply[4:])
    def kill(self, pid): self.rpc('KILL %d' % pid)
    def wait(self, predicate, timeout, what, step=0.5):
        deadline = time.monotonic() + timeout; last = None
        while time.monotonic() < deadline:
            last = predicate()
            if last: return last
            time.sleep(step)
        raise AssertionError('timed out waiting for ' + what + (': %r' % (last,) if last not in (None, False, '') else ''))
    def wpa(self, *words, iface='wlan0'):
        return self.run('/usr/sbin/wpa_cli -p /tmp/wpa -i %s %s' % (iface, ' '.join(w.replace(' ', '%20') for w in words)), want=None)
    def hapd(self, iface, *words):
        return self.run('/usr/sbin/hostapd_cli -p /tmp/hostapd -i %s %s' % (iface, ' '.join(words)), want=None)
    def status(self):
        out = self.wpa('status'); return dict(l.split('=', 1) for l in out.splitlines() if '=' in l)


def main():
    results = []
    def note(text): results.append(text); print('  ' + text, flush=True)
    agent = nw.build_agent(WORK)
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    fatput = load('wifi_fatput', ROOT / 'userland/fatput.py')
    fatput.main(['fatput', str(disk), 'put', str(agent), 'sbin/init'])
    cli = WORK / 'aurora-wifi.elf'                     # the Network app's Wi-Fi code, headless (tests/wifi/aurora_wifi_cli.c)
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                    '-DNEXUS_HOSTED=1', '-DNEXUS_AURORA=1', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/aurora_wifi.c'),
                    str(ROOT / 'tests/wifi/aurora_wifi_cli.c'), '-o', str(cli)], check=True)
    fatput.main(['fatput', str(disk), 'put', str(cli), 'usr/bin/aurora-wifi'])
    nettools = load('wifi_nettools', ROOT / 'arch/x86_64/nettools_install.py')
    assert nettools.install([str(disk), '--cache', str(NET_CACHE), '--report', str(WORK / 'nettools.json')]) == 0
    wifi = load('wifi_install', ROOT / 'arch/x86_64/wifi_install.py')
    assert wifi.nt.install([str(disk), '--cache', str(WIFI_CACHE), '--report', str(WORK / 'wifi.json')]) == 0
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    net = nw.Network()
    qemu = subprocess.Popen(nw.qemu_command(disk, serial, SMP), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def serial_text(): return serial.read_text(errors='replace') if serial.exists() else ''
    g = Guest(net, serial)
    try:
        net.wait(lambda: [e for e in net.dhcplog if e.get('answer') == 5], 90, 'DHCPv4'); net.guest4 = nw.a4('192.168.50.10')
        net.wait(lambda: 'NETWIRE AGENT READY' in serial_text(), 60, 'agent')
        net.wait(lambda: [e for e in net.icmp6log if e[1] == 135 and e[3] == nw.ANY6 and e[7][8:24] == nw.GUEST_GLOBAL], 20, 'global DAD')
        time.sleep(2.0)
        if SERVE: return serve(g)
        scenario(g, note)
        assert 'Kernel panic' not in serial_text() and 'KGUARD' not in serial_text(), serial_text()[-3000:]
        print('PASS native Wi-Fi (%s CPU): %s' % (SMP, '; '.join(results)), flush=True)
    except BaseException:
        for path in ('/tmp/wpa.log', '/tmp/hostapd1.log', '/tmp/hostapd2.log'):
            try: print('---- tail', path, '----\n' + g.rpc('TAIL %s 6000' % path, timeout=10), flush=True)
            except BaseException: pass
        try: print('---- wifi_sim ----\n' + g.sim_text(), flush=True)
        except BaseException: pass
        print('---- serial ----\n' + serial_text()[-4000:], flush=True)
        raise
    finally:
        qemu.kill(); qemu.wait()
        for name in ('disk.img', 'serial.log'):
            try: (WORK / name).unlink()
            except OSError: pass


def serve(g):
    """Development aid: RPCs from 127.0.0.1:9128 (one line in, the reply out), until 'QUIT'."""
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1', 9128)); s.listen(4)
    print('SERVING on 127.0.0.1:9128', flush=True)
    while True:
        c, _ = s.accept()
        line = c.makefile().readline().strip()
        if line == 'QUIT': c.close(); return
        try: reply = g.rpc(line, timeout=60)
        except BaseException as e: reply = 'HARNESS ERROR %r' % e
        c.sendall(reply.encode() + b'\n'); c.close()


def scenario(g, note):
    # ---- radios, self-test, iw ----
    g.sim('add 4'); g.sim('selftest')
    text = g.sim_text()
    for r in ('phy0 wlan0 ' + STA, 'phy1 wlan1 ' + AP1, 'phy2 wlan2 ' + AP2, 'phy3 wlan3 ' + EVIL): assert r in text, text
    assert 'selftest aes ok cmac ok ccmp ok gcmp ok' in text, text   # AES-128/256, CMAC, CCMP-128/256, GCMP-128/256 + GMAC KATs
    out = g.run('/usr/sbin/iw list', contains=('Wiphy phy0', 'Wiphy phy2', 'Supported interface modes:', '* managed', '* AP', 'CCMP-128',
                                              'CMAC (00-0f-ac:6)', 'Band 1:', 'Band 2:', '2412 MHz [1]', '5180 MHz [36]',
                                              'Supported commands:', 'authenticate', 'start_ap'))
    g.run('/usr/sbin/iw dev', contains=('Interface wlan0', 'Interface wlan2', 'type managed', 'addr ' + STA))
    note('kernel AES-128/AES-CMAC/CCMP self-test passed; 4 virtual radios (wlan3: the attacker); `iw list` shows phy0-2 (managed+AP, CCMP-128, '
         'BIP-CMAC-128, 2.4 + 5 GHz), `iw dev` wlan0-2')

    # ---- WPA2-PSK access point on wlan1 ----
    g.put('/tmp/hostapd1.conf', WPA2_AP.format(ifname='wlan1', ssid='NexusNet', channel=1))
    h1 = g.spawn('/usr/sbin/hostapd -dd -t -f /tmp/hostapd1.log /tmp/hostapd1.conf')
    g.wait(lambda: 'AP-ENABLED' in g.grep('/tmp/hostapd1.log', 'AP-ENABLED'), 30, 'hostapd AP-ENABLED')
    g.run('/usr/sbin/iw dev wlan1 info', contains=('type AP', 'ssid NexusNet', 'channel 1 (2412 MHz)'))
    g.run('/usr/bin/ip link set wlan0 up')
    scan = g.run('/usr/sbin/iw dev wlan0 scan', seconds=20, contains=('BSS ' + AP1, 'SSID: NexusNet', 'signal: -40.00 dBm', 'freq: 2412',
                                                                       'RSN:', 'PSK', 'CCMP'))
    note('hostapd (WPA2-PSK, channel 1) started on wlan1 (AP-ENABLED, `iw dev wlan1 info` type AP); `iw dev wlan0 scan` finds '
         'NexusNet at -40 dBm with its RSN element')

    # ---- wpa_supplicant: hostile first (wrong passphrase), then the right one ----
    g.put('/tmp/wpa.conf', WPA_CONF + 'network={\n ssid="NexusNet"\n psk="wrong passphrase!"\n key_mgmt=WPA-PSK\n}\n')
    w = g.spawn('/usr/sbin/wpa_supplicant -i wlan0 -D nl80211 -c /tmp/wpa.conf -dd -t -f /tmp/wpa.log')
    g.wait(lambda: 'WRONG_KEY' in g.grep('/tmp/wpa.log', 'WRONG_KEY') or 'pre-shared key may be incorrect' in g.grep('/tmp/wpa.log', 'may%20be%20incorrect'),
           60, 'wrong-key detection')
    assert g.status().get('wpa_state') != 'COMPLETED'
    g.kill(w); time.sleep(1.0)
    note('wrong WPA2 passphrase: the 4-way handshake failed ("pre-shared key may be incorrect"), no connection')
    g.put('/tmp/wpa.conf', WPA_CONF + 'network={\n ssid="NexusNet"\n psk="correct horse battery"\n key_mgmt=WPA-PSK\n}\n')
    g.put('/tmp/wpa.log', '\n')
    w = g.spawn('/usr/sbin/wpa_supplicant -i wlan0 -D nl80211 -c /tmp/wpa.conf -dd -t -f /tmp/wpa.log')
    g.wait(lambda: 'CTRL-EVENT-CONNECTED' in g.grep('/tmp/wpa.log', 'CTRL-EVENT-CONNECTED'), 60, 'WPA2 connection')
    st = g.wait(lambda: (lambda s: s if s.get('wpa_state') == 'COMPLETED' else None)(g.status()), 20, 'COMPLETED')
    assert st.get('bssid') == AP1 and st.get('ssid') == 'NexusNet' and st.get('key_mgmt') == 'WPA2-PSK' and st.get('pairwise_cipher') == 'CCMP', st
    sig = g.wpa('signal_poll'); assert 'RSSI=-40' in sig, sig
    nxt = re.search(r'next-event wifi (none|\d+ms)', g.sim_text()).group(1)   # tickless getter: AP beacon <= 1 interval
    assert nxt != 'none' and int(nxt[:-2]) <= 110, nxt
    g.run('/usr/sbin/iw dev wlan0 link', contains=('Connected to ' + AP1, 'SSID: NexusNet', 'freq: 2412', 'signal: -40 dBm'))
    g.run('/usr/sbin/iw dev wlan1 station dump', contains=('Station ' + STA, 'authorized:\tyes', 'associated:\tyes'))
    sta = g.hapd('wlan1', 'all_sta'); assert STA in sta and 'AUTHORIZED' in sta, sta
    note('wpa_supplicant connected to NexusNet: wpa_state=COMPLETED key_mgmt=WPA2-PSK pairwise_cipher=CCMP, signal_poll RSSI=-40, '
         '`iw dev wlan0 link` and `iw dev wlan1 station dump` (authorized) agree, hostapd lists the station AUTHORIZED')

    # ---- data over the link: ARP round trips on a packet socket (wlan1's stack answers over the CCMP data path) ----
    g.run('/usr/bin/ip addr add 192.168.77.1/24 dev wlan1'); g.run('/usr/bin/ip addr add 192.168.77.2/24 dev wlan0')
    before = medium(g)
    out = g.rpc('ARPING wlan0 192.168.77.2 192.168.77.1 4'); replies = int(re.search(r'replies (\d+)', out).group(1))
    assert replies >= 3 and out.endswith('from ' + AP1), out
    after = medium(g)
    assert after['data-protected'] - before['data-protected'] >= 2 * replies and after['data-plain'] == before['data-plain'], (before, after)
    note('ARP from wlan0 (packet socket) to 192.168.77.1: %d/4 replies from %s over the air; the medium carried %d protected and no plain data frames'
         % (replies, AP1, after['data-protected'] - before['data-protected']))

    # ---- signal follows the medium ----
    g.sim('signal wlan1 -67')
    g.wait(lambda: 'signal: -67 dBm' in g.run('/usr/sbin/iw dev wlan0 link', want=None), 10, 'signal -67')
    assert 'RSSI=-67' in g.wpa('signal_poll')
    note('signal: `signal wlan1 -67` on the medium -> iw link and wpa_cli signal_poll report -67 dBm')

    # ---- roaming: same SSID on wlan2 (channel 6, weaker than wlan1 so wpa_supplicant stays until told) ----
    g.sim('signal wlan1 -40'); g.sim('signal wlan2 -55')
    g.put('/tmp/hostapd2.conf', WPA2_AP.format(ifname='wlan2', ssid='NexusNet', channel=6))
    h2 = g.spawn('/usr/sbin/hostapd -dd -t -f /tmp/hostapd2.log /tmp/hostapd2.conf')
    g.wait(lambda: 'AP-ENABLED' in g.grep('/tmp/hostapd2.log', 'AP-ENABLED'), 30, 'hostapd 2 AP-ENABLED')
    g.wpa('scan'); g.wait(lambda: AP2 in g.wpa('scan_results'), 20, 'AP2 in scan results')
    assert g.status().get('bssid') == AP1
    assert 'OK' in g.wpa('roam', AP2)
    g.wait(lambda: g.status().get('bssid') == AP2 and g.status().get('wpa_state') == 'COMPLETED', 30, 'roam to AP2')
    g.run('/usr/sbin/iw dev wlan0 link', contains=('Connected to ' + AP2, 'freq: 2437'))
    g.run('/usr/bin/ip addr add 192.168.78.1/24 dev wlan2'); g.run('/usr/bin/ip addr add 192.168.78.2/24 dev wlan0')
    out = g.rpc('ARPING wlan0 192.168.78.2 192.168.78.1 3'); assert 'replies 0' not in out and out.endswith('from ' + AP2), out
    reassoc = g.grep('/tmp/hostapd2.log', 'reassociation request'); assert 'current_ap=' + AP1 in reassoc, reassoc
    note('roaming: `wpa_cli roam %s` reassociated to the second AP (channel 6, Reassociation Request with current_ap=%s), 4-way handshake '
         'again, ARP over it' % (AP2, AP1))
    g.sim('range wlan2 off')
    g.wait(lambda: g.status().get('bssid') == AP1 and g.status().get('wpa_state') == 'COMPLETED', 60, 'fallback to AP1')
    disconnected = g.grep('/tmp/wpa.log', 'CTRL-EVENT-DISCONNECTED'); assert AP2 in disconnected, disconnected
    g.sim('range wlan2 on')
    note('wlan2 out of range: beacon loss after 3 s -> CTRL-EVENT-DISCONNECTED from AP2 -> wpa_supplicant reconnected to AP1')

    # ---- WPA3-SAE + PMF on wlan2 ----
    g.kill(h2); time.sleep(1.5)
    g.put('/tmp/hostapd2.conf', SAE_AP.format(ifname='wlan2', ssid='NexusSAE', channel=11))
    g.put('/tmp/hostapd2.log', '\n')
    h2 = g.spawn('/usr/sbin/hostapd -dd -t -f /tmp/hostapd2.log /tmp/hostapd2.conf')
    g.wait(lambda: 'AP-ENABLED' in g.grep('/tmp/hostapd2.log', 'AP-ENABLED'), 30, 'hostapd SAE AP-ENABLED')
    g.kill(w); time.sleep(1.0)
    g.put('/tmp/wpa.conf', WPA_CONF + 'sae_pwe=2\nnetwork={\n ssid="NexusSAE"\n sae_password="nexus sae secret"\n key_mgmt=SAE\n ieee80211w=2\n}\n')
    g.put('/tmp/wpa.log', '\n')
    w = g.spawn('/usr/sbin/wpa_supplicant -i wlan0 -D nl80211 -c /tmp/wpa.conf -dd -t -f /tmp/wpa.log')
    st = g.wait(lambda: (lambda s: s if s.get('wpa_state') == 'COMPLETED' else None)(g.status()), 60, 'SAE COMPLETED')
    assert st.get('bssid') == AP2 and st.get('key_mgmt') == 'SAE' and st.get('pmf') == '2' and st.get('mgmt_group_cipher') == 'BIP', st
    assert 'sae_group' in st, st
    out = g.rpc('ARPING wlan0 192.168.78.2 192.168.78.1 3'); assert 'replies 0' not in out and out.endswith('from ' + AP2), out
    note('WPA3-SAE: SAE commit/confirm via NL80211_CMD_AUTHENTICATE, key_mgmt=SAE sae_group=%s pmf=%s mgmt_group_cipher=BIP (IGTK), ARP over it'
         % (st.get('sae_group'), st.get('pmf')))
    # spoofed unprotected deauth from AP2's address: ignored under PMF
    g.sim('tune wlan3 2462')
    marker = len(g.grep('/tmp/wpa.log', 'CTRL-EVENT-DISCONNECTED').splitlines())
    deauth = 'c000' + '0000' + STA.replace(':', '') + AP2.replace(':', '') + AP2.replace(':', '') + '0000' + '0700'
    g.sim('inject wlan3 ' + deauth)
    g.wait(lambda: 'Unprotected Deauthentication frame dropped' in g.grep('/tmp/wpa.log', 'Unprotected Deauthentication'), 10, 'unprotected deauth event')
    g.wait(lambda: 'Reply to pending SA Query received' in g.grep('/tmp/wpa.log', 'SA Query'), 10, 'SA Query answered')
    time.sleep(1.0)
    st = g.status(); assert st.get('wpa_state') == 'COMPLETED' and st.get('bssid') == AP2, st
    assert len(g.grep('/tmp/wpa.log', 'CTRL-EVENT-DISCONNECTED').splitlines()) == marker
    note('spoofed unprotected deauthentication (injected by wlan3 with AP2\'s address): reported as UNPROT_DEAUTHENTICATE, '
         'wpa_supplicant ran an SA Query (CCMP-protected action frames) that AP2 answered, still connected')
    # protected deauth from hostapd: disconnect, then reconnect
    marker = len(g.grep('/tmp/wpa.log', 'CTRL-EVENT-DISCONNECTED').splitlines())
    assert 'OK' in g.hapd('wlan2', 'deauthenticate', STA)
    g.wait(lambda: len(g.grep('/tmp/wpa.log', 'CTRL-EVENT-DISCONNECTED').splitlines()) > marker, 20, 'protected deauth')
    g.wait(lambda: g.status().get('wpa_state') == 'COMPLETED', 60, 'SAE reconnect')
    note('hostapd_cli deauthenticate: the CCMP-protected deauthentication disconnected the client, which reconnected with SAE')

    # ---- hostile frames and requests ----
    junk = ['b0', 'b000' + '0000' + STA.replace(':', ''),                                   # truncated
            '0802' + '0000' + STA.replace(':', '') + AP2.replace(':', '') + AP2.replace(':', '') + '0000' + 'aaaa030000000800' + '45' * 20,   # plain data from the AP
            '0842' + '0000' + STA.replace(':', '') + AP2.replace(':', '') + AP2.replace(':', '') + '0000' + '01000020000000' + '00' * 30,   # bad CCMP
            '8000' + '0000' + 'ffffffffffff' + '020000000099' * 2 + '0000' + '00' * 12 + '00ff' + '41' * 30,   # beacon with an overlong SSID IE
            '1000' + '0000' + STA.replace(':', '') + AP2.replace(':', '') + AP2.replace(':', '') + '0000' + '1100' + '0000' + 'ffff',   # assoc resp, bogus
            'd000' + '0000' + STA.replace(':', '') + AP2.replace(':', '') + AP2.replace(':', '') + '0000' + '08',   # truncated SA Query
            '0000' + '0000' + AP2.replace(':', '') + '020000000099' + AP2.replace(':', '') + '0000' + '3104' + '0a00' + '00ff',   # assoc req to AP
            'ffff' * 20]
    for frame in junk: g.sim('inject wlan3 ' + frame)
    time.sleep(2.0)
    st = g.status(); assert st.get('wpa_state') == 'COMPLETED', st
    g.run('/usr/sbin/iw dev wlan0 set type ibss', want=None)                                     # unsupported: EOPNOTSUPP
    g.run('/usr/sbin/iw dev wlan0 connect NexusNet', want=None)                                   # CONNECT: not supported
    g.run('/usr/sbin/iw phy#0 set rts 0', want=None); g.run('/usr/sbin/iw dev wlan0 scan freq 1234', want=None)
    for bad in ('/usr/sbin/iw dev wlan0 set type ibss', '/usr/sbin/iw dev wlan0 connect NexusNet', '/usr/sbin/iw dev wlan0 scan freq 1234'):
        out = g.run(bad, want=None); assert 'command failed' in out, (bad, out)
    g.run('/usr/sbin/iw dev wlan0 link', contains=('Connected to ' + AP2,))
    note('hostile: %d injected frames (truncated, plain data under keys, bad CCMP, overlong IE, bogus responses) and invalid '
         'iw requests (ibss, connect, bad frequency): rejected, link kept' % len(junk))

    # ---- the Aurora Network app's Wi-Fi logic (aurora_wifi.c, headless build): own wpa_supplicant, saved networks ----
    g.wpa('terminate'); time.sleep(1.5)
    cli = '/usr/bin/aurora-wifi'
    out = g.run(cli + ' status', contains=('wifi iface=wlan0', 'state=not running'))
    out = g.run(cli + ' connect NexusNet wrong%20passphrase', seconds=40, want=1)
    assert 'state=COMPLETED' not in out, out
    g.run(cli + ' forget NexusNet', want=0)
    out = g.run(cli + ' connect NexusNet correct%20horse%20battery', seconds=40, contains=('security=WPA2', 'state=COMPLETED', 'bssid=' + AP1))
    out = g.run(cli + ' connect NexusSAE nexus%20sae%20secret', seconds=40, contains=('security=WPA3', 'state=COMPLETED', 'bssid=' + AP2))
    g.run(cli + ' saved', contains=('saved NexusNet', 'saved NexusSAE'))
    conf = g.file('/home/nexus/.aurora/wpa_supplicant-wlan0.conf')
    assert 'ssid="NexusNet"' in conf and 'ssid="NexusSAE"' in conf and 'key_mgmt=SAE' in conf and 'ieee80211w=2' in conf, conf
    scan = g.run(cli + ' scan', seconds=20, contains=('NexusNet\t', 'NexusSAE\t', 'WPA3', 'connected'))
    g.run('/usr/sbin/wpa_cli -p /tmp/aurora-wpa -i wlan0 terminate', want=None); time.sleep(1.5)
    g.run(cli + ' status', contains=('state=not running',))
    g.run(cli + ' --boot', seconds=20)                                                            # the boot step
    g.wait(lambda: 'state=COMPLETED' in g.run(cli + ' status', want=None), 40, 'reconnect at boot')
    g.run(cli + ' forget NexusNet', contains=('Forgot NexusNet',))
    g.run(cli + ' saved', contains=('saved NexusSAE',), absent=('saved NexusNet',))
    assert 'NexusNet' not in g.file('/home/nexus/.aurora/wpa_supplicant-wlan0.conf')
    note('Aurora Network Wi-Fi (aurora_wifi.c, headless): wrong password refused; Connect saved NexusNet (WPA2) and NexusSAE '
         '(WPA3, ieee80211w=2) in ~/.aurora/wpa_supplicant-wlan0.conf and joined each; Scan lists both with security and '
         '"connected"; the boot step restarted wpa_supplicant and reconnected; Forget removed NexusNet from the saved file')

    # ---- teardown ----
    g.run('/usr/sbin/wpa_cli -p /tmp/aurora-wpa -i wlan0 terminate', want=None); g.kill(h1); g.kill(h2); time.sleep(1.5)
    g.run('/usr/sbin/iw dev wlan1 info', contains=('type managed',))   # hostapd restores station mode on exit
    g.sim('del phy2'); text = g.sim_text(); assert 'wlan2' not in text, text
    g.run('/usr/sbin/iw dev', absent=('wlan2',))
    note('teardown: wpa_supplicant/hostapd exit, `del phy2` removes wlan2 (iw dev agrees)')


def medium(g):
    text = g.sim_text(); m = re.search(r'medium frames (\d+) dropped (\d+) data-protected (\d+) data-plain (\d+)', text)
    assert m, text
    return {'frames': int(m.group(1)), 'dropped': int(m.group(2)), 'data-protected': int(m.group(3)), 'data-plain': int(m.group(4))}


if __name__ == '__main__':
    main()
