"""Install the pinned device firmware (arch/x86_64/firmware-lock.json) onto a NexusOS disk image.

    python -I arch/x86_64/firmware_install.py <disk.img> [--check] [--download CACHE]

Every file is checked against the lock (size + SHA-256) before anything is written: the copies in
arch/x86_64/firmware/ by default, or files fetched from the pinned linux-firmware URLs into CACHE (--download).
The files land in usr/lib/firmware/<name> (Debian's merged /usr; the kernel looks in /lib/firmware first, then
/usr/lib/firmware), with their licence files in usr/share/doc/nexus-firmware/. FAT32 disks are written with
userland/fatput.py, ext4 roots with userland/ext4put.py (detected from the partition table). --check only verifies.
Stop the VM that uses the disk first."""
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url):
    if not url.startswith('https://git.kernel.org/'):
        raise SystemExit(f'refusing to download from an unpinned host: {url}')
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def collect(lock, cache):
    items = []
    for entry in lock['licenses'] + lock['files']:
        source = HERE.parent.parent / entry['in_tree']
        if cache:
            source = pathlib.Path(cache) / entry['name']
            if not source.exists() or sha256(source.read_bytes()) != entry['sha256']:
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(fetch(entry['url']))
        data = source.read_bytes()
        if len(data) != entry['bytes'] or sha256(data) != entry['sha256']:
            raise SystemExit(f"{entry['name']}: size/SHA-256 differs from firmware-lock.json ({source})")
        target = entry.get('install') or 'usr/share/doc/nexus-firmware/' + entry['name']
        items.append((source, target))
    return items


def ext4_root(disk):
    """True when the image carries an ext4 root (MBR type 0x83 partition or a whole-disk ext4)."""
    with open(disk, 'rb') as f:
        mbr = f.read(512)
        offsets = [0]
        if mbr[510:512] == b'\x55\xaa':
            for i in range(4):
                entry = mbr[446 + 16 * i: 462 + 16 * i]
                if entry[4] == 0x83:
                    offsets.append(struct.unpack_from('<I', entry, 8)[0] * 512)
        for offset in offsets:
            f.seek(offset + 1024 + 56)
            if f.read(2) == b'\x53\xef':
                return True
    return False


def main(argv):
    if len(argv) < 2 or argv[1].startswith('-'):
        print(__doc__); return 2
    disk = pathlib.Path(argv[1]); check = '--check' in argv
    cache = argv[argv.index('--download') + 1] if '--download' in argv else None
    lock = json.loads((HERE / 'firmware-lock.json').read_text())
    items = collect(lock, cache)
    print(f'{len(items)} files verified against firmware-lock.json (linux-firmware {lock["linux_firmware_tag"]})')
    if check:
        return 0
    if not disk.exists():
        raise SystemExit(f'{disk}: no such disk image')
    if ext4_root(disk):
        command = [sys.executable, '-I', str(ROOT / 'userland/ext4put.py'), str(disk), '--mode', '644']
    else:
        command = [sys.executable, '-I', str(ROOT / 'userland/fatput.py'), str(disk)]
    for source, target in items:
        command += ['put', str(source), target]
    subprocess.run(command, check=True)
    print(f'installed into {disk} ({"ext4" if ext4_root(disk) else "FAT32"}): ' + ', '.join(t for _, t in items))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
