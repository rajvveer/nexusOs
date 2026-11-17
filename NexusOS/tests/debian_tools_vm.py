"""Run a shell script with Debian's storage tools (cryptsetup, dmsetup, fsck.*, mkfs.*: arch/x86_64/storage-lock.json) as
root in a NexusOS guest, with extra disks on AHCI ports (/dev/sda, /dev/sdb, ...). The ext4 tools root is built once per
work directory by arch/x86_64/storage_tools.py and kept sparse (zero runs deallocated).

    import debian_tools_vm as d
    ok, log = d.run_script(work, '#!/bin/dash\\n...\\necho "[64] FSTEST ALL PASSED"\\n', [disk1, disk2], files={'nexus/x': bytes})
The script prints "[64] FSTEST ..." lines; ok means "[64] FSTEST ALL PASSED" was printed."""
from pathlib import Path
import ctypes, importlib.util, msvcrt, os, shutil, subprocess, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def punch(path):
    """Deallocate all-zero 64 KiB runs (NTFS sparse): seeking past zeros does not leave holes on this host."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    def ioctl(f, code, data):
        out = ctypes.c_uint32(0); buf = ctypes.create_string_buffer(data)
        if not k32.DeviceIoControl(msvcrt.get_osfhandle(f.fileno()), code, buf, len(data), None, 0, ctypes.byref(out), None): raise OSError(ctypes.get_last_error())
    with open(path, 'r+b') as f:
        ioctl(f, 0x900C4, b'\x01'); size = os.fstat(f.fileno()).st_size; start = None; at = 0; zero = bytes(65536)
        while chunk := f.read(65536):
            if chunk == zero[:len(chunk)]:
                if start is None: start = at
            elif start is not None: ioctl(f, 0x980C8, start.to_bytes(8, 'little') + at.to_bytes(8, 'little')); start = None
            at += len(chunk)
        if start is not None: ioctl(f, 0x980C8, start.to_bytes(8, 'little') + size.to_bytes(8, 'little'))


def unlink(path):
    import gc
    for _ in range(40):
        try: Path(path).unlink(missing_ok=True); return
        except PermissionError: gc.collect(); time.sleep(0.5)


# lvm2 without udev (NexusOS runs none): lvm makes its /dev/mapper nodes and /dev/<vg> links itself
LVM_SETUP = """mkdir -p /etc/lvm /run/lvm /run/lock/lvm
cat > /etc/lvm/lvm.conf <<'EOF'
devices { obtain_device_list_from_udev = 0 external_device_info_source = "none" use_devicesfile = 0 }
global { use_lvmlockd = 0 }
activation { udev_sync = 0 udev_rules = 0 verify_udev_operations = 0 monitoring = 0 }
EOF
export LVM_SUPPRESS_FD_WARNINGS=1"""


def gpt_disk(path, parts, extra_mib=2):
    """A sparse GPT disk image: parts = [(MiB, type uuid.UUID, partition uuid.UUID, name)] from LBA 2048 (1 MiB aligned).
    Returns the partitions' first LBAs."""
    import struct, uuid, zlib
    total = (sum(p[0] for p in parts) + extra_mib) * 2048; at = 2048; entries = b''; starts = []
    for mib, kind, guid, name in parts:
        entries += kind.bytes_le + guid.bytes_le + struct.pack('<QQQ', at, at + mib * 2048 - 1, 0) + name.encode('utf-16-le').ljust(72, b'\0')
        starts.append(at); at += mib * 2048
    entries = entries.ljust(128 * 128, b'\0'); disk_guid = uuid.uuid4()
    def header(mine, other, table):
        h = bytearray(struct.pack('<8sIII4xQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, mine, other, 34, total - 34, disk_guid.bytes_le, table, 128, 128, zlib.crc32(entries)))
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:92]))); return bytes(h).ljust(512, b'\0')
    mbr = bytearray(512); struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\x02\0', 0xEE, b'\xff\xff\xff', 1, min(total - 1, 0xFFFFFFFF)); mbr[510:] = b'\x55\xaa'
    Path(path).unlink(missing_ok=True)
    with open(path, 'wb') as f:
        f.truncate(total * 512); f.write(mbr); f.write(header(1, total - 1, 2)); f.write(entries)
        f.seek((total - 33) * 512); f.write(entries); f.write(header(total - 1, 1, total - 33))
    punch(path)
    return starts


def raw_disk(path, mib):
    Path(path).unlink(missing_ok=True)
    with open(path, 'wb') as f: f.truncate(mib << 20)
    punch(path)


def tools_base(work):
    tools = load('dtv_storage_tools', ROOT / 'arch/x86_64/storage_tools.py')
    base, stamp_file = Path(work) / 'tools-base.img', Path(work) / 'tools-base.stamp'
    stamp = (ROOT / 'arch/x86_64/storage-lock.json').read_bytes()[:4096].hex()[:16] + str((OUT / 'apps64.img').stat().st_mtime_ns)
    if base.exists() and stamp_file.exists() and stamp_file.read_text() == stamp: return base
    unlink(base); tools.image(base, tools.CACHE, 640, []); punch(base); stamp_file.write_text(stamp)
    return base


def run_script(work, script, ahci_disks=(), files=None, timeout=1800, tag='debian-tools', kill_on=None, kill_delay=0.0):
    """kill_on: cut the power (kill QEMU) kill_delay seconds after this text appears in the serial log."""
    work = Path(work); work.mkdir(parents=True, exist_ok=True)
    ext4lib = load('dtv_ext4lib', ROOT / 'userland/ext4lib.py')
    fstools = work / 'fstools.elf'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'arch/x86_64/fstools_user.c'), '-o', str(fstools)], check=True)
    disk = work / f'{tag}-root.img'; shutil.copyfile(tools_base(work), disk); punch(disk)
    fs = ext4lib.Writer(str(disk)); data = fstools.read_bytes()
    try:
        fs.put_bytes(data, 'sbin/init', 0o755, 0, 0); fs.put_bytes(data, 'bin/fstools-root', 0o4755, 0, 0)
        fs.put_bytes(script.encode(), 'nexus/script.sh', 0o644, 0, 0)
        fs.put_bytes(b'script /nexus/script.sh\n', 'nexus/fstools-mode', 0o644, 0, 0)
        fs.put_bytes(b'root:x:0:0:root:/root:/bin/sh\n', 'etc/passwd', 0o644, 0, 0)
        for name, content in (files or {}).items(): fs.put_bytes(content, name, 0o644, 0, 0)
    finally: fs.close()
    command = [QEMU, '-machine', 'q35', '-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU, '-smp', '2', '-m', '2048', '-display', 'none', '-vga', 'none',
               '-device', 'virtio-vga', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
               '-drive', f'if=none,id=d0,format=raw,file={disk}', '-device', 'nvme,serial=tools0,drive=d0', '-device', 'ahci,id=ahci']
    for i, extra in enumerate(ahci_disks):
        command += ['-drive', f'if=none,id=t{i},format=raw,file={extra}', '-device', f'ide-hd,drive=t{i},bus=ahci.{i}']
    serial = work / f'{tag}.log'; serial.unlink(missing_ok=True)
    command += ['-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot']
    if kill_on:   # a power cut at a chosen moment
        vm = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); end = time.time() + timeout
        while vm.poll() is None and time.time() < end:
            if serial.exists() and kill_on in serial.read_text(errors='replace'): time.sleep(kill_delay); break
            time.sleep(0.2)
        if vm.poll() is None: vm.kill()
        vm.wait()
    else:
        try: subprocess.run(command, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired: pass
    log = serial.read_text(errors='replace') if serial.exists() else ''
    unlink(disk)
    return '[64] FSTEST ALL PASSED' in log, log
