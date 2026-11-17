"""Actual glibc socket syscalls against a host peer; all VM disks are private."""
from pathlib import Path
import hashlib
import importlib.util
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-inet')
WORK.mkdir(parents=True, exist_ok=True)
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36',
    '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
    str(ROOT / 'arch/x86_64/inet_user.c'), '-o', str(WORK / 'inet-user.elf')], check=True)
server = socket.socket(); server.bind(('127.0.0.1', 0)); server.listen(8); server.settimeout(100)
refused = socket.socket(); refused.bind(('127.0.0.1', 0)); refused_port = refused.getsockname()[1]
errors = []; completed = []
def exact(connection, size):
    result = bytearray()
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            raise AssertionError(f'peer EOF at {len(result)} of {size}')
        result.extend(part)
    return bytes(result)
def serve():
    try:
        for mode in (b'S', b'W', b'I', b'R'):
            connection, _ = server.accept()
            with connection:
                connection.settimeout(60)
                assert exact(connection, 1) == mode
                if mode == b'S':
                    connection.sendall(b'peek-data'); assert exact(connection, 2) == b'GO'
                    block = bytes((i * 37 + 11) & 255 for i in range(65536))
                    for _ in range(32):
                        assert exact(connection, len(block)) == block, 'upload mismatch'
                    for _ in range(32):
                        connection.sendall(block)
                    connection.shutdown(socket.SHUT_WR)
                    assert exact(connection, 4) == b'TAIL' and connection.recv(1) == b''
                elif mode == b'W':
                    time.sleep(.12); connection.sendall(b'wai'); time.sleep(.12); connection.sendall(b't-all')
                    connection.shutdown(socket.SHUT_WR); assert connection.recv(1) == b''
                elif mode == b'I':
                    assert exact(connection, 1) == b'D'
                    refused.close()
                    connection.sendall(b'discard-after-shutdown' * 2000)
                    connection.shutdown(socket.SHUT_WR); assert connection.recv(1) == b''
                else:
                    connection.sendall(b'reset'); time.sleep(.15)
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('HH' if sys.platform == 'win32' else 'ii', 1, 0))
                completed.append(mode)
                print('host completed', mode, flush=True)
    except BaseException as error:
        errors.append(repr(error))
thread = threading.Thread(target=serve, daemon=True); thread.start()
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
ports = WORK / 'ports'; ports.write_bytes(struct.pack('<HH', server.getsockname()[1], refused_port))
spec = importlib.util.spec_from_file_location('inet_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
fatput.main(['fatput', str(disk), 'put', str(ports), 'nexus/inet-ports', 'put', str(WORK / 'inet-user.elf'), 'sbin/init'])
before = hashlib.sha256(disk.read_bytes()).digest()
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
try:
    result = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '128',
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network',
        '-object', f'filter-dump,id=capture,netdev=network,file={WORK / "network.pcap"}',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], capture_output=True, timeout=110)
    log = serial.read_text(); thread.join(timeout=2)
    assert result.returncode == 33 and '[64] INIT SESSION RETURNED' in log, (result.stdout, result.stderr, log)
    for marker in ('INET 2 MIB UPLOAD/DOWNLOAD, EPOLL, ALIASES AND HALF-CLOSE PASSED',
                   'INET WAITALL, TIMEOUT, SIGNAL AND SHUTDOWN WAKE PASSED', 'INET CONNECT AND RESET ERRORS PASSED', 'INET SOCKET CAMPAIGN PASSED'):
        assert marker in log, (marker, log)
    assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and '[64] FAIL:' not in log, log
    assert completed == [b'S', b'W', b'I', b'R'] and not errors, (completed, errors)
    # Windows may silently drop a closed host port; compare the errno with actual wire evidence.
    capture = (WORK / 'network.pcap').read_bytes(); offset = 24; resets = 0; syns = 0
    while offset < len(capture):
        _, _, size, _ = struct.unpack_from('<IIII', capture, offset)
        packet = capture[offset + 16:offset + 16 + size]; offset += 16 + size
        if len(packet) < 54 or packet[12:14] != b'\x08\x00' or packet[23] != 6: continue
        tcp = 14 + (packet[14] & 15) * 4
        source, destination = struct.unpack_from('!HH', packet, tcp)
        if source == refused_port and packet[tcp + 13] & 4: resets += 1
        if destination == refused_port and packet[tcp + 13] & 2: syns += 1
    error = int(re.search(r'INET CONNECT ERROR (\d+)', log).group(1))
    assert error == (111 if resets else 110) and (resets or syns >= 4), (error, resets, syns)
    assert hashlib.sha256(disk.read_bytes()).digest() == before, 'socket campaign changed the application disk'
finally:
    server.close(); refused.close()
print('PASS native AF_INET: glibc, exact 2 MiB each way, epoll, nonblocking/vector/peek I/O, timeout/signal/shutdown, FIN/RST/wire-checked connection error and descriptor ownership')
