#!/bin/sh
# Guest side of tests/test_x64_upower.py: root, run by test-hook.service on an installed system whose ACPI tables carry
# the power test SSDT (tests/acpi_test_ssdt.py: AC adapter, battery BAT0, lid, processors with _PSS). The host
# ($NEXUS_TEST_HOST, http://10.0.2.2:PORT) serves upower-test, takes events (POST /event/NAME) and opens gates (GET /gate/NAME) after it changed
# the simulated hardware. Every check prints "UPW <name> PASS|FAIL <detail>"; the end is "UPW DONE".
PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH
HOST=${NEXUS_TEST_HOST:-http://10.0.2.2:8979}   # test-hook: this run's hook server (nexus.testhook=PORT)
ok() { echo "UPW $1 PASS ${2:-}"; }
bad() { echo "UPW $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/upw.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/upw.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 900 /tmp/upw.out | tr '\n' ' ')"; fi; }
event() { wget -q -O /dev/null --post-data="${2:-x}" "$HOST/event/$1" || true; }
gate() { i=0; while ! wget -q -O /dev/null "$HOST/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt "${2:-300}" ] && return 1; sleep 1; done; }
upload() { [ -f "$2" ] && wget -q -O /dev/null --post-file="$2" "$HOST/upload/$1" || true; }
as_user() { runuser -u nexus -- env HOME=/home/nexus USER=nexus LOGNAME=nexus XDG_RUNTIME_DIR=/run/user/1000 LANG=C.UTF-8 "$@"; }
T=/usr/lib/nexus/tests/upower-test

# ---------------------------------------------------------------- the service
check daemon-installed test -x /usr/libexec/nexus-upowerd
check service-active sh -c 'i=0; until nexusctl is-active nexus-upower >/dev/null 2>&1; do i=$((i+1)); [ $i -gt 30 ] && exit 1; sleep 1; done; nexusctl status nexus-upower'
check bus-name sh -c 'dbus-send --system --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.ListNames | grep -q "org.freedesktop.UPower\""'
check activation-files sh -c 'grep -l nexus-upowerd /usr/share/dbus-1/system-services/*.service | wc -l | grep -qx 3 && test -f /usr/share/dbus-1/system.d/nexus-upower.conf'
check kernel-profile sh -c 'cat /sys/power/policy/profile /sys/power/policy/profile_choices /sys/power/policy/lid_state'
check cpufreq-driver sh -c 'cat /sys/devices/system/cpu/cpufreq/policy0/scaling_driver /sys/devices/system/cpu/cpufreq/policy0/scaling_governor'

# ---------------------------------------------------------------- API conformance, profiles, holds, hostile calls (root)
$T check >/tmp/upw-check.log 2>&1; status=$?
cat /tmp/upw-check.log
[ $status = 0 ] && ok api-check "$(grep -c '^UPTEST' /tmp/upw-check.log) lines" || bad api-check "$(grep 'UPTEST FAIL' /tmp/upw-check.log | head -5 | tr '\n' ' ')"
check dbus-send-client sh -c 'dbus-send --system --print-reply --dest=org.freedesktop.UPower /org/freedesktop/UPower org.freedesktop.UPower.EnumerateDevices | grep -q battery_'

# ---------------------------------------------------------------- authorization: nobody is refused, the session user may switch
runuser -u nobody -- $T set performance >/tmp/upw-nobody.log 2>&1; status=$?
[ $status = 3 ] && ok unprivileged-denied "$(cat /tmp/upw-nobody.log)" || bad unprivileged-denied "status $status $(cat /tmp/upw-nobody.log)"
check profile-unchanged-after-denial sh -c 'test "$(cat /sys/power/policy/profile)" = balanced'
check session-user-allowed as_user $T set power-saver
check kernel-follows-user sh -c 'test "$(cat /sys/power/policy/profile)" = power-saver'
check root-back-to-balanced $T set balanced
# the kernel file written directly (Aurora's battery menu does that): the API follows through the uevent
echo performance > /sys/power/policy/profile 2>/dev/null || echo balanced > /sys/power/policy/profile
sleep 1
check api-follows-kernel sh -c "$T state | grep -q \"profile=\$(cat /sys/power/policy/profile)\""
echo balanced > /sys/power/policy/profile

# ---------------------------------------------------------------- Chrome's Battery Status API (navigator.getBattery)
CHROME=$(head -1 /nexus/chrome-desktop.args 2>/dev/null | tr -d '\r')
if [ -n "$CHROME" ] && [ -x "$CHROME" ]; then
    CHROME_ARGS=$(sed -n '2,$p' /nexus/chrome-desktop.args | tr -d '\r' | grep -v -e '^--user-data-dir' -e '^https\?:' -e '^file:' -e '^$' -e '^--ozone' -e '^--enable-features=UseOzone' | tr '\n' ' ')
    cat > /tmp/battery.html <<'EOF'
<!doctype html><title>battery</title><script>
function post(name, b) {
  fetch('http://10.0.2.2:8979/event/chrome-' + name, {method: 'POST', mode: 'no-cors',
        body: JSON.stringify({level: b.level, charging: b.charging, chargingTime: b.chargingTime, dischargingTime: b.dischargingTime})});
}
if (!navigator.getBattery) fetch('http://10.0.2.2:8979/event/chrome-error', {method: 'POST', mode: 'no-cors', body: 'no navigator.getBattery'});
else navigator.getBattery().then(b => {
  post('initial', b);
  for (const e of ['levelchange', 'chargingchange', 'dischargingtimechange', 'chargingtimechange']) b.addEventListener(e, () => post(e, b));
}, e => fetch('http://10.0.2.2:8979/event/chrome-error', {method: 'POST', mode: 'no-cors', body: String(e)}));
</script>
EOF
    sed -i "s|http://10.0.2.2:8979|$HOST|g" /tmp/battery.html   # the page posts to this run's hook server
    chmod 644 /tmp/battery.html; rm -rf /tmp/upw-chrome
    as_user "$CHROME" $CHROME_ARGS --headless=new --user-data-dir=/tmp/upw-chrome --no-first-run --no-default-browser-check file:///tmp/battery.html >/tmp/upw-chrome.log 2>&1 &
    CHROME_PID=$!
    ok chrome-started "pid $CHROME_PID"
else
    bad chrome-started "no Chrome in /nexus/chrome-desktop.args"
fi

# ---------------------------------------------------------------- signals while the host changes the simulated hardware
$T watch 400 >/tmp/upw-watch.log 2>&1 &
WATCH=$!
sleep 2
for step in initial ac-off low ac-on removed inserted; do
    event "step-$step"
    gate "$step" 120 || bad "gate-$step" timeout
    sleep 4
    echo "UPW STATE $step $($T state)"
    echo "UPW SYSFS $step $(cat /sys/class/power_supply/*/status /sys/class/power_supply/*/capacity /sys/class/power_supply/*/online /sys/class/power_supply/*/present 2>/dev/null | tr '\n' ' ')"
done
event steps-done
sleep 3
kill $WATCH 2>/dev/null; wait $WATCH 2>/dev/null
cat /tmp/upw-watch.log | head -200
upload watch.log /tmp/upw-watch.log
[ -n "${CHROME_PID:-}" ] && { kill $CHROME_PID 2>/dev/null; sleep 2; upload chrome.log /tmp/upw-chrome.log; }

# ---------------------------------------------------------------- still healthy afterwards
check daemon-alive sh -c 'pidof nexus-upowerd || pgrep -f nexus-upowerd'
check service-still-active nexusctl is-active nexus-upower
check journal-no-errors sh -c '! nexusctl status nexus-upower 2>&1 | grep -qi -e segfault -e "core dumped"'
echo "UPW DONE"
