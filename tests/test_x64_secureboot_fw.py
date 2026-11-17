"""Firmware -> loader -> kernel Secure Boot under QEMU + OVMF (edk2-x86_64-secure-code.fd, q35 + SMM + pflash).

Enrolls the NexusOS PK/KEK/db (arch/x86_64/uefi/secureboot_efi.py) into an OVMF variable store, signs the kernel,
and boots the signed chain. Two firmware trust methods are tested:
  cert (default)  db holds only the NexusOS db X.509 cert; BOOTX64.EFI carries an Authenticode signature
                  (secureboot_efi.py sign-pe) that the firmware verifies against it
  hash (fallback) db holds the loader's Authenticode SHA-256 (hash enrollment); the loader is unsigned
Cases:
  cert-trusted    SecureBoot active, the firmware verifies the signed BOOTX64.EFI against the db cert, the loader
                  verifies KERNEL.BIN, the kernel runs its self-tests and reports "SECUREBOOT firmware=on kernel_verified=yes"
  cert-tampered   one byte of the signed BOOTX64.EFI flipped -> the firmware refuses it (Access Denied)
  cert-unsigned   the unsigned BOOTX64.EFI with only the cert in db -> refused (no signature, no hash)
  hash-trusted / hash-tampered   the same with hash enrollment
The tampered-kernel case (loader refuses) is covered by tests/test_x64_secureboot.py.

Usage: python -I tests/test_x64_secureboot_fw.py [WORKDIR]
"""
import importlib.util, shutil, struct, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
SHARE = Path(r'C:\Program Files\qemu\share')
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-securebootfw')
WORK.mkdir(parents=True, exist_ok=True)


def mod(name, p):
    s = importlib.util.spec_from_file_location(name, p); m = importlib.util.module_from_spec(s); s.loader.exec_module(m); return m
sb = mod('sb', ROOT / 'arch/x86_64/uefi/secureboot.py')
sbefi = mod('sbefi', ROOT / 'arch/x86_64/uefi/secureboot_efi.py')
if not (sb.DEFAULT_KEYDIR / 'kernel_private.pem').exists():
    raise SystemExit(f'no keys in {sb.DEFAULT_KEYDIR}; run `python -I arch/x86_64/uefi/secureboot.py gen` first')

sig = WORK / 'kernel.sig'; sb.sign(OUT / 'kernel.bin', sig, sb.DEFAULT_KEYDIR)
code = WORK / 'secure-code.fd'; shutil.copyfile(SHARE / 'edk2-x86_64-secure-code.fd', code)


signed = WORK / 'BOOTX64-signed.EFI'
sbefi.sign_pe(str(OUT / 'BOOTX64.EFI'), str(signed))
sbefi.verify_pe(signed, ROOT / 'arch/x86_64/uefi/keys/db.der')   # offline edk2-style check first


def boot(tag, loader, tamper_loader, hash_enroll):
    esp = WORK / (tag + '.img')
    subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(loader), str(OUT / 'kernel.bin'),
                    str(esp), '--kernel-sig', str(sig)], check=True)
    vars_fd = WORK / (tag + '-vars.fd')
    sbefi.enroll_varstore(str(SHARE / 'edk2-i386-vars.fd'), str(vars_fd), loader_hash=str(loader) if hash_enroll else None)
    if tamper_loader:   # corrupt the loader on the ESP AFTER its hash was enrolled -> firmware must refuse it
        raw = bytearray(esp.read_bytes())
        needle = loader.read_bytes()[4096:4160]
        at = raw.find(needle)
        assert at >= 0, 'loader not found on ESP'
        raw[at + 8] ^= 0x40; esp.write_bytes(bytes(raw))
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    cmd = [QEMU, '-machine', 'q35,smm=on', '-cpu', 'max', '-m', '256', '-smp', '2',
           '-global', 'driver=cfi.pflash01,property=secure,value=on', '-global', 'ICH9-LPC.disable_s3=1',
           '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={code}',
           '-drive', f'if=pflash,format=raw,unit=1,file={vars_fd}',
           '-drive', f'if=none,id=esp,format=raw,snapshot=on,file={esp}', '-device', 'ide-hd,drive=esp,bus=ide.2,bootindex=0',
           '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
           '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try: p.communicate(timeout=120)
    except subprocess.TimeoutExpired: p.kill(); p.communicate()
    esp.unlink(missing_ok=True); vars_fd.unlink(missing_ok=True)
    return p.returncode, serial.read_text(errors='replace')


def trusted(tag, loader, hash_enroll):
    rc, log = boot(tag, loader, False, hash_enroll)
    assert rc == 33, (tag + ': expected exit 33', rc, log[-2500:])
    assert 'Firmware Secure Boot: active' in log, (tag + ': firmware SB not active', log[-2500:])
    assert '[64] SECUREBOOT firmware=on kernel_verified=yes' in log, (tag + ': kernel SB report', log[-2500:])
    assert '[64] ALL TESTS PASSED' in log, (tag + ': kernel did not run', log[-2500:])


def refused(tag, loader, tamper, hash_enroll):
    rc, log = boot(tag, loader, tamper, hash_enroll)
    assert 'Access Denied' in log or 'Security Violation' in log or 'failed to load' in log, (tag + ': firmware did not refuse the loader', log[-2500:])
    assert '[64] ALL TESTS PASSED' not in log, (tag + ': refused loader still ran the kernel!', log[-2500:])
    assert rc != 33, (tag + ': boot unexpectedly clean', rc)


trusted('cert-trusted', signed, False)
print('PASS cert-trusted: SecureBoot=1, the firmware verified the Authenticode-signed loader against the db cert, the loader verified the kernel')
refused('cert-tampered', signed, True, False)
print('PASS cert-tampered: the firmware refused the tampered signed BOOTX64.EFI; the kernel never ran')
refused('cert-unsigned', OUT / 'BOOTX64.EFI', False, False)
print('PASS cert-unsigned: with only the db cert enrolled the unsigned BOOTX64.EFI is refused')
trusted('hash-trusted', OUT / 'BOOTX64.EFI', True)
print('PASS hash-trusted: hash enrollment (fallback) boots the unsigned loader')
refused('hash-tampered', OUT / 'BOOTX64.EFI', True, True)
print('PASS hash-tampered: the firmware refused the tampered BOOTX64.EFI')
signed.unlink(missing_ok=True); code.unlink(missing_ok=True)
print('PASS firmware Secure Boot: PK/KEK/db enrolled, SecureBoot=1, cert-signed and hash-enrolled loaders boot, tampered/unsigned refused')
