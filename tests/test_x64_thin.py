"""dm-thin and thin-pool (arch/x86_64/dm_thin.inc) with Debian's lvm2 and thin-provisioning-tools.

  tools      lvcreate -T (the kernel formats the metadata), an over-provisioned thin LV (256 MiB on a 64 MiB pool), data
             round trip, a thin snapshot (the origin's later writes do not reach it), lvs Data%/Meta%; deactivation and
             thin_check / thin_dump on the metadata (mappings and shared blocks as written); reactivation (lvm2 runs
             thin_check itself) with the data intact; out of data space with --errorwhenfull y: EIO, status
             out_of_data_space, lvextend of the pool, writes go on; queue mode: a write waits while the pool is full and
             completes after lvextend; lvremove; thin_check at the end.
  powerloss  rounds of fsynced 4 MiB writes to a thin LV and a snapshot of it, the power cut while writing (--cuts, default
             2): lvm2's activation (its thin_check) passes, every fsynced round reads back (the round in flight: each
             sector old or new), the snapshot unchanged, thin_check clean.
  hostile    the kernel's own checks (lvm2's thin_check switched off): a superblock checksum and a mapping-tree node
             checksum broken on the disk -- the pool table is refused; repaired, the pool activates.
Usage: test_x64_thin.py WORK [--cases tools,powerloss,hostile] [--cuts 2]"""
from pathlib import Path
import argparse, re, struct, sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='tools,powerloss,hostile')
ap.add_argument('--cuts', type=int, default=2)
ap.add_argument('--smp', default='2', help='unused (the Debian tools VM has 2 CPUs)')
ap.add_argument('--qmp-port', type=int, default=4739, help='unused')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
results = []
def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True); return ok
def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-3000:]
LVM = lambda extra='': f'''mkdir -p /etc/lvm /run/lvm /run/lock/lvm
cat > /etc/lvm/lvm.conf <<'EOF'
devices {{ obtain_device_list_from_udev = 0 external_device_info_source = "none" use_devicesfile = 0 }}
global {{ use_lvmlockd = 0 {extra} }}
activation {{ udev_sync = 0 udev_rules = 0 verify_udev_operations = 0 monitoring = 0 }}
EOF
export LVM_SUPPRESS_FD_WARNINGS=1
'''
HEAD = '#!/bin/dash\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1\nfail() { echo "[64] FSTEST FAIL $*"; dmsetup status; exit 1; }\nok() { echo "[64] FSTEST PASS $*"; }\n'
META = 'lvchange -ay -y vgt/pool_tmeta >/dev/null 2>&1 || fail "component activation"\n'

if 'tools' in cases:
    a, b = WORK / 'thin-a.img', WORK / 'thin-b.img'
    dtv.raw_disk(a, 192); dtv.raw_disk(b, 192)
    script = HEAD + LVM() + r'''
dd if=/dev/urandom of=/tmp/a bs=1M count=16 status=none; dd if=/dev/urandom of=/tmp/q bs=1M count=96 status=none
pvcreate -y /dev/sda /dev/sdb >/dev/null && vgcreate vgt /dev/sda /dev/sdb >/dev/null || fail vg
lvcreate -y -L 64M -T vgt/pool >/tmp/x 2>&1 || fail "pool: $(cat /tmp/x)"
dmsetup table | grep -q "thin-pool 253:[0-9]* 253:[0-9]* 128 0 0" || fail "pool table: $(dmsetup table)"
lvcreate -y -V 256M -T vgt/pool -n thin1 >/tmp/x 2>&1 || fail "thin1: $(cat /tmp/x)"
dmsetup table vgt-thin1 | grep -q "^0 524288 thin 253:[0-9]* 1$" || fail "thin table: $(dmsetup table vgt-thin1)"
dd if=/tmp/a of=/dev/vgt/thin1 bs=1M conv=fsync status=none && cmp -n 16777216 /tmp/a /dev/vgt/thin1 || fail "thin data"
dd if=/tmp/a of=/dev/vgt/thin1 bs=1M seek=200 count=1 conv=fsync status=none && cmp -n 1048576 -i 0:209715200 /tmp/a /dev/vgt/thin1 || fail "a write at 200 MiB of 256"
cmp -n 1048576 -i 0:104857600 /dev/zero /dev/vgt/thin1 || fail "unmapped space reads zero"
dmsetup status vgt-thin1 | grep -q "thin 34816 411647$" || fail "thin status: $(dmsetup status vgt-thin1)"
lvs --noheadings -o lv_name,data_percent vgt/thin1 | grep -q "thin1 *6.64" || fail "lvs: $(lvs vgt)"
ok "thin pool and an over-provisioned thin LV (256 MiB on 64 MiB): data, sparse writes, zeroes, status, lvs"
lvcreate -y -s vgt/thin1 -n snap1 >/dev/null 2>&1 && lvchange -ay -K vgt/snap1 >/dev/null 2>&1 || fail "thin snapshot"
dd if=/dev/zero of=/dev/vgt/thin1 bs=1M count=2 conv=fsync,notrunc status=none
cmp -n 16777216 /tmp/a /dev/vgt/snap1 && cmp -n 2097152 /dev/zero /dev/vgt/thin1 && cmp -n 14680064 -i 2097152 /tmp/a /dev/vgt/thin1 || fail "snapshot sharing"
ok "thin snapshot: the origin's later writes do not reach it"
vgchange -an vgt >/dev/null || fail "deactivate"
''' + META + r'''
thin_check /dev/mapper/vgt-pool_tmeta >/tmp/x 2>&1 || fail "thin_check: $(cat /tmp/x)"
thin_dump /dev/mapper/vgt-pool_tmeta >/tmp/d 2>&1 || fail "thin_dump"
grep -q '<device dev_id="1" mapped_blocks="272"' /tmp/d && grep -q '<device dev_id="2" mapped_blocks="272"' /tmp/d || fail "thin_dump: $(head -12 /tmp/d)"
lvchange -an vgt/pool_tmeta >/dev/null 2>&1
ok "thin_check passes, thin_dump shows both devices with their mappings"
vgchange -ay vgt >/tmp/x 2>&1 && lvchange -ay -K vgt/snap1 >/dev/null 2>&1 || fail "reactivation (lvm2 runs thin_check): $(cat /tmp/x)"
cmp -n 16777216 /tmp/a /dev/vgt/snap1 && cmp -n 14680064 -i 2097152 /tmp/a /dev/vgt/thin1 && cmp -n 1048576 -i 0:209715200 /tmp/a /dev/vgt/thin1 || fail "data after reactivation"
ok "reactivation through lvm2's thin_check, data intact"
lvchange --errorwhenfull y vgt/pool >/dev/null 2>&1 || fail errorwhenfull
dmsetup table vgt-pool-tpool | grep -q "1 error_if_no_space" || fail "error_if_no_space table"
lvcreate -y -V 256M -T vgt/pool -n thin2 >/dev/null 2>&1 || fail thin2
dd if=/tmp/q of=/dev/vgt/thin2 bs=1M count=80 conv=fsync status=none 2>/tmp/x && fail "80 MiB fitted in a full pool"
grep -q "Input/output error\|No space left" /tmp/x || fail "error: $(cat /tmp/x)"
dmsetup status vgt-pool-tpool | grep -q "out_of_data_space" || fail "status: $(dmsetup status vgt-pool-tpool)"
lvextend -y -L +64M vgt/pool >/tmp/x 2>&1 || fail "lvextend: $(cat /tmp/x)"
dmsetup status vgt-pool-tpool | grep -q " rw " || fail "status after lvextend: $(dmsetup status vgt-pool-tpool)"
dd if=/tmp/q of=/dev/vgt/thin2 bs=1M count=40 conv=fsync,notrunc status=none && cmp -n 41943040 /tmp/q /dev/vgt/thin2 || fail "writes after lvextend"
ok "out of data space with error_if_no_space: EIO, out_of_data_space; lvextend of the pool, writes go on"
lvchange --errorwhenfull n vgt/pool >/dev/null 2>&1 || fail queue
lvcreate -y -V 256M -T vgt/pool -n thin3 >/dev/null 2>&1 || fail thin3
( dd if=/tmp/q of=/dev/vgt/thin3 bs=1M count=96 conv=fsync status=none; echo "rc=$?" > /tmp/qrc ) &
i=0; until dmsetup status vgt-pool-tpool | grep -q out_of_data_space; do i=$((i+1)); [ $i -gt 600 ] && fail "the pool did not fill"; sleep 0.1; done
sleep 1; [ -e /tmp/qrc ] && fail "the write did not wait: $(cat /tmp/qrc)"
lvextend -y -L +96M vgt/pool >/tmp/x 2>&1 || fail "lvextend (queue): $(cat /tmp/x)"
i=0; until [ -e /tmp/qrc ]; do i=$((i+1)); [ $i -gt 600 ] && fail "the queued write never completed"; sleep 0.1; done
grep -q "rc=0" /tmp/qrc && cmp -n 100663296 /tmp/q /dev/vgt/thin3 || fail "queued write: $(cat /tmp/qrc)"
ok "queue_if_no_space: a write waits while the pool is full and completes after lvextend"
lvremove -y vgt/snap1 vgt/thin2 vgt/thin3 >/dev/null 2>&1 || fail lvremove
cmp -n 14680064 -i 2097152 /tmp/a /dev/vgt/thin1 || fail "thin1 after removing the others"
vgchange -an vgt >/dev/null || fail deactivate2
''' + META + r'''
thin_check /dev/mapper/vgt-pool_tmeta >/tmp/x 2>&1 || fail "final thin_check: $(cat /tmp/x)"
lvchange -an vgt/pool_tmeta >/dev/null 2>&1
ok "lvremove of thin LVs and snapshots, thin_check clean at the end"
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [a, b], {}, timeout=1800, tag='thin-tools')
    for line in log.splitlines():
        if 'FSTEST' in line or 'thin-pool' in line: print('    ' + line.strip()[:200])
    check(ok and 'KERNEL_FAULT' not in log, 'tools: lvm2 thin pool, over-provisioning, snapshot, thin_check/thin_dump, error and queue modes when full',
          markers(log, 'FSTEST', 'DM ', 'FAULT'))
    dtv.unlink(a); dtv.unlink(b)

if 'powerloss' in cases:
    a = WORK / 'thin-p.img'; dtv.raw_disk(a, 160)
    fill = r'''fill() { oct=$(printf '%03o' $(( $1 % 250 + 1 ))); dd if=/dev/zero bs=1M count=4 status=none | tr '\000' "\\$oct" | dd of=$2 bs=1M seek=$(( ($1 % 8) * 4 )) conv=notrunc,fsync status=none; }
'''
    first = HEAD + LVM() + fill + r'''
pvcreate -y /dev/sda >/dev/null && vgcreate vgt /dev/sda >/dev/null && lvcreate -y -L 96M -T vgt/pool >/dev/null 2>&1 && lvcreate -y -V 128M -T vgt/pool -n t1 >/dev/null 2>&1 || fail setup
r=1; while [ $r -le 8 ]; do fill $r /dev/vgt/t1 || fail "fill $r"; r=$((r + 1)); done
lvcreate -y -s vgt/t1 -n s1 >/dev/null 2>&1 && lvchange -ay -K vgt/s1 >/dev/null 2>&1 || fail snapshot
echo "[64] FSTEST BASE DONE"
r=9; while :; do fill $r /dev/vgt/t1 || fail "write $r"; echo "[64] FSTEST ROUND $r DONE"; [ $r = 11 ] && echo "[64] FSTEST WRITING"; r=$((r + 1)); done
'''
    ok, log = dtv.run_script(WORK, first, [a], {}, timeout=900, tag='thin-power0', kill_on='[64] FSTEST WRITING', kill_delay=0.7)
    done = [int(x) for x in re.findall(r'FSTEST ROUND (\d+) DONE', log)]
    good = '[64] FSTEST BASE DONE' in log and '[64] FSTEST WRITING' in log
    check(good, f'powerloss: base rounds and a snapshot, power cut 1 after round {max(done) if done else "-"}', markers(log, 'FSTEST', 'DM '))
    for cut in range(1, args.cuts + 1):
        if not good: break
        last = cut == args.cuts
        # expected byte value per 4 MiB slot from the fsynced rounds; the round in flight may or may not have landed
        expect = {}
        for r in range(1, 9): expect[r % 8] = [r % 250 + 1]
        for r in sorted(done): expect[r % 8] = [r % 250 + 1]
        nxt = (max(done) + 1) if done else 9
        expect[nxt % 8] = expect[nxt % 8] + [nxt % 250 + 1]
        spec = ''.join(f'{s} {" ".join(map(str, v))}\n' for s, v in sorted(expect.items()))
        script = HEAD + LVM() + fill + r'''
vgchange -ay vgt >/tmp/x 2>&1 && lvchange -ay -K vgt/s1 >/dev/null 2>&1 || fail "activation after the cut (lvm2 thin_check): $(cat /tmp/x)"
pat() { [ -e /tmp/p$1 ] || dd if=/dev/zero bs=64k count=1 status=none | tr '\000' "\\$(printf '%03o' $1)" > /tmp/p$1; }
slot_ok() {   # device slot values: every 64 KiB block of the 4 MiB slot is one of the values, whole
  k=0; while [ $k -lt 64 ]; do
    dd if=$1 of=/tmp/blk bs=64k skip=$(($2 * 64 + k)) count=1 status=none
    okv=0; nv=0; del=""; for v in $3; do pat $v; cmp -s /tmp/blk /tmp/p$v && okv=1; del="$del\\$(printf '%03o' $v)"; nv=$((nv + 1)); done
    if [ $okv = 0 ] && [ $nv -gt 1 ] && [ "$(tr -d "$del" < /tmp/blk | wc -c)" = 0 ]; then okv=1; fi   # the round in flight: sectors old or new
    [ $okv = 1 ] || fail "$1 slot $2 block $k is none of: $3"
    k=$((k + 1)); done; }
while read slot values; do slot_ok /dev/vgt/t1 $slot "$values"; done < /nexus/expect
s=0; while [ $s -lt 8 ]; do r=$s; [ $r = 0 ] && r=8; slot_ok /dev/vgt/s1 $s "$((r % 250 + 1))"; s=$((s + 1)); done
echo "[64] FSTEST VERIFIED"
''' + ('vgchange -an vgt >/dev/null && ' + META.strip() + '\nthin_check /dev/mapper/vgt-pool_tmeta >/tmp/x 2>&1 || fail "thin_check: $(cat /tmp/x)"\nsync; echo "[64] FSTEST ALL PASSED"\n' if last else
       f'r={nxt + 1}; while :; do fill $r /dev/vgt/t1 || fail "write $r"; echo "[64] FSTEST ROUND $r DONE"; [ $r = {nxt + 3} ] && echo "[64] FSTEST WRITING"; r=$((r + 1)); done\n')
        ok, log = dtv.run_script(WORK, script, [a], {'nexus/expect': spec.encode()}, timeout=900, tag=f'thin-power{cut}',
                                 kill_on=None if last else '[64] FSTEST WRITING', kill_delay=0.5 + 0.6 * cut)
        done += [int(x) for x in re.findall(r'FSTEST ROUND (\d+) DONE', log)]
        good = '[64] FSTEST VERIFIED' in log and 'FSTEST FAIL' not in log and (ok if last else '[64] FSTEST WRITING' in log)
        check(good, f'powerloss: after cut {cut} lvm2 activates (its thin_check), every fsynced round and the snapshot read back' + (', thin_check clean' if last else f'; cut {cut + 1}'),
              markers(log, 'FSTEST', 'DM '))
    dtv.unlink(a)

if 'hostile' in cases:
    a = WORK / 'thin-h.img'; dtv.raw_disk(a, 128)
    script = HEAD + LVM('thin_check_executable = ""') + r'''
pvcreate -y /dev/sda >/dev/null && vgcreate vgt /dev/sda >/dev/null && lvcreate -y -L 32M -T vgt/pool >/dev/null 2>&1 && lvcreate -y -V 64M -T vgt/pool -n t1 >/dev/null 2>&1 || fail setup
dd if=/dev/urandom of=/dev/vgt/t1 bs=1M count=4 conv=fsync status=none || fail write
echo "[64] FSTEST TMETA $(dmsetup table vgt-pool_tmeta)"
vgchange -an vgt >/dev/null || fail deactivate
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, [a], {}, timeout=900, tag='thin-hostile-0')
    m = re.search(r'FSTEST TMETA 0 \d+ linear 8:0 (\d+)', log)
    if check(ok and m, 'hostile: a pool with data, its metadata location', markers(log, 'FSTEST')):
        base = int(m.group(1)) * 512
        with open(a, 'rb') as f: f.seek(base); sb = f.read(4096)
        mapping_root = struct.unpack_from('<Q', sb, 320)[0]
        for what, offset in (('superblock checksum', base + 100), ('mapping-tree node checksum', base + mapping_root * 4096 + 200)):
            with open(a, 'r+b') as f: f.seek(offset); old = f.read(1); f.seek(offset); f.write(bytes([old[0] ^ 0xFF]))
            script = HEAD + LVM('thin_check_executable = ""') + 'vgchange -ay vgt >/tmp/x 2>&1 && [ -e /dev/mapper/vgt-t1 ] && fail "activated"\nsync; echo "[64] FSTEST ALL PASSED"\n'
            ok, log = dtv.run_script(WORK, script, [a], {}, timeout=600, tag='thin-hostile')
            check(ok and 'metadata refused' in log, f'hostile: a broken {what} -- the kernel refuses the pool', markers(log, 'FSTEST', 'DM '))
            with open(a, 'r+b') as f: f.seek(offset); f.write(old)
        script = HEAD + LVM() + 'vgchange -ay vgt >/tmp/x 2>&1 || fail "repaired pool: $(cat /tmp/x)"\n[ -b /dev/vgt/t1 ] && dd if=/dev/vgt/t1 of=/dev/null bs=1M count=4 status=none || fail read\nvgchange -an vgt >/dev/null\nsync; echo "[64] FSTEST ALL PASSED"\n'
        ok, log = dtv.run_script(WORK, script, [a], {}, timeout=600, tag='thin-hostile-ok')
        check(ok, 'hostile: repaired, the pool activates (with lvm2 thin_check)', markers(log, 'FSTEST', 'DM '))
    dtv.unlink(a)
print(f'SUMMARY thin: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
