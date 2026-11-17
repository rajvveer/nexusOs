"""LVM2 end to end: Debian's lvm2 on the NexusOS device mapper (linear, striped, snapshot-origin, snapshot), and NexusOS
booting from logical volumes (arch/x86_64/lvm_boot.c), plain and on LUKS.

  tools     in a NexusOS guest with Debian's lvm2: pvcreate/vgcreate on two disks, linear and striped LVs (data read
            back, dmsetup tables), lvextend, a snapshot (origin writes copied out, the snapshot keeps the old data,
            vgchange -an/-ay reloads it from its persistent store), COW exhaustion (Invalid, origin intact), a damaged
            COW header (the snapshot refuses to activate, the origin works), lvremove, lvs/vgs, /dev/<vg>/<lv> links.
  root      root LV striped over a GPT partition and a whole-disk PV, a vfat data LV mounted by /etc/fstab through
            /dev/vgroot/data, a snapshot of it taken before a later file; five hostile VGs beside them (text checksum,
            PV extents beyond the device, 99 stripes, deep nesting, bad label CRC) and a good one;
            arch/x86_64/lvm_user.c checks everything; root=/dev/vgroot/root (smp1) and root=/dev/mapper/vgroot-root.
  luks      LVM on LUKS2: cryptroot=PARTUUID=... root=/dev/mapper/vgc-root; the passphrase at the prompt, then the VG
            inside the unlocked volume.
  verify    Debian's lvm2 again on the disks NexusOS used: the VG is consistent, the snapshot still valid, the file
            NexusOS wrote is on the root LV (fuse2fs), fsck.vfat clean.
  many      more than 16 device-mapper devices (the old block-device table held 16 disks, dm and md devices in all):
            24 LVs and 3 snapshots (36 mappings) made by lvm2, each LV's own data, vgchange -an/-ay; then a reboot where
            the kernel activates them all at boot (lvm_boot_late) and the data is read back.
Usage: test_x64_lvm.py WORK [--cases tools,root,luks,verify,many] [--smp 1,2] [--qmp-port 4735]"""
from pathlib import Path
import argparse, os, re, shutil, struct, subprocess, sys, time, uuid

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402
import qmpkeys                  # noqa: E402
QEMU, CPU, ZIG = dtv.QEMU, dtv.CPU, dtv.ZIG
LVM_TYPE = uuid.UUID('e6d6d379-f507-44c2-a23c-238f2a3df928')
LUKS_TYPE = uuid.UUID('ca7d7ccb-63ed-4c53-861c-1742536059cc')
PASS = 'Correct horse battery 42'

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='tools,root,luks,verify,many')
ap.add_argument('--smp', default='1,2')
ap.add_argument('--qmp-port', type=int, default=4735)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
results = []
def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True); return ok
def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-3000:]
HEAD = '#!/bin/dash\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1\nfail() { echo "[64] FSTEST FAIL $*"; exit 1; }\nok() { echo "[64] FSTEST PASS $*"; }\n' + dtv.LVM_SETUP + '\n'


def lvm_crc(data, crc=0xf597a6cf):
    table = [0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
             0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c]
    for b in data:
        crc ^= b; crc = (crc >> 4) ^ table[crc & 0xf]; crc = (crc >> 4) ^ table[crc & 0xf]
    return crc


def edit_pv(image, start_lba, change=None, label=False):
    """Change one PV's metadata text (change(text) -> text, checksums fixed) or break its label CRC."""
    base = start_lba * 512
    with open(image, 'r+b') as f:
        f.seek(base); head = bytearray(f.read(2048))
        s = next(i for i in range(4) if head[i * 512:i * 512 + 8] == b'LABELONE')
        if label:
            f.seek(base + s * 512 + 100); byte = f.read(1); f.seek(base + s * 512 + 100); f.write(bytes([byte[0] ^ 0xFF])); return
        sector = head[s * 512:(s + 1) * 512]; at = struct.unpack_from('<I', sector, 20)[0] + 40
        while struct.unpack_from('<QQ', sector, at) != (0, 0): at += 16
        at += 16; mda_offset, mda_size = struct.unpack_from('<QQ', sector, at)
        f.seek(base + mda_offset); header = bytearray(f.read(512))
        offset, size, checksum, flags = struct.unpack_from('<QQII', header, 40)
        assert offset + size <= mda_size, 'wrapped metadata'
        f.seek(base + mda_offset + offset); text = f.read(size)
        new, fix = change(text)
        assert offset + len(new) <= mda_size
        f.seek(base + mda_offset + offset); f.write(new)
        struct.pack_into('<QQI', header, 40, offset, len(new), lvm_crc(new) if fix else checksum)
        struct.pack_into('<I', header, 0, lvm_crc(bytes(header[4:])))
        f.seek(base + mda_offset); f.write(header)


def build(source):
    out = WORK / (Path(source).stem + '.elf')
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-musl', '-static', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64' / source), '-o', str(out)], check=True)
    return out.read_bytes()


def ext4_root(name, files):
    ext4lib = dtv.load('lvm_ext4lib_' + name, ROOT / 'userland/ext4lib.py')
    staged = WORK / f'{name}-staged.img'; shutil.copyfile(OUT / 'apps64-ext4.img', staged)
    fs = ext4lib.Writer(str(staged))
    try:
        for path, (data, mode) in files.items(): fs.put_bytes(data, path, mode, 0, 0)
    finally: fs.close()
    raw = WORK / f'root-{name}.raw'
    with open(staged, 'rb') as a, open(raw, 'wb') as b: a.seek(32768 * 512); b.write(a.read(98304 * 512))
    dtv.unlink(staged); dtv.punch(raw)
    return raw


class Boot:
    def __init__(self, tag, disks, cmdline, cpus):
        self.serial = WORK / f'{tag}.log'; self.serial.unlink(missing_ok=True); self.log = ''
        command = [QEMU, '-machine', 'q35', '-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU, '-smp', str(cpus), '-m', '2048', '-display', 'none',
                   '-vga', 'none', '-device', 'virtio-vga', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a', '-device', 'ahci,id=ahci']
        for i, d in enumerate(disks): command += ['-drive', f'if=none,id=d{i},format=raw,file={d}', '-device', f'ide-hd,drive=d{i},bus=ahci.{i}']
        command += ['-serial', f'file:{self.serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot',
                    '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
        self.vm = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.q = qmpkeys.Qmp(args.qmp_port)
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
        if self.vm.poll() is None: self.vm.kill(); self.vm.wait()
        self.read()
    def faults(self): return [w for w in ('KERNEL_FAULT', '[64] FAIL:', 'USER_FAULT', 'PANIC') if w in self.log]


# ---------------- tools: Debian lvm2 on the NexusOS device mapper ----------------
if 'tools' in cases:
    pv0, pv1 = WORK / 'tools-pv0.img', WORK / 'tools-pv1.img'
    dtv.raw_disk(pv0, 96); dtv.raw_disk(pv1, 96)
    script = HEAD + r'''
pvcreate -y /dev/sda /dev/sdb >/dev/null || fail pvcreate
vgcreate vg0 /dev/sda /dev/sdb >/dev/null || fail vgcreate
lvcreate -y -L 16M -n lin vg0 >/dev/null || fail "lvcreate linear"
lvcreate -y -L 16M -i 2 -I 64k -n str vg0 >/dev/null || fail "lvcreate striped"
dmsetup table vg0-str | grep -q "striped 2 128 8:0 [0-9]* 8:16 [0-9]*" || fail "striped table: $(dmsetup table vg0-str)"
[ "$(readlink /dev/vg0/lin)" = "../dm-0" ] && [ -b /dev/vg0/str ] || fail "/dev/vg0 links: $(ls -l /dev/vg0)"
ok "pvcreate, vgcreate, linear and striped LVs, /dev/vg0 links"
dd if=/dev/urandom of=/tmp/a bs=1M count=16 status=none
dd if=/tmp/a of=/dev/vg0/str bs=1M conv=fsync status=none && cmp /tmp/a /dev/vg0/str || fail "striped data"
dd if=/tmp/a of=/dev/vg0/lin bs=1M conv=fsync status=none && cmp /tmp/a /dev/vg0/lin || fail "linear data"
ok "data through linear and striped LVs"
lvextend -y -L +8M vg0/str >/dev/null && [ "$(blockdev --getsz /dev/vg0/str)" = 49152 ] || fail "lvextend"
cmp -n 16777216 /tmp/a /dev/vg0/str || fail "data after lvextend"
ok "lvextend keeps the data"
lvcreate -y -s -L 8M -n snap vg0/lin >/dev/null || fail "lvcreate snapshot"
dmsetup table vg0-lin | grep -q "snapshot-origin 253:" && dmsetup table vg0-snap | grep -q "snapshot 253:[0-9]* 253:[0-9]* P 8" || fail "snapshot tables: $(dmsetup table)"
dd if=/dev/zero of=/dev/vg0/lin bs=1M count=2 conv=fsync status=none
cmp /tmp/a /dev/vg0/snap || fail "the snapshot changed with its origin"
cmp -n 2097152 /dev/zero /dev/vg0/lin || fail "origin write"
dd if=/dev/urandom of=/tmp/b bs=1M count=1 status=none
dd if=/tmp/b of=/dev/vg0/snap bs=1M seek=4 conv=fsync status=none
cmp -i 4194304:0 -n 1048576 /dev/vg0/snap /tmp/b && cmp -i 4194304 -n 1048576 /dev/vg0/lin /tmp/a || fail "snapshot write"
lvs --noheadings -o lv_name,lv_attr,origin,data_percent vg0/snap | grep -q "snap *swi-a-s--- *lin" || fail "lvs snapshot: $(lvs vg0)"
ok "snapshot: origin writes copied out, snapshot writes kept apart, lvs"
vgchange -an vg0 >/dev/null && vgchange -ay vg0 >/dev/null || fail "vgchange -an/-ay"
cmp -n 4194304 /tmp/a /dev/vg0/snap && cmp -i 4194304:0 -n 1048576 /dev/vg0/snap /tmp/b || fail "snapshot after reactivation"
ok "the persistent exception store survives deactivation"
dd if=/dev/urandom of=/dev/vg0/lin bs=1M count=16 conv=fsync status=none
dmsetup status vg0-snap | grep -q "snapshot Invalid" || fail "COW exhaustion: $(dmsetup status vg0-snap)"
lvs --noheadings -o lv_attr vg0/snap | grep -q "swi-I-s---" || fail "lvs invalid: $(lvs vg0)"
dd if=/dev/vg0/snap of=/dev/null bs=4096 count=1 status=none 2>/dev/null && fail "reading an invalid snapshot"
dd if=/dev/vg0/lin of=/dev/null bs=1M count=16 status=none || fail "origin after exhaustion"
ok "COW exhaustion invalidates the snapshot, the origin goes on"
lvremove -y vg0/snap >/dev/null && ! dmsetup info vg0-lin-real >/dev/null 2>&1 && dmsetup table vg0-lin | grep -q linear || fail "lvremove snapshot"
ok "lvremove of the snapshot leaves a plain linear origin"
lvcreate -y -s -L 8M -n snap2 vg0/lin >/dev/null || fail "second snapshot"
START=$(pvs --noheadings -o pe_start --units s /dev/sda | tr -dc 0-9)
EXT=$(lvs --noheadings -o seg_pe_ranges vg0/snap2 | sed 's/.*://; s/-.*//' | tr -dc 0-9)
vgchange -an vg0 >/dev/null || fail "vgchange -an"
printf 'JUNKJUNKJUNKJUNK' | dd of=/dev/sda bs=512 seek=$((START + EXT * 8192)) conv=notrunc,fsync status=none
vgchange -ay vg0 >/tmp/act 2>&1
dmsetup table vg0-snap2 2>/dev/null | grep -q snapshot && fail "a damaged COW was activated: $(dmsetup table)"
dmsetup remove vg0-snap2-cow 2>/dev/null   # lvm2 leaves the COW layer of a failed snapshot activation behind (as on Linux)
lvremove -y -ff vg0/snap2 >/dev/null 2>&1; vgchange -ay vg0 >/dev/null 2>&1
dmsetup table vg0-lin | grep -q linear && dd if=/dev/vg0/lin of=/dev/null bs=1M count=16 status=none || fail "origin after a damaged COW: $(cat /tmp/act)"
ok "a damaged COW header is refused (no snapshot table), the origin activates without it"
lvremove -y vg0/lin vg0/str >/dev/null || fail lvremove
vgs --noheadings -o vg_name,lv_count vg0 | grep -q "vg0 *0" || fail "vgs: $(vgs)"
vgremove -y vg0 >/tmp/x 2>&1 || fail "vgremove: $(cat /tmp/x)"
pvremove -y /dev/sda /dev/sdb >/tmp/x 2>&1 || fail "pvremove: $(grep -v async /tmp/x | head -2 | tr '\n' ' ') dm: $(dmsetup ls | tr '\n' ' ') holders: $(ls /sys/block/sda/holders | tr '\n' ' ')"
[ ! -e /dev/vg0 ] || fail "/dev/vg0 left behind"
ok "lvremove, vgremove, pvremove; /dev/vg0 gone"
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [pv0, pv1], {}, timeout=1500, tag='lvm-tools')
    for line in log.splitlines():
        if 'FSTEST' in line or 'DM ' in line: print('    ' + line.strip()[:200])
    check(ok and 'KERNEL_FAULT' not in log, 'tools: Debian lvm2 -- linear, striped, lvextend, snapshots (COW, reactivation, exhaustion, damaged COW), removal', markers(log, 'FSTEST', 'DM ', 'FAULT'))
    dtv.unlink(pv0); dtv.unlink(pv1)

# ---------------- many: more than 16 mappings ----------------
if 'many' in cases:
    m0, m1 = WORK / 'many-pv0.img', WORK / 'many-pv1.img'
    dtv.raw_disk(m0, 128); dtv.raw_disk(m1, 128)
    marks = r'''
marks() { i=1; while [ $i -le 24 ]; do [ "$(dd if=/dev/vgmany/lv$i bs=512 count=1 status=none | tr -d '\000')" = "lv$i" ] || fail "lv$i data $1"; i=$((i + 1)); done
          i=1; while [ $i -le 3 ]; do [ "$(dd if=/dev/vgmany/snap$i bs=512 count=1 status=none | tr -d '\000')" = "lv$i" ] || fail "snap$i data $1"; i=$((i + 1)); done; }
'''
    script = HEAD + marks + r'''
pvcreate -y /dev/sda /dev/sdb >/dev/null && vgcreate vgmany /dev/sda /dev/sdb >/dev/null || fail "vgmany"
i=1; while [ $i -le 24 ]; do lvcreate -y -L 4M -n lv$i vgmany >/dev/null || fail "lvcreate lv$i: $(dmsetup ls | wc -l) mappings"
  printf "lv$i" | dd of=/dev/vgmany/lv$i bs=512 conv=notrunc,fsync status=none || fail "write lv$i"; i=$((i + 1)); done
i=1; while [ $i -le 3 ]; do lvcreate -y -s -L 8M -n snap$i vgmany/lv$i >/dev/null || fail "snapshot $i"
  printf "new$i" | dd of=/dev/vgmany/lv$i bs=512 seek=8 conv=notrunc,fsync status=none; i=$((i + 1)); done
N=$(dmsetup ls | grep -c vgmany); [ "$N" -ge 33 ] || fail "only $N mappings"
marks created
vgchange -an vgmany >/dev/null && [ "$(dmsetup ls | grep -c vgmany)" = 0 ] && vgchange -ay vgmany >/dev/null || fail "vgchange -an/-ay"
[ "$(dmsetup ls | grep -c vgmany)" = "$N" ] || fail "after -ay: $(dmsetup ls | grep -c vgmany) of $N"
marks reactivated
[ "$(ls /dev/vgmany | wc -l)" = 27 ] || fail "/dev/vgmany: $(ls /dev/vgmany | wc -l)"
ok "lvm2 made $N mappings (24 LVs, 3 snapshots), each LV's data, vgchange -an/-ay"
vgchange -an vgmany >/dev/null
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [m0, m1], {}, timeout=1500, tag='lvm-many')
    check(ok and 'KERNEL_FAULT' not in log, 'many: lvm2 with more than 16 device-mapper devices (24 LVs + 3 snapshots)', markers(log, 'FSTEST', 'DM ', 'FAULT', 'BLOCK'))
    if ok:
        script = HEAD + marks + r'''
N=$(dmsetup ls | grep -c vgmany); [ "$N" -ge 33 ] || fail "the kernel activated only $N mappings at boot"
marks "after the boot activation"
lvs --noheadings vgmany | grep -c " -wi-a\| owi-a\| swi-a" | grep -q 27 || fail "lvs: $(lvs vgmany)"
ok "the kernel activated all $N mappings at boot (lvm_boot_late), lvm2 sees them active"
vgchange -an vgmany >/dev/null; vgremove -y -ff vgmany >/dev/null 2>&1
sync; echo "[64] FSTEST ALL PASSED"
'''
        ok, log = dtv.run_script(WORK, script, [m0, m1], {}, timeout=1500, tag='lvm-many-boot')
        check(ok and 'volume group vgmany activated (late)' in log, 'many: after a reboot the kernel activates all of them', markers(log, 'FSTEST', 'LVM', 'FAULT'))
    dtv.unlink(m0); dtv.unlink(m1)

# ---------------- prepared disks for the boots ----------------
boot_cases = [c for c in ('root', 'luks') if c in cases]
disk_a, disk_b, disk_c, disk_h = WORK / 'root-a.img', WORK / 'root-b.img', WORK / 'luks.img', WORK / 'hostile.img'
g = {k: uuid.uuid4() for k in ('a', 'c')}
if boot_cases:
    init = build('lvm_user.c')
    a_start = dtv.gpt_disk(disk_a, [(64, LVM_TYPE, g['a'], 'lvm-a')])
    dtv.raw_disk(disk_b, 64)
    dtv.gpt_disk(disk_c, [(96, LUKS_TYPE, g['c'], 'lvm-luks')])
    hostile_parts = dtv.gpt_disk(disk_h, [(12, LVM_TYPE, uuid.uuid4(), f'h{i}') for i in range(6)])
    plain = ext4_root('plain', {'sbin/init': (init, 0o755), 'nexus/init-as-root': (b'lvm\n', 0o644), 'nexus/lvm-vg': (b'vgroot\n', 0o644),
                                'nexus/lvm-hostile': (b'1\n', 0o644), 'etc/fstab': (b'/dev/vgroot/data /mnt/data vfat defaults 0 0\n', 0o644),
                                'mnt/data/.mountpoint': (b'', 0o644)})
    luks = ext4_root('luks', {'sbin/init': (init, 0o755), 'nexus/init-as-root': (b'lvm\n', 0o644), 'nexus/lvm-vg': (b'vgc\n', 0o644), 'nexus/lvm-luks': (b'1\n', 0o644)})
    script = HEAD + r'''
pvcreate -y /dev/sda1 /dev/sdb >/dev/null && vgcreate vgroot /dev/sda1 /dev/sdb >/dev/null || fail "vgroot"
lvcreate -y -i 2 -I 64k -L 56M -n root vgroot >/dev/null || fail "root LV"
dd if=/dev/sde of=/dev/vgroot/root bs=1M conv=fsync status=none || fail "root copy"
lvcreate -y -L 16M -n data vgroot >/dev/null && mkfs.vfat -n DATA /dev/vgroot/data >/dev/null || fail "data LV"
mkdir -p /mnt/d; mount -t vfat /dev/vgroot/data /mnt/d && printf 'before the snapshot\n' > /mnt/d/before.txt && umount /mnt/d || fail "data files"
lvcreate -y -s -L 8M -n datasnap vgroot/data >/dev/null || fail "snapshot"
mount -t vfat /dev/vgroot/data /mnt/d && printf 'after the snapshot\n' > /mnt/d/after.txt && umount /mnt/d || fail "later file"
vgchange -an vgroot >/dev/null || fail "vgchange -an vgroot"
ok "vgroot: striped root, data, snapshot"
printf '%s' "Correct horse battery 42" > /tmp/pass
cryptsetup luksFormat -q --type luks2 --pbkdf pbkdf2 --pbkdf-force-iterations 1000 --key-file /tmp/pass /dev/sdc1 || fail luksFormat
cryptsetup open --key-file /tmp/pass /dev/sdc1 c && pvcreate -y /dev/mapper/c >/dev/null && vgcreate vgc /dev/mapper/c >/dev/null || fail "vgc on LUKS"
lvcreate -y -L 56M -n root vgc >/dev/null && dd if=/dev/sdf of=/dev/vgc/root bs=1M conv=fsync status=none || fail "vgc root"
vgchange -an vgc >/dev/null && cryptsetup close c || fail "close vgc"
ok "vgc on LUKS2"
for i in 1 2 3 4 5; do pvcreate -y /dev/sdd$i >/dev/null && vgcreate vgbad$i /dev/sdd$i >/dev/null && lvcreate -y -L 4M -n lv vgbad$i >/dev/null || fail "vgbad$i"; done
pvcreate -y /dev/sdd6 >/dev/null && vgcreate vgok /dev/sdd6 >/dev/null && lvcreate -y -L 4M -n lv vgok >/dev/null || fail vgok
vgchange -an >/dev/null
ok "hostile VGs and a good one"
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [disk_a, disk_b, disk_c, disk_h, plain, luks], {}, timeout=1500, tag='lvm-prepare')
    for line in log.splitlines():
        if 'FSTEST' in line: print('    ' + line.strip()[:200])
    check(ok, 'Debian lvm2 made vgroot (striped root, data + snapshot), vgc on LUKS2, hostile VGs', markers(log, 'FSTEST', 'rror'))
    dtv.unlink(plain); dtv.unlink(luks)
    if not ok: boot_cases = []
    else:   # hostile metadata, host side (checksums recomputed unless the test is the checksum)
        edit_pv(disk_h, hostile_parts[0], lambda t: (t.replace(b'seqno', b'seqnO', 1), False))
        edit_pv(disk_h, hostile_parts[1], lambda t: (re.sub(rb'pe_count = \d+', b'pe_count = 9999999', t, count=1), True))
        edit_pv(disk_h, hostile_parts[2], lambda t: (re.sub(rb'stripe_count = 1', b'stripe_count = 99', t, count=1), True))
        edit_pv(disk_h, hostile_parts[3], lambda t: (b'vgbad4 {\n' + b'a {\n' * 40 + b'}\n' * 41, True))
        edit_pv(disk_h, hostile_parts[4], label=True)
for cpus in [int(x) for x in args.smp.split(',')]:
    if 'root' in boot_cases:
        spec = 'root=/dev/vgroot/root' if cpus == 1 else 'root=/dev/mapper/vgroot-root'
        b = Boot(f'root-smp{cpus}', [disk_a, disk_b, disk_h], spec, cpus)
        good = b.wait('[64] LVM TEST ALL PASSED', 420); b.exit()
        refused = ['metadata checksum mismatch', 'PV extents beyond the device', 'LV skipped (segment type not supported): lv',
                   'metadata text refused', 'bad PV label checksum', 'volume group vgroot activated', 'volume group vgok activated (late)']
        missing = [r for r in refused if r not in b.log]
        check(good and not missing and not b.faults(), f'root smp{cpus}: {spec}: striped root LV, fstab /dev/vgroot/data, the snapshot at boot, hostile VGs refused',
              (missing, b.faults(), markers(b.log, 'LVM', 'DM ', 'FSTAB')))
    if 'luks' in boot_cases:
        b = Boot(f'luks-smp{cpus}', [disk_c], f'cryptroot=PARTUUID={g["c"]} root=/dev/mapper/vgc-root', cpus)
        good = b.wait('CRYPTROOT PROMPT', 240)
        try:
            if good: qmpkeys.type_text(b.q, PASS + '\n'); good = b.wait('[64] LVM TEST ALL PASSED', 420)
        except OSError: good = False
        b.exit()
        check(good and 'volume group vgc activated' in b.log and not b.faults(), f'luks smp{cpus}: LVM on LUKS2 -- unlock, the VG inside, root /dev/mapper/vgc-root',
              (b.faults(), markers(b.log, 'LVM', 'CRYPTROOT', 'DM ')))
if 'verify' in cases and 'root' in boot_cases and all(results):
    script = HEAD + r'''
vgchange -ay vgroot >/dev/null || fail "vgchange -ay after NexusOS"
lvs --noheadings -o lv_name,lv_attr vgroot | grep -q "datasnap *swi-a-s---" || fail "snapshot state: $(lvs vgroot)"
fsck.vfat -n /dev/vgroot/data >/tmp/f 2>&1 || { cat /tmp/f; fail "fsck data"; }
mkdir -p /mnt/r && fuse2fs -o ro,fakeroot /dev/vgroot/root /mnt/r >/dev/null 2>&1 || fail "fuse2fs root"
grep -q "written on a logical volume" /mnt/r/home/nexus/lvm-proof.txt || fail "the file NexusOS wrote"
umount /mnt/r; vgchange -an vgroot >/dev/null
ok "Debian lvm2 after NexusOS: VG consistent, snapshot valid, NexusOS's file on the root LV, data clean"
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [disk_a, disk_b], {}, timeout=900, tag='lvm-verify')
    check(ok, 'verify: Debian lvm2 reads what NexusOS used and wrote', markers(log, 'FSTEST', 'rror'))
for d in (disk_a, disk_b, disk_c, disk_h): dtv.unlink(d)
print(f'SUMMARY lvm: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
