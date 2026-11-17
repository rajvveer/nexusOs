/* ============================================================================
 * NexusOS - Kernel Main (5.1 "Aurora")
 * ============================================================================
 * Initializes all subsystems and launches the shell.
 * Phase 14: VESA VBE graphics mode (1024x768x32bpp).
 * Phase 26: Network driver (RTL8139).
 * Phase 27: TCP/IP stack (ARP, IP, ICMP, UDP, TCP, Socket API).
 * Phase 28: DNS resolver and HTTP client.
 * Phase 29: Text web browser.
 * Phase 30: Network services (DHCP, NTP, HTTP server, remote shell).
 * Phase 31: POSIX compatibility (/proc, termios, Unix tools).
 * Phase 32: ELF dynamic linking (shared libraries, dlopen/dlsym/dlclose).
 * Phase 33: X11 compatibility shim (Windows, Events, GC, Graphics).
 * Phase 34: Win32 compatibility layer (PE32, kernel32/user32/gdi32, Registry).
 * Phase 35: Package manager (.npk archive format, repository, dependency resolver).
 * Phase 36: Scripting engine (NexusScript interpreter, OS automation/macros).
 * Phase 37: macOS compatibility shim (Mach-O loader, Core Foundation, Cocoa).
 * ============================================================================ */

#include "types.h"
#include "version.h"
#include "port.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"
#include "vfs.h"
#include "ramfs.h"
#include "process.h"
#include "scheduler.h"
#include "syscall.h"
#include "uproc.h"
#include "rtc.h"
#include "speaker.h"
#include "mouse.h"
#include "shell.h"
#include "desktop.h"
#include "string.h"
#include "framebuffer.h"
#include "login.h"
#include "ata.h"
#include "fat32.h"
#include "elf.h"
#include "vmm.h"
#include "pipe.h"
#include "env.h"
#include "pci.h"
#include "driver.h"
#include "blkdev.h"
#include "partition.h"
#include "usb.h"
#include "usb_hid.h"
#include "usb_storage.h"
#include "net.h"
#include "rtl8139.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "socket.h"
#include "dns.h"
#include "http.h"
#include "browser.h"
#include "dhcp.h"
#include "ntp.h"
#include "httpd.h"
#include "rshell.h"
#include "vnc.h"
#include "sync.h"
#include "assistant.h"
#include "mobile.h"
#include "perf.h"
#include "appstore.h"
#include "finale.h"
#include "npfs.h"
#include "procfs.h"
#include "termios.h"
#include "posix.h"
#include "dynlink.h"
#include "libc.h"
#include "x11.h"
#include "pe.h"
#include "registry.h"
#include "win32.h"
#include "pkg.h"
#include "script.h"
#include "macho.h"
#include "cocoa.h"
#include "audio.h"
#include "ac97.h"
#include "image.h"
#include "video.h"
#include "gpu.h"
#include "gamepad.h"
#include "game.h"
#include "accessibility.h"
#include "users.h"
#include "firewall.h"
#include "kguard.h"     /* Phase 55: kernel self-protection + crash containment */
#include "persist.h"

/* Timer tick counter */
volatile uint32_t system_ticks = 0;

/* Phase 48: until automated init finishes, the timer IRQ (which fires from
 * the sti right after pic_init onward) must NOT touch subsystems that are
 * still being built. Only system_ticks++ runs early; net_poll / schedule /
 * the status-bar redraw (reads pmm/rtc/VGA) are deferred behind this flag.
 * (The boot double fault itself was IRQ0 arriving at the un-remapped BIOS
 * vector 8 — fixed by masking the PIC in boot2 and the late sti.)
 * MUST be volatile (written in task ctx, read in IRQ ctx). */
volatile bool boot_init_done = false;

/* Status bar update interval (~1 second at 18.2 Hz) */
#define STATUSBAR_INTERVAL 18

/* --------------------------------------------------------------------------
 * update_statusbar: Refresh the status bar content
 * -------------------------------------------------------------------------- */
static void update_statusbar(void) {
    /* Left: OS name + version */
    char left[40] = " " NEXUS_VERSION_STRING;

    /* Center: time */
    rtc_time_t t;
    rtc_read(&t);
    char center[20];
    rtc_format_time(&t, center);

    /* Right: memory + uptime */
    char right[40];
    uint32_t free_kb = pmm_get_free_pages() * 4;
    char num[12];
    strcpy(right, "Free:");
    int_to_str(free_kb, num);
    strcat(right, num);
    strcat(right, "KB ");

    vga_update_statusbar(left, center, right);
}

/* --------------------------------------------------------------------------
 * timer_callback: IRQ0 handler — ticks + scheduler + status bar
 * -------------------------------------------------------------------------- */
static void timer_callback(struct registers* regs) {
    system_ticks++;   /* MUST stay unconditional — all timeouts depend on it */

    /* Phase 48: defer all heavy IRQ-time work until init has finished. During
     * boot the timer fires while pmm/heap/rtc/VGA/net are still being built;
     * touching them here (status-bar redraw reads rtc/pmm/VGA; net_poll allocs)
     * is the suspected trigger of the intermittent boot #DF. The PIC EOI is
     * already sent by irq_handler BEFORE this callback, so an early return does
     * not wedge the PIC. */
    if (!boot_init_done) return;

    /* Update status bar every ~1 second (skip in VESA — desktop has its own) */
    if (system_ticks % STATUSBAR_INTERVAL == 0 && !fb_is_vesa()) {
        /* Save and restore cursor so status bar doesn't disturb shell */
        int saved_row = vga_get_cursor_row();
        int saved_col = vga_get_cursor_col();
        update_statusbar();
        vga_set_cursor(saved_row, saved_col);
    }

    /* Process queued network packets */
    net_poll();

    schedule(regs);
}

/* --------------------------------------------------------------------------
 * print_banner: Display the NexusOS boot banner
 * -------------------------------------------------------------------------- */
static void print_banner(void) {
    vga_print_color("\n", VGA_COLOR(VGA_WHITE, VGA_BLACK));
    vga_print_color("  ███╗   ██╗███████╗██╗  ██╗██╗   ██╗███████╗ ██████╗ ███████╗\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ████╗  ██║██╔════╝╚██╗██╔╝██║   ██║██╔════╝██╔═══██╗██╔════╝\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ██╔██╗ ██║█████╗   ╚███╔╝ ██║   ██║███████╗██║   ██║███████╗\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ██║╚██╗██║██╔══╝   ██╔██╗ ██║   ██║╚════██║██║   ██║╚════██║\n", VGA_COLOR(VGA_CYAN, VGA_BLACK));
    vga_print_color("  ██║ ╚████║███████╗██╔╝ ██╗╚██████╔╝███████║╚██████╔╝███████║\n", VGA_COLOR(VGA_CYAN, VGA_BLACK));
    vga_print_color("  ╚═╝  ╚═══╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝ ╚═════╝ ╚══════╝\n", VGA_COLOR(VGA_CYAN, VGA_BLACK));
    vga_print("\n");
    vga_print_color("  " NEXUS_VERSION " " NEXUS_CODENAME, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    vga_print_color(" — The Hybrid Operating System\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  Best of Windows + macOS + Linux\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  Initializing kernel subsystems...\n\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * kernel_main: The main entry point of the kernel
 * -------------------------------------------------------------------------- */
void kernel_main(void) {
    /* 1. Initialize VGA display */
    vga_init();
    print_banner();

    /* 2. Set up Global Descriptor Table */
    gdt_init();

    /* 3. Set up Interrupt Descriptor Table */
    idt_init();

    /* 4. Initialize PIC (remap hardware interrupts) */
    pic_init();

    /* 5. Register timer (IRQ0), then enable interrupts for the first time.
     * IF has been clear since boot2's cli and the 8259s were masked there:
     * until pic_init remapped them, IRQ0 would have arrived as vector 8 and
     * been misread as a #DF (the old intermittent boot double fault). */
    register_interrupt_handler(32, timer_callback);
    __asm__ volatile("sti");
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Timer initialized (IRQ0, ~18.2 Hz)\n");

    /* 6. Initialize keyboard */
    keyboard_init();

    /* 6b. Initialize mouse */
    mouse_init();

    /* 7. Initialize physical memory manager */
    pmm_init();

    /* 8. Enable paging */
    paging_init();

    /* ---- Phase 55: kernel self-protection + in-kernel crash containment ----
     * Write-protect .text/.rodata, unmap the NULL page and the boot-stack
     * guard band, then install kguard's double-fault task gate so a kernel
     * stack overflow is recoverable too. Must follow paging_init (uses the
     * live identity map and the kernel CR3) and precede everything else. */
    paging_protect_kernel();
    kguard_init();
    /* ---- end Phase 55 ---- */

    /* === Phase 14: VESA Framebuffer (Bochs VBE) === */

    /* 8b. Initialize framebuffer — sets mode via Bochs VBE I/O ports */
    fb_init();

    /* 8c. If VESA activated, map framebuffer pages and switch rendering */
    if (fb_is_vesa()) {
        /* Map the hardware framebuffer into page tables (full size — a
         * 1920x1080x32 LFB is ~8MB and needs more than one 4MB page table). */
        paging_map_vesa_fb(fb_get_phys_addr(), fb_get_lfb_size());

        /* Now safe to write to front buffer — present the cleared screen */
        fb_flip();

        vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("VESA 1024x768x32 framebuffer active\n");

        /* Switch VGA text output to render on pixel framebuffer */
        vga_reinit_vesa();
    }

    /* === Phase 2 Subsystems === */

    /* 9. Initialize heap allocator */
    heap_init();

    /* 10. Initialize virtual filesystem */
    vfs_init();

    /* 11. Mount RAM filesystem */
    ramfs_init();

    /* 11b. Initialize ATA disk driver */
    ata_init();

    /* 11c. Mount FAT32 filesystem from disk */
    fat32_init();

    /* 12. Initialize process manager */
    process_init();

    /* 13. Initialize scheduler */
    scheduler_init();

    /* 14. Initialize syscall interface */
    syscall_init();

    /* 14b. Initialize ELF loader */
    elf_init();

    /* 14c. Initialize VMM (COW, shmem, mmap) */
    vmm_init();

    /* 14d. Initialize pipes and environment */
    pipe_init();
    env_init();

    /* 14e. PCI bus + driver framework */
    pci_init();
    driver_init();
    driver_probe_all();

    /* 14f. Block device layer + partitions */
    blkdev_init();
    partition_init();

    /* 14g. USB stack */
    usb_init();
    usb_hid_init();
    usb_storage_init();

    /* === Phase 26: Network === */

    /* 14h. Network subsystem + RTL8139 NIC driver */
    net_init();
    rtl8139_init();
    driver_probe_all();   /* Re-probe to match RTL8139 against PCI */

    /* === Phase 27: TCP/IP Stack === */

    /* 14i. Protocol stack */
    arp_init();
    ip_init();
    icmp_init();
    udp_init();
    tcp_init();
    socket_init();

    /* === Phase 44: Firewall (packet filter — hooks the IP layer) === */
    firewall_init();

    /* === Phase 28: DNS & HTTP === */
    dns_init();
    http_init();

    /* === Phase 29: Browser === */
    browser_init();

    /* === Phase 30: Network Services === */
    dhcp_init();
    ntp_init();
    httpd_init();
    rshell_init();

    /* === Phase 45: Cloud & Sync (VNC remote desktop + file/settings sync) === */
    vnc_init();
    sync_init();

    /* === Phase 31: POSIX Compatibility === */
    procfs_init();
    termios_init();
    posix_init();

    /* === Phase 32: ELF Dynamic Linking === */
    dynlink_init();
    libc_init();

    /* === Phase 33: X11 Compatibility Shim === */
    x11_init();

    /* === Phase 34: Win32 Compatibility Layer === */
    pe_init();
    registry_init();
    win32_init();

    /* === Phase 35: Package Manager === */
    pkg_init();

    /* === Phase 36: Scripting Engine === */
    script_init();

    /* === Phase 37: macOS Compatibility Shim === */
    macho_init();
    cocoa_init();

    /* === Phase 38: Sound — Audio Mixer + AC'97 Driver === */
    audio_init();
    ac97_init();   /* probes PCI, registers itself with the audio core */

    /* === Phase 39: Image Formats (BMP/PNG/JPEG/GIF + viewer) === */
    image_init();

    /* === Phase 40: Video Playback (AVI/MJPEG media player) === */
    video_init();

    /* === Phase 41: GPU Acceleration (VirtIO-GPU scanout takeover) === */
    gpu_init();

    /* === Phase 42: Gaming Framework (controller layer + NexusSDL) === */
    gamepad_init();
    game_init();

    /* === Phase 3 Subsystems === */

    /* 15. Initialize RTC */
    rtc_init();

    /* 16. Initialize PC speaker */
    speaker_init();

    /* === Phase 43: Accessibility (Era 6 opener) ===
     * After the speaker (earcons need it) and the gamepad (we chain its raw
     * keyboard hook). Owns the keyboard raw-hook slot from here on. */
    accessibility_init();

    /* === Phase 44: Security — user accounts (must init BEFORE login_run). === */
    users_init();

    /* === Phase 46: AI Assistant (offline command helper) === */
    assistant_init();

    /* === Phase 47: Mobile/Embedded mode (touch/orientation/power/arm) === */
    mobile_init();

    /* === Phase 49: App Store & Ecosystem (storefront over the pkg manager;
     * needs pkg_init + users_init already done above). === */
    appstore_init();

    /* === Phase 50: v5.0 Grand Finale (universal binaries, footprint report,
     * disk installer; needs ata/pmm/loaders already up). === */
    finale_init();

    /* === Phase 51: NPFS journaling filesystem (mounts if present in its
     * reserved high-LBA window; needs ata_init done above). === */
    npfs_init();

    /* ---- Phase 55: persistent ramfs ----
     * Everything created by the _inits above is a built-in demo file: stamp
     * it, then restore the user's snapshot from the disk window (files,
     * directories, modes, deleted built-ins, accounts). Must run BEFORE
     * login_run so restored accounts/passwords can log in. */
    persist_mark_builtins();
    persist_init();

    /* === Phase 48: automated init is complete. Stamp the boot time, then let
     * the timer IRQ resume its full duties (status bar / net_poll / schedule).
     * Order matters: mark boot done AFTER the last _init and BEFORE login_run
     * (a blocking loop), and after rtc_init so the status bar can read the RTC. */
    perf_mark_boot_done();
    boot_init_done = true;

    /* All systems go! */
    vga_print("\n");
    vga_print_color("  ============================================\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print_color("  All systems initialized successfully!\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print_color("  ============================================\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    {
        char nb[12];
        uint32_t bt = perf_boot_ticks();
        vga_print_color("  Boot: ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        int_to_str((int)bt, nb); vga_print(nb); vga_print(" ticks (~");
        int_to_str((int)(bt * 55), nb); vga_print(nb); vga_print(" ms)");
        if (bt * 55 < 1000) vga_print_color("  [under 1 second]\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        else vga_print("\n");
    }

    /* 17. Draw initial status bar */
    update_statusbar();

    /* 18. Play boot sound */
    play_boot_sound();

    /* 19. Login screen (Phase 44: real auth; returns the authenticated uid and
     * has already set the current user). */
    uint32_t login_uid = login_run();

    /* ---- Phase 57: per-user Downloads folder ----
     * Make sure /home/<user>/Downloads exists for the account that just logged
     * in and is owned by it. Still inside the persist boot phase, so this is a
     * built-in rather than a dirtying edit; files saved into it are snapshotted
     * together with their directory chain. */
    ramfs_ensure_user_dirs(users_name_of(login_uid), login_uid);
    /* ---- end Phase 57 ---- */

    /* 20. Create shell as process PID 1, owned by the logged-in user. */
    process_t* shell_proc = process_create("shell", NULL);
    if (shell_proc != NULL) {
        shell_proc->uid = login_uid;
        shell_proc->state = PROC_RUNNING;
        process_set_current(shell_proc);
    }

    /* Phase 54: register the shell as the kernel thread (process 0), install
     * the built-in demo binaries, and enable scheduling. Must run AFTER the
     * shell PCB is current so the scheduler has a base context to return to. */
    uproc_init();

    /* Phase 55: the demo binaries uproc_init just installed are built-ins
     * too; from here on every ramfs change is user state (autosaved). */
    persist_boot_done();

    /* Phase 14: In VESA mode, launch desktop directly (full screen) */
    if (fb_is_vesa()) {
        desktop_run();
    }

    /* Fallback: text-mode shell */
    shell_run();

    /* Should never reach here */
    while (1) {
        __asm__ volatile("hlt");
    }
}
