"""Bare-metal readiness paths of the native x86-64 kernel that QEMU can still exercise.

    python -I tests/test_x64_hwready.py WORK [--only CASE,...]

  tsc_pit        default calibration (PIT channel 2) - the reference frequency
  tsc_hpet       tsc=hpet: calibrated from the ACPI HPET table's counter as on a board whose PIT is gated/absent
  tsc_pmtimer    tsc=pmtimer: from the ACPI PM timer (FADT PM_TMR_BLK)
  tsc_nohpet     tsc=hpet on -machine hpet=off: no HPET table, the PM timer takes over
  pci_unassigned an e1000 behind a pci-bridge whose BAR0 is cleared at boot (pci.test-unassign): the BAR fixup
                 assigns it inside the bridge window again and the NIC still gets link and a DHCP lease
  diag_uefi      UEFI boot with `diag` in CMDLINE.TXT, twice: the loader writes the firmware part, the kernel replaces it
                 with the full NEXUSDIAG.TXT on the ESP (sections checked; FAT checked for lost/cross-linked clusters)
  diag_uefi_key  UEFI boot without the option, D pressed from power-on: BOOTX64.EFI turns diagnostics on
  diag_bios_key  BIOS boot, D pressed while stage 2 starts (QMP keys); the report lands on an attached NexusOS ESP disk
  diag_usb       UEFI boot from a USB stick (qemu-xhci + usb-storage): the loader writes its firmware part, then the
                 kernel (USB mass storage, usbstorage.c) reaches the stick and replaces it with the full report
Every case boots TCG to "[64] ALL TESTS PASSED" (no root disk); the TSC frequencies must agree within 2 %.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CASES = ['tsc_pit', 'tsc_hpet', 'tsc_pmtimer', 'tsc_nohpet', 'pci_unassigned', 'diag_uefi', 'diag_uefi_key', 'diag_bios_key', 'diag_usb']
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--only', default=','.join(CASES))
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.only.split(',') if c]
assert all(c in CASES for c in cases), cases


SHARE = Path(QEMU).parent / 'share'
QMP_PORT = 4683


def press_keys(process, key, seconds):
    """QMP: tap `key` every 40 ms for `seconds` (a key held from power-on)."""
    end = time.time() + 30
    while True:
        try: s = socket.create_connection(('127.0.0.1', QMP_PORT), timeout=5); break
        except OSError:
            if time.time() > end or process.poll() is not None: return
            time.sleep(.05)
    f = s.makefile('rwb'); f.readline()
    def cmd(name, arguments=None):
        f.write(json.dumps({'execute': name, **({'arguments': arguments} if arguments else {})}).encode() + b'\n'); f.flush()
        while True:
            reply = json.loads(f.readline())
            if 'event' not in reply: return reply
    cmd('qmp_capabilities'); cmd('cont')                                 # started paused (-S): no key is missed
    stop = time.time() + seconds
    while time.time() < stop and process.poll() is None:
        try: cmd('send-key', {'keys': [{'type': 'qcode', 'data': key}], 'hold-time': 20})
        except Exception: break
        time.sleep(.04)
    s.close()


def boot(tag, cmdline=None, machine='pc', extra=(), marker='[64] ALL TESTS PASSED', seconds=300, uefi=None, floppy=True, keys=None):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, '-machine', machine, '-accel', 'tcg', '-cpu', 'max', '-smp', '2', '-m', '512', '-display', 'none',
               '-vga', 'none', '-device', 'VGA', '-serial', f'file:{serial}', '-no-reboot',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', *extra]
    if uefi:
        code, variables = WORK / 'code.fd', WORK / (tag + '-vars.fd')
        if not code.exists(): shutil.copyfile(SHARE / 'edk2-x86_64-code.fd', code)
        shutil.copyfile(SHARE / 'edk2-i386-vars.fd', variables)
        command += ['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={code}', '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    elif floppy:
        command += ['-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a']
    if keys: command += ['-S', '-qmp', f'tcp:127.0.0.1:{QMP_PORT},server=on,wait=off']
    if cmdline: command += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        if keys: press_keys(process, *keys)
        end = time.time() + seconds
        while time.time() < end:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if marker in text or process.poll() is not None: break
            time.sleep(.25)
        process.wait(30) if process.poll() is None and marker in text else None
    finally:
        if process.poll() is None: process.kill(); process.wait()
    text = serial.read_text(errors='replace') if serial.exists() else ''
    assert marker in text and '[64] FAIL' not in text and 'KERNEL_FAULT' not in text, (tag, text[-3000:])
    return text


def esp_volume(image):
    """The ESP of a mkuefi.py GPT image (first partition entry)."""
    data = image.read_bytes()
    first, last = struct.unpack_from('<QQ', data, 2 * 512 + 32)
    return bytearray(data[first * 512:(last + 1) * 512])


def fat_files(volume):
    """Independent FAT16/32 reader: {path: bytes} with long names, plus a consistency check of the FAT (no cluster
    used twice, chain lengths match file sizes, no allocated cluster outside every chain)."""
    bps, spc, rsv, fats, root_entries = struct.unpack_from('<HBHBH', volume, 11)
    fat_size = struct.unpack_from('<H', volume, 22)[0] or struct.unpack_from('<I', volume, 36)[0]
    total = struct.unpack_from('<H', volume, 19)[0] or struct.unpack_from('<I', volume, 32)[0]
    root_sectors = (root_entries * 32 + bps - 1) // bps
    data = (rsv + fats * fat_size + root_sectors) * bps
    clusters = (total - (rsv + fats * fat_size + root_sectors)) // spc
    fat32 = clusters >= 65525
    fat = volume[rsv * bps:(rsv + fat_size) * bps]
    for copy in range(1, fats):
        assert volume[(rsv + copy * fat_size) * bps:(rsv + (copy + 1) * fat_size) * bps] == fat, 'FAT copies differ'
    def next_of(c): return struct.unpack_from('<I', fat, c * 4)[0] & 0x0FFFFFFF if fat32 else struct.unpack_from('<H', fat, c * 2)[0]
    end = 0x0FFFFFF8 if fat32 else 0xFFF8
    used = {}
    def chain(cluster, owner):
        out = []
        while 2 <= cluster < end:
            assert cluster not in used, ('cross-linked cluster', cluster, owner, used.get(cluster))
            used[cluster] = owner; out.append(cluster); cluster = next_of(cluster)
            assert len(out) <= clusters, 'chain loop'
        return out
    def read(cs): return b''.join(volume[data + (c - 2) * spc * bps:data + (c - 1) * spc * bps] for c in cs)
    files = {}
    def walk(raw, prefix):
        lfn = {}
        for at in range(0, len(raw), 32):
            e = raw[at:at + 32]
            if e[0] == 0: break
            if e[0] == 0xE5: lfn = {}; continue
            if e[11] == 0x0F:
                lfn[e[0] & 0x1F] = ''.join(chr(c) for c in struct.unpack_from('<5H', e, 1) + struct.unpack_from('<6H', e, 14) + struct.unpack_from('<2H', e, 28) if c not in (0, 0xFFFF))
                continue
            short = e[:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else '')
            name = ''.join(lfn[k] for k in sorted(lfn)) if lfn else short; lfn = {}
            if e[11] & 0x08 or name in ('.', '..'): continue
            cluster = struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0] if fat32 else struct.unpack_from('<H', e, 26)[0]
            cs = chain(cluster, prefix + name)
            if e[11] & 0x10: walk(read(cs), prefix + name + '/')
            else:
                size = struct.unpack_from('<I', e, 28)[0]
                assert len(cs) == (size + spc * bps - 1) // (spc * bps), ('chain length', prefix + name, len(cs), size)
                files[prefix + name] = read(cs)[:size]
    root = read(chain(struct.unpack_from('<I', volume, 44)[0], '/')) if fat32 else volume[(rsv + fats * fat_size) * bps:data]
    walk(root, '')
    lost = [c for c in range(2, clusters + 2) if next_of(c) and c not in used]
    assert not lost, ('allocated clusters outside every file', lost[:10], len(lost))
    return files


def check_report(text, *, kernel=True):
    assert 'NexusOS hardware diagnostics' in text, text[:500]
    if not kernel:
        assert 'FIRMWARE PART' in text and '== PCI (config space) ==' in text and '== UEFI memory map' in text and '== ACPI ==' in text, text[:3000]
        return
    for section in ('== CPU and APIC topology ==', '== Memory map ==', '== ACPI tables ==', '== PCI devices ==', '== Display ==',
                    '== Storage ==', '== Network ==', '== USB ==', '== Kernel log =='):
        assert section in text, (section, text[:3000])
    assert re.search(r'\r\n  FACP at [0-9a-f]{8} len \d+', text) and 'MTRR cap' in text and 'online CPUs (index:apic id): 0:0' in text, text[:6000]
    assert 'e1000: driving eth0' in text and 'no native GPU driver' in text and '[64] Calibrated TSC' in text, text[:20000]
    assert re.search(r'MSR 0x[0-9a-f]+ [^\r]*(: absent| = 0x)', text) and 'IA32_PAT 0x' in text, text[:6000]   # #GP-safe MSR reads


def tsc(text):
    match = re.search(r'\[64\] Calibrated TSC Hz=0x([0-9A-F]+) source=(\S+)', text)
    assert match, text[-2000:]
    return int(match.group(1), 16), match.group(2)


results, failed = {}, []
reference = None
for case in cases:
    try:
        if case == 'tsc_pit': text = boot(case)
        elif case == 'tsc_hpet': text = boot(case, 'tsc=hpet')
        elif case == 'tsc_pmtimer': text = boot(case, 'tsc=pmtimer')
        elif case == 'tsc_nohpet': text = boot(case, 'tsc=hpet', machine='pc,hpet=off')
        elif case == 'pci_unassigned':
            text = boot(case, 'pci.test-unassign', extra=['-nic', 'none', '-device', 'pci-bridge,id=br1,chassis_nr=1,addr=5',
                                                          '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0,bus=br1,addr=1'])
            assert '[64] PCI test: BAR0 of 01:01.0 cleared' in text, text[-3000:]
            match = re.search(r'\[64\] PCI 01:01\.0 BAR0 size 0x00020000 assigned 0x([0-9a-f]+) in the window of bridge 00:05\.0', text)
            assert match and '[64] PCI BAR fixup: 1 assigned, 0 left unassigned' in text, text[-3000:]
            assert 'link up' in text and 'IPv4 initialized: 10.0.2.15' in text, text[-3000:]
            results[case] = {'passed': True, 'bar0': '0x' + match.group(1)}
            print('PASS', case, results[case], flush=True); continue
        elif case == 'diag_uefi':
            image = WORK / 'diag-esp.img'; image.unlink(missing_ok=True)
            subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(OUT / 'BOOTX64.EFI'), str(OUT / 'kernel.bin'), str(image),
                            '--cmdline', 'diag diag.wait=0'], check=True, stdout=subprocess.DEVNULL)
            for round in (1, 2):                                          # the second boot replaces the first report
                text = boot(f'{case}{round}', uefi=True, extra=['-drive', f'if=none,id=esp,format=raw,file={image}', '-device', 'ide-hd,drive=esp,bus=ide.0,bootindex=0',
                                                              '-nic', 'user,model=e1000'])
                assert 'Diagnostics: firmware part written to' in text and '[64] DIAG report: NEXUSDIAG.TXT written to' in text, text[-4000:]
                files = fat_files(esp_volume(image))
                report = files['NEXUSDIAG.TXT'].decode('latin1'); check_report(report)
                assert 'FIRMWARE PART' not in report and 'GOP modes' in report and '== Display ==' in report
                assert {'EFI/BOOT/BOOTX64.EFI', 'EFI/NEXUS/KERNEL.BIN', 'EFI/NEXUS/CMDLINE.TXT'} <= set(files), sorted(files)
                assert sum(1 for name in files if name.upper() == 'NEXUSDIAG.TXT') == 1
            results[case] = {'passed': True, 'report_bytes': len(report)}; image.unlink(); (WORK / (case + '-NEXUSDIAG.TXT')).write_text(report, encoding='latin1')
            print('PASS', case, results[case], flush=True); continue
        elif case == 'diag_uefi_key':
            image = WORK / 'diag-key.img'; image.unlink(missing_ok=True)
            subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(OUT / 'BOOTX64.EFI'), str(OUT / 'kernel.bin'), str(image)],
                           check=True, stdout=subprocess.DEVNULL)
            text = boot(case, 'diag.wait=0', uefi=True, machine='q35', keys=('d', 40),     # a USB keyboard, as on most PCs
                        extra=['-device', 'qemu-xhci,id=x', '-device', 'usb-kbd,bus=x.0', '-drive', f'if=none,id=esp,format=raw,file={image}',
                               '-device', 'ide-hd,drive=esp,bus=ide.0,bootindex=0', '-nic', 'user,model=e1000'])
            assert 'D held: diagnostics boot' in text and '[64] DIAG report: NEXUSDIAG.TXT written to' in text, text[-4000:]
            report = fat_files(esp_volume(image))['NEXUSDIAG.TXT'].decode('latin1'); check_report(report)
            results[case] = {'passed': True, 'report_bytes': len(report)}; image.unlink(); (WORK / (case + '-NEXUSDIAG.TXT')).write_text(report, encoding='latin1')
            print('PASS', case, results[case], flush=True); continue
        elif case == 'diag_bios_key':
            image = WORK / 'diag-bios.img'; image.unlink(missing_ok=True)
            subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(OUT / 'BOOTX64.EFI'), str(OUT / 'kernel.bin'), str(image)],
                           check=True, stdout=subprocess.DEVNULL)
            text = boot(case, 'diag.wait=0', keys=('d', 4), extra=['-drive', f'if=none,id=disk,format=raw,file={image}', '-device', 'ide-hd,drive=disk,bus=ide.0',
                                                                   '-nic', 'user,model=e1000'])
            assert re.search(r'\[64\] BOOT source=BIOS .*cmdline="diag', text) and '[64] DIAG report: NEXUSDIAG.TXT written to' in text, text[-4000:]
            report = fat_files(esp_volume(image))['NEXUSDIAG.TXT'].decode('latin1'); check_report(report)
            assert 'BIOS E820' in report and 'EDID source: VBE/DDC' in report
            results[case] = {'passed': True, 'report_bytes': len(report)}; image.unlink(); (WORK / (case + '-NEXUSDIAG.TXT')).write_text(report, encoding='latin1')
            print('PASS', case, results[case], flush=True); continue
        elif case == 'diag_usb':
            image = WORK / 'diag-usb.img'; image.unlink(missing_ok=True)
            subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(OUT / 'BOOTX64.EFI'), str(OUT / 'kernel.bin'), str(image),
                            '--cmdline', 'diag diag.wait=0'], check=True, stdout=subprocess.DEVNULL)
            text = boot(case, uefi=True, machine='q35', extra=['-device', 'qemu-xhci,id=x', '-drive', f'if=none,id=stick,format=raw,file={image}',
                                                              '-device', 'usb-storage,bus=x.0,drive=stick,bootindex=0', '-nic', 'user,model=e1000'])
            assert 'Diagnostics: firmware part written to' in text and '[64] DIAG report: NEXUSDIAG.TXT written to sda1' in text, text[-4000:]
            report = fat_files(esp_volume(image))['NEXUSDIAG.TXT'].decode('latin1'); check_report(report)
            assert 'FIRMWARE PART' not in report, report[:2000]
            results[case] = {'passed': True, 'report_bytes': len(report)}; image.unlink(); (WORK / (case + '-NEXUSDIAG.TXT')).write_text(report, encoding='latin1')
            print('PASS', case, results[case], flush=True); continue
        hz, source = tsc(text)
        want = {'tsc_pit': 'PIT', 'tsc_hpet': 'HPET', 'tsc_pmtimer': 'ACPI-PM-timer', 'tsc_nohpet': 'ACPI-PM-timer'}[case]
        assert source == want, (case, source)
        if case == 'tsc_pit': reference = hz
        elif reference: assert abs(hz - reference) <= reference * 0.02, (case, hz, reference)
        results[case] = {'passed': True, 'hz': hz, 'source': source}
        print('PASS', case, results[case], flush=True)
    except Exception as error:
        results[case] = {'passed': False, 'error': repr(error)[:2000]}; failed.append(case)
        print('FAIL', case, repr(error)[:3000], flush=True)
(WORK / 'result.json').write_text(json.dumps(results, indent=1))
for pattern in ('*.img', '*.fd', '*.qcow2', '*.ppm'):                           # disk hygiene: no images left behind
    for path in WORK.glob(pattern): path.unlink(missing_ok=True)
assert not failed, failed
print('PASS bare-metal readiness:', ' '.join(cases))
