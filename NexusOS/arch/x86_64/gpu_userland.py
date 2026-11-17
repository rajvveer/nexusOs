"""Install the Mesa virgl GPU userland (EGL/GLES via glvnd, GBM, gallium virtio_gpu) on a NexusOS disk image.

Usage:
  python -I arch/x86_64/gpu_userland.py lock            # resolve gpu-lock.json from the verified Debian index
  python -I arch/x86_64/gpu_userland.py install DISK [--cache DIR] [--chrome-args FILE]

The Debian trixie index (tools/chrome-packages/debian-Packages.xz) is checked against the SHA256 that
Debian's InRelease lists for main/binary-amd64/Packages.xz (fetched over HTTPS from deb.debian.org);
every .deb is checked against the size/SHA256 from that index. Packages already staged for Chrome
(chrome-lock.json: libgbm1, mesa-libgallium, libdrm2, libwayland-*, libLLVM, ...) are not repeated; the
new ones must match their exact versions (mesa-libgallium = 25.0.7-2+deb13u1). The FAT-backed VFS has
no symlink metadata, so package symlinks are materialized as copies under /usr/lib and /lib, like chrome.py.
No maintainer script runs; nothing outside the given disk image is modified.
"""
from pathlib import Path, PurePosixPath
import hashlib
import importlib.util
import io
import json
import lzma
import posixpath
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
INDEX = ROOT / 'tools/chrome-packages/debian-Packages.xz'
LOCK = HERE / 'gpu-lock.json'
WANTED = ['libegl1', 'libegl-mesa0', 'libgles2', 'libglvnd0', 'libgl1-mesa-dri', 'libgbm1', 'mesa-libgallium']
MIRROR = 'https://deb.debian.org/debian/'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verified_index():
    raw = INDEX.read_bytes()
    with urllib.request.urlopen(MIRROR + 'dists/trixie/InRelease', timeout=60) as response:
        release = response.read().decode('utf-8')
    listed = re.findall(r'^ ([0-9a-f]{64})\s+(\d+) main/binary-amd64/Packages\.xz$', release, re.M)
    if (digest(raw), str(len(raw))) not in listed:
        raise ValueError('Packages.xz does not match the SHA256 in Debian trixie InRelease')
    packages = {}
    for block in lzma.decompress(raw).decode('utf-8').split('\n\n'):
        fields, key = {}, None
        for line in block.split('\n'):
            if line.startswith(' ') and key: fields[key] += '\n' + line
            elif ':' in line: key, value = line.split(':', 1); fields[key] = value.strip()
        if 'Package' in fields: packages[fields['Package']] = fields
    return packages


def dependencies(fields):
    names = []
    for key in ('Pre-Depends', 'Depends'):
        for group in fields.get(key, '').split(','):
            group = group.strip()
            if group: names.append(group.split('|')[0].split('(')[0].split(':')[0].strip())
    return names


def make_lock():
    packages = verified_index()
    staged = {p['name']: p['version'] for p in json.loads((HERE / 'chrome-lock.json').read_text())}
    lock, queue, seen = [], list(WANTED), set()
    while queue:
        name = queue.pop(0)
        if name in seen: continue
        seen.add(name)
        fields = packages[name]
        version = fields['Version']
        if name in staged:
            if staged[name] != version: raise ValueError(f'{name}: staged {staged[name]} but index has {version}')
            continue  # already on the desktop disk (chrome.py) together with its dependencies
        lock.append({'name': name, 'version': version, 'url': MIRROR + fields['Filename'], 'sha256': fields['SHA256'], 'bytes': int(fields['Size'])})
        queue += [d for d in dependencies(fields) if d not in seen]
    LOCK.write_text(json.dumps(sorted(lock, key=lambda p: p['name']), indent=2) + '\n')
    print('wrote', LOCK, 'with', len(lock), 'packages:', ', '.join(p['name'] for p in lock))


def fetch(package, cache):
    url = urllib.parse.urlsplit(package['url'])
    if url.scheme != 'https' or url.hostname != 'deb.debian.org': raise ValueError('Unexpected package origin')
    path = cache / PurePosixPath(url.path).name
    if not path.exists() or path.stat().st_size != package['bytes'] or digest(path.read_bytes()) != package['sha256']:
        with urllib.request.urlopen(package['url'], timeout=120) as response: data = response.read(package['bytes'] + 1)
        if len(data) != package['bytes'] or digest(data) != package['sha256']: raise ValueError('Package verification failed: ' + package['name'])
        path.write_bytes(data)
    return path


def payload(path):
    data = path.read_bytes()
    if data[:8] != b'!<arch>\n': raise ValueError('Invalid package header')
    offset = 8
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]; size = int(header[48:58]); name = header[:16].decode().strip().rstrip('/')
        if name.startswith('data.tar'): return data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
    raise ValueError('Missing package payload')


def safe(name):
    while name.startswith('./'): name = name[2:]
    pure = PurePosixPath(name)
    if pure.is_absolute() or '..' in pure.parts or '\\' in name or ':' in name: raise ValueError('Unsafe path ' + name)
    return pure.as_posix()


def wanted(name):
    base = name.rsplit('/', 1)[-1]
    return ((name.startswith('usr/lib/x86_64-linux-gnu/') and (re.search(r'\.so(?:\.|$)', base) or name.endswith('.json'))) or
            name.startswith(('usr/share/glvnd/egl_vendor.d/', 'usr/share/drirc.d/')))


def install(disk, cache, chrome_args=None):
    packages = json.loads(LOCK.read_text())
    cache.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='ws_gpu_stage_', dir=str(cache)))
    regular, links = {}, {}
    for package in packages:
        with tarfile.open(fileobj=io.BytesIO(payload(fetch(package, cache)))) as archive:
            for member in archive:
                name = safe(member.name)
                if name.startswith('usr/share/doc/') and name.endswith('/copyright') and member.isfile():
                    target = staging / 'licenses' / (package['name'] + '.txt'); target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.extractfile(member).read()); regular['share/licenses/' + package['name'] + '.txt'] = target
                    continue
                if not wanted(name): continue
                if member.isfile():
                    target = staging.joinpath(*PurePosixPath(name).parts); target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.extractfile(member).read()); regular[name] = target
                elif member.issym():
                    link = member.linkname.lstrip('/') if member.linkname.startswith('/') else posixpath.join(posixpath.dirname(name), member.linkname)
                    links[name] = safe(posixpath.normpath(link))
    def resolve(name, depth=0):
        if depth > 16: raise ValueError('symlink loop ' + name)
        if name in regular: return regular[name]
        if name in links: return resolve(links[name], depth + 1)
        return None
    installed = dict(regular)
    for name in links:
        source = resolve(name)
        if source: installed[name] = source
    for name, source in list(installed.items()):
        if name.startswith('usr/lib/'): installed[name[4:]] = source
    # Every NEEDED library of what we add must be on the disk or in this set.
    spec = importlib.util.spec_from_file_location('gpu_fatput', ROOT / 'userland/fatput.py')
    fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
    fs = fatput.Fat32(str(disk))
    try: present = {entry[0] for entry in fs.named_entries(fs.lookup_dir(['usr', 'lib', 'x86_64-linux-gnu']))}
    finally: fs.close()
    available = present | {PurePosixPath(n).name for n in installed}
    readelf = ROOT / 'tools/i686-elf/bin/i686-elf-readelf.exe'
    for name, source in sorted(installed.items()):
        if not name.startswith('usr/lib/') or not re.search(r'\.so', name): continue
        text = subprocess.check_output([str(readelf), '-d', str(source)], text=True)
        for needed in re.findall(r'Shared library: \[(.*?)\]', text):
            if needed not in available: raise ValueError(f'{name} needs {needed}, which is not on the disk')
    args = ['fatput', str(disk)]
    for name, source in sorted(installed.items()): args += ['put', str(source), name]
    if chrome_args: args += ['put', str(chrome_args), 'nexus/chrome-desktop.args']
    fatput.main(args)
    print('PASS installed', len(packages), 'verified packages,', len(installed), 'files on', disk, flush=True)


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == 'lock': make_lock()
    elif len(sys.argv) >= 3 and sys.argv[1] == 'install':
        options = sys.argv[3:]
        cache = Path(options[options.index('--cache') + 1]) if '--cache' in options else nexus_paths.cache('gpu')
        chrome_args = Path(options[options.index('--chrome-args') + 1]) if '--chrome-args' in options else None
        install(Path(sys.argv[2]), cache, chrome_args)
    else: raise SystemExit(__doc__)
