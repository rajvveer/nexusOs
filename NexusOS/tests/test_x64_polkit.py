"""Authorization broker (nexus-polkitd) + pkexec, end to end on an ext4 (POSIX) root.

python -I tests/test_x64_polkit.py [WORK] [--smp N] [--tcg]

A copy of out/apps64-ext4.img gets /usr/libexec/nexus-polkitd, /usr/bin/pkexec (04755 root), the shared password
helper /usr/libexec/nexus-auth (04755 root), /sbin/init and /bin/pkexec-probe (both out/polkit_test_user.elf, 0755 -
pkexec itself is the set-uid door), /etc/{passwd,group,shadow} with administrators 'nexus', 'lockb', 'lockp' (sudo)
and a non-admin 'alice', and /nexus/init-as-root so init runs as root. The driver (arch/x86_64/polkit_test_user.c)
checks the broker's QUERY/CHECK decisions, the SO_PEERCRED subject binding, that pkexec escalates to root only for an
administrator with the right password, and that both the broker and pkexec's no-broker fallback go through
nexus-auth: 5 wrong passwords (2 s each) lock the account, the 6th attempt is refused even with the right password,
the tally /var/lib/nexus-auth/USER counts 5, and after the 60 s lockout the account authenticates again (tally 0).
About 90 s of the run is that lockout waiting.

Needs out/nexus-polkitd.elf, out/pkexec.elf, out/polkit_test_user.elf, out/nexus-auth.elf (arch/x86_64/desktop.py).
"""
from pathlib import Path
import importlib.util
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
from sha512_crypt import sha512_crypt  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] not in ('--smp', '--qmp-port')]
WORK = Path(args[0]) if args else Path(r'C:\nexustest\wave2\ws_sec2_runs\polkit')
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
# QMP port: --qmp-port N, else the sec2 agent range (desk_vm's default 4660-4669 belongs to the desk agent)
QMP = int(sys.argv[sys.argv.index('--qmp-port') + 1]) if '--qmp-port' in sys.argv else 4714
desk_vm.QMP_PORTS = range(QMP, QMP + 1)
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('polkit_ext4lib', ROOT / 'userland/ext4lib.py'); ext4lib = importlib.util.module_from_spec(spec); spec.loader.exec_module(ext4lib)

for req in ('nexus-polkitd.elf', 'pkexec.elf', 'polkit_test_user.elf', 'nexus-auth.elf'):
    assert (OUT / req).exists(), f'build first (arch/x86_64/desktop.py): missing {req}'

disk = WORK / 'disk.img'
shutil.copyfile(OUT / 'apps64-ext4.img', disk)
staged = WORK / 'staged'; staged.mkdir(exist_ok=True)
ACCOUNTS = (   # name, uid, admin, password, salt
    ('nexus', 1000, True, 'nexusPass1', 'NexusSaltValue12'), ('alice', 1001, False, 'alicePass2', 'AliceSaltValue34'),
    ('lockb', 1002, True, 'lockbPass3', 'LockBSaltValue56'), ('lockp', 1003, True, 'lockpPass4', 'LockPSaltValue78'))
files = {
    'passwd': 'root:x:0:0:root:/root:/bin/bash\n' + ''.join(f'{n}:x:{u}:{u}:{n}:/home/{n}:/bin/bash\n' for n, u, _, _, _ in ACCOUNTS),
    'group': 'root:x:0:\nshadow:x:42:\nsudo:x:27:' + ','.join(n for n, _, a, _, _ in ACCOUNTS if a) + '\n'
             + ''.join(f'{n}:x:{u}:\n' for n, u, _, _, _ in ACCOUNTS),
    'shadow': 'root:%s:20000:0:99999:7:::\n' % sha512_crypt('rootSecret8', 'RootSaltValue123')
              + ''.join('%s:%s:20000:0:99999:7:::\n' % (n, sha512_crypt(p, s)) for n, _, _, p, s in ACCOUNTS),
}
for name, text in files.items(): (staged / name).write_text(text, newline='\n')
marker = staged / 'marker'; marker.write_bytes(b'1')
v = ext4lib.Writer(str(disk))
v.put_file(str(OUT / 'nexus-polkitd.elf'), 'usr/libexec/nexus-polkitd', mode=0o755, uid=0, gid=0)
v.put_file(str(OUT / 'pkexec.elf'), 'usr/bin/pkexec', mode=0o4755, uid=0, gid=0)
v.put_file(str(OUT / 'nexus-auth.elf'), 'usr/libexec/nexus-auth', mode=0o4755, uid=0, gid=0)   # the one password checker
v.put_file(str(OUT / 'polkit_test_user.elf'), 'sbin/init', mode=0o755, uid=0, gid=0)
v.put_file(str(OUT / 'polkit_test_user.elf'), 'bin/pkexec-probe', mode=0o755, uid=0, gid=0)
v.put_file(str(staged / 'passwd'), 'etc/passwd', mode=0o644, uid=0, gid=0)
v.put_file(str(staged / 'group'), 'etc/group', mode=0o644, uid=0, gid=0)
v.put_file(str(staged / 'shadow'), 'etc/shadow', mode=0o640, uid=0, gid=42)
v.put_file(str(marker), 'nexus/init-as-root', mode=0o644, uid=0, gid=0)
v.close()
shutil.rmtree(staged)

import re
vm = desk_vm.DeskVm(WORK / 'vm', disk, heads=1, accel=ACCEL, smp=SMP, memory='512', raw_disk=True, vnc_heads=[])
try:
    vm.wait(r'POLKIT ALL PASSED', seconds=480, fail=r'\[64\] (POLKIT FAIL|KERNEL_FAULT|USER_FAULT)[^\n]*')
    vm.wait(r'INIT SESSION RETURNED', seconds=60)
    vm.proc.wait(timeout=60)
    assert vm.proc.returncode == 33, vm.proc.returncode
    passes = re.findall(r'POLKIT PASS ([^\n]*)', vm.text())
finally:
    vm.close()
disk.unlink()
print('PASS nexus-polkitd + pkexec on ext4 (smp %d): %s' % (SMP, '; '.join(passes)), flush=True)
