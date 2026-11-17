"""Passwords on an installed Debian ext4 root: Aurora's greeter, lock screen and Settings > Users against what Debian's
own tools write (yescrypt, $y$), and sudo (PAM) against what Aurora writes.

    python -I tests/test_aurora_auth_debian.py WORK --base BASE [--accel auto|whpx|tcg] [--qmp-port 4667]
           [--hook-port 8967] [--nbd-port 8966]

BASE: an installed system (tests/test_x64_services.py --keep-installed). WORK/stage.qcow2 over it gets the tree's
services, desktop, aurora-app and the account helper (sysinstall.py services --aurora --tests: /usr/libexec/nexus-auth
04755 root) and a test hook served on --hook-port (the hook runs as root after the boot units; host gates order it):
  hook:  the helper is set-user-ID root; Debian's unix_chkpwd exists; `chpasswd` (the guest's own) sets PW1 and writes
         a $y$ yescrypt hash; sudo accepts PW1, refuses a wrong password                                   -> event ready
  GUI:   the greeter refuses a wrong password (code=1)                                          -> gate wrong
  hook:  the helper's failure tally for nexus counts 1                                           -> event tally
  GUI:   the greeter accepts PW1; Super+L, the lock screen accepts PW1; System Settings (Spotlight) > Users:
         Change Password PW1 -> PW2 (helper exit 0)                                             -> gate changed
  hook:  the hash is still $y$; sudo accepts PW2 and refuses PW1; the tally is back to 0       -> event sudo2
  GUI:   the lock screen accepts PW2 (helper)                                                     -> gate unprivileged
  hook:  the helper loses its set-user-ID bit                                                     -> event unpriv
  GUI:   the lock screen refuses a wrong password (code=1, not "unavailable") and accepts PW2: the unprivileged helper
         hands the check to unix_chkpwd                                                          -> gate nohelper
  hook:  the helper is moved away                                                                 -> event nohelper
  GUI:   the lock screen accepts PW2 through unix_chkpwd directly (the desktop's own fallback)    -> gate final
  hook:  the helper comes back (04755); AUTHDEB DONE. No kernel or user fault (the VM is then closed: an Aurora
         session outlives its main program).
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
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4667); ap.add_argument('--hook-port', type=int, default=8967)
ap.add_argument('--nbd-port', type=int, default=8966)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'aurora_auth_debian', 'checks': [], 'passed': False}
PW1, PW2 = 'Yescrypt-Pass-61', 'Settings-Pass-83'

HOOK_RUNNER = '''#!/bin/sh
# test-hook (sysinstall.py --tests) on this test's port
rm -f /run/test-hook.sh
if wget -q -T 5 -t 1 -O /run/test-hook.sh http://10.0.2.2:@PORT@/hook.sh 2>/dev/null && [ -s /run/test-hook.sh ]; then
    echo "TEST-HOOK running"; sh /run/test-hook.sh; echo "TEST-HOOK finished status=$?"
fi
cat /proc/sys/kernel/random/boot_id > /run/test-hook.done
'''
HOOK = r'''PATH=/usr/sbin:/usr/bin:/sbin:/bin
H=http://10.0.2.2:@PORT@
PW1='@PW1@'; PW2='@PW2@'
ok() { echo "AUTHDEB $1 PASS ${2:-}"; }
bad() { echo "AUTHDEB $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/ad.out 2>&1; then ok "$name" "$(tail -c 200 /tmp/ad.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 400 /tmp/ad.out | tr '\n' ' ')"; fi; }
event() { wget -q -O /dev/null --post-data=x "$H/event/$1" || true; }
gate() { i=0; while ! wget -q -O /dev/null "$H/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt 3600 ] && return 1; sleep 1; done; }
kind() { getent shadow nexus | cut -d: -f2 | cut -c1-3; }
sudo_as() { su nexus -s /bin/sh -c "printf '%s\n' '$1' | sudo -S -k -p '' id -u"; }
do_helper() { m=$(stat -c %a:%U /usr/libexec/nexus-auth) && echo "mode $m" && [ "$m" = 4755:root ]; }
do_chkpwd() { ls -l /usr/sbin/unix_chkpwd && [ -x /usr/sbin/unix_chkpwd ]; }
do_kind() { k=$(kind); echo "hash $k"; [ "$k" = '$y$' ]; }
do_chpasswd() { printf 'nexus:%s\n' "$PW1" | chpasswd && do_kind; }
do_sudo() { out=$(sudo_as "$1") && echo "id $out" && [ "$out" = 0 ]; }
do_sudo_refused() { ! sudo_as "$1"; }
do_tally() { t=$(cut -d' ' -f1 /var/lib/nexus-auth/nexus 2>/dev/null || echo none); echo "tally $t"; [ "$t" = "$1" ]; }
check helper do_helper
check chkpwd do_chkpwd
check chpasswd do_chpasswd
check sudo-pw1 do_sudo "$PW1"
check sudo-wrong-refused do_sudo_refused wrong-password
event ready
gate wrong
check tally-after-wrong do_tally 1
event tally
gate changed
check hash-after-settings do_kind
check sudo-pw2 do_sudo "$PW2"
check sudo-pw1-refused do_sudo_refused "$PW1"
check tally-reset do_tally 0
event sudo2
gate unprivileged
chmod 0755 /usr/libexec/nexus-auth; echo "AUTHDEB helper mode $(stat -c %a /usr/libexec/nexus-auth)"
event unpriv
gate nohelper
mv /usr/libexec/nexus-auth /usr/libexec/nexus-auth.aside; echo "AUTHDEB helper moved away"
event nohelper
gate final
mv /usr/libexec/nexus-auth.aside /usr/libexec/nexus-auth; chmod 4755 /usr/libexec/nexus-auth
check helper-restored do_helper
echo "AUTHDEB DONE"
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


def answer(vm, mark, seconds=60):
    """The greeter/lock screen's verdict after a password was typed: ('UNLOCKED', mode) or ('FAILED', code)."""
    m = vm.wait(r'\[64\] AURORA (?:UNLOCKED user=nexus mode=(\w+)|LOCK FAILED code=(\d+))', mark, seconds)
    return ('UNLOCKED', m.group(1)) if m.group(1) else ('FAILED', m.group(2))


def type_password(vm, text):
    mark = len(vm.text()); vm.type(text); time.sleep(.3); vm.tap('ret'); return answer(vm, mark)


def lock(vm):
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('l'); vm.key('meta_l', False)
    vm.wait(r'\[64\] AURORA LOCKED reason=shortcut user=nexus', mark, 30); time.sleep(1)


def spotlight(vm, text):
    vm.point(960, 540); vm.key('ctrl', True); vm.tap('spc'); vm.key('ctrl', False); time.sleep(1.5); vm.type(text); time.sleep(1); vm.tap('ret')


server = vm = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    def stage(fs):
        pkg_vm.refresh_services(fs)   # services, desktop, aurora-app, the account helper (04755), test-hook unit
        pkg_vm.put(fs, '/usr/lib/nexus/tests/test-hook', HOOK_RUNNER.replace('@PORT@', str(args.hook_port)), 0o755)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage, nbd_port=args.nbd_port), flush=True)
    hook = HOOK.replace('@PORT@', str(args.hook_port)).replace('@PW1@', PW1).replace('@PW2@', PW2)
    server = pkg_vm.HookServer(hook, {}, WORK / 'uploads', None, port=args.hook_port)
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='authdeb', accel=args.accel, smp=2, memory='2048')
    vm.wait(r'\[64\] AURORA GREETER reason=boot user=nexus', 0, 900)
    check('the installed system boots to the greeter (password set, no automatic login)', True)
    check('the hook set the password with chpasswd and sudo agrees (event ready)', wait_event(server, vm, 'ready'),
          re.findall(r'AUTHDEB [^\n]*', vm.text()))
    time.sleep(2); vm.shot('greeter')
    verdict = type_password(vm, 'not-the-password')
    check('greeter: a wrong password is refused (code 1 = wrong, not 5 = unavailable)', verdict == ('FAILED', '1'), verdict)
    server.open_gate('wrong'); check('the helper counted the failure (event tally)', wait_event(server, vm, 'tally', 120))
    time.sleep(1); verdict = type_password(vm, PW1)
    check('greeter: the chpasswd (yescrypt) password opens the session', verdict == ('UNLOCKED', 'greeter'), verdict)
    time.sleep(3); lock(vm); verdict = type_password(vm, PW1)
    check('lock screen: the same password unlocks', verdict == ('UNLOCKED', 'lock'), verdict)
    # Settings > Users (Spotlight starts System Settings: its log lines reach the serial console), PW1 -> PW2
    time.sleep(2); mark = len(vm.text()); spotlight(vm, 'system settings')
    win = vm.wait(r'AURORA WINDOW app=nexus.settings title=[^\n]* x=(-?\d+) y=(-?\d+)', mark, 120); sx, sy = int(win.group(1)), int(win.group(2))
    time.sleep(2); mark = len(vm.text()); vm.click(sx + 100, sy + 29 + 63)   # sidebar row 2: Users (title bar 29 px)
    page = vm.wait(r'\[64\] SETTINGS USERS count=(\d+) writable=(\d) helper=(\d)', mark, 60)
    check('Settings > Users: accounts writable through the helper', page.group(2) == '1' and page.group(3) == '1', page.group(0))
    c = vm.wait(r'\[64\] SETTINGS USERS CONTROLS password=(\d+),(\d+)', mark, 30); time.sleep(1)
    mark = len(vm.text()); vm.click(sx + int(c.group(1)), sy + int(c.group(2))); time.sleep(1)
    f = vm.wait(r'\[64\] SETTINGS USERS CONTROLS [^\n]*field0=(\d+),(\d+)', mark, 30)
    vm.click(sx + int(f.group(1)), sy + int(f.group(2))); time.sleep(.5); vm.shot('settings-form')
    mark = len(vm.text()); vm.type(PW1 + '\n' + PW2 + '\n' + PW2 + '\n')
    changed = vm.wait(r'\[64\] SETTINGS USERS Change password code=(\d+)', mark, 60); vm.shot('settings-changed')
    check('Settings > Users changes the password (helper exit 0)', changed.group(1) == '0', changed.group(0))
    server.open_gate('changed'); check('sudo agrees with the new password (event sudo2)', wait_event(server, vm, 'sudo2', 300))
    lock(vm); verdict = type_password(vm, PW2)
    check('lock screen: the Settings password unlocks (helper)', verdict == ('UNLOCKED', 'lock'), verdict)
    server.open_gate('unprivileged'); wait_event(server, vm, 'unpriv', 120)
    lock(vm); wrong = type_password(vm, 'not-the-password'); time.sleep(2.5); right = type_password(vm, PW2)
    check('without the set-user-ID bit the helper hands the check to unix_chkpwd: wrong refused (code 1), right unlocks',
          wrong == ('FAILED', '1') and right == ('UNLOCKED', 'lock'), (wrong, right))
    server.open_gate('nohelper'); wait_event(server, vm, 'nohelper', 120)
    lock(vm); verdict = type_password(vm, PW2)
    check('without the helper the desktop checks through unix_chkpwd itself', verdict == ('UNLOCKED', 'lock'), verdict)
    server.open_gate('final')
    vm.wait(r'AUTHDEB DONE', 0, 300)
    hook_lines = re.findall(r'^AUTHDEB (\S+) (PASS|FAIL)(.*)$', vm.text(), re.M)
    for name, state, detail in hook_lines: check('guest: ' + name, state == 'PASS', detail.strip())
    check('every guest check ran', {n for n, _, _ in hook_lines} >= {'helper', 'chkpwd', 'chpasswd', 'sudo-pw1', 'sudo-wrong-refused',
          'tally-after-wrong', 'hash-after-settings', 'sudo-pw2', 'sudo-pw1-refused', 'tally-reset', 'helper-restored'}, [n for n, _, _ in hook_lines])
    check('no kernel or user fault', not vm.faults(), vm.faults()[:3])   # (an Aurora session outlives its main program: the VM is closed)
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
