"""Native ALSA playback through the x86-64 AC97 driver, captured with QEMU's wav backend and analyzed.

Usage: python -I tests/test_x64_audio.py [WORK] [--tcg] [--qmp-port N] [--backend wav|dsound]
Builds /bin/audio64 against the unmodified Debian libasound.so.2 (from out/chrome-root), copies it with
alsa.conf into a private copy of apps64.img and boots the kernel's /nexus/test-audio campaign.
With the wav backend the capture is checked for the four expected tones and for gaps/discontinuities.
"""
from pathlib import Path
import argparse
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('native64-audio'))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4481)
parser.add_argument('--backend', default='wav', choices=['wav', 'dsound', 'sdl'])
parser.add_argument('--audiodev', help='override the -audiodev argument (id must be snd0)')
parser.add_argument('--probe', choices=['latency', 'stability'], help='measure host-side buffering/steadiness instead of the tone test')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland' / ('ext4put.py' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'fatput.py'))  # NEXUS_ROOT_FS=ext4: ext4 root
wav_check = module('wav_check', HERE / 'wav_check.py')
root = OUT / 'chrome-root'
libasound = root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
if not libasound.exists():
    raise SystemExit('SKIP: no Debian libasound in out/chrome-root (install Chrome first)')

program = OUT / 'audio_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'audio_user.c'), str(libasound), '-lm', '-o', str(program)], check=True)
disk = WORK / 'disk.img'; shutil.copyfile(OUT / ('apps64-ext4.img' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'apps64.img'), disk)
marker = WORK / 'marker'; marker.write_bytes({'latency': b'l', 'stability': b's'}.get(options.probe, b'1'))
install = ['fatput', str(disk), 'put', str(program), 'bin/audio64', 'put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2',
           'put', str(marker), 'nexus/test-audio']
for source in sorted((root / 'usr/share/alsa').rglob('*')):
    if source.is_file():
        install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(root / 'usr/share/alsa').as_posix()]
fatput.main(install)

capture = WORK / 'audio.wav'; capture.unlink(missing_ok=True)
serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
audiodev = {'wav': f'wav,id=snd0,path={capture},out.frequency=48000',
            'dsound': 'dsound,id=snd0,timer-period=5000,out.buffer-length=40000,out.frequency=48000',
            'sdl': 'sdl,id=snd0,timer-period=5000,out.frequency=48000'}[options.backend]
if options.audiodev: audiodev = options.audiodev
command = [QEMU, *build.desktop_cpu_options(options.tcg), '-m', '512',
           '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
           '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga',
           '-audiodev', audiodev, '-device', 'AC97,audiodev=snd0',
           '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
(WORK / 'cmd.json').write_text(json.dumps([str(c) for c in command], indent=1))
process = subprocess.Popen([str(c) for c in command], stdout=(WORK / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
print('VM', process.pid, flush=True)
text = ''
try:
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        text = serial.read_bytes().decode('latin1') if serial.exists() else ''
        if re.search(r'\[64\] AUDIO TEST (PASSED|FAILED)|\[64\] FAIL:|KERNEL_FAULT', text) or process.poll() is not None:
            break
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
    try:
        process.wait(30)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait()
text = serial.read_bytes().decode('latin1') if serial.exists() else ''
lines = [line for line in text.splitlines() if 'AUDIO' in line or 'SOUND' in line or 'AC97' in line or 'ALSA' in line]
print('\n'.join(lines))
assert '[64] AUDIO TEST PASSED' in text and '[64] AUDIO CAMPAIGN PASSED' in text, 'guest test failed (see serial.log)'
if options.probe: raise SystemExit(0)
assert 'ALSA lib' not in text, 'alsa-lib reported errors'
result = {'backend': options.backend, 'guest': lines}
if options.backend == 'wav':
    report = wav_check.analyze(capture); result['wav'] = report
    expected = [(440, 2.9), (880, 1.9), (660, 0.9), (330, 0.9)]
    tones = [s for s in report['segments'] if s['duration_s'] >= 0.3]
    print(json.dumps({k: report[k] for k in ('duration_s', 'peak', 'nonsilent_fraction', 'finalized_header', 'channels_identical')}))
    for s in tones:
        print({k: s[k] for k in ('frequency_hz', 'duration_s', 'rms', 'peak', 'dropout_windows_10ms', 'silence_runs_over_1ms',
                                 'longest_silence_ms', 'discontinuities', 'worst_residual_rel')})
    assert [round(s['frequency_hz'] / 10) * 10 for s in tones[:4]] == [f for f, _ in expected], 'unexpected tone sequence'
    for s, (frequency, minimum) in zip(tones, expected):
        assert abs(s['frequency_hz'] - frequency) < frequency * 0.005 and s['duration_s'] >= minimum, s
        if frequency != 660:
            assert not s['discontinuities'] and not s['silence_runs_over_1ms'] and not s['dropout_windows_10ms'], ('glitch', s)
    assert tones[2]['silence_runs_over_1ms'] >= 1, 'forced underrun should leave a silent gap, not stale audio'
    assert report['channels_identical'] and report['peak'] > 0.2
    # Phase E: two clients mixed. Fit 1000 Hz + 1500 Hz per 20 ms window; both at ~0.25 and a near-zero residual
    # (any gap, splice or stale block in either stream leaves a large residual).
    import numpy as np
    samples, rate, _ = wav_check.load(capture); x = samples[:, 0]; n = rate // 50; t = np.arange(n)
    basis = np.stack([f(2 * np.pi * hz * t / rate) for hz in (1000, 1500) for f in (np.sin, np.cos)], axis=1)
    windows = []
    for start in range(0, len(x) - n, n):
        coefficients, *_ = np.linalg.lstsq(basis, x[start:start + n], rcond=None)
        residual = x[start:start + n] - basis @ coefficients
        windows.append((start, np.hypot(*coefficients[:2]), np.hypot(*coefficients[2:]), float(np.sqrt(np.mean(residual ** 2)))))
    both = [w for w in windows if w[1] > 0.15 and w[2] > 0.15]
    run = longest = 0; previous = None
    for w in both:
        run = run + 1 if previous is not None and w[0] - previous == n else 1; previous = w[0]; longest = max(longest, run)
    inner = both[1:-1] or both  # the first/last window straddles a stream start/stop
    mixture = {'windows_with_both': len(both), 'longest_both_s': longest * n / rate,
               'amplitude_1000': [round(float(min(w[1] for w in inner)), 4), round(float(max(w[1] for w in inner)), 4)] if both else None,
               'amplitude_1500': [round(float(min(w[2] for w in inner)), 4), round(float(max(w[2] for w in inner)), 4)] if both else None,
               'max_residual': round(max((w[3] for w in inner), default=1.0), 5)}
    result['mixture'] = mixture; print('mixture', mixture)
    assert mixture['longest_both_s'] >= 1.8 and mixture['max_residual'] < 0.005, 'concurrent clients were not mixed cleanly'
    assert 0.23 < mixture['amplitude_1000'][0] and mixture['amplitude_1000'][1] < 0.27 and 0.23 < mixture['amplitude_1500'][0] and mixture['amplitude_1500'][1] < 0.27
    # Phase F: pause leaves silence (no stale audio) and playback resumes.
    paused = [s for s in tones if abs(s['frequency_hz'] - 550) < 3]
    assert paused and paused[0]['silence_runs_over_1ms'] == 1 and 150 <= paused[0]['longest_silence_ms'] <= 450, paused
    assert paused[0]['discontinuities'] <= 2, ('pause/resume should only show the two gap edges', paused)
(WORK / 'result.json').write_text(json.dumps(result, indent=1))
print('PASS native ALSA playback through AC97:', options.backend)
