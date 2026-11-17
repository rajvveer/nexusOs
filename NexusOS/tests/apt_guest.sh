#!/bin/sh
# Guest side of tests/test_x64_apt.py (root, run by apttest.service). The host serves Debian's archive through a caching
# proxy and a signed test repository at http://10.0.2.2:PORT/nexus-test; "repo VARIANT" asks the host to republish it.
# Every check prints "APT <name> PASS|FAIL <detail>".
PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH DEBIAN_FRONTEND=noninteractive
HOST=http://10.0.2.2:@PORT@
ok() { echo "APT $1 PASS ${2:-}"; }
bad() { echo "APT $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/apt.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/apt.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 900 /tmp/apt.out | tr '\n' ' ')"; fi; }
repo() { wget -q -O - "$HOST/control/repo?variant=$1" >/dev/null; }
# the Google Chrome repository (https, added by google-chrome-stable) is not part of this test: set aside, restored at the end
for f in /etc/apt/sources.list.d/google-chrome*; do [ -e "$f" ] && mv "$f" "/tmp/$(basename "$f").aside"; done
APTGET="apt-get -y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold"

repo v1
check update sh -c 'apt-get update'
check secure-sources sh -c 'apt-cache policy | grep -q "deb.debian.org/debian trixie/main" && apt-cache policy | grep -q nexus-test'
check policy-test-repo sh -c 'apt-cache policy nexus-hello | grep -q "Candidate: 1.0"'
check install sh -c "$APTGET install nexus-hello cowsay jq"
check run-hello sh -c '[ "$(nexus-hello)" = "nexus-hello 1.0" ]'
check run-debian sh -c '/usr/games/cowsay moo | grep -q moo && [ "$(echo "{\"a\": 41}" | jq ".a + 1")" = 42 ]'
check maint-install sh -c 'cat /var/log/nexus-hello-maint.log; sed "s/ *$//" /var/log/nexus-hello-maint.log | grep -q "^preinst install$" && sed "s/ *$//" /var/log/nexus-hello-maint.log | grep -q "^postinst configure$"'
check adduser-in-postinst sh -c 'getent passwd nexushello | grep -q nologin'
check alternatives sh -c '[ "$(readlink -f /usr/bin/nexus-greet)" = /usr/bin/nexus-hello ] && update-alternatives --query nexus-greet | grep -q "Value: /usr/bin/nexus-hello"'
check daemon-started sh -c 'for i in $(seq 1 20); do nexusctl is-active --quiet nexus-hello && break; sleep 0.5; done; nexusctl status nexus-hello && pgrep -f "nexus-hello [-]-serve"'
check dpkg-status sh -c 'dpkg -s nexus-hello | grep -q "Status: install ok installed" && dpkg -L nexus-hello | grep -q /usr/bin/nexus-hello && dpkg -V nexus-hello'
echo "# local change" >> /etc/nexus-hello.conf
pid1=$(pgrep -f "nexus-hello [-]-serve" | head -1)
: > /var/log/nexus-hello-maint.log
repo v2
check update-v2 sh -c 'apt-get update'
check upgradable sh -c 'apt list --upgradable 2>/dev/null | grep -q "nexus-hello/nexus-test 2.0"'
check upgrade sh -c "$APTGET upgrade"
check run-hello-v2 sh -c '[ "$(nexus-hello)" = "nexus-hello 2.0" ]'
check maint-upgrade-order sh -c 'cat /var/log/nexus-hello-maint.log; [ "$(sed "s/ *\$//" /var/log/nexus-hello-maint.log | tr "\n" "|")" = "prerm upgrade 2.0|preinst upgrade 1.0 2.0|postrm upgrade 2.0|postinst configure 1.0|" ]'
check conffile-kept sh -c 'grep -q "# local change" /etc/nexus-hello.conf && [ -e /etc/nexus-hello.conf.dpkg-dist ] && grep -q "version=2" /etc/nexus-hello.conf.dpkg-dist'
check daemon-restarted sh -c 'for i in $(seq 1 20); do p=$(pgrep -f "nexus-hello [-]-serve" | head -1); [ -n "$p" ] && [ "$p" != "'$pid1'" ] && exit 0; sleep 0.5; done; exit 1'
check remove sh -c "$APTGET remove nexus-hello && ! [ -e /usr/bin/nexus-hello ] && [ -e /etc/nexus-hello.conf ] && dpkg -l nexus-hello | grep -q '^rc'"
check daemon-stopped sh -c '! pgrep -f "nexus-hello [-]-serve" && ! nexusctl is-active --quiet nexus-hello'
check alternative-removed sh -c '! [ -e /usr/bin/nexus-greet ]'
check purge sh -c "$APTGET purge nexus-hello && ! [ -e /etc/nexus-hello.conf ] && ! dpkg -l nexus-hello 2>/dev/null | grep -q '^.[ic]' && grep -q '^postrm purge$' /var/log/nexus-hello-maint.log && ! getent passwd nexushello"
check autoremove sh -c "$APTGET remove jq && $APTGET autoremove && ! dpkg -l libjq1 2>/dev/null | grep -q '^ii' && ! dpkg -l libonig5 2>/dev/null | grep -q '^ii'"
check dist-upgrade-noop sh -c "$APTGET dist-upgrade"
# ---------------------------------------------------------------- apt-secure: hostile repositories are refused
repo badsig
apt-get update >/tmp/badsig.out 2>&1; rc=$?
if grep -qE "NO_PUBKEY|Missing key|signature verification failed|invalid|not signed|is not valid" /tmp/badsig.out && grep -q "nexus-test" /tmp/badsig.out && apt-cache policy nexus-hello | grep -q "Candidate: 2.0"; then ok badsig-refused "rc=$rc"; else bad badsig-refused "rc=$rc $(tail -c 600 /tmp/badsig.out | tr '\n' ' ')"; fi
repo unsigned
apt-get update >/tmp/unsigned.out 2>&1; rc=$?
if grep -qE "is not signed|no longer signed|does not have a Release file" /tmp/unsigned.out; then ok unsigned-refused "rc=$rc"; else bad unsigned-refused "rc=$rc $(tail -c 600 /tmp/unsigned.out | tr '\n' ' ')"; fi
repo badhash
check update-badhash sh -c 'apt-get update'
$APTGET install nexus-hello >/tmp/badhash.out 2>&1; rc=$?
if [ $rc != 0 ] && grep -qiE "hash sum mismatch|unexpected size|Hashes of expected file" /tmp/badhash.out && ! dpkg -l nexus-hello 2>/dev/null | grep -q '^ii'; then ok badhash-refused "rc=$rc"; else bad badhash-refused "rc=$rc $(tail -c 600 /tmp/badhash.out | tr '\n' ' ')"; fi
repo v1
check recovers sh -c 'apt-get update && apt-get -y install nexus-hello && nexus-hello'
# ---------------------------------------------------------------- the system stays consistent
check dpkg-audit sh -c '[ -z "$(dpkg --audit)" ]'
check apt-check sh -c 'apt-get check'
for f in /tmp/google-chrome*.aside; do [ -e "$f" ] && mv "$f" "/etc/apt/sources.list.d/$(basename "$f" .aside)"; done
echo "APT DONE"
touch /run/apttest.done
