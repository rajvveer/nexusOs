"""Aurora multi-monitor end to end: hot-plug, extended desktop, windows across displays, Settings > Displays, persistence.

python -I tests/test_aurora_displays.py [WORK] [--disk DESKTOP64.img] [--tcg] [--smp N]

Two boots of one qcow2 overlay over the prepared desktop disk (the disk itself is only staged: /sbin/init = out/desktop.elf,
/bin/aurora-app = out/aurora_app.elf). QEMU: virtio-vga with 2 heads (EDID), PS/2 + USB tablet (absolute pointer on head 0),
head 1 hot-plugged/unplugged through its VNC server (RFB SetDesktopSize -> QEMU UI resize). Checks QMP screendumps of each
head and the serial log:
 boot 1: head 0 alone (boot framebuffer kept); plug head 1 (1280x720): second display right of the first, wallpaper,
         no shell there; Calculator dragged onto head 1 and maximized there (1280x720); unplug: it moves to head 0 and is
         re-maximized (1920x960 work area); replug at 1024x768; Settings > Displays: drag head 1 left of head 0 (snapped;
         windows stay on their display), make it the main display (menu bar and dock move), mirror it on head 0, extend
         again, step head 1's resolution and keep it (and a second change reverts by itself after 15 s).
 boot 2: head 1 present from the start: the saved layout (positions, main display, mode) is restored.
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
WORK = Path(args[0]) if args and not args[0].endswith('.img') else nexus_paths.runs('displays')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
SMP = int(sys.argv[sys.argv.index('--smp') + 1]) if '--smp' in sys.argv else 2
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
(WORK / 'overlay.qcow2').unlink(missing_ok=True)
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n'})
RED = (255, 95, 87)
results = {}


def varied(p, box=None):
    return len((p.crop(box) if box else p).getcolors(1 << 20) or []) > 100


def layout(vm, after=0):
    result = {}
    for m in re.finditer(r'\[64\] AURORA LAYOUT (\S+) (on|off) (\d+)x(\d+)@(\d+) at (-?\d+),(-?\d+) size (\d+)x(\d+) scale=(\d+) primary=(\d) mirror=(\S+)', vm.text()[after:]):
        result[m.group(1)] = {'on': m.group(2) == 'on', 'mode': (int(m.group(3)), int(m.group(4))), 'pos': (int(m.group(6)), int(m.group(7))),
                              'size': (int(m.group(8)), int(m.group(9))), 'scale': int(m.group(10)), 'primary': m.group(11) == '1', 'mirror': m.group(12)}
    return result


def track(vm, lay):
    """Keep the harness's idea of head 0 (the USB tablet's display; when it mirrors, its source) in step with the layout."""
    v1 = lay['Virtual-1']
    if v1['mirror'] != '-': v1 = lay[v1['mirror']]
    vm.head0 = (*v1['pos'], *v1['size'])


def settings_rects(vm, after):
    rects = {}
    for m in re.finditer(r'SETTINGS LAYOUT (\S+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)', vm.text()[after:]): rects[m.group(1)] = tuple(map(int, m.groups()[1:]))
    return rects


def controls(vm, after, name):
    found = None
    for m in re.finditer(r'SETTINGS CONTROLS (\S+) (.*)', vm.text()[after:]):
        if m.group(1) == name: found = {k: tuple(map(int, v.split(','))) for k, v in re.findall(r'(\w+)=([-\d,]+)', m.group(2))}
    return found


class Window:
    """The Settings window's desktop position (follows AURORA WINDOW / SHIFTED / MOVED lines)."""
    def __init__(self, vm, app, x, y): self.vm, self.app, self.x, self.y = vm, app, x, y
    def update(self, after):
        for m in re.finditer(r'AURORA (?:WINDOW (?:SHIFTED|MOVED)|MOVED) app=' + re.escape(self.app) + r' (?:screen=\S+ )?x=(-?\d+) y=(-?\d+)', self.vm.text()[after:]):
            self.x, self.y = int(m.group(1)), int(m.group(2))


def click_rect(vm, win, rect, dx=None):
    x, y, w, h = rect; vm.goto(win.x + x + (w // 2 if dx is None else dx), win.y + y + h // 2); vm.click()


OVERLAY = WORK / 'overlay.qcow2'   # shared by both boots: the second sees what the first saved
vm = desk_vm.DeskVm(WORK / 'boot1', DISK, heads=2, accel=ACCEL, smp=SMP, usb=True, keep_overlay=True, overlay=OVERLAY)
try:
    vm.wait(r'AURORA READY', seconds=300)
    lay = layout(vm)
    assert lay['Virtual-1'] == {'on': True, 'mode': (1920, 1080), 'pos': (0, 0), 'size': (1920, 1080), 'scale': 100, 'primary': True, 'mirror': '-'}, lay
    vm.frame('head0-alone', lambda p: p.size == (1920, 1080) and varied(p, (400, 300, 1500, 900)))
    # -- hot-plug head 1
    mark = len(vm.text()); vm.head_size(1, 1280, 720)
    vm.wait(r'AURORA OUTPUTS CHANGED count=2 desktop=3200x1080', mark, seconds=60)
    lay = layout(vm, mark); assert lay['Virtual-2']['on'] and lay['Virtual-2']['pos'] == (1920, 0) and not lay['Virtual-2']['primary'], lay
    head1 = vm.frame('head1-plugged', lambda p: p.size == (1280, 720) and varied(p), head=1)
    head0 = vm.shot('head0-with-head1')
    assert head0.getpixel((960, 1040)) != head1.getpixel((640, 700)) or head0.getpixel((300, 6)) != head1.getpixel((300, 6)), 'head 1 must show no menu bar or dock'
    results['hotplug'] = 'head 1 lit up right of head 0'
    # -- a window dragged onto head 1, maximized there
    mark = len(vm.text()); vm.home(1120, 1040); vm.click()
    x, y = map(int, vm.wait(r'AURORA WINDOW app=nexus.calculator title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=120).groups())
    vm.frame('calc-head0', lambda p: p.getpixel((x + 15, y + 14)) == RED)
    mark = len(vm.text()); vm.drag(x + 140, y + 14, 1920 + 300, 200)
    nx, ny = map(int, vm.wait(r'AURORA MOVED app=nexus.calculator x=(-?\d+) y=(-?\d+) screen=Virtual-2', mark, seconds=30).groups())
    vm.frame('calc-head1', lambda p: p.getpixel((nx - 1920 + 15, ny + 14)) == RED, head=1)
    mark = len(vm.text()); vm.move_to(nx + 57, ny + 14); vm.click()
    vm.wait(r'DESKTOP CONFIGURE surface=\d+ 1280x720', mark, seconds=30)
    vm.frame('calc-max-head1', lambda p: p.getpixel((15, 14)) == RED, head=1)
    results['window_across'] = 'Calculator dragged onto head 1 and maximized there (1280x720)'
    # -- unplug: the window comes back to head 0 and is maximized in its work area
    mark = len(vm.text()); vm.head_size(1, 0, 0)
    vm.wait(r'AURORA WINDOW MOVED app=nexus.calculator screen=Virtual-1', mark, seconds=60)
    vm.wait(r'DESKTOP CONFIGURE surface=\d+ 1920x960', mark, seconds=30)
    vm.frame('calc-back-head0', lambda p: p.getpixel((15, 32 + 14)) == RED)
    results['unplug'] = 'the maximized window moved to head 0 and fills its work area (1920x960)'
    mark = len(vm.text()); vm.tap('alt', 'f4'); vm.wait(r'AURORA APP CLOSED app=calculator status=0', mark, seconds=30)
    # -- replug at 1024x768, then Settings > Displays
    mark = len(vm.text()); vm.head_size(1, 1024, 768)
    vm.wait(r'AURORA OUTPUTS CHANGED count=2 desktop=2944x1080', mark, seconds=60)
    mark = len(vm.text()); vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('system settings'); time.sleep(.5); vm.tap('ret')
    win = Window(vm, 'nexus.settings', *map(int, vm.wait(r'AURORA WINDOW app=nexus.settings title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, seconds=120).groups()))
    vm.wait(r'SETTINGS LAYOUT Virtual-2 ', mark, seconds=30); time.sleep(.5)
    rects = settings_rects(vm, mark); r1, r2 = rects['Virtual-1'], rects['Virtual-2']
    vm.shot('settings-start')
    # drag head 1's rectangle to the left of head 0's: it snaps edge to edge, the Settings window stays on head 0
    mark = len(vm.text())
    vm.drag(win.x + r2[0] + r2[2] // 2, win.y + r2[1] + r2[3] // 2, win.x + r1[0] - r2[2] // 2 - 8, win.y + r1[1] + r2[3] // 2)
    vm.wait(r'SETTINGS DISPLAY MOVED Virtual-2 x=-1024 y=0', mark, seconds=30)
    vm.wait(r'AURORA OUTPUTS CHANGED count=2 desktop=2944x1080', mark, seconds=30)
    lay = layout(vm, mark); assert lay['Virtual-2']['pos'] == (0, 0) and lay['Virtual-1']['pos'] == (1024, 0), lay
    track(vm, lay); win.update(mark); vm.pos = [vm.pos[0] + 1024, vm.pos[1]]
    assert win.x >= 1024, ('the Settings window must stay on Virtual-1', win.x)
    vm.frame('settings-after-arrange', lambda p: p.getpixel((win.x - 1024 + 15, win.y + 14)) == RED)
    results['arrange'] = 'Virtual-2 dragged left of Virtual-1, snapped; windows stayed on their display'
    # make head 1 the main display: the menu bar and dock move there
    vm.wait(r'SETTINGS LAYOUT Virtual-2 x=', mark, seconds=30); time.sleep(.5)
    click_rect(vm, win, settings_rects(vm, mark)['Virtual-2'])
    time.sleep(1); c = controls(vm, mark, 'Virtual-2'); assert c and c['main'][2] > 0, c
    before0, before1 = vm.shot('head0-before-main'), vm.shot('head1-before-main', head=1)
    dock0, dock1 = (960 - 300, 1000, 960 + 300, 1075), (512 - 300, 690, 512 + 300, 765)
    mark = len(vm.text()); click_rect(vm, win, c['main'])
    vm.wait(r'AURORA OUTPUTS CHANGED', mark, seconds=30)
    lay = layout(vm, mark); assert lay['Virtual-2']['primary'] and not lay['Virtual-1']['primary'], lay
    after1 = vm.frame('head1-main', lambda p: p.crop(dock1).tobytes() != before1.crop(dock1).tobytes(), head=1)   # the dock appears there
    after0 = vm.frame('head0-not-main', lambda p: p.crop(dock0).tobytes() != before0.crop(dock0).tobytes())       # and leaves head 0
    assert after1.crop((0, 0, 1024, 28)).tobytes() != before1.crop((0, 0, 1024, 28)).tobytes(), 'the menu bar moved to head 1'
    results['main_display'] = 'Virtual-2 became the main display: menu bar and dock moved there'
    # mirror: head 0 shows head 1 (scaled 1024x768 -> 1920x1080); windows on head 0 move to head 1
    mark = len(vm.text()); win.update(0); click_rect(vm, win, settings_rects(vm, 0)['Virtual-1'])
    time.sleep(1); c = controls(vm, mark, 'Virtual-1'); assert c and c['use'][2] > 0, c
    mark = len(vm.text()); click_rect(vm, win, c['use'], dx=c['use'][2] * 3 // 4)
    vm.wait(r'AURORA OUTPUTS CHANGED', mark, seconds=30)
    lay = layout(vm, mark); assert lay['Virtual-1']['mirror'] == 'Virtual-2' and lay['Virtual-1']['pos'] == lay['Virtual-2']['pos'], lay
    vm.wait(r'AURORA WINDOW MOVED app=nexus.settings screen=Virtual-2', mark, seconds=30); win.update(mark); track(vm, lay)
    time.sleep(1.5)
    m0, m1 = vm.shot('mirror-head0'), vm.shot('mirror-head1', head=1)
    samples = [(100, 100), (512, 384), (900, 700), (300, 600), (win.x + 15, win.y + 14)]
    same = sum(m0.getpixel((x * 1920 // 1024, y * 1080 // 768)) == m1.getpixel((x, y)) for x, y in samples)
    assert same >= 4, ('head 0 must mirror head 1', same)
    results['mirror'] = 'Virtual-1 mirrors Virtual-2 (scaled 1024x768 -> 1920x1080); its windows moved to Virtual-2'
    # extended again
    vm.wait(r'SETTINGS LAYOUT Virtual-2 x=', mark, seconds=30); time.sleep(.5)
    sel = controls(vm, 0, 'Virtual-2')['select']   # the display selector (a mirror is not in the arrangement)
    vm.goto(win.x + sel[0] + sel[2] // 4, win.y + sel[1] + sel[3] // 2); vm.click(); time.sleep(1)   # segment 1 of 2: Virtual-1
    assert re.findall(r'SETTINGS CONTROLS (\S+)', vm.text())[-1] == 'Virtual-1', 'Virtual-1 selected'
    c = controls(vm, 0, 'Virtual-1'); assert c and c['use'][2] > 0, c
    mark = len(vm.text()); click_rect(vm, win, c['use'], dx=c['use'][2] // 4)
    vm.wait(r'AURORA OUTPUTS CHANGED', mark, seconds=30)
    lay = layout(vm, mark); assert lay['Virtual-1']['mirror'] == '-' and lay['Virtual-1']['pos'] == (1024, 0), lay
    track(vm, lay)
    results['extend'] = 'Virtual-1 extended again, right of Virtual-2'
    # a resolution change that is kept, then one that reverts by itself
    vm.wait(r'SETTINGS LAYOUT Virtual-1 x=', mark, seconds=30); time.sleep(.5)
    click_rect(vm, win, settings_rects(vm, mark)['Virtual-2']); time.sleep(1)
    c = controls(vm, 0, 'Virtual-2'); assert c and c['prev'][2] > 0, c
    mark = len(vm.text()); click_rect(vm, win, c['prev'])   # one step smaller: 800x600
    vm.wait(r'SETTINGS DISPLAYS APPLIED \(confirm\)', mark, seconds=30); vm.wait(r'AURORA OUTPUTS CHANGED', mark, seconds=30)
    lay = layout(vm, mark); assert lay['Virtual-2']['mode'] == (800, 600) and lay['Virtual-1']['pos'] == (800, 0), lay
    track(vm, lay); win.update(mark)
    vm.frame('head1-800', lambda p: p.size == (800, 600), head=1)
    time.sleep(1); c = controls(vm, mark, 'Virtual-2'); assert c and c['keep'][2] > 0, c
    mark = len(vm.text()); click_rect(vm, win, c['keep']); vm.wait(r'SETTINGS DISPLAYS KEPT', mark, seconds=30)
    mark = len(vm.text()); time.sleep(.5); c = controls(vm, 0, 'Virtual-2'); click_rect(vm, win, c['prev'])   # 640x480, not kept
    vm.wait(r'SETTINGS DISPLAYS APPLIED \(confirm\)', mark, seconds=30)
    vm.wait(r'SETTINGS DISPLAYS REVERTED', mark, seconds=40)
    lay = layout(vm, mark); assert lay['Virtual-2']['mode'] == (800, 600), lay
    track(vm, lay)
    results['resolution'] = 'Virtual-2 800x600 kept; 640x480 reverted by itself after 15 s'
    vm.q.cmd('quit'); vm.proc.wait(30)
except BaseException:
    try: vm.shot('failure-head0'); vm.shot('failure-head1', head=1); print('pointer (tracked)', vm.pos, 'head0', vm.head0, flush=True)
    except Exception: pass
    raise
finally:
    vm.close()

# ---- boot 2: the saved layout comes back
vm = desk_vm.DeskVm(WORK / 'boot2', DISK, heads=2, accel=ACCEL, smp=SMP, usb=True, keep_overlay=True, overlay=OVERLAY, plug={1: (1024, 768)})
try:
    vm.wait(r'AURORA READY', seconds=300)
    lay = layout(vm)
    assert lay['Virtual-2']['primary'] and lay['Virtual-2']['mode'] == (800, 600) and lay['Virtual-2']['pos'] == (0, 0), lay
    assert lay['Virtual-1']['pos'] == (800, 0) and lay['Virtual-1']['mode'] == (1920, 1080) and not lay['Virtual-1']['primary'], lay
    vm.frame('boot2-head1', lambda p: p.size == (800, 600) and varied(p, (100, 560, 700, 600)), head=1)
    results['persistence'] = 'layout restored at boot: Virtual-2 main 800x600 at 0,0, Virtual-1 at 800,0'
    assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT', 'DESKTOP RESOURCE LEAK'))
finally:
    vm.close()
(WORK / 'overlay.qcow2').unlink(missing_ok=True)
(WORK / 'results.json').write_text(json.dumps(results, indent=1))
print('PASS Aurora displays:', '; '.join(f'{k}: {v}' for k, v in results.items()), flush=True)
