"""TUN/TAP virtual interfaces (/dev/net/tun, tun.c): the kernel campaign /nexus/test-tun runs arch/x86_64/tun_user.c,
which opens /dev/net/tun, builds IFF_TUN and IFF_TAP interfaces, addresses them and proves the data path both ways
(a routed datagram read from the device fd, a packet written in delivered to a socket), the packet-information header,
TUNGETIFF/TUNGETFEATURES, and TUNSETPERSIST/TUNSETOWNER. No external network is needed. Usage:
test_x64_tun.py [work dir] [cpus]. The disk is a private copy."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-tun')
SMP = sys.argv[2] if len(sys.argv) > 2 else '1'
WORK.mkdir(parents=True, exist_ok=True)
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/tun_user.c'), '-o', str(WORK / 'tun-user.elf')], check=True)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
spec = importlib.util.spec_from_file_location('tun_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
fatput.main(['fatput', str(disk), 'put', str(WORK / 'tun-user.elf'), 'bin/tun64', 'put', str(marker), 'nexus/test-tun'])
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
try:
    result = subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '256', '-smp', SMP,
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network,mac=52:54:00:12:34:56',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], capture_output=True, timeout=180)
    log = serial.read_text(errors='replace')
    assert result.returncode == 33 and '[64] TUN CAMPAIGN PASSED' in log, (result.returncode, log[-6000:])
    for m in ('TUN FEATURES', 'TUN ROUNDTRIP', 'TUN PI', 'TAP:', 'TUN PERSIST'):
        assert m in log, (m, log[-4000:])
    assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and '[64] FAIL' not in log, log[-4000:]
finally:
    disk.unlink(missing_ok=True)
summary = [l for l in log.splitlines() if l.startswith('[64] TUN') or l.startswith('[64] TAP')]
print(f'PASS native TUN/TAP ({SMP} CPU): ' + '; '.join(s.split('] ', 1)[1] for s in summary if 'CAMPAIGN' not in s))
