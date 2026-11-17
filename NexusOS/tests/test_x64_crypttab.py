"""Secondary LUKS volumes from /etc/crypttab, the configured keyboard layout at the passphrase prompts, and hibernation
with an encrypted root -- volumes made by Debian's cryptsetup (tests/debian_tools_vm.py), opened by NexusOS at boot.

  keymap     LUKS2 root, passphrase 'Grüße zY-42é' (UTF-8) typed on a German keyboard: rd.vconsole.keymap=de-latin1
             selects the layout (kbd alias), so z/y, ü, ß, '-' and the dead acute come out as on Linux/Aurora.
  crypttab   encrypted ext4 root whose /etc/crypttab names: data1 (keyfile /etc/keys/data1.key 0400, discard),
             data2 (passphrase typed in the layout /etc/vconsole.conf names: KEYMAP=de-latin1; one wrong try),
             data3 (keyfile with mode 0644: refused), data4 (absent UUID, nofail), data5 (keyfile, readonly),
             data6 (noauto) and the root's own line (skipped); /etc/fstab mounts /dev/mapper/data1 after them.
             arch/x86_64/crypttab_user.c checks all that and writes files; then cryptsetup round trip: Debian's
             cryptsetup opens data1 (keyfile) and data2 (passphrase) again and finds those files, fsck.vfat clean.
  hibernate  encrypted FAT32 root with /nexus/hiber.img: the session hibernates into it (allowed: the area is the
             encrypted root); the patterned memory's marker is not on the raw disk; the next boot is refused three
             passphrases and powers off (the image untouched); the boot after unlocks (one wrong try), finds the image
             on the unlocked root and restores it; the session checks its 16 MiB of memory ("RESUMED OK").
Every boot runs on 1 and 2 CPUs (--smp).

Usage: test_x64_crypttab.py WORK [--cases keymap,crypttab,hibernate] [--smp 1,2] [--qmp-port 4734] [--keep]"""
from pathlib import Path
import argparse, importlib.util, os, re, secrets, shutil, struct, subprocess, sys, time, uuid, zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
sys.path.insert(0, str(ROOT / 'tests'))
import debian_tools_vm as dtv   # noqa: E402
import qmpkeys                  # noqa: E402
QEMU, CPU, ZIG = dtv.QEMU, dtv.CPU, dtv.ZIG
PASS_US = 'Correct horse battery 42'
PASS_DE = 'Grüße zY-42é'            # the keymap root
PASS_DE2 = 'Daten-Grüße 2024'       # crypttab data2, prompt in the vconsole.conf layout
LUKS_TYPE = uuid.UUID('ca7d7ccb-63ed-4c53-861c-1742536059cc')
LINUX_DATA = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--cases', default='keymap,crypttab,hibernate')
ap.add_argument('--smp', default='1,2')
ap.add_argument('--qmp-port', type=int, default=4734)
ap.add_argument('--keep', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
cases = [c for c in args.cases.split(',') if c]
results = []


def check(ok, what, detail=''):
    results.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + ('' if ok or not detail else '\n     ' + str(detail)[-3000:]), flush=True)
    return ok


def gpt_disk(path, parts):
    """parts: [(MiB, type uuid, partition uuid, name)] from LBA 2048, each a whole number of 4 KiB units"""
    total = (sum(p[0] for p in parts) + 2) * 2048; at = 2048; entries = b''
    for mib, kind, guid, name in parts:
        entries += kind.bytes_le + guid.bytes_le + struct.pack('<QQQ', at, at + mib * 2048 - 1, 0) + name.encode('utf-16-le').ljust(72, b'\0')
        at += mib * 2048
    entries = entries.ljust(128 * 128, b'\0'); disk_guid = uuid.uuid4()
    def header(mine, other, table):
        h = bytearray(struct.pack('<8sIII4xQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, mine, other, 34, total - 34, disk_guid.bytes_le, table, 128, 128, zlib.crc32(entries)))
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:92]))); return bytes(h).ljust(512, b'\0')
    mbr = bytearray(512); struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\0\x02\0', 0xEE, b'\xff\xff\xff', 1, total - 1); mbr[510:] = b'\x55\xaa'
    path.unlink(missing_ok=True)
    with open(path, 'wb') as f:
        f.truncate(total * 512); f.write(mbr); f.write(header(1, total - 1, 2)); f.write(entries)
        f.seek((total - 33) * 512); f.write(entries); f.write(header(total - 1, 1, total - 33))
    dtv.punch(path)


def build(source):
    out = WORK / (Path(source).stem + '.elf')
    subprocess.run([str(ZIG), 'cc', '-target', 'x86_64-linux-musl', '-static', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'arch/x86_64' / source), '-o', str(out)], check=True)
    return out.read_bytes()


def ext4_root(name, files):
    """apps64's ext4 root volume plus `files` {path: (bytes, mode)}, as a raw partition image"""
    ext4lib = dtv.load('ct_ext4lib_' + name, ROOT / 'userland/ext4lib.py')
    staged = WORK / f'{name}-staged.img'; shutil.copyfile(OUT / 'apps64-ext4.img', staged)
    fs = ext4lib.Writer(str(staged))
    try:
        for path, (data, mode) in files.items(): fs.put_bytes(data, path, mode, 0, 0)
    finally: fs.close()
    raw = WORK / f'root-{name}.raw'
    with open(staged, 'rb') as a, open(raw, 'wb') as b: a.seek(32768 * 512); b.write(a.read(98304 * 512))
    dtv.unlink(staged); dtv.punch(raw)
    return raw


def fat_root(name, files, mib):
    fatimage = dtv.load('ct_fatimage', ROOT / 'tests/fatimage.py')
    vol = fatimage.FatImage.create(mib << 20, 32, 'NEXUSHIB', cluster_sectors=4)
    for path, data in files.items(): vol.put(path, data)
    raw = WORK / f'root-{name}.raw'; raw.write_bytes(vol.finish()); dtv.punch(raw)
    return raw


class Boot:
    """one VM boot of a target disk; wait()/count() poll the serial log"""
    def __init__(self, tag, target, cmdline, cpus):
        self.serial = WORK / f'{tag}.log'; self.serial.unlink(missing_ok=True); self.log = ''
        command = [QEMU, '-machine', 'q35', '-accel', 'whpx,kernel-irqchip=off', '-cpu', CPU, '-smp', str(cpus), '-m', '2048', '-display', 'none',
                   '-vga', 'none', '-device', 'virtio-vga', '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
                   '-drive', f'if=none,id=root,format=raw,file={target}', '-device', 'nvme,serial=crypttab,drive=root',
                   '-serial', f'file:{self.serial}', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-no-reboot',
                   '-qmp', f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off', '-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
        self.vm = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.q = qmpkeys.Qmp(args.qmp_port)
    def read(self):
        self.log = self.serial.read_text(encoding='utf-8', errors='replace') if self.serial.exists() else ''; return self.log
    def count(self, text, n=1, seconds=300):
        end = time.time() + seconds
        while time.time() < end:
            if self.read().count(text) >= n: return True
            if self.vm.poll() is not None: return self.read().count(text) >= n
            time.sleep(.2)
        return False
    def exit(self, seconds=180):
        try: self.vm.wait(seconds)
        except subprocess.TimeoutExpired: pass
        if self.vm.poll() is None: self.vm.kill(); self.vm.wait()
        self.read()
        return self.vm.returncode
    def faults(self): return [w for w in ('KERNEL_FAULT', '[64] FAIL:', 'USER_FAULT', 'PANIC') if w in self.log]


def markers(log, *words): return '\n'.join(l for l in log.splitlines() if any(w in l for w in words))[-2500:]


# ---------------- volumes ----------------
cryptroot_init = build('cryptroot_user.c') if {'keymap', 'hibernate'} & set(cases) else None
crypttab_init = build('crypttab_user.c') if 'crypttab' in cases else None
key1, key3 = secrets.token_bytes(64), secrets.token_bytes(64)
g = {name: uuid.uuid4() for name in ('keymap', 'root', 'data1', 'data2', 'data3', 'data5', 'hibernate')}
u = {name: uuid.uuid4() for name in ('keymap', 'root', 'data1', 'data3', 'data4', 'hibernate')}
disks, sources, script = [], [], ['#!/bin/dash', 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1', 'fail() { echo "[64] FSTEST FAIL $*"; exit 1; }',
                                  'mkdir -p /run/cryptsetup /mnt/v', 'FAST="--type luks2 --pbkdf pbkdf2 --pbkdf-force-iterations 1000"']
def disk_letter(): return chr(97 + len(disks))   # the targets are on the first AHCI ports
def fill(dev, source, options, keyfile, luks_uuid, tag):
    script.extend([f'cryptsetup luksFormat -q {options} --uuid {luks_uuid} --key-file {keyfile} {dev} || fail "luksFormat {tag}"',
                   f'cryptsetup open --key-file {keyfile} {dev} {tag} || fail "open {tag}"',
                   f'dd if={source} of=/dev/mapper/{tag} bs=1M conv=fsync status=none || fail "dd {tag}"',
                   f'cryptsetup close {tag} || fail "close {tag}"', f'echo "[64] FSTEST PASS prepared {tag}"'])
def vfat(dev, options, keyfile, luks_uuid, tag, text):
    script.extend([f'cryptsetup luksFormat -q {options} {f"--uuid {luks_uuid}" if luks_uuid else ""} --key-file {keyfile} {dev} || fail "luksFormat {tag}"',
                   f'cryptsetup open --key-file {keyfile} {dev} {tag} || fail "open {tag}"',
                   f'mkfs.vfat -n {tag.upper()} /dev/mapper/{tag} >/dev/null || fail "mkfs {tag}"',
                   f'mount -t vfat /dev/mapper/{tag} /mnt/v || fail "mount {tag}"', f'printf "{text}\\n" > /mnt/v/debian-wrote.txt && sync',
                   f'umount /mnt/v || fail "umount {tag}"', f'cryptsetup close {tag} || fail "close {tag}"', f'echo "[64] FSTEST PASS prepared {tag}"'])

files = {'nexus/key1': key1, 'nexus/key3': key3, 'nexus/pass-us': PASS_US.encode(), 'nexus/pass-de': PASS_DE.encode(), 'nexus/pass-de2': PASS_DE2.encode()}
targets = {}
if 'keymap' in cases:
    targets['keymap'] = WORK / 'target-keymap.img'; gpt_disk(targets['keymap'], [(96, LUKS_TYPE, g['keymap'], 'keymap')])
    raw = ext4_root('keymap', {'sbin/init': (cryptroot_init, 0o755), 'nexus/init-as-root': (b'keymap\n', 0o644)})
    dev = f'/dev/sd{disk_letter()}'; disks.append(targets['keymap']); src = f'/dev/sd{chr(97 + 6 - 1 - len(sources))}'
    sources.append((raw, src)); fill(dev + '1', src, '$FAST', '/nexus/pass-de', u['keymap'], 'keymap')
if 'crypttab' in cases:
    targets['crypttab'] = WORK / 'target-crypttab.img'
    gpt_disk(targets['crypttab'], [(96, LUKS_TYPE, g['root'], 'root'), (40, LINUX_DATA, g['data1'], 'data1'), (40, LINUX_DATA, g['data2'], 'data2'),
                                   (24, LINUX_DATA, g['data3'], 'data3'), (24, LINUX_DATA, g['data5'], 'data5')])
    crypttab = (f'# NexusOS crypttab test\ndata1 UUID={u["data1"]} /etc/keys/data1.key luks,discard\n'
                f'data2\tPARTUUID={g["data2"]}\tnone\tluks,tries=2\n'
                f'data3 UUID={u["data3"]} /etc/keys/data3.key luks\n'
                f'data4 UUID={u["data4"]} none luks,nofail\n'
                f'data5 PARTUUID={g["data5"]} /etc/keys/data1.key luks,readonly\n'
                f'data6 /dev/sdz9 none luks,noauto\n'
                f'cryptroot UUID={u["root"]} none luks\n')
    raw = ext4_root('crypttab', {'sbin/init': (crypttab_init, 0o755), 'nexus/init-as-root': (b'crypttab\n', 0o644),
                                 'etc/crypttab': (crypttab.encode(), 0o644), 'etc/fstab': (b'/dev/mapper/data1 /mnt/data1 vfat defaults 0 0\n', 0o644),
                                 'etc/vconsole.conf': (b'KEYMAP=de-latin1\n', 0o644), 'etc/keys/data1.key': (key1, 0o400),
                                 'etc/keys/data3.key': (key3, 0o644), 'mnt/data1/.mountpoint': (b'', 0o644)})
    dev = f'/dev/sd{disk_letter()}'; disks.append(targets['crypttab']); src = f'/dev/sd{chr(97 + 6 - 1 - len(sources))}'
    sources.append((raw, src))
    fill(dev + '1', src, '$FAST', '/nexus/pass-us', u['root'], 'root')
    vfat(dev + '2', '$FAST', '/nexus/key1', u['data1'], 'data1', 'written by cryptsetup data1')
    vfat(dev + '3', '$FAST', '/nexus/pass-de2', None, 'data2', 'written by cryptsetup data2')
    script.extend([f'cryptsetup luksFormat -q $FAST --uuid {u["data3"]} --key-file /nexus/key3 {dev}4 || fail "luksFormat data3"'])
    vfat(dev + '5', '$FAST', '/nexus/key1', None, 'data5', 'written by cryptsetup data5')
if 'hibernate' in cases:
    targets['hibernate'] = WORK / 'target-hibernate.img'; gpt_disk(targets['hibernate'], [(240, LUKS_TYPE, g['hibernate'], 'hibernate')])
    # 8.3 directory entries as Linux vfat or Windows write them (hibernate.c finds NEXUS\HIBER.IMG by its 8.3 name)
    raw = fat_root('hibernate', {'SBIN/INIT': cryptroot_init, 'NEXUS/init-as-root': b'hibernate\n', 'NEXUS/cryptroot-hibernate': b'1\n',
                                 'HOME/NEXUS/.keep': b'keep\n', 'NEXUS/HIBER.IMG': bytes(160 << 20)}, 224)
    dev = f'/dev/sd{disk_letter()}'; disks.append(targets['hibernate']); src = f'/dev/sd{chr(97 + 6 - 1 - len(sources))}'
    sources.append((raw, src))
    fill(dev + '1', src, '--type luks2 --pbkdf argon2id --pbkdf-memory 65536 --pbkdf-force-iterations 4', '/nexus/pass-us', u['hibernate'], 'hibernate')
script += ['sync', 'echo "[64] FSTEST ALL PASSED"']
# AHCI ports: the targets first (sda...), the root images from the last port down (sdf, sde, ...)
ports = disks + [None] * (6 - len(disks) - len(sources)) + [raw for raw, _ in reversed(sources)]
blank = WORK / 'blank.img'
if None in ports:
    blank.write_bytes(b''); os.truncate(blank, 1 << 20); ports = [p or blank for p in ports]
ok, log = dtv.run_script(WORK, '\n'.join(script) + '\n', ports, files, tag='prepare')
for line in log.splitlines():
    if 'FSTEST' in line: print('    ' + line.strip()[:160])
check(ok, f'cryptsetup formatted and filled the volumes ({", ".join(targets)})', markers(log, 'FSTEST', 'cryptsetup', 'rror'))
for raw, _ in sources: dtv.unlink(raw)
dtv.unlink(blank)
if not ok: print(f'SUMMARY crypttab: {sum(results)}/{len(results)} passed'); sys.exit(1)

# ---------------- boots ----------------
for cpus in [int(x) for x in args.smp.split(',')]:
    if 'keymap' in cases:
        b = Boot(f'keymap-smp{cpus}', targets['keymap'], f'cryptroot=PARTUUID={g["keymap"]} rd.vconsole.keymap=de-latin1', cpus)
        good = b.count('CRYPTROOT PROMPT', 1, 240)
        if good: qmpkeys.type_text(b.q, 'not the passphrase\n'); good = b.count('Wrong passphrase', 1) and b.count('CRYPTROOT PROMPT', 2)
        if good: qmpkeys.type_layout(b.q, PASS_DE + '\n', 'de'); good = b.count('CRYPTROOT ROOT OK', 1, 600)
        b.exit()
        check(good and '[64] KEYMAP de (German, from rd.vconsole.keymap)' in b.log and 'CRYPTROOT UNLOCKED' in b.log and not b.faults(),
              f'keymap smp{cpus}: rd.vconsole.keymap=de-latin1 -> German layout; "{PASS_DE}" typed on German keys opens the LUKS2 root',
              (b.faults(), markers(b.log, 'CRYPTROOT', 'KEYMAP')))
    if 'crypttab' in cases:
        b = Boot(f'crypttab-smp{cpus}', targets['crypttab'], f'cryptroot=PARTUUID={g["root"]}', cpus)
        good = b.count('CRYPTROOT PROMPT', 1, 240)
        if good: qmpkeys.type_text(b.q, PASS_US + '\n'); good = b.count('CRYPTTAB data2 PROMPT', 1, 600)
        if good: qmpkeys.type_layout(b.q, 'Daten-Grüße 2025\n', 'de'); good = b.count('CRYPTTAB data2 Wrong passphrase', 1) and b.count('CRYPTTAB data2 PROMPT', 2)
        if good: qmpkeys.type_layout(b.q, PASS_DE2 + '\n', 'de'); good = b.count('[64] CRYPTTAB TEST ALL PASSED', 1, 600)
        b.exit()
        log = b.log
        check(good and not b.faults(), f'crypttab smp{cpus}: crypttab_user.c checks passed', (b.faults(), markers(log, 'CRYPTTAB', 'CRYPTROOT', 'FSTAB', 'KEYMAP')))
        expect = {'data1 UNLOCKED with its keyfile': 'data1 opened from its keyfile',
                  'CRYPTTAB data1 ready: /dev/mapper/data1 (discard)': 'data1 discard option',
                  'KEYMAP de (German, from the system configuration)': 'data2 prompt in the /etc/vconsole.conf layout',
                  'data3 FAILED: keyfile /etc/keys/data3.key is readable by group or others (mode 0644): refused': 'data3 keyfile mode refused',
                  f'data4 not present (nofail): UUID={u["data4"]}': 'data4 nofail',
                  'data5 ready: /dev/mapper/data5 (read-only)': 'data5 read-only',
                  'data6 skipped (noauto)': 'data6 noauto',
                  'CRYPTTAB cryptroot skipped': "the root's own line skipped"}
        missing = [what for text, what in expect.items() if text not in log]
        check(not missing, f'crypttab smp{cpus}: kernel messages for every crypttab line', (missing, markers(log, 'CRYPTTAB', 'KEYMAP')))
        t_nofail = re.search(r'data4 not present', log)
        check(t_nofail is not None and 'no LUKS volume UUID=' + str(u['data4']) not in log, f'crypttab smp{cpus}: nofail did not wait for or fail on the absent volume')
    if 'hibernate' in cases:
        b = Boot(f'hibernate-smp{cpus}-1', targets['hibernate'], f'cryptroot=PARTUUID={g["hibernate"]}', cpus)
        good = b.count('CRYPTROOT PROMPT', 1, 240)
        if good: qmpkeys.type_text(b.q, 'wrong one\n'); good = b.count('CRYPTROOT PROMPT', 2)
        if good: qmpkeys.type_text(b.q, PASS_US + '\n'); good = b.count('PM: hibernation image written', 1, 900)
        code = b.exit(300)
        check(good and 'CRYPTROOT HIBERNATE writing' in b.log and 'hibernation refused' not in b.log and not b.faults(),
              f'hibernate smp{cpus}: the session hibernated into /nexus/hiber.img on the encrypted root (exit {code})', (b.faults(), markers(b.log, 'PM:', 'CRYPTROOT')))
        plain = False
        with open(targets['hibernate'], 'rb') as f:
            while chunk := f.read(4 << 20):
                if b'NEXUS-HIBER-PLAINTEXT' in chunk: plain = True; break
        check(not plain, f'hibernate smp{cpus}: the image is encrypted at rest (no memory marker on the raw disk)')
        b = Boot(f'hibernate-smp{cpus}-2', targets['hibernate'], f'cryptroot=PARTUUID={g["hibernate"]}', cpus)
        good = True
        for attempt in range(3):
            good = good and b.count('CRYPTROOT PROMPT', attempt + 1, 240)
            if good: qmpkeys.type_text(b.q, f'wrong {attempt}\n'); good = b.count('Wrong passphrase', attempt + 1)
        good = good and b.count('CRYPTROOT POWER OFF', 1, 120)
        b.exit()
        check(good and 'restored from the hibernation image' not in b.log and 'hibernation image found' not in b.log and not b.faults(),
              f'hibernate smp{cpus}: three wrong passphrases at resume power off without touching the image', (b.faults(), markers(b.log, 'PM:', 'CRYPTROOT')))
        b = Boot(f'hibernate-smp{cpus}-3', targets['hibernate'], f'cryptroot=PARTUUID={g["hibernate"]}', cpus)
        good = b.count('CRYPTROOT PROMPT', 1, 240)
        if good: qmpkeys.type_text(b.q, 'still wrong\n'); good = b.count('CRYPTROOT PROMPT', 2)
        if good: qmpkeys.type_text(b.q, PASS_US + '\n'); good = b.count('CRYPTROOT RESUMED OK', 1, 900) and b.count('CRYPTROOT ROOT OK', 1, 300)
        b.exit()
        check(good and 'PM: restored from the hibernation image' in b.log and 'looking for a hibernation image on the unlocked root' in b.log and
              'CRYPTROOT HIBERNATE writing' not in b.log and not b.faults(),   # the restored session went on, it did not start over
              f'hibernate smp{cpus}: after the unlock the image was found on the root and restored; the session resumed with its memory intact',
              (b.faults(), markers(b.log, 'PM:', 'CRYPTROOT')))

if 'crypttab' in cases and all(results):   # cryptsetup round trip: Debian opens what NexusOS opened and wrote
    verify = ['#!/bin/dash', 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1', 'fail() { echo "[64] FSTEST FAIL $*"; exit 1; }', 'mkdir -p /run/cryptsetup /mnt/v',
              'cryptsetup open --key-file /nexus/key1 /dev/sda2 data1 || fail "open data1 with the keyfile"',
              'fsck.vfat -n /dev/mapper/data1 >/tmp/f1 2>&1 || { cat /tmp/f1; fail "fsck data1"; }',
              'mount -t vfat /dev/mapper/data1 /mnt/v || fail "mount data1"',
              'grep -q "nexus wrote data1 cpus=" /mnt/v/nexus-wrote.txt || fail "data1 file: $(cat /mnt/v/nexus-wrote.txt)"',
              'echo "[64] FSTEST PASS data1: $(cat /mnt/v/nexus-wrote.txt)"', 'umount /mnt/v; cryptsetup close data1',
              'cryptsetup open --key-file /nexus/pass-de2 /dev/sda3 data2 || fail "open data2 with the passphrase"',
              'fsck.vfat -n /dev/mapper/data2 >/tmp/f2 2>&1 || { cat /tmp/f2; fail "fsck data2"; }',
              'mount -t vfat /dev/mapper/data2 /mnt/v || fail "mount data2"',
              'grep -q "nexus wrote data2 cpus=" /mnt/v/nexus-wrote.txt || fail "data2 file"',
              'echo "[64] FSTEST PASS data2: $(cat /mnt/v/nexus-wrote.txt)"', 'umount /mnt/v; cryptsetup close data2',
              'cryptsetup open --test-passphrase --key-file /nexus/key3 /dev/sda4 || fail "data3 key"',
              'sync', 'echo "[64] FSTEST ALL PASSED"']
    ok, log = dtv.run_script(WORK, '\n'.join(verify) + '\n', [targets['crypttab']], files, tag='verify')
    check(ok, 'crypttab round trip: Debian cryptsetup opens data1 (keyfile) and data2 (German passphrase); the files NexusOS wrote are there, fsck.vfat clean',
          markers(log, 'FSTEST', 'fsck', 'rror'))

if not args.keep:
    for t in targets.values(): dtv.unlink(t)
print(f'SUMMARY crypttab: {sum(results)}/{len(results)} passed')
sys.exit(0 if all(results) else 1)
