"""VMware Workstation harness for NexusOS tests: writes a .vmx, attaches raw images through flat VMDK
descriptors (independent-nonpersistent unless asked otherwise, so VMware never writes the image file),
runs the VM headless with vmrun, reads the serial log and drives/captures the screen over VMware's
built-in VNC server."""
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time

WORKSTATION = Path(r'C:\Program Files (x86)\VMware\VMware Workstation')
VMRUN = WORKSTATION / 'vmrun.exe'


def available():
    return VMRUN.is_file()


def descriptor(raw, path):
    size = Path(raw).stat().st_size
    assert size % 512 == 0, raw
    sectors = size // 512
    Path(path).write_text('# Disk DescriptorFile\nversion=1\nencoding="UTF-8"\nCID=fffffffe\nparentCID=ffffffff\n'
                          'createType="monolithicFlat"\n\n# Extent description\n'
                          f'RW {sectors} FLAT "{Path(raw).resolve()}" 0\n\n# The Disk Data Base\n#DDB\n\n'
                          f'ddb.adapterType = "lsilogic"\nddb.geometry.cylinders = "{max(1, min(65535, sectors // (255 * 63)))}"\n'
                          'ddb.geometry.heads = "255"\nddb.geometry.sectors = "63"\nddb.virtualHWVersion = "21"\n')


class VM:
    def __init__(self, work, name, out, firmware='bios', boot='floppy', root=None, root_bus='nvme', nic='e1000e',
                 network='hostonly', cpus=2, memory=2048, usb=True, vnc_port=5971, options=None):
        self.name, self.dir = name, Path(work) / name
        if self.dir.exists(): shutil.rmtree(self.dir)
        self.dir.mkdir(parents=True)
        self.vmx, self.serial, self.vnc_port = self.dir / f'{name}.vmx', self.dir / 'serial.log', vnc_port
        v = {'.encoding': 'UTF-8', 'config.version': '8', 'virtualHW.version': '21', 'displayName': f'nexus-test-{name}',
             'guestOS': 'other-64', 'firmware': firmware, 'memsize': str(memory), 'numvcpus': str(cpus),
             'cpuid.coresPerSocket': str(cpus), 'msg.autoAnswer': 'TRUE', 'disk.locking': 'FALSE',
             'pciBridge0.present': 'TRUE', 'vmci0.present': 'TRUE', 'hpet0.present': 'TRUE', 'tools.syncTime': 'FALSE',
             'svga.present': 'TRUE', 'svga.vramSize': str(256 * 1024 * 1024), 'mks.enable3d': 'FALSE', 'sound.present': 'FALSE',
             'serial0.present': 'TRUE', 'serial0.fileType': 'file', 'serial0.fileName': 'serial.log',
             'serial0.yieldOnMsrRead': 'TRUE', 'RemoteDisplay.vnc.enabled': 'TRUE', 'RemoteDisplay.vnc.port': str(vnc_port),
             'floppy0.present': 'FALSE', 'ide0:0.present': 'FALSE', 'ide1:0.present': 'FALSE'}
        if firmware == 'efi': v['uefi.secureBoot.enabled'] = 'FALSE'
        for n in (4, 5, 6, 7):
            v.update({f'pciBridge{n}.present': 'TRUE', f'pciBridge{n}.virtualDev': 'pcieRootPort', f'pciBridge{n}.functions': '8'})
        if boot == 'floppy':
            shutil.copyfile(Path(out) / 'nexus64.img', self.dir / 'boot.flp')
            v.update({'floppy0.present': 'TRUE', 'floppy0.fileType': 'file', 'floppy0.fileName': 'boot.flp',
                      'floppy0.startConnected': 'TRUE', 'floppy0.readonly': 'TRUE', 'bios.bootOrder': 'floppy'})
        elif boot == 'cdrom':
            shutil.copyfile(Path(out) / 'nexus64.iso', self.dir / 'nexus64.iso')
            v.update({'sata0.present': 'TRUE', 'sata0:1.present': 'TRUE', 'sata0:1.deviceType': 'cdrom-image',
                      'sata0:1.fileName': 'nexus64.iso', 'sata0:1.startConnected': 'TRUE', 'bios.bootOrder': 'cdrom'})
        elif boot == 'esp':
            shutil.copyfile(Path(out) / 'nexus64-uefi.img', self.dir / 'esp.img')
            descriptor(self.dir / 'esp.img', self.dir / 'esp.vmdk')
            v.update({'sata0.present': 'TRUE', 'sata0:2.present': 'TRUE', 'sata0:2.fileName': 'esp.vmdk',
                      'sata0:2.mode': 'independent-nonpersistent', 'bios.bootOrder': 'hdd', 'bios.hddOrder': 'sata0:2'})
        if root:
            descriptor(root, self.dir / 'root.vmdk')
            slot = {'nvme': 'nvme0:0', 'sata': 'sata0:3', 'ide': 'ide0:0'}[root_bus]
            v[slot.split(':')[0] + '.present'] = 'TRUE'
            v.update({f'{slot}.present': 'TRUE', f'{slot}.fileName': 'root.vmdk', f'{slot}.mode': 'independent-nonpersistent'})
        if nic:
            v.update({'ethernet0.present': 'TRUE', 'ethernet0.virtualDev': nic, 'ethernet0.connectionType': network,
                      'ethernet0.addressType': 'generated', 'ethernet0.startConnected': 'TRUE'})
        if usb:
            v.update({'usb.present': 'TRUE', 'usb_xhci.present': 'TRUE', 'ehci.present': 'FALSE',
                      'keyboard.vusb.enable': 'TRUE', 'mouse.vusb.enable': 'TRUE', 'mouse.vusb.useBasicMouse': 'FALSE',
                      'usb.generic.autoconnect': 'FALSE', 'usb.vbluetooth.startConnected': 'FALSE'})
        v.update(options or {})
        self.vmx.write_text(''.join(f'{k} = "{val}"\n' for k, val in v.items()))

    def vmrun(self, *args, check=True, timeout=180):
        p = subprocess.run([str(VMRUN), '-T', 'ws', *args], capture_output=True, text=True, timeout=timeout)
        if check and p.returncode: raise RuntimeError(f'vmrun {args[0]} failed: {p.stdout} {p.stderr}')
        return p

    def start(self):
        self.vmrun('start', str(self.vmx), 'nogui')

    def running(self):
        return str(self.vmx).lower() in self.vmrun('list', check=False).stdout.lower()

    def stop(self):
        if self.running(): self.vmrun('stop', str(self.vmx), 'hard', check=False)
        for _ in range(60):
            if not self.running(): break
            time.sleep(1)

    def log(self):
        return self.serial.read_text(errors='replace') if self.serial.exists() else ''

    def wait(self, markers, timeout, fail=('KERNEL_FAULT', '[64] FAIL', 'PANIC', 'panic:')):
        start = time.time()
        while time.time() - start < timeout:
            text = self.log()
            if all(m in text for m in markers): return time.time() - start
            if any(f in text for f in fail): return None
            if not self.running():   # powered off (ACPI S5): the last lines may land after the read above
                time.sleep(1); return time.time() - start if all(m in self.log() for m in markers) else None
            time.sleep(1)
        return None

    def cleanup(self, keep=('serial.log', 'vmware.log')):
        self.stop()
        for item in self.dir.iterdir():
            if item.name in keep or item.suffix == '.png': continue
            shutil.rmtree(item, ignore_errors=True) if item.is_dir() else item.unlink(missing_ok=True)


def recvn(s, n):
    buf = b''
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d: raise EOFError
        buf += d
    return buf


class VNC:
    """RFB 3.8/3.3 client without authentication: screenshots, key and pointer events. VMware answers every
    update request after the first incrementally, so the client keeps the whole framebuffer."""
    def __init__(self, port, host='127.0.0.1', timeout=15):
        self.s = socket.create_connection((host, port), timeout=timeout); self.image = None
        version = recvn(self.s, 12)
        if version >= b'RFB 003.007':
            self.s.sendall(b'RFB 003.008\n')
            types = recvn(self.s, recvn(self.s, 1)[0])
            if 1 not in types: raise RuntimeError(f'no None security: {list(types)}')
            self.s.sendall(b'\x01')
            if struct.unpack('>I', recvn(self.s, 4))[0]: raise RuntimeError('security failed')
        else:
            self.s.sendall(b'RFB 003.003\n')
            if struct.unpack('>I', recvn(self.s, 4))[0] != 1: raise RuntimeError('security')
        self.s.sendall(b'\x01')
        init = recvn(self.s, 24)
        self.width, self.height = struct.unpack('>HH', init[:4])
        recvn(self.s, struct.unpack('>I', init[20:24])[0])
        self.s.sendall(struct.pack('>BxxxBBBBHHHBBBxxx', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
        self.s.sendall(struct.pack('>BxHii', 2, 2, 0, -223))   # Raw + DesktopSize

    def screenshot(self, path=None, settle=1.5):
        from PIL import Image
        image = self.image
        self.s.sendall(struct.pack('>BBHHHH', 3, 0 if image is None else 1, 0, 0, self.width, self.height))
        deadline, last = time.time() + 30, time.time()
        self.s.settimeout(0.25)
        while time.time() < deadline:
            try:
                kind = recvn(self.s, 1)[0]
            except (TimeoutError, socket.timeout):
                if image is not None and time.time() - last > settle: break
                continue
            self.s.settimeout(15)
            if kind == 0:
                recvn(self.s, 1); count = struct.unpack('>H', recvn(self.s, 2))[0]
                for _ in range(count):
                    x, y, w, h, encoding = struct.unpack('>HHHHi', recvn(self.s, 12))
                    if encoding == -223:
                        self.width, self.height = w, h; image = None
                        continue
                    if image is None: image = Image.new('RGB', (self.width, self.height))
                    image.paste(Image.frombytes('RGB', (w, h), recvn(self.s, w * h * 4), 'raw', 'BGRX'), (x, y))
                self.image = image; last = time.time()
                self.s.sendall(struct.pack('>BBHHHH', 3, 1, 0, 0, self.width, self.height))
            elif kind == 2: pass
            elif kind == 3: recvn(self.s, 3); recvn(self.s, struct.unpack('>I', recvn(self.s, 4))[0])
            else: raise RuntimeError(f'unexpected message {kind}')
            self.s.settimeout(0.25)
        self.s.settimeout(15)
        if path and image: image.save(path)
        return image

    def key(self, keysym, down=None):
        for d in ((True, False) if down is None else (down,)):
            self.s.sendall(struct.pack('>BBxxI', 4, 1 if d else 0, keysym)); time.sleep(0.03)

    def type(self, text):
        for ch in text: self.key(0xFF0D if ch == '\n' else ord(ch))

    def pointer(self, x, y, buttons=0):
        self.s.sendall(struct.pack('>BBHH', 5, buttons, x, y))

    def close(self):
        self.s.close()
