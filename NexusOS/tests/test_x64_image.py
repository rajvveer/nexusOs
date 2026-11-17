"""ELF-to-boot-image regression: segment addresses, gaps and corrupt ranges."""
from pathlib import Path
import importlib.util
import struct

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build64', root / 'arch/x86_64/build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
ident = b'\x7fELF\x02\x01\x01' + bytes(9)
image = bytearray(512)
vma = build.KERNEL_VMA  # higher-half kernel: linked at KERNEL_VMA + physical, loaded at physical 0x100000
struct.pack_into('<16sHHIQQQIHHHHHH', image, 0, ident, 2, 62, 1, vma + 0x100000, 64, 0, 0, 64, 56, 2, 0, 0, 0)
struct.pack_into('<II6Q', image, 64, 1, 5, 256, vma + 0x100000, 0x100000, 3, 3, 4096)
struct.pack_into('<II6Q', image, 120, 1, 6, 300, vma + 0x109000, 0x109000, 4, 8192, 4096)
image[256:259] = b'CPU'; image[300:304] = b'GDT!'
result = build.flat_kernel(image)
assert result == b'CPU' + bytes(0x9000 - 3) + b'GDT!'
for offset, value in ((72, 9999), (96, 9999), (56, 1000), (24, 0), (144, 0xFFFFFF)):
    bad = bytearray(image)
    struct.pack_into('<H' if offset == 56 else '<Q', bad, offset, value)
    try:
        build.flat_kernel(bad)
    except ValueError:
        continue
    raise AssertionError(('accepted invalid image', offset))
print('PASS ELF64 image addresses, padding, truncation, header bounds and load ranges')
