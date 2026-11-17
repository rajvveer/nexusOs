"""IPv6 end to end: SLAAC from QEMU's slirp router, AF_INET6 sockets over loopback and to host IPv6 listeners
([::1] through slirp's prefix::2), dual stack, ancillary data, fragmentation, ping/raw sockets, and wire checks
(DAD probes, router solicitation, MLDv2 reports) on the captured frames. All disks are private copies."""
from pathlib import Path
import importlib.util
import shutil
import socket
import struct
import subprocess
import sys
import threading
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-ipv6')
WORK.mkdir(parents=True, exist_ok=True)
TCP_PORT, UDP_PORT = 8941, 8942
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                '-pthread', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/net6_user.c'), '-o', str(WORK / 'net6-user.elf')], check=True)
server = socket.socket(socket.AF_INET6, socket.SOCK_STREAM); server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('::1', TCP_PORT)); server.listen(4); server.settimeout(120)
udp = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM); udp.bind(('::1', UDP_PORT)); udp.settimeout(0.25)
stop = threading.Event(); errors = []; served = []
def serve_tcp():
    try:
        connection, peer = server.accept()
        with connection:
            connection.settimeout(60)
            request = b''
            while len(request) < 4: request += connection.recv(4 - len(request))
            assert request == b'GET6', request
            block = bytes((i * 29 + 3) & 255 for i in range(1 << 20))
            connection.sendall(block)
            served.append(peer)
    except BaseException as error: errors.append(repr(error))
def serve_udp():
    while not stop.is_set():
        try: data, peer = udp.recvfrom(65535)
        except socket.timeout: continue
        except OSError: break
        udp.sendto(data, peer)
threads = [threading.Thread(target=serve_tcp, daemon=True), threading.Thread(target=serve_udp, daemon=True)]
for t in threads: t.start()
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
ports = WORK / 'ports'; ports.write_bytes(struct.pack('<HH', TCP_PORT, UDP_PORT))
spec = importlib.util.spec_from_file_location('net6_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
fatput.main(['fatput', str(disk), 'put', str(ports), 'nexus/net6-ports', 'put', str(WORK / 'net6-user.elf'), 'sbin/init'])
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True); pcap = WORK / 'network.pcap'
smp = sys.argv[2] if len(sys.argv) > 2 else '1'
try:
    result = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '256', '-smp', smp,
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', 'user,id=network,ipv4=on,ipv6=on,ipv6-net=fd42:6e65:7875::/64', '-device', 'e1000e,netdev=network,mac=52:54:00:12:34:56',
        '-object', f'filter-dump,id=capture,netdev=network,file={pcap}',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], capture_output=True, timeout=180)
    log = serial.read_text(errors='replace')
    for t in threads: t.join(timeout=2)
    assert result.returncode == 33 and '[64] INIT SESSION RETURNED' in log, (result.returncode, log[-6000:])
    for marker in ('NET6 SLAAC, TEMPORARY ADDRESS', 'NET6 LOOPBACK TCP 256 KIB', 'NET6 DUAL STACK', 'NET6 UDP: PKTINFO', 'NET6 LINK-LOCAL SCOPE',
                   'NET6 HOST TCP 1 MIB', 'NET6 PING SOCKET', 'NET6 GETIFADDRS', 'NET6 CAMPAIGN PASSED'):
        assert marker in log, (marker, log[-6000:])
    assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and '[64] FAIL:' not in log, log[-4000:]
    assert served and not errors, (served, errors)
    # Wire evidence: DAD probes from ::, a router solicitation, MLDv2 reports with Router Alert, our echo request
    capture = pcap.read_bytes(); offset = 24; dad = rs = mld = 0; fragments = []
    while offset + 16 <= len(capture):
        _, _, size, _ = struct.unpack_from('<IIII', capture, offset); frame = capture[offset + 16:offset + 16 + size]; offset += 16 + size
        if len(frame) < 54 or frame[12:14] != b'\x86\xdd' or frame[0:2] == b'\x52\x55': continue
        if frame[6:12] != bytes.fromhex('525400123456'): continue
        src = frame[22:38]; nxt = frame[20]; payload = frame[54:]
        if nxt == 44 and len(payload) >= 8:   # our fragment train of the 4000-byte datagram
            field = struct.unpack('!H', payload[2:4])[0]
            fragments.append((payload[4:8], field & 0xFFF8, field & 1, payload[0], payload[8:]))
        if nxt == 0 and len(payload) >= 8 and payload[2:4] == b'\x05\x02':   # hop-by-hop with Router Alert
            nxt = payload[0]; payload = payload[8:]
            if nxt == 58 and payload and payload[0] == 143: mld += 1
        elif nxt == 58 and payload:
            if payload[0] == 135 and src == bytes(16): dad += 1
            if payload[0] == 133: rs += 1
    assert dad >= 2 and rs >= 1 and mld >= 2, ('wire', dad, rs, mld)
    train = [f for f in fragments if f[0] == fragments[0][0]] if fragments else []
    assert len(train) == 3 and train[-1][2] == 0 and all(f[2] for f in train[:-1]) and all(f[3] == 17 for f in train), train
    whole = b''.join(f[4] for f in sorted(train, key=lambda f: f[1]))
    assert all(len(f[4]) % 8 == 0 for f in train[:-1]) and len(whole) == 4008 and whole[8:] == bytes((i * 5 + 1) & 255 for i in range(4000)), len(whole)
finally:
    stop.set(); server.close(); udp.close()
print(f'PASS native IPv6 ({smp} CPU): SLAAC + temporary address, AF_INET6 TCP/UDP loopback and through QEMU to host [::1], dual stack, '
      f'cmsgs, fragmentation, link-local scope, ping/raw ICMPv6, glibc/netlink views; wire: {dad} DAD probes, {rs} RS, {mld} MLDv2 reports, '
      f'a {len(train)}-fragment 4000-byte datagram')
