"""Secure-boot chain: the UEFI loader (BOOTX64.EFI) cryptographically verifies KERNEL.BIN before jumping to it.

Uses the NexusOS kernel signing key (uefi/secureboot.py; private key in SECUREBOOT_KEYDIR, public modulus embedded
in the loader via uefi/sb_pubkey.h). Signs out/kernel.bin, stages KERNEL.SIG on a UEFI ESP image, and boots it under
QEMU + OVMF (TCG). Cases:
  signed     a valid signature -> the loader prints "KERNEL.BIN signature: verified", the kernel runs its self-tests
             and exits 33; the kernel reports "SECUREBOOT firmware=off kernel_verified=yes".
  tampered   one byte of the signature flipped -> the loader refuses ("signature verification FAILED") and the
             kernel never runs (no "ALL TESTS PASSED").

This verifies the loader->kernel link of the chain under ordinary (non-enrolled) OVMF. The firmware->loader link
(Authenticode on BOOTX64.EFI verified against enrolled db/KEK/PK) needs an enrolled OVMF varstore + an Authenticode
signature; see HANDOFF_CODEX.md for the enrollment procedure and the honest status of that step.

Usage: python -I tests/test_x64_secureboot.py [WORKDIR]
"""
import os
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
SHARE = QEMU.parent / 'share'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-secureboot')
WORK.mkdir(parents=True, exist_ok=True)
KEYDIR = Path(os.environ.get('SECUREBOOT_KEYDIR', str(Path.home() / '.nexusos' / 'keys' / 'secureboot')))

sb = importlib.util.module_from_spec(importlib.util.spec_from_file_location('sb', ROOT / 'arch/x86_64/uefi/secureboot.py'))
importlib.util.spec_from_file_location('sb', ROOT / 'arch/x86_64/uefi/secureboot.py').loader.exec_module(sb)

if not (KEYDIR / 'kernel_private.pem').exists():
    print(f'kernel signing key missing in {KEYDIR}; generating (also rewrites uefi/sb_pubkey.h - rebuild the loader after)')
    sb.gen(KEYDIR)
    raise SystemExit('Generated fresh keys; re-run `build.py build` so the loader embeds the new public key, then re-run this test.')

CODE = WORK / 'edk2-x86_64-code.fd'; shutil.copyfile(SHARE / 'edk2-x86_64-code.fd', CODE)
VARS = WORK / 'vars.fd'; shutil.copyfile(SHARE / 'edk2-i386-vars.fd', VARS)


def build_uefi_image(sig_path, image_path):
    subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(OUT / 'BOOTX64.EFI'), str(OUT / 'kernel.bin'),
                    str(image_path), '--kernel-sig', str(sig_path)], check=True)


def boot(tag, image, timeout=120):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    vars_copy = WORK / (tag + '-vars.fd'); shutil.copyfile(VARS, vars_copy)
    command = [str(QEMU), '-machine', 'q35', '-cpu', 'max', '-smp', '2', '-m', '256', '-display', 'none', '-vga', 'none',
               '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
               '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={CODE}',
               '-drive', f'if=pflash,format=raw,unit=1,file={vars_copy}',
               '-drive', f'if=none,id=esp,format=raw,snapshot=on,file={image}',
               '-device', 'ide-hd,drive=esp,bus=ide.2,bootindex=0',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    start = time.monotonic()
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    rc = None
    try:
        proc.communicate(timeout=timeout); rc = proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill(); proc.communicate()
    log = serial.read_text(errors='replace')
    print(f'  {tag}: rc={rc} in {time.monotonic() - start:.1f}s')
    return rc, log


# --- sign the real kernel and boot it ---
good_sig = WORK / 'kernel.sig'
sb.sign(OUT / 'kernel.bin', good_sig, KEYDIR)
good_img = WORK / 'uefi-signed.img'; build_uefi_image(good_sig, good_img)
rc, log = boot('signed', good_img)
assert rc == 33, ('signed: expected exit 33', rc, log[-3000:])
assert 'KERNEL.BIN signature: verified' in log, ('signed: loader did not verify', log[-3000:])
assert '[64] ALL TESTS PASSED' in log, ('signed: kernel did not run', log[-3000:])
assert '[64] SECUREBOOT firmware=off kernel_verified=yes' in log, ('signed: kernel SB report', log[-3000:])
print('PASS signed: loader verified KERNEL.BIN, kernel ran, reported kernel_verified=yes')

# --- tamper the signature: the loader must refuse and the kernel must not run ---
bad = bytearray(good_sig.read_bytes()); bad[100] ^= 0x40
bad_sig = WORK / 'kernel-bad.sig'; bad_sig.write_bytes(bytes(bad))
bad_img = WORK / 'uefi-tampered.img'; build_uefi_image(bad_sig, bad_img)
rc, log = boot('tampered', bad_img, timeout=60)
assert 'signature verification FAILED' in log, ('tampered: loader did not refuse', log[-3000:])
assert '[64] ALL TESTS PASSED' not in log, ('tampered: kernel ran despite a bad signature!', log[-3000:])
assert rc != 33, ('tampered: unexpected clean exit', rc)
print('PASS tampered: loader refused a bad KERNEL.BIN signature; kernel never ran')

for f in WORK.glob('*.img'): f.unlink(missing_ok=True)
print('PASS secure-boot chain: loader cryptographically verifies the kernel (RSA-2048/SHA-256)')
