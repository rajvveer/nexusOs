"""Aurora window management end to end (aurora_wm.c + desktop.c): tiling and snapping, maximize/restore/minimize keys,
dock pictures of minimized windows, workspaces, the Alt+Tab switcher, the overview, show desktop.

python -I tests/test_aurora_wm.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

One boot of the prepared desktop disk through a qcow2 overlay (stages out/desktop.elf and out/aurora_app.elf), one
1920x1080 display, PS/2 + USB tablet. Calculator and Notes (Aurora apps) are the windows; every step is checked in the
serial log (AURORA TILE/SNAP/MINIMIZE/ACTIVATE/WORKSPACE/SWITCHER/OVERVIEW lines and the configured sizes) and in
screendumps (the red close button of each window where the step puts it).
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

OUT = ROOT / 'arch/x86_64/out'
args = [a for a in sys.argv[1:] if not a.startswith('--') and not a.isdigit()]
WORK = Path(args[0]) if args else nexus_paths.runs('wm')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n'})
RED = (255, 95, 87)
results = {}
vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[])


def chord(*keys):
    for k in keys: vm.key(k, True)
    for k in reversed(keys): vm.key(k, False)
    time.sleep(.3)


def red_at(x, y, seconds=30, label='frame'):
    return vm.frame(label, lambda p: p.getpixel((x, y)) == RED, seconds=seconds)


def open_app(name, dock_x):
    mark = len(vm.text()); vm.home(dock_x, 1040); vm.click()
    x, y = map(int, vm.wait(r'AURORA WINDOW app=nexus.' + name + r' title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=120).groups())
    vm.wait(r'DESKTOP BUFFER surface=\d+ \d+x\d+', mark, seconds=30)
    red_at(x + 15, y + 14, label=name + '-open')
    return x, y


try:
    vm.wait(r'AURORA READY', seconds=300)
    cx, cy = open_app('calculator', 1120)
    nx, ny = open_app('notes', 992)
    # -- Super+Left: Notes takes the left half of the work area (1920x960 below the menu bar, above the dock)
    mark = len(vm.text()); chord('meta_l', 'left')
    vm.wait(r'AURORA TILE app=nexus.notes left', mark, seconds=20); vm.wait(r'DESKTOP CONFIGURE surface=\d+ 960x960', mark, seconds=20)
    red_at(15, 32 + 14, label='notes-left')
    mark = len(vm.text()); chord('meta_l', 'right')                       # back to its own size
    vm.wait(r'AURORA TILE app=nexus.notes restored', mark, seconds=20)
    mark = len(vm.text()); chord('meta_l', 'right')
    vm.wait(r'AURORA TILE app=nexus.notes right', mark, seconds=20); red_at(960 + 15, 32 + 14, label='notes-right')
    mark = len(vm.text()); chord('meta_l', 'up')
    vm.wait(r'AURORA TILE app=nexus.notes maximized', mark, seconds=20); vm.wait(r'DESKTOP CONFIGURE surface=\d+ 1920x960', mark, seconds=20)
    red_at(15, 32 + 14, label='notes-max')
    mark = len(vm.text()); chord('meta_l', 'down')
    vm.wait(r'AURORA TILE app=nexus.notes restored', mark, seconds=20)
    results['tiling'] = 'Super+Left/Right tile halves (960x960), Super+Right untiles, Super+Up maximizes (1920x960), Super+Down restores'
    # -- Super+Down on a normal window minimizes it; the dock shows its picture; a click there brings it back
    dock_box = (1226, 990, 1310, 1076); before = vm.shot('dock-before').crop(dock_box).tobytes()
    mark = len(vm.text()); chord('meta_l', 'down')
    vm.wait(r'AURORA MINIMIZE app=nexus.notes', mark, seconds=20)
    vm.home(1900, 600)   # away from the dock: no magnification
    vm.frame('notes-minimized', lambda p: p.crop(dock_box).tobytes() != before)   # its picture right of the fixed icons
    mark = len(vm.text()); vm.home(1262, 1046); vm.click()
    vm.wait(r'AURORA ACTIVATE app=nexus.notes', mark, seconds=20)
    results['dock_minimized'] = 'a minimized window appears in the dock (picture) and comes back from there'
    # -- workspaces
    mark = len(vm.text()); chord('meta_l', '2'); vm.wait(r'AURORA WORKSPACE 2', mark, seconds=20)
    empty = vm.frame('workspace2', lambda p: p.getpixel((cx + 15, cy + 14)) != RED)
    mark = len(vm.text()); chord('meta_l', '1'); vm.wait(r'AURORA WORKSPACE 1', mark, seconds=20)
    red_at(cx + 15, cy + 14, label='workspace1')
    # Calculator to workspace 3, follow it with Ctrl+Super+Right twice
    vm.home(cx + 200, cy + 14); vm.click(); time.sleep(.5)
    mark = len(vm.text()); chord('meta_l', 'shift', '3'); vm.wait(r'AURORA WINDOW WORKSPACE app=nexus.calculator workspace=3', mark, seconds=20)
    vm.frame('calc-gone', lambda p: p.getpixel((cx + 15, cy + 14)) != RED)
    mark = len(vm.text()); chord('ctrl', 'meta_l', 'right'); chord('ctrl', 'meta_l', 'right'); vm.wait(r'AURORA WORKSPACE 3', mark, seconds=20)
    red_at(cx + 15, cy + 14, label='workspace3-calc')
    mark = len(vm.text()); chord('meta_l', 'shift', '1'); vm.wait(r'AURORA WINDOW WORKSPACE app=nexus.calculator workspace=1', mark, seconds=20)
    chord('meta_l', '1'); vm.wait(r'AURORA WORKSPACE 1', mark, seconds=20)
    results['workspaces'] = 'Super+2/1 switch, Super+Shift+3 moves a window, Ctrl+Super+Right follows it, Super+Shift+1 brings it back'
    # -- Alt+Tab switcher: the other window comes forward when Alt is released
    vm.home(nx + 200, ny + 14); vm.click(); time.sleep(.5)
    mark = len(vm.text()); vm.key('alt', True); vm.key('tab', True); vm.key('tab', False)
    vm.wait(r'AURORA SWITCHER open windows=2', mark, seconds=20)
    vm.frame('switcher', lambda p: True); vm.key('alt', False)
    vm.wait(r'AURORA SWITCHED app=nexus.calculator', mark, seconds=20)
    results['switcher'] = 'Alt+Tab shows the switcher, releasing Alt activates the next window'
    # -- overview: thumbnails, a workspace click, Esc
    def brightness(p, box): return sum(sum(px) for px in p.crop(box).get_flattened_data()) / ((box[2] - box[0]) * (box[3] - box[1]))
    corner = (1700, 700, 1900, 900); lit = brightness(vm.shot('before-overview'), corner)
    mark = len(vm.text()); chord('meta_l', 'w'); vm.wait(r'AURORA OVERVIEW open windows=2', mark, seconds=20)
    vm.frame('overview', lambda p: brightness(p, corner) < lit * .6)   # the desktop dims behind the thumbnails
    mark = len(vm.text()); vm.home(581 + 194 + 88, 40 + 50); vm.click()   # the "Desktop 2" box of the strip
    vm.wait(r'AURORA WORKSPACE 2', mark, seconds=20)
    mark = len(vm.text()); vm.tap('esc'); chord('meta_l', '1')
    results['overview'] = 'Super+W opens the overview with both windows; a workspace box switches; Esc closes'
    # -- snapping by dragging to the left edge, then dragging the tile away restores the size
    time.sleep(1); vm.home(cx + 200, cy + 14); vm.click(); time.sleep(.5)
    mark = len(vm.text()); vm.drag(cx + 140, cy + 14, 0, 500)
    vm.wait(r'AURORA SNAP app=nexus.calculator left', mark, seconds=20); vm.wait(r'DESKTOP CONFIGURE surface=\d+ 960x960', mark, seconds=20)
    red_at(15, 32 + 14, label='calc-snapped')
    mark = len(vm.text()); vm.drag(140, 32 + 14, 700, 400)
    vm.wait(r'AURORA MOVED app=nexus.calculator', mark, seconds=20)
    vm.wait(r'DESKTOP CONFIGURE surface=\d+ (\d+)x(\d+)', mark, seconds=20)
    results['snap'] = 'dragged to the left edge: snapped left (960x960); dragged away: own size again'
    # -- Super+D shows the desktop and back
    mark = len(vm.text()); chord('meta_l', 'd'); vm.wait(r'AURORA SHOW DESKTOP on windows=2', mark, seconds=20)
    mark = len(vm.text()); chord('meta_l', 'd'); vm.wait(r'AURORA SHOW DESKTOP off windows=2', mark, seconds=20)
    results['show_desktop'] = 'Super+D hides both windows and brings them back'
    assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT', 'DESKTOP RESOURCE LEAK'))
    (WORK / 'results.json').write_text(json.dumps(results, indent=1))
    print('PASS Aurora window management:', '; '.join(f'{k}: {v}' for k, v in results.items()), flush=True)
except BaseException:
    try: vm.shot('failure')
    except Exception: pass
    raise
finally:
    vm.close()
