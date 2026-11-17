"""Exercise production ATA waits and FAT mount/mutation errors with simulated I/O in C."""
from pathlib import Path
import subprocess
import tempfile
import struct

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-ata-') as directory:
    for source in ('ata_wait_test', 'ata_transfer_test', 'ata_dma_test', 'fat_mount_test', 'fat_rename_test', 'fat_seek_test', 'fat_cache_test'):
        targets=('x86_64-windows-gnu','x86-windows-gnu') if source.startswith('ata_') or source == 'fat_cache_test' else ('x86_64-windows-gnu',)
        for target in targets:
            executable = Path(directory) / (source + '-' + target + '.exe')
            checks = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if source in ('fat_cache_test', 'ata_dma_test') else []
            if source == 'ata_dma_test': checks += ['-Wl,--image-base,0x400000']
            subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc',
                            '-target', target, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            *checks, str(ROOT / 'tests' / (source + '.c')), '-o', str(executable)], check=True)
            if source == 'ata_dma_test':
                # Only this temporary host fixture needs low identity addresses for fake PCI DMA.
                image = bytearray(executable.read_bytes())
                optional = struct.unpack_from('<I', image, 0x3C)[0] + 24
                flags = struct.unpack_from('<H', image, optional + 70)[0]
                struct.pack_into('<H', image, optional + 70, flags & ~0x60)
                executable.write_bytes(image)
            subprocess.run([str(executable)], check=True, timeout=10)
