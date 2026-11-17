"""MT7922 Wi-Fi firmware layout groundwork (arch/x86_64/mt7922fw.c; not linked into the kernel) against synthetic and
hostile images; with --firmware DIR also the pinned linux-firmware images (names, sizes and SHA-256 from the
"inventory" of arch/x86_64/firmware-lock.json are checked first). End-to-end Wi-Fi/Bluetooth needs the hardware.

    python -I tests/test_mt7922fw.py [--firmware DIR]"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
firmware = Path(sys.argv[sys.argv.index('--firmware') + 1]) if '--firmware' in sys.argv else None
extra = []
if firmware:
    lock = json.loads((ROOT / 'arch/x86_64/firmware-lock.json').read_text(encoding='utf-8'))
    pins = {Path(item['name']).name: item for item in lock['inventory']}
    for name, item in pins.items():
        data = (firmware / name).read_bytes()
        assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256'], name
    extra = [str(firmware / 'WIFI_MT7922_patch_mcu_1_1_hdr.bin'), str(firmware / 'WIFI_RAM_CODE_MT7922_1.bin')]
with tempfile.TemporaryDirectory(prefix='nexus-mt7922-') as directory:
    executable = Path(directory) / 'mt7922.exe'
    subprocess.run([str(ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O1',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(ROOT / 'tests/mt7922fw_test.c'), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable), *extra], capture_output=True, text=True, timeout=60)
    print(result.stdout[-3000:], result.stderr[-3000:])
    assert result.returncode == 0 and 'MT7922 FIRMWARE LAYOUT TESTS PASSED' in result.stdout, result.returncode
print('PASS MT7922 firmware layout' + (' (pinned images verified)' if firmware else ''))
