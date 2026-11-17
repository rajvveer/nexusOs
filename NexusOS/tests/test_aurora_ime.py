"""Aurora input methods end to end: zwp_text_input_v3 + zwp_input_method_v2 (keyboard grab, candidate popup) +
zwp_virtual_keyboard_v1 (arch/x86_64/aurora_ime.c), driven by the test pair arch/x86_64/ime_user.c (/bin/imtest).
Aurora starts the input method itself ("/bin/imtest im" from ~/.aurora/input-method.conf); the desktop's child is the app.

python -I tests/test_aurora_ime.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

One boot of the desktop disk through a qcow2 overlay (a test /etc/shadow and automatic login are staged and removed
again); /bin/imtest starts a toy transliterating input method and a text field application. Checks:
  * hostile, before typing: the application does not see the input-method / virtual-keyboard globals and binding every
    unlisted global name is refused; a 4050-byte surrounding text (protocol limit 4000) is dropped; a second input method
    gets "unavailable";
  * typed keys go to the input method's grab, not the application: "ni" is a preedit (the candidate popup shows right
    below the field's cursor rectangle), space commits 你; "hao" -> 好; "namaste" -> नमस्ते (Devanagari);
    backspace edits the preedit; "1" is not used by the input method and reaches the application through the virtual
    keyboard; the application never sees a letter key directly;
  * Super+L: the input method is deactivated (focus leaves the field) and gets none of the password keys; after the
    unlock it is activated again; "zhongwen" -> 中文, the application leaves the field: deactivated, the popup is gone;
  * the input method then asks for a popup on a subsurface: protocol error "role", its connection ends, Aurora restarts
    the program (start=2); a key then reaches the application directly (the field is no longer enabled);
  * no kernel or user fault.
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
args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] not in ('--disk', '--smp')]
WORK = Path(args[0]) if args else nexus_paths.runs('ime')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
sys.stdout.reconfigure(errors='backslashreplace')   # Chinese/Devanagari results on a cp1252 console
POPUP = (0x2E, 0x8B, 0x57)
shadow = 'root:*:20000:0:99999:7:::\nnexus:%s:20000:0:99999:7:::\n' % sha512_crypt('imeAurora5', 'ImeTestSaltValue')
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'bin/imtest': OUT / 'ime_user.elf',
                       'nexus/desktop.args': '--aurora\n/bin/imtest\napp\n',
                       'home/nexus/.aurora/input-method.conf': 'program=/bin/imtest im\n',
                       'etc/shadow': shadow, 'home/nexus/.aurora/login.conf': 'autologin=1\nlock_after=0\nlock_on_sleep=1\n'})
results = {}
FAIL = r'\[64\] (IMTEST FAIL|KERNEL_FAULT|USER_FAULT)[^\n]*'


def popup_box(image):
    w, h = image.size; px = image.load(); xs, ys = [], []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            if px[x, y][:3] == POPUP: xs.append(x); ys.append(y)
    return (min(xs), min(ys), max(xs), max(ys)) if xs else None


def u(text):   # the serial log is read as latin-1: UTF-8 text as it appears there
    return text.encode('utf-8').decode('latin-1')


def typed(vm, keys, expect, mark=None):
    mark = len(vm.text()) if mark is None else mark
    for k in keys: vm.tap(k)
    return vm.wait(expect, mark, seconds=30, fail=FAIL)


vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[])
try:
    vm.wait(r'AURORA READY', seconds=300, fail=FAIL)
    vm.wait(r'AURORA IME PROGRAM /bin/imtest pid=\d+ start=1', seconds=60, fail=FAIL)
    vm.wait(r'IMTEST IM READY second-unavailable', seconds=60, fail=FAIL)
    vm.wait(r'IMTEST APP NO-IM-GLOBALS', seconds=60, fail=FAIL)
    refused = int(vm.wait(r'IMTEST APP HIDDEN-BIND refused=(\d+)', seconds=60, fail=FAIL).group(1)); assert refused >= 2, refused
    m = vm.wait(r'AURORA WINDOW app=nexus.imtest title=IME test x=(-?\d+) y=(-?\d+)', seconds=60, fail=FAIL); wx, wy = int(m.group(1)), int(m.group(2))
    vm.wait(r'IMTEST APP READY', seconds=60, fail=FAIL)
    vm.wait(r'IMTEST IM ACTIVATE surrounding_max=(\d+)', seconds=30, fail=FAIL)
    assert 'AURORA IME GRAB keyboard' in vm.text()
    results['hostile'] = ("the app sees no input-method/virtual-keyboard global; binding the %d unlisted global names was refused; "
                          "a second input method got 'unavailable'" % refused)
    vm.home(1700, 900); time.sleep(.5)
    mark = len(vm.text())
    typed(vm, 'ni', r'IMTEST APP PREEDIT text=ni', mark)
    shot = vm.frame('preedit', lambda p: popup_box(p) is not None, seconds=20); box = popup_box(shot)
    want = (wx + 40, wy + 80)
    assert abs(box[0] - want[0]) <= 2 and abs(box[1] - want[1]) <= 2 and 150 <= box[2] - box[0] + 1 <= 162, (box, want)
    results['popup'] = 'candidate popup at %d,%d (%dx%d), cursor rectangle bottom-left %d,%d' % (box[0], box[1], box[2] - box[0] + 2, box[3] - box[1] + 2, *want)
    typed(vm, ['spc'], re.escape(u('IMTEST APP COMMIT text=你 ')))
    typed(vm, ['h', 'a', 'o', 'spc'], re.escape(u('IMTEST APP COMMIT text=你好 ')))
    mark = len(vm.text())
    typed(vm, list('namastx'), r'IMTEST APP PREEDIT text=namastx', mark)
    typed(vm, ['backspace', 'e'], r'IMTEST APP PREEDIT text=namaste')
    typed(vm, ['spc'], re.escape(u('IMTEST APP COMMIT text=你好नमस्ते ')))
    m = typed(vm, ['1'], r'IMTEST APP KEY key=2 state=1 direct_letters=(\d+)')
    assert m.group(1) == '0', 'the application got letter keys past the grab'
    assert 'IM FORWARD key=2 state=1' in vm.text()
    results['compose'] = 'ni -> 你, hao -> 好, namastx<BS>e -> नमस्ते (preedit edited), "1" forwarded through the virtual keyboard; no letter reached the app directly'
    log = vm.text(); smax = int(re.findall(r'IMTEST IM ACTIVATE surrounding_max=(\d+)', log)[-1])
    assert smax <= 4000, 'oversized surrounding text was passed on'
    # lock: deactivated, password keys are the lock screen's
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('l'); vm.key('meta_l', False)
    vm.wait(r'AURORA LOCKED reason=shortcut', mark, seconds=20, fail=FAIL); vm.wait(r'IMTEST IM DEACTIVATE count=1', mark, seconds=20, fail=FAIL)
    keys_before = vm.text().count('IMTEST IM KEY ')
    for k in 'ime': vm.tap(k)
    vm.key('shift', True); vm.tap('a'); vm.key('shift', False)
    for k in 'urora5': vm.tap(k)
    vm.tap('ret'); vm.wait(r'AURORA UNLOCKED user=nexus', mark, seconds=30, fail=FAIL)
    assert vm.text().count('IMTEST IM KEY ') == keys_before, 'the input method saw lock-screen keys'
    vm.wait(r'IMTEST IM ACTIVATE', mark, seconds=20, fail=FAIL)
    results['lock'] = 'Super+L deactivated the input method; the password went to the lock screen only; re-activated after unlock'
    time.sleep(.5); mark = len(vm.text())
    typed(vm, list('zhongwen') + ['spc'], re.escape(u('IMTEST APP COMMIT text=你好नमस्ते中文 ')), mark)
    vm.wait(r'IMTEST IM DEACTIVATE count=2', mark, seconds=20, fail=FAIL)
    vm.frame('deactivated', lambda p: popup_box(p) is None, seconds=20)
    vm.wait(r'IMTEST IM ROLE-ERROR ok', mark, seconds=20, fail=FAIL); vm.wait(r'AURORA IME GONE', mark, seconds=20, fail=FAIL)
    vm.wait(r'AURORA IME PROGRAM GONE starts=1', mark, seconds=20, fail=FAIL)
    vm.wait(r'AURORA IME PROGRAM /bin/imtest pid=\d+ start=2', mark, seconds=20, fail=FAIL); vm.wait(r'IMTEST IM READY', mark, seconds=30, fail=FAIL)
    results['deactivate'] = ('the app left the field: deactivated, popup hidden; a popup on a subsurface was the protocol error '
                             '"role", the program was restarted (start=2)')
    time.sleep(.5); typed(vm, ['a'], r'IMTEST APP KEY key=30 state=1')
    results['no_field'] = 'with no enabled field keys reach the application directly'
    assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT'))
except BaseException:
    try: vm.shot('failure')
    except Exception: pass
    raise
finally:
    vm.close()
    aurora_vm.module('ime_fatput', ROOT / 'userland/fatput.py').edit(['fatput', str(DISK), 'rm', 'etc/shadow', 'rm', 'home/nexus/.aurora/login.conf',
                                                                     'rm', 'home/nexus/.aurora/input-method.conf'])
results['smp'] = SMP
(WORK / 'results.json').write_text(json.dumps(results, indent=1, ensure_ascii=False), encoding='utf-8')
print('PASS Aurora input methods (smp %d):' % SMP, '; '.join(f'{k}: {v}' for k, v in results.items() if k != 'smp'), flush=True)
