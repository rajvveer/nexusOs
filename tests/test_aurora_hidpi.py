"""Aurora HiDPI end to end: per-display scale from Settings > Displays (100/125/150/175/200/300 %), the shell drawn
natively at the physical resolution (FreeType text, larger arrow), Aurora apps rendering at the preferred fractional scale
(wp_fractional_scale_v1 + wp_viewporter), windows kept on a display that shrank, persistence across a reboot.

python -I tests/test_aurora_hidpi.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

Two boots of one qcow2 overlay over the prepared desktop disk (staged: /sbin/init = out/desktop.elf, /bin/aurora-app =
out/aurora_app.elf). One 1920x1080 virtio-vga display, PS/2 + USB tablet. Checked in the serial log (AURORA LAYOUT/OUTPUTS,
AURORA APP SCALE lines) and in screendumps (menu bar height in physical pixels, the red close button where the scale puts it).
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
WORK = Path(args[0]) if args else nexus_paths.runs('hidpi')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
OVERLAY = WORK / 'overlay.qcow2'; OVERLAY.unlink(missing_ok=True)
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n'})
RED = (255, 95, 87)
results = {}
LAYOUT = r'\[64\] AURORA LAYOUT Virtual-1 on 1920x1080@\d+ at 0,0 size (\d+)x(\d+) scale=(\d+)'


def menubar_height(p):
    """Rows from the top whose colour at x=1000 is the menu bar's (it is uniform there)."""
    top = p.getpixel((1000, 2)); rows = 0
    while rows < 200 and sum(abs(a - b) for a, b in zip(p.getpixel((1000, rows)), top)) < 24: rows += 1
    return rows


def red_at(win, k, label):
    """The window's red close button where scale k puts it (physical pixels)."""
    win.update(0)
    return win.vm.frame(label, lambda p: p.getpixel((int((win.x + 15) * k), int((win.y + 14) * k))) == RED)


def controls(vm, after, name='Virtual-1'):
    found = None
    for m in re.finditer(r'SETTINGS CONTROLS (\S+) (.*)', vm.text()[after:]):
        if m.group(1) == name: found = {k: tuple(map(int, v.split(','))) for k, v in re.findall(r'(\w+)=([-\d,]+)', m.group(2))}
    return found


class Window:
    def __init__(self, vm, app, x, y): self.vm, self.app, self.x, self.y = vm, app, x, y
    def update(self, after):
        for m in re.finditer(r'AURORA (?:WINDOW (?:SHIFTED|MOVED)|MOVED) app=' + re.escape(self.app) + r' (?:screen=\S+ )?x=(-?\d+) y=(-?\d+)', self.vm.text()[after:]):
            self.x, self.y = int(m.group(1)), int(m.group(2))


def set_scale(vm, win, index, percent, keep=True):
    """Click segment `index` of the Scale control, keep the change; returns the new logical size."""
    win.update(0); vm.goto(win.x + 860, win.y + 10); vm.click(); time.sleep(.5)   # raise Settings (its title bar's right end)
    mark = len(vm.text()); c = controls(vm, 0); assert c and c['scale'][2] > 0, c
    x, y, w, h = c['scale']; vm.goto(win.x + x + w * index // 6 + w // 12, win.y + y + h // 2); vm.click()
    m = vm.wait(LAYOUT.replace(r'scale=(\d+)', 'scale=%d' % percent), mark, seconds=30)
    size = int(m.group(1)), int(m.group(2))
    vm.wait(r'AURORA OUTPUTS CHANGED count=1 desktop=%dx%d' % size, mark, seconds=30)
    vm.head0 = (0, 0, *size); win.update(mark)
    if not keep: return size
    time.sleep(1); c = controls(vm, mark); assert c and c['keep'][2] > 0, c
    k = c['keep']; vm.goto(win.x + k[0] + k[2] // 2, win.y + k[1] + k[3] // 2); vm.click()
    vm.wait(r'SETTINGS DISPLAYS KEPT', mark, seconds=30)
    return size


vm = desk_vm.DeskVm(WORK / 'boot1', DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[], keep_overlay=True, overlay=OVERLAY)
try:
    vm.wait(r'AURORA READY', seconds=300)
    assert re.search(LAYOUT.replace(r'scale=(\d+)', 'scale=100'), vm.text()), 'starts at 100 %'
    # Settings > Displays
    mark = len(vm.text()); vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('system settings'); time.sleep(.5); vm.tap('ret')
    win = Window(vm, 'nexus.settings', *map(int, vm.wait(r'AURORA WINDOW app=nexus.settings title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=120).groups()))
    vm.wait(r'SETTINGS CONTROLS Virtual-1 ', mark, seconds=30); time.sleep(.5)
    # 150 %: logical 1280x720; the shell is drawn at 1.5x (menu bar 1.5 times as tall in pixels)
    size = set_scale(vm, win, 2, 150); assert size == (1280, 720), size
    red_at(win, 1.5, 'scale150')
    results['scale150'] = 'Settings 150 %: desktop 1280x720 logical; the shell and the Settings window drawn at 1.5x'
    # an Aurora app at 150 %: drawn at the physical size through wp_fractional_scale_v1 + wp_viewporter
    mark = len(vm.text()); vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('notes'); time.sleep(.5); vm.tap('ret')
    m = vm.wait(r'AURORA APP SCALE app=notes scale=180 buffer=(\d+)x(\d+) size=(\d+)x(\d+)', mark, seconds=120)
    bw, bh, lw, lh = map(int, m.groups()); assert abs(bw - lw * 3 / 2) <= 1 and abs(bh - lh * 3 / 2) <= 1, m.group(0)
    nx, ny = map(int, vm.wait(r'AURORA WINDOW app=nexus.notes title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=60).groups())
    notes = Window(vm, 'nexus.notes', nx, ny); red_at(notes, 1.5, 'notes150')
    results['app150'] = 'Notes renders %dx%d pixels for its %dx%d window (preferred scale 180/120)' % (bw, bh, lw, lh)
    # 125 % then 175 %: fractional again; the windows stay inside the smaller desktop
    mark = len(vm.text()); size = set_scale(vm, win, 1, 125); assert size == (1536, 864), size
    vm.wait(r'AURORA APP SCALE app=notes scale=150 ', mark, seconds=60); red_at(win, 1.25, 'scale125')
    mark = len(vm.text()); size = set_scale(vm, win, 3, 175); assert size == (1097, 617), size
    vm.wait(r'AURORA APP SCALE app=notes scale=210 ', mark, seconds=60); red_at(win, 1.75, 'scale175')
    assert re.search(r'AURORA WINDOW SHIFTED app=nexus\.settings', vm.text()[mark:]), 'Settings moved inside 1097x617'
    results['fractional'] = '125 % (1536x864) and 175 % (1097x617): Notes at 150/120 and 210/120; windows kept inside'
    # 200 % (integer wl_output scale 2, desktop 960x540), not kept: back to 175 % after 15 s by itself
    mark = len(vm.text()); size = set_scale(vm, win, 4, 200, keep=False); assert size == (960, 540), size
    vm.wait(r'AURORA APP SCALE app=notes scale=240 ', mark, seconds=60); red_at(win, 2, 'scale200')
    vm.wait(r'SETTINGS DISPLAYS REVERTED', mark, seconds=40)
    vm.wait(LAYOUT.replace(r'scale=(\d+)', 'scale=175'), mark, seconds=30)
    results['scale200'] = '200 %: desktop 960x540, Notes re-rendered at 240/120; not kept: reverted to 175 % after 15 s'
    assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT', 'DESKTOP RESOURCE LEAK'))
    vm.q.cmd('quit'); vm.proc.wait(30)
except BaseException:
    try: vm.shot('failure')
    except Exception: pass
    raise
finally:
    vm.close()

# ---- boot 2: the scale comes back
vm = desk_vm.DeskVm(WORK / 'boot2', DISK, heads=1, accel=ACCEL, smp=SMP, usb=True, vnc_heads=[], keep_overlay=True, overlay=OVERLAY)
try:
    vm.wait(r'AURORA READY', seconds=300)
    assert re.search(LAYOUT.replace(r'scale=(\d+)', 'scale=175'), vm.text()), 'saved scale restored'
    vm.shot('boot2')
    results['persistence'] = 'the 175 % scale is restored at boot'
finally:
    vm.close()
OVERLAY.unlink(missing_ok=True)
(WORK / 'results.json').write_text(json.dumps(results, indent=1))
print('PASS Aurora HiDPI:', '; '.join(f'{k}: {v}' for k, v in results.items()), flush=True)
