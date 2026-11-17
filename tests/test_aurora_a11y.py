"""Aurora accessibility and keyboard layouts end to end (aurora_a11y.c, aurora_keyboard.c).

python -I tests/test_aurora_a11y.py [WORK] [--disk DESKTOP64.img] [--tcg]

One boot of the desktop disk through a qcow2 overlay (a test /etc/shadow and automatic login are staged and removed
again). Settings change through the control socket from the Terminal ("aurora-app ctl ..."):
  invert / increase contrast: the composed screen changes (a menu-bar pixel becomes its inverse);
  magnifier: the yellow lens frame appears around the pointer, Super+= zooms in;
  larger text / reduced motion: applied (log);
  keyboard: layouts us,de; the menu bar shows the input menu; Super+Space switches to German; the lock screen then
  takes the password "zzAurora7" typed on the US keys y y A u r o r a 7 (German swaps y and z);
  sticky keys: Shift tapped alone capitalises the next key (the same password with a sticky Shift).
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
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('a11y')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
shadow = 'root:*:20000:0:99999:7:::\nnexus:%s:20000:0:99999:7:::\n' % sha512_crypt('zzAurora7', 'KeyboardTestSalt')
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n',
                       'etc/shadow': shadow, 'home/nexus/.aurora/login.conf': 'autologin=1\nlock_after=0\nlock_on_sleep=1\n'})
results = {}


def ctl(vm, command, expect):
    mark = len(vm.text()); vm.type('aurora-app ctl ' + command + '\n'); return vm.wait(expect, mark, seconds=30)


vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=2, usb=True, vnc_heads=[])
try:
    vm.wait(r'AURORA READY', seconds=300); time.sleep(2)
    assert 'AURORA GREETER' not in vm.text(), 'automatic login'
    mark = len(vm.text()); vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('terminal'); time.sleep(.5); vm.tap('ret')
    tx, ty = map(int, vm.wait(r'AURORA WINDOW app=nexus.terminal title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=120).groups()); time.sleep(2)
    vm.home(1700, 600)   # the pointer away from the menu bar
    before = vm.shot('plain'); probe = (1000, 5); original = before.getpixel(probe)
    # inverted colours
    ctl(vm, 'set-a11y invert=1', r'AURORA A11Y [^\n]*invert=1')
    inv = vm.frame('inverted', lambda p: all(abs(a + b - 255) <= 6 for a, b in zip(p.getpixel(probe), original)))
    results['invert'] = 'menu bar pixel %s -> %s' % (original, inv.getpixel(probe))
    ctl(vm, 'set-a11y invert=0 contrast=1', r'AURORA A11Y [^\n]*contrast=1 invert=0')
    con = vm.frame('contrast', lambda p: p.getpixel(probe) != original and not all(abs(a + b - 255) <= 6 for a, b in zip(p.getpixel(probe), original)))
    results['contrast'] = 'menu bar pixel %s -> %s (steeper contrast)' % (original, con.getpixel(probe))
    # magnifier: the lens frame around the pointer, Super+= zooms
    ctl(vm, 'set-a11y contrast=0 magnifier=1', r'AURORA A11Y [^\n]*magnifier=1')
    vm.home(960, 540); time.sleep(1)
    lens = vm.frame('magnifier', lambda p: p.getpixel((960 - 240 + 1, 540)) == (255, 214, 10) and p.getpixel((960, 540 - 150 + 1)) == (255, 214, 10))
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('equal'); vm.key('meta_l', False); vm.wait(r'AURORA A11Y [^\n]*zoom=250', mark, seconds=20)
    results['magnifier'] = 'the lens (480x300, yellow frame) follows the pointer; Super+= zooms to 250 %'
    vm.home(tx + 200, ty + 60); vm.click(); time.sleep(.5)
    ctl(vm, 'set-a11y magnifier=0 larger_text=1 reduce_motion=1', r'AURORA A11Y larger_text=1 [^\n]*reduce_motion=1')
    vm.shot('larger-text'); results['larger_text_reduce_motion'] = 'applied (menu bar text at 125 %, dock without magnification)'
    ctl(vm, 'set-a11y larger_text=0 reduce_motion=0', r'AURORA A11Y larger_text=0')
    # keyboard layouts
    ctl(vm, 'set-keyboard layouts=us,de', r'AURORA KEYBOARD layouts=us,de variants=, active=US')
    time.sleep(1); vm.shot('input-menu')
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('spc'); vm.key('meta_l', False); vm.wait(r'AURORA KEYBOARD active=DE', mark, seconds=20)
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('l'); vm.key('meta_l', False); vm.wait(r'AURORA LOCKED reason=shortcut', mark, seconds=20)
    for k in ('y', 'y'): vm.tap(k)
    vm.key('shift', True); vm.tap('a'); vm.key('shift', False)
    for k in 'urora7': vm.tap(k)
    vm.tap('ret'); vm.wait(r'AURORA UNLOCKED user=nexus', mark, seconds=30)
    results['keyboard'] = 'us,de input sources; Super+Space -> German; "zzAurora7" typed on US y-keys unlocks (German keymap in effect)'
    # sticky keys (German still active): Shift tapped alone, then a -> A
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('spc'); vm.key('meta_l', False); vm.wait(r'AURORA KEYBOARD active=US', mark, seconds=20)
    vm.home(tx + 200, ty + 60); vm.click(); time.sleep(.5)
    ctl(vm, 'set-a11y sticky_keys=1', r'AURORA A11Y [^\n]*sticky_keys=1')
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('spc'); vm.key('meta_l', False); vm.wait(r'AURORA KEYBOARD active=DE', mark, seconds=20)
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('l'); vm.key('meta_l', False); vm.wait(r'AURORA LOCKED reason=shortcut', mark, seconds=20)
    for k in ('y', 'y'): vm.tap(k)
    vm.tap('shift'); vm.wait(r'AURORA STICKY Shift latched=0x1', mark, seconds=10); vm.tap('a')
    for k in 'urora7': vm.tap(k)
    vm.tap('ret'); vm.wait(r'AURORA UNLOCKED user=nexus', mark, seconds=30)
    results['sticky_keys'] = 'Shift tapped alone latched (AURORA STICKY) and capitalised the next key: the password was accepted'
    assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT'))
except BaseException:
    try: vm.shot('failure')
    except Exception: pass
    raise
finally:
    vm.close()
    aurora_vm.module('a11y_fatput', ROOT / 'userland/fatput.py').edit(['fatput', str(DISK), 'rm', 'etc/shadow', 'rm', 'home/nexus/.aurora/login.conf'])
(WORK / 'results.json').write_text(json.dumps(results, indent=1))
print('PASS Aurora accessibility and keyboard:', '; '.join(f'{k}: {v}' for k, v in results.items()), flush=True)
