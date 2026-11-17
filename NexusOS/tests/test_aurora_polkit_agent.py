"""Aurora's authentication agent (arch/x86_64/aurora_authdialog.c: aurora-app authenticate, and Settings > Users through
it) against sec2's authorization broker nexus-polkitd + pkexec, on an installed Debian ext4 root.

    python -I tests/test_aurora_polkit_agent.py WORK --base BASE [--polkit-dir DIR] [--accel auto|whpx|tcg] [--smp 2]
           [--qmp-port 4749] [--hook-port 9149] [--nbd-port 9144]

DIR (optional; default: this tree's out/nexus-polkitd.elf and out/pkexec.elf, built by desktop.py) holds nexus-polkitd and
pkexec; they are staged as /usr/libexec/nexus-polkitd and /usr/bin/pkexec
(04755 root) and the hook starts the broker as root.
  no dialog: org.nexus.policy.query (rule yes) is authorized at once; a standard user is refused pkexec's action
  dialog: pkexec's action for "id -u": a wrong password (attempt 1 of 5 shown), then the right one -> the program runs as
          root (prints 0); a second dialog cancelled with Esc -> exit 1, nothing run
  Settings > Users: New account -> the dialog (the form has no "Your password" field) -> the account exists
  lockout: five wrong passwords -> the dialog locks itself (countdown), then the broker answers DENIED throttled
  secure attention key (Super+Esc, aurora_sak.h): on the genuine dialog a banner confirms it; on the same dialog run
          with LD_PRELOAD (code injected into a look-alike) and on another window (Calculator) the real lock screen
          comes up at once, and the password unlocks it
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
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True); ap.add_argument('--polkit-dir', type=Path)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg')); ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4749); ap.add_argument('--hook-port', type=int, default=9149)
ap.add_argument('--nbd-port', type=int, default=9144)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'aurora_polkit_agent', 'checks': [], 'passed': False}
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
ok() { echo "AURAGENT $1 PASS ${2:-}"; }
bad() { echo "AURAGENT $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/ag.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/ag.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 600 /tmp/ag.out | tr '\n' ' ')"; fi; }
event() { for i in 1 2 3 4 5; do wget -q -T 10 -t 1 -O /dev/null --post-data=x "$H/event/$1" && return 0; sleep 1; done; true; }   # (retried: one POST must not decide the run)
gate() { i=0; while ! wget -q -O /dev/null "$H/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt 3600 ] && return 1; sleep 1; done; }
as_nexus() { setpriv --reuid=1000 --regid=1000 --init-groups env HOME=/home/nexus USER=nexus XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "$@"; }
AA=/bin/aurora-app
do_broker() { /usr/libexec/nexus-polkitd > /tmp/polkitd.log 2>&1 & i=0; while [ ! -S /run/nexus-polkit.sock ] && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done; [ -S /run/nexus-polkit.sock ]; }
do_admin() { getent group sudo | grep -qw nexus || usermod -aG sudo nexus; id nexus; id nexus | grep -q sudo; }
do_carol() { useradd -M -u 1301 -s /bin/bash carol && printf 'carol:%s\n' 'Carol-pw-731' | chpasswd && id carol; }
do_yes() { as_nexus $AA authenticate org.nexus.policy.query; }
do_standard() { setpriv --reuid=1301 --regid=1301 --init-groups env HOME=/tmp XDG_RUNTIME_DIR=/tmp $AA authenticate org.freedesktop.policykit.exec -- /usr/bin/id -u; r=$?; echo "exit $r"; [ $r = 3 ]; }
do_run_root() { cat /tmp/agent1.out; [ "$(cat /tmp/agent1.rc)" = 0 ] && grep -qx 0 /tmp/agent1.out; }
do_cancel() { cat /tmp/agent2.out; [ "$(cat /tmp/agent2.rc)" = 1 ] && ! grep -qx 0 /tmp/agent2.out; }
do_account() { id dora && getent passwd dora | grep -q '/home/dora'; }
do_throttled() { cat /tmp/agent4.out | tail -3; cat /tmp/agent4.rc; [ "$(cat /tmp/agent4.rc)" = 1 ] || [ "$(cat /tmp/agent4.rc)" = 4 ]; }
# the dialogs' and Settings' "[64] ..." lines go to these files: stream them to the console (the host follows them)
touch /tmp/agent1.out /tmp/agent2.out /tmp/agent4.out /tmp/agent5.out /tmp/settings.out
tail -q -n +1 -F /tmp/agent1.out /tmp/agent2.out /tmp/agent4.out /tmp/agent5.out /tmp/settings.out 2>/dev/null &
check broker-started do_broker
check nexus-is-admin do_admin
check standard-user do_carol
event ready
gate login
check rule-yes-no-dialog do_yes
check standard-user-refused do_standard
( as_nexus $AA authenticate org.freedesktop.policykit.exec --app "Agent test" -- /usr/bin/id -u > /tmp/agent1.out 2>&1; echo $? > /tmp/agent1.rc ) &
event dialog1
gate dialog1-done
sleep 2
check pkexec-program-ran-as-root do_run_root
( as_nexus $AA authenticate org.freedesktop.policykit.exec --app "Agent test" -- /usr/bin/id -u > /tmp/agent2.out 2>&1; echo $? > /tmp/agent2.rc ) &
event dialog2
gate dialog2-done
sleep 2
check cancel-runs-nothing do_cancel
( as_nexus env LD_PRELOAD=/lib/x86_64-linux-gnu/libc.so.6 $AA authenticate org.freedesktop.policykit.exec --app "Agent test" -- /usr/bin/id -u > /tmp/agent5.out 2>&1; echo $? > /tmp/agent5.rc ) &
event sak-preload
gate sak-preload-done
as_nexus $AA calculator > /tmp/calc.out 2>&1 &
event sak-other
gate sak-other-done
pkill -u nexus -f 'aurora-app calculator'; sleep 1
as_nexus $AA settings users > /tmp/settings.out 2>&1 &
event settings
gate settings-done
sleep 2
check settings-users-created-account do_account
pkill -u nexus -f 'aurora-app settings'
( as_nexus $AA authenticate org.freedesktop.policykit.exec --app "Agent test" -- /usr/bin/id -u > /tmp/agent4.out 2>&1; echo $? > /tmp/agent4.rc ) &
event dialog4
gate dialog4-done
sleep 2
check lockout-ends-dialog do_throttled
echo "AURAGENT DONE"
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


def seen(vm, pattern, mark, seconds):
    try: return vm.wait(pattern, mark, seconds)
    except TimeoutError: return None


def sak(vm):   # Super+Esc
    vm.key('meta_l', True); time.sleep(.1); vm.tap('esc'); time.sleep(.1); vm.key('meta_l', False); time.sleep(.5)


def unlock(vm):
    mark = len(vm.text()); vm.type(NPW); time.sleep(.3); vm.tap('ret')
    check('the password unlocks after the SAK', seen(vm, r'\[64\] AURORA UNLOCKED user=nexus', mark, 60)); time.sleep(1.5)


def dialog(vm, mark):
    m = seen(vm, r'\[64\] AURORA AUTH DIALOG action=(\S+) app=([^\n]*) user=(\S+)', mark, 60)
    if m: seen(vm, r'\[64\] AURORA WINDOW app=nexus.authenticate', mark, 60); time.sleep(1.5)
    return m


server = vm = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    broker, pkexec = ((args.polkit_dir / 'nexus-polkitd', args.polkit_dir / 'pkexec') if args.polkit_dir else
                      (ROOT / 'arch/x86_64/out/nexus-polkitd.elf', ROOT / 'arch/x86_64/out/pkexec.elf'))   # merged tree: its own build
    if not broker.is_file() or not pkexec.is_file(): raise SystemExit('no nexus-polkitd/pkexec in ' + str(args.polkit_dir))
    def stage(fs):
        pkg_vm.refresh_services(fs)
        pkg_vm.put(fs, '/usr/lib/nexus/tests/test-hook', HOOK_RUNNER.replace('@PORT@', str(args.hook_port)), 0o755)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
        pkg_vm.put(fs, '/usr/libexec/nexus-polkitd', broker.read_bytes(), 0o755)
        pkg_vm.put(fs, '/usr/bin/pkexec', pkexec.read_bytes(), 0o4755)
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage, nbd_port=args.nbd_port), flush=True)
    server = pkg_vm.HookServer(HOOK.replace('@PORT@', str(args.hook_port)), {}, WORK / 'uploads', None, port=args.hook_port)
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='agent', accel=args.accel, smp=args.smp, memory='2048')
    vm.wait(r'\[64\] AURORA GREETER reason=boot user=nexus', 0, 900)
    check('hook: broker up, nexus admin, carol standard (event ready)', wait_event(server, vm, 'ready', 600), re.findall(r'AURAGENT [^\n]*', vm.text())[-4:])
    time.sleep(2); mark = len(vm.text()); vm.type(NPW); time.sleep(.3); vm.tap('ret')
    check('greeter: nexus signs in', seen(vm, r'\[64\] AURORA UNLOCKED user=nexus', mark, 60)); time.sleep(2); server.open_gate('login')
    if wait_event(server, vm, 'dialog1', 300):   # wrong, then right
        mark = len(vm.text()); d = dialog(vm, 0)
        check('the dialog names the action, the application and the user', d and d.group(1) == 'org.freedesktop.policykit.exec' and d.group(2) == 'Agent test' and d.group(3) == 'nexus', d and d.group(0))
        vm.shot('dialog'); mark = len(vm.text()); vm.type('wrong-password-1'); vm.tap('ret')
        check('a wrong password is reported (attempt 1)', seen(vm, r'\[64\] AURORA AUTH WRONG attempt=1 reply=DENIED', mark, 30)); time.sleep(1.5); vm.shot('dialog-wrong')
        mark = len(vm.text()); vm.type(NPW); vm.tap('ret')
        check('the right password authorizes', seen(vm, r'\[64\] AURORA AUTH AUTHORIZED action=org.freedesktop.policykit.exec', mark, 30))
        check('pkexec ran the program (status 0)', seen(vm, r'\[64\] AURORA AUTH PROGRAM status=0', mark, 30))
        server.open_gate('dialog1-done')
    if wait_event(server, vm, 'dialog2', 300):   # cancelled
        d = seen(vm, r'\[64\] AURORA AUTH DIALOG action=org.freedesktop.policykit.exec', len(vm.text()) - 4000, 60); time.sleep(2)
        mark = len(vm.text()); sak(vm)
        g = seen(vm, r'\[64\] AURORA SAK genuine=(\d) pid=\d+ action=(\S+) app=([^\n]*)', mark, 20)
        check('SAK on the genuine dialog: confirmed (action, application)', g and g.group(1) == '1' and g.group(2) == 'org.freedesktop.policykit.exec' and g.group(3) == 'Agent test', g and g.group(0))
        check('the genuine dialog was recognised when it connected', re.search(r'\[64\] AURORA SAK dialog connected pid=\d+ action=org.freedesktop.policykit.exec', vm.text()))
        time.sleep(1); vm.shot('sak-genuine'); check('SAK did not lock the screen there', not re.search(r'AURORA LOCKED', vm.text()[mark:]))
        mark = len(vm.text()); vm.tap('esc')
        check('Esc cancels the dialog', seen(vm, r'\[64\] AURORA AUTH CANCELLED', mark, 30)); time.sleep(1)
        server.open_gate('dialog2-done')
    if wait_event(server, vm, 'sak-preload', 300):   # the same program with code injected: not trusted
        seen(vm, r'\[64\] AURORA AUTH DIALOG action=org.freedesktop.policykit.exec', len(vm.text()) - 4000, 60)
        seen(vm, r'\[64\] AURORA WINDOW app=nexus.authenticate', len(vm.text()) - 4000, 30); time.sleep(2)
        mark = len(vm.text()); sak(vm)
        g = seen(vm, r'\[64\] AURORA SAK genuine=(\d) pid=\d+ app_id=(\S+) -> lock', mark, 20)
        check('SAK on an LD_PRELOAD look-alike: not genuine, the screen locks', g and g.group(1) == '0' and seen(vm, r'\[64\] AURORA LOCKED reason=secure-attention', mark, 20), re.findall(r'AURORA (?:SAK|LOCKED)[^\n]*', vm.text()[mark:])[:3])
        time.sleep(1); vm.shot('sak-lock'); unlock(vm)
        vm.tap('esc'); seen(vm, r'\[64\] AURORA AUTH CANCELLED', mark, 30); time.sleep(1)
        server.open_gate('sak-preload-done')
    if wait_event(server, vm, 'sak-other', 300):   # an ordinary window
        seen(vm, r'\[64\] AURORA WINDOW app=nexus.calculator', len(vm.text()) - 4000, 60); time.sleep(2)
        mark = len(vm.text()); sak(vm)
        g = seen(vm, r'\[64\] AURORA SAK genuine=0 pid=\d+ app_id=nexus.calculator -> lock', mark, 20)
        check('SAK on another window: the real lock screen at once', g and seen(vm, r'\[64\] AURORA LOCKED reason=secure-attention', mark, 20))
        mark = len(vm.text()); sak(vm)
        check('SAK on the lock screen confirms it', seen(vm, r'\[64\] AURORA SAK lock-screen genuine=1', mark, 20))
        unlock(vm); server.open_gate('sak-other-done')
    if wait_event(server, vm, 'settings', 300):   # Settings > Users > Add User... through the agent
        c = seen(vm, r'\[64\] SETTINGS USERS CONTROLS password=(\d+),(\d+) add=(\d+),(\d+)', 0, 120)
        win = seen(vm, r'AURORA WINDOW app=nexus.settings title=[^\n]* x=(-?\d+) y=(-?\d+)', 0, 60)
        agent_on = re.search(r'\[64\] SETTINGS USERS count=\d+ writable=1 helper=1 [^\n]* agent=1', vm.text())
        check('Settings > Users uses the agent (broker reachable, writable accounts)', agent_on, re.findall(r'SETTINGS USERS count[^\n]*', vm.text())[-1:])
        if c and win:
            sx, sy = int(win.group(1)), int(win.group(2)); time.sleep(2)
            mark = len(vm.text()); vm.click(sx + int(c.group(3)), sy + int(c.group(4))); time.sleep(1)
            f = seen(vm, r'\[64\] SETTINGS USERS CONTROLS [^\n]*field0=(\d+),(\d+) field1=(\d+),(\d+) field2=(\d+),(\d+) field3=(-?\d+),(-?\d+) ok=(\d+),(\d+)', mark, 30)
            if check('the New account form opened', f):
                for i, text in enumerate(('Dora Example', 'dora', 'Dora-pw-5521')):
                    vm.click(sx + int(f.group(1 + 2 * i)), sy + int(f.group(2 + 2 * i))); time.sleep(.4); vm.type(text); time.sleep(.3)
                vm.shot('settings-form'); mark = len(vm.text()); vm.click(sx + int(f.group(9)), sy + int(f.group(10)))
                d = seen(vm, r'\[64\] AURORA AUTH DIALOG action=(\S+) app=System Settings user=nexus', mark, 60)
                check('Create opens the authentication dialog', d, d and d.group(0))
                seen(vm, r'\[64\] AURORA WINDOW app=nexus.authenticate', mark, 60); time.sleep(1.5); vm.shot('settings-dialog')
                mark = len(vm.text()); vm.type(NPW); vm.tap('ret')
                check('the account is added after authentication', seen(vm, r'\[64\] SETTINGS USERS Add user code=0', mark, 60))
        server.open_gate('settings-done')
    if wait_event(server, vm, 'dialog4', 300):   # lockout
        seen(vm, r'\[64\] AURORA AUTH DIALOG action=org.freedesktop.policykit.exec', len(vm.text()) - 4000, 60); time.sleep(2)
        for attempt in range(1, 6):
            mark = len(vm.text()); vm.type('nope-%d' % attempt); vm.tap('ret')
            seen(vm, r'\[64\] AURORA AUTH (WRONG attempt=%d|LOCKED)' % attempt, mark, 40); time.sleep(.5)
        vm.shot('dialog-locked')
        check('five wrong passwords lock the dialog (countdown shown)', re.search(r'\[64\] AURORA AUTH WRONG attempt=5', vm.text()) or re.search(r'\[64\] AURORA AUTH LOCKED', vm.text()))
        mark = len(vm.text()); vm.type(NPW); vm.tap('ret'); time.sleep(3)
        check('typing is refused while locked (no AUTHORIZED)', not re.search(r'AURORA AUTH AUTHORIZED', vm.text()[mark:]))
        vm.tap('esc'); time.sleep(1)
        server.open_gate('dialog4-done')
    vm.wait(r'AURAGENT DONE', 0, 600)
    lines = re.findall(r'^AURAGENT (\S+) (PASS|FAIL)(.*)$', vm.text(), re.M)
    for name, state, detail in lines: check('guest: ' + name, state == 'PASS', detail.strip())
    check('every guest check ran', len(lines) >= 9, len(lines))
    check('no typed password appears in the log', all(pw not in vm.text() for pw in (NPW, 'wrong-password-1', 'nope-3')))
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
