<p align="center">
  <img src="https://img.shields.io/badge/Architecture-x86_(i386)-blue?style=for-the-badge" alt="x86"/>
  <img src="https://img.shields.io/badge/Language-C_&_Assembly-orange?style=for-the-badge" alt="C & ASM"/>
  <img src="https://img.shields.io/badge/Display-1024×768_32bpp-green?style=for-the-badge" alt="VESA"/>
  <img src="https://img.shields.io/badge/Kernel_Size-~400KB-red?style=for-the-badge" alt="Kernel"/>
  <img src="https://img.shields.io/badge/Phase-33_of_51-purple?style=for-the-badge" alt="Phase"/>
</p>

# NexusOS

> **A hybrid operating system built entirely from scratch — taking the best of Windows, macOS, and Linux, with none of the bloat.**

NexusOS is a bare-metal x86 operating system written in C and Assembly. No GRUB, no third-party bootloader, no Linux kernel underneath — just raw metal. It features a custom bootloader, VESA graphics at 1024×768, a full TCP/IP networking stack, a windowed desktop environment, 20+ built-in applications, and POSIX/X11/Win32 compatibility layers — all in under 1 MB of kernel code.

---

## ✨ Feature Highlights

### 🖥️ Desktop Environment
- **Pixel-mode GUI** — VESA VBE 2.0 at 1024×768×32bpp with double-buffered rendering
- **Window Manager** — Overlapping windows with drag, resize, minimize, maximize & close animations
- **Desktop Shell** — Login screen, desktop icons, wallpapers, right-click context menus, notification toasts
- **Taskbar & Start Menu** — App launcher, system tray, window switcher
- **4 Themes** — Dark, Light, Retro, Ocean with full theme engine

### 🌐 Networking
- **RTL8139 NIC Driver** with full Ethernet frame handling
- **TCP/IP Stack** — ARP → IP → ICMP → UDP → TCP with socket API
- **DNS & HTTP Client** — Resolve domains, download files
- **Web Browser** — HTML parser with link navigation and history
- **Network Services** — DHCP, NTP time sync, HTTP server, SSH client

### 🔧 Compatibility Layers
- **POSIX** — `open`/`read`/`write`/`fork`/`exec`, `/proc` filesystem, signals, termios
- **ELF Dynamic Linking** — Shared libraries, `dlopen`/`dlsym`, libc
- **X11 Shim** — Basic X protocol for running simple X11 applications
- *(In Progress)* **Win32 & macOS** — PE loader, Mach-O loader

### 🧠 Core OS
- **Custom 2-stage bootloader** — Real Mode → VESA mode → Protected Mode
- **Memory Management** — Bitmap physical allocator, paging (16 MB), heap, virtual memory with CoW `fork()`
- **Process Management** — ELF binary loader, round-robin scheduler, signals, pipes, IPC
- **FAT32 Filesystem** — Persistent storage with directory hierarchy and long filenames
- **Device Drivers** — PCI bus, ATA/AHCI disk, USB (UHCI/OHCI), PS/2 keyboard & mouse

### 🎮 Built-in Applications (20+)
| | | | |
|---|---|---|---|
| 🖥️ Shell (60+ commands) | 📝 Text Editor | 🧮 Calculator | 📁 File Manager |
| 📊 System Monitor | ⚙️ Settings | 🐍 Snake | 🏓 Pong |
| 💣 Minesweeper | 🧱 Tetris | 🎨 Paint | 🎵 Music Player |
| 🌐 Web Browser | 📓 Notepad | 📅 Calendar | ✅ Todo List |
| 🔍 Search | 📇 Contacts | 🎨 Color Picker | 📂 Hex Viewer |
| 🗑️ Recycle Bin | 🔒 Lock Screen | 🖼️ Screensaver | ❓ Help Center |

---

## 🏗️ Architecture

| Component | Implementation |
|---|---|
| **CPU Architecture** | x86 (i386), 32-bit Protected Mode |
| **Bootloader** | Custom 2-stage (Real Mode → VESA → Protected Mode) |
| **Display** | VESA VBE 2.0 — 1024×768, 32bpp, linear framebuffer |
| **Input** | PS/2 Keyboard & Mouse (IRQ1 / IRQ12) |
| **Memory** | Bitmap physical allocator + Paging (16 MB) + Heap + VMM with CoW |
| **Filesystem** | VFS layer → RAM filesystem + FAT32 on disk |
| **Networking** | RTL8139 → Ethernet → ARP/IP/ICMP/UDP/TCP → Sockets → DNS/HTTP |
| **Process Model** | ELF32 loader, ring-3 userspace, round-robin scheduler, POSIX signals |
| **GUI Toolkit** | Framebuffer → GFX primitives → Font engine → Widget toolkit → Window manager |
| **Image Format** | Raw floppy `.img` (1.44 MB) / Hard disk image (16 MB) |

---

## 📂 Project Structure

```
NexusOS/
├── boot/
│   ├── boot.asm              # Stage 1: MBR bootloader (512 bytes)
│   └── boot2.asm             # Stage 2: VESA setup, load kernel, enter protected mode
│
├── kernel/
│   ├── ── Core ──────────────────────────────────────────
│   ├── kernel_entry.asm      # ASM → C bridge
│   ├── kernel.c              # Main init (kernel_main)
│   ├── types.h               # Fixed-width type definitions
│   ├── port.h                # Hardware I/O port access
│   ├── string.c/h            # String utilities
│   ├── libc.c/h              # Standard C library functions
│   │
│   ├── ── CPU & Interrupts ──────────────────────────────
│   ├── gdt.c/h + asm         # Global Descriptor Table
│   ├── idt.c/h + asm         # Interrupt Descriptor Table
│   ├── isr_asm.asm           # CPU exception stubs
│   ├── irq_asm.asm           # Hardware interrupt stubs
│   ├── pic.c/h               # Programmable Interrupt Controller
│   ├── syscall.c/h           # System call interface
│   │
│   ├── ── Memory ────────────────────────────────────────
│   ├── memory.c/h            # Physical page allocator
│   ├── paging.c/h            # Virtual memory (paging)
│   ├── heap.c/h              # Dynamic memory allocator (kmalloc)
│   ├── vmm.c/h               # Virtual memory manager (CoW, mmap)
│   │
│   ├── ── Filesystem ────────────────────────────────────
│   ├── vfs.c/h               # Virtual filesystem layer
│   ├── ramfs.c/h             # RAM-based filesystem
│   ├── fat32.c/h             # FAT32 disk filesystem
│   ├── procfs.c/h            # /proc filesystem
│   │
│   ├── ── Processes ─────────────────────────────────────
│   ├── process.c/h           # Process management
│   ├── scheduler.c/h         # Round-robin scheduler
│   ├── switch.asm            # Context switch (ASM)
│   ├── elf.c/h               # ELF binary loader
│   ├── dynlink.c/h           # Dynamic linker (shared libraries)
│   ├── pipe.c/h              # Inter-process communication
│   ├── env.c/h               # Environment variables
│   ├── posix.c/h             # POSIX compatibility layer
│   ├── termios.c/h           # Terminal I/O control
│   │
│   ├── ── Drivers ───────────────────────────────────────
│   ├── keyboard.c/h          # PS/2 keyboard driver
│   ├── mouse.c/h             # PS/2 mouse driver
│   ├── vga.c/h               # VGA text mode driver
│   ├── rtc.c/h               # Real-time clock
│   ├── speaker.c/h           # PC speaker
│   ├── pci.c/h               # PCI bus enumeration
│   ├── ata.c/h               # ATA disk driver
│   ├── usb.c/h               # USB host controller
│   ├── usb_hid.c/h           # USB HID (keyboard/mouse)
│   ├── usb_storage.c/h       # USB mass storage
│   ├── driver.c/h            # Driver framework
│   ├── blkdev.c/h            # Block device layer
│   ├── partition.c/h         # Disk partitioning (MBR)
│   │
│   ├── ── Networking ────────────────────────────────────
│   ├── net.c/h               # Network core
│   ├── rtl8139.c/h           # RTL8139 NIC driver
│   ├── arp.c/h               # Address Resolution Protocol
│   ├── ip.c/h                # Internet Protocol
│   ├── icmp.c/h              # ICMP (ping)
│   ├── udp.c/h               # User Datagram Protocol
│   ├── tcp.c/h               # Transmission Control Protocol
│   ├── socket.c/h            # BSD socket API
│   ├── dhcp.c/h              # DHCP client
│   ├── dns.c/h               # DNS resolver
│   ├── http.c/h              # HTTP/1.1 client
│   ├── httpd.c/h             # HTTP server
│   ├── ntp.c/h               # NTP time sync
│   ├── html.c/h              # HTML parser
│   │
│   ├── ── Graphics & GUI ────────────────────────────────
│   ├── gui.c/h               # GUI framebuffer core
│   ├── framebuffer.c/h       # Framebuffer driver
│   ├── gfx.c/h               # Graphics primitives
│   ├── font.c/h              # Font rendering engine
│   ├── font8x8.h             # Bitmap font data
│   ├── ui_widgets.c/h        # Widget toolkit
│   ├── window.c/h            # Window manager
│   ├── desktop.c/h           # Desktop compositor
│   ├── taskbar.c/h           # Taskbar / dock
│   ├── start_menu.c/h        # Start menu
│   ├── theme.c/h             # Theme engine
│   ├── palette.c/h           # Color palette system
│   ├── wallpaper.c/h         # Wallpaper rendering
│   ├── icons.c/h             # Desktop icons
│   ├── context_menu.c/h      # Right-click menus
│   ├── notify.c/h            # Notification toasts
│   ├── notifcenter.c/h       # Notification center
│   ├── appearance.c/h        # Appearance settings
│   │
│   ├── ── Desktop Utilities ─────────────────────────────
│   ├── login.c/h             # Login screen
│   ├── lockscreen.c/h        # Lock screen
│   ├── screensaver.c/h       # Screensaver
│   ├── switcher.c/h          # Window switcher (Alt+Tab)
│   ├── workspaces.c/h        # Virtual workspaces
│   ├── shortcuts.c/h         # Keyboard shortcuts
│   ├── dock.c/h              # Application dock
│   ├── clipboard.c/h         # Clipboard manager
│   ├── clipmgr.c/h           # Clipboard UI
│   ├── power.c/h             # Power management
│   ├── recycle.c/h           # Recycle bin
│   │
│   ├── ── Applications ──────────────────────────────────
│   ├── shell.c/h             # Interactive command shell
│   ├── editor.c/h            # Text editor
│   ├── notepad.c/h           # Notepad (GUI)
│   ├── calculator.c/h        # Calculator
│   ├── filemgr.c/h           # File manager
│   ├── sysmon.c/h            # System monitor
│   ├── taskmgr.c/h           # Task manager
│   ├── settings.c/h          # Settings app
│   ├── browser.c/h           # Web browser
│   ├── calendar.c/h          # Calendar
│   ├── contacts.c/h          # Contacts
│   ├── todo.c/h              # Todo list
│   ├── music.c/h             # Music player
│   ├── paint.c/h             # Paint app
│   ├── colorpick.c/h         # Color picker
│   ├── hexview.c/h           # Hex viewer
│   ├── search.c/h            # File search
│   ├── help.c/h              # Help center
│   ├── sysinfo.c/h           # System information
│   ├── syslog.c/h            # System logger
│   ├── fileops.c/h           # File operations GUI
│   │
│   ├── ── Games ─────────────────────────────────────────
│   ├── snake.c/h             # Snake game
│   ├── pong.c/h              # Pong game
│   ├── minesweeper.c/h       # Minesweeper
│   ├── tetris.c/h            # Tetris
│   │
│   ├── ── Compatibility ─────────────────────────────────
│   ├── x11.c/h               # X11/Wayland protocol shim
│   ├── rshell.c/h            # Remote shell
│   └── userlib.h             # Userspace library interface
│
├── tools/                     # Build utilities
├── assets/                    # Static assets
├── linker.ld                  # Kernel memory layout
├── build.bat                  # Windows build script
├── Makefile                   # Linux/macOS build system
└── README.md
```

> **113 source files** · **~400 KB kernel** · Written entirely in C and x86 Assembly

---

## 🔥 Build & Run

### Prerequisites
- **NASM** — Assembler for x86
- **i686-elf-gcc** — Cross-compiler (GCC targeting i386-elf)
- **QEMU** — x86 system emulator

### Build Commands

```bash
# Windows — Build everything
build.bat

# Windows — Build and launch in QEMU
build.bat run

# Windows — Clean build artifacts
build.bat clean

# Linux / macOS
make
make run
make clean
```

---

## ⚡ Boot Flow

```
┌──────┐    ┌──────────┐    ┌───────────┐    ┌────────────────┐    ┌──────────────┐
│ BIOS │───▶│ boot.asm │───▶│ boot2.asm │───▶│ kernel_entry   │───▶│ kernel_main()│
│      │    │ (Stage 1)│    │ (Stage 2) │    │ (ASM→C bridge) │    │              │
└──────┘    └──────────┘    └───────────┘    └────────────────┘    └──────┬───────┘
                                                                         │
  ┌──────────────────────────────────────────────────────────────────────┘
  │
  ▼
  Init GDT → IDT → PIC → Memory → Paging → Heap → VFS → Keyboard → Mouse
  → RTC → PCI → ATA → FAT32 → Network → VESA Framebuffer → Font Engine
  │
  ▼
  ┌──────────────┐    ┌───────────────┐    ┌───────────────────────────┐
  │ Login Screen │───▶│ Command Shell │───▶│ Desktop GUI               │
  │              │    │               │    │ (Icons + Windows + Themes) │
  └──────────────┘    └───────────────┘    └───────────────────────────┘
```

---

## 🖥️ Shell Commands

<details>
<summary><b>View all commands (40+)</b></summary>

| Command | Description |
|---|---|
| **System** | |
| `help` | Show available commands |
| `about` | Display NexusOS info |
| `meminfo` | Show memory statistics |
| `heapinfo` | Show heap allocator stats |
| `uptime` | Show system uptime |
| `date` | Show current date/time |
| `reboot` | Restart the system |
| **Display** | |
| `clear` / `cls` | Clear the screen |
| `echo <text>` | Print text to screen |
| `color <0-15>` | Change text color |
| `history` | Show command history |
| **Files** | |
| `ls` | List files |
| `cat <file>` | Display file contents |
| `touch <file>` | Create empty file |
| `write <file> <text>` | Write text to file |
| `rm <file>` | Delete a file |
| `edit <file>` | Open text editor |
| **Processes** | |
| `ps` | List processes |
| `run <name>` | Start a process |
| `kill <pid>` | Terminate a process |
| **Apps & Games** | |
| `gui` | Launch desktop environment |
| `calc` | Launch Calculator |
| `files` | Launch File Manager |
| `sysmon` | Launch System Monitor |
| `settings` | Launch Settings |
| `snake` | Play Snake |
| `beep` | Play boot sound |
| `theme <name>` | Switch theme (dark/light/retro/ocean) |

</details>

---

## 🎨 Desktop GUI

### Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Esc` | Exit desktop |
| `Tab` | Cycle themes |
| `Ctrl+A` | About window |
| `Ctrl+T` | Terminal |
| `Ctrl+M` | Start Menu |
| `Ctrl+F` | File Manager |
| `Ctrl+K` | Calculator |
| `Ctrl+S` | System Monitor |
| `Ctrl+N` | Settings |
| `Ctrl+W` | Close focused window |
| Right-click | Context menu |
| Click icon | Launch application |

### Desktop Features

| Feature | Description |
|---|---|
| **Login Screen** | Animated splash with username/password authentication |
| **Desktop Icons** | Clickable app shortcuts with labels |
| **Right-Click Menu** | New File · Refresh · Theme · About · Settings |
| **Notifications** | Auto-dismiss toasts at top-right corner |
| **Settings App** | Theme, System, and About tabs |
| **Lock Screen** | Secure lock with password prompt |
| **Screensaver** | Auto-activating animated screensaver |
| **Virtual Workspaces** | Multiple desktops for organizing windows |
| **Window Switcher** | Alt+Tab style window cycling |

### Themes

| Theme | Description |
|---|---|
| 🌙 **Dark** | Cyan on black — default |
| ☀️ **Light** | Blue on white |
| 💚 **Retro** | Green on black — classic terminal |
| 🌊 **Ocean** | Blue tones — calm and modern |

---

## 🗺️ Roadmap

### Progress: `████████████████████████████████░░░░░░░░░░░░░░░░░░` **65%** (33/51 phases)

<details>
<summary><b>🎨 Era 1 — Graphics Revolution (Phase 14–18)</b> ✅ Complete</summary>

- [x] **Phase 14** — VESA VBE Graphics (1024×768×32bpp pixel mode)
- [x] **Phase 15** — Font Rendering Engine
- [x] **Phase 16** — Graphics Primitives & Widget Toolkit
- [x] **Phase 17** — Modern Window Manager
- [x] **Phase 18** — Desktop Shell v2
</details>

<details>
<summary><b>🖥️ Era 2 — OS Foundations (Phase 19–25)</b> ✅ Complete</summary>

- [x] **Phase 19** — FAT32 Filesystem
- [x] **Phase 20** — ELF Binary Loader
- [x] **Phase 21** — Memory Management v2 (VMM, CoW fork)
- [x] **Phase 22** — Advanced Process Management (signals, pipes, IPC)
- [x] **Phase 23** — Device Driver Framework (PCI, plug-and-play)
- [x] **Phase 24** — Storage Stack (ATA, block devices, partitions)
- [x] **Phase 25** — USB Stack (UHCI/OHCI, HID, mass storage)
</details>

<details>
<summary><b>🌐 Era 3 — Networking (Phase 26–30)</b> ✅ Complete</summary>

- [x] **Phase 26** — Network Driver (RTL8139)
- [x] **Phase 27** — TCP/IP Stack (ARP, IP, ICMP, UDP, TCP, sockets)
- [x] **Phase 28** — DNS & HTTP Client
- [x] **Phase 29** — Web Browser (HTML parser, link navigation)
- [x] **Phase 30** — Network Services (DHCP, NTP, HTTP server)
</details>

<details>
<summary><b>🔧 Era 4 — Compatibility Layers (Phase 31–37)</b> 🔨 In Progress</summary>

- [x] **Phase 31** — POSIX Compatibility Layer
- [x] **Phase 32** — ELF Dynamic Linking
- [x] **Phase 33** — X11/Wayland Shim ← *current*
- [ ] **Phase 34** — Win32 Compatibility Layer (PE loader, Win32 API)
- [ ] **Phase 35** — Package Manager (`.npk` format, repositories)
- [ ] **Phase 36** — Scripting Engine (Lua or custom interpreter)
- [ ] **Phase 37** — macOS Compatibility Shim (Mach-O, Cocoa subset)
</details>

<details>
<summary><b>🎮 Era 5 — Multimedia (Phase 38–42)</b></summary>

- [ ] **Phase 38** — Sound Driver (AC97/HDA, WAV playback)
- [ ] **Phase 39** — Image Formats (PNG, JPEG, GIF decoder)
- [ ] **Phase 40** — Video Playback (AVI, media player)
- [ ] **Phase 41** — GPU Acceleration (VirtIO-GPU, 60fps)
- [ ] **Phase 42** — Gaming Framework (SDL-like API, Doom port)
</details>

<details>
<summary><b>💎 Era 6 — Polish & Superiority (Phase 43–48)</b></summary>

- [ ] **Phase 43** — Accessibility (screen reader, high contrast)
- [ ] **Phase 44** — Security (authentication, permissions, firewall)
- [ ] **Phase 45** — Cloud & Sync (file sync, VNC server)
- [ ] **Phase 46** — AI Assistant (NLP commands, autocomplete)
- [ ] **Phase 47** — Mobile/Embedded Mode (touchscreen, ARM)
- [ ] **Phase 48** — Performance Optimization (SMP, <1s boot)
</details>

<details>
<summary><b>🚀 Era 7 — World Domination (Phase 49–51)</b></summary>

- [ ] **Phase 49** — App Store & Ecosystem
- [ ] **Phase 50** — v5.0 Grand Finale (universal binary support, ISO image)
- [ ] **Phase 51** — NPFS (NexusOS Persistent File System)
</details>

---

## 📊 Growth Projection

| Phase | Apps | Source Files | Kernel Size | Milestone |
|:---:|:---:|:---:|:---:|---|
| 13 | 30 | 90 | 140 KB | Text-mode OS |
| 18 | 35 | 120 | 250 KB | **Pixel graphics** |
| 25 | 40 | 180 | 400 KB | USB + persistent disk |
| 30 | 45 | 230 | 600 KB | **Internet access** |
| 33 | 50 | 340 | 400 KB | **Current** ⭐ |
| 37 | 50 | 380 | 800 KB | Run Win/Mac/Linux apps |
| 42 | 55 | 420 | 900 KB | **Doom runs** |
| 50 | 60+ | 500+ | < 1 MB | 🌍 World domination |

---

<p align="center">
  <b>NexusOS</b> — Built from scratch. No compromises. 🔥
  <br><br>
  <i>"From 2,000 character cells to 786,432 pixels — a 393× improvement."</i>
</p>
