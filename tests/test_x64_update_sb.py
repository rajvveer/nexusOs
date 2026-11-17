"""Core updates under UEFI Secure Boot, release signing, and the refusal of A/B slots on a FAT32 root.

    python -I tests/test_x64_update_sb.py WORK --base BASE [--accel auto|whpx|tcg] [--qmp-port 4772] [--only fat,sb,release]

release  (host) update_bundle.py release-key + release with a scratch key folder and a scratch copy of the tree keyring:
         the bundle carries kernel.sig (made with the Secure Boot kernel key) and the db-signed loader and passes
         `verify`; a release with a private key that is not the tree's is refused; `verify` refuses a bundle whose
         kernel.sig is another key's, an unsigned kernel, and a member changed after signing; release-key never
         replaces an existing key without --force.
fat      (Secure Boot off, out/nexus64-uefi.img) a FAT32 root that holds /boot/nexus/bootctl and a trial slot: the loader
         prints that slots on a FAT root are not used and boots the ESP kernel (no nexus.slot on the command line).
sb       (OVMF secure-code + SMM, the NexusOS PK/KEK/db enrolled, the db-signed loader with KERNEL.SIG) on BASE (an
         installed system, tests/test_x64_services.py --keep-installed):
  boot 1  Secure Boot is on (firmware + kernel verified; nexus.secureboot=1 for the system); nexus-update refuses a
          bundle whose kernel has no kernel.sig and one whose kernel.sig is another key's; the good bundle (kernel +
          kernel.sig + signed loader + Aurora) goes into slot b on trial
  boot 2  slot b on trial: the firmware accepts slot b's signed loader (chained), the slot kernel verifies; the boot
          check makes b the default; the hook then plants, as an attacker with root on the disk would, a slot-a kernel
          with a broken kernel.sig and a trial for it
  boot 3  the loader refuses slot a's kernel before it runs (signature FAILED) and boots slot b with
          nexus.rollback=a; the hook plants an unsigned slot-a kernel on trial
  boot 4  the unsigned trial kernel is refused (Secure Boot active) and slot b boots; the hook replaces slot b's loader
          by an unsigned one
  boot 5  the firmware refuses slot b's unsigned loader; the first-stage loader boots slot b's verified kernel itself
Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import importlib.util
import io
import json
import re
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4772)
ap.add_argument('--only', default='release,fat,sb')
args = ap.parse_args()
ONLY = set(args.only.split(','))
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
OUT = ROOT / 'arch/x86_64/out'; HERE = ROOT / 'arch/x86_64'
BUNDLER = HERE / 'update_bundle.py'
SHARE = Path(pkg_vm.QEMU).parent / 'share'
results = {'test': 'update_sb', 'checks': [], 'passed': False}


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def bundler(*argv, ok=True):
    p = subprocess.run([sys.executable, '-I', str(BUNDLER), *map(str, argv)], capture_output=True, text=True)
    if ok and p.returncode: raise RuntimeError('update_bundle.py %s failed: %s' % (argv[0], (p.stdout + p.stderr)[-600:]))
    return p


def rsa_wrong_signature(data):
    """A kernel.sig of the right size made by another RSA-2048 key."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, rsa
    return rsa.generate_private_key(public_exponent=65537, key_size=2048).sign(data, padding.PKCS1v15(), hashes.SHA256())


def repack(source, target, drop=(), replace=None):
    """A copy of a bundle without some members or with one member's bytes replaced (the manifest stays as signed)."""
    src = tarfile.open(source); buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for m in src.getmembers():
            if m.name in drop: continue
            data = src.extractfile(m).read()
            if replace and m.name == replace[0]: data = replace[1]
            m.size = len(data); tar.addfile(m, io.BytesIO(data))
    target.write_bytes(buffer.getvalue())


# ---------------------------------------------------------------------------------------------- host: release signing
def release_cases():
    keydir, tree = WORK / 'release-key', WORK / 'tree-keys' / 'nexusos-update.pgp'
    shutil.rmtree(keydir, ignore_errors=True); shutil.rmtree(tree.parent, ignore_errors=True)
    p = bundler('release-key', '--keydir', keydir, '--tree-keyring', tree)
    check('release-key: private key outside the tree, public keyring + fingerprint written', (keydir / 'update-key.json').is_file() and tree.is_file()
          and tree.with_suffix('.fingerprint').is_file() and not str(keydir).startswith(str(ROOT)), p.stdout[-300:])
    p = bundler('release-key', '--keydir', keydir, '--tree-keyring', tree, ok=False)
    check('release-key: an existing key is never replaced without --force', p.returncode != 0 and 'exists already' in p.stderr, p.stderr[-300:])
    good = WORK / 'release.nxu'
    p = bundler('release', good, '--version', '2026.10.30.1', '--keydir', keydir, '--tree-keyring', tree, ok=False)
    check('release: builds and verifies a bundle with kernel.sig and the signed loader', p.returncode == 0 and 'verifies' in p.stdout, (p.stdout + p.stderr)[-500:])
    names = tarfile.open(good).getnames() if good.is_file() else []
    check('release: members MANIFEST, MANIFEST.sig, kernel.bin, kernel.sig, init, BOOTX64.EFI', names == ['MANIFEST', 'MANIFEST.sig', 'kernel.bin', 'kernel.sig', 'init', 'BOOTX64.EFI'], names)
    other = WORK / 'other-key'; shutil.rmtree(other, ignore_errors=True); bundler('keygen', other)
    p = bundler('release', WORK / 'wrong.nxu', '--version', '2026.10.30.1', '--keydir', other, '--tree-keyring', tree, ok=False)
    check('release: a private key that is not the tree\'s is refused', p.returncode != 0 and 'is not the one in' in p.stderr and not (WORK / 'wrong.nxu').exists(), p.stderr[-300:])
    if not good.is_file(): return
    data = {m.name: tarfile.open(good).extractfile(m).read() for m in tarfile.open(good).getmembers()}
    cases = {'nosig': dict(drop=('kernel.sig',)), 'othersig': dict(replace=('kernel.sig', rsa_wrong_signature(data['kernel.bin']))),
             'tampered': dict(replace=('init', data['init'][:-1] + bytes([data['init'][-1] ^ 1])))}
    for name, change in cases.items():
        repack(good, WORK / (name + '.nxu'), **change)
        p = bundler('verify', WORK / (name + '.nxu'), '--keyring', tree, ok=False)
        check('verify refuses %s' % name, p.returncode != 0 and 'refused' in p.stderr, (p.stdout + p.stderr)[-300:])
    p = bundler('verify', good, '--keyring', WORK / 'other-key' / 'nexusos-update.pgp', ok=False)
    check('verify refuses a bundle signed by another release key', p.returncode != 0 and 'signature does not verify' in p.stderr, p.stderr[-300:])


# ---------------------------------------------------------------------------------------------- FAT root: refused
def fat_case():
    fatimage = module('sb_fatimage', ROOT / 'tests/fatimage.py')
    img = fatimage.FatImage(48 << 20, 32, label='NEXUSFAT', cluster_sectors=1)
    img.mkdir('boot'); img.mkdir('boot/nexus'); img.mkdir('boot/nexus/slot-b')
    img.put('boot/nexus/bootctl', b'default=a\ntrial=b\ntries=2\nnonce=0123abcd\n')
    img.put('boot/nexus/bootcount', b'NXBC1 nonce=0123abcd count=0'.ljust(64, b'\n'))
    img.put('boot/nexus/slot-b/kernel.bin', (OUT / 'kernel.bin').read_bytes())
    disk = WORK / 'fatroot.img'; disk.write_bytes(img.finish())
    vm = pkg_vm.Vm(WORK, disk, args.qmp_port, tag='fat', accel=args.accel, smp=2, memory='1024', uefi=OUT / 'nexus64-uefi.img')
    counter = None
    try:
        try: vm.wait(r'\[64\] BOOT source=UEFI[^\n]*', 0, 240, fail=None)
        except (TimeoutError, AssertionError): pass
        time.sleep(2); log = vm.text()
        try: vm.q.cmd('quit')
        except Exception: pass
        vm.exited(30)
        view = pkg_vm.Qcow2View(str(vm.overlay))   # what the guest (firmware, loader) wrote lands in the overlay
        try: counter = fatimage.FatVolume(view.read_at(0, view.size)).read('/boot/nexus/bootcount')
        finally: view.close()
    finally:
        vm.close(); disk.unlink(missing_ok=True)
    boot = re.search(r'\[64\] BOOT source=UEFI[^\n]*', log)
    check('fat: the loader refuses slots on a FAT32 root with a message', '[uefi] A/B refused: slots on a FAT root are not used' in log, log[-600:])
    check('fat: the ESP kernel boots, no slot', boot is not None and 'nexus.slot=' not in boot.group(0) and '[uefi] A/B kernel from slot' not in log and
          '[uefi] A/B trial' not in log, boot.group(0) if boot else log[-300:])
    check('fat: the FAT disk\'s trial counter is untouched (no attempt counted)', counter == b'NXBC1 nonce=0123abcd count=0'.ljust(64, b'\n'), counter)


# ---------------------------------------------------------------------------------------------- Secure Boot A/B
COMMON = '''PATH=/usr/sbin:/usr/bin:/sbin:/bin
H=@HOST@/files
ok() { echo "UPD $1 PASS ${2:-}"; }
bad() { echo "UPD $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/upd.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/upd.out | tr '\\n' ' ')"; else bad "$name" "$(tail -c 600 /tmp/upd.out | tr '\\n' ' ')"; fi; }
refused() { name=$1; why=$2; shift 2; if "$@" >/tmp/upd.out 2>&1; then bad "$name" "accepted: $(tail -c 300 /tmp/upd.out | tr '\\n' ' ')";
  elif grep -q "$why" /tmp/upd.out; then ok "$name" "$(tail -c 300 /tmp/upd.out | tr '\\n' ' ')"; else bad "$name" "other error: $(tail -c 300 /tmp/upd.out | tr '\\n' ' ')"; fi; }
get() { wget -q -O "/tmp/$1" "$H/$1"; }
trial_a() {   # what nexus-update apply would leave: slot a pending with a new nonce (default b)
  printf '# planted by the test\\ndefault=b\\ntrial=a\\ntries=2\\nnonce=%s\\n' "$1" > /boot/nexus/bootctl
  t="NXBC1 nonce=$1 count=0"; { printf '%s' "$t"; i=${#t}; while [ $i -lt 64 ]; do printf '\\n'; i=$((i + 1)); done; } > /boot/nexus/bootcount; sync
}
echo "UPD cmdline $(cat /proc/cmdline)"
'''
HOOKS = {
    1: COMMON + '''
check secure-boot-flag sh -c 'grep -q "nexus.secureboot=1" /proc/cmdline && nexus-update status | grep -q "Secure Boot:   active"'
check root-is-ext4 sh -c '[ "$(stat -f -c %t /)" = ef53 ] && [ -s /usr/share/nexus/secureboot/kernel.crt ]'
get nosig.nxu; refused refused-unsigned-kernel "no Secure Boot signature" nexus-update apply /tmp/nosig.nxu
get othersig.nxu; refused refused-wrong-kernel-signature "does not verify" nexus-update apply /tmp/othersig.nxu
check nothing-installed sh -c '! [ -e /boot/nexus/slot-b ]'
get good.nxu; check apply-good nexus-update apply /tmp/good.nxu
check pending-b sh -c 'grep -qx "trial=b" /boot/nexus/bootctl && [ -s /boot/nexus/slot-b/kernel.sig ] && [ -s /boot/nexus/slot-b/BOOTX64.EFI ]'
''',
    2: COMMON + '''
check booted-slot-b sh -c 'grep -q "nexus.slot=b" /proc/cmdline && grep -q "nexus.trial=" /proc/cmdline'
check marked-good sh -c 'grep -qx "default=b" /boot/nexus/bootctl && grep -qx "trial=none" /boot/nexus/bootctl'
mkdir -p /boot/nexus/slot-a && cp /boot/nexus/slot-b/kernel.bin /boot/nexus/slot-a/kernel.bin &&
  { head -c 255 /boot/nexus/slot-b/kernel.sig; printf 'X'; } > /boot/nexus/slot-a/kernel.sig && trial_a 1111aaaa
check planted-bad-signature sh -c '[ $(wc -c < /boot/nexus/slot-a/kernel.sig) = 256 ] && ! cmp -s /boot/nexus/slot-a/kernel.sig /boot/nexus/slot-b/kernel.sig && grep -qx "trial=a" /boot/nexus/bootctl'
''',
    3: COMMON + '''
check rolled-back sh -c 'grep -q "nexus.rollback=a" /proc/cmdline && grep -q "nexus.slot=b" /proc/cmdline'
rm -f /boot/nexus/slot-a/kernel.sig && trial_a 2222bbbb
check planted-unsigned sh -c '! [ -e /boot/nexus/slot-a/kernel.sig ] && grep -qx "trial=a" /boot/nexus/bootctl'
''',
    4: COMMON + '''
check rolled-back-unsigned sh -c 'grep -q "nexus.rollback=a" /proc/cmdline && grep -q "nexus.slot=b" /proc/cmdline'
check no-trial-left sh -c 'grep -qx "default=b" /boot/nexus/bootctl && grep -qx "trial=none" /boot/nexus/bootctl'
cp /boot/nexus/slot-b/BOOTX64.EFI /boot/nexus/slot-b/BOOTX64.signed && cp /usr/lib/nexus/tests/unsigned-loader.efi /boot/nexus/slot-b/BOOTX64.EFI && sync
check planted-unsigned-loader sh -c '! cmp -s /boot/nexus/slot-b/BOOTX64.EFI /boot/nexus/slot-b/BOOTX64.signed'
''',
    5: COMMON + '''
check default-b-again sh -c 'grep -q "nexus.slot=b" /proc/cmdline && ! grep -q "nexus.trial=" /proc/cmdline'
mv -f /boot/nexus/slot-b/BOOTX64.signed /boot/nexus/slot-b/BOOTX64.EFI; nexus-update status | sed 's/^/UPD status| /'
''',
}


def sb_case():
    base = args.base.resolve() if args.base else None
    if not base or not base.is_file(): raise SystemExit('--base: an installed system (tests/test_x64_services.py --keep-installed)')
    sbm = module('sb_secureboot', HERE / 'uefi/secureboot.py'); efi = module('sb_efi', HERE / 'uefi/secureboot_efi.py')
    if not (sbm.DEFAULT_KEYDIR / 'kernel_private.pem').is_file(): raise SystemExit('no Secure Boot keys in %s (uefi/secureboot.py gen)' % sbm.DEFAULT_KEYDIR)
    # firmware: secure code + an enrolled variable store; ESP: the db-signed loader, the kernel and its KERNEL.SIG
    sig = WORK / 'kernel.sig'; sbm.sign(OUT / 'kernel.bin', sig, sbm.DEFAULT_KEYDIR)
    loader = WORK / 'BOOTX64-signed.EFI'; efi.sign_pe(str(OUT / 'BOOTX64.EFI'), str(loader)); efi.verify_pe(loader, HERE / 'uefi/keys/db.der')
    esp = WORK / 'esp-signed.img'; esp.unlink(missing_ok=True)
    subprocess.run([sys.executable, '-I', str(ROOT / 'mkuefi.py'), str(loader), str(OUT / 'kernel.bin'), str(esp), '--kernel-sig', str(sig)], check=True, stdout=subprocess.DEVNULL)
    code = WORK / 'secure-code.fd'; shutil.copyfile(SHARE / 'edk2-x86_64-secure-code.fd', code)
    varstore = WORK / 'vars.fd'; efi.enroll_varstore(str(SHARE / 'edk2-i386-vars.fd'), str(varstore))
    # bundles (a per-run release key; the good one exactly as `release` makes them)
    keys = WORK / 'keys'; shutil.rmtree(keys, ignore_errors=True); bundler('keygen', keys)
    b = WORK / 'bundles'; b.mkdir(exist_ok=True)
    bundler('build', b / 'good.nxu', '--version', '2026.10.30.2', '--key', keys / 'update-key.json', '--kernel-sig', sig, '--loader', loader)
    bundler('build', b / 'nosig.nxu', '--version', '2026.10.30.2', '--key', keys / 'update-key.json', '--loader', loader)
    wrong = WORK / 'wrong.sig'; wrong.write_bytes(rsa_wrong_signature((OUT / 'kernel.bin').read_bytes()))
    bundler('build', b / 'othersig.nxu', '--version', '2026.10.30.2', '--key', keys / 'update-key.json', '--kernel-sig', wrong, '--loader', loader)
    files = {p.name: p.read_bytes() for p in b.glob('*.nxu')}
    keyring = (keys / 'nexusos-update.pgp').read_bytes()

    def stage(fs):
        pkg_vm.refresh_services(fs)
        pkg_vm.put(fs, '/usr/share/keyrings/nexusos-update.pgp', keyring)
        pkg_vm.put(fs, '/etc/nexus/update.conf', 'TRIES=2\n')
        pkg_vm.put(fs, '/usr/lib/nexus/tests/unsigned-loader.efi', (OUT / 'BOOTX64.EFI').read_bytes())
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage), flush=True)
    server = pkg_vm.HookServer(b'', files, WORK / 'uploads')
    firmware = ['-global', 'driver=cfi.pflash01,property=secure,value=on', '-global', 'ICH9-LPC.disable_s3=1']
    logs = {}
    try:
        for n in range(1, 6):
            server.hook = HOOKS[n].encode()
            vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='sb%d' % n, accel=args.accel, smp=2, memory='2048',
                           fresh_overlay=(n == 1), overlay_name='boot.qcow2', uefi=esp, uefi_pflash=(code, varstore), extra=firmware,
                           hook_port=server.port)
            try:
                vm.exited(1800)
                logs[n] = vm.text()
            finally:
                vm.close(keep_overlay=True)
            log = logs[n]
            for m in re.finditer(r'^UPD (\S+) (PASS|FAIL) ?(.*)$', log, re.M): check('sb%d %s' % (n, m.group(1)), m.group(2) == 'PASS', m.group(3))
            check('sb%d: the guest checks ran' % n, 'TEST-HOOK finished' in log, log[-300:])
            check('sb%d: Secure Boot on, kernel verified' % n, '[64] SECUREBOOT firmware=on kernel_verified=yes' in log, re.findall(r'\[64\] SECUREBOOT[^\n]*', log)[:2])
            check('sb%d: no kernel fault' % n, 'KERNEL_FAULT' not in log, re.findall(r'.*KERNEL_FAULT.*', log)[:2])
    finally:
        server.close()
    L = logs
    check('sb1: the ESP kernel (no slots yet)', 1 in L and '[uefi] A/B kernel from slot' not in L[1])
    check('sb2: slot b on trial; the firmware accepts its signed loader (chained); its kernel verifies', 2 in L and '[uefi] A/B trial slot=b attempt=1' in L[2] and
          '[uefi] A/B chained loader slot=b' in L[2] and '[uefi] A/B kernel from slot=b' in L[2])
    check('sb3: a broken slot signature is refused before the kernel runs; slot b boots', 3 in L and '[uefi] A/B trial slot=a attempt=1' in L[3] and
          '[uefi] A/B slot kernel signature FAILED' in L[3] and '[uefi] A/B unusable kernel in slot=a' in L[3] and '[uefi] A/B kernel from slot=b' in L[3])
    check('sb4: an unsigned trial kernel is refused while Secure Boot is active', 4 in L and '[uefi] A/B slot kernel unsigned while Secure Boot is active' in L[4] and
          '[uefi] A/B unusable kernel in slot=a' in L[4])
    check('sb5: the firmware refuses slot b\'s unsigned loader; the first stage boots slot b\'s kernel', 5 in L and
          '[uefi] A/B the firmware refused the slot loader' in L[5] and '[uefi] A/B chained loader' not in L[5] and '[uefi] A/B kernel from slot=b' in L[5])


started = time.time()
try:
    if not (OUT / 'kernel.bin').is_file() or not (OUT / 'nexus64-uefi.img').is_file(): raise SystemExit('build first (arch/x86_64/build.py build)')
    if 'release' in ONLY: release_cases()
    if 'fat' in ONLY: fat_case()
    if 'sb' in ONLY: sb_case()
finally:
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    for pattern in ('*.qcow2', '*.img', '*.fd', '*.nxu'):
        for f in WORK.glob(pattern): f.unlink(missing_ok=True)
    for d in ('bundles',): shutil.rmtree(WORK / d, ignore_errors=True)
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
