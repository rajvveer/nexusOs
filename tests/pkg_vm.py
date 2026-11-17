"""Shared VM harness for the system-services / packaging / printing tests (test_x64_services.py, test_x64_apt.py, ...).

A prepared desktop disk (raw image, never written by the guest) is booted through a qcow2 overlay in WORK, which can be
kept across several boots (persistence checks) and read back on the host (Qcow2View + ext4lib). The machine matches the
desktop launcher (q35, NVMe root, e1000e on QEMU user networking, xHCI keyboard + tablet, virtio-vga); QMP runs over
TCP, the guest serial log is WORK/<tag>.log. A small HTTP server on the host (guest address 10.0.2.2) can serve files
and collect POSTed results.

Ports are per run: HookServer and Results take a free port by default (port 0), Vm(hook_port=...) tells the guest's
test-hook which one (kernel option nexus.testhook=PORT via fw_cfg opt/nexus/cmdline), and edit_overlay's qemu-nbd picks
a free port too, so several test VMs can run on one host without fetching each other's hooks.
"""
from pathlib import Path
import http.server
import importlib.util
import json
import re
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
QEMU_NBD = r'C:\Program Files\qemu\qemu-nbd.exe'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


ext4lib = module('pkg_vm_ext4lib', ROOT / 'userland/ext4lib.py')
fatput = module('pkg_vm_fatput', ROOT / 'userland/fatput.py')
nexus_paths = module('pkg_vm_nexus_paths', ROOT / 'arch/x86_64/nexus_paths.py')


def whpx_usable():
    return module('pkg_vm_build', ROOT / 'arch/x86_64/build.py').whpx_available()


def free_port():
    """A TCP port nobody listens on right now (127.0.0.1); used where the listener is another process (qemu-nbd)."""
    s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close(); return port


def test_hook_kind(fs):
    """'per-run' when the installed test-hook reads nexus.testhook= (this tree's), 'fixed' for an older image's hook
    (fetches port 8979), None without one. fs: guest_ext4() of the image."""
    hook = guest_read(fs, '/usr/lib/nexus/tests/test-hook')
    if hook is None: return None
    return 'per-run' if b'nexus.testhook=' in hook else 'fixed'


def compressed_dir(path):
    """A directory whose new files are NTFS-compressed: on this host sparse disk images still end up fully allocated,
    while compression stores the unused (zero) parts of an 8 GiB image in almost nothing. QEMU only reads base images."""
    path = Path(path); path.mkdir(parents=True, exist_ok=True)
    subprocess.run(['compact', '/c', '/q', str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return path


def sparse_copy(source, destination):
    source, destination = Path(source), Path(destination)
    if destination.exists(): raise FileExistsError(destination)
    destination.parent.mkdir(parents=True, exist_ok=True); destination.touch()
    subprocess.run(['fsutil', 'sparse', 'setflag', str(destination)], check=True, stdout=subprocess.DEVNULL)
    with source.open('rb') as a, destination.open('r+b') as b:
        b.truncate(source.stat().st_size)
        while block := a.read(1 << 20):
            if block.strip(b'\0'): b.write(block)
            else: b.seek(len(block), 1)


def fat_used_copy(source, destination):
    """A working copy of a FAT32 desktop disk that holds only what the volume uses: everything before the data area
    (partition table, boot sector, FATs) and the clusters the FAT marks used; free clusters stay unwritten (zero).
    In a compressed_dir() it takes about the used data, compressed - not the image size (the integrator's disks have
    old data in their free clusters, so a plain sparse copy of them is fully allocated). Returns bytes copied."""
    source, destination = Path(source), Path(destination)
    if destination.exists(): raise FileExistsError(destination)
    fs = fatput.Fat32(str(source), readonly=True)
    try:
        data_start = fs.base + fs.cl_off(2); used = [c for c in range(2, fs.nclusters + 2) if fs.fat_get(c)]   # file offsets: the partition starts at fs.base
        cl = fs.cl_bytes; size = source.stat().st_size
    finally: fs.close()
    copied = 0
    with source.open('rb') as a, destination.open('wb') as b:
        b.truncate(size)
        for offset in range(0, data_start, 1 << 20):   # partition table, reserved sectors and both FATs
            a.seek(offset); block = a.read(min(1 << 20, data_start - offset))
            if block.strip(b'\0'): b.seek(offset); b.write(block); copied += len(block)
        i = 0
        while i < len(used):   # runs of consecutive used clusters, 32 MiB at most per read
            j = i
            while j + 1 < len(used) and used[j + 1] == used[j] + 1 and j - i < 511: j += 1
            offset = data_start + (used[i] - 2) * cl; length = (j - i + 1) * cl
            a.seek(offset); block = a.read(length); b.seek(offset); b.write(block); copied += len(block); i = j + 1
        for offset in range(max(data_start, size - (1 << 20)), size, 1 << 16):   # the disk's last MiB (a GPT's backup header)
            a.seek(offset); block = a.read(1 << 16)
            if block.strip(b'\0'): b.seek(offset); b.write(block); copied += len(block)
    return copied


class Qcow2View:
    """Read-only file object over a qcow2 image (plain, zero and deflate-compressed clusters); the backing chain (raw or
    qcow2) is taken from the header unless `backing` names the raw image; without one, unallocated clusters read as zeros."""
    def __init__(self, path, backing=None):
        import struct
        self.f = open(path, 'rb')
        h = self.f.read(104)
        if h[:4] != b'QFI\xfb': raise ValueError('not qcow2')
        if backing is None:
            offset, length = struct.unpack('>QI', h[8:20])
            if offset: self.f.seek(offset); backing = self.f.read(length).decode()
        self.backing = None if backing is None else Qcow2View(backing) if str(backing).endswith('.qcow2') else open(backing, 'rb')
        self.cluster_bits = struct.unpack('>I', h[20:24])[0]; self.cluster = 1 << self.cluster_bits
        self.size = struct.unpack('>Q', h[24:32])[0]
        self.l1_size = struct.unpack('>I', h[36:40])[0]; l1_offset = struct.unpack('>Q', h[40:48])[0]
        self.f.seek(l1_offset); self.l1 = struct.unpack('>%dQ' % self.l1_size, self.f.read(8 * self.l1_size))
        self.l2_entries = self.cluster // 8; self.l2_cache = {}; self.pos = 0; self._struct = struct

    def _cluster(self, index):
        l1i, l2i = divmod(index, self.l2_entries)
        if l1i >= len(self.l1): return None
        l2_offset = self.l1[l1i] & 0x00FFFFFFFFFFFE00
        if not l2_offset: return None
        table = self.l2_cache.get(l2_offset)
        if table is None:
            self.f.seek(l2_offset); table = self._struct.unpack('>%dQ' % self.l2_entries, self.f.read(self.cluster))
            self.l2_cache[l2_offset] = table
        entry = table[l2i]
        if entry & (1 << 62):   # compressed cluster (qemu-img convert -c): raw deflate at a host offset, in 512-byte sectors
            shift = 62 - (self.cluster_bits - 8)
            offset = entry & ((1 << shift) - 1); sectors = ((entry >> shift) & ((1 << (self.cluster_bits - 8)) - 1)) + 1
            return ('deflate', offset, sectors * 512 - (offset & 511))
        if entry & 1 and not (entry & 0x00FFFFFFFFFFFE00): return 'zero'
        offset = entry & 0x00FFFFFFFFFFFE00
        return offset or None

    def read_at(self, offset, count):
        out = bytearray()
        while count > 0 and offset < self.size:
            inside = offset & (self.cluster - 1); part = min(count, self.cluster - inside, self.size - offset)
            where = self._cluster(offset >> self.cluster_bits)
            if isinstance(where, tuple):
                if getattr(self, '_inflated', (None,))[0] != where[1]:
                    import zlib
                    self.f.seek(where[1]); data = zlib.decompressobj(-15).decompress(self.f.read(where[2]), self.cluster)
                    self._inflated = (where[1], data + bytes(self.cluster - len(data)))
                out += self._inflated[1][inside:inside + part]
            elif where == 'zero' or (where is None and self.backing is None): out += bytes(part)
            elif where is None:
                if isinstance(self.backing, Qcow2View): data = self.backing.read_at(offset, part)
                else: self.backing.seek(offset); data = self.backing.read(part)
                out += data + bytes(part - len(data))
            else: self.f.seek(where + inside); out += self.f.read(part)
            offset += part; count -= part
        return bytes(out)

    def seek(self, pos, whence=0):
        self.pos = pos if whence == 0 else self.pos + pos if whence == 1 else self.size + pos; return self.pos

    def tell(self): return self.pos

    def read(self, n=-1):
        if n < 0: n = self.size - self.pos
        data = self.read_at(self.pos, n); self.pos += len(data); return data

    def write(self, data): raise PermissionError('read-only overlay view')

    def close(self):
        self.f.close()
        if self.backing: self.backing.close()


class CapturingView:
    """A writable file object over a read-only view (Qcow2View or raw file): writes are kept in memory per 4 KiB block."""
    BLOCK = 4096

    def __init__(self, view):
        self.view = view; self.blocks = {}; self.pos = 0
        self.size = view.size if hasattr(view, 'size') else view.seek(0, 2)

    def _block(self, n):
        if n not in self.blocks:
            self.view.seek(n * self.BLOCK); data = self.view.read(self.BLOCK); self.blocks[n] = bytearray(data + bytes(self.BLOCK - len(data)))
        return self.blocks[n]

    def read(self, count=-1):
        if count < 0: count = self.size - self.pos
        out = bytearray()
        while count > 0 and self.pos < self.size:
            n, inside = divmod(self.pos, self.BLOCK); part = min(count, self.BLOCK - inside, self.size - self.pos)
            if n in self.blocks: out += self.blocks[n][inside:inside + part]
            else: self.view.seek(self.pos); out += self.view.read(part)
            self.pos += part; count -= part
        return bytes(out)

    def write(self, data):
        data = memoryview(bytes(data)); done = 0
        while done < len(data):
            n, inside = divmod(self.pos, self.BLOCK); part = min(len(data) - done, self.BLOCK - inside)
            self._block(n)[inside:inside + part] = data[done:done + part]; self.pos += part; done += part
        return len(data)

    def seek(self, pos, whence=0):
        self.pos = pos if whence == 0 else self.pos + pos if whence == 1 else self.size + pos; return self.pos

    def tell(self): return self.pos
    def flush(self): pass
    def close(self): pass


def edit_overlay(below, overlay, edit, nbd_port=None):
    """Create the qcow2 `overlay` over `below` (a qcow2 overlay chain or a raw disk) holding the changes edit(fs) makes
    with an ext4lib.Writer: the Writer works on an in-memory capture of the chain, then the changed 4 KiB blocks are
    written into the new overlay through qemu-nbd (QEMU does the cluster allocation). Nothing below the new overlay is
    written. (qemu-io's "write -s" is no use here: on Windows it reads its pattern file in text mode, up to a 0x1A byte.)"""
    below, overlay = Path(below).resolve(), Path(overlay)
    overlay.unlink(missing_ok=True)
    subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(below), '-F', 'qcow2' if below.suffix == '.qcow2' else 'raw', str(overlay)], check=True)
    view = Qcow2View(str(overlay)); capture = CapturingView(view)
    original = ext4lib.__dict__.get('open')
    import builtins
    ext4lib.open = lambda path, mode='r', *a, **k: capture if str(path) == str(overlay) else builtins.open(path, mode, *a, **k)
    try:
        fs = ext4lib.Writer(str(overlay)); edit(fs); fs.close()
    finally:
        if original is None: del ext4lib.open
        else: ext4lib.open = original
        view.close()
    runs, numbers = [], sorted(capture.blocks)
    for n in numbers:
        if runs and runs[-1][1] == n: runs[-1][1] = n + 1
        else: runs.append([n, n + 1])
    with Nbd(overlay, nbd_port) as nbd:   # nbd_port None: a free port (retried if another process takes it first)
        for first, end in runs: nbd.write(first * CapturingView.BLOCK, b''.join(bytes(capture.blocks[n]) for n in range(first, end)))
        nbd.flush()
    check = Qcow2View(str(overlay))   # read back through the qcow2 metadata QEMU wrote
    try:
        for n in numbers:
            check.seek(n * CapturingView.BLOCK)
            if check.read(CapturingView.BLOCK) != bytes(capture.blocks[n]): raise RuntimeError('overlay block %d did not read back' % n)
    finally: check.close()
    return len(numbers)


class Nbd:
    """qemu-nbd serving one qcow2 image on 127.0.0.1:port (None: a free one), and a minimal NBD client (fixed newstyle,
    export '')."""
    def __init__(self, image, port=None):
        import struct
        self.struct = struct; self.handle = 0
        for attempt in range(5):
            chosen = port or free_port()
            self.proc = subprocess.Popen([QEMU_NBD, '-f', 'qcow2', '-b', '127.0.0.1', '-p', str(chosen), '--cache=writeback', str(image)],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            deadline = time.time() + 30; self.s = None
            while self.s is None:
                if self.proc.poll() is not None: break    # the port was taken meanwhile (or the image is busy)
                try: self.s = socket.create_connection(('127.0.0.1', chosen), timeout=60)
                except OSError:
                    if time.time() > deadline: break
                    time.sleep(0.2)
            if self.s is not None: break
            if self.proc.poll() is None: self.proc.kill()
            self.proc.wait()
            if port: raise RuntimeError('qemu-nbd did not start on port %d' % port)
        else: raise RuntimeError('qemu-nbd did not start')
        self.port = chosen
        hello = self._recv(18)
        if hello[:16] != b'NBDMAGICIHAVEOPT': raise RuntimeError('not a newstyle NBD server')
        self.s.sendall(struct.pack('>I', 3))                                   # FIXED_NEWSTYLE | NO_ZEROES
        self.s.sendall(b'IHAVEOPT' + struct.pack('>II', 1, 0))                # NBD_OPT_EXPORT_NAME ""
        self.size, self.flags = struct.unpack('>QH', self._recv(10))

    def _recv(self, n):
        data = b''
        while len(data) < n:
            chunk = self.s.recv(n - len(data))
            if not chunk: raise EOFError('NBD connection closed')
            data += chunk
        return data

    def _request(self, kind, offset, length, data=b''):
        self.handle += 1
        self.s.sendall(self.struct.pack('>IHHQQI', 0x25609513, 0, kind, self.handle, offset, length) + data)
        magic, error, handle = self.struct.unpack('>IIQ', self._recv(16))
        if magic != 0x67446698 or handle != self.handle or error: raise RuntimeError('NBD request %d failed: error %d' % (kind, error))

    def write(self, offset, data):
        for i in range(0, len(data), 1 << 20): self._request(1, offset + i, len(data[i:i + (1 << 20)]), data[i:i + (1 << 20)])

    def read(self, offset, length):
        self.handle += 1
        self.s.sendall(self.struct.pack('>IHHQQI', 0x25609513, 0, 0, self.handle, offset, length))
        magic, error, handle = self.struct.unpack('>IIQ', self._recv(16))
        if magic != 0x67446698 or error: raise RuntimeError('NBD read failed: error %d' % error)
        return self._recv(length)

    def flush(self): self._request(3, 0, 0)

    def __enter__(self): return self

    def __exit__(self, *exc):
        try: self.s.sendall(self.struct.pack('>IHHQQI', 0x25609513, 0, 2, 0, 0, 0)); self.s.close()
        except OSError: pass
        try: self.proc.wait(30)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def refresh_services(fs, aurora=True, tests=True):
    """Bring an installed test system up to the worktree's NexusOS services (sysinstall.py services --aurora --tests)
    through an open ext4lib.Writer, e.g. inside edit_overlay: tests run against the code they ship with."""
    sysinstall = module('pkg_vm_sysinstall', ROOT / 'arch/x86_64/sysinstall.py')
    sysinstall.install_services(None, aurora=aurora, tests=tests, fs=fs)


def put(fs, guest, data, mode=0o644, uid=0, gid=0):
    """Write (or replace) a regular file through an ext4lib.Writer."""
    fs.ensure_dir(fs.split(guest)[:-1])
    try:
        if fs.lookup(guest): fs.remove(guest)
    except (KeyError, ValueError, FileNotFoundError): pass
    fs.put_bytes(data.encode() if isinstance(data, str) else data, guest, mode, uid, gid)


def guest_ext4(disk, overlay=None):
    """ext4lib.Ext4 over the raw disk, or over the qcow2 overlay on top of it (read only, any backing chain)."""
    if overlay is None or not Path(overlay).exists(): return ext4lib.Ext4(str(disk))
    view = Qcow2View(str(overlay))
    original = ext4lib.__dict__.get('open')
    ext4lib.open = lambda path, mode='rb': view
    try: return ext4lib.Ext4(str(disk))
    finally:
        if original is None: del ext4lib.open
        else: ext4lib.open = original


def guest_read(fs, path):
    """Bytes of a regular guest file, or None."""
    try: ino = fs.lookup(path)
    except (KeyError, ValueError, FileNotFoundError): return None
    if ino is None: return None
    inode = fs.read_inode(ino)
    if (inode.mode & 0o170000) != 0o100000: return None
    return fs.read_file(inode)


class Results(http.server.ThreadingHTTPServer):
    """Host HTTP server: GET serves files from `root` (and registered byte blobs), POST bodies are collected. port 0:
    a free port (self.port)."""
    daemon_threads = True
    def __init__(self, port=0, root=None):
        self.root = Path(root) if root else None; self.posts = []; self.gets = []; self.blobs = {}
        server = self
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *a): pass
            def do_GET(self):
                path = self.path.split('?')[0]; server.gets.append(self.path)
                body = server.blobs.get(path)
                if body is None and server.root:
                    candidate = (server.root / path.lstrip('/')).resolve()
                    if server.root.resolve() in candidate.parents and candidate.is_file(): body = candidate.read_bytes()
                if body is None: self.send_error(404); return
                self.send_response(200); self.send_header('content-length', str(len(body)))
                self.send_header('cache-control', 'no-store'); self.end_headers(); self.wfile.write(body)
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get('content-length', 0)))
                server.posts.append((self.path, body))
                self.send_response(204); self.send_header('content-length', '0'); self.end_headers()
        super().__init__(('127.0.0.1', port), Handler); self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()


class HookServer(http.server.ThreadingHTTPServer):
    """Host side of a test on an installed system with sysinstall.py --tests: test-hook.service runs, as root after every
    boot unit, the script served at http://10.0.2.2:PORT/hook.sh, then the session ends and the machine powers off.
    PORT = self.port: a free one by default (port=0); boot the VM with Vm(hook_port=server.port). The hook script sees
    NEXUS_TEST_HOST=http://10.0.2.2:PORT (and @HOST@ in the hook and in served .sh/.conf/.sources/.list files is replaced
    by it).
      GET  /hook.sh, /files/NAME   the hook and its files (`files`: {name: bytes})
      GET  /gate/NAME              200 once the host opened the gate (open_gate), 404 before: the guest polls it
      POST /upload/NAME            stored in `uploads`; POST /event/TEXT is recorded in `events`
      GET  http://deb.debian.org/... (proxy form, apt's Acquire::http::Proxy "http://10.0.2.2:PORT/") goes to the real
           archive through a cache (pool files forever, indices 6 h). apt verifies the signatures itself; other hosts are
           refused (listed in `refused`)."""
    daemon_threads = True
    ARCHIVE = ('deb.debian.org',)

    def __init__(self, hook, files=None, uploads=None, proxy_cache=None, port=0):
        self.hook = hook.encode() if isinstance(hook, str) else hook; self.files = dict(files or {})
        self.uploads = Path(uploads) if uploads else None; self.proxy_cache = Path(proxy_cache) if proxy_cache else None
        self.gates, self.events, self.refused, self.proxied, self.cached = set(), [], [], 0, 0
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *a): pass
            def reply(self, code, body=b'', kind='application/octet-stream'):
                self.send_response(code); self.send_header('content-type', kind); self.send_header('content-length', str(len(body)))
                self.send_header('cache-control', 'no-store'); self.end_headers()
                if self.command != 'HEAD': self.wfile.write(body)
            def do_HEAD(self): self.do_GET()
            def do_CONNECT(self): server.refused.append('CONNECT ' + self.path); self.reply(403)   # no https tunnels
            def do_GET(self):
                url = urllib.parse.urlsplit(self.path)
                host = url.hostname if url.scheme == 'http' else None
                if host is None or host.startswith('10.0.2.2'):
                    if url.path == '/hook.sh': return self.reply(200, server.text(server.hook), 'text/plain')
                    if url.path.startswith('/files/') and url.path[7:] in server.files:
                        body = server.files[url.path[7:]]
                        return self.reply(200, server.text(body) if url.path.endswith(('.sh', '.conf', '.sources', '.list')) else body)
                    if url.path.startswith('/gate/'): return self.reply(200 if url.path[6:] in server.gates else 404, b'gate\n', 'text/plain')
                    return self.reply(404)
                if host not in server.ARCHIVE or not server.proxy_cache: server.refused.append(self.path); return self.reply(403)
                parts = [p for p in url.path.split('/') if p]
                if any(p in ('.', '..') or ':' in p or '\\' in p for p in parts): return self.reply(400)
                cache = server.proxy_cache.joinpath(host, *parts)
                volatile = '/dists/' in url.path
                if cache.is_file() and (not volatile or time.time() - cache.stat().st_mtime < 6 * 3600):
                    server.cached += 1; return self.reply(200, cache.read_bytes())
                try:
                    with urllib.request.urlopen(urllib.request.Request('https://%s%s' % (host, url.path), headers={'User-Agent': 'NexusOS-test-proxy'}), timeout=300) as r:
                        body = r.read()
                except urllib.error.HTTPError as e: return self.reply(e.code)
                except OSError: return self.reply(502)
                server.proxied += 1
                cache.parent.mkdir(parents=True, exist_ok=True); part = cache.with_name(cache.name + '.part'); part.write_bytes(body); part.replace(cache)
                return self.reply(200, body)
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get('content-length', 0)))
                path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
                if path.startswith('/event/'): server.events.append((time.time(), path[7:], body)); return self.reply(204)
                name = path[8:] if path.startswith('/upload/') else ''
                if not server.uploads or not name or '/' in name or '\\' in name or name.startswith('.'): return self.reply(404)
                server.uploads.mkdir(parents=True, exist_ok=True); (server.uploads / name).write_bytes(body); return self.reply(204)
        super().__init__(('127.0.0.1', port), Handler); self.port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

    @property
    def host(self): return 'http://10.0.2.2:%d' % self.port

    def text(self, body):
        """@HOST@ in served text (the hook, scripts, apt configuration) becomes this run's http://10.0.2.2:PORT."""
        body = body.encode() if isinstance(body, str) else body
        return body.replace(b'@HOST@', self.host.encode()) if b'@HOST@' in body else body

    def open_gate(self, name): self.gates.add(name)

    def close(self): self.shutdown(); self.server_close()


class Qmp:
    def __init__(self, port):
        deadline = time.time() + 60
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=120); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(.3)
        self.f = self.s.makefile('rwb'); self.read(); self.cmd('qmp_capabilities')

    def read(self):
        while True:
            line = self.f.readline()
            if not line: raise EOFError('QMP closed')
            message = json.loads(line)
            if 'event' not in message: return message

    def cmd(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None: request['arguments'] = arguments
        self.f.write(json.dumps(request).encode() + b'\n'); self.f.flush()
        reply = self.read()
        if 'error' in reply: raise RuntimeError(reply['error'])
        return reply.get('return')


TEST_PASSWORD = 'nexus-test-pw'   # the installer password of the services test base (test_x64_services.py config)


def greeter_login(vm, password=TEST_PASSWORD, after=0, seconds=600):
    """A base with a password and no automatic login boots Aurora to its greeter (AURORA GREETER reason=boot): log in
    there with the password, as the user would, before anything is typed into the session. Returns (ok, detail);
    ok without a greeter (automatic login)."""
    vm.wait(r'\[64\] AURORA READY', after, seconds)
    if not re.search(r'\[64\] AURORA GREETER reason=boot', vm.text()[after:]): return True, 'no greeter (automatic login)'
    time.sleep(2); mark = len(vm.text())
    vm.type(password); time.sleep(.3); vm.tap('ret')
    try:
        m = vm.wait(r'\[64\] AURORA (UNLOCKED user=\S+ mode=greeter|LOCK FAILED[^\n]*)', mark, 60)
    except TimeoutError: return False, 'no answer from the greeter'
    return m.group(1).startswith('UNLOCKED'), m.group(0)


class Vm:
    """One boot of `disk` through WORK/overlay.qcow2 (created when missing, kept when keep_overlay)."""
    def __init__(self, work, disk, qmp_port, tag='boot', accel='auto', smp=2, memory='2048', fresh_overlay=True,
                 extra=(), display='none', net='user,id=network', uefi=False, overlay_name='overlay.qcow2', hook_port=None,
                 cmdline='', uefi_pflash=None):
        """disk: a raw image, or a qcow2 overlay (its backing chain is used read-only). hook_port: this run's HookServer
        port (kernel option nexus.testhook=PORT for the guest's test-hook); None = no hook (nexus.testhook=none), so a
        hook server of another run is never fetched. cmdline: more kernel options (fw_cfg opt/nexus/cmdline).
        uefi_pflash=(CODE, VARS): OVMF as two flash devices (e.g. the secure-code build with an enrolled variable store,
        Secure Boot; the machine gets SMM), VARS is written by the firmware."""
        self.work = Path(work); self.work.mkdir(parents=True, exist_ok=True)
        self.disk = Path(disk).resolve(); self.tag = tag
        self.serial, self.overlay = self.work / f'{tag}.log', self.work / overlay_name
        self.serial.unlink(missing_ok=True)
        if fresh_overlay: self.overlay.unlink(missing_ok=True)
        if not self.overlay.exists():
            backing_format = 'qcow2' if self.disk.suffix == '.qcow2' else 'raw'
            subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(self.disk), '-F', backing_format, str(self.overlay)], check=True)
        # uefi_pflash (Secure Boot OVMF) needs SMM (q35,smm=on): WHPX has none ("System Management Mode not supported
        # by this hypervisor", QEMU exits at once), so such a machine always runs under TCG
        if accel == 'auto': accel = 'tcg' if uefi_pflash else ('whpx' if whpx_usable() else 'tcg')
        if accel == 'whpx' and uefi_pflash: raise ValueError('uefi_pflash (Secure Boot, SMM) needs accel tcg: WHPX has no System Management Mode')
        self.accel = accel
        acceleration = ['-accel', 'tcg', '-cpu', 'max'] if accel == 'tcg' else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU]
        if uefi:   # uefi: an ESP image (BOOTX64.EFI + KERNEL.BIN); OVMF as one ROM (-bios), as the launcher's --uefi does
            ovmf = OUT / 'ovmf-unified.fd'
            if not ovmf.is_file():
                share = Path(QEMU).parent / 'share'; ovmf.write_bytes((share / 'edk2-i386-vars.fd').read_bytes() + (share / 'edk2-x86_64-code.fd').read_bytes())
            firmware = (['-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={Path(uefi_pflash[0]).resolve()}',
                         '-drive', f'if=pflash,format=raw,unit=1,file={Path(uefi_pflash[1]).resolve()}'] if uefi_pflash else ['-bios', str(ovmf)])
            boot = [*firmware, '-drive', f'if=none,id=esp,format=raw,snapshot=on,file={Path(uefi).resolve()}',
                    '-device', 'ide-hd,drive=esp,bus=ide.2,bootindex=0']
        else:
            boot = ['-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a']
        if hook_port and int(hook_port) != 8979:   # an older image's test-hook only knows the fixed port 8979
            try:
                fs = guest_ext4(self.overlay, self.overlay); kind = test_hook_kind(fs); fs.f.close()
            except Exception: kind = None
            if kind == 'fixed':
                raise RuntimeError('%s has the old fixed-port test-hook (8979): refresh its services first (edit_overlay with '
                                   'refresh_services), or serve the hook on port 8979 under pkgvm_lock' % self.disk)
        options = ('nexus.testhook=%s %s' % (int(hook_port) if hook_port else 'none', cmdline)).strip()
        if any('opt/nexus/cmdline' in str(x) for x in extra): raise ValueError('pass kernel options as cmdline=, not in extra')
        command = [QEMU, *acceleration, '-machine', 'q35,smm=on' if uefi_pflash else 'q35', '-smp', str(smp), '-m', str(memory), *boot,
                   '-fw_cfg', 'name=opt/nexus/cmdline,string=' + options.replace(',', ',,'),
                   '-drive', f'if=none,id=root,file={self.overlay},format=qcow2', '-device', 'nvme,serial=nexus-root,drive=root',
                   '-vga', 'none', '-device', 'virtio-vga,id=video0', '-display', display,
                   '-netdev', net, '-device', 'e1000e,netdev=network',
                   '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,display=video0', '-device', 'usb-tablet,bus=xhci.0,display=video0',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{self.serial}',
                   '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off', '-no-reboot', *extra]
        (self.work / f'{tag}.cmd.json').write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, stdout=(self.work / f'{tag}.qemu.log').open('wb'), stderr=subprocess.STDOUT)
        print('VM', self.proc.pid, accel, tag, flush=True)
        try: nexus_paths.unthrottle(self.proc.pid)
        except Exception: pass
        self.q = Qmp(qmp_port); self.pos = [960, 540]

    def text(self): return self.serial.read_bytes().decode('utf-8', 'replace') if self.serial.exists() else ''

    def wait(self, pattern, after=0, seconds=300, fail=r'KERNEL_FAULT|\[64\] FAIL|PKGTEST FAIL[^\n]*'):
        end = time.time() + seconds
        while time.time() < end:
            log = self.text(); match = re.search(pattern, log[after:])
            if match: return match
            if fail:
                bad = re.search(fail, log)
                if bad: raise AssertionError('%s while waiting for %s' % (bad.group(0), pattern))
            if self.proc.poll() is not None: raise AssertionError('VM exited while waiting for ' + pattern)
            time.sleep(.25)
        raise TimeoutError(pattern)

    def exited(self, seconds):
        try: return self.proc.wait(seconds)
        except subprocess.TimeoutExpired: return None

    def move(self, dx, dy, step=40, delay=.03):
        while dx or dy:
            sx = max(-step, min(step, dx)); sy = max(-step, min(step, dy)); events = []
            if sx: events.append({'type': 'rel', 'data': {'axis': 'x', 'value': sx}})
            if sy: events.append({'type': 'rel', 'data': {'axis': 'y', 'value': sy}})
            self.q.cmd('input-send-event', {'events': events}); dx -= sx; dy -= sy; time.sleep(delay)

    def point(self, x, y, width=1920, height=1080):
        """Absolute pointer position (usb-tablet), in screen pixels."""
        # the tablet is bound to the display (usb-tablet,display=video0): name that console, or QEMU finds no abs handler
        self.q.cmd('input-send-event', {'device': 'video0', 'events': [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (width - 1))}},
                                                   {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (height - 1))}}]})
        self.pos = [x, y]; time.sleep(.08)

    def button(self, name, down):
        self.q.cmd('input-send-event', {'device': 'video0', 'events': [{'type': 'btn', 'data': {'button': name, 'down': down}}]}); time.sleep(.1)

    def click(self, x=None, y=None, name='left'):
        if x is not None: self.point(x, y); time.sleep(.15)
        self.button(name, True); self.button(name, False)

    def key(self, name, down):
        self.q.cmd('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': name}}}]}); time.sleep(.06)

    def tap(self, *names):
        for name in names: self.key(name, True)
        for name in reversed(names): self.key(name, False)
        time.sleep(.08)

    def type(self, text):
        shifted = {':': 'semicolon', '_': 'minus', '>': 'dot', '<': 'comma', '"': 'apostrophe', '|': 'backslash', '!': '1',
                   '&': '7', '?': 'slash', '*': '8', '(': '9', ')': '0', '$': '4', '#': '3', '@': '2', '%': '5', '+': 'equal',
                   '{': 'bracket_left', '}': 'bracket_right', '~': 'grave_accent', '^': '6'}
        plain = {' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash', '=': 'equal', ';': 'semicolon', "'": 'apostrophe',
                 ',': 'comma', '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent', '\t': 'tab'}
        for ch in text:
            if ch in shifted: self.key('shift', True); self.tap(shifted[ch]); self.key('shift', False)
            elif ch.isupper(): self.key('shift', True); self.tap(ch.lower()); self.key('shift', False)
            else: self.tap(plain.get(ch, ch))

    def shot(self, label):
        from PIL import Image
        ppm = self.work / f'{label}.ppm'
        self.q.cmd('screendump', {'filename': str(ppm)})
        with Image.open(ppm) as image: picture = image.convert('RGB')
        picture.save(self.work / f'{label}.png'); ppm.unlink(); return picture

    def faults(self):
        return [line for line in self.text().splitlines() if 'KERNEL_FAULT' in line or 'USER_FAULT' in line]

    def enosys(self):
        return sorted({int(m, 16) for m in re.findall(r'ENOSYS syscall=([0-9a-fA-F]+)', self.text())})

    def close(self, keep_overlay=False):
        if self.proc.poll() is None:
            try: self.q.cmd('quit')
            except Exception: pass
            try: self.proc.wait(30)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()
        if not keep_overlay: self.overlay.unlink(missing_ok=True)
