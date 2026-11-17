"""Multi-user sessions and logind on an installed Debian ext4 root, end to end through Aurora's greeter.

    python -I tests/test_aurora_sessions.py WORK --base BASE [--accel auto|whpx|tcg] [--smp 2] [--qmp-port 4741]
           [--hook-port 9141] [--nbd-port 9142]

BASE: an installed system (tests/test_x64_services.py --keep-installed; the desktop user nexus has a password, no
automatic login). WORK/stage.qcow2 over it gets the tree's services, desktop, aurora-app, nexus-auth + nexus-session
(04755), nexus-logind and the test client logind-test; a test hook on --hook-port runs as root after the boot units.
  hook   alice (uid 1201, no home yet, group audio) with a password; a private file of nexus      -> event ready
  GUI    the greeter lists both; alice signs in (first login); Spotlight > Terminal; in it alice runs id/env, tries
         nexus's private file, and logind-test can                                               -> gate alice
  hook   alice's terminal runs as alice; HOME/XDG_RUNTIME_DIR/XDG_SESSION_ID; home 0700 from /etc/skel + XDG folders;
         /run/user/1201 0700; nexus's file refused; logind: sessions, seat, session 2 active (Type wayland, Leader the
         agent), GetSessionByPID(terminal) = session 2, CanPowerOff yes for alice; console devices alice's, nexus's
         processes refused (input device, alice's runtime folder, the control socket); hostile logind requests;
         an idle inhibitor reaches Aurora                                                          -> event checked1
  GUI    Switch User (aurora-app ctl switch-user in alice's terminal); the greeter: nexus signs in   -> gate nexus
  hook   nexus active, alice online; alice's terminal is not among the windows; logind Active flags; console nexus's;
         alice's TakeDevice reader is revoked                                                      -> event checked2
  GUI    Super+L, Tab (Switch User), alice's password: her session and terminal come back           -> gate back
  hook   alice active, her terminal listed again; a delay inhibitor + Suspend through logind: PrepareForSleep(true),
         the delay honoured, the machine sleeps                                                    (host wakes it)
  GUI    the lock screen after the resume: alice's password                                         -> gate resumed
  hook   PrepareForSleep(false) seen, delay ordering in the journal                               -> event checked3
  GUI    Log Out (aurora-app ctl logout in her terminal): the greeter                               -> gate out
  hook   no process of alice left, /run/user/1201 empty, logind: session 2 removed (SessionRemoved), nexus's remains
         -> event checked4; GUI: nexus unlocks his session -> gate final; hook: PowerOff through logind (root): Aurora
         shuts down and the machine powers off.
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
ap.add_argument('--qmp-port', type=int, default=4741); ap.add_argument('--hook-port', type=int, default=9141)
ap.add_argument('--nbd-port', type=int, default=9142)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'aurora_sessions', 'checks': [], 'passed': False}
NPW, APW = pkg_vm.TEST_PASSWORD, 'Alice-Pass-71'

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
APW='@APW@'
LT=/usr/lib/nexus/tests/logind-test
ok() { echo "AURSESS $1 PASS ${2:-}"; }
bad() { echo "AURSESS $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/as.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/as.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 600 /tmp/as.out | tr '\n' ' ')"; fi; }
event() { wget -q -O /dev/null --post-data=x "$H/event/$1" || true; }
gate() { i=0; while ! wget -q -O /dev/null "$H/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt 3600 ] && return 1; sleep 1; done; }
as_nexus() { setpriv --reuid=1000 --regid=1000 --init-groups env HOME=/home/nexus XDG_RUNTIME_DIR=/run/user/1000 "$@"; }
as_alice() { setpriv --reuid=1201 --regid=1201 --init-groups env HOME=/home/alice XDG_RUNTIME_DIR=/run/user/1201 XDG_SESSION_ID=2 "$@"; }
show() { echo "AURSESS info $1:"; shift; "$@" 2>&1 | sed 's/^/AURSESS   /'; }
terminal_pid() { for p in $(pgrep -f 'aurora-app terminal'); do [ "$(stat -c %u /proc/$p)" = "$1" ] && { echo $p; return 0; }; done; return 1; }
do_logind() { nexusctl is-active nexus-logind.service && $LT manager | grep -q 'NCurrentSessions'; }
do_alice() { useradd -M -u 1201 -s /bin/bash -c Alice alice && printf 'alice:%s\n' "$APW" | chpasswd && usermod -aG audio alice && [ ! -e /home/alice ] && id alice; }
do_private() { echo 'nexus private data' > /home/nexus/private.txt && chown nexus:nexus /home/nexus/private.txt && chmod 600 /home/nexus/private.txt; }
do_id() { cat /tmp/alice-id.txt; grep -q 'uid=1201(alice)' /tmp/alice-id.txt && grep -q 'audio' /tmp/alice-id.txt; }
do_env() { grep -x 'XDG_RUNTIME_DIR=/run/user/1201' /tmp/alice-env.txt && grep -x 'HOME=/home/alice' /tmp/alice-env.txt && grep -x 'USER=alice' /tmp/alice-env.txt && grep -x 'XDG_SESSION_ID=2' /tmp/alice-env.txt && grep -x 'WAYLAND_DISPLAY=wayland-0' /tmp/alice-env.txt && grep -q '^DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1201/bus' /tmp/alice-env.txt; }
do_home() { m=$(stat -c %a:%U /home/alice) && echo "home $m" && [ "$m" = 700:alice ] && [ -f /home/alice/.bashrc ] && [ "$(stat -c %U /home/alice/.bashrc)" = alice ] && [ -d /home/alice/Documents ] && [ -f /home/alice/.config/user-dirs.dirs ]; }
do_runtime() { m=$(stat -c %a:%U /run/user/1201) && echo "runtime $m" && [ "$m" = 700:alice ] && [ -S /run/user/1201/wayland-0 ]; }
do_refused() { cat /tmp/alice-cat.txt; grep -q 'Permission denied' /tmp/alice-cat.txt && ! grep -q 'nexus private data' /tmp/alice-cat.txt; }
do_terminal() { p=$(terminal_pid 1201) && echo "terminal pid $p" && grep -E '^Uid:\s+1201' /proc/$p/status; }
do_sessions() { $LT list | tee /tmp/lt-list.txt; grep -q '(2,1201,alice,seat0,/org/freedesktop/login1/session/_32)' /tmp/lt-list.txt; }
do_session2() { $LT session 2 | tee /tmp/lt-s2.txt; grep -q 'SESSION Active=true' /tmp/lt-s2.txt && grep -q 'SESSION Name=alice' /tmp/lt-s2.txt && grep -q 'SESSION Type=wayland' /tmp/lt-s2.txt && grep -q 'SESSION Class=user' /tmp/lt-s2.txt; }
do_bypid() { p=$(terminal_pid 1201) && $LT by-pid $p | tee /tmp/lt-bp.txt && grep -q 'BYPID /org/freedesktop/login1/session/_32' /tmp/lt-bp.txt; }
do_seat() { $LT seat | tee /tmp/lt-seat.txt; grep -q 'SEAT ActiveSession=(2,/org/freedesktop/login1/session/_32)' /tmp/lt-seat.txt; }
do_alice_can() { cat /tmp/lt-alice.txt; grep -q 'CAN CanPowerOff yes' /tmp/lt-alice.txt && grep -q 'CAN CanHybridSleep na' /tmp/lt-alice.txt; }
do_console() { u=$(stat -c %u /dev/input/event0) && echo "event0 owner $u" && [ "$u" = 1201 ]; }
do_nexus_input() { ! as_nexus head -c 1 /dev/input/event0; }
do_nexus_runtime() { ! as_nexus ls /run/user/1201; }
do_nexus_control() { out=$(as_nexus /bin/aurora-app ctl lock 2>&1); echo "$out"; echo "$out" | grep -q 'permission denied'; }
do_root_control() { /bin/aurora-app ctl sessions | tee /tmp/as-sessions.txt; grep -q 'id=2 uid=1201 gid=1201 user=alice' /tmp/as-sessions.txt && grep -q 'state=active active=1' /tmp/as-sessions.txt; }
do_hostile() { as_alice $LT hostile | tee /tmp/lt-hostile.txt; grep -q 'HOSTILE DONE' /tmp/lt-hostile.txt && ! grep -q 'HOSTILE .* FAIL' /tmp/lt-hostile.txt; }
do_idle() { as_alice $LT inhibit idle block 12 > /tmp/lt-idle.txt 2>&1 & sleep 3; a=$(/bin/aurora-app ctl sessions | grep -o 'idle_inhibited=[01]'); wait; sleep 2; b=$(/bin/aurora-app ctl sessions | grep -o 'idle_inhibited=[01]'); cat /tmp/lt-idle.txt; echo "during $a after $b"; [ "$a" = idle_inhibited=1 ] && [ "$b" = idle_inhibited=0 ]; }
do_take_device() { as_alice $LT take-device 13 64 600 > /tmp/lt-dev.txt 2>&1 & echo $! > /tmp/lt-dev.pid; sleep 4; cat /tmp/lt-dev.txt; grep -q 'DEVICE TAKEN 13:64' /tmp/lt-dev.txt; }
do_nexus_active() { /bin/aurora-app ctl sessions | tee /tmp/as-s2.txt; grep -q 'id=1 uid=1000 gid=1000 user=nexus leader=1 state=active active=1' /tmp/as-s2.txt && grep -q 'id=2 uid=1201 .* state=online active=0' /tmp/as-s2.txt; }
do_parked() { /bin/aurora-app ctl windows | tee /tmp/as-w.txt; ! grep -q 'app=nexus.terminal' /tmp/as-w.txt && grep -q 'session_uid=1000' /tmp/as-w.txt; }
do_logind_switched() { $LT session 2 | grep -q 'SESSION Active=false' && $LT seat | grep -q 'SEAT ActiveSession=(1,'; }
do_console_back() { [ "$(stat -c %u /dev/input/event0)" = 1000 ]; }
do_revoked() { sleep 2; cat /tmp/lt-dev.txt; grep -q 'DEVICE REVOKED' /tmp/lt-dev.txt && grep -q 'SIGNAL PauseDevice' /tmp/lt-dev.txt; }
do_alice_back() { /bin/aurora-app ctl sessions | grep -q 'id=2 uid=1201 .* state=active active=1' && /bin/aurora-app ctl windows | tee /tmp/as-w2.txt | grep -q 'app=nexus.terminal' && grep -q 'session_uid=1201' /tmp/as-w2.txt; }
do_sleep_signals() { cat /tmp/lt-watch.txt; grep -q 'SIGNAL PrepareForSleep true' /tmp/lt-watch.txt && grep -q 'DELAY released after 1500 ms' /tmp/lt-watch.txt && grep -q 'SIGNAL PrepareForSleep false' /tmp/lt-watch.txt; }
do_sleep_order() { r=$(grep -o 'DELAY released after 1500 ms at=[0-9]*' /tmp/lt-watch.txt | sed 's/.*at=//'); s=$(grep 'nexus-logind' /var/log/journal/system.journal | grep 'sleeping: ' | tail -1 | cut -d' ' -f2); echo "released ${r}000 sleeping $s"; [ -n "$r" ] && [ -n "$s" ] && [ "$s" -ge "${r}000" ]; }
do_gone() { sleep 3; n=$(ps -u alice -o pid= | wc -l); echo "alice processes $n"; [ "$n" = 0 ] && [ -z "$(ls -A /run/user/1201 2>/dev/null)" ]; }
do_logind_removed() { cat /tmp/lt-sig.txt; grep -q 'SIGNAL SessionRemoved /org/freedesktop/login1 2' /tmp/lt-sig.txt && ! $LT list | grep -q '(2,1201'; }
check logind-running do_logind
check alice-account do_alice
check private-file do_private
show home-modes stat -c '%n %a %U' /home/nexus /home
show sessions-at-greeter $LT list
event ready
gate alice
check alice-id do_id
check alice-env do_env
check first-login-home do_home
check runtime-folder do_runtime
check other-users-file-refused do_refused
check terminal-is-alice do_terminal
check logind-ListSessions do_sessions
check logind-session-2 do_session2
check logind-GetSessionByPID do_bypid
check logind-seat do_seat
check logind-Can-from-alice-session do_alice_can
check console-devices-alice do_console
check nexus-cannot-read-input do_nexus_input
check nexus-cannot-reach-alice-runtime do_nexus_runtime
check nexus-control-refused do_nexus_control
check root-sees-sessions do_root_control
check logind-hostile do_hostile
check idle-inhibitor-to-aurora do_idle
check alice-TakeDevice do_take_device
$LT --log /tmp/lt-sig.txt watch 1800 > /dev/null 2>&1 &
event checked1
gate nexus
check nexus-active-alice-online do_nexus_active
check alice-windows-parked do_parked
check logind-active-switched do_logind_switched
check console-devices-nexus do_console_back
check alice-device-revoked do_revoked
kill $(cat /tmp/lt-dev.pid) 2>/dev/null
event checked2
gate back
check alice-back do_alice_back
$LT --log /tmp/lt-watch.txt watch 120 --delay 1500 > /dev/null 2>&1 &
sleep 3
event suspend
$LT call Suspend b:0
gate resumed
sleep 3
check prepare-for-sleep-signals do_sleep_signals
check delay-inhibitor-honoured do_sleep_order
event checked3
gate out
check alice-logged-out do_gone
check logind-session-removed do_logind_removed
show sessions-after-logout /bin/aurora-app ctl sessions
event checked4
gate final
show can-root $LT can
echo "AURSESS POWEROFF"
$LT call PowerOff b:0
sleep 120
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


def answer(vm, mark, user, seconds=90):
    m = vm.wait(r'\[64\] AURORA (?:UNLOCKED user=%s mode=(\w+)|LOCK FAILED code=(\d+))' % user, mark, seconds)
    return ('UNLOCKED', m.group(1)) if m.group(1) else ('FAILED', m.group(2))


def type_password(vm, text, user):
    mark = len(vm.text()); vm.type(text); time.sleep(.3); vm.tap('ret'); return answer(vm, mark, user)


def select_user(vm, name, key='right'):
    """Greeter: move the selection (Right/Left) until the account is selected (AURORA GREETER SELECT lines)."""
    for _ in range(8):
        mark = len(vm.text()); vm.tap(key)
        m = vm.wait(r'\[64\] AURORA GREETER SELECT user=(\S+)', mark, 20)
        if m.group(1) == name: return True
        time.sleep(.3)
    return False


def spotlight(vm, text):
    vm.point(960, 540); vm.key('ctrl', True); vm.tap('spc'); vm.key('ctrl', False); time.sleep(1.5); vm.type(text); time.sleep(1); vm.tap('ret')


def terminal_command(vm, command):
    vm.type(command); time.sleep(.3); vm.tap('ret'); time.sleep(2)


server = vm = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    def stage(fs):
        pkg_vm.refresh_services(fs)   # services, desktop, aurora-app, nexus-auth + nexus-session, nexus-logind, logind-test
        pkg_vm.put(fs, '/usr/lib/nexus/tests/test-hook', HOOK_RUNNER.replace('@PORT@', str(args.hook_port)), 0o755)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage, nbd_port=args.nbd_port), flush=True)
    hook = HOOK.replace('@PORT@', str(args.hook_port)).replace('@APW@', APW)
    server = pkg_vm.HookServer(hook, {}, WORK / 'uploads', None, port=args.hook_port)
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='sessions', accel=args.accel, smp=args.smp, memory='2048')
    vm.wait(r'\[64\] AURORA GREETER reason=boot user=nexus users=\d+', 0, 900)
    check('the greeter comes first at boot', True)
    check('hook: alice created, logind running (event ready)', wait_event(server, vm, 'ready'), re.findall(r'AURSESS [^\n]*', vm.text())[-8:])
    time.sleep(2); vm.shot('greeter')
    check('greeter: alice can be selected', select_user(vm, 'alice')); time.sleep(.5)
    verdict = type_password(vm, 'wrong-password', 'alice')
    check('greeter: a wrong password for alice is refused by the launcher (code 1)', verdict == ('FAILED', '1'), verdict)
    time.sleep(2.5); mark = len(vm.text())
    verdict = type_password(vm, APW, 'alice')
    login = re.search(r'\[64\] AURORA SESSION LOGIN user=alice uid=1201 id=2 agent=(\d+) first=1', vm.text()[mark:])
    check('greeter: alice signs in (first login, session 2, the launcher became her agent)', verdict == ('UNLOCKED', 'greeter') and login, (verdict, bool(login)))
    time.sleep(3); mark = len(vm.text()); spotlight(vm, 'terminal')
    launch = vm.wait(r'\[64\] AURORA LAUNCH app=terminal pid=(\d+)', mark, 60)
    win = vm.wait(r'\[64\] AURORA WINDOW app=nexus.terminal title=[^\n]*', mark, 120); time.sleep(2)
    terminal_command(vm, '/bin/sh -c "id > /tmp/alice-id.txt; env > /tmp/alice-env.txt; cat /home/nexus/private.txt > /tmp/alice-cat.txt 2>&1"')
    terminal_command(vm, '/usr/lib/nexus/tests/logind-test --log /tmp/lt-alice.txt can')
    vm.shot('alice-terminal'); time.sleep(2)
    server.open_gate('alice'); check('hook: checks of alice\'s session (event checked1)', wait_event(server, vm, 'checked1', 600))
    mark = len(vm.text()); terminal_command(vm, '/bin/aurora-app ctl switch-user')
    vm.wait(r'\[64\] AURORA GREETER reason=switch-user user=alice', mark, 60); time.sleep(1); select_user(vm, 'nexus', 'left'); time.sleep(.5)
    verdict = type_password(vm, NPW, 'nexus')
    check('Switch User: the greeter signs nexus in while alice\'s session stays', verdict == ('UNLOCKED', 'greeter'), verdict)
    time.sleep(3); vm.shot('nexus-session')
    server.open_gate('nexus'); check('hook: nexus active, alice parked (event checked2)', wait_event(server, vm, 'checked2', 600))
    mark = len(vm.text()); vm.key('meta_l', True); vm.tap('l'); vm.key('meta_l', False)
    vm.wait(r'\[64\] AURORA LOCKED reason=shortcut user=nexus', mark, 30); time.sleep(1); vm.tap('tab')
    vm.wait(r'\[64\] AURORA GREETER reason=switch-user user=nexus', mark, 30); time.sleep(.5); select_user(vm, 'alice'); time.sleep(.5)
    verdict = type_password(vm, APW, 'alice')
    check('lock screen > Tab > alice: her running session comes back with her password', verdict == ('UNLOCKED', 'greeter'), verdict)
    time.sleep(3); vm.shot('alice-back')
    server.open_gate('back')
    if check('hook: the sleep test starts (event suspend)', wait_event(server, vm, 'suspend', 600)):
        end = time.time() + 120; status = None
        while time.time() < end:
            status = vm.q.cmd('query-status').get('status')
            if status == 'suspended': break
            time.sleep(.5)
        check('Suspend through logind: the machine sleeps (after the delay inhibitor)', status == 'suspended', status)
        time.sleep(2); mark = len(vm.text()); vm.q.cmd('system_wakeup')
        vm.wait(r'\[64\] AURORA RESUMED locked=1', mark, 120); time.sleep(2)
        verdict = type_password(vm, APW, 'alice')
        check('after the resume the lock screen asks alice\'s password', verdict == ('UNLOCKED', 'lock'), verdict)
    server.open_gate('resumed'); check('hook: sleep signals (event checked3)', wait_event(server, vm, 'checked3', 600))
    time.sleep(1); mark = len(vm.text()); terminal_command(vm, '/bin/aurora-app ctl logout')
    vm.wait(r'\[64\] AURORA GREETER reason=logout', mark, 60)
    end = vm.wait(r'\[64\] AURORA SESSION END uid=1201', mark, 60)
    check('Log Out: the greeter, alice\'s session ends', bool(end))
    server.open_gate('out'); check('hook: alice gone (event checked4)', wait_event(server, vm, 'checked4', 600))
    time.sleep(1); verdict = type_password(vm, NPW, 'nexus')
    check('the greeter: nexus\'s session (still signed in) unlocks', verdict == ('UNLOCKED', 'greeter'), verdict)
    mark = len(vm.text()); server.open_gate('final')
    vm.wait(r'AURSESS POWEROFF', 0, 300)
    code = vm.exited(300)
    check('PowerOff through logind: Aurora shuts down and the machine powers off', code is not None, code)
    hook_lines = re.findall(r'^AURSESS (\S+) (PASS|FAIL)(.*)$', vm.text(), re.M)
    for name, state, detail in hook_lines: check('guest: ' + name, state == 'PASS', detail.strip())
    for line in re.findall(r'^AURSESS (?:info|  )[^\n]*', vm.text(), re.M)[:60]: print('   ', line, flush=True)
    check('every guest check ran', len(hook_lines) >= 30, len(hook_lines))
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
