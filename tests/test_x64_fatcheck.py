"""FAT32 root: dirty flag, boot-time check/repair, orphans at shutdown and power cuts (kernel/fat32.c +
arch/x86_64/fatcheck.inc; guest program arch/x86_64/fatcheck_user.c as /sbin/init, as root, on a copy of apps64.img).

  repair      damage planted by tests/fatcorrupt.py (lost chains, long/short chains, cross-link, orphaned long-name
              slots, an unpublished staged entry, an out-of-range FAT entry, wrong '..', a sized directory, a
              duplicate entry from an interrupted rename, an entry after the end marker, differing FAT copies,
              dirty flag): the boot check repairs it to exactly the expected state; userland/fatfsck.py -n is clean
  checkonly   the same damage with fsck.repair=no: reported, mounted read-only (EROFS), the image is byte-identical
  force       a clean-flagged volume with lost clusters: untouched by default (check skipped), repaired with fsck.mode=force
  orphans     8 files unlinked while open (some mapped and written after the unlink), reboot(POWER_OFF) with them held:
              no lost clusters, clean flag set
  crash       the same files held, then the VM is killed: dirty + lost clusters on the host; the next boot frees exactly
              those (kernel count = host count) and the volume is clean
  powerloss   --rounds random power cuts during a create/replace/rename/unlink/truncate/append/orphan workload; every
              boot repairs and verifies the journal of durable operations; a final verify boot leaves a clean volume
Every case runs on 1 and 2 CPUs unless --smp is given.

Usage: test_x64_fatcheck.py WORK [--cases repair,checkonly,force,orphans,crash,powerloss] [--smp 1,2] [--rounds 6]
       [--tcg] [--bus ide|ahci|nvme] [--qmp-port N (unused; kept for w2_suite)]"""
from pathlib import Path
import argparse, hashlib, importlib.util, random, shutil, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')
WHPX_CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
fatput = load('fc_fatput', ROOT / 'userland/fatput.py')
fatfsck = load('fc_fatfsck', ROOT / 'userland/fatfsck.py')
fatcorrupt = load('fc_fatcorrupt', ROOT / 'tests/fatcorrupt.py')

ap = argparse.ArgumentParser()
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='repair,checkonly,force,orphans,crash,powerloss')
ap.add_argument('--smp', default='1,2')
ap.add_argument('--rounds', type=int, default=6)
ap.add_argument('--tcg', action='store_true')
ap.add_argument('--bus', default='ide', choices=('ide', 'ahci', 'nvme'))
ap.add_argument('--qmp-port', type=int, default=4730)
ap.add_argument('--seed', type=int, default=None)
args = ap.parse_args()
WORK = args.work; WORK.mkdir(parents=True, exist_ok=True)
rnd = random.Random(args.seed)
app = WORK / 'fatcheck_user.elf'
subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/fatcheck_user.c'), '-o', str(app)], check=True)
results = []


def check(ok, what, detail=''):
    results.append((ok, what)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True)
    return ok


def stage(disk, mode):
    conf = WORK / 'fatcheck.conf'; conf.write_text(mode + '\n', newline='\n')
    marker = WORK / 'init-as-root'; marker.write_text('fatcheck runs init as root\n', newline='\n')
    fatput.edit(['fatput', str(disk), 'put', str(app), 'sbin/init', 'put', str(conf), 'nexus/fatcheck.conf', 'put', str(marker), 'nexus/init-as-root'])


def fresh(tag):
    disk = WORK / (tag + '.img'); disk.unlink(missing_ok=True); shutil.copyfile(OUT / 'apps64.img', disk); return disk


def qemu(disk, serial, cpus, cmdline=''):
    if args.bus == 'ide': drive = ['-drive', f'file={disk},format=raw,if=ide']
    elif args.bus == 'ahci': drive = ['-drive', f'file={disk},format=raw,if=none,id=d0', '-device', 'ahci,id=ahci', '-device', 'ide-hd,drive=d0,bus=ahci.0']
    else: drive = ['-drive', f'file={disk},format=raw,if=none,id=d0', '-device', 'nvme,drive=d0,serial=fatcheck']
    accel = ['-accel', 'tcg,thread=multi' if cpus > 1 else 'tcg', '-cpu', 'max'] if args.tcg else \
            ['-accel', 'whpx,kernel-irqchip=off', '-cpu', WHPX_CPU]
    return [str(QEMU), *accel, '-m', '512', '-smp', str(cpus), '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
            *drive, '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', *(['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}'] if cmdline else [])]


def boot(disk, tag, cpus, cmdline='', timeout=600):
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    try: code = subprocess.run(qemu(disk, serial, cpus, cmdline), capture_output=True, timeout=timeout).returncode
    except subprocess.TimeoutExpired: code = 'timeout'
    return code, serial.read_text(errors='replace') if serial.exists() else ''


def boot_and_kill(disk, tag, cpus, marker, delay, timeout=600):
    """Run until `marker` is on the serial line, wait `delay` seconds more, then kill the VM (a power cut)."""
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    vm = subprocess.Popen(qemu(disk, serial, cpus), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    seen, start = None, time.time()
    try:
        while time.time() - start < timeout and vm.poll() is None:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if marker in text and seen is None: seen = time.time()
            if seen is not None and time.time() - seen >= delay: break
            if '[64] FATCHECK FAIL' in text or 'KERNEL_FAULT' in text: break
            time.sleep(0.2)
    finally:
        alive = vm.poll() is None
        if alive: vm.kill()
        vm.wait()
    return seen is not None and alive, serial.read_text(errors='replace') if serial.exists() else ''


def bad(log):
    return [w for w in ('[64] FATCHECK FAIL', 'KERNEL_FAULT', '[64] FAIL', 'USER_FAULT', 'panic') if w in log]


def kernel_lost(log):
    for line in log.splitlines():
        if '[FAT32] check:' in line and ' lost, ' in line:
            return int(line.split(' clusters in use, ')[1].split(' lost')[0])
    return None


def case_repair(cpus):
    disk = fresh(f'repair-smp{cpus}'); stage(disk, 'verify'); expect = fatcorrupt.plant(disk)
    code, log = boot(disk, f'repair-smp{cpus}', cpus)
    ok = check(code in (0, 33) and 'repaired in' in log and 'FATCHECK DONE' in log and not bad(log), f'repair smp{cpus}: boot check repaired the damage', log[-4000:])
    if ok:
        problems = fatcorrupt.verify_repaired(disk, expect)
        check(not problems, f'repair smp{cpus}: repaired state as expected', problems)
        status, r = fatfsck.check(disk)
        check(status == 0, f'repair smp{cpus}: host fatfsck clean afterwards', r.problems)
        check(kernel_lost(log) == expect['lost'], f'repair smp{cpus}: the kernel freed exactly the {expect["lost"]} planted lost clusters ({kernel_lost(log)})')
        check('[FAT32] volume cleanly unmounted' in log, f'repair smp{cpus}: clean unmount after the session')
    disk.unlink(missing_ok=True)


def case_checkonly(cpus):
    disk = fresh(f'checkonly-smp{cpus}'); stage(disk, 'readonly'); fatcorrupt.plant(disk)
    before = hashlib.sha256(disk.read_bytes()).digest()
    code, log = boot(disk, f'checkonly-smp{cpus}', cpus, 'fsck.repair=no')
    check(code in (0, 33) and 'NOT repaired' in log and 'mounted read-only until filesystem repair' in log and 'READONLY PASS' in log and not bad(log),
          f'checkonly smp{cpus}: problems reported, read-only mount (EROFS)', log[-4000:])
    check(hashlib.sha256(disk.read_bytes()).digest() == before, f'checkonly smp{cpus}: image unchanged')
    disk.unlink(missing_ok=True)


def lost_only(disk, count):
    fs = fatput.Fat32(str(disk))
    try: fs.alloc(count); fs.flush_fat()
    finally: fs.close()


def case_force(cpus):
    disk = fresh(f'force-smp{cpus}'); stage(disk, 'verify'); lost_only(disk, 33)
    code, log = boot(disk, f'force-smp{cpus}a', cpus)
    status, r = fatfsck.check(disk)
    check(code in (0, 33) and '[FAT32] check:' not in log and r.counts['lost_clusters'] == 33, f'force smp{cpus}: a clean-flagged volume is not checked by default', log[-3000:])
    code, log = boot(disk, f'force-smp{cpus}b', cpus, 'fsck.mode=force')
    status, r = fatfsck.check(disk)
    check(code in (0, 33) and 'check forced' in log and kernel_lost(log) == 33 and status == 0, f'force smp{cpus}: fsck.mode=force frees the 33 lost clusters', (log[-3000:], r.problems))
    disk.unlink(missing_ok=True)


def case_orphans(cpus):
    disk = fresh(f'orphans-smp{cpus}'); stage(disk, 'orphans:reboot')
    _, base = fatfsck.check(disk)
    code, log = boot(disk, f'orphans-smp{cpus}', cpus)
    status, r = fatfsck.check(disk)
    check(code in (0, 33) and 'POWER OFF with the orphans held' in log and 'volume cleanly unmounted' in log and not bad(log),
          f'orphans smp{cpus}: power-off with 8 open-unlinked files', log[-3000:])
    fs = fatput.Fat32(str(disk), readonly=True)
    try: dir_clusters = len(fs.chain(fs.lookup_dir(['home', 'nexus', 'fc'])))
    except SystemExit: dir_clusters = -1
    finally: fs.f.close()
    check(status == 0 and r.counts['lost_clusters'] == 0 and r.counts['free'] == base.counts['free'] - dir_clusters,
          f'orphans smp{cpus}: no lost clusters, clean flag set, all orphan clusters free again (free {r.counts["free"]}, '
          f'before {base.counts["free"]}, new directory {dir_clusters})', r.problems)
    disk.unlink(missing_ok=True)


def case_crash(cpus):
    disk = fresh(f'crash-smp{cpus}'); stage(disk, 'orphans:hold')
    held, log = boot_and_kill(disk, f'crash-smp{cpus}a', cpus, 'FATCHECK HOLDING', 1.0)
    status, r = fatfsck.check(disk)
    sizes = [1000, 4096, 65536, 300000, 1 << 20, 2 << 20, 7, 123456]
    fs = fatput.Fat32(str(disk), readonly=True); cs = fs.cl_bytes; fs.f.close()
    want = sum(-(-s // cs) for s in sizes)
    check(held and status == 4 and r.counts['dirty'] and r.counts['lost_clusters'] == want,
          f'crash smp{cpus}: power cut while holding orphans -> dirty, {r.counts["lost_clusters"]} lost clusters (expected {want})', (log[-3000:], r.problems))
    stage(disk, 'verify')   # the host edit itself is no repair: fatput leaves the dirty flag and the lost chains
    code, log = boot(disk, f'crash-smp{cpus}b', cpus)
    status, r2 = fatfsck.check(disk)
    check(code in (0, 33) and kernel_lost(log) == want and status == 0, f'crash smp{cpus}: next boot freed {kernel_lost(log)} lost clusters, volume clean', (log[-3000:], r2.problems))
    disk.unlink(missing_ok=True)


def case_powerloss(cpus):
    disk = fresh(f'power-smp{cpus}')
    for k in range(args.rounds):
        stage(disk, f'cycle:{k + 1}')
        delay = rnd.uniform(1.0, 9.0)
        running, log = boot_and_kill(disk, f'power-smp{cpus}-{k}', cpus, 'FATCHECK WORKLOAD RUNNING', delay)
        steps = [l for l in log.splitlines() if 'WORKLOAD step' in l]
        verified = k == 0 or 'VERIFY PASS' in log
        if not check(running and verified and not bad(log), f'powerloss smp{cpus} round {k + 1}: verified, workload cut after {delay:.1f}s '
                     f'({steps[-1].split("step ")[1].split()[0] if steps else "0"} steps)', log[-4000:]): break
        status, r = fatfsck.check(disk)
        print(f'     host after the cut: dirty={r.counts["dirty"]} lost={r.counts["lost_clusters"]} problems={len(r.problems)}', flush=True)
    stage(disk, 'verify')
    code, log = boot(disk, f'power-smp{cpus}-final', cpus)
    status, r = fatfsck.check(disk)
    check(code in (0, 33) and 'VERIFY PASS' in log and status == 0 and not bad(log), f'powerloss smp{cpus}: final verify, volume clean', (log[-3000:], r.problems))
    disk.unlink(missing_ok=True)


def tools_base():
    """The Debian tools root of tests/test_x64_fstools.py (ext4, storage-lock.json packages incl. dosfstools), cached in WORK."""
    tools = load('fc_storage_tools', ROOT / 'arch/x86_64/storage_tools.py')
    base, stamp_file = WORK / 'tools-base.img', WORK / 'tools-base.stamp'
    stamp = hashlib.sha256((ROOT / 'arch/x86_64/storage-lock.json').read_bytes() + (OUT / 'apps64.img').read_bytes()[:1 << 20]).hexdigest()[:16]
    if base.exists() and stamp_file.exists() and stamp_file.read_text() == stamp: return base
    dense = WORK / 'tools-base.dense'; dense.unlink(missing_ok=True); base.unlink(missing_ok=True)
    tools.image(dense, tools.CACHE, 640, [])
    sparse_copy(dense, base); unlink_retry(dense); stamp_file.write_text(stamp)
    return base


def unlink_retry(path):   # an ext4lib writer may hold the handle until it is collected
    import gc
    for _ in range(40):
        try: Path(path).unlink(missing_ok=True); return
        except PermissionError: gc.collect(); time.sleep(0.5)
    Path(path).unlink(missing_ok=True)


def sparse_copy(source, destination):
    destination = Path(destination); destination.unlink(missing_ok=True); destination.touch()
    subprocess.run(['fsutil', 'sparse', 'setflag', str(destination)], check=True, stdout=subprocess.DEVNULL)
    with open(source, 'rb') as a, destination.open('r+b') as b:
        b.truncate(Path(source).stat().st_size)
        while block := a.read(1 << 20):
            if block.strip(bytes(1)): b.write(block)
            else: b.seek(len(block), 1)


def case_dosfstools(cpus):
    """Debian's fsck.fat on the same damage: what it reports, its repair (checked by userland/fatfsck.py), and our two
    repairs (host tool, kernel boot check), which it must find clean."""
    ext4lib = load('fc_ext4lib', ROOT / 'userland/ext4lib.py')
    d1, d2, d3 = fresh(f'dosfs-damaged-smp{cpus}'), fresh(f'dosfs-host-smp{cpus}'), fresh(f'dosfs-kernel-smp{cpus}')
    stage(d3, 'verify')
    for d in (d1, d2, d3): expect = fatcorrupt.plant(d)
    status, r = fatfsck.check(d2, repair=True)
    code, log = boot(d3, f'dosfs-kernel-smp{cpus}', cpus)
    check(status == 1 and code in (0, 33) and 'repaired in' in log, f'dosfstools smp{cpus}: host and kernel repairs made', log[-2000:])
    root = WORK / f'dosfs-root-smp{cpus}.img'; sparse_copy(tools_base(), root)
    elf = WORK / 'fstools.elf'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie',
                    '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64/fstools_user.c'), '-o', str(elf)], check=True)
    fs = ext4lib.Writer(str(root)); data = elf.read_bytes()
    try:
        fs.put_bytes(data, 'sbin/init', 0o755, 0, 0); fs.put_bytes(data, 'bin/fstools-root', 0o4755, 0, 0)
        fs.put_bytes((ROOT / 'tests/storage_scripts/fs_fatcheck.sh').read_bytes(), 'nexus/fs-fatcheck.sh', 0o644, 0, 0)
        fs.put_bytes(b'script /nexus/fs-fatcheck.sh\n', 'nexus/fstools-mode', 0o644, 0, 0)
    finally: fs.close()
    serial = WORK / f'dosfs-smp{cpus}.log'; serial.unlink(missing_ok=True)
    accel = ['-accel', 'tcg', '-cpu', 'max'] if args.tcg else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', WHPX_CPU]
    disks = []
    for i, d in enumerate((root, d1, d2, d3)):
        disks += ['-drive', f'if=none,id=d{i},format=raw,file={d}', '-device', f'nvme,serial=fat{i},drive=d{i}']
    command = [str(QEMU), '-machine', 'q35', *accel, '-smp', str(cpus), '-m', '1024', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a', *disks,
               '-serial', f'file:{serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot']
    try: subprocess.run(command, capture_output=True, timeout=900)
    except subprocess.TimeoutExpired: pass
    log = serial.read_text(errors='replace') if serial.exists() else ''
    def status_of(tag):
        for line in log.splitlines():
            if f'[64] FATFSCK {tag} status=' in line: return int(line.split('status=')[1])
        return None
    def lines(tag): return [l.split(f'[64] FATFSCK {tag}: ', 1)[1] for l in log.splitlines() if f'[64] FATFSCK {tag}: ' in l]
    text = '\n'.join(lines('damaged-n'))
    print('     fsck.fat -n on the damage:\n       ' + '\n       '.join(lines('damaged-n')[:40]), flush=True)
    check('FSTEST ALL PASSED' in log and status_of('damaged-n') == 1, f'dosfstools smp{cpus}: fsck.fat -n finds the damage', log[-3000:])
    check(status_of('damaged-again') == 0, f'dosfstools smp{cpus}: fsck.fat -a repaired it (its own -n clean)', '\n'.join(lines('damaged-a') + lines('damaged-again')))
    check(status_of('host-repaired') == 0, f'dosfstools smp{cpus}: fsck.fat finds the userland/fatfsck.py repair clean', '\n'.join(lines('host-repaired')))
    check(status_of('kernel-repaired') == 0, f'dosfstools smp{cpus}: fsck.fat finds the kernel boot-check repair clean', '\n'.join(lines('kernel-repaired')))
    status, r = fatfsck.check(d1)
    # fsck.fat -a leaves long-name problems alone ('Not auto-correcting this'): the orphaned slot and the staged entry
    # (which it keeps as a plain T0000042.TMP file, whose 2 clusters our policy then counts as unreachable)
    leftover = {k: v for k, v in r.counts.items() if v and k not in ('files', 'dirs', 'referenced', 'allocated', 'free', 'orphan_lfn', 'pending_entries', 'lost_clusters', 'lost_chains', 'hidden_entries')}
    check(not leftover and r.counts['lost_clusters'] == 2 * r.counts['pending_entries'],
          f'dosfstools smp{cpus}: userland/fatfsck.py finds the fsck.fat repair consistent (only the long-name items fsck.fat -a leaves)', (leftover, r.problems))
    for d in (root, d1, d2, d3): unlink_retry(d)


CASES = {'repair': case_repair, 'checkonly': case_checkonly, 'force': case_force, 'orphans': case_orphans, 'crash': case_crash, 'powerloss': case_powerloss,
         'dosfstools': case_dosfstools}
for cpus in [int(x) for x in args.smp.split(',')]:
    for name in args.cases.split(','):
        CASES[name](cpus)
failed = [w for ok, w in results if not ok]
print(f'SUMMARY fatcheck: {len(results) - len(failed)}/{len(results)} passed')
sys.exit(1 if failed else 0)
