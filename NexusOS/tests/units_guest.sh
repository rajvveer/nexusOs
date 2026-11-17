#!/bin/sh
# Guest side of tests/test_x64_units.py (root, run by test-hook.service after every boot unit): socket, timer and path
# units, per-user managers, journal rate limiting. Every check prints "UNT <name> PASS|FAIL <detail>"; the units are
# the test's (/etc/nexus/services/t-*, /etc/nexus/user/u-*), see test_x64_units.py.
PATH=/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; export PATH
T=/usr/lib/nexus/tests/nexus-svc-test
ok() { echo "UNT $1 PASS ${2:-}"; }
bad() { echo "UNT $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/unt.out 2>&1; then ok "$name" "$(tail -c 200 /tmp/unt.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 600 /tmp/unt.out | tr '\n' ' ')"; fi; }
# asuser "COMMAND": run COMMAND as the desktop user nexus (uid 1000) with its runtime folder (a script: checks run in sh -c)
printf '#!/bin/sh\nexec su nexus -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/1000; export XDG_RUNTIME_DIR; $1"\n' > /usr/local/bin/asuser
chmod 0755 /usr/local/bin/asuser
waitfor() { i=0; while [ $i -lt "${2:-40}" ]; do sh -c "$1" >/dev/null 2>&1 && return 0; sleep 0.25; i=$((i + 1)); done; sh -c "$1"; }
boot=$(cat /var/lib/units-test.boot 2>/dev/null || echo 0); boot=$((boot + 1)); echo $boot > /var/lib/units-test.boot
echo "UNITS boot $boot start"

if [ "$boot" = 1 ]; then
  # ------------------------------------------------------------------ sockets, Accept=no
  check sock-listening sh -c 'nexusctl is-active t-sock.socket && [ -S /run/t-sock.sock ] && ! nexusctl is-active --quiet t-sock.service && nexusctl status t-sock.socket | grep -q "Listen: /run/t-sock.sock (Stream)"'
  check sock-activates sh -c "$T client /run/t-sock.sock hello | grep 'LISTEN fds=1 names=t-sock.socket uid=65534 fd=3 hello' && nexusctl is-active t-sock.service"
  check sock-second-conn sh -c "$T client /run/t-sock.sock again | grep 'fd=3 again'"
  check sock-reactivates sh -c "nexusctl stop t-sock.service && nexusctl is-active t-sock.socket && $T client /run/t-sock.sock third | grep third && nexusctl is-active t-sock.service"
  check sock-service-exit sh -c "$T client /run/t-sock.sock exit | grep exit; sleep 1; ! nexusctl is-active --quiet t-sock.service && $T client /run/t-sock.sock back | grep back"
  check sock-multi sh -c "$T dgram /run/t-multi.dgram first-dgram && sleep 1 && $T client /run/t-multi.sock multi | grep 'fds=2 names=multi:multi' && nexusctl logs -u t-multi-svc -g first-dgram -n 1 | grep first-dgram"
  check sock-tcp sh -c "$T client 127.0.0.1:9123 tcp-hello | grep 'fds=1 names=t-tcp.socket uid=0 fd=3 tcp-hello'"
  check sock-started-directly sh -c "nexusctl stop t-sock.service; nexusctl start t-sock.service && nexusctl status t-sock.service | grep -q 'Active: active' && $T client /run/t-sock.sock direct | grep 'fds=1 names=t-sock.socket'"
  # ------------------------------------------------------------------ sockets, Accept=yes
  check sock-accept sh -c "$T client /run/t-echo.sock ping | grep 'INETD fds=1 uid=65534 accepting=0 ping'"
  check sock-accept-gc sh -c 'sleep 1; ! nexusctl list | grep "t-echo@"'
  check sock-accept-tcp sh -c "$T client 127.0.0.1:9124 conn-hello | grep 'CONN fds=1 stdin-null=1 conn-hello'"
  rm -f /tmp/mc*; ( $T client /run/t-echo.sock sleep4 > /tmp/mc1 & $T client /run/t-echo.sock sleep4 > /tmp/mc2 & $T client /run/t-echo.sock sleep4 > /tmp/mc3 &
    sleep 1.5; $T client /run/t-echo.sock overflow > /tmp/mc4 2>&1; wait )
  check sock-maxconn sh -c 'cat /tmp/mc*; grep -q sleep4 /tmp/mc1 && grep -q sleep4 /tmp/mc2 && grep -q sleep4 /tmp/mc3 && grep -q "CLIENT eof" /tmp/mc4 && nexusctl logs -u nexusd -g "too many connections" -n 1 | grep t-echo'
  check sock-accept-after-burst sh -c "$T client /run/t-echo.sock later | grep 'INETD fds=1 uid=65534 accepting=0 later'"
  # ------------------------------------------------------------------ sockets: hostile and failure cases
  check sock-not-a-socket sh -c 'h=$(sha256sum /etc/hostname); ! nexusctl start t-badsock.socket; nexusctl is-failed --quiet t-badsock.socket && [ "$(sha256sum /etc/hostname)" = "$h" ] && nexusctl logs -u nexusd -g "is not a socket" -n 1 | grep hostname'
  check sock-addr-in-use sh -c '! nexusctl start t-busy.socket; nexusctl is-failed --quiet t-busy.socket && nexusctl is-active t-tcp.socket && nexusctl logs -u nexusd -g "t-busy.socket: bind" -n 1'
  check sock-bad-address sh -c '! nexusctl start t-badaddr.socket; nexusctl is-failed --quiet t-badaddr.socket && nexusctl logs -u nexusd -g "invalid ListenStream=99999" -n 1 | grep t-badaddr'
  check sock-trigger-limit sh -c "nexusctl start t-quit.socket; for i in 1 2 3 4 5 6; do timeout 5 $T client /run/t-quit.sock x >/dev/null 2>&1; sleep 0.3; done; sleep 1;
    nexusctl show t-quit.socket | grep -E '^(ActiveState|Result)='; nexusctl is-failed --quiet t-quit.socket && nexusctl show t-quit.socket | grep -Eq '^Result=(trigger-limit-hit|service-start-limit-hit)'"
  check sock-template-required sh -c '! nexusctl start t-notpl.socket; nexusctl is-failed --quiet t-notpl.socket && [ ! -e /run/t-notpl.sock ] && nexusctl logs -u nexusd -g "t-notpl.socket: its service" -n 1 | grep template'
  check list-sockets sh -c 'nexusctl list-sockets | grep "/run/t-sock.sock" | grep t-sock.service'
  # ------------------------------------------------------------------ timers
  check timer-boot sh -c '[ -s /run/t-boot.ran ] && nexusctl status t-boot.timer | grep "Triggers: t-boot.service"'
  rm -f /run/t-rep.ran /run/t-cal.ran /run/t-slow.ran
  nexusctl start t-rep.timer t-cal.timer t-slow.timer; sleep 9.5
  check timer-repeat sh -c 'n=$(wc -l < /run/t-rep.ran); echo "$n runs"; [ $n -ge 3 ] && [ $n -le 5 ]'
  check timer-repeat-interval sh -c 'prev=; for t in $(cat /run/t-rep.ran); do if [ -n "$prev" ]; then d=$(( (t - prev) / 1000000 )); echo "gap $d ms"; [ $d -ge 1800 ] || exit 1; fi; prev=$t; done'
  check timer-calendar sh -c 'n=$(wc -l < /run/t-cal.ran); last=$(nexusctl show t-cal.timer | sed -n "s/^LastTriggerUSec=//p"); echo "$n runs, last $last"; [ $n -ge 1 ] && [ $((last / 1000000 % 5)) = 0 ]'
  check timer-running-skip sh -c '[ $(wc -l < /run/t-slow.ran) = 1 ] && nexusctl is-active t-slow.service && nexusctl logs -u nexusd -g "t-slow.timer: t-slow.service still running" -n 1'
  check list-timers sh -c 'nexusctl list-timers | grep t-cal.timer | grep t-cal.service'
  nexusctl stop t-rep.timer t-cal.timer t-slow.timer t-slow.service
  check timer-stopped sh -c 'n=$(wc -l < /run/t-cal.ran); sleep 6; [ $(wc -l < /run/t-cal.ran) = $n ] && ! nexusctl is-active --quiet t-cal.timer'
  check timer-bad-calendar sh -c '! nexusctl start t-badcal.timer; nexusctl is-failed --quiet t-badcal.timer && nexusctl logs -u nexusd -g "invalid OnCalendar" -n 3 | grep t-badcal'
  check timer-missing-unit sh -c '! nexusctl start t-nounit.timer; nexusctl is-failed --quiet t-nounit.timer'
  old=$(( $(date +%s) - 3 * 86400 ))
  echo $old > /var/lib/nexus/timers/stamp-t-pers.timer; echo $old > /var/lib/nexus/timers/stamp-t-npers.timer; rm -f /var/lib/t-pers.ran /var/lib/t-npers.ran
  nexusctl restart t-pers.timer t-npers.timer; sleep 2
  check timer-persistent-catchup sh -c '[ -s /var/lib/t-pers.ran ] && [ $(cat /var/lib/nexus/timers/stamp-t-pers.timer) -gt '"$old"' ] && nexusctl logs -u nexusd -g "t-pers.timer: an elapse was missed" -n 1'
  check timer-not-persistent sh -c '[ ! -e /var/lib/t-npers.ran ] && [ $(cat /var/lib/nexus/timers/stamp-t-npers.timer) = '"$old"' ]'
  # ------------------------------------------------------------------ paths
  check path-exists sh -c 'touch /run/t-flag; waitfor() { i=0; while [ $i -lt 40 ]; do [ -s /run/t-pe.ran ] && [ ! -e /run/t-flag ] && return 0; sleep 0.25; i=$((i+1)); done; return 1; }; waitfor'
  check path-makedir sh -c '[ -d /tmp/t-watch ] && [ -d /run/t-spool ]'
  touch /tmp/t-watch/mod; sleep 1
  check path-changed sh -c 'echo data > /tmp/t-watch/file; i=0; while [ $i -lt 40 ]; do [ -s /run/t-pc.ran ] && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  check path-modified-vs-changed sh -c 'sleep 1; rm -f /run/t-pm.ran /run/t-pc2.ran; exec 3>>/tmp/t-watch/mod; echo x >&3; sleep 2; m=0; c=0; [ -s /run/t-pm.ran ] && m=1; [ -e /run/t-pc2.ran ] && c=1;
    exec 3>&-; i=0; while [ $i -lt 40 ] && [ ! -s /run/t-pc2.ran ]; do sleep 0.25; i=$((i+1)); done; echo "modified=$m changed-before-close=$c changed-after-close=$([ -s /run/t-pc2.ran ] && echo 1)"; [ $m = 1 ] && [ $c = 0 ] && [ -s /run/t-pc2.ran ]'
  check path-dir-not-empty sh -c 'touch /run/t-spool/job1; i=0; while [ $i -lt 40 ]; do grep -q job1 /run/t-pdir.ran 2>/dev/null && [ -z "$(ls /run/t-spool)" ] && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  check path-glob sh -c 'mkdir -p /run/t-glob; touch /run/t-glob/a.txt; sleep 1; [ ! -e /run/t-pglob.ran ] && touch /run/t-glob/b.job && i=0; while [ $i -lt 40 ]; do [ -s /run/t-pglob.ran ] && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  check path-trigger-limit sh -c 'touch /run/t-loop; sleep 4; nexusctl show t-ploop.path | grep -E "^(ActiveState|Result)="; nexusctl is-failed --quiet t-ploop.path && nexusctl show t-ploop.path | grep -q "^Result=trigger-limit-hit"'
  check list-paths sh -c 'nexusctl list-paths | grep t-pe.path | grep t-pe.service'
  # ------------------------------------------------------------------ user managers
  check user-manager sh -c 'nexusctl is-active user@1000.service && asuser "nexusctl --user ping" | grep user-manager && [ "$(stat -c %U:%a /run/user/1000/nexus/control)" = nexus:600 ]'
  check user-unit-at-login sh -c '[ "$(cat /home/nexus/u-hello.ran)" = 1000 ] && asuser "nexusctl --user is-active u-hello"'
  check user-ondemand sh -c 'asuser "nexusctl --user start u-ondemand" && asuser "nexusctl --user logs -u u-ondemand" | grep on-demand-user-output && asuser "nexusctl --user logs -u u-ondemand" | grep -x ".*nexus"'
  check user-journal-own sh -c '[ "$(stat -c %U /home/nexus/.local/state/nexus/journal/user.journal)" = nexus ] && ! nexusctl logs -g on-demand-user-output -n 1 | grep -q on-demand'
  check user-timer sh -c '[ -s /home/nexus/u-tick.ran ] && [ "$(stat -c %U /home/nexus/u-tick.ran)" = nexus ] && asuser "nexusctl --user list-timers" | grep u-tick.timer'
  check user-socket sh -c 'asuser "'"$T"' client /run/user/1000/u-sock.sock hi" | grep "uid=1000 fd=3 hi" && [ "$(stat -c %U:%a /run/user/1000/u-sock.sock)" = nexus:666 ]'
  check user-own-dir sh -c 'asuser "mkdir -p ~/.config/nexus/user && printf \"[Service]\nType=oneshot\nExecStart=/bin/echo own-unit-ran\n\" > ~/.config/nexus/user/u-own.service && nexusctl --user daemon-reload && nexusctl --user start u-own" && asuser "nexusctl --user logs -u u-own" | grep own-unit-ran'
  check user-enable sh -c 'asuser "nexusctl --user enable u-ondemand" && [ -L /home/nexus/.config/nexus/user-enabled/u-ondemand.service ]'
  check user-other-denied sh -c '! su nobody -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/1000 nexusctl --user ping"'
  check user-root-query sh -c 'XDG_RUNTIME_DIR=/run/user/1000 nexusctl --user ping | grep user-manager'
  # a user manager needs the user's own runtime folder (0700, made by the system manager): daemon has none, bin a 0755 one
  check user-manager-needs-runtime sh -c 'rm -rf /run/user/1 /run/user/2; mkdir -m 0755 /run/user/2; chown bin /run/user/2;
    out=$(timeout 10 su daemon -s /bin/sh -c "/usr/lib/nexus/nexusd --user" 2>&1); rc=$?; out2=$(timeout 10 su bin -s /bin/sh -c "/usr/lib/nexus/nexusd --user" 2>&1); rc2=$?;
    rm -rf /run/user/2; echo "$rc $out / $rc2 $out2"; [ $rc = 1 ] && [ $rc2 = 1 ] && echo "$out" | grep -q "mode 0700" && echo "$out2" | grep -q "mode 0700"'
  check user-cannot-session-other sh -c '! su nobody -s /bin/sh -c "nexusctl session-end 1000"; nexusctl is-active user@1000.service'
  check user-session-verb-refused sh -c 'asuser "nexusctl --user session 1000"; [ $? = 2 ]'
  check session-end sh -c 'nexusctl session-end 1000 && i=0; while [ $i -lt 60 ]; do ! nexusctl is-active --quiet user@1000.service && grep -q stopped /home/nexus/u-hello.stop && exit 0; sleep 0.25; i=$((i+1)); done; nexusctl status user@1000; exit 1'
  check session-again sh -c 'nexusctl session 1000 && i=0; while [ $i -lt 80 ]; do nexusctl is-active --quiet user@1000.service && asuser "nexusctl --user is-active u-hello" >/dev/null 2>&1 && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  # ------------------------------------------------------------------ parity: the kernel's named pipes
  check fifo-kernel-enxio sh -c "rm -f /run/kf; mkfifo -m 0600 /run/kf && [ -p /run/kf ] && $T fifoprobe /run/kf | grep 'errno=ENXIO'"
  check fifo-kernel-blocking sh -c '(sleep 1; echo blocking-hello > /run/kf) & timeout 10 cat /run/kf | grep blocking-hello'
  check fifo-kernel-rdwr sh -c "exec 3<>/run/kf; $T fifoprobe /run/kf | grep 'opened fifo=1' && echo rw-line >&3 && read -r l <&3 && echo \"\$l\" && [ \"\$l\" = rw-line ]"
  check fifo-kernel-ext4 sh -c 'rm -f /var/tmp/kf; mkfifo /var/tmp/kf && { (sleep 1; echo ext4-fifo > /var/tmp/kf) & } && timeout 10 cat /var/tmp/kf | grep ext4-fifo; r=$?; rm -f /var/tmp/kf /run/kf; exit $r'
  # ------------------------------------------------------------------ parity: IPv6 / dual stack, sequential packets, FIFO, netlink
  check sock-dual-v4 sh -c "nexusctl status t-dual.socket | grep -q 'Stream, IPv4+IPv6' && $T client 127.0.0.1:9125 v4-hello | grep 'names=t-dual.socket uid=0 fd=3 v4-hello peer=::ffff:127.0.0.1'"
  check sock-dual-v6 sh -c "$T client '[::1]:9125' v6-hello | grep 'fd=3 v6-hello peer=::1'"
  check sock-v6only sh -c "! $T client 127.0.0.1:9126 refused >/dev/null 2>&1 && $T client '[::1]:9126' v6only | grep 'fd=3 v6only peer=::1'"
  check sock-v6both-dgram sh -c "$T dgram 127.0.0.1:9127 v4-datagram && i=0; while [ \$i -lt 40 ]; do nexusctl logs -u t-v6both -g v4-datagram -n 1 | grep -q 'LISTEN datagram' && exit 0; sleep 0.25; i=\$((i+1)); done; exit 1"
  check sock-seqpacket sh -c "nexusctl status t-seqp.socket | grep -q SequentialPacket && $T seqclient /run/t-seqp.sock seq-hello | grep 'names=t-seqp.socket uid=0 fd=3 seq-hello'"
  check sock-fifo sh -c 'nexusctl status t-fifo.socket | grep -q "(FIFO)" && [ -p /run/t-fifo/pipe ] && [ "$(stat -c %a /run/t-fifo/pipe)" = 620 ] && [ "$(stat -c %a /run/t-fifo)" = 750 ] && ! nexusctl is-active --quiet t-fifo.service &&
    echo fifo-hello > /run/t-fifo/pipe && i=0; while [ $i -lt 40 ]; do nexusctl logs -u t-fifo -g fifo-hello -n 1 | grep -q "LISTEN fifo fds=1 names=t-fifo.socket fd=3 fifo-hello" && exit 0; sleep 0.25; i=$((i+1)); done; nexusctl logs -u t-fifo -n 5; exit 1'
  check sock-fifo-while-running sh -c 'nexusctl is-active t-fifo.service && echo second-line > /run/t-fifo/pipe && i=0; while [ $i -lt 40 ]; do nexusctl logs -u t-fifo -g second-line -n 1 | grep -q "fd=3 second-line" && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  check sock-fifo-remove-on-stop sh -c 'nexusctl stop t-fifo.socket t-fifo.service; [ ! -e /run/t-fifo/pipe ] && nexusctl start t-fifo.socket && [ -p /run/t-fifo/pipe ]'
  check sock-netlink sh -c 'nexusctl status t-nl.socket | grep -q "(Netlink)" && ip addr add 10.77.0.1/32 dev lo && i=0; while [ $i -lt 40 ]; do nexusctl logs -u t-nl -g "LISTEN netlink" -n 3 | grep -q "type=20" && exit 0; sleep 0.25; i=$((i+1)); done; nexusctl logs -u t-nl -n 5; exit 1'
  check sock-fifo-accept-refused sh -c '! nexusctl start t-fifoacc.socket; nexusctl is-failed --quiet t-fifoacc.socket && nexusctl logs -u nexusd -g "Accept=yes needs stream sockets" -n 1 | grep t-fifoacc'
  check sock-bad-netlink sh -c '! nexusctl start t-badnl.socket; nexusctl is-failed --quiet t-badnl.socket && nexusctl logs -u nexusd -g "invalid ListenNetlink=nosuchfamily" -n 1'
  # ------------------------------------------------------------------ parity: Debian's links (deb-systemd-helper) and timers
  check debian-timers-active sh -c 'nexusctl list-timers; for t in apt-daily apt-daily-upgrade logrotate dpkg-db-backup fstrim; do [ -L /etc/systemd/system/timers.target.wants/$t.timer ] || { echo "$t: no link"; exit 1; }; nexusctl is-active --quiet $t.timer || { echo "$t inactive"; exit 1; }; done'
  # apt-daily runs "flock -w", which needs POSIX timers (timer_create): kabi's posix_timers.c of the same wave; on a
  # kernel without them the only accepted failure is that one ("cannot set up timer")
  timers=$($T timerprobe); echo "UNITS $timers"
  check debian-timers-fire sh -c 'bad=0; for s in dpkg-db-backup logrotate apt-daily apt-daily-upgrade; do i=0;
    while [ $i -lt 480 ] && ! nexusctl logs -u nexusd -g "$s.service finished (result:" -n 1 | grep -q . && ! nexusctl logs -u nexusd -g "$s.service failed (result:" -n 1 | grep -q .; do sleep 0.5; i=$((i+1)); done;
    nexusctl logs -u nexusd -g "$s.timer: an elapse was missed" -n 1 | grep -q missed || { echo "$s: no catch-up"; bad=1; };
    if nexusctl logs -u nexusd -g "$s.service finished (result: success)" -n 1 | grep -q .; then echo "$s: success";
    elif [ "'"$timers"'" = "TIMERPROBE errno=ENOSYS" ] && nexusctl logs -u $s -n 20 | grep -qF "flock: cannot set up timer: Function not implemented"; then echo "$s: fired; flock -w needs timer_create (not in this kernel)";
    else echo "$s: failed"; nexusctl logs -u $s -n 8; bad=1; fi; done; exit $bad'
  check debian-timer-effects sh -c 'ls -l /var/lib/logrotate/status /var/backups/dpkg.status.0; [ -s /var/lib/logrotate/status ] && [ -e /var/backups/dpkg.status.0 ] && [ "$(cat /var/lib/nexus/timers/stamp-logrotate.timer)" -gt $(( $(date +%s) - 3600 )) ]'
  check debian-vendor-link sh -c '[ -s /run/t-vendorlink.ran ] && nexusctl is-active t-vendorlink.service'
  check debian-mask sh -c '[ ! -e /run/t-masked.ran ] && ! nexusctl start t-masked.service 2>/dev/null; [ ! -e /run/t-masked.ran ]'
  check user-debian-default-link sh -c '[ "$(cat /home/nexus/u-deb.ran)" = deb ] && asuser "nexusctl --user is-active u-deb"'
  check user-debian-wants-dir sh -c '[ "$(cat /home/nexus/u-debdep.ran)" = dep ]'
  check user-debian-sockets-link sh -c "asuser 'nexusctl --user is-active u-debsock.socket' && asuser '$T client /run/user/1000/u-debsock.sock deb-sock' | grep 'uid=1000 fd=3 deb-sock'"
  check user-debian-mask sh -c '[ ! -e /home/nexus/u-masked.ran ]'
  check user-session-bus-kept sh -c '! asuser "nexusctl --user status dbus.socket" >/dev/null 2>&1 && nexusctl is-active dbus-session@1000.service && [ -S /run/user/1000/bus ]'
  check user-debian-gpg-agent sh -c 'ls /etc/systemd/user/sockets.target.wants/; [ -L /etc/systemd/user/sockets.target.wants/gpg-agent.socket ] && asuser "nexusctl --user is-active gpg-agent.socket" && [ -S /run/user/1000/gnupg/S.gpg-agent ] &&
    asuser "gpg-connect-agent \"GETINFO version\" /bye" | grep "^D 2\." && asuser "nexusctl --user is-active gpg-agent.service"'
  # ------------------------------------------------------------------ journal rate limiting (journald.conf of the test: 2000 per 10 s)
  check ratelimit sh -c 'nexusctl start t-spam; sleep 1; logger -t ratecheck other-source-ok; sleep 1; n=$(nexusctl logs -u t-spam -o cat | grep -c spam-line-padding); echo "$n kept"; [ $n -ge 1990 ] && [ $n -le 4000 ] && nexusctl logs -g other-source-ok -n 1 | grep ratecheck'
  check ratelimit-notice sh -c 'i=0; while [ $i -lt 80 ]; do nexusctl logs -u nexusd -g Suppressed -n 3 | grep -E "Suppressed [0-9]+ messages from t-spam.service" && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  check ratelimit-manager-own sh -c 'nexusctl logs -u nexusd -n 400 | grep -q "Started t-spam" || nexusctl logs -u nexusd -g "t-spam.service finished" -n 1'
  check no-failed-system-units sh -c 'skip="^t-"; [ "'"$timers"'" = "TIMERPROBE errno=ENOSYS" ] && skip="^(t-|apt-daily)";
    nexusctl list | grep failed | grep -Ev "$skip"; ! nexusctl list | grep failed | grep -Eqv "$skip"'
else
  # ------------------------------------------------------------------ after a reboot
  check boot2-socket sh -c "nexusctl is-active t-sock.socket && $T client /run/t-sock.sock boot2 | grep boot2"
  check boot2-persistent-no-repeat sh -c '[ $(wc -l < /var/lib/t-pers.ran) = 1 ] && [ -s /var/lib/nexus/timers/stamp-t-pers.timer ]'
  check boot2-user-enabled sh -c 'i=0; while [ $i -lt 60 ]; do asuser "nexusctl --user show u-ondemand" | grep -q "^Result=success" && asuser "nexusctl --user logs -b -u u-ondemand" | grep -q on-demand-user-output && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  check boot2-user-at-login sh -c 'asuser "nexusctl --user is-active u-hello"'
  check boot2-paths sh -c 'nexusctl is-active t-pe.path && touch /run/t-flag && i=0; while [ $i -lt 40 ]; do [ -s /run/t-pe.ran ] && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
  check boot2-debian-timers-no-repeat sh -c 'sleep 3; for s in dpkg-db-backup logrotate apt-daily; do nexusctl is-active --quiet $s.timer || exit 1; nexusctl logs -b -u nexusd -g "$s.timer: " -n 50 | grep -qE "an elapse was missed|elapsed" && { echo "$s ran again"; exit 1; }; done; exit 0'
  check boot2-fifo sh -c '[ -p /run/t-fifo/pipe ] && echo boot2-fifo > /run/t-fifo/pipe && i=0; while [ $i -lt 40 ]; do nexusctl logs -u t-fifo -g boot2-fifo -n 1 | grep -q "fd=3 boot2-fifo" && exit 0; sleep 0.25; i=$((i+1)); done; exit 1'
fi
echo "UNITS DONE boot=$boot"
