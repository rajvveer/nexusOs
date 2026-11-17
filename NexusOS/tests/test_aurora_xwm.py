"""Aurora as the X11 window manager of a rootless Xwayland (arch/x86_64/aurora_xwm.c), end to end on QEMU.

    python -I tests/test_aurora_xwm.py [WORK] [--base DESKTOP64.img] [--smp 1,2] [--tcg] [--apps xterm,xeyes,wine,firefox] [--cache DIR]
           [--no-fallback]

The base image (default D:\\nexustest\\prod\\integ_desk_disk\\desktop64.img: Debian's Xwayland 24.1 + xkbcomp) is never
written: one qcow2 overlay gets xterm, x11-apps (xeyes) and xdotool (arch/x86_64/wine_install.py --only x11-tools, the
pinned debs of wine-lock.json, installed into a copy-on-write view of the base), out/desktop.elf as /sbin/init,
aurora-app, /bin/xwmtest (arch/x86_64/xwm_test_user.c) and the new /usr/bin/nexus-x11-session; every boot runs on a
throw-away qcow2 on top of it. /nexus/desktop.args starts "xwmtest run xterm,xeyes": it connects to the display Aurora
publishes ($XDG_RUNTIME_DIR/aurora-x11.env; Xwayland starts on that first connection) and runs its script, printing
"XWMTEST CHECK" lines; steps that need a user print "XWMTEST STEP <name> READY ..." and this test acts over QMP
(USB tablet + keyboard) and checks the screen:
  * the WM is announced (_NET_SUPPORTING_WM_CHECK "Aurora", _NET_SUPPORTED, WM_S0); a window is reparented into an
    Aurora frame (_NET_FRAME_EXTENTS 4,4,28,4, WM_STATE Normal), shown with its UTF-8 title and app id (WM_CLASS) in
    Aurora's window list (dock/switcher), focused on map (FocusIn, _NET_ACTIVE_WINDOW, _NET_CLIENT_LIST); the screen
    shows the title bar with the traffic lights above the X content;
  * focus by click (FocusIn, _NET_ACTIVE_WINDOW, X stacking raised, the click passed on); move by the title bar and
    resize by the corner reach the client as synthetic ConfigureNotify; the client's own resize, WM_NORMAL_HINTS min;
  * _NET_WM_STATE maximize/fullscreen from the client and the green button (the screen is the client's when
    fullscreen, keys still reach it); a WM_TRANSIENT_FOR dialog is centred and stays above its parent (X stacking,
    Aurora's order, screen); an override-redirect menu is drawn at its X position above, unmanaged, clickable, without
    taking the focus;
  * CLIPBOARD X -> Wayland -> X (a Wayland test window reads the X copy, the X window pastes the Wayland copy,
    TARGETS; 400 kB X -> Wayland and 600 kB Wayland -> X through INCR); _NET_WM_ICON shown in the dock; close via
    WM_DELETE_WINDOW (the dialog's red
    button), via XKillClient (Alt+F4 on a window without it);
  * hostile clients (forged WL_SURFACE_SERIAL/ID, bogus EWMH messages, 64 KiB/invalid UTF-8 titles, broken hints,
    transient cycles, absurd sizes) and a client that aborts holding a window and the clipboard: Aurora survives;
  * minimize by WM_CHANGE_STATE and the yellow button (WM_STATE Iconic, _NET_WM_STATE_HIDDEN), restore by
    _NET_ACTIVE_WINDOW; WM_TAKE_FOCUS; Xwayland killed: the next connection starts a new one, which is managed again;
  * real programs: xterm (managed, typed command runs, Alt+F4 closes it), xeyes (drawn in its frame, the close button),
    Wine's notepad with DISPLAY set (winex11: an X window Aurora manages, Alt+F4 closes it), Firefox (GTK 3) with
    GDK_BACKEND=x11 (managed, drawn in its frame, the red light closes it);
  * no kernel/user fault, the Wayland test client never sees xwayland_shell_v1; on 1 and 2 CPUs; then one more boot
    (2 CPUs, no programs) with shell=0 in ~/.aurora/x11.conf: the same script through WL_SURFACE_ID (older Xwayland).
"""
from pathlib import Path
import builtins
import json
import re
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
import aurora_vm  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
sys.stdout.reconfigure(errors='replace')   # guest text (UTF-8 read as Latin-1) on a cp1252 console
argv = sys.argv[1:]
def option(name, default=None):
    if name in argv: i = argv.index(name); value = argv[i + 1]; del argv[i:i + 2]; return value
    return default
BASE = Path(option('--base', r'D:\nexustest\prod\integ_desk_disk\desktop64.img')).resolve()
SMPS = [int(v) for v in option('--smp', '1,2').split(',')]
APPS = option('--apps', 'xterm,xeyes,wine,firefox')
FALLBACK = '--no-fallback' not in argv
if not FALLBACK: argv.remove('--no-fallback')
CACHE = Path(option('--cache', r'C:\nexustest\fix\ws_wayland_pkgs\cache'))
ACCEL = 'tcg' if '--tcg' in argv else 'whpx'
if '--tcg' in argv: argv.remove('--tcg')
WORK = Path(argv[0] if argv else r'D:\nexustest\wave2\ws_desk2_runs\xwm').resolve(); WORK.mkdir(parents=True, exist_ok=True)
checks = []


def check(name, ok, detail=''):
    checks.append((name, bool(ok))); print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def prepare(overlay, apps, extra=None):
    """x11-tools (when apps) + the test files into a copy-on-write view of BASE, written as one qcow2 overlay."""
    xo = aurora_vm.module('xwm_x64_overlay', ROOT / 'tests/x64_overlay.py')
    wi = aurora_vm.module('xwm_wine_install', ROOT / 'arch/x86_64/wine_install.py')
    work = Path(tempfile.mkdtemp(prefix='xwm-stage-', dir=overlay.parent)); view = xo.CowFile(BASE, work / 'fsync.tmp')
    view_close = view.close; view.close = lambda: None   # the tools' closes must not tear the shared view down
    try:
        if apps:
            temp = {'path': None}
            def ai_open(path, mode='r', *a, **k):   # writes (the "working copy" and the disk) go to the view; reads of the copy to the base
                p = str(path)
                if any(c in mode for c in '+wa') and (p == temp['path'] or Path(p).resolve() == BASE): return view
                if p == temp['path']: return builtins.open(BASE, mode, *a, **k)
                return builtins.open(path, mode, *a, **k)
            def no_grow(*a, **k): raise RuntimeError('x11-tools does not fit the base image')
            wi.ai.open = ai_open; wi.os = xo._OsProxy(); wi.ai.sparse_copy = lambda src, dst, length=None: temp.__setitem__('path', str(dst)); wi.ai.grow = no_grow
            rc = wi.main([str(BASE), '--only', 'x11-tools', '--cache', str(CACHE), '--report', str(work / 'x11-tools.json')])
            if rc: raise RuntimeError(f'wine_install --only x11-tools returned {rc}')
        fat = xo._fatput(); fat.open = xo._view_open(view, BASE)
        files = {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'bin/xwmtest': OUT / 'xwm_test_user.elf',
                 'usr/bin/nexus-x11-session': OUT / 'x11_session_user.elf',
                 'nexus/desktop.args': '--aurora\n/bin/xwmtest\nrun\n%s\n' % (apps or 'none'),
                 'home/nexus/.aurora/login.conf': 'autologin=1\nlock_after=0\nlock_on_sleep=1\n', **(extra or {})}
        edits = ['fatput', str(BASE)]
        for i, (guest, host) in enumerate(files.items()):
            if not isinstance(host, Path): src = work / f'file{i}'; src.write_text(host, newline='\n'); host = src
            edits += ['put', str(host), guest]
        if fat.edit(edits): raise RuntimeError('fatput failed')
        xo.snapshot(view, BASE, overlay)
    finally:
        view.close = view_close; view.close()
        for p in sorted(work.rglob('*'), reverse=True): p.unlink() if p.is_file() else p.rmdir()
        if work.exists(): work.rmdir()


def near(a, b, tolerance=24): return all(abs(x - y) <= tolerance for x, y in zip(a, b))
def params(text): return {k: v for k, v in re.findall(r'(\w+)=(\S+)', text)}


def run(smp, staged, tag=None):
    tag = tag or f'smp{smp}'; work = WORK / tag; work.mkdir(parents=True, exist_ok=True)
    boot = work / 'boot.qcow2'; boot.unlink(missing_ok=True)
    subprocess.run([desk_vm.QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(staged), '-F', 'qcow2', str(boot)], check=True)
    vm = desk_vm.DeskVm(work, BASE, heads=1, accel=ACCEL, smp=smp, usb=True, vnc_heads=[], keep_overlay=True, overlay=boot)
    try:
        vm.wait(r'\[64\] AURORA READY', seconds=300)
        m = vm.wait(r'XWMTEST CONNECTED display=(:\d+) root=(\d+)x(\d+)', seconds=300, fail=None)
        W, H = int(m.group(2)), int(m.group(3)); vm.head0 = (0, 0, W, H)
        check(f'{tag}: X client connected to {m.group(1)} (Xwayland started on demand)', True)
        pos, deadline = 0, time.time() + 2400
        while True:
            log = vm.text(); st = re.compile(r'\[64\] XWMTEST STEP (\w+) READY ([^\n]*)\n').search(log, pos)
            done = re.search(r'\[64\] XWMTEST DONE passed=(\d+) failed=(\d+)', log)
            if not st and done: break
            if not st:
                if time.time() > deadline: raise TimeoutError('xwmtest did not finish')
                if vm.proc.poll() is not None: raise AssertionError('VM exited')
                me = re.search(r'XWMTEST START pid=(\d+)', log)
                if me and re.search(r'AURORA CHILD EXIT pid=%s status=' % me.group(1), log): raise AssertionError('xwmtest exited early: ' + log[-1500:])
                time.sleep(.2); continue
            pos = st.end(); name, p = st.group(1), params(st.group(2)); print('STEP', name, p, flush=True)
            if name in ('focus', 'transient', 'clipboard_x', 'clipboard_x2', 'close', 'zoom', 'unzoom', 'minimize'):
                if name == 'transient': vm.shot(f'{tag}_dialog')
                vm.goto(int(p['x']), int(p['y'])); time.sleep(.4); vm.click()
            elif name in ('move', 'resize'):
                x, y, dx, dy = int(p['x']), int(p['y']), int(p['dx']), int(p['dy'])
                if name == 'move':   # A, focused: its frame and content on screen (the title point is frame + 120,12)
                    fx, fy = x - 120, y - 12
                    shot = vm.frame(f'{tag}_frame', lambda s: near(s.getpixel((fx + 16, fy + 14)), (0xFF, 0x5F, 0x57), 40), seconds=20)
                    light, bar, content = shot.getpixel((fx + 16, fy + 14)), shot.getpixel((fx + 200, fy + 4)), shot.getpixel((fx + 200, fy + 28 + 100))
                    check(f'{tag}: Aurora draws the X frame (red light {light}, title bar {bar}) above the X content {content}',
                          near(light, (0xFF, 0x5F, 0x57), 40) and min(bar) >= 0xC0 and near(content, (0x20, 0x60, 0xC0), 8))
                    dock = sum(1 for qx in range(0, W, 3) for qy in range(H - 110, H, 3) if near(shot.getpixel((qx, qy)), (0x00, 0xFF, 0x40), 10))
                    check(f'{tag}: the dock shows the X window _NET_WM_ICON ({dock} sampled pixels of its colour in the dock band)', dock >= 20)
                vm.drag(x, y, x + dx, y + dy); time.sleep(.3)
            elif name == 'shot_fullscreen':
                shot = vm.frame(f'{tag}_fullscreen', lambda s: near(s.getpixel((5, 5)), (0x20, 0x60, 0xC0), 8), seconds=20)
                corners = [shot.getpixel(q) for q in ((5, 5), (W - 5, H - 5), (W // 2, H - 5), (W // 2, 5))]
                check(f'{tag}: fullscreen X window covers the screen (no menu bar, no dock) {corners[:2]}', all(near(c, (0x20, 0x60, 0xC0), 8) for c in corners))
                vm.tap('n')
            elif name == 'shot_transient':
                x, y = int(p['x']), int(p['y'])
                shot = vm.frame(f'{tag}_transient', lambda s: near(s.getpixel((x, y)), (0xFF, 0x80, 0x00), 12), seconds=20)
                check(f'{tag}: the dialog is drawn above its focused parent ({shot.getpixel((x, y))})', near(shot.getpixel((x, y)), (0xFF, 0x80, 0x00), 12))
                vm.tap('n')
            elif name == 'menu':
                x, y = int(p['x']), int(p['y'])
                try: shot = vm.frame(f'{tag}_menu', lambda s: near(s.getpixel((x, y)), (0xC0, 0x00, 0xC0), 12), seconds=20); ok = True
                except AssertionError: shot = vm.shot(f'{tag}_menu'); ok = False
                check(f'{tag}: the override-redirect menu is drawn at its X position above the window ({shot.getpixel((x, y))})', ok)
                vm.goto(x, y); time.sleep(.4); vm.click()
            elif name == 'clipboard_wayland':
                try: got = vm.wait(r'\[64\] XWMTEST WL selection: ([^\n]*)', pos, 60, fail=None).group(1)
                except TimeoutError: got = None
                want = 'x11 to wayland \u2713'.encode().decode('latin1')
                check(f'{tag}: a Wayland window pastes the X copy', got == want, repr(got))
            elif name == 'clipboard_big':   # the new Wayland window reads 400 kB the X window copied (INCR into Aurora)
                want = sum(ord('\n') if i % 64 == 63 else ord('a') + i % 26 for i in range(400000)) & 0xFFFFFFFF
                try: got = vm.wait(r'\[64\] XWMTEST WL selection bytes=(\d+) sum=(\d+)', pos, 90, fail=None).groups()
                except TimeoutError: got = None
                check(f'{tag}: a Wayland window pastes a 400 kB X copy (INCR)', got == ('400000', str(want)), got)
            elif name in ('kill', 'xterm_close', 'wine_close'):
                if name != 'kill': vm.shot(f'{tag}_{name}')
                vm.key('alt', True); vm.tap('f4'); vm.key('alt', False)
            elif name == 'xterm':
                time.sleep(1.5); vm.type('echo xwm-xterm-ok > /tmp/xwm-xterm.txt\n')
            elif name == 'firefox':   # GTK 3 under X11: drawn (in an Aurora frame, or its own CSD title bar), then closed
                x, y, framed = int(p['x']), int(p['y']), p.get('framed') == '1'; time.sleep(3); shot = vm.shot(f'{tag}_firefox')
                colors = {shot.getpixel((x + dx, y + 40 + dy)) for dx in range(-200, 400, 8) for dy in range(-150, 200, 8)}
                check(f'{tag}: Firefox (GTK, X11) is on screen ({len(colors)} colours, {"Aurora frame" if framed else "its own title bar"})',
                      len(colors) >= 3 and (not framed or near(shot.getpixel((x, y)), (0xFF, 0x5F, 0x57), 40)))
                if framed: vm.goto(x, y); time.sleep(.4); vm.click()
                else: vm.key('alt', True); vm.tap('f4'); vm.key('alt', False)
            elif name == 'xeyes':
                x, y, sx, sy = int(p['x']), int(p['y']), int(p['shot_x']), int(p['shot_y'])
                time.sleep(2); shot = vm.shot(f'{tag}_xeyes')
                colors = {shot.getpixel((sx + dx, sy + dy)) for dx in range(-60, 60, 6) for dy in range(-40, 40, 6)}
                check(f'{tag}: xeyes is drawn in its Aurora frame ({len(colors)} colours, light {shot.getpixel((x, y))})', len(colors) >= 2 and near(shot.getpixel((x, y)), (0xFF, 0x5F, 0x57), 40))
                vm.goto(x, y); time.sleep(.4); vm.click()
        log = vm.text(); done = re.search(r'\[64\] XWMTEST DONE passed=(\d+) failed=(\d+)', log)
        for name, ok, detail in re.findall(r'\[64\] XWMTEST CHECK (\S+) ok=(\d) ([^\n]*)', log):
            check(f'{tag}: {name}: {detail[:160]}', ok == '1', detail)
        check(f'{tag}: guest script finished ({done.group(0) if done else "no DONE"})', done and done.group(2) == '0')
        check(f'{tag}: Aurora started Xwayland rootless and became its window manager',
              re.search(r'AURORA X11 STARTED Xwayland pid=\d+ display=:\d+', log) and 'AURORA X11 WINDOW MANAGER READY' in log)
        check(f'{tag}: xwayland_shell_v1 is hidden from other Wayland clients', 'WL SEES xwayland_shell_v1' not in log)
        if tag.endswith('surface_id'): check(f'{tag}: Xwayland named its surfaces with WL_SURFACE_ID (no xwayland_shell_v1)', 'no xwayland_shell_v1 global: WL_SURFACE_ID only' in log)
        check(f'{tag}: Xwayland restarted after it was killed', len(re.findall(r'AURORA X11 STARTED Xwayland', log)) >= 2 and 'AURORA X11 Xwayland exited' in log)
        check(f'{tag}: the X copy went into the clipboard history', re.search(r'AURORA X11 CLIPBOARD from X owner', log) and re.search(r'AURORA CLIPBOARD kept uid=\d+ bytes=18', log))
        faults = re.findall(r'[^\n]*(?:KERNEL_FAULT|USER_FAULT)[^\n]*', log)
        check(f'{tag}: no kernel or user fault, Aurora still runs', not faults and 'DESKTOP EXIT' not in log, faults[:3])
        (work / 'x11-lines.txt').write_text('\n'.join(re.findall(r'\[64\] (?:AURORA X11|XWMTEST)[^\n]*', log)) + '\n', errors='replace')
    except Exception as error:   # noqa: BLE001
        check(f'{tag}: test ran to the end', False, repr(error))
        try: vm.shot(f'{tag}_failure')
        except Exception: pass
    finally:
        vm.close(); boot.unlink(missing_ok=True)


staged = WORK / 'staged.qcow2'
started = time.time()
try:
    prepare(staged, APPS)
    print('STAGED', staged, staged.stat().st_size // 1024, 'KiB', flush=True)
    for smp in SMPS: run(smp, staged)
    if FALLBACK:   # WL_SURFACE_ID instead of xwayland_shell_v1 serials
        staged.unlink(missing_ok=True); prepare(staged, '', {'home/nexus/.aurora/x11.conf': 'shell=0\n'})
        run(max(SMPS), staged, f'smp{max(SMPS)}_surface_id')
finally:
    staged.unlink(missing_ok=True)
ok = checks and all(c[1] for c in checks)
(WORK / 'result.json').write_text(json.dumps({'passed': bool(ok), 'checks': checks, 'seconds': round(time.time() - started)}, indent=1) + '\n')
print('RESULT', 'PASS' if ok else 'FAIL', len(checks), 'checks', sum(1 for c in checks if not c[1]), 'failed', flush=True)
sys.exit(0 if ok else 1)
