"""Firmware framebuffers beyond 1920x1080 XRGB, end to end on QEMU (native x86-64).

    python -I tests/test_x64_display_modes.py WORK [--only CASE,...] [--source DESKTOP64.img] [--accel auto|whpx|tcg]
                                              [--qmp-port 4682]

Kernel cases (no disk; kernel self-tests must still pass):
  uefi_1440   OVMF + VGA, loader option video=2560x1440: GOP mode set, pitch 10240, console moved to a PAT write-combining
              mapping (the framebuffer's MTRR type is UC under OVMF, as on PCs)
  uefi_4k     video=3840x2160: the console clears the whole 33 MB framebuffer (bottom-right pixel checked by screendump)
  bios_native SeaBIOS VBE/DDC EDID (2560x1440) used for the mode: the stage-2 test byte after 'NXVE' is set in a copy of
              nexus64.img so QEMU's "RHT" EDID counts as a real monitor's
  bios_bgr    fb.test-bgr: the kernel treats the framebuffer as XBGR8888; console colours come out swapped
Aurora cases (a private sparse copy of desktop64.img with out/desktop.elf staged as /sbin/init, booted via an overlay):
  aurora_1440   UEFI 2560x1440: Aurora covers the whole screen (no console pixels outside 1920x1080)
  aurora_bgr    BIOS 1920x1080 with and without fb.test-bgr: the BGR screen is the XRGB one with red/blue swapped
  aurora_stride BIOS 1920x1080 with fb.test-width=1800 (pitch 7680 > 1800*4): Aurora draws 1800 columns, unsheared
"""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
QEMU = Path(aurora_vm.QEMU)
SHARE = QEMU.parent / 'share'
KERNEL_CASES = ['uefi_1440', 'uefi_4k', 'bios_native', 'bios_bgr']
AURORA_CASES = ['aurora_1440', 'aurora_bgr', 'aurora_stride']
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--only', default=','.join(KERNEL_CASES + AURORA_CASES))
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4682)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.only.split(',') if c]
assert all(c in KERNEL_CASES + AURORA_CASES for c in cases), cases
CONSOLE_BG = (0x10, 0x15, 0x22)
results = {}


def esp_image(name, cmdline):
    image = WORK / name
    subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(OUT / 'BOOTX64.EFI'), str(OUT / 'kernel.bin'), str(image),
                    '--cmdline', cmdline], check=True, stdout=subprocess.DEVNULL)
    return image


def floppy(vm_edid):
    if not vm_edid: return OUT / 'nexus64.img'
    data = bytearray((OUT / 'nexus64.img').read_bytes()); at = data.find(b'NXVE', 512, 2560)
    assert at > 0 and data[at + 4] == 0, 'stage-2 EDID test byte'
    data[at + 4] = 1; path = WORK / 'floppy-vm-edid.img'; path.write_bytes(data); return path


class Machine:
    def __init__(self, tag, *, uefi=None, bios=None, device, kernel_cmdline=None, disk=None, accel='tcg', memory='1024'):
        self.tag, self.serial = tag, WORK / (tag + '.log'); self.serial.unlink(missing_ok=True)
        acceleration = ['-accel', 'tcg', '-cpu', 'max'] if accel == 'tcg' else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', aurora_vm.CPU]
        command = [str(QEMU), '-machine', 'pc', *acceleration, '-smp', '2', '-m', memory, '-display', 'none', '-vga', 'none',
                   '-device', device, '-serial', f'file:{self.serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off',
                   '-action', 'shutdown=pause']   # no isa-debug-exit: the finished kernel powers off into a paused VM that still has its screen
        if uefi:
            code, variables = WORK / 'code.fd', WORK / (tag + '-vars.fd')
            if not code.exists(): shutil.copyfile(SHARE / 'edk2-x86_64-code.fd', code)
            shutil.copyfile(SHARE / 'edk2-i386-vars.fd', variables)
            if accel == 'tcg':
                command += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={code}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
            else:   # WHPX cannot emulate OVMF's pflash MMIO: the same EDK2 build as one ROM (as tests/test_x64_uefi.py)
                unified = WORK / (tag + '-ovmf.fd'); unified.write_bytes(variables.read_bytes() + code.read_bytes())
                command += ['-bios', str(unified)]
            command += ['-drive', f'if=none,id=esp,format=raw,snapshot=on,file={uefi}', '-device', 'ide-hd,drive=esp,bus=ide.1,bootindex=0']
        else:
            command += ['-drive', f'file={bios},format=raw,if=floppy,readonly=on', '-boot', 'a']
        self.overlay = None
        if disk:
            self.overlay = WORK / (tag + '.qcow2'); self.overlay.unlink(missing_ok=True)
            subprocess.run([aurora_vm.QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(disk), '-F', 'raw', str(self.overlay)], check=True)
            command += ['-drive', f'file={self.overlay},format=qcow2,if=none,id=root', '-device', 'ide-hd,drive=root,bus=ide.0',
                        '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network']
        if kernel_cmdline: command += ['-fw_cfg', f'name=opt/nexus/cmdline,string={kernel_cmdline}']
        (WORK / (tag + '.cmd.json')).write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, stdout=(WORK / (tag + '.qemu.log')).open('wb'), stderr=subprocess.STDOUT)
        print(' ', tag, 'pid', self.proc.pid, accel, flush=True)
        if accel != 'tcg':
            try: nexus_paths.unthrottle(self.proc.pid)
            except Exception: pass
        self.q = aurora_vm.Qmp(args.qmp_port)

    def text(self): return self.serial.read_bytes().decode('latin1') if self.serial.exists() else ''

    def wait(self, marker, seconds=240):
        end = time.time() + seconds
        while time.time() < end:
            text = self.text()
            if marker in text: return text
            if '[64] FAIL' in text or 'KERNEL_FAULT' in text: raise AssertionError((self.tag, text[-3000:]))
            if self.proc.poll() is not None: raise AssertionError((self.tag, 'VM exited before ' + marker, text[-3000:]))
            time.sleep(.25)
        raise TimeoutError((self.tag, marker, self.text()[-3000:]))

    def shot(self, label):
        from PIL import Image
        ppm = WORK / f'{self.tag}-{label}.ppm'
        self.q.cmd('screendump', {'filename': str(ppm)})
        with Image.open(ppm) as image: picture = image.convert('RGB')
        picture.save(WORK / f'{self.tag}-{label}.png'); ppm.unlink(); return picture

    def close(self):
        if self.proc.poll() is None:
            try: self.q.cmd('quit')
            except Exception: pass
            try: self.proc.wait(30)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()
        if self.overlay: self.overlay.unlink(missing_ok=True)


def run_kernel_case(case):
    if case == 'uefi_1440':
        m = Machine(case, uefi=esp_image('esp-1440.img', 'video=2560x1440'), device='VGA,vgamem_mb=64')
        try:
            text = m.wait('[64] ALL TESTS PASSED', 600)
            assert 'GOP: 2560x1440 pitch=10240' in text and '[video= option]' in text, text[:3000]
            assert '[64] DISPLAY 2560x1440 stride=10240 XRGB8888' in text and 'console=WC (PAT)' in text and 'mode=video=' in text, text[:3000]
            return {'mtrr': text.split('MTRR=')[1].split()[0]}
        finally: m.close()
    if case == 'uefi_4k':
        m = Machine(case, uefi=esp_image('esp-4k.img', 'video=3840x2160'), device='VGA,vgamem_mb=64')
        try:
            text = m.wait('[64] ALL TESTS PASSED', 600); time.sleep(1); picture = m.shot('console')
            assert picture.size == (3840, 2160) and picture.getpixel((3839, 2159)) == CONSOLE_BG and picture.getpixel((0, 0)) == CONSOLE_BG, picture.size
            assert 'GOP: 3840x2160 pitch=15360' in text, text[:3000]
            return {'size': picture.size}
        finally: m.close()
    if case == 'bios_native':
        m = Machine(case, bios=floppy(True), device='VGA,vgamem_mb=64,edid=on,xres=2560,yres=1440')
        try:
            text = m.wait('[64] DISPLAY', 240)
            assert 'fb=2560x1440' in text and '[64] DISPLAY 2560x1440 stride=10240' in text, text[:3000]
            assert 'EDID=VBE-DDC RHT preferred 2560x1440 (virtual display) mode=native' in text, text[:3000]
            return {'edid': 'VBE-DDC'}
        finally: m.close()
    if case == 'bios_bgr':
        m = Machine(case, bios=floppy(False), device='VGA,vgamem_mb=64,edid=on,xres=2560,yres=1440', kernel_cmdline='fb.test-bgr')
        try:
            text = m.wait('[64] ALL TESTS PASSED', 600); time.sleep(1); picture = m.shot('console')
            assert '[64] DISPLAY 1920x1080 stride=7680 XBGR8888' in text and 'EDID=VBE-DDC RHT preferred 2560x1440 (virtual display) mode=default' in text, text[:3000]
            assert picture.getpixel((2, 2)) == CONSOLE_BG[::-1], picture.getpixel((2, 2))
            return {'pixel': picture.getpixel((2, 2))}
        finally: m.close()


def aurora_disk():
    disk = WORK / 'desktop.img'
    if disk.exists(): return disk
    aurora_vm.sparse_copy(args.source, disk)
    aurora_vm.stage(disk, {'sbin/init': OUT / 'desktop.elf', 'nexus/desktop.args': '--aurora\n'})
    return disk


def boot_aurora(tag, accel, **options):
    m = Machine(tag, disk=aurora_disk(), accel=accel, memory='2048', **options)
    try:
        m.wait('AURORA READY persistent desktop', 300); time.sleep(4)
        picture = m.shot('desktop')
        assert not [l for l in m.text().splitlines() if 'KERNEL_FAULT' in l or 'USER_FAULT' in l], m.text()[-3000:]
        return picture, m.text()
    finally: m.close()


def run_aurora_case(case, accel):
    if case == 'aurora_1440':
        picture, text = boot_aurora(case, accel, uefi=esp_image('esp-aurora.img', 'video=2560x1440'), device='VGA,vgamem_mb=64')
        assert picture.size == (2560, 1440) and '[64] DISPLAY 2560x1440' in text
        outside = [picture.getpixel((x, y)) for x in range(0, 2560, 9) for y in range(0, 1440, 9) if x >= 1920 or y >= 1080]
        console = sum(p == CONSOLE_BG for p in outside)
        assert console == 0, ('console pixels outside 1920x1080', console, len(outside))
        assert picture.getpixel((2555, 4)) == picture.getpixel((1280, 4)) or picture.getpixel((2555, 4)) != CONSOLE_BG
        # the full wallpaper (hills), not the plain-gradient fallback Aurora used above 8 MiB / 2048 columns
        row = [picture.getpixel((x, 900)) for x in range(0, 900, 3)]
        spread = max(max(p[c] for p in row) - min(p[c] for p in row) for c in range(3))
        assert spread > 30, ('wallpaper row is a flat gradient', spread)
        return {'size': picture.size, 'sampled_outside_1080p': len(outside), 'wallpaper_row_spread': spread}
    if case == 'aurora_bgr':
        device = 'VGA,vgamem_mb=64'
        normal, _ = boot_aurora(case + '-xrgb', accel, bios=floppy(False), device=device)
        swapped, text = boot_aurora(case + '-xbgr', accel, bios=floppy(False), device=device, kernel_cmdline='fb.test-bgr')
        assert '[64] DISPLAY 1920x1080 stride=7680 XBGR8888' in text and normal.size == swapped.size == (1920, 1080)
        same = total = 0
        for y in range(200, 1000, 3):
            for x in range(0, 1920, 3):
                r, g, b = swapped.getpixel((x, y)); total += 1; same += normal.getpixel((x, y)) == (b, g, r)
        assert same >= total * 0.99, ('BGR screen is not the swapped XRGB screen', same, total)
        assert normal.getpixel((960, 900)) != swapped.getpixel((960, 900)) or normal.getpixel((960, 900))[0] == normal.getpixel((960, 900))[2]
        return {'matching': round(same / total, 4)}
    if case == 'aurora_stride':
        picture, text = boot_aurora(case, accel, bios=floppy(False), device='VGA,vgamem_mb=64', kernel_cmdline='fb.test-width=1800')
        assert '[64] DISPLAY 1800x1080 stride=7680' in text and picture.size == (1920, 1080), text[-2000:]
        strip = [picture.getpixel((x, y)) for x in range(1800, 1920, 4) for y in range(0, 1080, 5)]
        foreign = [p for p in strip if p not in (CONSOLE_BG, (0xD8, 0xE8, 0xFF), (0, 0, 0))]   # padding: never drawn, or console
        assert not foreign, ('Aurora drew past its 1800 columns (stride handling)', len(foreign), foreign[:5])
        inside = [picture.getpixel((x, y)) for x in range(1700, 1800, 4) for y in range(40, 1080, 5)]
        assert sum(p == CONSOLE_BG for p in inside) < len(inside) * 0.01, 'Aurora did not cover its right edge'
        return {'strip_samples': len(strip)}


accel = args.accel
if accel == 'auto': accel = 'whpx' if aurora_vm.whpx_usable() else 'tcg'
failed = []
try:
    for case in cases:
        start = time.time()
        try:
            detail = run_kernel_case(case) if case in KERNEL_CASES else run_aurora_case(case, accel)
            results[case] = {'passed': True, 'seconds': round(time.time() - start), **(detail or {})}
            print('PASS', case, results[case], flush=True)
        except Exception as error:
            results[case] = {'passed': False, 'error': repr(error)[:2000]}; failed.append(case)
            print('FAIL', case, repr(error)[:3000], flush=True)
finally:
    (WORK / 'desktop.img').unlink(missing_ok=True)
    for pattern in ('esp-*.img', 'floppy-*.img', '*.qcow2', '*-vars.fd', '*-ovmf.fd', 'code.fd'):
        for path in WORK.glob(pattern): path.unlink(missing_ok=True)
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
assert not failed, failed
print('PASS firmware framebuffer modes:', ' '.join(cases))
