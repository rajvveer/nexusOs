"""Drag and drop between Wayland clients on Aurora (wl_data_device v3, desktop.c), with arch/x86_64/dnd_user.c.

python -I tests/test_aurora_dnd.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

The desktop disk (a writable sparse copy) gets out/desktop.elf, aurora-app and /bin/dndtest; one boot with automatic
login runs "dndtest pair": a target window (accepts text/plain, copy|move, prefers copy) and a source window (offers
text/plain with a drag icon). The source is tiled to the right (found on a screenshot by its colour), then over the USB tablet:
  * a stale-serial drag is refused (the source gets cancelled) and the manager is version 3;
  * press on the source, move to the target, release: Aurora starts the drag, the source loses the pointer, the target
    gets enter + motions + the copy action, the drop, receives "dragged text 42", finishes; the source sees target,
    send, dnd_drop_performed, dnd_finished;
  * a second drag cancelled with Esc, a third released over the dock: the source gets cancelled each time;
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
WORK = Path(positional[0]) if positional else Path(r'D:\nexustest\wave2\ws_desk2_runs\dnd')
DISK = Path(argv[argv.index('--disk') + 1]) if '--disk' in argv else Path(r'D:\nexustest\wave2\ws_desk2_disk\desktop64.img')
SMP = int(argv[argv.index('--smp') + 1]) if '--smp' in argv else 2
ACCEL = 'tcg' if '--tcg' in argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'bin/dndtest': OUT / 'dnd_user.elf',
                       'nexus/desktop.args': '--aurora\n/bin/dndtest\npair\n400\n',
                       'home/nexus/.aurora/login.conf': 'autologin=1\nlock_after=0\nlock_on_sleep=1\n'})
checks = []


def check(name, ok, detail=''):
    checks.append((name, bool(ok))); print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:300]), flush=True)
    return ok


def drag(vm, x0, y0, x1, y1, finish=None):
    vm.goto(x0, y0); time.sleep(.3); vm.button('left', True); time.sleep(.4)
    steps = 12
    for i in range(1, steps + 1): vm.move_to(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps); time.sleep(.08)
    time.sleep(.5)
    if finish: finish()
    vm.button('left', False); time.sleep(1.5)


vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[])
try:
    vm.wait(r'\[64\] AURORA READY', seconds=300)
    vm.wait(r'\[64\] DNDTEST TARGET ready', seconds=120, fail=None); vm.wait(r'\[64\] DNDTEST SOURCE ready', seconds=120, fail=None)
    log = vm.text()
    check('the data device manager is version 3', 'DNDTEST manager version 3' in log)
    vm.wait(r'\[64\] AURORA WINDOW app=nexus.dndsource', seconds=60, fail=None); time.sleep(1)
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('right'); vm.key('meta_l', False)   # the source (focused) to the right half
    vm.wait(r'\[64\] AURORA TILE app=nexus.dndsource right', mark, 30, fail=None)
    time.sleep(1); shot = vm.shot('tiled')
    def center(rgb):   # the window painted in rgb (dnd_user.c fills its windows with one colour)
        px = shot.load(); xs = [x for x in range(0, shot.size[0], 4) for y in (range(40, shot.size[1] - 120, 40)) if px[x, y] == rgb]
        ys = [y for y in range(40, shot.size[1] - 120, 4) for x in range(0, shot.size[0], 40) if px[x, y] == rgb]
        return (sum(xs) // len(xs), sum(ys) // len(ys)) if xs and ys else None
    src, dst = center((48, 160, 80)), center((48, 96, 192))
    check('both windows are on screen apart (source tiled right)', src and dst and abs(src[0] - dst[0]) > 300, (src, dst))
    # 1: a complete drag, source (right) -> target (left)
    mark = len(vm.text()); drag(vm, src[0], src[1], dst[0], dst[1])
    m = vm.wait(r'\[64\] DNDTEST TARGET received: ([^\n]*)', mark, 30, fail=None)
    vm.wait(r'\[64\] DNDTEST SOURCE finished', mark, 30, fail=None); after = vm.text()[mark:]
    check('a stale-serial drag is refused (source cancelled)', 'SOURCE stale-serial cancelled' in after, re.findall(r'DNDTEST [^\n]*', after)[:6])
    check('Aurora started the drag; the source lost the pointer', '[64] AURORA DND START app=nexus.dndsource types=2' in after and 'SOURCE pointer leave' in after)
    check('the target got enter (offer with text) and motions', re.search(r'DNDTEST TARGET enter own=1 offer=1 text=1', after) and re.search(r'TARGET drop motions=[1-9]', after))
    check('copy was negotiated (target and source action 1)', 'TARGET action 1' in after and 'SOURCE action 1' in after)
    check('the drop delivered the text', m.group(1).strip() == 'dragged text 42', m.group(1))
    check('the source saw target, send, dnd_drop_performed and dnd_finished',
          all(x in after for x in ('SOURCE target text/plain;charset=utf-8', 'SOURCE send text/plain;charset=utf-8', 'SOURCE drop performed', 'SOURCE finished')))
    check('Aurora finished the drag (action copy)', '[64] AURORA DND FINISHED action=1' in after)
    # 2: Esc cancels
    mark = len(vm.text()); drag(vm, src[0], src[1], dst[0], dst[1], finish=lambda: vm.tap('esc'))
    vm.wait(r'\[64\] DNDTEST SOURCE drag cancelled', mark, 30, fail=None); after = vm.text()[mark:]
    check('Esc cancels a drag (the target is left, the source cancelled)', '[64] AURORA DND CANCELLED escape' in after and 'TARGET leave' in after and 'TARGET received' not in after)
    # 3: released over the dock (no client): cancelled
    mark = len(vm.text()); drag(vm, src[0], src[1], 960, 1050)   # over the dock: no client takes it
    vm.wait(r'\[64\] DNDTEST SOURCE drag cancelled', mark, 30, fail=None); after = vm.text()[mark:]
    check('a drop where nothing accepts is cancelled', '[64] AURORA DND CANCELLED no target accepted' in after)
    log = vm.text()
    check('no kernel or user fault; Aurora still runs', 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and 'DESKTOP EXIT' not in log, re.findall(r'[^\n]*_FAULT[^\n]*', log)[:3])
except Exception as error:   # noqa: BLE001
    check('test ran to the end', False, repr(error))
    try: vm.shot('failure')
    except Exception: pass
finally:
    vm.close()
    # the desk disk is shared by the desk tests: leave no password, login options or test program behind
    aurora_vm.module('dnd_fatput', ROOT / 'userland/fatput.py').edit(['fatput', str(DISK), 'rm', 'home/nexus/.aurora/login.conf', 'rm', 'bin/dndtest'])
ok = checks and all(c[1] for c in checks)
print('RESULT', 'PASS' if ok else 'FAIL', len(checks), 'checks', flush=True)
sys.exit(0 if ok else 1)
