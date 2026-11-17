#!/bin/sh
# Guest side of tests/test_x64_services.py: run as root by svctest.service (after every boot unit settled).
# Every check prints "SVC <name> PASS|FAIL <detail>"; the host test asserts on these lines and on the disk afterwards.
PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH
T=/usr/lib/nexus/tests/nexus-svc-test
ok() { echo "SVC $1 PASS ${2:-}"; }
bad() { echo "SVC $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/svc.out 2>&1; then ok "$name" "$(head -c 300 /tmp/svc.out | tr '\n' ' ')"; else bad "$name" "$(head -c 600 /tmp/svc.out | tr '\n' ' ')"; fi; }
as_user() { su nexus -c "$1"; }
mkdir -p /var/lib/svctest
BOOT=$(cat /var/lib/svctest/boot 2>/dev/null || echo 0); BOOT=$((BOOT + 1)); echo $BOOT > /var/lib/svctest/boot
echo "SVC-BOOT $BOOT"

if [ "$BOOT" = 1 ]; then
  # ---------------------------------------------------------------- first boot: installation and boot units
  check firstboot-done test -e /var/lib/nexus/bootstrap/done
  check dpkg-audit sh -c '[ -z "$(dpkg --audit)" ]'
  check dpkg-count sh -c 'n=$(dpkg-query -f "\${db:Status-Abbrev}\n" -W | grep -c "^ii"); echo "$n installed"; [ "$n" -ge 300 ]'
  check merged-usr sh -c '[ -L /bin ] && [ -L /sbin ] && [ -L /lib ] && [ "$(readlink /bin)" = usr/bin ]'
  check diversion-xdg-open sh -c 'dpkg-divert --list /usr/bin/xdg-open | grep -q xdg-open.debian && [ -e /usr/bin/xdg-open.debian ] && ! grep -q "xdg-utils" /usr/bin/xdg-open'
  for u in dbus cron chrony cups; do check "active-$u" nexusctl is-active $u; done
  check active-session-bus nexusctl is-active dbus-session@1000
  check firstboot-result sh -c 'nexusctl show nexus-firstboot | grep -q "^Result=success"'
  check printers-unit sh -c 'lpstat -v PDF && lpstat -d | grep -q PDF'
  check hostname sh -c '[ "$(hostname)" = nexus-svctest ] && [ "$(cat /proc/sys/kernel/hostname)" = nexus-svctest ]'
  check uname sh -c 'uname -sr; [ "$(uname -s)" = Linux ]'
  # coreutils open a target folder with O_PATH|O_DIRECTORY (cp/mv/install into a folder)
  check coreutils-into-folder sh -c 'rm -rf /tmp/cpt; mkdir /tmp/cpt && cp /etc/hostname /etc/hosts /tmp/cpt/ && cp -t /tmp/cpt /etc/passwd &&
    mv /tmp/cpt/hosts /tmp/cpt/hosts2 && touch /tmp/m1 && mv /tmp/m1 /tmp/cpt && install -m 600 -t /tmp/cpt /etc/hostname &&
    [ "$(ls /tmp/cpt | tr "\n" " ")" = "hostname hosts2 m1 passwd " ] && [ "$(stat -c %a /tmp/cpt/hostname)" = 600 ] && rm -rf /tmp/cpt'
  check user-account sh -c 'id nexus | grep -q "(sudo)" && id nexus | grep -q "(lpadmin)" && [ "$(getent shadow nexus | cut -d: -f2 | cut -c1-3)" = "\$y\$" ]'
  check root-locked sh -c 'getent shadow root | cut -d: -f2 | grep -q "^!"'
  check locales sh -c 'locale -a | grep -qx en_US.utf8 && locale -a | grep -qx hi_IN.utf8 && grep -q "LANG=en_US.UTF-8" /etc/default/locale'
  check locale-hindi sh -c 'LC_ALL=hi_IN.UTF-8 date -d 2026-01-15 +%B | grep -q "जनवरी"'
  check timezone sh -c '[ "$(cat /etc/timezone)" = Asia/Kolkata ] && [ "$(date +%Z)" = IST ]'
  check utmp-boot sh -c 'who -b | grep -q "system boot"'

  # ---------------------------------------------------------------- service manager
  check list nexusctl list
  check status-text sh -c 'nexusctl status cron | grep -q "Active: active (running)"'
  check deps sh -c 'nexusctl start t-b && [ -e /run/t-a.time ] && [ -e /run/t-b.time ] && [ $(cat /run/t-a.time) -le $(cat /run/t-b.time) ] && nexusctl is-active t-a'
  s=$(date +%s%N); nexusctl start t-notify; rc=$?; e=$(date +%s%N); ms=$(( (e - s) / 1000000 ))
  if [ $rc = 0 ] && [ $ms -ge 1300 ] && nexusctl status t-notify | grep -q "serving test requests"; then ok notify-ready "${ms} ms"; else bad notify-ready "rc=$rc ${ms} ms $(nexusctl status t-notify | tr '\n' ' ')"; fi
  s=$(date +%s%N); nexusctl start t-ready; rc=$?; e=$(date +%s%N); ms=$(( (e - s) / 1000000 ))
  if [ $rc = 0 ] && [ $ms -ge 1800 ] && [ -e /run/t-ready.flag ]; then ok ready-path "${ms} ms"; else bad ready-path "rc=$rc ${ms} ms"; fi
  nexusctl start --no-block t-crash; sleep 14
  n=$(wc -l < /run/t-crash.log 2>/dev/null || echo 0)
  if nexusctl show t-crash | grep -q "^Result=start-limit-hit" && nexusctl is-failed --quiet t-crash && [ "$n" = 5 ]; then ok start-limit "$n starts"; else bad start-limit "starts=$n $(nexusctl show t-crash | tr '\n' ' ')"; fi
  # exponential backoff: the gaps between starts grow (RestartSec=200ms doubling)
  awk 'NR>1 {print $1-p} {p=$1}' /run/t-crash.log > /tmp/gaps
  if awk 'BEGIN{ok=1; prev=0} {if ($1 < 180 || $1 < prev*1.6) ok=0; prev=$1} END{exit !ok}' /tmp/gaps; then ok backoff "$(tr '\n' ' ' < /tmp/gaps)"; else bad backoff "$(tr '\n' ' ' < /tmp/gaps)"; fi
  check reset-failed sh -c 'nexusctl reset-failed t-crash && nexusctl is-failed --quiet t-crash; [ $? != 0 ]'
  nexusctl start t-flaky; sleep 3
  if nexusctl is-active --quiet t-flaky && [ "$(cat /var/lib/t-flaky.count)" = 3 ]; then ok restart-recovers "after 2 failures"; else bad restart-recovers "$(nexusctl status t-flaky | tr '\n' ' ') count=$(cat /var/lib/t-flaky.count)"; fi
  check user-env sh -c 'nexusctl start t-user && grep -q "uid=65534 gid=65534" /tmp/t-user.env && grep -q "cwd=/tmp FOO=bar baz HOME=/nonexistent USER=nobody INVOCATION=yes" /tmp/t-user.env && cat /tmp/t-user.env'
  check exec-missing sh -c '! nexusctl start t-badexec && nexusctl is-failed --quiet t-badexec'
  check condition sh -c 'nexusctl start t-cond && nexusctl show t-cond | grep -q "^Result=condition" && ! nexusctl is-active --quiet t-cond'
  check junk-unit sh -c 'nexusctl start t-junk && nexusctl logs -u nexusd -g "unknown or invalid setting" -n 5 | grep -q t-junk'
  check exec-stop sh -c 'nexusctl start t-stop && nexusctl stop t-stop && [ "$(cat /run/t-stop.flag)" = stopping ] && ! nexusctl is-active --quiet t-stop'
  check restart-verb sh -c 'p1=$(nexusctl show t-stop | grep -c x); nexusctl start t-stop && a=$(nexusctl show t-stop | sed -n "s/^MainPID=//p") && nexusctl restart t-stop && b=$(nexusctl show t-stop | sed -n "s/^MainPID=//p") && [ "$a" != "$b" ] && [ "$b" != 0 ]'
  check user-template-own sh -c "su nexus -c 'nexusctl start t-own@1000' && nexusctl is-active t-own@1000 && [ \"\$(ps -o user= -p \$(nexusctl show t-own@1000 | sed -n s/^MainPID=//p))\" = nexus ]"
  check user-template-other sh -c "su nexus -c 'nexusctl start t-own@0'; [ \$? = 4 ] && ! nexusctl is-active --quiet t-own@0"
  check user-denied sh -c "su nexus -c 'nexusctl stop cron'; [ \$? = 4 ] && nexusctl is-active cron"
  check control-hostile-root $T control
  check control-hostile-user su nexus -c "$T control"
  check user-cannot-start-manager sh -c '! su nexus -c "/usr/lib/nexus/nexusd --system" && nexusctl ping'
  check enable-disable sh -c 'nexusctl disable cron && ! nexusctl is-enabled --quiet cron && [ -e /etc/nexus/disabled/cron.service ] && nexusctl enable t-ready && [ -L /etc/nexus/enabled/t-ready.service ]'

  # ---------------------------------------------------------------- journal, syslog, kmsg, dmesg
  logger -t svctest -p user.warning "syslog record via /dev/log"
  sleep 1
  check journal-syslog sh -c 'nexusctl logs -g "syslog record via /dev/log" -n 1 | grep -q "svctest"'
  # the pid in a syslog record comes from the kernel (SCM_CREDENTIALS), not from the text a user writes
  check journal-syslog-pid sh -c 'su nexus -c "logger -t spoof --id=1 spoofed-pid-record" 2>/dev/null; sleep 1; nexusctl logs -g spoofed-pid-record -n 1;
    nexusctl logs -g spoofed-pid-record -n 1 | grep -q "spoof\[" && ! nexusctl logs -g spoofed-pid-record -n 1 | grep -q "spoof\[1\]"'
  check journal-stdout sh -c 'nexusctl logs -u t-user -n 5 | grep -q "env written"'
  check journal-priority sh -c 'nexusctl start t-log; nexusctl logs -u t-log -p err | grep -q "t-log error line" && ! nexusctl logs -u t-log -p err | grep -q "t-log line 5 "'
  check journal-kernel sh -c 'nexusctl logs -k -n 50 | grep -q "\[64\]"'
  check journal-follow sh -c 'timeout 5 nexusctl logs -f -n 0 > /tmp/follow.out & sleep 1; logger -t svctest follow-marker; sleep 2; wait; grep -q follow-marker /tmp/follow.out'
  check journal-perms sh -c '[ "$(stat -c %a:%G /var/log/journal)" = 2750:adm ] && su nexus -c "nexusctl logs -n 1" && ! su nobody -s /bin/sh -c "nexusctl logs -n 1" 2>/dev/null'
  check kmsg-root $T kmsg
  check kmsg-user su nexus -c "$T kmsg"
  check dmesg-root sh -c 'dmesg | grep -q "\[64\]"'
  check dmesg-user sh -c '! su nexus -c dmesg 2>&1 | grep -q "\[64\]"'
  # rotation: 64 KiB files, at most 3, records keep flowing
  printf '[Journal]\nSystemMaxFileSize=64K\nSystemMaxFiles=3\nForwardToConsole=no\n' > /etc/nexus/journald.conf
  kill -HUP $(cat /run/nexus/nexusd.lock); sleep 1
  for i in 1 2 3 4 5 6; do nexusctl restart t-log; done
  check journal-rotation sh -c 'ls -l /var/log/journal; [ -e /var/log/journal/system.journal.1 ] && [ -e /var/log/journal/system.journal.2 ] && [ ! -e /var/log/journal/system.journal.3 ] && [ $(stat -c %s /var/log/journal/system.journal) -le 70000 ]'
  logger -t svctest "after rotation"; sleep 1
  check journal-after-rotation sh -c 'nexusctl logs -g "after rotation" -n 1 | grep -q svctest'

  # ---------------------------------------------------------------- kernel interfaces
  check rlimits-root $T rlimit
  check rlimits-user su nexus -c "$T rlimit"
  check pam-limits sh -c "su nexus -c 'ulimit -n'"

  # ---------------------------------------------------------------- sudo / PAM
  check sudo-ok sh -c "su nexus -c 'echo nexus-test-pw | sudo -S -k id -u' 2>/dev/null | tail -1 | grep -qx 0"
  check sudo-wrong sh -c "! su nexus -c 'echo wrong | sudo -S -k id -u' 2>/tmp/sudo.err; grep -q 'incorrect password' /tmp/sudo.err"
  useradd -m -s /bin/sh guest 2>/dev/null; echo 'guest:guest-pw' | chpasswd
  check sudo-not-admin sh -c "! su guest -c 'echo guest-pw | sudo -S -k id -u' 2>/tmp/sudo2.err; grep -q 'not in the sudoers' /tmp/sudo2.err"
  check sudo-logged sh -c 'nexusctl logs -g "incorrect password attempt" -n 3 | grep -q sudo'
  # AT_SECURE: ld.so must ignore LD_PRELOAD in the set-user-ID sudo (the probe leaves /tmp/authpre-ran when loaded)
  rm -f /tmp/authpre-ran
  check sudo-ld-preload-ignored sh -c "su nexus -c 'echo nexus-test-pw | LD_PRELOAD=/usr/lib/nexus/tests/authpre.so sudo -S -k id -u' 2>/dev/null | tail -1 | grep -qx 0 && [ ! -e /tmp/authpre-ran ]"
  check ld-preload-control sh -c "rm -f /tmp/authpre-ran; su nexus -c 'LD_PRELOAD=/usr/lib/nexus/tests/authpre.so /bin/true' && [ -e /tmp/authpre-ran ] && rm -f /tmp/authpre-ran"

  # ---------------------------------------------------------------- D-Bus
  check dbus-system sh -c 'dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep -q org.freedesktop.DBus'
  check dbus-session sh -c "su nexus -c 'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId'"
  check notifications sh -c "su nexus -c 'XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus /usr/lib/nexus/tests/nexus-notify-test svctest-notification hello-from-dbus 1500' | grep -q 'server=Aurora/NexusOS/1.2 caps=1 id=.* closed=1'"
  check machine-id sh -c '[ -s /etc/machine-id ] && [ "$(cat /etc/machine-id | wc -c)" = 33 ]'

  # ---------------------------------------------------------------- time
  check timedatectl-status sh -c 'timedatectl status | grep -q "Time zone: Asia/Kolkata"'
  check ntp-sync sh -c 'for i in $(seq 1 30); do chronyc -n sources 2>/dev/null | grep -q "^\^\* 10.0.2.2" && exit 0; sleep 1; done; chronyc -n sources; exit 1'
  check ntp-synchronized sh -c 'timedatectl show | grep -q NTPSynchronized=yes'
  check set-ntp-false sh -c 'timedatectl set-ntp false && ! nexusctl is-active --quiet chrony && timedatectl show | grep -q NTP=no'
  check time-syscalls-root $T time
  check time-syscalls-user su nexus -c "$T time"
  check set-time sh -c 'timedatectl set-time "2030-01-02 03:04:05" && date +%Y | grep -qx 2030'
  check set-timezone sh -c 'timedatectl set-timezone Europe/Berlin && [ "$(date -d 2030-07-01 +%Z)" = CEST ] && [ "$(readlink /etc/localtime)" = /usr/share/zoneinfo/Europe/Berlin ]'
  check set-timezone-bad sh -c '! timedatectl set-timezone ../../etc/passwd && ! timedatectl set-timezone Nowhere/City'
  check timedatectl-user sh -c "! su nexus -c 'timedatectl set-timezone UTC' 2>/dev/null"
  check set-ntp-true sh -c 'timedatectl set-ntp true && nexusctl is-active chrony && for i in $(seq 1 40); do [ $(date +%Y) -lt 2030 ] && exit 0; sleep 1; done; exit 1'
  timedatectl set-timezone Asia/Kolkata
  check list-timezones sh -c 'timedatectl list-timezones | grep -qx Asia/Kolkata && [ $(timedatectl list-timezones | wc -l) -gt 300 ]'
  printf '[Journal]\nSystemMaxFileSize=4M\nSystemMaxFiles=8\n' > /etc/nexus/journald.conf; kill -HUP $(cat /run/nexus/nexusd.lock)
  logger -t svctest "svctest boot 1 end"; sleep 1
fi

if [ "$BOOT" = 2 ]; then
  # ---------------------------------------------------------------- second boot: persistence
  check firstboot-skipped sh -c 'nexusctl show nexus-firstboot | grep -q "^Result=condition"'
  check disabled-persists sh -c '! nexusctl is-active --quiet cron && ! nexusctl is-enabled --quiet cron'
  check enabled-persists sh -c 'nexusctl start t-ready && nexusctl is-active t-ready'
  check journal-this-boot sh -c '[ $(nexusctl logs -b -g "-- boot --" | wc -l) = 1 ]'
  check timezone-persists sh -c '[ "$(date +%Z)" = IST ]'
  check journal-previous-boot sh -c 'nexusctl logs -g "svctest boot 1 end" -n 1 | grep -q svctest && [ -z "$(nexusctl logs -b -g "svctest boot 1 end")" ]'
  check dbus-again nexusctl is-active dbus
  nexusctl enable cron
fi
echo "SVC DONE boot=$BOOT"
touch /run/svctest.done
