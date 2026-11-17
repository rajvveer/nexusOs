"""Aurora greeter, lock screen and Settings > Users end to end (aurora_lock.c, aurora_auth.c, aurora_settings_users.c).

python -I tests/test_aurora_lock.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

The desktop disk (with /etc/passwd from apps_install.py) gets a test /etc/shadow for nexus (SHA-512-crypt made here,
tests/sha512_crypt.py) and the account helper; both are removed again at the end. Two boots of one qcow2 overlay:
 boot 1: the greeter comes first (a password is set, automatic login is off): a wrong password is refused, the right one
         opens the session; Super+L locks (the windows disappear), the password unlocks (they come back); the Terminal
         runs "aurora-app ctl prepare-sleep" (the power-management hook: locked and presented) and "resumed";
         Settings > Users lists the account (read-only on this FAT root), "lock after 1 min" locks the idle desktop,
         "Automatic login" is switched on.
 boot 2: automatic login: no greeter.
"""
from pathlib import Path
import json
import re
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
import aurora_vm  # noqa: E402
from sha512_crypt import sha512_crypt  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
args = [a for a in sys.argv[1:] if not a.startswith('--') and not a.isdigit()]
WORK = Path(args[0]) if args else nexus_paths.runs('lock')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
OVERLAY = WORK / 'overlay.qcow2'; OVERLAY.unlink(missing_ok=True)
PASSWORD = 'Aurora-lock-7'
shadow = 'root:*:20000:0:99999:7:::\nnexus:%s:20000:0:99999:7:::\n' % sha512_crypt(PASSWORD, 'NexusTestSalt16x')
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n',
                       'etc/shadow': shadow, 'usr/libexec/nexus-auth': OUT / 'nexus-auth.elf'})
RED = (255, 95, 87)
results = {}


def red_at(vm, x, y, want=True, label='frame', seconds=30):
    return vm.frame(label, lambda p: (p.getpixel((x, y)) == RED) == want, seconds=seconds)


def spotlight(vm, text):
    vm.tap('ctrl', 'spc'); time.sleep(1); vm.type(text); time.sleep(.5); vm.tap('ret')


def type_password(vm, text):
    vm.type(text); time.sleep(.3); vm.tap('ret')


try:
    vm = desk_vm.DeskVm(WORK / 'boot1', DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[], keep_overlay=True, overlay=OVERLAY)
    try:
        vm.wait(r'\[64\] AURORA GREETER reason=boot user=nexus', seconds=300); time.sleep(2)
        vm.shot('greeter')
        mark = len(vm.text()); type_password(vm, 'not-the-password')
        vm.wait(r'AURORA LOCK FAILED code=1', mark, seconds=30)
        mark = len(vm.text()); type_password(vm, PASSWORD)
        vm.wait(r'AURORA UNLOCKED user=nexus mode=greeter', mark, seconds=30)
        results['greeter'] = 'shown at boot (password set, no automatic login); wrong password refused; the right one opens the session'
        # Super+L hides the windows behind the lock screen; the password brings them back
        mark = len(vm.text()); spotlight(vm, 'calculator')
        cx, cy = map(int, vm.wait(r'AURORA WINDOW app=nexus.calculator title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=120).groups())
        red_at(vm, cx + 15, cy + 14, label='calc')
        mark = len(vm.text()); vm.key('meta_l', True); vm.tap('l'); vm.key('meta_l', False)
        vm.wait(r'AURORA LOCKED reason=shortcut user=nexus', mark, seconds=20)
        red_at(vm, cx + 15, cy + 14, want=False, label='locked')
        mark = len(vm.text()); type_password(vm, PASSWORD)
        vm.wait(r'AURORA UNLOCKED user=nexus mode=lock', mark, seconds=30)
        red_at(vm, cx + 15, cy + 14, label='unlocked')
        results['lock'] = 'Super+L locks (windows hidden); the password unlocks (windows back)'
        # the power-management hook through the control socket, from the Terminal
        mark = len(vm.text()); spotlight(vm, 'terminal')
        vm.wait(r'AURORA WINDOW app=nexus.terminal', mark, seconds=120); time.sleep(2)
        mark = len(vm.text()); vm.type('aurora-app ctl prepare-sleep\n')
        vm.wait(r'AURORA LOCKED reason=sleep user=nexus', mark, seconds=30); vm.wait(r'AURORA PREPARE SLEEP locked=1', mark, seconds=30)
        mark = len(vm.text()); type_password(vm, PASSWORD); vm.wait(r'AURORA UNLOCKED user=nexus mode=lock', mark, seconds=30)
        time.sleep(1); mark = len(vm.text()); vm.type('aurora-app ctl resumed\n'); vm.wait(r'AURORA RESUMED locked=0', mark, seconds=30)
        results['sleep_hook'] = '"prepare-sleep" locks and presents before replying; "resumed" restarts the idle timer'
        # Settings > Users: the account, read-only here; lock after 1 min of idle; automatic login on
        mark = len(vm.text()); spotlight(vm, 'system settings')
        sx, sy = map(int, vm.wait(r'AURORA WINDOW app=nexus.settings title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=120).groups()); time.sleep(1)
        mark = len(vm.text()); vm.home(sx + 100, sy + 29 + 63); vm.click()   # sidebar row 2: Users
        vm.wait(r'SETTINGS USERS count=1 writable=0 helper=1', mark, seconds=30)
        c = vm.wait(r'SETTINGS USERS CONTROLS password=(\d+),(\d+) add=(\d+),(\d+) autologin=(\d+),(\d+) lock_after=(\d+),(\d+),(\d+) sleep=(\d+),(\d+)', mark, seconds=30)
        ax, ay, lx, ly, lw = int(c.group(5)), int(c.group(6)), int(c.group(7)), int(c.group(8)), int(c.group(9))
        vm.shot('users')
        results['users_page'] = 'Settings > Users lists nexus (password set); accounts read-only on this FAT root (helper status writable=0)'
        mark = len(vm.text()); vm.home(sx + lx + lw * 3 // 8, sy + ly); vm.click()   # segment 2 of 4: 1 min (window-local coordinates)
        vm.wait(r'SETTINGS LOGIN autologin=0 lock_after=60', mark, seconds=30)
        vm.home(sx + 400, sy + 10); idle = len(vm.text())
        vm.wait(r'AURORA LOCKED reason=idle user=nexus', idle, seconds=120)
        results['idle_lock'] = 'with "lock after 1 min" the idle desktop locked by itself'
        mark = len(vm.text()); type_password(vm, PASSWORD); vm.wait(r'AURORA UNLOCKED user=nexus mode=lock', mark, seconds=30)
        mark = len(vm.text()); vm.home(sx + ax, sy + ay); vm.click()
        vm.wait(r'SETTINGS LOGIN autologin=1', mark, seconds=30)
        results['autologin_setting'] = 'Automatic login switched on (login.conf)'
        assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT'))
        vm.q.cmd('quit'); vm.proc.wait(30)
    except BaseException:
        try: vm.shot('failure')
        except Exception: pass
        raise
    finally:
        vm.close()
    vm = desk_vm.DeskVm(WORK / 'boot2', DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[], keep_overlay=True, overlay=OVERLAY)
    try:
        vm.wait(r'AURORA READY', seconds=300); time.sleep(5)
        assert 'AURORA GREETER' not in vm.text(), 'automatic login is on: no greeter'
        results['autologin'] = 'second boot: no greeter (automatic login)'
    finally:
        vm.close()
finally:
    OVERLAY.unlink(missing_ok=True)
    fatput = aurora_vm.module('lock_fatput', ROOT / 'userland/fatput.py')
    fatput.edit(['fatput', str(DISK), 'rm', 'etc/shadow'])   # the disk goes back to no passwords
(WORK / 'results.json').write_text(json.dumps(results, indent=1))
print('PASS Aurora lock:', '; '.join(f'{k}: {v}' for k, v in results.items()), flush=True)
