"""Second half of the application install: run the Debian trigger tools inside NexusOS.

python -I arch/x86_64/apps_postinst.py <disk.img> [--tcg] [--keep-overlay]

glib-compile-schemas, gdk-pixbuf-query-loaders, update-mime-database and fc-cache run as native Linux
programs on NexusOS (arch/x86_64/apps_postinst_user.c, started by the Aurora session). The disk is booted
through a temporary qcow2 overlay, so the image itself is never written by the guest; the generated files
are read back from the overlay and installed at their system paths:
  /usr/share/glib-2.0/schemas/gschemas.compiled, /usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache,
  /usr/share/mime/{mime.cache,globs2,...}, /var/cache/fontconfig/*.cache-*
Stop QEMU before use; only /nexus/desktop.args is changed temporarily (restored afterwards).
"""
from pathlib import Path, PurePosixPath
import argparse
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
QEMU_IMG = Path(r'C:\Program Files\qemu\qemu-img.exe')
GUEST_OUT = 'home/nexus/.nexus-postinst'
spec = importlib.util.spec_from_file_location('postinst_fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)


class Qcow2:
    """Read-only view of a qcow2 overlay (uncompressed clusters) on top of a raw backing image."""
    def __init__(self, path, backing):
        self.f = open(path, 'rb'); self.backing = open(backing, 'rb')
        header = self.f.read(104)
        if header[:4] != b'QFI\xfb': raise ValueError('not a qcow2 image')
        version, = struct.unpack_from('>I', header, 4)
        self.cluster_bits, = struct.unpack_from('>I', header, 20)
        self.size, = struct.unpack_from('>Q', header, 24)
        self.l1_size, = struct.unpack_from('>I', header, 36)
        self.l1_offset, = struct.unpack_from('>Q', header, 40)
        if version >= 3 and struct.unpack_from('>Q', header, 72)[0] & ~0x2: raise ValueError('unsupported qcow2 features')
        self.cluster = 1 << self.cluster_bits; self.l2_bits = self.cluster_bits - 3
        self.f.seek(self.l1_offset); self.l1 = struct.unpack('>%dQ' % self.l1_size, self.f.read(8 * self.l1_size))
        self.l2_cache = {}; self.pos = 0
    def entry(self, offset):
        l1 = offset >> (self.cluster_bits + self.l2_bits)
        if l1 >= self.l1_size: return 0
        table = self.l1[l1] & 0x00FFFFFFFFFFFE00
        if not table: return 0
        if table not in self.l2_cache:
            self.f.seek(table); self.l2_cache[table] = struct.unpack('>%dQ' % (1 << self.l2_bits), self.f.read(self.cluster))
        return self.l2_cache[table][(offset >> self.cluster_bits) & ((1 << self.l2_bits) - 1)]
    def read_at(self, offset, count):
        out = bytearray()
        while count > 0:
            inside = offset & (self.cluster - 1); part = min(count, self.cluster - inside)
            e = self.entry(offset)
            if e & (1 << 62): raise ValueError('compressed qcow2 cluster')
            host = e & 0x00FFFFFFFFFFFE00
            if e & 1: data = bytes(part)
            elif host: self.f.seek(host + inside); data = self.f.read(part)
            else: self.backing.seek(offset); data = self.backing.read(part)
            out += data; offset += part; count -= part
        return bytes(out)
    # file-like interface for fatput.Fat32
    def seek(self, pos, whence=0): self.pos = pos if whence == 0 else self.pos + pos if whence == 1 else self.size + pos; return self.pos
    def tell(self): return self.pos
    def read(self, n=-1):
        if n < 0: n = self.size - self.pos
        data = self.read_at(self.pos, n); self.pos += len(data); return data
    def write(self, data): raise PermissionError('read-only overlay view')
    def flush(self): pass
    def fileno(self): return self.backing.fileno()
    def close(self): self.f.close(); self.backing.close()


def fat_entry(fs, path):
    parts = [p for p in path.split('/') if p]
    if parts[:3] in (['home', 'nexus', '.config'], ['home', 'nexus', '.cache']): raise ValueError('private profile path')
    hit = fs.find_name(fs.lookup_dir(parts[:-1]), parts[-1])
    if not hit: return None
    e = hit[2]
    return ((struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0], bool(e[11] & 0x10))


def collect(fs, path, prefix=''):
    """Files below a FAT directory: {relative path: bytes}."""
    first, size, is_dir = fat_entry(fs, path)
    if not is_dir: return {prefix.rstrip('/'): fs.read_chain(first, size)}
    files = {}
    for name, c, o, e, slots in fs.named_entries(first):
        if e[0] in (0, 0xE5) or e[11] == 0x0F or (e[11] & 8) or name in ('.', '..'): continue
        if not slots: name = name.lower()   # NexusOS shows 8.3-only entries in lower case
        files.update(collect(fs, path + '/' + name, prefix + name + '/'))
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('disk', type=Path)
    ap.add_argument('--tcg', action='store_true'); ap.add_argument('--keep-overlay', action='store_true')
    ap.add_argument('--timeout', type=int, default=900)
    args = ap.parse_args()
    disk = args.disk.resolve(); helper = OUT / 'apps_postinst_user.elf'
    if not helper.exists(): raise SystemExit('build first: python -I arch/x86_64/build.py build')
    fs = fatput.Fat32(str(disk)); flush = fs.flush_fat; fs.flush_fat = lambda: None
    try:
        hit = fat_entry(fs, 'nexus/desktop.args')
        original = fs.read_chain(hit[0], hit[1]) if hit else None
        if original is not None and b'apps/postinst' in original: raise SystemExit('desktop.args already points at the helper: restore it first')
        fs.put(str(helper), 'nexus/apps/postinst')
        with tempfile.NamedTemporaryFile('wb', delete=False, dir=OUT, suffix='.args') as t: t.write(b'--aurora\n/nexus/apps/postinst\n')
        fs.put(t.name, 'nexus/desktop.args'); os.unlink(t.name)
    finally:
        flush(); fs.close()
    overlay = disk.with_name(disk.stem + '-postinst.qcow2'); overlay.unlink(missing_ok=True)
    serial = disk.with_name(disk.stem + '-postinst.log'); serial.unlink(missing_ok=True)
    try:
        subprocess.run([str(QEMU_IMG), 'create', '-q', '-f', 'qcow2', '-b', str(disk), '-F', 'raw', str(overlay)], check=True)
        accel = ['-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt']
        qemu = subprocess.Popen([str(QEMU), *accel, '-smp', '2', '-m', '2048',
                                 '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
                                 '-drive', f'file={overlay},format=qcow2,if=ide', '-boot', 'a', '-display', 'none',
                                 '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
                                 '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        deadline = time.time() + args.timeout; log = ''
        while time.time() < deadline and qemu.poll() is None:
            log = serial.read_text(errors='replace') if serial.exists() else ''
            if 'APPS POSTINST DONE' in log: break
            time.sleep(1)
        time.sleep(3); qemu.kill(); qemu.wait()
        print('\n'.join(line for line in log.splitlines() if 'POSTINST' in line))
        if 'APPS POSTINST DONE' not in log: raise SystemExit('postinst helper did not finish; see ' + str(serial))
        view = Qcow2(str(overlay), str(disk))
        fatput.open = lambda path, mode='rb': view
        try:
            guest = fatput.Fat32(str(overlay)); results = collect(guest, GUEST_OUT)
        finally:
            del fatput.open; view.close()
    finally:
        fs = fatput.Fat32(str(disk)); flush = fs.flush_fat; fs.flush_fat = lambda: None
        try:
            if original is not None:
                with tempfile.NamedTemporaryFile('wb', delete=False, dir=OUT, suffix='.args') as t: t.write(original)
                fs.put(t.name, 'nexus/desktop.args'); os.unlink(t.name)
            fs.rm('nexus/apps/postinst')
        finally:
            flush(); fs.close()
        if not args.keep_overlay: overlay.unlink(missing_ok=True)
    installs = {}
    for name, data in results.items():
        if name == 'gschemas.compiled': installs['usr/share/glib-2.0/schemas/gschemas.compiled'] = data
        elif name == 'loaders.cache' and data.strip():
            installs['usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache'] = data
        elif name.startswith('mime/') and not name.startswith('mime/packages/'): installs['usr/share/' + name] = data
        elif name.startswith('xdg-cache/fontconfig/'): installs['var/cache/fontconfig/' + name.split('/')[-1]] = data
    required = ('usr/share/glib-2.0/schemas/gschemas.compiled', 'usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache', 'usr/share/mime/mime.cache')
    missing = [path for path in required if path not in installs]
    if missing: raise SystemExit('helper did not produce: ' + ', '.join(missing) + '; got: ' + ', '.join(sorted(results)[:40]))
    work = Path(tempfile.mkdtemp(dir=OUT, prefix='postinst-'))
    fs = fatput.Fat32(str(disk)); flush = fs.flush_fat; fs.flush_fat = lambda: None
    try:
        for index, (path, data) in enumerate(sorted(installs.items())):
            source = work / str(index); source.write_bytes(data); fs.put(str(source), path); source.unlink()
    finally:
        flush(); fs.close(); work.rmdir()
    print('[OK] installed %d generated files (%d bytes):' % (len(installs), sum(len(d) for d in installs.values())))
    for path in sorted(installs): print('   /' + path)


if __name__ == '__main__':
    main()
