"""The desktop portal on an installed Debian ext4 root: Debian's xdg-desktop-portal with Aurora's backend
(arch/x86_64/nexus_portal.c), driven through the frontend by arch/x86_64/portal_test_user.c (/usr/lib/nexus/tests/portal-test).

    python -I tests/test_aurora_portal.py WORK --base BASE [--accel auto|whpx|tcg] [--smp 2] [--qmp-port 4745]
           [--hook-port 9145] [--nbd-port 9146] [--apt-cache DIR]

BASE: an installed system (tests/test_x64_services.py --keep-installed). WORK/stage.qcow2 gets the tree's services,
desktop, aurora-app, the portal backend + its configuration, logind and the test clients; the hook (root) installs
Debian's xdg-desktop-portal with apt (signed archive, through the host's caching proxy) and prepares files.
  hook   apt install xdg-desktop-portal; nexus's appearance: dark + blue accent; ~/Documents/report.txt -> event ready
  GUI    the greeter: nexus signs in                                                         -> gate login
  hook   Settings: ReadAll/ReadOne color-scheme 1 (dark) + accent (0.208,0.518,0.894), unknown key refused; the file
         changed to light: SettingChanged color-scheme 2. Screenshot (not interactive) -> permission prompt:
  GUI    the prompt (Deny focused): Tab, Enter = Take                                        -> PNG in ~/Pictures (1920x1080)
         an interactive one (the backend's own prompt): Esc = denied (no file); OpenFile in ~/Documents: Enter on report.txt -> its URI; SaveFile with
         current_name: Enter -> the URI in ~/Documents; OpenFile cancelled with Esc -> response 1; Inhibit idle through
         the portal -> Aurora's idle inhibition (logind) while held; OpenURI.OpenFile of report.txt (text/plain) -> Notes opens it;
         Notification -> Aurora's notice; hostile calls to the backend (signatures, a bad folder, a request closed
         during its dialog) fail cleanly; Settings > Appearance > Dark (the real UI) -> SettingChanged color-scheme 1.
Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import json
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg')); ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4745); ap.add_argument('--hook-port', type=int, default=9145)
ap.add_argument('--nbd-port', type=int, default=9146)
ap.add_argument('--apt-cache', type=Path, default=Path(r'D:\nexustest\wave2\ws_desk2_aptcache'))
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'aurora_portal', 'checks': [], 'passed': False}
NPW = pkg_vm.TEST_PASSWORD

HOOK_RUNNER = '''#!/bin/sh
rm -f /run/test-hook.sh
if wget -q -T 5 -t 1 -O /run/test-hook.sh http://10.0.2.2:@PORT@/hook.sh 2>/dev/null && [ -s /run/test-hook.sh ]; then
    echo "TEST-HOOK running"; sh /run/test-hook.sh; echo "TEST-HOOK finished status=$?"
fi
cat /proc/sys/kernel/random/boot_id > /run/test-hook.done
'''
HOOK = r'''PATH=/usr/sbin:/usr/bin:/sbin:/bin
H=http://10.0.2.2:@PORT@
PT=/usr/lib/nexus/tests/portal-test
ok() { echo "AURPORT $1 PASS ${2:-}"; }
bad() { echo "AURPORT $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/ap.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/ap.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 700 /tmp/ap.out | tr '\n' ' ')"; fi; }
event() { wget -q -O /dev/null --post-data=x "$H/event/$1" || true; }
gate() { i=0; while ! wget -q -O /dev/null "$H/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt 3600 ] && return 1; sleep 1; done; }
as_nexus() { setpriv --reuid=1000 --regid=1000 --init-groups env HOME=/home/nexus USER=nexus XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "$@"; }
appearance() { as_nexus sh -c "mkdir -p /home/nexus/.config/aurora && printf 'color_scheme=%s\naccent=#3584E4\n' $1 > /home/nexus/.config/aurora/appearance.conf"; }
png_size() { od -An -tu1 -j16 -N8 "$1" | awk '{print $1*16777216+$2*65536+$3*256+$4 "x" $5*16777216+$6*65536+$7*256+$8}'; }
do_apt() { apt-get update >/tmp/apt-u.log 2>&1; DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends xdg-desktop-portal shared-mime-info >/tmp/apt-i.log 2>&1; r=$?; tail -3 /tmp/apt-i.log; [ $r = 0 ] && [ -x /usr/libexec/xdg-desktop-portal ]; }
do_files() { appearance dark && as_nexus sh -c 'mkdir -p /home/nexus/Documents && echo "quarterly report" > /home/nexus/Documents/report.txt && rm -f /home/nexus/Documents/out.txt'; }
do_settings() { as_nexus $PT settings | tee /tmp/pt-s.txt; grep -q 'color-scheme=1' /tmp/pt-s.txt && grep -q 'accent-color=(0.208,0.518,0.894)' /tmp/pt-s.txt && grep -q 'READONE color-scheme 1' /tmp/pt-s.txt && grep -q 'READONE unknown org.freedesktop.portal.Error.NotFound' /tmp/pt-s.txt; }
do_changed() { as_nexus $PT watch 12 > /tmp/pt-w.txt 2>&1 & sleep 3; appearance light; wait; cat /tmp/pt-w.txt; grep -q 'SETTING-CHANGED org.freedesktop.appearance color-scheme 2' /tmp/pt-w.txt; }
do_shot() { ls /home/nexus/Pictures 2>/dev/null | wc -l > /tmp/shots-before; as_nexus $PT screenshot | tee /tmp/pt-shot.txt; f=$(grep -o 'file://[^ ]*\.png' /tmp/pt-shot.txt | head -1 | sed 's|file://||; s|%20| |g; s|%28|(|g; s|%29|)|g'); echo "file [$f]"; [ -f "$f" ] && sz=$(png_size "$f") && echo "png $sz" && [ "$sz" = 1920x1080 ] && grep -q 'RESPONSE 0' /tmp/pt-shot.txt; }
do_shot_denied() { n0=$(ls /home/nexus/Pictures | wc -l); as_nexus $PT screenshot interactive | tee /tmp/pt-shot2.txt; n1=$(ls /home/nexus/Pictures | wc -l); echo "files $n0 -> $n1"; grep -q 'RESPONSE [12]' /tmp/pt-shot2.txt && [ "$n0" = "$n1" ]; }
do_open() { as_nexus $PT open /home/nexus/Documents | tee /tmp/pt-o.txt; grep -q 'uris=\[file:///home/nexus/Documents/report.txt\]' /tmp/pt-o.txt && grep -q 'RESPONSE 0' /tmp/pt-o.txt; }
do_save() { as_nexus $PT save out.txt /home/nexus/Documents | tee /tmp/pt-sv.txt; grep -q 'uris=\[file:///home/nexus/Documents/out.txt\]' /tmp/pt-sv.txt && grep -q 'RESPONSE 0' /tmp/pt-sv.txt; }
do_cancel() { as_nexus $PT open /home/nexus/Documents | tee /tmp/pt-c.txt; grep -q 'RESPONSE 1' /tmp/pt-c.txt; }
do_inhibit() { as_nexus $PT inhibit 8 10 > /tmp/pt-i.txt 2>&1 & sleep 4; a=$(/bin/aurora-app ctl sessions | grep -o 'idle_inhibited=[01]'); wait; sleep 2; b=$(/bin/aurora-app ctl sessions | grep -o 'idle_inhibited=[01]'); cat /tmp/pt-i.txt; echo "during $a after $b"; [ "$a" = idle_inhibited=1 ] && [ "$b" = idle_inhibited=0 ]; }
do_openuri() { as_nexus $PT openfile /home/nexus/Documents/report.txt | tee /tmp/pt-u.txt; grep -q 'RESPONSE 0' /tmp/pt-u.txt; }   # OpenURI.OpenFile: text/plain -> Notes
do_notify() { as_nexus $PT notify "Portal note" "from portal-test" | tee /tmp/pt-n.txt; grep -q 'NOTIFY ok' /tmp/pt-n.txt; }
do_dnd() { /bin/aurora-app ctl set-dnd 1 && as_nexus $PT notify "Quiet note" "during do not disturb" && sleep 2 && /bin/aurora-app ctl notices | tee /tmp/pt-nc.txt; grep -q 'text=.*Quiet note' /tmp/pt-nc.txt && grep -q 'text=.*Portal note' /tmp/pt-nc.txt && grep -q 'dnd=1' /tmp/pt-nc.txt; }
do_centre() { /bin/aurora-app ctl notices | tee /tmp/pt-nc2.txt; grep -q 'count=0 unread=0 dnd=0' /tmp/pt-nc2.txt; }
do_hostile() { as_nexus $PT hostile | tee /tmp/pt-h.txt; grep -q 'HOSTILE DONE' /tmp/pt-h.txt && ! grep -q 'HOSTILE .* FAIL' /tmp/pt-h.txt; }
do_ui_dark() { cat /tmp/pt-w2.txt; grep -q 'SETTING-CHANGED org.freedesktop.appearance color-scheme 1' /tmp/pt-w2.txt && grep -q '^color_scheme=dark' /home/nexus/.config/aurora/appearance.conf && grep -q 'gtk-application-prefer-dark-theme=1' /home/nexus/.config/gtk-3.0/settings.ini; }
check apt-xdg-desktop-portal do_apt
check files do_files
event ready
gate login
check portal-settings do_settings
check portal-setting-changed do_changed
event shot1
check portal-screenshot-allowed do_shot
event shot2
check portal-screenshot-denied do_shot_denied
event open
check portal-openfile do_open
event save
check portal-savefile do_save
event cancel
check portal-openfile-cancelled do_cancel
check portal-inhibit-idle do_inhibit
event openuri
check portal-openuri do_openuri
check portal-notification do_notify
check notification-history-and-dnd do_dnd
event centre
gate centre-done
check notification-centre-cleared do_centre
check portal-hostile do_hostile
as_nexus $PT watch 400 > /tmp/pt-w2.txt 2>&1 &
event ui
gate ui-done
sleep 2; kill %1 2>/dev/null; wait
check settings-appearance-ui-dark do_ui_dark
grep -a 'xdg-desktop-portal\|AURORA PORTAL\|aurora-app' /var/log/journal/system.journal | tail -40 | cut -c1-300 | sed 's/^/AURPORT-LOG /'
echo "AURPORT DONE"
'''


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def wait_event(server, vm, name, seconds=900):
    end = time.time() + seconds
    while time.time() < end and vm.proc.poll() is None:
        if any(event == name for _, event, _ in server.events): return True
        time.sleep(1)
    return False


def window(vm, app, mark, seconds=60):
    """The next window of app after log position mark: (match, position after it) or (None, mark)."""
    try:
        m = vm.wait(r'\[64\] AURORA WINDOW app=%s title=[^\n]* x=(-?\d+) y=(-?\d+)' % re.escape(app), mark, seconds)
        return m, mark + m.end()
    except TimeoutError: check('the %s window appeared' % app, False, 'timeout'); return None, mark


server = vm = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    def stage(fs):
        pkg_vm.refresh_services(fs)
        pkg_vm.put(fs, '/usr/lib/nexus/tests/test-hook', HOOK_RUNNER.replace('@PORT@', str(args.hook_port)), 0o755)
        pkg_vm.put(fs, '/etc/apt/apt.conf.d/99nexus-test-proxy', 'Acquire::http::Proxy "http://10.0.2.2:%d/";\n' % args.hook_port, 0o644)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage, nbd_port=args.nbd_port), flush=True)
    args.apt_cache.mkdir(parents=True, exist_ok=True)
    server = pkg_vm.HookServer(HOOK.replace('@PORT@', str(args.hook_port)), {}, WORK / 'uploads', args.apt_cache, port=args.hook_port)
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='portal', accel=args.accel, smp=args.smp, memory='2048')
    vm.wait(r'\[64\] AURORA GREETER reason=boot user=nexus', 0, 900)
    check('hook: xdg-desktop-portal installed (event ready)', wait_event(server, vm, 'ready', 1500), re.findall(r'AURPORT [^\n]*', vm.text())[-4:])
    time.sleep(2); mark = len(vm.text()); vm.type(NPW); time.sleep(.3); vm.tap('ret')
    check('greeter: nexus signs in', vm.wait(r'\[64\] AURORA UNLOCKED user=nexus mode=greeter', mark, 60))
    time.sleep(2); mark = len(vm.text()); server.open_gate('login')
    # each dialog is searched after the previous one: the guest may open the next before the host looks again
    if wait_event(server, vm, 'shot1', 300):
        _, mark = window(vm, 'nexus.prompt', mark, 90); time.sleep(1.5); vm.tap('tab'); time.sleep(.3); vm.tap('ret')
    if wait_event(server, vm, 'shot2', 300):
        _, mark = window(vm, 'nexus.prompt', mark, 90); time.sleep(1.5); vm.tap('esc')
    if wait_event(server, vm, 'open', 300):
        _, mark = window(vm, 'nexus.filechooser', mark, 90); time.sleep(1.5); vm.shot('filechooser'); vm.tap('ret')
    if wait_event(server, vm, 'save', 300):
        _, mark = window(vm, 'nexus.filechooser', mark, 90); time.sleep(1.5); vm.tap('ret')
    if wait_event(server, vm, 'cancel', 300):
        _, mark = window(vm, 'nexus.filechooser', mark, 90); time.sleep(1.5); vm.tap('esc')
    if wait_event(server, vm, 'openuri', 300):
        notes, mark = window(vm, 'nexus.notes', mark, 120)
        check('OpenURI: Notes opens the text file (nexus-notes.desktop handler)', notes)
    if wait_event(server, vm, 'centre', 600):   # the Notification Centre from the menu-bar bell: D (Do Not Disturb off), C (clear)
        bell = re.findall(r'\[64\] AURORA BELL x=(\d+) cy=(\d+)', vm.text())
        if check('the menu-bar bell is drawn', bell):
            mark = len(vm.text()); vm.click(int(bell[-1][0]) + 10, int(bell[-1][1]))
            opened = vm.wait(r'\[64\] AURORA NOTIFICATIONS open count=(\d+) dnd=(\d)', mark, 90)
            check('the Notification Centre lists both notifications with Do Not Disturb on', opened.group(1) == '2' and opened.group(2) == '1', opened.group(0))
            vm.wait(r'\[64\] AURORA WINDOW app=nexus.notifications', mark, 60); time.sleep(1.5); vm.shot('notifications')
            mark = len(vm.text()); vm.tap('d'); vm.wait(r'\[64\] AURORA NOTIFICATIONS dnd=0', mark, 30)
            mark = len(vm.text()); vm.tap('c'); vm.wait(r'\[64\] AURORA NOTIFICATIONS cleared', mark, 30)
            vm.key('alt', True); vm.tap('f4'); vm.key('alt', False); time.sleep(1)
        server.open_gate('centre-done')
    if wait_event(server, vm, 'ui', 600):
        mark = len(vm.text()); vm.point(960, 540); vm.key('ctrl', True); vm.tap('spc'); vm.key('ctrl', False); time.sleep(1.5); vm.type('system settings'); time.sleep(1); vm.tap('ret')
        win = vm.wait(r'\[64\] AURORA WINDOW app=nexus.settings title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, 120); sx, sy = int(win.group(1)), int(win.group(2)); time.sleep(2)
        sidebar = re.findall(r'&settings_(\w+)', re.search(r'pages\[\] = \{(.*?)\};', (ROOT / 'arch/x86_64/aurora_settings.c').read_text(), re.S).group(1))
        row = sidebar.index('appearance')   # the sidebar order of the tree's aurora_settings.c (merged trees add pages before it)
        mark = len(vm.text()); vm.click(sx + 100, sy + 29 + 12 + row * 34 + 17)   # sidebar row: Appearance (title bar 29; rows of 34 px from 12, aurora_settings.c)
        grid = vm.wait(r'\[64\] SETTINGS APPEARANCE GRID scheme_x=(\d+) scheme_y=(\d+) scheme_w=(\d+)', mark, 30)
        x0, y0, w = (int(g) for g in grid.groups())
        mark = len(vm.text()); vm.click(sx + 201 + x0 + w * 5 // 6, sy + 29 + y0)   # third segment: Dark (the page starts right of the 200 px sidebar)
        vm.wait(r'\[64\] SETTINGS APPEARANCE scheme=dark', mark, 30); vm.shot('appearance'); time.sleep(4)
        server.open_gate('ui-done')
    vm.wait(r'AURPORT DONE', 0, 600)
    lines = re.findall(r'^AURPORT (\S+) (PASS|FAIL)(.*)$', vm.text(), re.M)
    for name, state, detail in lines: check('guest: ' + name, state == 'PASS', detail.strip())
    for line in re.findall(r'^AURPORT-LOG [^\n]*', vm.text(), re.M): print('   ', line[:250], flush=True)
    check('every guest check ran', len(lines) >= 15, len(lines))
    check('Aurora showed the portal notification', re.search(r'\[64\] AURORA NOTIFY app=[^\n]* summary=Portal note', vm.text()))
    check('no kernel or user fault', not vm.faults(), vm.faults()[:3])
except Exception as error:   # noqa: BLE001
    check('test ran to the end', False, repr(error))
    if vm:
        try: vm.shot('failure')
        except Exception: pass
finally:
    if vm: vm.close()
    if server: server.close()
    for name in ('stage.qcow2', 'overlay.qcow2'): (WORK / name).unlink(missing_ok=True)
results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
(WORK / 'result.json').write_text(json.dumps(results, indent=1))
print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)
