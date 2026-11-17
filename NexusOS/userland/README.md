# NexusOS userland

These are the original i386 programs. From `NexusOS/` on Windows:

```powershell
.\build.bat apps
.\build.bat legacy
```

Log in as `root` / `root`. Spotlight (`Ctrl+Space`) launches Lua, Kilo, 2048,
Tetris and App Store. The GUI Terminal can run the same programs. `F12` opens
the kernel console, where `apps` lists installed programs.

```text
lua /disk/tests/check.lua
kilo /disk/notes.txt
sh
echo hello nexus | wc
echo saved > /disk/note.txt
cat /disk/note.txt
exit
```

Kilo: `Ctrl+S` saves, `Ctrl+Q` exits. Games: arrows/WASD, `q` exits; Tetris
also uses Space for a hard drop. `syscheck` runs the C runtime/process tests.
The shell provides quotes, pipes, `<`, `>`, `>>`, semicolons, `cd` and `exit`.
It does not implement expansion, scripting loops or job control. `grep` is a
literal substring filter. `cp`/`mv` refuse to overwrite an existing destination.

## Build

`build.py` uses the vendored `tools/i686-elf` compiler. On first use it creates
an isolated tool environment under `tools/userland-build`, installs pinned
Meson 1.12.1/Ninja 1.13.2, and downloads checksum-pinned Picolibc 1.8.12 from
its official release. Python 3.12+ and internet access are needed for that
first setup. Subsequent builds use the local cache.

[Picolibc](https://github.com/picolibc/picolibc/tree/1.8.12) supplies the C library,
maths and stdio. `libc/src/platform.c` connects its POSIX hooks to NexusOS.
`libc/user.ld` places static i386 ELFs at `0x08048000`; the kernel provides
private pages, a 256 KiB user stack and a per-process heap. Library thread-local
storage is disabled because each process currently has one thread.

Lua 5.4.7 and Kilo C sources are vendored unchanged. They are compiled, and their
build scripts are not run. Picolibc's license collection is in
`licenses/Picolibc.txt`; Lua's notice is in `vendor/lua-5.4.7/doc/readme.html`
and Kilo's BSD notice is at the beginning of `vendor/kilo.c`.

`fatput.py` updates a temporary copy of the disk, checks the unchanged tail,
then replaces the original. It formats only an entirely empty FAT window and
rejects corrupt nonempty volumes. It supports the legacy FAT window and the new
MBR FAT partition. Bytes at LBA 20480..32767 (NPFS and RAM snapshots) are preserved.
Long UTF-8 filenames are supported. Stop QEMU before installing into its disk image.

`build.bat apps` and `build.bat legacy` migrate legacy images to a 1 GiB disk while
preserving existing files. To grow it further, stop QEMU and run
`build.bat storage 8192` for an 8 GiB image. The current ATA driver addresses up to
128 GiB; FAT32 permits files up to 4 GiB minus one byte. Actual downloaded data
also needs sufficient free space on the host.

Browser downloads and `wget` stream to `/disk/Downloads`. Use Ctrl+S in the browser
to save a complete page/image even when its preview is limited. `wget URL
"/disk/Downloads/my file.zip"` chooses an explicit destination without overwriting
an existing file. Default filenames are made unique automatically.

To build/install on a separate test disk:

```powershell
python -I userland/build.py C:\nexustest\mytest\disk.img
```

## Tests

```powershell
python -I userland/tests/test_fatput.py
python -I userland/tests/test_vm.py
```

The VM test boots image copies under `C:\nexustest\dailyuse_runtime`, exercises
math/stdio/malloc/setjmp, file I/O, invalid user pointers, pipes, spawn/wait,
close-on-exec and concurrent x87 state; runs Lua and game assertions; saves a
file in Kilo; and checks exact FAT file contents after shutdown. Screenshots
and serial logs remain in the work directory. `!EX:` or `!KR:` fails the test.

These are programs compiled for NexusOS, not general Linux/Windows/macOS binary
compatibility. There is no compiler running inside the guest yet.
