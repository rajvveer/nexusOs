@echo off
REM ============================================================================
REM NexusOS — Windows Build Script
REM ============================================================================
REM Usage: build.bat [run|clean|debug]
REM ============================================================================

REM --- Toolchain paths (local tools directory) ---
set TOOLS_DIR=%~dp0tools
set NASM=%TOOLS_DIR%\nasm-2.16.01\nasm.exe
set CC=%TOOLS_DIR%\i686-elf\bin\i686-elf-gcc.exe
set LD=%TOOLS_DIR%\i686-elf\bin\i686-elf-ld.exe

REM --- Check for QEMU in standard locations ---
set QEMU=
if exist "C:\Program Files\qemu\qemu-system-i386.exe" set QEMU=C:\Program Files\qemu\qemu-system-i386.exe
if exist "%TOOLS_DIR%\qemu\qemu-system-i386.exe" set QEMU=%TOOLS_DIR%\qemu\qemu-system-i386.exe

REM --- Flags ---
set CFLAGS=-ffreestanding -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -Wall -Wextra -c
set LDFLAGS=-T linker.ld --oformat binary -nostdlib
set ASM_BIN=-f bin
set ASM_ELF=-f elf32

REM --- Handle arguments ---
if "%1"=="clean" goto clean
if "%1"=="run" goto build_and_run
if "%1"=="debug" goto build_and_debug

:build
echo.
echo ========================================
echo  NexusOS Build System
echo ========================================
echo.

REM --- Check tools exist ---
if not exist "%NASM%" (
    echo [ERROR] NASM not found at: %NASM%
    echo         Download from https://www.nasm.us/
    exit /b 1
)
if not exist "%CC%" (
    echo [ERROR] i686-elf-gcc not found at: %CC%
    echo         Download from https://github.com/lordmilko/i686-elf-tools
    exit /b 1
)
echo [OK] Tools found

REM --- Assemble bootloader ---
echo [BUILD] Stage 1 bootloader...
"%NASM%" %ASM_BIN% boot\boot.asm -o boot\boot.bin
if errorlevel 1 (echo [FAIL] boot.asm & exit /b 1)

echo [BUILD] Stage 2 bootloader...
"%NASM%" %ASM_BIN% boot\boot2.asm -o boot\boot2.bin
if errorlevel 1 (echo [FAIL] boot2.asm & exit /b 1)

REM --- Assemble kernel ASM files ---
echo [BUILD] Kernel assembly...
"%NASM%" %ASM_ELF% kernel\kernel_entry.asm -o kernel\kernel_entry.o
if errorlevel 1 (echo [FAIL] kernel_entry.asm & exit /b 1)

"%NASM%" %ASM_ELF% kernel\gdt_asm.asm -o kernel\gdt_asm.o
if errorlevel 1 (echo [FAIL] gdt_asm.asm & exit /b 1)

"%NASM%" %ASM_ELF% kernel\idt_asm.asm -o kernel\idt_asm.o
if errorlevel 1 (echo [FAIL] idt_asm.asm & exit /b 1)

"%NASM%" %ASM_ELF% kernel\isr_asm.asm -o kernel\isr_asm.o
if errorlevel 1 (echo [FAIL] isr_asm.asm & exit /b 1)

"%NASM%" %ASM_ELF% kernel\irq_asm.asm -o kernel\irq_asm.o
if errorlevel 1 (echo [FAIL] irq_asm.asm & exit /b 1)

"%NASM%" %ASM_ELF% kernel\switch.asm -o kernel\switch.o
if errorlevel 1 (echo [FAIL] switch.asm & exit /b 1)

REM --- Compile C files ---
echo [BUILD] Kernel C files...
for %%f in (kernel\*.c) do (
    echo         %%f
    "%CC%" %CFLAGS% "%%f" -o "%%~dpnf.o"
    if errorlevel 1 (echo [FAIL] %%f & exit /b 1)
)

REM --- Link kernel ---
echo [BUILD] Linking kernel...
"%LD%" %LDFLAGS% -o kernel.bin ^
    kernel\kernel_entry.o ^
    kernel\kernel.o ^
    kernel\vga.o ^
    kernel\gdt.o ^
    kernel\gdt_asm.o ^
    kernel\idt.o ^
    kernel\idt_asm.o ^
    kernel\isr_asm.o ^
    kernel\irq_asm.o ^
    kernel\pic.o ^
    kernel\keyboard.o ^
    kernel\memory.o ^
    kernel\paging.o ^
    kernel\heap.o ^
    kernel\vfs.o ^
    kernel\ramfs.o ^
    kernel\process.o ^
    kernel\scheduler.o ^
    kernel\switch.o ^
    kernel\syscall.o ^
    kernel\rtc.o ^
    kernel\speaker.o ^
    kernel\editor.o ^
    kernel\snake.o ^
    kernel\mouse.o ^
    kernel\gui.o ^
    kernel\window.o ^
    kernel\desktop.o ^
    kernel\taskbar.o ^
    kernel\theme.o ^
    kernel\start_menu.o ^
    kernel\calculator.o ^
    kernel\filemgr.o ^
    kernel\sysmon.o ^
    kernel\login.o ^
    kernel\icons.o ^
    kernel\context_menu.o ^
    kernel\notify.o ^
    kernel\settings.o ^
    kernel\switcher.o ^
    kernel\screensaver.o ^
    kernel\lockscreen.o ^
    kernel\clipboard.o ^
    kernel\wallpaper.o ^
    kernel\wallpaper_img.o ^
    kernel\palette.o ^
    kernel\notepad.o ^
    kernel\taskmgr.o ^
    kernel\calendar.o ^
    kernel\music.o ^
    kernel\help.o ^
    kernel\widgets.o ^
    kernel\paint.o ^
    kernel\workspaces.o ^
    kernel\minesweeper.o ^
    kernel\recycle.o ^
    kernel\sysinfo.o ^
    kernel\todo.o ^
    kernel\clock.o ^
    kernel\pong.o ^
    kernel\power.o ^
    kernel\search.o ^
    kernel\tetris.o ^
    kernel\hexview.o ^
    kernel\contacts.o ^
    kernel\colorpick.o ^
    kernel\syslog.o ^
    kernel\notifcenter.o ^
    kernel\dock.o ^
    kernel\clipmgr.o ^
    kernel\shortcuts.o ^
    kernel\appearance.o ^
    kernel\fileops.o ^
    kernel\framebuffer.o ^
    kernel\font.o ^
    kernel\gfx.o ^
    kernel\ata.o ^
    kernel\fat32.o ^
    kernel\elf.o ^
    kernel\vmm.o ^
    kernel\pipe.o ^
    kernel\env.o ^
    kernel\pci.o ^
    kernel\driver.o ^
    kernel\blkdev.o ^
    kernel\partition.o ^
    kernel\usb.o ^
    kernel\usb_hid.o ^
    kernel\usb_storage.o ^
    kernel\net.o ^
    kernel\rtl8139.o ^
    kernel\arp.o ^
    kernel\ip.o ^
    kernel\icmp.o ^
    kernel\udp.o ^
    kernel\tcp.o ^
    kernel\socket.o ^
    kernel\dns.o ^
    kernel\http.o ^
    kernel\html.o ^
    kernel\browser.o ^
    kernel\dhcp.o ^
    kernel\ntp.o ^
    kernel\httpd.o ^
    kernel\rshell.o ^
    kernel\procfs.o ^
    kernel\termios.o ^
    kernel\posix.o ^
    kernel\dynlink.o ^
    kernel\libc.o ^
    kernel\shell.o ^
    kernel\string.o ^
    kernel\ui_widgets.o ^
    kernel\x11.o
if errorlevel 1 (echo [FAIL] Linking & exit /b 1)

REM --- Create OS image ---
echo [BUILD] Creating disk image...
copy /b boot\boot.bin + boot\boot2.bin + kernel.bin nexus.img > nul
if errorlevel 1 (echo [FAIL] Image creation & exit /b 1)

REM --- Pad to 1.44MB floppy size ---
powershell -Command "$f = [System.IO.File]::Open('nexus.img', 'Open'); $size = $f.Length; $f.SetLength(1474560); $f.Close(); Write-Host ('Image size: ' + $size + ' bytes -> padded to 1474560 bytes')"

echo.
echo ========================================
echo  BUILD SUCCESS: nexus.img
echo ========================================
echo.
if "%1"=="" exit /b 0
goto :eof

:build_and_run
call :build
if errorlevel 1 exit /b 1
echo [RUN] Starting QEMU...
if "%QEMU%"=="" (
    echo [ERROR] QEMU not found. Install QEMU or set QEMU path in build.bat
    echo         Download from https://qemu.weilnetz.de/w64/
    exit /b 1
)
REM --- Create FAT32 disk image if needed ---
if not exist disk.img (
    echo [BUILD] Creating 16MB FAT32 disk image...
    python mkfat32.py
    if errorlevel 1 (
        echo [WARN] Python not found, creating raw disk image
        fsutil file createnew disk.img 16777216 >nul 2>&1
    )
)
"%QEMU%" -m 256 -drive file=nexus.img,format=raw,if=floppy -drive file=disk.img,format=raw,if=ide -boot a -vga std -serial file:serial.log -netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=tcp::2323-:2323 -device rtl8139,netdev=net0
goto :eof

:build_and_debug
call :build
if errorlevel 1 exit /b 1
echo [RUN] Starting QEMU in debug mode...
if "%QEMU%"=="" (
    echo [ERROR] QEMU not found.
    exit /b 1
)
"%QEMU%" -m 256 -drive file=nexus.img,format=raw,if=floppy -drive file=disk.img,format=raw,if=ide -boot a -vga std -d int -no-reboot -no-shutdown -serial file:serial.log -netdev user,id=net0 -device rtl8139,netdev=net0
goto :eof

:clean
echo [CLEAN] Removing build artifacts...
del /q kernel\*.o 2>nul
del /q boot\*.bin 2>nul
del /q kernel.bin 2>nul
del /q nexus.img 2>nul
echo [CLEAN] Done.
goto :eof
