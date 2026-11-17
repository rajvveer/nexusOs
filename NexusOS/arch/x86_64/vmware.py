"""Run the installed NexusOS desktop in VMware Workstation (build.bat x64 vmware [--uefi] [--hidden]).

Writes out/vmware/NexusOS.vmx and out/vmware/desktop64.vmdk, a descriptor whose only extent is
out/desktop64.img itself (no copy, no conversion: QEMU and VMware share the same disk, but never at the
same time), then powers the VM on in the Workstation window. The virtual PC matches the QEMU launcher's
modern machine: NVMe root disk, Intel e1000e on VMware's NAT network (DHCP), xHCI with USB keyboard and an
absolute USB pointer (no mouse grab), VMware SVGA (the kernel uses the VBE/GOP framebuffer, 1920x1080), HD Audio,
4 vCPUs. BIOS boot from the nexus64.img floppy by default; --uefi boots nexus64.iso through VMware's EFI.
Keys VMware adds to the .vmx (UUIDs, MAC address, EFI variables) are kept between launches.
"""
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
OUT = HERE / 'out'
WORKSTATION = Path(r'C:\Program Files (x86)\VMware\VMware Workstation')
FOLDER = OUT / 'vmware'
VMX = FOLDER / 'NexusOS.vmx'


def installed():
    return (WORKSTATION / 'vmware.exe').is_file() and (WORKSTATION / 'vmrun.exe').is_file()


def running_vms():
    """Paths of the VMs VMware is running now (empty when Workstation is not installed)."""
    if not installed(): return []
    try:  # best effort: a failing query must never block a launch
        result = subprocess.run([str(WORKSTATION / 'vmrun.exe'), '-T', 'ws', 'list'], capture_output=True, text=True, timeout=60)
        return [line.strip() for line in (result.stdout or '').splitlines()[1:] if line.strip()]
    except Exception:
        return []


def disk_in_use_by_vmware():
    # A powered-on VM holds lock directories (*.lck) in its folder; only then ask VMware which VMs run.
    if not FOLDER.is_dir() or not any(FOLDER.glob('*.lck')): return False
    return any(Path(path).resolve() == VMX.resolve() for path in running_vms() if Path(path).exists())


def write_descriptor(disk, descriptor):
    sectors = disk.stat().st_size // 512
    text = ('# Disk DescriptorFile\nversion=1\nencoding="UTF-8"\nCID=fffffffe\nparentCID=ffffffff\ncreateType="monolithicFlat"\n\n'
            '# Extent description\n'
            f'RW {sectors} FLAT "{disk}" 0\n\n'
            '# The Disk Data Base\n#DDB\n\nddb.adapterType = "lsilogic"\n'
            f'ddb.geometry.cylinders = "{max(1, min(65535, sectors // (255 * 63)))}"\nddb.geometry.heads = "255"\n'
            'ddb.geometry.sectors = "63"\nddb.virtualHWVersion = "21"\n')
    if not descriptor.exists() or descriptor.read_text() != text: descriptor.write_text(text)


def settings(uefi, cpus, memory_mib, hidden=False):
    v = {'.encoding': 'UTF-8', 'config.version': '8', 'virtualHW.version': '21', 'displayName': 'NexusOS',
         'msg.autoAnswer': 'TRUE' if hidden else 'FALSE',  # without a window nobody can answer a VMware question
         'guestOS': 'other-64', 'firmware': 'efi' if uefi else 'bios', 'memsize': str(memory_mib), 'numvcpus': str(cpus),
         'cpuid.coresPerSocket': str(cpus), 'pciBridge0.present': 'TRUE', 'vmci0.present': 'TRUE', 'hpet0.present': 'TRUE',
         'tools.syncTime': 'FALSE', 'svga.present': 'TRUE', 'svga.vramSize': str(256 * 1024 * 1024), 'mks.enable3d': 'FALSE',
         'nvme0.present': 'TRUE', 'nvme0:0.present': 'TRUE', 'nvme0:0.fileName': 'desktop64.vmdk', 'nvme0:0.mode': 'persistent',
         'ethernet0.present': 'TRUE', 'ethernet0.virtualDev': 'e1000e', 'ethernet0.connectionType': 'nat',
         'ethernet0.addressType': 'generated', 'ethernet0.startConnected': 'TRUE',
         'usb.present': 'TRUE', 'usb_xhci.present': 'TRUE', 'ehci.present': 'FALSE', 'keyboard.vusb.enable': 'TRUE',
         'mouse.vusb.enable': 'TRUE', 'mouse.vusb.useBasicMouse': 'FALSE', 'usb.generic.autoconnect': 'FALSE',
         'sound.present': 'TRUE', 'sound.virtualDev': 'hdaudio', 'sound.fileName': '-1', 'sound.autodetect': 'TRUE',
         'serial0.present': 'TRUE', 'serial0.fileType': 'file', 'serial0.fileName': 'serial.log', 'serial0.yieldOnMsrRead': 'TRUE',
         'ide0:0.present': 'FALSE', 'ide1:0.present': 'FALSE'}
    for n in (4, 5, 6, 7):
        v.update({f'pciBridge{n}.present': 'TRUE', f'pciBridge{n}.virtualDev': 'pcieRootPort', f'pciBridge{n}.functions': '8'})
    if uefi:
        v.update({'uefi.secureBoot.enabled': 'FALSE', 'floppy0.present': 'FALSE', 'sata0.present': 'TRUE', 'sata0:1.present': 'TRUE',
                  'sata0:1.deviceType': 'cdrom-image', 'sata0:1.fileName': 'nexus64.iso', 'sata0:1.startConnected': 'TRUE',
                  'bios.bootOrder': 'cdrom'})
    else:
        v.update({'floppy0.present': 'TRUE', 'floppy0.fileType': 'file', 'floppy0.fileName': 'nexus64.flp',
                  'floppy0.startConnected': 'TRUE', 'floppy0.readonly': 'TRUE', 'bios.bootOrder': 'floppy',
                  'sata0.present': 'FALSE'})
    return v


def write_vmx(values):
    """Update the managed keys, keep everything VMware itself added."""
    lines, seen = [], set()
    if VMX.exists():
        for line in VMX.read_text(encoding='utf-8', errors='replace').splitlines():
            key = line.split('=', 1)[0].strip()
            if '=' in line and key in values:
                lines.append(f'{key} = "{values[key]}"'); seen.add(key)
            elif '=' in line and key.startswith(('sata0', 'floppy0', 'uefi.secureBoot')) and key not in values:
                continue   # firmware/boot keys of the other mode
            else:
                lines.append(line)
    lines += [f'{key} = "{value}"' for key, value in values.items() if key not in seen]
    VMX.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def prepare(disk, uefi=False, cpus=4, memory_mib=4096, hidden=False):
    FOLDER.mkdir(exist_ok=True)
    write_descriptor(disk.resolve(), FOLDER / 'desktop64.vmdk')
    boot = OUT / ('nexus64.iso' if uefi else 'nexus64.img')
    target = FOLDER / ('nexus64.iso' if uefi else 'nexus64.flp')
    if not target.exists() or target.read_bytes() != boot.read_bytes(): shutil.copyfile(boot, target)
    write_vmx(settings(uefi, cpus, memory_mib, hidden))
    return VMX


def launch(disk, uefi=False, hidden=False, cpus=4):
    if not installed(): raise SystemExit('VMware Workstation is not installed (expected ' + str(WORKSTATION) + ').')
    if disk_in_use_by_vmware(): raise SystemExit('NexusOS is already running in VMware.')
    vmx = prepare(disk, uefi, cpus, hidden=hidden)
    if hidden:
        result = subprocess.run([str(WORKSTATION / 'vmrun.exe'), '-T', 'ws', 'start', str(vmx), 'nogui'], capture_output=True, text=True)
        if result.returncode: raise SystemExit('vmrun start failed: ' + result.stdout + result.stderr)
        print('NexusOS is running in VMware without a window; stop it with:', f'"{WORKSTATION / "vmrun.exe"}" -T ws stop "{vmx}"')
    else:
        subprocess.Popen([str(WORKSTATION / 'vmware.exe'), '-x', str(vmx)])   # opens Workstation and powers the VM on
        print('NexusOS is starting in VMware Workstation:', vmx)
    print('Serial log:', FOLDER / 'serial.log')


if __name__ == '__main__':
    launch(OUT / 'desktop64.img', '--uefi' in sys.argv, '--hidden' in sys.argv)
