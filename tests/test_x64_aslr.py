"""ASLR conformance campaign.

Boots the native kernel with /nexus/test-aslr on a private copy of apps64.img. The kernel runs /bin/aslr64
(arch/x86_64/aslr_user.c), which re-execs itself several times and verifies that with randomize_va_space=2 the
PIE/interpreter base, mmap base, heap break and stack all vary across execs while AT_RANDOM is fresh each time,
and that personality(ADDR_NO_RANDOMIZE) makes the whole layout deterministic. Also checks
/proc/sys/kernel/randomize_va_space reports the live level.

Usage: python -I tests/test_x64_aslr.py [WORKDIR]
"""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-aslr')
WORK.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)

EXPECT = ['[64] ASLR ALL PASSED:', '[64] PASS ASLR:', '[64] PASS ASLR campaign pages reclaimed', '[64] ASLR CAMPAIGN PASSED']
FORBIDDEN = ('[64] FAIL', 'KERNEL_FAULT', '[64] ASLR FAIL', 'uncontained kernel exception')

for tag, smp in (('smp1', 1), ('smp2', 2)):
    disk = WORK / (tag + '-disk.img'); shutil.copyfile(OUT / 'apps64.img', disk)
    marker = WORK / 'marker'; marker.write_bytes(b'1')
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-aslr'])
    serial = WORK / (tag + '.log'); serial.unlink(missing_ok=True)
    command = [QEMU, '-cpu', 'max', '-m', '512', '-smp', str(smp),
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
               '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
               '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output = process.communicate(timeout=120)[0]
    except subprocess.TimeoutExpired:
        process.kill(); output = process.communicate()[0]
        raise AssertionError(f'{tag}: timed out\n' + serial.read_text(errors='replace')[-4000:])
    log = serial.read_text(errors='replace')
    disk.unlink(missing_ok=True)
    for line in log.splitlines():
        if 'ASLR' in line or 'FAIL' in line or 'FAULT' in line:
            print(' ', line)
    assert process.returncode == 33, (tag, process.returncode, output, log[-4000:])
    for bad in FORBIDDEN:
        assert bad not in log, (tag, bad, log[-4000:])
    for text in EXPECT:
        assert text in log, (tag, 'missing: ' + text, log[-4000:])
    print(f'PASS {tag} (smp={smp}) in {time.monotonic() - started:.1f} s', flush=True)
print('PASS ASLR campaign: PIE/ld/mmap/stack/brk + AT_RANDOM + personality/randomize_va_space')
