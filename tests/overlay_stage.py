"""Stage files into a desktop disk without copying it: a qcow2 overlay on the read-only raw base holds only the
clusters userland/fatput.py changes (computed on a copy-on-write view of the base; the qcow2 v3 file is written
here, then checked with `qemu-img check`).

    import overlay_stage; overlay_stage.stage(base_raw, overlay_qcow2, {'sbin/init': Path(...), 'nexus/x.args': b'...'})

The base image is never opened for writing; the overlay holds a few MiB, so tests need no multi-GB disk copy.
QEMU then boots the overlay (its own writes go into the overlay too)."""
from pathlib import Path
import importlib.util
import os
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
CLUSTER = 1 << 16


class CowFile:
    """File-like view of `base`: reads fall through, writes are kept per 64 KiB cluster in memory."""
    def __init__(self, base, scratch):
        self.base = open(base, 'rb'); self.base.seek(0, 2); self.size = self.base.tell(); self.pos = 0; self.chunks = {}
        self.closed = False; self.scratch = open(scratch, 'wb')   # fatput fsyncs its file: a real descriptor for that
    def _chunk(self, index):
        if index not in self.chunks:
            self.base.seek(index * CLUSTER); data = self.base.read(CLUSTER)
            self.chunks[index] = bytearray(data.ljust(CLUSTER, bytes(1)))
        return self.chunks[index]
    def seek(self, offset, whence=0):
        self.pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset
        return self.pos
    def tell(self): return self.pos
    def read(self, n=-1):
        if n < 0: n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos)); out = bytearray()
        while n:
            index, inside = divmod(self.pos, CLUSTER); part = min(n, CLUSTER - inside)
            if index in self.chunks: out += self.chunks[index][inside:inside + part]
            else: self.base.seek(self.pos); out += self.base.read(part)
            self.pos += part; n -= part
        return bytes(out)
    def write(self, data):
        data = memoryview(bytes(data)); done = 0
        while done < len(data):
            index, inside = divmod(self.pos, CLUSTER); part = min(len(data) - done, CLUSTER - inside)
            self._chunk(index)[inside:inside + part] = data[done:done + part]
            self.pos += part; done += part
        self.size = max(self.size, self.pos)
        return len(data)
    def flush(self): pass
    def fileno(self): return self.scratch.fileno()
    def truncate(self, size=None): pass
    def close(self): self.closed = True
    def __enter__(self): return self
    def __exit__(self, *a): pass


def write_qcow2(path, backing, size, clusters):
    """A qcow2 v3 image (64 KiB clusters, 16-bit refcounts) over raw `backing` whose only allocated guest clusters
    are `clusters` {guest cluster index: 64 KiB bytes}."""
    entries = CLUSTER // 8                                   # L2 entries per table
    l1_size = -(-size // (CLUSTER * entries))
    tables = sorted({index // entries for index in clusters})
    layout = 1 + 1                                           # header, refcount table
    l1_clusters = -(-l1_size * 8 // CLUSTER)
    data_count = len(clusters)
    used = layout + l1_clusters + len(tables) + data_count
    per_block = CLUSTER // 2
    blocks = -(-(used + 4) // per_block)
    used += blocks
    refcount_table, refcount_blocks = CLUSTER, 2 * CLUSTER
    l1_offset = refcount_blocks + blocks * CLUSTER
    l2_offset = l1_offset + l1_clusters * CLUSTER
    data_offset = l2_offset + len(tables) * CLUSTER
    name = str(backing).encode()
    header = struct.pack('>4sIQIIQIIQQIIQQQQII', b'QFI\xfb', 3, 128, len(name), 16, size, 0, l1_size, l1_offset,
                         refcount_table, 1, 0, 0, 0, 0, 0, 4, 104)
    image = bytearray(data_offset + data_count * CLUSTER)
    image[0:len(header)] = header
    struct.pack_into('>II8s', image, 104, 0xE2792ACA, 3, b'raw')   # header extension: backing file format
    struct.pack_into('>II', image, 120, 0, 0)                     # end of header extensions
    image[128:128 + len(name)] = name                             # backing file name
    struct.pack_into('>Q', image, refcount_table, refcount_blocks)
    for b in range(1, blocks): struct.pack_into('>Q', image, refcount_table + 8 * b, refcount_blocks + b * CLUSTER)
    for c in range(len(image) // CLUSTER): struct.pack_into('>H', image, refcount_blocks + 2 * c, 1)
    for t, table in enumerate(tables): struct.pack_into('>Q', image, l1_offset + 8 * table, (l2_offset + t * CLUSTER) | (1 << 63))
    for d, index in enumerate(sorted(clusters)):
        t = tables.index(index // entries)
        struct.pack_into('>Q', image, l2_offset + t * CLUSTER + 8 * (index % entries), (data_offset + d * CLUSTER) | (1 << 63))
        image[data_offset + d * CLUSTER:data_offset + (d + 1) * CLUSTER] = bytes(clusters[index])
    Path(path).write_bytes(bytes(image))


def stage(base, overlay, files):
    base, overlay = Path(base).resolve(), Path(overlay)
    overlay.unlink(missing_ok=True)
    spec = importlib.util.spec_from_file_location('overlay_fatput', ROOT / 'userland/fatput.py')
    fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
    work = Path(tempfile.mkdtemp(prefix='stage-', dir=overlay.parent)); view = CowFile(base, work / 'fsync.tmp')
    real_open = open
    fatput.open = lambda path, mode='rb', *a, **k: view if os.path.abspath(path) == str(base) else real_open(path, mode, *a, **k)
    try:
        args = ['fatput', str(base)]
        for guest, host in files.items():
            if not isinstance(host, Path):
                data = host.encode() if isinstance(host, str) else host
                host = work / guest.replace('/', '_'); host.write_bytes(data)
            args += ['put', str(host), guest.strip('/')]
        if fatput.edit(args) != 0: raise RuntimeError('fatput failed')
        write_qcow2(overlay, base, base.stat().st_size, view.chunks)
        check = subprocess.run([QEMU_IMG, 'check', '-q', str(overlay)], capture_output=True, text=True)
        if check.returncode: raise RuntimeError('qemu-img check: ' + check.stdout + check.stderr)
        return len(view.chunks) * CLUSTER
    finally:
        view.scratch.close(); view.base.close()
        for f in work.iterdir(): f.unlink()
        work.rmdir()
