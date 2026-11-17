"""The screen reader on an installed Debian ext4 root: Debian's Orca 48 + speech-dispatcher/espeak-ng + at-spi2-core
with Aurora's bridge (arch/x86_64/aurora_a11y_bridge.c, aurora_reader.c), and keyboard-only use of the shell.

    python -I tests/test_aurora_orca.py WORK --base BASE [--accel auto|whpx|tcg] [--smp 2] [--qmp-port 4747]
           [--hook-port 9147] [--nbd-port 9148] [--apt-cache DIR]

BASE: an installed system (tests/test_x64_services.py --keep-installed). WORK/stage.qcow2 gets the tree's services,
desktop, aurora-app, the bridge, orca-wayland and atspi-watch; the hook (root) installs orca, speech-dispatcher with
espeak-ng and at-spi2-core with apt (signed archive, through the host's caching proxy). Sound goes to WORK/audio.wav.
  GUI   the greeter: nexus signs in
  part 1, the shell's AT-SPI2 tree (bridge without Orca, an AT-SPI2 client as the user): Ctrl+F2 opens the menu bar
        (window:activate "Menu bar", focus "About This NexusOS" menu item), Down moves the focus, the tree shows the
        menus and their items, Esc; Ctrl+F3 focuses the Dock (push buttons), Right, Esc; Ctrl+Space Spotlight "term"
        (dialog, list item Terminal), Enter starts the terminal (its banner becomes a notification object)
  part 2, Orca (bridge --orca): the bridge points speech-dispatcher at ALSA (no PulseAudio), spd-say is heard in the
        capture; Orca uses the bridge's org.freedesktop.a11y.KeyboardMonitor (no X display); it speaks
        a GTK 3 window (button, entry, check box), the menu bar via Ctrl+F2, and Orca+T (Insert+T: the time) while
        Insert never reaches the application; another process may not watch the keyboard; nothing of the lock
        screen's keys reaches the screen reader
  part 3, switching it on and off: Super+Alt+S (Aurora starts aurora-a11y-bridge --orca as the user, and stops it),
        "set-screen-reader 1|0" on the control socket; hostile: a watch without a descriptor, the bridge as root
  part 4, aurora-app windows and text (aurora_a11y_client.c): System Settings is a frame of "Aurora" with headings,
        labels and list items (window:activate); the authentication dialog (with --polkit-dir: sec2's broker) has a
        password_text field whose text stays empty while a password is typed into it; Spotlight's search field has
        the Text interface ("calc", text-changed:insert); the lock screen's password field never shows what is typed;
        the setting is kept across logins: log out, sign in again -> Aurora starts the screen reader itself
Result: WORK/result.json.
"""
from pathlib import Path
import argparse
import array
import json
import re
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg')); ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4747); ap.add_argument('--hook-port', type=int, default=9147)
ap.add_argument('--nbd-port', type=int, default=9148)
ap.add_argument('--apt-cache', type=Path, default=Path(r'D:\nexustest\wave2\ws_desk2_aptcache'))
ap.add_argument('--polkit-dir', type=Path, default=Path(r'D:\nexustest\wave2\ws_desk2_tools\polkit'),
                help="sec2's nexus-polkitd (the authentication dialog's tree; skipped when absent)")
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'aurora_orca', 'checks': [], 'passed': False}
NPW = pkg_vm.TEST_PASSWORD
AUDIO = WORK / 'audio.wav'

HOOK_RUNNER = '''#!/bin/sh
rm -f /run/test-hook.sh
if wget -q -T 5 -t 1 -O /run/test-hook.sh http://10.0.2.2:@PORT@/hook.sh 2>/dev/null && [ -s /run/test-hook.sh ]; then
    echo "TEST-HOOK running"; sh /run/test-hook.sh; echo "TEST-HOOK finished status=$?"
fi
cat /proc/sys/kernel/random/boot_id > /run/test-hook.done
'''
HOOK = r'''PATH=/usr/sbin:/usr/bin:/sbin:/bin
H=http://10.0.2.2:@PORT@
BR=/usr/libexec/aurora-a11y-bridge
AW=/usr/lib/nexus/tests/atspi-watch
ok() { echo "AURORCA $1 PASS ${2:-}"; }
bad() { echo "AURORCA $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/ao.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/ao.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 700 /tmp/ao.out | tr '\n' ' ')"; fi; }
event() { for i in 1 2 3 4 5; do wget -q -T 10 -t 1 -O /dev/null --post-data=x "$H/event/$1" && return 0; sleep 1; done; true; }   # (retried: one POST must not decide the run)
gate() { i=0; while ! wget -q -O /dev/null "$H/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt 3600 ] && return 1; sleep 1; done; }
as_nexus() { setpriv --reuid=1000 --regid=1000 --init-groups env HOME=/home/nexus USER=nexus XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus WAYLAND_DISPLAY=wayland-0 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=NexusOS "$@"; }
waitfor() { i=0; while ! grep -q "$2" "$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt $3 ] && return 1; sleep 1; done; }
gtk_window() { cat > /tmp/gtk-test.py <<'PY'
import gi, sys
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib
win = Gtk.Window(title="Orca test window"); box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
button = Gtk.Button(label="Press me"); entry = Gtk.Entry(); entry.set_placeholder_text("Your name"); check = Gtk.CheckButton(label="Remember this")
def key(widget, event): print("GTK-KEY", Gtk.accelerator_name(event.keyval, 0), flush=True); return False
win.connect("key-press-event", key)
for w in (button, entry, check): box.pack_start(w, False, False, 0)
win.add(box); win.connect("destroy", Gtk.main_quit); win.show_all(); button.grab_focus()
def step(n): [entry, check][n].grab_focus(); print("GTK-FOCUS", ["entry", "check"][n], flush=True); return False
GLib.timeout_add_seconds(8, step, 0); GLib.timeout_add_seconds(13, step, 1); GLib.timeout_add_seconds(int(sys.argv[1]), Gtk.main_quit)
print("GTK-READY", flush=True); Gtk.main()
PY
chmod 644 /tmp/gtk-test.py; }
do_apt() { apt-get update >/tmp/apt-u.log 2>&1; DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends orca speech-dispatcher speech-dispatcher-espeak-ng at-spi2-core espeak-ng dbus-bin >/tmp/apt-i.log 2>&1; r=$?; tail -3 /tmp/apt-i.log; [ $r = 0 ] && [ -x /usr/bin/orca ] && [ -x $BR ] && [ -x /usr/lib/nexus/orca-wayland ]; }
do_speech() { as_nexus timeout -k 3 45 spd-say -w "speech check one two three"; r=$?; echo "spd-say rc=$r"; [ $r = 0 ]; }
do_apps() { cat /tmp/aw.log | head -3; grep -q "^APPS .*'Aurora'" /tmp/aw.log; }
ev() { grep -q "^EVENT $1" /tmp/aw.log; }
do_menubar() { ev "window:activate detail1=0 name='Menu bar' role=frame states=[a-z,]*active"; }
do_menu_item() { ev "object:state-changed:focused detail1=1 name='About This NexusOS' role=menu item states=focused[a-z,]* parent='NexusOS'"; }
do_menu_down() { ev "object:state-changed:focused detail1=1 name='Applications' role=menu item"; }
do_tree() { cat /tmp/tree.txt | head -40; grep -q "role=menu bar" /tmp/tree.txt && grep -q "name='Log Out' role=menu item" /tmp/tree.txt && [ "$(grep -c "role=menu states" /tmp/tree.txt)" = 8 ]; }
do_dock() { ev "window:activate detail1=0 name='Dock' role=frame" && ev "object:state-changed:focused detail1=1 name='[^']*' role=\(push \)\?button states=focused[a-z,]* parent='Dock' index=1"; }
do_spotlight() { ev "window:activate detail1=0 name='Spotlight Search' role=dialog" && ev "object:state-changed:focused detail1=1 name='Terminal' role=list item"; }
do_banner() { ev "object:state-changed:showing detail1=1 name='Opening application...' role=notification"; }
do_left() { ev "window:deactivate detail1=0 name='Spotlight Search'"; }
do_watch_refused() { out=$(/bin/aurora-app ctl a11y-watch 2>&1); echo "$out"; echo "$out" | grep -q 'needs a SOCK_SEQPACKET descriptor'; }
do_root_refused() { $BR > /tmp/br-root.log 2>&1; r=$?; cat /tmp/br-root.log; [ $r != 0 ] && grep -q 'refusing to run as root' /tmp/br-root.log; }
D=/run/user/1000/orca-debug.txt
do_monitor() { /bin/aurora-app ctl screen-reader | tee /tmp/sr0.txt; grep -q 'screen_reader=1 keys=1' /tmp/sr0.txt; }
keys_on() { i=0; while ! /bin/aurora-app ctl screen-reader 2>/dev/null | grep -q 'keys=1'; do i=$((i + 1)); [ $i -gt $1 ] && return 1; sleep 1; done; }
do_foreign_monitor() { as_nexus dbus-send --session --print-reply --dest=org.freedesktop.a11y.Manager /org/freedesktop/a11y/Manager org.freedesktop.a11y.KeyboardMonitor.WatchKeyboard > /tmp/fm.txt 2>&1; cat /tmp/fm.txt; grep -q 'AccessDenied' /tmp/fm.txt; }
speech() { grep -a "SPEECH OUTPUT: '[^']*$1" $D | head -3 | cut -c1-200; grep -aq "SPEECH OUTPUT: '[^']*$1" $D; }
do_orca_button() { speech 'Press me'; }
do_orca_entry() { speech 'Your name'; }
do_orca_check() { speech 'Remember this'; }
do_orca_menu() { speech 'About This NexusOS'; }
do_orca_time() { grep -aq "SPEECH OUTPUT: '[0-9]\{1,2\}:[0-9][0-9]" $D; }
do_insert_taken() { grep -a 'GTK-KEY' /tmp/gtk.log | head; ! grep -aq 'GTK-KEY Insert' /tmp/gtk.log && ! grep -aq 'GTK-KEY t$' /tmp/gtk.log && grep -aq 'GTK-KEY x' /tmp/gtk.log; }
do_lock_private() { a=$(cat /tmp/keys-before-lock); grep -a '^\[64\] AURORA A11Y BRIDGE KEY ' /tmp/br2.log | tail -n +$((a + 1)) > /tmp/keys-lock.txt; cat /tmp/keys-lock.txt | head -20; [ -s /tmp/keys-lock.txt ] || return 1; ! grep -q 'keysym=0x\(73\|65\|63\|72\|70\|77\|6e\|78\|75\) ' /tmp/keys-lock.txt; }
do_control_on() { /bin/aurora-app ctl set-screen-reader 1 && sleep 10 && /bin/aurora-app ctl screen-reader | tee /tmp/sr.txt && grep -q 'screen_reader=1' /tmp/sr.txt && pgrep -u nexus -f "$BR --orca"; }
do_settings_tree() { head -50 /tmp/tree4.txt; grep -q "name='System Settings' role=frame" /tmp/tree4.txt && grep -q "role=list item" /tmp/tree4.txt && grep -q "role=heading\|role=label" /tmp/tree4.txt; }
do_settings_event() { grep -aq "EVENT window:activate detail1=0 name='System Settings' role=frame" /tmp/aw4.log; }
do_auth_tree() { head -30 /tmp/tree5.txt; grep -q "name='Authentication Required' role=frame" /tmp/tree5.txt && grep "role=password text" /tmp/tree5.txt | grep -q "text=''$" && grep -q "role=\(push \)\?button" /tmp/tree5.txt; }
do_auth_secret() { ! grep -aq 'Pw-typed' /tmp/aw4.log /tmp/tree5.txt && ! grep -a 'text-changed' /tmp/aw4.log | grep -aq 'role=password text'; }
do_spot_text() { grep -a "text-changed:insert" /tmp/aw4.log | head -4; grep -a "text-changed:insert" /tmp/aw4.log | grep -aq "name='Search' role=entry" && grep -aq "name='Search' role=entry .*text='calc'" /tmp/aw4.log; }
do_lock_text() { grep -a "role=password text" /tmp/aw4.log | head -3; ! grep -aq 'secretpw' /tmp/aw4.log && ! grep -a "role=password text" /tmp/aw4.log | grep -aqv "text=''"; }
do_autostart() { i=0; while ! /bin/aurora-app ctl screen-reader 2>/dev/null | grep -q 'screen_reader=1'; do i=$((i + 1)); [ $i -gt 120 ] && return 1; sleep 1; done; /bin/aurora-app ctl screen-reader; }
do_saved_off() { /bin/aurora-app ctl set-screen-reader 0 && i=0; while pgrep -u nexus -f "$BR" >/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done; ! pgrep -u nexus -f "$BR"; }
do_broker() { [ -x /usr/libexec/nexus-polkitd ] || return 0; /usr/libexec/nexus-polkitd > /tmp/polkitd.log 2>&1 & i=0; while [ ! -S /run/nexus-polkit.sock ] && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done; [ -S /run/nexus-polkit.sock ]; }
do_control_off() { /bin/aurora-app ctl set-screen-reader 0 || return 1; i=0; while pgrep -u nexus -f "$BR" >/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done; echo "gone after $i s"; /bin/aurora-app ctl screen-reader | tee /tmp/sr2.txt; grep -q 'screen_reader=0 keys=0' /tmp/sr2.txt && ! pgrep -u nexus -f "$BR"; }
check apt-orca-speechd-atspi do_apt
event ready
gate login
# part 1: the shell's tree, read by an AT-SPI2 client
as_nexus $BR --trace > /tmp/br1.log 2>&1 &
waitfor /tmp/br1.log 'READY' 120; grep -aE 'READY|Embed' /tmp/br1.log | head -3
as_nexus python3 $AW 200 > /tmp/aw.log 2>&1 &
waitfor /tmp/aw.log WATCHING 60
check atspi-lists-aurora do_apps
event nav
gate menu-open
as_nexus python3 $AW tree Aurora > /tmp/tree.txt 2>&1
event tree-done
gate nav-done
sleep 3
check atspi-menubar-window do_menubar
check atspi-menu-item-focus do_menu_item
check atspi-menu-down do_menu_down
check atspi-tree-menus do_tree
check atspi-dock do_dock
check atspi-spotlight do_spotlight
check atspi-banner-notification do_banner
check atspi-shell-left do_left
pkill -u nexus -f atspi-watch; pkill -u nexus -f "$BR"; sleep 2
grep -a 'EVENT' /tmp/aw.log | head -40 | cut -c1-240 | sed 's/^/AURORCA-AW /'
# part 2: Orca
gtk_window
/bin/aurora-app ctl set-screen-reader 1 debug
keys_on 120
check orca-keyboard-monitor do_monitor
check speech-configured-for-alsa grep -q '^AudioOutputMethod "alsa"' /home/nexus/.config/speech-dispatcher/speechd.conf
check keyboard-monitor-only-orca do_foreign_monitor
as_nexus python3 /tmp/gtk-test.py 90 > /tmp/gtk.log 2>&1 &
waitfor /tmp/gtk.log 'GTK-FOCUS check' 60; sleep 4
event orca-keys
gate orca-keys-done
sleep 4
check speech-dispatcher-speaks do_speech   # a second client while Orca runs
grep -a 'ERROR\|rror\|XRUN\|underrun' /run/user/1000/speech-dispatcher/log/speech-dispatcher.log 2>/dev/null | tail -12 | cut -c1-200 | sed 's/^/AURORCA-SPD /'
event speech-done
grep -a "SPEECH OUTPUT" $D | head -40 | cut -c1-200 | sed 's/^/AURORCA-SAID /'
check orca-speaks-button do_orca_button
check orca-speaks-entry do_orca_entry
check orca-speaks-check-box do_orca_check
check orca-speaks-menu-bar do_orca_menu
check orca-time-command do_orca_time
check orca-keys-not-delivered do_insert_taken
event lock
gate lock-done
/bin/aurora-app ctl set-screen-reader 0; sleep 3; pkill -u nexus -f gtk-test
# part 3
i=0; while pgrep -u nexus -f "$BR" >/dev/null && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done
check control-screen-reader-on do_control_on
check control-screen-reader-off do_control_off
check watch-without-descriptor-refused do_watch_refused
check bridge-as-root-refused do_root_refused
as_nexus $BR --orca > /tmp/br3.log 2>&1 &   # not started by Aurora: Orca's WatchKeyboard must not get keys
sleep 30; pkill -u nexus -f "$BR"; sleep 4
event toggle
gate toggle-done
# part 4
check polkit-broker do_broker
/bin/aurora-app ctl set-screen-reader 1
keys_on 120
as_nexus python3 $AW 900 > /tmp/aw4.log 2>&1 &
waitfor /tmp/aw4.log WATCHING 60
as_nexus /bin/aurora-app settings > /tmp/settings4.out 2>&1 &
sleep 10
as_nexus python3 $AW tree Aurora > /tmp/tree4.txt 2>&1
check aurora-app-settings-tree do_settings_tree
check aurora-app-settings-window-event do_settings_event
pkill -u nexus -f 'aurora-app settings'; sleep 2
if [ -x /usr/libexec/nexus-polkitd ]; then
    ( as_nexus /bin/aurora-app authenticate org.freedesktop.policykit.exec --app "Orca test" -- /bin/true > /tmp/auth4.out 2>&1 ) &
    event auth4
    gate auth4-typed
    as_nexus python3 $AW tree Aurora > /tmp/tree5.txt 2>&1
    event auth4-tree
    gate auth4-done
    check auth-dialog-tree do_auth_tree
    check auth-dialog-password-never-exported do_auth_secret
fi
event text
gate text-done
sleep 2
check spotlight-entry-text do_spot_text
check lock-password-text-empty do_lock_text
/bin/aurora-app ctl logout
event relogin
gate relogin-done
check screen-reader-kept-across-login do_autostart
check screen-reader-off-again do_saved_off
grep -a 'EVENT' /tmp/aw4.log | grep -av 'showing' | head -60 | cut -c1-240 | sed 's/^/AURORCA-AW4 /'
pkill -u nexus -f atspi-watch
pkill -u nexus speech-dispatch
grep -a 'AURORA A11Y BRIDGE' /tmp/br1.log /tmp/br3.log | grep -av ' KEY \| call ' | head -40 | cut -c1-220 | sed 's/^/AURORCA-BR /'
echo "AURORCA DONE"
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


def sound_seconds():
    """Seconds of non-silent sound in the capture so far (QEMU writes the WAV while the guest plays)."""
    try: data = AUDIO.read_bytes()
    except OSError: return 0.0
    if len(data) < 44: return 0.0
    channels, rate = struct.unpack('<H', data[22:24])[0], struct.unpack('<I', data[24:28])[0]
    pcm = array.array('h', data[44:44 + (len(data) - 44) // 2 * 2]); step = max(1, rate * channels // 10)
    return sum(1 for i in range(0, len(pcm), step) if max((abs(x) for x in pcm[i:i + step]), default=0) > 1000) / 10


def seen(vm, pattern, mark, seconds):
    try: return vm.wait(pattern, mark, seconds)
    except TimeoutError: return None


def chord(vm, mods, key):
    for m in mods: vm.key(m, True)
    time.sleep(.1); vm.tap(key); time.sleep(.1)
    for m in reversed(mods): vm.key(m, False)
    time.sleep(.6)


server = vm = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    def stage(fs):
        pkg_vm.refresh_services(fs)
        pkg_vm.put(fs, '/usr/lib/nexus/tests/test-hook', HOOK_RUNNER.replace('@PORT@', str(args.hook_port)), 0o755)
        pkg_vm.put(fs, '/etc/apt/apt.conf.d/99nexus-test-proxy', 'Acquire::http::Proxy "http://10.0.2.2:%d/";\n' % args.hook_port, 0o644)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
        broker = args.polkit_dir / 'nexus-polkitd'
        if broker.is_file(): pkg_vm.put(fs, '/usr/libexec/nexus-polkitd', broker.read_bytes(), 0o755)   # part 4: the dialog
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage, nbd_port=args.nbd_port), flush=True)
    args.apt_cache.mkdir(parents=True, exist_ok=True); AUDIO.unlink(missing_ok=True)
    server = pkg_vm.HookServer(HOOK.replace('@PORT@', str(args.hook_port)), {}, WORK / 'uploads', args.apt_cache, port=args.hook_port)
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='orca', accel=args.accel, smp=args.smp, memory='2048',
                   extra=['-audiodev', 'wav,id=snd0,path=' + str(AUDIO), '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'])
    vm.wait(r'\[64\] AURORA GREETER reason=boot user=nexus', 0, 900)
    check('hook: orca, speech-dispatcher, at-spi2-core installed (event ready)', wait_event(server, vm, 'ready', 1800), re.findall(r'AURORCA [^\n]*', vm.text())[-4:])
    time.sleep(2); mark = len(vm.text()); vm.type(NPW); time.sleep(.3); vm.tap('ret')
    check('greeter: nexus signs in', vm.wait(r'\[64\] AURORA UNLOCKED user=nexus mode=greeter', mark, 60))
    time.sleep(2); server.open_gate('login')
    if wait_event(server, vm, 'nav', 300):   # part 1: keyboard-only shell, read by the AT-SPI2 client
        vm.point(960, 540); mark = len(vm.text()); chord(vm, ['ctrl'], 'f2')
        check('Ctrl+F2 opens the menu bar', vm.wait(r'\[64\] AURORA KEYNAV menubar', mark, 20)); time.sleep(1)
        vm.tap('down'); time.sleep(1.2); vm.shot('menu'); server.open_gate('menu-open')
        wait_event(server, vm, 'tree-done', 120); vm.tap('esc'); time.sleep(1)
        mark = len(vm.text()); chord(vm, ['ctrl'], 'f3')
        check('Ctrl+F3 focuses the Dock', vm.wait(r'\[64\] AURORA KEYNAV dock focus=0', mark, 20)); time.sleep(1)
        vm.tap('right'); time.sleep(1); vm.shot('dock')
        mark = len(vm.text()); vm.tap('esc'); check('Esc leaves the Dock', vm.wait(r'\[64\] AURORA KEYNAV dock focus=-1 action=0', mark, 20)); time.sleep(1)
        chord(vm, ['ctrl'], 'spc'); time.sleep(1.5); vm.type('term'); time.sleep(1.5); vm.shot('spotlight')
        mark = len(vm.text()); vm.tap('ret')
        check('Spotlight starts the terminal from the keyboard', vm.wait(r'\[64\] AURORA LAUNCH app=terminal', mark, 30)); time.sleep(4)
        server.open_gate('nav-done')
    before_orca = sound_seconds()
    if wait_event(server, vm, 'orca-keys', 600):   # part 2: Orca in the GTK window and the shell
        vm.wait(r'\[64\] AURORA WINDOW app=[^\n]*title=Orca test window', 0, 60); time.sleep(1)
        chord(vm, ['insert'], 't'); time.sleep(2)            # Orca+T: the time (Insert never reaches the window)
        vm.tap('x'); time.sleep(1)                           # an ordinary key does
        mark = len(vm.text()); chord(vm, ['ctrl'], 'f2'); vm.wait(r'\[64\] AURORA KEYNAV menubar', mark, 20); time.sleep(3)
        vm.tap('esc'); time.sleep(2); server.open_gate('orca-keys-done')
    if wait_event(server, vm, 'speech-done', 300):
        check('Orca and spd-say: speech in the sound capture (> 2 s)', sound_seconds() > before_orca + 2.0, '%.1f -> %.1f s' % (before_orca, sound_seconds()))
    if wait_event(server, vm, 'lock', 300):         # the lock screen: its keys stay in Aurora
        before = len(re.findall(r'AURORA A11Y BRIDGE KEY ', vm.text()))
        lock_mark = len(vm.text()); chord(vm, ['meta_l'], 'l'); vm.wait(r'\[64\] AURORA LOCKED', lock_mark, 30); time.sleep(1)
        vm.type('secretpw'); time.sleep(1); vm.tap('esc'); time.sleep(.5)
        mark = len(vm.text()); vm.type(NPW); time.sleep(.3); vm.tap('ret'); vm.wait(r'\[64\] AURORA UNLOCKED user=nexus', mark, 60); time.sleep(2)
        during = re.findall(r'AURORA A11Y BRIDGE KEY press keysym=0x([0-9a-f]+)', vm.text()[lock_mark:])
        secret = {'%x' % ord(c) for c in 'secretpw' + NPW if c.isalnum()}
        check('the screen reader got keys before the lock (monitor alive)', before > 0, before)
        check('no key typed on the lock screen reached the screen reader', not (set(during) & secret), during[:12])
        server.open_gate('lock-done')
    if wait_event(server, vm, 'toggle', 600):       # part 3: Super+Alt+S
        mark = len(vm.text()); chord(vm, ['meta_l', 'alt'], 's')
        on = seen(vm, r'\[64\] AURORA READER START uid=1000 pid=(\d+)', mark, 20)
        check('Super+Alt+S starts the screen reader as the user', on)
        check('the bridge Aurora started is ready (READY, keyboard monitor)', seen(vm, r'AURORA A11Y BRIDGE READY bus=\S+ keyboard_monitor=1', mark, 60),
              re.findall(r'AURORA A11Y BRIDGE [^\n]*', vm.text()[mark:])[:4]); time.sleep(8)
        mark = len(vm.text()); chord(vm, ['meta_l', 'alt'], 's')
        check('Super+Alt+S again stops it', seen(vm, r'\[64\] AURORA READER STOP uid=1000', mark, 20))
        check('the screen reader exited', seen(vm, r'\[64\] AURORA READER EXIT uid=1000', mark, 30))
        server.open_gate('toggle-done')
    if wait_event(server, vm, 'auth4', 600):        # part 4: the authentication dialog's field
        d = seen(vm, r'\[64\] AURORA WINDOW app=nexus.authenticate', 0, 60); time.sleep(2)   # (its own lines go to /tmp/auth4.out)
        check('the authentication dialog is up (broker staged)', d)
        vm.type('Pw-typed-123'); time.sleep(3); vm.shot('auth4'); server.open_gate('auth4-typed')
        wait_event(server, vm, 'auth4-tree', 120); mark = len(vm.text()); vm.tap('esc')
        seen(vm, r'\[64\] AURORA AUTH CANCELLED', mark, 30); time.sleep(1); server.open_gate('auth4-done')
    if wait_event(server, vm, 'text', 600):         # Spotlight's entry, the lock screen's password field
        chord(vm, ['ctrl'], 'spc'); time.sleep(1.5); vm.type('calc'); time.sleep(2); vm.shot('spotlight4'); vm.tap('esc'); time.sleep(1.5)
        lock_mark = len(vm.text()); chord(vm, ['meta_l'], 'l'); seen(vm, r'\[64\] AURORA LOCKED', lock_mark, 30); time.sleep(1.5)
        vm.type('secretpw'); time.sleep(2); vm.tap('esc'); time.sleep(.5)
        mark = len(vm.text()); vm.type(NPW); time.sleep(.3); vm.tap('ret')
        check('unlocked after the lock-screen text check', seen(vm, r'\[64\] AURORA UNLOCKED user=nexus', mark, 60)); time.sleep(2)
        server.open_gate('text-done')
    if wait_event(server, vm, 'relogin', 600):      # log out, sign in: the screen reader comes back by itself
        g = seen(vm, r'\[64\] AURORA GREETER reason=logout', 0, 60); time.sleep(3)
        check('logged out (greeter)', g)
        mark = len(vm.text()); vm.type(NPW); time.sleep(.3); vm.tap('ret')
        check('nexus signs in again', seen(vm, r'\[64\] AURORA UNLOCKED user=nexus', mark, 60))
        check('Aurora starts the screen reader by itself (kept setting)', seen(vm, r'\[64\] AURORA READER AUTOSTART uid=1000', mark, 60))
        server.open_gate('relogin-done')
    vm.wait(r'AURORCA DONE', 0, 900)
    lines = re.findall(r'^AURORCA (\S+) (PASS|FAIL)(.*)$', vm.text(), re.M)
    for name, state, detail in lines: check('guest: ' + name, state == 'PASS', detail.strip())
    for line in re.findall(r'^AURORCA-(?:AW|AW4|SAID|BR|SPD) [^\n]*', vm.text(), re.M): print('   ', line[:250], flush=True)
    check('every guest check ran', len(lines) >= 31, len(lines))
    check('the setting was saved on and off', re.search(r'AURORA READER SAVED uid=1000 screen_reader=1', vm.text()) and re.search(r'AURORA READER SAVED uid=1000 screen_reader=0', vm.text()))
    check('no typed password or secret in the log', all(pw not in vm.text() for pw in (NPW, 'secretpw', 'Pw-typed-123')))
    check("a bridge Aurora did not start gets no keys (KEYS REFUSED)", re.search(r'\[64\] AURORA READER KEYS REFUSED uid=1000', vm.text()))
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
