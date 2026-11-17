"""Aurora's Wayland globals for Wine (winewayland.drv) and Xwayland, end to end on QEMU.

    python -I tests/test_aurora_wine_protocols.py WORK [--source DESKTOP64.img] [--disk PREPARED.img]
           [--qmp-port 4610] [--accel auto|whpx|tcg] [--no-wayland-info] [--keep-disk]

Without --disk, WORK/disk.img is a sparse copy of --source (default NEXUS_TEST_SOURCE, arch/x86_64/nexus_paths.py)
with Debian's wayland-info installed by arch/x86_64/wine_install.py --only wayland-tools (pinned in wine-lock.json).
out/desktop.elf becomes /sbin/init, out/wine_protocols_user.elf /bin/wptest, and /nexus/desktop.args starts Aurora with
the test client. The guest boots through a qcow2 overlay; the prepared image is deleted afterwards unless --keep-disk.

Checks (client side: arch/x86_64/wine_protocols_user.c reads /dev/fb0; host side: QMP screendumps + serial log):
  * wayland-info lists every Aurora global with the advertised versions (viewporter, pointer constraints, relative
    pointer, xdg-output v3, xdg-decoration) and exits 0;
  * zxdg_output_v1: name Virtual-1 (KMS connector), logical position 0,0 and size = wl_output mode; v3 done through wl_output.done;
  * xdg-decoration: configure(client_side) before the first xdg_surface.configure, set_mode answered with a new
    configure pair; errors already_constructed / invalid_mode / orphaned;
  * wp_viewport: crop + 2x upscale and 2.5x downscale visible in the composed frame (checked in /dev/fb0 and in the
    screendump), errors viewport_exists / bad_value (source, destination) / bad_size / out_of_buffer / no_surface;
  * pointer lock: activates on enter, wl_pointer.motion stops, relative motion totals the PS/2 input exactly, the
    cursor position hint warps the pointer on unlock; already_constrained error;
  * confinement to a 200x100 region: motion clamps at the region edges in both directions;
  * cursor surfaces: a viewport-scaled client cursor is drawn at the hotspot, set_cursor(NULL) hides it, a stale
    serial is ignored; wheel: axis_source(wheel) + axis_discrete(1) + axis(10) + frame; key events, repeat 25/500;
  * no kernel or user-mode fault; the client exits 0 while Aurora keeps running.
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
ap.add_argument('--disk', type=Path); ap.add_argument('--qmp-port', type=int, default=4610)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--no-wayland-info', action='store_true'); ap.add_argument('--keep-disk', action='store_true')
ap.add_argument('--cache', type=Path, default=nexus_paths.cache('wayland'))
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
OUT = aurora_vm.OUT
started = time.time(); result = {'test': 'aurora_wine_protocols', 'passed': False}
GLOBALS = {'wl_compositor': 4, 'wl_subcompositor': 1, 'wl_shm': 2, 'xdg_wm_base': 5, 'wl_seat': 5, 'wl_output': 4,
           'wl_data_device_manager': 3, 'wp_viewporter': 1,   # 3: drag and drop actions (desk2)
           'zwp_pointer_constraints_v1': 1,
           'zwp_relative_pointer_manager_v1': 1, 'zxdg_output_manager_v1': 3, 'zxdg_decoration_manager_v1': 1}

own_disk = args.disk is None
disk = (args.disk or WORK / 'disk.img').resolve()
vm = None
try:
    if own_disk:
        if disk.exists(): disk.unlink()
        aurora_vm.sparse_copy(args.source, disk)
        if not args.no_wayland_info:
            subprocess.run([sys.executable, '-I', str(ROOT / 'arch/x86_64/wine_install.py'), str(disk), '--only', 'wayland-tools',
                            '--cache', str(args.cache), '--report', str(WORK / 'install-report.json')], check=True)
    command = '--aurora\n/bin/wptest\n' + ('' if args.no_wayland_info else '--wayland-info\n/usr/bin/wayland-info\n')
    aurora_vm.stage(disk, {'sbin/init': OUT / 'desktop.elf', 'bin/wptest': OUT / 'wine_protocols_user.elf', 'nexus/desktop.args': command})
    vm = aurora_vm.Vm(WORK, disk, args.qmp_port, args.accel)
    result['accel'] = vm.accel
    vm.wait(r'AURORA READY', seconds=240)
    if not args.no_wayland_info:
        vm.wait(r'WPTEST WAYLAND-INFO status=0', seconds=120)
        log = vm.text()
        listed = dict((name, int(version)) for name, version in re.findall(r"interface: '([a-z0-9_]+)',\s+version:\s+(\d+)", log))
        result['wayland_info'] = listed
        for name, version in GLOBALS.items():
            assert listed.get(name) == version, f'wayland-info: {name} version {listed.get(name)} != {version}'
        assert 'zxdg_output_v1' in log or 'Virtual-1' in log, 'wayland-info shows no xdg-output name'
    vm.wait(r'WPTEST AUTOMATIC PASSED', seconds=120)
    log = vm.text()
    origin = tuple(int(v) for v in re.search(r'WPTEST ORIGIN x=(\d+) y=(\d+)', log).groups())
    result['origin'] = origin
    shot = vm.shot('viewport')
    ox, oy = origin
    def px(x, y): return shot.getpixel((ox + x, oy + y))
    assert all(px(x, y) == (0x13, 0x57, 0x9B) for x in range(10) for y in range(10)), 'marker block'
    assert px(100, 100) == (255, 0, 0) and px(500, 100) == (0, 0xC0, 0) and px(100, 400) == (0, 0, 255) and px(500, 400) == (128, 128, 128)
    assert px(200, 100) == (0, 0, 0) and px(201, 100) == (0, 0, 0) and px(199, 100) == (255, 0, 0) and px(202, 100) == (255, 0, 0)
    assert px(585, 25) == (255, 0x80, 0) and px(615, 25) == (0x80, 0, 255), 'downscaled subsurface'
    assert all(shot.getpixel((ox + x, oy + y)) != (255, 255, 0) for x in range(0, 640, 7) for y in range(0, 480, 7)), 'cropped border visible'
    result['screendump_viewport'] = 'pass'
    # pointer lock + relative motion
    vm.wait(r'WPTEST POINTER READY')
    vm.move_to(ox + 320, oy + 240)
    vm.wait(r'WPTEST LOCKED')
    for dx, dy in ((20, 10), (20, 10), (30, 45)): vm.move(dx, dy)
    vm.wait(r'WPTEST RELATIVE dx=70 dy=65 motions=0')
    vm.wait(r'WPTEST HINT PASSED x=100 y=80'); vm.pos = [ox + 100, oy + 80]
    # confinement
    vm.wait(r'WPTEST CONFINED')
    vm.move(300, 300, step=30)
    vm.wait(r'WPTEST CONFINE MAX PASSED x=249 y=149'); vm.pos = [ox + 249, oy + 149]
    vm.move(-400, -400, step=30)
    vm.wait(r'WPTEST CONFINE MIN PASSED x=50 y=50'); vm.pos = [ox + 50, oy + 50]
    # client cursors (automatic in the guest); host check of the drawn cursor
    vm.wait(r'WPTEST CURSOR SURFACE PASSED at=(\d+),(\d+)')
    vm.wait(r'WPTEST CURSOR HOTSPOT PASSED')
    cursor = vm.shot('cursor')
    cx, cy = ox + 50, oy + 50
    assert all(cursor.getpixel((cx + x, cy + y)) == (255, 0, 255) for x in range(0, 24, 3) for y in range(0, 24, 3)), 'client cursor in screendump'
    result['screendump_cursor'] = 'pass'
    # wheel + key
    vm.wait(r'WPTEST WHEEL READY')
    vm.button('wheel-down', True); vm.button('wheel-down', False)
    vm.wait(r'WPTEST WHEEL PASSED discrete=1 value=10')
    vm.tap('a')
    vm.wait(r'WPTEST KEY PASSED')
    vm.wait(r'\[64\] WPTEST PASSED')
    vm.wait(r'AURORA CHILD EXIT pid=\d+ status=0', seconds=30)
    faults = vm.faults(); assert not faults, faults
    result['markers'] = re.findall(r'\[64\] WPTEST ([A-Z][^\n]*)', vm.text())
    result['passed'] = True
finally:
    if vm: vm.close()
    result['seconds'] = round(time.time() - started, 1)
    (WORK / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    if own_disk and not args.keep_disk: disk.unlink(missing_ok=True)
print('PASS Aurora Wine/Xwayland protocols' if result['passed'] else 'FAIL', json.dumps({k: result.get(k) for k in ('accel', 'origin', 'seconds')}))
