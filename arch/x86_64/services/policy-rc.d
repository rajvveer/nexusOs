#!/bin/sh
# /usr/sbin/policy-rc.d - NexusOS: Debian maintainer scripts never start SysV init scripts directly.
# invoke-rc.d asks this script before running /etc/init.d/<name> <action>. NexusOS supervises daemons with nexusd
# (units in /usr/lib/nexus/services and /etc/nexus/services, else the systemd unit the Debian package ships): when a
# unit <name>.service exists, the request is handed to
# nexusd (start/restart/reload/stop of the supervised daemon) and the init script itself is refused (exit 101), so no
# daemon ever runs outside supervision. Without a unit, or while nexusd is not running (first boot, chroots), the
# action is refused too. See invoke-rc.d(8) "POLICY-RC.D INTERFACE".
while [ $# -gt 0 ]; do
    case "$1" in
        --quiet) shift ;;
        --list) exit 101 ;;
        --*) shift ;;
        *) break ;;
    esac
done
name="$1"; actions="$2"
[ -n "$name" ] || exit 101
unit="$name.service"
[ -S /run/nexus/control ] || exit 101
found=; for d in /etc/nexus/services /usr/lib/nexus/services /etc/systemd/system /usr/lib/systemd/system; do [ -e "$d/$unit" ] && found=1; done
[ -n "$found" ] || exit 101
for action in $actions; do
    # invoke-rc.d puts an action in parentheses when the script is not configured for the current runlevel; without
    # SysV runlevels (nexusd decides) that is always the case, so the parentheses carry no meaning here
    action=${action#\(}; action=${action%\)}
    case "$action" in
        start|restart|force-reload|try-restart|reload)
            verb=$action
            [ "$action" = force-reload ] && verb=restart
            nexusctl daemon-reload >/dev/null 2>&1   # a unit file the package just installed or changed
            if [ "$action" = start ] && ! nexusctl is-enabled --quiet "$unit"; then exit 101; fi
            nexusctl "$verb" --no-block "$unit" >/dev/null 2>&1 || true
            exit 101 ;;
        stop)
            nexusctl stop "$unit" >/dev/null 2>&1 || true
            exit 101 ;;
        status) exit 101 ;;
    esac
done
exit 101
