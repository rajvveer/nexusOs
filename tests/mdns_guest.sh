#!/bin/sh
# NexusOS mDNS / DNS-SD checks on an installed system (tests/test_x64_mdns.py), run as root by the test hook.
# Prints "MDNS <check> PASS|FAIL <detail>" and ends with "MDNS DONE". The host side (tests/netfix.py) is the network:
# it answers mDNS for fixture.local and advertises "Fixture Printer" (_ipp._tcp, relayed to tests/ipp_printer.py).
H=${NEXUS_TEST_HOST:-http://10.0.2.2:8979}   # test-hook: this run's hook server (nexus.testhook=PORT)
r() { if [ "$2" = 0 ]; then echo "MDNS $1 PASS $3"; else echo "MDNS $1 FAIL $3"; fi; }
gate() { i=0; while [ $i -lt "${2:-120}" ]; do wget -q -O /dev/null "$H/gate/$1" 2>/dev/null && return 0; sleep 1; i=$((i + 1)); done; return 1; }
event() { wget -q -O /dev/null --post-data=x "$H/event/$1" 2>/dev/null; }

# 1. the daemon is on by default and running
i=0; while [ ! -S /run/avahi-daemon/socket ] && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
st=$(nexusctl status avahi-daemon 2>&1 | head -3 | tr '\n' ' ')
[ -S /run/avahi-daemon/socket ] && avahi-daemon --check; r daemon-running $? "$st"
echo "MDNS-INFO hostname $(hostname) nsswitch $(grep '^hosts' /etc/nsswitch.conf)"
sleep 3
diag() {
    nexusctl logs -b -u avahi-daemon -n 40 2>&1 | sed 's/^/MDNS-LOG /'
    for f in /proc/net/igmp /proc/net/igmp6 /proc/net/if_inet6 /proc/net/dev_mcast; do sed "s|^|MDNS-PROC $f |" $f; done
    grep -v '^#' /etc/avahi/avahi-daemon.conf | grep . | sed 's/^/MDNS-CONF /'
}
diag

# 2. the guest publishes: a service by hand and an IPP Everywhere printer (cups-ipp-utils ippeveprinter, via DNS-SD)
avahi-publish -s "NexusOS Guest Web" _http._tcp 8080 "path=/nexus" >/tmp/publish.log 2>&1 &
PUB=$!
mkdir -p /tmp/ipp-spool
ippeveprinter -p 8631 -k -d /tmp/ipp-spool -f application/pdf "NexusOS Guest Printer" >/tmp/ippeve.log 2>&1 &
EVE=$!
sleep 6
kill -0 $PUB 2>/dev/null; r publish-running $? "$(head -c 200 /tmp/publish.log | tr '\n' ' ')"
kill -0 $EVE 2>/dev/null; r ippeveprinter-running $? "$(tail -c 300 /tmp/ippeve.log | tr '\n' ' ')"
event published

# 3. names: avahi's resolver and the C library (nss-mdns) find the fixture's .local name
out=$(avahi-resolve -4 -n fixture.local 2>&1); echo "$out" | grep -q '10\.0\.2\.2'; r resolve-v4 $? "$out"
out=$(avahi-resolve -6 -n fixture.local 2>&1); echo "$out" | grep -q 'fd00:2::2'; r resolve-v6 $? "$out"
out=$(getent hosts fixture.local 2>&1); echo "$out" | grep -q '10\.0\.2\.2'; r nss-mdns $? "$out"
out=$(getent hosts nosuchhost.local 2>&1); [ -z "$out" ]; r nss-mdns-unknown $? "$out"
out=$(avahi-resolve -a 10.0.2.2 2>&1); echo "$out" | grep -q 'fixture.local'; r reverse $? "$out"

# 4. DNS-SD browsing: the fixture's printer and our own services
# (-p escapes blanks in names as \032; -k keeps the raw service types; files, not echo, keep the backslashes)
avahi-browse -rtpk _ipp._tcp >/tmp/browse.txt 2>&1
sed 's/^/MDNS-BROWSE /' /tmp/browse.txt | head -20
grep '^=' /tmp/browse.txt | grep -q 'Fixture\\032Printer'; r browse-fixture-printer $? ""
grep '^=' /tmp/browse.txt | grep 'Fixture\\032Printer' | grep -q '10\.0\.2\.2;631;'; r browse-fixture-resolved $? ""
grep '^=' /tmp/browse.txt | grep 'Fixture\\032Printer' | grep -q ';IPv6;'; r browse-fixture-over-ipv6 $? "$(grep IPv6 /tmp/browse.txt | head -2 | tr '\n' ' ')"
grep '^=' /tmp/browse.txt | grep -q 'NexusOS\\032Guest\\032Printer'; r browse-own-printer $? ""
own=$(grep '^=' /tmp/browse.txt | grep 'NexusOS\\032Guest\\032Printer' | grep ';IPv4;' | head -1 | cut -d';' -f8,9 | tr ';' ':')
avahi-browse -rtpk _http._tcp >/tmp/browse.txt 2>&1
grep '^=' /tmp/browse.txt | grep 'NexusOS\\032Guest\\032Web' | grep -q ';8080;'; r browse-own-service $? "$(head -3 /tmp/browse.txt | tr '\n' ' ')"
out=$(avahi-browse -atpk 2>&1 | cut -d';' -f5 | sort -u | tr '\n' ' '); echo "$out" | grep -q '_ipp._tcp'; r browse-all $? "$out"

# 5. CUPS finds both printers by DNS-SD and prints to the fixture's (IPP Everywhere, through the relay to the host)
out=$(lpinfo --include-schemes dnssd -v 2>&1); echo "$out" | sed 's/^/MDNS-LPINFO /'
uri=$(echo "$out" | grep -o 'dnssd://Fixture[^ ]*' | head -1)
[ -n "$uri" ]; r cups-dnssd-fixture $? "$uri"
if [ -n "$uri" ]; then
    out=$(lpadmin -p FixtureMdns -E -v "$uri" -m everywhere 2>&1); r lpadmin-everywhere $? "$out"
    wget -q -O /tmp/mdns-doc.pdf "$H/files/mdns-doc.pdf"
    out=$(lp -d FixtureMdns /tmp/mdns-doc.pdf 2>&1); r lp-fixture $? "$out"
    gate printed 120; r fixture-got-job $? ""
fi
# CUPS does not list printers of this host as dnssd:// network printers: the address avahi resolved is used
if [ -n "$own" ]; then
    out=$(lpadmin -p GuestMdns -E -v "ipp://$own/ipp/print" -m everywhere 2>&1); r lpadmin-own $? "ipp://$own $out"
    out=$(lp -d GuestMdns /tmp/mdns-doc.pdf 2>&1); r lp-own $? "$out"
    i=0; while [ -z "$(ls /tmp/ipp-spool 2>/dev/null)" ] && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
    [ -n "$(ls -R /tmp/ipp-spool 2>/dev/null | grep -i pdf)" ]; r own-printer-got-job $? "$(ls -R /tmp/ipp-spool | tr '\n' ' ')"
fi

# 6. the host asks for our name (answered by avahi on the wire); then the daemon restarts cleanly
event ask-host
gate asked 60
nexusctl restart avahi-daemon; sleep 4
[ -S /run/avahi-daemon/socket ]; r daemon-restart $? ""
out=$(avahi-resolve -4 -n fixture.local 2>&1); echo "$out" | grep -q '10\.0\.2\.2'; r resolve-after-restart $? "$out"
kill $PUB $EVE 2>/dev/null
# diagnostics: the daemon's own log (the service logs through syslog from its chroot, where /dev/log is missing)
nexusctl stop avahi-daemon
timeout 12 avahi-daemon --no-chroot --debug >/tmp/avahi-debug.log 2>&1
sed 's/^/MDNS-DEBUG /' /tmp/avahi-debug.log | head -60
nexusctl start avahi-daemon
echo "MDNS DONE"
