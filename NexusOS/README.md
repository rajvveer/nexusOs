# NexusOS 🚀

**A hybrid operating system taking the best of Windows, macOS, and Linux.**

> *Phase 14: VESA VBE Graphics Mode (1024×768×32bpp)*

## Architecture

| Component | Technology |
|-----------|-----------|
| Architecture | x86 (i386), 32-bit Protected Mode |
| Bootloader | Custom 2-stage (Real Mode → VESA → Protected Mode) |
| Display | VESA VBE 2.0 (1024×768, 32bpp, linear framebuffer) |
| Input | PS/2 Keyboard + Mouse |
| Memory | Bitmap physical allocator + Paging (16MB) + Heap |
| GUI | Pixel-mode desktop with bitmap font rendering |
| Desktop | Login, icons, right-click menus, notifications |
| Image | Raw floppy `.img` (1.44 MB) |

## Roadmap

- [x] **Phase 1** — Bare Metal Kernel
- [x] **Phase 2** — OS Services (filesystem, multitasking)
- [x] **Phase 3** — Userspace (terminal, editor, games)
- [x] **Phase 4** — GUI (window manager, themes)
- [x] **Phase 5** — Apps (start menu, calculator, file manager)
- [x] **Phase 6** — UX (login, icons, context menus, notifications)
- [x] **Phase 7-13** — Advanced UX, apps, widgets, tools
- [x] **Phase 14** — VESA VBE Graphics (1024×768×32bpp pixel mode!)
- [x] **Phase 15** — Font Rendering Engine
- [x] **Phase 16** — Graphics Primitives & Widget Toolkit
- [x] **Phase 17** — Modern Window Manager
- [x] **Phase 18** — Desktop Shell v2
- [x] **Phase 19** — FAT32 Filesystem
- [x] **Phase 20** — ELF Binary Loader
- [x] **Phase 21** — Memory Management v2
- [x] **Phase 22** — Advanced Process Management
- [x] **Phase 23** — Device Driver Framework
- [x] **Phase 24** — Storage Stack
- [x] **Phase 25** — USB Stack (Basic)
- [x] **Phase 26** — Network Driver
- [x] **Phase 27** — TCP/IP Stack
- [x] **Phase 28** — DNS & HTTP Client
- [x] **Phase 29** — Web Browser (Text)
- [x] **Phase 30** — Network Services
- [x] **Phase 31** — POSIX Compatibility Layer
- [x] **Phase 32** — ELF Dynamic Linking
- [x] **Phase 33** — X11/Wayland Shim

---

*NexusOS — Built from scratch, no compromises.* 🔥

## Project Structure

```
NexusOS/
├── boot/
│   ├── boot.asm           # Stage 1: MBR bootloader (512 bytes)
│   └── boot2.asm          # Stage 2: Load kernel, switch to 32-bit
├── kernel/
│   ├── kernel_entry.asm   # ASM → C bridge
│   ├── kernel.c           # Main init (kernel_main)
│   ├── vga.c/h            # Screen output driver
│   ├── gdt.c/h + asm      # CPU memory segments
│   ├── idt.c/h + asm      # Interrupt table
│   ├── isr_asm.asm        # CPU exception stubs
│   ├── irq_asm.asm        # Hardware interrupt stubs
│   ├── pic.c/h            # Interrupt controller
│   ├── keyboard.c/h       # Keyboard input driver
│   ├── mouse.c/h          # PS/2 mouse driver (IRQ12)
│   ├── memory.c/h         # Physical page allocator
│   ├── paging.c/h         # Virtual memory
│   ├── heap.c/h           # Dynamic memory allocator
│   ├── vfs.c/h            # Virtual filesystem
│   ├── ramfs.c/h          # RAM-based filesystem
│   ├── process.c/h        # Process manager
│   ├── scheduler.c/h      # Round-robin scheduler
│   ├── syscall.c/h        # System call interface
│   ├── rtc.c/h            # Real-time clock
│   ├── speaker.c/h        # PC speaker driver
│   ├── gui.c/h            # GUI framebuffer & drawing
│   ├── window.c/h         # Window manager
│   ├── desktop.c/h        # Desktop compositor
│   ├── taskbar.c/h        # Taskbar (dock)
│   ├── theme.c/h          # Theme engine (4 themes)
│   ├── start_menu.c/h     # Start menu popup
│   ├── calculator.c/h     # Calculator app
│   ├── filemgr.c/h        # File manager app
│   ├── sysmon.c/h         # System monitor app
│   ├── login.c/h          # Login screen (Phase 6)
│   ├── icons.c/h          # Desktop icons (Phase 6)
│   ├── context_menu.c/h   # Right-click menu (Phase 6)
│   ├── notify.c/h         # Notification toasts (Phase 6)
│   ├── settings.c/h       # Settings app (Phase 6)
│   ├── editor.c/h         # Text editor
│   ├── snake.c/h          # Snake game
│   ├── shell.c/h          # Interactive command shell
│   ├── string.c/h         # String utilities
│   ├── port.h             # Hardware I/O port access
│   └── types.h            # Fixed-width type definitions
├── linker.ld              # Memory layout for kernel
├── build.bat              # Windows build script
├── Makefile               # Linux/Mac build system
└── README.md              # This file
```

## Build & Run

```bash
# Build (Windows)
build.bat

# Build and run in QEMU
build.bat run

# Clean build files
build.bat clean
```

## Boot Flow

```
BIOS → boot.asm → boot2.asm → kernel_entry.asm → kernel_main()
    → Init all subsystems → Login Screen → Shell
        └── gui command → Desktop (Icons + Start Menu + Windows + Themes)
```

## Shell Commands (29)

| Command | Description |
|---------|------------|
| `help` | Show available commands |
| `clear` / `cls` | Clear the screen |
| `about` | Display NexusOS info |
| `meminfo` | Show memory statistics |
| `heapinfo` | Show heap allocator stats |
| `uptime` | Show system uptime |
| `date` | Show current date/time |
| `echo <text>` | Print text to screen |
| `color <0-15>` | Change text color |
| `history` | Show command history |
| `ls` | List files |
| `cat <file>` | Display file contents |
| `touch <file>` | Create empty file |
| `write <file> <text>` | Write text to file |
| `rm <file>` | Delete a file |
| `edit <file>` | Open text editor |
| `ps` | List processes |
| `run <name>` | Start a process |
| `kill <pid>` | Terminate a process |
| `snake` | Play Snake game |
| `beep` | Play boot sound |
| `gui` | Launch desktop GUI |
| `theme <name>` | Switch theme |
| `calc` | Launch Calculator |
| `files` | Launch File Manager |
| `sysmon` | Launch System Monitor |
| `settings` | Launch Settings |
| `reboot` | Restart the system |

## Desktop GUI

| Key | Action |
|-----|--------|
| `Esc` | Exit desktop |
| `Tab` | Cycle themes |
| `Ctrl+A` | About window |
| `Ctrl+T` | Terminal |
| `Ctrl+M` | Start Menu |
| `Ctrl+F` | File Manager |
| `Ctrl+K` | Calculator |
| `Ctrl+S` | System Monitor |
| `Ctrl+N` | Settings |
| `Ctrl+W` | Close window |
| Right-click | Context menu |
| Click icon | Launch app |

### Desktop Features (Phase 6)

| Feature | Description |
|---------|-------------|
| **Login Screen** | Animated splash + username/password |
| **Desktop Icons** | 6 clickable app shortcuts |
| **Right-Click Menu** | New File, Refresh, Theme, About, Settings |
| **Notifications** | Auto-dismiss toasts at top-right |
| **Settings App** | 3-tab settings (Theme, System, About) |

### Themes
- **dark** — Cyan on black (default)
- **light** — Blue on white
- **retro** — Green on black (terminal)
- **ocean** — Blue tones


