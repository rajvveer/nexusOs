---
name: nexusos-boot-and-test
description: "How to build, run, and drive the NexusOS bare-metal OS in QEMU for verification"
metadata: 
  node_type: memory
  type: reference
  originSessionId: b0f2b8aa-ba08-4b77-b0ff-55f157badd00
  modified: 2026-10-08T07:33:13.584Z
---

NexusOS lives in `NexusOS/` (bare-metal x86 OS, C + NASM). Build/run on Windows:

- **Build from Claude's Bash tool:** `cmd.exe //c "C:\\Users\\admin\\Desktop\\New folder\\NexusOS\\build.bat"` — a bare `build.bat` (even after `cd`, or via PowerShell `cmd /c`) fails with "not recognized"; only the full path works. ~10 s, 0 warnings (since 2026-10-08); the build fails if kernel.bin > 1,015,808 bytes. For a symbolized ELF, re-run the build.bat `ld` object list without `--oformat binary`.
- **Build:** `cd NexusOS; .\build.bat` (toolchain in `NexusOS/tools/`: nasm, i686-elf-gcc/ld). Makefile auto-globs `kernel/*.c`; **build.bat lists each `kernel\*.o` explicitly in the link step — new `.c` files must be added there**. Output: `nexus.img` (floppy) + `disk.img` (FAT32, auto-created).
- **Run + drive:** launch QEMU with a monitor socket, e.g. add `-monitor tcp:127.0.0.1:55555,server,nowait -serial file:serial.log`. Since Phase 41 use `-vga none -device virtio-vga` for the GPU-accelerated display path (`-vga std` still works as the VESA fallback). Then `python qemu_drive.py` (needs PIL) sends monitor `sendkey`/`screendump`. Actions: `type:<text>`, `ret`, `key:<name>` (e.g. `key:esc`), `sleep:<s>`, `shot:<name>` (saves `<name>.png`). `NexusOS/qmon.py` sends arbitrary monitor commands (info registers, x/, quit).
- **Login:** real auth since Phase 44 (root/root, guest/guest). It boots splash → Aurora login → desktop; press **F12** (not Esc, since 2026-10-08) to reach the text shell, drawn as a centred Terminal window. See [[aurora-ui-architecture]].
- **Held keys (Phase 42 games):** QEMU monitor `sendkey` takes a hold-time, passed through qemu_drive.py as one token: `'key:w 2000'`. **Never overlap a long hold with more sendkeys** — delivery order garbles; serialize with `sleep:` ≥ the hold time between them.
- **Since Phase 42** the kernel links at 1 MB (`linker.ld` 0x100000) and boot2.asm chunk-copies it high via PM hops (ceiling raised to 1984 sectors = 1,015,808 bytes on 2026-10-08; was 720 KB, before that 576 KB). boot2's low scratch must stay in 0x500–0x7BFF (the VBE buffer at 0x8000 once corrupted the GDT).
- **Crash diagnosis:** unhandled CPU exceptions are written to `serial.log` as `!EX:<int_no> <err_code> <eip>` and drawn as a red BSOD bar. See [[intermittent-boot-doublefault]].
