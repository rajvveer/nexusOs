"""Rootful Xwayland inside Aurora: the "X11 Desktop" launcher, X pixels on screen, X keyboard and pointer input.

    python -I tests/test_xwayland.py WORK [--source DESKTOP64.img] [--disk PREPARED.img] [--qmp-port 4611]
           [--accel auto|whpx|tcg] [--keep-disk] [--cache DIR]

Without --disk, WORK/disk.img is a sparse copy of --source with Debian's Xwayland, xkbcomp, dash, xterm, x11-apps and
xdotool installed by arch/x86_64/wine_install.py --only xwayland,x11-tools (pinned in wine-lock.json), which also adds
/usr/bin/nexus-x11-session, /usr/bin/nexus-xtest and /nexus/apps/x11.args. out/desktop.elf becomes /sbin/init. For the
run, /nexus/apps/x11.args starts the session with xeyes and nexus-xtest (a core-protocol client that logs its events).
The guest boots through a qcow2 overlay.

Checks: Spotlight "X11 Desktop" launches the session; Xwayland (:1, 1280x800, rootful) maps one Aurora toplevel;
nexus-xtest's window shows its colour bars at X root 100,100 in the screendump; a PS/2 click at window position 200,100
arrives as an X ButtonPress at 200,100; typed keys arrive as X KeyPress events with the keysyms n,e,x,u,s (XKB keymap
from Aurora compiled by xkbcomp); 'q' ends the client; Alt+F4 closes the Xwayland window and ends the session;
no kernel or user-mode fault.
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--disk', type=Path); ap.add_argument('--qmp-port', type=int, default=4611)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg')); ap.add_argument('--keep-disk', action='store_true')
ap.add_argument('--cache', type=Path, default=nexus_paths.cache('wayland'))
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
OUT = aurora_vm.OUT
started = time.time(); result = {'test': 'xwayland', 'passed': False}
own_disk = args.disk is None
disk = (args.disk or WORK / 'disk.img').resolve()
vm = None
try:
    if own_disk:
        if disk.exists(): disk.unlink()
        aurora_vm.sparse_copy(args.source, disk)
        subprocess.run([sys.executable, '-I', str(ROOT / 'arch/x86_64/wine_install.py'), str(disk), '--only', 'xwayland,x11-tools',
                        '--cache', str(args.cache), '--report', str(WORK / 'install-report.json')], check=True)
    aurora_vm.stage(disk, {'sbin/init': OUT / 'desktop.elf', 'nexus/desktop.args': '--aurora\n',
                           'usr/bin/nexus-x11-session': OUT / 'x11_session_user.elf', 'usr/bin/nexus-xtest': OUT / 'x11_test_user.elf',
                           'nexus/apps/x11.args': '/usr/bin/nexus-x11-session\n/usr/bin/xeyes\n/usr/bin/nexus-xtest\n'})
    vm = aurora_vm.Vm(WORK, disk, args.qmp_port, args.accel)
    result['accel'] = vm.accel
    vm.wait(r'AURORA READY', seconds=240); time.sleep(2)
    before = len(vm.text())
    vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('x11'); time.sleep(1); vm.tap('ret')
    vm.wait(r'AURORA LAUNCH app=x11 pid=\d+', before, 60)
    ready = vm.wait(r'X11 SESSION READY DISPLAY=(:\d+)', before, 180); result['display'] = ready.group(1)
    window = vm.wait(r'AURORA WINDOW app=([^ ]*) title=([^\n]*?) x=(-?\d+) y=(-?\d+)', before, 120)
    result['x_window'] = {'app_id': window.group(1), 'title': window.group(2).strip()}
    wx, wy = int(window.group(3)), int(window.group(4))
    vm.wait(r'XTEST READY window=0x[0-9a-f]+ root=1280x800 x=100 y=100', before, 120)
    time.sleep(2)
    shot = vm.shot('x11')
    def px(x, y): return shot.getpixel((wx + 100 + x, wy + 100 + y))
    expected = {(5, 5): (0, 0x80, 0x80), (50, 100): (255, 0, 0), (150, 100): (0, 255, 0), (250, 100): (0, 0, 255), (350, 100): (255, 255, 255), (200, 250): (0, 0x80, 0x80)}
    for (x, y), color in expected.items():
        assert px(x, y) == color, f'X window pixel {x},{y}: {px(x, y)} != {color}'
    # xeyes draws at the X root origin: its 150x100 window is not all background
    eyes = {shot.getpixel((wx + x, wy + y)) for x in range(0, 150, 5) for y in range(0, 100, 5)}
    assert len(eyes) >= 2, 'xeyes window not visible'
    result['screendump'] = 'pass'
    # pointer: click at window position 200,100
    vm.move_to(wx + 300, wy + 200); time.sleep(.5)
    mark = len(vm.text()); vm.click()
    vm.wait(r'XTEST BUTTON press button=1 x=200 y=100', mark, 30)
    vm.wait(r'XTEST BUTTON release button=1 x=200 y=100', mark, 30)
    # keyboard
    mark = len(vm.text()); vm.type('nexus')
    for sym in (0x6e, 0x65, 0x78, 0x75, 0x73):
        found = vm.wait(r'XTEST KEY press keycode=(\d+) keysym=0x%x' % sym, mark, 30); mark += found.end()
    result['keys'] = 'pass'
    vm.tap('q'); vm.wait(r'XTEST QUIT', seconds=30)
    vm.wait(r'X11 SESSION CLIENT /usr/bin/nexus-xtest exited status=0', seconds=30)
    # closing the X window (Alt+F4 -> xdg_toplevel.close) ends Xwayland and the session
    vm.move_to(wx + 640, wy + 400); vm.click(); time.sleep(.5)
    vm.key('alt', True); vm.tap('f4'); vm.key('alt', False)
    end = vm.wait(r'X11 SESSION (END|Xwayland exited status=\d+)', seconds=60)
    result['close'] = end.group(0)
    faults = vm.faults(); assert not faults, faults
    result['log'] = re.findall(r'\[64\] (?:X11 SESSION|XTEST) [^\n]*', vm.text())[:60]
    result['passed'] = True
finally:
    if vm: vm.close()
    result['seconds'] = round(time.time() - started, 1)
    (WORK / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    if own_disk and not args.keep_disk: disk.unlink(missing_ok=True)
print('PASS rootful Xwayland in Aurora' if result['passed'] else 'FAIL', json.dumps({k: result.get(k) for k in ('accel', 'display', 'x_window', 'seconds')}))
