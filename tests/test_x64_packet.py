"""AF_PACKET sockets: the kernel campaign /nexus/test-packet runs arch/x86_64/packet_user.c (raw/cooked capture of a
ping to QEMU's gateway, ARP sent from packet sockets, BPF filters and checker, memberships, TPACKET_V2/V3 rings,
statistics, errors) and checks that sockets, rings and frames are reclaimed; the wire capture must show the ARP
requests the program built. Usage: test_x64_packet.py [work dir] [cpus]. The disk is a private copy."""
from pathlib import Path
import importlib.util
import shutil
import struct
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-packet')
SMP = sys.argv[2] if len(sys.argv) > 2 else '1'
WORK.mkdir(parents=True, exist_ok=True)
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/packet_user.c'), '-o', str(WORK / 'packet-user.elf')], check=True)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
spec = importlib.util.spec_from_file_location('packet_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
fatput.main(['fatput', str(disk), 'put', str(WORK / 'packet-user.elf'), 'bin/packet64', 'put', str(marker), 'nexus/test-packet'])
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True); pcap = WORK / 'network.pcap'; pcap.unlink(missing_ok=True)
try:
    result = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '256', '-smp', SMP,
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network,mac=52:54:00:12:34:56',
        '-object', f'filter-dump,id=capture,netdev=network,file={pcap}',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], capture_output=True, timeout=240)
    log = serial.read_text(errors='replace')
    assert result.returncode == 33 and '[64] PACKET CAMPAIGN PASSED' in log, (result.returncode, log[-6000:])
    for marker_text in ('PACKET RAW CAPTURE', 'PACKET COOKED', 'PACKET ARP', 'PACKET BPF', 'PACKET PROMISC', 'PACKET TPACKET_V2', 'PACKET TPACKET_V3'):
        assert marker_text in log, (marker_text, log[-4000:])
    assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and '[64] FAIL' not in log, log[-4000:]
    capture = pcap.read_bytes(); offset = 24; arp_requests = 0
    while offset + 16 <= len(capture):
        _, _, size, _ = struct.unpack_from('<IIII', capture, offset); frame = capture[offset + 16:offset + 16 + size]; offset += 16 + size
        if len(frame) >= 42 and frame[12:14] == b'\x08\x06' and frame[20:22] == b'\x00\x01' and frame[6:12] == bytes.fromhex('525400123456') \
           and frame[38:42] == bytes([10, 0, 2, 2]): arp_requests += 1
    assert arp_requests >= 2, ('ARP requests on the wire', arp_requests)
finally:
    disk.unlink(missing_ok=True)
v3 = [l for l in log.splitlines() if 'PACKET TPACKET_V3' in l][0].split('V3: ')[1]
print(f'PASS native AF_PACKET ({SMP} CPU): raw/cooked capture, ARP from packet sockets ({arp_requests} on the wire), BPF + checker, '
      f'PACKET_AUXDATA, promiscuous memberships, TPACKET_V2, TPACKET_V3 ({v3}); sockets, rings and frames reclaimed')
