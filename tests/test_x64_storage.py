"""Native storage: block layer, AHCI and NVMe drivers, MBR/GPT root selection, recovery, flush and throughput.

python -I tests/test_x64_storage.py <work> [--accel tcg,whpx] [--cases campaign,gpt,...] [--timeouts]

Boots the kernel in QEMU (q35 for AHCI/NVMe, pc for IDE; 2 vCPUs, so application processors poll while
holding the kernel lock) and asserts serial markers. Cases (all by default; 'timeouts' only with --timeouts):
  campaign  full on-disk campaigns of apps64.img on IDE, AHCI and NVMe; the image must stay unchanged
  gpt       GPT copies made by userland/gptwrap.py, full campaign each: ESP decoy + NexusOS root on AHCI,
            damaged primary header (backup GPT) on NVMe, 4 KiB logical blocks (4Kn) on NVMe
  multi     IDE (MBR) + AHCI (MBR) + NVMe (GPT NexusOS root, mdts=0 -> 2 MiB commands with chained PRP
            lists) at once: every disk self-tests and the documented root priority selects the NVMe GPT root
  writes    write campaign (nexus/test-writes) on IDE, AHCI and 4Kn NVMe: identical results on every bus
            and host-verified bytes of the persisted fixtures
  errors    blkdebug-injected read and write I/O errors: AHCI task-file error -> COMRESET -> retry,
            NVMe error status -> retry; data verified in the guest and on the host
  bench     16 MiB write+fsync then cold read after reboot, on IDE/AHCI/NVMe; FLUSH CACHE EXT / NVMe Flush
            counted in QEMU traces must equal the guest's flush count, and no write may follow the last flush
  timeouts  QMP-throttled writes -> AHCI command timeout + COMRESET / NVMe timeout + controller reset -> retry
Writes <work>/results.json (per accelerator: markers, BLOCKSTAT lines, the throughput table).
Disk budget: image copies are sparse, live only while their case runs (deleted even on failure) and at most two
exist at once; read-only extra disks use QEMU snapshot=on. Only logs and results.json remain.
"""
from pathlib import Path
import argparse, hashlib, importlib.util, json, os, re, shutil, socket, struct, subprocess, sys, time
from contextlib import contextmanager
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
WHPX_CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+xsave,+avx,+avx2'
ALL_CASES = ('campaign', 'gpt', 'multi', 'writes', 'errors', 'bench')
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('native64-storage-drivers'))
ap.add_argument('--accel', default='tcg,whpx')
ap.add_argument('--cases', default=','.join(ALL_CASES))
ap.add_argument('--timeouts', action='store_true', help='also run the slow QMP-throttled timeout recovery case')
ap.add_argument('--qmp-port', type=int, default=4520)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c] + (['timeouts'] if args.timeouts and 'timeouts' not in args.cases else [])
accels = [a for a in args.accel.split(',') if a]
for case in cases: assert case in ALL_CASES + ('timeouts',), case


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
fatput = module('storage_fatput', ROOT / 'userland/fatput.py')
storage = module('storage_grow', ROOT / 'storage.py')
build64 = module('storage_build64', ROOT / 'arch/x86_64/build.py')
if 'whpx' in accels and not build64.whpx_available():
    print('WHPX unavailable: running TCG only', flush=True); accels = [a for a in accels if a != 'whpx']


def digest(path):
    with open(path, 'rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()


def unthrottle(pid):
    """Windows 11 ignores timeBeginPeriod(1) for windowless processes and may put them on EcoQoS: headless
    QEMU then runs its device timers (NVMe queue processing among them) on the 15.6 ms host tick.
    Opt the QEMU process out of both power-throttling policies (as core_harness/hostqos.py does)."""
    if sys.platform != 'win32': return False
    import ctypes
    from ctypes import wintypes
    class State(ctypes.Structure):
        _fields_ = [('Version', wintypes.ULONG), ('ControlMask', wintypes.ULONG), ('StateMask', wintypes.ULONG)]
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True); kernel32.OpenProcess.restype = wintypes.HANDLE
    handle = kernel32.OpenProcess(0x0200, False, pid)
    if not handle: return False
    try:
        state = State(1, 0x1 | 0x4, 0)
        return bool(kernel32.SetProcessInformation(wintypes.HANDLE(handle), 4, ctypes.byref(state), ctypes.sizeof(state)))
    finally: kernel32.CloseHandle(wintypes.HANDLE(handle))


def copy_image(src, dst):
    """Sparse copy, flushed on the host: a guest FLUSH then costs only the guest's own writes, not
    gigabytes of dirty host cache left behind by the copy (that made flush timings meaningless)."""
    dst = Path(dst); dst.unlink(missing_ok=True); dst.touch()
    subprocess.run(['fsutil', 'sparse', 'setflag', str(dst)], check=True, stdout=subprocess.DEVNULL)
    with open(src, 'rb') as a, open(dst, 'r+b') as b:
        b.truncate(Path(src).stat().st_size)
        while block := a.read(1 << 20):
            if block.count(0) != len(block): b.write(block)
            else: b.seek(len(block), 1)
        b.flush(); os.fsync(b.fileno())
    return dst


def put(image, *pairs):
    argv = ['fatput', str(image)]
    for host, guest in pairs: argv += ['put', str(host), guest]
    assert fatput.edit(argv) == 0


def marker(image, name):
    flag = WORK / 'marker.txt'; flag.write_bytes(b'1'); put(image, (flag, 'nexus/' + name))


@contextmanager
def scratch(path):
    """An image that exists only while a case uses it."""
    path = Path(path)
    try: yield path
    finally: path.unlink(missing_ok=True)


def disk_args(bus, path, index=0, extra_device='', blkdebug=None, snapshot=False):
    """QEMU arguments attaching one raw image on IDE (index n), AHCI (own controller) or NVMe."""
    node = f'{bus}{index}'
    if snapshot:
        backend = ['-drive', f'if=none,id={node},file={path},format=raw,snapshot=on']
    elif blkdebug:
        rules = ''.join(f',file.inject-error.{i}.event={event},file.inject-error.{i}.errno=5,file.inject-error.{i}.once=on'
                        + (f',file.inject-error.{i}.sector={sector}' if sector is not None else '') for i, (event, sector) in enumerate(blkdebug))
        backend = ['-blockdev', f'driver=raw,node-name={node},file.driver=blkdebug,file.image.driver=file,file.image.filename={path}{rules}']
    else:
        backend = ['-drive', f'if=none,id={node},file={path},format=raw']
    if bus == 'ide': return backend + ['-device', f'ide-hd,drive={node},bus=ide.{index // 2},unit={index % 2}']
    if bus == 'ahci': return backend + ['-device', f'ahci,id=ahci{index}', '-device', f'ide-hd,drive={node},bus=ahci{index}.0']
    return backend + ['-device', f'nvme,serial=nx{index},drive={node}{extra_device}']


def boot(accel, tag, machine, disks, extra=(), timeout=150, qmp=None, monitor=None, debug_exit=True):
    """debug_exit=False keeps the VM alive after the kernel finishes (hlt), so a monitor can query it."""
    serial = WORK / accel / (tag + '.log'); serial.parent.mkdir(parents=True, exist_ok=True); serial.unlink(missing_ok=True)
    cpu = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', WHPX_CPU] if accel == 'whpx' else ['-cpu', 'max']
    cmd = [QEMU, '-machine', machine, *cpu, '-smp', '2', '-m', '256',
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
           '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
           '-no-reboot', *(['-device', 'isa-debug-exit,iobase=0xf4,iosize=4'] if debug_exit else ['-no-shutdown']), *disks, *extra]
    if qmp: cmd += ['-qmp', f'tcp:127.0.0.1:{qmp},server=on,wait=off']
    (serial.with_suffix('.cmd')).write_text(' '.join(map(str, cmd)))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    unthrottle(proc.pid)
    try:
        if monitor: monitor(proc, serial)
        output = proc.communicate(timeout=timeout)[0]
    except BaseException:
        proc.kill(); proc.communicate(); raise
    log = serial.read_text(errors='replace')
    return proc.returncode, log, output.decode(errors='replace')


def check_campaign(log):
    """The read-only disk campaign of tests/test_x64.py (check_log with disk=True)."""
    assert '[64] ALL TESTS PASSED' in log, log[-4000:]
    assert '[64] FAIL' not in log and 'KERNEL_FAULT' not in log, log[-4000:]
    assert 'FILE_WRITEBACK failed' not in log and 'FILE_CACHE read failed' not in log and 'PAGE_IO' not in log, log[-4000:]
    assert log.count('[64] USER_FAULT') == 14 and log.count('[64] USER PASS:') == 3, log[-4000:]
    for text in ('[64] DISK TESTS PASSED', '[64] DISK USER PASS:', '[64] FILE API PASS:', '[64] SIGNAL ALL PASSED:', '[64] IPC ALL PASSED:',
                 '[64] PROCESS ALL PASSED:', '[64] EXEC IMAGE ISOLATION PASSED:', '[64] THREAD ALL PASSED:', '[64] PIPE ALL PASSED:',
                 '[64] EVENT ALL PASSED:', '[64] PID ALL PASSED:', '[64] FS ALL PASSED:', '[64] SHMEM ALL PASSED:', '[64] LIMIT ALL PASSED:',
                 '[64] FILEMAP READONLY PASSED:', '[64] JIT ALL PASSED:', '[64] LOCK ALL PASSED:', '[64] STATFS API PASSED:',
                 '[64] NAMESPACE ALL PASSED:', '[64] DEVICE PASS:', '[64] UNIX ALL PASSED:', '[64] SMP: 2 CPUs online'):
        assert text in log, (text, log[-4000:])
    assert log.count('[64] DYNAMIC ALL PASSED') == 3, log[-4000:]


def blockstat(log, name):
    lines = [l for l in log.splitlines() if l.startswith('[64] BLOCKSTAT ' + name + ' ')]
    assert lines, ('no BLOCKSTAT for', name, log[-3000:])
    return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', lines[-1])}


def expect(log, *texts):
    for text in texts: assert text in log, (text, log[-5000:])


results = {}
apps = OUT / 'apps64.img'; apps_hash = digest(apps)
images = WORK / 'images'; images.mkdir(exist_ok=True)


def gpt(name, *options):
    path = images / name; path.unlink(missing_ok=True)
    subprocess.run([sys.executable, '-I', ROOT / 'userland/gptwrap.py', apps, path, *options], check=True, stdout=subprocess.DEVNULL)
    return path


def campaign_boot(accel, tag, machine, image, root_line, devices, extra=()):
    """Full read-only campaign from a private scratch image, which must come back bit-identical."""
    before = digest(image)
    rc, log, out = boot(accel, tag, machine, devices, extra)
    assert rc == 33, (tag, rc, out, log[-4000:])
    check_campaign(log); expect(log, root_line)
    assert digest(image) == before, tag + ': the read-only campaign modified its disk'
    return log


def run_campaign(accel, record):
    for bus, machine, name, root in (('ide', 'pc', 'ide0', '[64] ROOT ide0p1 (MBR FAT32, FAT32) on IDE'),
                                     ('ahci', 'q35', 'sata0', '[64] ROOT sata0p1 (MBR FAT32, FAT32) on AHCI'),
                                     ('nvme', 'q35', 'nvme0n1', '[64] ROOT nvme0n1p1 (MBR FAT32, FAT32) on NVMe')):
        with scratch(copy_image(apps, images / f'{accel}-campaign-{bus}.img')) as copy:
            log = campaign_boot(accel, 'campaign-' + bus, machine, copy, root, disk_args(bus, copy))
        expect(log, f'[64] BLOCK SELFTEST {name} PASS')
        stats = blockstat(log, name); assert stats['irq'] > 0 and stats['err'] == 0 and stats['rd'] > 0, stats
        if bus != 'ide': assert '[64] BLOCK no IDE controller' in log, log[-3000:]
        record['campaign_' + bus] = {'blockstat': stats, 'block_lines': [l for l in log.splitlines() if l.startswith(('[64] BLOCK ', '[64] PART', '[64] ROOT', '[64] AHCI', '[64] NVMe'))]}
        print(f'PASS {accel} campaign on {bus}: irq={stats["irq"]} cmds={stats["rd"]}', flush=True)


def run_gpt(accel, record):
    with scratch(gpt('gpt-esp.img', '--esp', '4')) as esp:
        log = campaign_boot(accel, 'gpt-ahci-esp', 'q35', esp, '[64] ROOT sata0p2 (GPT NexusOS root, FAT32) on AHCI start=10240', disk_args('ahci', esp))
    expect(log, '[64] PART sata0p1 GPT name="EFI system" start=2048 blocks=8192', '[64] PART sata0p2 GPT name="NEXUSROOT" start=10240')
    with scratch(gpt('gpt-damaged.img', '--damage-primary')) as damaged:
        log = campaign_boot(accel, 'gpt-nvme-backup', 'q35', damaged, '[64] ROOT nvme0n1p1 (GPT NexusOS root, FAT32) on NVMe start=2048', disk_args('nvme', damaged))
    expect(log, '[64] PART nvme0n1: primary GPT damaged; using the backup header')
    with scratch(gpt('gpt-4k.img', '--sector', '4096', '--type', 'basic')) as native:
        log = campaign_boot(accel, 'gpt-nvme-4kn', 'q35', native, '[64] ROOT nvme0n1p1 (GPT basic data, FAT32) on NVMe start=256 blocks=12288',
                            disk_args('nvme', native, extra_device=',logical_block_size=4096,physical_block_size=4096'))
    expect(log, ' bs=4096 ', '[64] BLOCK SELFTEST nvme0n1 PASS')
    record['gpt'] = {'esp_ahci': True, 'backup_header_nvme': True, 'nvme_4kn': blockstat(log, 'nvme0n1')}
    print(f'PASS {accel} GPT: ESP decoy on AHCI, backup header on NVMe, 4Kn NVMe', flush=True)


def run_multi(accel, record):
    """IDE and AHCI carry apps64.img read-only (snapshot=on; apps64's hash is checked at the end of the run)."""
    with scratch(gpt('gpt-esp.img', '--esp', '4')) as esp:
        devices = (disk_args('ide', apps, snapshot=True) + disk_args('ahci', apps, snapshot=True) + disk_args('nvme', esp) +
                   ['-drive', f'if=none,id=ahci1,file={apps},format=raw,snapshot=on', '-device', 'ide-hd,drive=ahci1,bus=ahci0.1'])  # 2nd port, same HBA
        log = campaign_boot(accel, 'multi', 'pc', esp, '[64] ROOT nvme0n1p2 (GPT NexusOS root, FAT32) on NVMe', devices, extra=('-global', 'nvme.mdts=0'))
    expect(log, '[64] BLOCK SELFTEST ide0 PASS', '[64] BLOCK SELFTEST sata0 PASS', '[64] BLOCK SELFTEST sata1 PASS', '[64] BLOCK SELFTEST nvme0n1 PASS',
           '[64] PART ide0p1 MBR type=0x0C', '[64] PART sata0p1 MBR type=0x0C', '[64] PART sata1p1 MBR type=0x0C', 'mdts_bytes=0x00200000')
    names = ('ide0', 'sata0', 'sata1', 'nvme0n1')
    for name in names: assert blockstat(log, name)['err'] == 0
    for name in names[:3]: assert blockstat(log, name)['wr'] == 0, (name, 'a non-root disk was written')
    record['multi'] = {name: blockstat(log, name) for name in names}
    print(f'PASS {accel} multi-disk: IDE + 2 AHCI ports on one HBA + NVMe, root by GPT type priority, chained PRP lists', flush=True)


def fixture(image, *parts):
    fs = fatput.Fat32(str(image))
    try:
        entry = fs.find_name(fs.lookup_dir(list(parts[:-1])), parts[-1])
        if not entry: return None
        entry = entry[2]; size = struct.unpack_from('<I', entry, 28)[0]
        first = struct.unpack_from('<H', entry, 20)[0] << 16 | struct.unpack_from('<H', entry, 26)[0]
        return fs.read_chain(first, size) if size else b''
    finally: fs.close()


def run_writes(accel, record):
    outcomes = {}
    for bus, machine, device in (('ide', 'pc', ''), ('ahci', 'q35', ''), ('nvme', 'q35', ',logical_block_size=4096,physical_block_size=4096')):
      with scratch(gpt(f'writes-{bus}.img', '--sector', '4096', '--type', 'basic') if bus == 'nvme' else copy_image(apps, images / f'{accel}-writes-{bus}.img')) as copy:
        marker(copy, 'test-writes')
        rc, log, out = boot(accel, 'writes-' + bus, machine, disk_args(bus, copy, extra_device=device))
        assert 'KERNEL_FAULT' not in log and 'FILE_WRITEBACK failed' not in log and 'PAGE_IO' not in log, log[-4000:]
        passed = sorted(set(re.findall(r'\[64\] ([A-Z][A-Z ]+(?:PASSED|PASS)\b)', log)))
        failures = [l for l in log.splitlines() if 'FAIL' in l]
        data = fixture(copy, 'home', 'nexus', 'native64-persist', 'data.bin')
        mapped = fixture(copy, 'home', 'nexus', 'native64-persist', 'mapped.bin')
        outcomes[bus] = {'exit': rc, 'passed': passed, 'failures': failures, 'data': data, 'mapped': mapped,
                         'blockstat': blockstat(log, {'ide': 'ide0', 'ahci': 'sata0', 'nvme': 'nvme0n1'}[bus])}
        expect(log, '[64] FILE WRITE API PASSED', '[64] FS MUTATIONS PASSED:', '[64] FILEMAP WRITE PASSED:', '[64] RENAME ALL PASSED')
        assert outcomes[bus]['blockstat']['wr'] > 0 and outcomes[bus]['blockstat']['flush'] > 0, outcomes[bus]['blockstat']
    expected = bytes(90 if i == 1 else ((i * 19 + 7) & 255) if i < 4099 else 0 for i in range(8193))
    for bus, o in outcomes.items():
        assert o['data'] == expected, (bus, 'data.bin differs from the expected bytes')
        assert o['passed'] == outcomes['ide']['passed'] and o['failures'] == outcomes['ide']['failures'] and o['exit'] == outcomes['ide']['exit'], (bus, o['passed'], o['failures'])
        assert o['mapped'] == outcomes['ide']['mapped'], bus
    # Every mutation test passes on every bus (statfs counts FAT allocations made at writeback since 2026-10-13).
    assert not outcomes['ide']['failures'], outcomes['ide']['failures']
    record['writes'] = {bus: {'exit': o['exit'], 'passed': len(o['passed']), 'failures': o['failures'], 'blockstat': o['blockstat']} for bus, o in outcomes.items()}
    print(f'PASS {accel} write campaign identical on IDE, AHCI and 4Kn NVMe ({len(outcomes["ide"]["passed"])} PASS markers, data.bin verified)', flush=True)


bench_image = None
def bench_disk():
    global bench_image
    if bench_image: return bench_image
    app = WORK / 'storage.elf'
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'arch/x86_64/storage_user.c'), '-o', str(app)], check=True)
    bench_image = images / 'bench-template.img'; bench_image.unlink(missing_ok=True)
    copy_image(apps, bench_image); storage.grow(bench_image, 2048); put(bench_image, (app, 'sbin/init'))
    return bench_image


def root_start(image):
    with open(image, 'rb') as f: mbr = f.read(512)
    return struct.unpack_from('<I', mbr, 446 + 8)[0]


def verify_bench(image):
    data = fixture(image, 'home', 'nexus', 'storage-bench.bin')
    block = bytes((i * 37 + 11) & 255 for i in range(65536))
    assert data is not None and hashlib.sha256(data).digest() == hashlib.sha256(block * 256).digest(), 'benchmark file bytes differ on the host'


def run_errors(accel, record):
    template = bench_disk(); start = root_start(template); out = {}
    for bus, machine, name in (('ahci', 'q35', 'sata0'), ('nvme', 'q35', 'nvme0n1')):
      with scratch(copy_image(template, images / f'{accel}-errors-{bus}.img')) as copy:
        rc, log, qemu = boot(accel, 'errors-' + bus, machine, disk_args(bus, copy, blkdebug=[('read_aio', start), ('write_aio', None)]))
        assert rc == 33 and '[64] INIT SESSION RETURNED' in log, (rc, qemu, log[-4000:])
        expect(log, '[64] STORAGE DATA VERIFIED')
        assert 'STORAGE BENCH FAIL' not in log and 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, log[-4000:]
        stats = blockstat(log, name)
        if bus == 'ahci':
            expect(log, '[AHCI] sata0: command error cmd=0x00000025', '[AHCI] sata0: command error cmd=0x00000035')
            assert log.count('[AHCI] sata0: port reset, link up') == 2 and stats['err'] == 2 and stats['reset'] == 2, stats
        else:
            expect(log, '[NVMe] nvme0: command error opcode=0x00000002', '[NVMe] nvme0: command error opcode=0x00000001')
            assert stats['err'] == 2, stats
        verify_bench(copy)
        out[bus] = stats
        print(f'PASS {accel} {bus}: injected read+write I/O errors recovered, data verified', flush=True)
    record['errors'] = out


class Qmp:
    """Minimal QMP client over TCP (ports from this agent's range)."""
    def __init__(self, port, proc):
        for _ in range(300):
            try: self.socket = socket.create_connection(('127.0.0.1', port), timeout=120); break
            except OSError:
                if proc.poll() is not None: raise AssertionError('QEMU exited before QMP connected')
                time.sleep(.1)
        else: raise TimeoutError('QMP')
        self.file = self.socket.makefile('rwb'); self.file.readline(); self.command('qmp_capabilities')
    def command(self, name, arguments=None):
        self.file.write(json.dumps({'execute': name, **({'arguments': arguments} if arguments else {})}).encode() + b'\n'); self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if 'event' not in reply:
                assert 'error' not in reply, reply
                return reply.get('return')
    def close(self): self.socket.close()


def wait_serial(proc, serial, marker, after=0, seconds=180):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        text = serial.read_text(errors='replace') if serial.exists() else ''
        if marker in text[after:]: return text
        if proc.poll() is not None: raise AssertionError(text[-4000:])
        time.sleep(.05)
    raise TimeoutError(marker)


def run_bench(accel, record):
    """16 MiB write+fsync, then a cold read after a reboot. The first boot keeps the VM alive after the kernel
    finishes (final write-back + block_flush_all + BLOCKSTAT) so QMP query-blockstats can count what reached the
    QEMU block layer: the device must have executed exactly the guest's FLUSH commands and written its bytes."""
    template = bench_disk(); table = {}
    for bus, machine, name in (('ide', 'pc', 'ide0'), ('ahci', 'q35', 'sata0'), ('nvme', 'q35', 'nvme0n1')):
      port = args.qmp_port + 3; device = {}
      with scratch(copy_image(template, images / f'{accel}-bench-{bus}.img')) as copy:
        def monitor(proc, serial):
            wait_serial(proc, serial, f'[64] BLOCKSTAT {name} ')
            qmp = Qmp(port, proc)
            device.update(next(s for s in qmp.command('query-blockstats') if s.get('device') == bus + '0')['stats'])
            qmp.command('quit'); qmp.close()
        rc, log, qemu = boot(accel, 'bench-' + bus, machine, disk_args(bus, copy), qmp=port, monitor=monitor, debug_exit=False,
                             extra=('-trace', 'pci_nvme_flush_ns') if bus == 'nvme' else ())
        if bus == 'nvme': device['flush_operations'] = qemu.count('pci_nvme_flush_ns')  # not accounted in blockstats
        assert '[64] STORAGE DATA VERIFIED' in log and '[64] INIT SESSION RETURNED' in log and 'STORAGE BENCH FAIL' not in log, (rc, qemu, log[-4000:])
        write_ms = int(re.search(r'STORAGE WRITE bytes=16777216 ms=(\d+)', log).group(1))
        fsync_ms = int(re.search(r'STORAGE FSYNC ms=(\d+)', log).group(1))
        stats = blockstat(log, name)
        assert stats['flush'] >= 1 and device['flush_operations'] == stats['flush'], (bus, 'device flushes', device['flush_operations'], 'guest', stats['flush'])
        assert device['wr_operations'] == stats['wr'] and device['wr_bytes'] == stats['wrsec'] * 512, (bus, device, stats)
        assert stats['pending'] == 0, (bus, 'writes left unflushed at shutdown')
        rc2, log2, qemu2 = boot(accel, 'bench-reboot-' + bus, machine, disk_args(bus, copy))
        assert rc2 == 33 and '[64] STORAGE DATA VERIFIED' in log2 and 'STORAGE WRITE' not in log2, (rc2, qemu2, log2[-4000:])
        read_ms = int(re.search(r'STORAGE READ bytes=16777216 ms=(\d+) cold=1', log2).group(1))
        stats2 = blockstat(log2, name)
        verify_bench(copy)
        table[bus] = {'write_fsync_ms': write_ms, 'fsync_ms': fsync_ms, 'write_MBps': round(16 / max(write_ms, 1) * 1000, 1),
                      'cold_read_ms': read_ms, 'read_MBps': round(16 / max(read_ms, 1) * 1000, 1),
                      'device_flushes': device['flush_operations'], 'device_wr_bytes': device['wr_bytes'], 'guest_flushes': stats['flush'],
                      'write_boot': stats, 'read_boot': stats2}
        print(f'PASS {accel} bench {bus}: 16 MiB write+fsync {write_ms} ms (fsync {fsync_ms}), cold read {read_ms} ms; device executed {device["flush_operations"]}/{stats["flush"]} FLUSH, {device["wr_operations"]} writes', flush=True)
    record['bench'] = table


def run_timeouts(accel, record):
    """A throttled write outlives the 30 s command timeout: AHCI resets the port (COMRESET), NVMe resets the
    controller and recreates its queues, the retry succeeds once the throttle is lifted, and the bytes persist."""
    app = WORK / 'recovery.elf'; payload = WORK / 'recovery.dat'; payload.write_bytes(bytes([0xa5]) * 8192)
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'arch/x86_64/ata_recovery_user.c'), '-o', str(app)], check=True)
    out = {}
    for bus, name, timeout_text, reset_text in (('ahci', 'sata0', '[AHCI] sata0: command timeout cmd=0x00000035', '[AHCI] sata0: port reset, link up'),
                                                ('nvme', 'nvme0n1', '[NVMe] nvme0: command timeout opcode=0x00000001', '[NVMe] nvme0: controller reset, queues recreated')):
      with scratch(copy_image(apps, images / f'{accel}-timeouts-{bus}.img')) as copy:
        put(copy, (app, 'sbin/init'), (payload, 'home/nexus/ata-recovery.dat'))
        port = args.qmp_port + 1 + (bus == 'nvme')
        def monitor(proc, serial):
            text = wait_serial(proc, serial, 'ATA RECOVERY WRITE READY')
            qmp = Qmp(port, proc); node = bus + '0'
            qmp.command('block_set_io_throttle', {'device': node, 'bps': 0, 'bps_rd': 0, 'bps_wr': 128, 'iops': 0, 'iops_rd': 0, 'iops_wr': 0})
            wait_serial(proc, serial, timeout_text, len(text), 120)
            qmp.command('block_set_io_throttle', {'device': node, 'bps': 0, 'bps_rd': 0, 'bps_wr': 0, 'iops': 0, 'iops_rd': 0, 'iops_wr': 0})
            qmp.close()
        rc, log, qemu = boot(accel, 'timeouts-' + bus, 'q35', disk_args(bus, copy), qmp=port, monitor=monitor, timeout=300)
        assert rc == 33 and 'ATA RECOVERY ALL PASSED' in log and reset_text in log and 'ATA RECOVERY FAIL' not in log, (rc, qemu, log[-4000:])
        rc2, log2, _ = boot(accel, 'timeouts-reboot-' + bus, 'q35', disk_args(bus, copy))
        assert rc2 == 33 and 'ATA RECOVERY REBOOT PASSED' in log2, log2[-3000:]
        assert fixture(copy, 'home', 'nexus', 'ata-recovery.dat') == bytes((i * 37 + 11) & 255 for i in range(8192))
        out[bus] = blockstat(log, name)
        print(f'PASS {accel} {bus}: write timeout -> {"COMRESET" if bus == "ahci" else "controller reset"} -> retry; bytes persisted', flush=True)
    record['timeouts'] = out


RUNNERS = {'campaign': run_campaign, 'gpt': run_gpt, 'multi': run_multi, 'writes': run_writes, 'errors': run_errors, 'bench': run_bench, 'timeouts': run_timeouts}
try:
    for accel in accels:
        record = results.setdefault(accel, {})
        for case in cases: RUNNERS[case](accel, record)
finally:
    (WORK / 'results.json').write_text(json.dumps(results, indent=1, default=str) + '\n')
    if bench_image and bench_image.exists(): bench_image.unlink()
assert digest(apps) == apps_hash, 'apps64.img changed during the storage tests'
if 'bench' in cases:
    for accel in accels:
        print(accel, 'throughput (16 MiB):', ', '.join(f'{bus} write+fsync {v["write_MBps"]} MB/s read {v["read_MBps"]} MB/s' for bus, v in results[accel]['bench'].items()), flush=True)
print('PASS native storage: block layer, AHCI, NVMe, MBR/GPT root selection (' + ','.join(accels) + ')', flush=True)
