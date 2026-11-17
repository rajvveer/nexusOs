"""nexus-auth (the set-user-ID root account helper) on an ext4 root, end to end.

python -I tests/test_x64_auth_ext4.py [WORK] [--smp N] [--tcg]

A copy of out/apps64-ext4.img (built by arch/x86_64/build.py) gets /usr/libexec/nexus-auth (04755 root), /etc/passwd,
group and shadow (0640 root:shadow; SHA-512-crypt hashes made on the host), /sbin/init = out/auth_test_user.elf (runs as
uid 1000; arch/x86_64/auth_test_user.c lists its checks), /bin/authtest-root (the same program, 04755 root, used once to
make nexus an administrator) and /bin/authpre.so (an LD_PRELOAD probe). The host then reads the image: nexus has the new
password hash, alice is gone from passwd/group/shadow, the tally /var/lib/nexus-auth/root is root's 0600 and counts the
failures. The image copy is deleted afterwards.
"""
from pathlib import Path
import importlib.util
import shutil
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
from sha512_crypt import sha512_crypt  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] != '--smp']
WORK = Path(args[0]) if args else nexus_paths.runs('auth_ext4')
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('auth_ext4lib', ROOT / 'userland/ext4lib.py'); ext4lib = importlib.util.module_from_spec(spec); spec.loader.exec_module(ext4lib)

disk = WORK / 'disk.img'
shutil.copyfile(OUT / 'apps64-ext4.img', disk)
staged = WORK / 'staged'; staged.mkdir(exist_ok=True)
files = {
    'passwd': 'root:x:0:0:root:/root:/bin/bash\nnexus:x:1000:1000:Nexus:/home/nexus:/bin/bash\n',
    'group': 'root:x:0:\nshadow:x:42:\nsudo:x:27:\nnexus:x:1000:\n',
    'shadow': 'root:%s:20000:0:99999:7:::\nnexus:%s:20000:0:99999:7:::\n' % (sha512_crypt('rootSecret8', 'RootSaltValue123'), sha512_crypt('nexusPass1', 'NexusSaltValue12')),
}
for name, text in files.items(): (staged / name).write_text(text, newline='\n')
v = ext4lib.Writer(str(disk))
v.put_file(str(OUT / 'nexus-auth.elf'), 'usr/libexec/nexus-auth', mode=0o4755, uid=0, gid=0)
v.put_file(str(OUT / 'auth_test_user.elf'), 'sbin/init', mode=0o755, uid=0, gid=0)
v.put_file(str(OUT / 'auth_test_user.elf'), 'bin/authtest-root', mode=0o4755, uid=0, gid=0)
v.put_file(str(OUT / 'auth_preload.so'), 'bin/authpre.so', mode=0o755, uid=0, gid=0)
v.put_file(str(staged / 'passwd'), 'etc/passwd', mode=0o644, uid=0, gid=0)
v.put_file(str(staged / 'group'), 'etc/group', mode=0o644, uid=0, gid=0)
v.put_file(str(staged / 'shadow'), 'etc/shadow', mode=0o640, uid=0, gid=42)
v.close()
shutil.rmtree(staged)
results = []
vm = desk_vm.DeskVm(WORK / 'vm', disk, heads=1, accel=ACCEL, smp=SMP, memory='512', raw_disk=True, vnc_heads=[])
try:
    vm.wait(r'AUTHTEST ALL PASSED', seconds=300, fail=r'\[64\] (AUTHTEST FAIL|KERNEL_FAULT|USER_FAULT)[^\n]*')
    vm.wait(r'INIT SESSION RETURNED', seconds=60)
    vm.proc.wait(timeout=60)
    assert vm.proc.returncode == 33, vm.proc.returncode
    import re
    results = re.findall(r'AUTHTEST PASS ([^\n]*)', vm.text())
finally:
    vm.close()
r = ext4lib.Ext4(str(disk))
def read(path):
    ino = r.lookup(path); assert ino, path; inode = r.read_inode(ino); return inode, r.read_file(inode)
inode, shadow = read('etc/shadow')
assert inode.uid == 0 and inode.gid == 42 and inode.mode & 0o7777 == 0o640, (inode.uid, inode.gid, oct(inode.mode))
lines = dict(l.split(':', 1) for l in shadow.decode().splitlines() if l)
assert set(lines) == {'root', 'nexus'} and not lines['nexus'].startswith(sha512_crypt('nexusPass1', 'NexusSaltValue12')), lines.keys()
new_hash = lines['nexus'].split(':')[0]
assert new_hash.startswith('$6$') and sha512_crypt('nexusNew22', new_hash.split('$')[2]) == new_hash, ('new nexus hash', new_hash[:12])
_, passwd = read('etc/passwd'); _, group = read('etc/group')
assert b'alice' not in passwd and b'alice' not in group and b'sudo:x:27:nexus\n' in group
inode, tally = read('var/lib/nexus-auth/root')
assert inode.uid == 0 and inode.mode & 0o777 == 0o600 and int(tally.split()[0]) >= 5, (inode.uid, oct(inode.mode), tally)
dinode, _ = read('var/lib/nexus-auth'); assert dinode.mode & 0o777 == 0o700
if hasattr(r, 'close'): r.close()
disk.unlink()
results.append('host: shadow 0640 root:shadow with the new nexus hash; alice removed; tally /var/lib/nexus-auth/root 0600 root (%s failures)' % tally.split()[0].decode())
print('PASS nexus-auth on ext4 (smp %d): %s' % (SMP, '; '.join(results)), flush=True)
