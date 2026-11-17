<p align="center">
  <img src="https://img.shields.io/badge/Architecture-x86_64_%2B_i386-blue?style=for-the-badge" alt="x86-64 and i386"/>
  <img src="https://img.shields.io/badge/Language-C_&_Assembly-orange?style=for-the-badge" alt="C & ASM"/>
  <img src="https://img.shields.io/badge/Display-1920×1080_32bpp-green?style=for-the-badge" alt="VESA"/>
  <img src="https://img.shields.io/badge/Kernel-~760KB-red?style=for-the-badge" alt="Kernel"/>
  <img src="https://img.shields.io/badge/Release-5.1_Aurora-purple?style=for-the-badge" alt="Release"/>
  <img src="https://img.shields.io/badge/Files-130+_source-yellow?style=for-the-badge" alt="Files"/>
  <a href="../LICENSE"><img src="https://img.shields.io/badge/License-All_Rights_Reserved-critical?style=for-the-badge" alt="License: All Rights Reserved"/></a>
</p>

<h1 align="center">NexusOS</h1>

<p align="center"><b>A hybrid operating system built entirely from scratch — taking the best of Windows, macOS, and Linux, with none of the bloat.</b></p>

<p align="center"><i>No GRUB. No Linux kernel. No third-party bootloader. Just raw metal.</i></p>

> **© 2026 rajvveer. All rights reserved.** The source is published for viewing only and is **not open source**.
> Copying, modifying, redistributing, rebranding or launching NexusOS (or any part of it) as your own
> product is not allowed without written permission. See [LICENSE](../LICENSE).

---

**Startup and rendering fixes published for `build.bat run`:** DMA disk reads,
bounded filesystem caching and selective redraws reduce repeated work. On the
tested 1080p Windows host, the final permanent image opened Chrome's first window
in **9.20 seconds** and its complete HTTPS page in **13.62 seconds**, down from
53.63/61.04 seconds. Window dragging measured 16.82 ms median; startup cursor
samples peaked at 130.89 ms. Loaded-page cursor latency was 33.05 ms median,
with an observed **311.19 ms spike**, so occasional stutter remains. These include
capture/polling overhead and are not FPS. Desktop/Chrome automatically use WHPX with a validated
CPU configuration when Windows reports the required capabilities; otherwise they
use TCG. Force TCG with `python -I arch/x86_64/build.py desktop --tcg`.
The two-boot app/Chrome/Hello campaign and data-preserving publication passed;
the temporary 3 GiB test image was removed. The actual published launcher also
passed Chrome HTTPS, physical URL/text input, button activation, wheel scrolling,
return to Aurora and clean shutdown with the snapshot base unchanged. The final
artifact/preservation audit passed; see [the current handoff](HANDOFF_CODEX.md)
for its evidence path and measured scope.

**Current workspace default:** `build.bat run` launches the published native x86-64
Aurora desktop with the original wallpaper, dock, menu bar and fonts. Chrome opens
from the dock and closes back to the desktop. Native launchers include Files,
Notes, Terminal, Calculator, Calendar, Paint, Minesweeper, Help and About. The
original Calculator/Calendar/Paint/Minesweeper source runs in isolated processes.
Terminal runs native commands without a TTY; existing i386 binaries use the legacy
environment.

**Native networking (2026-10-19):** the native x86-64 kernel now provides Linux-level
networking. That covers IPv6 (SLAAC and temporary addresses, NDP, MLDv2, fragments, extension
headers, stateless DHCPv6) and a DHCPv4 client that renews, rebinds and handles lease expiry.
Programs get AF_INET6, ping, raw and AF_PACKET sockets (classic BPF and mmap rings), and
rtnetlink plus `/proc/net` and `/proc/sys/net`. The kernel also has TUN/TAP, WireGuard and an
nf_tables firewall with connection tracking that Debian's `nft` drives. The Aurora **Network**
app (Spotlight "Network", or the network item in the menu bar) shows every interface. It sets
DHCP or a static IPv4 address, the DNS servers and the firewall. The settings are saved in
`/etc/nexus/network.conf` and applied at every boot.
`python -I arch/x86_64/nettools_install.py <disk.img>` installs Debian's iproute2, ping,
tcpdump, nftables, wireguard-tools and net-tools; every package is pinned. All of this was
tested in QEMU only; see the handoff.

**Networking, wave 2 (2026-10-30):** Chrome, Firefox and Windows programs (Wine) work over
IPv6, also on IPv6-only networks, with fallback when IPv6 is broken; DNS servers come from router
advertisements and DHCPv6. The Network app also sets a static IPv6 address and gateway and can
turn automatic IPv6 off. Multicast works for programs (IGMPv3/MLDv2 source filters), and
avahi-daemon now runs by default on ext4 installs: `.local` names resolve and network printers
are found (DNS-SD). nftables gains NAT (masquerade, SNAT, DNAT/port forwarding), sets, maps and
rate limits, and the system can route between interfaces (`ip_forward`). **Wi-Fi:** the kernel
has an nl80211 Wi-Fi stack. Debian's wpa_supplicant and hostapd run on it with WPA2-PSK and
WPA3-SAE, scanning, signal reports and roaming. Testing uses virtual radios
(`/proc/sys/net/nexus/wifi_sim`), so no hardware is needed; `arch/x86_64/wifi_install.py`
installs the packages. The Network app has a **Wi-Fi** tab (networks, password, Connect,
Forget, saved networks that reconnect at boot). A driver for real Wi-Fi cards (the MT7922)
is not written yet; `arch/x86_64/wifi.h` describes what it must provide. The stack also enumerates through
`/sys/class/ieee80211`, has monitor mode (radiotap capture with tcpdump), the GCMP and CCMP-256 ciphers
(WPA3/Wi-Fi 6), and can run a hotspot and a client at once on one adapter. Tested in QEMU only.

`build.bat legacy` preserves the original i386 desktop, its 30+ application
catalogue, login/accounts, Lua/Kilo and original `disk.img`. The historical feature
catalogue, architecture tables and screenshots below describe that legacy target
unless explicitly labelled native x86-64. Both kernels and the bootloader are
NexusOS; Chrome and upstream application libraries use its native compatibility ABI.

The legacy i386 target is a bare-metal x86 operating system written in C and x86 Assembly. It boots from a custom 2-stage bootloader, drives a native 1920×1080×32bpp framebuffer, sets up paging, and launches **Aurora** — a macOS-style desktop with a translucent menu bar, a magnifying dock, Spotlight, Launchpad and 30+ built-in applications. It provides a complete TCP/IP networking stack with a web browser, implements POSIX/X11 compatibility layers, supports ELF dynamic linking with shared libraries, includes a PE32 loader for Windows executables, ships a package manager with its own `.npk` archive format, CRC32 integrity checking, and recursive dependency resolution, embeds a scripting engine (NexusScript) for OS automation, provides a macOS compatibility shim with a Mach-O loader and a Cocoa/Core Foundation API subset, drives real sound hardware through an Intel AC'97 audio driver with a software mixer and WAV player, decodes BMP/PNG/JPEG/GIF images (with a from-scratch DEFLATE inflater, baseline-JPEG IDCT, and LZW) shown in a fullscreen viewer, and plays Motion-JPEG `.avi` video with synchronized PCM audio through its own RIFF/AVI parser and media player — with a roughly 1 MiB kernel image. It needs at least 40 MiB of RAM; 512 MiB or more is recommended for applications. The first 40 MiB are mapped for the kernel, graphics and browser arenas.

---

## 🆕 What's new in 5.1 "Aurora"

- **A real Linux distribution underneath (native x86-64, production round).** New installs use an **ext4 root** (`arch/x86_64/sysinstall.py install`; `sysinstall.py migrate` moves a FAT32 desktop disk over, every file kept and compared). The first boot installs the **Debian 13 base with dpkg**; daemons run under the **nexusd** service manager (`nexusctl start|stop|status|enable|logs`, a persistent journal, `dmesg`), with D-Bus, NTP (chrony, `timedatectl`), locales (incl. Hindi) and `sudo` through PAM. **apt** installs, upgrades and removes Debian packages from the signed archive, also from the new **Software** app (search, install/remove, Update all). **Printing** works through CUPS: "Print to PDF" and driverless IPP Everywhere printers, from Chrome, Firefox and LibreOffice. **Core updates** (kernel + Aurora + boot loader) come as signed bundles (`nexus-update apply`) into A/B slots; the UEFI loader rolls back by itself when an update does not boot. Wine now carries **Wine Mono and Wine Gecko**: .NET Framework programs and MSHTML pages run without a download prompt. Details and limits: [HANDOFF_CODEX.md](HANDOFF_CODEX.md).
- **Linux ABI (wave 2).** POSIX timers on every clock, timerfd, signalfd, pidfd, clone3, waitid(P_PIDFD) and epoll_pwait2 behave as on Linux, and 32-bit system call arguments ignore their register's upper half: Debian's **firefox-esr** installed with apt now runs. **System V IPC** (semaphores, message queues, shared memory, /proc/sysvipc, ipcs/ipcrm, per IPC namespace) serves libdevmapper's udev sync and PostgreSQL; **Landlock** (ABI v4) sandboxes files and TCP ports; util-linux mount uses the **new mount API** (fsopen, open_tree, move_mount, mount_setattr); _ALARM timers wake the machine from sleep. Kernel trace records stay out of dmesg and the journal unless the `kmsg.trace` boot option is given. Details: [HANDOFF_CODEX.md](HANDOFF_CODEX.md).
- **Upgrade a FAT32 disk to the full system, more service types, signed updates (wave 2).** **System Settings > System Upgrade** turns the FAT32 desktop disk into the full system: choose a password, shut down, and the launcher converts the disk while NexusOS is off (every file copied and compared - documents, browser profiles, Windows programs and settings), then starts the full system; afterwards **Keep the Full System** or **Go Back to the Old Disk** (the old disk stays on the PC until then; `arch/x86_64/migrate_disk.py` does the same by hand). nexusd now runs **socket, timer and path units** (socket activation with `LISTEN_FDS`, calendar timers that catch up after the PC was off, file watches), a **per-user service manager** for every session (`nexusctl --user`) and limits journal floods; it also takes FIFO, netlink and dual-stack IPv6 sockets, honours the links Debian packages leave (their **timers now run**: apt-daily, logrotate, dpkg-db-backup, fstrim; user units such as gnupg's agent sockets), and the kernel has **named pipes** (`mkfifo`). Core updates are signed with the **NexusOS release key** (`update_bundle.py release`); with Secure Boot on, every A/B slot kernel must carry a valid signature before it runs, and FAT32 roots refuse A/B slots. 32-bit Windows programs that probe OpenGL (Notepad++ x86) no longer hang under Wine (fixed Wayland driver, `winewayland_build.py`), and **`.hta` applications run** (NexusOS's own `mshta`, 64- and 32-bit; Wine's is a stub). Tests keep their data where `arch/x86_64/nexus_paths.py` says (environment or `~/.nexusos/paths.json`), never on the installed disk.
- **Disk streaming downloads.** Browser and `wget` now write bounded chunks to disk; the old 256 KiB download cap is gone. Ctrl+S/the Save arrow saves a whole URL even when the page/image preview is limited. Long filenames, unique default names, progress, cancellation and interrupted-transfer cleanup are supported. Downloads appear in the File Manager at `/disk/Downloads`.
- **Expandable storage.** The default disk is 1 GiB. `build.bat storage 8192` grows it to 8 GiB with existing files preserved. FAT lives in a partition after the fixed NPFS/persistence region; host migration uses a temporary image and atomic replacement. A 167,683,000-byte official Chrome installer was downloaded by NexusOS over trusted HTTPS and its disk SHA-256 matched the Google-signed reference.

- **Phase 57 integrated: internet + real userland.** TLS 1.3, trusted certificate checks, reliable TCP, `curl`, `wget`, and a graphical HTML/image browser with history, scrolling and downloads. HTTPS rejects unknown certificates by default; `curl -k` and the browser warning page require an explicit override. `build.bat run` uses `-cpu max` so secure random generation is available.
- **Real programs on disk.** `build.bat apps` builds Picolibc 1.8.12, Lua 5.4.7, Kilo, a small `sh`, file utilities, 2048 and Tetris as static i386 user processes. Programs have isolated address spaces, file descriptors, pipes, spawn/wait, ANSI terminals and saved FPU state. Lua/Kilo/games also launch from Spotlight and App Store.
- **Reproducible checks.** Network campaign: 33/33 checks; existing server regression: 58/58; userland tests verify actual files written by Lua, Kilo and shell pipelines. See [the current handoff](HANDOFF_CODEX.md) and [userland build/test instructions](userland/README.md).

- **macOS-style desktop.** Translucent top menu bar (NexusOS menu, app menus, Wi-Fi, Spotlight, Control Center, clock), a frosted dock that magnifies under the pointer with name bubbles and running-app dots, Spotlight (`Ctrl+Space`), Launchpad (`F4`), a Cmd+Tab-style app switcher, Notification Center and banner notifications.
- **New look everywhere.** Big Sur-style procedural app icons, a Sonoma-like dusk wallpaper, rounded windows with soft shadows and hover-glyph traffic lights, anti-aliased UI text (Aurora fonts) and a smooth monospace font for every app, and a macOS dark-mode palette.
- **Mac-style login and lock screen** with a big clock, user pictures, a pill password field that shakes on a wrong password, and Restart / Shut Down buttons. A boot splash with a real progress bar replaces the scrolling boot log.
- **Hardened kernel, persistent files, real servers (Phase 55).** The kernel now protects itself: its code and read-only data are mapped read-only (W^X), the NULL page is unmapped, a guard band sits under the kernel stack, and a **crash-containment layer (kguard)** turns a fault inside any built-in app, shell command or desktop frame into a closed window plus a notification instead of a dead OS — even a kernel-stack overflow is caught by a double-fault task gate (`crashtest`). The RAM filesystem is **persistent**: every change is snapshotted to a crash-safe A/B slot on the disk (autosave 2 s after a change and on reboot/shutdown) and restored at boot, user accounts included (`persist`). `vnc` now requires real **RFB VNC Authentication** (DES challenge/response, `vncpasswd`) and `httpd` is a real HTTP file server (`/www` document root, directory listings, `/api/status`, `/fs/<path>` with HTTP Basic auth against the OS accounts, uploads with PUT). A **multi-controller USB stack** replaces the UHCI-only driver: xHCI (USB 3, every modern PC), EHCI (USB 2, with companion hand-off), OHCI and UHCI host controllers behind one HCD abstraction, **hub walking** (devices behind hubs, nested hubs, hot-plug), and a tree view in `usbinfo`.
- **Isolation, USB, folders, logins (Phase 54).** `runelf` runs ELF/PE/Mach-O programs in their **own ring-3 address space** with int-0x80 syscalls; a faulting program is killed on its own (Segmentation fault) while the OS keeps running, and the scheduler time-slices background user processes. A real **UHCI USB stack** drives USB keyboard, mouse and flash drives (`usbinfo`). The RAM filesystem has **real directories** (`cd`/`mkdir`/`rmdir`/`pwd`/paths). The **rshell and sync** services now require a login against the OS user accounts.
- **Fixed:** the intermittent boot double fault (~14% of boots), the page allocator handing out heap/stack/.bss pages, a whole-OS freeze when ARP ran inside the timer interrupt, length-overflow bugs in the PNG/AVI/WAV/BMP/JPEG/GIF/ELF/PE/Mach-O parsers, IP/TCP length validation, a uid collision between new users and `guest`, stack overflows in the GUI terminal, editor and registry, `cmd > file` redirection, F1 colliding with `;`, the 1024×768 grid left over after the 1080p switch, and more.

### Native x86-64 Aurora desktop (development)

From `NexusOS/` in this prepared workspace:

```powershell
.\build.bat run          # Aurora desktop; launch Chrome from its dock
.\build.bat x64 chrome   # Direct Chrome diagnostic with preserved arguments
.\build.bat legacy       # Original i386 desktop and original data disk
```

Normal `run` launches the verified `arch/x86_64/out/desktop64.img` with 2 GiB guest
RAM and a 3 GiB persistent disk. It does not rebuild or replace that image. Use
Ctrl+Space for Spotlight, F4/Win for Launchpad, Alt+Tab to switch windows and Alt+F4
to close one. The Nexus menu shuts down the session. Notes uses Ctrl+S to save;
unsaved edits require saving or explicit discard before closing.

Aurora drives several monitors (virtio-gpu heads: hot-plug, arrangement, main
display, mirroring, resolution) and scales each one by 100-400 % (125/150/175 %
included) in System Settings > Displays; the choice is kept across reboots. Window
shortcuts: Super+Left/Right tile, Super+Up/Down maximize/restore, Super+W overview
of 4 workspaces, Super+1..4 switch, Super+D show desktop, drag to an edge to snap.
Super+L locks the screen; with a password set (System Settings > Users) a greeter
comes first at boot unless automatic login is on. Keyboard layouts (US, UK, German,
French, Spanish, Hindi InScript, ...) are chosen in Settings > Keyboard and switched
with Super+Space; Settings > Accessibility has inverted colours, more contrast, a
magnifier (Super+= / Super+-), larger text, reduced motion and sticky keys. On an
ext4 install every account can sign in at the greeter with its own home, runtime
folder and D-Bus session (menu N > Switch User... / Log Out...; Tab on the lock screen
switches user); other users' windows and files stay private, and logind
(org.freedesktop.login1) tells applications about sessions, sleep and inhibitors.
With `apt install orca` the screen reader starts with Super+Alt+S (or Settings >
Accessibility) and also reads Aurora's own menu bar, Dock, Spotlight and greeter;
Ctrl+F2 opens the menu bar and Ctrl+F3 moves into the Dock from the keyboard. With
`apt install xdg-desktop-portal` sandboxed and portal-aware applications get Aurora's
file chooser, screenshots (with a permission prompt), dark mode and accent colour
(Settings > Appearance). Files can be dragged between Wayland applications; the bell
in the menu bar opens the Notification Centre with Do Not Disturb. With
`desk_install.py install <disk> --only fonts` Aurora and the applications show
CJK, Indic, Arabic, Hebrew, Thai, Greek, Cyrillic and other scripts and colour
emoji. Windows programs minimize to the dock like other windows. Applications using
Wayland text input get an input method named in `~/.aurora/input-method.conf`
(`program=/path args`): Aurora starts it and only it may grab the keyboard. X11
programs (Xwayland installed) are ordinary Aurora windows with Aurora's title bar,
dock entry, focus, minimize/zoom/close, dialogs above their windows and a shared
clipboard: start them with `nexus-x11-session --rootless xterm` (or `X11=rootless` in
a launch file; `export=1` in `~/.aurora/x11.conf` gives every program DISPLAY); the X
server starts on the first X connection.
`arch/x86_64/ime_install.py install <disk>` adds fcitx5 from Debian 13 with Pinyin, Anthy
(Japanese) and Hangul; with `program=/usr/libexec/nexus-fcitx5` they type into GTK
applications such as Firefox and into Chrome (Ctrl+Shift+F9 switches the engine).

Memory: the native kernel swaps like Linux. `build.bat run` adds a swap file once
(`/swapfile`, up to 2 GiB, about half the free space, listed in `/etc/fstab`; the kernel
enables it at boot), so memory-heavy programs page out instead of being killed
(8 Chrome tabs holding 1.6 GiB in a 2 GiB guest swap ~300 MB and keep their data;
under heavier pressure Chrome's browser process can still crash, see HANDOFF_CODEX.md).
`python -I arch\x86_64\build.py desktop --no-swap` boots without it;
`python -I arch\x86_64\swap_tools.py swapfile <disk> --remove` deletes it for good, and
`--size MIB [--grow MIB]` makes another (edited in place; stop the VM first). Debian's `swapon`, `swapoff`, `mkswap` and `free`
can be installed with `swap_tools.py install <disk>`. Without swap, or when swap is
full, the OOM killer ends the process with the worst Linux badness score
(`/proc/PID/oom_score`, `oom_score_adj`) and logs it; the system keeps running.
Hibernation is not part of this.

Actual Google Chrome 155.0.8059.39 has been guest-downloaded, natively installed,
and verified rendering HTTPS on this kernel. In Chrome Downloads (Ctrl+J), opening
a supported `.deb` launches the native installer: choose Install, then Run selected.
Files also opens `.deb` names case-insensitively. Private application bundles are
supported. On an ext4 installation apt and the Software app install Debian packages with their
dependencies and maintainer scripts.

Legacy FAT files and readable saved RAMFS data are imported under
`/home/nexus/Legacy/<source-sha256>/`. Original files, the original legacy disk and
its accounts remain preserved. Restricted/account snapshot records stay in host
preservation evidence; legacy permissions are recorded, not silently remapped.
See [HANDOFF_CODEX.md](HANDOFF_CODEX.md) for exact verification and migration scope.

The native desktop includes no automatic i386 binary compatibility. Its Terminal
captures native command output without emulating a TTY. The kernel now provides
Linux seccomp (strict + classic-BPF filters + USER_NOTIF with TSYNC/ESRCH, NEW_LISTENER +
RECV/SEND/ID_VALID/ADDFD/SET_FLAGS, WAIT_KILLABLE_RECV, FLAG_LOG/SPEC_ALLOW and
GET_ACTION_AVAIL/GET_NOTIF_SIZES), user/PID/network/IPC namespaces with
setns(2) - Chrome's chrome://sandbox is all green ("adequately sandboxed") and Firefox's
content-process sandbox runs at level 6 (seccomp-bpf + namespaces) - full ASLR (PIE/ld/mmap/stack/brk
+ AT_RANDOM, honoring personality and randomize_va_space), SMEP, SMAP on by default (every kernel access
to user memory goes through STAC/CLAC windows with fault fixups; the `nosmap` boot option disables it),
UMIP (with SGDT/SIDT/SLDT/SMSW/STR emulation), a boot-time stack canary, W^X, Yama ptrace_scope=1,
kernel-oops containment (a stray kernel access or a kernel-stack overflow in a syscall kills the task, not the
system), kernel-pointer hygiene (root-writable kptr_restrict/dmesg_restrict, /proc/kallsyms with addresses gated
by kptr_restrict + CAP_SYSLOG), kernel lockdown (integrity/confidentiality, from Secure Boot or `lockdown=`,
exposed at /sys/kernel/security/lockdown), an authorization broker (nexus-polkitd + pkexec, admin password via
/etc/shadow), and UEFI Secure Boot: the
firmware verifies the Authenticode-signed loader against the enrolled db certificate and the loader verifies
KERNEL.BIN (RSA-2048/SHA-256). KASLR (relocation table only, no
boot-time slide yet), GPU/media coverage and input-method engines (IBus, Mozc; fcitx5 with Pinyin, Anthy and
Hangul works) remain incomplete. This remains a development OS.

Power management (native x86-64): the NexusOS menu offers Sleep, Hibernate, Restart and Shut Down; the power
button opens a Shut Down / Restart / Sleep / Cancel dialog (press it again, or wait 60 s, to shut down); laptops
get a menu-bar battery item with time remaining and a brightness slider. Hibernate needs a hibernation area: an
MBR partition of type 0xA0 or a preallocated `/nexus/hiber.img`. Linux tools see `/sys/power`, `/sys/class/power_supply`,
`/sys/class/thermal`, `/sys/class/backlight`, `/sys/devices/system/cpu/cpufreq` and `/dev/rtc0` (rtcwake works).
Idle CPUs stop their timer tick (tickless idle; `nohz=off` restores the periodic tick). The battery panel offers
Low Power / Balanced / Performance power modes, and Low Power turns on by itself when the battery runs low (until
power is connected). Installed systems run a UPower + power-profiles service on D-Bus (desktop apps and Chrome's
battery API see the battery). Hibernation can also use a swap partition or swap file (`resume=`, `resume_offset=`)
and is refused in lockdown mode. Closing the lid while a second display is connected does not sleep (the built-in
panel turns off until the lid opens)
(`/sys/power/policy/lid_switch_docked`; `lid_switch_external_power` sets the action on AC).
Tested on QEMU only; real laptops (battery, embedded controller, CPU power states) are unverified.

#### Real PCs (preview, not yet run on real hardware)

The native kernel boots from the UEFI image `arch/x86_64/out/nexus64-uefi.img` (write it to a USB stick in DD mode)
or BIOS. Without a root disk (SATA, NVMe, or a USB stick made with `build.py usb-image`) it runs its
self-tests and diagnostics only; the desktop needs the root disk. Hardware support aimed at current desktops:

- **Ethernet:** Intel e1000/e1000e/igb, I217/I218/I219, I225/I226; Realtek RTL8111/8168, RTL8125 (2.5 GbE), RTL8126,
  RTL8139. Realtek PHY firmware is optional; put it on the root disk with `python -I arch/x86_64/firmware_install.py <disk>`.
  No Wi-Fi yet (MediaTek MT7922 and others).
- **Storage:** AHCI SATA and NVMe (AHCI must be selected in the firmware setup; RAID mode is reported).
- **Sound:** HD Audio analog outputs (e.g. Realtek ALC897 line-out/headphone with jack detection) and HDMI/DisplayPort
  audio on AMD and NVIDIA graphics; `hda.output=hdmi` or `hda.output=analog` overrides the choice. Microphone and line-in
  recording (front/rear mic jacks, built-in codec mics; Chrome getUserMedia, `arecord`). **USB audio** (USB Audio
  Class 1 and 2 headsets, DACs and microphones) appears as a second ALSA card next to HD Audio; Aurora's
  Settings > Sound picks the default output (newest device, or one you choose) and follows hot-plug.
- **Webcams:** USB Video Class cameras appear as `/dev/video0` (V4L2, YUYV/NV12 streaming) for Chrome and Firefox
  (getUserMedia) and other V4L2 programs. The kernel option `vcam` adds a virtual test camera.
- **Game controllers:** Xbox 360 / One / Series pads (USB), DualShock 4 and DualSense (USB) and generic HID gamepads
  and joysticks appear as `/dev/input/eventN` + `/dev/input/jsN` with rumble (found through libudev); Chrome's
  Gamepad API sees them with the standard mapping.
  `/dev/uinput` lets programs create virtual input devices. The kernel option `vpad` adds a virtual Xbox 360 pad.
- **CPUs:** many-core PCs whose firmware enables x2APIC mode keep it (the kernel option `x2apic` turns it on,
  `nox2apic` off).
- **Bluetooth:** Debian's BlueZ 5.82 (`bluetoothd`, `bluetoothctl`, `btmon`; `arch/x86_64/bluez_install.py`) runs on
  the kernel's own stack: discovery, pairing (BR/EDR Secure Simple Pairing, LE Secure Connections), L2CAP and RFCOMM
  sockets, GATT for bluetoothd, USB adapters through the standard HCI-over-USB interface. Not yet: Bluetooth audio
  (no SCO/ISO), Bluetooth keyboards/mice (no HID host), the MediaTek MT7922's firmware download.
- **Display:** the firmware framebuffer at the monitor's native resolution (`video=1920x1080` on the loader command line
  in `EFI\NEXUS\CMDLINE.TXT` picks a mode). There is no GPU driver for AMD or NVIDIA cards: no 3D acceleration, one
  monitor, CPU-drawn desktop.
- **Diagnostics:** add `diag` to `CMDLINE.TXT` or hold **D** while the PC starts: `NEXUSDIAG.TXT` (hardware, firmware
  tables, drivers, kernel log) is written to the boot medium and a summary is shown. Please attach it to bug reports.

### Native x86-64 storage: USB drives, USB boot, file systems, encryption

- **USB drives** (Bulk-Only and UAS, USB 2 and 3) appear as `/dev/sdX`; Aurora mounts them at `/media/nexus/<label>`
  (FAT, exFAT, ISO 9660; NTFS and ext4 through ntfs-3g/fuse2fs when installed) and Files shows them in its Drives
  sidebar with Unmount and Eject. Pulling a drive without unmounting is survived (the desktop warns).
- **Boot from USB:** `python -I arch\x86_64\build.py usb-image nexus-usb.img` makes one stick image for BIOS and UEFI
  (GPT plus a hybrid MBR that lists the ESP and the active root, so older BIOSes boot it too; `--mbr protective` writes
  a plain protective MBR); write it with Rufus (DD mode) or `dd`. On the running stick, `nexus-os-install nvme0n1`
  installs the system onto the internal disk at the next boot (after typing YES on the console);
  `nexus-os-install --encrypt nvme0n1` puts the root on LUKS2 (Argon2id) and asks for the passphrase twice.
- **Encrypted root:** a LUKS1/LUKS2 root (`cryptroot=PARTUUID=...` or `rd.luks.uuid=...` with
  `root=/dev/mapper/cryptroot`, or found by its GPT type) is unlocked before Aurora starts: a passphrase dialog on the
  framebuffer (console prompt otherwise), PS/2 or USB keyboard in the system's layout (any layout Aurora offers;
  `vconsole.keymap=de` on the command line, kept up to date at shutdown when the layout is changed in Aurora), 3 tries
  (`cryptroot.tries=`), then the PC powers off. Hibernation works with an encrypted root: the image goes into the
  encrypted root (`/nexus/hiber.img`) and is restored after the passphrase at the next start; while other encrypted
  volumes are open, hibernating to an unencrypted place is refused.
- **/etc/crypttab:** more LUKS volumes are opened at boot before /etc/fstab mounts them: `name UUID=... none|/etc/keys/k luks,discard,nofail`
  (keyfiles must be root-owned and not readable by others; also noauto, readonly, tries=).
- **FAT32 check and repair:** the kernel marks a FAT32 root dirty while it writes and clean at shutdown; after a crash
  or power loss it checks and repairs the volume at boot (lost clusters, cross-links, bad sizes, broken chains;
  `fsck.mode=force|skip`, `fsck.repair=no`), and mounts it read-only if repair is impossible.
  On the host: `python -I userland\fatfsck.py -n|-a disk.img` (like `fsck.fat -n|-a`).
- **Swap partitions:** `mkswap` partitions in `/etc/fstab` (`UUID=`/`LABEL=`/`/dev/...`, `pri=`, `nofail`) are enabled at
  boot together with the swap file; swapon/swapoff with priorities and `/proc/swaps` behave as on Linux.
- **File systems and tools:** mount/umount, `/etc/fstab`, loop devices (`losetup`, `mount -o loop`), exFAT read/write,
  ISO 9660 (Rock Ridge, Joliet), FUSE (sshfs, ntfs-3g, fuse2fs, user mounts through fusermount3), and Debian's
  sfdisk, parted, mkfs/fsck for FAT, exFAT and ext4, blkid and lsblk.
- **Encryption:** device mapper with dm-crypt (AES-XTS, AES-NI or constant-time software AES) and kernel keyrings, so
  Debian's `cryptsetup` (LUKS2 with Argon2id, LUKS1, plain) and `dmsetup` work: `cryptsetup luksFormat /dev/sdb1`,
  `cryptsetup open /dev/sdb1 secret`, `mount /dev/mapper/secret /mnt`. AES-NI runs XTS and CBC eight blocks at a
  time (several GB/s through dm-crypt under QEMU/WHPX; plain64, plain and ESSIV IVs, 512-byte and 4 KiB sectors);
  `dmsetup suspend/resume` and `cryptsetup luksSuspend/luksResume` (the key is wiped while suspended) work.
- **LVM and software RAID:** Debian's `lvm2` (linear, striped, snapshot and thin logical volumes; `pvcreate`, `vgcreate`,
  `lvcreate`, `lvextend`, `lvremove`, `vgchange -ay`, `lvs`) and `mdadm` (RAID0, RAID1 with resync, `--fail`,
  `--remove`, `--add` with rebuild, `--stop`, `--assemble`, `/proc/mdstat`) work. The root can be on an LV
  (`root=/dev/vg/root`, also inside an encrypted volume, the usual Debian layout) or on a RAID1 array
  (`root=/dev/md0`; it boots degraded when a disk is missing and resynchronises after a power cut).
- **dm-verity:** `veritysetup format/open/verify` work; a block that does not match the hash tree reads as an I/O
  error (or resets the machine with `--restart-on-corruption`). A verified read-only root comes from
  `roothash=... systemd.verity_root_data=... systemd.verity_root_hash=...` on the command line (the command line itself
  is not signed yet, see HANDOFF_CODEX.md).
- **dm-integrity:** `integritysetup format/open` keep a checksum of every block (crc32c or sha256); a block changed
  behind its back reads as an I/O error, and the journal keeps blocks whole across a power cut.
- **More RAID and thin provisioning:** `mdadm` RAID5, RAID6 and RAID10 with degraded operation and rebuilds, and
  internal write-intent bitmaps (a re-added disk resyncs only what changed; after a power cut only dirty regions
  resync). LVM thin pools: `lvcreate -T`, over-provisioned thin volumes and snapshots; `thin_check` accepts the
  metadata; a full pool either fails writes or holds them until `lvextend` gives it room. Mirrored logical volumes
  (`lvcreate -m 1`) resync, can be checked and repaired, and are rebuilt with `lvconvert --repair` after a disk is
  lost. More than 16 disks/volumes at once.

### Known limitations

- **Application compatibility:** the legacy i386 desktop uses the NexusOS userland ABI. The native x86-64 desktop runs unmodified Linux programs (Chrome, VS Code, Firefox, LibreOffice) and Windows `.exe`/`.msi` programs, 64-bit and 32-bit, through Wine 10.0 (tested: 7-Zip, Notepad++, PuTTY, SumatraPDF, Inno Setup); .NET Framework programs run on Wine Mono and MSHTML/web views on Wine Gecko (tested: NuGet, KeePass, HTML Help, Internet Explorer pages); macOS `.app` packages are not supported, and Linux/Windows compatibility in general is not established. This remains a development OS.
- **Legacy i386 browser:** basic HTML and images, no JavaScript engine or modern CSS support. In-memory page/image previews remain bounded; Ctrl+S saves the complete URL to disk. Downloads have no RAM-buffer size cap. Explicit destinations are never silently overwritten; automatic filenames are made unique.
- **Legacy i386 HTTPS:** TLS 1.3 AES-128-GCM/X25519 only, no TLS 1.2, HelloRetryRequest, OCSP/CRL or independent crypto audit. RDRAND is required; unsupported CPUs fail closed. Use the supplied QEMU command with `-cpu max`.
- **Legacy i386 userland:** static i386 programs, a small shell (quotes, pipes, redirection, `cd`, `exit`), no fork/exec replacement, job control or full POSIX compatibility. The FAT volume is expandable; RAM snapshots can omit files when their 1.5 MiB slot fills, with an explicit status warning.

- **Legacy i386 process model:** user processes are isolated; built-in apps are contained, not isolated. `runelf`'d programs get their own ring-3 page directory and a page fault in one kills only that process. Built-in apps and the desktop still share the kernel's ring-0 address space; since Phase 55 the kernel image is write-protected and a fault in an app/command/frame is caught by kguard (window closed, OS keeps running), but a fault inside an interrupt handler or during boot is still fatal.
- **Legacy i386 preemption is user-only.** The scheduler time-slices ring-3 user processes; the kernel/desktop thread is never pre-empted (it yields cooperatively), so lock-free kernel state stays safe.
- **Legacy i386 USB** is fully polled (no USB interrupts): xHCI/EHCI/OHCI/UHCI controllers, hubs, HID boot-protocol keyboard/mouse and Bulk-Only mass storage. No isochronous transfers (audio/video class devices), no USB 3 SuperSpeed hubs, no report-descriptor parsing (keyboards/mice must speak the boot protocol).
- **Legacy i386 remote services:** `rshell` (2323), `sync` (7070), `vnc` (5900, RFB DES auth via `vncpasswd`, or `vnc --insecure`) and `httpd` (8080, public `/www` + Basic-auth `/fs/`) all start only when you run them. The transport is plain TCP (no TLS), VNC uses the classic 8-character DES scheme, and httpd is HTTP/1.0 serving one client at a time — fine for a LAN, not for the open internet.
- **Legacy i386 filesystem:** the RAM-FS has real directories and is persisted to the IDE disk (LBA 26624+, two 1.5 MiB slots, after the NPFS window); RAM files are capped at 256 KiB / 96 nodes, and a CD/ISO boot without a disk runs non-persistently. NPFS remains a separate journaled volume.

## Table of Contents

- [Screenshots](#-screenshots)
- [Feature Highlights](#-feature-highlights)
- [Architecture Overview](#-architecture-overview)
- [Subsystem Deep Dive](#-subsystem-deep-dive)
- [Built-in Applications](#-built-in-applications-30)
- [Shell Commands](#-shell-commands-93-total)
- [Desktop GUI](#-desktop-gui)
- [Project Structure](#-project-structure)
- [Build & Run](#-build--run)
- [Boot Flow](#-boot-flow)
- [Roadmap](#-roadmap)
- [Growth Projection](#-growth-projection)
- [License](#-license)

---

## 📸 Screenshots

<p align="center">
  <img src="screenshots/aurora-desktop.png" width="85%" alt="NexusOS Aurora desktop"/>
  <br>
  <sub><b>Aurora</b> (1920×1080) — menu bar, magnifying dock, rounded windows and the dusk-hills wallpaper</sub>
</p>

<table>
  <tr>
    <td width="50%" align="center"><img src="screenshots/aurora-login.png" alt="Login"/><br><sub><b>Login window</b></sub></td>
    <td width="50%" align="center"><img src="screenshots/aurora-windows.png" alt="Windows"/><br><sub><b>Terminal, Finder and About This NexusOS</b></sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><img src="screenshots/aurora-launchpad.png" alt="Launchpad"/><br><sub><b>Launchpad</b> (<code>F4</code>)</sub></td>
    <td width="50%" align="center"><img src="screenshots/aurora-spotlight.png" alt="Spotlight"/><br><sub><b>Spotlight</b> (<code>Ctrl+Space</code>)</sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><img src="screenshots/aurora-menu.png" alt="NexusOS menu"/><br><sub><b>NexusOS menu</b> and Notification Center</sub></td>
    <td width="50%" align="center"><img src="screenshots/aurora-console.png" alt="Console"/><br><sub><b>Console</b> (<code>F12</code>) — the text shell in a Terminal window</sub></td>
  </tr>
</table>

**Phase 55 — hardened kernel, persistent files, real servers, full USB stack**

<table>
  <tr>
    <td width="50%" align="center"><img src="screenshots/phase55-crash-contained.png" alt="App crash contained"/><br><sub><b>Crash containment:</b> a built-in app dereferences NULL - the window closes, a notification explains, the desktop keeps running</sub></td>
    <td width="50%" align="center"><img src="screenshots/phase55-crashtest.png" alt="crashtest"/><br><sub><b><code>crashtest</code>:</b> NULL read, kernel-stack overflow (double fault) and a write into kernel code - all recovered at the shell</sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><img src="screenshots/phase55-persist.png" alt="persist status"/><br><sub><b>Persistent RAM-FS:</b> files, folders, a new user and a deleted built-in all back after a reboot (<code>persist status</code>)</sub></td>
    <td width="50%" align="center"><img src="screenshots/phase55-vnc-auth.png" alt="VNC auth"/><br><sub><b>VNC authentication:</b> DES self-test, <code>vncpasswd</code>, RFB challenge/response with lockout</sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><img src="screenshots/phase55-usb-xhci-hub.png" alt="xHCI hub"/><br><sub><b>xHCI</b> with a hub: SuperSpeed stick, keyboard and mouse behind the hub (<code>usbinfo</code> tree)</sub></td>
    <td width="50%" align="center"><img src="screenshots/phase55-usb-ehci-companion.png" alt="EHCI companion"/><br><sub><b>EHCI + UHCI companion:</b> the high-speed stick stays on EHCI, the full-speed hub is handed to the companion</sub></td>
  </tr>
</table>

<details><summary>Earlier builds (pre-Aurora)</summary>

<p align="center">
  <img src="screenshots/desktop.png" width="85%" alt="NexusOS desktop environment"/>
  <br>
  <sub><b>The VESA desktop</b> (1024×768×32bpp) — window manager, taskbar, dock, and 30+ built-in apps</sub>
</p>

<table>
  <tr>
    <td width="50%" align="center">
      <img src="screenshots/login.png" alt="Login screen"/>
      <br><sub><b>Boot login screen</b></sub>
    </td>
    <td width="50%" align="center">
      <img src="screenshots/package-manager.png" alt="Package manager"/>
      <br><sub><b>Package manager</b> — <code>npkg list</code></sub>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center">
      <img src="screenshots/package-install.png" alt="Installing a package"/>
      <br><sub><b>Installing a package</b> — <code>.npk</code> build, CRC32 verify, extract</sub>
    </td>
    <td width="50%" align="center">
      <img src="screenshots/scripting.png" alt="NexusScript scripting engine"/>
      <br><sub><b>Scripting engine</b> — <code>script demo.ns</code> (loops, conditionals, <code>run</code>)</sub>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center">
      <img src="screenshots/sound.png" alt="AC'97 sound subsystem"/>
      <br><sub><b>Sound subsystem</b> — <code>sndinfo</code> (Intel AC'97 codec, mixer, WAV player)</sub>
    </td>
    <td width="50%" align="center">
      <img src="screenshots/image-viewer.png" alt="Image viewer"/>
      <br><sub><b>Image viewer</b> — <code>view logo.png</code> (BMP/PNG/JPEG/GIF decoders)</sub>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center">
      <img src="screenshots/video-player.png" alt="AVI/MJPEG media player"/>
      <br><sub><b>Media player</b> — <code>mplay</code> (RIFF/AVI parser, MJPEG video + synced audio)</sub>
    </td>
    <td width="50%" align="center">
      <img src="screenshots/gpu-sprites.png" alt="VirtIO-GPU sprite engine"/>
      <br><sub><b>Sprite engine</b> — <code>sprites</code> (VirtIO-GPU zero-copy presents, ~520 fps)</sub>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center">
      <img src="screenshots/gpu-bench.png" alt="GPU benchmark"/>
      <br><sub><b>GPU benchmark</b> — <code>gpubench</code> (4620 fps GPU present vs 107 fps VESA memcpy)</sub>
    </td>
    <td width="50%" align="center">
      <img src="screenshots/doom_title.png" alt="NEXUSDOOM title screen"/>
      <br><sub><b>NEXUSDOOM</b> — <code>doom</code> (from-scratch raycaster on the NexusSDL gaming framework)</sub>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center">
      <img src="screenshots/doom.png" alt="NEXUSDOOM gameplay"/>
      <br><sub><b>NEXUSDOOM gameplay</b> — textured DDA raycaster, z-tested demon sprites, hitscan + HUD</sub>
    </td>
    <td width="50%" align="center">
      <img src="screenshots/breakout.png" alt="Breakout"/>
      <br><sub><b>Breakout</b> — <code>breakout</code> (Q16.16 ball physics, paddle english, ~200-line NexusSDL demo)</sub>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center">
      <img src="screenshots/gamepad.png" alt="Game controller tester"/>
      <br><sub><b>Controller tester</b> — <code>gamepad</code> (12-button virtual pad from raw PS/2 scancodes)</sub>
    </td>
    <td width="50%" align="center">
      &nbsp;
    </td>
  </tr>
</table>

</details>

---

## ✨ Feature Highlights

### 🖥️ Desktop Environment (Aurora)
- **Native 1080p** — 1920×1080×32bpp linear framebuffer, 32bpp back buffer with dirty-rect presents, VirtIO-GPU scanout when available
- **Menu bar** — NexusOS menu (About, System Settings, Force Quit, Sleep/Restart/Shut Down, Lock Screen, Exit to Console), the active app's name and File/Edit/View/Go/Window/Help menus, plus Wi-Fi, Spotlight, Control Center and the clock
- **Dock** — frosted shelf with magnification, name bubbles, running-app dots and the Trash; clicking a running app brings its window forward
- **Spotlight** (`Ctrl+Space`) — finds apps and system actions as you type
- **Launchpad** (`F4`) — full-screen grid of every app
- **Windows** — up to 8 overlapping windows: rounded corners, soft shadows, traffic lights with hover glyphs, drag, resize from the sides and corners, zoom (double-click the title bar), minimize, tile left/right
- **App switcher** (`Tab`/`F3`), **Notification Center** (click the clock), banner notifications, right-click desktop menu, keyboard-shortcuts sheet (`F1`)
- **Login & lock screen** — big clock, user pictures, password field with shake feedback, Restart / Shut Down
- **Boot splash** — logo and progress bar driven by the kernel's real init steps
- **Typography** — anti-aliased proportional UI fonts and an anti-aliased monospace cell font (generated by `gen_uifont.py`)
- **Virtual workspaces** (`Ctrl+F5`…`F8`), widgets (`Ctrl+I`), clipboard history, system log, screensaver after 5 minutes idle
- **Themes** — macOS dark palette by default; Light, Retro, Ocean and High-Contrast themes remain for the classic cell apps

### 🌐 Networking Stack
- **RTL8139 NIC Driver** — PCI-based Ethernet driver with full TX/RX, MAC addressing, link status, packet statistics
- **ARP** — Address Resolution Protocol with aging cache
- **IPv4** — Internet Protocol with header construction, validation, routing
- **ICMP** — Ping send/receive with RTT measurement, statistics
- **UDP** — User Datagram Protocol with port binding, callbacks
- **TCP** — Full TCP with 3-way handshake, sequence numbers, windowing, retransmission, state machine (LISTEN→SYN_SENT→ESTABLISHED→FIN_WAIT→CLOSED)
- **BSD Socket API** — socket(), bind(), listen(), accept(), connect(), send(), recv()
- **DNS Resolver** — Query construction, response parsing, domain-to-IP resolution
- **HTTP/1.1 Client** — GET requests, response parsing, header extraction, body download
- **HTML Parser** — Tag parsing (p, h1-h6, a, ul, li, pre, br), link extraction
- **Web Browser** — URL navigation, link clicking, page history, bookmarks
- **DHCP Client** — Discover→Offer→Request→Ack, auto IP/gateway/DNS configuration
- **NTP Client** — Time synchronization with NTP servers
- **HTTP Server** — Serves pages on port 8080 with start/stop control
- **Remote Shell** — Telnet-style remote access on port 2323

### 🔧 Compatibility Layers
- **POSIX Compatibility** — `open`/`read`/`write`/`close`/`fork`/`exec` syscall translation, signal handling (SIGTERM, SIGKILL, SIGSTOP), job control
- **procfs** — `/proc/cpuinfo`, `/proc/meminfo`, `/proc/uptime`, `/proc/version`, `/proc/status`, `/proc/mounts`
- **termios** — Terminal I/O control for Unix-style terminal behavior
- **Environment Variables** — `PATH`, `HOME`, `USER`, `SHELL`, `TERM`, `PWD` with `export KEY=VALUE`
- **ELF32 Binary Loader** — Load and execute ELF32 binaries in ring-3 userspace
- **ELF Dynamic Linking** — Shared libraries (.so), dynamic linker, `dlopen`/`dlsym`/`dlclose` API
- **Built-in libc** — 26+ symbols: `printf`, `malloc`, `free`, `memcpy`, `memset`, `strcmp`, `strlen`, `fopen`, `fread`, `fwrite`, `fclose`, etc.
- **X11 Compatibility Shim** — X11R6 protocol mapped to NexusOS window manager:
  - Display: `XOpenDisplay`, `XCloseDisplay`, `XDefaultScreen`, `XDisplayWidth/Height`
  - Windows: `XCreateSimpleWindow`, `XDestroyWindow`, `XMapWindow`, `XUnmapWindow`, `XMoveWindow`, `XResizeWindow`, `XRaiseWindow`, `XStoreName`
  - Drawing: `XDrawPoint`, `XDrawLine`, `XDrawRectangle`, `XFillRectangle`, `XDrawArc`, `XFillArc`, `XDrawString`, `XClearArea`
  - Events: `XSelectInput`, `XNextEvent`, `XPending`, `XCheckMaskEvent`
  - GC: `XCreateGC`, `XFreeGC`, `XSetForeground`, `XSetBackground`, `XSetLineAttributes`
  - Atoms: `XInternAtom`, `XGetAtomName`
  - Misc: `XFlush`, `XSync`, `XBell`
  - Supports 16 X11 windows, 32 GCs, 8 pixmaps, 64 atoms, 32-event queue
- **Win32 Compatibility Layer** — Full PE32 loader + Win32 API shim:
  - **PE32 Loader** — DOS MZ header validation, PE signature parsing, i386 section loading (.text/.data/.bss), import table resolution, ring-3 userspace execution
  - **kernel32.dll** — `GetModuleHandle`, `ExitProcess`, `CreateFileA`, `ReadFile`, `WriteFile`, `CloseHandle`, `GetFileSize`, `HeapAlloc`, `HeapFree`, `Sleep`, `GetTickCount`, `GetCommandLineA`, `OutputDebugStringA`
  - **user32.dll** — `RegisterClassA`, `CreateWindowExA`, `ShowWindow`, `UpdateWindow`, `GetMessageA`, `TranslateMessage`, `DispatchMessageA`, `PostQuitMessage`, `DefWindowProcA`, `MessageBoxA`, `DestroyWindow`, `BeginPaint`, `EndPaint`, `GetDC`, `ReleaseDC`
  - **gdi32.dll** — `TextOutA`, `SetBkColor`, `SetTextColor`, `GetStockObject`, `SelectObject`, `CreateSolidBrush`, `DeleteObject`, `FillRect`, `Rectangle`, `MoveToEx`, `LineTo`, `SetPixel`
  - **advapi32.dll** — `RegOpenKeyExA`, `RegCreateKeyExA`, `RegCloseKey`, `RegQueryValueExA`, `RegSetValueExA`
  - **Registry Emulation** — In-memory flat key-value store, `HKEY_LOCAL_MACHINE`/`HKEY_CURRENT_USER`/`HKEY_CLASSES_ROOT`, pre-populated Windows version keys
  - **Import Resolver** — `win32_resolve_import()` dynamically binds PE imports to shim functions
  - 47 Win32 API functions shimmed, 16 windows, 8 classes, 32 handles, 64-event message queue

### 📦 Package Manager
- **`.npk` Archive Format** — Binary NexusOS Package: fixed header (magic `NPK1`, name/version/description/author, dependency + file tables) followed by a concatenated payload section
- **CRC32 Integrity** — A header CRC covers the entire archive body, and every payload file carries its own CRC32 — a corrupt download is rejected *before* anything is written to disk
- **Bundled Repository** — 8 curated packages (`libnx`, `coreutils`, `hello`, `fetch`, `nano`, `nxedit`, `sdl-shim`, `doom`) with a real dependency graph
- **Recursive Dependency Resolution** — Depth-first, dependencies installed first, with cycle detection, deduplication, and minimum-version checking (`>=`)
- **Install Pipeline** — `serialize → CRC verify → extract to VFS → register` with all-or-nothing rollback on filesystem failure
- **Installed Database** — Tracks each installed package's version, files, and on-disk size so `npkg remove` cleans up precisely (leaving shared dependencies intact)
- **Remote Mirror Probe** — `npkg update` does a best-effort HTTP reach of the configured mirror, falling back to the bundled local mirror
- **CLI** — `npkg list / search / info / install / remove / installed / update`

### 📜 Scripting Engine (NexusScript)
- **Real Interpreter** — lexer → token stream → recursive-descent evaluator walked by a token cursor (no external dependencies, fully in-kernel)
- **Language** — integer + string values, variables (`let`/reassignment), `if/elif/else/end`, `while/do/end`, `print`, and comments (`#`)
- **Expressions** — `+ - * / %`, comparisons `== != < <= > >=`, logical `and / or / not`, parentheses, string concatenation with `+`
- **Builtins** — `len(x)`, `str(x)`, `abs(x)`
- **OS Automation** — `run "<command>"` executes any shell command line from within a script, enabling macros and batch tasks
- **Safe by Design** — every statement and loop iteration draws from a step budget, so a runaway script can never hang the kernel; recursion and nesting are also bounded
- **CLI** — `script <file.ns>` (a sample `demo.ns` is installed at boot)

### 🍎 macOS Compatibility Shim
- **Mach-O Loader** — Parses 32-bit i386 Mach-O executables: validates the header (`MH_MAGIC`, `CPU_TYPE_X86`, `MH_EXECUTE`), walks the load commands (`LC_SEGMENT`, `LC_UNIXTHREAD`/`LC_MAIN`, `LC_LOAD_DYLIB`), maps segments (skipping `__PAGEZERO`), resolves the entry point, and transitions to ring-3
- **Core Foundation** — Reference-counted object model (`CFRetain`/`CFRelease`/`CFGetRetainCount`) over a fixed pool: `CFStringCreateWithCString`, `CFStringGetCStringPtr`, `CFNumberCreate`/`CFNumberGetValue`, `CFGetTypeID`, `CFShow`
- **Cocoa / AppKit** — `NSWindow` bridged to the NexusOS window manager (create / order-front / close), `NSString`, `NSLog`; a released `NSWindow` tears down its underlying window
- **CLI** — `machoinfo` (subsystem status), `runmacho <file>` (load a Mach-O), `cocoademo` (build CF objects + a live NSWindow on the desktop)

### 🔊 Sound (Era 5 — Multimedia)
- **Intel AC'97 Driver** — Real driver for the Intel 82801AA AC'97 controller (PCI `8086:2415`, class `04:01`): probes the PCI bus, enables bus-master DMA, brings the controller out of cold reset, resets the codec, and waits for the primary codec to report ready
- **Bus-Master PCM-out** — Streams 16-bit PCM through the NABM engine using a **Buffer Descriptor List**; reads the mixer (NAM) and bus-master (NABM) I/O windows straight from the device's two PCI BARs, with poll-to-completion playback bounded by a tick timeout + spin cap so a wedged engine can never hang the kernel
- **Software Mixer** — Blends up to **8 simultaneous PCM voices** into one 16-bit stereo stream, with per-voice + master volume (0–100), mono→stereo up-mix, and nearest-neighbour resampling to the device rate (sources at any rate play correctly)
- **WAV Player** — Parses RIFF/WAVE files (8- and 16-bit PCM, mono/stereo), borrowing 16-bit samples in place and converting 8-bit on the fly; a `startup.wav` chime is generated and installed in the filesystem at boot
- **Tone Generator** — Integer-only sine synthesis (Bhaskara approximation + Q16 phase accumulator — no FPU, no libm) with small looping waveform buffers for arbitrary frequencies
- **Device-Agnostic Core** — The mixer/WAV layer (`audio.c`) sits above a small sound-device interface; AC'97 (`ac97.c`) registers itself as the active sink, and a timed null-sink fallback keeps the commands working when no codec is present
- **CLI** — `sndinfo` (status), `play <file.wav>`, `volume <0-100>`, `tone <hz> <ms>`, `mixer` (3-voice C-E-G chord demo)

### 🖼️ Image Formats (Era 5 — Multimedia)
- **BMP** — Uncompressed 24/32-bit, top-down and bottom-up scanline order
- **PNG** — A from-scratch **DEFLATE/zlib inflater** (stored + fixed + dynamic Huffman) plus all five scanline filters (none/sub/up/avg/paeth); supports 8-bit grayscale, RGB, palette, gray+alpha and RGBA
- **JPEG** — A **baseline sequential** decoder: DQT/DHT/SOF0/DRI/SOS parsing, canonical Huffman decode, dequantization, an integer **IDCT**, chroma upsampling for any sampling factor, and YCbCr→RGB
- **GIF** — GIF87a/89a with **LZW** decompression, global/local colour tables, interlacing, transparency, and **animated multi-frame** playback with disposal
- **Unified Pipeline** — Magic-byte format detection → per-format decoder → 32-bit ARGB buffer; integer-only math throughout (no FPU, no libm, no 64-bit division)
- **Fullscreen Viewer** — Scales each image to fit 1024×768 with a caption; animated GIFs cycle their frames until a key is pressed. A `startup.wav`-style set of sample images (`icon.bmp`, `logo.png`, `photo.jpg`, `anim.gif`) is installed at boot
- **CLI** — `imginfo` (decode every sample + report format/size/centre pixel), `view <file>` (open any image fullscreen)

### 🎬 Video Playback (Era 5 — Multimedia)
- **RIFF/AVI Parser** — Walks the AVI container: `hdrl` (main `avih` header + per-stream `strh`/`strf`) and the `movi` chunk list (`NNdc`/`NNdb` video, `NNwb` audio), with no dependency on the `idx1` index
- **Motion-JPEG Video** — Each frame is a full JPEG decoded through the Phase 39 baseline decoder, then rendered scaled-to-fit on the framebuffer with a live frame counter
- **Synchronized Audio** — The PCM audio stream is concatenated and played through the Phase 38 AC'97 mixer; **video is paced off the audio clock** (one frame's worth of samples per video frame) so picture and sound stay aligned, with a tick-paced fallback for silent clips
- **Media Player** — Fullscreen playback with caption (`name WxH frame N/total fps +audio`); press any key to stop. A bundled `demo.avi` (80×60, 16 frames, 8 fps, 8 kHz audio) is embedded in the kernel
- **CLI** — `vidinfo [file]` (parse + report dimensions, frame count, fps, codec, audio format), `mplay [file]` (play; no arg = the bundled demo)

### 🖥️ GPU Acceleration (Era 5 — Multimedia)
- **VirtIO Transport** — A from-scratch **VirtIO 1.0 modern PCI transport**: walks the PCI capability list for the vendor config structures (common/notify/ISR/device), identity-maps the MMIO windows, negotiates `VERSION_1`, and drives **split virtqueues** in polled mode (tick timeout + spin cap, INTx disabled — no IRQs needed)
- **VirtIO-GPU Driver** — Probes PCI `1AF4:1050` (QEMU `-device virtio-vga`), then takes over the display: `GET_DISPLAY_INFO` → `RESOURCE_CREATE_2D` (B8G8R8X8) → `ATTACH_BACKING` → `SET_SCANOUT`; once the scanout flips, the VGA-compat output is retired and the driver owns presentation
- **Zero-Copy Presents** — The scanout resource is backed **directly by the kernel back buffer**, so presenting a frame is a single `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` pair instead of a 3 MB memcpy — **4,620 fps full-frame vs 107 fps for the legacy VESA flip (43× faster)**, and dirty-rectangle presents hit 31,000+ fps
- **Accelerated 2D** — `rep stosl`/`rep movsl` fill and blit primitives on the back buffer (4 GB/s fills — 4× the per-pixel path; 3.1 GB/s blits with overlap-safe copies)
- **Sprite Engine** — Up to 32 ARGB sprites with **per-pixel alpha blending**, z-ordering, show/hide and clipping, composited each frame; the bundled demo animates 10 shaded, anti-aliased balls over a gradient at **~520 fps**
- **Transparent Fallback** — `fb_flip()` routes through the GPU when the scanout is active and falls back to the VESA memcpy otherwise; the whole desktop renders identically with or without the device
- **CLI** — `gpuinfo` (transport, scanout, features, present count), `gpubench` (present/fill/blit benchmark), `sprites` (animated sprite demo)

### 🎮 Gaming Framework (Era 5 — Multimedia)
- **NexusSDL** — An SDL-like game API for the bare-metal kernel: open a **256-color palettized surface** (up to 320×240), draw with pixels/lines/rects/circles/sprites/scaled text, and present — the framework palette-expands and **integer-scales the surface to 1024×768** through the VirtIO-GPU dirty-rect present path (VESA flip fallback)
- **Fixed-Point Math Kit** — Q16.16 multiply/divide (single widening `imull` / inline `idivl` — no libgcc, no FPU), LUT sine/cosine on a 1024-unit circle (Bhaskara approximation), integer sqrt — everything a game sim needs in a freestanding kernel
- **Virtual Game Controller** — A raw-scancode hook in the PS/2 driver feeds a **12-button virtual pad** (D-pad, A/B/X/Y, L/R, Start/Select) with held-state, edge detection, and an SDL-style event queue; games *acquire* the pad (keys stop reaching the console) and *release* it on exit
- **Tick-Locked Timing** — Frames sync to the PIT tick (~18 fps, 55 ms) for deterministic simulation, with frame/fps/ms counters and non-blocking PC-speaker sfx
- **NEXUSDOOM** — A from-scratch **textured raycaster FPS**: one DDA walk per screen column with a per-column depth buffer, procedural 64×64 wall textures, **z-tested billboard demons** that chase and melee, hitscan pistol, HUD, minimap, and title/play/dead/victory states — clear all 6 demons, then find the glowing EXIT door
- **Breakout** — A second, ~200-line API demo: 6×10 brick wall, Q16 ball physics with paddle english, lives and scoring
- **CLI** — `gameinfo` (framework + pad map), `gamepad` (visual 12-button controller tester), `doom`, `breakout`

### 🧠 Core Kernel
- **Custom 2-Stage Bootloader** — Stage 1 (512-byte MBR at 0x7C00): loads stage 2 via BIOS INT 0x13. Stage 2: sets up VESA VBE mode, then **high-loads the kernel to 1 MB** — BIOS-reads 32 KB chunks into a low bounce buffer and copies each up during brief protected-mode hops (lifts the old 576 KB real-mode ceiling; kernel can now grow to ~720 KB), then switches to 32-bit Protected Mode
- **GDT** — Global Descriptor Table with kernel/user code and data segments
- **IDT** — Interrupt Descriptor Table with 256 entries, CPU exception handlers (ISR 0–31), hardware interrupt stubs (IRQ 0–15)
- **PIC** — 8259 Programmable Interrupt Controller, remapped to IRQ 32–47
- **Timer** — IRQ0 at ~18.2 Hz driving system tick counter, scheduler, status bar updates
- **Physical Memory Manager** — Bitmap page allocator (4 KB pages); detects installed RAM via CMOS and allocates from the 40 MiB identity-mapped window, with the kernel, heap, stack, .bss and frame buffers reserved
- **Paging** — Page directory + page tables, identity-mapped kernel, VESA framebuffer mapping
- **Heap Allocator** — `kmalloc`/`kfree` with free-list management, block splitting/coalescing
- **Virtual Memory Manager** — Copy-on-Write `fork()`, shared memory between processes, memory-mapped files
- **VFS** — Virtual Filesystem layer with mountpoint support
- **RAM Filesystem** — In-memory filesystem for runtime file storage
- **FAT32** — Read/write FAT32 on ATA disk, directory hierarchy, long filename support
- **Process Manager** — Process creation/termination, PID tracking, state machine (UNUSED→READY→RUNNING→BLOCKED→TERMINATED)
- **Round-Robin Scheduler** — Enabled for ring-3 user processes (assembly context switch, per-process CR3); the kernel/desktop thread stays cooperative so lock-free kernel state is safe
- **Kernel self-protection (Phase 55)** — W^X kernel image (read-only .text/.rodata via CR0.WP), unmapped NULL page, 16 KB kernel-stack guard band, and `kguard`: setjmp/longjmp-style crash containment for shell commands, desktop frames and every app draw/key/click callback, with a double-fault task gate (own TSS) that even recovers from a kernel-stack overflow
- **Persistent RAM-FS (Phase 55)** — crash-safe A/B snapshot slots on the IDE disk (CRC32-checked header + payload, generation counter), built-in/dirty tracking with tombstones for deleted built-ins, debounced autosave, flush on reboot/shutdown, user-account table persisted as `/etc/passwd.db`
- **System Calls** — INT 0x80 interface for userspace→kernel transitions
- **Pipes** — Inter-process communication with read/write endpoints
- **PCI Bus** — Enumeration of PCI devices, vendor/device ID matching, BAR reading
- **Driver Framework** — init/probe/remove model with IRQ routing
- **ATA Disk Driver** — PIO-mode ATA disk read/write
- **Block Device Layer** — Abstract block device interface
- **Disk Partitioning** — MBR partition table parsing
- **USB** — Polled host-controller stack with one HCD abstraction: xHCI, EHCI (companion hand-off), OHCI and UHCI drivers, hub class driver (nested hubs, hot-plug), descriptor-driven enumeration, HID keyboard/mouse (boot protocol) injected into the input path, and Bulk-Only mass storage registered as a block device (`usb0`)
- **Real-Time Clock** — CMOS RTC read, time/date formatting
- **PC Speaker** — Boot sound, beep tones
- **Keyboard Driver** — PS/2 keyboard with IRQ1, scancode translation, modifier keys, arrow key history navigation
- **Mouse Driver** — PS/2 mouse with IRQ12, absolute positioning, button state, scroll support

---

## 🎮 Built-in Applications (30+)

| App | Description | Source |
|---|---|---|
| 🖥️ **Shell** | Interactive command shell with 93 commands, history, pipe/redirect, $VAR expansion | `shell.c` (67 KB) |
| 📝 **Text Editor** | Full-screen text editor with cursor navigation and file save | `editor.c` |
| 📓 **Notepad** | GUI notepad window with clipboard support | `notepad.c` |
| 🧮 **Calculator** | Multi-operation calculator with button UI | `calculator.c` |
| 📁 **File Manager** | Browse files, view sizes, open/delete | `filemgr.c` |
| 📊 **System Monitor** | CPU, memory, process stats in real-time | `sysmon.c` |
| 📋 **Task Manager** | View and kill running processes | `taskmgr.c` |
| ⚙️ **Settings** | 3-tab settings (Theme, System, About) | `settings.c` |
| 🎨 **Appearance** | Theme and visual customization | `appearance.c` |
| 🌐 **Web Browser** | HTML rendering, URL navigation, link following, history | `browser.c` (20 KB) |
| 📅 **Calendar** | Month view calendar with date navigation | `calendar.c` |
| 📇 **Contacts** | Contact list manager | `contacts.c` |
| ✅ **Todo List** | Add, check, delete todo items | `todo.c` |
| 🎵 **Music Player** | PC speaker music with play/stop controls | `music.c` |
| 🎨 **Paint** | Freehand drawing canvas | `paint.c` |
| 🎨 **Color Picker** | Visual color selection tool | `colorpick.c` |
| 📂 **Hex Viewer** | View file contents in hexadecimal | `hexview.c` |
| 🔍 **Search** | Search files by name | `search.c` |
| ❓ **Help Center** | In-app help documentation | `help.c` |
| ℹ️ **System Info** | Detailed hardware and OS information | `sysinfo.c` |
| 📜 **System Log** | View system event log | `syslog.c` |
| 📋 **Clipboard Manager** | Clipboard history viewer | `clipmgr.c` |
| 📂 **File Operations** | Copy, move, rename GUI | `fileops.c` |
| 🕐 **Clock** | Desktop clock widget | `clock.c` |
| 🗑️ **Recycle Bin** | Deleted file recovery | `recycle.c` |
| 🔒 **Lock Screen** | Secure screen lock | `lockscreen.c` |
| 🖼️ **Screensaver** | Animated idle screensaver | `screensaver.c` |
| ⚡ **Power Menu** | Shutdown, restart, lock | `power.c` |
| 🐍 **Snake** | Classic snake game with score | `snake.c` (10 KB) |
| 🏓 **Pong** | Two-paddle pong game | `pong.c` |
| 💣 **Minesweeper** | Grid-based mine detection game | `minesweeper.c` |
| 🧱 **Tetris** | Falling block puzzle game | `tetris.c` |

---

## 🖥️ Shell Commands (105 Total)

### System Commands

| Command | Description |
|---|---|
| `help` | Show all available commands, grouped by category |
| `about` | Display NexusOS version and info |
| `meminfo` | Show physical memory statistics (total/used/free KB) |
| `heapinfo` | Show heap allocator stats (total/used/free bytes) |
| `uptime` | Show system uptime in minutes, seconds, and ticks |
| `date` | Show current date and time from RTC |
| `reboot` | Restart the system (triple-fault reboot) |
| `uname` | Print OS name, version, architecture, phase |
| `whoami` | Print current username (from $USER env var) |
| `sysinfo` | Launch system information viewer (GUI) |
| `log` | Show system log entry count |

### Display Commands

| Command | Description |
|---|---|
| `clear` / `cls` | Clear the entire screen |
| `echo <text>` | Print text to screen (supports `$VAR` expansion) |
| `color <0-15>` | Change terminal text color (VGA palette) |
| `history` | Show last 16 commands from ring buffer |

### File Commands

| Command | Description |
|---|---|
| `ls` | List all files with sizes, directories highlighted in blue |
| `cat [-n] <file>` | Display file contents (optional line numbers with `-n`), supports `/proc/*` paths |
| `head [-n N] <file>` | Show first N lines of a file (default 10) |
| `tail [-n N] <file>` | Show last N lines of a file (default 10) |
| `grep <pattern> <file>` | Search file for pattern with highlighted matches |
| `wc <file>` | Count lines, words, and characters in a file |
| `touch <file>` | Create a new empty file |
| `write <file> <text>` | Write text content to a file (auto-creates if needed) |
| `rm <file>` | Delete a file |
| `edit <file>` | Open file in full-screen text editor |
| `trash` | Show recycle bin item count |
| `restore` | Restore most recent item from recycle bin |

### Process Commands

| Command | Description |
|---|---|
| `ps` | List all processes with PID, state, and name |
| `run <name>` | Start a new background process |
| `kill <pid>` | Terminate a process by PID (cannot kill PID 0/1) |

### Network Commands

| Command | Description |
|---|---|
| `net` | Show all network interfaces with MAC, link status, TX/RX stats |
| `ifconfig` | Show full network configuration (IP, mask, gateway, DNS, stats) |
| `ping <ip>` | Send 4 ICMP echo requests with RTT measurement and statistics |
| `netstat` | Show active TCP connections and UDP bindings with state |
| `arp` | Display ARP cache table (IP → MAC mappings with age) |
| `dns <hostname>` | Resolve a domain name to IP address |
| `wget <url>` | Download and display HTTP response (status, size, body preview) |
| `browse <url>` | Open URL in GUI web browser (requires desktop mode) |
| `dhcp` | Run DHCP client to auto-configure IP/gateway/DNS |
| `ntp` | Sync system clock with NTP time server |
| `httpd` | Start built-in HTTP server on port 8080 |
| `httpd stop` | Stop the HTTP server |
| `rshell` | Start remote shell server on port 2323 |
| `rshell stop` | Stop the remote shell server |

### Dynamic Linking Commands

| Command | Description |
|---|---|
| `ldd` | List all loaded shared libraries with base address and size |
| `ldconfig` | Show all 26+ built-in libc symbols with memory addresses |
| `dlopen <lib.so>` | Load a shared library, returns handle |
| `dlsym <handle> <symbol>` | Look up symbol address (handle 0 = search all) |

### X11 Compatibility Commands

| Command | Description |
|---|---|
| `xinfo` | Show X11 shim status, display info, protocol version, supported API |
| `xdemo` | Launch X11 demo window with colored rectangles, lines, arcs, and text |

### Win32 Compatibility Commands

| Command | Description |
|---|---|
| `win32info` | Show Win32 subsystem status (active windows, handles, classes, shimmed DLLs) |
| `regedit` | Display all registry entries (root key, path, value name, data) |
| `runexe <file>` | Load and execute a PE32 `.exe` from filesystem |

### Package Manager Commands

| Command | Description |
|---|---|
| `npkg list` | List all packages in the bundled repository (installed ones marked) |
| `npkg search <term>` | Search packages by name or description (case-insensitive) |
| `npkg info <pkg>` | Show package version, author, dependencies, and files |
| `npkg install <pkg>` | Install a package, auto-resolving and installing its dependencies |
| `npkg remove <pkg>` | Uninstall a package and delete its files (shared deps kept) |
| `npkg installed` | List installed packages with file count and on-disk size |
| `npkg update` | Refresh the package index (probes the remote mirror) |

### Scripting Commands

| Command | Description |
|---|---|
| `script <file.ns>` | Run a NexusScript file (variables, `if`/`while`, `print`, `run`, expressions) |
| `script demo.ns` | Run the sample script installed at boot |

### macOS Compatibility Commands

| Command | Description |
|---|---|
| `machoinfo` | Show the Mach-O loader + Cocoa/Core Foundation shim status |
| `runmacho <file>` | Parse and load a 32-bit i386 Mach-O executable (segments, dylibs, entry) |
| `cocoademo` | Create Core Foundation objects and a live Cocoa `NSWindow` on the desktop |

### Sound Commands

| Command | Description |
|---|---|
| `sndinfo` | Show the sound subsystem status (AC'97 BARs, codec ready, mixer rate, master volume, active voices) |
| `play <file.wav>` | Parse and play a PCM WAV file through the AC'97 driver (defaults to `startup.wav`) |
| `volume [0-100]` | Show or set the master output volume |
| `tone <hz> <ms>` | Synthesize and play a sine tone for the given duration |
| `mixer` | Demo the software mixer — play a 3-voice C-E-G major chord simultaneously |

### Image Commands

| Command | Description |
|---|---|
| `imginfo` | Decode every bundled sample and report its format, dimensions, frames, and centre pixel |
| `view <file>` | Decode and display an image fullscreen (BMP/PNG/JPEG/GIF; animated GIFs cycle frames) |

### Video Commands

| Command | Description |
|---|---|
| `vidinfo [file]` | Parse an AVI and report dimensions, frame count, fps, codec, and audio format (no arg = bundled `demo.avi`) |
| `mplay [file]` | Play an AVI/MJPEG clip fullscreen with synchronized audio (no arg = bundled demo) |

### GPU Commands

| Command | Description |
|---|---|
| `gpuinfo` | Show the VirtIO-GPU state (transport, scanout resource, zero-copy backing, features, present count) |
| `gpubench` | Benchmark GPU presents vs the VESA memcpy flip, plus fill/blit throughput |
| `sprites` | Animated sprite-engine demo (alpha-blended balls at several hundred fps; any key exits) |

### Application Launchers

| Command | Description |
|---|---|
| `gui` | Launch full desktop environment |
| `theme <name>` | Switch theme (`dark` / `light` / `retro` / `ocean`) |
| `calc` | Launch Calculator (opens desktop) |
| `files` | Launch File Manager (opens desktop) |
| `sysmon` | Launch System Monitor (opens desktop) |
| `settings` | Launch Settings (opens desktop) |
| `notepad` | Launch Notepad (opens desktop) |
| `taskmgr` | Launch Task Manager (opens desktop) |
| `calendar` | Launch Calendar (opens desktop) |
| `music` | Launch Music Player (opens desktop) |
| `paint` | Launch Paint (opens desktop) |
| `minesweeper` | Launch Minesweeper (opens desktop) |
| `todo` | Launch Todo List (opens desktop) |
| `pong` | Launch Pong (opens desktop) |
| `search` | Launch Search (opens desktop) |
| `tetris` | Launch Tetris (opens desktop) |
| `hexview` | Launch Hex Viewer (opens desktop) |
| `contacts` | Launch Contacts (opens desktop) |
| `colors` | Launch Color Picker (opens desktop) |
| `snake` | Play Snake game (text mode) |
| `beep` | Play boot sound through PC speaker |

### POSIX Commands

| Command | Description |
|---|---|
| `env` | Show all environment variables (PATH, HOME, USER, SHELL, TERM, PWD) |
| `export KEY=VALUE` | Set an environment variable |

### Shell Operators

| Operator | Description |
|---|---|
| `cmd > file` | Redirect command output to a file |
| `cmd \| grep pattern` | Pipe output through grep (supports `ls`, `ps`, `env`, `cat`) |

---

## 🎨 Desktop GUI

### Keyboard Shortcuts

This table and the feature/theme catalogue below describe `build.bat legacy`.
Native x64 shortcuts are listed in the native Aurora section above.

| Shortcut | Action |
|---|---|
| `Ctrl+Space` (or `Win+Space`, `Ctrl+P`) | Spotlight |
| `F4` / `Ctrl+M` | Launchpad |
| `Tab` / `F3` | App switcher |
| `F1` | Keyboard shortcuts sheet |
| `F2` | Tips (help) |
| `F12` | Exit to Console (text shell) |
| `Ctrl+A` | About This NexusOS |
| `Ctrl+T` | Terminal |
| `Ctrl+F` | Finder (file manager) |
| `Ctrl+K` | Calculator |
| `Ctrl+S` | Activity Monitor |
| `Ctrl+B` | Notes |
| `Ctrl+G` | Task Manager |
| `Ctrl+D` | Calendar |
| `Ctrl+N` | Notification Center |
| `Ctrl+L` | Lock screen |
| `Ctrl+I` | Toggle desktop widgets |
| `Ctrl+W` / `Ctrl+Q` | Close the focused window |
| `Ctrl+V` | Paste from clipboard |
| `Ctrl+←` / `Ctrl+→` | Tile the focused window left / right |
| `Ctrl+↑` / `Ctrl+↓` | Zoom / restore the focused window |
| `Ctrl+F5`…`Ctrl+F8` | Switch to desktop 1–4 |
| `Esc` | Close the open menu / overlay |
| Right-click desktop | Context menu |
| Double-click title bar | Zoom |

### Desktop Features

| Feature | Details |
|---|---|
| **Menu bar** | NexusOS menu, app menus, status items, clock; menus slide across like macOS |
| **Dock** | Magnification, name bubbles, running dots, Trash |
| **Spotlight / Launchpad** | Search or browse every app and system action |
| **Login / Lock Screen** | Aurora login window with real password checks |
| **Notifications** | Banners under the menu bar; history in Notification Center |
| **Right-Click Menu** | New File, Get Info, Change Appearance, System Settings, Refresh |
| **Virtual Workspaces** | 4 independent desktops |
| **App Switcher** | Icons of open windows, Tab to cycle |
| **Clipboard Manager** | History of copied text |
| **System Logger** | Tracks app opens and system events |
| **Power** | Sleep, Restart, Shut Down (ACPI) from the NexusOS menu or the login window |

### Themes

| Theme | Style | Description |
|---|---|---|
| 🌙 **Dark** | Cyan on black | Default — modern dark interface |
| ☀️ **Light** | Blue on white | Clean light interface |
| 💚 **Retro** | Green on black | Classic terminal aesthetic |
| 🌊 **Ocean** | Blue tones | Calm, blue-gray palette |

### Terminal Window (Desktop)

The legacy i386 desktop includes an embedded GUI terminal with its own command set.
Native x64 Terminal runs native processes and basic commands (`pwd`, `cd`, `echo`,
`ls`, `cat`); it does not supply this kernel-command catalogue or a TTY.

Legacy commands:
- `help` / `help net` / `help srv` / `help dl` / `help x11` — Category-specific help
- All network commands: `ifconfig`, `ping`, `netstat`, `arp`, `dns`, `wget`, `browse`
- All server commands: `dhcp`, `ntp`, `httpd`, `rshell`
- POSIX commands: `uname`, `whoami`, `env`, `export`, `cat`, `wc`, `grep`
- Dynamic linking: `ldd`, `ldconfig`, `dlopen`, `dlsym`
- X11: `xinfo`, `xdemo`
- Win32: `win32info`, `regedit`, `runexe`

---

## 🏗️ Architecture Overview

| Component | Implementation |
|---|---|
| **CPU** | x86 (i386), 32-bit Protected Mode, ring-0 kernel / ring-3 userspace |
| **Bootloader** | Custom 2-stage: Stage 1 MBR (512 bytes @ 0x7C00) → Stage 2 (VESA + Protected Mode) |
| **Display** | VESA VBE 2.0 — 1920×1080, 32bpp, linear framebuffer with a 32bpp back buffer and dirty-rect presents; VirtIO-GPU scanout when present; Aurora compositor (macui/menubar/dock/spotlight) |
| **Input** | PS/2 Keyboard (IRQ1) + PS/2 Mouse (IRQ12) |
| **Memory** | Bitmap PMM (4 KB pages, 40 MiB identity window, CMOS RAM detection) → Paging → Heap (kmalloc) → VMM (CoW, shmem, mmap) |
| **Filesystem** | VFS → RAM filesystem + FAT32 on ATA disk + procfs |
| **Networking** | RTL8139 NIC → Ethernet → ARP → IPv4 → ICMP/UDP/TCP → Socket API → DNS/HTTP |
| **Process Model** | ELF32 loader, round-robin scheduler, POSIX signals, pipes, IPC, context switch (ASM) |
| **GUI Pipeline** | Framebuffer → GFX primitives → 8×16 bitmap font → Widget toolkit → Window manager → Desktop compositor |
| **Compatibility** | POSIX syscalls, ELF dynamic linker (.so), X11R6 shim, Win32 API layer (PE32 + kernel32/user32/gdi32 + registry), Mach-O loader + Cocoa shim |
| **Audio** | Intel AC'97 (82801AA) bus-master PCM-out → software mixer (8 voices, resampling, volume) → WAV player |
| **Images** | Magic-byte detect → BMP / PNG (inflate+filters) / JPEG (baseline IDCT) / GIF (LZW, animated) → ARGB → fullscreen viewer |
| **Video** | RIFF/AVI parse → Motion-JPEG frame decode → framebuffer render + AC'97 PCM, paced off the audio clock |
| **Disk Image** | Floppy `.img` (1.44 MB) + growable hard disk image (1 GiB default, FAT32 partition) |

---

## 📂 Project Structure

```
NexusOS/
├── boot/
│   ├── boot.asm                 # Stage 1 MBR bootloader (512 bytes, loaded at 0x7C00)
│   └── boot2.asm                # Stage 2: VESA VBE setup, kernel load, switch to Protected Mode
│
├── kernel/
│   │
│   │  ═══ CORE ═══════════════════════════════════════════════════════════
│   ├── kernel_entry.asm         # Assembly → C bridge (calls kernel_main)
│   ├── kernel.c                 # Main entry point, initializes all subsystems
│   ├── types.h                  # Fixed-width type definitions (uint8_t, uint32_t, bool)
│   ├── port.h                   # Hardware I/O port access (inb, outb, inw, outw)
│   ├── string.c/h               # String utilities (strcmp, strcpy, strlen, strcat, memcpy, memset)
│   ├── libc.c/h                 # Standard C library (printf, malloc, free, fopen, fread, fwrite)
│   │
│   │  ═══ CPU & INTERRUPTS ═══════════════════════════════════════════════
│   ├── gdt.c/h                  # Global Descriptor Table (kernel/user code+data segments)
│   ├── gdt_asm.asm              # GDT load routine (lgdt + far jump)
│   ├── idt.c/h                  # Interrupt Descriptor Table (256 entries)
│   ├── idt_asm.asm              # IDT load routine (lidt)
│   ├── isr_asm.asm              # CPU exception stubs (ISR 0–31: div-by-zero, page fault, etc.)
│   ├── irq_asm.asm              # Hardware interrupt stubs (IRQ 0–15)
│   ├── pic.c/h                  # 8259 PIC initialization, remapping IRQs to 32–47
│   ├── syscall.c/h              # INT 0x80 system call interface
│   │
│   │  ═══ MEMORY MANAGEMENT ══════════════════════════════════════════════
│   ├── memory.c/h               # Physical page allocator (bitmap, 4 KB pages, 40 MiB window)
│   ├── paging.c/h               # Page directory/tables, identity mapping, VESA FB mapping
│   ├── heap.c/h                 # Kernel heap (kmalloc/kfree, free-list, split/coalesce)
│   ├── vmm.c/h                  # Virtual memory manager (CoW fork, shared memory, mmap)
│   │
│   │  ═══ FILESYSTEM ═════════════════════════════════════════════════════
│   ├── vfs.c/h                  # Virtual filesystem layer (mount, read, write, readdir)
│   ├── ramfs.c/h                # RAM-based filesystem (create, delete, read, write)
│   ├── fat32.c/h                # FAT32 disk filesystem (persistent, directories, LFN)
│   ├── procfs.c/h               # /proc filesystem (cpuinfo, meminfo, uptime, version, mounts)
│   │
│   │  ═══ PROCESS MANAGEMENT ═════════════════════════════════════════════
│   ├── process.c/h              # Process create/terminate, PID, state machine
│   ├── scheduler.c/h            # Round-robin scheduler (present, disabled)
│   ├── switch.asm               # Context switch (save/restore registers)
│   ├── elf.c/h                  # ELF32 binary loader (ring-3 userspace)
│   ├── dynlink.c/h              # Dynamic linker (.so, dlopen/dlsym/dlclose, symbol resolution)
│   ├── pipe.c/h                 # IPC pipes (read/write endpoints)
│   ├── env.c/h                  # Environment variables (get/set/list)
│   ├── posix.c/h                # POSIX compatibility (open/read/write/fork/exec translation)
│   ├── termios.c/h              # Terminal I/O control (termios struct, tcgetattr/tcsetattr)
│   ├── pe.c/h                   # PE32 executable loader (DOS MZ + PE headers, section loading)
│   ├── win32.c/h                # Win32 API shim (kernel32/user32/gdi32, 47 functions, import resolver)
│   ├── registry.c/h             # Windows registry emulation (flat key-value store, advapi32 API)
│   ├── pkg.c/h                  # Package manager (.npk format, CRC32, repository, dependency resolver)
│   ├── script.c/h               # Scripting engine (NexusScript lexer + recursive-descent interpreter)
│   ├── macho.c/h                # Mach-O i386 loader (header, load commands, segments, ring-3 exec)
│   ├── cocoa.c/h                # Cocoa + Core Foundation shim (refcounted objects, NSWindow bridge)
│   │
│   │  ═══ SOUND (Phase 38) ════════════════════════════════════════════════
│   ├── audio.c/h                # Audio core (software mixer, WAV parser, tone gen, PCM HAL)
│   ├── ac97.c/h                # Intel AC'97 driver (PCI probe, codec reset, BDL bus-master PCM out)
│   │
│   │  ═══ IMAGES (Phase 39) ═══════════════════════════════════════════════
│   ├── image.c/h               # Image core (format detect, BMP, dispatch, fullscreen viewer)
│   ├── png.c                    # PNG decoder + from-scratch DEFLATE/zlib inflater + filters
│   ├── jpeg.c                   # Baseline JPEG decoder (Huffman, integer IDCT, YCbCr)
│   ├── gif.c                    # GIF decoder (LZW, palette, interlace, animation)
│   ├── sample_images.h          # Embedded BMP/PNG/JPEG/GIF samples (gen_images.py)
│   │
│   │  ═══ VIDEO (Phase 40) ════════════════════════════════════════════════
│   ├── video.c/h               # RIFF/AVI parser + MJPEG media player (A/V sync)
│   ├── sample_video.h           # Embedded demo.avi (gen_video.py)
│   │
│   │  ═══ GPU (Phase 41) ══════════════════════════════════════════════════
│   ├── virtio.c/h              # VirtIO 1.0 modern PCI transport (caps, virtqueues, polled)
│   ├── gpu.c/h                  # VirtIO-GPU driver (zero-copy scanout, dirty rects, fill/blit)
│   ├── sprite.c/h               # Sprite engine (32 ARGB sprites, alpha blend, z-order)
│   │
│   │  ═══ HARDWARE DRIVERS ═══════════════════════════════════════════════
│   ├── keyboard.c/h             # PS/2 keyboard (IRQ1, scancode→ASCII, modifiers, arrows)
│   ├── mouse.c/h                # PS/2 mouse (IRQ12, absolute coords, buttons, scroll)
│   ├── vga.c/h                  # VGA text mode driver (80×25, color, cursor, status bar)
│   ├── rtc.c/h                  # CMOS real-time clock (read time/date, format strings)
│   ├── speaker.c/h              # PC speaker (tone generation, boot sound)
│   ├── pci.c/h                  # PCI bus enumeration (vendor/device ID, BAR, IRQ)
│   ├── ata.c/h                  # ATA disk driver (PIO mode read/write)
│   ├── usb.c/h                  # UHCI host controller + enumeration + transfers
│   ├── usb_hid.c/h              # USB HID boot keyboard/mouse
│   ├── usb_storage.c            # USB Bulk-Only mass storage (blkdev usb0)
│   ├── uproc.c/h                # User processes: per-AS isolation, ring 3, scheduling
│   ├── usb_hid.c/h              # USB HID driver stub
│   ├── usb_storage.c/h          # USB mass storage stub
│   ├── driver.c/h               # Driver framework (init/probe/remove, IRQ routing)
│   ├── blkdev.c/h               # Block device abstraction layer
│   ├── partition.c/h            # MBR partition table parsing
│   │
│   │  ═══ NETWORKING ═════════════════════════════════════════════════════
│   ├── net.c/h                  # Network core (device registry, packet dispatch, MAC utils)
│   ├── rtl8139.c/h              # RTL8139 NIC driver (PCI, Ethernet TX/RX, IRQ)
│   ├── arp.c/h                  # ARP (request/reply, cache with aging)
│   ├── ip.c/h                   # IPv4 (header build/parse, routing, config)
│   ├── icmp.c/h                 # ICMP (echo request/reply, ping state machine)
│   ├── udp.c/h                  # UDP (send/recv, port binding, callbacks)
│   ├── tcp.c/h                  # TCP (3-way handshake, windowing, retransmit, state machine)
│   ├── socket.c/h               # BSD socket API (socket/bind/listen/accept/connect/send/recv)
│   ├── dhcp.c/h                 # DHCP client (discover/offer/request/ack)
│   ├── dns.c/h                  # DNS resolver (query build, response parse)
│   ├── http.c/h                 # HTTP/1.1 client (GET, headers, body)
│   ├── httpd.c/h                # HTTP server (port 8080, serve pages)
│   ├── ntp.c/h                  # NTP time sync client
│   ├── html.c/h                 # HTML parser (tags, attributes, text extraction)
│   │
│   │  ═══ GRAPHICS & GUI ═════════════════════════════════════════════════
│   ├── gui.c/h                  # GUI cell grid (240×67 cells at 1080p, sized at runtime)
│   ├── macui.c/h                # Aurora toolkit: AA text & shapes, shadows, wallpaper, icons, splash
│   ├── menubar.c/h              # macOS-style menu bar and dropdown menus
│   ├── spotlight.c/h            # Spotlight search and Launchpad
│   ├── uifont.h                 # Generated UI/cell fonts + pointer (gen_uifont.py)
│   ├── framebuffer.c/h          # VESA framebuffer driver (pixel plotting, flip, dimensions)
│   ├── gfx.c/h                  # Graphics primitives (lines, rects, circles, gradients, shadows)
│   ├── font.c/h                 # Font rendering engine (8×8, 8×16 bitmap fonts, scaling)
│   ├── font8x8.h                # 8×8 bitmap font data (CP437 character set)
│   ├── ui_widgets.c/h           # Widget toolkit (buttons, checkboxes, sliders, scrollbars, text inputs)
│   ├── window.c/h               # Window manager (8 windows, macOS chrome, drag, edge resize, zoom, minimize, tile, z-order)
│   ├── desktop.c/h              # Desktop compositor (main loop, event dispatch, rendering pipeline)
│   ├── taskbar.c/h              # Taskbar (start button, window list, system tray, clock)
│   ├── start_menu.c/h           # Start menu (categorized app launcher, keyboard navigation)
│   ├── theme.c/h                # Theme engine (4 themes, color definitions)
│   ├── palette.c/h              # Color palette system (command palette UI)
│   ├── wallpaper.c/h            # Wallpaper rendering (solid color, gradient, BMP photo)
│   ├── wallpaper_data.h         # Embedded wallpaper image data (363 KB)
│   ├── wallpaper_img.c          # Wallpaper image loader
│   ├── icons.c/h                # Desktop icons (render, click, keyboard nav)
│   ├── context_menu.c/h         # Right-click context menu (New File, Refresh, Theme, About, Settings)
│   ├── notify.c/h               # Notification toast system (auto-dismiss)
│   ├── notifcenter.c/h          # Notification center (scrollable history)
│   ├── appearance.c/h           # Appearance settings panel
│   │
│   │  ═══ DESKTOP UTILITIES ══════════════════════════════════════════════
│   ├── login.c/h                # Login screen (animated splash, username/password)
│   ├── lockscreen.c/h           # Lock screen (password prompt)
│   ├── screensaver.c/h          # Screensaver (auto-activate after 60s idle)
│   ├── switcher.c/h             # Window switcher (Tab to cycle, Enter to select)
│   ├── workspaces.c/h           # Virtual workspaces (4 desktops)
│   ├── shortcuts.c/h            # Keyboard shortcuts overlay (F1)
│   ├── dock.c/h                 # Application dock (quick launch bar)
│   ├── clipboard.c/h            # Clipboard manager (copy/paste backend)
│   ├── clipmgr.c/h              # Clipboard history UI
│   ├── power.c/h                # Power management (shutdown via ACPI, restart, lock)
│   ├── recycle.c/h              # Recycle bin (delete/restore files)
│   ├── widgets.c/h              # Desktop widgets (toggle with Ctrl+I)
│   │
│   │  ═══ APPLICATIONS ═══════════════════════════════════════════════════
│   ├── shell.c/h                # Interactive shell (93 commands, history, pipes, redirects)
│   ├── editor.c/h               # Full-screen text editor
│   ├── notepad.c/h              # GUI notepad with clipboard
│   ├── calculator.c/h           # Calculator with button UI
│   ├── filemgr.c/h              # File manager (browse, open, delete)
│   ├── sysmon.c/h               # System monitor (CPU, memory, processes)
│   ├── taskmgr.c/h              # Task manager (view/kill processes)
│   ├── settings.c/h             # Settings (Theme, System, About tabs)
│   ├── browser.c/h              # Web browser (HTML render, URL nav, history)
│   ├── calendar.c/h             # Calendar (month view, navigation)
│   ├── contacts.c/h             # Contact list manager
│   ├── todo.c/h                 # Todo list (add, check, delete)
│   ├── music.c/h                # Music player (PC speaker, play/stop)
│   ├── paint.c/h                # Paint (freehand drawing canvas)
│   ├── colorpick.c/h            # Color picker
│   ├── hexview.c/h              # Hex file viewer
│   ├── search.c/h               # File search
│   ├── help.c/h                 # Help center
│   ├── sysinfo.c/h              # System information viewer
│   ├── syslog.c/h               # System event logger
│   ├── fileops.c/h              # File operations GUI (copy, move, rename)
│   ├── clock.c/h                # Desktop clock
│   │
│   │  ═══ GAMES ══════════════════════════════════════════════════════════
│   ├── snake.c/h                # Snake game with scoring
│   ├── pong.c/h                 # Pong (two paddles)
│   ├── minesweeper.c/h          # Minesweeper grid game
│   ├── tetris.c/h               # Tetris (falling blocks)
│   │
│   │  ═══ COMPATIBILITY ══════════════════════════════════════════════════
│   ├── x11.c/h                  # X11/Wayland compatibility shim (X11R6 API → NexusOS WM)
│   ├── rshell.c/h               # Remote shell server (port 2323)
│   └── userlib.h                # Userspace library interface
│
├── tools/                        # Build utilities
├── assets/                       # Static assets (wallpapers, etc.)
├── img2wallpaper.py              # Convert images to embedded wallpaper data
├── mkfat32.py                    # Generate FAT32 disk image
├── linker.ld                     # Kernel memory layout (text @ 0x1000)
├── build.bat                     # Windows build script
├── Makefile                      # Linux/macOS build system
└── README.md
```

> **125+ source files** · **350+ total files** · **~460 KB kernel** · Written entirely in C and x86 Assembly

---

## 🔥 Build & Run

### Prerequisites

| Tool | Purpose |
|---|---|
| **NASM** | x86 assembler (compile `.asm` files) |
| **i686-elf-gcc** | Cross-compiler (GCC targeting i386-elf, no hosted libc) |
| **i686-elf-ld** | Cross-linker |
| **QEMU** | `qemu-system-x86_64` for native Aurora; `qemu-system-i386` for legacy |
| **Python 3** | For `mkfat32.py` and `img2wallpaper.py` build tools |

### Build Commands

The prepared native image is required by `run`; missing/unpublished images fail
explicitly. Native source builds use `build.bat x64 build` and
`python -I arch/x86_64/aurora.py build`, followed by verified publication. Stop any
VM using those artifacts before rebuilding. The Makefile targets below remain i386.

```bash
# Windows - Build the legacy i386 kernel/boot image
build.bat

# Build/install legacy i386 Lua, Kilo, shell, file tools and games
build.bat apps

# Launch the published native Aurora desktop (no rebuild)
build.bat run

# Build and launch the original i386 desktop/data disk
build.bat legacy

# Launch the preserved direct Chrome session
build.bat x64 chrome
# (run/debug use `-display sdl`: QEMU's GTK backend on Windows repaints the 1080p
#  window at only 2-4 fps, which looks like mouse lag; SDL is smooth. If you launch
#  QEMU by hand, add -display sdl.)

# Windows - Clean legacy build outputs; preserve data disks
build.bat clean

# Linux / macOS — Build
make

# Linux / macOS — Build and run in QEMU
make run

# Linux / macOS — Clean
make clean
```

### Downloads and storage (legacy i386)

These commands target original `disk.img` and `/disk` paths. Native Chrome
downloads use `/home/nexus/Downloads` on `arch/x86_64/out/desktop64.img`;
`build.bat storage` does not target the native disk.

```text
wget https://dl.google.com/chrome/install/standalonesetup64.exe
wget https://example.com "/disk/Downloads/example.html"
```

Use the browser's Save arrow or Ctrl+S to save any current HTTP(S) URL. Open Downloads
shows the files; Esc/Cancel removes a partial transfer. Saving does not install or
make a Windows/Linux/macOS program compatible with NexusOS.

To enlarge the virtual disk, stop QEMU and run `build.bat storage 8192` (MiB).
Existing files and the fixed persistence region are preserved. Enough host free
space is needed for the actual data. The current ATA driver supports up to 128 GiB;
[FAT32's native per-file bound](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison)
is 4 GiB minus one byte. Removing the old RAM download cap does not remove these
filesystem/hardware bounds.

### QEMU Launch (Manual, legacy i386)

```bash
qemu-system-i386 -cpu max -fda nexus.img -hda disk.img -m 512M -display sdl \
  -vga none -device virtio-vga \
  -device rtl8139,netdev=net0 -netdev user,id=net0 -serial file:serial.log
```

> `-device virtio-vga` enables the Phase 41 GPU-accelerated display path (zero-copy
> scanout presents). With a plain `-vga std` the OS runs identically on the VESA
> framebuffer fallback.

---

## ⚡ Boot Flow

```
┌──────┐    ┌──────────────────┐    ┌──────────────────────┐    ┌──────────────────┐
│ BIOS │───▶│ boot.asm         │───▶│ boot2.asm            │───▶│ kernel_entry.asm │
│      │    │ Stage 1 (512B)   │    │ Stage 2              │    │ ASM → C bridge   │
│      │    │ Load Stage 2     │    │ Set VESA 1920×1080   │    │ Call kernel_main │
│      │    │ via INT 0x13     │    │ Load kernel to 0x1000│    │                  │
│      │    │ @ 0x7C00         │    │ Enter Protected Mode │    │                  │
└──────┘    └──────────────────┘    └──────────────────────┘    └────────┬─────────┘
                                                                         │
┌────────────────────────────────────────────────────────────────────────┘
│
▼ kernel_main() Initialization Sequence:
│
├── 1.  VGA text mode init + boot banner
├── 2.  GDT (Global Descriptor Table)
├── 3.  IDT (Interrupt Descriptor Table, 256 entries)
├── 4.  PIC (remap IRQs to 32–47)
├── 5.  Timer (IRQ0, ~18.2 Hz tick counter)
├── 6.  Keyboard (IRQ1, PS/2 scancode handler)
├── 7.  Mouse (IRQ12, PS/2 mouse handler)
├── 8.  Physical Memory Manager (bitmap, CMOS RAM detection)
├── 9.  Paging (page directory + tables)
├── 10. VESA Framebuffer (1920×1080×32bpp) + Aurora boot splash
├── 11. Heap Allocator (kmalloc/kfree)
├── 12. VFS + RAM Filesystem
├── 13. ATA Disk Driver + FAT32
├── 14. Process Manager + Scheduler
├── 15. System Calls (INT 0x80)
├── 16. ELF Loader + VMM (CoW fork) + Pipes + Env
├── 17. PCI Bus + Driver Framework + Block Devices + Partitions
├── 18. USB controller detection (stub)
├── 19. Network (RTL8139 + ARP + IP + ICMP + UDP + TCP + Sockets)
├── 20. DNS + HTTP Client + Browser
├── 21. DHCP + NTP + HTTP Server + Remote Shell
├── 22. POSIX (procfs + termios + signal translation)
├── 23. Dynamic Linker (shared libs + libc symbols)
├── 24. X11 Compatibility Shim
├── 25. PE Loader + Win32 API Layer + Registry Emulation
├── 26. Package Manager (.npk format + bundled repository)
├── 27. Scripting Engine (NexusScript interpreter + sample script)
├── 28. macOS Shim (Mach-O loader + Core Foundation + Cocoa)
├── 29. Sound (Audio mixer + Intel AC'97 driver + startup.wav)
├── 30. Images (BMP/PNG/JPEG/GIF decoders + viewer + sample images)
├── 31. Video (RIFF/AVI parser + MJPEG media player + demo.avi)
├── 32. RTC + PC Speaker + Boot Sound
│
▼
┌──────────────┐    ┌───────────────┐    ┌──────────────────────────────────┐
│ Login Screen │───▶│ Desktop GUI   │    │ Desktop Environment              │
│ (user/pass)  │    │ (VESA mode)   │    │ ├── Wallpaper + Icons            │
│              │    │     or        │    │ ├── Window Manager (8 windows)   │
│              │    │ Text Shell    │    │ ├── Taskbar + Start Menu         │
│              │    │ (VGA fallback)│    │ ├── Dock + Notification Center   │
│              │    │               │    │ └── 30+ Applications             │
└──────────────┘    └───────────────┘    └──────────────────────────────────┘
```

---

## 🗺️ Roadmap

### Progress: `██████████████████████████████████████████████████` **100%** (51/51 phases complete 🎉)

<details>
<summary><b>✅ Phase 1–13 — Foundation (Complete)</b></summary>

- [x] **Phase 1** — Bare Metal Kernel (boot, GDT, IDT, interrupts)
- [x] **Phase 2** — OS Services (filesystem, multitasking)
- [x] **Phase 3** — Userspace (terminal, text editor, snake game)
- [x] **Phase 4** — GUI (window manager, themes)
- [x] **Phase 5** — Apps (start menu, calculator, file manager)
- [x] **Phase 6** — UX (login, icons, context menus, notifications)
- [x] **Phase 7–13** — Advanced UX, desktop apps, widgets, tools
</details>

<details>
<summary><b>✅ Era 1 — Graphics Revolution (Phase 14–18)</b></summary>

- [x] **Phase 14** — VESA VBE Graphics (1024×768×32bpp, linear framebuffer, double buffering)
- [x] **Phase 15** — Font Rendering Engine (8×8, 8×16 bitmap fonts, anti-aliased text)
- [x] **Phase 16** — Graphics Primitives & Widget Toolkit (lines, rects, circles, gradients, buttons, sliders)
- [x] **Phase 17** — Modern Window Manager (pixel-perfect borders, rounded corners, drop shadows, resize, animations)
- [x] **Phase 18** — Desktop Shell v2 (high-res wallpapers, pixel-perfect icons, taskbar with system tray)
</details>

<details>
<summary><b>✅ Era 2 — OS Foundations (Phase 19–25)</b></summary>

- [x] **Phase 19** — FAT32 Filesystem (read/write, directories, long filenames, persistent storage)
- [x] **Phase 20** — ELF Binary Loader (ELF32, ring-3 userspace, INT 0x80 syscalls, basic libc)
- [x] **Phase 21** — Memory Management v2 (full VMM, page fault handling, CoW fork, shared memory, mmap)
- [x] **Phase 22** — Advanced Process Management (POSIX signals, process groups, pipes, IPC, env vars)
- [x] **Phase 23** — Device Driver Framework (PCI enumeration, plug-and-play, init/probe/remove, IRQ routing)
- [x] **Phase 24** — Storage Stack (ATA/AHCI driver, block device layer, MBR partitioning, multi-mount)
- [x] **Phase 25** — USB Stack (UHCI/OHCI host controller, device enumeration, HID keyboard/mouse, mass storage)
</details>

<details>
<summary><b>✅ Era 3 — Networking (Phase 26–30)</b></summary>

- [x] **Phase 26** — Network Driver (RTL8139 NIC, PCI probe, Ethernet frame TX/RX, MAC, link status, stats)
- [x] **Phase 27** — TCP/IP Stack (ARP cache, IPv4 routing, ICMP ping, UDP sockets, TCP with full state machine)
- [x] **Phase 28** — DNS & HTTP Client (DNS query/response, HTTP/1.1 GET, response parsing, file download)
- [x] **Phase 29** — Web Browser (HTML parser for p/h1-h6/a/ul/li/pre, text rendering, link navigation, history)
- [x] **Phase 30** — Network Services (DHCP auto-config, NTP time sync, HTTP server on :8080, remote shell on :2323)
</details>

<details>
<summary><b>✅ Era 4 — Compatibility Layers (Phase 31–37) — Complete</b></summary>

- [x] **Phase 31** — POSIX Compatibility Layer (syscall translation, /proc filesystem, signals, termios, Unix tools)
- [x] **Phase 32** — ELF Dynamic Linking (shared libraries .so, dynamic linker ld.so, dlopen/dlsym, 26+ libc symbols)
- [x] **Phase 33** — X11/Wayland Shim (X11R6 API, display/window/GC/drawing/events, 16 windows, 32 GCs)
- [x] **Phase 34** — Win32 Compatibility Layer (PE32 loader, Win32 API shim — 47 functions, registry emulation, runexe/regedit/win32info commands)
- [x] **Phase 35** — Package Manager (.npk archive format with CRC32 integrity, bundled repository, recursive dependency resolution, `npkg list/search/info/install/remove/installed/update`)
- [x] **Phase 36** — Scripting Engine (NexusScript custom interpreter — variables, expressions, `if`/`while`, builtins, `run` for OS automation, step-budget safety, `script <file.ns>`)
- [x] **Phase 37** — macOS Compatibility Shim (Mach-O i386 loader, Core Foundation reference-counted objects, Cocoa `NSWindow` bridged to the WM, `machoinfo`/`runmacho`/`cocoademo`)
</details>

<details>
<summary><b>🔨 Era 5 — Multimedia (Phase 38–42)</b></summary>

- [x] **Phase 38** — Sound Driver (Intel AC'97 bus-master PCM-out, software mixer with 8 voices + resampling + volume, WAV player, tone generator, `sndinfo`/`play`/`volume`/`tone`/`mixer` commands)
- [x] **Phase 39** — Image Formats (BMP, PNG with from-scratch DEFLATE inflater + filters, baseline JPEG with integer IDCT, animated GIF with LZW; magic-byte detection, fullscreen viewer, `imginfo`/`view` commands)
- [x] **Phase 40** — Video Playback (RIFF/AVI parser, Motion-JPEG frame decode + render, PCM audio synced off the audio clock, fullscreen media player, `vidinfo`/`mplay` commands, embedded `demo.avi`)
- [x] **Phase 41** — GPU Acceleration (VirtIO-GPU driver over a from-scratch VirtIO 1.0 PCI transport, zero-copy scanout presents — 4,620 fps vs 107 fps VESA, dirty rectangles, rep-string fill/blit, 32-sprite alpha-blending engine, `gpuinfo`/`gpubench`/`sprites` commands)
- [x] **Phase 42** — Gaming Framework (NexusSDL 256-color game API with Q16.16 math kit, 12-button virtual game pad from raw PS/2 scancodes, tick-locked timing, NEXUSDOOM — a from-scratch textured raycaster FPS with z-tested billboard demons — plus Breakout; chunked high-load bootloader lifts the 576 KB kernel ceiling, kernel now links at 1 MB)
</details>

<details>
<summary><b>Era 6 — Polish & Superiority (Phase 43–48)</b></summary>

- [x] **Phase 43** — Accessibility (PC-speaker screen reader with per-event earcons and pitch-mapped text spell-out, high-contrast theme with a plain-black desktop field, global 1×–4× UI font scaling, and global Alt+Shift hotkeys via a chained raw-scancode hook; `accinfo`/`fontsize`/`contrast`/`reader`/`say` commands) ✅
- [x] **Phase 44** — Security (real login auth with salted password hashing, Unix-style rwx file permissions + ownership with `chmod`/`chown`/`ls -l`, cooperative process isolation via per-process uid + kill permission checks, and a stateless packet-filter firewall hooked into the IP layer; `id`/`users`/`passwd`/`useradd`/`firewall` commands) ✅
- [x] **Phase 45** — Cloud & Sync (from-scratch **RFB 3.3 VNC remote-desktop server** streaming the live 1024×768 framebuffer with keyboard + pointer injection; **clipboard sync** both ways over the VNC channel; a **cloud file-sync** protocol with LIST/GET/PUT over TCP; and **settings sync** that serializes theme/font-scale/accessibility/wallpaper to a `settings.cfg` that rides the sync channel; `vnc`/`sync`/`synccfg`/`clipsync` commands) ✅
- [x] **Phase 46** — AI Assistant (an **offline, rule-based** assistant — no internet model: `ask "<plain English>"` scores the request against an intent table, extracts an argument, and runs the matching command via the shell — with a **destructive-command guard** that refuses to auto-run `rm`/`kill`/etc.; `ai [cmd]` explains commands from an embedded knowledge base; `find <query>` does **smart file search** over names *and* contents with snippets; Tab **autocomplete** in the editor against keywords + buffer words) ✅
- [x] **Phase 47** — Mobile/Embedded Mode (a **touch gesture layer** that turns the PS/2 mouse into tap / long-press / drag / swipe-up/down/left/right; a **responsive UI** logical-orientation model — `orientation portrait` swaps the logical screen to 768×1024 for layout-aware code; a real **low-power mode** that stretches the idle redraw cadence ~4× so the CPU stays in HLT; and an honest **ARM/embedded status** report of what a cross-compiled port would require; `gesture`/`orientation`/`lowpower`/`arm`/`mobileinfo` commands) ✅
- [x] **Phase 48** — Performance Optimization (rep-string `memcpy`/`memset` — **~2.9 GB/s copy, ~3.2 GB/s fill**; **boot-time measurement** — init completes in ~3 ticks / **~165 ms, under 1 second**; a **boot-init IRQ guard** that defers heavy interrupt-time work until init finishes, closing the window on the long-standing intermittent boot fault; `perf` benchmark command; and **honest SMP / preemption status** — the kernel is single-CPU cooperative by design, with `smp`/`preempt` documenting what a real implementation needs rather than faking it) ✅ — **Era 6 complete**
</details>

<details>
<summary><b>Era 7 — World Domination (Phase 49–51)</b></summary>

- [x] **Phase 49** — App Store & Ecosystem (`store`: catalog/ratings/featured, version-check auto-updates, developer SDK scaffold, honest cooperative app sandboxing — all layered over the package manager)
- [x] **Phase 50** — v5.0 Grand Finale (`urun` universal binary launcher PE/ELF/Mach-O; footprint report card — kernel 688 KB < 1 MB, RAM 1.7 MB < 16 MB, both measured & PASS; real MBR disk installer verified on the IDE disk; bootable `nexus.iso` via El Torito, boots in QEMU)
- [x] **Phase 51** — NPFS (NexusOS Persistent File System) — from-scratch **journaling** FS with a write-ahead log + CRC32 commit records and **crash recovery** (verified via `npfs crashtest`); inodes with direct+single-indirect blocks; 512 B block-aligned I/O; **persists across reboot** (verified — files + mount counter survive a power cycle). `npfs format|mount|ls|write|cat|rm|stat|journal|crashtest`
</details>

---

## 📊 Growth Projection

| Phase | Apps | Source Files | Kernel Size | Key Milestone |
|:---:|:---:|:---:|:---:|---|
| 1–13 | 30 | 90 | 140 KB | VGA text-mode OS with shell & apps |
| 14–18 | 35 | 120 | 250 KB | **Pixel graphics — 393× visual improvement** |
| 19–25 | 40 | 180 | 400 KB | USB, persistent disk, ELF binaries |
| 26–30 | 45 | 230 | 600 KB | **Internet — browse the web from scratch** |
| 31–37 | 50 | 360+ | ~460 KB | POSIX, X11, dynamic linking, Win32, package manager, scripting, macOS shim |
| 38–42 | 60 | 420 | 900 KB | **Current ⭐ (Phases 38–40 done)** — Sound (AC'97), images, video (MJPEG) — Doom runs |
| 43–48 | 65 | 480 | 950 KB | Security, cloud sync, multi-core |
| 49–51 | 70+ | 500+ | < 1 MB | 🌍 **App store, ISO installer, world domination** |

---

## 📜 License

**NexusOS Proprietary License — All Rights Reserved.** Copyright © 2026 [rajvveer](https://github.com/rajvveer).

NexusOS is **source-available, not open source**. You may view the code on GitHub and read it for private study.
Without written permission from the copyright holder you may **not**:

- copy, mirror, modify or create derivative works of NexusOS;
- redistribute, publish, sell or sublicense it;
- use it in or as any product, service, OS distribution or website;
- rebrand it, present it as your own work, or launch it under another name;
- remove copyright or author notices, or use the NexusOS name, logo or screenshots to promote anything.

For permissions or commercial licensing, contact [rajvveer on GitHub](https://github.com/rajvveer).
Third-party components (uACPI, Lua, kilo, Picolibc, the Aileron font, Realtek firmware, Wayland protocol files and
the certifi root certificates) keep their own licenses; the full terms and the list are in [LICENSE](../LICENSE).

---

<p align="center">
  <b>NexusOS</b> — Built from scratch. No compromises. 🔥
  <br><br>
  <i>"From 2,000 character cells to 786,432 pixels — a 393× improvement."</i>
  <br><br>
  <sub>140+ source files · 109 shell commands · 30+ applications · 4 themes · Full TCP/IP stack · Win32 + macOS compatibility · package manager · scripting engine · AC'97 audio · BMP/PNG/JPEG/GIF images · AVI/MJPEG video · VirtIO-GPU acceleration · NexusSDL gaming framework + NEXUSDOOM · All in ~600 KB</sub>
</p>
