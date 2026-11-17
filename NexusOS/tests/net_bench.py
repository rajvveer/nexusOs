"""TCP throughput of the native stack through QEMU user networking, without a
browser or disk load in the guest: a glibc client (arch/x86_64/netbench_user.c)
downloads and uploads patterned data from/to a host peer, verifying it.

Usage: python -I tests/net_bench.py [WORK] [--kernel nexus64.img] [--mib 64] [--rounds 3] [--tcg]
Uses WHPX when available (like the desktop), so numbers are comparable to Chrome runs.
"""
from pathlib import Path
import argparse
import importlib.util
import re
import shutil
import socket
import struct
import subprocess
import threading
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
parser = argparse.ArgumentParser()
parser.add_argument('work', nargs='?', default=str(nexus_paths.runs('native64-netbench')))
parser.add_argument('--kernel', default=str(OUT / 'nexus64.img'))
parser.add_argument('--mib', type=int, default=64)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--tcg', action='store_true')
parser.add_argument('--nic', default='rtl8139', help='QEMU NIC model: rtl8139, e1000, e1000e or igb')
options = parser.parse_args()
WORK = Path(options.work); WORK.mkdir(parents=True, exist_ok=True)
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36',
    '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/netbench_user.c'),
    '-o', str(WORK / 'netbench.elf')], check=True)

BLOCK = bytes((i * 37 + 11) & 255 for i in range(1 << 20))
server = socket.socket(); server.bind(('127.0.0.1', 0)); server.listen(8); server.settimeout(300)
port = server.getsockname()[1]
errors, host = [], []
def exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part: raise AssertionError('peer closed early')
        data.extend(part)
    return bytes(data)
def serve():
    try:
        for _ in range(2 * options.rounds):
            connection, _ = server.accept()
            with connection:
                connection.settimeout(300)
                request = exact(connection, 5); kind = request[:1]; mib = struct.unpack('<I', request[1:])[0]
                start = time.time()
                if kind == b'D':
                    for _ in range(mib): connection.sendall(BLOCK)
                    connection.shutdown(socket.SHUT_WR)
                    connection.recv(1)
                else:
                    total, good = 0, True
                    while True:
                        part = connection.recv(1 << 19)
                        if not part: break
                        if part != BLOCK[total & 255:(total & 255) + len(part)]: good = False
                        total += len(part)
                    assert total == mib << 20 and good, (total, good)
                    connection.sendall(b'OK')
                host.append((kind.decode(), mib, round(time.time() - start, 3)))
    except BaseException as error:
        errors.append(repr(error))
thread = threading.Thread(target=serve, daemon=True); thread.start()

disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
config = WORK / 'netbench'; config.write_bytes(struct.pack('<HHH', port, options.mib, options.rounds))
spec = importlib.util.spec_from_file_location('netbench_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
fatput.main(['fatput', str(disk), 'put', str(config), 'nexus/netbench', 'put', str(WORK / 'netbench.elf'), 'sbin/init'])
spec = importlib.util.spec_from_file_location('netbench_build', ROOT / 'arch/x86_64/build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
cpu = build.desktop_cpu_options(options.tcg)
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
try:
    started = time.time()
    result = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', *cpu, '-m', '512',
        '-drive', f'file={options.kernel},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', 'user,id=network', '-device', f'{options.nic},netdev=network',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], capture_output=True, timeout=900)
    log = serial.read_text(errors='replace'); thread.join(timeout=5)
    # INIT SESSION RETURNED comes after the kernel's process-ownership (leak) check.
    assert '[64] NETBENCH DONE' in log and '[64] INIT SESSION RETURNED' in log and 'KERNEL_FAULT' not in log and not errors, \
        (result.returncode, errors, log[-3000:])
finally:
    server.close()
rates = {kind: [float(m.group(2)) for m in re.finditer(r'NETBENCH %s round=\d+ MiB=\d+ seconds=([\d.]+) MBps=([\d.]+)' % kind, log)]
         for kind in ('download', 'upload')}
print('accelerator:', ' '.join(cpu), '| nic:', options.nic, '| kernel:', options.kernel, '| wall %.1fs' % (time.time() - started))
for kind, values in rates.items():
    print('%-8s MB/s per round:' % kind, values, '| best %.2f' % max(values))
print('host side (kind, MiB, seconds):', host)
