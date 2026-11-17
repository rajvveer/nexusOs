#!/usr/bin/env python3
"""Install Debian's Wi-Fi userland onto a NexusOS disk image (FAT root, e.g. a copy of out/apps64.img).

    python -I arch/x86_64/wifi_install.py <disk.img> [--grow MIB] [--cache DIR] [--report PATH]
    python -I arch/x86_64/wifi_install.py lock [--allow-older]   # re-resolve wifi-lock.json from signed indices

Packages (Debian 13 trixie + updates/security): wpasupplicant (wpa_supplicant, wpa_cli, wpa_passphrase), hostapd
(hostapd, hostapd_cli) and iw, with their dependency closure; the disk's glibc runtime stands for libc6. They talk to the
kernel's nl80211 (arch/x86_64/nl80211.c) and to the virtual radios of arch/x86_64/wifi.c (tests/test_x64_wifi.py).

This is arch/x86_64/nettools_install.py's verified pipeline unchanged (signed InRelease -> Packages.xz -> closure ->
every .deb pinned by URL + size + SHA-256 + snapshot.debian.org fallback in wifi-lock.json; installing only uses the
lock), run with its own lock, root set and manifest (/nexus/wifi-installed.json). No maintainer script runs.
Licenses: wpa_supplicant/hostapd BSD-3-Clause, iw ISC, libnl LGPL-2.1 (user space only; nothing enters the kernel).
"""
from pathlib import Path
import importlib.util
import os
import sys

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location('wifi_nettools_install', HERE / 'nettools_install.py')
nt = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(nt)

nt.NAME = 'wifi'
nt.LOCK = HERE / 'wifi-lock.json'
nt.DEFAULT_CACHE = Path(os.environ.get('NEXUS_WIFI_PKGS', r'D:\nexustest\pkgs\wifi'))
nt.ROOTS = ['wpasupplicant', 'hostapd', 'iw']
nt.GROUPS = {'wifi': {'trixie': nt.ROOTS}}
nt.SKIP = dict(nt.SKIP, **{
    'lsb-base': 'init-script helpers (no SysV init scripts run on NexusOS)',
    'sysvinit-utils': 'init-script helpers (no SysV init scripts run on NexusOS)',
})
nt.ENTRY_POINTS = ['/usr/sbin/wpa_supplicant', '/usr/sbin/wpa_cli', '/usr/bin/wpa_passphrase', '/usr/sbin/hostapd',
                   '/usr/sbin/hostapd_cli', '/usr/sbin/iw']
InstallError = nt.InstallError

if __name__ == '__main__':
    try: sys.exit(nt.main())
    except InstallError as e: raise SystemExit('wifi_install: ' + str(e))
