"""LVM raid1 (lvm2's default for lvcreate -m N) on the NexusOS dm "raid" target (arch/x86_64/dm_raid.inc).

  tools      lvcreate --type raid1 -m 1: the initial resync, dmsetup status / lvs Cpy%Sync, the data on both images;
             lvchange --syncaction check (0 mismatches), a byte changed behind the array's back (check counts it,
             repair fixes it, check 0 again); vgchange -an/-ay (clean: no resync); lvconvert -m 2 (only the new leg is
             recovered), lvconvert -m 1 (the leg removed while the sub-LVs are suspended).
  repair     one PV gone: activation in degraded mode (the missing image fails), reads and writes go on; lvconvert
             --repair onto the spare PV rebuilds it, the data matches, vgreduce --removemissing.
  powerloss  a power cut while writing: the next activation resyncs (the set was dirty), both images equal, every
             fsynced write there.
Usage: test_x64_lvmraid.py WORK [--cases tools,repair,powerloss]"""
from pathlib import Path
import argparse, sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='tools,repair,powerloss')
ap.add_argument('--smp', default='2', help='unused (the Debian tools VM has 2 CPUs)')
ap.add_argument('--qmp-port', type=int, default=4739, help='unused')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
results = []
def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True); return ok
def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-3000:]
HEAD = ('#!/bin/dash\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1\nfail() { echo "[64] FSTEST FAIL $*"; dmsetup status; exit 1; }\nok() { echo "[64] FSTEST PASS $*"; }\n'
        + dtv.LVM_SETUP + '\n' +
        'synced() { i=0; until [ "$(lvs --noheadings -o sync_percent vgr/r1 2>/dev/null | tr -d \' \')" = "100.00" ]; do i=$((i+1)); [ $i -gt 600 ] && fail "not in sync: $(dmsetup status vgr-r1)"; sleep 0.2; done; }\n'
        'idle() { i=0; until dmsetup status vgr-r1 | grep -q " idle "; do i=$((i+1)); [ $i -gt 600 ] && fail "not idle: $(dmsetup status vgr-r1)"; sleep 0.2; done; }\n'
        'mism() { lvs --noheadings -o raid_mismatch_count vgr/r1 | tr -d " "; }\n')

if 'tools' in cases:
    disks = [WORK / f'r{i}.img' for i in range(3)]
    for d in disks: dtv.raw_disk(d, 64)
    script = HEAD + r'''
dd if=/dev/urandom of=/tmp/a bs=1M count=16 status=none
pvcreate -y /dev/sda /dev/sdb /dev/sdc >/dev/null && vgcreate vgr /dev/sda /dev/sdb /dev/sdc >/dev/null || fail vg
lvcreate -y --type raid1 -m 1 -L 32M -n r1 vgr /dev/sda /dev/sdb >/tmp/x 2>&1 || fail "lvcreate raid1: $(cat /tmp/x)"
dmsetup table vgr-r1 | grep -q "raid raid1 3 0 region_size [0-9]* 2 253:[0-9]* 253:[0-9]* 253:[0-9]* 253:[0-9]*$" || fail "table: $(dmsetup table vgr-r1)"
synced; idle
dmsetup status vgr-r1 | grep -q "raid raid1 2 AA 65536/65536 idle 0 0 -" || fail "status: $(dmsetup status vgr-r1)"
lvs --noheadings -o lv_attr vgr/r1 | grep -q "rwi-a-r---" || fail "lvs attr: $(lvs vgr)"
dd if=/tmp/a of=/dev/vgr/r1 bs=1M conv=fsync status=none && cmp -n 16777216 /tmp/a /dev/vgr/r1 || fail data
cmp -n 16777216 /tmp/a /dev/mapper/vgr-r1_rimage_0 && cmp -n 16777216 /tmp/a /dev/mapper/vgr-r1_rimage_1 || fail "the data on both images"
ok "lvcreate --type raid1 -m 1: resync, status AA idle, lvs, the data on both images"
lvchange -y --syncaction check vgr/r1 >/dev/null 2>&1 && idle && [ "$(mism)" = 0 ] || fail "check: $(mism)"
printf XXXXXXXX | dd of=/dev/mapper/vgr-r1_rimage_1 bs=512 seek=100 conv=notrunc,fsync status=none
lvchange -y --syncaction check vgr/r1 >/dev/null 2>&1 && sleep 0.5 && idle && [ "$(mism)" -gt 0 ] || fail "check after damage: $(mism)"
lvchange -y --syncaction repair vgr/r1 >/dev/null 2>&1 && sleep 0.5 && idle || fail repair
lvchange -y --syncaction check vgr/r1 >/dev/null 2>&1 && sleep 0.5 && idle && [ "$(mism)" = 0 ] || fail "check after repair: $(mism)"
cmp -n 16777216 /tmp/a /dev/mapper/vgr-r1_rimage_1 || fail "repaired image"
ok "--syncaction check counts a damaged block, repair fixes it, check 0 again"
vgchange -an vgr >/dev/null && vgchange -ay vgr >/dev/null || fail "deactivate/activate"
dmsetup status vgr-r1 | grep -q "AA 65536/65536 idle" || fail "a clean set resynced: $(dmsetup status vgr-r1)"
cmp -n 16777216 /tmp/a /dev/vgr/r1 || fail "data after reactivation"
ok "vgchange -an/-ay: clean on disk, no resync"
lvconvert -y -m 2 vgr/r1 /dev/sdc >/tmp/x 2>&1 || fail "lvconvert -m 2: $(cat /tmp/x)"
synced; idle; dmsetup status vgr-r1 | grep -q "raid1 3 AAA" || fail "3 legs: $(dmsetup status vgr-r1)"
cmp -n 16777216 /tmp/a /dev/mapper/vgr-r1_rimage_2 || fail "the new leg"
lvconvert -y -m 1 vgr/r1 /dev/sdc >/tmp/x 2>&1 || fail "lvconvert -m 1: $(cat /tmp/x)"
dmsetup status vgr-r1 | grep -q "raid1 2 AA" && cmp -n 16777216 /tmp/a /dev/vgr/r1 || fail "back to 2 legs: $(dmsetup status vgr-r1)"
ok "lvconvert -m 2 (the new leg recovered) and -m 1"
vgchange -an vgr >/dev/null
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, disks, {}, timeout=1500, tag='lvmraid-tools')
    for line in log.splitlines():
        if 'FSTEST' in line or 'DM raid' in line: print('    ' + line.strip()[:200])
    check(ok and 'KERNEL_FAULT' not in log and 'resync done' not in log.split('DM raid: check done')[-1], 'tools: lvm2 raid1 -- resync, status/lvs, check/repair, clean reactivation, lvconvert -m 2/-m 1',
          markers(log, 'FSTEST', 'DM ', 'FAULT'))
    for d in disks: dtv.unlink(d)

if 'repair' in cases:
    disks = [WORK / f'p{i}.img' for i in range(3)]
    for d in disks: dtv.raw_disk(d, 64)
    first = HEAD + r'''
pvcreate -y /dev/sda /dev/sdb /dev/sdc >/dev/null && vgcreate vgr /dev/sda /dev/sdb /dev/sdc >/dev/null || fail vg
lvcreate -y --type raid1 -m 1 -L 32M -n r1 vgr /dev/sda /dev/sdb >/dev/null 2>&1 || fail lvcreate
synced
yes RAIDDATA | head -c 16777216 > /tmp/a
dd if=/tmp/a of=/dev/vgr/r1 bs=1M conv=fsync status=none || fail write
vgchange -an vgr >/dev/null
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, first, disks, {}, timeout=900, tag='lvmraid-repair-1')
    if check(ok, 'repair: raid1 on two PVs with data, a spare PV', markers(log, 'FSTEST')):
        second = HEAD + r'''
yes RAIDDATA | head -c 16777216 > /tmp/a
vgchange -ay --activationmode degraded vgr >/tmp/x 2>&1; [ -b /dev/vgr/r1 ] || fail "degraded activation: $(cat /tmp/x)"
dmsetup status vgr-r1 | grep -q "raid1 2 AD\|raid1 2 DA" || fail "status with a leg gone: $(dmsetup status vgr-r1)"
cmp -n 16777216 /tmp/a /dev/vgr/r1 || fail "degraded read"
dd if=/tmp/a of=/dev/vgr/r1 bs=1M seek=16 count=4 conv=notrunc,fsync status=none && cmp -n 4194304 -i 0:16777216 /tmp/a /dev/vgr/r1 || fail "degraded write"
ok "a PV gone: degraded activation, the missing leg D, reads and writes go on"
lvconvert -y --repair vgr/r1 >/tmp/x 2>&1 || fail "lvconvert --repair: $(cat /tmp/x)"
synced; idle
dmsetup status vgr-r1 | grep -q "raid1 2 AA" || fail "after repair: $(dmsetup status vgr-r1)"
cmp -n 16777216 /tmp/a /dev/mapper/vgr-r1_rimage_0 && cmp -n 16777216 /tmp/a /dev/mapper/vgr-r1_rimage_1 && cmp -n 4194304 -i 0:16777216 /tmp/a /dev/vgr/r1 || fail "images after repair"
vgreduce -y --removemissing --force vgr >/tmp/x 2>&1 || fail "vgreduce: $(cat /tmp/x)"
ok "lvconvert --repair rebuilt the leg on the spare PV, vgreduce --removemissing"
vgchange -an vgr >/dev/null
sync; echo "[64] FSTEST ALL PASSED"
'''
        ok, log = dtv.run_script(WORK, second, [disks[0], disks[2]], {}, timeout=1200, tag='lvmraid-repair-2')
        for line in log.splitlines():
            if 'FSTEST' in line or 'DM raid' in line: print('    ' + line.strip()[:200])
        check(ok and 'KERNEL_FAULT' not in log, 'repair: degraded activation, --repair onto the spare, vgreduce --removemissing', markers(log, 'FSTEST', 'DM ', 'FAULT'))
    for d in disks: dtv.unlink(d)

if 'powerloss' in cases:
    disks = [WORK / f'w{i}.img' for i in range(2)]
    for d in disks: dtv.raw_disk(d, 64)
    fill = r'''fill() { oct=$(printf '%03o' $(( $1 % 250 + 1 ))); dd if=/dev/zero bs=1M count=4 status=none | tr '\000' "\\$oct" | dd of=/dev/vgr/r1 bs=1M seek=$(( ($1 % 6) * 4 )) conv=notrunc,fsync status=none; }
'''
    first = HEAD + fill + r'''
pvcreate -y /dev/sda /dev/sdb >/dev/null && vgcreate vgr /dev/sda /dev/sdb >/dev/null && lvcreate -y --type raid1 -m 1 -L 32M -n r1 vgr >/dev/null 2>&1 || fail setup
synced
r=1; while :; do fill $r || fail "write $r"; echo "[64] FSTEST ROUND $r DONE"; [ $r = 8 ] && echo "[64] FSTEST WRITING"; r=$((r + 1)); done
'''
    ok, log = dtv.run_script(WORK, first, disks, {}, timeout=900, tag='lvmraid-power-1', kill_on='[64] FSTEST WRITING', kill_delay=0.8)
    import re
    done = [int(x) for x in re.findall(r'FSTEST ROUND (\d+) DONE', log)]
    if check('[64] FSTEST WRITING' in log, f'powerloss: a power cut while writing (after round {max(done) if done else "-"})', markers(log, 'FSTEST', 'DM ')):
        last = {}
        for r in done: last[r % 6] = r % 250 + 1
        inflight = (max(done) + 1) % 6
        checks = ''.join(f'slot {s} {v}\n' for s, v in sorted(last.items()) if s != inflight)
        second = HEAD + r'''
vgchange -ay vgr >/dev/null 2>&1 || fail activation
dmsetup status vgr-r1 | grep -q " resync \| idle " || fail "status: $(dmsetup status vgr-r1)"
synced; idle
cmp /dev/mapper/vgr-r1_rimage_0 /dev/mapper/vgr-r1_rimage_1 || fail "the images differ after the resync"
while read word s v; do dd if=/dev/zero bs=1M count=4 status=none | tr '\000' "\\$(printf '%03o' $v)" > /tmp/p
  cmp -n 4194304 -i 0:$((s * 4194304)) /tmp/p /dev/vgr/r1 || fail "slot $s lost its fsynced round"; done < /nexus/expect
ok "after the cut: resync, both images equal, every fsynced round there"
vgchange -an vgr >/dev/null
sync; echo "[64] FSTEST ALL PASSED"
'''
        ok, log = dtv.run_script(WORK, second, disks, {'nexus/expect': checks.encode()}, timeout=900, tag='lvmraid-power-2')
        check(ok and 'DM raid: resync done' in log, 'powerloss: the dirty set resynced at activation, images equal, fsynced data intact', markers(log, 'FSTEST', 'DM '))
    for d in disks: dtv.unlink(d)
print(f'SUMMARY lvmraid: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
