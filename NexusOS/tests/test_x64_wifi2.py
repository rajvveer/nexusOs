"""Wave-2 follow-up Wi-Fi features on native NexusOS (after test_x64_wifi.py's base coverage), over the same virtual radios
and Debian wpa_supplicant/hostapd:
  1. sysfs enumeration: /sys/class/ieee80211/phyN/{index,name,macaddress}, /sys/class/net/wlanN/{uevent (DEVTYPE=wlan),
     phy80211 link}, so `iw phy NAME` (not phy#N) resolves and NetworkManager-style enumeration works.
  2. Monitor mode + radiotap: a monitor interface captures every frame on the channel with a radiotap header
     (sll_hatype ARPHRD_IEEE80211_RADIOTAP 803), read with a raw AF_PACKET reader (the netwire agent's MONCAP).
  3. Ciphers beyond CCMP-128: the kernel's AES-128/256, CCMP-128/256, GCMP-128/256 and GMAC known-answer self-test, and
     a live WPA3-SAE connection with GCMP (pairwise+group) + BIP-GMAC-256 management protection, data over the link.
  4. STA+AP concurrency on one phy: while wlan0 stays associated as a client, a second AP interface is added on the same
     phy (`iw phy phy0 interface add ... type __ap`), a hotspot runs on it and a third radio connects to it; an AP on a
     different channel than the client is refused (one radio, one channel).
Usage: test_x64_wifi2.py [work dir] [cpus]. Ports: fixture 9126, QEMU 9127, QMP 4726 (reuses test_x64_wifi.py's)."""
import importlib.util
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wifitest', ROOT / 'tests/test_x64_wifi.py')
w = importlib.util.module_from_spec(spec); spec.loader.exec_module(w)
AP1, AP2, STA = w.AP1, w.AP2, w.STA
HS = '02:00:00:00:00:80'   # the AP vif's locally-administered address (radio mac ^ 0x80 in the last byte)


def scenario2(g, note):
    g.sim('add 4')
    g.sim('selftest'); text = g.sim_text()
    assert 'selftest aes ok cmac ok ccmp ok gcmp ok' in text, text
    note('kernel self-test: AES-128/256, AES-CMAC, CCMP-128/256, GCMP-128/256 and BIP-GMAC known-answer vectors all pass')

    # ---- (1) sysfs enumeration ----
    assert g.file('/sys/class/ieee80211/phy0/index').strip() == '0'
    assert g.file('/sys/class/ieee80211/phy1/name').strip() == 'phy1'
    assert g.file('/sys/class/ieee80211/phy1/macaddress').strip() == AP1
    assert 'DEVTYPE=wlan' in g.file('/sys/devices/platform/wifi0/net/wlan0/uevent')
    g.run('/usr/bin/ip link set wlan1 up')
    g.run('/usr/sbin/iw phy phy1 info', contains=('Wiphy phy1',))            # resolves the phy by NAME via /sys
    note('sysfs: /sys/class/ieee80211/phy0..1 (index, name, macaddress), the wlan net uevent (DEVTYPE=wlan), and '
         '`iw phy phy1 info` resolving the phy by name all read correctly')

    # ---- a WPA2 AP on wlan1 (ch1) as a beacon source and a client target ----
    g.put('/tmp/hostapd1.conf', w.WPA2_AP.format(ifname='wlan1', ssid='NexusNet', channel=1))
    h1 = g.spawn('/usr/sbin/hostapd -t -f /tmp/hostapd1.log /tmp/hostapd1.conf')
    g.wait(lambda: 'AP-ENABLED' in g.grep('/tmp/hostapd1.log', 'AP-ENABLED'), 30, 'hostapd AP-ENABLED')
    g.run('/usr/bin/ip link set wlan0 up')
    g.put('/tmp/wpa.conf', w.WPA_CONF + 'network={\n ssid="NexusNet"\n psk="correct horse battery"\n key_mgmt=WPA-PSK\n}\n')
    sta = g.spawn('/usr/sbin/wpa_supplicant -i wlan0 -D nl80211 -c /tmp/wpa.conf -t -f /tmp/wpa.log')
    g.wait(lambda: g.status().get('wpa_state') == 'COMPLETED', 60, 'wlan0 associated')

    # ---- (2) monitor mode + radiotap ----
    g.run('/usr/sbin/iw dev wlan3 set type monitor')
    g.sim('tune wlan3 2412')
    g.run('/usr/bin/ip link set wlan3 up')
    g.run('/usr/sbin/iw dev wlan3 info', contains=('type monitor',))
    cap = g.rpc('MONCAP wlan3 6')
    m = re.search(r'MONCAP frames (\d+) hatype (\d+) first ([0-9a-f]*)', cap); assert m, cap
    assert int(m.group(1)) >= 1 and int(m.group(2)) == 803, cap                 # ARPHRD_IEEE80211_RADIOTAP
    frame = bytes.fromhex(m.group(3)); it_len = frame[2] | frame[3] << 8        # radiotap it_len -> the 802.11 frame
    dot11 = frame[it_len:]; assert (dot11[0] & 0xFC) == 0x80, cap.encode()      # a beacon
    assert b'NexusNet' in dot11, 'the captured beacon carries the SSID'
    note('monitor mode: `iw dev wlan3 set type monitor`, a raw AF_PACKET reader captured %s frames with sll_hatype=803 '
         '(radiotap); the first is a beacon carrying SSID NexusNet' % m.group(1))

    # ---- (3) GCMP / BIP-GMAC live: a WPA3-SAE AP with GCMP on wlan2 (ch6), connect wlan0 ----
    g.kill(sta); time.sleep(1.0)
    gcmp_ap = ('interface=wlan2\ndriver=nl80211\nctrl_interface=/tmp/hostapd\nssid=NexusG\nhw_mode=g\nchannel=6\nwpa=2\n'
               'wpa_key_mgmt=SAE\nrsn_pairwise=GCMP\ngroup_cipher=GCMP\ngroup_mgmt_cipher=BIP-GMAC-256\nieee80211w=2\n'
               'sae_password=gcmp secret pass\nsae_pwe=2\n')
    g.put('/tmp/hostapd2.conf', gcmp_ap)
    h2 = g.spawn('/usr/sbin/hostapd -t -f /tmp/hostapd2.log /tmp/hostapd2.conf')
    g.wait(lambda: 'AP-ENABLED' in g.grep('/tmp/hostapd2.log', 'AP-ENABLED'), 30, 'GCMP AP-ENABLED')
    g.put('/tmp/wpa.conf', w.WPA_CONF + 'sae_pwe=2\nnetwork={\n ssid="NexusG"\n sae_password="gcmp secret pass"\n key_mgmt=SAE\n'
          ' pairwise=GCMP\n group=GCMP\n ieee80211w=2\n}\n')
    g.put('/tmp/wpa.log', '\n')
    sta = g.spawn('/usr/sbin/wpa_supplicant -i wlan0 -D nl80211 -c /tmp/wpa.conf -t -f /tmp/wpa.log')
    st = g.wait(lambda: (lambda s: s if s.get('wpa_state') == 'COMPLETED' else None)(g.status()), 60, 'GCMP connect')
    assert st.get('pairwise_cipher') == 'GCMP' and st.get('group_cipher') == 'GCMP' and st.get('bssid') == AP2, st
    g.run('/usr/bin/ip addr add 192.168.88.1/24 dev wlan2'); g.run('/usr/bin/ip addr add 192.168.88.2/24 dev wlan0')
    before = w_medium(g)
    out = g.rpc('ARPING wlan0 192.168.88.2 192.168.88.1 3'); assert out.endswith('from ' + AP2) and 'replies 0' not in out, out
    after = w_medium(g)
    assert after['data-protected'] > before['data-protected'] and after['data-plain'] == before['data-plain'], (before, after)
    note('WPA3-SAE with GCMP: wpa_state=COMPLETED pairwise_cipher=GCMP group_cipher=GCMP (group mgmt BIP-GMAC-256); ARP over '
         'the link succeeded and the medium carried only protected data frames')
    g.kill(sta); g.kill(h2); time.sleep(1.0)

    # ---- (4) STA+AP concurrency on one phy: wlan0 is a client on NexusNet (ch1); add an AP vif on phy0 ----
    g.put('/tmp/wpa.conf', w.WPA_CONF + 'network={\n ssid="NexusNet"\n psk="correct horse battery"\n key_mgmt=WPA-PSK\n}\n')
    g.put('/tmp/wpa.log', '\n')
    sta = g.spawn('/usr/sbin/wpa_supplicant -i wlan0 -D nl80211 -c /tmp/wpa.conf -t -f /tmp/wpa.log')
    g.wait(lambda: g.status().get('wpa_state') == 'COMPLETED' and g.status().get('bssid') == AP1, 60, 'wlan0 on NexusNet (ch1)')
    g.run('/usr/sbin/iw phy phy0 interface add hs0 type __ap', want=None)
    g.run('/usr/sbin/iw dev wlan0 info', contains=('type managed',))
    g.run('/usr/sbin/iw dev hs0 info', contains=('type AP', 'wiphy 0'))                 # the AP vif shares phy0
    # a hotspot on the AP vif, on the SAME channel as the client (enforced)
    g.put('/tmp/hs.conf', 'interface=hs0\ndriver=nl80211\nctrl_interface=/tmp/hostapd\nssid=NexusHotspot\nhw_mode=g\nchannel=1\n'
          'wpa=2\nwpa_passphrase=hotspot pass 1\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n')
    hs = g.spawn('/usr/sbin/hostapd -t -f /tmp/hs.log /tmp/hs.conf')
    g.wait(lambda: 'AP-ENABLED' in g.grep('/tmp/hs.log', 'AP-ENABLED'), 30, 'hotspot AP-ENABLED')
    g.put('/tmp/wc.conf', 'ctrl_interface=/tmp/wc\nupdate_config=0\nnetwork={\n ssid="NexusHotspot"\n psk="hotspot pass 1"\n key_mgmt=WPA-PSK\n}\n')
    client = g.spawn('/usr/sbin/wpa_supplicant -i wlan2 -D nl80211 -c /tmp/wc.conf -t -f /tmp/wc.log')
    def wc(): out = g.run('/usr/sbin/wpa_cli -p /tmp/wc -i wlan2 status', want=None); return dict(l.split('=', 1) for l in out.splitlines() if '=' in l)
    g.wait(lambda: wc().get('wpa_state') == 'COMPLETED', 60, 'client on the hotspot')
    assert wc().get('bssid') == HS and wc().get('ssid') == 'NexusHotspot', wc()
    assert g.status().get('wpa_state') == 'COMPLETED' and g.status().get('bssid') == AP1, 'the uplink STA stayed associated'
    sta_dump = g.run('/usr/sbin/hostapd_cli -p /tmp/hostapd -i hs0 all_sta', want=None); assert '02:00:00:00:00:02' in sta_dump and 'AUTHORIZED' in sta_dump, sta_dump
    note('STA+AP concurrency on phy0: wlan0 stayed a client on NexusNet while hs0 (added with `iw phy phy0 interface add`) '
         'ran a WPA2 hotspot on the same channel that wlan2 joined (both COMPLETED); the uplink was unaffected')
    # the one-radio channel constraint: an AP on a different channel than the client is refused
    g.kill(hs); time.sleep(1.0)
    g.put('/tmp/hs6.conf', 'interface=hs0\ndriver=nl80211\nctrl_interface=/tmp/hostapd\nssid=NexusHotspot6\nhw_mode=g\nchannel=6\n'
          'wpa=2\nwpa_passphrase=hotspot pass 1\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n')
    g.spawn('/usr/sbin/hostapd -t -f /tmp/hs6.log /tmp/hs6.conf'); time.sleep(3)
    assert 'AP-ENABLED' not in g.grep('/tmp/hs6.log', 'AP-ENABLED'), 'an AP on another channel should be refused while the client holds ch1'
    assert g.status().get('wpa_state') == 'COMPLETED', 'the client uplink survived the rejected cross-channel AP'
    g.run('/usr/sbin/iw dev hs0 del', want=None)
    note('channel constraint: a hotspot requested on channel 6 while the client holds channel 1 was refused (one radio), and '
         'the uplink stayed up; the AP vif was then deleted')
    g.kill(sta); g.kill(client); g.kill(h1)


def w_medium(g):
    text = g.sim_text(); m = re.search(r'medium frames (\d+) dropped (\d+) data-protected (\d+) data-plain (\d+)', text)
    assert m, text
    return {'data-protected': int(m.group(3)), 'data-plain': int(m.group(4))}


# Reuse test_x64_wifi.py's boot harness, substituting this scenario.
w.scenario = scenario2
if __name__ == '__main__':
    w.main()
