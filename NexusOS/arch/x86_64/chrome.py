"""Stage the real Chrome application and verified libraries on a private NexusOS disk.

This is a development bootstrap, not proof of the final in-OS installation flow.
No package maintenance script, external kernel, init system or daemon is run.
"""
from pathlib import Path, PurePosixPath
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import io
import json
import posixpath
import re
import shutil
import subprocess
import tarfile
import urllib.parse
import urllib.request
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
CACHE = ROOT / 'tools/chrome-packages'
TREE = OUT / 'chrome-root'


def digest(path):
    with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()


def fetch(package):
    url = urllib.parse.urlsplit(package['url'])
    if url.scheme != 'https' or url.hostname not in ('deb.debian.org', 'dl.google.com'):
        raise ValueError('Unexpected package origin')
    path = CACHE / PurePosixPath(url.path).name
    if not path.exists():
        candidates = [ROOT / 'tools' / path.name]
        if package['name'] == 'google-chrome-stable':
            candidates.append(nexus_paths.file('NEXUS_CHROME_DEB', nexus_paths.cache('chrome') / 'google-chrome-stable_current_amd64.deb'))
        cached = next((p for p in candidates if p.exists() and digest(p) == package['sha256']), None)
        temporary = path.with_suffix('.part')
        if cached:
            shutil.copyfile(cached, temporary)
        else:
            with urllib.request.urlopen(package['url'], timeout=60) as response, temporary.open('wb') as f:
                received = 0
                while block := response.read(1024 * 1024):
                    received += len(block)
                    if received > package['bytes']: raise ValueError('Oversized package response')
                    f.write(block)
        if temporary.stat().st_size != package['bytes'] or digest(temporary) != package['sha256']:
            raise ValueError('Package verification failed: ' + package['name'])
        temporary.replace(path)
    if path.stat().st_size != package['bytes'] or digest(path) != package['sha256']:
        raise ValueError('Cached package verification failed: ' + package['name'])
    return path


def tar_payload(path):
    with path.open('rb') as f:
        if f.read(8) != b'!<arch>\n': raise ValueError('Invalid package header')
        while header := f.read(60):
            if len(header) != 60 or header[58:60] != b'`\n': raise ValueError('Invalid package member')
            size = int(header[48:58]); name = header[:16].decode().strip().rstrip('/')
            if name.startswith('data.tar'):
                data = f.read(size)
                if len(data) != size: raise ValueError('Truncated package payload')
                return data
            f.seek(size + (size & 1), 1)
    raise ValueError('Missing package payload')


def safe_name(name):
    while name.startswith('./'): name = name[2:]
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError('Unsafe package path: ' + name)
    return path.as_posix()


def wanted(name):
    return (name.startswith('opt/google/chrome/') or
            (name.startswith(('usr/lib/', 'lib/')) and re.search(r'\.so(?:\.|$)', name.rsplit('/', 1)[-1])) or
            name.startswith(('etc/fonts/', 'usr/share/fonts/', 'usr/share/fontconfig/', 'usr/share/alsa/',
                             'usr/share/X11/xkb/', 'usr/share/glib-2.0/schemas/')))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result); return result


def main():
    packages = json.loads((HERE / 'chrome-lock.json').read_text())
    CACHE.mkdir(parents=True, exist_ok=True); TREE.mkdir(parents=True, exist_ok=True)
    print('Verifying', len(packages), 'application/library archives', flush=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        archives = list(pool.map(fetch, packages))
    regular, links = {}, {}
    for package, path in zip(packages, archives):
        with tarfile.open(fileobj=io.BytesIO(tar_payload(path))) as archive:
            for member in archive:
                name = safe_name(member.name)
                if name.endswith('/copyright') and name.startswith('usr/share/doc/'):
                    if not member.isfile(): continue
                    name = 'share/licenses/' + package['name'] + '.txt'
                elif not wanted(name): continue
                if member.isfile():
                    destination = TREE.joinpath(*PurePosixPath(name).parts)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with archive.extractfile(member) as src, destination.open('wb') as dst:
                        shutil.copyfileobj(src, dst, length=1024 * 1024)
                    regular[name] = destination
                elif member.issym() or member.islnk():
                    target = member.linkname
                    if target.startswith('/'): target = target.lstrip('/')
                    elif member.issym(): target = posixpath.join(posixpath.dirname(name), target)
                    links[name] = safe_name(posixpath.normpath(target))
    def resolve(name, seen=None):
        seen = set() if seen is None else seen
        if name in seen: raise ValueError('Cyclic package symlink: ' + name)
        seen.add(name)
        if name in regular: return regular[name]
        if name in links: return resolve(links[name], seen)
        if name.startswith(('lib/', 'lib64/')): return resolve('usr/' + name, seen)
        raise ValueError('Unresolved package symlink: ' + name)
    installed = dict(regular)
    for name in ('resolv.conf', 'nsswitch.conf', 'hosts'): installed['etc/' + name] = HERE / 'etc' / name
    for name in links: installed[name] = resolve(name)
    # The current FAT-backed VFS has no symlink metadata; materialize path aliases.
    for name, source in list(installed.items()):
        if name.startswith('usr/lib/'): installed[name[4:]] = source
    loader = 'lib/x86_64-linux-gnu/ld-linux-x86-64.so.2'
    installed['lib64/ld-linux-x86-64.so.2'] = installed[loader]
    installed['lib/ld-linux-x86-64.so.2'] = installed[loader]
    by_name = {}
    for name in installed: by_name.setdefault(PurePosixPath(name).name, name)
    readelf = ROOT / 'tools/i686-elf/bin/i686-elf-readelf.exe'
    queue = ['opt/google/chrome/chrome']; checked = set()
    while queue:
        name = queue.pop()
        if name in checked: continue
        checked.add(name)
        output = subprocess.check_output([str(readelf), '-d', str(installed[name])], text=True)
        for dependency in re.findall(r'Shared library: \[(.*?)\]', output):
            if dependency not in by_name: raise ValueError(name + ' requires missing ' + dependency)
            queue.append(by_name[dependency])
    print('PASS Chrome dependency closure:', len(checked), 'ELF images', flush=True)
    fatput = module('chrome_fatput', ROOT / 'userland/fatput.py')
    storage = module('chrome_storage', ROOT / 'storage.py')
    disk = OUT / 'chrome64.img'
    if not disk.exists(): shutil.copyfile(OUT / 'apps64.img', disk)
    if disk.stat().st_size < 2 * 1024**3: storage.grow(disk, 2048)
    args = ['fatput', str(disk), 'mkdir', 'home/nexus', 'mkdir', 'tmp', 'mkdir', 'run/user/1000']
    for name, source in sorted(installed.items()): args += ['put', str(source), name]
    fatput.main(args)
    manifest = {'packages': packages, 'files': sorted(installed), 'checked_dependencies': sorted(checked),
                'chrome_started': False, 'purpose': 'Isolated native-kernel startup probe; host staging only'}
    (OUT / 'chrome-installed.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Chrome files staged on', disk, '; execution not yet verified', flush=True)


if __name__ == '__main__':
    assert safe_name('./usr/lib/libtest.so') == 'usr/lib/libtest.so'
    for bad in ('../../escape', '/absolute', 'C:/host', 'usr\\escape'):
        try: safe_name(bad)
        except ValueError: continue
        raise AssertionError('Unsafe archive path accepted')
    main()
