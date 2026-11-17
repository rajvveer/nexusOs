"""Software RAID end to end: Debian's mdadm on the NexusOS md driver (arch/x86_64/md.c), and NexusOS booting from a
RAID1 root.

  tools     in a NexusOS guest with Debian's mdadm: --create RAID1 (initial resync), data on both members, --fail
            (degraded, reads go on), writes while degraded, --remove, --add (recovery rebuilds the member with them),
            --stop/--assemble, --examine (clean superblocks), --detail; RAID0 (64k chunks alternate over the members,
            vfat, stop/assemble); /proc/mdstat throughout. Then the boot disks: md0 RAID1 with the root filesystem, md1
            RAID0 with a vfat file, and nine one-member RAID1 arrays on a third disk, eight of them made hostile on the
            host (checksum, max_dev, data beyond the device, data over the superblock, level 5, 99 disks, size beyond
            the member, a device number outside the roles) and one good.
  root      root=/dev/md0 (smp1) and root=UUID=<ext4 uuid> (smp2): arch/x86_64/md_user.c checks /proc/mdstat, sysfs,
            the RAID0 file; the hostile arrays refused; after the power-off both members clean, equal events, same data.
  degraded  one member missing: the array runs degraded ([U_]), the root is written.
  readd     both members again: the stale one is kicked (its events are older), ADD_NEW_DISK + recovery rebuild it,
            afterwards both members hold the same data.
  unclean   writes to md0 and a power cut while the array is dirty: the next boot resynchronises (resync started/done),
            both members equal again.
  verify    Debian's mdadm and e2fsck on the disks NexusOS used: the array assembles clean, the filesystem is clean, the
            files NexusOS wrote are there, the RAID0 vfat is clean.
Usage: test_x64_md.py WORK [--cases tools,root,degraded,readd,unclean,verify] [--smp 1,2] [--qmp-port 4736] [--keep]
(without tools the disks of an earlier run in WORK are reused; they are kept after a failure)"""
from pathlib import Path
import argparse, hashlib, shutil, struct, subprocess, sys, time, uuid

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402
import qmpkeys                  # noqa: E402
QEMU, CPU, ZIG = dtv.QEMU, dtv.CPU, dtv.ZIG
RAID_TYPE = uuid.UUID('a19d880f-05fc-4d3b-a006-743f0f84911e')

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='tools,root,degraded,readd,unclean,verify')
ap.add_argument('--smp', default='1,2')
ap.add_argument('--qmp-port', type=int, default=4736)
ap.add_argument('--keep', action='store_true', help='keep the disks (also kept after a failure: rerun without tools to reuse them)')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
smps = [int(x) for x in args.smp.split(',')]
results = []
def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True); return ok
def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-3000:]
HEAD = '#!/bin/dash\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin\nfail() { echo "[64] FSTEST FAIL $*"; cat /proc/mdstat; exit 1; }\nok() { echo "[64] FSTEST PASS $*"; }\n' \
       'mkdir -p /run/mdadm /etc/mdadm /mnt\nwait_sync() { i=0; while grep -q "resync =\\|recovery =" /proc/mdstat; do i=$((i+1)); [ $i -gt 600 ] && fail "sync did not finish"; sleep 0.2; done; }\n'


def build(source):
    out = WORK / (Path(source).stem + '.elf')
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-musl', '-static', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64' / source), '-o', str(out)], check=True)
    return out.read_bytes()


def ext4_root(name, files):
    ext4lib = dtv.load('md_ext4lib_' + name, ROOT / 'userland/ext4lib.py')
    staged = WORK / f'{name}-staged.img'; shutil.copyfile(OUT / 'apps64-ext4.img', staged)
    fs = ext4lib.Writer(str(staged))
    try:
        for path, (data, mode) in files.items(): fs.put_bytes(data, path, mode, 0, 0)
    finally: fs.close()
    raw = WORK / f'root-{name}.raw'
    with open(staged, 'rb') as a, open(raw, 'wb') as b: a.seek(32768 * 512); b.write(a.read(98304 * 512))
    dtv.unlink(staged); dtv.punch(raw)
    return raw


def sb_csum(sb):
    max_dev = struct.unpack_from('<I', sb, 220)[0]; size = 256 + max_dev * 2; total = 0
    for i in range(0, size - size % 4, 4):
        if i != 216: total += struct.unpack_from('<I', sb, i)[0]
    if size % 4 == 2: total += struct.unpack_from('<H', sb, size - 2)[0]
    return ((total & 0xFFFFFFFF) + (total >> 32)) & 0xFFFFFFFF


def superblock(image, base=0):
    with open(image, 'rb') as f: f.seek(base + 8 * 512); sb = f.read(1024)
    assert struct.unpack_from('<I', sb, 0)[0] == 0xa92b4efc, f'no md superblock on {image}'
    data_offset, data_size = struct.unpack_from('<QQ', sb, 128); events, resync = struct.unpack_from('<QQ', sb, 200)
    return {'data_offset': data_offset, 'data_size': data_size, 'events': events, 'resync': resync, 'csum_ok': sb_csum(sb) == struct.unpack_from('<I', sb, 216)[0]}


def mutate(image, base, change, fix=True):
    with open(image, 'r+b') as f:
        f.seek(base + 8 * 512); sb = bytearray(f.read(1024)); change(sb)
        if fix: struct.pack_into('<I', sb, 216, sb_csum(sb))
        f.seek(base + 8 * 512); f.write(sb)


def data_hash(image):
    s = superblock(image); h = hashlib.sha256()
    with open(image, 'rb') as f:
        f.seek(s['data_offset'] * 512); left = s['data_size'] * 512
        while left: chunk = f.read(min(left, 4 << 20)); h.update(chunk); left -= len(chunk)
    return h.hexdigest()


def members_state(a, b):
    sa, sb = superblock(a), superblock(b)
    return sa['resync'] == sb['resync'] == (1 << 64) - 1 and sa['events'] == sb['events'] and sa['csum_ok'] and sb['csum_ok'] and data_hash(a) == data_hash(b), (sa, sb)


class Boot:
    def __init__(self, tag, disks, cmdline, cpus):
        self.serial = WORK / f'{tag}.log'; self.serial.unlink(missing_ok=True); self.log = ''
        command = [QEMU, '-machine', 'q35', '-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU, '-smp', str(cpus), '-m', '2048', '-display', 'none',
                   '-vga', 'none', '-device', 'virtio-vga', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a', '-device', 'ahci,id=ahci']
        for i, d in enumerate(disks):
            if d: command += ['-drive', f'if=none,id=d{i},format=raw,file={d}', '-device', f'ide-hd,drive=d{i},bus=ahci.{i}']
        command += ['-serial', f'file:{self.serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot',
                    '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
        self.vm = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def read(self):
        self.log = self.serial.read_text(encoding='utf-8', errors='replace') if self.serial.exists() else ''; return self.log
    def wait(self, text, seconds=300):
        end = time.time() + seconds
        while time.time() < end:
            if text in self.read(): return True
            if self.vm.poll() is not None: return text in self.read()
            time.sleep(.2)
        return False
    def exit(self, seconds=120):
        try: self.vm.wait(seconds)
        except subprocess.TimeoutExpired: pass
        self.kill()
    def kill(self):
        if self.vm.poll() is None: self.vm.kill(); self.vm.wait()
        self.read()
    def faults(self): return [w for w in ('KERNEL_FAULT', '[64] FAIL:', 'USER_FAULT', 'PANIC', 'MD TEST FAIL') if w in self.log]


disk_a, disk_b, disk_c, disk_d, disk_h = (WORK / f'md-{n}.img' for n in 'abcdh')
HOSTILE = [   # (what, mutation, fix checksum, kernel message)
    ('checksum', lambda sb: sb.__setitem__(40, sb[40] ^ 0x55), False, 'bad superblock on sde1'),
    ('max_dev 100000', lambda sb: struct.pack_into('<I', sb, 220, 100000), False, 'bad superblock on sde2'),
    ('data beyond the device', lambda sb: struct.pack_into('<Q', sb, 128, 1 << 40), True, 'bad superblock on sde3'),
    ('data over the superblock', lambda sb: struct.pack_into('<Q', sb, 128, 4), True, 'bad superblock on sde4'),
    ('level 5', lambda sb: struct.pack_into('<I', sb, 72, 5), True, 'unsupported RAID level'),
    ('99 raid disks', lambda sb: struct.pack_into('<I', sb, 92, 99), True, 'unsupported RAID level'),
    ('size beyond the member', lambda sb: struct.pack_into('<Q', sb, 80, 1 << 40), True, 'a member is smaller than the array'),
    ('device number outside the roles', lambda sb: struct.pack_into('<I', sb, 160, 5000), True, 'no member is usable'),
]
prepared = 'tools' not in cases and all(d.exists() for d in (disk_a, disk_b, disk_c, disk_d, disk_h))   # reuse the disks of an earlier tools run
if 'tools' in cases:
    init = build('md_user.c')
    root = ext4_root('md', {'sbin/init': (init, 0o755), 'nexus/init-as-root': (b'md\n', 0o644)})
    for d, mib in ((disk_a, 96), (disk_b, 96), (disk_c, 40), (disk_d, 40)): dtv.raw_disk(d, mib)
    h_parts = dtv.gpt_disk(disk_h, [(8, RAID_TYPE, uuid.uuid4(), f'h{i}') for i in range(len(HOSTILE) + 1)])
    script = HEAD + r'''
yes | mdadm --create /dev/md0 --level=1 --raid-devices=2 --run /dev/sda /dev/sdb >/tmp/x 2>&1 || fail "create RAID1: $(cat /tmp/x)"
grep -q "md0 : active raid1 sdb\[1\] sda\[0\]" /proc/mdstat || fail "mdstat after create"
wait_sync; grep -q "\[2/2\] \[UU\]" /proc/mdstat || fail "initial resync"
mdadm --detail /dev/md0 | grep -Eq "State : (clean|active) *$" || fail "detail: $(mdadm --detail /dev/md0)"
ok "mdadm --create RAID1, initial resync, /proc/mdstat, --detail"
OFF=$(mdadm --examine /dev/sda | sed -n 's/.*Data Offset : \([0-9]*\) sectors.*/\1/p'); [ -n "$OFF" ] || fail "data offset"
dd if=/dev/urandom of=/tmp/a bs=1M count=8 status=none
dd if=/tmp/a of=/dev/md0 bs=1M conv=fsync status=none && cmp -n 8388608 /tmp/a /dev/md0 || fail "RAID1 data"
cmp -n 8388608 -i $((OFF * 512)):0 /dev/sda /tmp/a && cmp -n 8388608 -i $((OFF * 512)):0 /dev/sdb /tmp/a || fail "mirror on both members"
ok "RAID1 writes reach both members at the data offset ($OFF sectors)"
mdadm /dev/md0 --fail /dev/sdb >/tmp/x 2>&1 || fail "--fail: $(cat /tmp/x)"
grep -q "sdb\[1\](F)" /proc/mdstat && grep -q "\[2/1\] \[U_\]" /proc/mdstat || fail "mdstat after --fail"
mdadm --detail /dev/md0 | grep -q "State : .*degraded" || fail "detail degraded: $(mdadm --detail /dev/md0)"
cmp -n 8388608 /tmp/a /dev/md0 || fail "degraded read"
dd if=/dev/urandom of=/tmp/b bs=1M count=4 status=none
dd if=/tmp/b of=/dev/md0 bs=1M seek=2 conv=fsync,notrunc status=none || fail "degraded write"
ok "--fail: degraded [U_], reads and writes go on"
mdadm /dev/md0 --remove /dev/sdb >/tmp/x 2>&1 || fail "--remove: $(cat /tmp/x)"
grep -q "sdb\[" /proc/mdstat && fail "removed member listed"
mdadm /dev/md0 --add /dev/sdb >/tmp/x 2>&1 || fail "--add: $(cat /tmp/x)"
wait_sync; grep -q "\[2/2\] \[UU\]" /proc/mdstat || fail "recovery"
cmp -n 4194304 -i $((OFF * 512 + 2097152)):0 /dev/sdb /tmp/b || fail "the rebuilt member lacks what was written while degraded"
ok "--remove, --add: recovery rebuilt the member with the writes made while degraded"
mdadm --stop /dev/md0 >/tmp/x 2>&1 || fail "--stop: $(cat /tmp/x)"
grep -q "^md0" /proc/mdstat && fail "md0 listed after --stop"
mdadm --examine /dev/sda | grep -q "State : clean" && mdadm --examine /dev/sdb | grep -q "State : clean" || fail "superblocks not clean after --stop"
mdadm --examine --scan | grep -q "ARRAY /dev/md/0 .*UUID=" || fail "examine --scan: $(mdadm --examine --scan)"
mdadm --assemble /dev/md0 /dev/sda /dev/sdb >/tmp/x 2>&1 || fail "--assemble: $(cat /tmp/x)"
grep -q "\[2/2\] \[UU\]" /proc/mdstat && cmp -n 2097152 /tmp/a /dev/md0 && cmp -n 4194304 -i 0:2097152 /tmp/b /dev/md0 || fail "data after --assemble"
ok "--stop (clean superblocks), --examine --scan, --assemble"
yes | mdadm --create /dev/md1 --level=0 --raid-devices=2 --chunk=64 --run /dev/sdc /dev/sdd >/tmp/x 2>&1 || fail "create RAID0: $(cat /tmp/x)"
grep -q "md1 : active raid0" /proc/mdstat && grep -q "64k chunks" /proc/mdstat || fail "RAID0 mdstat"
O1=$(mdadm --examine /dev/sdc | sed -n 's/.*Data Offset : \([0-9]*\) sectors.*/\1/p')
dd if=/dev/urandom of=/tmp/c bs=64k count=4 status=none
dd if=/tmp/c of=/dev/md1 bs=64k conv=fsync status=none || fail "RAID0 write"
cmp -n 65536 -i 0:$((O1 * 512)) /tmp/c /dev/sdc && cmp -n 65536 -i 65536:$((O1 * 512)) /tmp/c /dev/sdd && cmp -n 65536 -i 131072:$((O1 * 512 + 65536)) /tmp/c /dev/sdc || fail "RAID0 chunk layout"
mkfs.vfat -n RAID0 /dev/md1 >/dev/null || fail "mkfs.vfat md1"
mkdir -p /mnt/r0 && mount -t vfat /dev/md1 /mnt/r0 && printf 'raid0 data\n' > /mnt/r0/hello.txt && dd if=/dev/urandom of=/mnt/r0/r.bin bs=1M count=8 status=none && cp /mnt/r0/r.bin /tmp/r && umount /mnt/r0 || fail "vfat on md1"
mdadm --stop /dev/md1 >/dev/null && mdadm --assemble /dev/md1 /dev/sdc /dev/sdd >/dev/null || fail "RAID0 stop/assemble"
mount -t vfat /dev/md1 /mnt/r0 && cmp /tmp/r /mnt/r0/r.bin && umount /mnt/r0 || fail "RAID0 data after assemble"
fsck.vfat -n /dev/md1 >/tmp/x 2>&1 || fail "fsck.vfat md1: $(cat /tmp/x)"
mdadm --detail /dev/md1 | grep -q "Chunk Size : 64K" || fail "detail md1"
mdadm --stop /dev/md1 >/dev/null || fail "stop md1"
ok "RAID0: 64k chunks alternate over the members, vfat, stop/assemble, fsck clean"
dd if=/dev/sde of=/dev/md0 bs=1M conv=fsync status=none || fail "root copy"
mdadm --stop /dev/md0 >/dev/null || fail "stop md0"
for i in 1 2 3 4 5 6 7 8 9; do
  yes | mdadm --create /dev/md5 --level=1 --raid-devices=2 --run /dev/sdf$i missing >/tmp/x 2>&1 || fail "hostile base $i: $(cat /tmp/x)"
  mdadm --stop /dev/md5 >/dev/null || fail "stop h$i"
done
ok "boot disks: md0 RAID1 with the root, md1 RAID0 with a vfat file, nine one-member RAID1 sets (md5 made on first use)"
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [disk_a, disk_b, disk_c, disk_d, root, disk_h], {}, timeout=1500, tag='md-tools')
    for line in log.splitlines():
        if 'FSTEST' in line or '[64] MD ' in line: print('    ' + line.strip()[:200])
    check(ok and 'KERNEL_FAULT' not in log, 'tools: Debian mdadm -- RAID1 create/resync/fail/remove/add/recovery/stop/assemble/examine, RAID0, boot disks',
          markers(log, 'FSTEST', '[64] MD ', 'FAULT'))
    dtv.unlink(root)
    if ok:
        for i, (what, change, fix, _) in enumerate(HOSTILE): mutate(disk_h, h_parts[i] * 512, change, fix)
        prepared = True

if prepared:
    with open(disk_a, 'rb') as f:   # the root filesystem's UUID (ext4 superblock in md0's data area) for root=UUID=
        f.seek(superblock(disk_a)['data_offset'] * 512 + 1024 + 104); fs_uuid = str(uuid.UUID(bytes=f.read(16)))
    if 'root' in cases:
        for cpus in smps:
            spec = 'root=/dev/md0' if cpus == 1 else f'root=UUID={fs_uuid}'
            b = Boot(f'root-smp{cpus}', [disk_a, disk_b, disk_c, disk_d, disk_h], f'{spec} mdtest=root', cpus)
            good = b.wait('[64] MD TEST ALL PASSED', 300); b.exit()
            missing = [m for _, _, _, m in HOSTILE if m not in b.log] + [m for m in ('md0 holds the root',) if m not in b.log]
            clean, detail = members_state(disk_a, disk_b)
            check(good and not missing and not b.faults() and clean, f'root smp{cpus}: {spec}: RAID1 root, RAID0 file, hostile superblocks refused, clean equal members after power-off',
                  (missing, b.faults(), detail, markers(b.log, '[64] MD', 'MD TEST')))
    if 'degraded' in cases or 'readd' in cases:
        b = Boot('degraded', [disk_a, None, disk_c, disk_d], 'root=/dev/md0 mdtest=degraded', smps[-1])
        good = b.wait('[64] MD TEST ALL PASSED', 300); b.exit()
        check(good and 'md0 active raid1 1/2 members' in b.log and '(degraded)' in b.log and not b.faults(), f'degraded smp{smps[-1]}: one member missing, [U_], root written',
              (b.faults(), markers(b.log, '[64] MD', 'MD TEST')))
        if 'readd' in cases:
            b = Boot('readd', [disk_a, disk_b, disk_c, disk_d], 'root=/dev/md0 mdtest=readd', smps[0])
            good = b.wait('[64] MD TEST ALL PASSED', 300); b.exit()
            clean, detail = members_state(disk_a, disk_b)
            check(good and 'sdb kicked' in b.log and 'recovery done' in b.log and clean and not b.faults(),
                  f'readd smp{smps[0]}: stale member kicked, ADD_NEW_DISK + recovery, both members equal and clean', (b.faults(), detail, markers(b.log, '[64] MD', 'MD TEST')))
    if 'unclean' in cases:
        b = Boot('unclean', [disk_a, disk_b], 'root=/dev/md0 mdtest=unclean', smps[-1])
        good = b.wait('[64] MD TEST WRITING', 300); time.sleep(0.5); b.kill()
        dirty = superblock(disk_a)['resync'] == 0 and superblock(disk_b)['resync'] == 0
        check(good and dirty and not b.faults(), 'unclean: power cut while writing, superblocks dirty (resync_offset 0)', (b.faults(), superblock(disk_a), markers(b.log, '[64] MD', 'MD TEST')))
        if good and dirty:
            b = Boot('resync', [disk_a, disk_b], 'root=/dev/md0 mdtest=resync', smps[0])
            good = b.wait('[64] MD TEST ALL PASSED', 300); b.exit()
            clean, detail = members_state(disk_a, disk_b)
            check(good and 'md0 resync started' in b.log and 'md0 resync done' in b.log and clean and not b.faults(),
                  f'resync smp{smps[0]}: the dirty array resynchronised at boot, members equal and clean', (b.faults(), detail, markers(b.log, '[64] MD', 'MD TEST')))
    if 'verify' in cases and all(results):
        script = HEAD + r'''
grep -q "^md0 : active raid1" /proc/mdstat || mdadm --assemble /dev/md0 /dev/sda /dev/sdb >/tmp/x 2>&1 || fail "assemble after NexusOS: $(cat /tmp/x)"   # the kernel assembles them at boot
grep -q "\[2/2\] \[UU\]" /proc/mdstat && ! grep -q "resync =" /proc/mdstat || fail "md0 not in sync"
mdadm --detail /dev/md0 | grep -Eq "State : clean *$" || fail "detail: $(mdadm --detail /dev/md0)"
mdadm --detail /dev/md0 | grep -q "Events : " && mdadm --examine /dev/sda | grep -q "Checksum : .* correct" || fail "examine after NexusOS"
e2fsck -fn /dev/md0 >/tmp/x 2>&1 || fail "e2fsck md0: $(tail -5 /tmp/x)"
debugfs -R "cat /md-proof.txt" /dev/md0 2>/dev/null | grep -q "written on a RAID1 root" || fail "md-proof.txt"
debugfs -R "cat /degraded.txt" /dev/md0 2>/dev/null | grep -q "written while degraded" || fail "degraded.txt"
{ grep -q "^md1 : active raid0" /proc/mdstat || mdadm --assemble /dev/md1 /dev/sdc /dev/sdd >/dev/null 2>&1; } && fsck.vfat -n /dev/md1 >/tmp/x 2>&1 || fail "md1: $(cat /tmp/x)"
mdadm --stop /dev/md1 >/dev/null; mdadm --stop /dev/md0 >/dev/null
ok "Debian mdadm + e2fsck after NexusOS: md0 clean, the filesystem clean, NexusOS's files there, md1 vfat clean"
sync; echo "[64] FSTEST ALL PASSED"
'''
        ok, log = dtv.run_script(WORK, script, [disk_a, disk_b, disk_c, disk_d], {}, timeout=900, tag='md-verify')
        check(ok, 'verify: Debian mdadm and e2fsck read what NexusOS used and wrote', markers(log, 'FSTEST', 'rror'))
if not args.keep and all(results):
    for d in (disk_a, disk_b, disk_c, disk_d, disk_h): dtv.unlink(d)
print(f'SUMMARY md: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
