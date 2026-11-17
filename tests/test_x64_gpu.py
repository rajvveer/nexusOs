"""Boot the native kernel with a virgl-capable virtio-gpu and run the on-disk DRM render-node test.

python -I tests/test_x64_gpu.py [WORKDIR] [--whpx] [--vga] [--display=egl-headless]
Default device: -vga none -device virtio-gpu-gl-pci -display sdl,gl=on (TCG), started with SW_HIDE so
no window appears (this QEMU 9.1 Windows build crashes with egl-headless: it ships no EGL provider).
--vga uses virtio-vga-gl instead and also checks that the VBE framebuffer stays the visible scanout.
The campaign (/nexus/test-gpu) runs /bin/gpu64 (arch/x86_64/gpu_user.c) against the unmodified Debian
libdrm for discovery and the raw virtgpu uapi for rendering, then asserts every GPU object, mapping and
descriptor was reclaimed.
"""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('native64-gpu')
WHPX = '--whpx' in sys.argv
VGA = '--vga' in sys.argv
DISPLAY = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--display=')), 'sdl,gl=on')
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
spec = importlib.util.spec_from_file_location('qmp_local', ROOT / 'tests/qmp_local.py')
qmp_local = importlib.util.module_from_spec(spec); spec.loader.exec_module(qmp_local)

disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
libdrm = OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libdrm.so.2.124.0'
fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-gpu', 'put', str(libdrm), 'lib/x86_64-linux-gnu/libdrm.so.2'])
disk_hash = hashlib.sha256(disk.read_bytes()).digest()
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
control = qmp_local.QmpMonitor(WORK)
device = 'virtio-vga-gl' if VGA else 'virtio-gpu-gl-pci'
command = [str(QEMU), *(['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand'] if WHPX else ['-cpu', 'max']), '-m', '512',
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
           '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a',
           '-vga', 'none', '-device', device, '-display', DISPLAY,
           '-serial', f'file:{serial}', '-no-reboot', '-qmp', control.argument,
           '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', *(['-S'] if VGA else [])]
hidden = subprocess.STARTUPINFO(); hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW; hidden.wShowWindow = 0  # SW_HIDE: SDL windows stay hidden
p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, startupinfo=hidden)
try:
    if VGA:
        # Paused at start: connect first, then stop the guest right after the host GPU rendered.
        with control.open() as monitor:
            monitor.command('cont')
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and not (serial.exists() and '[64] GPU PASS virgl clear' in serial.read_text(errors='replace')):
                if p.poll() is not None: break
                time.sleep(.005)
            monitor.command('stop')
            ppm = WORK / 'vbe.ppm'; monitor.command('screendump', {'filename': str(ppm)})
            monitor.command('cont')
            from PIL import Image
            with Image.open(ppm) as image:
                image.save(WORK / 'vbe.png')
                assert image.size[0] >= 640 and image.getpixel((4, 4)) == (0x10, 0x15, 0x22), ('VBE console not presented', image.size, image.getpixel((4, 4)))
    output = p.communicate(timeout=240)[0]
except subprocess.TimeoutExpired:
    p.kill(); output = p.communicate()[0]
    raise AssertionError(('GPU campaign timed out', output, serial.read_text(errors='replace')))
finally:
    if p.poll() is None: p.kill(); p.wait()
    control.cleanup(p)
log = serial.read_text(errors='replace')
assert p.returncode == 33, (p.returncode, output, log)
for needed in ('[64] VIRTIO-GPU virgl ready', '[64] DRM render node', '[64] GPU ALL PASSED:', 'GPU objects, mappings and descriptors reclaimed',
               '[64] GPU CAMPAIGN PASSED'):
    assert needed in log, (needed, log)
assert '[64] FAIL' not in log and 'KERNEL_FAULT' not in log and 'GPU FAIL' not in log and 'USER_FAULT' not in log, log
assert 'VIRTIO-GPU error' not in log, log
assert hashlib.sha256(disk.read_bytes()).digest() == disk_hash, 'GPU test altered its disk'
disk.unlink(); (WORK / 'vbe.ppm').unlink(missing_ok=True)  # keep only logs and the PNG
print('PASS virtio-gpu virgl render node:', ', '.join(line.split('PASS ', 1)[1] for line in log.splitlines() if line.startswith('[64] GPU PASS')), flush=True)
