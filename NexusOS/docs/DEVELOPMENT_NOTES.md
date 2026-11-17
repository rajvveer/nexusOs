# NexusOS handoff - native Aurora, Chrome, Windows programs and preserved legacy environment (2026-10-14)

Previous handoffs are preserved at `handoff/notes/HANDOFF_BEFORE_DOWNLOADS.md`
and `handoff/notes/HANDOFF_BEFORE_DAILYUSE.md`. The ws_* trees are historical snapshots;
**do not apply their merge recipe again**.

## Production round: published 2026-10-30 (read this first)

Eight areas were built in parallel worktrees, 3-way merged (m1..m9) and verified, then published into this tree. The
sections below this one are the per-area handoffs (power, sec, mem, net, storage, desk, pkg, hw); agents' full reports
are in `C:\nexustest\prod\reports\*.md`, the integrator's merge record in `C:\nexustest\prod\integrator_notes.md`.
- **Areas**: sec (seccomp, ASLR/KASLR, SMEP/UMIP, W^X, Yama, namespaces, Secure Boot chain, browser sandboxes), mem (swap,
  reclaim, OOM killer, block I/O without the big lock, orphan-file page fix), power (uACPI AML, S3, hibernation,
  cpufreq/cpuidle, Aurora power UI), net (IPv6, nftables, WireGuard, TUN, packet sockets, network tools + Network app),
  storage (dm-crypt/LUKS2, USB storage/boot/install, exFAT/ISO/loop/FUSE, automount), desk (displays/HiDPI, window
  management, lock screen + greeter + Settings > Users, CJK/emoji fonts, a11y, IME), pkg (nexusd services, D-Bus, cron,
  chrony, CUPS printing, Debian base + apt + Software app on ext4 installs, A/B core updates, FAT->ext4 migrate, Wine
  Mono/Gecko), hw (NIC/HDA/NVMe/AHCI/xHCI driver work, audio capture, UEFI GOP, MT7922 firmware loader, diagnostics).
- **Verification of the merged tree** (m9, sequential, one VM at a time): full regression 71/73 (the 2 failures = one
  host-load audio glitch, see below), audio x3 6/6, desk 11/11, Aurora power 1/1, swap + Chrome under memory pressure
  2/2, Firefox apps 4/4, pkg 6/6 + Debian-root password 1/1; after publish: build 0 warnings, smoke 5/5 on this tree.
- **Kernel size**: ~1.83 MB; the BIOS boot image is a 2.88 MB floppy (cap 2,946,560 bytes, `floppy288.py` logic in build.py).
- **Swap is opt-in** for the installed disk: `build.py desktop|chrome --swap` creates the default `/swapfile` once;
  without `--swap` the launcher never writes the disk (`--no-swap` disables an existing one for one boot).
- **Secure Boot private keys** live in `%USERPROFILE%\.nexusos\keys\secureboot` (override `SECUREBOOT_KEYDIR`); only
  public certificates are in the tree. Back that folder up: without it no new signed kernel can be made.
- **The user's desktop disk** (`out\desktop64.img`, FAT32) got: desk fonts + libcrypt (`desk_install --only fonts,auth`),
  Wine Mono 9.4.0 + Gecko 2.47.4, the patched winewayland.so (minimize), `/sbin/init` + `/bin/aurora-app` +
  `/bin/xdg-open` from this tree's build, a prebuilt fontconfig cache. Checked before it replaced the disk: user files
  unchanged, FAT consistent, Aurora READY (no greeter: no password set), Chrome session maps its window.
  apt / services / Software need an ext4 install (`sysinstall.py migrate`), not done on the user's disk.
- **Rollback**: sources `python -I C:\nexustest\prod\publish_prod.py rollback` (backup `C:\nexustest\prod\publish_backup_prod`);
  disk `python -I C:\nexustest\prod\publish_disk_prod.py rollback` (backup `D:\nexustest\prod\publish_backup_prod_disk`,
  SHA-256 before b0d88c3a...0870, after cacb119c...635e).
- **Known open items** (consolidated at the wave-2 merge, 2026-11-06; fixed in wave 2: firefox-esr, POSIX timers,
  timerfd/signalfd, the journal flood, SMAP default-on, the FAT lost-cluster cause, fatput rmdir, workspace paths in the
  scripts pkg2 converted, Notepad++ x86 under Wine - see the wave-2 sections below):
  the `test_x64_hda` click is QEMU's timer-mode hda-codec overrun, attributed by the test (hw2 section; interleaved on the merged tree 2026-11-12: test_x64_hda 4/4 vs base 4/4, test_x64_audio 4/4 vs base 4/4);
  **security**: Secure Boot signs KERNEL.BIN only - the command line (EFI LoadOptions, `\EFI\NEXUS\CMDLINE.TXT`) is not
  signed, so ESP access can set `lockdown=none`, `nosmap`, `roothash=`, `init=` (needs a signed/built-in command line or
  an allowlist under Secure Boot); the kernel has no entropy source besides RDRAND (Bluetooth SC pairing, getrandom on
  CPUs without it); KASLR has no boot-time slide; the desktop owner (uid 1000) still administers the network implicitly
  (netlink.c `desktop_owner`): moving the Network panel and the Wi-Fi supplicant behind nexus-polkitd needs a root
  network service first (FAT32 roots have no set-ID and no root process);
  **desktop**: logind InteractiveAuthorization through the broker, the ScreenCast portal (PipeWire; timerfd/signalfd/
  timer_create exist now), X11 PRIMARY selection (zwp_primary_selection_v1), an Aurora Wi-Fi tab screenshot test;
  Wine 10.0's winewayland has no wl_data_device (no clipboard/drag and drop for Wayland Wine windows) and HTAs cannot
  create unsafe ActiveX objects (FSO);
  **kernel/ABI**: POSIX message queues, statmount/listmount, io_uring, rseq (deliberate); tickless idle still wakes for
  network housekeeping (4/s), kswapd (1/s) and a 1 s cap (power2 section); USB autosuspend, battery charge thresholds
  (no EC/WMI), wake-on-LAN (QEMU NICs);
  **hardware**: MT7922 Wi-Fi (`struct wifi_driver_ops`, `arch/x86_64/wifi.h`) and btmtk firmware, Bluetooth SCO/ISO
  audio, HID host/uhid, rfkill, LE privacy, SDL2/ffmpeg application checks; every driver still unverified on real hardware;
  **size**: kernel.bin is 2,768,896 of the 2,946,560-byte floppy cap (177,664 left, 94% used): the next wave needs a
  compressed kernel image or a larger cap;
  **workspace**: new wave-2 tests and installers again name `D:\nexustest` paths (32 tests; bluez/wifi caches default to
  the agents' folders `D:\nexustest\wave2\ws_hw2_pkgs` / `ws_net2_pkgs`): move them to nexus_paths + shared read-only
  caches like `D:\nexustest\pkgs\storage2`;
  **the user's disk** (publish steps, with a backup): its 6,576 lost FAT clusters from before wave 2 (`userland/fatfsck.py -a`
  or one boot with `fsck.mode=force`), pkg2's patched winewayland.so (`winewayland_build.py install`), fcitx5 optional
  (`ime_install.py`, 55 MiB).

## Wave 2 merge (integ2, 2026-11-06): eight worktrees in one tree

The eight wave-2 worktrees (kabi, sec2, power2, net2, hw2 + bt, stor2, pkg2, desk2 + xwm) were 3-way merged against the
2026-10-30 published tree, one side per step in that order; each side's own section follows this one unchanged except
where a bracketed "[wave-2 merge: ...]" note says what the merge changed. Record: `C:\nexustest\wave2\integrator_notes_wave2.md`
(every conflict and its resolution), scripts `C:\nexustest\wave2\integ_scripts\integ2_*.py`.
- **Seams resolved while merging** (details in the notes): UEFI loader = kabi's 6 MiB KERNEL_LIMIT + bt's
  `allocate_outside` (probes released) + pkg2's `nexus.secureboot=1`; SMAP (sec2, default on) applied to the other
  sides' new user accesses (kabi's get_user/put_user sites, net2's multicast filter copies and SO_PASSCRED, stor2's md
  ioctl copies); the _ALARM wake seam (kabi) moved onto power2's RTC-clock wakealarm; power2's `sound_next_event` loops
  over hw2's sound cards and uses desk2's clamped wake threshold; one `lapic_reg_read/write` (hw2's, over apic.c);
  preadv/pwritev(2) once (kabi's); MAP_POPULATE and the sysfs statfs magic once; stor2's dm/md/LVM/verity boot order
  before the hibernation resume (power2 re-parses resume= after the unlock); BlueZ runs without tests/bt_shim.c (kabi's
  signalfd/timerfd).
- **O_PATH on /sys (hw2) and lsblk**: with O_PATH working, libudev finds block devices and util-linux lsblk takes
  FSTYPE/LABEL/UUID/PART* from it (sd-device reads the sysfs `uevent` file + the udev database; no udevd runs here).
  The block devices' sysfs `uevent` file now also carries what udev's blkid builtin would store (fsprobe.c
  `fsprobe_udev_properties`: ID_FS_TYPE/USAGE/UUID(_ENC)/LABEL(_ENC), ID_PART_ENTRY_SCHEME/UUID/NAME/TYPE/NUMBER,
  ID_PART_TABLE_TYPE, ID_BUS=usb; never probing a suspended dm device), as the kernel's own block uevents already did.
- **USB stick / installed-disk BIOS boot partition is 8 MiB now** (`usbimage.py BIOS_SECTORS = 16384`, was 4096 = 2 MiB;
  ESP at LBA 18432): the merged kernel.bin no longer fits 2 MiB. Sticks made before, and disks installed from them, keep
  the 2 MiB partition: they need a new stick + reinstall, or a UEFI boot (nexus-update's A/B slots are UEFI).
- **Authorization broker installed**: `sysinstall.py services --aurora` installs /usr/bin/pkexec (04755) and
  /usr/libexec/nexus-polkitd; `services/nexus-polkitd.service` starts the broker as root; POLICY[] knows
  `org.nexus.accounts.manage` (Settings > Users). NOT done: dropping the desktop owner's implicit network
  administration (netlink.c `desktop_owner` in `netlink_admin`) and moving the Network panel behind the broker - on
  FAT32 roots there is no set-ID and no root process, so it needs a root network service + a broker/pkexec Apply path
  first (see Known open items).
- **Storage tool cache**: `D:\nexustest\pkgs\storage2` (read only) holds every .deb of the merged storage-lock.json not in
  the apps cache; this host's `%USERPROFILE%\.nexusos\paths.json` `NEXUS_STORAGE_PKGS` points there.
- **Post-merge fixes** (2026-11-12/14, found by the final chain; record `C:\nexustest\wave2\w2fix_notes.md`): a kernel
  race - kabi's `space_exit_mappings()` in `process.c terminate()` and `kernel.c fd_release()` (SCM_RIGHTS dropped by an
  I/O-free recvmsg) did filesystem work (shared-map write-back, orphan disposal) without `block_io_barrier()` when the task
  was inside an I/O-free call, seen once as an ext4 "double free of an inode" at shutdown; both now take the barrier.
  nft.c read `a[17]` past its 16-entry attribute table (now `a[18]`, max 17). Test seams: kabi_debian, firefox_esr, mdns
  and upower pass pkg2's per-run hook port (`hook_port`, `@HOST@`); update_sb picks TCG for its Secure Boot (SMM) case
  (`pkg_vm.py` auto -> TCG with `uefi_pflash`); aurora_portal looks the Appearance row up in `aurora_settings.c pages[]`;
  test_x64_upgrade waits for 40 s without disk writes before its power-failure edge; the w2pkg upgrade spec uses the
  Wine-carrying integ disk.
- **Kernel size** (kernel.bin; floppy cap 2,946,560 = build.py KERNEL_MAX): 2,768,896 bytes (177,664 left); the UEFI
  loader's KERNEL_LIMIT is 6 MiB and new USB sticks / installs have an 8 MiB BIOS boot partition.
- **Verification** (QEMU, sequential, final kernel with the post-merge fixes, 2026-11-14): full + storage + mem + w2stor
  51/51 (osinstall passed on its rerun after a host-load timeout), power net sec hw power_hw 42/42 (test_x64_acpi's
  cpufreq-powersave read race and test_x64_suspend's hibernate-swap-squeeze precondition passed on their reruns),
  w2nodisk (w2kabi w2sec w2net w2stor w2hw w2power w2desk without the pkg-base tests) 46/46, fdrace smp 1+2 8/8, nettools
  firewall nat 6/6, desk 11/11 + w2desk 6/6, power_aurora 1/1, mem_disk 2/2, wine 2/2, the pkg base (services 98) + its 10
  tests 10/10, test_x64_upgrade 49/49 checks; audio interleaved against the base on 2026-11-12 (see Known open items).

## Linux ABI completeness: POSIX timers, timerfd, signalfd, pidfd/clone3, System V IPC, Landlock, new mount API, firefox-esr, fcitx5 (wave 2, kabi, 2026-10-30)

New kernel files (arch/x86_64): `kslab.c` (small-object slab: timers, signal queue entries, signalfd/pidfd objects),
`posix_timers.c`, `signalfd.c`, `pidfd.c`, `kabi.c`, and in the follow-up `sysvipc.c`, `landlock.c`, `mountapi.c`,
`fdinfo.c`; test programs `kabi_user.c` (`/bin/kabi64`) and `kabi2_user.c` (`/bin/kabi2`); tools `ime_install.py` +
`ime-lock.json`; tests `test_x64_kabi.py`, `test_x64_firefox_esr.py`, `test_aurora_fcitx5.py`, `test_x64_kabi_debian.py`.
Hooks in shared files are small: kernel.c (dispatch cases, CPU/TAI clocks in clock_ns, argument narrowing around the
switch, statfs sysfs magic, `/nexus/test-kabi`), process.c (exit_signal + __WALL/__WCLONE, clone3 frame + CLONE_PIDFD +
CLONE_CLEAR_SIGHAND in task_clone, timer/pidfd hooks at exec/exit/reap/expiry, a helper block at the end), signal.c
(slab queue entries with a timer cookie, enqueue/dequeue helpers), timekeeping.c (clock steps), kmsg.c, mount.c,
device.c, native.h (one block at the end), linker.ld, uefi/bootx64.c, build.py.
- **POSIX timers** (timer_create/settime/gettime/getoverrun/delete): REALTIME (absolute ones re-evaluated on every clock
  step), MONOTONIC, BOOTTIME, the _ALARM clocks (CAP_WAKE_ALARM; they do not wake a sleeping machine), TAI, the CPU clocks
  (process, thread, and other threads/processes by clock_getcpuclockid/pthread_getcpuclockid ids, which clock_gettime now
  reads too); SIGEV_SIGNAL/THREAD/THREAD_ID/NONE; si_code SI_TIMER, si_timerid, si_value, overruns (one queued signal,
  later expirations counted), re-arm/delete take a queued signal back; deleted at exec and exit, not inherited; at most
  RLIMIT_SIGPENDING timers per process (EAGAIN) and 65536 in all. ITIMER_VIRTUAL/ITIMER_PROF run on process CPU time.
  One engine: an intrusive pairing heap driven by `next_deadline` from trap64's timer interrupt (1 ms granularity).
- **timerfd** (TFD_TIMER_ABSTIME, TFD_TIMER_CANCEL_ON_SET -> ECANCELED after clock_settime/settimeofday/ADJ_SETOFFSET/
  resume, tick counts, TFD_IOC_SET_TICKS, poll/epoll), **signalfd/signalfd4** (Linux signalfd_siginfo layout per signal
  kind, readiness in the reader's context, mask updates, SFD_NONBLOCK/CLOEXEC), **rt_sigtimedwait**, **rt_sigqueueinfo /
  rt_tgsigqueueinfo** (forged kernel/kill origins only for oneself).
- **pidfd_open / pidfd_send_signal / pidfd_getfd / waitid(P_PIDFD)**: POLLIN when the process exited, POLLHUP once reaped;
  PIDFD_NONBLOCK -> EAGAIN from waitid; pidfd_getfd needs ptrace attach rights (Yama). **clone3**: CLONE_PIDFD (also
  legacy clone), exit_signal (0 or any signal; such "clone children" are waited for with __WALL/__WCLONE, as Linux),
  stack + stack_size, tls, CLONE_CLEAR_SIGHAND, every Linux argument check (E2BIG for non-zero tails). glibc 2.41 now
  creates every thread and posix_spawn child through clone3. set_tid: EPERM unprivileged, else EINVAL (pid numbers here
  only grow); CLONE_INTO_CGROUP: EBADF (no cgroup2); PIDFD_THREAD: EINVAL.
- **quotactl/quotactl_fd** answer as Linux does without quotas (ENOENT/ENOTBLK/ENODEV, vfat ENOSYS, ext4 ESRCH/EPERM per
  command); **getcpu**; **pkey_alloc/free/mprotect** (no PKU: ENOSPC, keys -1/0 = mprotect); **set/get_mempolicy, mbind**
  (one node, mode kept per thread); **ioprio_set/get** (kept per thread, inherited; not used by the block layer);
  **epoll_pwait2** (timespec timeout rounded up to the 1 ms tick; fcitx5's sd-event waits with it); **mmap
  MAP_POPULATE / MAP_NONBLOCK** accepted (pages still fault in on demand; libime maps its language model with them);
  vDSO **getcpu** reads the CPU number from RDTSCP's TSC_AUX when the CPU has RDTSCP (else the syscall).
- **rseq stays ENOSYS on purpose** (not logged): glibc >= 2.35 then sets __rseq_size = 0 and sched_getcpu() uses getcpu;
  /bin/kabi64 checks that. A registered rseq would need cpu_id updates and critical-section aborts on the lockless
  preemption return path.
- **32-bit syscall arguments** (kabi.c `narrow_table`, applied in native_dispatch, registers restored afterwards): an
  `int`/`unsigned int` argument ignores the register's upper half, as Linux's SYSCALL_DEFINE casts do. This was the
  firefox-esr crash: `memfd_create(name, MFD_CLOEXEC|MFD_ALLOW_SEALING|MFD_NOEXEC_SEAL)` arrived as 0x1_0000000B,
  NexusOS answered EINVAL, Firefox MOZ_CRASHed (the NULL write). Numbers (fds, pids, signals, timeouts) are sign-extended
  exactly as glibc passes them; flag sets zero-extended (so __WCLONE, bit 31, now works in wait4/waitid). futex's op is
  narrowed too (an upper half made it EINVAL), and FUTEX_WAIT now answers EAGAIN for a changed word before ETIMEDOUT for
  an expired timeout, as Linux.
- **Debian firefox-esr** (153.6 from apt on an ext4 install) runs: `--version` and `--headless --screenshot` of a page.
- **fcitx5 input methods** (Debian 13: fcitx5 + chinese-addons Pinyin, anthy, hangul; `arch/x86_64/ime_install.py`, 43
  .debs / 55 MiB pinned in `ime-lock.json` with the signed InRelease; userland LGPL/GPL packages, copyright files
  installed): fcitx5 is Aurora's zwp_input_method_v2 client (`~/.aurora/input-method.conf`:
  `program=/usr/libexec/nexus-fcitx5`, fcitx5 without its D-Bus/X11/XIM addons, system defaults in /etc/xdg/fcitx5:
  Pinyin active, Ctrl+Shift+F9 cycles Pinyin/Anthy/Hangul, Ctrl+Shift+F10 plain keyboard). Pinyin, Anthy and Hangul
  text commits into Firefox (GTK 3, text-input-v3) and Chrome (Ozone/Wayland `--enable-wayland-ime
  --wayland-text-input-version=3`). Root causes on the way: epoll_pwait2 (ENOSYS), MAP_POPULATE (EINVAL: the pinyin
  addon did not load), and **GTK 3's IM module cache**: Debian's libgtk-3-0t64 postinst writes
  `/usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0/immodules.cache`; our disk installers never ran it, so every GTK 3 application
  used the built-in simple context and never bound text-input-v3. The IME installer writes the im-wayland entry (an
  existing cache is kept). No Aurora change was needed. Mozc is not installed (Anthy covers Japanese; Mozc would add
  mozc-server and ~100 MB).
- **System V IPC** (`sysvipc.c`, per IPC namespace): semget/semop/semtimedop/semctl (all operations or none, in order,
  ERANGE above SEMVMX; SEM_UNDO kept per process and set, applied at exit; IPC_RMID wakes sleepers with EIDRM;
  GETNCNT/GETZCNT/GETPID, SETVAL/SETALL clear the undo values, IPC_STAT/IPC_SET/SEM_STAT(_ANY)/SEM_INFO/IPC_INFO),
  msgget/msgsnd/msgrcv/msgctl (types 0, >0, <0 and MSG_EXCEPT, MSG_NOERROR, MSG_COPY, msg_qbytes bounds bytes and
  messages, blocking both ways), shmget/shmat/shmdt/shmctl (a shared-memory file mapped MAP_SHARED; shm_nattch counts
  the mappings, forks included; SHM_RND/SHM_REMAP/SHM_RDONLY/SHM_EXEC; removed while attached = SHM_DEST, key
  IPC_PRIVATE, gone after the last detach; /proc/PID/maps "/SYSVkkkkkkkk (deleted)"), Linux's structures, ids
  (seq << 15 | index), ipcperms with CAP_IPC_OWNER, owner/creator or CAP_SYS_ADMIN for IPC_SET/IPC_RMID.
  /proc/sysvipc/{sem,msg,shm} and the sysctls kernel.sem, msgmax, msgmnb, msgmni, shmmax, shmall, shmmni,
  shm_rmid_forced (Linux formats and ranges) show and set the caller's IPC namespace; a namespace takes its objects
  with it. A process's mappings now go before waitpid can see its exit (Linux exit_mm), so nattch is exact then.
  Debian: util-linux ipcmk/ipcs/ipcrm, libdevmapper's udev synchronisation (dmsetup cookies are semaphores again;
  it had disabled itself on ENOSYS) and PostgreSQL 17 (its System V shared memory interlock) run.
- **Landlock ABI v4** (`landlock.c`): landlock_create_ruleset (version query, size/E2BIG/ENOMSG rules),
  landlock_add_rule (PATH_BENEATH, NET_PORT; EBADFD/EINVAL as Linux), landlock_restrict_self (no_new_privs or
  CAP_SYS_ADMIN, 16 layers, E2BIG). Domains are per thread, inherited by every thread/process created afterwards, kept
  over exec; ptrace and the PTRACE_MODE_READ /proc files reach only the same or a nested domain. native_dispatch asks
  landlock_check (only while some thread has a domain) before open/openat/openat2/creat (READ_FILE, WRITE_FILE,
  READ_DIR, TRUNCATE for O_TRUNC, MAKE_REG to create), execve(at) (EXECUTE), mkdir, mknod, symlink, unlink, rmdir,
  link, rename (REFER: reparenting needs it on both sides and must not gain rights, EXDEV after EACCES), truncate,
  ftruncate (the layers that existed when the file was opened), bind of a path AF_UNIX socket (MAKE_SOCK) and TCP
  bind/connect by port. Works under seccomp filters; Chrome's sandbox (which probes Landlock) still passes.
- **The new mount API** (`mountapi.c`): fsopen/fsconfig/fsmount/move_mount/open_tree/fspick/mount_setattr with Linux's
  argument checks and phases (EBUSY), legacy-context parameters (SET_FLAG/SET_STRING; BINARY/PATH/FD EOPNOTSUPP),
  error messages read from the context fd; FSCONFIG_CMD_CREATE holds the source block device until move_mount (a loop
  device set up with autoclear by `mount -o loop` stays bound after libmount closes it, as Linux's CMD_CREATE does). util-linux libmount (2.41) switches to it once fsopen exists: `mount
  --bind`, `remount,bind,ro`, `mount -o loop` of a vfat image go through open_tree/mount_setattr/move_mount and
  fsopen/fsconfig/fsmount/move_mount. Read-only bind mounts of root-filesystem directories are now enforced (they
  were not, with mount(2) either).
- **RLIMIT_SIGPENDING for every queued signal** (`signalfd.c` sigpending_*): counted per real uid of the receiver
  (timers: the creator) against its soft limit, as Linux __sigqueue_alloc: kill() and kernel standard signals
  override; over the limit a signal is queued without its information (SI_USER, pid/uid 0, once per number), and a
  real-time signal sent with information (sigqueue, tgkill, pidfd_send_signal) fails with EAGAIN; POSIX timers draw
  on the same budget.
- **/proc/PID/fdinfo in Linux's format** (`fdinfo.c`): eventfd (eventfd-count/-id/-semaphore), epoll (tfd lines with
  events, data, pos, ino, sdev), timerfd (clockid, ticks, settime flags, it_value, it_interval), signalfd (sigmask),
  pidfd (Pid, NSpid; -1 once reaped), inotify (wd, ino, sdev, mask and the name_to_handle_at handle).
- **preadv/pwritev/preadv2/pwritev2** (`kabi.c`; were ENOSYS: PostgreSQL writes its WAL with pwritev): offset pos_l,
  -1 = the file position for the *v2 calls, RWF_HIPRI/DSYNC/SYNC/NOWAIT (DSYNC/SYNC write the data out), others
  EOPNOTSUPP. **PR_SET_PDEATHSIG / PR_GET_PDEATHSIG** (were "Unsupported prctl"; PostgreSQL's children treat that as
  FATAL): per thread, sent to the process when its parent process is gone (reparenting), not inherited by fork.
  mmap(MAP_HUGETLB) answers ENOMEM as Linux without huge pages (was EINVAL).
- **_ALARM clocks wake the machine**: at suspend the earliest armed REALTIME_ALARM/BOOTTIME_ALARM timer is programmed
  into the RTC alarm (posix_timers.c pm hook "alarmtimer"; a suspend with one due within 2 s is refused with EBUSY, as
  Linux); relative REALTIME_ALARM timers run on wall time. Seam with power.c: `power_alarm_suspend(seconds)` arms the
  RTC unless the user's wakealarm (sysfs / /dev/rtc0) comes first, `power_alarm_resume()` puts the user's alarm back.
  QEMU wakes from S3 on it; suspend-to-idle needs an SCI the QEMU RTC alarm does not raise.
- **GTK IM module caches** (ime_install.py): immodules.cache lists every GTK 3 module present (im-wayland, im-fcitx5 of
  fcitx5-frontend-gtk3, im-ibus, ...): the GtkIMContextInfo records are read from each module's ELF file, which is what
  gtk-query-immodules-3.0 prints (`ime_install.py gtk-selftest <libgtk-3-0t64.deb>` checks all 13 modules); with
  GTK 4 and loadable GTK 4 IM modules a giomodule.cache is written too (GTK 4's Wayland context is built in). No desk
  disk carries GTK 4 today.
- **Kernel trace records** (EXEC_ARGS, INTERPRETER, PROCESS_EXIT, USER_EXIT, EXIT_STACK/IMAGE, OPEN_MISS, IPC errors,
  MEMSTAT/KSTAT/IOSTAT, the FAULT_REGS/AREA/MEM dumps, LAST_SYSCALL, SBXTRACE, mmap/mprotect errors) stay on the serial
  console but no longer reach /dev/kmsg, dmesg or the journal; boot option `kmsg.trace` puts them there as KERN_DEBUG.
  USER_FAULT, FAULT_IMAGE, CALLER_IMAGE and ENOSYS lines are unchanged.
- **/proc/mounts, mountinfo, statfs**: an ext4 root shows its real device (was /dev/root); stat of /dev/pts and /dev/shm
  carries the devpts/tmpfs device mountinfo names (0:11, 0:10), so findmnt/df/systemd match; statfs on /sys returns
  SYSFS_MAGIC (was ENOSYS).
- **Kernel image + BSS may now use 6 MiB** (was 4): text/rodata stay asserted in the 4 KiB-protected first 4 MiB; BSS
  beyond 4 MiB lies in boot_pd[2] (a 2 MiB writable, NX page), the UEFI loader's KERNEL_LIMIT and build.py's check moved
  with linker.ld. Verified with a temporary 1.5 MiB BSS buffer reaching 5.24 MiB: test_x64 (BIOS, ISO) and
  test_x64_uefi (OVMF q35/pc, TCG/WHPX) passed. Headroom now ~2.25 MiB.

### Using it / tests
- `python -I tests\test_x64_kabi.py WORK [--only smp1,smp2,smp4,ext4]`: /bin/kabi64 (as the user, then the clock-step
  part as root) on 1, 2 and 4 CPUs on FAT and on 2 CPUs on an ext4 root, hostile arguments included; then every timer
  object and page must be reclaimed.
- `python -I C:\nexustest\wave2\pkgvm_lock.py run <agent> -- python -I tests\test_x64_firefox_esr.py WORK --base
  INSTALLED.qcow2 [--port] [--qmp-port]`: apt-installs firefox-esr on the Debian base (test_x64_services
  --keep-installed), checks --version and the screenshot's pixels, no ENOSYS, no firefox fault; checks that the kernel
  traces are on the serial console but not in dmesg / `nexusctl logs -k -b`, and a second boot with `kmsg.trace`.
- /bin/kabi2 runs in the same campaign (user and root): RLIMIT_SIGPENDING, fdinfo, System V IPC with /proc/sysvipc,
  IPC sysctls and namespaces, Landlock (fork, exec, seccomp, ptrace scope), the mount API, an _ALARM timer waking S3
  (the test VMs get `-global PIIX4_PM.disable_s3=0`); the campaign then requires every IPC object and Landlock domain
  released.
- `python -I C:\nexustest\wave2\pkgvm_lock.py run <agent> -- python -I tests\test_x64_kabi_debian.py WORK --base
  INSTALLED.qcow2 [--no-postgres]`: apt-installs dmsetup, dosfstools, postgresql; runs ipcmk/ipcs/ipcrm, dmsetup's udev
  cookies, PostgreSQL initdb/start/query/stop, and mount(8) through libmount's new-API path (bind, remount ro, loop
  vfat).
- `python -I arch\x86_64\ime_install.py install DISK.img` (or `overlay BASE.img OUT.qcow2`), then
  `program=/usr/libexec/nexus-fcitx5` in `~/.aurora/input-method.conf`. Test: `python -I tests\test_aurora_fcitx5.py WORK
  [--apps firefox,chrome] [--base DESKTOP64.img]`: installs into a qcow2 overlay over the read-only desk disk, types
  `zhongguo` Space, Ctrl+Shift+F9 `nihongo` Space Enter, Ctrl+Shift+F9 `dkssud` Space with QMP keys and requires the
  page's field (an HTTP page POSTs it) to read U+4E2D U+56FD, U+65E5 U+672C U+8A9E, U+C548 U+B155 (Zhongguo, Nihongo,
  Annyeong) in each browser; no fault, ENOSYS listed.

### Limits
- No io_uring, no rseq (above), no POSIX message queues (mq_open), no statmount/listmount (libmount falls back).
- System V IPC: CLONE_SYSVSEM between processes does not share the undo list; SHM_LOCK only sets SHM_LOCKED (pages may
  still be swapped); SHM_HUGETLB is ENOMEM; the IPC sysctls are written by the initial root only (Linux: the
  namespace's root too).
- Landlock: rules hold the path their fd had (Linux: the inode), so renaming a rule directory or another bind mount of
  it does not carry the rule; the ELF interpreter and #! interpreters are not checked for EXECUTE; the check and the
  operation are two steps (a concurrent rename of a parent in between is not excluded); open_by_handle_at is refused
  while a layer handles filesystem rights; no ABI v5+ (IOCTL_DEV, scopes, audit).
- New mount API: a detached mount is only a description until move_mount attaches it (the filesystem is created there,
  so device/option errors appear at move_mount, not at CMD_CREATE); AT_RECURSIVE clones no submounts;
  MOVE_MOUNT_SET_GROUP/BENEATH and MOUNT_ATTR_IDMAP are EINVAL; filesystem types are mount.c's (vfat, exfat, iso9660,
  fuse, fuseblk): tmpfs, proc, ext4 and the others are ENODEV through either API.
- RLIMIT_SIGPENDING counters are per kernel uid (not per user namespace); /proc/PID/status has no SigQ line.
- PR_SET_PDEATHSIG fires when the parent process is gone (Linux: the parent thread) and is not cleared by a set-user-ID
  exec or a credential change.
- [wave-2 merge: open(O_PATH) of /proc and /sys entries works now (hw2). Allowing it had made util-linux lsblk lose
  FSTYPE/LABEL (its libudev path found the device but no udev database); block devices' sysfs uevent files now carry
  the udev blkid properties (merge section), and test_x64_fstools basic passes with O_PATH on.]

## Security wave 2: SMAP default-on, seccomp completeness, kernel lockdown (2026-10-30, `sec2`)

- **SMAP is ON by default** (CR4.SMAP; `nosmap` boot option turns it off). New `arch/x86_64/uaccess.asm`
  (`uaccess_copy`/`uaccess_clear`: STAC/`rep movsb|stosb`/CLAC with an exception table) + `uaccess.c`
  (`uaccess_fixup` resumes a faulting user copy at its fixup so it returns short like Linux `copy_*_user`;
  `uaccess_smap_violation`; `strncpy_from_user`; `native_random_user`) + rewritten `uaccess.h`
  (`copy_to_user`/`copy_from_user` return bytes-not-copied, `uget8/16/32/64`, `uput8/16/32/64`, `clear_user`).
  `native.h` includes `uaccess.h`. Every direct kernel access to user memory (the 151-site list and the sites added
  since) now goes through these helpers; an accidental ring-0 user access with AC clear is reported
  (`KERNEL_FAULT ... SMAP violation`) and contained, never silently allowed. In `native_dispatch`: a kernel #PF on a
  user address with AC clear is treated as an SMAP bug (not "resolved"), else the block-I/O page-in runs, then
  `uaccess_fixup`. `process.c trap64` does `CLAC` at entry so a nested trap during a copy window leaves a clean AC.
  Boot line: `[64] Hardening: SMEP=on SMAP=on UMIP=on stack-canary=on`.
  - **WHPX**: `build.py whpx_security_flags()` adds `+smep,+smap` to the guest CPU when the hypervisor offers them
    (WHV capability bits 23/35); the test VMs' CPU models gained `+smep,+smap`. TCG `-cpu max` already has SMAP.
  - Converted shared files (access expression only, no reflow): `kernel.c process.c signal.c inet.c ipc.c event.c
    credentials.c netlink.c packet.c netioctl.c tun.c netproc.c device.c fsext.c pty.c inotify.c procext.c proc.c
    sysfs.c vfs.c segments.c locks.c input.c shmem.c keyring.c timekeeping.c kmsg.c uts.c pipe.c vm_reclaim.inc
    seccomp.c sound.c drm.c kms.c virtio_gpu.c fuse.c loop.c dm.c blkdev_node.c mount.c power.c acpi.c`. Full file
    list in `C:\nexustest\wave2\ws_sec2_notes.md`. A latent exec argv/envp TOCTOU (second pass trusted the measured
    sizes) is now bounds-checked. `tests/datagram_native_test.c` got host stubs for the new uaccess symbols.
  - `tests/test_x64_smap.py` still proves enforcement (ring-0 user write without a window faults + contained).
- **seccomp completeness** (`seccomp.c`): TSYNC now uses Linux `is_ancestor` matching and only sets NO_NEW_PRIVS on
  siblings when the caller has it; `TSYNC_ESRCH` returns ESRCH (not a tid) and together with NEW_LISTENER is allowed
  (otherwise TSYNC+NEW_LISTENER is EINVAL). `NEW_LISTENER`: one listener per filter chain (duplicate = EBUSY), the fd
  is cleaned up if a combined TSYNC fails, poll reports EPOLLIN/EPOLLOUT/EPOLLHUP, `NOTIF_RECV` requires a zeroed
  buffer, `SET_FLAGS` accepts `SYNC_WAKE_UP` and rejects unknown bits, `ADDFD`/`ADDFD_SEND` and `ID_VALID` (received,
  unanswered, live) as before; `WAIT_KILLABLE_RECV` makes the post-RECV wait killable (only a fatal signal, via
  `task_wait_channel_killable`), an interrupted USER_NOTIF returns ERESTARTSYS/EINTR. `FLAG_LOG`/`FLAG_SPEC_ALLOW`
  accepted; `/proc/sys/kernel/seccomp/{actions_avail,actions_logged}` (procext.c) with audit lines
  `[64] SECCOMP audit ... action=...`. Per-path instruction cap (`MAX_INSNS_PER_PATH`). `seccomp_user.c` gained
  TSYNC_ESRCH, FLAG_LOG/SPEC_ALLOW, duplicate-listener/SET_FLAGS, USER_NOTIF CONTINUE, ADDFD_SEND and
  listener-gone(ENOSYS) cases; `tests/test_x64_seccomp.py` still passes 1+2 CPUs.
- **Kernel lockdown** (new `arch/x86_64/lockdown.c`, Linux lockdown semantics). `int kernel_lockdown_level(void)`
  (0 none / 1 integrity / 2 confidentiality) is the strong definition power2 declares weak and checks before writing a
  hibernation image. Level comes from the `lockdown=integrity|confidentiality|none` boot option, else Secure Boot with
  a verified kernel = integrity. One-way ratchet (`kernel_lockdown_set`). `kernel_lockdown_denied(reason, confidential)`
  gates: `hibernate_write()` refuses under integrity (the image is CRC-checked, not authenticated);
  `kernel_lockdown_allows_kernel_image(authenticated)` for the update/kexec paths (kexec/`/dev/mem`/`/dev/port`/ioperm
  do not exist here, so are refused inherently; `kernel_lockdown_allows_kernel_image` has no in-kernel caller yet:
  nexus-update verifies bundles in user space and the UEFI loader verifies slot kernels). `kernel_lockdown_text`/
  `kernel_lockdown_write` produce/parse the Linux `/sys/kernel/security/lockdown` text, now exposed as a real
  **securityfs** file `/sys/kernel/security/lockdown` (sysfs.c; read by anyone, written by CAP_SYS_ADMIN). Boot line
  `[64] Lockdown: <level>`. Test: `tests/test_x64_lockdown.py` (3 levels x 1+2 CPUs, + `/bin/secfs64` securityfs check).
- **kguard oops containment** (round 2): a recoverable kernel-mode exception caused by a task in a syscall (e.g. an
  SMAP violation outside a uaccess window, or a kernel-stack overflow that hits the guard page as #PF/#DF on IST) is
  now killed like a Linux oops - the task gets SIGKILL and the system stays alive (`[64] kguard: killed pid=.. - system
  alive`) - instead of panicking. Panic remains for an IRQ/boot context or when an unsafe lock is held; #DF is contained
  only when it is a stack overflow. process.c `task_kguard_containable()/task_kguard_kill()/task_kernel_stack_overflow()`,
  wired in kernel.c `native_dispatch`. Test: `tests/test_x64_kguard.py` (SMAP stray read + stack overflow, 1+2 CPUs).
- **Kernel pointer hygiene** (round 2): `/proc/sys/kernel/kptr_restrict` (0..2) and `dmesg_restrict` (0..1) are now
  root-writable (procext.c; dmesg read honours dmesg_restrict in kmsg.c). New `/proc/kallsyms` lists kernel symbols;
  addresses are shown ONLY when `kptr_restrict==0` AND the reader has CAP_SYSLOG, else all-zero (mode 0400 when
  kptr_restrict==2). The symbol table is embedded in a loadable `.kallsyms` section after `.data` (linker.ld,
  gen_kallsyms.py, kallsyms_blob.asm; build.py links twice so addresses are exact). Test: `tests/test_x64_kptr.py`.
  KASLR boot slide still NOT applied (see Not done).
- **Authorization broker + pkexec** (round 2, NEW additive files): `/usr/libexec/nexus-polkitd` (nexus_polkitd.c) is a
  PolicyKit1-CheckAuthorization-subset broker on the Unix socket `/run/nexus-polkit.sock`
  (`QUERY uid action` -> AUTHORIZED|CHALLENGE|DENIED; `CHECK uid action password` -> AUTHORIZED|DENIED; rules
  yes/auth_self/auth_admin/no); the subject is bound to the peer (SO_PEERCRED). `/usr/bin/pkexec` (pkexec_user.c,
  set-uid root) runs a program as root after the caller (an admin) authenticates (via the broker if up, else itself).
  Both check passwords ONLY through the nexus-auth helper (`aurora_auth_verify_helper`, aurora_auth.c), never in
  process, so they share its per-account lockout (5 wrong -> 60 s) and 2 s delay with the lock screen and login.
  Built in desktop.py. Test: `tests/test_x64_polkit.py` (ext4 + DeskVm, 1+2 CPUs; includes the lockout on both
  paths and recovery after it expires). Client API +
  "move the Network panel behind the broker / drop the session's implicit CAP_NET_ADMIN" steps are in
  `C:\nexustest\wave2\ws_sec2_notes.md` (deferred from live code because net2 is editing netlink.c/aurora_network.c).
- **Use**: default build is SMAP-on; add `nosmap` to the kernel cmdline to disable, `lockdown=integrity` to lock down.
  power2: `int kernel_lockdown_level(void);` (weak) then `if (kernel_lockdown_level() >= 1) /* refuse unauth hibernate */`.
  desk2/pkg2: gate a privileged GUI action via `/run/nexus-polkit.sock` (QUERY then CHECK), then act through pkexec or a
  root helper - the Aurora auth dialog is the agent that collects the admin password; see the notes for the contract.
- **Tests**: `python -I tests/test_x64_smap.py W`, `test_x64_seccomp.py`, `test_x64_lockdown.py`, `test_x64_kguard.py`,
  `test_x64_kptr.py`, `test_x64_polkit.py`, plus the whole `sec` group and `full` stay green with SMAP on.
- **Not done this wave** (honest): KASLR slide still not applied at boot (rewriting entry.asm's BIOS+UEFI early paging +
  a boot-time reloc pass is too large/risky for the 2.88 MB cap, and the new .kallsyms addresses are data that would
  also need a slide fixup); `test_x64_kaslr.py` proves the relocation table only. Moving the Network panel behind the
  broker and dropping the session's implicit CAP_NET_ADMIN is specified in the notes but not landed (net2 owns those
  files concurrently; needs a live-desktop boot to test). [wave-2 merge: `sysinstall.py services --aurora` now
  installs /usr/bin/pkexec (04755) and /usr/libexec/nexus-polkitd; `services/nexus-polkitd.service` starts the broker
  as root.] TPM: no swtpm on this host.

## Power wave 2: tickless idle, UPower + power profiles, hibernation to swap (2026-10-30, power2)

- **Tickless idle** (`tick.c`; one-line hooks in process.c trap64 + BSP pass, cpufreq.c `cpu_idle_enter`, smp.c tail,
  pm.c, power.c): idle CPUs stop their tick; the boot CPU arms a one-shot for its next event (TSC-deadline > local-APIC
  one-shot > PIT; the PIT is masked after boot) from `task_next_deadline()` and every BSP poll's next-event function.
  Early events are re-armed, delivery latency is learned. `nohz=off` (old periodic ticks), `tick=tsc-deadline|lapic|pit`;
  `/proc/interrupts`, `/proc/timer_list`, `/sys/devices/system/clockevents`. **Contract**: work polled in the boot CPU's
  pass must say when it is due (`tick_bsp_pass` or `tick_register_poller(name, next)`, `power.h`), else it runs only
  when that CPU wakes anyway (<= 1 s). Measured idle (QEMU): boot CPU 685 -> 5 timer irq/s, wake-ups 681 -> 5/s (smp1),
  936 -> 6/s (smp2), QEMU host CPU 3.7-6.4% -> 0.2-0.6%; audio playback 600 -> 18-89 wake-ups/s (`sound_next_event`).
- **UPower + power profiles**: `nexus_upowerd.c` (`/usr/libexec/nexus-upowerd`, `services/nexus-upower.service`, D-Bus
  policy + activation via sysinstall.py): `org.freedesktop.UPower` 1.90 (power_supply devices, DisplayDevice, OnBattery,
  lid, GetCriticalAction, uevent-driven signals) and power-profiles-daemon 0.30 (`org.freedesktop.UPower.PowerProfiles`
  + `net.hadess.PowerProfiles`, holds). Kernel `/sys/power/policy/profile{,_choices}` -> cpufreq governor/EPP. Aurora's
  battery panel has a Power Mode row (keys 1-3) and battery saver (Low on battery: Balanced -> Low Power until power
  returns). Chrome's `navigator.getBattery()` reads UPower.
- **Hibernation**: ACPI NVS saved/restored in S3 and S4; image into an active swap partition or swap file (`resume=`
  UUID/LABEL/PARTUUID/`/dev/NAME`/MAJ:MIN, `resume_offset=`, `/sys/power/resume{,_offset}`; header page tagged, SWAPSPACE2
  back after restore/discard), read back and verified before it is marked valid, memory reclaimed down to `image_size`
  (default 2/5 RAM) or to what the area holds; refused with EPERM under lockdown (`kernel_lockdown_level()`, weak, else
  `lockdown=`): no TPM, so no authenticated hibernation. `resume=UUID=`/`LABEL=` also name a filesystem (a swap file:
  `resume=UUID=<fs> resume_offset=N`); with stor2's encrypted root, `/dev/mapper/NAME` or a UUID inside it is resolved
  on the second `hibernate_resume()` (after the unlock).
- **Sleep/lid fixes**: the RTC wake alarm wakes suspend-to-idle (QEMU raises RTC_STS only in S3) and "+N" counts from the
  RTC clock; playback continues across s2idle/S3 (one recoverable xrun per sleep, as ALSA after resume);
  `/sys/power/policy/lid_switch_external_power` (follows `lid_switch` until written) and `lid_switch_docked` (default
  ignore; docked = more than one connected virtio-gpu head, `kms_connected_heads()`). When the lid closes and the
  machine stays up (action ignore, or docked) the built-in panel (KMS head 0) goes dark whatever the DRM master asks
  (`kms_lid()`), and comes back at open.
- **Kernel fix: `struct open_file` use-after-free** (kernel.c; kernel faults in `file_views_moved`, `memory_file_release`,
  `description_read`, seen in UPower runs and on the desktop): `fd_release` dropped the last reference, then slept
  (inotify_event's parent lookup waits for the disk, the big lock released) while the description already looked free;
  I/O-free calls on other CPUs (socket/eventfd/epoll creation, SCM_RIGHTS -> `descriptors_prune`) reused or freed it.
  Now a `releasing` description is skipped by `descriptor_new` and `descriptors_prune`, and its page-cache view is
  unlinked before anything that can sleep. Reproducer `tests/test_x64_fdrace.py` (`fdrace_user.c`; boot option
  `test.fd-release-sleep` makes the window deterministic): before 3/3 kernel faults or hangs, after 12/12 (FAT and
  ext4 roots, 1/2/4 CPUs, injected and natural).
- **Tests**: `tests/test_x64_tickless.py` (measure, accuracy, devices, nohz, audio; audio-sleep on request),
  `test_x64_upower.py` (pkg base, 44 checks incl. Chrome), `test_x64_suspend.py --only hibernate-swap,hibernate-swap-corrupt,
  hibernate-swap-full,hibernate-swap-squeeze,hibernate-lockdown,hibernate-swapfile`, `test_x64_acpi.py --only lid-policy`,
  `test_x64_aurora_power.py --scenarios profile,lid` (in the integrator's power_aurora group since the wave-2 merge),
  `test_x64_fdrace.py`.
- **Limits**: QEMU only. No charge thresholds (no EC/WMI vendor driver; UPower reports unsupported), wake-on-LAN (QEMU
  NICs cannot wake a guest), USB autosuspend or ACPI dock stations. Idle wake-ups left, and what removes each:
  network housekeeping 4/s (network.c returns +250 ms while no TCP timer runs: per-item deadlines for TIME_WAIT/keepalive,
  DHCP T1/T2, the link watchdog of NICs without a link interrupt, IGMP/MLD reports, held-packet ARP retries, neighbour
  expiry); kswapd 1/s (reclaim.c 1 s timeout: wake it from the allocator below the low watermark, then sleep without a
  timeout); the boot CPU's 1 s cap (tick.c TICK_MAX_SLEEP_NS: once every boot-CPU poll reports its next event - hw2's
  v4l2/usbclass must register - raise it to the event device's range, ~68 s local APIC, unlimited TSC-deadline).

## Networking wave 2 (net2, 2026-10-30): IPv6 in apps, Network app IPv6 + Wi-Fi, multicast/mDNS, NAT, Wi-Fi stack

- **IPv6 in real apps**: Chrome, Firefox and Wine winsock over dual-stack, IPv6-only (RA/RDNSS or DHCPv6 DNS) and
  slirp `ipv4=off`; happy-eyeballs fallback measured. `dhcp64.c` withdraws QEMU's provisional 10.0.2.15 after the 4th
  DISCOVER without a server (IPv6-only nets looked dual-stack to AI_ADDRCONFIG). `wine_runner_user.c`: `host PORT ADDR`.
  Test `tests/test_x64_ipv6_apps.py` (desk disk overlay; `tests/netfix.py` = reusable Python network: ARP/NDP/RA/DHCPv4/
  DHCPv6/DNS/user-space TCP+HTTP/mDNS/tunnels), `tests/wine/winsock6.c`.
- **Network app** (`aurora_network.c`): `slaac=0|1`, `static6=ADDR/PLEN GW` (+ replaced static4/6 removed); saves to
  `/etc/nexus/network.conf`, or on EROFS/EACCES (FAT32 desk disk, private_path) to `/home/nexus/.aurora/network.conf`
  which overrides. `ndisc_forget_ra()` drops RA/DHCPv6 DNS when accept_ra goes 0. Test `tests/test_aurora_network6.py`.
- **Multicast**: new `mcast.c` (RFC 3678 API incl. MCAST_MSFILTER, IGMPv1/v2/v3 + MLDv1/v2 with state-change bursts,
  delivery filter, `/proc/net/mcfilter[6]`) replacing the IGMP of net4.c and MLD of icmp6.c; agent `mcast_user.c`, test
  `tests/test_x64_multicast.py`. **mDNS**: avahi-daemon on by default (`services/avahi-daemon.service`, `sysinstall.py`
  LEGACY_OFF removes the old disabled marker); kernel fixes: netlink SO_PASSCRED (SCM_CREDENTIALS) and IPv6 multicast
  sendmsg scope from IPV6_PKTINFO. Test `tests/test_x64_mdns.py` (pkg-style VM, run under `pkgvm_lock.py`: avahi-browse,
  nss-mdns `.local`, CUPS dnssd printer from ippeveprinter-like fixture, lp).
- **NAT/forwarding**: new `forward.c` (IPv4/IPv6 forwarding, PREROUTING copy), `conntrack.c` NAT engine (port search,
  ICMP-error inner translation, masquerade flush on address loss), `nft.c` nat/masq/redir, nat+route chains, sets
  (interval, concatenations, maps/vmaps, timeouts, dynamic sets + meters), limit/quota/counter objects, numgen/fib/rt/
  exthdr (MSS clamp), meta/ct mark set, payload mangling. Fixed: immediate data values were never loaded. Test
  `tests/test_x64_nat.py` (`taprelay_user.c` TAP<->UDP LAN, firewalld- and ufw-style rule sets).
- **Wi-Fi**: `wifi.h` (driver interface, documented for the MT7922 port: frames in/out + set_channel/set_mode, the
  softmac does MLME, crypto, scanning), `wifi.c` (softmac STA + AP, CCMP-128 + BIP-CMAC-128 with known-answer
  self-test, beacon loss, port control, virtual medium: `/proc/sys/net/nexus/wifi_sim` add N | del phyN | signal
  wlanN DBM | range wlanN on|off | tune wlanN MHZ | inject wlanN HEX | selftest), `nl80211.c` (+ `nl80211_defs.h`
  generated from nl80211.h, ISC), hooks in wireguard.c (nlctrl), netlink.c (nl_unicast, close hook), netdev.c,
  network.c, netproc.c. Packages: `wifi_install.py` + `wifi-lock.json` (wpasupplicant/hostapd 2:2.10-24, iw 6.9-1;
  cache `NEXUS_WIFI_PKGS`). Aurora: `aurora_wifi.c` = the Network app's Wi-Fi tab and `aurora-app network --wifi
  status|scan|saved|connect SSID [PASS]|forget SSID` (own wpa_supplicant, `~/.aurora/wpa_supplicant-IFACE.conf` 0600,
  reconnect in the `--apply` boot step). Test `tests/test_x64_wifi.py` (WPA2, wrong key, scan, signal, ARP over CCMP,
  roam + beacon-loss fallback, WPA3-SAE + PMF, spoofed deauth -> SA Query, protected deauth, injected junk, the app's
  logic headless via `tests/wifi/aurora_wifi_cli.c`). netwire agent: SPAWN/PUTA/TAIL/GREP/KILL/ARPING.
- **Wi-Fi follow-up (net2, round 2)**: /sys/class/ieee80211/phyN (index/name/macaddress) + /sys/class/net/wlanN
  (uevent DEVTYPE=wlan, phy80211 link), created by wifi.c via a new sysfs.c `sysfs_add_wifi` (content WIFI_ATTR),
  so `iw phy NAME` resolves. Monitor mode (`iw dev X set type monitor`): a radiotap-prefixed tap to packet sockets
  (ARPHRD_IEEE80211_RADIOTAP 803) for tcpdump/raw capture. Ciphers generalised: AES-128/256, CCMP-128/256,
  GCMP-128/256 (GHASH+GCM) and BIP-GMAC-128/256, all with known-answer vectors (cross-checked vs Python); the key
  struct now carries rounds+mic_len and rk[60]. STA+AP concurrency on one phy: a 2nd vif via NL80211 NEW/DEL_INTERFACE
  (`wifi_add_vif`/`wifi_del_vif`, wifi_radio.vif2), dual-vif rx/poll/next-event, the AP forced onto the client's
  channel (one radio; a cross-channel AP is refused in set_channel and wifi_start_ap). Tests: tests/test_x64_wifi2.py
  (sysfs, monitor via the netwire agent's MONCAP, GCMP connect+ARP, STA+AP concurrency + channel constraint), 1+2 CPU.
  Still a gap: an automated screenshot test of the Aurora Wi-Fi tab (its logic has headless coverage; the drawing does not).
- **Not done / limits**: no real Wi-Fi driver (MT7922: implement wifi_driver_ops + firmware), no CONNECT (fullmac),
  monitor/mesh/P2P, TKIP/GCMP, hardware scan offload; one interface per radio; `/sys/class/ieee80211` absent (`iw phy
  NAME` needs `phy#N`); the Network app's Wi-Fi tab drawing was not exercised by an automated GUI test (its logic is);
  sec2's privilege broker should take over starting wpa_supplicant and the network settings writes.
- **Merge seams**: power2's tickless core meets this in `network.c` (the virtual-medium fast path `air`) and through
  `tick_register_poller` (weak; `netdev_init` registers `mc_next_event` and `wifi_next_event`, ~0 when idle, one compare
  with no radios). conntrack lookups skip expired entries and nft set elements are checked at lookup, so their GC may run
  rarely. sec2's SMAP conversion must cover `mc_user_in/out` (inet.c), the SO_PASSCRED cmsg (netlink.c), netproc_write.
## Hardware wave 2: audio click, USB audio, cameras, game controllers, x2APIC (2026-10-30, hw2)

- **The `test_x64_hda` click is QEMU's, not ours**: hda-codec in timer mode (`use-timer=true`, QEMU's default) drops
  exactly 2048 frames once its 8 KiB ring overruns ~1.2-2 s after a stream starts under host load (traced with
  `-trace hda_audio_overrun`; also with `-audiodev none`; never with `use-timer=false`). All 19 historical splices were
  exact 2048-frame drops with a traced overrun. `test_x64_hda.py` now runs QEMU with `-msg timestamp=on` + that trace
  and accepts only such matched drops (`wav_check.classify_splice`/`explain_breaks`; anything else still fails);
  the desktop launcher's `NEXUS_AUDIO=hda` uses `use-timer=false`. Pause-segment failures were an analyzer artifact.
- **USB Audio Class 1/2** (`uaudio.c`, isochronous pipes in `xhci.c` `usb_pipe_*`, class binding `usbclass.c`):
  playback + capture, async feedback, volume/mute controls, hot-plug; each device is ALSA card 1..3 next to HDA
  (`sound.c` multi-card, driver name "NexusUSB"). Aurora `aurora_sound.c` + Settings > Sound: default = newest card or
  the chosen one (`/etc/nexus-sound.conf`), written to `/run/user/1000/asound.conf` and exported through
  `ALSA_CONFIG_PATH`, follows uevents; a user default in `/etc/asound.conf`/`~/.asoundrc` wins.
- **Cameras**: V4L2 core `v4l2.c` (`/dev/videoN`, QUERYCAP/ENUM_FMT/G/S/TRY_FMT/REQBUFS/QUERYBUF/mmap/QBUF/DQBUF/
  STREAMON/OFF, frame sizes/intervals, (ext) controls, priority), UVC driver `uvc.c` (probe/commit, isoch + bulk,
  payload reassembly), virtual camera `vcam.c` (kernel option `vcam`: colour bars + frame counter, YUYV/NV12, 3 sizes).
- **Game controllers**: `evdev.c` (dynamic `/dev/input/eventN` + joydev `/dev/input/jsN`, force feedback, grab,
  SYN_DROPPED), `uinput.c` (`/dev/uinput`, FF upload/erase protocol), `hwsysfs.c` (input sysfs objects + uevent
  properties ID_INPUT_JOYSTICK, so libudev finds pads without udevd), `gamepad.c` (generic HID like hid-input, DS4 /
  DualSense layout + rumble), `xpad.c` (Xbox 360 XInput, Xbox One/Series GIP), `vpad.c` (kernel option `vpad`:
  virtual Xbox 360 pad with a 4 s input pattern, rumble logged). libudev needed two generic fixes: `O_PATH` (and
  `O_PATH|O_NOFOLLOW` on sysfs links) in `proc.c`, `statfs` of sysfs = SYSFS_MAGIC in `kernel.c` (was ENOSYS).
- **x2APIC** (`apic.c`; `irq.c`/`smp.c`/`pm.c` reach the LAPIC only through it): MSR registers, 64-bit ICR IPIs. Kept
  when the firmware hands over in x2APIC mode, or with kernel option `x2apic`; `nox2apic` forces xAPIC; default stays
  xAPIC. A refused switch (#GP, e.g. WHPX) falls back to xAPIC. IOAPIC/MSI destinations stay 8-bit (no interrupt
  remapping): CPUs with APIC id > 254 are not used.
- **Bluetooth** (wave 2, `bt`: `bluetooth.c` core + `bt_mgmt.c`, `bt_l2cap.c`, `bt_smp.c`, `bt_rfcomm.c`, `bt_crypto.c`,
  `bt_alg.c`, `btusb.c`; internal header `bt.h`): a host stack with the Linux ABI that Debian 13's BlueZ 5.82 runs on.
  Controllers come from `/dev/vhci` (btvirt/hciemu/test programs) or USB (`btusb.c`: class E0/01/01, interrupt events,
  bulk ACL, class control requests for commands; reassembly of split events/ACL). Core: per-controller command FIFO with
  Num_HCI_Command_Packets credits and 2 s timeouts, ACL fragmentation against the controller's buffers and credits
  (Number Of Completed Packets), reassembly up to 64 KiB, BR/EDR + LE connections (incoming/outgoing, idle disconnect,
  attempt cancelled when its last user goes), security (Authentication Requested + encryption, LE via SMP/stored LTK),
  setup at registration (Index Added after the identity is read; default settings = Linux's: BR/EDR only, user space
  enables SSP/LE/SC/bondable). Sockets: HCI raw/user/monitor (btmon incl. mgmt tracing, SO_TIMESTAMP)/control,
  BTPROTO_L2CAP (SEQPACKET, STREAM, RAW for l2ping; BR/EDR basic mode with configuration and the feature-mask exchange,
  LE credit-based and Ext-Flowctl/ECRED channels, ATT fixed channel, BT_SECURITY/BT_DEFER_SETUP/BT_MODE/L2CAP_OPTIONS/
  SO_SNDTIMEO...), BTPROTO_RFCOMM (stream, PN/MSC/RPN/RLS, credit flow control), AF_ALG `ecb(aes)`/`cmac(aes)` (BlueZ's
  crypto: btvirt, bluetoothd and the testers need it). mgmt 1.22: every command bluetoothd issues (settings, name/class/
  UUID/EIR, discovery with name resolution, PAIR/UNPAIR, link keys/LTK/IRK loading + NEW_* events incl. NEW_CSRK,
  user confirm/passkey/PIN replies, advertising instance, device add/remove/block/flags, connection info) runs on a
  cooperative worker (`write()` returns at once as on Linux, so an emulated controller in the writer's own thread can
  answer). SMP: LE legacy (JW, passkey) and Secure Connections (JW, numeric comparison, passkey; P-256 keys validated),
  Linux's pairing-command rules (responder echoes AuthReq, LTK+CSRK distribution, identity once IRKs are loaded, SC
  link key derived for BR/EDR). SSP: IO capability exchange, Linux's auto-accept rules, link-key notification/requests.
  Behaviour that peers depend on follows Linux (identifiers 1-128, "pending" connection responses during the
  information exchange, reject formats, lenient LE K-frame and RFCOMM length handling). Crypto known answers at boot:
  `[64] PASS Bluetooth crypto known answers` (FIPS-197, RFC 4493, the spec's SMP vectors, debug key pair, ECDH).
- **BlueZ** pinned like the Wi-Fi tools: `arch/x86_64/bluez_install.py` + `bluez-lock.json` (33 .debs from trixie:
  signed InRelease -> Packages.xz -> closure, URL + size + SHA-256 + snapshot fallback; groups `bluez` = bluez +
  dbus-daemon/dbus-bin + libs, `bluez-test` = bluez-test-tools: btvirt and BlueZ's kernel testers). Cache:
  `NEXUS_BLUEZ_PKGS` (default `D:\nexustest\wave2\ws_hw2_pkgs`). `python -I arch/x86_64/bluez_install.py <disk.img>
  --groups bluez`; `... lock` re-pins. On a FAT root only `/tmp`, `/home/nexus` are writable: run the bus with a config
  listening under `/tmp` and bluetoothd with `STATE_DIRECTORY=...` (the test does); no nexusd unit for bluetoothd yet.
- **Using it**: system bus up, `bluetoothd -n`, then `bluetoothctl` (`power on`, `scan on`, `pair`, `connect`,
  `remove`), `btmgmt`, `btmon`, `l2ping`, own programs with AF_BLUETOOTH sockets. Kernel log `[64] BLUETOOTH hciN
  registered/removed`, `... ACL|LE <addr> connected/disconnected`.
- **Bluetooth tests**: `tests/test_x64_bluez.py` (cases bredr, le, hostile + `_smp` variants on 2 CPUs; `--only testers`
  runs BlueZ's l2cap/smp/rfcomm-tester one test per process): btvirt -l2 gives two controllers that see each other,
  bluetoothd manages both, bluetoothctl (agent NoInputNoOutput) does power on / scan / pair (BR/EDR SSP Just Works, LE
  SC Just Works) / info / remove; L2CAP echo (BR/EDR basic mode and LE CoC, security level 2), RFCOMM echo 60 kB,
  l2ping; `hostile` = a raw controller program injecting malformed events, ACL, L2CAP/LE signalling, SMP (invalid P-256
  key) and RFCOMM frames + 2300 fuzzed frames, with liveness checks; every case ends with the campaign's page check.
  Testers: l2cap 51/62, smp 8/8, rfcomm 11/11. `tests/test_x64_bluetooth.py` (HCI layer); `tests/test_usbclass_sim.py`
  btusb (simulated dongle, fuzzed transfers). signalfd/timerfd come from kabi's tree [wave-2 merge: BlueZ now
  runs on the kernel's own signalfd/timerfd; `tests/bt_shim.c` was dropped after test_x64_bluez passed 6/6 without it].
- **Bluetooth limits**: no SCO/eSCO or ISO (no Bluetooth audio: HFP/A2DP need SCO/ISO + PipeWire), no BNEP, no
  HIDP/uhid (Bluetooth keyboards/mice), no /dev/rfkill, no LE privacy (RPA generation/resolution), legacy advertising/
  scanning only, L2CAP ERTM/streaming answered with basic mode, no RFCOMM TTYs, BR/EDR SMP over CID 7 refused, no
  SO_TIMESTAMPING (3 testers), LE connections are created directly instead of after a passive scan (4 testers), no
  Security Block for unencrypted SSP links (1 tester). btusb is verified only against a simulator.
  **MT7922 (the user's adapter) is not usable yet**: it needs the btmtk firmware patch download before standard HCI:
  read chip id (vendor UHW register 0x70010200 = 0x7922, fw variant from 0x80021004), load
  `mediatek/BT_RAM_CODE_MT7922_1_1_hdr.bin` (linux-firmware), for each section of its header (global descriptor at 32,
  section maps at 96 + 64*i) send WMT PATCH_DWNLD (HCI vendor command 0xFC6F {dir 1, op 1, dlen, flag, data}) - query
  with the 52-byte section info, then 250-byte chunks flagged first/continue/last - reading WMT events (0xE4) through the
  vendor control IN request, then WMT FUNC_CTRL (op 6, param 1). `btusb.c` binds MT79xx IDs and logs that this step is
  missing; none of it is implemented or verified (no hardware).
- **UEFI loader fix** (`uefi/bootx64.c` `allocate_outside`, merge note for kabi's 6 MiB `KERNEL_LIMIT`): the loader
  kept every probe allocation that clashed with [1 MiB, KERNEL_LIMIT) as loader data for good; with the 6 MiB limit a
  kernel.bin above ~2 MiB made its file buffer walk down from 8 MiB, filling 1-8 MiB with loader data, so the kernel
  range could not be allocated ("kernel range 0x100000.. is in use by the firmware" under OVMF: test_x64_uefi,
  display_modes uefi_*, hwready diag_*). The probes are now released once a buffer is found.
- **Device memory and tickless** (merge notes): class drivers (uaudio, uvc, xpad, gamepad, evdev table, USB sound-card
  rings) allocate through `storage_alloc` (counted, outside run_image's per-program audit); the audio/hw campaigns
  compare without counted pages. `hwnode_next_event` is registered with power2's `tick_register_poller` (weak):
  camera frames, force-feedback ends, vpad steps, ~0 when idle; usbclass has no timed work. power2's
  `sound_next_event()` must loop over `cards[]` (multi-card sound.c). `smp.c` keeps `lapic_reg_read/write` over
  `apic_read/apic_write` for tick.c (x2APIC-safe).
- **Audio settings, measured** (`tests/measure_audio_drops.py`): HDA timer mode drops 2048 frames in 8/8 runs (throttled)
  and 2/6 (unthrottled); `use-timer=false` and AC97 0/7 each; longer timer/buffer no better. `desktop_audio_options`
  keeps AC97 5 ms/40 ms and HDA use-timer=false.
- **Tests**: `test_x64_hda.py`; `test_x64_usbaudio.py` (QEMU usb-audio UAC1: usb/both/hotplug/default/stay);
  `test_x64_bluetooth.py` (hci, smp);
  `test_usbclass_sim.py` (host simulators `uaudio_sim_test.c` UAC2+UAC1, `uvc_sim_test.c` C270 + bulk camera,
  `gamepad_sim_test.c` F310/HID/DS4/DualSense/Xbox 360/One + fuzz); `test_x64_camera.py` (vcam/smp/nocam);
  `test_x64_camera_chrome.py --browser chrome|firefox` (real getUserMedia; read-only desk disk + `overlay_stage.py`); `test_x64_gamepad.py`
  (uinput/smp/vpad/none/udev with Debian's libudev); `test_x64_gamepad_chrome.py` (real Chrome Gamepad API, standard
  mapping, dual-rumble); `test_x64_x2apic.py` (TCG campaign 4 CPUs, MSI devices, WHPX fallback, default, off);
  `test_x64_suspend.py --cmdline x2apic --smp 4 --only s3`. Guest programs `uaudio_user.c`, `camera_user.c`,
  `gamepad_user.c` (run as `/bin/hw64` by the `/nexus/test-hw` campaign).
- **Limits**: UVC, UAC2, HID/Sony/Xbox pads verified only against host simulators (QEMU has none of them; usb-audio
  is UAC1); no real USB device was tested. Not done: SDL2 and ffmpeg checks (Debian ffmpeg pulls ~100 library
  packages), Bluetooth audio/HID and MT7922 firmware (see Bluetooth limits), laptop DMIC/SOF
  (needs the AMD ACP/SOF DSP driver + topology firmware on real hardware), interrupt remapping (APIC ids > 254).

## Wave 2 storage: FAT32 check/repair, encrypted root, fast AES-NI, dm suspend, swap partitions, hybrid USB (stor2)

Native x86_64 only (`kernel/fat32.c` keeps compiling for i686: the new code is under `NEXUS_NATIVE64`). Verified in QEMU
(WHPX, 1 and 2 CPUs); nothing on a physical PC.
- **FAT32 lost clusters: cause and fix.** Files unlinked while still open or mapped (Chrome's `/tmp/.com.google.Chrome.*`
  shared memory and BrowserMetrics `.pma`, Wine's unlinked mappings) are orphans whose clusters are freed at the last
  close. `disk_shutdown()` did nothing on FAT and a crash/VM kill lost them too, so every session ended with whole
  EOC-terminated chains nobody referenced (the shared desk disk: 4,529 clusters in 11 chains, three of 4 MiB). Now:
  `inode.c disk_orphan_shutdown()` frees every orphan at an orderly power-off/reboot, and the volume carries a **dirty
  flag** (FAT[1] clean bit cleared at the first write of a session, set again by `fat32_unmount_clean()` after FAT,
  FSInfo and orphans are on disk; rename commits no longer toggle it). Error paths of `f57_create`/`fat32_write_file`
  that leaked a fresh cluster free it.
- **Boot check/repair** (`arch/x86_64/fatcheck.inc`, included by `kernel/fat32.c`): a dirty root volume (FAT[1] or the
  Linux boot-sector dirty bit) is checked before the mount, like `fsck.fat -a`: differing FAT copies, FAT entries
  outside the volume, chains that leave the volume / reach a free cluster / loop / are cross-linked (truncated), sizes
  against chains, directories with a size, `.`/`..`, orphaned long-name slots, unpublished NexusOS staged entries
  (`T#######.TMP`), the second entry of a file left by an interrupted rename, lost clusters (freed), FSInfo, flags.
  Entries after a directory's end marker keep their clusters. Options `fsck.mode=auto|force|skip` (skip: a dirty volume
  mounts read-only, the old behaviour), `fsck.repair=preen|yes|no` (no: report; problems -> read-only). Memory: the
  first FAT + two bitmaps in page vectors for the duration (8 GiB volume: ~8.5 MiB). Serial: `[FAT32] check: ...`.
  Every public `fat32_*` entry also waits while another task sleeps in a disk wait (`FAT_ENTRY`), and a directory's
  name filter is only installed after a scan that reached the real end of its chain.
- **Host tool** `python -I userland/fatfsck.py IMAGE [-n|-a] [--recover] [--json F]` (same policy; `--recover` keeps lost
  chains as `/FSCK0000.REC`...; never prints names below `/home/*/.config|.cache`, reads no file contents; exit 0/1/4/8
  as fsck). `userland/fatput.py ... rmdir DIR` (empty directories); `desk_install.py fccache --in-place` removes its
  build folder.
- **Large-directory lookup bug** (Wine cmd32/npp32, syswow64 `zlib1.dll`): not reproducible here -- 850 long-named
  entries over fragmented 4 KiB clusters (`test_x64_fatdir.py`) and `test_wine.py` console+apps+gui (129/129 incl. cmd32,
  npp32) pass; the entry barrier and filter rule above remove the two ways found by review that could produce it.
- **Encrypted root** (`cryptroot.c`, `luks.c/.h`, `argon2.c`, `sha256.c`, `kcrypto.h`): `cryptroot=<UUID=|PARTUUID=|LABEL=|
  /dev/...>` or `rd.luks.uuid=<uuid>` (else the only LUKS volume typed "Linux LUKS"/NexusOS root, if root= names no plain
  device), `cryptroot.name=` (else NAME of `root=/dev/mapper/NAME`, else cryptroot), `cryptroot.tries=` (3). After the USB controllers are up and no plain root exists, a dialog
  on the framebuffer (console prompt without one) asks for the passphrase; PS/2 and USB keyboards; a wrong passphrase
  asks again, the last failure powers off. LUKS2 (argon2id/argon2i/pbkdf2 keyslots, both header copies with SHA-256
  checksums) and LUKS1 (pbkdf2-sha256); ciphers aes-xts-plain64/plain, aes-cbc-essiv:sha256, aes-cbc-plain64/plain;
  512/4096-byte sectors. `/dev/mapper/<name>` (dm-crypt, `dm_crypt_create`) becomes the root (ext4 or FAT32);
  `/proc/mounts` names it. cryptsetup's own default volume (argon2id, ~1 GiB, 6 iterations) opens in ~5.5 s.
  Hostile headers are refused, never trusted (bounds on every offset/size, JSON nesting 16, one pass per object, 32
  keyslots, Argon2 memory checked against free memory); a bad primary copy falls back to the secondary.
- **Keyboard layout at the prompts** (`keymaps.c/.h`, `keymaps_data.h` generated by `gen_keymaps.py` from the
  xkeyboard-config symbols Aurora's xkbcommon uses, MIT): the 19 layouts Aurora offers (us, gb, us(intl), de, fr, es, it,
  pt, in, ru, gr, il, ara, th, jp, kr, tr, pl, se): Shift, AltGr (right Alt), Caps Lock, dead keys composed (NFC); the
  passphrase is the UTF-8 Aurora/a terminal would produce. Layout: `rd.vconsole.keymap=` / `vconsole.keymap=` (xkb names
  or kbd names such as `de-latin1`, `uk`, `sv-latin1`), then (root mounted: crypttab, installer) `/etc/vconsole.conf`
  KEYMAP=, Aurora's `/home/nexus/.aurora/keyboard.conf` (first layout), `/etc/default/keyboard`. An encrypted root's
  prompt cannot read those files, so the installer adds `vconsole.keymap=` to the target command line and, at every
  orderly shutdown, `osinstall.c boot_keymap_sync()` rewrites that disk's command line (stage 2 copy + CMDLINE.TXT,
  read back) when the configured layout changed -- Linux's update-initramfs step. The dialog shows the layout.
- **/etc/crypttab** (`crypttab.c`, before /etc/fstab, swap and init): `name device keyfile|none|- options`; devices
  UUID=/PARTUUID=/LABEL=/PARTLABEL=//dev/...; keyfile bytes as the key (on ext4 roots refused unless a root-owned
  regular file without group/other bits; FAT32 has no permissions: a warning); options luks, discard (allow_discards),
  nofail (absent device: no wait, no error; otherwise 10 s), noauto, readonly, tries=, keyfile-size=, keyfile-offset=;
  plain/tcrypt/bitlk/swap/tmp/header=/keyscript= lines are skipped with a message; the root's own line is skipped.
  Passphrase lines use the cryptroot dialog in the configured layout; a failed volume stays closed, the boot goes on.
- **Installer "Encrypt disk"**: `nexus-os-install DISK --encrypt` (or `nexus.install.encrypt`); after YES the kernel asks the
  passphrase twice (8+ characters), makes the root partition 16 MiB larger and "Linux LUKS", formats LUKS2 itself
  (argon2id with an eighth of the RAM up to 1 GiB, aes-xts-plain64, 512-bit random key; Debian's cryptsetup opens it),
  copies the root through a temporary mapping and writes `cryptroot=PARTUUID=... root=/dev/mapper/cryptroot`.
  Every install (plain or encrypted) now writes the copy's FSInfo free count from the FAT it copied.
- **AES**: `aes_ni.asm` runs eight blocks in flight (XTS both ways, CBC decryption) and saves/restores the 16 XMM
  registers itself (kernel code is never preempted mid-routine and IRQ handlers use no SSE); boot line
  `[64] AES throughput MB/s ...` (~8 GB/s XTS here; dm-crypt over a zero target 4.9 GB/s read / 3.0 GB/s write).
  KATs: FIPS-197, SP 800-38A CBC, IEEE 1619, ESSIV, SHA-256/HMAC/PBKDF2, BLAKE2b, Argon2 (RFC 9106), both AES paths.
  PCLMULQDQ is not used (no GHASH mode is offered). `noaesni` still forces the constant-time software AES.
- **dm suspend**: `dmsetup suspend` / `cryptsetup luksSuspend` freeze the filesystems mounted on the device at their
  entry points (mount.c) and hold new requests until resume; `key wipe` works on an open volume; resume with a wiped key
  is refused (EINVAL); the device holding the root cannot be suspended (EBUSY). `luksResume` works.
- **Swap partitions**: `/etc/fstab` UUID=/LABEL= partitions with `pri=` and `nofail` were already implemented (swap.c);
  now tested end to end; `swap_kernel_pages()` keeps run_image's ownership check right when a session swapoffs/swapons
  boot-time areas (it panicked before).
- **Hibernation with an encrypted root** (hunks in power2's `hibernate.c` and in `kernel.c`; merge notes in
  `C:\nexustest\wave2\ws_stor2_notes.md`): the Linux initramfs order -- after `cryptroot_boot()` unlocked the root,
  kernel.c calls `hibernate_resume_unlocked()`, so an image in an area on the unlocked mapping (here `/nexus/hiber.img`
  on the encrypted FAT32 root; with power2's swap areas a swap file/partition on it) is found and restored; no image or
  a stale one: the boot goes on. `hibernate_prepare` allows an area on the encrypted root's device (the image, keys
  included, is encrypted at rest) and refuses any other area while a dm-crypt volume is open, and areas on other dm
  devices (they do not exist yet at resume time). Lockdown rules are power2's/sec2's and apply unchanged. A wrong
  passphrase at resume only retries/powers off: the image is not read or written before the unlock.
- **USB stick**: `build.py usb-image OUT --mbr hybrid|active|protective` (default hybrid: 0xEE over the GPT at LBA 1, the
  ESP as 0xEF and the root active, for BIOSes that only boot a disk with an active partition; UEFI/Linux/NexusOS keep
  using the GPT).
- **Tests**: `userland/tests/test_fatfsck.py`, `tests/test_x64_fatcheck.py WORK [--cases repair,checkonly,force,orphans,
  crash,powerloss,dosfstools] [--rounds N]` (dosfstools: Debian `fsck.fat` cross-check), `tests/test_x64_fatdir.py`,
  `tests/test_x64_cryptroot.py` (cryptsetup-made LUKS2/LUKS1 volumes, wrong passphrase, USB keyboard, lockout, dialog
  screendump, 9 hostile headers, hibernation refusal), `tests/test_x64_crypttab.py` (German-layout root passphrase;
  crypttab keyfile/passphrase/mode refusal/nofail/noauto/readonly/discard + Debian cryptsetup round trip; hibernate into
  the encrypted root, 3 wrong passphrases at resume, resume with memory intact, no plaintext on disk),
  `tests/test_x64_osinstall.py --cases encrypt` (also: a German layout chosen in the session reaches the boot prompt;
  `usbboot` checks the hybrid MBR),
  `tests/test_x64_swappart.py`, `tests/test_x64_fstools.py --cases crypt` (CBC KATs, luksSuspend/Resume, throughput),
  `tests/test_x64_readonly.py` (now with `fsck.mode=skip`). `tests/debian_tools_vm.py` runs a script with the Debian
  storage tools. Host gotcha: plain sparse copies (seek past zeros) stay fully allocated on this NTFS; the tests
  deallocate zero runs with FSCTL_SET_ZERO_DATA (`debian_tools_vm.punch`).
- **Limits**: the boot check needs memory for the whole FAT (refused below half of free memory: read-only mount); orphans
  of a session that ends by a crash are freed at the next boot's check, not before; the encrypted installer copies the
  FAT32 stick root (an ext4 stick root is copied whole); keymaps cover the main block (no keypad digits, no IME);
  crypttab keyfiles must be on the root (no keyfile devices); LVM2, md RAID, dm-verity and dm-integrity: round 3 below.
  **Out of scope here** (no swtpm, no token, no hardware in this environment): TPM2 and FIDO2 unlock
  (systemd-cryptenroll tokens are ignored: the passphrase keyslots still work) and every physical test -- real USB
  sticks, real BIOS/UEFI firmware, real keyboards with non-US layouts, hibernation on a real PC.

## Wave 2 storage round 3: LVM2, software RAID (md), dm-verity, dm-integrity (stor2)

Native x86_64 only; verified in QEMU (WHPX, 1 and 2 CPUs) with Debian's own tools (lvm2, mdadm, cryptsetup-bin's
veritysetup, e2fsprogs, dosfstools: `storage-lock.json` gained lvm2, thin-provisioning-tools, mdadm and their
dependencies -- fetch them into the shared package cache when merging); nothing on a physical PC.
- **Device mapper targets for lvm2** (`arch/x86_64/dm_snap.inc`, included by `dm.c`): `striped` (up to 16 stripes,
  the length a multiple of stripes x chunk), `snapshot-origin` and `snapshot` with lvm2's persistent exception store (dm-snap-persistent's
  on-disk format: header, metadata areas, commit order data -> flush -> area -> flush; reloaded at activation with every
  entry checked; COW exhaustion invalidates the snapshot, `dmsetup status` "Invalid", the origin goes on). Target
  versions as Linux reports them (striped 1.6.0, snapshot-origin 1.9.0, snapshot 1.16.0). A refused table logs
  `[64] DM <name>: table refused, target <type> errno N params '...'` (crypt parameters never). `dm_kernel_create()`
  makes a mapping from dmsetup-style lines for kernel callers. Debian's lvm2 runs without udev (lvm.conf in
  `tests/debian_tools_vm.py LVM_SETUP`): pvcreate/vgcreate/lvcreate/lvextend/lvremove/vgchange/lvs/vgs/pvremove work;
  `/dev/<vg>/<lv>` are kernel links (`blkdev_node.c`), lvm2's unlink/symlink of them are no-ops (`kernel.c`).
  Not done here: cache segment types (dm-thin, raid1: round 4 below).
- **Root on LVM** (`lvm_boot.c`; `kernel.c` calls `lvm_boot()` right after `cryptroot_boot()` and `lvm_boot_late()`
  after crypttab): PV labels and metadata text (CRCs, the newest seqno, circular areas) are parsed in the kernel, linear,
  striped and snapshot LVs activated with lvm2's dm names/uuids, `root=/dev/mapper/VG-LV` or `root=/dev/VG/LV`,
  `rd.lvm.vg=`, `rd.lvm.lv=`, `rd.lvm=0`; LVM on LUKS (the Debian layout) works: the VG inside the unlocked volume is
  activated after the passphrase. Other VGs are activated before `/etc/fstab` (`/dev/VG/LV` sources work, `mount.c`).
  Hostile metadata (checksums, extents beyond a PV, 99 stripes, nesting, label CRC) is refused per VG/LV.
- **Software RAID** (`md.c`): RAID0 and RAID1 with v1.0/1.1/1.2 superblocks, `/dev/md0..md7` (md0-md3 at boot, others
  made on first use as Linux md_probe), mdadm's ioctls (RAID_VERSION, GET_ARRAY_INFO, GET_DISK_INFO, SET_ARRAY_INFO,
  ADD_NEW_DISK, HOT_REMOVE_DISK, SET_DISK_FAULTY, RUN_ARRAY, STOP_ARRAY(_RO), RESTART_ARRAY_RW, GET_BITMAP_FILE),
  `/proc/mdstat` (resync/recovery progress), `/sys/block/mdN/md/` and `md/dev-<member>/` (mdadm's `sysfs_read`),
  mknod in `/dev` (mdadm's `/dev/.tmp.md.*` nodes: `blkdev_node.c storage_mknod`, `vfs.c do_mknod` hunk) and opening
  block-special files from disk (`kernel.c`, O_DIRECT allowed for block nodes). RAID1: reads from an in-sync member
  (another after a read error), writes to all; a failing member becomes faulty and the array degraded; dirty at the
  first write, clean after 200 ms idle (Linux safe mode), at stop and at shutdown (`vfs.c disk_shutdown -> md_shutdown`);
  resync (unclean shutdown, new array) and recovery (spare into a missing role) run in the `md_resync` kernel thread
  with 8 MiB checkpoints. Members whose events lag are kicked and never start a second array. Boot (`md_boot()` first
  in the kernel.c line with cryptroot/LVM: md -> LUKS -> LVM -> verity stack): every set is assembled, degraded RAID1
  runs, `root=/dev/mdN` or `root=UUID=` of the filesystem on it; `md=0`/`rd.md=0`/`raid=noautodetect` disable it.
  Hostile superblocks (checksum, max_dev, data beyond the device or over the superblock, level 5, 99 disks, size,
  device number) are refused. RAID5/6/10 and bitmaps: round 4 below. Not done: reshape, IMSM/DDF.
- **dm-verity** (`dm_verity.inc`): the verity target 1.10 (versions 0/1, sha256, data/hash blocks 512..4096, the Linux
  tree layout; verified hash blocks cached per device), EIO on a corrupted block with the block named in the log and
  status "C", `ignore_corruption`, `restart_on_corruption` (immediate reset), `panic_on_corruption`,
  `ignore_zero_blocks`, `check_at_most_once`; writable tables, other algorithms, FEC and `root_hash_sig_key_desc` are
  refused. veritysetup format/verify/open/status/close work unchanged. **Root at boot**: systemd's `roothash=<hex>
  systemd.verity_root_data=<spec> systemd.verity_root_hash=<spec> [systemd.verity_root_options=restart-on-corruption,
  ...,hash-offset=N]` makes `/dev/mapper/root` (read-only) from the veritysetup superblock; `root=/dev/mapper/root`.
  **Secure Boot seam (not closed):** sec2's signed path verifies `KERNEL.BIN` against `KERNEL.SIG`, but the command line
  (EFI LoadOptions, `\EFI\NEXUS\CMDLINE.TXT`) is not covered by any signature, so a `roothash=` there can be replaced
  by someone with access to the ESP. Closing it needs the root hash inside a signed object: a signed command line (a
  UKI-style `.cmdline` section or a `CMDLINE.SIG` checked by `bootx64.c` like `KERNEL.SIG`), or a root hash built into
  `kernel.bin` (then `verity_boot` must ignore a command-line `roothash=` when `NEXUS_BOOT_KERNEL_VERIFIED` is set).
- **dm-integrity** (`dm_integrity.inc`, target 1.10): Linux's on-disk layout (superblock "integrt" v1-5 with
  fix_padding/fix_hmac, journal sections with commit ids, tag areas interleaved with 2^N data sectors); a zeroed
  superblock is formatted at the first activation like Linux, so `integritysetup format/open/status/dump/close` work
  unchanged (crc32c or sha256 tags, 512..4096-byte sectors). Modes J (journal), D (`--integrity-no-journal`) and R
  (`--integrity-recovery-mode`: unchecked, read-only). A tag mismatch fails the read (EIO; Linux EILSEQ), logs
  `[64] DM integrity <dev>: checksum failed at sector N` and counts in the status. Journal mode is synchronous here:
  each write goes to a journal section (flush), in place with its tags (flush), and the section is emptied (flush);
  activation replays a committed section and drops a torn one (`[64] DM integrity: journal replayed ...`), so after a
  power cut every block is old or new with a matching tag. Slower than Linux's batched journal (several flushes per
  write). Not done: bitmap mode (B), keyed hashes (hmac), journal_crypt/journal_mac, meta_device, recalculate, discards
  (accepted, not passed on), inline tags (v6), dm-crypt AEAD over integrity: refused with EINVAL. Compatibility with a
  journal left by a Linux kernel was not tested (no Linux kernel here): the layout follows Linux, the replay rule is
  NexusOS's.
- **Tests**: `tests/test_x64_lvm.py WORK [--cases tools,root,luks,verify] [--smp 1,2]` (+ `arch/x86_64/lvm_user.c`),
  `tests/test_x64_md.py WORK [--cases tools,root,degraded,readd,unclean,verify] [--smp 1,2] [--keep]` (+ `md_user.c`),
  `tests/test_x64_verity.py WORK [--cases tools,boot] [--smp 1,2]` (+ `verity_user.c`),
  `tests/test_x64_integrity.py WORK [--cases tools,powerloss] [--cuts 4]` (power cuts through
  `debian_tools_vm.run_script(kill_on=..., kill_delay=...)`). All need the storage tools
  (`NEXUS_STORAGE_PKGS=<dir with the pinned .debs>` when the shared cache lacks the new packages).
- **Results (2026-10-30, final kernel, WHPX)**: lvm 7/7 (tools; root smp1 `root=/dev/vgroot/root` + smp2
  `root=/dev/mapper/vgroot-root` with five hostile VGs; LVM on LUKS2 smp1+smp2; Debian verify), md 8/8 (Debian mdadm
  tools; RAID1 root smp1 `root=/dev/md0` + smp2 `root=UUID=` beside eight hostile superblocks; degraded; stale member
  kicked + re-add + recovery; power cut while dirty -> resync at boot; Debian mdadm/e2fsck verify), verity 6/6
  (veritysetup round trip with EIO, ignore, hash-block corruption, options, nine refused tables, restart; verity root
  smp1+smp2; EIO on a flipped root byte; restart-on-corruption at boot), integrity 6/6 (integritysetup tools incl.
  corruption/recovery/direct/sha256-4096/11 refused superblocks and tables; 4 power cuts in journal mode, every block
  whole and readable after each, 3 journal sections replayed). Final suite (w2_suite, one run, 18/18): storage 5/5,
  power 4/4, lvm, md, verity, integrity, cryptroot 48/48, crypttab 18/18, osinstall encrypt, fatcheck repair+crash
  14/14, swappart 2/2.
  Found and fixed on the way: an md boot deadlock (the uevent read the array under its lock), resync_offset 0 written
  at every clean shutdown, a kicked member started as a second array, md presets using up the 16 block-device slots
  (lvm2 then got ENOMEM creating its sixth mapping), /proc/mounts naming an md root /dev/root, an ext4 root on a
  read-only device mounted rw.
- **Limits**: (the 16 block-device slots of this round were lifted in round 4: 512.)
- **Merge notes (shared files, small hunks):** `kernel.c` (the md/cryptroot/LVM/verity boot line and `md_boot_late`/
  `lvm_boot_late` next to `crypttab_boot`; `file_unlink`/`file_symlink` kernel-link and mknod-name no-ops;
  `file_open_resolved` O_DIRECT bit and block-special files), `vfs.c` (`do_mknod` S_IFBLK under /dev, `md_shutdown` in
  `disk_shutdown`), `procext.c` (`mdstat`, " 9 md" in /proc/devices), `blkdev_node.c` (LVM dirs/links, md sysfs, mknod
  names), `mount.c` (kernel links as mount sources; an md root shows as /dev/mdN), `ext4.c` + `ext4_dev.c` (a read-only
  device -- the verity root -- mounts read-only: `ext4_dev_readonly()`), `cryptroot.c` (no LUKS name from
  `root=/dev/mapper/VG-LV`), `build.py` (lvm_boot, md in the storage sources), `storage-lock.json`/`storage_tools.py`,
  `tests/debian_tools_vm.py` (LVM_SETUP, gpt_disk/raw_disk, kill_on power cuts).

### Round 4 (stor2): 512 block devices, RAID5/6/10 and bitmaps, dm-thin, LVM raid1

- **Block devices** (`blockdev.c`): the 16-slot table is gone. Devices, their whole-disk views and hot-plug partition
  chunks live in 32-slot pages allocated as slots are first used (512 slots; pages never move, so readers on other
  CPUs stay safe); listed partitions 256 (boot scan 64); `/dev` node slots 1024 (`blkdev_node.c`); dm devices 255
  (`dm.c`). Static BSS shrank (the old per-slot arrays were static). md arrays are made on first use (no presets).
  `test_x64_lvm.py --cases many`: 24 LVs + 3 snapshots (36 mappings) by lvm2, then all activated at boot.
- **RAID5/6/10 and write-intent bitmaps** (`md_raid.inc`, included by `md.c`):
  - RAID5/6: layouts 0-3 (Linux raid5_compute_sector), P = XOR, Q over GF(2^8) in Linux's syndrome order. Reads go
    to the chunk's member, else the row is reconstructed (any 2 missing for RAID6). Writes are reconstruct-writes
    (read the row, rebuild what is missing, recompute P/Q, write data and parity).
  - Write hole, as Linux: an unclean shutdown leaves the array dirty and the next start resyncs parity; a dirty
    *degraded* RAID5/6 is not started (`md_mod.start_dirty_degraded=1` overrides).
  - RAID10: near layout (n2 ...); far/offset layouts are refused.
  - Internal bitmap (the "bitm" superblock + 1 bit per chunk, on every member, Linux md-bitmap format): a bit is set
    and flushed before its chunk's first write and cleared when idle and not degraded. Resync covers only dirty chunks.
    `mdadm --re-add` (ADD_NEW_DISK with MD_DISK_SYNC) of a member not older than events_cleared recovers only dirty
    chunks. A bitmap older than the array marks everything dirty.
  - `/proc/mdstat`, `mdadm --detail/-X/--examine` show level/layout/chunk/bitmap as on Linux.
  - Not done: RAID4, external bitmaps, reshape/grow, PPL/journal, write-mostly/write-behind.
- **dm-thin + thin-pool** (`dm_thin.inc`): lvm2's `lvcreate -T`, thin LVs (over-provisioned), thin snapshots, lvextend
  of the pool, lvremove; the persistent-data metadata format thin_check/thin_dump (thin-provisioning-tools 1.1)
  accept: superblock, btrees, mapping/details trees, data and metadata space maps with checksums.
  - Commits (on flush, after messages, when the pool table goes) rewrite the metadata into blocks the committed
    version does not use, then switch the superblock, so a power cut leaves one consistent version.
  - Messages: create_thin, create_snap, delete, set_transaction_id. Out of data space: EIO with error_if_no_space,
    else writes wait until the pool grows (`dm_thin_pool.no_space_timeout`, default 60 s); the pool takes a grown
    data device at resume.
  - Limits: 2^22 data blocks per pool, 1024 thin devices, mappings in memory; not done: external origins, discards,
    metadata snapshots (reserve_metadata_snap), the periodic 1 s commit.
- **preadv/pwritev(2)** (`kernel.c`, syscalls 295/296/327/328): thin_check reads with preadv (it failed with ENOSYS).
- **LVM raid1** (`dm_raid.inc`, the dm "raid" target 1.15.1 for `raid1`; lvm2's default for `lvcreate -m N`):
  - dm-raid's superblock in each `_rmeta` sub-LV ("DmRd", events, failed devices, recovery and resync offsets). A wiped
    one means a new set (resync unless `nosync`); stale, new, failed or `rebuild` legs are recovered. I/O and dirty/
    clean handling are those of md.c's RAID1; a `dm_raid` kernel thread runs resync, recovery, check and repair.
  - Status/messages as Linux: `lvs` Cpy%Sync, raid_sync_action, raid_mismatch_count; `lvchange --syncaction
    check|repair`; `lvconvert -m +1/-1`; `lvconvert --repair` after a lost PV (degraded activation).
  - Superblocks are written at presuspend while the sub-LVs are live. A uevent no longer probes a suspended dm device
    (`blkdev_node.c`): lvm2 renames suspended sub-LVs, and the probe's read would have waited for a resume the caller
    could not issue.
  - Not done: the write-intent bitmap in `_rmeta` (resyncs are whole), raid0/4/5/6/10 through dm-raid (mdadm/md.c
    serve those), the legacy "mirror" target, activation at boot (`lvm_boot.c` leaves raid LVs and hidden sub-LVs such
    as `_tmeta`, `_tdata`, `_rimage`, `pmspare` to lvm2).
- **Tests:** `tests/test_x64_md_raid.py WORK [--cases tools,writehole,hostile]` (RAID5/6/10 + bitmap with mdadm; power
  cuts with host-side parity/Q/copy checks; hostile layouts/chunk/far/disks/bitmap), `tests/test_x64_thin.py WORK
  [--cases tools,powerloss,hostile] [--cuts 2]`, `tests/test_x64_lvmraid.py WORK [--cases tools,repair,powerloss]`,
  `tests/test_x64_lvm.py --cases many`.
  Results (2026-11-06, final build, kernel.bin 2,051,658 bytes): lvm 9/9 (with many), md_raid 12/12, thin 8/8,
  lvmraid 5/5, md 8/8, verity 6/6, integrity 6/6, cryptroot 48/48, crypttab 18/18, osinstall encrypt 7/7, fatcheck
  14/14, swappart 2/2; w2_suite storage 5/5 and power 4/4. test_x64_suspend's gpu case failed once with host-side
  virglrenderer errors in QEMU's stderr ("Illegal resource 2"), then passed 3/3 on reruns of the same build.
- **Merge notes (round 4 shared hunks):** `blockdev.c` (paged device table, BLOCK_MAX_PARTITIONS 256 /
  BLOCK_BOOT_PARTITIONS 64), `blkdev_node.c` (SLOTS 1024; no uevent probe of a suspended dm device), `kernel.c` (preadv
  family), `lvm_boot.c` (hidden LVs). dm.c: DM_MAX 255, the size check accepts a lower device's inactive (bigger)
  table, as Linux checks at resume; the raid presuspend hook in DEV_SUSPEND; `dm_device_suspended()`.
- **Kernel size (watch when merging):** the USB stick (`build.py usb-image`, `usbimage.py`) keeps kernel.bin in a
  GPT "BIOS boot" partition: stage 2 (4 sectors), kernel.bin, and the NXBOOTD1 descriptor in the last sector. Until
  the wave-2 merge it was 2 MiB (`BIOS_SECTORS = 4096`, at most (4096 - 5) x 512 = 2,094,592 bytes); the merged
  kernel is larger, so it is now 8 MiB (`BIOS_START, BIOS_SECTORS = 2048, 16384`, ESP at LBA 18432; see the merge
  section). Above that, build_image raises
  "kernel.bin does not fit the BIOS boot partition": no stick, and `test_x64_osinstall` fails. The floppy, the ISO and
  UEFI boots are not bound by it (their limits: build.py `KERNEL_MAX` 2,946,560 bytes for the 2.88 MB floppy every test
  VM boots; linker.ld's 4 MiB image + BSS asserts, which uefi/bootx64.c `KERNEL_LIMIT` mirrors).
  - The round-4 code first pushed kernel.bin to 2,121,290 bytes and `test_x64_osinstall` (usbboot, install) failed. The
    storage code off the hot path is now compiled with clang's minsize attribute (a `#pragma clang attribute push`
    region in dm_snap/verity/integrity/thin/raid.inc, md_raid.inc, md.c, lvm_boot.c, and dm.c from `word()` on; the dm
    I/O path, aes.c and aes_ni.asm stay -O2): 2,051,658 bytes.
  - The fix when merged kernels outgrow it is raising `BIOS_SECTORS` (16384 = 8 MiB covers a 6 MiB kernel). Stage 2
    (boot2.asm) reads exactly `kernel_sectors` (16-bit, build.py writes the real count) from `kernel_lba`, with no
    partition bound; usbboot.asm loads only the 4 stage-2 sectors; ESP_START, ROOT_START, the descriptor and the hybrid
    MBR derive from the constant; osinstall.c copies the stick's partition offsets and sizes. Sticks made before, and
    disks installed from them, keep 2 MiB: nothing rewrites a BIOS-booted kernel after the install (nexus-update's A/B
    slots are UEFI), so those need a new stick and a reinstall, or a UEFI boot.

## Services, updates, System Upgrade, test paths (wave 2, pkg2, 2026-10-30)

- **Test harness, per run** (`tests/pkg_vm.py`): the test hook of sysinstall/debian_base test disks
  (`services/test-hook`) reads `nexus.testhook=PORT|none` from the kernel command line (QEMU fw_cfg
  `opt/nexus/cmdline`; `pkg_vm.Vm(hook_port=server.port)`, default `none`) and exports `NEXUS_TEST_HOST`; without the
  option (an older harness) it still fetches port 8979. `HookServer`/`Results` take a free port (`port=0`, `.port`;
  `@HOST@` in the hook and served `.sh/.conf/.sources/.list` files becomes `http://10.0.2.2:PORT`), `edit_overlay`'s
  qemu-nbd picks a free port, `Vm` refuses an image whose test-hook is the old fixed-port one unless it was refreshed
  (`refresh_services`). pkg-style VMs no longer need `pkgvm_lock.py` against each other (old trees still do).
- **Where data lives** (`arch/x86_64/nexus_paths.py`, tests use the `tests/nexus_paths.py` shim): every location is
  an environment variable, else this machine's `%USERPROFILE%\.nexusos\paths.json` (`nexus_paths.py show|set|unset`),
  else a default under `arch/x86_64/out/data/`: `NEXUS_RUNS` (work folders of tests run without one),
  `NEXUS_CACHE` / `NEXUS_<KIND>_CACHE` (BASE, DESK, APPS, WINE, WAYLAND, WINE_APPS, GPU, APT_PROXY, EXT4_DEBS; the
  older NEXUS_NET/STORAGE/MEM/POWER/HW_PKGS still work), `NEXUS_TEST_SOURCE` (a FAT32 desktop disk tests copy),
  `NEXUS_TEST_DISK` (a writable copy Aurora tests stage into), `NEXUS_CHROME_DEB`, `NEXUS_WINE_SOURCE`,
  `NEXUS_GPU_HARNESS`. No script or test of the tree names `C:\nexustest` or `D:\nexustest` any more. Nothing ever
  defaults to the launcher's disk: `refuse_user_disk()` refuses every `*/arch/x86_64/out/desktop64.img` even when a
  setting names it; the old publication checks that do run on it (`test_published_desktop.py`,
  `test_installed_desktop_flow.py`) need `--published-disk`. This host's `paths.json` points at the shared caches
  (`D:\nexustest\pkgs\*`, `C:\nexustest\fix\ws_*`), `NEXUS_TEST_SOURCE=D:\nexustest\fix\final_disk_fresh\desktop64.img`
  and `NEXUS_RUNS=D:\nexustest\runs`, so suites that relied on the old defaults keep working.
- **nexusd units** (`nexusd_units.c/.h`, included by nexusd.c): `.socket` (ListenStream/Datagram/SequentialPacket on
  unix paths, @abstract, IPv4 `PORT`/`ADDR:PORT`, IPv6 `[ADDR]:PORT`; Accept=no hands the listening sockets to the
  service as fds 3.. with LISTEN_FDS/LISTEN_PID/LISTEN_FDNAMES and stops polling them while it runs; Accept=yes starts
  `stem@N-PID-UID.service` per connection with the connection as fd 3 or stdin/stdout (StandardInput=socket), removes
  the instances afterwards, MaxConnections; SocketMode/User/Group, DirectoryMode, Backlog, PassCredentials, buffers,
  KeepAlive, ReusePort, RemoveOnStop, FileDescriptorName, Service=, trigger limit), `.timer` (OnCalendar with
  systemd's calendar syntax and shorthands, OnBootSec/OnStartupSec/OnActiveSec/OnUnitActiveSec/OnUnitInactiveSec,
  Persistent= with stamps in /var/lib/nexus/timers, RandomizedDelaySec, Unit=), `.path` (PathExists, PathExistsGlob,
  PathChanged, PathModified, DirectoryNotEmpty through inotify, MakeDirectory, trigger limit). `nexusctl list-timers |
  list-sockets | list-paths`, status/show per kind. Debian's own `.socket`/`.path` units whose service NexusOS ships
  itself (dbus, cups) are not used. Fixed on the way: ExecStop of `Type=oneshot RemainAfterExit=yes` units now runs.
- **systemd parity (round 2)**: a plain `ListenStream=PORT`/`ListenDatagram=PORT` is `[::]:PORT` taking IPv4 too
  (IPv4-mapped peers; IPv4 only when the kernel has no IPv6), `[::]:PORT` follows `BindIPv6Only=default|both|ipv6-only`;
  `ListenFIFO=` (made with SocketMode/DirectoryMode, opened O_RDWR, RemoveOnStop) on the kernel's new named pipes;
  `ListenNetlink=FAMILY [GROUP]` (systemd's family names; e.g. `route 16` = address changes); `Accept=yes` with
  anything but stream sockets is refused. Units: `/dev/null` masks, `NAME.wants/` + `NAME.requires/` folders,
  `TARGET.target.wants/` links in every systemd folder (`/etc` and `/usr/lib`, also for user managers), specifiers
  `%S %C %L %E %T %V %H`, `ConditionACPower=` and `ConditionVirtualization=`. **Debian's timers now run**
  (timers.target.wants links as deb-systemd-helper leaves them: apt-daily, apt-daily-upgrade, logrotate,
  dpkg-db-backup, fstrim, man-db once installed; a stamp in /var/lib/systemd/timers also counts for Persistent=). Their
  cron.daily twins still run under cron (they look for /run/systemd/system): harmless, each job keeps its own
  once-a-period stamp. apt-daily's `flock -w` needs POSIX timers (`timer_create`, kabi's posix_timers.c in this wave):
  on a kernel without them apt-daily fires and then fails ("flock: cannot set up timer"); the units test probes the
  kernel and accepts only that failure there. logrotate needed `MAP_POPULATE` (the same memory_map hunk as kabi's).
- **Named pipes** (kernel, `arch/x86_64/fifo.c` + pipe.c helpers, a hook in kernel.c's open): `mkfifo` nodes on the
  ext4 root and on tmpfs open as Linux does: O_RDONLY waits for a writer and O_WRONLY for a reader (O_NONBLOCK:
  O_RDONLY at once, O_WRONLY without a reader ENXIO), O_RDWR never waits; the pipe lives while a description is open;
  POLLHUP only after a writer left; lseek is ESPIPE; fstat is the inode's. Before, every open of a FIFO was ENXIO.
- **User managers**: `user@UID.service` (vendor unit, WantedBy=session) runs `nexusd --user` as that user when its
  session starts (`session UID` on /run/nexus/control, as Aurora sends at boot; desk2's multi-user sessions send it per
  login and `session-end UID` at logout). It drops the set-user-ID root at once, needs /run/user/UID (0700, made by
  the system manager), reads `~/.config/nexus/user`, `/etc/nexus/user`, `/usr/lib/nexus/user`, then the systemd user
  directories with their `*.target.wants` links (deb-systemd-helper --user, e.g. gnupg's gpg-agent sockets; a Debian
  `dbus.socket`/`dbus.service` is never loaded there: the session bus is NexusOS's dbus-session@UID),
  journals to `~/.local/state/nexus/journal/user.journal`, timer stamps in `~/.local/state/nexus/timers`, control
  `/run/user/UID/nexus/control` (0600; only the user and root): `nexusctl --user start|status|logs|enable ...`.
- **Journal rate limit**: per source (unit, else syslog identifier + pid, `kernel`): `RateLimitBurst=` records per
  `RateLimitIntervalSec=` (journald.conf, default 10000 per 30 s, 0 = off); the rest is dropped and counted, then
  "Suppressed N messages from SOURCE". nexusd's own records are never limited.
- **Release signing of core updates**: `python -I arch/x86_64/update_bundle.py release-key` made the NexusOS release
  key once (2026-10-30, fingerprint `4207CB22DE834487F91DEFD9C8D0DC16D884E93E`): private half in
  `%USERPROFILE%\.nexusos\keys\update\update-key.json` (override `NEXUS_UPDATE_KEYDIR`; never replaced without
  `--force`; **back it up** with the Secure Boot keys), public keyring `arch/x86_64/keys/nexusos-update.pgp`
  (+ `.fingerprint`), installed by sysinstall as `/usr/share/keyrings/nexusos-update.pgp`.
  `update_bundle.py release OUT.nxu --version V` builds a bundle of this tree's build: kernel + `kernel.sig` (signed
  now with the Secure Boot kernel key), Aurora, the db-signed loader; it refuses a private key that is not the
  tree's and verifies the result; `update_bundle.py verify BUNDLE` checks one as a system does.
- **Secure Boot with A/B**: the loader puts `nexus.secureboot=1` on the command line when the firmware's Secure Boot is
  on. `nexus-update apply` then refuses a bundle whose kernel has no `kernel.sig`, and checks a present one with
  openssl against `/usr/share/nexus/secureboot/kernel.crt` (installed by sysinstall from `uefi/keys/kernel.crt`) before
  anything is installed; the loader still checks every slot kernel before it runs (a broken or missing signature:
  immediate rollback) and the firmware refuses an unsigned slot loader (the first stage then boots the slot kernel).
  **FAT32 roots: A/B is refused cleanly**: `nexus-update` says the full system (ext4) is needed, the loader ignores
  `/boot/nexus` on a FAT volume ("slots on a FAT root are not used": no permissions there, anyone could plant a kernel).
- **System Upgrade (FAT32 disk -> full system)**: System Settings > System Upgrade (`aurora_settings_upgrade.c`) on the
  FAT32 disk asks for the account's password (hashed there), writes `~/.local/state/nexus/upgrade/request.json` (on a FAT32 root the desktop user can write only below /home/nexus and /tmp) and shuts down; the
  launcher (`build.py desktop`/`chrome`, before and after each session) runs `arch/x86_64/migrate_disk.py`, which
  shows its progress in the launcher window and refuses (nothing changed) when the disk is open elsewhere, the PC has
  too little room beside it (used + 1.5 GiB), or the request is malformed (the page then shows `failed.json`). It
  converts every file (fat2ext4: names, case, owners, times; profiles, Wine prefixes, settings), compares every file
  with the original (SHA-256), stages the Debian base and the services, sets the password, checks the ext4 volume, and
  only then renames: `desktop64.img` -> `desktop64-fat.img` (kept byte for byte), the new disk -> `desktop64.img`
  (Windows does not rename an open file: the renames wait out a handle that is still open, such as a virus scanner
  reading the new disk; `ext4lib.check_image` returns a checker that keeps the image open through a reference cycle,
  so debian_base/sysinstall/fat2ext4 now close it). Files of the guest-written disk are read with bounded cluster
  chains, and every request field is checked before it reaches the installer's settings.
  The first start installs the Debian base (the page shows the progress and the report). On an upgraded disk it keeps
  the accounts (`update-passwd` merges Debian's system users and groups into the old /etc/passwd and /etc/group: before
  this, base-files' `chown root:mail` failed and the first start stopped), keeps the old configuration files (dpkg
  `--force-confold`, the package's version beside it as `.dpkg-dist`), and gives the existing account the page's password
  and the admin groups; a failed first start is shown on the page, which then offers only the way back. It offers "Keep the Full
  System" (the old disk is deleted at the next start) or "Go Back to the Old Disk" (`desktop64.img` -> 
  `desktop64-full.img`, the FAT disk back, its request removed). Host side by hand: `python -I
  arch/x86_64/migrate_disk.py status|run|rollback|finish|pending [--disk D]` (run asks for a password without a
  request). **The user's own disk has not been upgraded** (only copies of the integrator's disk).
- **Wine: Notepad++ x86 no longer hangs** (not the FAT large-directory bug): the 32-bit process crashed in its UI
  thread inside winewayland's `wayland_wglMakeContextCurrentARB` (NULL+0x19: during wined3d's OpenGL probe the WoW64
  opengl32 handed the driver the context value 1), so its window stayed on screen without input or exit.
  `winewayland_build.py` now also patches `opengl.c`: the driver accepts only contexts it made (wglMakeCurrent,
  wglMakeContextCurrentARB, wglDeleteContext, wglShareLists, the share context of wglCreateContextAttribsARB; anything
  else fails with ERROR_INVALID_HANDLE) and never returns a context whose attributes or EGL context failed; wined3d then
  falls back. Rebuild + install: `python -I arch/x86_64/winewayland_build.py build`, then `... install DISK` (the
  user's disk still has the previous winewayland.so until that is run on it). `test_wine.py` (console, apps, gui):
  PASS 129 checks including npp32 open/edit/save/exit. **32-bit MSHTML**: `test_wine_dotnet.py` now also runs the
  32-bit `hh.exe` (syswow64) on KeePass.chm: the x86 Gecko is installed into syswow64 at its first use and the topic is
  rendered. **HTA host** (round 2): Wine 10.0's own `mshta.exe` is an upstream stub ("mshta.exe is a stub!", and
  mshtml's `RunHTMLApplication` a FIXME). `arch/x86_64/wine_mshta.c` is a small host: a window with the WebBrowser
  control (in place, as hhctrl hosts help), loads the `.hta`, runs its scripts, ends on `window.close()`
  (WindowClosing; exit code 0, 3 when closed from outside), takes the document's title, and runs `<body onload>`
  itself when Wine's mshtml lost the handler (an `<HTA:APPLICATION>` in the head makes the parser start the body early).
  Built for x86_64 and x86 by `winewayland_build.py build` (or `build-mshta`), marked as a Wine builtin, installed
  over the Debian stubs by `winewayland_build.py install DISK` (originals kept as `mshta.exe.debian`). Limit: objects
  that are not safe for scripting (Scripting.FileSystemObject, WScript.Shell) stay refused in an HTA, unlike Windows:
  Wine's mshtml checks every ActiveXObject against the zone policy and the safe-for-scripting category with no host
  override (its secmgr.c). `test_wine_dotnet.py` (now on a qcow2 overlay over the read-only disk, nothing copied):
  64- and 32-bit HTAs build the DOM, report over XMLHttpRequest (platform Win64/Win32) and close themselves.
- **Tests**: `test_x64_units.py WORK --base installed.qcow2 [--smp 1]` (sockets, timers, paths, user managers, rate
  limit, and the round-2 parity block: dual stack/IPv6-only/both, sequential packets, FIFO activation and the kernel's
  named pipes, netlink route events, Debian timers catching up at boot, vendor links, masks, user-unit links and
  NAME.wants, gnupg's real gpg-agent socket activation; two boots), `test_x64_update_sb.py WORK --base installed.qcow2 --accel tcg` (release signing on the host, the
  FAT32 refusal, five Secure Boot boots under OVMF secure-code + SMM: WHPX has no SMM, so TCG),
  `test_x64_upgrade.py WORK --source DESKTOP_DISK [--cache D]` (the page in two boots, the host side with its hostile
  cases, back and keep; round 2: a failing first start on an overlay - the page offers only the way back - and a
  "Go Back" followed by a power failure, the request only in the ext4 journal, still found by the launcher), and the
  pkg group (`test_x64_apt/printing/software/update` now on per-run ports).
- **Test disks (round 2)**: no test of this area copies an 8 GB image any more. `tests/x64_overlay.py` (the same file
  as sec2's) stages files into a qcow2 overlay over the read-only FAT disk (test_wine_dotnet); test_x64_upgrade, which
  must rename and convert a real raw disk, takes `pkg_vm.fat_used_copy()` (only the clusters the FAT uses, in an
  NTFS-compressed folder: about 2 GB for the integrator's disk) and leaves Wine, the apps and downloads out of its two
  upgrades (BIG; the product never leaves anything out).
- **test_x64_printing**: the test session's `~/.aurora/login.conf` has `lock_after=0` (Aurora's 300 s idle lock had
  locked the session during the 20+ minute command-line phase and hid the print dialogs); `--slow-cli N` makes the
  guest wait N s before the dialogs, and a check fails if the session was ever locked.
- **Results (2026-10-30, worktree ws_pkg2, WHPX unless noted)**: round 1: test_wine PASS 129 (console, apps, gui;
  npp32 included), w2_suite full 29/30 (test_x64_hda: the known click/mixing glitch under host load). Round 2 (final
  tree): test_x64_units PASS 96 with smp 2 and smp 1 (this kernel has no timer_create, so apt-daily's flock -w part
  is the accepted "needs POSIX timers" case); test_x64_upgrade PASS 49 (with the four edge-case checks);
  test_wine_dotnet PASS 28 (HTA 64/32 included); test_x64_update_sb PASS 48 (TCG); test_x64_printing --slow-cli 330
  PASS 52; w2_suite pkg 6/6 (services 98, apt 35, printing 51, software 27, update 42, migrate 30), desk 11/11.
- **Limits**: no `ListenSpecial`/`ListenMessageQueue`/`ListenUSBFunction`; Debian's cron.daily twins of the timers
  also run; HTAs cannot create objects that are not safe for scripting (above); the System Upgrade is done by the
  launcher on the host (a NexusOS on bare metal with a FAT32 root has no in-place converter yet); `migrate_disk.py`
  needs the old disk's used size + 1.5 GiB free beside it and keeps both disks until Keep/finish. Not exercised by a
  test: a rename refused by another program for longer than the 120 s wait. The user's disk has neither the upgrade
  nor the new winewayland.so / HTA host yet (`winewayland_build.py install DISK`).

## Multi-user sessions, logind, screen reader, portals, drag and drop (wave 2, desk2, 2026-10-30)

- **Sessions** (`aurora_session.c/.h`, `aurora_session_proto.h`, set-user-ID launcher `nexus_session_user.c` =
  `/usr/libexec/nexus-session` 04755): the greeter (`aurora_lock.c`) signs any account in; the launcher (only for a
  SEQPACKET pair made by pid 1) checks the password through `nexus-auth verify` (tally), refuses system/locked/nologin
  accounts, makes the home (0700, /etc/skel, XDG folders) and `/run/user/UID` (0700) at the first login, drops to the user
  (initgroups/setresgid/setresuid), binds the user's wayland-0/aurora-control/aurora-notify sockets AS the user and hands
  them to Aurora, starts the user's D-Bus session bus (nexusd `session UID`) and stays as the session agent (all of the
  session's programs are its children; logout = SIGTERM/SIGKILL every process of the user, runtime folder emptied).
  One session is active: other sessions' windows are parked, the clipboard is cleared on every switch, the control
  socket answers root + the active user only, the console devices follow the active user (kernel `seat.c`:
  `prctl(PR_NEXUS_SEAT)` by pid 1; input/fb/sound opens of other users fail; open input/fb descriptors of the previous
  user are revoked (ENODEV), like EVIOCREVOKE). Menu N > Switch User... / Log Out... (appended LAST: tests click rows by
  position), Tab on the lock screen = Switch User. **FAT roots** (the user's desktop disk): only the boot account (no
  set-ID bits, private-path rule in the kernel), documented; file privacy between users needs an ext4 root.
- **logind** (`nexus_logind.c` = `/usr/lib/nexus/nexus-logind`, unit `services/nexus-logind.service`, policy
  `services/org.freedesktop.login1.conf`, D-Bus activation): org.freedesktop.login1 Manager/Session/User/Seat subset
  (ListSessions/Users/Seats, GetSession*/GetUser*, Can*/PowerOff/Reboot/Suspend/Hibernate/HybridSleep,
  Inhibit (block/delay, 128/uid), PrepareForSleep/PrepareForShutdown, Lock/Unlock/Activate/Terminate, TakeControl /
  TakeDevice (revocable descriptors) / PauseDevice / ResumeDevice, properties). Seat truth = Aurora's `sessions`
  command; `authorize()` is the seam for sec2's polkit-like broker (now: active session's user or root).
- **Screen reader** (`aurora_reader.c/.h`, `aurora_a11y_bridge.c` = `/usr/libexec/aurora-a11y-bridge`,
  `a11y/orca-wayland`): Super+Alt+S (also on the lock screen) or System Settings > Accessibility > Screen reader starts
  Debian's Orca (`apt install orca`) for the active session's user. The bridge (1) exports the Aurora shell (menu bar +
  menus, Dock, Spotlight, Launchpad, greeter/lock screen, banners as notifications) as the AT-SPI2 application "Aurora"
  and (2) owns `org.freedesktop.a11y.Manager` (KeyboardMonitor) on the session bus: Orca 48's keyboard device
  segfaults without an X display unless that service exists. Aurora reports the session's keys to the bridge (never
  while locked / at the greeter) and keeps Orca's grabs (Insert/KP_Insert, keys while held, its keystrokes) from the
  applications. Keys go only to the bridge Aurora itself started (control-connection pid checked), and in it only to
  the Orca it started (KeyEvent unicast); speech-dispatcher is pointed at ALSA in the user's speechd.conf when there is
  no PulseAudio (Debian's default output, absent here: no sound, `spd-say -w` hung). Keyboard access to the
  shell: Ctrl+F2 menu bar, Ctrl+F3 Dock (arrows/Home/End, Enter/Space, Esc), Ctrl+Space Spotlight, F4 Launchpad.
  **aurora-app windows** (System Settings, Files, the dialogs, the authentication dialog) have no toolkit that speaks
  AT-SPI: the appui primitives report what each frame draws (`appui_a11y_note`, kernel/appui.c), `aurora_a11y_client.c`
  turns it into a tree (frame = window title, headings/labels, push buttons, entries, password_text, check boxes, list
  items, radio buttons; names from the text beside a control or inside a row; focus = field with the caret, else the
  selected row) and sends it on change to the bridge's `$XDG_RUNTIME_DIR/aurora-a11y-apps` (SEQPACKET, the user's own
  processes only); each window becomes a frame of "Aurora" (window:activate/deactivate, focus, children-changed).
  Entries have the **Text interface** (GetText, CharacterCount, CaretOffset, Get*AtOffset, attribute runs) and
  text-changed/caret-moved events (Spotlight's search field included); password fields (lock screen, greeter,
  `appui_px_secret_field`: authentication dialog, Settings > Users) are password_text and their text is never sent.
  The **setting is kept across logins** (`/home/nexus/.aurora/screen-reader.conf`, "UID 0|1", written by Super+Alt+S,
  System Settings and `set-screen-reader`): a session coming on screen unlocked gets its screen reader back
  ("AURORA READER AUTOSTART").
  Kernel fixes found on the way: `link()` inside /dev/shm (glibc `sem_open`: speech-dispatcher modules crashed;
  `shmfs.c shm_link_path`, hooks in `fsext.c` and `vfs.c`), and `sound.c ready_mask` clamps avail_min to the buffer
  (alsa-lib's rate plugin rounds speech-dispatcher's "buffer - 4" above the 48 kHz buffer: poll never woke).
- **Portals** (`nexus_portal.c` = `/usr/libexec/xdg-desktop-portal-aurora`, `portal/`): backend for Debian's
  xdg-desktop-portal: Settings (color-scheme/accent/contrast from System Settings > Appearance, SettingChanged),
  FileChooser (Aurora's own dialog `aurora-app filechooser`), Screenshot (permission prompt, remembered), AppChooser,
  Access, Inhibit (via logind), Notification; OpenURI/OpenFile through `/usr/share/applications/nexus-*.desktop` and
  `mimeapps.list` (GIO finds no handler without mimeinfo.cache, which needs desktop-file-utils; note a file:// URI
  passed to OpenURI.OpenURI asks for x-scheme-handler/file - files go through OpenFile with a descriptor). Not done:
  ScreenCast (PipeWire crashes without timerfd/signalfd/timer_create - kabi's area); `aurora_capture.c` already exports
  frames over the control socket ("frame"/"screenshot") for it.
- **Authentication agent** (`aurora_authdialog.c/.h`: `aurora-app authenticate ACTION [--app NAME] [--description
  TEXT] [--passfd N] [-- PROGRAM ARGS]`) for sec2's broker (nexus-polkitd on /run/nexus-polkit.sock: QUERY/CHECK) and
  pkexec: QUERY; AUTHORIZED = no dialog; CHALLENGE = a modal naming the application, what it wants to do and the
  action id, asking the user's own password; each try is a CHECK (the broker's 2 s delay runs in a child, the dialog
  keeps drawing), wrong ones show "N of 5 attempts left", 5 failures or "DENIED throttled" lock it with a countdown.
  Success: password to fd N and/or PROGRAM via /usr/bin/pkexec with $PKEXEC_PASSFD. Exit 0/1 cancel/2 no broker/
  3 not allowed/4 locked. Settings > Users (add, delete, make/remove administrator) goes through it when the broker
  answers (no "Your password" field then; the verified password still goes to nexus-auth, which re-checks it); action
  `org.nexus.accounts.manage` if the broker lists it, else pkexec's `org.freedesktop.policykit.exec`.
  **Secure attention key** (`aurora_sak.c/.h`): Super+Esc is handled before anything else sees the keyboard (clients,
  input method, the screen reader's grabs). On the genuine dialog a banner confirms it ("Genuine NexusOS
  authentication: APP asks for ACTION"); anything else - another window, a look-alike - brings the real lock screen up
  at once; on the lock screen it confirms that screen. "Genuine" is decided when the dialog's Wayland connection is
  made (pid via SO_PEERCRED: /proc/PID/exe is /bin/aurora-app, argv[1] "authenticate", no LD_PRELOAD /
  LD_LIBRARY_PATH / LD_AUDIT), so a process that connected first and then exec'd aurora-app does not pass; the dialog
  makes itself non-dumpable once connected (no ptrace / /proc/PID/mem from the user's other processes).
  **Integrator seams (ws_sec2)**: `nexus_polkitd.c` (+ add `{"org.nexus.accounts.manage", RULE_AUTH_ADMIN}` to POLICY[]),
  `pkexec_user.c`, sec2's `aurora_auth.c`/`.h` (`aurora_auth_verify_helper`), their desktop.py build lines and the
  sysinstall/unit lines that install pkexec 04755 + nexus-polkitd and start the broker as root [all done at the
  wave-2 merge]. Without the broker everything behaves as before. Test: `tests/test_aurora_polkit_agent.py` (the
  tree's own out/nexus-polkitd.elf + out/pkexec.elf; `--polkit-dir DIR` for others).
- **Clipboard history** (`aurora_clipboard.c/.h`, picker `aurora_clipboard_app.c` = `aurora-app clipboard`, Super+V):
  Aurora reads every text selection an application sets (as a clipboard manager does), keeps 25 per user (256 KiB
  each) in memory only, forgets a user's at logout; the picker puts an entry back (Aurora serves it), Delete removes,
  C / "Clear History" clears (serving an entry back never raises SIGPIPE in Aurora: the write ignores it). Never kept: copies offering `x-kde-passwordManagerHint` (password managers), copies made
  while the focused text input is a password/PIN/sensitive field (text-input-v3), anything while locked; Aurora's log
  never shows clipboard text. Control: `clipboard-history`, `clipboard-use N`, `clipboard-remove N`, `clipboard-clear`.
- **Sleep acknowledgement** (power_aurora was 2/4 vs base 4/4): (a) the sleep path read login.conf, /etc/passwd and
  /etc/shadow inside the kernel's 500 ms sleep_delay window, and while another task (Chrome) owns the disk path every
  non-I/O-free syscall waits at blockio's I/O barrier -> missed acknowledgement; `aurora_lock.c` now keeps the options
  and "has a password" (refreshed at start, reload-login, unlock, session change, every 30 s) and the sleep path reads
  no file ("PREPARE SLEEP locked=0 in 0 ms"); (b) `aurora_power.c` printed "sleep acknowledged" after writing
  sleep_ack, but the kernel freezes the writer as that write returns, so the line (the test's sync point) was lost
  although the machine slept correctly; now printed first. Kernel `pm.c`: when the delay ends without an ack it prints
  "PM: no sleep_ack after N ms (pid 1 held at the I/O barrier | not ...)" and, while pid 1 sleeps at that barrier,
  extends the delay (up to 5 s) - helpers `task_at_channel` (process.c), `block_io_barrier_channel` (blockio.c).
- **Other desktop work**: wl_data_device v3 drag and drop between Wayland clients (actions, finish, cancel, Esc);
  `zwp_idle_inhibit_manager_v1` (video players keep the screen awake; logind idle inhibitors too); notification history
  per user + Do Not Disturb + Notification Centre (menu-bar bell, `aurora-app notifications`, control `notices`,
  `set-dnd`); System Settings > Appearance (Default/Light/Dark, accent) writes `~/.config/aurora/appearance.conf` + GTK
  settings.
- **X11 programs as Aurora windows (rootless Xwayland, wave 2 xwm)** (`aurora_xwm.c/.h`): Aurora owns an X display
  (`/tmp/.X11-unix/X0` + its abstract twin and `/tmp/.X0-lock`; N = 0 or the first free of 2..32, `:1` stays the rootful
  `nexus-x11-session`'s) and starts `Xwayland :0 -rootless -wm FD -listenfd FD -auth $XDG_RUNTIME_DIR/aurora-xauth-0`
  as its child on the first X connection (MIT cookie, 0600; an Xwayland that exits is started again by the next
  connection, at most every 3 s). Aurora is the X11 window manager on the -wm socket with its own X protocol client
  (no libxcb): SubstructureRedirect + Composite manual redirection on the root; MapRequest -> properties -> a frame
  window (title bar with traffic lights and title drawn with macui and sent by PutImage, 4 px resize borders; none for
  `_MOTIF_WM_HINTS` without decorations or splash screens) -> an Aurora window (dock, Alt+Tab, workspaces, menu-bar
  title), matched to its wl_surface by `xwayland_shell_v1` serials (only Aurora's Xwayland sees that global) or by
  `WL_SURFACE_ID` (`shell=0`, Xwayland before 23.1). desktop.c's X11 role: a server-side placeholder role resource
  (never announced) in `s->role` + `s->x11`, so every window path works unchanged; `configure()` ->
  `aurora_xwm_configure` (frame + client ConfigureWindow, synthetic ConfigureNotify), close -> WM_DELETE_WINDOW else
  XKillClient (Alt+F4, menu, the red light, shutdown); on the frame: title drag moves, double click or the green light
  zooms, the yellow one minimizes (WM_STATE Iconic, the window stays mapped for the dock picture), borders resize; the X
  frame position follows Aurora's at every paint. Focus: SetInputFocus (WM_HINTS input) + WM_TAKE_FOCUS +
  _NET_ACTIVE_WINDOW; X stacking follows Aurora's order; WM_TRANSIENT_FOR dialogs are centred on and kept above their
  window (modal ones take its focus); override-redirect windows and non-OR menu/tooltip/notification types are drawn
  unmanaged at their X position above all windows and never take the focus. ICCCM/EWMH: _NET_SUPPORTED,
  _NET_SUPPORTING_WM_CHECK ("Aurora"), WM_S0 + _NET_WM_CM_S0, _NET_CLIENT_LIST[_STACKING], _NET_WM_STATE requests
  (fullscreen, maximized; HIDDEN/MODAL/FOCUSED kept), _NET_WM_MOVERESIZE (CSD clients), _NET_CLOSE_WINDOW,
  _NET_FRAME_EXTENTS + _NET_REQUEST_FRAME_EXTENTS, _NET_WM_ALLOWED_ACTIONS, WM_CHANGE_STATE, WM_NORMAL_HINTS (min/max,
  USPosition/PPosition, win_gravity), WM_HINTS (input, initial iconic), _NET_WM_NAME/WM_NAME (cleaned UTF-8),
  WM_CLASS -> app id (class, lower case), _GTK_FRAME_EXTENTS -> window geometry, _NET_WM_PID (shown by `ctl x11`),
  _NET_WM_ICON (the smallest picture of at least 64 px, else the largest, scaled into the dock entry of an X program
  that has no Aurora icon, and into the corner of its minimized-window picture).
  CLIPBOARD <-> the Wayland selection both ways (XFixes, TARGETS, MIME-named targets, INCR both ways, 64 MiB cap;
  only while an X window has the keyboard, as for Wayland clients); X copies enter the clipboard history. Hostile
  input: SendEvent-forged WL_SURFACE_SERIAL/ID ignored, property reads bounded, transient loops refused, at most 4096
  X windows tracked. **DISPLAY is opt-in**: Wine 10 tries winex11 before winewayland whenever DISPLAY is set (the
  tested Wine path is winewayland), so Aurora gives DISPLAY/XAUTHORITY only on request: `nexus-x11-session --rootless
  PROGRAM ARGS`, a launch-file line `X11=rootless`, `$XDG_RUNTIME_DIR/aurora-x11.env`, or `export=1` in
  `~/.aurora/x11.conf` (`rootless=0` turns the server off, `shell=0` forces WL_SURFACE_ID). `aurora-app ctl x11` lists
  the display, Xwayland's pid and every X window. Wine started with DISPLAY set (winex11) is managed like any X program;
  Firefox with GDK_BACKEND=x11 (GTK 3) draws its own title bar (managed without an Aurora frame).
  Test: `tests/test_aurora_xwm.py` (overlays only: x11-tools installed into a copy-on-write view of the integ desk
  disk; `xwm_test_user.c` = /bin/xwmtest drives an X session + a Wayland window; smp 1 and 2, then a WL_SURFACE_ID boot;
  xterm, xeyes, Wine's notepad (winex11), Firefox under X11). `tests/desk_overlay_run.py WORK TEST.py` runs any
  desk-group test on qcow2 overlays only (its staging goes into a copy-on-write view of the read-only desk disk).
  Verified (WHPX, one VM at a time): run 2 of test_aurora_xwm.py RESULT PASS 230 checks 0 failed (1 CPU, 2 CPUs and the
  WL_SURFACE_ID boot: map/title/dock, click focus, move/resize -> synthetic ConfigureNotify, min hints, maximize and
  fullscreen from the client and the frame, minimize/restore, WM_TAKE_FOCUS, transient above parent, override-redirect
  menu, CLIPBOARD both ways, WM_DELETE_WINDOW and XKillClient, hostile clients, an aborting client, Xwayland killed and
  restarted, xterm, xeyes, Wine notepad via winex11). The later build (hardening + INCR test) passed the same on 1 CPU
  except three test-client faults fixed in the source afterwards (stale INCR PropertyNotify, xterm exit race, Firefox
  CSD expectation); 400 kB X -> Wayland via INCR passed, Firefox under X11 was managed.
  Limits: one Xwayland for the boot user's session (other users' X clients cannot read its cookie; their sessions
  have no X display); PRIMARY is not bridged (Aurora has no primary-selection protocol) and neither is XDND <->
  wl_data_device drag and drop (PRIMARY needs a zwp_primary_selection_v1 implementation in Aurora first, which it does
  not have; X programs share PRIMARY among themselves as always); frames are drawn light
  only (no dark appearance) and show no resize cursors; X content renders at scale 1 and is scaled by Aurora on HiDPI
  displays (as before); the WL_SURFACE_ID path was tested by forcing it (`shell=0`), not with an older Xwayland.
- **Tests** (all new): `test_x64_sessions.py` (launcher, seat, revocation, hostile records; `--smp 1,2`),
  `test_aurora_sessions.py` (Debian GUI: greeter sign-in, isolation, logind, switch, suspend with delay inhibitor,
  logout, PowerOff), `test_aurora_dnd.py`, `test_aurora_idle.py`, `test_aurora_portal.py`, `test_aurora_clipboard.py`
  (cliptest: kept/private copies, picker use/remove/clear), `test_aurora_polkit_agent.py` (+ SAK: genuine, LD_PRELOAD
  look-alike, other window, lock screen), `test_aurora_orca.py`
  (apt orca/speech-dispatcher/at-spi2-core; the shell's AT-SPI tree and events read by `a11y/atspi-watch`, Orca's
  speech of a GTK window and of the menu bar from its debug log, Orca+T, grabbed keys, sound in the WAV capture,
  lock-screen key privacy, Super+Alt+S, control commands, hostile cases; part 4: Settings and the authentication
  dialog as AT-SPI trees, a password typed into the dialog never exported, Spotlight's Text interface, the lock
  screen's field empty, log out + sign in -> AUTOSTART). Debian-base tests take `--base` =
  an installed qcow2 (`test_x64_services.py --keep-installed`).
- **Verified** (round 2, one final build, WHPX, one VM at a time): power_aurora interleaved against a built base
  (4 rounds, desk-disk copy with nexus/hiber.img): before the fixes mine 2/4, base 4/4; after them mine 4/4, base 4/4,
  every sleep acknowledged in 3-4 ms (was 3-58 ms); 6/6 more sleep-only runs; the suite's power_aurora PASS.
  orca 56/56 (part 4 included), polkit_agent 33/33 (SAK), clipboard 12/12, dnd 12/12, idle 4/4, portal 24/24,
  x64_sessions PASS (smp 1 and 2), aurora_sessions 51/51, auth_debian 25/25; w2_suite desk 11/11, full 28/30 + ext4
  rerun PASS (it failed only because its nested test_x64_display QMP unix-socket path reached 108 characters under
  the longer run folder) + test_x64_hda: the same single click at ~1.3 s in the first tone also on the base build
  (interleaved: mine 0/3, base 1/3, pre-existing), capture 1/1. Round 1: x64_sessions 45/45, gui sessions 51/51,
  orca 40/40, polkit_agent 25/25, desk 11/11, pkg 6/6, sec 9/9. Desk tests share the desk-disk copy: tests remove
  what they stage (idle/dnd/clipboard do).
- **Limits / open**: ScreenCast (PipeWire) and logind's polkit routing (after the merge); speech-dispatcher 0.12's ALSA
  output underruns between chunks by design: it waits until only 4 frames are left (avail_min = buffer - 4) before
  fetching the next chunk, and with stop_threshold = buffer the next write finds an xrun - the same on Linux, where the
  wake-up comes from the period interrupt at the moment the buffer runs empty (its "underrun at least N ms" figure is
  bogus because alsa-lib asks for monotonic timestamps and speechd subtracts them from gettimeofday); the second-client
  stall was the poll that never woke (avail_min above the buffer), fixed by the `sound.c ready_mask` clamp (hw2's
  file: the 3 lines are the comment + `uint64_t wake = ...` + `wake` in the two `avail(s) >= ...` tests). Wine 10.0's
  winewayland.drv binds no wl_data_device at all (no clipboard, no drag and drop: its sources are display/opengl/
  vulkan/wayland/keyboard/output/pointer/surface/window), so there is nothing to patch beside winewayland_build.py;
  Aurora's side (data device v3, test_aurora_dnd) is ready. Rootless Xwayland window management: done (the X11 bullet
  above; the rootful "X11 Desktop" session is unchanged). SAK: same-user code could still race a ptrace attach between the
  dialog's exec and its connection (the kernel reports TracerPid 0); the fully trusted path is a prompt drawn by
  Aurora itself. Logind's challenges still answer InteractiveAuthorizationRequired; switching users needs the target's
  password even for the previous user (macOS behaviour).

## ACPI and power management: uACPI, sleep, hibernation, cpufreq (2026-10-18, power agent)

The native x86-64 kernel now runs the firmware's AML through **uACPI 6.1.1** (MIT, vendored unmodified in
`arch/x86_64/uacpi/`, pinned in `arch/x86_64/power-lock.json`) and implements Linux-style power management end to end.
Only QEMU (q35/pc, TCG and WHPX, SeaBIOS and OVMF, 1 and 2 CPUs) was tested; **no physical PC, laptop battery, embedded
controller, MWAIT C-state or HWP/CPPC processor**.

### What exists
- **AML** (`acpi_os.c` kernel API: heap, sleeping mutexes/events, PCI/IO/MMIO, SCI via irq.c, deferred-work worker;
  `acpi_aml.c`): namespace load/init, Linux `_OSI` policy (`acpi_osi=` override), `_PIC(1)`, `_PRT` INTx routing with link
  devices and bridge swizzle (`irq_attach_pci`; MSI stays preferred), PCI root bridges, EC (ECDT/PNP0C09, `_Qxx`, `_GLK`,
  burst; protocol in `acpi_ec.c`), GPEs and GPE block devices, fixed power/sleep buttons and RTC events, `_PRW` wake
  devices (buttons and lid wake by default and keep their run-wake GPE enabled, `_DSW`/`_PSW` around sleep). Failure (or
  `acpi=off`, `acpi.noaml`) falls back to table-only mode, never to a failed boot.
- **Devices and policy** (`power.c`): AC/batteries in `/sys/class/power_supply` (Linux names, uevents, low/critical
  alerts, critical action hibernate else power-off), thermal zones in `/sys/class/thermal` (`_CRT` orderly power-off,
  `_HOT` hibernate, `_PSV` passive CPU cap, `_ACx` fans), backlight `/sys/class/backlight/acpi_video*`, lid, buttons
  (KEY_POWER/SLEEP/WAKEUP/BRIGHTNESS* on the keyboard evdev), uevents incl. `SUBSYSTEM=power PM_EVENT=<state>-prepare|post-<state>|shutdown-prepare`.
  Policy files `/sys/power/policy/{power_key,sleep_key,lid_switch,critical_battery_action,...,sleep_delay_ms,sleep_ack}`;
  `interactive` = the process that wrote it (Aurora) answers the key while it lives.
- **Sleep** (`pm.c`, `pm_asm.asm`): `/sys/power/state` freeze/mem/disk, `/sys/power/mem_sleep`, staged notify ->
  freezer -> driver `pm_ops` (NVMe, AHCI, e1000/e1000e, RTL8139, xHCI + USB re-enumeration, HDA/AC97, PS/2, Bochs DISPI,
  virtio-gpu: S3 refused while virgl objects exist) -> S3 with FACS waking vector and real-mode->long-mode trampoline,
  APs offlined/restarted, PCI/IOAPIC/MSI/LAPIC/PIT/MTRR/PAT restore, CLOCK_MONOTONIC continuous, BOOTTIME/REALTIME
  include the sleep. Wake: power button, `system_wakeup`, RTC alarm (`/sys/class/rtc/rtc0/wakealarm`, `/dev/rtc0`, rtcwake).
  A driver joins with one call at probe time (`power.h`): `pm_register(&ops)` (suspend may veto with -errno),
  `pm_register_storage(&ops)` (kept up while the hibernation image is written), or for plain hooks
  `pm_register_hooks("nic", native_nic_suspend, native_nic_resume)`. Shutdown/reboot do not go through `pm_ops`: a
  final NVMe shutdown notification belongs after the last flush in `kernel.c finish(0x10)`, `acpi.c
  platform_reboot_call` (before `acpi_reset`/`acpi_poweroff`) and `power.c power_orderly` (no-user-space branch).
- **Hibernation** (`hibernate.c`): area = MBR partition type 0xA0 / GPT type `C0B1A0B7-A548-4E4E-9F1D-4E5848494245`, else
  a preallocated `/nexus/hiber.img` on the FAT32 root (`dd if=/dev/zero of=/nexus/hiber.img bs=1M count=1024`). LZ77-
  compressed, CRC32-checked image of every used page; restored by the next boot (BIOS or UEFI) right after the disks are
  probed; refused and discarded when corrupt, written by another kernel, on changed hardware/memory map, or when the
  root file system changed meanwhile. `/sys/power/disk` platform|shutdown|reboot, reboot(2) `SW_SUSPEND`.
- **CPU** (`cpufreq.c`): intel_pstate (HWP), amd-pstate-epp (AMD CPPC MSRs, Zen 2+), acpi-cpufreq (`_PSS/_PCT/_PPC`,
  FFixedHW or SystemIO); governors performance/powersave(/schedutil); `/sys/devices/system/cpu/cpufreq/policyN`;
  cpuidle (POLL, C1 HLT/MWAIT, `_CST` states) in `/sys/devices/system/cpu/cpu*/cpuidle`.
- **Reboot/shutdown** (`acpi.c`): `_PTS/_S5` through AML, FADT reset register, then KBC/triple fault; reboot(2)
  POWER_OFF/RESTART/RESTART2/HALT/CAD_ON/CAD_OFF/SW_SUSPEND with Linux permission and PID-namespace rules.
- **Aurora** (`aurora_power.c` + small hooks in `desktop.c`, `kernel/menubar.c`, `kernel/macui.h`): power-button dialog
  (Shut Down/Restart/Sleep/Cancel, 60 s countdown, second press shuts down; Ctrl+Alt+Del opens it), Apple menu Sleep /
  Hibernate / Restart / Shut Down, menu-bar battery item with a panel (percent, charging, time left, brightness slider),
  low/critical notifications, full repaint after wake, SIGTERM = clean session end. Lock-screen hook: define
  `aurora_lock_for_sleep()` (weak in `aurora_power.h`); Aurora acknowledges `sleep_ack` after the next frame.

### Tests
`tests/test_x64_acpi.py [--only devices|lid|battery|thermal|fallback]` (test SSDT `tests/acpi_test_ssdt.py` on
pc-testdev: 20 hardware steps incl. cpufreq; lid close -> S3 with `_DSW` arming checked),
`tests/test_x64_suspend.py WORK [--smp N] [--uefi] [--only s3|rtc|button|freeze|devices|hda|e1000|rtl8139|ahci|gpu|rtcwake|hibernate|hibernate-corrupt|hibernate-kernel|hibernate-disk|hibernate-swsusp]`,
`tests/test_x64_aurora_power.py WORK --disk DESKTOP.img` (Chrome on a heartbeat page through S3 and hibernation; the
disk needs `/sbin/init` = `out/desktop.elf` and, for hibernate, a 512 MiB `/nexus/hiber.img`: stage a sparse copy with
`tests/stage_desktop_disk.py`), `tests/test_cpufreq_host.py`,
`tests/test_acpi_ec_host.py` (simulated processors / embedded controller).

### Limits
Real hardware unverified (EC, batteries, `_CST` MWAIT, HWP/CPPC MSRs, S3 firmware paths, ACPI NVS). The hibernation
image is not encrypted (a file area is readable like any file: prefer the 0xA0 partition). The image is written from
live memory with interrupts on (only the storage driver runs; `pm_snapshotting()` makes process.c defer timeouts and
alarms meanwhile, so no task moves between scheduler lists while pages are saved). virgl state cannot be saved
(sleep is refused while GPU objects live). Kernel text+rodata may now span the first 4 MiB (was 2 MiB); the floppy
image caps `kernel.bin` at 1,441,792 bytes (now ~1.0 MB).

## Networking at Linux level: IPv6, packet sockets, firewall, VPN, network settings (production round, 2026-10-19)

The native x86-64 kernel's network stack was extended to what Linux programs expect. Everything below runs under the
kernel lock (BKL) and is covered by QEMU tests on 1 and 2 CPUs. No physical NIC or PC was tested.

What exists (new files in `arch/x86_64/`; shared files only got small hooks):
- **IPv6** (`netdev.c route.c ipv6.c icmp6.c ndisc.c dhcp6.c net4.c`):
  - Addressing: EUI-64 and RFC 7217 link-local, SLAAC with RFC 8981 temporary addresses, DAD (nonce, collisions).
  - Neighbours and routers: NUD, router and prefix lists, RA MTU/RDNSS/DNSSL, redirects.
  - ICMPv6: echo, errors with Linux codes and pointers, rate limits, PMTU.
  - MLDv2 with v1 fallback.
  - Fragmentation and reassembly under the RFC 5722/6946/7112 rules and limits, and a robust extension-header walker.
  - Routing: tables, policy rules (fwmark, iif/oif, suppress_prefixlength) and RFC 6724 source selection.
  - Stateless DHCPv6.
  - TCP over IPv6 uses "address handles", so `kernel/tcp.c` stays IPv4-shaped.
  - `inet.c` gives full AF_INET6 sockets: v6only/dual stack, scope ids, IPV6_* options (PKTINFO, RECVERR, MTU,
    multicast), ping sockets and raw sockets with ICMP filters.
- **DHCPv4** (`dhcp64.c`):
  - RFC 2131 renew (unicast), rebind, expiry, NAK, INIT-REBOOT and release.
  - `/etc/resolv.conf` is generated from DHCP, RA RDNSS/DNSSL, DHCPv6 and static servers. Its mtime changes with its
    content, so glibc reloads it.
  - One-shot kernel packets wait up to 3 s for eth0's ARP instead of being lost (`net4.c` hold queue, Linux unres_qlen).
    These are DHCPRELEASE, nft TCP resets and ICMP rejects, and ICMP port-unreachable.
- **rtnetlink** (`netlink.c`):
  - Links, addresses, routes, neighbours, rules and netconf.
  - Link creation for tun, tap, dummy and wireguard, plus notifications.
  - `/proc/net/*` (`netprocnet.c`), `/proc/sys/net/*` (`netproc.c`) and SIOCGIF*/SIOCSIF* (`netioctl.c`).
  - NexusOS knobs: `/proc/sys/net/nexus/dhcp` (0/1), `/proc/sys/net/nexus/dns` (static servers, `search=`) and
    `/proc/sys/net/nexus/owner_raw`.
- **AF_PACKET** (`packet.c`): raw and cooked capture and send, classic BPF with a checker, memberships (promisc),
  AUXDATA, TPACKET_V1/V2/V3 mmap rings (mapped through `kernel.c memory_map` -> `packet_mapping`) and statistics.
- **Firewall** (`nft.c`, `conntrack.c`): nf_tables over nfnetlink for Debian `nft`.
  - Atomic batches: a failed message aborts the whole transaction, and a second socket's batch gets EBUSY.
  - Objects: inet/ip/ip6 tables, filter base chains (prerouting/input/forward/output/postrouting, ordered by priority
    across tables), regular chains with jump/goto, and rules placed by insert, add or position and deleted by handle.
  - Sets: named and anonymous (`{ 80, 443 }`), with elements.
  - Expressions: payload, meta, ct, cmp, bitwise, immediate, counter (live values in `nft list`), lookup, log, and
    reject (`with tcp reset`, ICMP/ICMPv6/icmpx codes).
  - `nft list ruleset` / `list chain`, and `-a` handles.
  - `create` of an existing table, chain or set returns EEXIST. The `destroy` commands are delete commands that ignore
    a missing object.
  - Refused with EOPNOTSUPP instead of silently ignored: anything else, such as nat/route chains, maps,
    interval/timeout sets, mangling (`meta mark set`), objref, limit, quota, non-counter objects, flowtables and
    `nft reset`. Unknown messages are never acknowledged.
  - Stateful connection tracking: NEW, ESTABLISHED, RELATED (ICMP errors included) and INVALID.
  - Legacy `kernel/firewall.c` is unchanged and serves the 32-bit OS only.
- **TUN/TAP** (`tun.c`, `/dev/net/tun`).
- **WireGuard in the kernel** (`wireguard.c`, `wgcrypto.c`):
  - Noise IKpsk2 in both roles, ChaCha20-Poly1305 transport, replay window, cryptokey routing, roaming,
    keepalives/timers and fwmark.
  - Generic netlink `nlctrl` plus the `wireguard` family for `wg`. Device dumps are split into <= 3776-byte messages,
    as Linux does.
  - Limits: 16 peers x 16 allowed IPs per device. Cookie replies (under-load mode) are not implemented.
- **Aurora**:
  - `aurora_network.c` is the Network settings app. It shows the status of every interface, route, DNS server and
    VPN, and sets DHCP, static IPv4, DNS, the firewall and the allowed TCP ports.
  - Settings are saved in `/etc/nexus/network.conf`. The firewall is built over nfnetlink, so `nft` is not needed.
    `/etc/nftables.conf` is written when it is absent or is ours.
  - `aurora_netstatus.c` is the menu-bar network item, with a VPN badge; a click opens Network.
  - At startup the desktop runs `aurora-app network --apply` once. That also loads a user's own `/etc/nftables.conf`
    with `nft` when nftables is installed.
  - Hooks: 2 in `kernel/menubar.c` (NEXUS_AURORA), 5 in `desktop.c`, 4 in `aurora_app.c`, and the file lists in
    `aurora.py`/`desktop.py`.
- **Debian tools**: `python -I arch/x86_64/nettools_install.py <disk.img> [--grow MIB] [--cache DIR]` installs iproute2,
  iputils-ping, tcpdump, nftables, wireguard-tools, net-tools and netbase.
  - 40 debs are pinned by URL, size and SHA-256 in `arch/x86_64/nettools-lock.json`.
  - The lock is resolved by `... nettools_install.py lock` from gpgv-verified InRelease files, reusing wine_install.py's
    verified pipeline.
  - The default desktop disk does not have these tools installed.

Policy decision (security-relevant): NexusOS has no root login, so the desktop owner holds two capabilities. The
desktop owner is uid 1000 in the initial user and PID namespaces.
- CAP_NET_ADMIN, always, for network configuration.
- CAP_NET_RAW while `/proc/sys/net/nexus/owner_raw` is 1 (the default). This covers raw and packet sockets, which ping,
  tcpdump and DHCP clients need.

Processes in any other user or PID namespace (sandboxes) and other users get neither; this is tested. Set owner_raw to
0 to withdraw raw access.

Tests (all `python -I tests/<name> <work dir> [cpus]`, private disk copies, ports 8943/8944 + QMP 4640/4641):
- `test_x64_ipv6.py`: slirp IPv6 plus host [::1] TCP/UDP.
- `test_x64_netwire.py`: this Python process is the whole network. It covers RA, SLAAC, DAD, MLD, the DHCPv4 lifecycle,
  DHCPv6, DNS, PTB, NUD, redirects, hostile IPv6 and IPv4 fragments, and carrier changes via QMP set_link.
- `test_x64_packet.py`: AF_PACKET, BPF and rings.
- `test_x64_tun.py`.
- `test_x64_wireguard.py`: an independent Python WireGuard peer. Ping and HTTP go through the tunnel, plus rekey and
  wg-quick policy routing.
- `test_x64_nettools.py`: the real Debian ip, ss, ping, tcpdump, nft (anonymous sets, insert/add/delete, a refused
  write) and wg (a full 16-peer device), plus ifconfig.
- `test_x64_firewall.py`: nft enforcement with conntrack, reject with an RST or ICMPv6 seen on the wire, and live
  counters.
- `test_aurora_network.py <work>`: a desktop disk copy. It checks the boot-applied static IP and the DHCPRELEASE on the
  wire, firewall enforcement from the wire, and a Network app screenshot.
- Host-side: `tests/wgcrypto_native_test.c` (RFC vectors) and `tests/{datagram,tcp,arp}_native_test.c`.

Limits and follow-ups:
- No WireGuard cookie/under-load mode.
- No loopback capture for tcpdump (`-i lo`).
- No mDNS.
- No NAT or forwarding chains (the box is not a router).
- Named counter objects can be created but no rule can reference them.
- Chrome and Wine IPv6 page loads were not run in this round.
- One unexplained miss: the first tunnel ping in `test_x64_wireguard.py` failed once in a full-suite run on an earlier
  build. It passed 5 times out of 5 in reruns. The test now prints wg0's counters and /proc/net/snmp if it happens again.

The kernel image is 1,013,482 bytes; the floppy limit is 1,441,792.

## Security: seccomp, sandboxes, namespaces, ASLR, hardening, Secure Boot (2026-10-23, `sec`)

Linux-level self-protection for the native x86-64 kernel. New files: `arch/x86_64/{seccomp,hardening,umip,namespaces}.c`,
`uaccess.h`, `{seccomp,aslr,umip,nsfs}_user.c`, `uefi/{sb_verify.h,sb_pubkey.h,secureboot.py,secureboot_efi.py,kaslr.py}`,
`uefi/keys/*` (public certs only), `tests/test_x64_{seccomp,aslr,umip,smap,nsfs,kaslr,secureboot,secureboot_fw,sandbox}.py`,
`tests/x64_overlay.py`. Boot option `sbxtrace` logs every sandbox setup step (`[64] SBXTRACE ...`: namespace probes and
clones, SBX_* env at exec, each seccomp filter install with its executable).

- **seccomp (complete).** `prctl(PR_SET/GET_SECCOMP)`, `seccomp(2)`: strict, classic-BPF filter (Linux validator rules,
  native-endian loads), GET_ACTION_AVAIL, GET_NOTIF_SIZES; flags TSYNC(+ESRCH), LOG, SPEC_ALLOW, NEW_LISTENER, WAIT_KILLABLE_RECV;
  all actions incl. TRAP (SIGSYS siginfo) and USER_NOTIF (RECV/SEND/ID_VALID/ADDFD); precedence, inheritance, int 0x80 =
  AUDIT_ARCH_I386. Checks run in Linux's order (copy program -> length -> NO_NEW_PRIVS/CAP_SYS_ADMIN in the caller's own
  userns -> validate), so the browsers' unprivileged `seccomp(..., NULL)` probe gets EFAULT. `/proc/PID/status` Seccomp.
- **Browser sandboxes are ON and verified.** Chrome (no `--no-sandbox`): chrome://sandbox ALL GREEN, "adequately
  sandboxed" - Layer 1 user+PID+network namespaces, Seccomp-BPF, TSYNC, Yama broker. Firefox 157: about:support shows
  Seccomp-BPF true, TSYNC true, User Namespaces true, Content Process Sandboxing true, level 6; content processes run in new
  user+IPC(+net) namespaces with the TSYNC content policy, no rejected syscalls. VS Code (Electron = Chromium's sandbox) does
  not fit the shared desktop image without an 8 GB grow and was not run (the test prints SKIP). `tests/test_x64_sandbox.py
  WORK [--apps chrome,firefox,vscode]` builds each app into a qcow2 overlay of the read-only desktop image (apps_install into
  a copy-on-write view + apps_postinst caches, `tests/x64_overlay.py`), boots with `sbxtrace`, asserts the namespace clones,
  SBX_* env, seccomp installs, no crash, a host page loads, and saves the chrome://sandbox / about:support screenshots.
- **Namespaces / nsfs.** `/proc/PID/ns/{user,ipc,net,pid,pid_for_children}` are magic links: access/stat/open/readlink
  through them work (this was why Chrome's "Network namespaces" row was red: access(F_OK) on ns/net failed), an open ns file
  keeps its namespace alive and works with `setns(2)` (net/ipc/user/pid, Linux capability rules: a userns root cannot setns
  back into the initial netns; owner-only user entry, not with a CLONE_FS-shared fs). CLONE_NEWNET/NEWIPC in clone/unshare
  (namespaces.c: refcounted slots owned by a user namespace); a non-initial netns is loopback-only (inet.c `route_check`:
  connect and sendto/sendmsg get ENETUNREACH, bind of a non-loopback address EADDRNOTAVAIL; sockets created there receive
  only loopback traffic and their listeners reset non-loopback connections; netlink.c `netlink_admin`/`net_raw_capable`
  are false there, so raw/packet sockets, TUN/TAP, WireGuard/rtnetlink/nftables changes and net sysctl writes are EPERM). IPC namespaces are trivially isolated (no SysV IPC/mqueue exists). Opening
  `/proc/PID/fd/N` of a pipe reopens the pipe by access mode (`/dev/stdin` on a pipe). Limits: net/IPC namespaces are
  per-process (unshare of them from a multithreaded process is EINVAL); netlink dumps still show the host interfaces. Test: `tests/test_x64_nsfs.py` (1+2 CPUs).
- **Yama** ptrace_scope=1 (ancestor / CAP_SYS_PTRACE / PR_SET_PTRACER), `/proc/sys/kernel/yama/ptrace_scope`.
- **ASLR (complete).** PIE/ld.so (20 bits, 2 MiB), mmap (28 bits), stack gap, brk gap (level 2), AT_RANDOM;
  `personality(ADDR_NO_RANDOMIZE)`, `/proc/sys/kernel/randomize_va_space`. Test: `tests/test_x64_aslr.py`.
- **Kernel hardening.** SMEP per CPU; stack protector with a boot-seeded canary (entry.asm); W^X audit at boot; UMIP ON with
  Linux-compatible emulation (`tests/test_x64_umip.py`); kptr/dmesg_restrict. SMAP: mechanism + `smap` boot option, OFF by
  default (151 direct user-access sites, incl. memory.c self-tests, still need uaccess windows - list in the sec notes;
  `tests/test_x64_smap.py` proves enforcement). Boot line `[64] Hardening: SMEP=on SMAP=present-off UMIP=on stack-canary=on`.
- **KASLR: NOT active.** The relocation half is done: `build.py` also links the kernel with `zig ld.lld --emit-relocs`
  (`out/kernel-relocs.elf`, `out/kernel-link.json`); `uefi/kaslr.py` extracts the 7719 in-image absolute relocations
  (R_X86_64_64 + R_X86_64_32S; entry.asm's physical PA() constants excluded) into a compact table and proves it against a
  link-twice-and-diff ground truth and a byte-exact relink at 5 slides (`tests/test_x64_kaslr.py`). Not done: applying a
  slide at boot (slide-aware kernel page directory in entry.asm, protect_kernel on it, `virt_to_phys` of kernel addresses,
  the BIOS 32-bit / UEFI C reloc loops, a pre-C `nokaslr` switch).
- **Secure Boot (firmware -> loader -> kernel, verified under OVMF secure-code.fd).** `uefi/secureboot.py gen|sign`
  (RSA-2048 PK/KEK/db + kernel key; private keys in `$SECUREBOOT_KEYDIR`, default `%USERPROFILE%\.nexusos\keys\secureboot`); BOOTX64.EFI
  verifies KERNEL.SIG (mandatory when the firmware reports Secure Boot). `uefi/secureboot_efi.py sign-pe|verify-pe|
  enroll-varstore`: Authenticode signing in edk2's format (messageDigest over the SpcIndirectDataContent contents, DER-sorted
  attributes, 8-aligned WIN_CERTIFICATE) - OVMF accepts the loader by its db certificate; hash enrollment stays as fallback.
  `python -I arch/x86_64/build.py secureboot` -> out/BOOTX64-signed.EFI, out/sb-vars.fd (certs), out/sb-vars-hash.fd,
  out/nexus64-uefi-signed.img. Tests: `test_x64_secureboot.py` (kernel signature), `test_x64_secureboot_fw.py` (cert-signed
  loader boots SB=1; tampered and unsigned refused; hash fallback). No TPM (no swtpm here).

## Audio capture: microphones and line-in (hw agent, production round wave 2, 2026-10-23)

- **ALSA capture** `/dev/snd/pcmC0D0c` (`sound.c`, Linux ALSA kernel ABI; `sound_abi.h`, `sound_hw.h`, `device.c`): PCM
  INFO stream 1, hw/sw params like playback (48 kHz S16 stereo in the kernel, alsa-lib's plug converts), READI_FRAMES
  and read(), mmap + SYNC_PTR, poll (POLLIN at avail_min), start threshold (a smaller readi waits, as on Linux), overrun
  = XRUN at stop_threshold (snd_pcm_recover works), pause (frames during the pause are dropped), drain (captured data
  stays readable, then SETUP), reset/rewind/forward, several recorders at once (one input ring, own positions) and full
  duplex. The input DMA starts with the first recorder and stops 2 s after the last; if it stops or fails the recorders
  get an xrun. Mixer controls through CTL ELEM_LIST/INFO/READ/WRITE/LOCK/UNLOCK/TLV_READ (TLV dB scale): "Capture
  Volume", "Capture Switch", "Mic Boost Volume" (HDA) or "Mic Boost (+20dB) Switch" (AC'97).
- **HD Audio** (`hda.c`): input converters routed through mixers/selectors to mic/line-in pins; source by jack sense:
  plugged mic > plugged line-in > built-in mic (fixed pin or no presence detect, e.g. a codec DMIC) > empty jacks, front
  mic before rear, re-checked when recording starts and every second while it runs (a headset plugged mid-call takes
  over). Mic bias VREF 80/50/100 %, pin boost one step, other mixer inputs muted; input stream descriptor 0 with a cyclic
  BDL over a 512 KiB ring, LPIB position with a 64-frame FIFO hold-back. When the playback controller has no input
  path (HDMI/DP monitor on a graphics card) the first other HDA controller with one (the motherboard codec) records,
  as a second controller instance. ALC897/ALC892: front/rear mic and rear line-in through mixer 0x23 to ADC 0x08.
- **AC'97** (`ac97.c`): PCM-in bus master kept cyclic (LVI moved behind CIV); mic input (`ac97.capture=line` for
  line-in), record gain/mute and +20 dB mic boost.
- Test-only boot option `snd.test-capture=counter|tone`: the captured samples become a frame counter or a 1 kHz sine
  (-12 dBFS) above the DMA (QEMU has no audio backend that records known data on Windows).
- **Tests**: `tests/test_hda_codec.py` (simulated controllers: QEMU duplex/micro, a Conexant-style laptop with headset
  hot-plug, the ALC897/ALC892 corpus, the user's PC with NVIDIA DP playback + ALC892 capture, controls, input DMA and
  LPIB wrap; 20/20 capture mutations killed), `tests/test_sound_abi.py`, `tests/test_x64_capture.py [WORK] [--smp 2]`
  (6 QEMU cases: AC'97/HDA counter, HDA tone, AC'97/HDA real input DMA, HDA without input; libasound blocking,
  non-blocking, mmap, poll, xrun, pause, drain, two recorders, duplex, simple mixer, plus Debian **arecord** -l, 2 s
  WAV, -M mmap, 16 kHz mono via plughw and its error exits), `tests/test_x64_capture_chrome.py` (real Chrome on Aurora:
  getUserMedia + MediaRecorder records 2 s Opus/WebM from the ALSA device; live spectrum and decoded recording at 1 kHz).
  `arecord` is Debian alsa-utils 1.2.14-1 (`aplay`), pinned in `arch/x86_64/alsa-utils-lock.json` (GPL-2+ userland tool,
  installed on test disks only).
- **Limits**: unverified on real hardware; one capture stream format per card in the kernel (48 kHz S16 stereo, the
  rest converted in alsa-lib); no Intel SST/SOF DSP microphones (PCH DMIC arrays on recent laptops); after S3 the
  separate capture controller needs the HDA probe re-run (power agent hook). Not started in this round (follow-up wave):
  USB Audio Class, UVC webcams + V4L2, game controllers, x2APIC ids > 254, root-bus BAR assignment.

## Bare-metal hardware readiness (hw agent, production round 2026-10-18)

Goal: the native x86-64 kernel on real PCs, first the user's AMD AM5 desktop (Ryzen 7 9700X, Realtek RTL8125,
ALC897 on AMD HDA 1022:15e3, AMD/NVIDIA HDMI audio, RTX 5070 + RDNA2 iGPU, AMD SATA, Kingston/Lexar NVMe, MT7922).
**Nothing here has run on real hardware yet**: drivers that QEMU cannot emulate are verified by register-level host
simulators (each with a mutation check) plus the full QEMU regression.

- **Ethernet** (`rtl8169.c` new, `e1000.c` rewritten, `netdrv.c/.h` new, `nic.c`): Realtek RTL8111/8168 g/h/ep/fp,
  RTL8125 A/B/D, RTL8126A (XID detection, MAC/PHY OCP, EPHY/ERI/CSI, rtl_nic firmware from `/lib/firmware` with pinned
  SHA-256 - `arch/x86_64/firmware_install.py <disk>` puts the 10 pinned files from `arch/x86_64/firmware/rtl_nic`;
  without them the chip still runs on its ROM PHY code), Intel I217/I218/I219 (PCH LPT..MTP: ULP/ME handshake, LANPHYPC,
  SMBus, ring flush) and I225/I226 (igc: SW_FW_SYNC, 2.5G). MSI-X -> MSI -> INTx via irq.c, DMA rings anywhere, interrupt
  mitigation, link speed/duplex to netlink (IFLA_CARRIER) and `/sys/class/net/eth0/{carrier,speed,duplex,operstate}`,
  TX-timeout reset. **Power agent**: `native_nic_suspend()`/`native_nic_resume()` (nic.c) are the NIC sleep hooks; there
  is no PM registry yet. Tests: `tests/test_nic_sim.py` (RTL8125B/8168h/8126, I219-V/LM, I226, 82574L).
- **HD Audio** (`hda.c`): Realtek ALC662-family coefficients (PLL/EAPD: ALC892/897), jack detection by unsolicited
  responses (2 s backstop poll), headphone automute, microphone/line-in capture (see the audio capture section),
  HDMI/DP: ELD (standard + AMD emulation verbs), NVIDIA DP-MST device select, slot mapping, audio InfoFrame. Output
  choice across controllers: analog jack plugged > HDMI/DP monitor with 48 kHz LPCM > analog > digital; override with
  `hda.output=hdmi|analog`. Tests: `tests/test_hda_codec.py` (corpus of 6 public alsa-info codec dumps, sources in
  `tests/fixtures/hda/*.txt`).
- **Display**: UEFI loader picks `video=WxH`, else the monitor's EDID preferred mode (EDID Active/Discovered protocol),
  else 1920x1080, else the firmware's mode; VM EDIDs are ignored unless `video=native`. BIOS (x86-64 stage 2 only):
  VBE/DDC EDID, native mode then 1920x1080, VBE 3 LinBytesPerScanLine. Kernel: RGB (XBGR) framebuffers, any pitch,
  sizes that are not a page multiple (1366x768 used to give no console), the low framebuffer remapped write-combining
  through PAT (the firmware's MTRR makes it UC), `[64] DISPLAY` line with MTRR type and EDID. Aurora swaps R/B for RGB
  framebuffers and keeps its wallpaper above 2048 columns / 8 MiB. **No native GPU driver**: on the RDNA2 iGPU and the
  RTX 5070 (Blackwell) NexusOS draws into the firmware framebuffer - one monitor, the boot mode, CPU rendering, no 3D,
  no mode switch; UI is not scaled (HiDPI is the desktop agent's). Tests: `tests/test_uefi_gop.py` (host),
  `tests/test_x64_display_modes.py` (1440p/4K GOP, BIOS EDID-native, BGR, pitch > width, Aurora at 1440p).
- **Platform**: TSC calibration falls back to HPET, then the ACPI PM timer when the PIT is gated/absent (AMD has no
  CPUID 0x15/0x16; this used to panic); bounded COM1 waits; NVMe: MSI-X first, APST off + PS0 (`nvme.apst=keep`), Do Not
  Retry honoured, all-ones detection, CC.SHN shutdown notification (`nvme_shutdown_all()` from `finish()`; **power agent**:
  call it before S3/S5/reboot too); AHCI: link power management off, 100 ms DET debounce up to 2 s (was 20 ms), RAID-mode
  controllers reported; xHCI up to 8 controllers, 16 hubs; unassigned BARs behind bridges assigned in the bridge window
  (`pcifix.c`; root-bus ones only reported). x2APIC: the kernel runs xAPIC (MADT ids > 254 skipped), MAX_CPUS 32 - the
  9700X's 16 threads fit; the IOMMU is left as the firmware set it. Tests: `tests/test_nvme_sim.py`,
  `tests/test_ahci_sim.py`, `tests/test_x64_hwready.py` (TSC sources, BAR fixup, diagnostics).
- **Diagnostics**: boot option `diag` (CMDLINE.TXT or the loader command line) or hold **D** at power-on (UEFI: USB
  keyboard, 300 ms window; BIOS: 0.5 s). The UEFI loader writes a firmware-only `\NEXUSDIAG.TXT` (GOP, EDID, ACPI, UEFI
  map, PCI, CPU); the kernel then replaces it on the boot medium (FAT volume with `EFI\NEXUS\KERNEL.BIN`) with the full
  report: CPU/APIC/MTRR/PAT, firmware memory map, ACPI tables, PCI tree with BARs/IRQ/MSI/PCIe link and the driver of
  each function, display/EDID, storage, network, USB and the kernel log (all probe messages and error codes), shows a
  summary and boots on after `diag.wait=N` s (15). From a USB stick the kernel cannot write (no USB storage driver):
  the loader's part stays and the screen says so.
- **MT7922** (groundwork only, no driver): firmware inventory in `firmware-lock.json` "inventory" (3 files, sizes,
  SHA-256, LICENCE.mediatek; not installed); `mt7922fw.c` validates the ROM-patch/RAM-code layout (not linked;
  `tests/test_mt7922fw.py [--firmware DIR]`). A driver needs: PCIe BAR0 WFDMA rings (TX 15/16 for MCU commands and
  firmware scatter, RX event ring), the patch semaphore + FW_SCATTER download of both images, MCU start, then the
  connac2 MAC/WTBL and an 802.11 + WPA2/3 supplicant; Bluetooth is a USB HCI device with vendor firmware download.
  End-to-end Wi-Fi/BT needs the hardware.
- **Boot options added**: `video=native`, `hda.output=hdmi|analog`, `nvme.apst=keep`, `tsc=hpet|pmtimer`, `diag`,
  `diag.wait=N`; test-only: `fb.test-bgr`, `fb.test-width=N`, `pci.test-unassign`.

## System services, Debian packages, printing, core updates, Wine .NET/MSHTML (production round, pkg agent, 2026-10-18..29)

A NexusOS installation on an **ext4 root** is now a real Debian 13 (trixie) system: the Debian base is installed by real
`dpkg` at the first boot, daemons run under a service manager with a persistent journal, `apt` installs/upgrades/removes
packages with their maintainer scripts, `sudo` uses PAM, CUPS prints (Print to PDF and driverless IPP Everywhere), and
core updates (kernel + Aurora + UEFI loader) come as signed bundles into A/B slots with automatic rollback. Only QEMU
(WHPX, q35, NVMe, e1000e) was used; **no physical PC**. The user's FAT32 desktop disk was not touched or migrated.

### What is where
- **New install / migration** (`arch/x86_64/sysinstall.py`): `install NEW.img [--source FAT.img] [--config K=V]` builds
  an ext4 install (the default file system for new installs): `userland/fat2ext4.py` converts the NexusOS userland,
  `debian_base.py stage` stages the Debian base (debootstrap-style first stage on the host: merged /usr, the
  Priority:required closure extracted, all 320 .debs of `base-lock.json` + the signed InRelease/Packages.xz copied),
  then `sysinstall.py services --aurora`. `migrate OLD-FAT.img NEW.img [--skip PATH]` does the same from a FAT32 desktop
  disk keeping **every** file (profiles included), compares every file afterwards (SHA-256, link targets, folders) and
  writes `NEW.img.migration.json`; the old disk is only read. FAT32 stores 8.3-compatible names without case: the case
  comes back from the Debian manifests, `userland/fat2ext4-case.txt` (Chrome/VS Code/XDG names, taken from a profile
  Chrome 155 made on ext4) and LevelDB/Chromium rules; names still unknown are listed in the report.
- **First boot**: `nexus-firstboot.service` runs `/usr/lib/nexus/bootstrap/stage2` (dpkg in dependency order,
  NexusOS `xdg-open` kept by dpkg-divert), then the installer settings (user + password via chpasswd -> yescrypt,
  groups sudo/lpadmin/..., host name, time zone, `locale-gen`, e.g. en_US.UTF-8 + hi_IN).
- **nexusd** (`arch/x86_64/nexusd.c`, `/usr/lib/nexus/nexusd` set-user-ID root on ext4; Aurora stays pid 1 as the user
  and starts it from `aurora_services.c`; only pid 1 or root may start it): systemd-style units from
  `/usr/lib/nexus/services` (vendor), `/etc/nexus/services` (admin) and Debian's `/etc|/usr/lib/systemd/system`;
  Requires/Wants/After/Before/Conflicts, templates, drop-ins, Type simple/exec/forking/oneshot/notify (+ReadyPath),
  Restart with doubling backoff and a start limit, User/Group/Environment/RuntimeDirectory..., conditions, session
  units. Journal `/var/log/journal/system.journal` (rotation, `/etc/nexus/journald.conf`) from unit output, syslog
  (`/dev/log`, sender pid from SCM_CREDENTIALS) and `/dev/kmsg`. `policy-rc.d` hands `invoke-rc.d` to nexusd (no SysV
  script ever runs). CLI `nexusctl start|stop|restart|status|enable|disable|logs ...` (socket `/run/nexus/control`).
- **Kernel** (new files `kmsg.c`, `timekeeping.c`, `uts.c`, `bootslot.c`; small hunks in kernel.c/process.c/
  credentials.c/ipc.c/procext.c/fsext.c/vdso.c): /dev/kmsg + syslog(2) (`dmesg`), settable/slewed CLOCK_REALTIME
  (adjtimex, clock_adjtime, clock_settime, settimeofday, vDSO `NXVDSO03`), sethostname/setdomainname, AF_UNIX
  SOCK_DGRAM, **O_PATH** (coreutils `cp/mv/install` into a folder failed with EINVAL before), close_range,
  sync_file_range, recvmmsg, the full rlimit table + RLIMIT_FSIZE/NPROC, prctl KEEPCAPS/SECUREBITS/CAP_AMBIENT, the
  Linux initial-stack string layout (setproctitle), `/proc/cmdline`, per-slot init. **uname now reports
  `Linux 6.12.0-nexusos-amd64`** (sysname was "NexusOS"; chrony and scripts parse it); `/proc/version` names NexusOS.
- **D-Bus** system + session buses (Debian dbus-daemon and its policy), `org.freedesktop.Notifications` (`nexus-notifyd`,
  D-Bus activated) shown as Aurora notifications; **chrony** NTP + `timedatectl`; locales; `sudo` + PAM
  (pam_unix/shadow, pam_limits). Apps' logs show only optional services missing (UPower for Chrome's battery API,
  org.a11y.Bus for LibreOffice); no login1 or portal request was seen. **avahi-daemon is installed but off**
  (`/etc/nexus/disabled`): it needs IPv4 multicast (IP_MULTICAST_*, IP_ADD_MEMBERSHIP, IP_PKTINFO).
- **CUPS**: "Print to PDF" queue (cups-pdf, `nexus-printers.service`), driverless IPP Everywhere queues by URI
  (`lpadmin -p X -v ipp://host/ipp/print -m everywhere`). Printing from Chrome (the desktop's
  `/nexus/chrome-desktop.args`), Firefox (Mozilla's release, as NexusOS ships it) and LibreOffice Writer print dialogs
  reaches a network printer with the document's text.
- **Software app** (`software_ui.c` -> `/usr/bin/nexus-software`, Aurora app 47 / Spotlight "Software"): search,
  install, remove, Installed filter, Updates (Check for updates, Update all) with progress; changes run
  `sudo /usr/lib/nexus/nexus-pkg-helper` with the user's password (wrong password refused and logged).
- **Core updates**: `arch/x86_64/update_bundle.py keygen|build|show` (format in its header: tar with MANIFEST, an
  OpenPGP detached MANIFEST.sig, kernel.bin, init, BOOTX64.EFI); `sudo nexus-update apply BUNDLE` (signature via
  `VERIFY_COMMAND` in `/etc/nexus/update.conf`, default `sqv --keyring /usr/share/keyrings/nexusos-update.pgp`; every
  file bound by size + SHA-256; regular files only; no downgrade without --allow-downgrade) installs into the other
  slot under `/boot/nexus/slot-a|b`. The UEFI loader (`uefi/abslot.c`: read-only ext4 reader, FAT through the
  firmware) boots it on trial, counts attempts in `/boot/nexus/bootcount` (rewritten in place), starts the slot's own
  BOOTX64.EFI (chained), and rolls back after TRIES failed boots, or at once when the slot's kernel is not a NexusOS
  kernel; `nexus-update-check.service` makes a trial that booted the default. The release signing key's public keyring
  is `arch/x86_64/keys/nexusos-update.pgp` (wave 2: `update_bundle.py release-key/release`). The `sec` agent can
  swap the scheme by changing the bundle's `Signature:`/signer and VERIFY_COMMAND only. BIOS floppy boots have no A/B.
  Secure Boot (merge of sec + pkg): a slot kernel is verified against its own `/boot/nexus/slot-X/kernel.sig` (the
  ESP's KERNEL.SIG only covers the ESP kernel): `update_bundle.py build --kernel-sig out/kernel.sig` ships it,
  nexus-update installs it; a present signature must verify, none is accepted only with Secure Boot off; an untrusted
  trial kernel rolls back, an untrusted default slot falls back to the ESP kernel (uefi/abslot.c ab_kernel_trusted).
- **Wine**: `wine_install.py --only wine,addons` (now the default groups) installs Wine Mono 9.4.0 and Wine Gecko 2.47.4
  (x86 + x86_64) into `/usr/share/wine/{mono,gecko}` (pinned in `wine-addons-lock.json` by the SHA-256 the signed
  libwine's appwiz.cpl checks): a new prefix gets .NET and MSHTML without a download prompt.

### Tests (tests/, WHPX; installed base = one compressed qcow2 made by the services run)
- `test_x64_services.py RUN --base-cache DIR [--keep-installed X.qcow2]` (new install, two boots: first-boot dpkg + 70+
  guest checks incl. hostile control requests, backoff/start limit, journal rotation, kmsg, rlimits, sudo/PAM, D-Bus,
  chrony against a host NTP server, locales incl. Hindi, coreutils into folders; persistence after reboot).
- `test_x64_apt.py RUN --base X.qcow2 [--smp 1]` (real archive via a caching proxy, signed test repo: maintainer
  script order, conffile, alternatives, adduser, daemon via policy-rc.d; bad signature/unsigned/tampered refused).
- `test_x64_printing.py` (host IPP Everywhere printer `tests/ipp_printer.py`, printer going away and back, user vs
  admin rights, Chrome/Firefox/Writer print dialogs), `test_x64_software.py` (GUI through QMP), `test_x64_update.py`
  (6 UEFI boots: refused bundles incl. symlink member, trial, chained loader, mark-good, immediate and counted
  rollback), `test_x64_migrate.py` (host only), `test_wine_dotnet.py` (NuGet, KeePass WinForms, HTML Help, iexplore JS).
- Tests stage the worktree's services into their own qcow2 (`pkg_vm.edit_overlay` + `refresh_services`, written
  through qemu-nbd: qemu-io's `write -s` reads its pattern file in text mode on Windows).

### Limits and follow-ups
- Debian's **firefox-esr 153 crashes** at start on NexusOS (NULL write in libxul right after `quotactl` ENOSYS /
  `prctl(PR_SET_SECCOMP)` EINVAL); Mozilla's release (apps_install.py) works. Likely needs seccomp (sec agent).
- No mDNS (avahi, above); `ippeveprinter` needs IPv6 (network agent). `/proc/mounts` still says vfat on ext4 roots.
- mshta (HTA): NexusOS's own HTA host since wave 2 (Wine 10.0's is a stub; unsafe ActiveX objects stay refused);
  32-bit MSHTML (syswow64 hh.exe + x86 Gecko) works since wave 2 (pkg2 section).
- Only ext4 installs get the Debian base/apt/services; FAT32 disks keep working as before until migrated.
- Socket, timer and path units, user managers and journal rate limits: see the wave-2 pkg2 section.

## Aurora desktop: displays, HiDPI, window management, text, input methods, lock, accessibility (production round, desk)

What: multi-monitor KMS (`arch/x86_64/kms.c`: virtio-gpu heads as DRM CRTC/encoder/connector/plane, EDID modes, dumb
buffers, SETCRTC/PAGE_FLIP/SETPLANE/DIRTYFB, DPMS, hot-plug uevents; with virgl card0 is now this KMS node next to
renderD128 (`gpu_user.c` checks GETRESOURCES there instead of "render-only"); GOP/fbdev-only machines stay one display) and an
extended Aurora desktop (`aurora_outputs.c`: layout, mirroring, per-display scale, persistence in
`/home/nexus/.aurora/displays.conf`; wl_output v4 + xdg-output per display; windows follow unplugged/shrunk displays).
HiDPI 100-400 % in 25 % steps: fractional composition in `desktop.c`, `wp_fractional_scale_v1`, the shell and Aurora
apps drawn natively at the physical resolution (`macui.c` scale layer, `aurora_text.c` FreeType/HarfBuzz/fontconfig by
dlopen with the embedded Aileron face `arch/x86_64/fonts/`, scaled cursor). Window management (`aurora_wm.c`):
xdg_wm_base v5 (tiled states, bounds, capabilities), Super shortcuts, Alt+Tab, 4 workspaces + overview (Super+W),
edge snapping, Super+D, dock pictures of minimized windows; `xdg_activation_v1`. Wine minimize: Wine 10.0's
winewayland had no minimize; `arch/x86_64/winewayland_build.py` builds Wine 10.0's `winewayland.so` (pinned tarball)
with `out/winewayland-minimize.patch` (set_minimized, SC_RESTORE on focus, xdg-activation restore) and installs it
(Debian's kept as `winewayland.so.debian`). International text: `arch/x86_64/desk_install.py` (`desk-lock.json`, signed
Debian indices) installs Noto (19 scripts + colour emoji) with a fontconfig rule and a prebuilt cache (`fccache`), a
Wine font-replacement `.reg`, libcrypt; Aurora draws any script (Notes, titles, menus). Keyboard layouts
(`aurora_keyboard.c`, Super+Space, menu-bar input menu via `menubar_status_add()`). Input methods (`aurora_ime.c`):
`zwp_text_input_v3` for applications, `zwp_input_method_v2` (keyboard grab, candidate popup under the text cursor) and
`zwp_virtual_keyboard_v1` for ONE input-method program that Aurora itself starts from `~/.aurora/input-method.conf`
(`program=/abs/path args`, over WAYLAND_SOCKET, restarted up to 5 times, not during shutdown); other clients do not
see those two globals (display global filter), so no application can grab the keyboard or inject keys. Greeter + lock
screen + users (`aurora_lock.c`, `aurora_auth.c`, set-user-ID helper `/usr/libexec/nexus-auth` = out/nexus-auth.elf,
faillock-style tally in `/var/lib/nexus-auth`; password checks: in process when /etc/shadow is readable, else the helper,
else Debian's `unix_chkpwd` for the session user - every libxcrypt hash, $y$ yescrypt included); accessibility (`aurora_a11y.c`: invert/contrast filter, magnifier
lens, larger text, reduced motion, sticky keys). System Settings pages: Displays, Users, Keyboard, Accessibility
(`aurora_settings.h` registry: one line per page). Kernel: `AT_SECURE` was always 0, so ld.so honoured LD_PRELOAD for
set-user-ID programs (root for anyone once a set-ID binary exists); now `credentials_exec_secure()` +
`disk_exec_secure()` set it (checked in `posixfs_user.c` and `tests/test_x64_auth_ext4.py`).

How to use: Settings app (Spotlight "System Settings", menu N > System Settings...). Shell: Super+L lock, Super+Space
next layout, Super+= / Super+- magnifier, Super+W overview, Super+arrows tile, Alt+Tab. Scripts: `aurora-app ctl CMD`
talks to the control socket `/run/user/1000/aurora-control` (`displays`, `scale NAME PCT`, `lock`, `lock-state`,
`prepare-sleep`, `resumed`, `keyboard`, `set-keyboard layouts=us,de`, `a11y`, `set-a11y key=0|1`). An input method:
`program=/abs/path args` in `~/.aurora/input-method.conf`. Disk extras: `desk_install.py install <disk> --only fonts,auth`, `desk_install.py fccache <disk> [--in-place]`,
`winewayland_build.py build` + `install <disk>`. On an ext4 root `sysinstall.py services --aurora` installs nexus-auth
(04755 root) and /usr/bin/aurora-app with the desktop; without the helper accounts stay read-only (sign-in still works).

Extension points for other components (keep each addition to one line in the shared file):
- System Settings page: write `const struct settings_page settings_<x> = {id, name, icon, enter, draw, click, drag,
  key, tick};` in a new `arch/x86_64/aurora_settings_<x>.c` (draw gets the content rectangle, click/drag
  content-relative coordinates; talk to the compositor with `settings_request()` over the control socket), add ONE
  line `&settings_<x>,` to `pages[]` in
  `aurora_settings.c`, the `extern` to `aurora_settings.h` and the file to `aurora.py`'s Aurora app sources.
  Pages now: displays, users, keyboard, accessibility (`aurora-app settings <id>` opens one directly).
- Menu-bar status item (network, battery, ...): `static const menubar_status_t item = {width_fn, draw_fn, UI_ACT_x};
  menubar_status_add(&item);` (kernel/menubar.h, Aurora build; up to 8, drawn right to left left of the search icon,
  draw gets x and the bar's vertical centre; a click runs the action; width 0 hides it).
- Control-socket command: `aurora_control_register(name, block, handler)` (aurora_control.h).
- Sleep/hibernate (power): `aurora_session_prepare_sleep()` before suspending (true once the lock screen is
  presented) and `aurora_session_resumed()` after, or the control commands `prepare-sleep` / `resumed`
  (aurora_lock.h documents the contract).

Tests: `tests/test_x64_kms.py [--smp 2] [--gpu-pci]`, `test_aurora_displays.py`, `test_aurora_wm.py`,
`test_aurora_hidpi.py`, `test_aurora_hidpi_apps.py`, `test_aurora_lock.py`, `test_aurora_fonts.py`,
`test_aurora_a11y.py`, `test_aurora_ime.py` (toy IM + text field: grab, preedit, popup position, Devanagari/CJK commits,
lock deactivation, hidden globals, role error, restart), `test_x64_auth_ext4.py` (set-ID helper on ext4: AT_SECURE,
throttling, admin rules), `test_aurora_auth_debian.py` (installed Debian root: chpasswd yescrypt password at the
greeter and lock screen, wrong one tallied, sudo agrees; Settings > Users change then sudo; unix_chkpwd fallbacks); `test_wine.py` checks minimize/restore (new `--stage-in-place` for a scratch disk).

Limits [wave 2 kabi: timerfd/signalfd exist now and fcitx5 runs - see "Linux ABI completeness"]: fcitx5/IBus are not installed: fcitx5 5.1.12 (Debian trixie: 52 packages, 68 MiB download, 139 MiB installed
for Pinyin + Mozc + Hangul + m17n/Hindi) runs on systemd's sd-event, which needs timerfd and signalfd - NexusOS has
neither (timerfd_create/settime/gettime 283/286/287 and signalfd/signalfd4 282/289 are ENOSYS); IBus also needs dbus-daemon + Python GI. Aurora's protocol side is what fcitx5's
waylandim frontend uses. Accounts can be changed only on an ext4 (POSIX) root (FAT roots: read-only /etc, readable
shadow); Orca: see the wave-2 desk2 section; GTK3/Xwayland/Wine render at integer scale and are scaled by Aurora at
fractional scales; slow keys not implemented; the magnifier is a lens, not full-screen zoom.

## Storage: USB disks, USB boot + installer, exFAT/ISO/loop/FUSE, device mapper + dm-crypt (production round, 2026-10-29)

Native x86_64 only (the legacy i386 OS is unchanged; `kernel/fat32.h` got one field and the `mnt_*` routing
prototypes, `kernel/*.c` still compile with the i686 toolchain). Verified in QEMU (WHPX and TCG, 1 and 2 CPUs); no
physical PC, no real USB stick.

### What it is
- **USB mass storage** (`scsi.c/.h`, `usbstorage.c`, `xhci.c`/`usb.c` bulk + streams): Bulk-Only and UAS (USB 3
  streams, USB 2 READ/WRITE READY), SCSI disk layer (INQUIRY, READ CAPACITY 10/16, MODE SENSE, READ/WRITE 10/16,
  SYNCHRONIZE CACHE, START STOP (eject), PREVENT, UNMAP, REPORT LUNS, sense decoding + retries), error recovery through
  the USB worker (reset recovery, endpoint halt clearing), multi-LUN, 4 KiB-sector media, hot-plug and surprise
  removal (I/O fails with EIO, mounts on the device are detached lazily, the desktop says "unsafe removal").
- **Block layer** (`blockdev.c`, `blkdev_node.c`, `fsprobe.c`, `storage_alloc.c`): Linux names (sdX, nvmeXnY,
  loopN, dm-N, partitions), whole-disk views, `/dev` nodes with the Linux block ioctls (BLKGETSIZE64, BLKSSZGET,
  BLKPBSZGET, BLKDISCARD, BLKRRPART, BLKPG add/delete/resize, HDIO_GETGEO, CDROMEJECT, ...), `/sys/block` with
  queue/, device/, holders/, slaves/, dm/, loop/, `/proc/partitions`, uevents on AF_NETLINK group 1 with ID_FS_*,
  `/dev/disk/by-{uuid,label,partuuid}` from a built-in blkid (LUKS1/2, ext2/3/4, exFAT, NTFS, FAT, ISO9660, swap).
- **Mounts** (`mount.c/.h`, `fatfs.c`, `exfat.c`, `iso9660.c`, `fuse.c`): mount(2)/umount2(2) (MS_BIND/MOVE/
  REMOUNT/RDONLY/NOSUID/NODEV/SYNCHRONOUS, MNT_DETACH/FORCE/EXPIRE), `/proc/mounts`, `/proc/self/mountinfo`,
  `/etc/fstab` at boot, users may mount hot-plugged media below `/media/nexus` (nosuid,nodev). Multi-instance vfat
  (FAT12/16/32, LFN, uid/gid/umask/showexec/flush), exFAT read/write (up-case, checksums, NoFatChain, ValidDataLength),
  ISO9660 with Rock Ridge and Joliet, loop devices (`/dev/loop-control`, LOOP_CONFIGURE/SET_FD/SET_STATUS64/
  CHANGE_FD/SET_CAPACITY/SET_DIRECT_IO, autoclear, partscan), FUSE 7.31 (`/dev/fuse`, fuse/fuse.<subtype>/fuseblk,
  fd=/rootmode=/user_id=/group_id=/default_permissions/allow_other, FUSE_DEV_IOC_CLONE, set-user-ID fusermount3 user
  mounts, ENOTCONN after the daemon dies, a signal ends a waiting request with EINTR).
- **Desktop** (`aurora_media.c`, `aurora_places.c`): plugged media are mounted at `/media/nexus/<label>` (vfat, exfat,
  iso9660 read-only, ntfs through ntfs-3g and ext4 through fuse2fs when installed), Files has a Drives sidebar with
  Unmount and Eject, notifications for mounted/unsafe removal.
- **USB boot image + installer** (`usbimage.py`, `usbboot.asm`, `boot/boot2.asm` LBA reads, `boot_info.c`,
  `osinstall.c`, `osinstall_user.c`, `console_prompt.c`): `python -I arch\x86_64\build.py usb-image OUT.img [--size MiB]
  [--root-from IMG] [--cmdline ...]` writes GPT (BIOS boot partition with stage 2 + kernel, a FAT32 ESP with
  BOOTX64.EFI, the root) for BIOS and UEFI; write it with Rufus (DD mode) or `dd if=OUT.img of=/dev/sdX bs=4M`.
  root= accepts /dev/..., UUID=, LABEL=, PARTUUID=, PARTLABEL=; `rootwait` waits for USB disks. `nexus-os-install DISK`
  on the live system (or the option `nexus.install=DISK`) copies it to the internal disk at the next boot after a YES
  on the console (FAT-aware copy, read back, new GUIDs, root=PARTUUID of the copy).
- **Device mapper** (`dm.c`, `aes.c`, `aes_ni.asm`, `keyring.c`): `/dev/mapper/control` (ioctl interface 4.48: VERSION,
  REMOVE_ALL, LIST_DEVICES, DEV_CREATE/REMOVE (deferred too)/RENAME/SUSPEND/STATUS/WAIT, TABLE_LOAD/CLEAR/DEPS/STATUS,
  LIST_VERSIONS, TARGET_MSG, GET_TARGET_VERSION), targets linear, crypt (aes-xts-plain64 and aes-xts-plain, hex keys or
  `:<size>:logon|user:<desc>` keyring keys, allow_discards, sector_size:512..4096, iv_large_sectors), zero, error;
  `/dev/dm-N` + `/dev/mapper/NAME`, sysfs dm/, holders/slaves. Requests hold a counted reference on the live table, so a
  reload or removal never frees a table under I/O. AES: AES-NI (FXSAVE/FXRSTOR around the XMM use) or a constant-time
  software AES (no secret-indexed tables); `aes_selftest()` checks FIPS-197 + IEEE 1619 vectors at boot and compares
  both paths ("[64] PASS AES/XTS known answers"); boot option `noaesni` forces the software path. Kernel keyrings:
  add_key/request_key/keyctl (user, logon, keyring; thread/process/session/user/user-session keyrings, user quota).
- **Debian tools** (`storage_tools.py`, `storage-lock.json`: 94 trixie packages pinned by size + SHA-256, InRelease
  checked with gpgv, licenses recorded): util-linux, mount, fdisk (sfdisk), parted, dosfstools, exfatprogs,
  e2fsprogs + fuse2fs, ntfs-3g, sshfs + openssh-sftp-server, xorriso, fuse3, cryptsetup-bin, dmsetup, keyutils.
- Shared-file hunks: `kernel.c` (mount/pivot_root/umount2 cases, O_PATH, pread/pwrite on block devices, device fcntl
  locks, unlink of /dev nodes = EPERM, stat reports ENOTCONN/EIO from mounts, keyring syscalls 248-250, FUSE writev,
  write-through, osinstall_check, uname
  release `6.12.0-nexusos-amd64` - **the pkg agent sets the same release with a bigger hunk: take pkg's**),
  `procext.c` (/proc/partitions, /proc/devices, /proc/misc), `device.c`, `sysfs.c`, `netlink.c`, `shmem.c` (nested
  write-back buffer), `blockdev.c/.h`, `vfs.h`, `ext4_dev.c`, `nvme.c` (serial), `input.c`, `boot_info.c/.h`,
  `xhci.c`, `usb.c/.h`, `desktop.c`, `aurora_extra.c`, `aurora.py`, `desktop.py`, `build.py`, `boot/boot2.asm`,
  `kernel/fat32.h`, `tests/test_wine.py` (Files clicks shifted by the 168 px sidebar).

### Using it
- `losetup`, `mount`, `mkfs.*`, `fsck.*`, `sfdisk`, `parted`, `blkid`, `lsblk`, `cryptsetup`, `dmsetup`, `keyctl` work
  as on Linux once the packages are on the disk (`python -I arch\x86_64\storage_tools.py image|fetch ...`). libdevmapper
  without udev: export `DM_DISABLE_UDEV=1` (the kernel creates and removes `/dev/mapper/NAME` itself; libdm notes that
  there are no SysV semaphores).
- Encrypted volume: `cryptsetup luksFormat --type luks2 /dev/sdb1`, `cryptsetup open /dev/sdb1 secret`,
  `mount /dev/mapper/secret /mnt`, `cryptsetup close secret`.

### Tests
`tests\test_x64_usbstorage.py` (bot, uas, uas-hs, bot-4k, multilun, smp1), `tests\test_x64_mount.py` (root, fuzz,
removal, fatroot, smp1), `tests\test_x64_automount.py` (15 checks on a qcow2 overlay of the desktop disk),
`tests\test_x64_osinstall.py` (usbboot BIOS+UEFI, install 9 checks incl. a sector-by-sector check of the copied root;
`desktop` needs ~5 GB and is not in the default list), `tests\test_x64_fstools.py WORK [--cases basic,fuse,crypt,
cryptsw,smp1]` (Debian tools on a 640 MiB ext4 root: fs_basic.sh, fs_fuse.sh, fs_crypt.sh; exFAT/ISO fuzzing;
cryptsw = the same crypt script with `noaesni`; smp1 = all on one CPU).

### Limits
- Encrypted ROOT is not done: no early unlock stage (passphrase prompt, LUKS2 header parsing, Argon2id in the kernel or
  an initramfs-style stage with pivot_root), and the installer has no "Encrypt disk" option. LUKS volumes other than
  the root work with cryptsetup.
- dm suspend does not hold back new requests (Linux queues them until resume); "key wipe" is refused while the device is
  open (so `cryptsetup luksSuspend` of a mounted volume fails with EBUSY); DM_DEV_WAIT returns at once; no dm-integrity,
  dm-verity, LVM targets, multipath, snapshots.
- No SysV semaphores (libdevmapper's udev synchronisation is off); no udev; `setpriv` fails (no PR_SET_KEEPCAPS).
- System images need the Debian layout bits the tools expect: `/bin/sh`, `/bin/mount` (libfuse runs it when
  `/etc/mtab` exists) and `/etc/mtab -> ../proc/self/mounts` (fusermount3 -u reads it); the fstools test image adds
  them (`storage_tools.py image`), the desktop disk should get them from its package setup.
- A disk carries at most 15 partitions here (sysfs ext_range 16; BLKPG beyond that: ENXIO/EINVAL).
- The software AES is slow (~1 MB/s); real CPUs with AES-NI use the fast path.
- `tests\test_wine.py` and `tests\test_x64_desktop_apps.py` were not run in this round: each needs its own multi-GB
  Wine/apps disk, which does not fit the 4 GB scratch cap.

## Swap, page reclaim, OOM killer; disk waits without the big lock (2026-10-29, production round: mem)

The native kernel now swaps the way Linux does. Anonymous memory, COW copies, shared anonymous memory, memfd and
`/dev/shm` pages go to swap. The page cache is reclaimed: clean pages are dropped, dirty pages are written back first,
and mapped pages are unmapped with a TLB shootdown. Kernel allocations get room by reclaim. An OOM killer kills a
process instead of the system panicking or hanging. A task sleeping in a disk wait no longer holds the big kernel lock.
Hibernation is not part of this (power agent).

### Using it
- `build.bat run` / `build.py desktop|chrome` create `/swapfile` once: `swap_tools.ensure_default`, up to 2 GiB, about
  half the free FAT space.
  - The file is contiguous clusters with an mkswap v1 header (random UUID, label `nexusswap`), listed in `/etc/fstab`.
  - It is written in place, zeroed sparse, so host space is used only when the guest swaps.
  - It never raises, and it is skipped while `subprocess.Popen` is mocked (the launch unit tests).
- `--no-swap` creates nothing and passes fw_cfg `opt/nexus/swap=off`; the kernel option `noswap` does the same.
- `swap_boot()` (swap.c) enables fstab swap entries before `/sbin/init`, with `UUID=`, `LABEL=`, `pri=`, `discard`,
  `noauto` and `nofail`.
- `python -I arch\x86_64\swap_tools.py swapfile <disk> [--size MIB|auto] [--grow MIB] [--path P] [--priority N] [--remove] [--copy]`
  - `--remove` records `/nexus/swap-declined`, so the launcher does not re-create the file.
  - The image is edited in place unless it grows or `--copy` asks for a transactional copy.
- `python -I arch\x86_64\swap_tools.py install <disk>` installs Debian trixie util-linux, mount and procps, for
  `swapon`, `swapoff`, `mkswap` and `free`.
  - 14 debs, pinned by size + SHA-256 in `arch/x86_64/swap-tools-lock.json`; InRelease checked with gpgv; `lock`
    regenerates the file.
  - The default cache is `C:\nexustest\prod\ws_mem_pkgs`. `status <disk>` reports the state.
- Kernel interfaces:
  - swapon(2)/swapoff(2) (167/168): CAP_SYS_ADMIN, SWAP_FLAG_PREFER, DISCARD accepted, Linux EPERM/EBUSY/EINVAL.
    swapoff checks first that MemAvailable plus the other areas' free slots can take the pages (else ENOMEM), then pages
    everything back in.
  - Swap files on FAT32 and ext4, with extents mapped once at swapon. Holes, inline data, unaligned runs, busy or
    mounted targets, directories, /proc and /dev are refused.
  - `/proc/swaps`, `/proc/meminfo` (full Linux field set), `/proc/vmstat`, `/proc/zoneinfo`.
  - `/proc/PID/status` VmSwap and State `D` for disk waits, `/proc/PID/smaps` + `smaps_rollup` (ptrace read access),
    `/proc/PID/oom_score`, `oom_score_adj`, and mincore(2).
  - `/proc/sys/vm/{swappiness, drop_caches, min_free_kbytes, page-cluster, watermark_scale_factor}`.
- Session trace lines:
  - every 5 s: `[64] MEMSTAT free_kb= swap_used_kb= swap_cached_kb= pswpin= pswpout= oom_kills= kswapd_wakes= disk_sleeps= io_barrier_waits=`.
  - OOM kill: `[64] OOM Out of memory: Killed process <global pid> (name) total-vm:... oom_score_adj:...`.
  - `[64] SYSCALL_ENOMEM ...` for fork/clone failures and the first 64 ENOMEM results (EFAULT while memory is low).

### Design (new: swap.h, swap.c, reclaim.c, vm_reclaim.inc, blockio.c; hooks in kernel.c, memory.c, shmem.c, process.c, smp.c, blockdev.c)
- **Swap entries.** A non-present PTE holds `(offset<<12)|(type<<2)|2`, and so does a shmem slot.
  - swap.c keeps the per-slot counts and the swap cache: one frame reference, reverse-mapped by a new per-frame aux word
    in memory.c. A page shared by fork is written once.
  - All I/O goes through the generic block layer. Read-ahead follows `page-cluster`; write-back merges contiguous runs
    in a 256 KiB DMA32 buffer.
- **Reclaim.** A clock over every address space's PTEs (Linux 2.4 swap_out style), with the accessed bit as the second
  chance.
  - PTEs are cleared in batches with xchg, then one `space_flush`, and only then are pages swapped or dropped.
  - A shared-file PTE's dirty bit goes to the page cache.
  - Foreign spaces are only trylocked. mlocked, framebuffer and device areas are skipped. Exec pages are kept below
    priority 8.
  - Watermarks per zone; `frame_alloc_user` leaves `min` to the kernel.
  - kswapd is a kernel thread. Direct reclaim goes through priorities 0..12, then the OOM killer.
  - A writer past 10% of RAM dirty writes back first.
  - Valid page faults retry after reclaim instead of failing with SIGSEGV/SIGBUS.
- **OOM killer.** Linux badness: RSS + swap + page tables, scaled by oom_score_adj. pid 1 and adj -1000 are excluded.
  No second victim is chosen while the first still holds its address space.
- **Kernel memory under pressure.**
  - A kernel-stack pool (process.c: one 68 KiB stack per 8 MiB of RAM, filled at boot before the baselines) keeps thread
    creation working when free memory is fragmented. Without it, Chrome's pthread_create failed.
  - fork reclaims room for its page tables and VMAs first (`reclaim_kernel_room`).
  - Reclaim and swap-in never sleep while holding a foreign address-space lock (`space_access`).
- **Write-back order fix (shmem.c write_run).** Shared mappings are write-protected and pages marked clean *before* the
  data is copied to disk (Linux page_mkclean order), so a store during the write re-dirties the page. The old order
  lost such stores, and reclaim then dropped the newer data.
- **Disk waits without the BKL (blockio.c).**
  - A task in a completion wait sleeps without the BKL. This covers `block_wait` for AHCI/NVMe, and `ata_halt_wait`
    plus a `kernel/ata.c` hook for long ATA waits.
  - The sleeping task is the *I/O owner* until its next non-I/O lock release.
  - Other tasks wait in `block_io_barrier`:
    - at system call entry, except the `syscall_io_free` set: signals, ids, time, futex, scheduling, poll/epoll,
      eventfd, socket and pipe I/O;
    - on resume from a sleep (a page fault clears the per-task io_free flag);
    - in file and swap faults, reclaim, kswapd, `space_access`, and the block layer.
  - Page faults (code page-in, mapped files, swap-in) wait the first millisecond of a request the old way
    (`SPIN_FIRST_NS`, `block_io_fault`). Sleeping on every short page-in read broke test_x64_hda's pause/resume timing
    (2 of 2 runs). Requests from system calls and write-back sleep at once, which is what lets other CPUs run beside
    heavy I/O.
  - Task-less BSP work (write-back, gc, reaping) is deferred.
  - The owner takes the BKL first when it wakes (`lock_urgent`).
  - A disk wait sleeps only with no spinlock other than its own vm_lock. `blockio.spin` restores the old behaviour.

### Tests
- `python -I tests\test_x64_swap.py <work> [--only api,stress,oom,ext4,debian] [--smp 1,2,4] [--repeat N] [--desktop-disk IMG]`.
  - It boots `arch/x86_64/swap_user.c` as `/sbin/init` (as root) in a 256 MiB guest.
  - stress: 2x RAM touched and verified; shared anon across fork; COW children; dirty MAP_PRIVATE file pages;
    MAP_SHARED file stores during fsync + reclaim; memfd and /dev/shm; mlock; swapoff ENOMEM, then swapoff under load.
  - oom: badness order, adj 1000/-1000, sentinel survives.
  - debian: Debian tools on a desktop disk copy, edited in place and booted snapshot=on.
- `python -I tests\test_x64_blockio.py <work>`: on NVMe, AHCI and IDE, compares a CPU-bound syscall-heavy process's rate
  during heavy synchronous disk I/O with `blockio.spin` (before) and without it (after).
- `python -I tests\test_x64_swap_chrome.py <work> --desktop-disk IMG [--no-swap] [--tab-mb N]`: 8 Chrome tabs fill and
  verify N MiB each (default 208) in a 2 GiB guest. WHPX; the disk is edited in place and booted snapshot=on.

### Verification (2026-10-29, final tree; TCG/MTTCG unless noted)
- `test_x64_swap.py`:
  - Final kernel: api, stress, oom, ext4 PASS on 1 and 2 CPUs; stress, oom, api PASS on 4 CPUs.
  - debian PASS, and stress repeated 10x on 2 CPUs 10/10 PASS. Both ran on the kernel just before the last blockio
    change (the page-fault spin), which does not touch their paths.
  - One stress round: 2x RAM touched and verified, ~82K pages swapped, pswpout ~1M, swap read-ahead ~750K pages.
- SMP disk-campaign stress (`ws_mem_scratch\smp_stress.py`, full `test_x64.py` disk campaign; IDE/AHCI/NVMe x 4 and
  2 CPUs): 42/42 on the final kernel. Two earlier 42-boot runs were also 42/42, after one failure (1/42) that was a
  race in process_user.c's EINTR test, fixed.
- `test_x64_blockio.py` (2 CPUs): syscall rate during heavy disk I/O, as a share of its rate alone:
  - with `blockio.spin`: 0% on every bus;
  - without it: NVMe 37-45%, AHCI 29%, IDE 25-46% on 2026-10-29, at 200-270 MiB/s (final kernel: 37/29/25%);
  - earlier runs with slower host disks: 48-98%;
  - disk throughput kept;
  - with every request spinning its first millisecond (tried): 7-10%, so only page-fault requests spin.
- `test_x64_swap_chrome.py` (WHPX, 2 CPUs, 2 GiB, 8 tabs, 256 MiB swap file), after the orphan-page fix (follow-up,
  2026-10-29):
  - 208 MiB per tab: 5/5 consecutive PASS (peak swap 247-255 MiB, min free 5-8 MiB, no OOM kill, every tab verified
    after the last fill), plus 2 earlier PASS with `mm.audit` (0 checksum mismatches, 36 clean ownership audits);
  - `--no-swap`: 2/2 PASS: the OOM killer takes one renderer (oom_score_adj 300, page cache already at 0), the browser
    and the other 7 tabs keep running and verifying;
  - 224 MiB per tab (1 run): RAM and the 256 MiB swap file really run out, the OOM killer takes a renderer and the
    browser does not fault (the test counts any OOM kill in swap mode as FAIL, by design).
  - `test_x64.py`, `test_x64_memlayout.py`, `test_x64_procmem.py`, `test_x64_blockio.py` PASS; `test_x64_swap.py` api,
    stress, oom, ext4 on 1 and 2 CPUs PASS. The debian case was not re-run: the disk copy with the Debian tools was
    deleted, and reinstalling needs a second full working copy (over the 4 GB cap); its paths are unchanged.
- Full regression from AGENT_COMMON_PROD.md on the final kernel: **30/30 after one re-run**.
  - The QMP ports of test_x64_hda, test_x64_audio, test_x64_storage and the three desktop-disk tests were moved into
    the mem range; the defaults collided with other agents' VMs.
  - First pass 29/30: test_x64_ext4_crash hit ENOSPC, reported as EIO, when its random workload passed ~7990 4-KiB
    log records on its 48 MiB volume (7490 records by iteration 19). It passed on re-run (6174 records at iteration 19).
    The test's capacity is marginal for a faster fsync path.
  - Earlier full pass (kernel before the page-fault spin): test_x64_hda failed its pause/resume audio check 2 of 2 times
    (fixed, see Design), and test_x64_ext4 once on the known audio-glitch flake.
- Legacy i686 compile of `kernel/fat32.c` and `kernel/ata.c` (-Wall -Wextra -Werror, -O0/-O2): OK.

### Limits / open items
- **Chrome's browser crash under memory pressure: FIXED (follow-up 2026-10-29).** The browser died with a NULL write
  (`lock incl (%rax)`, rax=0, chrome file offsets 0x49418F8 / 0x4941770 / 0x3C29373: SparseHistogram::Add after
  PersistentSampleMap::GetOrCreateSampleCountStorage at 0x4942A10 returned NULL), right after child processes exited.
  - Cause: Chrome keeps its shared memory in unlinked files (`/tmp/.com.google.Chrome.XXXXXX`; /tmp is on the FAT
    root here) and deletes its mapped `BrowserMetrics-*.pma` histogram file (no metrics consent). Orphan disk caches are
    never written back by design, but page reclaim (`memory_file_shrink`, the write-back trim) dropped their pages as
    clean; the next fault re-read the stale or zero clusters, the persistent histogram allocator turned corrupt and the
    record lookup returned NULL. `mm.audit` proved it: re-read checksum mismatches on the .pma (disk held zeros or older
    data) and on a Chrome temp file. Without swap the same reclaim ran, hence the crash after the OOM kill.
  - Fix (shmem.c): a PINNED slot bit marks an orphan's pages that are newer than its clusters (set at unlink for dirty
    pages and by every later store); reclaim keeps them; MemAvailable leaves them out; MEMSTAT shows `pinned_kb`
    (about 1 MiB under Chrome).
  - Also fixed: pipe rings are page vectors (the 64 KiB contiguous ring made pipe2 fail under fragmentation and the
    zygote CHECK-crashed); hole punch (MADV_REMOVE / PUNCH_HOLE) now takes each mapping space's lock and cache_lock;
    a fault short of memory reclaims, runs the OOM killer and retries instead of failing or eating the reserve (the
    reserve below min is for exiting OOM victims, non-sleeping contexts, or when nothing is killable); read/write on
    files retry the same way.
  - The EFAULT lines in the old SYSCALL_ENOMEM trace were Chrome's sandbox probes (seccomp, prctl(PR_SET_SECCOMP) and
    prlimit64 with NULL pointers, which expect EFAULT; they occur at every child start without pressure). The trace
    now logs ENOMEM only.
  - Debugging aid: kernel option `mm.audit` (`--kernel-options mm.audit`): every 5 s kswapd cross-checks page
    ownership (PTE + cache holders vs reference counts, stale shared mappings, mapped free frames), swap slots and
    dropped clean file pages are checksummed and compared on swap-in or re-read, and freeing a still-mapped file is
    reported (`[64] AUDIT ...`).
  - Kernel allocations still never sleep (their callers assume page_alloc cannot): they use the reserve, which user
    memory can no longer consume.
- Block I/O from a context without a task while an owner sleeps is refused and logged (`BLOCKIO request from a task-less
  context`); never seen. New code that issues disk I/O from `usb_poll`/the BSP loop must check `block_io_defer()`.
- The block-I/O test's residual loss (28-46% kept) is the I/O task's CPU time under the BKL. Finer FS/page-cache locking
  would be the next step.
- Swap on a partition (MBR 0x82 / GPT Linux swap, `/dev/<label>`) is implemented but has no end-to-end test (no test
  disk with a swap partition). Swap files on FAT32 and ext4 are tested.
- Kernel stacks beyond the pool still need 68 KiB of contiguous memory.
- zswap/zram and THP are not done (optional).
- The desktop session runs as uid 1000 without capabilities: swapon/swapoff/drop_caches need root, so swap is managed at
  boot from /etc/fstab. `/nexus/init-as-root` (written from the host only) makes init root for test disks.
- FAT has no owners: Debian mkswap/swapon warn "insecure file owner 1000". util-linux swapoff refuses non-root by itself
  ("Not superuser"); the kernel's EPERM is tested through the syscalls.
- The launcher edits the installed desktop disk in place the first time (adds `/swapfile`). `--no-swap` or
  `swap_tools.py swapfile <disk> --remove` opt out.

## Windows programs through Wine (2026-10-14)

Real Windows `.exe`/`.msi` programs, 64-bit and 32-bit, now run on the native x86_64 desktop through **Wine 10.0**
(Debian sid `10.0~repack-12`, new WoW64 + the Wayland driver `winewayland.drv`). The old 32-bit OS's PE shim
(`kernel/pe.c`, `win32.c`) is unrelated and unchanged. Only QEMU (WHPX/TCG, `pc` and `q35`) and VMware Workstation 17.5
were tested; **no physical PC**.

Backups and rollback:
- Sources: `C:\nexustest\fix\publish_backup_wine` (`python -I C:\nexustest\fix\publish_wine.py rollback`; manifest lists
  72 modified + 36 added files). This file's previous version is kept in the local `publish_backup_wine` backup.
- Desktop disk (before Wine was installed, SHA-256 `d63ead6f...`): `publish_backup_wine\disk\desktop64.img`
  (`python -I C:\nexustest\fix\publish_disk_wine.py rollback`).
- Final tree and history: `C:\nexustest\fix\ws_fix` (base `base_wine`, merge `merge_w1`, Wine bring-up `ws_wine`).

### Using it
- The desktop disk has Wine installed (`arch/x86_64/wine_install.py <disk> --only wine [--grow MIB] [--cache DIR]`,
  about 1.2 GB; every .deb pinned by size + SHA-256 in `arch/x86_64/wine-lock.json`, whose InRelease files were checked
  with gpgv against the Debian archive keys). Record: `/nexus/wine-installed.json` on the disk, `out/wine-install-report.json`.
- Double-click an `.exe` or `.msi` in Files, or `xdg-open file.exe`, or `wine program.exe` in Terminal. Programs that
  install Start-menu shortcuts appear in Spotlight and Launchpad (`aurora_wineapps.c` reads winemenubuilder's
  `.desktop` files). The first start creates `~/.wine` (37-44 s); later starts take a few seconds.
- Verified programs (`tests/test_wine.py`, 143 checks): 7-Zip (exe + MSI, x64 and x86), Notepad++ (portable x64,
  32-bit NSIS installer), PuTTY (+ MSI with Spotlight icons), SumatraPDF, Inno Setup, Wine's notepad/winecfg/regedit/cmd,
  and own 32/64-bit console, GDI and C++ test programs.
- Limits: Wine 10.0's Wayland driver cannot minimize; dialog ownership/stacking is approximate; no Mono, Gecko or
  GStreamer (.NET and web views do not work); Greek/CJK glyphs are missing; OpenGL needs `--gpu` (virgl) and was not
  part of the tests; SDL2/ODBC libraries are not installed.

### Kernel and desktop changes
- **Memory layout** (`linker.ld`, `entry.asm`, `memory.c`, `kernel.c`): the kernel runs at `KERNEL_VMA 0xFFFFFFFF80000000`
  (`-mcmodel=kernel`) with a direct map of RAM at `0xFFFF800000000000` (`phys_to_virt`/`virt_to_phys`); user space is
  `0x10000..0x7FFFFFFFF000` (Wine needs the low 4 GiB); RAM above 4 GiB is used (multi-zone PMM, `dma32_alloc` for 32-bit
  DMA devices); `MAP_32BIT`, `MAP_FIXED` below 64 KiB is EPERM. The low identity map exists only during boot and AP start.
- **32-bit code** (`segments.c`): Linux GDT layout (0x23 USER32_CS, 0x2b USER_DS, 0x33 USER_CS, LDT, TLS from 0x63, TSS
  from 0x80, per-CPU copies), `set_thread_area`/`get_thread_area`, `modify_ldt`, `arch_prctl`, an IA-32 int 0x80 subset,
  sigreturn selector checks.
- **Wine's needs**: ptrace subset (ATTACH/SEIZE/TRACEME, PEEK/POKE, PEEKUSER, CONT/DETACH, fault-signal stops, dumpable
  rules for execute-only and set-ID images), `/proc/PID/mem`, `netlink.c` (NETLINK_ROUTE, SIOCGIF*), loopback TCP
  listen/accept, ITIMER_REAL/alarm, xattr ENOTSUP on FAT, FAT utimes, `RLIMIT_MEMLOCK` (8 MiB, cumulative, `VmLck`,
  `MAP_LOCKED`, `MCL_FUTURE`), unlinked open files never dirty (large wineserver speed-up).
- **Aurora**: wp_viewporter, pointer constraints, relative pointer, xdg-output v3, xdg-decoration, `wl_pointer.set_cursor`,
  axis_discrete (`aurora_protocols.c`); rootful Xwayland session (`x11_session_user.c`, optional `--only xwayland`).
- FAT32 stores `" * : < > ? |` in long names as U+F000+c (Wine's `dosdevices/c:`).

### Review and verification
Three adversarial reviews (about 95 confirmed findings, including user buffers written after sleeping, an xattr stack
overflow, `/proc/PID/mem` checking the reader instead of the opener, SYN-flood TCB allocation, a netlink infinite loop,
a PCM buffer use-after-free) were fixed and each fix re-checked; a second round fixed traced-exit resource retention,
CR2 after a ptrace stop, `/proc/PID/mem` pinning exited images, and RLIMIT_MEMLOCK. Final results on the published tree:
regression suite 30/30 (`test_x64`, memlayout, wow32 on 4 CPUs, HDA, audio, network, inet, UEFI, USB, storage, ext4,
crash, files, procmem, display, GPU, Wine protocols, Xwayland, Wine prefix FS, ...), `tests/test_wine.py` 143/143
(pc, and q35 on a copy of the real disk), desktop apps 4/4 (VS Code, Firefox, Writer, Calc), VMware 6/6
(BIOS NVMe/SATA/IDE, EFI CD/ESP, audio), legacy `build.bat` builds.

### Gotchas
- In a worktree whose `out/desktop-sdk` is a junction into this tree, plain `aurora.py build` / `installer.py` rewrite
  that SDK through the junction; build worktrees with `C:\nexustest\fix\ws_fix_integ\build_noprep.py <tree> aurora|installer`.
- `tests/test_wine.py --disk` now stages into a sparse copy in WORK (it used to write test files into the given image).
- cmd on this host does not run batch files from the current directory by name: use `.\build.bat`.
- **Launcher freeze fixed (2026-10-15):** since the q35 + xHCI usb-tablet launcher, `build.bat desktop` could freeze at
  `USB xhci0 port 6 ... slot 2: HID device` with the SDL window 'Not Responding' (every headless test passed). Cause, from
  QEMU 9.1's source: `qemu_input_handler_activate()` notifies the mouse-mode notifiers on every call, and the first
  notification after display init is the guest's first USB-tablet poll, in a vCPU thread holding the big QEMU lock;
  `sdl_mouse_mode_change()` then calls `absolute_mouse_grab()`: with the host pointer inside the screen-sized window and
  the window focused, `sdl_grab_start()` sets the window title (SendMessage to the window thread, which waits for that
  lock). The PS/2 controller is irrelevant (a first attempt used `i8042=off`; reverted). Fix in `arch/x86_64/build.py`:
  the SDL window starts hidden (`hidden_startup()`: STARTUPINFO SW_HIDE applies to a process's first ShowWindow) and
  `reveal_window()` shows it once the serial log passes the activation (`absolute pointer ready`, ~2 s; 15 s fallback).
  Verified: 3/3 boots with the hidden window on the PS/2+tablet machine, reveal at 2.0-2.2 s; `tests/test_aurora_launch.py`
  checks the STARTUPINFO. Test disks made from `C:\nexustest\fix\final_disk_fresh` carry an old Aurora without
  USB-tablet support: stage `out/desktop.elf` before judging the pointer.

## VMware Workstation and real-PC readiness (2026-10-13)

NexusOS was brought up on a second, independent virtual PC: VMware Workstation 17.5 on this host (it runs in
"ULM" mode on top of Hyper-V because WHPX is enabled). VMware has its own BIOS/EFI firmware, ACPI tables, an
i440BX chipset with 32 PCIe root ports, and its own NVMe/AHCI/IDE/e1000e/xHCI/SVGA models, so it catches
QEMU-only assumptions. Still virtual: **no physical PC was tested**. Backups: `C:\nexustest\fix\publish_backup_vmw`
(`python -I C:\nexustest\fix\publish_vmw.py rollback`).

### Run it
- `build.bat vmware` (= `python -I arch\x86_64\build.py vmware`; `--uefi` boots `nexus64.iso` through VMware's EFI,
  `--hidden` runs without a window via `vmrun ... nogui`). `arch/x86_64/vmware.py` writes `out/vmware/NexusOS.vmx` and
  `out/vmware/desktop64.vmdk`, a flat descriptor whose extent is `out/desktop64.img` itself (no copy). Machine: NVMe root,
  e1000e on VMware NAT (DHCP), xHCI with USB keyboard and absolute USB pointer (no mouse grab), SVGA (the kernel uses the
  VBE/GOP framebuffer, 1920x1080), HD Audio, 4 vCPUs, 4 GiB, serial log `out/vmware/serial.log`. Keys VMware adds
  (UUIDs, MAC, NVRAM) are kept between launches.
- QEMU and VMware share the same disk but must never run at the same time: `build.bat desktop` refuses while the
  VMware VM runs (lock files in `out/vmware` + `vmrun list`).
- `build.bat desktop` now forwards its flags (`build.bat desktop --gpu`), and `build.bat x64 <action> <flags>` too.

### Kernel changes found through VMware
- **TSC** (`smp.c`, `kernel.c`, `vdso.c`): every AP measures its TSC offset to the BSP by ping-pong at bring-up and runs a
  5 ms warp test (as Linux's `check_tsc_warp`). An offset beyond the round trip is cancelled with IA32_TSC_ADJUST, any
  residual becomes `cpus[i].tsc_offset`, added by `native_tsc()`/`monotonic_ns()` (interrupts off around the read) and by
  the vDSO through RDTSCP (`TSC_AUX` = CPU index, vDSO data magic `NXVDSO02`, 32 offsets). Warps that remain set
  `smp_tsc_unstable`: the kernel clock is clamped to never go backwards and the vDSO clock is disabled. Boot line:
  `[64] TSC: synchronized|per-CPU offsets|unstable ...`. `block_ticks()` uses the corrected TSC.
- **TSC frequency**: taken from the VMware/KVM hypervisor leaf 0x40000010 when present (the 10 ms PIT window was 0.16%
  off under VMware, and polling the PIT with interrupts off stalled VMware's virtual time for ~10 s), else PIT channel 2,
  else CPUID 0x15/0x16. Line: `Calibrated TSC Hz=... source=hypervisor|PIT|cpuid 0x15|cpuid 0x16`.
- **VMware time semantics** (measured with probes): RDTSC traps (several microseconds each) and returns +1 per read while
  VMware's apparent time catches up, so two reads around a busy loop can be equal; time stalls while the guest spins with
  interrupts off and later runs fast to catch up. `dynamic.c`'s clock check now waits for the clock to advance (never
  backwards). Do not set `monitor_control.virtual_rdtsc = FALSE`: the PIT calibration then fails.
- **Boot waits** (`pcidev.h` `boot_wait()`, `xhci.c`, `usb.c`): xHCI bring-up and boot USB enumeration halt until the next
  1 ms tick (`irq_wait_restricted`) instead of spinning. A spinning vCPU starved VMware's device emulation: each USB
  enumeration step took ~6 s and the self-test boot 65 s; now 11-18 s, desktop see below. Only for boot code on the BSP.
- **DHCP client** (`dhcp64.c`, asynchronous, driven by `native_network_poll`): DISCOVER/REQUEST from 0.0.0.0 with the
  broadcast flag, renewal at T1, restart on NAK/expiry, log `[64] DHCP lease A/len gateway G dns D lease S s from X`.
  Until a lease arrives the old static QEMU configuration (10.0.2.15) stays; QEMU's DHCP server offers exactly that.
  `kernel/udp.c` hands port 67->68 datagrams to the kernel client and `kernel/ip.c` accepts DHCP replies before the address
  is configured. While a lease is held `/etc/resolv.conf` is served from it (`proc.c`, read-only; the file on disk is used
  otherwise).
- **PCI**: the native kernel's device table is 256 entries (VMware alone has 41 bridges/chipset functions).
- **Intel HD Audio** (`hda.c`, `ac97.c`, `sound_hw.h`; built by a subagent in `C:\nexustest\fix\ws_hda`): `sound.c` is now
  the hardware-independent ALSA/mixing layer over a `struct sound_engine` (AC'97 or HDA). AC'97 wins when present
  (QEMU launcher default, unchanged); otherwise the first HDA controller (PCI class 04/03, any vendor). CORB/RIRB verbs
  with an Immediate Command fallback (`hda.immediate` boot option), timeouts so a dead codec cannot hang boot, a generic
  output parser (jack-detected line-out > speaker > headphone, routes through mixers/selectors to a DAC, extra
  speaker/headphone pins on the same stream, HDMI only as a fallback), 0 dB amps, EAPD, headphone auto-mute (polled
  every 300 ms), cyclic BDL over the mixing ring, position from LPIB, no interrupts. ALSA card `NexusHDA`.
  QEMU: `set NEXUS_AUDIO=hda` makes the desktop launcher use `ich9-intel-hda` + `hda-duplex`.
  On VMware (15AD:1977, codec 15ad1975): `out pin 0x14 <- mix 0x0c <- dac 0x02 (line-out, jack present)` and the whole
  ALSA campaign passes when started 5 s after boot (VMware runs its virtual time fast for a few seconds after a boot
  that kept interrupts off; the first rate measurement read 0.74x). VMware's DMA position (LPIB) advances in ~50 ms
  steps, so the test judges phase A by elapsed time there (marker `1w`); real playback rate measured 44,450 frames/s
  for a 44.1 kHz stream. Audible output on VMware was not checked (the host backend was disconnected in tests). Not covered: volume controls (fixed 0 dB), capture,
  Realtek coefficient/GPIO quirks, Intel HDMI audio (needs the GPU driver), DMA position buffer.
- **statfs on FAT** counts allocations still pending in the page cache (writeback first, as ext4 already did): the old
  `test_x64_files.py` line-45 failure is fixed and `test_x64_storage.py` no longer tolerates it.
- `tests/test_x64_launch.py` was stale (it expected `subprocess.run`, IDE, RTL8139) and **started a real QEMU on the
  installed disk**; it now mocks `Popen` and checks the q35/NVMe/e1000e machine.

### Tests
- `tests/test_vmware.py [WORK] [--unit] [--internet] [--desktop DISK]` (harness `tests/vmware_harness.py`, probe
  `tests/vmware_netprobe.c`): launcher unit test; live cases bios_nvme, bios_sata, bios_ide, efi_cd, efi_esp (whole
  self-test campaign, IOAPIC, 2 CPUs, hypervisor TSC, warp-free, DHCP lease, USB), audio (HD Audio, host sound
  backend disconnected); `--internet` (NAT: DHCP,
  generated resolv.conf, glibc DNS, HTTP 200 from example.com); `--desktop` (Aurora on the given disk, nonpersistent +
  host-only + SHA-256 check, USB keyboard opens Terminal through Spotlight and runs a command). VMware's VNC server answers
  every update request after the first incrementally; the harness keeps the framebuffer.
- HDA: `tests\test_x64_hda.py` (pc/intel-hda, q35/ich9-intel-hda, micro + immediate commands, AC'97 precedence,
  no card, no codec; wav captures analysed like the AC'97 test) and `tests\test_hda_codec.py` (the real `hda.c` on the
  host against simulated Realtek ALC269/ALC892, IDT and HDMI codecs, dead and missing codecs, UBSan).
- VMware boot times: self-test campaign 11-18 s (was 64-67 s), Aurora desktop ready in ~10 s (was 55 s).
- Final results on the merged tree (2026-10-13): `tests\test_vmware.py --internet --desktop <disk>` all 9 cases PASS;
  QEMU: `test_x64` (13), `test_hda_codec`, `test_x64_hda`, `test_x64_audio`, network/inet/udp, uefi (incl. WHPX),
  usbnet, storage, ext4, ext4_crash, files, cpuinfo, display, readonly, apps, launch, aurora_launch, image: 21/21 PASS.

### Not covered / next for real PCs
- VMware: no SVGA 3D (vmwgfx) driver, so no GPU acceleration there; vmxnet3/PVSCSI/LSI are not supported (the launcher
  uses e1000e/NVMe/SATA).
- Physical PCs: no USB mass-storage driver (a USB stick cannot be the root disk yet), no Realtek r8169 or Intel I219
  (e1000e "ich8lan") NIC, no Wi-Fi, no I2C-HID touchpads, no AML interpreter, Secure Boot must be off.

## Real-PC platform, storage, ext4, GPU, desktop apps and finer locks (2026-10-13)

Six parallel feature worktrees plus the locking work were merged and verified together (final tree
`C:\nexustest\fix\merge_f`, merge scripts `C:\nexustest\fix\{mergeplan,resolve_*,adapt_*,h1..h3}*.py`).
Only QEMU 9.1 (TCG and WHPX, SeaBIOS and OVMF) was tested; **no physical hardware**.
Backups of every replaced file: `C:\nexustest\fix\publish_backup_final` (`python -I C:\nexustest\fix\publish_final.py rollback`).

### Launcher (`build.bat desktop` = `python -I arch\x86_64\build.py desktop`)
- Default machine is now a modern PC: `-machine q35`, NVMe root disk, Intel e1000e, xHCI with USB
  keyboard + tablet (absolute pointer, no mouse grab), virtio-vga, BIOS boot from `nexus64.img`, 4 vCPUs.
  Measured on Chrome: window 5.8 s (old pc/IDE/RTL8139 machine 6.8-8.8 s), video and JS workers equal or better.
- `--gpu`: `virtio-vga-gl` + `-display sdl,gl=on`. Aurora then launches Chrome with
  `/nexus/chrome-desktop-gpu.args` (picked when `/dev/dri/renderD128` exists). WebGL ~57 fps instead of
  6-26 fps, but QEMU runs virgl on its main thread: video playback and JS throughput were worse and
  unstable in measurements, so it is **opt-in**. Video decode is always on the CPU (virgl has none).
- `--uefi`: OVMF (`out\ovmf-unified.fd`, made from QEMU's EDK2 files; WHPX cannot run OVMF from pflash)
  boots `BOOTX64.EFI` from `out\nexus64-uefi.img`.
- `--legacy-devices`: the previous pc/IDE/RTL8139/PS/2 machine. `--tcg` as before.
- The VM process is opted out of Windows 11 power throttling (`unthrottle`): a hidden/minimized QEMU
  otherwise gets 15.6 ms timer ticks.

### Platform (UEFI, ACPI, interrupts)
- `uefi/bootx64.c` loader (PE32+, Boot Services only) -> `boot_info.h` hand-off; `mkuefi.py` makes the GPT/ESP image; the ISO boots on BIOS and UEFI.
- `acpi.c`: RSDP/XSDT/RSDT, MADT (CPUs, IOAPICs, overrides), FADT (S5 power-off, reset register), MCFG; `reboot(2)` (169).
- `irq.c`: IOAPIC mode whenever a MADT exists (`noioapic` kernel option = legacy PIC), MSI/MSI-X, dynamic vectors.
  **Drivers must not touch the PIC ports**: use `irq_attach_isa`, or `irq_alloc_vector` + `irq_register` +
  `pci_enable_msi(x)`; `mmio_map()` for BARs above 4 GiB (OVMF places them there). `VEC_TLB` is 0xFC.
- SMP from the MADT (targeted INIT/SIPI), `MAX_CPUS` 32. The bootstrap image may end below 4 MiB.

### Storage (`blockdev.h`)
- Block layer (512/4096-byte devices, 512-byte partition view), MBR + GPT (CRC32, backup header), root
  priority: GPT NexusOS root -> GPT basic data -> MBR 0x0B/0x0C -> superfloppy; ext4 roots via `ext4_dev.c`.
- AHCI and NVMe drivers (MSI/MSI-X first, INTx fallback, polling fallback; port/controller reset on errors
  and timeouts). NVMe attaches its interrupt before creating queues (QEMU drops MSI-X otherwise).
- `fsync`/`sync` reach the device cache flush. `userland\gptwrap.py` wraps a FAT disk in GPT.

### USB and network
- xHCI, USB2/USB3 hubs, HID (report parser; keyboard, mouse, tablet) into `/dev/input/event0/1`.
- Intel e1000 / e1000e (82574L) / igb (82576), chosen before the RTL8139.
- Their interrupts go through `pci_intx_route()` (MSI when the device has it, else INTx via irq.c).

### Filesystem: ext4 (`ext4.c`, `ext4_journal.c`, `vfs.c`)
- ext4 with jbd2 (checksum v3, replay, orphans), metadata_csum, extents, htree, 64-bit, files over 4 GiB,
  POSIX permissions/ownership, set-user-ID exec, umask, links, symlinks, device nodes, fallocate.
- The root is chosen by content: ext4 if present, else FAT32. `vfs.h` maps the `fat32_*` API to `disk_*`.
- Filesystem syscalls: ext4 root -> `vfs_call` (POSIX); FAT root -> `fsext_call` (Linux vfat semantics).
- Host tools: `userland\{mkext4,ext4put,ext4check,fat2ext4}.py`. **The published desktop disk is still
  FAT32**: converting it would copy the Chrome profile; run `fat2ext4.py` yourself if you want an ext4 root.

### GPU (virtio-gpu 3D)
- `virtio_gpu.c` (virgl, fences, polled), `drm.c` (`/dev/dri/card0`, `renderD128`, PRIME dma-buf,
  sync_file), read-only `/sys` (`sysfs.c`) for libdrm; Aurora imports dma-bufs (`aurora_dmabuf.c`).
- Guest userland: `arch\x86_64\gpu_userland.py install <disk>` (Mesa 25.0.7 from Debian 13, pinned in `gpu-lock.json`).

### Desktop applications
- VS Code 1.141, Firefox 157, LibreOffice 25.2 Writer + Calc: `arch\x86_64\apps_install.py <disk> --grow 8192`
  (360 pinned, checksum/GPG-verified artifacts, `apps-lock.json`); after a GUI group it runs `apps_postinst.py <disk>`
  itself (GTK/glib/mime/font caches: without them GTK apps abort; `--no-postinst` to run it separately).
  Launch from Spotlight/Launchpad. They run without their seccomp sandbox (seccomp is not implemented).
- Kernel: ptys + job control, inotify, statx, fallocate, sendfile/copy_file_range, mremap, `/dev/shm`,
  select/pselect, many `/proc` files (`procext.c`), #! scripts.

### Finer kernel locking (also see the lock table below)
- Without the big lock: scheduling, private futexes, timer preemption, anonymous and page-cache page
  faults (~98% of Chrome's faults), TLB shootdowns, the page allocator, serial console output.
- Lock order: BKL -> own address-space `vm_lock` -> another space's lock -> `cache_lock` -> `pmm_lock`;
  `sched_lock`, `tlb_lock` and `serial_lock` are taken last (nothing else is acquired while holding them).
- Wake-up preemption (busy CPUs are interrupted for a woken task), measured -12% H.264 dropped frames.
- Remaining serialization: disk I/O waits still hold the big lock.

### Tests (all pass on the final tree unless noted)
`tests\test_x64.py` (13 cases incl. ahci, nvme, usbnet), `test_aurora_launch.py`, `test_x64_usbnet.py`,
`test_x64_uefi.py`, `test_x64_storage.py`, `test_x64_ext4.py` (+ `--linux` with `ext4_linux_validator.py`),
`test_x64_ext4_crash.py`, `test_x64_apps.py`, `test_x64_desktop_apps.py --disk <apps disk>`, `test_x64_gpu.py`,
`test_x64_gpu_chrome.py`; self-test loops `/nexus/test-vm-loop`, `/nexus/test-threads-loop`.
Known pre-existing flakes/failures: `test_x64_audio.py` pause/rate checks fail ~1 in 4 under host load
(the published kernel too); `test_x64_files.py` statfs after FAT ftruncate (line 45).

## Finer kernel locking (2026-10-12)

The big kernel lock (BKL, `smp.c`) is no longer taken for the hottest paths. Lock order, outermost
first: **BKL -> own address-space lock -> another space's lock -> cache_lock -> page-allocator lock**;
`sched_lock`, `tlb_lock` and `serial_lock` are leaves (nothing else is acquired while holding them).

| Lock | Protects | Taken by |
|---|---|---|
| BKL (`kernel_lock_*`) | everything not listed below | syscalls, IRQ device work, BSP polls; owned **per task** (released in `task_yield`, re-taken on resume) |
| `sched_lock` | run queue, task states, wait/futex/timer queues, `on_cpu`, CPU idle flags | scheduler loops (no BKL), wakers, futex fast path, timer preemption |
| `space->vm_lock` | VMAs (`maps`, `lookup_hint`) and user page tables of one address space | the BKL holder for its own task's space (in `kernel_lock_acquire`), `space_fault_fast`, cross-space file-mapping updates (`memory_file_invalidate/writeprotect*`) |
| `cache_lock` (`shmem.c`) | page-cache/shared-memory frames and blocks | removals (truncate, trim, `cache_free`), shared-memory inserts, `memory_file_fault_page` |
| `pmm_lock` (`memory.c`) | physical page refcounts/allocation | `arena_alloc`, `page_retain`, `page_free` |
| `tlb_lock` | the single in-flight shootdown request | `smp_tlb_shootdown` (initiators no longer need the BKL) |

Without the BKL now: scheduling and context switches; private futex wait/wake (`task_futex_fast`, user
word read with `user_read32` + #PF fixup); timer/wake-IPI preemption; **page faults** on anonymous
memory (zero-fill, COW break) and on resident page-cache pages (ELF image pages, shared memory/memfd,
private file mappings). Faults that need dirty tracking, disk I/O, partial image pages or the
framebuffer still take the locked path (`page_resolve`).

Rules:
- `spin_acquire` spins with interrupts off and answers TLB shootdowns while spinning (`smp_tlb_poll`);
  any new IF=0 wait loop that can run while another CPU waits for acknowledgements must call it too
  (the AP disk wait does).
- Code that edits a page table or VMA list of a space that is not its own task's must hold that
  space's lock (`space_lock_other`).
- Frames leave a cache only under `cache_lock`; disk-backed caches are filled only by the locked path
  and published with release stores.
- COW breaks free the old frame only after the shootdown.
- Wake-up preemption: if no CPU is idle, `kick` IPIs the CPU running the task furthest ahead in
  virtual time; a CPU in kernel mode re-checks on its way out (`cpu->resched`).
- Serial console (`put`/`console_write`): a 16 KiB ring under `serial_lock`, 16550 FIFO bursts (one LSR
  read per 16 bytes). User console writes (Chrome's stderr) are only queued under the BKL and reach the
  UART in `serial_drain` after trap64 dropped the BKL; kernel messages flush at each newline/`say`.
- Known remaining serialization: disk I/O waits hold the BKL (ATA IRQ14 / AP polling).

Measured (Chrome, 4 vCPUs, video + 2 JS workers): BKL acquisitions in steady state about -45% vs the
plain SMP kernel; page faults no longer appear among BKL holders (startup: ~1.2 s of BKL hold per
interval moved off the lock). Tests: `/bin/vmstress64` (zero-fill + COW across 4 processes x 4 threads
racing mmap/munmap/madvise; marker `/nexus/test-vm-loop` = 100 rounds), threads loop, 8-vCPU boots.

Host: the desktop launcher opts QEMU out of Windows 11 power throttling (`unthrottle` in build.py);
without it a hidden/minimized VM gets 15.6 ms timer ticks.

## Multi-core (SMP) kernel (2026-10-12)

_The locking described here was refined on 2026-10-12/13: see "Finer kernel locking" above (the big lock now belongs to the task and is no longer taken for scheduling, futexes, page faults or TLB shootdowns)._

The desktop now runs on several CPUs. `build.py` passes `-smp N`, where N is half the host's logical processors, capped at 4.

Design (`arch/x86_64/smp.c`, plus `process.c`, `kernel.c` and `entry.asm`):
- **Big kernel lock.** A CPU holds it whenever it runs kernel code, except while spinning for it or halted idle. Kernel data structures therefore need no finer locking. The lock belongs to the CPU, not the task: a task that blocks in the kernel can resume on another CPU. User code runs on every CPU at once.
- **Kernel context.** On lock acquire, the kernel globals (address space, fds, signals, credentials, filesystem) are reloaded for that CPU's task (`task_context_reload`).
- **Per-CPU state.** `current`, the scheduler context, FS/GS, CR3 (`cpus[i].loaded_root/loaded_space`), the TSS (selector 0x28+16*i), the SYSCALL stub (LSTAR) and the kernel stack. `cpu_self()` uses `STR`.
- **Bring-up.** APs start through INIT-SIPI-SIPI with a trampoline at 0x7000 (real mode → long mode). The CPU count comes from fw_cfg `FW_CFG_NB_CPUS`.
- **Interrupts.** Device IRQs stay on the BSP (legacy PIC). APs get a 250 Hz LAPIC timer for preemption and are woken by IPI 49. On PIC IRQs that interrupt user mode, the BSP runs the network/sound polls.
- **TLB shootdown (IPI 50).** It is sent after every PTE downgrade, unmap, COW frame replacement, file write-protect/truncate and guard-page change, to the CPUs that have that CR3 loaded. Freeing a space first drops it from every CPU. `page_resolve` treats a PTE that already allows the access as a stale TLB entry.
- **Scheduler.** Each task has an `on_cpu` flag, and every CPU runs its own scheduler loop. Only the BSP's loop reaps and polls devices.
- **Disk waits.** AP disk waits poll the controller, because IRQ14 reaches only the BSP.

Also fixed:
- `sysinfo(2)` and `/proc/meminfo` now exist. Without them, glibc's `sysconf(_SC_PHYS_PAGES)` returned stack garbage, and Chrome sized its budgets from it. Large `fetch().blob()` results then failed or hung at random sizes.
- The threads self-test no longer assumes a single CPU.

Verification:
- `tests/test_x64.py` now has an `smp` case: 4 CPUs under TCG, with every CPU required to run tasks.
- 40 stress boots of the full suite with 4 CPUs (TCG and WHPX) passed.
- 300-round threads loops passed on both TCG and WHPX (run with the marker file `/nexus/test-threads-loop` on the test disk).
- Chrome, WHPX, 4 CPUs: setTimeout(4 ms) median 4.6 ms, rAF 58–59 fps with max frame 20–34 ms, 720p video 28–29 fps at 1.0x, and sound clean in WAV capture.
- Under heavy load (2x decode plus 2 JS workers): 54 presented fps versus 36 on 1 CPU, 5 versus 38 dropped/s, and 2.2x the JS worker throughput.

Rollback: `python -I C:\nexustest\fix\smp_patches\publish_smp.py rollback`. Patches are in `C:\nexustest\fix\smp_patches`.

Open issues:
- On a fresh profile, `fetch().blob()` above about 12 MB fails cleanly in Chrome's blob storage; `arrayBuffer` and streaming work.
- The VP9 1080p blob-URL test clip sometimes reports "no supported source". This predates SMP; the cause is inside Chrome's media pipeline.
- Lock contention: syscall-heavy loads serialize on the big lock. Finer locks would be the next step.

## Performance overhaul: smooth Chrome video and sound (2026-10-11)

Problem: real Chrome on the native x86-64 desktop ran YouTube at 1-2 fps with no sound. The guest was ~97% kernel time.

What changed (sources in `arch/x86_64/` plus shared `kernel/fat32.c`, `kernel/ata.c`, `kernel/tcp.c`, `kernel/net.c`, `kernel/arp.c`):

- **Storage:** regular-file `write()` is write-back (a page cache flushed by an idle/periodic writer and on fsync/close) instead of write-through. FAT updates are batched, ATA DMA waits halt instead of spinning, and fsync issues FLUSH barriers.
- **Time and scheduling:** 1000 Hz PIT, fair scheduler with a runqueue, hashed futex/wait buckets with timed waiters, targeted epoll wakeups (no thundering herd), lazy CR3 switch, and a vDSO (`clock_gettime`, `gettimeofday`, `time`, `getcpu` without a syscall).
- **CPU:** XSAVE/AVX state is enabled. `build.py` passes the host's SIMD flags (SSE4.2, AVX2, FMA, BMI, AES, ...) to `-cpu qemu64,...`, gated by `IsProcessorFeaturePresent`.
- **Kernel hot paths:**
  - fd lookups go through an O(1) index;
  - VMA lookups use a per-space hint, and private futexes skip the VMA lookup;
  - socket GC walks only sockets, and runs on unpin only when SCM_RIGHTS descriptors are in flight;
  - unlink/rename use per-cache view lists;
  - mprotect/madvise range walks stop at the range end.
- **FAT directories:**
  - the free-slot search reads each sector once, with per-directory lower-bound hints;
  - name lookups use a verified (directory, name) -> slot cache plus a per-directory Bloom filter for "not found";
  - inode identities are hashed.
- **Sound:** AC97 driver with the ALSA ioctl ABI under `/dev/snd`. The desktop launcher adds a DirectSound audiodev at 48 kHz.
- **Network:**
  - TCP uses 256 KB receive/send rings (up to 4 MB) with RFC 7323 window scaling, out-of-order range tracking and delayed ACKs.
  - RTL8139 is IRQ-acknowledged, with batched RX and transmit that never waits on completions.
- **Compositor:** damage-based repaint, a cursor fast path, and 60 Hz frame pacing.

Results (WHPX, 1 vCPU, Chrome with a fresh profile, local test page, 7 s per clip):

| | Before | After |
|---|---|---|
| requestAnimationFrame | 1.3 fps | 58.8 fps |
| setTimeout(4 ms) median | 17.5 ms (max 141) | 10-14 ms (max ~20) |
| VP9 720p30 | did not play | 26-30 fps, 1.0x speed |
| H.264 720p30 | did not play | 29-30 fps, 1.0x speed |
| AV1 720p30 | did not play | 29 fps, 1.0x speed |
| VP9 1080p30 | did not load | 26 fps presented, 30 decoded, 1.0x speed |
| Download in Chrome | 0.3-0.9 MB/s | 50-109 MB/s |
| Kernel time during playback | ~97% | 4-15% |

The real, logged-in profile is slower: extensions, session restore and its disk cache add background work.

Measuring:
- Use `C:\nexustest\fix\core_harness\perf_run.py` on a disk whose `nexus/chrome-desktop.args` uses a fresh `--user-data-dir`.
- Do not measure on the real profile. Restored or extension tabs can take focus, and Chrome then throttles the background test page (1 Hz timers, no rAF, no video decode), which looks like a stall.

Rollback: `python -I C:\nexustest\fix\core_patches\publish_perf.py rollback`. This restores the sources, build outputs, metadata and the previous `desktop64.img`, which is kept as a renamed file in `C:\nexustest\fix\publish_backup`.

Not done yet: SMP (1 vCPU only), and an in-kernel LAPIC/IOAPIC (WHPX `kernel-irqchip=on` still hangs this PIC/PIT kernel). Synchronous disk I/O still blocks the single-threaded kernel during flushes.

Verification at publication (2026-10-11 23:09):
- `tests/test_x64.py`: PASS, 9/9 campaigns.
- Host-only tests passed: `test_aurora_graphics`, `test_aurora_damage`, `test_sound_abi`, `test_native_network`, `test_aurora_launch`.
- `tests/test_x64_audio.py` (ALSA through AC97, WAV capture): PASS.
- Chrome WebAudio plus video sound captured to WAV: correct tones, 0 dropouts, 0 ALSA errors.
- The legacy 32-bit `build.bat` build still links (kernel.bin 1,072,044 of 1,441,792 bytes).
- The published `desktop64.img` differs from the previous one only in `/sbin/init`; 6,075 other files and the reserved area were hash-checked.
- Open issue: on a fresh profile, an 8 MB blob-URL 1080p clip sometimes fails with "no supported source". It played 3 of 3 times in the logging rerun, and 720p clips are unaffected.

## Published Chrome startup and rendering update (2026-10-11)

**Normal `build.bat run` now uses the verified startup/rendering update.**
Publication passed in `C:\nexustest\chrome-startup-publication\result.json`, preserving
5,331 existing entries, reserved disk bytes and the original legacy disk. The
final two-boot WHPX lifecycle passed in `C:\nexustest\chrome-startup-lifecycle\result.json`;
its private 3 GiB image was removed. The final published-browser interaction test
also passed through the actual desktop launcher using a disposable snapshot:
public HTTPS text, a physically typed local-fixture URL and `nexus` input, button
confirmation, and wheel scrolling with changed page pixels and positive scrollY.
Chrome closed back to Aurora and normal shutdown exited33; the base image,
arguments and protected application hashes remained unchanged. Evidence:
`C:\nexustest\chrome-startup-published-interaction-final\result.json`.

The observed fixture responses were 110.25 ms for typing, 116.24 ms for the button
and 59.53 ms for scrolling. Button/wheel timings include approximately42 ms of
physical press hold before release, plus QMP capture overhead. These are scoped
interaction checks, not a general browser-performance claim. The final artifact
and preservation audit passed in `C:\nexustest\chrome-startup-final\verification.json`;
no test VM or staging image remained at that checkpoint.

Profiling found Chrome startup dominated by ATA PIO work. The update adds REP word
transfers, a bounded FAT cache, and native64 bus-master DMA limited to validated
QEMU HARDDISK/PIIX3 hardware. Other controllers retain PIO. DMA uses a bounded
64 KiB bounce buffer, checked PRDs, confirmed STOP, explicit error propagation,
bounded reset/replay and the existing final durability flush. Scene damage now
limits framebuffer updates, while window-move erasure and static-app behavior
remain verified. The published `arch/x86_64/out/nexus64.img` has SHA-256
`88c4bf7fc0c85cdd7a14da09766eceb187afce1dfe0d96780eb7a549d288e719`.

Installed QEMU 9.1 WHPX works with `-cpu qemu64,+rdrand` and
`-accel whpx,kernel-irqchip=off`. The original `max` failure was a SeaBIOS
FEATURE_CONTROL MSR write. The separately tried `max,-vmx,-sgx` passed firmware
but failed in the glibc loader with an invalid instruction; it was rejected.
No additional CPU flags or SMP were enabled. A verified, isolated QEMU 11.1
comparison was slower; its archive, extracted binaries and copied test images
were removed. Evidence remains in `C:\nexustest\chrome-accel-probe\verification.json`.

On the final permanent image, tested through a disposable snapshot, Chrome's
first window appeared at **9.20 seconds** and the complete public HTTPS page at
**13.62 seconds**, versus the prior TCG baseline **53.63/61.04 seconds**. Startup
cursor samples peaked at 130.89 ms versus 949.51 ms in the baseline. Loaded-page
cursor latency was **33.05 ms median, with a 311.19 ms spike** on the first move;
the remaining measured moves were 16-51 ms. This residual stutter remains a
limitation. Window dragging was 16.82 ms median and 17.44 ms maximum. These include
QMP capture/polling overhead and are not FPS measurements. Evidence:
`C:\nexustest\chrome-startup-published-timing\result.json`,
`chrome-startup-before\result.json` and `chrome-startup-lifecycle\result.json`.
Earlier measurements remain archived in `chrome-startup-delta`,
`chrome-startup-dma-whpx` and `chrome-startup-dma-repeat`.

Native CPU/disk/entropy tests passed with DMA under TCG and the conservative WHPX
CPU (`chrome-startup-native/dma-tcg` and `/dma-whpx`). Real read/write timeout
recovery, exact persisted bytes and cold reboot passed in
`C:\nexustest\chrome-dma-recovery\result.json`. Injected write errors returned
failure while retaining dirty data in `C:\nexustest\chrome-dma-errors\result.json`.
Those recovery/error private disks were removed. The eight-case native CPU/disk/
network matrix, ISO boot, three rendering probes and legacy build/login snapshot
also passed; outer logs are under `C:\nexustest\chrome-startup-final`.
Direct Chrome through the actual published launcher also passed HTTPS rendering
and normal exit33 under WHPX, preserving arguments, runtime hashes and the base
disk (`C:\nexustest\chrome-startup-published-direct\result.json`). The native probe
and CPU-matrix copies, 64 MiB each, were removed; see
`C:\nexustest\chrome-startup-final\cleanup.json`.

`arch/x86_64/build.py` now contains the tested `whpx_available()` and
`desktop_cpu_options()` helpers plus a `--tcg` diagnostic parser. The helper
requires successful, correctly sized Windows capability results for a running
hypervisor, x86 vendor and documented RDRAND support; missing/error results select
TCG. **Desktop and direct Chrome now select WHPX only when those checks pass**;
other launch actions retain their existing CPU settings. To force software
emulation for diagnosis, use `python -I arch/x86_64/build.py desktop --tcg`
(or `chrome --tcg`). There is no automatic retry after starting a guest.
`tests/test_aurora_launch.py` passes capability failure/size/vendor/RDRAND gates,
actual TCG/WHPX launch arguments, explicit override and one-QEMU/no-retry checks.
The regressing epoll scheduling experiment was rolled back. No commit or
physical-disk install is authorized.

## Earlier cursor performance milestone (2026-10-11)

**This preceding cursor-fix release was published before the startup update.** Publication
passed in `C:\nexustest\aurora-performance-publication/result.json`, preserving
5,303 existing entries, reserved disk bytes and the original legacy disk.

The cursor stutter was reproducible: moving over empty desktop space did not mark
the cursor dirty, leaving its position to update on the one-second clock repaint.
The update separates cursor damage from scene redraw, restores/copies only the
old and new cursor rectangles, reuses the cached scene, copies opaque surfaces by
row, and avoids unnecessary redraws in static native apps.

Measured at 1920x1080 with QEMU TCG, using 12 physical relative mouse moves per
workload and exact cursor pixels plus old-cursor erasure checks:

| Workload | Median latency | p95/max latency |
| --- | ---: | ---: |
| Original empty desktop | 856.06 ms | 919.06 ms |
| Published fixed empty desktop | 15.66 ms | 17.56 ms |
| Fixed Chrome, fully loaded public HTTPS page | 41.81 ms | 43.25 ms |

These are host-input-to-first-observed-frame latencies, including QMP capture and
polling overhead; they are **not FPS measurements**. The Chrome gate requires the
complete lower-page link, the reload icon and two seconds of stable page pixels.
All runs shut down normally with the snapshot base images unchanged. Evidence:
`C:\nexustest\aurora-latency-before/result.json`, `aurora-latency-published/result.json`
and `aurora-latency-chrome-final/result.json`; harness:
`tests/benchmark_aurora_cursor.py` (`--chrome` selects the browser workload).

Native client, subsurface and popup checks passed. The final two-boot lifecycle
passed in `C:\nexustest\aurora-performance-lifecycle/result.json`: Calculator and
window controls, modifier/Alt+Tab behavior, repeated Notes saves and exact
cold-boot content, Chrome HTTPS, another Hello download and successful 14-byte
output, clean shutdowns, and source-image preservation. Static Notes produced
zero extra commits over 2.032 seconds idle or ordinary pointer movement; control
hover enter and leave each produced exactly one visible update. Six physical
window-drag steps measured **32.50 ms median, 37.49 ms maximum**, including capture
overhead. The private 3 GiB test image and publication staging image were removed.
The final snapshot cursor check of the published image also passed all 12 exact
cursor/erasure checks, normal shutdown and unchanged base-image hashes. Its
compositor hash matches the lifecycle and publication evidence.

That milestone used TCG. Initial installed QEMU 9.1 probes with `-cpu max`
failed before guest kernel startup with both `kernel-irqchip=on` and `off`
(`Unexpected VP exit code 4`). Those exits were recorded as failures, not passes;
the small probe copy was removed and original hashes stayed intact. Evidence:
`C:\nexustest\whpx-probe/on/result.json` and `off/result.json`. No host settings,
CPU count or launcher acceleration defaults changed during that earlier campaign.
The validated conservative CPU and current launcher selection above supersede
that initial WHPX limitation.

## Latest runnable state

The commands and app scope below remain current. Their original integration
evidence predates the performance publication above.

**Native Aurora integration is published (2026-10-11).** Normal `build.bat run`
now launches the persistent x86-64 Aurora desktop. Original wallpaper, fonts,
dock, menus, Spotlight and Launchpad surround floating Wayland windows. Chrome
opens from the dock; closing it returns to the desktop.

```powershell
.\build.bat run          # Published native Aurora; no rebuild or image replacement
.\build.bat x64 chrome   # Direct Chrome diagnostic via firmware session selection
.\build.bat legacy       # Original i386 desktop, apps, accounts and disk.img
```

Native launchers are Files, Notes, Terminal, Calculator, Calendar, Paint,
Minesweeper, Help/About and Chrome. Calculator/Calendar/Paint/Minesweeper reuse
original application source in isolated native processes. This is a source port;
existing i386 binaries still use the legacy environment. Native Terminal runs
commands and captures output; it does not emulate a TTY or supply legacy kernel
shell commands. Ctrl+Space, F4/Win, Alt+Tab and Alt+F4 provide search, Launchpad,
window switching and close. Use the Nexus menu for normal shutdown.

The original integration two-boot campaign passed in
`C:\nexustest\native64-aurora-lifecycle/result.json`: Calculator input/math,
move/resize/minimize/restore, physical modifier/menu and Alt+Tab behavior, repeated
Notes saves, exact cold-boot Notes bytes, real Chrome HTTPS, fresh Hello download,
receipt reuse/Run with visible output, and clean shutdown. Its private 3 GiB image
was deleted after final checks. Earlier observer failures remain archived; they
are not represented as successful runs.

Additional real-VM launcher QA is in `C:\nexustest\aurora-apps-check/app-checks.json`:
Calendar navigation, Paint drawing/erase, Minesweeper flag toggle, Help/About,
Terminal execution, Files browsing and uppercase `.DEB` dispatch. That separate
case performed fresh native Hello extraction and Run while preserving its prior
bundle. Runtime hashes match the final lifecycle evidence.

Publication used `arch/x86_64/aurora.py` and one disposable staging image. It
preserved **5,140 existing entries** and the reserved disk bytes, retained original
legacy `disk.img` unchanged, updated the compositor/session configuration and added
the native app client. Direct-Chrome arguments survive in
`/nexus/chrome-desktop.args`; default `/nexus/desktop.args` selects `--aurora`.
Evidence: `C:\nexustest\aurora-publication/result.json` and before/after manifests.
`desktop64.json` records the verified runtime hashes and publication gate.

Legacy FAT data and validated, world-readable RAMFS snapshot data are available
under `/home/nexus/Legacy/<source-sha256>/fat` and `/ramfs`. Selected NXRS header
and payload CRCs passed. `/etc/passwd.db` was excluded from guest-visible imports;
original disk and host-side `legacy-reserved.bin` retain account/private/raw
NPFS/snapshot state. UID/GID/mode metadata is preserved; this does not claim old
identities/ACLs were remapped.

The final cross-check is `C:\nexustest\aurora-integration\verification.json`: it
links the verified results, confirms actual normal-command dispatch and records
that no temporary/private/staging image remains. PowerShell outer logs may be
UTF-16; machine-readable JSON evidence is UTF-8.

**Both actual published launch modes passed without cloning or staging.** Normal
Aurora dock-launched Chrome, displayed real HTTPS text, survived Chrome close and
shut down with code33. The direct mode reported `INIT SESSION chrome override`,
rendered its normal HTTPS URL without the Aurora shell, and exited33. Arguments,
original applications and runtime hashes were unchanged. Evidence:
`C:\nexustest\aurora-published-smoke/result.json` and
`C:\nexustest\chrome-published-smoke/result.json`.
The native eight-case CPU/disk/network matrix plus ISO passed. Legacy snapshot
smoke reached its actual login screen with base hashes preserved; persisted
credentials were not attempted. Screenshots: `screenshots/native64-aurora-desktop.png`,
`native64-aurora-chrome.png`, and `native64-aurora-fresh-install.png`.

Aurora protocol regression also passed activation, forged-serial rejection,
move/resize grabs, menu modifiers and unmap/cancel semantics; evidence is in
`aurora-integration/protocol-result.json` and `protocol-serial.log`.
Do not overwrite a live image or rebuild its attached boot file. Thin backups
remain in `aurora-integration`. No commit or physical-disk installation is
authorized. The Chrome-only history below records earlier milestones; the native
Aurora state above takes precedence.

**Actual Google Chrome 155.0.8059.39 is downloaded, natively installed, and
graphically running on NexusOS's own x86-64 kernel.** The installed browser also
downloaded Debian GNU Hello; our native installer installed it and its executable
printed `Hello, world!`. This is a verified milestone, not universal modern-OS
compatibility. The specified Chrome installation/interactive-execution goal is
verified; broader everyday desktop compatibility remains unfinished (see next work).

From `NexusOS/`, the preserved direct Chrome diagnostic is:

```powershell
.\build.bat x64 chrome
```

It uses `arch/x86_64/out/desktop64.img` (one **3 GiB** writable disk), the native
`nexus64.img` boot image, 2 GiB guest RAM, SDL display and native RTL8139 networking.
The launcher does not rebuild or replace the installed disk. In this direct
Chrome mode, closing Chrome ends the VM session cleanly; normal `run` instead
returns to Aurora. Normal profile: `/home/nexus/chrome-user-profile`.
The normal boot has **no test inspector or DevTools pipe**. Its actual HTTPS
framebuffer and Alt+F4 shutdown passed in `C:\nexustest\native64-normal-desktop`;
screenshot `screenshots/native64-normal-chrome.png`. The later normal-profile
installer acceptance also passed (see below); its VM exited cleanly. Recheck
processes before rebuilding an attached boot image.

The disk was **moved, not duplicated**, out of
`C:\nexustest\native64-chrome-download-cpuinfo`. That workspace retains evidence
and installation manifests with a `disk` field pointing to the persistent image;
the installed-browser probes and installer understand this relocated path.
`arch/x86_64/out/desktop64.json` records its provenance. Never remove this image
as disposable test output. The original i386 disk and bootstrap build image remain.

Verified evidence in `C:\nexustest\native64-chrome-download-cpuinfo`:

- `download-result.json`, `serial.log`: Google package, **143,552,428 bytes**,
  real PS/2 navigation, guest SHA-256/fsync and independent disk readback all pass.
- `install-chrome/serial.log`, `installation-result.json`: native extraction,
  digest verification, publication and execution of
  `/home/nexus/Applications/chrome/opt/google/chrome/chrome`. Original ELF hash
  `9bfb381296dffe75f3419d07b6cc64525105ed117d1453a4ef3b291cb97d2b58` and all **255**
  cached upstream Chrome resource files matched. The verified duplicate executable
  `/opt/google/chrome/chrome` was then removed from this private disk, reclaiming
  about 284 MiB and preventing the graphical tests from falling back to it.
- `installed-https/serial.log`, `https.png`: the installed Chrome rendered the
  original public `https://example.com/` document with normal certificate validation.
- `installed-download/serial.log`, `download-result.json`, `chrome-interactive.png`:
  physical keyboard/mouse, original page JavaScript and actual pixels pass; the
  installed browser downloaded `hello_2.10-5_amd64.deb` (**53,228 bytes**) over HTTPS.
- `install-hello/serial.log`, `hello-installation-result.json`: that guest download
  was natively installed and executed. ELF SHA-256:
  `bfda3fef063414535d116a04689caf300568e0691318bcf255f932930508311b`.
  Both downloaded archives came through guest Chrome; the installer did not
  receive a host-injected `.deb`. The initial browser bootstrap and application
  libraries were host-staged, as documented in the earlier milestones.

Additional current fixes/checks:

- FAT reads now keep a bounded, shared sparse index for fragmented files. Profiled
  installed-Chrome startup spent 18/20 samples in ATA reads/waits, mostly chain
  walking. The new regression gets **42 sectors cold, at most 6 warm**, including
  fresh aliases. Same-size chain replacement, remount, saturated epochs, read
  failures and the existing fault/power-cut tests pass. Native CPU/disk/ISO matrix
  and two-boot mutation/persistence checks pass. Rebuilt i386 TLS/network campaign
  is again **33/33**, no `!EX:`/`!KR:`. Logs: `native64/fragmented-*.log`.
- Current kernels are **278,894 bytes native** and **1,067,948 bytes i386**,
  warning-free. Native SHA-256:
  `6245b9db5489c939742335d6cd0406426b3558f94cb5d82d01a3222507b1fd66`;
  native boot `398ff82d1b106e2028957eb3d20e0fcb602d0dcee8f5760534f841cbc1e9ef5b`;
  i386 `e87f41141af7b56f3c5fdd0d90f5f37daa90c78110610825566d5b1de2eb0172`.
- Native installer tests now pass **26 cases**, including excessive XZ dictionaries
  and rollback when receipt writing runs out of disk space.
  Chrome needs a 256 MiB dictionary plus decoder state, so the decoder budget is
  512 MiB in the 2 GiB installer VM. It reports memory-limit errors distinctly.
  Installer build SHA-256: `eaf5906a3a3d500466120476848e0f5b945ee37d2f7040b32f6240f90fb4ac45`.
- Fixed two verifier races: wait for a real full-sized Wayland buffer before mouse
  input, and wait through an interim download-file size before hashing completed
  bytes. A first valid Hello download was preserved as
  `installed-download/first-guest-hello.deb`; fresh download verification then passed.
  Browser startup observation is 240 seconds and host observation 600 seconds
  (1200 for downloads); page/content/integrity checks remain intact.

## Chrome menus and popup checkpoint (2026-10-11)

The persistent desktop now includes actual **xdg-shell v1 positioners/popups**.
`desktop.c` copies positioner rules, applies anchor/gravity and flip/slide/resize
constraints, accounts for parent/child window geometry and shadows, and paints
nested popup trees in order. Geometry is committed with surface state. Explicit
grabs require real input serials; keyboard focus follows the top grabbing popup.
Escape, outside clicks, parent teardown and nested destruction release grabs and
restore focus, without forwarding a dismissal click to the underlying client.
Invalid sizes/parents/roles/destruction ordering are isolated protocol errors.
The existing shell still advertises version 1; reactive/reposition v3 requests
are not falsely advertised as implemented.

A real Chrome test found that moving keyboard focus into an **ungrabbed** menu
cancelled its action. Such menus now keep their toplevel's keyboard focus; explicit
grabs still receive popup keyboard focus. Native regression covers both paths,
including a late nested grab whose parent was already dismissed. XDG shell/surface
lifetimes now retain their owner correctly. Remapping an unmapped XDG surface
requires a fresh configure/ack; the subsurface test client was corrected to perform
that required handshake before attaching its new buffer.

Verified on actual native VMs:

- `tests/test_popup_position.py`: placement/constraint math and integer boundaries,
  compiled with undefined-behavior checks.
- `tests/probe_desktop64.py ... popups`: immutable positioners, shadow-aware and
  nested pixels, geometry commit timing, 13 malformed-client cases, forged-grab
  denial, abrupt-disconnect cleanup, real pointer/keyboard input, nested grab
  restoration, Escape/outside-click dismissal and ungrabbed-menu focus.
  Evidence: `C:\nexustest\native64-popup-test`, outer `native64/popup-test.log`.
- Existing window/clipboard and subsurface tests passed in `native64-popup-client`
  and `native64-popup-subsurfaces`.
- **Actual installed Chrome** passed `tests/test_chrome_menus.py`: keyboard and
  mouse three-dot menu, History submenu constrained to the left screen edge,
  Escape and outside-click dismissal, clicking **New Tab**, Ctrl+W returning to
  the original document, a link's right-click menu, and clean shutdown. No user
  fault, kernel fault or compositor resource leak. This uses PS/2 events and actual
  framebuffer pixels, with no DevTools or DOM interaction injection.
  Evidence: `C:\nexustest\native64-popup-chrome`, outer `native64/chrome-menus.log`.
  Screenshots: `screenshots/native64-chrome-menu.png`, `native64-chrome-submenu.png`,
  `native64-chrome-context-menu.png`, `native64-chrome-menu-new-tab.png`.

Compositor ELF: **190,200 bytes**, SHA-256
`d7ea73f9fe85a738ad847d0c5e68588b1467c3d8a677024de7d6ac27dceeef65`.
It was atomically installed as `/sbin/init` in `arch/x86_64/out/desktop64.img`;
normal launch arguments and both installed Chrome/Hello hashes were checked after
the update. The prior 151,736-byte compositor is preserved as
`C:\nexustest\native64-popup-chrome\desktop-before-popups.elf`. No kernel/i386
code changed in this checkpoint. All popup test disk copies were removed after
their VMs exited; the persistent desktop image remains intact and no VM is live.

**Current publication:** the large Chrome GUI installation/rendering test passed
and the verified compositor/CLI/graphical installer were published to the persistent
desktop on 2026-10-11. Normal-profile first install passed with manual input
takeover; the corrected automated download/reuse/run acceptance also passed.
Inspect the two normal-profile evidence directories described below.
See the publication checkpoint below for evidence and preservation details.
Seccomp is not implemented; GPU/media support and missing Arabic/CJK fonts remain
limits. Toplevel movement/minimization and client cursor rendering are still
desktop-shell work. The tested menu paths do not prove every dialog or menu action.
Do not claim every Linux/Windows/macOS program works or call this production-ready.
No commit or physical-disk installation is authorized. The history below records
earlier intermediate states; this section and live files take precedence.

## Chrome goal completion audit (2026-10-11)

The final audit is `C:\nexustest\chrome-completion-audit.json`. It checks the
concrete Chrome workflow and its required native foundations; it does not claim
that every application or browser feature works.

| Required outcome | Current authoritative evidence |
|---|---|
| Genuine current Chrome | Fresh official Google stable index matches version/size/SHA in `chrome-completion-current-package.json`; read-only current-disk comparison of the archive and all **255** regular Chrome package files passes in `chrome-completion-direct-readback.json`. |
| Own kernel and runtime support | Native image independently matches Nexus BIOS stages plus the flattened custom kernel. Current CPU/loader/process/memory/syscall regression logs pass their present validators: `native64-ata-recovery-matrix` and `native64-ata-timeout-fixed/build-hashes.json`. |
| Guest download, native install, real Chrome execution | Permanent native-CLI installation: `native64-chrome-download-cpuinfo`. Native GUI install/new Chrome launch and actual public HTTPS text: `native64-chrome-package-ui/result.json` and `newly-installed-chrome-https.png`. |
| Current-build interactive input and menus | **Rerun passed on the published boot/compositor hashes**: `native64-current-chrome-menus/result.json`, `current-chrome-menus.log`. Keyboard/mouse menus, constrained History submenu, New Tab, return, context menu, Escape/outside click and clean shutdown all pass. Its only private disk was deleted. |
| Download/install/run another application | Normal permanent-profile first Hello installation and visible output: `native64-installed-desktop-flow`; corrected unattended fresh download/reuse/run, persistence and shutdown: `native64-normal-installer-synchronized`. |
| Preserve data and authorized boundaries | Publication preserves **4,545** existing entries, intentionally replaces two runtimes and adds one installer; reserved bytes/normal args remain unchanged. Original Chrome/Hello/runtime hashes rechecked. Git HEAD remains `70871d6bf81a73756746825ff096dfe8f61aa99e`, no staged changes/commits or physical-disk installation. |

Permanent Chrome was installed with the native CLI; the GUI-installed second
Chrome was verified on the disposable image. The original bootstrap and dependency
libraries were host-staged. Nexus supplies the kernel, loader/syscalls and desktop
logic; upstream glibc/Wayland/xkb application libraries are used. No Linux kernel,
WSL/container, external guest OS or remote browser performs the execution.

No test VM remains active after this audit. Seccomp/full Chrome sandbox security,
dependency/maintainer-script installation, broad GPU/media/font/physical-hardware
coverage and general Windows/macOS/Linux application compatibility remain unfinished.

## Native package window checkpoint (2026-10-11)

`arch/x86_64/installer_ui.c` builds as `out/nexus-installer-ui.elf`, now installed
in the permanent desktop as `/bin/xdg-open`. A real Chrome download-file click opens it directly.
The window checks the local archive, offers Install, discovers native ELF64 entry
points, launches the selected program with `fexecve`, and displays its output.
It keeps operations running when the window closes. Installed bundles use their
package SHA-256 as the directory name and contain a synced checksum receipt.
Repeat-open offers Run without reinstalling or auto-running anything. This is
private application installation, not dpkg/system dependency or maintainer-script
support; the UI explicitly says publisher identity is unverified. The local
checksum checks consistency, not publisher authentication.

Verified so far:

- `tests/test_chrome_installer.py` / `C:\nexustest\native64-chrome-installer-flow`:
  the old Hello archive was removed from the **private test copy**, then actual
  installed Chrome freshly downloaded Hello over HTTPS. Real PS/2 input clicked
  the download, Install and Run. The actual screen shows `Hello, world!`; receipt,
  archive and installed ELF hashes were independently read back after shutdown.
  Repeat-open, return to Chrome and normal close pass. No DevTools/test-session
  installer or host-injected package was used. That test disk is deleted.
- `tests/test_installer_ui.py` / `native64-installer-orphan`: four native boots
  pass cancel-without-install, install/run/output, receipt reuse after reboot,
  and keeping the desktop/installer alive after its original launcher exits.
  The updated four-boot campaign in `native64-installer-background` additionally
  closes the actual window while installation is still in progress, checks that
  it disappears before the worker finishes, then boots again and runs Hello.
- `tests/test_x64_installer.py` / `native64-installer-receipt-full`: **26 cases**
  pass, including a forged receipt filename and ENOSPC specifically while writing
  the receipt after staging consumed the final free cluster. Rollback restores
  free space and leaves existing files intact.
- `execveat` and `/proc/.../fd/N` execution now use the opened inode rather than
  reopening its former pathname. Both filesystem boots in `native64-fd-exec`
  cover unlinked/replaced executables, relative paths, flags and CLOEXEC; the
  native CPU/disk/ISO matrix in `native64-fd-exec-matrix` passes.
- Chrome exposed an initial geometry commit before any Wayland buffer existed.
  The compositor now defers clamping until bounds exist. The native popup/error/
  input regression passes in `native64-ui-geometry`, including this new case.
  Subsurface pixels/error handling and normal window/input/clipboard regressions
  also pass in `native64-installer-subsurfaces` and `native64-installer-client`.
- Host UBSan local-file/URI and popup-placement checks, and launcher checks pass.

Before the ATA recovery change below, build artifacts were: native kernel **278,894 bytes**, SHA-256
`966d119553eb7732bf6ba2226232723cad6a06eeb5f5de73b45d95488b5e359e`;
boot `b946c8e8adcb0598888533fb72271b31fe628393f4f523ff9ba7f223c87759a8`;
desktop `a0a2cde00403acc8f1547974f918974eada719e3712ca55f39147006d77bfa91`;
CLI installer `eaf5906a3a3d500466120476848e0f5b945ee37d2f7040b32f6240f90fb4ac45`;
GUI `388fa5cdaaeb6d46389d9519474849797e198b4ed42b6d242816c5d64945a3c2`.
These hashes describe the pre-recovery checkpoint. The permanent-image manifest
now records the newer verified publication described below.

The analogous **fresh Google Chrome package** GUI flow initially ran in
`native64-chrome-package-ui` (one temporary 3 GiB image). Its first observer was
replaced before its too-short download deadline, without restarting QEMU or
resetting download progress; details are in `controller-takeover.json`. The
replacement controller is `C:\nexustest\resume_chrome_package_ui.py`; its log is
`native64/chrome-package-ui-resumed.log`. Inspect the actual process and result
before claiming this additional test passed. The repository test source now allows
one hour for this large download; UI, hash, execution and HTTPS assertions remain.
The download completed, but the installation subsequently **failed on an ATA
timeout**; PID 23140 was stopped after preserving the error log/screenshot.
That attempt did not pass. See the recovery checkpoint below for the actual
current retry. The earlier CLI Chrome install took about ten minutes; do not
restart a live installer merely because it is slower than Hello.
Google's current stable package metadata was rechecked and matches the lockfile;
evidence is `native64-chrome-installer-flow/current-google-package.json`.

Before any permanent update, `native64-installer-publish/before-files.json` records
all **4,547** existing file/directory entries with content hashes. It also holds
small prior runtime binaries and a hash of the reserved disk region. Preserve
the normal desktop arguments, profiles, all existing apps and archives. Do not
replace the permanent disk with a test image.
Prepared and subsequently **executed successfully**: `C:\nexustest\publish_installer_desktop.py`. It
requires the successful Chrome GUI result and matching build hashes, stages a
sparse 3 GiB image using the existing storage migration, then compares every
existing file and reserved-region byte before atomic publication of the three
runtime updates. This creates room for an additional Chrome bundle without
accumulating raw test copies. After publication, verify normal desktop boot,
update the README/this checkpoint, record current disk usage and remove no source
or recovery images. Current Hello GUI screenshots were copied to
`screenshots/native64-installer-from-chrome.png` and
`screenshots/native64-installed-hello-ui.png`.

## ATA timeout recovery checkpoint (2026-10-11)

The large GUI Chrome install exposed a PIO timeout while writing the new Chrome
executable, followed by failed reads of libc/Chrome and SIGBUS. Evidence:
`native64-chrome-package-ui/ata-failure.log` and `ata-failure.png`. The old driver
left the timed-out transfer active and accepted a channel with DRQ still set as
idle; subsequent command-register writes are not valid in that state. The precise
cause of the initial delay is not established. No failed image was published.

`kernel/ata.c` now waits for an actually idle channel before issuing a new command,
rechecks readiness at the timeout boundary, and performs a calibrated software
reset after a timeout. It retries at most once, replaying the **entire** request;
writes include a fresh flush so a reset cannot turn discarded cache data into a
false durability success. Hard device errors still fail. A failed final retry
leaves recovery pending for the next request. IDENTIFY retains its separate
busy-clear/data-ready sequence. PIT IRQ0 is unchanged. The i386 reset-delay
calculation avoids a dependency on a libgcc 64-bit division helper.

Verification:

- `tests/test_ata_recovery.py` first failed against the old kernel in
  `native64-ata-timeout-before`: a deliberately slow real QEMU IDE write caused
  fsync EIO. With the fix, `native64-ata-timeout-fixed` passes **both real write
  and read timeouts**, reset/retry, byte-for-byte host readback, cold reboot and
  protected-region checks. The test image is removed.
- Host ATA simulations run in **both 32-bit and 64-bit** executables. They cover
  interrupted batches, full-request replay after flush timeout, pending DRQ,
  bounded retries, failed reset and unchanged hard-error behavior. FAT fault,
  power-cut, rename and fragmented-read checks also pass.
- The native CPU/disk/ISO matrix, two-boot filesystem mutation checks, real
  writeback-error retention and read-only/quarantine checks pass. Logs:
  `native64/ata-recovery-{host,matrix,files,writeback-errors,readonly}.log`.
- Both kernels build cleanly. The i386 TLS/network campaign passes **33/33**,
  with no `!EX:`/`!KR:` or leaked streams/TCBs; log
  `native64/ata-recovery-i386-campaign.log`. Healthy-disk throttling to 10 read
  IOPS also passes without an ATA error/reset (`ata-recovery-latency.log`).
- `native64-ata-timeout-fixed/build-hashes.json` records current hashes. Recompiling
  the current ATA source confirms every native runtime section and relocation
  matches the running native build; only non-loaded DWARF debug records differ
  after the i386-only arithmetic fix. Do not overwrite an attached boot image.

Before retrying, an offline audit verified the guest-downloaded Google archive's
exact official hash/size and **4,544 unchanged original entries** in the private
test image. Only its three expected test runtime/config files differed from the
permanent baseline. `native64-chrome-package-ui/resume-download.json` records this
provenance. The failed staging directory is confined to that disposable image.

The repaired-kernel retry (PID 7424, `native64/chrome-package-ui-ata-retry.log`)
completed native Chrome installation, Run, repeat-open, receipt/archive/ELF
hash readback and clean shutdown. There were no ATA/USER_FAULT/KERNEL_FAULT
errors. The installer discovered four ELF programs and launched the actual Chrome
entry. However, visual review caught a **verifier race**: the HTTPS screenshot
contained the correct tab title and page background but no document text yet.
Therefore that attempt does **not** prove full document rendering. Its original
result is retained as `navigation-only-result.json`; `result.json` was explicitly
downgraded (`new_chrome_public_https_verified=false`). The raw log is preserved
as `ata-retry-navigation-only.log`, and the blank frame as `navigation-only.png`.
Do not promote that partial result to a complete rendering PASS.

The test now rejects background-only frames (with a negative self-check), waits
for actual dark document pixels below the browser toolbar, and records
`https_document_pixels_verified`. Publication additionally requires that field.
It fails promptly on a real installer/fault marker and permits 30 minutes for
large extraction, matching the existing native CLI campaign.

Current full-render retest: `tests/test_chrome_installer.py
C:\nexustest\native64-chrome-package-ui chrome --resume-download`, log
`native64/chrome-package-ui-document-profile.log`. The previous test disk was
automatically deleted on successful shutdown, so this is one new private copy of
the permanent image, retaining its **original verified guest Chrome download**.
`resume-download.json` records this origin. The preserved
`/home/nexus/chrome-desktop-profile/Default/History` was independently read using
SQLite and contains the exact Google archive, completed state 1, 143,552,428 bytes.
That profile is selected for this retest. An initial empty-history test profile
was caught and closed normally before correcting the fixture; its log is
`empty-history-profile.log`. No package was host-injected or re-downloaded.
Require the final full-render screenshot/stronger result before publication.
Finished matrix/files/writeback/read-only/i386 test disks (**1.25 GiB**) were
removed after their VMs stopped; manifest `C:\nexustest\cleanup-ata-recovery-tests.json`.

## Verified graphical installation and permanent publication (2026-10-11)

`native64/chrome-package-ui-document-profile.log` completed successfully. Actual
Chrome opened the preserved guest-downloaded Google `.deb`, the native GUI installed
it, and Run selected launched its new executable. Real keyboard input navigated
that new Chrome to `https://example.com/`; the resulting screenshot contains the
actual English document paragraph, not only the title/background. The stricter
pixel gate and visual review both passed. Repeat-open did not reinstall/auto-run;
both browsers/installer closed normally and independent host readback verified the
official archive hash, receipt and original installed executable hash. The one
temporary 3 GiB image was deleted. `native64-chrome-package-ui/result.json` now has
both rendering verification fields true; earlier partial/failed evidence remains
explicitly separated. Google stable metadata was rechecked at this checkpoint and
still matches version **155.0.8059.39-1** and the pinned SHA-256.

An independent review found and fixed a nested-atomic-replacement bug in the
prepared publisher before it touched the permanent image. It now calls `fat.edit`
only on the outer storage migration's disposable staging file. Actual publisher
helpers were tested on small images: successful single replacement, and refusal
with the original image unchanged after either unrelated-file or reserved-region
corruption. Evidence: `C:\nexustest\publication-check/result.json`; fixtures removed.

Publication then passed on the real permanent image. **4,545 existing entries**
and every reserved-region byte were preserved; the two existing runtime files
`sbin/init` and `bin/nexus-install` were updated, and `bin/xdg-open` was added.
Normal arguments, profiles, Chrome/Hello and all other existing files matched their
pre-update hashes. Capacity is now **3 GiB**, with sparse host storage and room for
another Chrome bundle. `native64-installer-publish/result.json`, before/after file
manifests and `native64/installer-desktop-publication.log` record the checks.

Fresh acceptance on the published image was started via
`tests/test_installed_desktop_flow.py C:\nexustest\native64-installed-desktop-flow`.
It uses the untouched normal startup arguments and existing profile, with no
test inspector, DevTools, staged configuration or cloned disk. It verifies public
document pixels, types the Hello HTTPS URL, opens the new download (including any
numbered filename suffix), installs/runs it, and verifies persisted hashes and
normal shutdown. On failure it preserves the permanent image and leaves a live
VM available for diagnosis. **Never delete that disk as test output.**

**Normal-profile acceptance passed with manual input takeover on the same VM.**
The original automated URL input lost its initial `h` and opened a search; its
observer was stopped without restarting QEMU. A slower mouse-focused retry was
visually checked before Enter (`hello-url-verified.png`). The full final `.deb`
was visible after pending rendering completed. Chrome downloaded a fresh
`hello_2.10-5_amd64 (1).deb`; real file click, native Install and Run displayed
`Hello, world!` (14 bytes, status 0). Esc/Alt+F4 then exited QEMU PID 17660 normally
with code 33 and no fault/resource-leak markers. No VM remains from this check.

Offline readback verified archive length/hash, bundle receipt and Hello ELF,
original Chrome/Hello entries, runtime/boot hashes and unchanged normal arguments.
The new archive and content-addressed bundle were absent from the pre-publication
manifest. Evidence: `native64-installed-desktop-flow/result.json`, `shutdown.json`
and screenshots; copied proof in `screenshots/native64-normal-installer.png` and
`screenshots/native64-normal-installed-hello.png`. This is explicitly **not** a
claim that the original automated script completed unattended. Input queue
overflow remains a possible mechanism, not a confirmed production keyboard bug.

The harness now waits for a visibly selected omnibox before typing and the complete
rendered Hello URL before Enter. Two reviewed fixture crops total 5,458 bytes;
they are intentionally pinned to the current Chrome/fonts/1080p and carry URL
metadata. Self-checks reject partial text/selection, extra suffixes and unfocused
frames; seven actual screenshots classify correctly. Real Ctrl+L focus was captured
after a normal cold boot in `native64-normal-input-check`, which also exited cleanly.

**The corrected unattended run then passed**, using the same permanent disk:
`tests/test_installed_desktop_flow.py C:\nexustest\native64-normal-installer-synchronized`.
It rendered the public HTTPS document, downloaded a fresh `hello_2.10-5_amd64 (2).deb`,
reused the prior installation via its receipt, ran Hello with visible 14-byte output,
and exited normally. Persisted package/receipt/ELF hashes and original
Chrome/Hello/runtime/boot/config hashes passed. This repeat checks reuse rather than
a second extraction; the preceding normal-profile run proves first installation.
Evidence: that directory's `result.json`/screenshots and
`C:\nexustest\normal-installer-synchronized.log`. No VM remains running.

Security scope was independently checked: unsupported seccomp/prctl filter calls
return ENOSYS, not fake success, and no sandbox-disable launch flags are used.
Implemented user/PID namespaces, capabilities and no_new_privs do real work.
Nevertheless, syscall filtering and Linux-equivalent Chrome sandbox protection
are absent; page rendering is not evidence of sandbox security.

## Disk-space incident and current work (2026-10-11)

The user reported excessive host disk use. Completed test runs had retained
separate 1-2 GiB raw VM copies. Removed **67 disposable images / 114 GiB** after
checking their resolved paths, excluding reparse points and checking active QEMU
command lines before each deletion. `C:\nexustest` fell from **126.96 to 12.96 GiB**;
C: free space increased to about **172 GiB**. Exact deleted paths/byte counts:
`C:\nexustest\cleanup-20261011-000731.json`. Historical test logs, screenshots,
packet captures, source/recovery snapshots and repo/base images remain intact;
historical disk-image paths in the evidence below may now be absent.

Follow-up cleanup (2026-10-11): removed another **168 completed test images /
5.016 GiB logical** (4.984 GiB measured free-space gain). At that checkpoint the
folder was **8.95 GiB**, with about **185.95 GiB free on C:**. Manifest:
`C:\nexustest\cleanup-remaining-test-images-result.json`. Source/recovery
snapshots, logs, captures, screenshots and repo images were preserved. The then-live
`native64-chrome-installer/disk.img` was a **2 GiB temporary clone**, excluded from
that cleanup and subsequently removed by its controller. The permanent
desktop remains `arch/x86_64/out/desktop64.img` and must never be swept as a test
artifact. Existing historical measurements above describe the earlier cleanup.

Latest read-only inventory after the user's renewed disk-space report:
`C:\nexustest` is **7,589,243,169 bytes (7.07 GiB)**, with **183.3 GiB free on C:**.
Evidence: `C:\nexustest\storage-audit-20261011.json`. At inventory time the only
live QEMU used the permanent image outside this folder; it subsequently exited
normally. No temporary multi-GiB native Chrome clone remains here. Most remaining
space is historical source/recovery images and
download/network evidence. Those retained files were not swept or deleted.
Final inventory after the small screenshot/focus checks is **7.09 GiB**, with
**183.5 GiB free on C:**; `C:\nexustest\storage-final-20261011.json` records bytes.
Those checks created no disk clones and all their VMs exited cleanly.

Do not recreate a disk per diagnosis and retain it indefinitely. The graphical
Chrome probe now removes its private disk after VM shutdown and validation,
including failed VM runs. A successfully verified download deliberately retains
its disk for the native installer step. Headless Chrome and storage-benchmark
probes also clean their private disks on validation errors as well as success;
all three refuse to overwrite an existing work disk. Both Chrome probes now also
clean owned images after partial-copy, import, staging and VM-start failures.
They stage disposable images in place, avoiding another multi-GiB copy; installed
images retain atomic staging. `tests/test_chrome_probe_cleanup.py` passed **19**
tiny-fixture lifecycle checks, including preservation of existing/permanent images,
verified downloads and an image whose VM could not be stopped. Log:
`C:\nexustest\probe-cleanup-check.log`. No real QEMU/build was run for this host-only
fix. Forced termination of Python can still bypass cleanup: confirm VM ownership
before removing an abandoned private image. Never sweep backup/source images.
The graphical cleanup was verified with a real successful `client` run in
`native64-disk-cleanup-check`: logs and actual input/display screenshot remain,
the 2 GiB disk and control socket do not. The scripts also pass Python compilation.

The successful download workspace is `C:\nexustest\native64-chrome-download-cpuinfo`;
its outer log is `native64/chrome-download-cpuinfo.log`. Its disk has since moved
to the persistent desktop path above. The original `native64-chrome-download` run reached 39,073,072 of
143,552,428 bytes, then failed the 900-second guest deadline and exited normally
through the diagnostic failure path. It did **not** complete or get manually
aborted. Its logs/pcap remain; its disposable disk was cleaned above.

Changes since milestone 27, not yet a completed download/install milestone:

- Real PS/2 click on the official Google package link, guest SHA-256/fsync and
  independent persisted-file verification are implemented in the download probe.
  Pin: Chrome `155.0.8059.39-1`, 143,552,428 bytes, SHA-256
  `c58aa0f2cd66179c9f050e062c882d27aa9b9f8c2b7c73fee3498560b5ed0b38`.
  No package payload was host-staged into this download test. The retry is still
  in progress at checkpoint; no complete native installation is proven.
- CPU sampling of the first run found 33/40 samples in ATA waits. Writeback now
  writes dirty data before zero-extending any remaining tail, preserving shrink
  ordering; FAT zeroing uses 4 KiB chunks; ATA batches up to 256 sectors while
  keeping per-sector DRQ waits, calibrated deadlines and final durable flush.
- Durable 16 MiB writes improved from **88,702 ms to 17,359 ms (5.11x)** on the
  same FAT geometry; both runs passed reboot/readback and independent byte/hash
  verification. Results/logs remain in `native64-storage-change` (large clones
  removed). Production ATA/FAT tests, cache-writeback tests, native two-boot file
  persistence and real writeback fault injection passed. Full native matrix and
  i386 rebuild/regression after these shared storage changes are still pending.
- Latest native kernel: **274,798 bytes**, SHA-256
  `4ddb88cd4a107c346911212276d9f10cc9d54e709bd46b22d1c51bfd4845c643`.
  Boot image SHA-256:
  `74370a3f8f901dc6ad665432d30b870ccdb145fa2791a72eac996d1945c3adfb`.
  Inspector uses existing house crypto via `NEXUS_HOSTED`, **127,752 bytes**,
  SHA-256 `31652c553ccd15844ddc20e7d004978734f8a045ef81c30721264d0c802d87b8`.
  Milestone 27 hashes below describe the earlier verified network checkpoint.

## Native installer and Chrome download checkpoint (2026-10-11)

- `native64-chrome-download-fast` received the full package, but Chrome correctly
  blocked its insecure HTTP initiating page. The warning is visible in
  `screenshots/native64-chrome-insecure-download.png`; the guest deadline later
  expired. The private disk was removed after confirmed exit; logs/pcap remain.
- The harness now opens a new tab with actual PS/2 keys and types the official
  HTTPS package URL into the omnibox. No download-security or TLS override was
  added. The subsequent `native64-chrome-download-https` run **completed the real
  143,552,428-byte download**; guest SHA-256/fsync matched the official digest,
  the actual download-history window showed completion, and Chrome/desktop
  exited normally. Screenshot: `screenshots/native64-chrome-download.png`.
  **The overall harness failed** because a ChromeML subprocess deliberately
  trapped after `Failed to initialize cpuinfo` / Dawn initialization failure.
  Unfortunately the failure cleanup removed that run's disk before independent
  host readback; it has no `download-result.json` and cannot be used to install.
  Do not describe that run as a full PASS or claim an installed Chrome exists.
- Fixed retention: a guest-verified download survives later test/readback errors.
  Future runs also record independent package readback before the subprocess
  fault assertion, without treating a subprocess fault as a passing browser run.
  There is still only one ongoing large download disk, rather than a disk per
  diagnosis. Use that same disk for installation and subsequent app tests.
- Added actual CPUID-derived `/proc/cpuinfo` (vendor, family/model/stepping, APIC
  ID, brand and supported FPU/SSE feature flags). It reports the scheduler's one
  online boot CPU; it does not advertise AVX/XSAVE state the scheduler cannot save.
  Readonly stat/open, descriptor paths, directory enumeration, partial reads and
  seek/pread are verified on `max`, `qemu64,+rdrand`, and `max,-aes` in
  `native64-cpuinfo`. `sched_getaffinity` remains unimplemented and was not part
  of this proc-file change. Full CPU/disk/isolation/ISO matrix passed again:
  `native64/cpuinfo-matrix.log`. ChromeML initialization remains subject to the
  current real Chrome retry; CPU metadata tests alone do not prove it.
- Latest native kernel: **274,798 bytes**, SHA-256
  `ac177823ff58d091960b27a63cc45c6a32a8ca6bfadf17ac98a586d5a4a5a122`;
  boot image `5f9c99969a99491e5a5eee37d92b9507fc429ebdedd785b3086cab6958764c75`.
  The earlier storage-only kernel hashes above are historical.
- Shared storage changes also passed the rebuilt i386 TLS/network campaign:
  **33/33**, no `!EX:` / `!KR:`, `native64-storage-i386/serial_network.log` and
  `native64/storage-i386-regression.log`. Its 1 GiB disposable disk was removed
  after confirmed VM exit; logs/screenshots remain.

`arch/x86_64/install_user.c` is the native `nexus-install` application; build with
`python -I arch/x86_64/installer.py`. It streams ar/XZ/tar using the already-staged
liblzma runtime and house SHA-256, creates a private staging directory, refuses
path traversal/unsafe members/symlink parents/overwrites, confines package links
to the bundle, fsyncs output, verifies the package digest, then publishes using
`renameat2(RENAME_NOREPLACE)`. Failed extraction removes only its own staging
tree. The disk-full test found that cached unwritten bytes prevented cleanup;
rollback now explicitly truncates its own newly-created partial output first.

The native installer campaign passed **23 cases**, including real disk-full
rollback with unchanged free-cluster counts, bad checksums, truncation, malicious
paths/links, duplicate files, existing destinations, and both GNU/ustar headers.
Actual Debian GNU Hello was installed and its installed binary printed exactly
`Hello, world!`; a second native install/execute session helper also passed.
Evidence: `native64-installer/serial.log`, `native64/installer.log`. Small fixture
packages here are host-staged tests, **not evidence of a Chrome-downloaded app**.
The session helper accepts LF/CRLF configurations. The pinned Hello binary has
an empty `--version` value (no `2.10` literal in its original ELF), so its real
default greeting is verified rather than inventing a version-output expectation.

The installer currently creates a private user application bundle under
`/home/nexus/Applications`; it is not dpkg and does not run system maintenance
scripts, change system ownership, or preserve POSIX mode bits unsupported by FAT.
Existing private-path access rules apply. Other compression/member formats fail
explicitly. Installer ELF after adding progress output: SHA-256
`9d209f5a92d4b31919df8e97fb80a18e71e550cb092c01c07b1be9b9c017a563`.

The completed native download/install/verification sequence was:

```powershell
python -u -I tests/install_download64.py C:\nexustest\native64-chrome-download-cpuinfo chrome
python -u -I tests/probe_desktop64.py C:\nexustest\native64-chrome-download-cpuinfo installed-https --ipc-connection-timeout=120
python -u -I tests/probe_desktop64.py C:\nexustest\native64-chrome-download-cpuinfo installed-download --ipc-connection-timeout=120
python -u -I tests/install_download64.py C:\nexustest\native64-chrome-download-cpuinfo hello
```

These steps have now all passed on the guest-downloaded packages (see latest state).
The first stages only the native installer/session helper and config, then boots
the existing downloaded disk and installs inside NexusOS. It checks the actual
installed executable and all cached upstream Chrome resource bytes. The remaining
probes launch `/home/nexus/Applications/chrome/opt/google/chrome/chrome`, verify public HTTPS and real
page typing/click/pixels, and downloads Hello through that installed Chrome. Its
artifacts go to `installed-https/` and `installed-download/` while reusing the same disk. The final step
installs that guest-downloaded Hello and checks its actual greeting. Do not mark
the goal complete until these real operations pass, including remaining runtime
faults. No commit or physical-disk installation is authorized.

## Current state

- Git root: `C:\Users\admin\Desktop\New folder`; project: `NexusOS/`.
- Branch `perf-1080p-modern-ui`, HEAD `70871d6`. No staging or commit performed.
- Last status: 150 tracked changes (including deletions), 108 untracked entries.
  Entries include whole directories, not just individual files.
- TLS, browser and userland/apps streams are integrated into the main tree.
- `build.bat apps` builds the OS and installs 25 program filenames in `/disk/bin`
  (including multicall aliases and self-checks). `build.bat run` launches it.
- Kernel: **1,063,852 bytes**, zero compiler warnings, cap **1,441,792**.
- Current i386 kernel SHA-256: `554a04204ec8caa27e011724642796115e79ac6297db598f850b4d1fb2862c4a`.
  Before sparse fragmented-file seek indexing it was
  `fbffe5a8741f4cf7cba79fe582f6c886b46989105829a700e75a41ef9f04fcef`.
  Before batched ATA writes and FAT zeroing changes it was
  `7d4241c1da7e8d9dc6b3e1744232b54af61c0884bbbaaef22e003c13b7bf3e14`.
  Before UDP checksum/destination validation it was
  `b0968688e18970694f968d5e766be321f911a9544100243133f3dcdc1f60d754`.
  Before native socket error/Nagle/read-shutdown transport support it was
  `9262540a1b6d1b264811c74630b03d8505e0e09c38b7652ba7901e1a81c77c19`.
  Before the network portability/reset/packet validation changes it was
  `98e735561331a13e66aaab1e3e46d40dd19877dcc6947f0692583bc2d7f9fef5`.
  Before FAT seek caching/multi-sector reads it was `3e781ea31e2e82d01d6d0eb74bb6a540de8050d8d693b1336ff7a621496c44d6`.
  Before checked FAT lookup it was `cbb92b8a3214ed46cef61b434e6726eaca082e299c9dfdbbd09d604e6c653170`.
  Before the native inode snapshot field it was `8cccce1f815dc8fedda7b4d82c07f8edf2d895eb26a0fc90e7ea8e03b48558cf`.
  Before the rename storage layer it was `7427c5cee74eb74187bc3c85df394dcb3d685e27928b84a293f3074cf2fae04d`.
  Before FAT free-space accounting it was `9ace12652a28634179ffb58c9fbcc199fbe1013f46699d82a9a55c9e1a789e9e`.
  Before metadata-first FAT truncate it was `430009ebb04451ae13f2d72febb061a62afd3366c79c5cc42ad54f4adb82b660`.
  Before calibrated ATA deadlines it was `4c1583c55d49b8475d03fbdef36e67c9c896f2cd114167fd02f66fc4c918c0ef`.
  Before native socket/link metadata support it was `70627c7e25eb6519702faaa359f1793627cc36c28af09c6c2d9db4c697bd84ed`.
  Before the additive directory-read helper it was `72254af4f7be50360a46a86e4b1a35e1be45f324bd192a6bdcaf61a0790c10f7`.
  The earlier pre-native-work hash was `417d05f730a83f404c1745696d6aa740810b1016d45ee719dd8f22d34e9943a9`.
  Latest rebuild is 1,063,852 bytes with zero warnings and passed the full 33/33
  TLS/network campaign in `C:\nexustest\native64-udp-i386-retry`.
  The earlier build passed 3/3 fresh login-stage boots in `C:\nexustest\native64-i386`.
- Outputs: `nexus.img`, `nexus.iso`, `disk.img` with installed apps.
- Pre-integration backup: `C:\nexustest\before_dailyuse_20261009_220038`
  (source and images, excluding tools/handoff).

## Chrome goal and milestone history

The user explicitly rejected a Linux-based edition or an external OS runtime.
Keep NexusOS's own kernel. The specified goal is **actual Chrome installation and
interactive page rendering**; the milestones below do not by themselves complete
that goal. The current end-to-end completion audit above supplies that proof.
No commit or physical-disk installation is authorized.
Latest user clarification: keep working until the full download -> install ->
launch path works in NexusOS. Verify Chrome browsing/downloads and a downloaded
application's install -> launch path too. A window opening or host-side staging
alone does not satisfy this gate.

Native x86-64 bring-up now lives in `arch/x86_64/`. It reuses our BIOS boot stages
and font data. Build/run/test with `build.bat x64`, `build.bat x64 run`, or
`build.bat x64 test`. Output is `arch/x86_64/out/nexus64.img` and `nexus64.iso`;
the existing i386 desktop images are separate. A checksum-pinned Zig 0.15.2
compiler is only a host build tool. No external OS kernel is used; the application
shared libraries are explicitly listed in the milestones below.

Verified first milestone (`C:\nexustest\native64`):

- Long mode, E820 usable-memory discovery, four-level paging, kernel W^X,
  unmapped null/stack-guard pages, 64-bit GDT/TSS/IDT and SYSCALL entry.
- Static ELF64 ring-3 program at `0x100400000`, stack above 4 GiB, proper initial
  argc/argv, zero BSS, isolated page tables, write/getpid/uname/exit syscall ABI.
  Unknown syscalls return ENOSYS; bad pointers return EFAULT.
- Twelve malformed/unsupported ELF cases rejected. Kernel read, null access,
  code write, NX execution, invalid opcode and privileged CLI faults contained;
  their exact vectors/error codes asserted. Process arena reclaimed, then another
  user program ran successfully.
- Three successful floppy runs (`max` 256/128 MiB and `qemu64` 64 MiB), explicit
  rejection of CPUs without long mode or NX, and successful ISO boot. Screenshot:
  `screenshots/native64-foundation.png`. `tests/test_x64.py` checks actual guest
  markers and VM exit codes; `tests/test_x64_image.py` checks image conversion.
- Fixed Zig objcopy raw output moving data away from ELF load addresses. The
  builder now copies validated PT_LOAD segments at their physical offsets,
  with a regression for the GDT address/gap and malformed segment ranges.
- Rebuilt i386 after the shared `size_t` fix and conditional E820 boot addition:
  kernel remained 1,059,756 bytes with the pre-native-work SHA-256 above; boot.bin remained
  `21f8f2845e9cfed5b3d856581b727095222979d307a5192d79a0e9500a307929` and boot2.bin
  remains `031168bc473276aceb9dd40eeeb124bc929294314deabd4fbcf5e90da50c3b6a`.

Second milestone verified (`C:\nexustest\native64\disk.log`, `iso.log`):

- Reused the existing NexusOS ATA, FAT32 and string code in the 64-bit kernel.
  Pointer-alignment checks now use `uintptr_t`; at this milestone the rebuilt
  i386 kernel still matched the pre-native-work SHA-256 above.
- The ELF loader reads program headers and streams segment bytes from FAT
  directly into user pages. A 3,167,408-byte executable loaded from
  `/bin/selftest64` and checked every byte of its 3 MiB test payload in ring 3.
  It is larger than the boot-image cap and is not embedded in the kernel.
- Native read-only file APIs now implement open/openat, read/pread64, lseek,
  stat/fstat/lstat/newfstatat and close, with the x86-64 stat layout, descriptor
  lifetime and user-pointer checks. The user test verifies cross-page reads,
  EOF, seek, positional reads, errors and an actual 10,003-byte file.
- Truncated and missing disk executables were rejected; process allocations
  were reclaimed. Both floppy and ISO paths passed. Test-disk SHA-256 remained
  unchanged across both boots. Current x64 kernel is **65,608 bytes**.
- `build.bat x64` builds a separate `arch/x86_64/out/apps64.img` (64 MiB), using
  the existing atomic FAT installer and storage migration. Tests attach a copy;
  interactive `x64 run` uses a QEMU snapshot. Main `disk.img` is never attached.
  Updated screenshot: `screenshots/native64-files.png`.

Third milestone verified (`C:\nexustest\native64\dynamic-campaign.log`):

- Native ELF64 PIE/PT_INTERP loading, randomized program/interpreter/mmap bases,
  initial auxiliary vector and secure AT_RANDOM. FS/GS bases support user TLS;
  x87/SSE state starts clean. The OS remains NexusOS's own kernel.
- A real dynamically linked C program runs with unmodified, checksum-verified
  glibc 2.41 application libraries/interpreter (not an external OS instance).
  Archive metadata: [Debian libc6 package](https://packages.debian.org/trixie/amd64/libc6/download).
  Only selected library files and their license are extracted; package scripts
  are never executed. Source/bootstrap is `arch/x86_64/runtime.py`.
- mmap/mprotect/munmap/brk, demand paging, file-backed mappings, virtual-area
  splits and physical page reuse. A sparse **1 TiB** reservation is tested by
  committing/touching its first and last pages, then releasing it.
- Dynamic test verifies libc printf/float formatting, TLS, malloc, stdio and
  getrandom. It touches/releases 160 MiB cumulatively on a 128 MiB VM, checking
  zeroed reused pages, MAP_FIXED_NOREPLACE, PROT_NONE restoration, initial user
  W^X refusal (superseded by the explicit user JIT permissions in milestone 16),
  protection over an unmapped gap and EOF behavior.
- Real protection/unmap faults terminate the user process; a file mapping past
  EOF gets the default SIGBUS termination status. Another dynamic program then
  runs successfully. Signal handlers/delivery APIs are not implemented yet.
- Normal disk/ISO runs each have exactly nine intentional user faults, no
  kernel faults, and two successful dynamic runs. Missing RDRAND fails closed
  for dynamic execution (no weak randomness fallback). Test disk bytes remain
  identical. x64 kernel: **77,896 bytes**; i386 kernel hash still unchanged.
- `set_robust_list` (273) and `rseq` (334) explicitly return ENOSYS; glibc tolerates
  those missing optional services in this single-process test. Do not turn them
  into fake success stubs when implementing threading.

Fourth milestone: the actual **Google Chrome 155.0.8059.39 `--version` command
now runs on the custom kernel and exits with status 0**. This is not interactive
browsing or the completed installation/download workflow.

- `arch/x86_64/chrome-lock.json` pins 143 application/library archives by version,
  URL, byte count and SHA-256. `chrome.py` verifies/extracts selected Chrome files,
  libraries, resources and licenses into a separate 2 GiB `out/chrome64.img`.
  It never executes package scripts or boots an external OS. This host bootstrap
  is explicitly not the final in-OS installer. Chrome's recursive DT_NEEDED
  dependency check resolves 81 ELF images. FAT symlink path aliases are currently
  materialized copies; real filesystem symlink support is still needed.
- The installed 297,798,984-byte Chrome ELF was read back from FAT and matched
  SHA-256 `9bfb381296dffe75f3419d07b6cc64525105ed117d1453a4ef3b291cb97d2b58`.
  The official package SHA-256 also matched Google's repository metadata:
  `c58aa0f2cd66179c9f050e062c882d27aa9b9f8c2b7c73fee3498560b5ed0b38`.
- Native clocks use PIT **channel 2** calibration of TSC plus the existing RTC
  driver. IRQ0/PIT channel 0 is unchanged. Real libc clock tests pass.
- `/proc/self/exe` readlink, cwd and argc/argv/environment handling now work.
  `madvise` discards/reloads anonymous, file and ELF-backed pages correctly.
  File-backed ELF loading now faults pages in on demand. The version probe fell
  from **82.5 seconds to 9.1 seconds** under the same QEMU/2 GiB test setup.
- `signal.c` implements action registration, masks/pending state, standard
  coalescing/realtime queues, alternate stacks, synchronous delivery and
  rt_sigreturn with register/FPU restoration. Tests cover real glibc handlers,
  SIGILL recovery, masks, altstack, SSE preservation, and rejection of malicious
  kernel-CS/MXCSR signal frames. Job-control stopping and multithreaded delivery
  still need the scheduler; no blocking futex wait queues exist yet.
- Current core regression has three successful dynamic runs, exactly ten
  intentional user CPU faults and two rejected forged signal returns per disk/
  ISO run, with no kernel fault. Read-only shared file mmap was added for Chrome's
  ICU data; its read/write-protection regression passes. Writable/shared-anonymous
  mappings remain unsupported and require real backing/coherence implementation.
- Probe commands: `python -I arch/x86_64/chrome.py` stages development files;
  `python -I tests/test_chrome64.py` checks the real version command;
  `python -I tests/test_chrome64.py C:\nexustest\chrome64-headless headless` probes
  the renderer. Optional `/nexus/chrome.args` supplies one argument per line.
  A Chrome disk boots directly into its selected application probe; the smaller
  `apps64.img` continues to run the foundation tests.
- Version evidence: `C:\nexustest\chrome_support\version-demand.log` and
  `C:\nexustest\chrome64\serial.log` (the latter may be replaced by later probes).
  Latest successful foundation campaign: `C:\nexustest\native64\shared-read-campaign.log`.
  Latest native kernel size: **90,184 bytes**. Chrome headless/interactive launch
  is still being debugged. No browser page, network download, in-OS installation
  or installed-app launch completion has been claimed.

Fifth milestone:

- Native mkdir/rmdir and file create/write/pwrite/truncate/fsync are working in
  private FAT subtrees `/home/nexus`, `/tmp`, `/run/user/1000`. Canonicalization
  handles dot/parent components before the write policy, including case-insensitive
  FAT prefix comparisons. System paths stay read-only to the current user.
  Per-file Unix modes are not stored by FAT; these subtrees use private mount
  permissions. Resolve link targets before applying this policy when real
  symlink support is added.
- `fat32_set_times` was added to the shared FAT driver. Native writes/creates
  get current RTC timestamps; shared read mappings see writes and truncations.
  Each ATA write already flushes its device cache; fsync reports retained write
  failures. Writable shared mappings, rename and open-file unlink semantics
  still need work.
- `tests/test_x64_files.py` verifies actual disk bytes across two boots, exclusive
  creation, bad pointers, truncation/zero fill, mmap coherence, path traversal
  denial, allocation counts and exact preservation of the protected first
  16 MiB. Artifacts: `C:\nexustest\native64-files`.
- File descriptors now separate descriptor handles from referenced open
  descriptions: dup/dup2/dup3, fcntl flags and shared offsets work, including
  redirecting/restoring descriptor 0. The current registry is still single-process.
- `ipc.c` implements native AF_UNIX stream/SEQPACKET pairs, message boundaries,
  credentials and SCM_RIGHTS, nonblocking reads, shutdown/EOF and ancillary
  truncation. Tests include 20,000-byte payloads, peek, forged credentials,
  invalid pointers, in-flight descriptor lifetime and 64 reference cycles.
  Closure is iterative; descriptor graph collection prevents orphaned cycles.
  Kernel asserts all socket endpoints are reclaimed at process exit.
  Blocking empty reads and MSG_WAITALL are explicitly unsupported pending a
  scheduler; credentials currently describe the only process (PID 1, UID/GID 1000).
- Full native boot/CPU/ISO, dynamic/runtime/signal/IPC and filesystem tests pass.
  Latest log: `C:\nexustest\native64\checkpoint-fixed.log`.
  Native kernel: **102,472 bytes**, SHA-256
  `7c43fe059916a48162298eafa91d5f977203366c697675956fb92a4f96946508`.
- A real boot regression was found and fixed: protect_kernel must leave the
  executing first 2 MiB PDE executable until its replacement 4 KiB page table
  is complete. Marking PDE 0 NX earlier can fault during the transition depending
  on code layout/TLB state. Native matrix passed after the fix. i386 additionally
  passed 3/3 login-stage boots (`C:\nexustest\native64-i386`).

Sixth milestone:

- `memory.c` replaces arena rewinds with global physical-page ownership/refcounts.
  `kernel.c` now has independent address-space objects and real COW fork: private
  writable pages copy on write, including pages made writable after fork. Shared
  file metadata/coherence updates visit every address space. Self-checks assert
  precise reclamation rather than freeing another process's allocations.
- Descriptor tables now belong to individual execution contexts. Fork preserves
  shared open descriptions/offsets with independent descriptor handles. Blocked
  kernel I/O pins the original description, so closing/reusing an fd cannot
  destroy a sleeping operation's endpoint. Signal contexts isolate masks/pending
  queues and support refcounted shared handler tables.
- `process.c` implements per-task guarded 64 KiB kernel stacks, cooperative
  kernel continuations, IRQ0 preemption of ring 3, FS/GS and FXSAVE context
  switches, child IDs, COW fork/clone, VM-sharing vfork, execve, wait4/waitid,
  sessions/process groups, SIGCHLD, orphan adoption and cleanup. The PIC is
  remapped; PIT channel 0's divisor stays unchanged. All user/signal returns
  force IF and remove privileged flags. Guard-page identity tables are permanent
  kernel allocations, so tests compare against the boot allocation baseline.
- Unix socket reads now block/wake across real processes. Credentials are
  captured from the actual sending/creating task, and queued descriptor rights
  survive source close. SA_RESTART and EINTR behavior are tested. Unknown
  sigaction flags are masked per the ABI, fixing glibc's sign-extended
  SA_RESETHAND used by Crashpad. One-shot handler/reset behavior is tested too.
- The ELF loader prepares a replacement address space before committing exec.
  argv/environment are snapshotted and bounded by the stack budget; invalid
  pointers/malformed executables leave the existing process intact. Successful
  exec closes CLOEXEC handles, resets caught signal handlers/altstack/TLS/FPU,
  preserves masks/ignored handlers/pending signals, updates `/proc/self/exe`,
  and releases a vfork parent only after the address-space switch.
- `process_user.c` tests actual unmodified glibc fork and posix_spawn, COW/TLS,
  interprocess credentials/SCM_RIGHTS, blocking reads, timer/FPU preservation,
  exit/signal status, repeated fork/reap, ignored SIGCHLD, session/group rules,
  interrupted/restarted I/O, exec failure rollback, arguments/environment,
  CLOEXEC, signal inheritance/reset, and waitid WNOWAIT. The boot runner asserts
  all per-program pages and IPC endpoints are reclaimed afterward.
- Full native CPU/floppy/ISO regression passed:
  `C:\nexustest\native64\process-final.log`. Filesystem/reboot checks passed
  (`process-final-files.log`); the original Chrome version command passed in
  11.2s (`C:\nexustest\chrome_support\version-processes.log`). Native kernel:
  **123,028 bytes**, SHA-256
  `2482cb5ad1980c02420e4f77d5eaf93886b55cd3c8bc5effddf0c33359f21974`.
  The original i386 kernel still hashes to
  `72254af4f7be50360a46a86e4b1a35e1be45f324bd192a6bdcaf61a0790c10f7`.

**At milestone six Chrome passed fork, setsid and posix_spawn/execve and started
its actual Crashpad executable.** The next fatal helper failure was pthread_create, clone
flags **0x003D0F00** (decimal 4001536). Evidence:
`C:\nexustest\chrome_support\headless-exec.log` (also `headless-process-final.log`). Main Chrome also hits missing
pipe2 (293) and flock (73); procfs fd enumeration, prctl and signal wait/queue
operations are still missing. No headless DOM or interactive page has rendered.
Do not bypass Crashpad/sandbox or return fake thread IDs to pass this gate.

Seventh milestone:

- Process wait records now outlive individual threads. CLONE_THREAD with
  CLONE_VM/FILES/SIGHAND/SETTLS, parent-TID writes and clear-child-TID wakeups
  work with the unmodified glibc pthread runtime. Process PID and thread TID are
  distinct. Threads have separate masks, altstacks, registers, TLS and FPU state;
  requested memory, descriptors and handlers are shared. CLONE_FS currently
  shares an immutable root cwd; cwd mutation and SysV semaphore syscalls are
  still absent, so those states have no mutable objects yet.
- Process-directed signals use a shared pending queue, while tgkill/tkill and
  synchronous faults target a particular thread. Another thread can unblock and
  receive a previously blocked process signal. SIGPIPE from a socket write is
  directed to the writing thread. Default fatal signals and exit_group wake
  blocked siblings; their kernel continuations unwind and drop pins before
  stacks and shared resources are freed. The initial thread can exit while the
  process remains alive. Fork from a worker records the process as the parent.
- Native futex WAIT/WAKE and WAIT_BITSET/WAKE_BITSET now have real wait queues,
  mask selection, relative/absolute deadlines, monotonic/realtime clock handling,
  pointer/value checks and clear-child-TID wakeups for pthread_join. IRQ0 wakes
  timed waits without changing its PIT divisor. Private/shared key classes are
  distinct; shared file keys use file identity plus offset across different
  processes/virtual addresses. Add inode generations with open-file unlink.
- `threads_user.c` exercises four real pthreads contending over mutex/condition
  variables, thread-local storage, per-thread FPU modes, join/TLS destructors,
  pthread_kill and blocked process signals, shared-fd close, worker fork,
  descriptor reuse during a pinned blocking read, selective futex wakes,
  cross-process shared-file futex wakes, and both timeout clock forms.
  Separate runs verify initial-thread exit, exit_group with siblings blocked in
  socket/futex operations, and fatal thread signal status (143). Every run must
  reclaim exactly its owned resources. Existing native tests still pass.
- Latest successful full CPU/floppy/ISO regression:
  `C:\nexustest\native64\threads-edges.log`. Native kernel: **127,132 bytes**,
  SHA-256 `77b2e6ac8c4d51afc8f909bdb9096da7239bc35ccf9e9a120d12ea378975dd3f`.
  Two-boot filesystem regression: `C:\nexustest\native64\threads-files.log`.
  Actual Chrome `--version` still passes in 11.3s:
  `C:\nexustest\chrome_support\version-threads.log`. ELF image-layout tests pass.
  The i386 kernel still has its unchanged `72254af4...` hash above.

**Chrome's pthread_create failure is gone.** Its actual helper threads start;
the next helper failures are epoll_create1 (291) and getdents64 (217). Main
Chrome still hits pipe2 (293) and flock (73), then aborts. Evidence:
`C:\nexustest\chrome_support\headless-threads.log`. No headless DOM or graphical
page has rendered; keep the full download/install/run goal active.

Eighth milestone:

- `pipe.c` implements anonymous pipe/pipe2 with CLOEXEC/nonblocking descriptors,
  bounded ring buffers, blocking reads/writes, EOF and thread-directed SIGPIPE.
  Writes up to PIPE_BUF remain atomic, including writev. F_GET/SETPIPE_SZ resize
  preserves queued bytes; FIONREAD reports available data. Default storage is
  64 KiB and size requests round to powers of two with allocation/error checks.
- Native readv/writev snapshot iovecs and pin the original open description.
  Unix SEQPACKET writev is one record, and pipe atomicity spans all iovecs.
  Buffer validation is shared by pipe/eventfd I/O and repeated after blocking.
  Tests cover a 200,000-byte fork transfer, simultaneous writer threads,
  buffer wrap/resize, no partial small writes on EAGAIN/EFAULT, duplication,
  EOF, read/write direction errors and real default SIGPIPE termination.
- `event.c` implements poll, epoll_create/epoll_create1, epoll_ctl, epoll_wait
  and epoll_pwait. Resource changes drive readiness and scheduler wakeups;
  deadlines use the existing monotonic timer. Tests cover level/edge/oneshot,
  rearming, delivery fairness, full 64-bit user event data, descriptor aliases,
  automatic removal at final close, timeout, invalid descriptors and EOF.
  Watches are weak references, so they do not keep a pipe/socket peer alive.
- epoll_pwait temporarily changes the calling thread's signal mask. Signal
  delivery saves the original mask in its frame, and timeout/error returns
  restore it too. The regression blocks SIGUSR1, temporarily unblocks it while
  waiting, checks EINTR and the handler's saved mask, then checks restoration.
- eventfd/eventfd2 implement real 64-bit counters, semaphore reads, overflow
  blocking/EAGAIN, shared descriptors, CLOEXEC, epoll readiness and vector I/O.
  Tests exercise blocking readers/writers and all-ones rejection. getresuid/
  getresgid expose the current immutable UID/GID 1000 identities with pointer
  validation; credential mutation and user namespaces are not yet implemented.
- Corrected Unix socket readiness: graceful shutdown is HUP/RDHUP, not a
  spurious EPOLLERR with SO_ERROR=0. Closing with unread incoming data records
  ECONNRESET; SO_ERROR/read consume the retained error. Regression covers both
  graceful close and reset and verifies clearing the error event.
  The corrected Chrome probe no longer logs `EPOLLERR: Success (0)`; Crashpad
  reaches its actual crash-report handling and then reports missing ptrace (101)
  and `/proc/1/task` while the main process aborts at the namespace probe.
- Full native regression passed: `C:\nexustest\native64\events-final.log`.
  Filesystem/two-boot checks: `C:\nexustest\native64\events-files.log`.
  Chrome's real version command passed in 9.6s (`version-events.log` under
  `C:\nexustest\chrome_support`). Native kernel is **135,340 bytes**, SHA-256
  `11676f4697dd864b8826eb59b42e47c035fcdfaac4093bbd6c47d433e2c6dded`.
  Existing i386 images remain unchanged. All tests use private disks.

**Chrome now passes pipe2, epoll_create1, eventfd and the RES-ID getters.** Its
next fatal startup probe is `clone(CLONE_NEWUSER | SIGCHLD)` with flags
**0x10000011** (268435473), which is currently rejected with ENOSYS. The
credentials probe expects a supported namespace or the platform's appropriate
feature/permission error; changing an errno alone would not provide a sandbox.
Evidence: `C:\nexustest\chrome_support\headless-eventfd.log` and the later
`headless-events-final.log`. Implement real isolation; do not use no-sandbox
flags or fake successful namespace operations. There is still no rendered DOM.

Ninth milestone:

- `credentials.c` stores real kernel UID/GIDs separately from their namespace
  presentation. Each thread owns its credential state; user namespaces and
  supplementary-group arrays have explicit lifetimes/refcounts. Actual
  CLONE_NEWUSER and unshare(CLONE_NEWUSER) create child namespaces with scoped
  capabilities, unmapped IDs, inherited setgroups restrictions and a 32-level
  nesting limit. They do not grant capabilities in ancestor namespaces.
- Native procfs supplies UID/GID maps and setgroups policy, proc/self and
  thread-self aliases, task/fd enumeration, executable/fd/namespace links and
  actual thread-dependent task-directory link counts. Map writes validate
  ranges, overlaps, parent mappings, caller/opener authority, one-write-only
  rules, unprivileged single-ID mappings and the parent-root SETFCAP restriction.
  Denying setgroups is irreversible and propagates into child namespaces.
- capget/capset, capability bounding read/drop, no_new_privs, dumpability and
  getgroups/setgroups operate on real state. Dropped permitted capabilities
  cannot be raised again. Exec recalculates credentials/capabilities and honors
  no_new_privs; auxv UID/GIDs reflect the executing namespace. Dumpability
  belongs to the address space and is shared by CLONE_VM threads.
- File permissions use kernel IDs; stat, socket peer/SCM credentials, signal
  siginfo and waitid translate IDs into the observer's namespace. Namespace-root
  credentials cannot write system paths or inspect the parent's fd table.
  Signal permissions now check actual identities/capabilities. Saved namespace
  descriptors and map readers keep the namespace/opener view captured at open.
- getdents64 handles FAT, procfs and device directories with aligned records,
  types, offsets, dot entries and I/O errors. Directory-relative openat/fstatat/
  mkdirat/readlinkat resolve against their directory handle. Proc directory
  aliases are bound to concrete task paths at open so fork does not retarget an
  inherited proc/self directory. O_NOFOLLOW and exe/fd stat-follow behavior
  have explicit checks. General filesystem symlinks are still not implemented.
- `fat32_read_dir_entry` was added without changing the legacy bool API; it
  distinguishes EOF from I/O failure. The original i386 build remains clean
  and passed the three fresh boot checks noted at the top of this handoff.
- Real Chrome then trapped opening `/dev/urandom`: a fault-only executable file
  offset/backtrace diagnostic located it at Chrome offset `0x486cac3`. The
  existing NASM ndisasm identified the failed open (debug helper artifact:
  `C:\nexustest\chrome_support\inspect_fault.py`). `device.c` now supplies
  /dev/null, /dev/zero and read-only /dev/random/urandom using the existing
  checked RDRAND source, with metadata, directory entries and readiness.
  Entropy injection is not implemented; no weak fallback was added. The Chrome
  probe now includes `--enable-logging=stderr` for diagnostics, without bypasses.
- `namespaces_user.c` verifies real namespace clone/unshare and procfs, scoped
  root IDs, parent isolation, cross-namespace socket/signal/wait credentials,
  one-shot map policy, capability-drop enforcement even through an old open fd,
  namespace-handle identity, no_new_privs across exec and 32-level reclamation.
  It also tests FAT/proc/device directory records and actual device reads.
- Full native CPU/floppy/ISO campaign passed:
  `C:\nexustest\native64\userns-final.log`. Native kernel **160,076 bytes**, SHA-256
  `f98f59f1388f6550b3c627cd7a444eda470a3b310f42b42f609d976b559070b2`.
  Two-boot file persistence and protected-region checks passed in
  `C:\nexustest\native64\userns-files.log`; ELF image-layout tests passed too.
  The original Chrome version command passed in 16.7s:
  `C:\nexustest\chrome_support\version-userns.log`.

**Actual Chrome now launches its zygote executables.** The first main-process
fatal is `process_singleton_posix.cc`: socket() syscall **41**, which is still
unsupported beyond socketpair. Evidence:
`C:\nexustest\chrome_support\headless-userns-devices.log`. Child zygote logs also
show missing ppoll (271), seccomp (317/PR_SET_SECCOMP), sysinfo (99), CPU affinity
(204), CPU/procfs information and inotify configuration. The parent has already
aborted by those later child failures. There is still no rendered DOM or GUI.

Tenth milestone:

- AF_UNIX stream/SEQPACKET socket, bind, listen, connect, accept/accept4 and
  getsockname/getpeername now have real endpoint state and ownership. Abstract
  names preserve embedded NULs and exact lengths; autobind, pathname addresses,
  truncation, nonblocking accept/connect and backlog waits are implemented.
  A blocked connect retains the listener object without keeping its user fd
  logically open, so listener close wakes it with ECONNREFUSED. Accepted sockets
  survive listener close, and old connections survive pathname unlink/rebind.
- Peer credentials are captured at listen/connect; message credentials retain
  the real sender IDs. Data/SCM_RIGHTS sent before accept survive source close.
  Listener accept queues participate in descriptor graph marking/collection;
  tests reclaim cycles where an unaccepted message contains the listener fd.
- Pathname sockets and symlinks have durable Nexus inode type metadata. This is
  an explicit **Nexus FAT extension**: NT-reserved directory-entry byte 12 is
  `0xA1` for sockets and `0xA2` for symlinks (ordinary files remain zero/ordinary
  FAT attributes). External FAT tools do not implement these inode semantics.
  Existing ordinary files are not migrated. Symlink targets are stored as bytes
  and published using the existing staged-file commit; failed creation cleans
  up its temporary file and retains disk-full/I/O errors.
- Path resolution now walks components and follows symlinks before processing
  later `..` components and checking target permissions. It bounds expansion,
  rejects loops, honors final O_NOFOLLOW/stat/readlink/unlink distinctions, and
  follows links for executable/interpreter loading. symlink/symlinkat and
  unlink/unlinkat are implemented. Read-only system targets stay protected when
  reached through a link in a writable directory. Socket names reserve real
  filesystem entries until unlink, even after close/reboot; slot reuse detaches
  old routing entries so new names cannot reconnect to the old listener.
- Temporary limitation: deleting open regular files/mappings or directories
  returns EBUSY until orphan inode storage can preserve their data/cursors.
  General rename and full open-file-unlink lifetime semantics remain pending.
- ppoll now uses nanosecond deadlines and the same readiness/pinning engine as
  poll. Raw timeout writeback and libc's const-timeout wrapper are tested;
  temporary signal masks are restored through interrupted waits/handlers.
- `unix_user.c` tests real traffic, queued rights, credentials, names, backlog
  pressure, blocked-connect close, cycle cleanup, symlink permission boundaries,
  live connection identity after unlink/rebind and stale pathname behavior.
  The private filesystem campaign checks socket types and symlink targets across
  two boots, exact allocation counts and the unchanged protected disk region.
- Full native CPU/floppy/ISO tests: `C:\nexustest\native64\ata-matrix.log`.
  Filesystem/reboot tests: `C:\nexustest\native64\ata-files-after.log`.
  Native kernel **176,460 bytes**, SHA-256
  `6ba8d040391289b4b53042f16c17f374fe444007d68a897a0e2f867ae6fc38d9`.
  Chrome's real version command passed in 6.4s (`ata-version.log` under
  `C:\nexustest\chrome_support`). i386 rebuilt clean and passed 3/3 fresh boots
  (`ata-i386-build.log`, `ata-i386-boots.log` under `C:\nexustest\native64`).
- The earlier `unix-files-final.log` campaign FAILED: `page_file_read` captured
  a libc backing-page I/O error causing SIGBUS. The first Chrome probe's loader
  SIGBUS (`headless-unix.log`) predates this diagnostic. A real QEMU disk limited
  to 10 read IOPS reproduced ATA's false timeout deterministically: BSY status
  0xD0, error register zero, failure after 100,000 CPU polls. `ata-latency-before.log`
  records that failure; `ata-latency-after.log` now passes the same healthy disk.
- ATA now uses calibrated time deadlines (3s I/O, 30s cache flush), retaining
  ERR/DF errors and waiting for DRQ/BSY completion after transfers. Native x64
  supplies its calibrated TSC; i386 measures TSC against PIT channel 2 without
  changing IRQ0's 18.2Hz rate. Calibration failure rejects disk initialization.
  Root-directory sector/chain errors now discard the partial cache and reject
  FAT mount. `tests/test_ata_wait.py` checks delayed readiness, real timeouts,
  errors, transfer completion, flush deadlines, ranges and failed FAT mounts
  against the production C implementations with mocked device I/O.
- The post-fix Chrome startup probe (`ata-headless.log`) again reaches the PID
  namespace failure below, without ATA/PAGE_IO or kernel faults. This does not
  prove Chrome browsing or physical hardware support. Keep failure-only page
  and ATA diagnostics for subsequent campaigns.

Eleventh milestone:

- Native PID namespaces now allocate real identities in each ancestor namespace.
  CLONE_NEWPID and unshare(CLONE_NEWPID), including combined NEWUSER creation,
  enforce capability and thread constraints. unshare changes future children,
  preserving the caller's own PID. Nesting is bounded at 32; a dead namespace
  rejects new children. IDs currently increase monotonically without reuse.
- Tasks, process groups/sessions, socket peers and queued SCM credentials retain
  identity objects. Reaped senders remain translatable until their last message
  or socket reference is released. getpid/gettid/getppid, clone TID writes, waits,
  kill/tkill/tgkill, capget/capset and process-group/session interfaces use the
  caller's visible IDs; scheduler and procfs bookkeeping keep internal IDs.
- Orphans are adopted by a live namespace reaper. Namespace init exit marks its
  namespace dead and terminates descendants, including threads blocked in kernel
  waits. Default signal protection for namespace init preserves caught signals
  and ancestor SIGKILL; general stop/continue remains explicitly unsupported.
- `/proc/self/ns/pid` and `pid_for_children` expose actual namespace identities;
  open namespace handles retain them after task exit. The existing procfs is
  mounted in the initial PID namespace: `/proc/self` and numeric entries keep
  that mount's IDs, even when viewed by a namespaced process. New procfs mounts
  and mount namespaces remain pending. The intended ABI follows
  [PID namespaces](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html).
- `pid.c` exercises sibling/ancestor mapping, retained ownership and allocation
  rollback in kernel checks. `pid_user.c` runs real glibc clone/fork/pthreads,
  namespace init signals/death, orphan adoption, waitid/waitpid, queued credentials
  after sender reaping, namespace fd lifetime, future-child unshare, dead namespace
  rejection, 32-level nesting and exec. All process resources are reclaimed.
- Native kernel **184,652 bytes**, SHA-256
  `72801935fe0916601983342bcefc23a07d7156a8614f286f8dc7a6df1550404b`.
  CPU/floppy/ISO matrix: `C:\nexustest\native64\pid-matrix.log`.
  Filesystem/reboot and expanded PID checks: `pid-final-files.log` in that directory.
  Actual Chrome `--version` passed again in 6.4s:
  `C:\nexustest\chrome_support\pid-version.log`. This is not the browsing gate.

**Chrome has passed the earlier PID-namespace trap at 0x4b1e708.** The real
post-change probe (`C:\nexustest\chrome_support\pid-headless.log`) reaches
filesystem isolation, then fails at Chrome file offset **0x4b1ea25**. This is the
failed-child-status branch in `Credentials::ChrootToSafeEmptyDir`, confirmed by
disassembly and Chromium's
[credentials implementation](https://raw.githubusercontent.com/chromium/chromium/main/sandbox/linux/services/credentials.cc).
The helper clones with flags `0x84311` (VM/VFORK/FS/SETTLS/SIGCHLD), calls chroot
on `/proc/self/fdinfo/`, then chdir on `/`. Syscall 161 (chroot) is still ENOSYS.
Its failure logging encounters libc at file offset `0x8f34d`, address `0x308`,
with the helper's deliberately minimal TLS; the parent rejects the failed child.
No environment variable, fake PID or sandbox-disable flag was injected.

Twelfth milestone:

- Filesystem contexts now own retained root/cwd directory descriptions. Fork
  copies their state; CLONE_FS shares changes; unshare(CLONE_FS) separates them.
  NEWUSER unshare also separates filesystem state, and creation of a user
  namespace while chrooted is rejected. Exec unshares the context while preserving
  root/cwd, and scheduler switches activate the correct per-task context.
- chroot, chdir, fchdir and getcwd implement actual directory state. chroot checks
  CAP_SYS_CHROOT in the caller's user namespace and leaves cwd/open fds unchanged.
  A cwd outside the new root produces the raw `(unreachable)` prefix; a retained
  deleted proc directory produces ENOENT from getcwd. See the intended
  [chroot semantics](https://man7.org/linux/man-pages/man2/chroot.2.html).
- Path walking starts at the process root, cwd or supplied directory fd; `..`
  stops at the process root. Ordinary absolute symlinks restart from that root.
  Proc magic links follow their actual bound object, so existing directory fds
  retain access outside a later chroot. FAT case/short-name aliases canonicalize
  to the stored name before boundary comparisons. The same resolution applies
  to executable and ELF interpreter loading. AT_EXECFN keeps the caller's exec
  filename instead of exposing the internal path prefixed by its chroot.
- Procfs adds fdinfo directories/files with actual descriptor position, octal
  flags including CLOEXEC, mount identity and inode; root/cwd magic links expose
  the task's filesystem context. Retained proc directories survive task exit as
  unlinked directory objects; they are not replaced by a fake empty directory.
  FAT directory deletion remains EBUSY while held as an open fd, root or cwd.
- `fs_user.c` tests fork/thread/CLONE_FS sharing, unshare isolation, failed path
  operations, fdinfo contents, retained external dirfds, magic links, chrooted
  exec with its own unmodified loader/libc, AT_EXECFN and the exact Chrome helper
  clone flags (VM/VFORK/FS/SETTLS/SIGCHLD) with a minimal TLS buffer. Its write
  mode checks absolute/relative symlinks, case aliases, root traversal and cleanup.
  Private two-boot tests still verify exact FAT allocation and protected bytes.
- Native kernel **188,756 bytes**, SHA-256
  `1627b88031fd97800d9c5f20eb2993b8fce01e6484cb94f4589ec4f80ef72638`.
  Latest build/matrix/filesystem logs: `C:\nexustest\native64\fs-execfn-build.log`,
  `fs-execfn-matrix.log`, `fs-execfn-files.log`. Actual Chrome version passed in
  10.0s (`C:\nexustest\chrome_support\fs-version.log`). No i386 driver change in
  this milestone; its preceding ATA build and 3/3 boot results remain applicable.

**Chrome passes the filesystem helper now.** The subsequent real startup probe
reaches more browser threads, then fails creating shared memory because syscall
319 (memfd_create) is absent and the fallback `/dev/shm` is not implemented.
Artifacts: `C:\nexustest\chrome_support\fs-execfn-headless.log` and the current
guest `C:\nexustest\chrome64-headless\serial.log`. No DOM/GUI rendered.

The next milestone followed Chromium's
[shared-memory implementation](https://raw.githubusercontent.com/chromium/chromium/main/base/memory/platform_shared_memory_region_posix.cc)
which uses memfd, seals and an independent read-only reopen before its `/dev/shm`
fallback. No persistent FAT alias or sandbox-disable flag was added.

Thirteenth milestone:

- `shmem.c` supplies sparse RAM-backed file objects, held independently by open
  descriptions and VM areas. memfd_create supports CLOEXEC, ALLOW_SEALING,
  NOEXEC_SEAL and EXEC flags; huge-page flags remain unsupported. Read/write,
  positional/vectored I/O, seek, ftruncate, stat, fsync and `/proc/self/fd/N`
  reopen operate on the same object. Reopening creates independent offsets and
  access modes. Closing all descriptors does not destroy live mappings.
- Shared memfd and shared-anonymous mappings use the same retained physical
  backing pages across processes, including pages first faulted after fork.
  Private maps use COW. Split/protect/unmap/fork/exec cleanup retains/releases
  backing ownership correctly. Shared futex keys use stable object IDs.
  Shrink invalidates affected mappings in every address space, zeros the tail
  and releases pages; later access beyond EOF delivers SIGBUS/BUS_ADRERR.
- Seals are inode-wide and monotonic: grow/shrink/write/future-write/execute/seal
  restrictions are enforced. WRITE rejects potential shared writers; FUTURE_WRITE
  preserves old writable mappings while rejecting new ones and write syscalls.
  Read-only/sealed mappings cannot gain write access via mprotect. NOEXEC_SEAL
  fixes the inode execute bits (it is not a blanket mmap PROT_EXEC ban). See
  [kernel memfd semantics](https://www.kernel.org/doc/html/latest/userspace-api/mfd_noexec.html)
  and [file seals](https://man7.org/linux/man-pages/man2/F_ADD_SEALS.2const.html).
- Chrome exposed a syscall ABI bug: its mmap fd register contained the valid fd
  in the low 32 bits and another value above it (e.g. `0x0000001b0000001a`).
  Descriptor lookup/dup and AT_FDCWD handling now use their declared 32-bit
  argument width. `shmem_user.c` reproduces the packed-register case explicitly.
- nanosleep/clock_nanosleep now block on real deadlines, support relative and
  absolute realtime/monotonic/boottime waits, and report EINTR/remaining time
  without automatic SA_RESTART. Process/thread CPU clocks charge scheduled
  execution intervals and exclude blocked/idle time. RLIMIT_DATA queries report
  the existing absence of a policy quota; limit setting is still unsupported.
  creat is routed through the existing validated create/truncate path.
- Guest checks cover sparse 1 TiB file size, coherent pages, COW, descriptor
  transfer after source close, sealed/reopened permissions, forked futex waits,
  remote truncate invalidation, fault signal metadata and exact cleanup. Timer
  checks cover interruption, absolute deadlines and thread/process CPU accounting.
  Native campaigns now expect 12 deliberate user faults on the disk run.
- Native kernel **196,948 bytes**, SHA-256
  `53da41dcb03920ec31da02d466b203b982000330fedb042e51568e3e8f49547c`.
  Build/matrix/two-boot filesystem logs are `C:\nexustest\native64\shmem-clocks-build.log`,
  `shmem-clocks-matrix.log`, `shmem-clocks-files.log`; all passed. Actual Chrome
  version passed in 6.7s (`C:\nexustest\chrome_support\shmem-version.log`).
- Deliberate gaps: named `/dev/shm` tmpfs files, memfd ELF execution, set-ID inode
  execution, huge pages, full timestamp/writeback semantics and writable FAT
  shared mappings are not implemented. Existing FAT maps still copy backing
  pages per fault and reject shared writes. Do not claim universal mmap or tmpfs.

**Chrome passes the initial shared-memory/descriptor failures now.** After the
CPU-clock/data-query fixes, the browser remains alive and repeatedly launches
renderers, but they fail with `sandbox_linux.cc:455`, ENOSYS. The real headless
probe times out after 300s with no DOM. Artifacts under `C:\nexustest\chrome_support`:
`shmem-clocks-headless.log` and saved full guest log `shmem-clocks-serial.log`.
No unexpected kernel/ATA/page-I/O fault was observed. The earlier getter traps
at Chrome file offsets `0x42b14a2` (getrlimit64 resource 2) and `0xbad4f47`
(clock_gettime clock 3) no longer occur. No GUI has been demonstrated.

The next milestone implemented resource-limit enforcement required by Chromium's
[sandbox initialization](https://raw.githubusercontent.com/chromium/chromium/155.0.8059.39/sandbox/policy/linux/sandbox_linux.cc)
(`LimitAddressSpace`/`ResourceLimits::Lower(RLIMIT_DATA, ...)`).

Fourteenth milestone:

- Each process now owns soft/hard RLIMIT_DATA, RLIMIT_AS and RLIMIT_NOFILE state.
  Fork copies it, threads share it, and exec preserves it. getrlimit, setrlimit
  and prlimit validate pointers/ranges, return the prior state, handle aliased
  input/output, check target credentials and reject unauthorized hard-limit
  increases. Root in a child user namespace does not gain initial-namespace
  CAP_SYS_RESOURCE. STACK queries retain the existing 8 MiB stack value;
  STACK updates and other resource classes are still explicitly unsupported.
- VM quotas count mapped virtual bytes and private writable data (including
  file-backed areas), excluding the main stack from DATA. mmap, brk, mprotect,
  fixed replacements and executable loading enforce the current limits.
  Failed quota checks preserve the old mappings and contents; failed exec
  releases the tentative address space and leaves the caller running. Limits
  may be lowered below current usage, but subsequent growth is refused. DATA
  zero is enforced strictly; no Linux Valgrind-specific zero-limit exemption.
- NOFILE defaults to 1024 soft / 1,048,576 hard; its implementation ceiling is
  2^31 slots. Open, dup, pipes, sockets, event descriptors and SCM_RIGHTS receive
  allocation enforce it. Existing high descriptors remain usable after lowering.
  Kernel root/cwd references use internal allocation independent of user slots.
  EMFILE is checked before creating or truncating a file through open.
- Queued SCM_RIGHTS are charged to the sender's real kernel UID across processes.
  Following the Linux Unix-socket rule, a new batch is refused with ETOOMANYREFS
  once the already-outstanding count exceeds the sender's current NOFILE limit.
  Receive/truncation/close/cycle collection releases the charges, including
  rejected or partially constructed messages. Initial-namespace privileged
  capability exceptions remain scoped; namespace root cannot bypass the quota.
- `limits_user.c` verifies memory/protection/fixed-map denial, private file-map
  charges, brk, AS, existing data preservation, descriptor exhaustion without
  create/truncate side effects, internal directory references, in-flight rights,
  cross-process changes, thread sharing, fork/exec inheritance, hard-limit
  restrictions and failed-exec recovery. A successful exec under a 4 MiB DATA
  limit proves that the separately mapped 8 MiB main stack is excluded.
  Credential checks also reject mismatched saved UIDs/effective GIDs.
- Native kernel **201,044 bytes**, SHA-256
  `2fb7c4745abd7a362400d81db0dd084af12dcf7b7209b90b45c0d011279fa46f`.
  Build/matrix/two-boot filesystem logs: `C:\nexustest\native64\limits-final-build.log`,
  `limits-final-matrix.log`, `limits-final-files.log`; all passed. Chrome version
  passed in 6.5s (`C:\nexustest\chrome_support\limits-version.log`).

**The renderer resource-limit fatal is gone.** The latest real Chrome probe still
times out after 300s without DOM. Saved artifacts under `C:\nexustest\chrome_support`:
`limits-final-headless.log` and full `limits-final-serial.log`.
Failure-only mmap diagnostics now identify the underlying descriptor. They prove
the next mapping failures are **ordinary FAT files opened read/write**, rejected
with EACCES on PROT_READ|PROT_WRITE / MAP_SHARED:

- `/home/nexus/chrome-profile/Default/GPUCache/index`
- `/home/nexus/chrome-profile/Default/DawnWebGPUCache/index`
- `/home/nexus/chrome-profile/Default/DawnGraphiteCache/index`
- `/home/nexus/chrome-profile/BrowserMetrics-spare.pma.tmp`

Some browser/GPU work traps at Chrome file offset `0xbae7048`; a later on-device
model utility also fails CPU-info/Dawn initialization at `0x52dbcbc`. No unexpected
kernel/ATA/page-I/O fault was observed. Rename (82), statfs (137), and database
locks continue failing, with many retries; no browsing or graphical Chrome gate
has passed. The test harness stopped its own timed-out VM.

Fifteenth milestone:

- FAT descriptors and mappings now retain a common inode cache built on the
  sparse page machinery. Ordinary reads/writes, shared mappings and private COW
  see coherent backing data. Shared writes are tracked by a protected-write fault;
  successful flushes revoke writable PTEs so subsequent stores become dirty again.
  Clean pages without mappings are evicted after writeback to bound streaming I/O.
- msync, fsync/fdatasync, close, munmap and MADV_DONTNEED perform real writeback.
  MS_ASYNC currently writes back eagerly. Private COW data is never written back.
  File growth, shrink/re-grow and partial EOF pages retain their actual semantics;
  mapped bytes past the final EOF are not persisted. New executable images reject
  writable descriptors and potentially writable shared mappings with ETXTBSY.
- Failed writeback retains dirty buffers and metadata for retry. A failed munmap
  leaves its mapping intact and reports the I/O error. Process exit can leave an
  explicitly owned dirty inode cache, which is distinct from a process allocation
  leak. There is no background writeback daemon or journal/recovery filesystem.
- Shared FAT truncate now refreshes metadata and publishes the smaller directory
  entry before releasing tail clusters. A failed directory write cannot leave a
  live entry pointing into already-reallocated clusters. Tail cleanup failure is
  reported and may leave allocated clusters for later filesystem repair.
- `filemap_user.c` checks shared byte visibility, ordinary-I/O coherence, private
  COW, repeated dirtying after flush, mapping lifetime after close, shrink/re-grow,
  EOF padding and executable-file protection. The two-boot campaign verifies the
  exact persisted `mapped.bin`, allocation counts and unchanged protected region.
- `tests/test_cache_writeback.py` uses the production C cache with deterministic
  partial-data/metadata failures, then verifies retry and complete cleanup.
  `tests/test_x64_writeback_errors.py` injects real EIO responses through QEMU's
  blkdebug IDE backing node (IDE primary master, werror=report). The guest checks
  fsync/msync/munmap/close failure, visible dirty bytes and retained ownership after
  exit; the host verifies the entire private disk hash is unchanged. The earlier
  read-only IDE attempt was rejected by QEMU and was replaced by error injection.
- i386 rebuilt warning-free at **1,059,756 bytes** and passed 3/3 login-stage boots:
  `C:\nexustest\native64\filecache-i386-build.log`, `filecache-i386-boots.log`.

Sixteenth milestone:

- Failure diagnostics showed V8 requests `mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)`
  on a 512 MiB code range. The former unconditional user RWX rejection returned
  EACCES, causing the CodeRange OOM trap. User mmap/mprotect now honor explicit
  execution/write permissions, as required by unmodified V8's
  [code-range initialization](https://raw.githubusercontent.com/v8/v8/main/src/heap/code-range.cc).
  This is a deliberate user-memory policy change: requested RWX is supported.
  Default data remains NX; kernel W^X, supervisor mappings, fd access permissions
  and private COW isolation remain enforced. ELF images with mixed W/X load
  segments are still rejected by the loader.
- `jit_user.c` executes generated machine code, exercises V8-sized reserve/
  permission/discard operations, checks COW across fork, and deliberately faults
  on default NX and read-only pages. Kernel-address changes and shared RWX on a
  read-only fd are rejected. Native disk campaigns now expect 14 deliberate faults.
- The next renderer trap at Chrome file offset `0x5db004a` was its
  [protected-memory check](https://raw.githubusercontent.com/chromium/chromium/main/base/memory/protected_memory.h):
  getrlimit64(RLIMIT_MEMLOCK, read-only-buffer) must fail with EFAULT. Queries now
  validate output writeability before reporting an unsupported resource class.
  The test verifies read-only buffers give EFAULT and writable buffers still give
  ENOSYS; MEMLOCK values/locking were not fabricated or advertised as supported.
- Native kernel **205,140 bytes**, SHA-256
  `801ab0431ca96f94373cc83b2d6fb97ec393e5d0717080e5763a2590f7d9a95c`.
  Latest build/matrix/filesystem logs under `C:\nexustest\native64`:
  `protected-output-build.log`, `protected-output-matrix.log`,
  `protected-output-files.log`; all passed. Real-device failure coverage passed
  in `filecache-final-device-errors.log`. Chrome version passed in 9.6s at
  `C:\nexustest\chrome_support\protected-output-version.log`.

**Actual Chrome now emits the about:blank DOM on the custom kernel:**
`<html><head></head><body></body></html>` is present in saved
`C:\nexustest\chrome_support\protected-output-serial.log`. This is a real renderer
advance, not a complete Chrome gate: it did not shut down within 300s, so the
headless harness still FAILED. No graphical session or downloads were verified.

The stronger `headless-js` probe runs a data URL with arithmetic and DOM/title
updates; it timed out without its expected output (`native-js.log`,
`native-js-serial.log` under `chrome_support`). The same document, tested as
`file:///home/nexus/js-proof.html` with `headless-file-js`, also timed out without
the expected DOM (`native-file-js.log`, `native-file-js-serial.log`). Do not claim
JavaScript-page verification. Tests retain the normal clean-exit requirement and
reject writeback/cache-read failures. All owned VMs were stopped by their harnesses.

Seventeenth milestone:

- Real fcntl process/OFD range locks and flock now enforce contention, shared/
  exclusive access, range splitting/merging, blocking wakeups, process deadlock
  detection, EINTR/SA_RESTART and close/exit/exec ownership. Locks work on FAT
  files and memfd objects. Tests cover independent opens, dup, fork, pthreads,
  last-description-close, closing a blocked request's fd, and CLOEXEC release.
  Traditional lock ownership currently follows the process; unusual cross-process
  CLONE_FILES ownership and /proc/locks reporting still need work.
- statfs/fstatfs now report actual FAT cluster geometry and free-space counts,
  plus proc, device, namespace, pipe/socket/anonymous and internal shmem types.
  Internal quota-free shmem reports zero capacity counters, as Linux does; named
  /dev/shm is not provided by this change. FAT free-space scanning is lazy and
  cached; allocation/free updates it, failed writes invalidate the count.
- New statfs fixture verifies growth/truncation/free counts and writes a disk
  record which the host compares with actual FAT allocation across two boots.
  Host fault tests verify scan caching and read/write-error recovery. Native
  matrix, two-boot persistence, and real QEMU write-failure recovery all PASS:
  `C:\nexustest\native64\locks-statfs-final-{build,matrix,files,errors}.log`.
  i386 remains 1,059,756 bytes, warning-free, with 3/3 fresh login-stage boots:
  `statfs-i386-build.log`, `statfs-i386-boots.log` in the same directory.
- Native kernel: **209,236 bytes**, SHA-256
  `2de1bb1f7cc2d0343c2dcf94137740c6599c71c5ff4dfc8993d13c53e346adf9`.
- Real Chrome's file-JavaScript probe still times out at 300 seconds without
  the required DOM/title. Database-lock and statfs errors are gone; LevelDB
  rename (82) errors remain. Evidence is preserved under `chrome_support` as
  `locks-statfs-file-js.log` and `locks-statfs-file-js-serial.log`.
  No interactive browsing, native downloads, or in-OS install gate has passed.

Eighteenth milestone, storage foundation:

- `fat32_rename_path` now implements actual directory-entry relocation/replacement,
  no-replace checks, UTF-8/long names, directory `..` updates, cycle/type/nonempty
  rejection and socket/symlink metadata preservation. Destination names are
  prepared in sector buffers; file contents are not copied or deleted to fake a
  rename. `fat32_detach_path` removes a name while retaining its chain, and
  `fat32_dispose_file` frees that chain only when the caller has no live users.
  A replaced/detached snapshot can still read, write and grow independently of
  the new destination. These APIs are serialized like the existing FAT driver.
- Metadata sectors have before-images for I/O rollback. The standard FAT clean
  bit is cleared in both FATs before namespace publication and restored after
  success/rollback. Failed rollback blocks further writes; a dirty FAT mirror
  mounts read-only on reboot. Native statfs reflects this and data/namespace
  mutations return EROFS. This is not a journal or automatic filesystem repair:
  interrupted changes may need repair, and detached chains can leak on power loss.
  A failed final clean-marker write leaves the completed rename successful but
  blocks further writes; callers must still adopt its returned moved/replaced
  objects. Allocation/free errors retain the existing non-journaled FAT limits.
- `tests/fat_rename_test.c` exercises production code with two FAT copies:
  replacement lifetime, detached growth/reclamation, 255-byte/UTF-8 names,
  remount, directory ancestry/type errors, special inode types, every preflight
  read/write fault, writes that fail after changing media, rollback failure and
  every power-cut boundary. Absent-destination publication is fault-tested too.
  Run with `python -I tests/test_ata_wait.py`; evidence:
  `C:\nexustest\native64\rename-storage-host.log`.
- `tests/test_x64_readonly.py` boots an image with only its secondary FAT marked
  dirty. Guest read/statfs succeeds, create/truncate/unlink/mkdir/symlink fails
  with EROFS, memfd still works, and host confirms the entire image is unchanged.
  Native build/matrix/files/readonly/image logs use `rename-storage-final-*` in
  `C:\nexustest\native64`. Real writeback-error recovery also passed in
  `rename-storage-errors.log`. Actual Chrome `--version` passed again in 7.1s:
  `C:\nexustest\chrome_support\rename-storage-version.log`.
- Current native kernel: **217,428 bytes**, SHA-256
  `d3a3ac2be7a07da56e3d7daa411a95065c896ef0dd01e5438292aefefb892eab`.
  Current i386 kernel: **1,063,852 bytes**, warning-free; build/boot artifacts:
  `rename-storage-final-i386-build.log`, `rename-storage-final-i386-boots.log`.

Nineteenth milestone:

- Native rename/renameat and renameat2(NOREPLACE) are now connected to the FAT
  implementation. `inode.c` maintains stable inode identities and generations for
  reused directory slots, with metadata reserved before disk mutation. Snapshot
  identities are in-memory only. Rename propagates to descriptors, cache objects,
  VM/image segments, locks, Unix pathname bindings, directory/cwd/root handles and
  process executable paths. Namespace generation pages have explicit ownership
  in the kernel's allocation checks; they persist for the mounted volume.
- Unlink/rmdir/replacement now retain live objects instead of returning EBUSY for
  every open file. Detached files report nlink=0, /proc/fd links show `(deleted)`,
  and reopening those magic links binds the original inode even after its old
  pathname is reused. Dirty cache state remains separate from a replacement.
  Detached chains are collected after their final descriptor/map/image reference;
  write/free failures keep an owned orphan record. Source/target caches sync
  before namespace mutation, propagating real I/O failures.
- `rename_user.c` tests actual guest rename/renameat/NOREPLACE, live source and
  target descriptors, flock conflicts, two writable mappings, deleted-file reopen,
  slot reuse, moving a directory with live dirfd plus another process's chroot/cwd,
  symlink moves, Unix listener moves, and lazy executable page reads after that
  executable is renamed/unlinked and its name reused. Both filesystem boots pass;
  host validates no leaked clusters, fixture cleanup and unchanged protected bytes.
  The former Unix EBUSY tests now check correct live unlink/rmdir behavior.
  Literal rmdir `.`/`..` still fail; a new regression prevents accidental removal
  of an empty cwd through those spellings. Checked FAT lookup distinguishes I/O
  errors from an absent rename destination; preflight errors cannot accidentally
  replace an existing target without retaining its live inode.
- `tests/test_cache_writeback.py` also runs production inode/cache code through
  700 moves/deletions across multiple identity pages, verifying distinct reused
  slots and last-reference orphan reclamation. Native matrix, filesystem, real
  writeback failures and dirty-volume read-only checks PASS:
  `C:\nexustest\native64\rename-native-{build,files,matrix,errors,readonly}.log`.
  i386 rebuild remains warning-free at 1,063,852 bytes and boots 3/3:
  `inode-i386-final-build.log`, `inode-i386-final-boots.log`. Host FAT/cache tests
  pass in `inode-host-fat.log`, `inode-host-cache.log`.
- Native kernel at milestone 19 (with Chrome-only exit diagnostics): **225,628 bytes**,
  SHA-256 `90adf579ec7daa1c68b5892b3ad2c7a79a3e4f1968f748dfa7820c8e485f5a90`.
  Final build/two-boot filesystem results: `rename-native-final-build.log` and
  `rename-native-final-files.log`. Before the dot-name/lookup fixes it was
  `dda63b3810877aadc55b23925565684dcf14c8ecf7622057b6e9bc4cd86df262`
  (`ipc-trace-build.log`); before additional IPC error/exec-argument tracing
  it was `20e84a3d39982209e6626477304d95e4071dcd2fddac0eb61bc43573136aaec8`.
  That preceding build's two-boot filesystem check is
  `rename-trace-files.log`. Diagnostics activate only for the Chrome disk probe:
  process wait statuses, nonzero/signal exit locations and unknown syscall PIDs;
  mremap diagnostics also include requested sizes/flags.
- Remaining namespace edges are explicit: RENAME_EXCHANGE/WHITEOUT return
  EOPNOTSUPP; traversal from a detached directory beyond `.` needs retained parent
  dentries (returns ENOENT). An unlinked executable's lazy mappings survive and
  /proc/exe readlink shows deletion, but opening/stat-following that magic link
  still needs an inode-bound target; it fails instead of opening a reused name.
  Multi-sector FAT recovery/journaling limitations from milestone 18 remain.

Chrome's post-rename file-JavaScript run **still timed out at 300s without the
required DOM/title**. Rename/lock/statfs errors disappeared; evidence is preserved
as `chrome_support/rename-native-file-js{,-serial}.log`. The diagnostic rerun is
`rename-trace-file-js.log` with serial saved as `rename-trace-file-js-serial.log`;
it also timed out at 300s. It shows children
exiting with status 0 after their 15-second Mojo/browser connection timeout,
rather than a CPU exception. Do not label all reported service restarts as kernel
faults. Chromium's [connection setup/timeout](https://raw.githubusercontent.com/chromium/chromium/main/content/child/child_thread_impl.cc)
ties this timeout to the browser's ChildProcessHost ping/channel handshake.

Additional diagnostics in `chrome_support/ipc-trace-file-js.log` (300s timeout;
serial preserved as `ipc-trace-file-js-serial.log`) log failed
socket/epoll/fcntl/ioctl calls (excluding ordinary EAGAIN/EINTR/ENOENT) and selected
Chrome child-type arguments. The current run identifies PID 33 as
`--type=utility --utility-sub-type=network.mojom.NetworkService`; it then exits
status 0 after its no-connection timeout. Reported errors so far include missing
AF_UNIX datagrams, AF_NETLINK, and ENOTTY for SQLite's F2FS feature probe
`0x8004F50C` (an expected failure on FAT; do not emulate F2FS). A recvmsg
ECONNRESET is after a child exit. No obvious rejected stream sendmsg/epoll setup
has yet explained the handshake failure. All three file-JavaScript probes in
this milestone ended without the expected DOM/title or clean harness completion.

The data-URL version of that proof also timed out before the storage optimization
below. Its log/serial are preserved under `chrome_support` as
`rename-data-js.log`, `rename-data-js-serial.log`. The 512-message diagnostic
trace `ipc-wire-data-js-serial.log` showed real bidirectional socket delivery and
SCM_RIGHTS transfers. Those verbose send/receive hooks were removed after use;
the compact syscall-error/process-exit diagnostics remain.

Twentieth milestone: **real Chrome executes JavaScript,
renders HTML/CSS/text to PNG, and exits cleanly on the custom kernel.** These
are headless tests; an interactive browser window and guest installation/downloads
are still required for the full goal.

- QMP CPU sampling located the major startup bottleneck: 35 of 40 samples were
  in ATA reads/waits. Stack samples repeatedly led through `fat32_read_file`
  at its FAT-chain seek loop into ELF demand paging. Evidence:
  `C:\nexustest\chrome64-profile\cpu-samples-early.jsonl`, `cpu-samples.jsonl`.
  One-off diagnostic tools are `chrome_support/probe_slow_ipc.py` (120-second
  Chrome IPC timeout plus loopback QMP) and `sample_guest.py`. Their runs still
  failed the strict 300-second harness; they are not completion proofs. Normal
  tests retain Chrome's default IPC timeout and the original clean-exit gate.
- FAT file snapshots now cache a verified contiguous prefix, avoiding repeated
  backward chain walks. A saturating FAT mutation epoch invalidates both prefix
  and sequential cursors when chains change, including independently opened
  handles after shrink/regrow. Fragmented chains still use checked FAT traversal.
  Host test: one cold seek reads 22 sectors; all 100 backward/random warm seeks
  read only 2 (directory metadata + data). Fragmented regrowth, old read/write
  handles, remount and epoch saturation preserve data. This also fixes stale
  sequential-write cursors potentially targeting a freed/reused cluster.
- ATA READ SECTORS now batches up to 256 sectors per command, preserving the
  per-sector DRQ checks, 400 ns delays, calibrated deadlines and final completion
  error check. Writes/flush ordering and PIT channel 0 are unchanged. A production-
  code simulated-port test checks batch boundaries, 28-bit LBA carry, intermediate
  and final errors. Actual QEMU slow-disk, write-error, read-only, filesystem,
  native CPU/ISO matrix and 3/3 i386 boot checks pass (`ata-batch-*.log` under
  `C:\nexustest\native64`). Both builds remain warning-free; i386 is 1,063,852 bytes.
- Main ELF/interpreter image segments now reuse the existing inode page cache.
  Full file pages are shared read-only/COW across processes; partial segment
  pages get private copies with zero-filled tails/BSS. Independent exec tests
  change initialized data/BSS in parent and child and verify later execs still
  start from original file bytes/zero BSS. Namespace/unlink/lazy-exec tests,
  allocation ownership and real writeback-error recovery still pass.
  Logs: `elf-cache-{build,matrix,files,errors,host}.log` in `native64`.
- `headless-js` now passes the unchanged arithmetic/DOM/title assertions AND
  status-0 application exit/VM exit 33: **137.7s** in
  `chrome_support/fat-seek-data-js.log`; full serial is
  `fat-seek-data-js-serial.log`. An earlier interrupted attempt had no terminal
  result and was not counted (`fat-seek-interrupted-serial.log`).
- New `headless-render` probe asks actual Chrome to write
  `/home/nexus/render.png`. Host reads it back from FAT and verifies PNG format,
  1000x700 dimensions, real text ink, background color and the green panel which
  JavaScript only sets after calculating 49995000. The PNG was visually inspected:
  `screenshots/native64-chrome-render.png`, SHA-256
  `4073bd7f9ab87e208ba2e72418e60505ccf58226f86fff5e02220689225be2ae`.
  The test passed with normal Chrome arguments and clean shutdown in **126.5s**;
  after shared executable caching, the same test passed in **58.1s** with an
  identical PNG. Evidence: `chrome_support/chrome-render.log`,
  `chrome-render-serial.log`, `chrome-render-cached.log`, and
  `C:\nexustest\chrome64-render-cached\serial.log`.
  A child connection timeout/retry still appears during startup; no CPU/kernel
  fault was observed. Do not claim all optional Chrome services are supported.
- Current native kernel: **225,628 bytes**, SHA-256
  `faac42baa329d9a0cf98ef4e3d6c1cc99b96444567f0f9bbbe8bc7db72c68d67`.
  The pre-ELF-cache render build was
  `8d873348c2d5d7c6852c528e57296e7008582a5f709f34616de3867fa7c17077`.
  Chrome remains the unchanged pinned 155.0.8059.39 binary, host-staged for these
  development probes; that is not an in-OS download/install result.

Twenty-first milestone: **native userland can draw directly
to the real framebuffer and receive keyboard, mouse and wheel events.** This is
the display/input foundation for graphical Chrome, not a Chrome window yet.

- `/dev/fb0` exposes the x86-64 fbdev geometry/color ioctls, bounded read/write
  and seek, and shared mmap. Firmware RGB masks are preserved by boot2; the
  native kernel accepts only the validated 32-bit XRGB aperture, aligned to
  whole pages, outside usable E820 RAM and below 4 GiB. Kernel/user aliases
  use matching uncached mappings. No arbitrary physical-memory mapping exists.
- Framebuffer PTEs have explicit MMIO ownership: fork shares them without PMM
  refcounts/COW, and unmap/exec/exit never return device addresses to the RAM
  allocator. VMA splits/clones retain display ownership. NX, read-only mapping
  permissions and offset/length bounds remain enforced. Kernel text remains
  serial-only while a display mapping or writable fd is held; panic restores
  emergency screen output. No fake mode-setting, panning or vblank success.
- `input.c` drives the existing PS/2 ports with calibrated 100 ms deadlines,
  ACK/RESEND handling, scan-set translation and negotiated wheel/five-button
  packets. IRQ1/IRQ12/cascade are unmasked only for available devices. PIT
  channel 0 remains unchanged; interrupt work is bounded and allocates nothing.
- `/dev/input/event0` (keyboard) and `event1` (mouse) implement the evdev event
  ABI, capability/name/id/key-state ioctls, per-open realtime/monotonic clocks,
  blocking/nonblocking reads, signal interruption, poll/epoll, and shared
  dup/fork cursors. Separate opens have independent cursors. Queue overflow
  reports SYN_DROPPED + SYN_REPORT with key-state recovery; EFAULT preserves
  unread events, including partial reads. Device access checks the actual
  filesystem UID (desktop UID 1000 or host-root UID 0), not namespace-local UID.
- `tests/test_input.py` compiles the production driver/device code with simulated
  ports: deadlines, retries, malformed input, standard/extended/Pause/Print
  keys, repeat, mouse signs/buttons/wheel, queue overflow, clock/ioctl bounds,
  partial EFAULT, permissions and directory entries all pass.
- `tests/test_x64_display.py` boots a private disk and uses actual QMP input
  injection. It checks real framebuffer colors, key/mouse-driven pixel changes,
  blocking read interruption, edge-triggered epoll, independent readers, 160
  key-pair overflow/recovery, mmap aliases/fork/splits, three deliberate access
  faults, complete process/display resource reclamation and unchanged disk hash.
  Final evidence: `C:\nexustest\native64-display-final\serial.log`, `input.png`,
  and `native64/display-final.log`. Visually inspected screenshot:
  `screenshots/native64-display-input.png` (four test quadrants, white key marker,
  magenta mouse marker; it is not browser UI).
- Native CPU/disk/ISO matrix and two-boot persistence campaign pass; the existing
  i386 build is warning-free and still has the same 1,063,852-byte kernel hash.
  Its shared boot-stage change passed 3/3 fresh boots in
  `C:\nexustest\native64-i386-display`. Logs are `display-*.log` under `native64`.
  The unchanged Chrome headless JavaScript/raster/clean-exit test passed again
  in 59.0 s (`chrome_support/native-input-render.log`), then in 57.5 s on the
  final kernel (`chrome_support/native-display-final.log`), with the same PNG
  hash as milestone 20. No sandbox/GPU disabling flags or shutdown shortcuts added.
- Native kernel: **233,820 bytes**, SHA-256
  `89cf4a87ae4bac39ce792d6b69058e6c78efef0e05f5f4a492294e45cc34b9f4`.
  Boot2 SHA-256: `ce01c4735f5f937d013ffac8dbe27060533ad15b9746bba1986adf85edb3c945`.
  Physical hardware, USB input on the native target, display mode switching and
  seat/session access revocation remain unverified or unimplemented.

Twenty-second milestone (compositor still in development):
**Chrome's real graphical window renders on the custom kernel and closes cleanly
through Alt+F4. The page remains on Loading, so interactive browsing is not passed.**

- Added a native `/sbin/init` launch path, before the development Chrome probe.
  Plain foundation and Chrome images keep their earlier behavior when this file
  is absent. `desktop.c` is our own userland window server, using the already
  staged libwayland-server and xkb helpers, `/dev/fb0` and evdev nodes. No Weston,
  Linux kernel, foreign guest OS or remote browser is involved.
- `desktop-sdk-lock.json` pins official Debian header/protocol/scanner archives
  to hashes from the trixie package index. `desktop.py` reuses the existing
  verified package downloader, extracts only headers/XML/scanner/licenses, and
  runs the **unmodified Linux wayland-scanner application on NexusOS**. Three
  fork/exec runs generate server/client headers and protocol code on private FAT;
  their contents/hashes are read back for compilation. Evidence:
  `C:\nexustest\native64-protocols\serial.log`, `native64/desktop-protocols.log`.
  Generated cache is `out/desktop-sdk`; compile uses the staged glibc 2.41 ABI.
- Initial compositor implements SHM surface copies/releases/frame callbacks,
  xdg toplevel configure/ack, output geometry/membership, input regions, keyboard
  keymap fd/modifiers/focus, pointer events, and focused clipboard transfers over
  actual passed pipe descriptors. It currently uses a maximized-window policy.
  Native test client `desktop_user.c` verifies real 1920x1080 frames, output enter,
  keymap mapping/parsing, key/click changes, clipboard bytes, and normal close.
  `tests/probe_desktop64.py WORK client` now automates its QMP interaction, pixel
  checks, assertions, clean child/server exit, VM exit 33 and resource cleanup.
  Final client evidence: `C:\nexustest\native64-desktop-final\serial.log`,
  `interactive.png`, `native64/desktop-final.log`; screenshot copied to
  `screenshots/native64-wayland-client.png`.
- First normal graphical Chrome probe hit absent native EGL/DRM initialization;
  it was stopped as a failed diagnostic. The software probe uses the genuine
  Chrome `--disable-gpu` software path, **without disabling the sandbox**. Real
  Chrome tabs/address bar and the Loading page are visible in
  `screenshots/native64-chrome-window-loading.png`. These pixels are Chrome's
  SHM buffers, not a headless PNG painted into a fake browser. The initial
  software and trace probes timed out at 300 s with the page still Loading;
  these are NOT successful browsing results.
- A headless Chrome child launched by the new desktop/init did execute JavaScript
  (`NEXUS_CHILD_JS_PASS:56088`) and exit cleanly with all tasks drained. Thus
  launching Chrome as PID 2 is independently verified, but does not prove GUI
  page loading. Evidence: `native64-desktop-child/serial.log`,
  `native64/desktop-child.log` under `C:\nexustest`.
- GTK's Wayland source requires `wl_data_device_manager` before creating a seat.
  The first prototype omitted it and produced GDK_IS_SEAT errors. Added actual
  clipboard handling; the subsequent Chrome trace has both Chrome and GTK
  keyboard bindings and no earlier Gdk seat assertion. Clipboard's native test
  passes (`native64/desktop-clipboard.log`). The Chrome seat probe remained on
  Loading, but **Alt+F4 sent xdg_toplevel.close and Chrome/server/VM exited cleanly**:
  `C:\nexustest\native64-desktop-seat\serial.log`, `native64/desktop-seat.log`.
  Ctrl+L events were emitted with the correct Ctrl modifier; a corresponding
  address-bar selection has NOT been observed. Output enter/leave was then added
  and passed the native client test. The subsequent Chrome output-event probe
  reached the window but stayed on Loading. Clicking the address bar visibly
  focused/selected text; mouse/typing interaction then **crashed Chrome**. This
  latest run is a failure, despite the earlier clean Alt+F4 result.
  Evidence: `C:\nexustest\native64-desktop-output\serial.log`, `typing.png`,
  `native64/desktop-output.log`. Chrome fault: null+0x40 at file offset
  **0x77D2FA4**, caller **0x7A46F4B**. Disassembly is `wl_proxy_get_version(NULL)`
  followed by a marshal using the **wl_subsurface** interface. ELF RELATIVE
  relocation lookup at VA **0x11669640** confirms the interface name. The
  missing **wl_subcompositor** is therefore a concrete next implementation,
  not a speculative GPU/keyboard workaround. No kernel fault occurred.
- Sampling the first software probe found repeated whole-descriptor graph scans
  from `poll_files -> fd_unpin`. `fd_unpin` now skips collection while an open fd
  or another pin still roots the same reachable graph. Last-root removal still
  performs collection/pruning. Full native CPU/disk/ISO matrix, including IPC
  descriptor ownership/cycles and process reclamation, passes on this change:
  `native64/desktop-gc-matrix.log`. Earlier samples:
  `native64-desktop-software/cpu-samples.jsonl` (20 samples).
- Current native kernel: **233,820 bytes**, SHA-256
  `769de192cee55aea464965f7a4c72075701844d9954e216046b8ebced664febf`.
  Desktop ELF: **127,648 bytes**, SHA-256
  `7069ec147fde44b6e4e0c4ae5598afec3d30ca8b0c5b3858f272a288ced9c6ab`.
  The i386 kernel/boot artifacts remain at the preceding milestone's verified build.

Build/test this work from `NexusOS/`:

```powershell
python -I arch/x86_64/build.py build
python -I arch/x86_64/desktop.py
python -u -I tests/probe_desktop64.py C:\nexustest\desktop_check client
python -u -I tests/probe_desktop64.py C:\nexustest\layers_check subsurfaces
python -u -I tests/probe_desktop64.py C:\nexustest\chrome_gui trace
```

The probe modes are default GPU, `software`, `trace` (software + Wayland protocol
trace), `client` (asserted integration test), `subsurfaces` (asserted scene/input
test), or `headless` (child-launch diagnostic). Invalid modes fail before disk creation.
All use new private copies of chrome64.img; work/qmp_port identifies the owned
VM's loopback monitor. `desktop.args` contains one argument per line; server
`--trace` is consumed before launching the configured program. GUI probes do
not automatically exercise/verify Chrome interaction; do not count VM exit alone
as the completion gate. The remaining compositor work after the next milestone
includes popups/positioners, drag-and-drop, client cursor rendering, rotated
buffers/nonzero toplevel attach offsets, window management and robust configure
histories. Clipboard focus/FD handling is tested only within the native test
client; broader isolation cases remain.

Twenty-third milestone: **real subsurfaces and layered input
work; the previous main-Chrome null-pointer crash is fixed. GUI page loading is
still unproven and remains on Loading in the latest probe.**

- Added actual `wl_subcompositor`/`wl_subsurface` resources, permanent role
  checks, cycle/sibling validation and retained parent lifetimes. Destroying
  either a wl_surface or its role first is safe; parent destruction hides
  descendants. Final client destruction checks that every surface, input/output
  binding and clipboard selection is gone before the compositor exits.
- Surface commits now separate pending, cached and displayed state. SHM pixels
  are copied before release, so a client can destroy/reuse its buffer while a
  synchronized commit waits for its parent. Parent commit snapshots include
  child positions and stacking; requests made after that snapshot do not leak
  into an earlier cached commit. Sync is inherited through nested subsurfaces;
  desync flushes when no ancestor forces synchronization. Frame callbacks wait
  until cached state is applied and the mapped scene is painted.
- Iterative scene traversal paints/hit-tests nested stacks, including below the
  parent, negative positions and children outside parent bounds. It does not
  recurse on client-controlled tree depth. Parent unmap/remap propagates to the
  whole tree. Empty/subtracted input regions affect hit-testing, not drawing.
- Pointer events use child-local coordinates. An implicit button grab retains
  the target when dragged outside it; keyboard focus stays on the main window.
  Evdev mouse-overflow recovery now compares current button state and emits
  missing transitions. New pointer bindings receive their own enter event
  without resetting existing bindings; redundant mouse events are no longer sent each paint.
- `subsurface_user.c` verifies actual framebuffer pixels, nested/ordered commits,
  frame timing, later position requests, sibling/parent stacking, inherited sync,
  desync, negative/outside coordinates, parent/role destruction order and remap.
  Five deliberately invalid clients (cycles, duplicate roles and invalid stack
  references) receive protocol errors while the compositor stays alive.
  QMP-driven input checks nested hit targets/local coordinates, drag outside the
  child, keyboard delivery to the main window and click-through input holes.
  Evidence: `C:\nexustest\native64-subsurface-final\serial.log`,
  `native64/subsurface-final.log`. Existing window/keymap/clipboard/input/close
  tests also pass on the final build (`native64/subsurface-client-final.log`).
- Chrome's original mouse/typing reproducer now reaches
  `wl_subcompositor.get_subsurface`, positions the child at (99,25), switches it
  to desync and continues rendering. There is no repeat of the main-browser
  null+0x40 fault at 0x77D2FA4. Alt+F4 then closes Chrome and the compositor with
  status 0, VM exit 33, and no compositor resource leak. Evidence:
  `C:\nexustest\native64-chrome-subsurfaces\serial.log`, `input-ack.png`,
  `native64/chrome-subsurfaces.log`. The data page still says Loading; the
  optional ChromeML process still raises its earlier contained breakpoint fault.
  Do not label this run full browsing or fault-free Chrome support.
- Input testing under Chrome load exposed QEMU PS/2 FIFO coalescing/loss when
  many mouse packets were injected before they were consumed. The successful
  reproducer waits for each server event before sending the next one. The
  layered-input harness likewise checks each movement before advancing. Native
  input responsiveness during long kernel operations still deserves testing;
  don't confuse a queued host event with a received browser event.
- The generated-binding cache now records both XML and scanner binary hashes.
  The official scanner was rerun on NexusOS, and both native client campaigns
  pass after the final warning-free build (`native64/subsurface-final-build.log`).
  Current desktop ELF: **151,736 bytes**, SHA-256
  `c77c84aa1182f16b3837e0b5a2ccf78d7609594b0c8ff7f2c0889796089f797f`.
  Native kernel remains **233,820 bytes**, SHA-256
  `769de192cee55aea464965f7a4c72075701844d9954e216046b8ebced664febf`.
  No kernel/i386 code changed in this milestone. All owned VMs exited at checkpoint.

Twenty-fourth milestone: **actual graphical Chrome loads
the original page, accepts real PS/2 typing/clicks, executes page JavaScript,
changes the visible framebuffer and exits cleanly. Native networking and the
in-OS download/install/launch workflow are still missing.**

- Added `inspect_user.c`: a diagnostic app launches unmodified Chrome with the
  standard DevTools pipe endpoints on fds 3/4. Native Unix socketpairs preserve
  Chrome's shutdown semantics. JSON frames are NUL-delimited, handle partial
  reads/writes, and have checked field/identifier parsing. The parser's startup
  self-check and `tests/test_inspect_json.py` cover malformed/truncated JSON,
  escaped text, nested fields, numeric overflow and nesting bounds. This is
  local process IPC; no remote browser or external OS runtime is involved.
- DevTools proved that the original Loading tab still had an initial empty
  `about:blank` document, while its renderer could execute JavaScript. A later
  stop/retry navigation loaded a real data URL and changed its title to
  `Nexus56088`; a QMP screenshot showed the actual rendered heading. This was
  diagnostic evidence, not the final input check. Artifacts:
  `C:\nexustest\native64-chrome-nav-visual\serial.log`, `navigation.png`.
- Controlled startup comparison isolated the child IPC deadline: with default
  Chrome settings, a child logs termination after 15 s without a connection and
  the original navigation remains pending through a further 90 s of read-only
  inspections. With **`--ipc-connection-timeout=120`**, the same original page
  loads without retry or document injection. Evidence:
  `native64/chrome-startup-default.log`, `chrome-startup-slow.log`, and
  `native64-chrome-startup-slow/startup.png` under `C:\nexustest`. This is an
  explicit software/TCG startup accommodation; the default-deadline failure has
  not been fixed or relabelled as a pass. The outer VM deadline remains 300 s.
- `probe_desktop64.py WORK interactive --ipc-connection-timeout=120` launches
  the original page with `--ozone-platform=wayland --disable-gpu`, without
  disabling the sandbox. The native inspector only reads layout/DOM state in
  this mode. The host sends actual PS/2 mouse/key events through QMP, waits for
  each event's delivery, types **nexus** into the input, then clicks the page's
  button. It asserts focus, the exact input value, page JavaScript's background
  change, actual green framebuffer pixels, Chrome status 0, compositor cleanup,
  all task drainage and VM exit 33. No DevTools input commands, DOM injection or
  navigation retry are used in this successful interactive run.
- Verified result: `C:\nexustest\native64-chrome-interactive\serial.log`,
  `chrome-interactive.png`, `native64/chrome-interactive.log`. Copied and visually
  inspected screenshot: `screenshots/native64-chrome-interactive.png`.
  This does not prove kernel seccomp enforcement, internet browsing, GPU/audio,
  Chrome installation from inside the OS, or launching a downloaded application.
  Chrome and its libraries are still host-staged on the private development image.
- The native kernel and compositor remain at milestone 23's hashes/sizes. Build
  the inspector with `python -I arch/x86_64/desktop.py`. Added probe modes are
  `inspect` (read state and diagnostic stop/retry), `startup` (only inspect the
  original navigation), and `interactive` (real input assertions). Additional
  individual Chrome switches may follow the mode; newline injection is rejected.
  All owned VMs have exited at this checkpoint; no commit/staging performed.

Twenty-fifth milestone: **native NIC and TCP transport
receive an exact 2 MiB stream through QEMU's real network device. Application
AF_INET socket syscalls are not wired yet; Chrome internet/download/install
support remains incomplete.**

- `arch/x86_64/rtl8139.c` reuses the house register definitions with native
  contiguous DMA allocation below 4 GiB, a 32 KiB receive ring plus overrun
  padding, and four TX descriptors. Reset has a calibrated 100 ms deadline;
  failed initialization disables DMA and releases allocations. Busy TX slots
  return EAGAIN without overwriting in-flight bytes. Receive work is bounded,
  validates packet headers, handles unfinished DMA and resynchronizes corruption.
  IRQ handlers only collect packets/status; no protocol, allocator or disk work.
- `network.c` connects the existing PCI, Ethernet, ARP, IPv4, ICMP, UDP, TCP and
  firewall code. Protocol/timer work runs at scheduler boundaries, outside IRQ
  dispatch. Legacy ticks are derived from the calibrated monotonic clock with
  the existing PIT period; PIT channel 0 is unchanged. Hardware is optional;
  absent RTL8139 keeps the earlier native foundation working. The current link
  uses the house static QEMU IPv4 configuration (10.0.2.15/24, gateway .2).
- Native TCP uses dynamically allocated, retained connection slots, not the
  legacy eight-element table. Closed TCBs survive while a caller holds a
  reference; `tcp_collect` releases inactive/unowned slots between protocol
  operations. Native ISNs use checked secure entropy. Legacy callers keep their
  existing table ABI. IRQ flag helpers use pointer-width registers on both
  architectures. Ephemeral-port exhaustion returns failure rather than reusing
  an occupied port. Blind RSTs outside/exceeding the expected receive sequence
  are dropped/challenge-ACKed; an exact reset still closes the stream.
- Shared wire guards now validate ARP address sizes/opcodes, reject IPv4
  fragments until reassembly exists, and reject invalid/overflowing IPv4 output
  payload sizes before constructing a frame. These are bounds/security fixes,
  not a claim of complete TCP/IPv4 standards coverage. UDP's legacy small queues,
  fragmentation/reassembly, richer TCP close/error/listener behavior, DHCP and
  IPv6 still need native application integration or further work.
- `tests/test_native_network.py` runs production C with simulated I/O: 64 live
  TCP connections, retained reset data, allocation/entropy failure, reclamation,
  reset sequences, ARP/IP parser boundaries, NIC reset deadlines, DMA bounds,
  TX ownership/padding and RX wrap/corrupt/incomplete headers all pass.
- `tests/test_x64_network.py` boots a private image with RTL8139 and a loopback
  host TCP peer. The guest itself performs ARP/TCP setup, verifies every byte of
  a 2 MiB stream, drains EOF, finishes FIN/ACK, releases connection allocations
  and checks memory ownership. Host verifies the request, normal EOF, VM exit 33,
  no faults and unchanged disk SHA-256. Evidence:
  `C:\nexustest\native64-network-release\serial.log`, `network.pcap`, and
  `native64/network-release-stream.log`. This is a kernel transport test, not
  an application socket or internet-browser test.
- The native CPU/disk/ISO matrix passes, including a new full userland run with
  RTL8139 attached (`native64/network-matrix.log`). Graphical Chrome still passes
  the original-page + real PS/2 typing/click + actual pixel + clean-exit gate on
  the new kernel with the documented 120-second child-connection allowance:
  `native64/network-chrome-regression.log`,
  `C:\nexustest\native64-chrome-network-regression\chrome-interactive.png`.
- Rebuilt i386 without warnings and ran its complete 33/33 campaign: real trusted
  HTTPS, 2 MiB HTTP/HTTPS, local certificate failures, 20 repeated transfers,
  zero stuck TCBs/streams and no !EX/!KR. Evidence:
  `C:\nexustest\native64-network-i386-campaign\serial_network.log`,
  `native64/network-i386-campaign.log`. The temporary controller is
  `C:\nexustest\network_i386_campaign.py`; it owns/stops loopback TLS servers.
- Native kernel: **254,318 bytes**, SHA-256
  `7bb50e46404e535d3e81baefb0652903003d2850cce0c33971935879da8bb0a4`.
  i386 kernel: **1,063,852 bytes**, SHA-256
  `9262540a1b6d1b264811c74630b03d8505e0e09c38b7652ba7901e1a81c77c19`.
  Both are warning-free and under the cap. All owned VMs/test servers exited.

Twenty-sixth milestone: **actual graphical Chrome fetches
an HTTP page through native application sockets, renders it, and accepts real
keyboard/mouse input. Download/install gates remain incomplete.**

- `arch/x86_64/inet.c` exposes native AF_INET/SOCK_STREAM descriptors using the
  existing TCP engine. Descriptor ownership, dup/fork/SCM transfer, final close,
  stat/proc identities, read/write/readv/writev, send/recv/sendmsg/recvmsg and
  poll/epoll use the existing file machinery. Socket calls pin descriptions
  while blocked; transport references keep buffers alive through reset/FIN.
- Implemented outbound blocking/nonblocking connect, bound/ephemeral ports,
  address queries, nonblocking flags/FIONBIO, FIONREAD, PEEK/WAITALL, partial I/O,
  signal interruption, SIGPIPE/MSG_NOSIGNAL, read/write shutdown, SO_ERROR and
  receive/send timeouts. Privileged binds check the initial network namespace's
  capability owner. TCP_NODELAY controls actual Nagle behavior; SO_LINGER with
  zero duration resets the connection. Unsupported options/listeners fail.
- TCP now distinguishes refusal (111), reset (104), timeout (110) and orderly
  EOF. SO_ERROR is consumed once, with bad output pointers preserving the error.
  Queued data survives peer reset and local read shutdown. Future data after
  read shutdown is acknowledged/discarded without permanently closing the
  receive window. Native process leak checks exclude only live transport slots
  with no application owner; owned TCB leaks cannot hide in this allowance.
- `tests/test_x64_inet.py` builds/runs the real glibc test `inet_user.c` on a
  private FAT image. It verifies **2 MiB each way byte-for-byte**, epoll readiness,
  nonblocking/alias/vector/peek I/O, WAITALL across delayed packets, timeout,
  signal and shutdown wakeups, FIN with continued sending, RST/error consumption,
  endpoint/proc/stat validation, normal process cleanup and unchanged disk hash.
  Final evidence: `C:\nexustest\native64-inet-confirm\serial.log`, `network.pcap`,
  `native64/inet-confirm.log`. A closed Windows host port was silently dropped;
  the test requires ETIMEDOUT in that case, or ECONNREFUSED if capture proves RST.
  Production-C tests independently exercise refused SYN and timeout paths.
- Preserve the failed `native64-inet-final` run: its capture shows a **51-second
  gap between initial SYN and SYN-ACK**, exceeding the application's unchanged
  45-second poll deadline while several campaigns/image copies overlapped.
  The subsequent run passed with the same bounds/assertions. This is evidence
  of a scheduling/host pause, not a reason to remove deadlines or label failure
  as success. The precise source of that pause is not proven.
- `tests/probe_desktop64.py ... network --ipc-connection-timeout=120` serves a
  controlled HTTP document on the host, attaches native RTL8139, launches the
  unchanged Chrome, and checks initial navigation without retry/injection.
  The host observes the real HTTP GET; read-only DevTools inspection confirms
  the HTTP URL/document. QMP sends actual PS/2 typing/clicks; DOM state and real
  framebuffer pixels agree. Chrome and compositor exit normally and all tasks
  drain. Final run: `C:\nexustest\native64-chrome-http-final\serial.log`,
  `http.log`, `network.pcap`, `chrome-interactive.png`; outer log
  `native64/chrome-http-final.log`. Earlier HTTP runs also passed.
- Visual evidence is copied to `screenshots/native64-chrome-http.png` (SHA-256
  `265edbaafc54d669c366fb722345da4e736b2d4ef099a0d7e8e556862c0e2fa6`).
  This proves numeric-IPv4 HTTP in Chrome, not DNS/public HTTPS/downloads or
  in-OS installation. Chrome is still host-staged, software-rendered and uses
  the documented 120-second child IPC allowance. Seccomp enforcement is pending.
- Native QMP harnesses now use **host-local Unix sockets**, not host TCP ports
  reachable through 10.0.2.2. `tests/qmp_local.py` works with Windows AF_UNIX even
  when Python lacks the named constant. It uses unique owned paths, bounded
  connection/command deadlines, request IDs and safe process-before-path cleanup.
  `test_qmp_local.py` passes actual paused-VM commands, errors and reconnects.
  `test_x64.py`, `test_x64_display.py` and `probe_desktop64.py` are migrated.
  Probe metadata is now `WORK/qmp_path`; old outside-repo helpers using qmp_port
  must be adapted before reuse. Windows QEMU named pipes were investigated but
  did not reliably answer paused-VM commands; use the tested Unix transport.
- Both builds are warning-free. Production transport/parser/DMA tests pass;
  native CPU/disk/ISO/RTL8139 matrix passed (`native64/inet-final-matrix.log`).
  The kernel-level 2 MiB stream still passes (`native64/inet-transport.log`).
  i386's complete **33/33** TLS/network campaign also passes, including trusted
  internet HTTPS, certificate failures, 2 MiB transfers, 20 repeated transfers,
  zero retained TCBs/streams and no !EX/!KR. Evidence:
  `C:\nexustest\native64-inet-i386-campaign\serial_network.log` and
  `native64/inet-i386-campaign.log`; controller `C:\nexustest\inet_i386_campaign.py`.
- Native kernel: **262,510 bytes**, SHA-256
  `2fb9c88d2ff83b2056c87f5f41714856a2300577d6caabc1f04f09d61f414f99`.
  i386 kernel: **1,063,852 bytes**, SHA-256
  `b0968688e18970694f968d5e766be321f911a9544100243133f3dcdc1f60d754`.

Twenty-seventh milestone (current checkpoint): **Chrome opens a public HTTPS
hostname and rejects an untrusted certificate on the custom kernel. Native UDP,
glibc DNS and sendmmsg are implemented and tested. Installation remains pending.**

- Native AF_INET/SOCK_DGRAM endpoints use dynamically allocated queues and
  configurable send/receive memory budgets. The legacy 8-binding/512-byte
  queue does not limit native application sockets. House UDP input validates
  declared length and nonzero checksums against the actual IPv4 destination;
  output checksums cover odd/empty/full-MTU payloads. Directed broadcast uses
  the Ethernet broadcast address; applications need SO_BROADCAST to send it.
- UDP supports bind/autobind, connected peer filtering, AF_UNSPEC disconnect,
  addresses, scalar/vector/message I/O, empty packets, PEEK/TRUNC, FIONREAD,
  nonblocking/epoll, timeouts/signals and shutdown. Reads preserve datagram
  boundaries. A zero-length read does not consume an empty datagram; recv does.
  Buffer charge includes allocated pages, including empty/error packets.
- Transmit queues retry ARP and busy NIC descriptors outside IRQ context. A
  socket closed with accepted packets pending remains owned by the transport
  until delivery or a bounded expiry. Only these orphan allocations are
  excluded from process leak checks. Tests verify later delivery and reaping,
  as well as OOM, queue pressure, malformed pointers and filtering.
- `IP_RECVERR` is real: validated ICMP unreachable/time/parameter errors reach
  SO_ERROR/poll and a separately readable MSG_ERRQUEUE with Linux extended-error,
  offender/address and payload metadata. Error queues share the receive budget,
  and closing/disabling the option frees them. ICMP quotes validate the inner
  IPv4 header, fragment state, lengths and local/connected tuple. Ordinary
  packet PEEK/TRUNC semantics differ from error-queue semantics and are tested.
  This is not complete PMTU discovery or support for every IPv4 socket option.
- The actual glibc resolver required IP_RECVERR before sending queries. Its
  combined A/AAAA lookup then exposed missing **sendmmsg (307)**. The batch
  implementation pins one original descriptor for the whole operation, reuses
  TCP/UDP/Unix sendmsg, returns completed message counts on later errors, checks
  per-message output pointers, supports restart before any success, and enforces
  the ABI's 1024-entry batch bound. UDP partial batches, Unix record boundaries,
  SCM_RIGHTS lifetime/CLOEXEC and the batch bound are tested.
- `arch/x86_64/etc/{resolv.conf,nsswitch.conf,hosts}` configures the existing QEMU
  network (resolver 10.0.2.3). The native builder, Chrome staging and private
  desktop probes install these actual files. No hostname/IP override is used
  for the public browser test. Glibc A-only and combined A/AAAA resolution and
  NXDOMAIN pass. Receiving AAAA records does not mean IPv6 transport is supported.
- `tests/test_native_network.py` exercises production UDP/checksum/queue/ICMP
  code with deterministic NIC/clock/allocation fixtures, alongside earlier TCP,
  packet and DMA tests. `tests/test_x64_udp.py` runs actual glibc syscalls on
  QEMU: full Ethernet MTU, 32 concurrent sockets, epoll, datagram semantics,
  timeout/signal, sendmmsg, orphan delivery and DNS over captured Ethernet.
  Final evidence: `C:\nexustest\native64-udp-final\serial.log`, `network.pcap`,
  and `native64/udp-final.log`; disk hash and process ownership checks pass.
- `probe_desktop64.py ... https --ipc-connection-timeout=120` runs unchanged
  Chrome against `https://example.com/`. Original navigation, returned document,
  secure context, actual framebuffer and normal browser/compositor exit pass:
  `C:\nexustest\native64-chrome-https-verified\serial.log`, `https.png`,
  `network.pcap`; outer log `native64/chrome-https-verified.log`. Certificate
  bypass/host resolver switches are prohibited by these HTTPS probe modes.
- `... https-reject --ipc-connection-timeout=120` generates an untrusted local
  certificate with the correct IP SAN. Chrome displays
  `NET::ERR_CERT_AUTHORITY_INVALID`; the server receives a certificate TLS alert
  and **no HTTP request**. The probe waits for the red warning to appear in the
  actual framebuffer, rather than accepting DOM state or a stale image alone.
  Final evidence: `C:\nexustest\native64-chrome-badcert-visible\serial.log`,
  `certificate-rejected.png`, `tls-result.json`, `network.pcap`; outer log
  `native64/chrome-badcert-visible.log`.
- Preserve diagnostic failures: the first public HTTPS verifier expected an
  old h1 that the live page no longer contains, although the returned document
  was secure. The first negative server's 5-second TLS timeout preceded the
  slow native certificate check and yielded EOF; a 30-second fixture timeout
  produced the required certificate alert. A later screenshot caught an
  unpainted body, so the final probe now checks warning pixels before passing.
  No certificate/transport assertion was removed to hide those failures.
- Screenshots: `screenshots/native64-chrome-https.png`, SHA-256
  `c4133c5fead665bb50639ba99e013a2a2b119d388bcb3b52639f89edbf581b80`;
  `screenshots/native64-chrome-badcert.png`, SHA-256
  `8a18acb105bffef9ff5837befa7e858221b1a6e12598a9c6a06ea4e72dfbdabe`.
  Missing Arabic/CJK glyphs are visibly present in the public-page screenshot:
  current font staging includes Liberation only. Broader font coverage is pending.
- Native CPU/disk/ISO/RTL8139 matrix and the 2 MiB each-way TCP socket regression
  pass (`native64/udp-final-matrix.log`, `native64/udp-tcp-regression.log`). i386
  builds without warnings and passes the complete **33/33** TLS/network campaign
  at `C:\nexustest\native64-udp-i386-retry\serial_network.log`, outer log
  `native64/udp-i386-retry.log`. The first i386 run stayed at login (commands went
  into the username field), was correctly failed, and its owned VM was stopped.
  Retry controller `C:\nexustest\udp_i386_retry.py` uses an outside-repo copy of
  the campaign with a longer startup wait and login screenshots. Historical
  handoff scripts were not rewritten. Confirm process exit; a monitor quit
  request alone did not stop the failed first VM.
- Current native kernel: **270,702 bytes**, SHA-256
  `197396788077d5659e0b78b12a50081f40ff8d31effdab8fed5e98d79de3252f`.
  The GUI HTTPS runs used `b95c32bd9a7e57c11474eb049c1ff51a1186ce8e8a86b8c5ec4c6b6afd8becff`;
  the final follow-up changes only error-queue PEEK/TRUNC handling, and its
  production-C, real UDP/DNS and native matrix checks pass. i386 remains
  **1,063,852 bytes**, SHA-256
  `7d4241c1da7e8d9dc6b3e1744232b54af61c0884bbbaaef22e003c13b7bf3e14`.
  Current inspector: **64,920 bytes**, SHA-256
  `098b631a0e488adbb2451ad336c31dac4782bf34e61d8fe927468cb67926d9c9`.
  Desktop ELF is unchanged at milestone 24's hash; all builds are warning-free.

Next work: actual Chrome downloads and **in-OS Chrome installation**, then a
downloaded application's installation and execution. Chrome is still host-staged;
public browsing is not the complete goal. Native UDP sends use DF and the Ethernet
MTU (EMSGSIZE above 1472 payload bytes), not silent truncation. IPv4 reassembly,
loopback, persistent TCP listeners/accept, netlink/interface reporting, IPv6,
additional socket options and broader TCP standards coverage remain. Current
route configuration is static QEMU Ethernet. Compositor popups/cursor images,
fonts, timing/performance and seccomp enforcement also remain. Never fake
successful options, namespaces or sandbox enforcement.
The existing
`chrome-lock.json` already stages **libwayland-server0 1.23.1-3**, libwayland-client,
libxkbcommon 1.7.0 and xkb-data. Reuse their protocol/keymap helpers; write our own
compositor rather than running Weston/Linux/a remote browser. Actual ELF libraries
are under `out/chrome-root/usr/lib/x86_64-linux-gnu/`; verified headers/protocol
bindings are now prepared by `desktop.py`. Wayland's event loop creates its timerfd lazily;
`wl_event_loop_dispatch` with a timeout plus input-fd sources can work before
native timerfd support. SHM pool resize may need the still-missing mremap syscall.
Primary references: [Wayland event loop](https://raw.githubusercontent.com/wayland-mirror/wayland/1.23.1/src/event-loop.c),
[SHM implementation](https://raw.githubusercontent.com/wayland-mirror/wayland/1.23.1/src/wayland-shm.c),
[xdg-shell protocol](https://raw.githubusercontent.com/wayland-mirror/wayland-protocols/main/stable/xdg-shell/xdg-shell.xml).
Preserve the existing i386 desktop separately.
mremap (25) is also actually requested with
MREMAP_MAYMOVE (for example 12 KiB -> 20 KiB); getpriority (140), scheduling queries
and other operations are still absent. HTTP and public HTTPS browsing have passed;
download and in-OS installation gates remain pending, and the full goal is active.
The on-device model utility still reports missing CPU-info/Dawn support. Keep
investigating actual child/task lifetimes rather than killing background tasks
to manufacture a clean exit. Then continue seccomp, graphics, networking and
in-OS download/install/launch; ptrace and signal wait/queue interfaces remain.
close_range (436) is absent; the observed `0x1bc` probe is landlock_create_ruleset
(444), not close_range. CPU/sysinfo/affinity and inotify support also remain.
User-ID setters, setns and network/mount namespaces are not yet supported;
seccomp must enforce real policies before claiming sandbox support. Nested
epoll, EPOLLEXCLUSIVE, pselect and epoll_pwait2 are also pending. The goal still
includes graphics, native networking and in-OS download/install/launch.
Thread gaps are explicit: multithreaded exec currently returns
ENOSYS (single-threaded fork/vfork/exec works); robust-list registration, futex
PI/requeue operations and rseq still return ENOSYS. Job-control stop/continue,
wait resource accounting and background writeback/journaling remain
unsupported. Never replace missing behavior with fake success.

The goal remains active: graphical Chrome, rendered HTTP/HTTPS pages and native
TCP/UDP/DNS application networking have passed; Chrome downloads and native
installation/launch of a downloaded app remain unproven.

Inspected official Linux Chrome 155.0.8059.39-1 package at
`C:\nexustest\chrome_support\google-chrome-stable_current_amd64.deb` (143,552,428
bytes). Its actual executable is ELF64/x86-64; package metadata/dependencies and
SHA-256 are in `package-inspection.json` there. Package was downloaded and
inspected only, not installed on the Windows host or an external guest OS.
The actual Chrome ELF has been extracted at `C:\nexustest\chrome_support\chrome`
(297,798,984 bytes), inspected as PIE with `/lib64/ld-linux-x86-64.so.2`, and has
32 direct DT_NEEDED entries. Its version command has now been executed as above.

A user-launched original i386 QEMU was previously observed (PID 23548), then the
user closed it. No QEMU process remained at this checkpoint. Never stop a
user-launched VM or overwrite its attached images. All native x64 work remains
in the separate out/ and C:\nexustest paths; re-check process state before touching
main artifacts. No commits/staging were performed.

## Fresh independent regression (2026-10-10)

Rebuilt and reran the current tree after the user's testing request. All VM runs
used private image copies under `C:\nexustest\retest_20261010`; the pre-native-work
kernel hash above reproduced exactly. No OS source fix was needed in that run.

| Check | Fresh result / artifact |
|---|---|
| Build | 1,059,756 bytes, zero warnings; `build.log` |
| Downloads | CLI 32 MiB, chunked 3 MiB+17, redirect, empty file; browser 8 MiB and full 1,200,070-byte HTML. Exact disk bytes checked, including 32 MiB after reboot; `downloads.log` |
| Failure cleanup | Full disk, CLI/browser cancel, truncated response and existing destination: sentinel intact, no leaked data clusters or partial files; `failures.log` |
| Storage tools | 16->64->128 MiB migration preserved contents, long/UTF-8 names, both FATs and protected bytes. Shrink/corrupt images refused; disk-full installation remained atomic |
| Userland | Runtime/FPU, Lua, shell pipeline, game self-check and Kilo edit/save: all five output files matched exactly; `userland.log` |
| HTTPS and crypto | TLS/HTTP 33/33, crypto/parser/TCP 44/44; real HTTPS and repeated transfers passed with zero leftover TCP connections/streams; `tls.log` |
| Network servers | VNC, HTTP, remote shell and sync: 58/58 host checks; `servers/run.log` |
| Boot | 10/10 reached login; ISO login/desktop/shell screenshots also reviewed; `boots.log`, `iso/` |
| Fault recovery | Six injected faults (null, GPF, invalid opcode, divide, write protection, stack overflow) contained; crypto tests passed afterward; `recovery/` |
| Command smoke test | 66 commands across 8 batches without unexpected faults; NPFS journal recovery screenshot showed intact recovered file; `commands.log` |

Final independent assertions scanned all serial logs for both `!EX:` and `!KR:`:
zero fatal faults, exactly six expected recoveries in the deliberate fault test,
and no recoveries elsewhere. No harness marker timeouts. Machine-readable result:
`C:\nexustest\retest_20261010\verified-results.json`. Owned VMs and test servers
were stopped. Main disk was not used as a test target; no staging or commit.

Scope: these are QEMU checks. Physical hardware and Chrome application execution
remain unverified/unsupported respectively; this pass does not establish general
Windows/Linux/macOS binary compatibility. The 66-command campaign is a crash
smoke test; download and userland checks additionally assert exact output bytes.

## Download goal: current behavior

The artificial 256 KiB download cap is removed. Browser and wget use 16 KiB
buffers and stream directly to FAT on disk. Binary files download automatically;
Ctrl+S or the toolbar Save arrow saves any HTTP(S) URL, including pages/images
larger than the rendering preview. Files go to `/disk/Downloads`; Open Downloads
opens the File Manager. Long UTF-8 filenames and Content-Disposition are handled.
Esc or the Cancel link removes partial data. Explicit existing destinations are
refused; automatic names receive a numbered suffix. A temporary short filename
is published only when the complete body and disk writes succeed.

The main disk is now **1 GiB**. All 26 pre-existing files were migrated with
identical SHA-256 values, and NPFS/persistence bytes were identical after the host
migration. Manifest: `C:\nexustest\downloads\main_migration.json`.

```powershell
.\build.bat storage 8192   # grow disk.img to 8 GiB; stop QEMU first
.\build.bat run
```

`build.bat apps` and `run` ensure the expanded layout for legacy images. Further
growth is explicit. The existing FAT32 format has its native maximum of
4,294,967,295 bytes per file; ATA LBA28 currently addresses disks up to 128 GiB.
These are filesystem/driver bounds, not the old RAM download cap. No binary
compatibility with Chrome/Windows/Linux/macOS was added by this download work.

Verified download evidence (`C:\nexustest\downloads`):

- Official Google Chrome offline installer: **167,683,000 bytes** downloaded by
  NexusOS over trusted HTTPS. SHA-256 read from the guest disk exactly matched an
  independently fetched reference with a valid Google LLC Authenticode signature:
  `e5d6e542768cd69637877a22b1a8772dacf3de83ecee7744425b9cdfac416d91`.
  Artifact: `chrome/chrome_disk.img`, guest `/disk/Downloads/ChromeSetup.exe`.
  The EXE was never executed; this proves downloading, not application compatibility.
- Duplicate automatic filenames were saved separately with original bytes intact (`duplicates/`).
- CLI: 32 MiB archive, 3 MiB+17 chunked response, redirect and empty file, all disk
  bytes verified. Browser: 8 MiB installer and full 1,200,070-byte HTML via Ctrl+S.
  The 32 MiB file was checked again after a second boot. `campaign/`.
- Disk full, CLI/browser cancellation, interrupted bodies, existing-file refusal:
  no partial files/data-cluster leaks; sentinel and protected bytes unchanged.
  `failures_fixed/`. Esc routing was fixed because the desktop consumed it.
- Growth 16->64->128 MiB: existing contents, long/UTF-8 names, mirrored FATs and
  protected bytes verified; shrinking/corrupt volumes refused without mutation.
- Userland runtime, Lua, Kilo edit/save, pipes and games passed again on the main
  1 GiB layout (`userland_final/`). TLS campaign passed **33/33**, crypto **44/44**
  (`tls_final/`). Broad command regression passed 66 commands without faults (`commands_final/`);
  final expanded-layout boot loop passed 10/10 (`boot_final/`).

Repeatable checks:

```powershell
python -I tests/test_storage.py
python -I tests/test_downloads.py
python -I tests/test_download_failures.py
python -I userland/tests/test_vm.py
```

The download campaign originally compared every protected byte across reboots;
NPFS intentionally updates its mount counter (superblock offset 52) and CRC (56).
The test now validates the exact expected count increment and CRC, and checks
all remaining protected bytes. Host migrations still require exact equality.

New/changed code: `download.c/h` shared sink; FAT driver volume bounds, long names,
sequential cursors/allocation hint and staged publication; browser save/cancel UI;
wget disk output; HTTP header/length checks; quoted shell args; File Manager disk
navigation; `storage.py` and expanded-image support in `userland/fatput.py`.
See `handoff/notes/downloads-in-progress.md` for the development trace.

## What changed

- Reliable TCP, streaming HTTP/HTTPS, `curl`, `wget`, `tlsinfo`, `cryptotest`.
- TLS 1.3 AES-128-GCM/X25519, RSA/ECDSA authentication and built-in roots.
  Self-signed/unknown certificates now fail by default. Explicit `-k` permits
  diagnostic access; browser overrides apply only to the accepted origin.
  Certificate usage, critical constraints, validity and hostname are checked.
  Secure randomness uses RDRAND; there is no clock-based fallback.
- Graphical browser: basic HTML/images, links, history, scroll, redirects,
  downloads, certificate errors and verified-HTTPS indication. The temporary
  `netstream_compat.c` and obsolete `html.c/h` were removed.
- Picolibc 1.8.12 supplies actual stdio/stdlib/string/math/setjmp. The thin
  `userland/libc/src/platform.c` port connects it to NexusOS syscalls.
- Lua 5.4.7 and Kilo compile from unchanged upstream C sources. A small `sh`,
  file utilities, 2048 and Tetris run as isolated static i386 user processes.
- Process support includes fds, pipes, spawn/wait, close-on-exec, private pages,
  ANSI terminals and FPU save/restore. Background programs do not starve the
  parked desktop thread. Ring-0 remains cooperative.
- App Store, Spotlight and GUI Terminal launch real applications.
- Download failures preserve existing files; oversized/partial downloads are
  reported as failures. FAT installation uses a temporary image and atomic replace.
- Fixed a 16-bit TCP receive-capacity truncation that stalled large GUI reads at 64 KiB. A new known-answer test covers the boundary; streaming consumers no longer narrow the capacity.
- Fixed quoted shell operators, user-address upper-bound validation and initial
  FPU state isolation. Kernel and userland builds are warning-free.

## Build and use

From PowerShell in `NexusOS/`:

```powershell
.\build.bat apps
.\build.bat run
```

Login `root`/`root` or `guest`/`guest`; Ctrl+Space opens Spotlight, F12 opens the
kernel console. `apps` lists programs. Run `sh` for userland pipes/redirection.
Kilo saves with Ctrl+S and exits with Ctrl+Q. See `userland/README.md` for examples,
build setup and repeatable tests.

First app build needs Python 3.12+ and internet for checksum-pinned Picolibc and
pinned Meson/Ninja in `tools/userland-build`. Later builds use local caches.
The existing vendored i686-elf GCC compiles both kernel and userland. QEMU lives
at `C:\Program Files\qemu\qemu-system-i386.exe`.

Use **-cpu max -display sdl** for interactive QEMU. RDRAND is required for HTTPS.
GTK on Windows repaints slowly. Keep RTC in UTC for certificate checks.
Every kernel C file needs an explicit object in build.bat; crypto/layout/syscall
hot paths use -O2. The kernel has no libgcc; user programs may link libgcc.
`build.bat` must retain CRLF endings (LF produces spurious cmd.exe M commands).

## Verification

The legacy VM campaigns below used disposable image copies under `C:\nexustest`.
Normal native desktop acceptance above intentionally uses the permanent image;
never treat it as a disposable test copy.

| Check | Result / artifacts |
|---|---|
| TLS/HTTP campaign | 33/33 checks, `dailyuse_tls/serial_campaign.log` |
| Crypto/parser/TCP checks | 44/44 latest, `dailyuse_browser_fixed/serial_fixed.log` (43/43 in the earlier campaign) |
| Real HTTPS | example.com, Google, Cloudflare, letsencrypt.org trusted; Wikipedia fetched |
| Bulk transfer | 2 MiB HTTP and HTTPS with exact CRC, 20 repeat downloads, no leaked TCBs/streams |
| Existing servers | 58/58 host checks, `dailyuse_netfinal/host_reg.log` |
| Userland | Exact outputs checked for runtime/FPU, Lua, pipeline, game logic, Kilo save |
| GUI | Browser + CPU-busy Lua + Calculator, playable GUI-terminal games, `dailyuse_gui/` |
| Boot loop | 10/10 reached login, no faults, `dailyuse_bootfinal/` |
| ISO | Login/desktop reached, `dailyuse_iso/` |
| USB | UHCI, xHCI hub, EHCI companion hub, OHCI hub, `dailyuse_usb/` |
| Persistence | File contents survived hard reset, `dailyuse_persist/` |
| Crash containment | Null, UD, divide, write-protect, stack faults recovered, `dailyuse_kguard/` |
| Missing entropy | qemu32 rejects HTTPS even with -k; `dailyuse_no_rng/` |
| App Store | Installed apps and Open actions displayed; `dailyuse_store/` |
| FAT installer | Contents/tail, disk-full atomicity, corrupt-volume refusal passed |
| Final command campaign | 66 commands across 8 batches, no faults; `dailyuse_final_campaign/` |
| Extended browser pages | Long/malformed HTML, redirects, images, chunking/reload, escaped errors, 6144-byte download CRC FA0104A6; `dailyuse_browser_final/` |
| Interrupted download | Explicit error instead of saving a partial file; real HTTPS and 2 MiB TLS rechecked, `dailyuse_final_edges/` |
| Large-page regression | 300 KiB page reaches explicit 256 KiB cap; no 64 KiB stall, `dailyuse_browser_fixed/` |

```powershell
python -I userland/tests/test_fatput.py
python -I userland/tests/test_vm.py
python -I handoff/tools/bootloop2.py . C:\nexustest\boot_new 10
python -u -I handoff/tools/fullcheck.py . C:\nexustest\commands_new virtio floppy 56200
```

For TLS: launch `handoff/ws_tls/tlsserver.py <server-workdir>` on loopback
18101-18104, then `python -I handoff/ws_tls/campaign.py <workdir> <port> <tag> .`.
`tlscheck.py <serial.log>` returns nonzero on failure. Its diagnostic parser was
fixed to retain CN= text. Self-signed positive cases use -k; default rejection
is tested separately. `nettests.py` now checks oversized PUT at 262145 bytes.
`harness_net.py` uses the accompanying `pydes.py`.

Verified screenshots: `screenshots/phase57-*.png`. Normal tests must contain no
`!EX:` (fatal) or `!KR:` (contained bug). Only explicit crashtest injection should
produce !KR. Tests that merely print serial:ok are not sufficient to prove a
feature works: inspect output/screenshots or actual saved files.

## Memory/disk landmines

- PIT stays at 18.2 Hz. Never do blocking/crypto work in IRQ handlers.
- Heap: 4 MiB at 0x400000..0x800000.
- Normal .bss: 0x800000..0xC22450, guard remains 0xC60000.
- Browser .app_bss: 0x02000000..0x024721E0, zeroed/reserved by pmm_init.
- Identity map: **40 MiB**, 10 page tables. High user frames begin at
  USER_PMM_BASE=0x02800000; keep it equal to IDENTITY_MAP_END. Linker asserts
  .app_bss ends below it. Legacy image/stacks/shmem/mmap and graphics addresses
  remain unchanged. Map MMIO before creating user process page directories.
- Disk: legacy images use FAT below LBA 20480. Expanded images have an MBR FAT32
  partition from LBA 32768 onward. NPFS stays at 20480..24802; RAM persistence
  stays at 26624..32767 (two 1.5 MiB A/B slots). Never overlap those regions.
- fatput validates the protected tail, detects concurrent image changes, then
  replaces the disk atomically. Only an entirely empty FAT window is formatted.
  The host migration preserved bytes at LBA 20480..32767 exactly; the new FAT
  partition above LBA 32768 is writable storage.
- Test work directories must have no spaces (QEMU screendump/drive path gotcha).
- User apps use private virtual memory, entry 0x08048000, 256 KiB stack and
  per-process heap. They are stored on FAT, not linked into kernel.bin.

## Boundaries and next work

This remains an independent development OS, not universal desktop compatibility.
The legacy i386 desktop cannot run existing Chrome/Office or ordinary desktop
binaries. Its C programs target the NexusOS ABI and compilation runs on the host.
The separate native x86-64 target now runs the installed Chrome and GNU Hello
described above; it remains a development target with incomplete desktop/runtime
coverage. General Windows/macOS/Linux application compatibility is not established.

Browser: no JavaScript engine, modern CSS or full Unicode; one page at a time,
256 KiB page/image preview buffers and bounded decoded-image pools. Ctrl+S saves
the complete URL to disk regardless of those preview limits. DNS/handshake work
can briefly stall the cooperative UI. Downloads use FAT and preserve existing files.

TLS: no TLS 1.2, HelloRetryRequest, resumption, client certs, OCSP/CRL or independent
crypto audit. Unsupported certificate constraints fail closed. Physical hardware
and actual audio/video playback were not newly validated.

Userland: no fork/exec replacement, job control, sockets API, full POSIX signals
or complete mmap/munmap (unsupported munmap fails explicitly). FAT storage is growable;
RAM snapshots can omit files when their 1.5 MiB slot fills, with a status warning.
SQLite/MicroPython and richer browser/application compatibility remain follow-up
work. Do not label the whole OS production-ready. No commit is authorized yet.
