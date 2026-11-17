"""Test Debian packages and a signed apt repository (tests/test_x64_apt.py, tests/test_x64_software.py).

Builds real .deb files (ar: debian-binary, control.tar.xz, data.tar.xz with owners/modes) and a repository
dists/<suite>/{Release,InRelease} + main/binary-amd64/Packages(.xz) + pool/, InRelease clear-signed by a per-run
OpenPGP key (pgp_sign.py). Variants for negative tests: 'badsig' (InRelease signed by another key), 'unsigned'
(no InRelease/Release.gpg), 'badhash' (a .deb that does not match its Packages entry).
"""
import hashlib
import io
import lzma
import tarfile
import time
from pathlib import Path

import pgp_sign


def _tar(members):
    """members: [(path, bytes or None for a directory, mode)] -> xz tar bytes (root:root)."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.GNU_FORMAT) as t:
        for path, data, mode in members:
            info = tarfile.TarInfo('./' + path.strip('/') if path != '.' else '.')
            info.uid = info.gid = 0; info.uname = info.gname = 'root'; info.mtime = 1760000000
            if data is None: info.type = tarfile.DIRTYPE; info.mode = mode; t.addfile(info)
            else: info.size = len(data); info.mode = mode; t.addfile(info, io.BytesIO(data))
    return lzma.compress(raw.getvalue(), format=lzma.FORMAT_XZ)


def _ar(members):
    out = bytearray(b'!<arch>\n')
    for name, data in members:
        header = '%-16s%-12d%-6d%-6d%-8s%-10d`\n' % (name, 1760000000, 0, 0, '100644', len(data))
        out += header.encode() + data + (b'\n' if len(data) & 1 else b'')
    return bytes(out)


def build_deb(package, version, files, scripts=None, depends='', conffiles=(), description='NexusOS test package', arch='all'):
    """files: {path: (bytes, mode)}; scripts: {'preinst'|'postinst'|'prerm'|'postrm': text}."""
    dirs = set()
    for path in files:
        parts = path.strip('/').split('/')
        for i in range(1, len(parts)): dirs.add('/'.join(parts[:i]))
    data_members = [('.', None, 0o755)] + [(d, None, 0o755) for d in sorted(dirs)] + \
                   [(p.strip('/'), data, mode) for p, (data, mode) in sorted(files.items())]
    size_kb = max(1, sum(len(d) for d, _ in files.values()) // 1024)
    control = ('Package: %s\nVersion: %s\nArchitecture: %s\nMaintainer: NexusOS tests <tests@nexus.invalid>\nInstalled-Size: %d\n%s'
               'Section: misc\nPriority: optional\nDescription: %s\n test package built by tests/debrepo.py\n') % (
        package, version, arch, size_kb, ('Depends: %s\n' % depends) if depends else '', description)
    md5 = ''.join('%s  %s\n' % (hashlib.md5(data).hexdigest(), p.strip('/')) for p, (data, _) in sorted(files.items()))
    control_members = [('.', None, 0o755), ('control', control.encode(), 0o644), ('md5sums', md5.encode(), 0o644)]
    if conffiles: control_members.append(('conffiles', ''.join(c + '\n' for c in conffiles).encode(), 0o644))
    for name, text in (scripts or {}).items(): control_members.append((name, text.encode(), 0o755))
    return _ar([('debian-binary', b'2.0\n'), ('control.tar.xz', _tar(control_members)), ('data.tar.xz', _tar(data_members))]), control


class Repo:
    """A flat-pool apt repository at root/ with suite `suite`, component main, architecture amd64 (+ all)."""
    def __init__(self, root, suite='nexus-test', key=None):
        self.root = Path(root); self.suite = suite
        self.key = key or pgp_sign.TestKey.generate('NexusOS test repository <test@nexus.invalid>')
        self.other_key = pgp_sign.TestKey.generate('Somebody else <other@nexus.invalid>')

    def publish(self, debs, variant='good'):
        """debs: [(deb bytes, control text)]; rewrites the whole repository."""
        import shutil
        if self.root.exists(): shutil.rmtree(self.root)
        pool = self.root / 'pool/main'; pool.mkdir(parents=True)
        binary = self.root / 'dists' / self.suite / 'main/binary-amd64'; binary.mkdir(parents=True)
        stanzas = []
        for i, (deb, control) in enumerate(debs):
            fields = dict(line.split(': ', 1) for line in control.splitlines() if ': ' in line and not line.startswith(' '))
            name = '%s_%s_%s.deb' % (fields['Package'], fields['Version'], fields['Architecture'])
            listed = deb
            if variant == 'badhash' and i == 0: deb = deb[:-200] + bytes(200)   # served bytes differ from the index
            (pool / name).write_bytes(deb)
            stanza = control.rstrip('\n').split('\nDescription:')[0] + '\nFilename: pool/main/%s\nSize: %d\nSHA256: %s\nDescription:%s' % (
                name, len(listed), hashlib.sha256(listed).hexdigest(), control.split('\nDescription:', 1)[1].rstrip('\n'))
            stanzas.append(stanza)
        packages = ('\n\n'.join(stanzas) + '\n').encode()
        (binary / 'Packages').write_bytes(packages); (binary / 'Packages.xz').write_bytes(lzma.compress(packages, format=lzma.FORMAT_XZ))
        files = ''.join(' %s %d main/binary-amd64/%s\n' % (hashlib.sha256((binary / n).read_bytes()).hexdigest(), (binary / n).stat().st_size, n)
                        for n in ('Packages', 'Packages.xz'))
        release = ('Origin: NexusOS tests\nLabel: NexusOS tests\nSuite: %s\nCodename: %s\nDate: %s\nValid-Until: %s\nArchitectures: amd64 all\n'
                   'Components: main\nDescription: NexusOS test repository\nSHA256:\n%s') % (
            self.suite, self.suite, time.strftime('%a, %d %b %Y %H:%M:%S UTC', time.gmtime()),
            time.strftime('%a, %d %b %Y %H:%M:%S UTC', time.gmtime(time.time() + 7 * 86400)), files)
        dists = self.root / 'dists' / self.suite
        (dists / 'Release').write_text(release, newline='\n')
        if variant == 'unsigned': return
        signer = self.other_key if variant == 'badsig' else self.key
        (dists / 'InRelease').write_text(signer.clearsign(release), newline='\n')
