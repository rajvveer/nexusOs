"""Clipboard history of Aurora (arch/x86_64/aurora_clipboard.c, picker aurora_clipboard_app.c), with clip_user.c.

python -I tests/test_aurora_clipboard.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

The desktop disk (a writable sparse copy) gets out/desktop.elf, aurora-app and /bin/cliptest; one boot with automatic
login runs "cliptest script": it copies "first copied text", "second copied text", then "hunter2-secret" offered with
x-kde-passwordManagerHint (a password manager's copy) and prints Aurora's history:
  * the two plain copies are kept (newest first), the password manager's copy is not (and never in Aurora's log);
  * Super+V opens the picker with 2 entries; Down + Enter puts "first copied text" back on the clipboard, the picker
    closes and the test window is offered exactly that text (Aurora serves it);
  * Super+V, Delete removes the chosen entry; C clears the history; the picker shows nothing afterwards;
  * no kernel or user fault, Aurora still runs.
"""
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
import aurora_vm  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
argv = sys.argv[1:]
positional = [a for i, a in enumerate(argv) if not a.startswith('--') and (i == 0 or argv[i - 1] not in ('--disk', '--smp'))]
WORK = Path(positional[0]) if positional else Path(r'D:\nexustest\wave2\ws_desk2_runs\clipboard')
DISK = Path(argv[argv.index('--disk') + 1]) if '--disk' in argv else Path(r'D:\nexustest\wave2\ws_desk2_disk\desktop64.img')
SMP = int(argv[argv.index('--smp') + 1]) if '--smp' in argv else 2
ACCEL = 'tcg' if '--tcg' in argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'bin/cliptest': OUT / 'clip_user.elf',
                       'nexus/desktop.args': '--aurora\n/bin/cliptest\nscript\n400\n',
                       'home/nexus/.aurora/login.conf': 'autologin=1\nlock_after=0\nlock_on_sleep=1\n'})
checks = []


def check(name, ok, detail=''):
    checks.append((name, bool(ok))); print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:300]), flush=True)
    return ok


def super_v(vm):
    vm.key('meta_l', True); time.sleep(.1); vm.tap('v'); time.sleep(.1); vm.key('meta_l', False); time.sleep(.3)


vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[])
try:
    vm.wait(r'\[64\] AURORA READY', seconds=300)
    vm.wait(r'\[64\] CLIPTEST script done', seconds=180, fail=None); time.sleep(1)
    log = vm.text()
    kept = re.findall(r'\[64\] AURORA CLIPBOARD kept uid=(\d+) bytes=(\d+) entries=(\d+)', log)
    check('the two plain copies are kept (17 and 18 bytes, entries 1 then 2)', [(b, e) for _, b, e in kept][:2] == [('17', '1'), ('18', '2')], kept)
    check("the password manager's copy is not kept", '[64] AURORA CLIPBOARD private (password field): not kept' in log and len(kept) == 2, kept)
    history = re.findall(r'\[64\] CLIPTEST history ([^\n]*)', log)
    check('the history lists the two copies, newest first, not the secret',
          len(history) >= 3 and history[0] == 'entries=2' and history[1].endswith('\tsecond copied text') and history[2].endswith('\tfirst copied text')
          and not any('hunter2' in h for h in history), history)
    aurora_lines = '\n'.join(re.findall(r'\[64\] AURORA [^\n]*', log))
    check("Aurora's own log never shows clipboard text", 'copied text' not in aurora_lines and 'hunter2' not in aurora_lines)
    # the picker: Down (the older entry) + Enter
    mark = len(vm.text()); super_v(vm)
    m = vm.wait(r'\[64\] AURORA CLIPBOARD APP open entries=(\d+)', mark, 60, fail=None)
    check('Super+V opens the picker with 2 entries', m and m.group(1) == '2', m and m.group(0))
    time.sleep(2); vm.shot('picker'); vm.tap('down'); time.sleep(.5)
    mark = len(vm.text()); vm.tap('ret')
    used = vm.wait(r'\[64\] AURORA CLIPBOARD use uid=\d+ bytes=(\d+)', mark, 30, fail=None)
    check('Enter puts the chosen entry back (17 bytes)', used and used.group(1) == '17', used and used.group(0))
    got = vm.wait(r'\[64\] CLIPTEST selection: first copied text', mark, 30, fail=None)
    check('the focused window is offered that text, served by Aurora', got, re.findall(r'CLIPTEST [^\n]*', vm.text()[mark:])[-6:])
    check('the picker closed', vm.wait(r'\[64\] AURORA APP CLOSED app=clipboard', mark, 30, fail=None))
    # Delete removes the chosen (newest: "first copied text" again), C clears
    time.sleep(1); mark = len(vm.text()); super_v(vm)
    vm.wait(r'\[64\] AURORA CLIPBOARD APP open entries=2', mark, 60, fail=None); time.sleep(2)
    vm.tap('delete'); removed = vm.wait(r'\[64\] AURORA CLIPBOARD removed uid=\d+ bytes=17 entries=1', mark, 30, fail=None)
    check('Delete removes the chosen entry', removed, re.findall(r'AURORA CLIPBOARD [^\n]*', vm.text()[mark:])[-4:])
    vm.tap('c'); cleared = vm.wait(r'\[64\] AURORA CLIPBOARD cleared uid=\d+ entries=1', mark, 30, fail=None)
    check('C clears the history', cleared)
    vm.tap('esc'); vm.wait(r'\[64\] AURORA APP CLOSED app=clipboard', mark, 30, fail=None); time.sleep(1)
    mark = len(vm.text()); super_v(vm)
    empty = vm.wait(r'\[64\] AURORA CLIPBOARD APP open entries=(\d+)', mark, 60, fail=None)
    check('after clearing, the picker is empty', empty and empty.group(1) == '0', empty and empty.group(0))
    time.sleep(1); vm.shot('picker_empty'); vm.tap('esc'); time.sleep(1)
    log = vm.text()
    check('no kernel or user fault; Aurora still runs', 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and 'DESKTOP EXIT' not in log, re.findall(r'[^\n]*_FAULT[^\n]*', log)[:3])
except Exception as error:   # noqa: BLE001
    check('test ran to the end', False, repr(error))
    try: vm.shot('failure')
    except Exception: pass
finally:
    vm.close()
    # the desk disk is shared by the desk tests: leave no login options or test program behind
    aurora_vm.module('clip_fatput', ROOT / 'userland/fatput.py').edit(['fatput', str(DISK), 'rm', 'home/nexus/.aurora/login.conf', 'rm', 'bin/cliptest'])
ok = checks and all(c[1] for c in checks)
print('RESULT', 'PASS' if ok else 'FAIL', len(checks), 'checks', flush=True)
sys.exit(0 if ok else 1)
