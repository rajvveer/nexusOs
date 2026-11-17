"""Boot the native x86-64 target, assert isolation and reject unsupported CPUs."""
from pathlib import Path
import hashlib
import shutil
import importlib.util
import subprocess
import sys
import time
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64')
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
WORK.mkdir(parents=True, exist_ok=True)
spec=importlib.util.spec_from_file_location('qmp_local',ROOT/'tests/qmp_local.py')
qmp_local=importlib.util.module_from_spec(spec);spec.loader.exec_module(qmp_local)


def check_log(log, disk=False):
    assert '[64] ALL TESTS PASSED' in log, log
    assert '[64] FAIL' not in log and 'KERNEL_FAULT' not in log, log
    assert 'FILE_WRITEBACK failed' not in log and 'FILE_CACHE read failed' not in log and 'PAGE_IO' not in log, log
    assert log.count('[64] USER_FAULT') == (14 if disk else 6), log
    assert log.count('[64] USER PASS:') == (3 if disk else 2), log
    assert 'process pages reclaimed' in log and 'pointers above 4 GiB' in log, log
    assert 'physical page ownership/refcount cleanup' in log, log
    assert 'COW address spaces preserve parent memory and reclaim ownership' in log, log
    assert 'descriptor tables isolate handles and retain shared descriptions/pins' in log, log
    assert 'signal contexts isolate masks/pending state and retain handler ownership' in log, log
    assert 'credential namespaces: ID maps, capability isolation, irreversible drops and ownership' in log, log
    assert 'PID identities: namespace visibility, retained mappings, rollback and ownership' in log, log
    if disk:
        assert '[64] DISK TESTS PASSED' in log and '[64] DISK USER PASS:' in log, log
        assert '[64] FILE API PASS:' in log, log
        assert '[64] DYNAMIC ALL PASSED' in log and '[64] DYNAMIC sparse 1 TiB reservation PASS' in log, log
        assert log.count('[64] DYNAMIC ALL PASSED') == 3 and 'file mmap past EOF terminates with SIGBUS' in log, log
        assert '[64] DYNAMIC page reuse/zeroing PASS' in log and '[64] DYNAMIC VM boundary/protection errors PASS' in log, log
        assert '[64] SIGNAL ALL PASSED:' in log and log.count('Invalid signal frame;') == 2, log
        assert '[64] IPC ALL PASSED:' in log, log
        assert '[64] PROCESS ALL PASSED:' in log and 'timer/FPU and wait status PASS' in log, log
        assert '[64] EXEC IMAGE ISOLATION PASSED:' in log, log
        assert 'PROCESS execve and glibc posix_spawn PASS' in log and 'SA_RESTART PASS' in log, log
        assert '[64] THREAD ALL PASSED:' in log and 'thread group survives initial thread exit' in log, log
        assert 'exit_group unwinds blocked IPC/futex threads' in log and 'fatal thread signal terminates the whole process' in log, log
        assert '[64] PIPE ALL PASSED:' in log and 'default SIGPIPE termination' in log, log
        assert '[64] EVENT ALL PASSED:' in log, log
        assert '[64] EVENTFD PASS:' in log, log
        assert '[64] PPOLL PASS:' in log, log
        assert '[64] SLEEP PASS:' in log, log
        assert '[64] CPU CLOCK PASS:' in log, log
        assert '[64] PID ALL PASSED:' in log, log
        assert '[64] FS ALL PASSED:' in log, log
        assert '[64] SHMEM ALL PASSED:' in log, log
        assert '[64] LIMIT ALL PASSED:' in log and '[64] LIMIT MEMLOCK PASSED:' in log, log
        assert '[64] FILEMAP READONLY PASSED:' in log, log
        assert '[64] JIT ALL PASSED:' in log, log
        assert '[64] LOCK ALL PASSED:' in log, log
        assert '[64] STATFS API PASSED:' in log, log
        assert '[64] NAMESPACE ALL PASSED:' in log and 'no_new_privs and mapped auxv survive exec PASS' in log, log
        assert '[64] DEVICE PASS:' in log, log
        assert '[64] UNIX ALL PASSED:' in log, log


disk_hash = hashlib.sha256((OUT / 'apps64.img').read_bytes()).digest()
test_disk = WORK / 'apps64-copy.img'
shutil.copyfile(OUT / 'apps64.img', test_disk)
for tag, cpu, memory, expected in [('max', 'max', 256, 33), ('small', 'qemu64', 64, 33),
                                    ('repeat', 'max', 128, 33), ('ethernet', 'max', 128, 33), ('no_long_mode', 'qemu32', 128, 35),
                                    ('no_nx', 'max,-nx', 128, 35), ('disk', 'max', 128, 33),
                                    ('no_entropy', 'max,-rdrand', 128, 37), ('smp', 'max', 256, 33),
                                    ('ahci', 'max', 128, 33), ('nvme', 'max', 128, 33)]:  # q35 + AHCI / NVMe root disk
    serial = WORK / (tag + '.log')
    serial.unlink(missing_ok=True)
    args = [str(QEMU), '-cpu', cpu, '-m', str(memory), '-drive',
            f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
            '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
            '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    if tag == 'smp':
        args += ['-smp', '4']
    if tag == 'ahci':
        args += ['-machine', 'q35', '-drive', f'if=none,id=d0,file={test_disk},format=raw', '-device', 'ahci,id=ahci', '-device', 'ide-hd,drive=d0,bus=ahci.0']
    if tag == 'nvme':
        args += ['-machine', 'q35', '-drive', f'if=none,id=n0,file={test_disk},format=raw', '-device', 'nvme,serial=nx0,drive=n0']
    if tag in ('disk', 'ethernet', 'no_entropy', 'smp'):
        args += ['-drive', f'file={test_disk},format=raw,if=ide']
    if tag == 'ethernet':
        args += ['-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network']
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output = proc.communicate(timeout=45)[0]
    except subprocess.TimeoutExpired:
        proc.kill(); output = proc.communicate()[0]
        raise AssertionError(f'{tag}: boot timed out\n{serial.read_text()}\n{output!r}')
    log = serial.read_text()
    assert proc.returncode == expected, (tag, proc.returncode, output, log)
    if expected == 33:
        check_log(log, disk=tag in ('disk', 'ethernet', 'smp', 'ahci', 'nvme'))
        if tag == 'ahci': assert '[64] ROOT sata0p1 (MBR FAT32, FAT32) on AHCI' in log and '[64] BLOCK SELFTEST sata0 PASS' in log, log
        if tag == 'nvme': assert '[64] ROOT nvme0n1p1 (MBR FAT32, FAT32) on NVMe' in log and '[64] BLOCK SELFTEST nvme0n1 PASS' in log, log
        if tag == 'smp':
            assert '[64] SMP: 4 CPUs online' in log, log
            switches = [line for line in log.splitlines() if line.startswith('[64] SMP switches')][-1]
            assert switches.count('=0x') == 4 and '=0x0000000000000000' not in switches, switches
            assert '[64] TSC: synchronized' in log and ' warps 0/' in log, 'APs measured against the BSP TSC, no warps'
        if tag == 'ethernet':
            assert 'RTL8139 native DMA/IRQ ready' in log, log
            assert '[64] DHCP lease 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3' in log, 'kernel DHCP client bound to QEMU user networking'
    elif expected == 35:
        assert '[64] UNSUPPORTED CPU:' in log and '[64] ALL TESTS PASSED' not in log
    else:
        assert 'Dynamic ELF requires secure entropy' in log and '[64] DYNAMIC ALL PASSED' not in log, log
        assert 'KERNEL_FAULT' not in log, log
    print('PASS', tag, flush=True)
# Full self-tests with an Intel NIC (e1000e), an xHCI controller, USB HID devices and a hub attached: boot-time
# enumeration happens before the memory baseline, and shared PCI interrupt lines must not disturb the tests.
serial = WORK / 'usbnet.log'; serial.unlink(missing_ok=True)
args = [str(QEMU), '-cpu', 'max', '-m', '256', '-smp', '2', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
        '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-drive', f'file={test_disk},format=raw,if=ide',
        '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network', '-device', 'qemu-xhci,id=xhci',
        '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0', '-device', 'usb-hub,bus=xhci.0,port=3', '-device', 'usb-mouse,bus=xhci.0,port=3.1']
result = subprocess.run(args, capture_output=True, timeout=90)
log = serial.read_text()
assert result.returncode == 33, ('usbnet', result.returncode, result.stdout, log)
check_log(log, disk=True)
assert 'Intel 82574L (e1000e) native DMA/IRQ ready' in log and '[64] USB boot enumeration complete, devices=4' in log, log
assert 'keyboard ready (boot protocol)' in log and 'absolute pointer ready' in log and 'relative pointer ready' in log, log
print('PASS usbnet', flush=True)
assert hashlib.sha256((OUT / 'apps64.img').read_bytes()).digest() == disk_hash, 'VM modified application disk'
assert hashlib.sha256(test_disk.read_bytes()).digest() == disk_hash, 'read-only loader wrote to its test disk'

# Exercise the existing ISO boot path and inspect its final rendered screen.
control=qmp_local.QmpMonitor(WORK)
serial = WORK / 'iso.log'; serial.unlink(missing_ok=True)
proc = subprocess.Popen([str(QEMU), '-cpu', 'max', '-m', '128', '-cdrom', str(OUT / 'nexus64.iso'),
                         '-drive', f'file={test_disk},format=raw,if=ide',
                         '-boot', 'd', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
                         '-serial', f'file:{serial}', '-qmp', control.argument,
                         '-no-reboot', '-no-shutdown'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if serial.exists() and '[64] ALL TESTS PASSED' in serial.read_text(): break
        if proc.poll() is not None: raise AssertionError(proc.communicate()[0])
        time.sleep(.2)
    check_log(serial.read_text(), disk=True)
    with control.open() as monitor:
        qmp=monitor.command
        ppm = WORK / 'native64.ppm'
        qmp('screendump', {'filename': str(ppm)})
        Image.open(ppm).save(WORK / 'native64.png')
        qmp('quit')
    proc.wait(timeout=5)
finally:
    if proc.poll() is None: proc.kill(); proc.wait()
    control.cleanup(proc)
assert hashlib.sha256(test_disk.read_bytes()).digest() == disk_hash, 'ISO run wrote to its test disk'
print('PASS ISO boot and screenshot; native x86-64 campaign complete', flush=True)
