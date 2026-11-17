#!/bin/sh
# Guest side of tests/test_x64_printing.py: root, run by test-hook.service while the Aurora session is up. The host
# ($NEXUS_TEST_HOST, this run's port) proxies Debian's archive, serves test documents, lists the documents its IPP Everywhere test printer
# (ipp://10.0.2.2:$IPP_PORT/ipp/print; the test passes --ipp-port, default 8971) received (/files/printed/index, /files/printed/NAME), takes events (POST /event/NAME:
# printer-off/printer-on, <app>-launched) and opens gates (GET /gate/NAME) once it has driven an application's print
# dialog. Every check prints "PRN <name> PASS|FAIL <detail>".
PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH DEBIAN_FRONTEND=noninteractive
HOST=${NEXUS_TEST_HOST:-http://10.0.2.2:8979}   # test-hook: this run's hook server
IPP_PORT=${IPP_PORT:-8971}
IPP=ipp://10.0.2.2:$IPP_PORT/ipp/print
ok() { echo "PRN $1 PASS ${2:-}"; }
bad() { echo "PRN $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/prn.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/prn.out | tr '\n' ' ')"; else bad "$name" "$(tail -c 900 /tmp/prn.out | tr '\n' ' ')"; fi; }
event() { wget -q -O /dev/null --post-data="${2:-x}" "$HOST/event/$1" || true; }
gate() { i=0; while ! wget -q -O /dev/null "$HOST/gate/$1" 2>/dev/null; do i=$((i + 1)); [ $i -gt "${2:-600}" ] && return 1; sleep 1; done; }
upload() { [ -f "$2" ] && wget -q -O /dev/null --post-file="$2" "$HOST/upload/$1" || true; }
get() { wget -q -O "$2" "$HOST/files/$1" && chmod 644 "$2"; }
as_user() { runuser -u nexus -- env HOME=/home/nexus USER=nexus LOGNAME=nexus XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
            DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus LANG=C.UTF-8 "$@"; }
printed() { wget -q -O - "$HOST/files/printed/index" 2>/dev/null | wc -l; }
# a document the IPP printer received (not matched before) whose text contains $1 (pdftotext); prints the local copy
newjob() {
    end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt $end ]; do
        for name in $(wget -q -O - "$HOST/files/printed/index" 2>/dev/null); do
            grep -qx "$name" /tmp/prn.seen 2>/dev/null && continue
            wget -q -O "/tmp/printed-$name" "$HOST/files/printed/$name" || continue
            if pdftotext "/tmp/printed-$name" - 2>/dev/null | grep -q "$1"; then echo "$name" >> /tmp/prn.seen; echo "/tmp/printed-$name"; return 0; fi
        done
        sleep 2
    done
    return 1
}
newpdf() {   # a cups-pdf output after the mark whose text contains $1, or whose name contains $3
    end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt $end ]; do
        for f in $(find /root/PDF /home/nexus/PDF /var/spool/cups-pdf -type f -name '*.pdf' -newer /tmp/prn.mark 2>/dev/null); do
            if pdftotext "$f" - 2>/dev/null | grep -q "$1"; then echo "$f"; return 0; fi
            [ -n "${3:-}" ] && case "$f" in *"$3"*) pdfinfo "$f" >/dev/null 2>&1 && { echo "$f"; return 0; } ;; esac
        done
        sleep 2
    done
    return 1
}
mark() { touch /tmp/prn.mark; sleep 1; }
job_ok() {   # job_ok NAME MARKER SECONDS [pdf [NAME-PART]]
    if [ "${4:-}" = pdf ]; then f=$(newpdf "$2" "$3" "${5:-}"); else f=$(newjob "$2" "$3"); fi
    if [ -n "$f" ]; then ok "$1" "$f $(head -c 5 "$f") $(pdfinfo "$f" 2>/dev/null | grep -E '^(Pages|Producer|Creator):' | tr '\n' ' ')"; upload "$1.pdf" "$f"; return 0; fi
    bad "$1" "no document with $2: $(lpstat -o 2>&1 | tail -3 | tr '\n' ' ') $(tail -4 /var/log/cups/error_log 2>/dev/null | tr '\n' ' ')"; return 1
}

# ---------------------------------------------------------------- CUPS and the Print to PDF queue
check cups-active sh -c 'nexusctl is-active cups && lpstat -r | grep -q "scheduler is running"'
check pdf-queue sh -c 'lpstat -p PDF && lpstat -v PDF | grep -q cups-pdf:/'
for d in NEXUS-CLI-PDF NEXUS-CLI-IPP NEXUS-HELD-JOB NEXUS-OFFLINE-JOB NEXUS-USER-JOB; do get "doc-$d.pdf" "/tmp/$d.pdf"; done
mark
check lp-pdf-submit lp -d PDF -t nexus-cli /tmp/NEXUS-CLI-PDF.pdf
job_ok print-to-pdf NEXUS-CLI-PDF 120 pdf
check pdf-text-file sh -c 'printf "plain text job\n" > /tmp/plain.txt && lp -d PDF /tmp/plain.txt && sleep 8 && ls /root/PDF | grep -q plain'

# ---------------------------------------------------------------- an IPP Everywhere network printer, added by URI (driverless)
# (the test printer is on the host; mDNS discovery needs IPv4 multicast, which the network stack does not have yet)
check ipp-printer-reachable sh -c "ipptool -tv $IPP get-printer-attributes.test > /tmp/ipp.txt; rc=\$?; grep -E 'printer-state |document-format-supported|PASS|FAIL' /tmp/ipp.txt; exit \$rc"
check driverless-queue lpadmin -p NexusIPP -E -v "$IPP" -m everywhere -D "NexusOS Test Printer" -L "QEMU host network"
check driverless-ppd sh -c 'lpoptions -p NexusIPP -l | head -4; grep -E "^\*(cupsFilter2|ModelName|NickName|DefaultPageSize)" /etc/cups/ppd/NexusIPP.ppd; lpoptions -p NexusIPP -l | grep -qi PageSize && grep -q "^\*cupsFilter2" /etc/cups/ppd/NexusIPP.ppd'
check default-printer sh -c 'lpadmin -d NexusIPP && lpstat -d | grep -q "system default destination: NexusIPP"'
check lp-ipp-submit lp -d NexusIPP -t nexus-ipp /tmp/NEXUS-CLI-IPP.pdf
job_ok ipp-everywhere-job NEXUS-CLI-IPP 180
check ipp-job-completed sh -c 'for i in $(seq 1 60); do lpstat -W completed -o NexusIPP | grep -q NexusIPP && exit 0; sleep 1; done; exit 1'
check user-can-print sh -c 'runuser -u nexus -- lp -d NexusIPP -t nexus-user /tmp/NEXUS-USER-JOB.pdf'
job_ok user-job-printed NEXUS-USER-JOB 180

# ---------------------------------------------------------------- hostile and failure cases
# a user without administrator rights (the desktop user is in lpadmin, CUPS' SystemGroup, and may manage jobs)
useradd -M -s /usr/sbin/nologin prntest 2>/dev/null
check user-cannot-add-printer sh -c '! runuser -u prntest -- lpadmin -p Rogue -E -v file:/dev/null -m raw 2>/tmp/rogue.txt && ! lpstat -p Rogue 2>/dev/null; cat /tmp/rogue.txt'
check user-cannot-cancel-others sh -c 'cupsdisable NexusIPP && id=$(lp -d NexusIPP /tmp/NEXUS-CLI-IPP.pdf | sed -n "s/.*is \(NexusIPP-[0-9]*\).*/\1/p") && [ -n "$id" ] &&
    ! runuser -u prntest -- cancel "$id" 2>/tmp/cancel.txt && cat /tmp/cancel.txt && lpstat -o NexusIPP | grep -q "$id" && cancel "$id"'
check admin-user-cancels-own sh -c 'id=$(runuser -u nexus -- lp -d NexusIPP /tmp/NEXUS-CLI-IPP.pdf | sed -n "s/.*is \(NexusIPP-[0-9]*\).*/\1/p") && [ -n "$id" ] && runuser -u nexus -- cancel "$id" && ! lpstat -o NexusIPP | grep -q "$id "'
userdel prntest 2>/dev/null
before=$(printed)
check paused-queue-holds-job sh -c 'lp -d NexusIPP /tmp/NEXUS-HELD-JOB.pdf && sleep 6 && lpstat -o NexusIPP | grep -q NexusIPP && [ "$(wget -q -O - '"$HOST"'/files/printed/index | wc -l)" = '"$before"' ]'
check resumed-queue-prints cupsenable NexusIPP
job_ok resumed-job-printed NEXUS-HELD-JOB 180
event printer-off; sleep 3
check printer-gone-job-waits sh -c 'lp -d NexusIPP /tmp/NEXUS-OFFLINE-JOB.pdf && sleep 25 && lpstat -o NexusIPP | grep -q NexusIPP && nexusctl is-active cups'
event printer-on; sleep 2
check printer-back sh -c "date; for i in \$(seq 1 120); do ipptool -tv $IPP get-printer-attributes.test > /tmp/back.txt 2>&1 && cupsenable NexusIPP && date && exit 0; sleep 1; done; date; tail -5 /tmp/back.txt; exit 1"
job_ok offline-job-delivered NEXUS-OFFLINE-JOB 300

# ---------------------------------------------------------------- applications: Chrome, Firefox, LibreOffice print dialogs
echo "Acquire::http::Proxy \"$HOST/\";" > /etc/apt/apt.conf.d/99nexus-test-proxy
# the Google Chrome repository (https) is not part of this test: set aside, restored at the end
for f in /etc/apt/sources.list.d/google-chrome*; do [ -e "$f" ] && mv "$f" "/tmp/$(basename "$f").aside"; done
check apt-update apt-get update
check install-apps sh -c 'apt-get -y install libreoffice-writer libreoffice-gtk3 >/tmp/apt-apps.log 2>&1; rc=$?; tail -5 /tmp/apt-apps.log; exit $rc'
# the marker must stay on one line in the printout (pdftotext reads it back): a modest font size
page() { printf '<!DOCTYPE html><html><head><meta charset="utf-8"><title>%s</title></head><body style="font: 24px sans-serif">' "$2" > "$1"
         printf '<p style="font-size: 28px">%s</p><p>%s</p></body></html>\n' "$3" "printed from a browser print dialog on NexusOS" >> "$1"; chmod 644 "$1"; }
# Chrome: the arguments the desktop uses (/nexus/chrome-desktop.args, CRLF tolerated), a throw-away profile, the system
# default printer as print preview's destination
# test_x64_printing.py --slow-cli N: a slow command-line phase (N s without any input before the dialogs)
delay=$(wget -q -O - "$HOST/files/cli-delay" 2>/dev/null); case "$delay" in ''|*[!0-9]*) delay=0 ;; esac
if [ "$delay" -gt 0 ]; then sleep "$delay"; echo "PRN slow-cli waited $delay s"; fi
CHROME=$(head -1 /nexus/chrome-desktop.args | tr -d '\r')
CHROME_ARGS=$(sed -n '2,$p' /nexus/chrome-desktop.args | tr -d '\r' | grep -v -e '^--user-data-dir' -e '^https\?:' -e '^file:' -e '^$' | tr '\n' ' ')
mkdir -p /etc/opt/chrome/policies/managed
echo '{"PrintPreviewUseSystemDefaultPrinter": true}' > /etc/opt/chrome/policies/managed/nexus-print-test.json
page /tmp/print-chrome.html "NexusOS Chrome print test" NEXUS-CHROME-PRINT
rm -rf /tmp/prn-chrome
# shellcheck disable=SC2086
as_user "$CHROME" $CHROME_ARGS --user-data-dir=/tmp/prn-chrome --no-first-run --no-default-browser-check file:///tmp/print-chrome.html >/tmp/prn-chrome.log 2>&1 &
event chrome-launched
gate chrome-printed 900
job_ok chrome-print-dialog NEXUS-CHROME-PRINT 240 || tail -20 /tmp/prn-chrome.log
upload chrome.log /tmp/prn-chrome.log; find /tmp/prn-chrome -printf "%P\n" > /tmp/chrome-names.txt 2>/dev/null; upload chrome-names.txt /tmp/chrome-names.txt
pkill -f prn-chrome; sleep 3; pkill -9 -f prn-chrome; rm -f /etc/opt/chrome/policies/managed/nexus-print-test.json
# Firefox (the NexusOS build: Mozilla's release in /opt/firefox, as apps_install.py ships it): print preview, Enter prints
if get firefox.tar.xz /tmp/firefox.tar.xz && mkdir -p /opt && tar -xJf /tmp/firefox.tar.xz -C /opt && [ -x /opt/firefox/firefox-bin ]; then ok firefox-installed "$(/opt/firefox/firefox-bin --version 2>&1 | tail -1)"; else bad firefox-installed; fi
mkdir -p /tmp/prn-ff; get firefox-user.js /tmp/prn-ff/user.js
printf 'user_pref("print.always_print_silent", false);\nuser_pref("print.prefer_system_dialog", false);\n' >> /tmp/prn-ff/user.js
chown -R nexus:nexus /tmp/prn-ff
page /tmp/print-ff.html "NexusOS Firefox print test" NEXUS-FIREFOX-PRINT
as_user env MOZ_ENABLE_WAYLAND=1 MOZ_CRASHREPORTER_DISABLE=1 /opt/firefox/firefox-bin --profile /tmp/prn-ff --no-remote file:///tmp/print-ff.html >/tmp/prn-ff.log 2>&1 &
event firefox-launched
gate firefox-printed 900
job_ok firefox-print-dialog NEXUS-FIREFOX-PRINT 240 || tail -20 /tmp/prn-ff.log
upload firefox.log /tmp/prn-ff.log
pkill -f prn-ff; sleep 3; pkill -9 -f prn-ff
# LibreOffice Writer (Debian, GTK3 on Wayland): Ctrl+P opens its print dialog, Enter = Print
mkdir -p /tmp/prn-lo/user; get registrymodifications.xcu /tmp/prn-lo/user/registrymodifications.xcu; chown -R nexus:nexus /tmp/prn-lo
printf 'NEXUS-LIBREOFFICE-PRINT\nprinted from the LibreOffice Writer print dialog on NexusOS\n' > /tmp/print-lo.txt; chown nexus:nexus /tmp/print-lo.txt
as_user env SAL_USE_VCLPLUGIN=gtk3 GDK_BACKEND=wayland /usr/lib/libreoffice/program/soffice --writer --norestore --nologo -env:UserInstallation=file:///tmp/prn-lo /tmp/print-lo.txt >/tmp/prn-lo.log 2>&1 &
event writer-launched
gate writer-printed 900
job_ok writer-print-dialog NEXUS-LIBREOFFICE-PRINT 240 || tail -20 /tmp/prn-lo.log
upload writer.log /tmp/prn-lo.log
pkill -f prn-lo; sleep 3; pkill -9 -f prn-lo
# LibreOffice without a window: soffice --pt (the same printing code) to Print to PDF (cups-pdf names the file after the job)
printf 'NEXUS-LIBREOFFICE-PDF\n' > /tmp/printlotwo.txt; chown nexus:nexus /tmp/printlotwo.txt; mark
check writer-pt-pdf as_user env SAL_USE_VCLPLUGIN=svp /usr/lib/libreoffice/program/soffice --headless --norestore -env:UserInstallation=file:///tmp/prn-lo2 --pt PDF /tmp/printlotwo.txt
job_ok writer-print-to-pdf NEXUS-LIBREOFFICE-PDF 180 pdf printlotwo

# ---------------------------------------------------------------- the system stays consistent
check cups-still-active nexusctl is-active cups
check no-stuck-jobs sh -c 'cancel -a -x 2>/dev/null; sleep 2; [ -z "$(lpstat -o)" ]'
rm -f /etc/apt/apt.conf.d/99nexus-test-proxy
for f in /tmp/google-chrome*.aside; do [ -e "$f" ] && mv "$f" "/etc/apt/sources.list.d/$(basename "$f" .aside)"; done
echo "PRN DONE"
