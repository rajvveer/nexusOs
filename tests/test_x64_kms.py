"""KMS modesetting campaign: boot the native kernel with a two-head virtio-vga and run /bin/kms64 (arch/x86_64/kms_user.c).

python -I tests/test_x64_kms.py [WORKDIR] [--smp N] [--whpx] [--gpu-pci]
The guest prints a READY marker per step and waits for a key; the host checks each head's screendump, hot-plugs head 1
(VNC SetDesktopSize 1024x768) and unplugs it (0x0), then the kernel checks that every framebuffer, dumb buffer and
descriptor was reclaimed. --gpu-pci uses virtio-gpu-pci without VGA (no firmware framebuffer: KMS only).
"""
from pathlib import Path
import shutil
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('kms')
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 1
if '--smp' in sys.argv: args = [a for a in args if a != str(SMP)]
WHPX = '--whpx' in sys.argv
PCI = '--gpu-pci' in sys.argv
WORK.mkdir(parents=True, exist_ok=True)
fatput = desk_vm.module('kms_fatput', ROOT / 'userland/fatput.py')
disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
marker = WORK / 'marker'; marker.write_bytes(b'pci' if PCI else b'1')   # kms_user: no firmware framebuffer without VGA
libdrm = OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libdrm.so.2.124.0'
fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-kms', 'put', str(libdrm), 'lib/x86_64-linux-gnu/libdrm.so.2'])


def near(a, b, tolerance=0):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def rgb(value): return ((value >> 16) & 255, (value >> 8) & 255, value & 255)


vm = desk_vm.DeskVm(WORK / 'vm', disk, heads=2, accel='whpx' if WHPX else 'tcg', smp=SMP, memory='512', raw_disk=True,
                    device='virtio-gpu-pci' if PCI else 'virtio-vga')
ok = False
try:
    v = vm
    m = v.wait(r'\[64\] KMS FRAME head=0 w=(\d+) h=(\d+)', seconds=240)
    w, h = int(m.group(1)), int(m.group(2))
    v.wait(r'KMS FRAME0 READY')
    p = v.frame('frame0', lambda p: p.size == (w, h) and p.getpixel((10, h // 2)) == rgb(0xC81E3C))
    assert p.getpixel((w - 10, h // 2)) == rgb(0x2878DC) and p.getpixel((130, 130)) == (255, 255, 255), 'head 0 pattern'
    after = len(v.text()); v.tap('a')
    v.wait(r'KMS DIRTY READY', after)
    p = v.frame('dirty', lambda p: p.getpixel((130, 130)) == (0, 255, 0))
    assert p.getpixel((620, 320)) == rgb(0x2878DC if 620 >= w // 2 else 0xC81E3C), 'a region outside every clip must not be presented'
    after = len(v.text()); v.tap('a')
    v.wait(r'KMS DIRTY2 READY', after)
    v.frame('dirty2', lambda p: p.getpixel((620, 320)) == (0, 0, 0))
    after = len(v.text()); v.tap('a')
    v.wait(r'KMS FLIP READY', after)
    p = v.frame('flip', lambda p: p.getpixel((10, 10)) == rgb(0xF0B428))
    assert p.getpixel((320, 220)) == rgb(0x102030), 'flipped buffer content'
    after = len(v.text()); v.tap('a')
    v.wait(r'KMS HOTPLUG READY', after, seconds=180)
    after = len(v.text()); v.head_size(1, 1024, 768); v.tap('a')
    m = v.wait(r'KMS FRAME head=1 w=(\d+) h=(\d+)', after)
    assert (int(m.group(1)), int(m.group(2))) == (1024, 768)
    v.wait(r'KMS FRAME1 READY', after)
    p = v.frame('frame1', lambda p: p.size == (1024, 768) and p.getpixel((10, 10)) == rgb(0x50C878), head=1)
    assert p.getpixel((240, 240)) == (255, 255, 255), 'head 1 pattern'
    head0 = v.shot('frame0-while-head1')
    assert head0.getpixel((10, h // 2)) == rgb(0xC81E3C) and head0.getpixel((130, 130)) == (0, 255, 0), 'head 0 (buffer A since SETPLANE) unchanged while head 1 is set'
    after = len(v.text()); v.tap('a')
    v.wait(r'KMS MIRROR READY', after)
    v.frame('mirror1', lambda p: p.getpixel((20, 20)) == (255, 0, 255) and p.getpixel((130, 130)) == (0, 255, 0), head=1)
    after = len(v.text()); v.tap('a')
    v.wait(r'KMS UNPLUG READY', after)
    after = len(v.text()); v.head_size(1, 0, 0); v.tap('a')
    v.wait(r'KMS RESTORE READY', after, seconds=180)
    if not PCI:   # the firmware framebuffer (kernel console) is shown again on head 0
        v.frame('restore', lambda p: p.getpixel((4, 4)) == (0x10, 0x15, 0x22))
    v.q.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': d, 'key': {'type': 'qcode', 'data': 'a'}}} for d in (True, False)]})
    v.proc.wait(120)   # the campaign ends right after this answer
    log = v.text()
    (WORK / 'serial.log').write_text(log, encoding='latin1')
    assert v.proc.returncode == 33, (v.proc.returncode, log[-4000:])
    for needed in ('[64] KMS ready heads=2', '[64] KMS ALL PASSED', 'KMS framebuffers, dumb buffers and descriptors reclaimed', '[64] KMS CAMPAIGN PASSED'):
        assert needed in log, (needed, log[-4000:])
    for bad in ('[64] FAIL', 'KERNEL_FAULT', 'USER_FAULT', 'KMS FAIL', 'VIRTIO-GPU error'):
        assert bad not in log, (bad, log[-4000:])
    passes = [line.split('PASS ', 1)[1] for line in log.splitlines() if line.startswith('[64] KMS PASS')]
    ok = True
    print('PASS KMS campaign (smp=%d%s): %s' % (SMP, ', virtio-gpu-pci' if PCI else '', '; '.join(passes)), flush=True)
finally:
    vm.close()
    if ok: disk.unlink(missing_ok=True)
