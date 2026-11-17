"""The privileged session launcher nexus-session (multi-user sessions) on an ext4 root, end to end, without a desktop.

python -I tests/test_x64_sessions.py [WORK] [--smp 1,2] [--tcg]

A copy of out/apps64-ext4.img gets /usr/libexec/nexus-session and nexus-auth (04755 root), accounts (passwd, group,
shadow 0640 root:shadow with SHA-512-crypt hashes made here): nexus 1000 (the desktop user), alice 1001 (+ group media),
bob 1002 (no password), carol (locked), nolog (nologin shell), dave (home owned by root), erin (throttling), daemon1
(uid 999), root; /etc/skel, /etc/default/locale (hi_IN.UTF-8), the desktop user's private files, /sbin/init =
out/session_test_user.elf (pid 1, uid 1000, as Aurora; arch/x86_64/session_test_user.c lists its checks) and the same
program as /bin/sessprobe, /bin/authpre.so (LD_PRELOAD probe). After the run the host reads the image: alice's home
(0700, hers, the file she wrote), bob's home, the failure tally of erin, alice's runtime folder emptied.
Each --smp value boots a fresh copy. Exit status 0 when every check passed.
"""
from pathlib import Path
import importlib.util
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
from sha512_crypt import sha512_crypt  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
argv = sys.argv[1:]
SMPS = [int(v) for v in argv[argv.index('--smp') + 1].split(',')] if '--smp' in argv else [1, 2]
ACCEL = 'tcg' if '--tcg' in argv else 'whpx'
positional = [a for i, a in enumerate(argv) if not a.startswith('--') and (i == 0 or argv[i - 1] != '--smp')]
WORK = Path(positional[0]) if positional else Path(r'D:\nexustest\wave2\ws_desk2_runs\sessions')
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('sessions_ext4lib', ROOT / 'userland/ext4lib.py')
ext4lib = importlib.util.module_from_spec(spec); spec.loader.exec_module(ext4lib)

USERS = [  # name, uid, gid, home, shell, password ('' none, None locked)
    ('root', 0, 0, '/root', '/bin/bash', 'rootPass9'), ('daemon1', 999, 999, '/var/lib/daemon1', '/bin/bash', 'daemonPass9'),
    ('nexus', 1000, 1000, '/home/nexus', '/bin/bash', 'nexusPass1'), ('alice', 1001, 1001, '/home/alice', '/bin/bash', 'alicePass1'),
    ('bob', 1002, 1002, '/home/bob', '/bin/bash', ''), ('carol', 1003, 1003, '/home/carol', '/bin/bash', None),
    ('nolog', 1004, 1004, '/home/nolog', '/usr/sbin/nologin', 'nologPass9'), ('dave', 1005, 1005, '/home/dave', '/bin/bash', 'davePass99'),
    ('erin', 1006, 1006, '/home/erin', '/bin/bash', 'erinPass99')]


def stage(disk):
    passwd = ''.join('%s:x:%d:%d:%s:%s:%s\n' % (n, u, g, n.title(), h, s) for n, u, g, h, s, _ in USERS)
    group = 'root:x:0:\nshadow:x:42:\nsudo:x:27:nexus\nmedia:x:2001:alice\n' + ''.join('%s:x:%d:\n' % (n, g) for n, _, g, _, _, _ in USERS if g)
    shadow = ''
    for n, u, _, _, _, pw in USERS:
        value = '' if pw == '' else '!' + sha512_crypt('carolPass9', 'CarolSaltValue12') if pw is None else sha512_crypt(pw, ('%sSaltValue1234' % n)[:16])
        shadow += '%s:%s:20000:0:99999:7:::\n' % (n, value)
    v = ext4lib.Writer(str(disk))
    v.put_file(str(OUT / 'nexus-session.elf'), 'usr/libexec/nexus-session', mode=0o4755, uid=0, gid=0)
    v.put_file(str(OUT / 'nexus-auth.elf'), 'usr/libexec/nexus-auth', mode=0o4755, uid=0, gid=0)
    v.put_file(str(OUT / 'session_test_user.elf'), 'sbin/init', mode=0o755, uid=0, gid=0)
    v.put_file(str(OUT / 'session_test_user.elf'), 'bin/sessprobe', mode=0o755, uid=0, gid=0)
    v.put_file(str(OUT / 'auth_preload.so'), 'bin/authpre.so', mode=0o755, uid=0, gid=0)
    v.put_bytes(passwd.encode(), 'etc/passwd', 0o644, 0, 0)
    v.put_bytes(group.encode(), 'etc/group', 0o644, 0, 0)
    v.put_bytes(shadow.encode(), 'etc/shadow', 0o640, 0, 42)
    v.put_bytes(b'LANG="hi_IN.UTF-8"\n', 'etc/default/locale', 0o644, 0, 0)
    v.put_bytes(b'# ~/.profile (skel)\n', 'etc/skel/.profile', 0o644, 0, 0)
    v.put_bytes(b'skel says hello\n', 'etc/skel/.config/skeltest/hello.txt', 0o644, 0, 0)
    v.put_bytes(b'nexus private\n', 'home/nexus/secret.txt', 0o600, 1000, 1000)
    v.ensure_dir(['home', 'nexus', 'private'], 0o700, 1000, 1000)
    v.ensure_dir(['run', 'user'], 0o755, 0, 0); v.ensure_dir(['run', 'user', '1000'], 0o700, 1000, 1000)
    v.put_bytes(b'', 'run/user/1000/wayland-0', 0o600, 1000, 1000)
    v.ensure_dir(['home', 'dave'], 0o755, 0, 0)   # an existing home that is not dave's
    v.ensure_dir(['var', 'lib'], 0o755, 0, 0)
    v.close()


def host_checks(disk):
    r = ext4lib.Ext4(str(disk)); notes = []
    def inode(path):
        ino = r.lookup(path); assert ino, path; return r.read_inode(ino)
    home = inode('home/alice'); assert home.uid == 1001 and home.mode & 0o7777 == 0o700, ('alice home', home.uid, oct(home.mode))
    doc = inode('home/alice/Documents/probe.txt'); assert doc.uid == 1001 and r.read_file(doc) == b'mine\n', 'probe.txt'
    bob = inode('home/bob'); assert bob.uid == 1002 and bob.mode & 0o777 == 0o700, ('bob home', oct(bob.mode))
    tally = r.read_file(inode('var/lib/nexus-auth/erin')); assert int(tally.split()[0]) >= 5, tally
    run = r.lookup('run/user/1001'); assert run and [n for n, _, _ in r.dir_entries(run) if n not in (b'.', b'..')] == [], 'runtime not empty'
    dave = inode('home/dave'); assert dave.uid == 0, 'dave home changed'
    notes.append('alice home 0700 uid 1001 + her file, bob home, erin tally %s, /run/user/1001 empty' % tally.split()[0].decode())
    if hasattr(r, 'close'): r.close()
    return notes


ok = True
for smp in SMPS:
    work = WORK / ('smp%d' % smp); work.mkdir(parents=True, exist_ok=True)
    disk = work / 'disk.img'
    shutil.copyfile(OUT / 'apps64-ext4.img', disk); stage(disk)
    vm = desk_vm.DeskVm(work / 'vm', disk, heads=1, accel=ACCEL, smp=smp, memory='768', raw_disk=True, vnc_heads=[])
    try:
        vm.wait(r'\[64\] SESSTEST (ALL PASSED|FAILED \d+|ABORT)', seconds=600, fail=None)
        log = vm.text()
        passed = re.findall(r'\[64\] SESSTEST PASS ([^\n]*)', log); failed = re.findall(r'\[64\] SESSTEST FAIL ([^\n]*)', log)
        vm.wait(r'INIT SESSION RETURNED', seconds=60, fail=None)
        vm.proc.wait(timeout=60)
        faults = [l for l in log.splitlines() if 'KERNEL_FAULT' in l or 'USER_FAULT' in l]
        notes = host_checks(disk) if not failed else []
        good = not failed and not faults and '[64] SESSTEST ALL PASSED' in log
        print('%s sessions smp %d: %d checks passed, %d failed%s' % ('PASS' if good else 'FAIL', smp, len(passed), len(failed), '; ' + '; '.join(notes) if notes else ''), flush=True)
        for f in failed: print('   FAIL', f, flush=True)
        for f in faults[:5]: print('   FAULT', f, flush=True)
        ok &= good
    except Exception as error:  # noqa: BLE001
        print('FAIL sessions smp %d: %r' % (smp, error), flush=True); ok = False
        print(vm.text()[-3000:], flush=True)
    finally:
        vm.close()
        disk.unlink(missing_ok=True)
print('RESULT', 'PASS' if ok else 'FAIL', flush=True)
sys.exit(0 if ok else 1)
