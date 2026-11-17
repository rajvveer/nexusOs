"""Idle inhibition on the Aurora desktop: zwp_idle_inhibit_manager_v1 (arch/x86_64/aurora_idle.c) against the idle lock.

python -I tests/test_aurora_idle.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

The desktop disk (a writable sparse copy) gets out/desktop.elf, aurora-app, /bin/idletest (arch/x86_64/idle_user.c), a
test /etc/shadow (the desktop user needs a password for an idle lock) and login.conf with lock_after=10, then boots
through a qcow2 overlay with /bin/idletest as the desktop's application:
  * hostile first: an inhibitor whose surface is destroyed first, two inhibitors on one surface, the manager destroyed
    before its inhibitors - Aurora goes on;
  * while the window holds its inhibitor (40 s, no input at all) there is no idle lock (lock_after is 10 s);
  * once it is released the idle lock comes within ~20 s;
  * no kernel or user fault.
"""
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
import aurora_vm  # noqa: E402
from sha512_crypt import sha512_crypt  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
argv = sys.argv[1:]
positional = [a for i, a in enumerate(argv) if not a.startswith('--') and (i == 0 or argv[i - 1] not in ('--disk', '--smp'))]
WORK = Path(positional[0]) if positional else Path(r'D:\nexustest\wave2\ws_desk2_runs\idle')
DISK = Path(argv[argv.index('--disk') + 1]) if '--disk' in argv else Path(r'D:\nexustest\wave2\ws_desk2_disk\desktop64.img')
SMP = int(argv[argv.index('--smp') + 1]) if '--smp' in argv else 2
ACCEL = 'tcg' if '--tcg' in argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
shadow = 'root:*:20000:0:99999:7:::\nnexus:%s:20000:0:99999:7:::\n' % sha512_crypt('idleAurora7', 'IdleTestSaltVal1')
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'bin/idletest': OUT / 'idle_user.elf',
                       'nexus/desktop.args': '--aurora\n/bin/idletest\n40\n120\nhostile\n', 'etc/shadow': shadow,
                       'home/nexus/.aurora/login.conf': 'autologin=1\nlock_after=10\nlock_on_sleep=1\n'})
checks = []


def check(name, ok, detail=''):
    checks.append((name, bool(ok))); print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:300]), flush=True)


vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[])
try:
    vm.wait(r'\[64\] AURORA READY', seconds=300)
    hostile = vm.wait(r'\[64\] IDLETEST (hostile steps done|FAIL[^\n]*)', seconds=120, fail=None)
    check('hostile inhibitor sequences are survived', hostile.group(1) == 'hostile steps done', hostile.group(0))
    start = vm.wait(r'\[64\] IDLETEST inhibiting for 40 s', seconds=60, fail=None); t0 = time.time()
    vm.wait(r'\[64\] AURORA IDLE INHIBITED surfaces=\d+', seconds=30, fail=None)   # 2: the hostile run inhibits the window twice
    released = vm.wait(r'\[64\] IDLETEST released', seconds=90, fail=None); t1 = time.time()
    before = vm.text()[:released.end()]
    check('no idle lock while the window holds its inhibitor (lock_after 10 s, %.0f s without input)' % (t1 - t0),
          'AURORA LOCKED reason=idle' not in before and t1 - t0 >= 35, t1 - t0)
    mark = len(vm.text())
    lock = vm.wait(r'\[64\] AURORA LOCKED reason=idle', mark, 60, fail=None); t2 = time.time()
    check('the idle lock comes once the inhibitor is released (%.0f s later)' % (t2 - t1), lock and t2 - t1 <= 30, t2 - t1)
    log = vm.text()
    check('no kernel or user fault', 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log, re.findall(r'[^\n]*_FAULT[^\n]*', log)[:3])
finally:
    vm.close()
    # the desk disk is shared by the desk tests: leave no password, login options or test program behind
    aurora_vm.module('idle_fatput', ROOT / 'userland/fatput.py').edit(['fatput', str(DISK), 'rm', 'etc/shadow', 'rm', 'home/nexus/.aurora/login.conf', 'rm', 'bin/idletest'])
ok = checks and all(c[1] for c in checks)
print('RESULT', 'PASS' if ok else 'FAIL', len(checks), 'checks', flush=True)
sys.exit(0 if ok else 1)
