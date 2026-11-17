"""Verify userland framebuffer pixels and ownership in a private native VM."""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import sys
import time
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-display')
WORK.mkdir(parents=True, exist_ok=True)
import os  # NEXUS_ROOT_FS=ext4 selects the ext4 apps disk
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland' / ('ext4put.py' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'fatput.py'))
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
spec=importlib.util.spec_from_file_location('qmp_local',ROOT/'tests/qmp_local.py')
qmp_local=importlib.util.module_from_spec(spec);spec.loader.exec_module(qmp_local)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / ('apps64-ext4.img' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'apps64.img'), disk)
marker = WORK / 'marker'; marker.write_bytes(b'1')
fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-display'])
disk_hash = hashlib.sha256(disk.read_bytes()).digest()
control=qmp_local.QmpMonitor(WORK)
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
p = subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe', *(['-accel','whpx,kernel-irqchip=off','-cpu','qemu64,+rdrand'] if os.environ.get('NEXUS_ACCEL') == 'whpx' else ['-cpu','max']), '-m', '128',
    '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
    '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a', '-display', 'none',
    '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
    '-qmp', control.argument, '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    def wait_marker(marker):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if serial.exists() and marker in serial.read_text(): return
            if p.poll() is not None: raise AssertionError((p.communicate()[0], serial.read_text()))
            time.sleep(.02)
        raise AssertionError(('Missing ' + marker, serial.read_text()))
    wait_marker('[64] DISPLAY FRAME READY')
    with control.open() as monitor:
        qmp=monitor.command
        ppm = WORK / 'frame.ppm'; qmp('screendump', {'filename': str(ppm)})
        with Image.open(ppm) as image:
            image.save(WORK / 'frame.png')
            w, h = image.size
            for x, y, color in [(w//4, h//4, (24,199,132)), (3*w//4, h//4, (40,120,221)),
                                (w//4, 3*h//4, (229,155,53)), (3*w//4, 3*h//4, (16,21,34)),
                                (24,24,(24,199,132))]:
                assert image.getpixel((x,y)) == color, (x,y,image.getpixel((x,y)),color)
        def key(name, down):
            qmp('input-send-event', {'events': [{'type':'key', 'data':{'down':down, 'key':{'type':'qcode','data':name}}}]})
        def mouse(dx=0, dy=0, button=None, down=False, wheel=None):
            events = []
            if dx: events.append({'type':'rel', 'data':{'axis':'x','value':dx}})
            if dy: events.append({'type':'rel', 'data':{'axis':'y','value':dy}})
            if button: events.append({'type':'btn', 'data':{'button':button,'down':down}})
            if wheel: events.append({'type':'btn', 'data':{'button':wheel,'down':True}})
            qmp('input-send-event', {'events':events})
            if wheel: qmp('input-send-event', {'events':[{'type':'btn','data':{'button':wheel,'down':False}}]})
        wait_marker('[64] INPUT READY'); key('a', True)
        wait_marker('[64] INPUT RELEASE READY'); key('a', False)
        wait_marker('[64] INPUT EXTENDED READY')
        for name in ('ctrl_r', 'print', 'pause'):
            key(name, True); time.sleep(.01); key(name, False); time.sleep(.01)
        wait_marker('[64] INPUT MOUSE READY'); mouse(37, -19, 'left', True, 'wheel-up')
        wait_marker('[64] INPUT MOUSE PRESS PASSED')
        qmp('screendump', {'filename': str(WORK / 'input.ppm')})
        with Image.open(WORK / 'input.ppm') as image:
            image.save(WORK / 'input.png')
            assert image.getpixel((8,8)) == (255,255,255), 'keyboard did not update framebuffer'
            assert image.getpixel((w//2+45,h//2-11)) == (255,0,255), 'mouse did not update framebuffer'
        mouse(-5, 8, 'left', False, 'wheel-down')
        wait_marker('[64] INPUT OVERFLOW READY')
        for _ in range(160):
            key('a', True); key('a', False); time.sleep(.002)
        key('b', True); time.sleep(.02); mouse(button='right', down=True)
        wait_marker('[64] INPUT OVERFLOW RECOVERED'); key('b', False); mouse(button='right', down=False)
    output = p.communicate(timeout=10)[0]
    log = serial.read_text()
    assert p.returncode == 33 and '[64] DISPLAY ALL PASSED:' in log and '[64] DISPLAY CAMPAIGN PASSED' in log, (output, log)
    assert '[64] FAIL:' not in log and 'KERNEL_FAULT' not in log and log.count('[64] USER_FAULT') == 3, log
    assert 'display mappings and descriptors reclaimed' in log
    assert '[64] INPUT ALL PASSED:' in log and 'PS/2 keyboard ready' in log and 'PS/2 mouse ready' in log
finally:
    if p.poll() is None: p.kill(); p.wait()
    control.cleanup(p)
assert hashlib.sha256(disk.read_bytes()).digest() == disk_hash, 'display test altered disk'
print('PASS native framebuffer and PS/2 input: actual pixels/key/mouse/wheel, blocking/poll/epoll, recovery, ABI and memory isolation')
