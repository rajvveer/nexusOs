"""What a Wine prefix needs from the NexusOS root filesystem (FAT32), checked in the guest by out/wine_fs_user.elf.

    python -I tests/test_wine_prefix_fs.py WORK [--source DESKTOP64.img] [--disk PREPARED.img] [--qmp-port 4612]
           [--accel auto|whpx|tcg] [--keep-disk]

Boots a desktop64 copy (sparse copy of --source unless --disk) through a qcow2 overlay with Aurora running
/bin/winefs: dosdevices/c: -> ../drive_c and z: -> / symlinks (':' in FAT names is stored as U+F03A and mapped back by
kernel/fat32.c), readlink/lstat/stat through them, case-insensitive lookups, listing, rename, the FAT-reserved characters
" * : < > ? \\ | in names, fcntl record locks, an 8 MiB file with pread/ftruncate, statfs.
"""
from pathlib import Path
import argparse
import json
import re
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--disk', type=Path); ap.add_argument('--qmp-port', type=int, default=4612)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg')); ap.add_argument('--keep-disk', action='store_true')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
started = time.time(); result = {'test': 'wine_prefix_fs', 'passed': False}
own_disk = args.disk is None
disk = (args.disk or WORK / 'disk.img').resolve()
vm = None
try:
    if own_disk:
        if disk.exists(): disk.unlink()
        aurora_vm.sparse_copy(args.source, disk)
    aurora_vm.stage(disk, {'sbin/init': aurora_vm.OUT / 'desktop.elf', 'bin/winefs': aurora_vm.OUT / 'wine_fs_user.elf',
                           'nexus/desktop.args': '--aurora\n/bin/winefs\n'})
    vm = aurora_vm.Vm(WORK, disk, args.qmp_port, args.accel); result['accel'] = vm.accel
    vm.wait(r'AURORA READY', seconds=240)
    match = vm.wait(r'\[64\] WINEFS (PASSED|FAIL[^\n]*)', seconds=120)
    result['markers'] = re.findall(r'\[64\] WINEFS ([^\n]*)', vm.text())
    assert match.group(1) == 'PASSED', match.group(0)
    vm.wait(r'AURORA CHILD EXIT pid=\d+ status=0', seconds=30)
    faults = vm.faults(); assert not faults, faults
    result['passed'] = True
finally:
    if vm: vm.close()
    result['seconds'] = round(time.time() - started, 1)
    (WORK / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    if own_disk and not args.keep_disk: disk.unlink(missing_ok=True)
print('PASS Wine prefix filesystem semantics' if result['passed'] else 'FAIL', json.dumps(result.get('markers')))
