#!/usr/bin/env python3
"""NexusOS core update bundles (kernel + Aurora + UEFI loader) for nexus-update's A/B slots.

    python -I arch/x86_64/update_bundle.py release-key [--keydir DIR] [--uid UID] [--force]
    python -I arch/x86_64/update_bundle.py release OUT.nxu --version V [--keydir DIR] [--no-init] [--no-loader]
    python -I arch/x86_64/update_bundle.py verify BUNDLE [--keyring arch/x86_64/keys/nexusos-update.pgp]
    python -I arch/x86_64/update_bundle.py keygen KEYDIR [--uid "NexusOS updates <updates@nexus.invalid>"]
    python -I arch/x86_64/update_bundle.py build OUT.nxu --version V (--key KEYDIR/update-key.json | --sign-command CMD)
                                          [--kernel out/kernel.bin] [--init out/desktop.elf] [--loader out/BOOTX64.EFI]
    python -I arch/x86_64/update_bundle.py show OUT.nxu

Release signing (the NexusOS release key):
  release-key   creates the release key ONCE: the private key goes to DIR (default: NEXUS_UPDATE_KEYDIR, else
                %USERPROFILE%\\.nexusos\\keys\\update; outside the tree, never on a NexusOS disk: back it up), its public
                keyring to arch/x86_64/keys/nexusos-update.pgp (+ .fingerprint), which sysinstall.py installs as
                /usr/share/keyrings/nexusos-update.pgp. An existing key is never replaced without --force (systems that
                trust the old key would refuse every later bundle).
  release       builds a release bundle of this tree's build: out/kernel.bin + its Secure Boot signature kernel.sig
                (made now with the kernel key of uefi/secureboot.py, SECUREBOOT_KEYDIR), out/desktop.elf, and the
                Secure Boot-signed loader (out/BOOTX64.EFI signed by the db key, checked against uefi/keys/db.der);
                signed with the release key after checking it is the one whose public half is in the tree; then
                verified like `verify`.
  verify        checks a bundle the way a NexusOS system does: the MANIFEST signature against the keyring, every
                member's size + SHA-256, regular files only, and kernel.sig against uefi/keys/kernel.crt.

Bundle (format 1): an uncompressed POSIX tar whose members are, in this order,
  MANIFEST       UTF-8 text:   NexusOS-Update: 1
                               Version: <version, dpkg ordering, [0-9A-Za-z.+~-]>
                               Built: <UTC time>
                               Signature: openpgp-detached
                               File: <name> <bytes> <sha256>     (one line per payload member)
  MANIFEST.sig   the signature of MANIFEST (here: a binary OpenPGP detached signature, RSA-3072/SHA-512)
  kernel.bin     the kernel (header NXKRNL64; the UEFI loader rejects anything else and rolls back)
  init           Aurora (/boot/nexus/slot-X/init, run as init when that slot boots)
  BOOTX64.EFI    the slot's UEFI loader (optional; the first-stage loader on the ESP starts it)
Only MANIFEST is signed; every payload is bound to it by size and SHA-256. On the system, nexus-update checks the
signature with VERIFY_COMMAND from /etc/nexus/update.conf (default: sqv --keyring /usr/share/keyrings/nexusos-update.pgp,
called as COMMAND SIGNATURE MANIFEST, exit status 0 = good). Another signature scheme only needs a different
"Signature:" value, a signer here (--sign-command "CMD" gets MANIFEST on stdin and writes the signature to stdout)
and a matching VERIFY_COMMAND; nothing else depends on the format.

keygen writes KEYDIR/update-key.json (private: keep it off the NexusOS disks) and KEYDIR/nexusos-update.pgp (the
public keyring to install as /usr/share/keyrings/nexusos-update.pgp).
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import io
import re
import subprocess
import sys
import tarfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
MEMBERS = ('kernel.bin', 'kernel.sig', 'init', 'BOOTX64.EFI')
VERSION_RE = re.compile(r'[0-9][0-9A-Za-z.+~-]{0,63}')


def default_keydir():
    import os
    value = os.environ.get('NEXUS_UPDATE_KEYDIR')
    return Path(value) if value else Path.home() / '.nexusos' / 'keys' / 'update'


TREE_KEYRING = HERE / 'keys' / 'nexusos-update.pgp'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def private_file(path, text):
    """Write a private key file readable by its owner only (POSIX mode 0600; on Windows the folder is the user's own)."""
    import os
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f: f.write(text)


def openpgp_packets(data):
    """(tag, body) of each packet (new and old formats)."""
    out, i = [], 0
    while i < len(data):
        head = data[i]; i += 1
        if not head & 0x80: raise ValueError('not an OpenPGP packet')
        if head & 0x40:
            tag = head & 0x3F; first = data[i]; i += 1
            if first < 192: n = first
            elif first < 224: n = ((first - 192) << 8) + data[i] + 192; i += 1
            elif first == 255: n = int.from_bytes(data[i:i + 4], 'big'); i += 4
            else: raise ValueError('partial packet lengths are not used here')
        else:
            tag = (head >> 2) & 0x0F; kind = head & 3; size = {0: 1, 1: 2, 2: 4}.get(kind)
            if size is None: raise ValueError('indeterminate packet length')
            n = int.from_bytes(data[i:i + size], 'big'); i += size
        if i + n > len(data): raise ValueError('truncated packet')
        out.append((tag, data[i:i + n])); i += n
    return out


def openpgp_verify(keyring, signature, document):
    """True when `signature` (a binary v4 RSA/SHA-512 or SHA-256 detached signature of a binary document) was made by the
    first public key of `keyring`."""
    import hashlib as h
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils
    from cryptography.exceptions import InvalidSignature
    keys = [b for t, b in openpgp_packets(keyring) if t == 6]
    sigs = [b for t, b in openpgp_packets(signature) if t == 2]
    if len(keys) < 1 or len(sigs) != 1: return False
    key, sig = keys[0], sigs[0]
    if key[0] != 4 or key[5] != 1: return False
    def mpi(buf, at):
        bits = int.from_bytes(buf[at:at + 2], 'big'); n = (bits + 7) // 8
        return int.from_bytes(buf[at + 2:at + 2 + n], 'big'), at + 2 + n
    n, at = mpi(key, 6); e, _ = mpi(key, at)
    if sig[0] != 4 or sig[1] != 0 or sig[2] != 1 or sig[3] not in (8, 10): return False
    hashed_len = int.from_bytes(sig[4:6], 'big'); head = sig[:6 + hashed_len]
    unhashed_len = int.from_bytes(sig[6 + hashed_len:8 + hashed_len], 'big'); at = 8 + hashed_len + unhashed_len
    left16 = sig[at:at + 2]; value, _ = mpi(sig, at + 2)
    algo = h.sha512 if sig[3] == 10 else h.sha256
    digest = algo(document + head + b'\x04\xff' + len(head).to_bytes(4, 'big')).digest()
    if digest[:2] != left16: return False
    public = rsa.RSAPublicNumbers(e, n).public_key()
    try:
        public.verify(value.to_bytes((n.bit_length() + 7) // 8, 'big'), digest, padding.PKCS1v15(),
                      utils.Prehashed(hashes.SHA512() if sig[3] == 10 else hashes.SHA256()))
        return True
    except InvalidSignature: return False


def verify_bundle(bundle, keyring):
    """Raise SystemExit with the reason unless the bundle is one a NexusOS system would install."""
    problems = []
    with tarfile.open(bundle) as tar:
        members = tar.getmembers(); names = [m.name for m in members]
        if any(not m.isreg() for m in members): problems.append('a member is not a regular file')
        if len(set(names)) != len(names): problems.append('duplicate members')
        if names[:2] != ['MANIFEST', 'MANIFEST.sig']: problems.append('MANIFEST and MANIFEST.sig must come first')
        data = {m.name: tar.extractfile(m).read() for m in members if m.isreg()}
    manifest = data.get('MANIFEST', b'')
    if not openpgp_verify(Path(keyring).read_bytes(), data.get('MANIFEST.sig', b''), manifest): problems.append('the MANIFEST signature does not verify with ' + str(keyring))
    lines = manifest.decode('utf-8', 'replace').splitlines()
    if not lines or lines[0] != 'NexusOS-Update: 1': problems.append('unknown bundle format')
    listed = {}
    for line in lines:
        if line.startswith('File: '):
            parts = line[6:].split()
            if len(parts) != 3: problems.append('bad File line'); continue
            listed[parts[0]] = (int(parts[1]), parts[2])
    for name in names:
        if name in ('MANIFEST', 'MANIFEST.sig'): continue
        if name not in MEMBERS: problems.append('unexpected member ' + name)
        elif name not in listed: problems.append(name + ' is not in the signed manifest')
        elif (len(data[name]), hashlib.sha256(data[name]).hexdigest()) != listed[name]: problems.append(name + ' does not match the manifest')
    for name in listed:
        if name not in data: problems.append(name + ' is listed but missing')
    if 'kernel.bin' in data:
        if 'kernel.sig' not in data: problems.append('the kernel has no Secure Boot signature (kernel.sig)')
        else:
            from cryptography import x509
            from cryptography.hazmat.primitives import hashes
            from cryptography.hazmat.primitives.asymmetric import padding
            from cryptography.exceptions import InvalidSignature
            cert = x509.load_der_x509_certificate((HERE / 'uefi/keys/kernel.der').read_bytes())
            try: cert.public_key().verify(data['kernel.sig'], data['kernel.bin'], padding.PKCS1v15(), hashes.SHA256())
            except InvalidSignature: problems.append('kernel.sig does not verify with uefi/keys/kernel.der')
    if problems: raise SystemExit('bundle %s refused: %s' % (bundle, '; '.join(problems)))
    version = next((l[9:] for l in lines if l.startswith('Version: ')), '?')
    print('[update] %s verifies: version %s, %s' % (bundle, version, ', '.join(n for n in names if n not in ('MANIFEST', 'MANIFEST.sig'))))


def pgp():
    spec = importlib.util.spec_from_file_location('nexus_pgp_sign', ROOT / 'tests/pgp_sign.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def manifest_text(version, payloads):
    lines = ['NexusOS-Update: 1', 'Version: ' + version, 'Built: ' + time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
             'Signature: openpgp-detached']
    lines += ['File: %s %d %s' % (name, len(data), hashlib.sha256(data).hexdigest()) for name, data in payloads]
    return ('\n'.join(lines) + '\n').encode()


def build(out, version, payloads, sign):
    if not VERSION_RE.fullmatch(version): raise SystemExit('bad version %r' % version)
    names = [n for n, _ in payloads]
    if 'kernel.bin' not in names and 'init' not in names: raise SystemExit('a bundle needs a kernel or an init')
    manifest = manifest_text(version, payloads); signature = sign(manifest)
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for name, data in [('MANIFEST', manifest), ('MANIFEST.sig', signature), *payloads]:
            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644; info.mtime = int(time.time()); info.uname = info.gname = 'root'
            tar.addfile(info, io.BytesIO(data))
    Path(out).write_bytes(buffer.getvalue())
    print('[update] %s: version %s, %s (%d bytes)' % (out, version, ', '.join(names), len(buffer.getvalue())))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='action', required=True)
    k = sub.add_parser('keygen'); k.add_argument('keydir', type=Path); k.add_argument('--uid', default='NexusOS updates <updates@nexus.invalid>')
    b = sub.add_parser('build'); b.add_argument('out', type=Path); b.add_argument('--version', required=True)
    b.add_argument('--key', type=Path); b.add_argument('--sign-command')
    b.add_argument('--kernel', type=Path, default=OUT / 'kernel.bin'); b.add_argument('--init', type=Path, default=OUT / 'desktop.elf')
    b.add_argument('--loader', type=Path, default=OUT / 'BOOTX64.EFI'); b.add_argument('--no-loader', action='store_true')
    b.add_argument('--kernel-sig', type=Path, help='Secure Boot signature of --kernel (build.py secureboot: out/kernel.sig)')
    b.add_argument('--no-kernel', action='store_true'); b.add_argument('--no-init', action='store_true')
    s = sub.add_parser('show'); s.add_argument('bundle', type=Path)
    rk = sub.add_parser('release-key'); rk.add_argument('--keydir', type=Path); rk.add_argument('--force', action='store_true')
    rk.add_argument('--uid', default='NexusOS release updates <updates@nexusos.invalid>')
    rk.add_argument('--tree-keyring', type=Path, default=TREE_KEYRING, help=argparse.SUPPRESS)   # tests: a scratch copy
    r = sub.add_parser('release'); r.add_argument('out', type=Path); r.add_argument('--version', required=True)
    r.add_argument('--keydir', type=Path); r.add_argument('--no-init', action='store_true'); r.add_argument('--no-loader', action='store_true')
    r.add_argument('--tree-keyring', type=Path, default=TREE_KEYRING, help=argparse.SUPPRESS)
    v = sub.add_parser('verify'); v.add_argument('bundle', type=Path); v.add_argument('--keyring', type=Path, default=TREE_KEYRING)
    a = ap.parse_args(argv)
    if a.action == 'release-key':
        keydir = a.keydir or default_keydir(); private = keydir / 'update-key.json'
        if private.exists() and not a.force:
            raise SystemExit('the release key exists already (%s); systems trust its public half: --force replaces it' % private)
        key = pgp().TestKey.generate(a.uid)
        if private.exists():
            backup = private.with_name('update-key.%d.json' % int(time.time())); private.replace(backup); print('[update] old key kept as', backup)
        private_file(private, key.save())
        (keydir / 'nexusos-update.pgp').write_bytes(key.public_packets())
        a.tree_keyring.parent.mkdir(parents=True, exist_ok=True); a.tree_keyring.write_bytes(key.public_packets())
        a.tree_keyring.with_suffix('.fingerprint').write_text(key.fingerprint.hex().upper() + '\n')
        print('[update] release key %s: private %s, public %s (back up %s)' % (key.fingerprint.hex().upper(), private, a.tree_keyring, keydir))
    elif a.action == 'release':
        keydir = a.keydir or default_keydir(); private = keydir / 'update-key.json'
        if not private.is_file(): raise SystemExit('no release key in %s: create it once with `update_bundle.py release-key`' % keydir)
        key = pgp().TestKey.load(private.read_text())
        if not a.tree_keyring.is_file() or openpgp_packets(a.tree_keyring.read_bytes())[0] != (6, key.key_body):
            raise SystemExit('the release key in %s is not the one in %s: systems would refuse the bundle' % (private, a.tree_keyring))
        sb = module('nexus_update_sb', HERE / 'uefi/secureboot.py')
        payloads = [('kernel.bin', (OUT / 'kernel.bin').read_bytes())]
        sb.sign(OUT / 'kernel.bin', OUT / 'kernel.sig', sb.DEFAULT_KEYDIR)   # the kernel signature for Secure Boot, made now
        payloads.append(('kernel.sig', (OUT / 'kernel.sig').read_bytes()))
        if not a.no_init: payloads.append(('init', (OUT / 'desktop.elf').read_bytes()))
        if not a.no_loader:
            efi = module('nexus_update_sbefi', HERE / 'uefi/secureboot_efi.py')
            efi.sign_pe(str(OUT / 'BOOTX64.EFI'), str(OUT / 'BOOTX64-signed.EFI')); efi.verify_pe(OUT / 'BOOTX64-signed.EFI', HERE / 'uefi/keys/db.der')
            payloads.append(('BOOTX64.EFI', (OUT / 'BOOTX64-signed.EFI').read_bytes()))
        build(a.out, a.version, payloads, key.detach_sign)
        verify_bundle(a.out, a.tree_keyring)
    elif a.action == 'verify':
        verify_bundle(a.bundle, a.keyring)
    elif a.action == 'keygen':
        key = pgp().TestKey.generate(a.uid); a.keydir.mkdir(parents=True, exist_ok=True)
        (a.keydir / 'update-key.json').write_text(key.save()); (a.keydir / 'nexusos-update.pgp').write_bytes(key.public_packets())
        print('[update] key %s: %s' % (key.fingerprint.hex().upper(), a.keydir))
    elif a.action == 'build':
        if bool(a.key) == bool(a.sign_command): raise SystemExit('give --key or --sign-command')
        if a.key:
            key = pgp().TestKey.load(a.key.read_text()); sign = key.detach_sign
        else:
            def sign(manifest):
                return subprocess.run(a.sign_command, shell=True, input=manifest, capture_output=True, check=True).stdout
        payloads = []
        for name, path, skip in (('kernel.bin', a.kernel, a.no_kernel), ('init', a.init, a.no_init), ('BOOTX64.EFI', a.loader, a.no_loader)):
            if not skip: payloads.append((name, Path(path).read_bytes()))
        if a.kernel_sig:   # the UEFI loader verifies a slot kernel against the slot's kernel.sig (uefi/abslot.c)
            if a.no_kernel: raise SystemExit('--kernel-sig needs the kernel in the bundle')
            signature = a.kernel_sig.read_bytes()
            if len(signature) != 256: raise SystemExit('kernel.sig must be a 256-byte RSA-2048 signature')
            payloads.append(('kernel.sig', signature))
        build(a.out, a.version, payloads, sign)
    else:
        with tarfile.open(a.bundle) as tar:
            for m in tar.getmembers(): print('%-14s %10d' % (m.name, m.size))
            print(tar.extractfile('MANIFEST').read().decode())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
