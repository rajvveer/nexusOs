"""dm-integrity end to end: Debian's integritysetup on the NexusOS integrity target (arch/x86_64/dm_integrity.inc).

  tools     integritysetup format (the kernel formats the zeroed superblock, the device is wiped through the
            mapping) / dump / open / status / close: table and status as Linux prints them, data round trip across a
            close, a vfat on it; a flipped data byte on the disk: EIO for that block only, "failures: 1", the kernel
            names the sector; --integrity-recovery-mode reads it unchecked; --integrity-no-journal (D) round trip;
            sha256 tags with 4096-byte sectors; hostile or unsupported superblocks and tables refused (version 9, tag
            size, interleave 2^50, 0 or 2^31 journal sections, an unknown flag, provided sectors beyond the device, a
            non-zero superblock without magic, bitmap mode, a keyed hash).
  powerloss journal mode, 4096-byte sectors: rounds of 1 MiB writes (each 4 KiB block one byte value) through the
            mapping, the power cut at different moments (--cuts N, default 4); after each cut integritysetup open
            (journal replay) and every block must read (no tag mismatch) and be whole (one value), never mixed.
Usage: test_x64_integrity.py WORK [--cases tools,powerloss] [--cuts 4]"""
from pathlib import Path
import argparse, re, sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='tools,powerloss')
ap.add_argument('--cuts', type=int, default=4)
ap.add_argument('--smp', default='2', help='unused (the Debian tools VM has 2 CPUs); kept for w2_suite')
ap.add_argument('--qmp-port', type=int, default=4738, help='unused; kept for w2_suite')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
results = []
def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True); return ok
def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-3000:]
HEAD = '#!/bin/dash\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1\nfail() { echo "[64] FSTEST FAIL $*"; exit 1; }\nok() { echo "[64] FSTEST PASS $*"; }\nmkdir -p /mnt /run/cryptsetup\n'

if 'tools' in cases:
    a, b, c = WORK / 'i-a.img', WORK / 'i-b.img', WORK / 'i-c.img'
    dtv.raw_disk(a, 32); dtv.raw_disk(b, 32); dtv.raw_disk(c, 16)
    script = HEAD + r'''
integritysetup format -q /dev/sda >/tmp/x 2>&1 || fail "format: $(cat /tmp/x)"
integritysetup dump /dev/sda >/tmp/d 2>&1 || fail "dump"
grep -q "integrity_tag_size 4" /tmp/d && grep -q "log2_interleave_sectors 15" /tmp/d && grep -q "provided_data_sectors" /tmp/d || fail "dump: $(cat /tmp/d)"
P=$(sed -n 's/^provided_data_sectors //p' /tmp/d)
integritysetup open /dev/sda idata >/tmp/x 2>&1 || fail "open: $(cat /tmp/x)"
dmsetup table idata | grep -q "^0 $P integrity 8:0 0 4 J [0-9]* journal_sectors:[0-9]* interleave_sectors:32768 .*internal_hash:crc32c" || fail "table: $(dmsetup table idata)"
[ "$(dmsetup status idata)" = "0 $P integrity 0 $P -" ] || fail "status: $(dmsetup status idata)"
integritysetup status idata | grep -q "failures: 0" || fail "integritysetup status: $(integritysetup status idata)"
dd if=/dev/mapper/idata of=/dev/null bs=1M status=none || fail "the wiped device does not read"
dd if=/dev/urandom of=/tmp/r bs=1M count=8 status=none
dd if=/tmp/r of=/dev/mapper/idata bs=1M conv=fsync status=none && cmp -n 8388608 /tmp/r /dev/mapper/idata || fail "round trip"
integritysetup close idata && integritysetup open /dev/sda idata && cmp -n 8388608 /tmp/r /dev/mapper/idata || fail "after reopen"
ok "integritysetup format/dump/open/status, table and status as Linux, wiped device reads, data across a close"
mkfs.vfat -n IDATA /dev/mapper/idata >/dev/null && mount -t vfat /dev/mapper/idata /mnt && printf 'kept\n' > /mnt/k.txt && umount /mnt && fsck.vfat -n /dev/mapper/idata >/dev/null || fail "vfat"
printf 'INTEGRITY-MARKER-0123456789' | dd of=/dev/mapper/idata bs=512 seek=9000 conv=notrunc,fsync status=none || fail "marker"
integritysetup close idata
OFF=$(grep -obUa "INTEGRITY-MARKER-0123456789" /dev/sda | tail -1 | cut -d: -f1); [ -n "$OFF" ] || fail "marker not on the disk"   # the last copy: in place (the first is in the journal)
printf 'X' | dd of=/dev/sda bs=1 seek=$((OFF + 3)) conv=notrunc,fsync status=none
integritysetup open /dev/sda idata || fail "open after corruption"
dd if=/dev/mapper/idata of=/dev/null bs=512 skip=9000 count=1 status=none 2>/tmp/x && fail "the corrupted sector read"
grep -q "Input/output error\|Invalid or incomplete" /tmp/x || fail "error: $(cat /tmp/x)"
dd if=/dev/mapper/idata of=/dev/null bs=512 skip=8999 count=1 status=none && dd if=/dev/mapper/idata of=/dev/null bs=512 skip=9001 count=1 status=none || fail "neighbours"
mount -t vfat -o ro /dev/mapper/idata /mnt && grep -q kept /mnt/k.txt && umount /mnt || fail "vfat after corruption"
integritysetup status idata | grep -q "failures: [1-9]" || fail "failure count: $(integritysetup status idata)"
integritysetup close idata
ok "a flipped byte on the disk: EIO for that sector only, failures counted, the filesystem still mounts"
integritysetup open --integrity-recovery-mode /dev/sda irec >/tmp/x 2>&1 || fail "recovery mode: $(cat /tmp/x)"
dmsetup table irec | grep -q " 4 R " || fail "R table: $(dmsetup table irec)"
dd if=/dev/mapper/irec of=/tmp/s bs=512 skip=9000 count=1 status=none && grep -q "INTX" /tmp/s || fail "recovery-mode read"
dd if=/dev/zero of=/dev/mapper/irec bs=512 count=1 conv=fsync status=none 2>/dev/null && fail "a write in recovery mode"
integritysetup close irec
ok "--integrity-recovery-mode: unchecked reads, no writes"
integritysetup open --integrity-no-journal /dev/sda idir || fail "no-journal open"
dmsetup table idir | grep -q " 4 D " || fail "D table: $(dmsetup table idir)"
dd if=/tmp/r of=/dev/mapper/idir bs=1M count=8 conv=fsync status=none && cmp -n 8388608 /tmp/r /dev/mapper/idir || fail "D round trip"
integritysetup close idir && integritysetup open /dev/sda idata && cmp -n 8388608 /tmp/r /dev/mapper/idata && integritysetup close idata || fail "D data in J mode"
ok "--integrity-no-journal (direct) round trip, read back in journal mode"
integritysetup format -q --sector-size 4096 --integrity sha256 /dev/sdb >/tmp/x 2>&1 || fail "sha256 format: $(cat /tmp/x)"
integritysetup open --integrity sha256 /dev/sdb i2 >/tmp/x 2>&1 || fail "sha256 open: $(cat /tmp/x)"
dmsetup table i2 | grep -q "integrity 8:16 0 32 J .*block_size:4096 .*internal_hash:sha256" || fail "sha256 table: $(dmsetup table i2)"
[ "$(blockdev --getss /dev/mapper/i2)" = 4096 ] || fail "4096-byte sectors"
dd if=/tmp/r of=/dev/mapper/i2 bs=1M count=8 conv=fsync status=none && cmp -n 8388608 /tmp/r /dev/mapper/i2 && integritysetup close i2 || fail "sha256 round trip"
ok "sha256 tags with 4096-byte sectors"
integritysetup format -q --no-wipe /dev/sdc >/dev/null 2>&1 || fail "format sdc"
dd if=/dev/sdc of=/tmp/sb bs=4096 count=1 status=none
T="0 8 integrity 8:32 0 4 J 1 internal_hash:crc32c"
dmsetup create ig --table "$T" && dmsetup remove ig || fail "the good superblock"
bad() { dd if=/tmp/sb of=/dev/sdc bs=4096 conv=notrunc,fsync status=none; printf "$2" | dd of=/dev/sdc bs=1 seek=$1 conv=notrunc,fsync status=none
        dmsetup create ih --table "${4:-$T}" >/dev/null 2>&1 && { dmsetup remove ih; fail "accepted: $3"; }; dmsetup remove ih >/dev/null 2>&1; true; }
bad 8 '\011' "version 9"
bad 9 '\062' "interleave 2^50"
bad 12 '\000\000\000\000' "no journal sections"
bad 12 '\377\377\377\177' "2^31 journal sections"
bad 24 '\100' "an unknown flag"
bad 16 '\377\377\377\377\377\377\377\177' "provided sectors beyond the device"
bad 0 'GARBAGE!' "no magic, not zero"
bad 0 '' "tag size 8 for a 4-byte superblock" "0 8 integrity 8:32 0 8 J 1 internal_hash:crc32c"
bad 0 '' "bitmap mode" "0 8 integrity 8:32 0 4 B 1 internal_hash:crc32c"
bad 0 '' "a keyed hash" "0 8 integrity 8:32 0 4 J 1 internal_hash:hmac(sha256):00112233"
bad 0 '' "journal_mac" "0 8 integrity 8:32 0 4 J 2 internal_hash:crc32c journal_mac:hmac(sha256):00112233"
dd if=/tmp/sb of=/dev/sdc bs=4096 conv=notrunc,fsync status=none
ok "hostile or unsupported superblocks and tables refused"
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [a, b, c], {}, timeout=1200, tag='integrity-tools')
    for line in log.splitlines():
        if 'FSTEST' in line or 'DM integrity' in line: print('    ' + line.strip()[:200])
    check(ok and 'KERNEL_FAULT' not in log and 'checksum failed at sector 9000' in log,
          'tools: Debian integritysetup -- format/open/status/close, corruption EIO, recovery and direct modes, sha256/4096, hostile superblocks',
          markers(log, 'FSTEST', 'DM ', 'FAULT'))
    for d in (a, b, c): dtv.unlink(d)

if 'powerloss' in cases:
    disk = WORK / 'i-power.img'; dtv.raw_disk(disk, 16)
    common = HEAD + r'''
fill() { oct=$(printf '%03o' $1); dd if=/dev/zero bs=65536 count=16 status=none | tr '\000' "\\$oct" | dd of=/dev/mapper/p bs=65536 iflag=fullblock oflag=direct conv=fsync status=none; }
check() { b=0; while [ $b -lt 256 ]; do
    dd if=/dev/mapper/p of=/tmp/blk bs=4096 skip=$b count=1 status=none 2>/dev/null || fail "EIO at block $b"
    f=$(od -An -tu1 -N1 /tmp/blk | tr -d ' '); [ "$f" != 0 ] || fail "block $b never written"
    [ "$(tr -d "\\$(printf '%03o' $f)" < /tmp/blk | wc -c)" = 0 ] || fail "block $b mixed"
    b=$((b + 1)); done; }
loop() { r=$1; while :; do fill $(( r % 250 + 1 )) || fail "write round $r"; [ $r = $(($1 + 1)) ] && echo "[64] FSTEST WRITING"; r=$((r + 1)); done; }
'''
    first = common + r'''
integritysetup format -q --sector-size 4096 --no-wipe /dev/sda >/tmp/x 2>&1 || fail "format: $(cat /tmp/x)"
integritysetup open /dev/sda p || fail open
fill 1 || fail "first fill"
check; echo "[64] FSTEST VERIFIED 0"
loop 2
'''
    ok, log = dtv.run_script(WORK, first, [disk], {}, timeout=900, tag='integrity-power0', kill_on='[64] FSTEST WRITING', kill_delay=0.9)
    good = '[64] FSTEST VERIFIED 0' in log and '[64] FSTEST WRITING' in log and 'FSTEST FAIL' not in log
    check(good, 'powerloss: format (4096-byte sectors, journal), first fill verified, power cut 1 while writing', markers(log, 'FSTEST', 'DM ', 'FAULT'))
    replays = 0
    for cut in range(1, args.cuts + 1):
        if not good: break
        last = cut == args.cuts
        script = common + rf'''
integritysetup open /dev/sda p >/tmp/x 2>&1 || fail "open after cut {cut}: $(cat /tmp/x)"
check; echo "[64] FSTEST VERIFIED {cut}"
''' + ('integritysetup close p && integritysetup dump /dev/sda >/dev/null || fail close\nsync; echo "[64] FSTEST ALL PASSED"\n' if last else f'loop {cut * 1000}\n')
        ok, log = dtv.run_script(WORK, script, [disk], {}, timeout=900, tag=f'integrity-power{cut}',
                                 kill_on=None if last else '[64] FSTEST WRITING', kill_delay=0.4 + 0.8 * cut)
        replays += len(re.findall(r'journal replayed [1-9]', log))
        torn = len(re.findall(r'[1-9]\d* torn section', log))
        good = f'[64] FSTEST VERIFIED {cut}' in log and 'FSTEST FAIL' not in log and 'KERNEL_FAULT' not in log and (ok if last else '[64] FSTEST WRITING' in log)
        check(good, f'powerloss: after cut {cut} every block reads (tags match) and is whole' + ('' if last else f'; cut {cut + 1} while writing')
              + f' (kernel: {replays} replayed, {torn} torn so far this run)', markers(log, 'FSTEST', 'DM ', 'FAULT'))
    print(f'    journal sections replayed after power cuts: {replays}')
    dtv.unlink(disk)
print(f'SUMMARY integrity: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
