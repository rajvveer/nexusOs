"""Run real Chrome version, JavaScript, and raster-output probes on the custom kernel.

This is not the interactive-browsing completion gate. Run build.py and chrome.py first.
"""
from pathlib import Path
import importlib.util
import shutil
import struct
import subprocess
import sys
import time
from urllib.parse import quote
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('chrome64')
MODE = sys.argv[2] if len(sys.argv) > 2 else 'version'
if MODE not in ('version', 'headless', 'headless-js', 'headless-file-js', 'headless-render'): raise ValueError('Unknown Chrome probe mode')
WORK.mkdir(parents=True, exist_ok=True)
serial = WORK / 'serial.log'
disk = OUT / 'chrome64.img'
owned_disk = False
try:
    if MODE != 'version':
        disk = WORK / 'probe-disk.img'
        if disk.exists(): raise FileExistsError('Refusing to overwrite an existing test disk: '+str(disk))
        owned_disk = True
        shutil.copyfile(OUT / 'chrome64.img', disk)
        arguments = WORK / 'chrome.args'
        target = 'about:blank'
        output_flags = '--dump-dom\n'
        if MODE in ('headless-js', 'headless-file-js'):
            document = ('<!doctype html><title>NexusOS test</title><main id="result">pending</main><script>'
                        'function sum(n){let value=0;for(let i=0;i<n;i++)value=(value+i)>>>0;return value}'
                        'let answer=0;for(let run=0;run<500;run++)answer=sum(10000);'
                        'document.getElementById("result").textContent="NEXUS_JS_PASS:"+answer;'
                        'document.title="NexusOS "+"verified";</script>')
            target = 'data:text/html,' + quote(document, safe='')
            if MODE == 'headless-file-js':
                page = WORK / 'proof.html'; page.write_text(document)
                target = 'file:///home/nexus/js-proof.html'
        if MODE == 'headless-render':
            document = ('<!doctype html><meta charset="utf-8"><title>Chrome on NexusOS</title>'
                        '<style>body{margin:0;background:#0b1220;color:#f8fafc;font-family:sans-serif}'
                        'h1{position:absolute;left:40px;top:20px;font-size:44px}'
                        'p{position:absolute;left:40px;top:120px;font-size:22px}'
                        '#result{position:absolute;left:40px;top:200px;width:700px;height:100px;'
                        'box-sizing:border-box;padding:30px;background:#dc2626;font-size:26px}'
                        '</style><h1>Chrome on NexusOS</h1><p>Custom kernel / real browser rendering</p>'
                        '<main id="result">JavaScript pending</main><script>'
                        'function sum(n){let value=0;for(let i=0;i<n;i++)value=(value+i)>>>0;return value}'
                        'let answer=0;for(let run=0;run<500;run++)answer=sum(10000);'
                        'if(answer===49995000){let r=document.getElementById("result");'
                        'r.textContent="JavaScript verified: "+answer;r.style.background="#18c784";}'
                        '</script>')
            target = 'data:text/html,' + quote(document, safe='')
            output_flags = '--screenshot=/home/nexus/render.png\n--window-size=1000,700\n--force-device-scale-factor=1\n'
        arguments.write_text('--headless\n' + output_flags + '--enable-logging=stderr\n--user-data-dir=/home/nexus/chrome-profile\n' + target + '\n')
        spec = importlib.util.spec_from_file_location('probe_fatput', ROOT / 'userland/fatput.py')
        fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
        additions = ['put', str(page), 'home/nexus/js-proof.html'] if MODE == 'headless-file-js' else []
        assert fatput.edit(['fatput', str(disk), 'put', str(arguments), 'nexus/chrome.args', *additions]) == 0
except BaseException:
    if owned_disk: disk.unlink(missing_ok=True)
    raise
args = [r'C:\Program Files\qemu\qemu-system-x86_64.exe', '-cpu', 'max', '-m', '2048',
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on',
        '-drive', f'file={disk},format=raw,if=ide' + (',snapshot=on' if MODE == 'version' else ''), '-boot', 'a',
        '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4']
start = time.monotonic()
process = None
try:
    serial.unlink(missing_ok=True)
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output = process.communicate(timeout=300)[0]
    except subprocess.TimeoutExpired:
        process.kill(); output = process.communicate()[0]
        raise AssertionError('Chrome probe timed out: ' + serial.read_text()[-5000:])
    log = serial.read_text()
    print(log[-6000:])
    assert process.returncode == 33, (process.returncode, output)
    if MODE == 'version':
        assert '[64] CHROME VERSION PROBE PASSED' in log and 'Google Chrome 155.0.8059.39' in log
    else:
        assert '[64] CHROME PROBE RETURNED' in log
        if MODE == 'headless-render':
            from PIL import Image
            fs = fatput.Fat32(disk)
            try:
                record = fs.find_name(fs.lookup_dir(['home', 'nexus']), 'render.png')
                assert record, 'Chrome did not save its screenshot'
                entry = record[2]; size = struct.unpack_from('<I', entry, 28)[0]
                first = struct.unpack_from('<H', entry, 20)[0] << 16 | struct.unpack_from('<H', entry, 26)[0]
                assert 0 < size < 8 * 1024 * 1024, size
                screenshot = WORK / 'render.png'; screenshot.write_bytes(fs.read_chain(first, size))
            finally:
                fs.close()
            with Image.open(screenshot) as png:
                assert png.format == 'PNG' and png.size == (1000, 700), (png.format, png.size)
                pixels = png.convert('RGB')
                assert pixels.getpixel((10, 10)) == (11, 18, 32)
                assert pixels.getpixel((45, 205)) == (24, 199, 132), 'JavaScript did not change the status panel'
                ink = sum(all(channel > 170 for channel in pixels.getpixel((x, y))) for y in range(40, 120) for x in range(40, 960))
                assert ink > 80, 'No text was painted'
            print('Verified native Chrome PNG:', screenshot)
        else:
            assert '<html>' in log and '</html>' in log
        if MODE in ('headless-js', 'headless-file-js'):
            assert '<main id="result">NEXUS_JS_PASS:49995000</main>' in log and '<title>NexusOS verified</title>' in log
    assert 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log
    assert 'FILE_WRITEBACK failed' not in log and 'FILE_CACHE read failed' not in log and 'PAGE_IO' not in log
finally:
    if process is not None and process.poll() is None: process.kill();process.wait()
    if owned_disk: disk.unlink(missing_ok=True)
print(f'PASS real Chrome {MODE} probe in {time.monotonic() - start:.1f}s; interactive/download/install gates still pending')
