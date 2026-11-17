"""md RAID5, RAID6, RAID10 and write-intent bitmaps (arch/x86_64/md_raid.inc) with Debian's mdadm.

  tools      RAID5 (3 disks; mdadm's degraded create + recovery), RAID6 (4 disks, resync), RAID10 (4 disks, near=2):
             data round trip, a failed member (RAID6: two) -- degraded reads and writes, --remove/--add rebuild,
             --stop/--assemble, --detail/--examine; RAID5 with --bitmap=internal --bitmap-chunk=1M: mdadm -X, a member
             failed and removed, writes while degraded mark 2 of 62 chunks, --re-add recovers only those, then 0 dirty.
  writehole  power cuts while writing: RAID5 without a bitmap (the next boot resyncs every row), RAID5 with a bitmap
             (only the dirty chunks), RAID6 and RAID10; the host then checks every row's parity (RAID6: P and Q over
             GF(2^8)) and the RAID10 copies; a dirty RAID5 with a member missing is not started.
  hostile    sets made by mdadm, then changed on the host: RAID5 layout 7, RAID5 chunk of 24 sectors, RAID10 far=2,
             RAID6 with 3 disks, a RAID5 bitmap without its magic -- all refused at boot; a good RAID5 beside them runs.
Usage: test_x64_md_raid.py WORK [--cases tools,writehole,hostile]"""
from pathlib import Path
import argparse, struct, sys, uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='tools,writehole,hostile')
ap.add_argument('--smp', default='2', help='unused (the Debian tools VM has 2 CPUs)')
ap.add_argument('--qmp-port', type=int, default=4738, help='unused')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
results = []
def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True); return ok
def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-3000:]
HEAD = ('#!/bin/dash\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin\nfail() { echo "[64] FSTEST FAIL $*"; cat /proc/mdstat; exit 1; }\nok() { echo "[64] FSTEST PASS $*"; }\n'
        'mkdir -p /run/mdadm /etc/mdadm\nwait_sync() { i=0; while grep -q "resync =\\|recovery =" /proc/mdstat; do i=$((i+1)); [ $i -gt 1500 ] && fail "sync did not finish"; sleep 0.2; done; }\n')
RAID_TYPE = uuid.UUID('a19d880f-05fc-4d3b-a006-743f0f84911e')


# ---- host side: md superblocks, parity ----
def sb_csum(sb):
    max_dev = struct.unpack_from('<I', sb, 220)[0]; size = 256 + max_dev * 2; total = 0
    for i in range(0, size - size % 4, 4):
        if i != 216: total += struct.unpack_from('<I', sb, i)[0]
    if size % 4 == 2: total += struct.unpack_from('<H', sb, size - 2)[0]
    return ((total & 0xFFFFFFFF) + (total >> 32)) & 0xFFFFFFFF
def read_sb(image, base=0):
    with open(image, 'rb') as f: f.seek(base + 8 * 512); return bytearray(f.read(1024))
def mutate_sb(image, base, change):
    with open(image, 'r+b') as f:
        f.seek(base + 8 * 512); sb = bytearray(f.read(1024)); change(sb); struct.pack_into('<I', sb, 216, sb_csum(sb)); f.seek(base + 8 * 512); f.write(sb)
GF_EXP = [0] * 512; GF_LOG = [0] * 256
v = 1
for i in range(255):
    GF_EXP[i] = GF_EXP[i + 255] = v; GF_LOG[v] = i; v <<= 1
    if v & 0x100: v ^= 0x11d
def gf_table(c):   # bytes.translate table for multiplication by c
    return bytes(0 if not (c and x) else GF_EXP[GF_LOG[c] + GF_LOG[x]] for x in range(256))
def xor(a, b): return (int.from_bytes(a, 'little') ^ int.from_bytes(b, 'little')).to_bytes(len(a), 'little')
def row_roles(level, layout, n, stripe):   # independent restatement of Linux raid5_compute_sector for checking
    dd = n - (2 if level == 6 else 1); r = stripe % n
    if level == 5:
        p = dd - r if layout in (0, 2) else r
        return p, None, [((j + 1 if j >= p else j) if layout <= 1 else (p + 1 + j) % n) for j in range(dd)]
    p = n - 1 - r if layout in (0, 2) else r
    if layout <= 1:
        q = p + 1
        if p == n - 1: return p, 0, [j + 1 for j in range(dd)]
        return p, q, [j + 2 if j >= p else j for j in range(dd)]
    return p, (p + 1) % n, [(p + 2 + j) % n for j in range(dd)]
def check_parity(images):
    """Every row of a stopped RAID5/6 array (members in role order): P (and Q) match the data. Returns (rows, bad)."""
    sbs = [read_sb(i) for i in images]
    level, layout, chunk, n = (struct.unpack_from('<I', sbs[0], o)[0] for o in (72, 76, 88, 92))
    size = struct.unpack_from('<Q', sbs[0], 80)[0] // chunk * chunk
    by_role = [None] * n
    for img, sb in zip(images, sbs):
        desc = struct.unpack_from('<I', sb, 160)[0]; role = struct.unpack_from('<H', sb, 256 + 2 * desc)[0]
        by_role[role] = (img, struct.unpack_from('<Q', sb, 128)[0])
    files = [open(img, 'rb') for img, _ in by_role]
    bad = rows = 0; cb = chunk * 512
    try:
        for stripe in range(size // chunk):
            data = []
            for (img, off), f in zip(by_role, files): f.seek((off + stripe * chunk) * 512); data.append(f.read(cb))
            p, q, roles = row_roles(level, layout, n, stripe)
            P = bytes(cb)
            for r in roles: P = xor(P, data[r])
            ok = P == data[p]
            if level == 6:
                d0 = (q + 1) % n; Q = bytes(cb)
                for r in roles: Q = xor(Q, data[r].translate(gf_table(GF_EXP[(r - d0) % n])))
                ok = ok and Q == data[q]
            rows += 1; bad += not ok
    finally:
        for f in files: f.close()
    return rows, bad
def check_copies(images):   # RAID10 near: every logical chunk's copies equal
    sbs = [read_sb(i) for i in images]
    layout, chunk, n = (struct.unpack_from('<I', sbs[0], o)[0] for o in (76, 88, 92)); near = layout & 0xFF
    size = struct.unpack_from('<Q', sbs[0], 80)[0]
    by_role = [None] * n
    for img, sb in zip(images, sbs):
        desc = struct.unpack_from('<I', sb, 160)[0]; by_role[struct.unpack_from('<H', sb, 256 + 2 * desc)[0]] = (img, struct.unpack_from('<Q', sb, 128)[0])
    files = [open(img, 'rb') for img, _ in by_role]; bad = 0; chunks = size // chunk * n // near
    try:
        for c in range(chunks):
            copies = []
            for k in range(near):
                cc = c * near + k; dev, stripe = cc % n, cc // n
                files[dev].seek((by_role[dev][1] + stripe * chunk) * 512); copies.append(files[dev].read(chunk * 512))
            bad += any(x != copies[0] for x in copies)
    finally:
        for f in files: f.close()
    return chunks, bad
def resync(image): return struct.unpack_from('<Q', read_sb(image), 208)[0]

if 'tools' in cases:
    disks = [WORK / f't{i}.img' for i in range(4)]
    for d in disks: dtv.raw_disk(d, 64)
    script = HEAD + r'''
dd if=/dev/urandom of=/tmp/a bs=1M count=16 status=none; dd if=/dev/urandom of=/tmp/b bs=1M count=4 status=none
both() { cmp -n 4194304 /tmp/a /dev/md0 && cmp -n 4194304 -i 0:4194304 /tmp/b /dev/md0 && cmp -n 8388608 -i 8388608:8388608 /tmp/a /dev/md0; }
yes | mdadm --create /dev/md0 --level=5 --raid-devices=3 --run /dev/sda /dev/sdb /dev/sdc >/tmp/x 2>&1 || fail "create RAID5: $(cat /tmp/x)"
grep -q "active raid5" /proc/mdstat && grep -q "level 5, 512k chunk, algorithm 2" /proc/mdstat || fail "RAID5 mdstat"
wait_sync; grep -q "\[3/3\] \[UUU\]" /proc/mdstat || fail "RAID5 initial recovery"
dd if=/tmp/a of=/dev/md0 bs=1M conv=fsync status=none && cmp -n 16777216 /tmp/a /dev/md0 || fail "RAID5 data"
mdadm /dev/md0 --fail /dev/sdb >/dev/null && grep -q "\[3/2\] \[U_U\]" /proc/mdstat || fail "RAID5 --fail"
cmp -n 16777216 /tmp/a /dev/md0 || fail "RAID5 degraded read"
dd if=/tmp/b of=/dev/md0 bs=1M seek=4 conv=fsync,notrunc status=none && both || fail "RAID5 degraded write"
mdadm /dev/md0 --remove /dev/sdb >/dev/null && mdadm /dev/md0 --add /dev/sdb >/dev/null || fail "RAID5 remove/add"
wait_sync; grep -q "\[3/3\] \[UUU\]" /proc/mdstat && both || fail "RAID5 rebuild"
mdadm --stop /dev/md0 >/dev/null && mdadm --assemble /dev/md0 /dev/sda /dev/sdb /dev/sdc >/dev/null && both || fail "RAID5 stop/assemble"
mdadm --detail /dev/md0 | grep -q "Raid Level : raid5" && mdadm --detail /dev/md0 | grep -q "Layout : left-symmetric" || fail "RAID5 detail"
mdadm --stop /dev/md0 >/dev/null; mdadm --zero-superblock /dev/sda /dev/sdb /dev/sdc
ok "RAID5: create (recovery), degraded read/write, rebuild, stop/assemble, detail"
yes | mdadm --create /dev/md0 --level=6 --raid-devices=4 --run /dev/sda /dev/sdb /dev/sdc /dev/sdd >/tmp/x 2>&1 || fail "create RAID6: $(cat /tmp/x)"
wait_sync; grep -q "level 6, 512k chunk, algorithm 2 \[4/4\] \[UUUU\]" /proc/mdstat || fail "RAID6 resync"
dd if=/tmp/a of=/dev/md0 bs=1M conv=fsync status=none && cmp -n 16777216 /tmp/a /dev/md0 || fail "RAID6 data"
mdadm /dev/md0 --fail /dev/sda --fail /dev/sdc >/dev/null && grep -q "\[4/2\] \[_U_U\]" /proc/mdstat || fail "RAID6 two failures"
cmp -n 16777216 /tmp/a /dev/md0 || fail "RAID6 read with two members missing"
dd if=/tmp/b of=/dev/md0 bs=1M seek=4 conv=fsync,notrunc status=none && both || fail "RAID6 write with two members missing"
mdadm /dev/md0 --remove /dev/sda /dev/sdc >/dev/null && mdadm /dev/md0 --add /dev/sda /dev/sdc >/dev/null || fail "RAID6 remove/add"
wait_sync; grep -q "\[4/4\] \[UUUU\]" /proc/mdstat && both || fail "RAID6 rebuild of two members"
mdadm /dev/md0 --fail /dev/sdb --fail /dev/sdd >/dev/null && both || fail "RAID6 read from the rebuilt pair"
mdadm --stop /dev/md0 >/dev/null; mdadm --zero-superblock /dev/sda /dev/sdb /dev/sdc /dev/sdd
ok "RAID6: resync, reads/writes with two members missing, two rebuilds, the rebuilt members alone serve the data"
yes | mdadm --create /dev/md0 --level=10 --raid-devices=4 --run /dev/sda /dev/sdb /dev/sdc /dev/sdd >/tmp/x 2>&1 || fail "create RAID10: $(cat /tmp/x)"
wait_sync; grep -q "512K chunks 2 near-copies \[4/4\] \[UUUU\]" /proc/mdstat || fail "RAID10 mdstat"
dd if=/tmp/a of=/dev/md0 bs=1M conv=fsync status=none && cmp -n 16777216 /tmp/a /dev/md0 || fail "RAID10 data"
mdadm /dev/md0 --fail /dev/sda --fail /dev/sdc >/dev/null && cmp -n 16777216 /tmp/a /dev/md0 || fail "RAID10 one copy of each lost"
dd if=/tmp/b of=/dev/md0 bs=1M seek=4 conv=fsync,notrunc status=none && both || fail "RAID10 degraded write"
mdadm /dev/md0 --remove /dev/sda /dev/sdc >/dev/null && mdadm /dev/md0 --add /dev/sda /dev/sdc >/dev/null || fail "RAID10 remove/add"
wait_sync; grep -q "\[4/4\] \[UUUU\]" /proc/mdstat || fail "RAID10 rebuild"
mdadm /dev/md0 --fail /dev/sdb --fail /dev/sdd >/dev/null && both || fail "RAID10 the rebuilt copies alone"
mdadm --detail /dev/md0 | grep -q "Layout : near=2" || fail "RAID10 detail"
mdadm --stop /dev/md0 >/dev/null; mdadm --zero-superblock /dev/sda /dev/sdb /dev/sdc /dev/sdd
ok "RAID10 near=2: data, two members lost, rebuild, the rebuilt copies serve the data"
yes | mdadm --create /dev/md0 --level=5 --raid-devices=3 --bitmap=internal --bitmap-chunk=1M --run /dev/sda /dev/sdb /dev/sdc >/tmp/x 2>&1 || fail "create RAID5+bitmap: $(cat /tmp/x)"
wait_sync; grep -q "bitmap: .* 1024KB chunk" /proc/mdstat || fail "bitmap mdstat"
mdadm --detail /dev/md0 | grep -q "Intent Bitmap : Internal" && mdadm --detail /dev/md0 | grep -q "Consistency Policy : bitmap" || fail "bitmap detail"
dd if=/tmp/a of=/dev/md0 bs=1M conv=fsync status=none; sleep 1
mdadm -X /dev/sda | grep -q "62 bits (chunks), 0 dirty" || fail "bitmap after writes: $(mdadm -X /dev/sda)"
mdadm /dev/md0 --fail /dev/sdc >/dev/null && mdadm /dev/md0 --remove /dev/sdc >/dev/null || fail "fail/remove"
dd if=/tmp/b of=/dev/md0 bs=1M seek=8 conv=fsync,notrunc status=none; sleep 1
mdadm -X /dev/sda | grep -q "62 bits (chunks), 2 dirty" || fail "bitmap while degraded: $(mdadm -X /dev/sda)"
mdadm /dev/md0 --re-add /dev/sdc >/tmp/x 2>&1 || fail "re-add: $(cat /tmp/x)"
wait_sync; sleep 1; grep -q "\[3/3\] \[UUU\]" /proc/mdstat || fail "re-add recovery"
mdadm -X /dev/sdc | grep -q "0 dirty" || fail "bitmap after re-add: $(mdadm -X /dev/sdc)"
mdadm /dev/md0 --fail /dev/sda >/dev/null && cmp -n 4194304 -i 0:8388608 /tmp/b /dev/md0 && cmp -n 8388608 /tmp/a /dev/md0 || fail "data through the re-added member"
mdadm --stop /dev/md0 >/dev/null
ok "bitmap: mdadm -X, 2 dirty chunks while degraded, --re-add recovers only them, then clean"
sync; echo "[64] FSTEST ALL PASSED"
'''
    ok, log = dtv.run_script(WORK, script, disks, {}, timeout=1800, tag='raid-tools')
    for line in log.splitlines():
        if 'FSTEST' in line: print('    ' + line.strip()[:200])
    check(ok and 'KERNEL_FAULT' not in log and "re-added: recovery of the bitmap's dirty chunks" in log and 'recovery done (bitmap: dirty chunks only)' in log,
          'tools: Debian mdadm -- RAID5, RAID6, RAID10 (degraded I/O, rebuilds), bitmap re-add of only the dirty chunks', markers(log, 'FSTEST', '[64] MD', 'FAULT'))
    for d in disks: dtv.unlink(d)

if 'writehole' in cases:
    for name, create, members, cut in (
            ('raid5', '--level=5 --raid-devices=3 --chunk=64', 3, 1.3),
            ('raid5-bitmap', '--level=5 --raid-devices=3 --chunk=64 --bitmap=internal --bitmap-chunk=1M', 3, 1.1),
            ('raid6', '--level=6 --raid-devices=4 --chunk=64', 4, 1.5),
            ('raid10', '--level=10 --raid-devices=4 --chunk=64', 4, 1.2)):
        disks = [WORK / f'w{i}.img' for i in range(members)]
        for d in disks: dtv.raw_disk(d, 40)
        devs = ' '.join(f'/dev/sd{chr(97 + i)}' for i in range(members))
        region = 4 if 'bitmap' in name else 48   # MiB written in the loop (bitmap: 4 of the 1 MiB chunks)
        script = HEAD + rf'''
yes | mdadm --create /dev/md0 {create} --run {devs} >/tmp/x 2>&1 || fail "create: $(cat /tmp/x)"
wait_sync; sleep 1
dd if=/dev/urandom of=/tmp/r bs=1M count=4 status=none
r=0; while :; do dd if=/tmp/r of=/dev/md0 bs=1M seek=$(( (r * 4) % {region} )) count=4 conv=notrunc,fsync status=none || fail write; r=$((r + 1)); [ $r = 3 ] && echo "[64] FSTEST WRITING"; done
'''
        ok, log = dtv.run_script(WORK, script, disks, {}, timeout=900, tag=f'wh-{name}-1', kill_on='[64] FSTEST WRITING', kill_delay=cut)
        dirty = all(resync(d) == 0 for d in disks)
        check('[64] FSTEST WRITING' in log and dirty, f'writehole {name}: power cut while writing, the superblocks dirty', markers(log, 'FSTEST', '[64] MD'))
        if name == 'raid5':   # a member missing as well: not started (Linux: cannot start dirty degraded array)
            ok, log = dtv.run_script(WORK, HEAD + 'grep -q "^md0 : active" /proc/mdstat && fail "started"\nsync; echo "[64] FSTEST ALL PASSED"\n', disks[:2], {}, timeout=600, tag='wh-degraded')
            check(ok and 'cannot start dirty degraded array' in log, 'writehole raid5: dirty with a member missing -- not started', markers(log, 'FSTEST', '[64] MD'))
        script = HEAD + r'''
grep -q "^md0 : active" /proc/mdstat || fail "the kernel did not assemble md0 at boot"
wait_sync; sleep 1; mdadm --stop /dev/md0 >/dev/null || fail stop
sync; echo "[64] FSTEST ALL PASSED"
'''
        ok, log = dtv.run_script(WORK, script, disks, {}, timeout=900, tag=f'wh-{name}-2')
        want = 'resync done (bitmap: dirty chunks only)' if 'bitmap' in name else 'resync done'
        rows, bad = check_copies(disks) if name == 'raid10' else check_parity(disks)
        check(ok and 'md0 resync started' in log and want in log and not bad and all(resync(d) == 2**64 - 1 for d in disks),
              f'writehole {name}: the boot resync{" of the dirty chunks" if "bitmap" in name else ""}, then all {rows} {"chunks have equal copies" if name == "raid10" else "rows have matching parity"}',
              (bad, markers(log, 'FSTEST', '[64] MD')))
        for d in disks: dtv.unlink(d)

if 'hostile' in cases:
    disk = WORK / 'hostile.img'
    sets = [('layout7', 5, 3), ('chunk24', 5, 3), ('far2', 10, 4), ('raid6x3', 6, 4), ('nobitmap', 5, 3), ('good', 5, 3)]
    parts = dtv.gpt_disk(disk, [(8, RAID_TYPE, uuid.uuid4(), f'p{i}') for i in range(sum(n for _, _, n in sets))])
    lines, at = [], 0
    for i, (name, level, n) in enumerate(sets):
        devs = ' '.join(f'/dev/sda{at + k + 1}' for k in range(n)); at += n
        extra = ' --layout=n2' if level == 10 else ' --bitmap=internal --bitmap-chunk=1M' if name == 'nobitmap' else ''
        lines.append(f'yes | mdadm --create /dev/md{i} --level={level} --raid-devices={n} --chunk=64{extra} --assume-clean --run {devs} >/tmp/x 2>&1 || fail "{name}: $(cat /tmp/x)"\nmdadm --stop /dev/md{i} >/dev/null || fail "stop {name}"')
    ok, log = dtv.run_script(WORK, HEAD + '\n'.join(lines) + '\nsync; echo "[64] FSTEST ALL PASSED"\n', [disk], {}, timeout=900, tag='raid-hostile-1')
    if check(ok, 'hostile: six sets made by mdadm', markers(log, 'FSTEST', '[64] MD')):
        at = 0
        for name, level, n in sets:
            for k in range(n):
                base = parts[at + k] * 512
                if name == 'layout7': mutate_sb(disk, base, lambda sb: struct.pack_into('<I', sb, 76, 7))
                if name == 'chunk24': mutate_sb(disk, base, lambda sb: struct.pack_into('<I', sb, 88, 24))
                if name == 'far2': mutate_sb(disk, base, lambda sb: struct.pack_into('<I', sb, 76, 0x201))
                if name == 'raid6x3': mutate_sb(disk, base, lambda sb: struct.pack_into('<I', sb, 92, 3))
                if name == 'nobitmap':
                    off = struct.unpack_from('<i', read_sb(disk, base), 96)[0]
                    with open(disk, 'r+b') as f: f.seek(base + (8 + off) * 512); f.write(b'XXXX')
            at += n
        ok, log = dtv.run_script(WORK, HEAD + '[ "$(grep -c " : active" /proc/mdstat)" = 1 ] && grep -q " : active raid5" /proc/mdstat || fail "mdstat"\ncat /proc/mdstat\nsync; echo "[64] FSTEST ALL PASSED"\n',
                                 [disk], {}, timeout=600, tag='raid-hostile-2')
        refused = log.count('unsupported RAID level, member count, layout or chunk size')
        check(ok and refused >= 4 and 'bitmap superblock refused' in log, f'hostile: layout 7, chunk 24, far=2, RAID6 of 3, a broken bitmap refused ({refused} refusals); the good set runs',
              markers(log, 'FSTEST', '[64] MD'))
    dtv.unlink(disk)
print(f'SUMMARY md-raid: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
