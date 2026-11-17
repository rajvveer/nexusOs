"""Build the native x86-64 development target without altering i386 images.

Usage: python -I arch/x86_64/build.py [build|run|test|desktop|chrome|vmware] [--tcg] [--gpu] [--uefi] [--legacy-devices] [--hidden]
       python -I arch/x86_64/build.py usb-image OUT.img [--size 8G] [--root-from DISK.img] [--cmdline "..."]   (usbimage.py)
The pinned compiler is a build tool; no external OS code is linked into the image.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'out'
VERSION = '0.15.2'
NAME = 'zig-x86_64-windows-' + VERSION
SHA256 = '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c'
QEMU = Path(r'C:\Program Files\qemu\qemu-system-x86_64.exe')


def whpx_available():
    """Query Windows capabilities without creating a partition or starting QEMU."""
    if sys.platform != 'win32':
        return False
    import ctypes
    try:
        query = ctypes.WinDLL('WinHvPlatform.dll', winmode=0x800).WHvGetCapability
        query.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        query.restype = ctypes.c_int32

        def capability(code, kind):
            value, written = kind(), ctypes.c_uint32()
            size = ctypes.sizeof(value)
            if query(code, ctypes.byref(value), size, ctypes.byref(written)) != 0 or written.value != size:
                raise OSError('WHPX capability unavailable')
            return value.value

        if not capability(0, ctypes.c_uint32):
            return False
        if capability(0x1000, ctypes.c_uint32) not in (0, 1, 2):
            return False
        # RdRandSupport is bit 21 in Microsoft's WinHvPlatformDefs.h, not a CPUID bit.
        # https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvgetcapability
        return bool(capability(0x1001, ctypes.c_uint64) & (1 << 21))
    except (OSError, AttributeError):
        return False


def host_cpu_flags():
    # Expose the host's SIMD to the guest (Chrome/ffmpeg pick AVX2/SSE4 code paths at runtime);
    # qemu64 alone is SSE2-only. The guest kernel enables XSAVE/AVX state when CPUID offers it.
    try:
        import ctypes
        present = ctypes.windll.kernel32.IsProcessorFeaturePresent
    except (ImportError, AttributeError):
        return ''
    flags = ''
    if present(38):  # PF_SSE4_2_INSTRUCTIONS_AVAILABLE (implies SSSE3/SSE4.1)
        flags += ',+ssse3,+sse4.1,+sse4.2,+popcnt'
    if present(40):  # PF_AVX2_INSTRUCTIONS_AVAILABLE: every AVX2 CPU also has these
        flags += ',+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm'
    return flags


def desktop_cpu_count():
    # Half of the host's logical processors, at most 4: Windows and QEMU's own threads need the rest,
    # and 6 vCPUs measured no faster than 4 for Chrome (video, workers, startup).
    return max(1, min(4, (os.cpu_count() or 2) // 2))


def ovmf_unified():
    # WHPX cannot run OVMF from pflash, so the EDK2 build QEMU ships is loaded with -bios as one ROM:
    # the variable-store template followed by the code (the classic OVMF.fd layout).
    target = OUT / 'ovmf-unified.fd'
    if not target.is_file():
        share = Path(QEMU).parent / 'share'
        target.write_bytes((share / 'edk2-i386-vars.fd').read_bytes() + (share / 'edk2-x86_64-code.fd').read_bytes())
    return target


def desktop_machine_options(disk, gpu=False, uefi=False, legacy=False):
    # Default: a modern PC (q35, NVMe root, e1000e, USB keyboard + tablet on xHCI). legacy: pc, IDE, RTL8139, PS/2.
    video = ['-vga', 'none', '-device', ('virtio-vga-gl' if gpu else 'virtio-vga') + ',id=video0', '-display', 'sdl,gl=on' if gpu else 'sdl']
    if uefi:
        boot = ['-bios', ovmf_unified(), '-drive', f'if=none,id=esp,format=raw,snapshot=on,file={OUT / "nexus64-uefi.img"}',
                '-device', 'ide-hd,drive=esp,bus=ide.1,unit=0,bootindex=0' if legacy else 'ide-hd,drive=esp,bus=ide.2,bootindex=0']
    else:
        boot = ['-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a']
    if legacy:
        return ['-machine', 'pc', *boot, '-drive', f'file={disk},format=raw,if=ide' + (',index=0' if uefi else ''), *video,
                '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network']
    return ['-machine', 'q35', *boot, '-drive', f'if=none,id=root,file={disk},format=raw', '-device', 'nvme,serial=nexus-root,drive=root',
            *video, '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network',
            '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,display=video0', '-device', 'usb-tablet,bus=xhci.0,display=video0']


def unthrottle(pid):
    # Windows 11 drops a process's 1 ms timer resolution (QEMU asks for it) and may put it on
    # efficiency cores while its window is minimized or covered; guest timers then tick at 15.6 ms.
    # Opt the VM process out of both policies (PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
    # PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, state 0 = never throttle).
    if sys.platform != 'win32':
        return False
    try:
        import ctypes
        from ctypes import wintypes
        class State(ctypes.Structure):
            _fields_ = [('Version', wintypes.ULONG), ('ControlMask', wintypes.ULONG), ('StateMask', wintypes.ULONG)]
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        handle = kernel32.OpenProcess(0x0200, False, pid)  # PROCESS_SET_INFORMATION
        if not handle:
            return False
        try:
            state = State(1, 0x1 | 0x4, 0)
            return bool(kernel32.SetProcessInformation(wintypes.HANDLE(handle), 4, ctypes.byref(state), ctypes.sizeof(state)))
        finally:
            kernel32.CloseHandle(wintypes.HANDLE(handle))
    except (ImportError, OSError, AttributeError, TypeError):
        return False


def hidden_startup():
    """QEMU's SDL window starts hidden; reveal_window() shows it once the guest has activated the USB tablet.

    QEMU 9.1's SDL UI runs its mouse-mode notifier on every input-handler activation (ui/input.c notifies without
    checking for a change), and the first one after display init is the guest's first poll of the USB tablet, i.e. it
    runs in a vCPU thread that holds the big QEMU lock. sdl_mouse_mode_change() then calls absolute_mouse_grab(): with
    the host pointer inside the window (it is screen-sized) and the window focused, sdl_grab_start() sets the window
    title, a SendMessage to the window's thread, which is waiting for that lock: the VM froze at 'USB xhci0 port 6 ...
    slot 2: HID device' with the window 'Not Responding'. A hidden window has neither pointer nor focus, so that first
    activation passes, and later ones are no-ops (absolute_enabled stays set). The first ShowWindow() of a process
    takes the show state from STARTUPINFO, so the window SDL creates stays hidden until reveal_window()."""
    if sys.platform != 'win32' or not hasattr(subprocess, 'STARTUPINFO'):
        return None
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0  # SW_HIDE
    return startup


# serial-log lines that follow the tablet's activation (the first interrupt transfer is queued before the kernel logs it)
REVEAL_MARKERS = ('absolute pointer ready', 'USB boot enumeration complete', 'LOADING /sbin/init', 'AURORA READY')


def reveal_window(vm, serial, timeout=15.0):
    """Show the hidden QEMU window and bring it to the front once the serial log reached a REVEAL_MARKERS line
    (or after `timeout`, so a failed boot is still visible). Returns whether a window was shown."""
    if sys.platform != 'win32' or not isinstance(vm.pid, int):
        return False
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    deadline = time.time() + timeout
    while time.time() < deadline and vm.poll() is None:
        try:
            text = Path(serial).read_text(errors='replace')
        except OSError:
            text = ''
        if any(marker in text for marker in REVEAL_MARKERS):
            break
        time.sleep(0.1)
    windows = []

    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == vm.pid:
            title = ctypes.create_unicode_buffer(64)
            user32.GetWindowTextW(hwnd, title, 64)
            if title.value.startswith('QEMU') and '-0)' in title.value:  # console 0 only: the monitor's window (-1) stays hidden
                windows.append(hwnd)
        return True

    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(visit)
    deadline = time.time() + 5
    while not windows and time.time() < deadline and vm.poll() is None:
        user32.EnumWindows(callback, 0)
        if not windows:
            time.sleep(0.1)
    for hwnd in windows:
        user32.ShowWindow(hwnd, 5)  # SW_SHOW
    if windows:
        user32.SetForegroundWindow(windows[0])
    return bool(windows)


def whpx_security_flags():
    """SMEP and SMAP for the guest when the hypervisor offers them (WHV_PROCESSOR_FEATURES SmepSupport bit 23,
    SmapSupport bit 35): the kernel runs with SMAP on by default (uaccess.h), and qemu64 alone lists neither."""
    if sys.platform != 'win32':
        return ''
    try:
        import ctypes
        query = ctypes.WinDLL('WinHvPlatform.dll', winmode=0x800).WHvGetCapability
        query.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        query.restype = ctypes.c_int32
        value, written = ctypes.c_uint64(), ctypes.c_uint32()
        if query(0x1001, ctypes.byref(value), 8, ctypes.byref(written)) != 0 or written.value != 8:
            return ''
        features = value.value
        return (',+smep' if features >> 23 & 1 else '') + (',+smap' if features >> 35 & 1 else '')
    except (OSError, AttributeError, TypeError, ValueError):
        return ''


def desktop_cpu_options(force_tcg=False):
    if not force_tcg and whpx_available():
        return ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand' + host_cpu_flags() + whpx_security_flags()]
    return ['-cpu', 'max']


def compiler():
    executable = ROOT / 'tools' / NAME / 'zig.exe'
    if not executable.exists():
        archive = ROOT / 'tools' / (NAME + '.zip')
        if not archive.exists():
            temporary = archive.with_suffix('.part')
            url = f'https://ziglang.org/download/{VERSION}/{NAME}.zip'
            print('Downloading pinned compiler:', url, flush=True)
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open('wb') as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            temporary.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError('Compiler archive checksum mismatch: ' + str(archive))
        with zipfile.ZipFile(archive) as package:
            package.extractall(ROOT / 'tools')
    return executable


def run(*args, cwd=ROOT):
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True)


FLOPPY_BYTES = 2949120          # x86-64 BIOS boot image: 2.88 MB floppy (80 x 2 x 36 sectors of 512 bytes)
KERNEL_MAX = FLOPPY_BYTES - 5 * 512   # boot sector + 4 stage-2 sectors first: 2,946,560 bytes of kernel
KERNEL_VMA = 0xFFFFFFFF80000000  # the kernel runs in the top 2 GiB, linked at KERNEL_VMA + physical (linker.ld)


def flat_kernel(elf):
    """Preserve PT_LOAD physical addresses; raw section packing can silently move the GDT."""
    if len(elf) < 64 or elf[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Expected little-endian ELF64')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', elf)
    _, kind, machine, version, entry, phoff, _, _, ehsize, phsize, count, *_ = header
    if (kind, machine, version, entry, ehsize, phsize) != (2, 62, 1, KERNEL_VMA + 0x100000, 64, 56):
        raise ValueError('Unexpected kernel ELF header')
    if not count or phoff > len(elf) or count > (len(elf) - phoff) // phsize:
        raise ValueError('Invalid ELF program header range')
    image = bytearray()
    ranges = []
    for i in range(count):
        kind, _, offset, va, pa, filesz, memsz, _ = struct.unpack_from('<II6Q', elf, phoff + i * phsize)
        if kind != 1:
            continue
        if filesz > memsz or offset > len(elf) or filesz > len(elf) - offset:
            raise ValueError('Truncated kernel segment')
        if va != pa + KERNEL_VMA or pa < 0x100000 or pa + memsz > 0x600000:   # image + BSS within 6 MiB (linker.ld)
            raise ValueError('Kernel segment outside bootstrap mapping')
        if not filesz:
            continue
        begin, end = pa - 0x100000, pa - 0x100000 + filesz
        if end > KERNEL_MAX or any(begin < b and end > a for a, b in ranges):
            raise ValueError('Overlapping/oversized kernel segments')
        ranges.append((begin, end))
        if end > len(image):
            image.extend(bytes(end - len(image)))
        image[begin:end] = elf[offset:offset + filesz]
    if not ranges or min(a for a, _ in ranges) != 0:
        raise ValueError('Missing kernel entry segment')
    return bytes(image)


def desktop_audio_options():
    """AC'97 (native kernel driver, /dev/snd) on QEMU's Windows DirectSound backend at 48 kHz: 5 ms audio timer and a
    40 ms host buffer measured ~15 ms ahead of real time; smaller DirectSound buffers stalled the stream.
    NEXUS_AUDIO=hda swaps in Intel HD Audio (ich9-intel-hda + hda-duplex, native hda.c driver) instead, with the codec's
    use-timer=false: QEMU's timer mode drops its whole 8 KiB stream buffer (2048 frames, an audible click) about 1.3 s
    after a stream starts on Windows hosts (traced hda_audio_overrun; see tests/test_x64_hda.py).
    Measured with tests/measure_audio_drops.py (20 s tone, wav backend with these timer/buffer values, 2 busy host
    processes, 2026-10-30): HDA timer mode had the 2048-frame drop in 8/8 runs throttled and 2/6 unthrottled; use-timer=false
    and AC97 0/7 each; timer-period 10 ms / buffer 80 ms changed nothing but latency. Multi-second stalls (every setting,
    ~1 run in 4) appeared only with Windows power throttling on, which the launcher turns off (unthrottle)."""
    if os.environ.get('NEXUS_AUDIO') == 'hda':
        return ['-audiodev', 'dsound,id=snd0,timer-period=5000,out.buffer-length=40000,out.frequency=48000',
                '-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0,use-timer=false']
    return ['-audiodev', 'dsound,id=snd0,timer-period=5000,out.buffer-length=40000,out.frequency=48000', '-device', 'AC97,audiodev=snd0']


def build():
    OUT.mkdir(exist_ok=True)
    zig = compiler()
    nasm = ROOT / 'tools/nasm-2.16.01/nasm.exe'
    flags = ['cc', '-target', 'x86_64-freestanding-none', '-std=c11', '-O2', '-DNEXUS_NATIVE64=1',
             '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic',
             '-fno-delete-null-pointer-checks', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
             '-mno-red-zone', '-mno-sse', '-mno-sse2', '-mno-mmx', '-msoft-float',
             '-Wall', '-Wextra', '-Werror']
    run(nasm, '-DNEXUS_X86_64=1', '-f', 'bin', ROOT / 'boot/boot.asm', '-o', OUT / 'boot.bin')
    run(nasm, '-DNEXUS_X86_64=1', '-f', 'bin', ROOT / 'boot/boot2.asm', '-o', OUT / 'boot2.bin')  # x86-64: EDID-native VBE mode
    run(nasm, '-f', 'bin', HERE / 'usbboot.asm', '-o', OUT / 'usbboot.bin')   # GPT disk boot code (usb-image)
    run(nasm, '-f', 'elf64', HERE / 'user_entry.asm', '-o', OUT / 'user_entry.o')
    run(zig, *flags, '-mcmodel=large', '-c', HERE / 'user.c', '-o', OUT / 'user.o')
    link = ['cc', '-target', 'x86_64-freestanding-none', '-nostdlib', '-static', '-Wl,--build-id=none']
    run(zig, *link, '-Wl,-T,' + str(HERE / 'user.ld'), OUT / 'user_entry.o', OUT / 'user.o', '-o', OUT / 'user.elf')
    run(zig, *flags, '-mcmodel=large', '-DDISK_PROGRAM=1', '-c', HERE / 'user.c', '-o', OUT / 'disk_user.o')
    run(zig, *link, '-Wl,-T,' + str(HERE / 'user.ld'), OUT / 'user_entry.o', OUT / 'disk_user.o', '-o', OUT / 'disk_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'dynamic.c', '-o', OUT / 'dynamic.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'signals.c', '-o', OUT / 'signals.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'ipc_user.c', '-o', OUT / 'ipc_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'process_user.c', '-o', OUT / 'process_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'threads_user.c', '-o', OUT / 'threads_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'vmstress_user.c', '-o', OUT / 'vmstress_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'pipes_user.c', '-o', OUT / 'pipes_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'events_user.c', '-o', OUT / 'events_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'namespaces_user.c', '-o', OUT / 'namespaces_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'unix_user.c', '-o', OUT / 'unix_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'pid_user.c', '-o', OUT / 'pid_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'fs_user.c', '-o', OUT / 'fs_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'shmem_user.c', '-o', OUT / 'shmem_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'limits_user.c', '-o', OUT / 'limits_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'filemap_user.c', '-o', OUT / 'filemap_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'jit_user.c', '-o', OUT / 'jit_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'locks_user.c', '-o', OUT / 'locks_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'statfs_user.c', '-o', OUT / 'statfs_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'rename_user.c', '-o', OUT / 'rename_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'display_user.c', '-o', OUT / 'display_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'gpu_user.c', '-o', OUT / 'gpu_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'seccomp_user.c', '-o', OUT / 'seccomp_user.elf')  # seccomp conformance (tests/test_x64_seccomp.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'aslr_user.c', '-o', OUT / 'aslr_user.elf')  # ASLR conformance (tests/test_x64_aslr.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'umip_user.c', '-o', OUT / 'umip_user.elf')  # UMIP emulation (tests/test_x64_umip.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'nsfs_user.c', '-o', OUT / 'nsfs_user.elf')  # nsfs magic links + setns (tests/test_x64_nsfs.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'kabi_user.c', '-o', OUT / 'kabi_user.elf')  # Linux ABI conformance: timers, signalfd, pidfd, clone3 (tests/test_x64_kabi.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'kabi2_user.c', '-o', OUT / 'kabi2_user.elf')  # SysV IPC, Landlock, mount API, fdinfo, RLIMIT_SIGPENDING (tests/test_x64_kabi.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'kguard_user.c', '-o', OUT / 'kguard_user.elf')  # kguard oops containment (tests/test_x64_kguard.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'secfs_user.c', '-o', OUT / 'secfs_user.elf')  # securityfs lockdown (tests/test_x64_lockdown.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'kptr_user.c', '-o', OUT / 'kptr_user.elf')  # kptr/kallsyms hygiene (tests/test_x64_kptr.py)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'kms_user.c', '-o', OUT / 'kms_user.elf')  # KMS campaign (tests/test_x64_kms.py)
    for source in ('pty', 'inotify', 'sysext', 'apps_postinst'):  # apps campaign (/nexus/test-apps); apps_postinst.py helper
        run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
            HERE / (source + '_user.c'), '-o', OUT / (source + '_user.elf'))
    for program in ('posixfs', 'crash'):  # ext4 root campaigns (tests/test_x64_ext4*.py)
        run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
            HERE / (program + '_user.c'), '-o', OUT / (program + '_user.elf'))
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        HERE / 'svc_test_user.c', '-o', OUT / 'svc_test_user.elf')   # tests/test_x64_services.py helper
    for program in ('nexusd', 'nexusctl'):  # service manager + journal, its CLI (sysinstall.py installs them)
        run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
            HERE / (program + '.c'), '-o', OUT / (program + '.elf'))
    services_sdk = OUT / 'services-sdk'   # libdbus headers: debian_base.py sdk (pinned, signature-verified libdbus-1-dev)
    if not (services_sdk / 'usr/include/dbus-1.0/dbus/dbus.h').exists():
        run(sys.executable, '-I', HERE / 'debian_base.py', 'sdk', services_sdk)
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        '-I', services_sdk / 'usr/include/dbus-1.0', '-I', services_sdk / 'usr/lib/x86_64-linux-gnu/dbus-1.0/include',
        HERE / 'nexus_notifyd.c', OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libdbus-1.so.3.38.3', '-o', OUT / 'nexus_notifyd.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        '-I', services_sdk / 'usr/include/dbus-1.0', '-I', services_sdk / 'usr/lib/x86_64-linux-gnu/dbus-1.0/include',
        HERE / 'notify_test_user.c', OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libdbus-1.so.3.38.3', '-o', OUT / 'notify_test_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # UPower + power profiles (system bus)
        '-I', services_sdk / 'usr/include/dbus-1.0', '-I', services_sdk / 'usr/lib/x86_64-linux-gnu/dbus-1.0/include',
        HERE / 'nexus_upowerd.c', OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libdbus-1.so.3.38.3', '-o', OUT / 'nexus_upowerd.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # its API client (tests/test_x64_upower.py)
        '-I', services_sdk / 'usr/include/dbus-1.0', '-I', services_sdk / 'usr/lib/x86_64-linux-gnu/dbus-1.0/include',
        HERE / 'upower_test_user.c', OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libdbus-1.so.3.38.3', '-o', OUT / 'upower_test_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
        HERE / 'tickless_user.c', '-o', OUT / 'tickless_user.elf')  # tickless idle campaign (tests/test_x64_tickless.py)
    for program in ('nexus_logind', 'logind_test_user', 'nexus_portal', 'portal_test_user', 'aurora_a11y_bridge'):  # login1, the desktop portal backend (sysinstall.py) + test clients
        run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
            '-I', services_sdk / 'usr/include/dbus-1.0', '-I', services_sdk / 'usr/lib/x86_64-linux-gnu/dbus-1.0/include',
            HERE / (program + '.c'), OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libdbus-1.so.3.38.3', '-o', OUT / (program + '.elf'))
    # User space from 64 KiB (tests/test_x64_memlayout.py): PIE checks, glibc ET_EXEC at 0x400000, static ET_EXEC at 0x10000.
    memlayout = HERE / 'memlayout_user.c'
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror', memlayout, '-o', OUT / 'memlayout_user.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fno-pie', '-no-pie', '-Wl,--image-base=0x400000', '-DMEMLAYOUT_EXEC=1',
        '-Wall', '-Wextra', '-Werror', memlayout, '-o', OUT / 'memlayout_exec.elf')
    run(zig, 'cc', '-target', 'x86_64-freestanding-none', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pie',
        '-nostdlib', '-static', '-DMEMLAYOUT_LOW=1', '-Wall', '-Wextra', '-Werror', '-Wl,--image-base=0x10000', '-Wl,-e,_start',
        '-Wl,--build-id=none', memlayout, '-o', OUT / 'memlayout_low.elf')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-fno-stack-protector', '-Wall', '-Wextra', '-Werror',
        HERE / 'wow32_user.c', '-o', OUT / 'wow32_user.elf')  # WoW64 CPU modes (tests/test_x64_wow32.py)
    spec = importlib.util.spec_from_file_location('nexus_runtime', HERE / 'runtime.py')
    runtime = importlib.util.module_from_spec(spec); spec.loader.exec_module(runtime)
    runtime.prepare(OUT / 'runtime')
    run(zig, 'cc', '-target', 'x86_64-linux-gnu', '-O2', '-fPIC', '-shared', '-nostdlib', '-fno-stack-protector',
        '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-mno-red-zone', '-Wall', '-Wextra', '-Werror',
        '-Wl,--version-script=' + str(HERE / 'vdso.lds'), '-Wl,-T,' + str(HERE / 'vdso_layout.lds'), '-Wl,-soname,linux-vdso.so.1', '-Wl,--hash-style=both',
        '-Wl,-z,norelro', '-Wl,--build-id=none', HERE / 'vdso.c', '-o', OUT / 'vdso.so')
    run(nasm, '-f', 'elf64', HERE / 'entry.asm', '-o', OUT / 'entry.o', cwd=OUT)
    # kernel objects from here on: higher-half image (KERNEL_VMA), with a stack-protector canary (hardening.c holds
    # __stack_chk_guard/__stack_chk_fail; -mstack-protector-guard=global reads the symbol, not %fs which ring 0 lacks).
    flags = [*flags, '-mcmodel=kernel', '-fstack-protector-strong', '-mstack-protector-guard=global']
    run(zig, *flags, '-c', HERE / 'kernel.c', '-o', OUT / 'kernel.o')
    run(zig, *flags, '-c', HERE / 'signal.c', '-o', OUT / 'signal.o')
    run(zig, *flags, '-c', HERE / 'memory.c', '-o', OUT / 'memory.o')
    run(zig, *flags, '-c', HERE / 'process.c', '-o', OUT / 'process.o')
    run(zig, *flags, '-c', HERE / 'smp.c', '-o', OUT / 'smp.o')
    run(zig, *flags, '-c', HERE / 'segments.c', '-o', OUT / 'segments.o')  # GDT/LDT/TLS, compatibility mode (WoW64)
    run(zig, *flags, '-c', HERE / 'pid.c', '-o', OUT / 'pid.o')
    run(zig, *flags, '-c', HERE / 'shmem.c', '-o', OUT / 'shmem.o')
    run(zig, *flags, '-c', HERE / 'locks.c', '-o', OUT / 'locks.o')
    run(zig, *flags, '-c', HERE / 'inode.c', '-o', OUT / 'inode.o')
    for source in ('vfs', 'ext4', 'ext4_journal', 'ext4_dev'):  # root filesystem dispatch + ext4 driver + block seam
        run(zig, *flags, '-Os', '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o'))
    run(zig, *flags, '-c', HERE / 'pipe.c', '-o', OUT / 'pipe.o')
    run(zig, *flags, '-c', HERE / 'event.c', '-o', OUT / 'event.o')
    run(zig, *flags, '-c', HERE / 'credentials.c', '-o', OUT / 'credentials.o')
    run(zig, *flags, '-c', HERE / 'seccomp.c', '-o', OUT / 'seccomp.o')
    run(zig, *flags, '-c', HERE / 'hardening.c', '-o', OUT / 'hardening.o')
    run(zig, *flags, '-c', HERE / 'uaccess.c', '-o', OUT / 'uaccess.o')   # SMAP user copies (uaccess.h) + their fault fixups
    run(nasm, '-f', 'elf64', HERE / 'uaccess.asm', '-o', OUT / 'uaccess_asm.o')
    run(zig, *flags, '-c', HERE / 'umip.c', '-o', OUT / 'umip.o')
    run(zig, *flags, '-c', HERE / 'lockdown.c', '-o', OUT / 'lockdown.o')   # kernel lockdown (kernel_lockdown_level for power2)
    run(zig, *flags, '-fno-optimize-sibling-calls', '-c', HERE / 'kguard.c', '-o', OUT / 'kguard.o')   # kguard oops-containment test triggers
    run(zig, *flags, '-c', HERE / 'namespaces.c', '-o', OUT / 'namespaces.o')  # net/IPC namespaces, setns
    run(zig, *flags, '-c', HERE / 'proc.c', '-o', OUT / 'proc.o')
    run(zig, *flags, '-c', HERE / 'device.c', '-o', OUT / 'device.o')
    run(zig, *flags, '-c', HERE / 'input.c', '-o', OUT / 'input.o')
    run(zig, *flags, *[f'-Dnative_nic_{name}=rtl8139_nic_{name}' for name in ('init', 'poll', 'irq', 'rx_pending', 'rx_mode')], '-c', HERE / 'rtl8139.c', '-o', OUT / 'rtl8139-native.o')
    usbnet = [OUT / (name + '.o') for name in ('pcidev', 'nic', 'e1000', 'xhci', 'usb', 'usbhub', 'usbhid', 'netdrv', 'rtl8169', 'hwinfo', 'pcifix', 'diag', 'diagfat', 'hwnode', 'usbclass', 'uaudio', 'uvc', 'xpad', 'v4l2', 'vcam', 'hwsysfs', 'evdev', 'uinput', 'gamepad', 'vpad', 'apic', 'bluetooth', 'bt_crypto', 'bt_alg', 'bt_mgmt', 'bt_l2cap', 'bt_smp', 'bt_rfcomm', 'btusb')]  # PCI helpers, NIC selection, Intel/Realtek NICs, USB
    for obj in usbnet: run(zig, *flags, '-c', HERE / (obj.stem + '.c'), '-o', obj)
    run(zig, *flags, '-c', HERE / 'sound.c', '-o', OUT / 'sound.o')
    for source in ('virtio_gpu', 'drm', 'sysfs', 'kms'): run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o'))
    for source in ('pty', 'inotify', 'fsext', 'shmfs', 'procext', 'fifo'): run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o'))
    run(zig, *flags, '-c', HERE / 'network.c', '-o', OUT / 'network.o')
    run(zig, *flags, '-c', HERE / 'dhcp64.c', '-o', OUT / 'dhcp64.o')  # DHCP client
    run(zig, *flags, '-Wframe-larger-than=32768', '-c', HERE / 'inet.c', '-o', OUT / 'inet.o')
    run(zig, *flags, '-Wframe-larger-than=32768', '-c', HERE / 'ipc.c', '-o', OUT / 'ipc.o')
    drivers = [OUT / 'signal.o', OUT / 'memory.o', OUT / 'process.o', OUT / 'smp.o', OUT / 'segments.o', OUT / 'pid.o', OUT / 'shmem.o', OUT / 'locks.o', OUT / 'inode.o', OUT / 'pipe.o', OUT / 'event.o', OUT / 'credentials.o', OUT / 'seccomp.o', OUT / 'hardening.o', OUT / 'uaccess.o', OUT / 'uaccess_asm.o', OUT / 'umip.o', OUT / 'lockdown.o', OUT / 'kguard.o', OUT / 'namespaces.o', OUT / 'proc.o', OUT / 'device.o', OUT / 'input.o', OUT / 'ipc.o']
    drivers += [OUT / 'rtl8139-native.o', OUT / 'network.o', OUT / 'dhcp64.o', OUT / 'inet.o', OUT / 'sound.o', *usbnet]
    for source in ('ac97', 'hda'):  # PCM engines behind sound.c's ALSA layer (sound_hw.h)
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    for source in ('boot_info', 'acpi', 'irq', 'platform_test'):  # platform: boot hand-off, ACPI, IOAPIC/MSI
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o'))
        drivers.append(OUT / (source + '.o'))
    # ACPI interpreter: uACPI 6.1.1 (MIT, uacpi/LICENSE, pinned in power-lock.json) built -Os, and the power-management layer
    # on it (power.h: acpi_os = uACPI kernel API, acpi_aml = namespace + ACPI devices, power = policy/sysfs, pm = sleep states).
    uacpi = ['-DUACPI_OVERRIDE_TYPES', '-DUACPI_OVERRIDE_LIBC', '-DUACPI_OVERRIDE_ARCH_HELPERS', '-DUACPI_SIZED_FREES',
             '-DUACPI_NATIVE_ALLOC_ZEROED', '-I', HERE / 'uacpi_port', '-I', HERE / 'uacpi/include']
    for source in sorted((HERE / 'uacpi/source').glob('*.c')):
        run(zig, *flags, '-Os', *uacpi, '-c', source, '-o', OUT / ('uacpi_' + source.stem + '.o')); drivers.append(OUT / ('uacpi_' + source.stem + '.o'))
    for source in ('acpi_os', 'acpi_aml', 'acpi_ec', 'power', 'pm', 'hibernate', 'cpufreq', 'tick'):  # tick: tickless idle
        run(zig, *flags, *uacpi, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    run(nasm, '-f', 'elf64', HERE / 'pm_asm.asm', '-o', OUT / 'pm_asm.o'); drivers.append(OUT / 'pm_asm.o')  # S3 wake trampoline
    for source in ('blockdev', 'partition', 'ahci', 'nvme'):  # block layer + SATA/NVMe drivers
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    drivers += [OUT / 'virtio_gpu.o', OUT / 'drm.o', OUT / 'sysfs.o', OUT / 'kms.o']
    drivers += [OUT / 'pty.o', OUT / 'inotify.o', OUT / 'fsext.o', OUT / 'shmfs.o', OUT / 'procext.o', OUT / 'fifo.o']
    drivers += [OUT / 'vfs.o', OUT / 'ext4.o', OUT / 'ext4_journal.o', OUT / 'ext4_dev.o']
    for source in ('storage_alloc', 'scsi', 'usbstorage', 'blkdev_node', 'mount', 'fatfs', 'fsprobe', 'console_prompt', 'osinstall', 'loop', 'iso9660', 'exfat', 'fuse', 'keyring', 'aes', 'sha256', 'argon2', 'luks', 'cryptroot', 'crypttab', 'keymaps', 'lvm_boot', 'md', 'dm'):  # storage: SCSI disks over USB Bulk-Only/UAS
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    run(nasm, '-f', 'elf64', HERE / 'aes_ni.asm', '-o', OUT / 'aes_ni.o'); drivers.append(OUT / 'aes_ni.o')  # dm-crypt AES-NI
    for source in ('swap', 'reclaim', 'blockio'):  # swap areas and cache, page reclaim, kswapd, OOM killer (swap.h); disk waits without the BKL
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    run(zig, *flags, '-c', HERE / 'netlink.c', '-o', OUT / 'netlink.o')  # AF_NETLINK, SIOCGIF* ioctls, /proc/net (Wine nsiproxy)
    drivers.append(OUT / 'netlink.o')
    for source in ('netmem', 'netdev', 'route', 'net4', 'ipv6', 'icmp6', 'ndisc', 'dhcp6', 'netproc', 'netprocnet', 'netioctl', 'wgcrypto', 'packet', 'conntrack', 'nft', 'tun', 'wireguard', 'mcast', 'forward', 'wifi', 'nl80211'):  # native network core (netdev.h)
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    for source in ('kmsg', 'timekeeping', 'uts', 'bootslot'):  # /dev/kmsg + syslog(2); settable/slewed clock; host name; A/B slot init
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    for source in ('kslab', 'posix_timers', 'signalfd', 'pidfd', 'kabi', 'fdinfo', 'sysvipc', 'landlock', 'mountapi'):  # Linux ABI (kabi): slab, POSIX timers + timerfd, signalfd, pidfd + clone3, quotactl
        run(zig, *flags, '-c', HERE / (source + '.c'), '-o', OUT / (source + '.o')); drivers.append(OUT / (source + '.o'))
    run(zig, *flags, '-c', HERE / 'seat.c', '-o', OUT / 'seat.o'); drivers.append(OUT / 'seat.o')  # the seat's console user (device access)
    for source in ('ata', 'fat32', 'string', 'rtc', 'pci', 'net', 'arp', 'ip', 'icmp', 'udp', 'tcp', 'firewall'):
        obj = OUT / (source + '.o')
        run(zig, *flags, '-c', ROOT / 'kernel' / (source + '.c'), '-o', obj)
        drivers.append(obj)
    link_objs = [OUT / 'entry.o', OUT / 'kernel.o', *drivers]
    # Pass 1: link without the /proc/kallsyms table (kernel.c/procext.c reference it through weak symbols, so this
    # link resolves them to 0). gen_kallsyms.py then builds out/kallsyms.bin from this ELF's symbol table.
    run(zig, *link, '-Wl,-e,start32', '-Wl,-T,' + str(HERE / 'linker.ld'),
        *link_objs, '-o', OUT / 'kernel.elf')
    run(sys.executable, '-I', HERE / 'gen_kallsyms.py', OUT / 'kernel.elf', OUT / 'kallsyms.bin')
    run(nasm, '-f', 'elf64', HERE / 'kallsyms_blob.asm', '-o', OUT / 'kallsyms_blob.o', cwd=OUT)
    # Pass 2 (final): link with the table in its .kallsyms section (after .data, so no text/rodata/data symbol moves).
    link_objs = [*link_objs, OUT / 'kallsyms_blob.o']
    run(zig, *link, '-Wl,-e,start32', '-Wl,-T,' + str(HERE / 'linker.ld'),
        *link_objs, '-o', OUT / 'kernel.elf')
    (OUT / 'kernel.bin').write_bytes(flat_kernel((OUT / 'kernel.elf').read_bytes()))
    # KASLR (tests/test_x64_kaslr.py, uefi/kaslr.py): record the exact link so the kernel can be re-linked at a
    # different virtual base, and emit a reloc-bearing ELF with `ld.lld --emit-relocs` (zig cc drops the relocs).
    import json as _json
    (OUT / 'kernel-link.json').write_text(_json.dumps({
        'zig': str(zig), 'script': str(HERE / 'linker.ld'), 'objs': [str(o) for o in link_objs]}))
    run(zig, 'ld.lld', '--emit-relocs', '-T', str(HERE / 'linker.ld'), '-e', 'start32', '--build-id=none',
        '-static', '-o', OUT / 'kernel-relocs.elf', *link_objs)
    boot = (OUT / 'boot.bin').read_bytes()
    stage2 = (OUT / 'boot2.bin').read_bytes()
    kernel = (OUT / 'kernel.bin').read_bytes()
    assert len(boot) == 512 and boot[-2:] == b'\x55\xaa'
    assert len(stage2) == 2048 and 0 < len(kernel) <= KERNEL_MAX, ('kernel.bin exceeds the 2.88 MB boot floppy', len(kernel))
    # stage 2 reads exactly the kernel's sectors (boot2.asm kernel_sectors at 0x7F4; its default is the whole area)
    assert struct.unpack_from('<H', stage2, 0x7F4)[0] == KERNEL_MAX // 512, 'boot2.bin has no kernel_sectors field'
    stage2 = stage2[:0x7F4] + struct.pack('<H', (len(kernel) + 511) // 512) + stage2[0x7F6:]
    (OUT / 'boot2.bin').write_bytes(stage2)
    image = boot + stage2 + kernel
    (OUT / 'nexus64.img').write_bytes(image.ljust(FLOPPY_BYTES, b'\0'))   # 2.88 MB: 80 x 2 x 36 sectors
    # UEFI: BOOTX64.EFI (PE32+ EFI application) + GPT/ESP disk image + El Torito EFI entry in the ISO.
    run(zig, 'cc', '-target', 'x86_64-uefi-msvc', '-std=c11', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-mno-stack-arg-probe', '-mno-red-zone', '-mno-sse', '-mno-mmx', '-msoft-float', '-nostdlib', '-g0',
        '-Wall', '-Wextra', '-Werror', HERE / 'uefi/bootx64.c', '-o', OUT / 'BOOTX64.EFI')
    run(sys.executable, '-I', ROOT / 'mkuefi.py', OUT / 'BOOTX64.EFI', OUT / 'kernel.bin', OUT / 'nexus64-uefi.img',
        '--efiboot', OUT / 'efiboot.img')
    run(sys.executable, '-I', ROOT / 'mkiso.py', OUT / 'nexus64.img', OUT / 'nexus64.iso', OUT / 'efiboot.img')
    disk = OUT / 'apps64.img'
    if not disk.exists():
        with disk.open('xb') as image:
            image.truncate(16 * 1024 * 1024)
        run(sys.executable, '-I', ROOT / 'userland/fatput.py', disk, 'mkdir', 'tmp')   # format the new volume
        run(sys.executable, '-I', ROOT / 'storage.py', '64', disk)                     # grow before installing
    (OUT / 'truncated.elf').write_bytes((OUT / 'disk_user.elf').read_bytes()[:200])
    (OUT / 'native64.dat').write_bytes(bytes((i * 37 + 11) & 255 for i in range(10003)))
    install = [sys.executable, '-I', ROOT / 'userland/fatput.py', disk,
        *[item for name in ('resolv.conf', 'nsswitch.conf', 'hosts') for item in ('put', HERE / 'etc' / name, 'etc/' + name)],
        'mkdir', 'home/nexus', 'mkdir', 'tmp', 'mkdir', 'run/user/1000',
        'put', OUT / 'disk_user.elf', 'bin/selftest64', 'put', OUT / 'truncated.elf', 'bin/truncated64',
        'put', OUT / 'pid_user.elf', 'bin/pid64',
        'put', OUT / 'fs_user.elf', 'bin/fs64',
        'put', OUT / 'shmem_user.elf', 'bin/shmem64',
        'put', OUT / 'limits_user.elf', 'bin/limits64',
        'put', OUT / 'filemap_user.elf', 'bin/filemap64',
        'put', OUT / 'jit_user.elf', 'bin/jit64',
        'put', OUT / 'locks_user.elf', 'bin/locks64',
        'put', OUT / 'statfs_user.elf', 'bin/statfs64',
        'put', OUT / 'rename_user.elf', 'bin/rename64',
        'put', OUT / 'display_user.elf', 'bin/display64',
        'put', OUT / 'gpu_user.elf', 'bin/gpu64', 'put', OUT / 'kms_user.elf', 'bin/kms64',
        'put', OUT / 'pty_user.elf', 'bin/pty64', 'put', OUT / 'inotify_user.elf', 'bin/inotify64', 'put', OUT / 'sysext_user.elf', 'bin/sysext64',
        'put', OUT / 'posixfs_user.elf', 'bin/posix64', 'put', OUT / 'posixfs_user.elf', 'bin/posix64-root',
        'put', OUT / 'crash_user.elf', 'bin/crash64',
        'put', OUT / 'fs_user.elf', 'nexus/fs-root/bin/fs64',
        'put', OUT / 'native64.dat', 'nexus/fs-root/token',
        'put', OUT / 'runtime/ld-linux-x86-64.so.2', 'nexus/fs-root/lib64/ld-linux-x86-64.so.2',
        'put', OUT / 'runtime/libc.so.6', 'nexus/fs-root/lib/x86_64-linux-gnu/libc.so.6',
        'put', OUT / 'native64.dat', 'etc/native64.dat', 'put', OUT / 'dynamic.elf', 'bin/dynamic64',
        'put', OUT / 'signals.elf', 'bin/signals64',
        'put', OUT / 'ipc_user.elf', 'bin/ipc64',
        'put', OUT / 'process_user.elf', 'bin/process64',
        'put', OUT / 'threads_user.elf', 'bin/threads64',
        'put', OUT / 'vmstress_user.elf', 'bin/vmstress64',
        'put', OUT / 'pipes_user.elf', 'bin/pipes64',
        'put', OUT / 'events_user.elf', 'bin/events64',
        'put', OUT / 'namespaces_user.elf', 'bin/namespaces64',
        'put', OUT / 'unix_user.elf', 'bin/unix64',
        'put', OUT / 'runtime/COPYRIGHT.glibc', 'share/licenses/glibc.txt']
    for library in runtime.LIBRARIES:
        install += ['put', OUT / 'runtime' / library, 'lib/x86_64-linux-gnu/' + library]
    install += ['put', OUT / 'runtime/ld-linux-x86-64.so.2', 'lib64/ld-linux-x86-64.so.2']
    run(*install)
    if disk.stat().st_size < 64 * 1024 * 1024:
        run(sys.executable, '-I', ROOT / 'storage.py', '64', disk)
    run(sys.executable, '-I', ROOT / 'userland/fatput.py', disk, 'put', OUT / 'wow32_user.elf', 'bin/wow32',  # after growth: 16 MiB is full
        'put', OUT / 'memlayout_user.elf', 'bin/memlayout64', 'put', OUT / 'memlayout_exec.elf', 'bin/memlayout-exec64',
        'put', OUT / 'memlayout_low.elf', 'bin/memlayout-low64',
        'put', OUT / 'seccomp_user.elf', 'bin/seccomp64',  # seccomp conformance (tests/test_x64_seccomp.py)
        'put', OUT / 'aslr_user.elf', 'bin/aslr64',  # ASLR conformance (tests/test_x64_aslr.py)
        'put', OUT / 'umip_user.elf', 'bin/umip64',  # UMIP emulation (tests/test_x64_umip.py)
        'put', OUT / 'nsfs_user.elf', 'bin/nsfs64',  # nsfs magic links + setns (tests/test_x64_nsfs.py)
        'put', OUT / 'kabi_user.elf', 'bin/kabi64',  # Linux ABI conformance (tests/test_x64_kabi.py)
        'put', OUT / 'kabi2_user.elf', 'bin/kabi2',  # Linux ABI conformance, part 2 (tests/test_x64_kabi.py)
        'put', OUT / 'kguard_user.elf', 'bin/kguard64',  # kguard oops containment (tests/test_x64_kguard.py)
        'put', OUT / 'secfs_user.elf', 'bin/secfs64',  # securityfs lockdown (tests/test_x64_lockdown.py)
        'put', OUT / 'kptr_user.elf', 'bin/kptr64')  # kptr/kallsyms hygiene (tests/test_x64_kptr.py)
    # The same applications on an ext4 root volume (MBR type 0x83 at LBA 32768); posix64-root is set-user-ID root.
    spec = importlib.util.spec_from_file_location('nexus_fat2ext4', ROOT / 'userland/fat2ext4.py')
    fat2ext4 = importlib.util.module_from_spec(spec); spec.loader.exec_module(fat2ext4)
    ext4_disk = OUT / 'apps64-ext4.img'; ext4_disk.unlink(missing_ok=True)
    fat2ext4.convert(str(disk), str(ext4_disk))
    volume = fat2ext4.ext4lib.Writer(str(ext4_disk)); volume.setattr('bin/posix64-root', mode=0o4755); volume.close()
    print(f'BUILD64 PASS: {len(kernel)} bytes; {OUT / "nexus64.img"}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', nargs='?', default='build', choices=['build', 'run', 'test', 'chrome', 'desktop', 'vmware', 'secureboot', 'usb-image'])
    parser.add_argument('output', nargs='?', type=Path, help='usb-image: the image file to write')
    parser.add_argument('--size', help='usb-image: image size such as 8G (default: just large enough)')
    parser.add_argument('--root-from', type=Path, help='usb-image: disk image with the root filesystem (default out/desktop64.img)')
    parser.add_argument('--cmdline', default='', help='usb-image: extra kernel options')
    parser.add_argument('--mbr', default='hybrid', choices=('hybrid', 'active', 'protective'), help='usb-image: MBR entries (usbimage.py mbr_entries)')
    parser.add_argument('--tcg', action='store_true', help='Use software emulation for desktop diagnostics')
    parser.add_argument('--gpu', action='store_true', help='virgl GPU acceleration for Chrome (WebGL/WebGPU); needs the GPU userland on the disk')
    parser.add_argument('--uefi', action='store_true', help='boot through UEFI (OVMF) instead of the BIOS floppy')
    parser.add_argument('--legacy-devices', action='store_true', help='pc chipset, IDE disk, RTL8139 and PS/2 (the previous machine)')
    parser.add_argument('--hidden', action='store_true', help='vmware: run without a Workstation window (vmrun nogui)')
    parser.add_argument('--no-swap', action='store_true', help='desktop/chrome: boot without the /etc/fstab swap file (none is created)')
    parser.add_argument('--swap', action='store_true', help='desktop/chrome: create the default /swapfile on the disk once (opt-in)')
    options = parser.parse_args()
    action = options.action
    if action == 'secureboot':
        # Produce the Secure Boot artifacts for the ESP/USB image: a detached kernel signature, the Authenticode-signed
        # loader (verified offline the way edk2 does before it is used), and pre-enrolled OVMF variable stores:
        # sb-vars.fd = PK/KEK/db certificates only (the firmware trusts the loader by its db signature), and
        # sb-vars-hash.fd = the same plus the unsigned loader's Authenticode hash in db (the hash-enrollment fallback).
        # Keys come from SECUREBOOT_KEYDIR (uefi/secureboot.py `gen` creates them); only public certs are in the tree.
        build()
        sbm = importlib.util.module_from_spec(importlib.util.spec_from_file_location('sb', HERE / 'uefi/secureboot.py'))
        sbm.__spec__.loader.exec_module(sbm)
        efi = importlib.util.module_from_spec(importlib.util.spec_from_file_location('sbefi', HERE / 'uefi/secureboot_efi.py'))
        efi.__spec__.loader.exec_module(efi)
        if not (sbm.DEFAULT_KEYDIR / 'kernel_private.pem').exists():
            sbm.gen(sbm.DEFAULT_KEYDIR)
        sbm.sign(OUT / 'kernel.bin', OUT / 'kernel.sig', sbm.DEFAULT_KEYDIR)
        efi.sign_pe(str(OUT / 'BOOTX64.EFI'), str(OUT / 'BOOTX64-signed.EFI'))
        efi.verify_pe(OUT / 'BOOTX64-signed.EFI', HERE / 'uefi/keys/db.der')
        template = Path(r'C:\Program Files\qemu\share\edk2-i386-vars.fd')
        if template.exists():
            efi.enroll_varstore(str(template), str(OUT / 'sb-vars.fd'))
            efi.enroll_varstore(str(template), str(OUT / 'sb-vars-hash.fd'), loader_hash=str(OUT / 'BOOTX64.EFI'))
        run(sys.executable, '-I', ROOT / 'mkuefi.py', OUT / 'BOOTX64-signed.EFI', OUT / 'kernel.bin',
            OUT / 'nexus64-uefi-signed.img', '--kernel-sig', OUT / 'kernel.sig')
        print('[secureboot] out/kernel.sig, out/BOOTX64-signed.EFI, out/sb-vars.fd (PK/KEK/db), out/sb-vars-hash.fd '
              '(+ loader hash), out/nexus64-uefi-signed.img (signed loader) ready')
    elif action in ('chrome', 'desktop', 'vmware'):
        if not (OUT / 'desktop64.img').is_file():
            raise SystemExit('No installed desktop image. Complete the native download/install workflow in docs/DEVELOPMENT_NOTES.md first.')
        metadata = json.loads((OUT / 'desktop64.json').read_text())
        if action in ('desktop', 'vmware') and not metadata.get('aurora', {}).get('verified'):
            raise SystemExit('Aurora desktop has not been verified and published yet.')
    else:
        build()
    vmware_spec = importlib.util.spec_from_file_location('nexus_vmware', HERE / 'vmware.py')
    vmware = importlib.util.module_from_spec(vmware_spec); vmware_spec.loader.exec_module(vmware)
    if action in ('chrome', 'desktop') and vmware.disk_in_use_by_vmware():
        raise SystemExit('NexusOS is running in VMware on the same desktop disk; shut it down first.')
    if action == 'usb-image':
        if not options.output: raise SystemExit('usb-image needs the output image path')
        usbimage_spec = importlib.util.spec_from_file_location('nexus_usbimage', HERE / 'usbimage.py')
        usbimage = importlib.util.module_from_spec(usbimage_spec); usbimage_spec.loader.exec_module(usbimage)
        usbimage.build_image(options.output, options.size, options.root_from, options.cmdline, mbr_mode=options.mbr)
    elif action == 'vmware':
        vmware.launch(OUT / 'desktop64.img', options.uefi, options.hidden)
    elif action == 'test':
        run(sys.executable, '-I', ROOT / 'tests/test_x64.py')
    elif action in ('run', 'chrome', 'desktop'):
        desktop = action in ('chrome', 'desktop')
        disk = OUT / ('desktop64.img' if desktop else 'apps64.img')
        # The default /swapfile + /etc/fstab line, created once (swap_tools.py) and only on request (--swap): the
        # launcher never writes the user's installed disk on its own; never while the VM launch is mocked
        # (tests/test_x64_launch.py, test_aurora_launch.py): a unit test must not edit the installed disk.
        if desktop and options.swap and not options.no_swap and isinstance(subprocess.Popen, type):
            swap_spec = importlib.util.spec_from_file_location('nexus_swap_tools', HERE / 'swap_tools.py')
            swap_tools = importlib.util.module_from_spec(swap_spec); swap_spec.loader.exec_module(swap_tools); swap_tools.ensure_default(disk)
        # System Settings > System Upgrade: the upgrade (or the way back) the system asked for runs between two sessions
        # (migrate_disk.py, progress in this window); never while the VM launch is mocked (see above)
        upgrades = None
        if desktop and isinstance(subprocess.Popen, type):
            try:
                upgrade_spec = importlib.util.spec_from_file_location('nexus_migrate_disk', HERE / 'migrate_disk.py')
                upgrades = importlib.util.module_from_spec(upgrade_spec); upgrade_spec.loader.exec_module(upgrades)
            except Exception as error:   # noqa: BLE001 - the session starts in any case
                upgrades = None; print('[upgrade] the System Upgrade is not available:', error, flush=True)
            if upgrades: upgrades.launcher_step(disk)
        cpu_options = desktop_cpu_options(options.tcg) if desktop else ['-cpu', 'max']
        machine = (desktop_machine_options(disk, options.gpu, options.uefi, options.legacy_devices) if desktop else
                   ['-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide,snapshot=on',
                    '-boot', 'a', '-vga', 'none', '-device', 'virtio-vga', '-display', 'sdl'])
        command = [QEMU, *cpu_options, *(['-smp', str(desktop_cpu_count())] if desktop else []), '-m', '2048' if desktop else '256', *machine,
            *(['-name', 'NexusOS - Aurora' if action == 'desktop' else 'NexusOS - Chrome',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', *desktop_audio_options()] if desktop else []),
            *(['-fw_cfg', 'name=opt/nexus/session,string=chrome'] if action == 'chrome' and metadata.get('aurora', {}).get('verified') else []),
            *(['-fw_cfg', 'name=opt/nexus/swap,string=off'] if desktop and options.no_swap else []),   # swap.c swap_boot
            '-serial', f'file:{OUT / ("desktop-serial.log" if desktop else "serial.log")}', '-no-reboot']
        if desktop:
            tablet = any('usb-tablet' in str(item) for item in machine)  # see hidden_startup()
            while True:
                vm = subprocess.Popen([str(value) for value in command], cwd=ROOT, startupinfo=hidden_startup() if tablet else None)
                unthrottle(vm.pid)
                if tablet:
                    reveal_window(vm, OUT / 'desktop-serial.log')
                returncode = vm.wait()
                if returncode not in (0, 33): raise SystemExit(returncode)
                if not (upgrades and upgrades.launcher_step(disk)): break   # asked for in this session: done, start the result
        else:
            run(*command)
