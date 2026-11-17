"""dm-verity end to end: Debian's veritysetup on the NexusOS verity target (arch/x86_64/dm_verity.inc), and a verity
root from roothash= at boot.

  tools     in a NexusOS guest with Debian's cryptsetup-bin: veritysetup format (hash tree on a second disk) / verify /
            open / status; reads through the mapping equal the data device, the vfat on it mounts, writes refused;
            a flipped data byte: EIO for that block only, status "C" ("corrupted"), the kernel names the block;
            --ignore-corruption passes the data through; a corrupted hash block fails the data blocks below it;
            --ignore-zero-blocks/--check-at-most-once tables; hostile or unsupported tables refused (version 2, block
            size 1000, sha1, short digest, hash device too small, data blocks shorter than the target, writable, two
            modes, FEC); last: --restart-on-corruption resets the machine on the bad block. Also formats the boot root.
  boot      the root filesystem verified at boot: roothash= systemd.verity_root_data=/dev/sda
            systemd.verity_root_hash=/dev/sdb root=/dev/mapper/root (smp1 and smp2): arch/x86_64/verity_user.c checks
            / is the read-only verity mapping and reads a canary file; then one flipped canary byte: EIO for that block
            (smp2), and with systemd.verity_root_options=restart-on-corruption the machine resets (smp1).
Usage: test_x64_verity.py WORK [--cases tools,boot] [--smp 1,2]"""
from pathlib import Path
import argparse, re, shutil, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402
QEMU, CPU, ZIG = dtv.QEMU, dtv.CPU, dtv.ZIG

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='tools,boot')
ap.add_argument('--smp', default='1,2')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
smps = [int(x) for x in args.smp.split(',')]
results = []
def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True); return ok
def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-3000:]
HEAD = '#!/bin/dash\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1\nfail() { echo "[64] FSTEST FAIL $*"; exit 1; }\nok() { echo "[64] FSTEST PASS $*"; }\nmkdir -p /mnt /run/cryptsetup\n'


def canary_byte(i):
    x = (i * 2654435761 + 12345) & 0xFFFFFFFF; x ^= x >> 13; return (x ^ (x >> 7)) & 0xFF


CANARY = bytes(canary_byte(i) for i in range(262144))


def build(source):
    out = WORK / (Path(source).stem + '.elf')
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-musl', '-static', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64' / source), '-o', str(out)], check=True)
    return out.read_bytes()


def ext4_root(name, files):
    ext4lib = dtv.load('verity_ext4lib_' + name, ROOT / 'userland/ext4lib.py')
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
                    '-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
        self.vm = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def finish(self, text, seconds=240):
        end = time.time() + seconds
        while time.time() < end and self.vm.poll() is None:
            self.log = self.serial.read_text(encoding='utf-8', errors='replace') if self.serial.exists() else ''
            if text in self.log: break
            time.sleep(.2)
        try: self.vm.wait(60)
        except subprocess.TimeoutExpired: pass
        if self.vm.poll() is None: self.vm.kill(); self.vm.wait()
        self.log = self.serial.read_text(encoding='utf-8', errors='replace') if self.serial.exists() else ''
        return text in self.log
    def faults(self): return [w for w in ('KERNEL_FAULT', '[64] FAIL:', 'USER_FAULT', 'PANIC', 'VERITY TEST FAIL') if w in self.log]


data, hashd, rootfs, roothash_disk = WORK / 'v-data.img', WORK / 'v-hash.img', None, WORK / 'v-roothash.img'
root_hash = None
if 'tools' in cases:
    init = build('verity_user.c')
    rootfs = ext4_root('verity', {'sbin/init': (init, 0o755), 'nexus/init-as-root': (b'verity\n', 0o644), 'nexus/verity-canary.bin': (CANARY, 0o644)})
    dtv.raw_disk(data, 32); dtv.raw_disk(hashd, 4); dtv.raw_disk(roothash_disk, 4)
    script = HEAD + r'''
mkfs.vfat -n VDATA /dev/sda >/dev/null && mount -t vfat /dev/sda /mnt && printf 'verified content\n' > /mnt/a.txt && dd if=/dev/urandom of=/mnt/r.bin bs=1M count=4 status=none && umount /mnt || fail "data fs"
veritysetup format /dev/sda /dev/sdb >/tmp/f 2>&1 || fail "format: $(cat /tmp/f)"
RH=$(sed -n 's/^Root hash:[[:space:]]*//p' /tmp/f); [ ${#RH} = 64 ] || fail "root hash: $(cat /tmp/f)"
veritysetup verify /dev/sda /dev/sdb $RH >/tmp/x 2>&1 || fail "userspace verify: $(cat /tmp/x)"
veritysetup open /dev/sda vdata /dev/sdb $RH >/tmp/x 2>&1 || fail "open: $(cat /tmp/x)"
dmsetup table vdata | grep -q "^0 65536 verity 1 8:0 8:16 4096 4096 8192 1 sha256 $RH " || fail "table: $(dmsetup table vdata)"
veritysetup status vdata | grep -q "status: *verified" || fail "status: $(veritysetup status vdata)"
[ "$(blockdev --getss /dev/mapper/vdata)" = 4096 ] && [ "$(blockdev --getro /dev/mapper/vdata)" = 1 ] || fail "block size / ro"
cmp /dev/mapper/vdata /dev/sda || fail "reads differ from the data device"
mount -t vfat -o ro /dev/mapper/vdata /mnt && grep -q "verified content" /mnt/a.txt && umount /mnt || fail "vfat through verity"
dd if=/dev/zero of=/dev/mapper/vdata bs=4096 count=1 conv=fsync status=none 2>/dev/null && fail "a write went through"
veritysetup close vdata || fail close
ok "veritysetup format/verify/open/status: table, 4096-byte read-only device, reads equal, vfat mounts, writes refused"
WRONG=$(echo $RH | tr '0-9a-f' '1-9a-f0')
if veritysetup open /dev/sda vbad /dev/sdb $WRONG >/dev/null 2>&1; then dd if=/dev/mapper/vbad of=/dev/null bs=4096 count=1 status=none 2>/dev/null && fail "a wrong root hash verified"; veritysetup close vbad; fi
ok "a wrong root hash verifies nothing"
dd if=/dev/sda of=/tmp/orig5 bs=4096 skip=5 count=1 status=none
printf '\377' | dd of=/dev/sda bs=1 seek=$((5 * 4096 + 100)) conv=notrunc,fsync status=none
veritysetup open /dev/sda vdata /dev/sdb $RH >/dev/null 2>&1 || fail "reopen"
dd if=/dev/mapper/vdata of=/dev/null bs=4096 skip=5 count=1 status=none 2>/tmp/x && fail "the corrupted block read"
grep -q "Input/output error" /tmp/x || fail "not EIO: $(cat /tmp/x)"
dd if=/dev/mapper/vdata of=/dev/null bs=4096 skip=4 count=1 status=none && dd if=/dev/mapper/vdata of=/dev/null bs=4096 skip=6 count=1 status=none || fail "neighbours"
dmsetup status vdata | grep -q "verity C$" && veritysetup status vdata | grep -q "status: *corrupted" || fail "status after corruption: $(dmsetup status vdata)"
veritysetup verify /dev/sda /dev/sdb $RH >/dev/null 2>&1 && fail "userspace verify passed corrupted data"
veritysetup close vdata
ok "a flipped data byte: EIO for that block only, status C/corrupted, userspace verify agrees"
veritysetup open --ignore-corruption /dev/sda vdata /dev/sdb $RH >/dev/null 2>&1 || fail "open ignore"
dmsetup table vdata | grep -q " 1 ignore_corruption$" || fail "ignore table: $(dmsetup table vdata)"
dd if=/dev/mapper/vdata of=/tmp/b5 bs=4096 skip=5 count=1 status=none && dd if=/dev/sda of=/tmp/s5 bs=4096 skip=5 count=1 status=none && cmp /tmp/b5 /tmp/s5 || fail "ignore_corruption read"
dmsetup status vdata | grep -q "verity C$" || fail "ignore status"
veritysetup close vdata
ok "--ignore-corruption: the block is returned as it is, the corruption still reported"
dd if=/tmp/orig5 of=/dev/sda bs=4096 seek=5 conv=notrunc,fsync status=none
dd if=/dev/sdb of=/tmp/h2 bs=4096 skip=2 count=1 status=none
printf '\125' | dd of=/dev/sdb bs=1 seek=$((2 * 4096 + 7)) conv=notrunc,fsync status=none
veritysetup open /dev/sda vdata /dev/sdb $RH >/dev/null 2>&1 || fail "open hash corrupt"
dd if=/dev/mapper/vdata of=/dev/null bs=4096 skip=0 count=1 status=none 2>/dev/null && fail "a block under a corrupted hash block read"
dd if=/dev/mapper/vdata of=/dev/null bs=4096 skip=200 count=1 status=none || fail "a block under another hash block"
veritysetup close vdata
dd if=/tmp/h2 of=/dev/sdb bs=4096 seek=2 conv=notrunc,fsync status=none
ok "a corrupted hash block fails the data blocks it covers, others read"
veritysetup open --ignore-zero-blocks --check-at-most-once /dev/sda vdata /dev/sdb $RH >/dev/null 2>&1 || fail "open options: $(dmsetup table vdata)"
dmsetup table vdata | grep -q " 2 ignore_zero_blocks check_at_most_once$" || fail "options table: $(dmsetup table vdata)"
cmp /dev/mapper/vdata /dev/sda && cmp /dev/mapper/vdata /dev/sda || fail "reads with options"
veritysetup close vdata
ok "--ignore-zero-blocks --check-at-most-once tables, reads equal (twice)"
SALT=$(veritysetup dump /dev/sdb | sed -n 's/^Salt:[[:space:]]*//p')
T="verity 1 8:0 8:16 4096 4096 8192 1 sha256 $RH $SALT"
dmsetup create --readonly vgood --table "0 65536 $T" && cmp /dev/mapper/vgood /dev/sda && dmsetup remove vgood || fail "dmsetup verity table"
bad() { dmsetup create --readonly vh --table "$1" >/dev/null 2>&1 && { dmsetup remove vh; fail "hostile table accepted: $2"; }; dmsetup remove vh >/dev/null 2>&1; true; }
bad "0 65536 verity 2 8:0 8:16 4096 4096 8192 1 sha256 $RH $SALT" "version 2"
bad "0 65536 verity 1 8:0 8:16 1000 4096 8192 1 sha256 $RH $SALT" "block size 1000"
bad "0 65536 verity 1 8:0 8:16 4096 4096 8192 1 sha1 $RH $SALT" "sha1"
bad "0 65536 verity 1 8:0 8:16 4096 4096 8192 1 sha256 abcd $SALT" "short digest"
bad "0 65536 verity 1 8:0 8:16 4096 4096 8192 1000 sha256 $RH $SALT" "hash device too small"
bad "0 65536 verity 1 8:0 8:16 4096 4096 100 1 sha256 $RH $SALT" "data blocks shorter than the target"
bad "0 65536 $T 2 ignore_corruption restart_on_corruption" "two modes"
bad "0 65536 $T 2 use_fec_from_device /dev/sdb" "FEC"
dmsetup create vrw --table "0 65536 $T" >/dev/null 2>&1 && { dmsetup remove vrw; fail "writable verity table accepted"; }
dmsetup remove vrw >/dev/null 2>&1
ok "hostile or unsupported verity tables refused"
veritysetup format /dev/sdc /dev/sdd >/tmp/f 2>&1 || fail "format the root"
echo "[64] FSTEST ROOTHASH $(sed -n 's/^Root hash:[[:space:]]*//p' /tmp/f)"
printf '\377' | dd of=/dev/sda bs=1 seek=$((5 * 4096 + 100)) conv=notrunc,fsync status=none
veritysetup open --restart-on-corruption /dev/sda vdata /dev/sdb $RH >/dev/null 2>&1 || fail "open restart"
dmsetup table vdata | grep -q " 1 restart_on_corruption$" || fail "restart table"
sync; echo "[64] FSTEST RESTART EXPECTED"
dd if=/dev/mapper/vdata of=/dev/null bs=4096 skip=5 count=1 status=none 2>/dev/null
echo "[64] FSTEST FAIL not restarted"
'''
    ok, log = dtv.run_script(WORK, script, [data, hashd, rootfs, roothash_disk], {}, timeout=900, tag='verity-tools')
    for line in log.splitlines():
        if 'FSTEST' in line or 'DM verity' in line: print('    ' + line.strip()[:200])
    restarted = '[64] FSTEST RESTART EXPECTED' in log and 'dm-verity device corrupted, restarting' in log and 'not restarted' not in log
    passes = log.count('[64] FSTEST PASS')
    check(restarted and passes == 7 and 'FSTEST FAIL' not in log and 'KERNEL_FAULT' not in log and 'data block 5 is corrupted' in log and 'metadata block 2 is corrupted' in log,
          f'tools: Debian veritysetup -- format/verify/open/status, EIO, ignore, hash-block corruption, options, hostile tables, restart ({passes}/7)',
          markers(log, 'FSTEST', 'DM ', 'FAULT'))
    m = re.search(r'\[64\] FSTEST ROOTHASH ([0-9a-f]{64})', log)
    root_hash = m.group(1) if m else None
    dtv.unlink(data); dtv.unlink(hashd)

if 'boot' in cases and root_hash and rootfs:
    base = f'roothash={root_hash} systemd.verity_root_data=/dev/sda systemd.verity_root_hash=/dev/sdb root=/dev/mapper/root'
    for cpus in smps:
        b = Boot(f'boot-smp{cpus}', [rootfs, roothash_disk], f'{base} veritytest=root', cpus)
        good = b.finish('[64] VERITY TEST ALL PASSED')
        check(good and '/dev/mapper/root verified against roothash=' in b.log and not b.faults(), f'boot smp{cpus}: root on dm-verity from roothash=, read-only, canary intact',
              (b.faults(), markers(b.log, 'VERITY', 'DM ', 'EXT4', 'ROOT')))
    raw = bytearray(rootfs.read_bytes()); at = raw.find(CANARY[3 * 4096:4 * 4096])
    if check(at >= 0 and at % 4096 == 0, 'the canary block in the root image'):
        with open(rootfs, 'r+b') as f: f.seek(at + 17); f.write(bytes([raw[at + 17] ^ 0x5A]))
        b = Boot('boot-eio', [rootfs, roothash_disk], f'{base} veritytest=eio', smps[-1])
        good = b.finish('[64] VERITY TEST ALL PASSED')
        check(good and 'is corrupted' in b.log and not b.faults(), f'boot smp{smps[-1]}: a flipped root byte -- EIO for that block, the rest reads',
              (b.faults(), markers(b.log, 'VERITY', 'DM ')))
        b = Boot('boot-restart', [rootfs, roothash_disk], f'{base} systemd.verity_root_options=restart-on-corruption veritytest=restart', smps[0])
        b.finish('dm-verity device corrupted, restarting', 240)
        check('READING THE CORRUPTED BLOCK' in b.log and 'dm-verity device corrupted, restarting' in b.log and 'NOT RESTARTED' not in b.log and b.vm.returncode is not None,
              f'boot smp{smps[0]}: restart-on-corruption resets the machine at the bad block', markers(b.log, 'VERITY', 'DM '))
for d in (data, hashd, roothash_disk, rootfs):
    if d: dtv.unlink(d)
print(f'SUMMARY verity: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
