"""Real applications on a 150 % display: Chrome, Firefox (GTK 3), the rootful Xwayland desktop and Wine's winecfg.

python -I tests/test_aurora_hidpi_apps.py [WORK] [--disk DESKTOP64.img] [--tcg]

The desktop disk needs apps_install.py and wine_install.py --only wine,xwayland. One boot through a qcow2 overlay; the
display goes to 150 % ("aurora-app ctl scale Virtual-1 150" from the Terminal); each application is started from
Spotlight, its window must map, and DESKTOP SCALE lines record how it renders (buffer size, wl_surface buffer scale,
wp_viewport destination: a fractional-scale client draws at the physical size); a screendump of each is kept.
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
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('hidpi_apps')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n'})
APPS = [('chrome', 'Chrome', r'google-chrome'), ('firefox', 'Firefox', r'firefox'), ('x11', 'X11 Desktop', r'org\.freedesktop\.Xwayland'),
        ('winecfg', 'Wine Configuration', r'winecfg\.exe')]
results = {}
vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=2, usb=True, vnc_heads=[], memory='3072')
try:
    vm.wait(r'AURORA READY', seconds=300); time.sleep(2)
    mark = len(vm.text()); vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('terminal'); time.sleep(.5); vm.tap('ret')
    vm.wait(r'AURORA WINDOW app=nexus.terminal', mark, seconds=120); time.sleep(2)
    mark = len(vm.text()); vm.type('aurora-app ctl scale Virtual-1 150\n')
    vm.wait(r'AURORA LAYOUT Virtual-1 on 1920x1080@\d+ at 0,0 size 1280x720 scale=150', mark, seconds=30)
    vm.head0 = (0, 0, 1280, 720); time.sleep(2)
    for name, query, app in APPS:
        mark = len(vm.text())
        for attempt in range(3):   # the first key after a closed window can be lost: retry Spotlight
            vm.tap('ctrl', 'spc'); time.sleep(1.5); vm.type(query.lower()); time.sleep(1); vm.tap('ret')
            try: vm.wait(r'AURORA (?:LAUNCH|RESTORE) app=%s' % name, mark, seconds=15); break
            except TimeoutError: vm.tap('esc'); time.sleep(1)
        try: win = vm.wait(r'AURORA WINDOW app=(%s) title=([^\n]*) x=(-?\d+) y=(-?\d+)' % app, mark, seconds=300)
        except TimeoutError: results[name] = 'FAILED: no window'; vm.shot(name + '-missing'); continue
        time.sleep(15 if name in ('chrome', 'firefox') else 8)
        vm.shot(name + '-150')
        scales = re.findall(r'DESKTOP SCALE surface=\d+ app=%s buffer=(\d+)x(\d+) buffer_scale=(\d+) viewport=(\d+)x(\d+)' % app, vm.text()[mark:])
        configures = re.findall(r'DESKTOP CONFIGURE surface=\d+ (\d+)x(\d+)', vm.text()[mark:])
        last = scales[-1] if scales else None
        how = ('buffer %sx%s, buffer scale %s, viewport %sx%s' % last) if last else 'no buffer log'
        results[name] = 'window "%s" mapped at 150 %%; %s; configured %s' % (win.group(2), how, 'x'.join(configures[-1]) if configures else '?')
        x, y = int(win.group(3)), int(win.group(4))
        vm.home(min(max(x + 200, 0), 1279), min(max(y + 12, 0), 719)); vm.click(); time.sleep(.5); vm.key('alt', True); vm.tap('f4'); vm.key('alt', False); time.sleep(4)
    assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT'))
    failed = [k for k, v in results.items() if v.startswith('FAILED')]
    assert not failed, results
finally:
    vm.close()
(WORK / 'results.json').write_text(json.dumps(results, indent=1))
print('PASS HiDPI applications:', '; '.join(f'{k}: {v}' for k, v in results.items()), flush=True)
