"""Stage files onto a desktop disk WITHOUT copying it: a qcow2 overlay on the read-only raw base image.

    import x64_overlay; x64_overlay.stage(BASE.img, WORK/overlay.qcow2, [(host_file, 'sbin/init'), ...])

The FAT edits (userland/fatput.py) run against a copy-on-write view of the base (reads fall through to the base,
writes stay in memory), and the 64 KiB clusters that contain a changed page are written into a fresh qcow2 overlay
whose backing file is the raw base. Boot the overlay (format=qcow2); the base is never opened for writing and nothing
multi-GB is copied. The qcow2 is built by hand (version 3, 64 KiB clusters) and checked with `qemu-img check`,
because qemu-io's two ways to put host data into an image both misbehave on Windows: `write -s FILE` opens FILE in
text mode (a 0x1A byte truncates the data) and the monitor reads nothing from a pipe.
"""
import builtins, importlib.util, os, re, struct, subprocess, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
QEMU_IMG = r'C:\Program Files\qemu\qemu-img.exe'
QEMU_IO = r'C:\Program Files\qemu\qemu-io.exe'
PAGE = 4096
CLUSTER_BITS = 16
CLUSTER = 1 << CLUSTER_BITS


class CowFile:
    """File-like view of `base` whose writes stay in memory (dirty pages)."""
    def __init__(self, base, scratch):
        self.f = builtins.open(base, 'rb'); self.f.seek(0, 2); self.size = self.f.tell()
        self.pos = 0; self.pages = {}; self.closed = False
        self.scratch = builtins.open(scratch, 'wb')   # a real descriptor for fatput's fsync
    def seek(self, offset, whence=0):
        self.pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset
        return self.pos
    def tell(self): return self.pos
    def _page(self, index):
        if index not in self.pages:
            self.f.seek(index * PAGE); self.pages[index] = bytearray(self.f.read(PAGE).ljust(PAGE, b'\0'))
        return self.pages[index]
    def read(self, n=-1):
        if n < 0: n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos)); out = bytearray()
        while n:
            index, offset = divmod(self.pos, PAGE); take = min(n, PAGE - offset)
            if index in self.pages: out += self.pages[index][offset:offset + take]
            else: self.f.seek(self.pos); out += self.f.read(take)
            self.pos += take; n -= take
        return bytes(out)
    def write(self, data):
        data = memoryview(bytes(data)); done = 0
        if self.pos + len(data) > self.size: raise ValueError('write beyond the end of the base image')
        while done < len(data):
            index, offset = divmod(self.pos, PAGE); take = min(len(data) - done, PAGE - offset)
            self._page(index)[offset:offset + take] = data[done:done + take]
            self.pos += take; done += take
        return len(data)
    def flush(self): pass
    def fileno(self): return self.scratch.fileno()
    def close(self):
        if not self.closed: self.f.close(); self.scratch.close(); self.closed = True


def _fatput():
    spec = importlib.util.spec_from_file_location('fatput_overlay', ROOT / 'userland/fatput.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def write_qcow2(overlay, base, size, clusters):
    """Write `overlay` as a qcow2 v3 (64 KiB clusters, refcount order 4) backed by raw `base`, holding exactly the
    data clusters `clusters` (a mapping cluster_index -> 64 KiB of data, read once each in index order). Every
    other cluster reads from the backing file. Layout: [0] header with the backing path right after it (as qemu-img
    writes it), refcount table, refcount blocks, L1 table, L2 tables, then the data clusters in index order."""
    backing = str(Path(base).resolve()).encode('utf-8')
    if len(backing) > 1023: raise ValueError('backing path too long for qcow2')
    l2_entries = CLUSTER // 8
    l1_size = (size + CLUSTER * l2_entries - 1) // (CLUSTER * l2_entries)
    l1_clusters = (l1_size * 8 + CLUSTER - 1) // CLUSTER
    indices = sorted(clusters)
    l2_tables = sorted({c // l2_entries for c in indices})
    refblock_entries = CLUSTER // 2                       # refcount_order 4 -> 16-bit refcounts
    n_refblocks, rt_clusters = 1, 1
    for _ in range(8):                                    # the refcount blocks must cover themselves too
        c_rb = 1 + rt_clusters
        c_l1 = c_rb + n_refblocks
        c_l2 = c_l1 + l1_clusters
        c_data0 = c_l2 + len(l2_tables)
        total = c_data0 + len(indices)
        need_rb = (total + refblock_entries - 1) // refblock_entries
        need_rt = (need_rb * 8 + CLUSTER - 1) // CLUSTER
        if (need_rb, need_rt) == (n_refblocks, rt_clusters): break
        n_refblocks, rt_clusters = need_rb, need_rt
    else:
        raise RuntimeError('qcow2 layout did not converge')
    meta = bytearray(c_data0 * CLUSTER)                  # every metadata cluster; data clusters are streamed after
    def put(cluster, offset, data): meta[cluster * CLUSTER + offset:cluster * CLUSTER + offset + len(data)] = data
    put(0, 0, struct.pack('>IIQIIQIIQQIIQQQQII',
                          0x514649FB, 3,                  # magic 'QFI\xfb', version 3
                          104, len(backing),              # backing file name: right after the header
                          CLUSTER_BITS, size, 0,          # cluster bits, virtual size, no encryption
                          l1_size, c_l1 * CLUSTER,        # L1 table
                          CLUSTER, rt_clusters,           # refcount table at cluster 1
                          0, 0,                           # no snapshots
                          0, 0, 0,                        # incompatible / compatible / autoclear features
                          4, 104))                        # refcount order, header length
    put(0, 104, backing)
    for i in range(n_refblocks): put(1, i * 8, struct.pack('>Q', (c_rb + i) * CLUSTER))
    for cluster in range(total):                          # every cluster of the file is used exactly once
        block, index = divmod(cluster, refblock_entries)
        put(c_rb + block, index * 2, struct.pack('>H', 1))
    l2_of = {table: c_l2 + i for i, table in enumerate(l2_tables)}
    for table, l2 in l2_of.items(): put(c_l1, table * 8, struct.pack('>Q', (l2 * CLUSTER) | (1 << 63)))
    for n, c in enumerate(indices):
        put(l2_of[c // l2_entries], (c % l2_entries) * 8, struct.pack('>Q', ((c_data0 + n) * CLUSTER) | (1 << 63)))
    with open(overlay, 'wb') as f:
        f.write(meta)
        for c in indices:
            data = clusters[c]
            if len(data) != CLUSTER: raise ValueError(f'cluster {c} is {len(data)} bytes')
            f.write(data)


class _Clusters:
    """The 64 KiB clusters that contain a dirty page of a CoW view, produced on demand (clean pages from the base),
    so an app-sized overlay is never held twice in memory."""
    def __init__(self, base, pages):
        self.base, self.pages, self.per = base, pages, CLUSTER // PAGE
        self.keys = sorted({index // self.per for index in pages}); self.src = builtins.open(base, 'rb')
    def __iter__(self): return iter(self.keys)
    def __len__(self): return len(self.keys)
    def __getitem__(self, c):
        self.src.seek(c * CLUSTER); data = bytearray(self.src.read(CLUSTER).ljust(CLUSTER, b'\0'))
        for p in range(c * self.per, c * self.per + self.per):
            if p in self.pages: data[(p - c * self.per) * PAGE:(p - c * self.per + 1) * PAGE] = self.pages[p]
        return bytes(data)
    def close(self): self.src.close()


def snapshot(view, base, overlay, pages=None):
    """Write the dirty pages of a CowFile `view` over raw `base` into a fresh qcow2 `overlay` (whole 64 KiB clusters:
    the clean part of each touched cluster comes from the base), then check it with qemu-img and read back the first
    and last dirty page. Returns the number of dirty pages."""
    base, overlay = Path(base).resolve(), Path(overlay)
    pages = view.pages if pages is None else pages
    clusters = _Clusters(base, pages)
    try:
        overlay.unlink(missing_ok=True)
        write_qcow2(overlay, base, view.size, clusters)
    finally:
        clusters.close()
    # qemu-img check validates the refcounts/metadata; a readback of the first and last dirty page confirms the data
    chk = subprocess.run([QEMU_IMG, 'check', str(overlay)], text=True, capture_output=True, stdin=subprocess.DEVNULL)
    if chk.returncode != 0:
        raise RuntimeError(f'qemu-img check failed: {chk.stdout} {chk.stderr}')
    for index in sorted({min(pages), max(pages)}) if pages else ():
        if _read_page(overlay, index) != bytes(pages[index]):
            raise RuntimeError(f'overlay page {index} did not read back')
    print(f'[overlay] {overlay.name}: {len(pages)} pages, {len(clusters)} clusters over {base.name}')
    return len(pages)


def _view_open(view, base):
    """An `open` for a loaded tool module: the base path opened for writing is the CoW view; read-only opens of the
    base hit the real (never written) file; anything else is a normal open."""
    def opener(path, mode='r', *a, **k):
        p = Path(path).resolve() if str(path) else Path(path)
        if p == base and any(c in mode for c in '+wa'): return view
        return builtins.open(path, mode, *a, **k)
    return opener


def stage(base, overlay, puts):
    """Create `overlay` (qcow2 backed by raw `base`) holding `puts` = [(host file, disk path), ...]."""
    base, overlay = Path(base).resolve(), Path(overlay)
    overlay.unlink(missing_ok=True)
    fat = _fatput(); work = Path(tempfile.mkdtemp(prefix='nx-overlay-', dir=overlay.parent))
    try:
        view = CowFile(base, work / 'fsync.tmp')
        fat.open = _view_open(view, base)
        args = ['fatput', str(base)]
        for host, target in puts: args += ['put', str(host), target]
        fat.edit(args)   # closes the view's base handle; the dirty data stays in view.pages
        view.close()
        return snapshot(view, base, overlay)
    finally:
        for p in work.iterdir(): p.unlink()
        work.rmdir()


class _OsProxy:
    """apps_install's `os`, with replace() neutralised (the "working copy" IS the CoW view) - without patching the
    real os module for the whole process."""
    def __init__(self): self.path = os.path
    def __getattr__(self, name): return getattr(os, name)
    @staticmethod
    def replace(src, dst, *a, **k): return None


def app_overlay(base, overlay, app, cache, session_tail=(), work=None, timeout=900,
                accel=('-accel', 'whpx,kernel-irqchip=off')):
    """One qcow2 overlay on the read-only raw `base` that RUNS a desktop app (arch/x86_64/apps_install.py `--only APP`):
      1. install the app into a CoW view of the base (only when it fits the free space: a --grow relocates gigabytes
         of clusters, so it is refused here);
      2. stage the apps_postinst helper and boot it on a throw-away qcow2 boot overlay to generate the GTK/glib/mime
         caches, then read them back through the qcow2 backing chain (apps_postinst.Qcow2 + collect);
      3. stage the caches, /sbin/init = out/desktop.elf and /nexus/chrome-desktop.args = the app's own launch file
         from its program line on (Aurora's session=chrome launcher execs argv directly, without the KEY=VALUE env
         lines) plus `session_tail`; write everything as `overlay`.
    Returns the program path of the session. The caller boots `overlay` with fw_cfg session=chrome and deletes it."""
    base, overlay = Path(base).resolve(), Path(overlay)
    out = ROOT / 'arch/x86_64/out'
    helper = out / 'apps_postinst_user.elf'
    if not helper.exists() or not (out / 'desktop.elf').exists():
        raise RuntimeError('build first: apps_postinst_user.elf and desktop.elf are missing from arch/x86_64/out')
    work = Path(work) if work else Path(tempfile.mkdtemp(prefix='nx-app-', dir=overlay.parent))
    work.mkdir(parents=True, exist_ok=True)
    apps = _load('x64_apps_install', ROOT / 'arch/x86_64/apps_install.py')
    post = _load('x64_apps_postinst', ROOT / 'arch/x86_64/apps_postinst.py')
    fat = _fatput()
    view = CowFile(base, work / 'fsync.tmp')
    view_close = view.close; view.close = lambda: None   # the tools' closes must not tear the shared view down
    try:
        # 1. install into the view: apps_install's "sparse working copy" is the view itself
        temp = {'path': None}
        def apps_open(path, mode='r', *a, **k):
            p = str(path)
            if any(c in mode for c in '+wa') and (Path(p).resolve() == base or p == temp['path']): return view
            if p == temp['path']: return builtins.open(base, mode, *a, **k)
            return builtins.open(path, mode, *a, **k)
        def no_grow(*a, **k): raise RuntimeError(f'{app} does not fit the base image without --grow; cannot overlay it')
        apps.open = apps_open
        apps.os = _OsProxy()
        apps.sparse_copy = lambda src, dst, length=None: temp.__setitem__('path', str(dst))
        apps.grow = no_grow
        try:
            rc = apps.main([str(base), '--only', app, '--cache', str(cache), '--report', str(work / 'install.json')])
        except SystemExit as e:   # e.g. "not enough space ... (use --grow MIB)"
            raise RuntimeError(f'apps_install --only {app}: {e}') from None
        if rc: raise RuntimeError(f'apps_install --only {app} returned {rc}')
        fat.open = _view_open(view, base)
        fs = fat.Fat32(str(base))
        try:
            hit = post.fat_entry(fs, f'nexus/apps/{app}.args')
            launch = fs.read_chain(hit[0], hit[1]).decode('utf-8', 'replace') if hit else ''
            hit = post.fat_entry(fs, 'nexus/desktop.args')
            original_args = fs.read_chain(hit[0], hit[1]) if hit else None
        finally:
            fs.close()
        lines = [l for l in launch.splitlines() if l.strip()]
        program = next((i for i, l in enumerate(lines) if l.startswith('/')), None)
        if program is None: raise RuntimeError(f'/nexus/apps/{app}.args has no program line: {launch!r}')
        # 2. caches: helper + desktop.args into the view, a boot overlay of it, the helper run, read back
        dargs = work / 'postinst.args'; dargs.write_text('--aurora\n/nexus/apps/postinst\n')
        fat.edit(['fatput', str(base), 'put', str(helper), 'nexus/apps/postinst', 'put', str(dargs), 'nexus/desktop.args'])
        stage1 = work / 'stage1.qcow2'; snapshot(view, base, stage1)
        boot = work / 'postinst-boot.qcow2'; boot.unlink(missing_ok=True)
        subprocess.run([QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(stage1), '-F', 'qcow2', str(boot)], check=True)
        serial = work / 'postinst.log'
        qemu = subprocess.Popen([QEMU, *accel,
                                 '-cpu', 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt', '-smp', '2', '-m', '2048',
                                 '-drive', f'file={out / "nexus64.img"},format=raw,if=floppy,readonly=on',
                                 '-drive', f'file={boot},format=qcow2,if=ide', '-boot', 'a', '-display', 'none',
                                 '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
                                 '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        try:
            deadline = time.time() + timeout; done = False
            while time.time() < deadline and qemu.poll() is None:
                if serial.exists() and 'APPS POSTINST DONE' in serial.read_text(errors='replace'): done = True; break
                time.sleep(2)
            time.sleep(3)
        finally:
            if qemu.poll() is None: qemu.kill()
            qemu.wait()
        if not done: raise RuntimeError(f'apps_postinst helper did not finish (see {serial})')
        chain = post.Qcow2(str(stage1), str(base))
        guest_view = post.Qcow2(str(boot), str(base))
        guest_view.backing.close(); guest_view.backing = chain    # boot -> stage1 -> raw base
        reader = _fatput(); reader.open = lambda path, mode='rb', *a, **k: guest_view
        try:
            results = post.collect(reader.Fat32(str(boot), readonly=True), post.GUEST_OUT)
        finally:
            guest_view.close(); chain.f.close()
        installs = {}
        for name, data in results.items():
            if name == 'gschemas.compiled': installs['usr/share/glib-2.0/schemas/gschemas.compiled'] = data
            elif name == 'loaders.cache' and data.strip(): installs['usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache'] = data
            elif name.startswith('mime/') and not name.startswith('mime/packages/'): installs['usr/share/' + name] = data
            elif name.startswith('xdg-cache/fontconfig/'): installs['var/cache/fontconfig/' + name.split('/')[-1]] = data
        missing = [p for p in ('usr/share/glib-2.0/schemas/gschemas.compiled', 'usr/share/mime/mime.cache',
                               'usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache') if p not in installs]
        if missing: raise RuntimeError('apps_postinst helper did not produce: ' + ', '.join(missing))
        # 3. caches + init + session into the view, written as the final overlay
        edits = ['fatput', str(base), 'put', str(out / 'desktop.elf'), 'sbin/init']
        for i, (path, data) in enumerate(sorted(installs.items())):
            src = work / f'cache{i}.bin'; src.write_bytes(data); edits += ['put', str(src), path]
        session = work / 'session.args'; session.write_text('\n'.join(lines[program:] + list(session_tail)) + '\n')
        edits += ['put', str(session), 'nexus/chrome-desktop.args', 'rm', 'nexus/apps/postinst']
        if original_args is not None:   # the helper's desktop.args goes back to what the image had
            restored = work / 'desktop.args'; restored.write_bytes(original_args)
            edits += ['put', str(restored), 'nexus/desktop.args']
        else:
            edits += ['rm', 'nexus/desktop.args']
        fat.edit(edits)
        snapshot(view, base, overlay)
        print(f'[overlay] {overlay.name}: {app} + {len(installs)} generated caches, session {lines[program]}')
        return lines[program]
    finally:
        view.close = view_close; view.close()
        for p in sorted(work.rglob('*'), reverse=True):
            p.unlink() if p.is_file() else p.rmdir()
        if work.exists(): work.rmdir()


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module); return module


def _read_page(overlay, index):
    r = subprocess.run([QEMU_IO, '-f', 'qcow2', '-r', '-c', f'read -v {index * PAGE} {PAGE}', str(overlay)],
                       text=True, capture_output=True, stdin=subprocess.DEVNULL, timeout=120)
    data = bytearray()
    for line in r.stdout.splitlines():   # "<hex offset>:  xx xx ... (16 bytes)  ascii"
        m = re.match(r'^[0-9a-fA-F]+:  ((?:[0-9a-fA-F]{2} ){15}[0-9a-fA-F]{2})', line)
        if m: data += bytes.fromhex(m.group(1).replace(' ', ''))
    return bytes(data[:PAGE])


def read_file(image, path):
    """Read a file off the FAT volume of a raw image (read-only)."""
    fat = _fatput(); fs = fat.Fat32(str(image), readonly=True)
    try:
        parts = path.strip('/').split('/'); found = fs.find_name(fs.lookup_dir(parts[:-1]), parts[-1])
        if not found or found[2][11] & 16: return None
        e = found[2]
        return fs.read_chain(struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0])
    finally:
        fs.close()
