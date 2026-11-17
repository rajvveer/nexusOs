"""NexusOS on VMware Workstation: a second, independent virtual PC (VMware's own BIOS/EFI firmware, ACPI tables,
i440BX chipset with PCIe root ports, NVMe/AHCI/IDE, e1000e, xHCI, SVGA) next to QEMU.

python -I tests/test_vmware.py [WORK] [--unit] [--internet] [--desktop DISK] [--cases a,b]

Always: unit test of the `build.bat vmware` launcher (arch/x86_64/vmware.py) in a scratch folder.
Live cases (skipped when VMware Workstation is not installed; each boots nexus64 on a fresh VM, root images
attached independent-nonpersistent so VMware never writes them):
  bios_nvme   BIOS floppy boot, apps64.img on NVMe: the whole self-test campaign; e1000e DHCP lease on host-only
  bios_sata   the same with the root disk on AHCI SATA
  bios_ide    the same on the PIIX IDE controller, no USB
  efi_cd      VMware EFI boots nexus64.iso from a SATA CD-ROM, root on NVMe
  efi_esp     VMware EFI boots nexus64-uefi.img (GPT + ESP) from SATA, root on NVMe
  audio       VMware HD Audio (15AD:1977, host sound backend disconnected so nothing is audible): the HDA driver finds
              the codec's output path and the whole /bin/audio64 ALSA campaign passes (rates, underrun, mmap, mixing,
              pause), started 5 s after boot
  --internet  NAT network: DHCP lease, kernel-generated /etc/resolv.conf, glibc DNS and an HTTP GET of example.com
  --desktop   Aurora on DISK (host-only network, nonpersistent, SHA-256 unchanged): AURORA READY, USB keyboard/pointer
              drive Spotlight into Terminal; screenshots in WORK/desktop.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import shutil
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('vmware-tests'))
parser.add_argument('--unit', action='store_true', help='launcher unit test only')
parser.add_argument('--internet', action='store_true', help='also run the NAT/DNS/HTTP case (needs internet access)')
parser.add_argument('--desktop', type=Path, help='installed desktop disk to boot read-only')
parser.add_argument('--cases', default='bios_nvme,bios_sata,bios_ide,efi_cd,efi_esp,audio')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


vmware = module('nexus_vmware_launcher', HERE / 'vmware.py')
harness = module('nexus_vmware_harness', ROOT / 'tests/vmware_harness.py')

# ---- launcher unit test ----
with tempfile.TemporaryDirectory(dir=WORK) as scratch:
    scratch = Path(scratch); out = scratch / 'out'; out.mkdir()
    disk = out / 'desktop64.img'; disk.write_bytes(bytes(1024 * 1024))
    (out / 'nexus64.img').write_bytes(b'F' * 2949120); (out / 'nexus64.iso').write_bytes(b'I' * 4096)
    vmware.OUT, vmware.FOLDER, vmware.VMX = out, out / 'vmware', out / 'vmware' / 'NexusOS.vmx'
    vmx = vmware.prepare(disk)
    text = vmx.read_text()
    descriptor = (out / 'vmware' / 'desktop64.vmdk').read_text()
    assert f'RW 2048 FLAT "{disk.resolve()}" 0' in descriptor and 'createType="monolithicFlat"' in descriptor
    for key, value in (('nvme0:0.fileName', 'desktop64.vmdk'), ('nvme0:0.mode', 'persistent'), ('ethernet0.virtualDev', 'e1000e'),
                       ('ethernet0.connectionType', 'nat'), ('usb_xhci.present', 'TRUE'), ('mouse.vusb.useBasicMouse', 'FALSE'),
                       ('firmware', 'bios'), ('floppy0.fileName', 'nexus64.flp'), ('sound.virtualDev', 'hdaudio'), ('numvcpus', '4'),
                       ('msg.autoAnswer', 'FALSE')):
        assert f'{key} = "{value}"' in text, (key, text)
    assert (out / 'vmware' / 'nexus64.flp').read_bytes() == (out / 'nexus64.img').read_bytes()
    vmx.write_text(text + 'uuid.bios = "56 4d 00 11"\nethernet0.generatedAddress = "00:0c:29:aa:bb:cc"\n')
    vmware.prepare(disk, uefi=True)
    text = vmx.read_text()
    assert 'uuid.bios = "56 4d 00 11"' in text and 'ethernet0.generatedAddress = "00:0c:29:aa:bb:cc"' in text, 'VMware-owned keys survive'
    assert 'firmware = "efi"' in text and 'sata0:1.fileName = "nexus64.iso"' in text and 'floppy0.fileName' not in text
    assert text.count('firmware = ') == 1 and 'uefi.secureBoot.enabled = "FALSE"' in text
    vmware.prepare(disk, hidden=True)
    text = vmx.read_text()
    assert 'firmware = "bios"' in text and 'sata0:1.fileName' not in text and 'uefi.secureBoot' not in text and 'uuid.bios' in text
    assert 'msg.autoAnswer = "TRUE"' in text and text.count('msg.autoAnswer') == 1, 'a window-less VM must never wait on a question'
    assert not vmware.disk_in_use_by_vmware(), 'no lock files: never reported as running'
print('PASS VMware launcher: flat VMDK over desktop64.img, NVMe/e1000e/xHCI/HDA machine, BIOS<->EFI switch keeps VMware-owned keys')
if options.unit: sys.exit(0)
if not harness.available():
    print('SKIP live VMware cases: VMware Workstation is not installed'); sys.exit(0)

CASES = {
    'bios_nvme': dict(firmware='bios', boot='floppy', root_bus='nvme'),
    'bios_sata': dict(firmware='bios', boot='floppy', root_bus='sata'),
    'bios_ide': dict(firmware='bios', boot='floppy', root_bus='ide', usb=False),
    'efi_cd': dict(firmware='efi', boot='cdrom', root_bus='nvme'),
    'efi_esp': dict(firmware='efi', boot='esp', root_bus='nvme'),
}
EXPECT = {'nvme': 'BLOCK nvme0n1 NVMe "VMware Virtual NVMe Disk"', 'sata': 'BLOCK sata0 AHCI "VMware Virtual SATA Hard Drive"',
          'ide': 'BLOCK ide0 IDE'}
failures = []
fatput = module('nexus_fatput', ROOT / 'userland/fatput.py')
for name in [c for c in options.cases.split(',') if c and c != 'audio']:
    config = CASES[name]
    vm = harness.VM(WORK, name, OUT, root=OUT / 'apps64.img', **config)
    try:
        vm.start(); took = vm.wait(['[64] ALL TESTS PASSED'], 600)
        log = vm.log()
        checks = {
            'campaign': took is not None and '[64] FAIL' not in log and 'KERNEL_FAULT' not in log,
            'firmware': f'BOOT source={"UEFI" if config["firmware"] == "efi" else "BIOS"}' in log,
            'ioapic': 'IRQ mode: IOAPIC' in log, 'smp': '[64] SMP: 2 CPUs online' in log,
            'tsc': 'source=hypervisor' in log and 'warps 0/' in log,
            'disk': EXPECT[config['root_bus']] in log,
            'dhcp': '[64] DHCP lease 192.168.' in log or '[64] DHCP lease 172.' in log or '[64] DHCP lease 10.' in log,
            'usb': not config.get('usb', True) or 'USB boot enumeration complete' in log,
        }
        bad = [k for k, ok in checks.items() if not ok]
        print(f'{"PASS" if not bad else "FAIL"} vmware {name}: ' + (f'campaign in {took:.0f} s' if took else 'campaign incomplete') +
              (f' (failed: {", ".join(bad)})' if bad else ''), flush=True)
        if bad: failures.append(name)
    finally:
        vm.cleanup()

if 'audio' in options.cases.split(','):
    build = module('nexus_build', HERE / 'build.py')
    chrome_root = OUT / 'chrome-root'; libasound = chrome_root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
    if not libasound.exists():
        print('SKIP vmware audio: no Debian libasound in out/chrome-root')
    else:
        program = WORK / 'audio64.elf'; disk = WORK / 'audio.img'; marker = WORK / 'audio.marker'
        subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                        str(HERE / 'audio_user.c'), str(libasound), '-lm', '-o', str(program)], check=True)
        shutil.copyfile(OUT / 'apps64.img', disk); marker.write_bytes(b'1w')
        install = ['fatput', str(disk), 'put', str(program), 'bin/audio64', 'put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2',
                   'put', str(marker), 'nexus/test-audio']
        for source in sorted((chrome_root / 'usr/share/alsa').rglob('*')):
            if source.is_file(): install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(chrome_root / 'usr/share/alsa').as_posix()]
        fatput.main(install)
        vm = harness.VM(WORK, 'audio', OUT, root=disk, root_bus='nvme',
                        options={'sound.present': 'TRUE', 'sound.virtualDev': 'hdaudio', 'sound.fileName': '-1',
                                 'sound.autodetect': 'TRUE', 'sound.startConnected': 'FALSE'})
        try:
            vm.start(); took = vm.wait(['[64] AUDIO CAMPAIGN PASSED'], 300); log = vm.log()
            ok = took is not None and 'HDA native PCM ready: NexusOS HD Audio 15AD:1977' in log and ' <- dac ' in log and 'AUDIO TEST PASSED failures=0' in log
            print(f'{"PASS" if ok else "FAIL"} vmware audio: HD Audio codec path and the ALSA campaign' +
                  (f' in {took:.0f} s' if took else ''), flush=True)
            if not ok: failures.append('audio')
        finally:
            vm.cleanup()
            for f in (disk, program, marker): f.unlink(missing_ok=True)

if options.internet:
    zig = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
    probe = WORK / 'netprobe.elf'; disk = WORK / 'netprobe.img'
    subprocess.run([str(zig), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', str(ROOT / 'tests/vmware_netprobe.c'), '-o', str(probe)], check=True)
    shutil.copyfile(OUT / 'apps64.img', disk)
    fatput = module('nexus_fatput', ROOT / 'userland/fatput.py'); fatput.main(['fatput', str(disk), 'put', str(probe), 'sbin/init'])
    vm = harness.VM(WORK, 'internet', OUT, root=disk, root_bus='nvme', network='nat')
    try:
        vm.start(); took = vm.wait(['NETPROBE done'], 300); log = vm.log()
        ok = took and 'NETPROBE resolv: # Generated by the NexusOS kernel' in log and 'NETPROBE dns example.com -> ' in log and 'HTTP/1.1 200' in log
        print(f'{"PASS" if ok else "FAIL"} vmware internet: DHCP on NAT, generated resolv.conf, DNS and HTTP', flush=True)
        if not ok: failures.append('internet')
    finally:
        vm.cleanup(); disk.unlink(missing_ok=True); probe.unlink(missing_ok=True)

if options.desktop:
    def digest(path):
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            while block := f.read(16 * 1024 * 1024): h.update(block)
        return h.hexdigest()
    before = digest(options.desktop)
    vm = harness.VM(WORK, 'desktop', OUT, root=options.desktop, root_bus='nvme', cpus=4, memory=4096, network='hostonly', vnc_port=5972)
    try:
        vm.start(); took = vm.wait(['AURORA READY'], 300)
        ok = took is not None
        if ok:
            time.sleep(8); vnc = harness.VNC(vm.vnc_port)
            vnc.screenshot(vm.dir / '1_desktop.png')
            vnc.key(0xFFE3, True); vnc.key(0x20); vnc.key(0xFFE3, False); time.sleep(2)     # Ctrl+Space: Spotlight
            vnc.type('terminal'); time.sleep(2); vnc.key(0xFF0D); time.sleep(8)
            vnc.type('uname -a\n'); time.sleep(4)
            vnc.screenshot(vm.dir / '2_terminal.png'); vnc.close()
            log = vm.log()
            ok = 'AURORA WINDOW app=nexus.terminal' in log and 'AURORA COMMAND DONE status=0' in log and 'KERNEL_FAULT' not in log
        print(f'{"PASS" if ok else "FAIL"} vmware desktop: Aurora ' + (f'ready in {took:.0f} s' if took else 'not ready') +
              ', USB keyboard opens Terminal through Spotlight and runs a command', flush=True)
        if not ok: failures.append('desktop')
    finally:
        vm.cleanup()
        assert digest(options.desktop) == before, 'VMware modified the desktop disk'
if failures: raise SystemExit('FAIL: ' + ', '.join(failures))
print('PASS NexusOS on VMware Workstation: ' + ', '.join([c for c in options.cases.split(',') if c] + (['internet'] if options.internet else []) +
                                                     (['desktop'] if options.desktop else [])))
