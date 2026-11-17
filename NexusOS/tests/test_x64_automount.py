"""Aurora removable media: USB drives are mounted for the desktop user, listed in the Files sidebar, unmounted/ejected.

    python -I tests/test_x64_automount.py WORK [--source DESKTOP64.img] [--qmp-port 4652] [--accel auto|whpx|tcg]
           [--smp 2]

--source (a published desktop64 image, default the fresh lead copy) is only read: out/desktop.elf (/sbin/init) and
out/aurora_app.elf (/bin/aurora-app) go into WORK/staged.qcow2, a few-MiB overlay on it (tests/overlay_stage.py),
and every boot adds its own throw-away overlay on top, on a PC with an xHCI controller. Two FAT sticks are built on the host (FAT32 "NEXUSUSB", FAT16 "PHOTOS").

boot 1 (hot-plug, QMP device_add/device_del usb-storage):
  - NEXUSUSB is plugged in: Aurora mounts /dev/sdX1 at /media/nexus/NEXUSUSB (vfat, nosuid,nodev, uid 1000);
  - Files (Spotlight) shows it in the sidebar; a click opens it (2 items); Eject unmounts it and stops the drive,
    which then leaves /dev (kernel "stopped by eject"); pulling it out afterwards gives no warning;
  - plugged in again it is mounted again; Unmount in the sidebar unmounts it (the drive stays);
  - PHOTOS is plugged in and pulled out while mounted: Aurora warns and lazily unmounts it;
boot 2 (coldplug): PHOTOS is attached at power-on and mounted when the desktop starts.
Every boot fails on a kernel or user-mode fault. Screenshots and serial logs stay in WORK/boot*/.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import re
import struct
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402

OUT = aurora_vm.OUT
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--qmp-port', type=int, default=4652)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--smp', type=int, default=2)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


fatimage = module('automount_fatimage', ROOT / 'tests/fatimage.py')


def mbr_stick(path, size, fat_type, label, files):
    img = fatimage.FatImage.create(size - 2048 * 512, fat_type, label, cluster_sectors=1 if fat_type == 32 else None)
    for name, data in files.items(): img.put(name, data)
    raw = img.finish()
    with open(path, 'wb') as f:
        f.truncate(size)
        mbr = bytearray(512)
        struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0x0C if fat_type == 32 else 0x0E, b'\xff\xff\xff', 2048, len(raw) // 512)
        mbr[440:444] = b'NXST'; mbr[510:512] = b'\x55\xaa'
        f.write(mbr); f.seek(2048 * 512); f.write(raw)


class MediaVm(aurora_vm.Vm):
    """aurora_vm.Vm plus an xHCI controller and optional cold-plugged USB sticks."""
    def __init__(self, work, disk, qmp_port, accel, smp, cold=()):
        extra = ['-device', 'qemu-xhci,id=xhci']
        for i, stick in enumerate(cold):
            extra += ['-drive', f'if=none,id=cold{i},format=raw,file={stick}', '-device', f'usb-storage,bus=xhci.0,drive=cold{i},id=coldusb{i}']
        real = subprocess.Popen

        def popen(command, *a, **k):
            if command and command[0] == aurora_vm.QEMU: command = command[:-1] + extra + command[-1:]
            if command and command[0] == aurora_vm.QEMU_IMG and str(disk).endswith('.qcow2'):   # the per-boot overlay sits on a qcow2
                command = [('qcow2' if c == 'raw' else c) for c in command]
            return real(command, *a, **k)
        aurora_vm.subprocess.Popen = popen
        try: super().__init__(work, disk, qmp_port, accel, smp)
        finally: aurora_vm.subprocess.Popen = real

    def plug(self, name, image):
        self.q.cmd('human-monitor-command', {'command-line': f'drive_add 0 if=none,id={name}d,format=raw,file={image}'})
        self.q.cmd('device_add', {'driver': 'usb-storage', 'bus': 'xhci.0', 'drive': name + 'd', 'id': name})

    def unplug(self, name):
        self.q.cmd('device_del', {'id': name})


results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok), detail)); print(('PASS ' if ok else 'FAIL ') + name + (' -- ' + str(detail) if detail and not ok else ''), flush=True)
    return ok


def files_window(vm, after):
    vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('files'); time.sleep(1); vm.tap('ret')
    vm.wait(r'AURORA LAUNCH app=files pid=\d+', after, 60)
    m = vm.wait(r'AURORA WINDOW app=([^ ]*) title=Files x=(-?\d+) y=(-?\d+)', after, 90)
    return int(m.group(2)) + 1, int(m.group(3)) + 28 + 1   # content origin (test_wine.py: titlebar 28, border 1)


def click(vm, x, y):
    vm.move(-2600, -1600, step=80, delay=.01); vm.pos = [0, 0]; time.sleep(.3)   # re-synchronise (Aurora clamps at 0,0)
    vm.move_to(x, y); time.sleep(.5); vm.click(); time.sleep(.5)


def boot_hotplug(disk, sticks):
    vm = MediaVm(WORK / 'boot1', disk, args.qmp_port, args.accel, args.smp)
    try:
        vm.wait(r'AURORA READY', seconds=300); check('listener', vm.wait(r'AURORA MEDIA listening', seconds=30))
        time.sleep(3)
        # 1. plug -> mounted -> Files sidebar -> open -> Eject
        mark = len(vm.text()); vm.plug('usb1', sticks['usb'])
        m = vm.wait(r'AURORA MEDIA MOUNTED source=(/dev/sd[a-z]+)1 point=(\S+) type=(\S+)', mark, 90)
        disk_node = m.group(1)
        check('hot-plugged FAT32 stick mounted at /media/nexus/NEXUSUSB (vfat)', m.group(2) == '/media/nexus/NEXUSUSB' and m.group(3) == 'vfat', m.group(0))
        log = vm.text()[mark:]
        check('kernel mount line is nosuid,nodev for the user', re.search(r'\[64\] MOUNT ' + disk_node + r'1 on /media/nexus/NEXUSUSB type vfat \(rw\)', log), log[-600:])
        time.sleep(1); vm.shot('notice')
        x, y = files_window(vm, mark); time.sleep(3); vm.shot('files')
        click(vm, x + 80, y + 400)   # focus click on an empty part of the sidebar
        click(vm, x + 40, y + 110 + 10)
        f = vm.wait(r'AURORA FILES FOLDER path=(\S+) items=(\d+)', mark, 30)
        check('sidebar opens the drive (HELLO.TXT, DATA)', f.group(1) == '/media/nexus/NEXUSUSB' and f.group(2) == '2', f.group(0))
        time.sleep(1); vm.shot('files-drive')
        mark = len(vm.text()); click(vm, x + 168 - 28, y + 110 + 31)
        e = vm.wait(r'AURORA FILES EJECT source=(\S+) disk=(\S+) result=(-?\d+)', mark, 30)
        check('Eject unmounts and stops the drive', e.group(1) == disk_node + '1' and e.group(2) == disk_node and e.group(3) == '0', e.group(0))
        check('kernel removed the ejected drive', vm.wait(r'stopped by eject', mark, 20))
        check('Files went back to Home', vm.wait(r'AURORA FILES FOLDER path=/home/nexus ', mark, 10))
        time.sleep(2); vm.shot('ejected')
        mark = len(vm.text()); vm.unplug('usb1'); time.sleep(5)
        check('pulling an ejected drive gives no warning', 'UNSAFE REMOVAL' not in vm.text()[mark:])
        # 2. plug again -> mounted again -> Unmount from the sidebar
        mark = len(vm.text()); vm.plug('usb2', sticks['usb'])
        m = vm.wait(r'AURORA MEDIA MOUNTED source=(/dev/sd[a-z]+)1 point=(\S+) type=vfat', mark, 90)
        check('re-plugged stick is mounted again', m.group(2) == '/media/nexus/NEXUSUSB', m.group(0))
        time.sleep(1.5); click(vm, x + 50, y + 110 + 32)
        u = vm.wait(r'AURORA FILES UNMOUNT source=(\S+) point=(\S+) result=(-?\d+)', mark, 30)
        check('Unmount from the sidebar', u.group(2) == '/media/nexus/NEXUSUSB' and u.group(3) == '0', u.group(0))
        vm.unplug('usb2'); time.sleep(3)
        # 3. surprise removal while mounted
        mark = len(vm.text()); vm.plug('usb3', sticks['photos'])
        m = vm.wait(r'AURORA MEDIA MOUNTED source=(/dev/sd[a-z]+1) point=(\S+) type=vfat', mark, 90)
        check('second stick (FAT16 PHOTOS) mounted', m.group(2) == '/media/nexus/PHOTOS', m.group(0))
        time.sleep(1); mark2 = len(vm.text()); vm.unplug('usb3')
        r = vm.wait(r'AURORA MEDIA UNSAFE REMOVAL source=(\S+) point=(\S+) detach=(-?\d+)', mark2, 30)
        check('surprise removal while mounted: warning + lazy unmount', r.group(1) == m.group(1) and r.group(3) == '0', r.group(0))
        time.sleep(2); vm.shot('unsafe')
        faults = vm.faults(); check('boot 1: no kernel or user-mode fault', not faults, faults[:5])
    finally:
        vm.close()


def boot_coldplug(disk, sticks):
    vm = MediaVm(WORK / 'boot2', disk, args.qmp_port, args.accel, args.smp, cold=[sticks['photos']])
    try:
        vm.wait(r'AURORA READY', seconds=300)
        m = vm.wait(r'AURORA MEDIA MOUNTED source=(/dev/sd[a-z]+1) point=(\S+) type=(\S+)', seconds=60)
        check('stick attached at power-on is mounted when the desktop starts', m.group(2) == '/media/nexus/PHOTOS', m.group(0))
        faults = vm.faults(); check('boot 2: no kernel or user-mode fault', not faults, faults[:5])
    finally:
        vm.close()


import overlay_stage  # noqa: E402
disk = WORK / 'staged.qcow2'
try:
    staged = overlay_stage.stage(args.source, disk, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n'})
    print(f'staged {staged >> 10} KiB into {disk.name} over {args.source}', flush=True)
    sticks = {'usb': WORK / 'nexususb.img', 'photos': WORK / 'photos.img'}
    mbr_stick(sticks['usb'], 64 * 1024 * 1024, 32, 'NEXUSUSB', {'HELLO.TXT': b'hello from a USB stick\n', 'DATA/README.TXT': b'nested\n'})
    mbr_stick(sticks['photos'], 24 * 1024 * 1024, 16, 'PHOTOS', {'IMG_0001.JPG': bytes(range(256)) * 64})
    boot_hotplug(disk, sticks)
    boot_coldplug(disk, sticks)
except Exception as error:
    check('scenario completed', False, repr(error))
finally:
    disk.unlink(missing_ok=True)
    for name in ('nexususb.img', 'photos.img'): (WORK / name).unlink(missing_ok=True)
failed = [r for r in results if not r[1]]
(WORK / 'result.json').write_text(json.dumps({'passed': not failed, 'checks': [{'name': n, 'ok': o, 'detail': str(d)} for n, o, d in results]}, indent=1))
print(('PASS' if not failed else 'FAIL') + ' Aurora automount: %d checks, %d failed' % (len(results), len(failed)), flush=True)
sys.exit(1 if failed else 0)
