"""Fetch verified application runtime libraries; never install or boot an external OS."""
from pathlib import Path
import hashlib
import io
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
URL = 'https://deb.debian.org/debian/pool/main/g/glibc/libc6_2.41-12+deb13u4_amd64.deb'
SHA256 = '967aa62605721081c3eb2a17650611a792aa802d76a6511d1840242623d204c9'
LIBRARIES = ('ld-linux-x86-64.so.2', 'libc.so.6', 'libm.so.6', 'libdl.so.2',
             'libpthread.so.0', 'librt.so.1', 'libresolv.so.2', 'libutil.so.1')


def prepare(directory):
    cache = ROOT / 'tools' / URL.rsplit('/', 1)[1]
    if not cache.exists():
        temporary = cache.with_suffix('.part')
        with urllib.request.urlopen(URL, timeout=60) as response, temporary.open('wb') as output:
            while block := response.read(1024 * 1024): output.write(block)
        temporary.replace(cache)
    data = cache.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Application runtime archive checksum mismatch')
    if data[:8] != b'!<arch>\n': raise ValueError('Invalid Debian archive')
    offset = 8
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        length = int(header[48:58]); name = header[:16].decode().strip().rstrip('/')
        body = data[offset + 60:offset + 60 + length]
        if len(body) != length or header[58:60] != b'`\n': raise ValueError('Truncated archive')
        offset += 60 + length + (length & 1)
        if not name.startswith('data.tar'): continue
        directory.mkdir(parents=True, exist_ok=True)
        found = set()
        with tarfile.open(fileobj=io.BytesIO(body)) as archive:
            for member in archive:
                leaf = member.name.rsplit('/', 1)[-1]
                if not member.isfile(): continue
                if leaf in LIBRARIES and member.name.startswith('./usr/lib/'):
                    if leaf in found: raise ValueError('Ambiguous library member: ' + leaf)
                    (directory / leaf).write_bytes(archive.extractfile(member).read()); found.add(leaf)
                elif member.name == './usr/share/doc/libc6/copyright':
                    (directory / 'COPYRIGHT.glibc').write_bytes(archive.extractfile(member).read())
        if found != set(LIBRARIES) or not (directory / 'COPYRIGHT.glibc').exists():
            raise ValueError('Missing runtime library or license: ' + repr(set(LIBRARIES) - found))
        return
    raise ValueError('Missing archive payload')


if __name__ == '__main__':
    prepare(Path(__file__).resolve().parent / 'out/runtime')
    print('Verified unmodified glibc application libraries and license extracted')
