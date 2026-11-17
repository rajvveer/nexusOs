"""UEFI boot, ACPI, IOAPIC, MSI/MSI-X and MADT SMP for the native x86-64 kernel (OVMF under QEMU).

python -I tests/test_x64_uefi.py [work dir] [--quick]

Cases (all must pass):
  media        BOOTX64.EFI is a PE32+ EFI application; nexus64-uefi.img is a valid GPT disk whose ESP
               (FAT32) holds the loader and the kernel; nexus64.iso carries an El Torito UEFI entry.
  q35_tcg      -machine q35, OVMF, 2 CPUs, TCG, no disk, edu + virtio-rng: kernel self-tests, IOAPIC mode,
               MADT targeted AP start, MSI, MSI-X, MMIO window, and ACPI S5 power-off (no isa-debug-exit:
               QEMU exits 0 because the guest powered off).
  q35_whpx     the same under WHPX (skipped when WHPX is unavailable). QEMU 9.1's WHPX cannot emulate
               OVMF's pflash MMIO, so the same EDK2 build is loaded as a unified ROM (vars + code) via -bios.
  poweroff     reboot(2) LINUX_REBOOT_CMD_POWER_OFF from ring 3 -> ACPI S5.
  restart      reboot(2) LINUX_REBOOT_CMD_RESTART from ring 3 -> FADT reset register (-no-reboot exits).
  pic          UEFI with `noioapic`: legacy 8259 mode, MSI still delivered.
  pc_disk      -machine pc, OVMF, 2 CPUs, IDE apps64 disk: every on-disk campaign; isa-debug-exit still
               ends the run (exit 33).
  pc_network   -machine pc, OVMF, RTL8139: 2 MiB TCP stream with the NIC's INTx routed level-triggered
               through the IOAPIC via irq_attach_isa.
  iso          nexus64.iso as a CD-ROM under OVMF (El Torito platform 0xEF).
  bios_pic     BIOS floppy path with `noioapic` (fw_cfg cmdline) and the apps disk: PIC fallback.
  bios_msi     BIOS floppy path (SeaBIOS ACPI) with edu + virtio-rng: IOAPIC mode, MSI, MSI-X.
  bios_noacpi  BIOS path with `-machine pc,acpi=off`: no RSDP -> legacy PIC, QEMU fw_cfg CPU count and the
               broadcast INIT/SIPI fallback.
  many_cpus    (only with --many-cpus) BIOS path with 12 CPUs on a single TCG thread: MAX_CPUS > 8 (GDT TSS
               slots, per-CPU SYSCALL stubs) with MADT targeted start-up.
Disks are attached read-only through QEMU snapshot=on; the one marker copy (network case) is deleted
right after its run, and the firmware copies are removed at the end.
The BIOS regression itself stays in tests/test_x64.py.
"""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('native64-uefi')
QUICK = '--quick' in sys.argv
MANY_CPUS = '--many-cpus' in sys.argv
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
SHARE = QEMU.parent / 'share'
WORK.mkdir(parents=True, exist_ok=True)
CODE = WORK / 'edk2-x86_64-code.fd'        # copies: QEMU option parsing must not see the "Program Files" space
VARS = WORK / 'edk2-vars-template.fd'
shutil.copyfile(SHARE / 'edk2-x86_64-code.fd', CODE)
shutil.copyfile(SHARE / 'edk2-i386-vars.fd', VARS)
UNIFIED = WORK / 'ovmf-unified.fd'          # OVMF.fd layout: variable store template followed by code
UNIFIED.write_bytes(VARS.read_bytes() + CODE.read_bytes())
WHPX_CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+xsave,+avx,+avx2'
spec = importlib.util.spec_from_file_location('uefi_build', ROOT / 'arch/x86_64/build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
spec = importlib.util.spec_from_file_location('uefi_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)


# The ACPI exits: with the AML interpreter (uACPI) S5 runs _PTS/_S5 and the reset goes through uACPI's FADT reset register;
# without it (table-only mode) the kernel's own sequences run.
S5_END = ('[64] ACPI S5 power-off: SLP_TYPa=0 SLP_TYPb=0', '[64] ACPI S5 power-off through AML (_PTS/_S5)')
RESET_END = ('[64] ACPI reset via FADT reset register 0xCF9=0xF', '[64] ACPI reset via FADT reset register (uACPI)')


def check_selftests(log, disk=False):
    assert '[64] ALL TESTS PASSED' in log, log
    assert '[64] FAIL' not in log and 'KERNEL_FAULT' not in log, log
    assert log.count('[64] USER_FAULT') == (14 if disk else 6), log
    assert 'process pages reclaimed' in log and 'pointers above 4 GiB' in log, log
    assert 'COW address spaces preserve parent memory and reclaim ownership' in log, log
    if disk:
        for marker in ('[64] DISK TESTS PASSED', '[64] FILE API PASS:', '[64] DYNAMIC ALL PASSED', '[64] SIGNAL ALL PASSED:',
                       '[64] IPC ALL PASSED:', '[64] PROCESS ALL PASSED:', '[64] THREAD ALL PASSED:', '[64] PIPE ALL PASSED:',
                       '[64] EVENT ALL PASSED:', '[64] PID ALL PASSED:', '[64] FS ALL PASSED:', '[64] SHMEM ALL PASSED:',
                       '[64] LIMIT ALL PASSED:', '[64] JIT ALL PASSED:', '[64] LOCK ALL PASSED:', '[64] UNIX ALL PASSED:',
                       '[64] NAMESPACE ALL PASSED:', '[64] DEVICE PASS:'):
            assert marker in log, (marker, log)


def check_uefi(log, cpus):
    assert 'NexusOS UEFI loader (BOOTX64.EFI)' in log and '[uefi] boot services exited' in log, log
    assert '[64] BOOT source=UEFI' in log and 'fb=1920x1080' in log, log
    assert '[64] ACPI rev=2 XSDT' in log and '[64] ACPI MADT cpus=%d ioapics=1' % cpus in log, log
    assert 'S5=0/0' in log, log


def qemu(tag, *, machine='q35', uefi='pflash', whpx=False, smp=2, mem=256, debug_exit=False, disk=None, extra=(),
         cmdline=None, cdrom=None, timeout=240, expect=0, tcg_single=False):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [str(QEMU), '-machine', machine, *(['-accel', 'whpx,kernel-irqchip=off', '-cpu', WHPX_CPU] if whpx else ['-cpu', 'max']),
               '-smp', str(smp), '-m', str(mem), '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot']
    if uefi == 'pflash':
        vars_copy = WORK / (tag + '-vars.fd'); shutil.copyfile(VARS, vars_copy)
        command += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}', '-drive', f'if=pflash,format=raw,unit=1,file={vars_copy}']
    elif uefi == 'rom':
        command += ['-bios', str(UNIFIED)]
    if uefi:
        boot_bus = 'ide.1' if machine == 'pc' else 'ide.2'
        if cdrom:
            command += ['-drive', f'if=none,id=cd,format=raw,media=cdrom,readonly=on,file={cdrom}', '-device', f'ide-cd,drive=cd,bus={boot_bus},bootindex=0']
        else:
            command += ['-drive', f'if=none,id=esp,format=raw,snapshot=on,file={OUT / "nexus64-uefi.img"}', '-device', f'ide-hd,drive=esp,bus={boot_bus},bootindex=0']
    else:
        command += ['-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a']
    if disk:
        command += ['-drive', f'file={disk},format=raw,if=none,id=apps,snapshot=on', '-device', 'ide-hd,drive=apps,bus=ide.0']
    if debug_exit:
        command += ['-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    if tcg_single:
        command += ['-accel', 'tcg,thread=single']
    if cmdline:
        command += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
    command += list(extra)
    start = time.monotonic()
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output = proc.communicate(timeout=timeout)[0]
    except subprocess.TimeoutExpired:
        proc.kill(); output = proc.communicate()[0]
        raise AssertionError(f'{tag}: timed out\n{serial.read_text(errors="replace")[-3000:]}\n{output!r}')
    log = serial.read_text(errors='replace')
    assert proc.returncode == expect, (tag, proc.returncode, output, log[-4000:])
    print(f'PASS {tag} ({time.monotonic() - start:.1f}s)', flush=True)
    return log


def fat_read(volume, path):
    """Independent FAT16/FAT32 reader (8.3 names) used to verify mkuefi.py output."""
    bps, spc, rsv, fats, root_entries = struct.unpack_from('<HBHBH', volume, 11)
    fat16_size = struct.unpack_from('<H', volume, 22)[0]
    fat32 = fat16_size == 0
    fat_size = struct.unpack_from('<I', volume, 36)[0] if fat32 else fat16_size
    root_sectors = (root_entries * 32 + bps - 1) // bps
    data = (rsv + fats * fat_size + root_sectors) * bps
    fat = volume[rsv * bps:(rsv + fat_size) * bps]

    def chain(cluster):
        while 2 <= cluster < (0x0FFFFFF8 if fat32 else 0xFFF8):
            yield volume[data + (cluster - 2) * spc * bps:data + (cluster - 1) * spc * bps]
            cluster = struct.unpack_from('<I', fat, cluster * 4)[0] & 0x0FFFFFFF if fat32 else struct.unpack_from('<H', fat, cluster * 2)[0]

    directory = b''.join(chain(struct.unpack_from('<I', volume, 44)[0])) if fat32 else \
        volume[(rsv + fats * fat_size) * bps:data]
    parts = path.split('/')
    for i, part in enumerate(parts):
        base, _, ext = part.upper().partition('.')
        name = (base.ljust(8) + ext.ljust(3)).encode()
        for at in range(0, len(directory), 32):
            entry = directory[at:at + 32]
            if entry[0] == 0:
                raise AssertionError('missing ' + path)
            if entry[:11] == name and not entry[11] & 0x08:
                cluster = struct.unpack_from('<H', entry, 20)[0] << 16 | struct.unpack_from('<H', entry, 26)[0]
                content = b''.join(chain(cluster))
                if i == len(parts) - 1:
                    return content[:struct.unpack_from('<I', entry, 28)[0]]
                directory = content
                break
        else:
            raise AssertionError('missing ' + path)


def check_media():
    efi = (OUT / 'BOOTX64.EFI').read_bytes()
    pe = struct.unpack_from('<I', efi, 0x3C)[0]
    assert efi[:2] == b'MZ' and efi[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', efi, pe + 4)[0] == 0x8664
    assert struct.unpack_from('<H', efi, pe + 24)[0] == 0x20B and struct.unpack_from('<H', efi, pe + 24 + 68)[0] == 10, 'PE32+ EFI application'
    image = (OUT / 'nexus64-uefi.img').read_bytes()
    assert image[510:512] == b'\x55\xAA' and image[446 + 4] == 0xEE, 'protective MBR'
    header = bytearray(image[512:512 + 92])
    assert header[:8] == b'EFI PART'
    crc = struct.unpack_from('<I', header, 16)[0]; struct.pack_into('<I', header, 16, 0)
    assert zlib.crc32(header) & 0xFFFFFFFF == crc, 'GPT header CRC'
    entries_lba, count, size, entries_crc = struct.unpack_from('<QIII', header, 72)
    entries = image[entries_lba * 512:entries_lba * 512 + count * size]
    assert zlib.crc32(entries) & 0xFFFFFFFF == entries_crc, 'GPT entry array CRC'
    esp_type = bytes.fromhex('28732AC11FF8D211BA4B00A0C93EC93B')
    assert entries[:16] == esp_type, 'first partition is an EFI System Partition'
    first, last = struct.unpack_from('<QQ', entries, 32)
    backup = image[-512:]
    assert backup[:8] == b'EFI PART' and struct.unpack_from('<Q', backup, 24)[0] == len(image) // 512 - 1, 'backup GPT header'
    esp = image[first * 512:(last + 1) * 512]
    assert esp[82:87] == b'FAT32', 'ESP is FAT32'
    for path, expected in (('EFI/BOOT/BOOTX64.EFI', efi), ('EFI/NEXUS/KERNEL.BIN', (OUT / 'kernel.bin').read_bytes())):
        assert fat_read(esp, path) == expected, path
    efiboot = (OUT / 'efiboot.img').read_bytes()
    assert efiboot[54:59] == b'FAT16' and fat_read(efiboot, 'EFI/BOOT/BOOTX64.EFI') == efi, 'El Torito FAT image'
    iso = (OUT / 'nexus64.iso').read_bytes()
    catalog = struct.unpack_from('<I', iso, 17 * 2048 + 0x47)[0] * 2048
    assert iso[catalog + 64] == 0x91 and iso[catalog + 65] == 0xEF and iso[catalog + 96] == 0x88, 'El Torito UEFI section'
    print('PASS media: PE32+ EFI loader, GPT/ESP image with loader and kernel, ISO El Torito UEFI entry', flush=True)


def network_case():
    server = socket.socket(); server.bind(('127.0.0.1', 0)); server.listen(1); server.settimeout(200)
    port = server.getsockname()[1]; errors = []; finished = []

    def serve():
        try:
            with server:
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(200); request = b''
                    while not request.endswith(b'\n') and len(request) < 64:
                        data = connection.recv(64 - len(request))
                        if not data: raise AssertionError('guest closed before request')
                        request += data
                    assert request == b'NEXUS64-NET\n', request
                    block = bytes((i * 37 + 11) & 255 for i in range(65536))
                    for _ in range(32): connection.sendall(block)
                    connection.shutdown(socket.SHUT_WR)
                    assert connection.recv(1) == b''
                    finished.append(True)
        except BaseException as error:
            errors.append(repr(error))
    thread = threading.Thread(target=serve, daemon=True); thread.start()
    disk = WORK / 'network-disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / 'port'; marker.write_bytes(struct.pack('<H', port))
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-network'])
    try:
        log = qemu('pc_network', machine='pc', smp=1, mem=256, debug_exit=True, disk=disk, expect=33,
                   extra=['-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network,romfile='], timeout=240)
    finally:
        server.close()
        disk.unlink(missing_ok=True)
    thread.join(timeout=5)
    assert '[64] NATIVE NETWORK 2 MIB STREAM PASSED' in log and 'RTL8139 native DMA/IRQ ready' in log, log
    assert '[64] IRQ mode: IOAPIC' in log and '[64] BOOT source=UEFI' in log, log
    assert finished and not errors, (finished, errors)


check_media()
log = qemu('q35_tcg', extra=['-device', 'edu', '-device', 'virtio-rng-pci'])
check_uefi(log, 2); check_selftests(log)
assert '[64] IRQ mode: IOAPIC (1 IOAPIC, 24 pins at 0xFEC00000), 8259 PIC masked' in log, log
assert '[64] SMP: 2 CPUs online (big kernel lock, LAPIC IPIs, MADT targeted INIT/SIPI)' in log, log
assert '[64] PASS PCI MSI: edu messages delivered' in log and '[64] PASS PCI MSI-X: virtio-rng queue interrupt' in log, log
assert '[64] PASS MMIO window: virtio common config' in log and 'above 4 GiB, kernel window' in log, log
assert '[64] ACPI MCFG ecam=0xE0000000' in log, log
assert log.rstrip().endswith(S5_END) and 'SHUTDOWN: no isa-debug-exit device; ACPI power-off' in log, log
if build.whpx_available():
    log = qemu('q35_whpx', uefi='rom', whpx=True, extra=['-device', 'edu', '-device', 'virtio-rng-pci'])
    check_uefi(log, 2); check_selftests(log)
    assert '[64] IRQ mode: IOAPIC' in log and 'MADT targeted INIT/SIPI' in log and '[64] PASS PCI MSI-X' in log, log
    assert log.rstrip().endswith(S5_END), log
else:
    print('SKIP q35_whpx: WHPX unavailable', flush=True)
log = qemu('poweroff', cmdline='selftest.poweroff')
assert '[64] USER REBOOT PASS: magic/command validation, EPERM in a user namespace; requesting power-off' in log, log
assert '[64] REBOOT syscall: power-off requested by pid' in log and log.rstrip().endswith(S5_END), log
log = qemu('restart', cmdline='selftest.restart')
assert 'requesting restart' in log and log.rstrip().endswith(RESET_END), log
log = qemu('pic', cmdline='noioapic', extra=['-device', 'edu'])
assert '[64] IRQ mode: legacy 8259 PIC (disabled on the command line)' in log and '(PIC mode)' in log, log
check_selftests(log)
if not QUICK:
    disk = OUT / 'apps64.img'
    before = hashlib.sha256(disk.read_bytes()).digest()
    log = qemu('pc_disk', machine='pc', debug_exit=True, disk=disk, expect=33, timeout=300)
    check_uefi(log, 2); check_selftests(log, disk=True)
    assert 'MADT targeted INIT/SIPI' in log and '[64] IRQ mode: IOAPIC' in log, log
    network_case()
    log = qemu('iso', cdrom=OUT / 'nexus64.iso', smp=1)
    assert '[64] BOOT source=UEFI' in log and '[64] ALL TESTS PASSED' in log, log
    log = qemu('bios_pic', machine='pc', uefi=None, debug_exit=True, disk=disk, cmdline='noioapic', expect=33, timeout=300)
    assert '[64] BOOT source=BIOS' in log and '[64] IRQ mode: legacy 8259 PIC (disabled on the command line)' in log, log
    check_selftests(log, disk=True)
    log = qemu('bios_msi', machine='pc', uefi=None, debug_exit=True, expect=33, extra=['-device', 'edu', '-device', 'virtio-rng-pci'])
    assert '[64] BOOT source=BIOS' in log and '[64] IRQ mode: IOAPIC' in log and 'MADT targeted INIT/SIPI' in log, log
    assert '[64] PASS PCI MSI: edu' in log and '[64] PASS PCI MSI-X: virtio-rng' in log, log
    log = qemu('bios_noacpi', machine='pc,acpi=off', uefi=None, debug_exit=True, expect=33)
    assert '[64] ACPI: no RSDP' in log and '[64] IRQ mode: legacy 8259 PIC (no ACPI MADT)' in log, log
    assert '[64] SMP: 2 CPUs online (big kernel lock, LAPIC IPIs, broadcast INIT/SIPI)' in log, log
    check_selftests(log)
    assert hashlib.sha256(disk.read_bytes()).digest() == before, 'snapshot=on runs changed apps64.img'
if MANY_CPUS:
    log = qemu('many_cpus', machine='pc', uefi=None, smp=12, mem=512, debug_exit=True, expect=33, tcg_single=True, timeout=300)
    assert '[64] ACPI MADT cpus=12' in log and '[64] SMP: 12 CPUs online (big kernel lock, LAPIC IPIs, MADT targeted INIT/SIPI)' in log, log
    check_selftests(log)
for leftover in WORK.glob('*.fd'):
    leftover.unlink()
print('PASS native x86-64 UEFI/ACPI/IOAPIC/MSI campaign', flush=True)
