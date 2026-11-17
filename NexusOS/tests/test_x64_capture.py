"""Native ALSA capture (pcmC0D0c) through the x86-64 AC'97 and Intel HD Audio drivers, end to end in QEMU.

Usage: python -I tests/test_x64_capture.py [WORK] [--only CASE,...] [--smp N] [--tcg] [--qmp-port N] [--cache DIR]
Builds /bin/audio64 (arch/x86_64/audio_user.c) against the unmodified Debian libasound.so.2, installs Debian's
arecord (alsa-utils 1.2.14 aplay, pinned in arch/x86_64/alsa-utils-lock.json, verified size + SHA-256, cached in
--cache) as /bin/arecord, and runs the capture campaign (mode byte in /nexus/test-audio) on a private copy of
apps64.img, one VM per case. Every recording case also runs arecord: -l, a 2 s WAV via "default", -M (mmap) on
hw:0,0, 16 kHz mono via plughw, and clean failures (missing card, 6 channels).
  ac97_counter    AC'97, kernel test pattern snd.test-capture=counter: blocking/non-blocking/mmap/poll capture,
                  overrun + recover, pause, drain, two recorders, full duplex, capture mixer (alsa-lib simple mixer)
  hda_counter     intel-hda + hda-duplex (line-in), same campaign through the HDA input stream descriptor
  hda_tone        ich9-intel-hda + hda-micro (mic), snd.test-capture=tone: 1 kHz at 48 kHz and resampled to 44.1 kHz
  hda_silent      intel-hda + hda-duplex with QEMU's "none" audiodev (no test pattern): the real input DMA, which
                  records silence at the device rate: frame rate, overruns, poll, mmap, mixer
  ac97_silent     AC'97 PCM-in with the "none" audiodev
  hda_nocapture   intel-hda + hda-output (no input converter): pcmC0D0c absent, capture open fails cleanly
QEMU has no audio backend that records known data, so the content checks use the kernel's test pattern (inserted
above the DMA, at the copy into the client buffers) and the real-DMA cases prove timing, format and silence.
"""
from pathlib import Path
import argparse
import contextlib
import io
import importlib.util
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CASES = {
    'ac97_counter': (['-device', 'AC97,audiodev=snd0'], 'snd.test-capture=counter', 'c'),
    'hda_counter': (['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'], 'snd.test-capture=counter', 'c'),
    'hda_tone': (['-device', 'ich9-intel-hda', '-device', 'hda-micro,audiodev=snd0'], 'snd.test-capture=tone', 'k'),
    'hda_silent': (['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'], '', 'z'),
    'ac97_silent': (['-device', 'AC97,audiodev=snd0'], '', 'z'),
    'hda_nocapture': (['-device', 'intel-hda', '-device', 'hda-output,audiodev=snd0'], '', 'x'),
}
parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
parser.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('capture'))
parser.add_argument('--only', default=','.join(CASES))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4683)
parser.add_argument('--smp', type=int, default=1)
parser.add_argument('--cache', type=Path, default=nexus_paths.cache('alsa-utils'))
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
selected = [c for c in options.only.split(',') if c]
assert all(c in CASES for c in selected), selected


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland' / ('ext4put.py' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'fatput.py'))
root = OUT / 'chrome-root'
libasound = root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
if not libasound.exists(): raise SystemExit('SKIP: no Debian libasound in out/chrome-root (install Chrome first)')
program = WORK / 'audio_user.elf'


def alsa_utils():
    """Pinned Debian alsa-utils: fetch (deb.debian.org, HTTPS) or reuse the cache, verify, return {guest path: host file}."""
    import hashlib, tarfile, urllib.parse, urllib.request
    lock = json.loads((HERE / 'alsa-utils-lock.json').read_text())
    files = {}
    for package in lock['packages']:
        url = urllib.parse.urlsplit(package['url'])
        if url.scheme != 'https' or url.hostname != 'deb.debian.org': raise ValueError('unexpected package origin')
        options.cache.mkdir(parents=True, exist_ok=True)
        deb = options.cache / Path(url.path).name
        data = deb.read_bytes() if deb.exists() else b''
        if len(data) != package['bytes'] or hashlib.sha256(data).hexdigest() != package['sha256']:
            with urllib.request.urlopen(package['url'], timeout=120) as response: data = response.read(package['bytes'] + 1)
            if len(data) != package['bytes'] or hashlib.sha256(data).hexdigest() != package['sha256']: raise ValueError('package verification failed: ' + package['name'])
            deb.write_bytes(data)
        offset, payload = 8, None
        if data[:8] != b'!<arch>\n': raise ValueError('not a Debian package')
        while offset + 60 <= len(data):
            header = data[offset:offset + 60]; size = int(header[48:58]); name = header[:16].decode().strip().rstrip('/')
            if name.startswith('data.tar'): payload = data[offset + 60:offset + 60 + size]; break
            offset += 60 + size + (size & 1)
        with tarfile.open(fileobj=io.BytesIO(payload)) as archive:
            for source, guest in package['files'].items():
                target = WORK / 'alsa-utils' / Path(source).name; target.parent.mkdir(exist_ok=True)
                target.write_bytes(archive.extractfile('./' + source).read()); files[guest] = target
    return files


arecord = alsa_utils()
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'audio_user.c'), str(libasound), '-lm', '-o', str(program)], check=True)


def run(case):
    devices, cmdline, mode = CASES[case]
    disk = WORK / f'{case}.img'; disk.unlink(missing_ok=True)
    shutil.copyfile(OUT / ('apps64-ext4.img' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'apps64.img'), disk)
    serial = WORK / f'{case}.log'; serial.unlink(missing_ok=True)
    try:
        marker = WORK / 'marker'; marker.write_bytes(mode.encode())
        install = ['fatput', str(disk), 'put', str(program), 'bin/audio64', 'put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2',
                   'put', str(marker), 'nexus/test-audio']
        for guest, host in arecord.items(): install += ['put', str(host), guest]
        for source in sorted((root / 'usr/share/alsa').rglob('*')):
            if source.is_file(): install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(root / 'usr/share/alsa').as_posix()]
        with contextlib.redirect_stdout(io.StringIO()): fatput.main(install)
        command = [QEMU, *build.desktop_cpu_options(options.tcg), '-smp', str(options.smp), '-m', '512',
                   '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
                   '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
                   '-audiodev', 'none,id=snd0', *devices,
                   '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
        if cmdline: command += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
        (WORK / f'{case}.cmd.json').write_text(json.dumps([str(c) for c in command], indent=1))
        process = subprocess.Popen([str(c) for c in command], stdout=(WORK / f'{case}.qemu.log').open('wb'), stderr=subprocess.STDOUT)
        print(case, 'VM', process.pid, flush=True)
        try:
            deadline = time.monotonic() + 300
            while time.monotonic() < deadline:
                text = serial.read_bytes().decode('latin1') if serial.exists() else ''
                if re.search(r'\[64\] AUDIO CAMPAIGN PASSED|\[64\] FAIL:|KERNEL_FAULT', text) or process.poll() is not None: break
                time.sleep(.2)
            time.sleep(1)
        finally:
            try:
                with socket.create_connection(('127.0.0.1', options.qmp_port), timeout=5) as link:
                    stream = link.makefile('rwb'); stream.readline()
                    for request in ({'execute': 'qmp_capabilities'}, {'execute': 'quit'}):
                        stream.write(json.dumps(request).encode() + b'\n'); stream.flush(); stream.readline()
            except OSError:
                pass
            try: process.wait(30)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
    finally:
        disk.unlink(missing_ok=True)
    text = serial.read_bytes().decode('latin1') if serial.exists() else ''
    lines = [line for line in text.splitlines() if any(k in line for k in ('AUDIO', 'SOUND', 'AC97', 'HDA', 'ALSA'))]
    print('\n'.join(lines), flush=True)
    ok = '[64] AUDIO TEST PASSED' in text and '[64] AUDIO CAMPAIGN PASSED' in text and 'ALSA lib' not in text and 'KERNEL_FAULT' not in text
    if mode != 'x': ok = ok and '[64] SOUND capture device ready' + (' (test pattern)' if cmdline else '') in text
    if case.startswith('hda') and mode != 'x': ok = ok and re.search(r'\[64\] HDA \S+ codec 0 capture pin 0x05 -> adc 0x04', text) is not None
    if mode != 'x': ok = ok and '[64] AUDIO CH arecord wav status=0 bytes=384044 data=384000' in text
    return ok, lines


results = {}
for case in selected:
    ok, lines = run(case); results[case] = {'pass': ok, 'lines': lines}
    print('CASE', case, 'PASS' if ok else 'FAIL', flush=True)
(WORK / 'result.json').write_text(json.dumps(results, indent=1))
failed = [c for c, r in results.items() if not r['pass']]
assert not failed, f'capture cases failed: {failed}'
print('PASS native ALSA capture:', ', '.join(selected))
