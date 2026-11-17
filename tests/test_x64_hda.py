"""Native ALSA playback through the x86-64 Intel High Definition Audio driver (arch/x86_64/hda.c), captured with
QEMU's wav backend and analyzed like tests/test_x64_audio.py.

Usage: python -I tests/test_x64_hda.py [WORK] [--tcg] [--qmp-port N] [--only CASE ...]
Builds /bin/audio64 against the unmodified Debian libasound.so.2 (from out/chrome-root), installs it with alsa.conf
into a private copy of apps64.img and boots the kernel's /nexus/test-audio campaign once per case:
  pc       intel-hda (ICH6 8086:2668) + hda-output on the pc machine: the full campaign (four tones, forced-underrun
           gap, two mixed clients, pause/resume), checked tone by tone for gaps and discontinuities
  q35      ich9-intel-hda (ICH9 8086:293E) + hda-duplex on q35: the full campaign
  micro    intel-hda + hda-micro (speaker pin) with the Immediate Command interface forced (hda.immediate): two tones
           3 s apart, so the idle DMA is released and restarted (checked in QEMU's hda_audio_running trace)
  ac97     AC'97 and intel-hda both present: AC'97 keeps the PCM, the HDA controller is left alone (two tones)
  nocard   no audio device: the kernel boots, /dev/snd is absent and ALSA opens fail cleanly
  nocodec  intel-hda without a codec: the driver gives up cleanly; then the nocard checks
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
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CASES = ['pc', 'q35', 'micro', 'ac97', 'nocard', 'nocodec']
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=nexus_paths.runs('native64-hda'))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4581)
parser.add_argument('--only', nargs='+', choices=CASES, default=CASES)
parser.add_argument('--no-timer', action='store_true', help="QEMU codecs with use-timer=false: DMA pulled by the audio backend (bursty LPIB)")
parser.add_argument('--trace', action='append', default=[], help='extra QEMU trace event (diagnosis)')
parser.add_argument('--audiodev', help='diagnosis: another -audiodev (id snd0) instead of the wav capture; only guest checks + QEMU overruns')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
ext4 = os.environ.get('NEXUS_ROOT_FS') == 'ext4'
fatput = module('fatput', ROOT / 'userland' / ('ext4put.py' if ext4 else 'fatput.py'))
wav_check = module('wav_check', HERE / 'wav_check.py')
root = OUT / 'chrome-root'
libasound = root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
if not libasound.exists():
    raise SystemExit('SKIP: no Debian libasound in out/chrome-root (install Chrome first)')

program = OUT / 'audio_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'audio_user.c'), str(libasound), '-lm', '-o', str(program)], check=True)
base = WORK / 'base.img'; shutil.copyfile(OUT / ('apps64-ext4.img' if ext4 else 'apps64.img'), base)
install = ['fatput', str(base), 'put', str(program), 'bin/audio64', 'put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2']
for source in sorted((root / 'usr/share/alsa').rglob('*')):
    if source.is_file():
        install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(root / 'usr/share/alsa').as_posix()]
fatput.main(install)


def boot(case, devices, mode, machine=(), cmdline=None):
    """Boot one VM through the audio campaign; returns (serial text, wav capture or None). Deletes its disk copy."""
    if options.no_timer: devices = [d + ',use-timer=false' if d.startswith('hda-') else d for d in devices]
    work = WORK / case; work.mkdir(exist_ok=True)
    disk = work / 'disk.img'; shutil.copyfile(base, disk)
    marker = work / 'marker'; marker.write_bytes(mode.encode())
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-audio'])
    capture = work / 'audio.wav'; capture.unlink(missing_ok=True)
    serial = work / 'serial.log'; serial.unlink(missing_ok=True)
    audio = ['-audiodev', options.audiodev or f'wav,id=snd0,path={capture},out.frequency=48000'] if any('snd0' in d for d in devices) else []
    # QEMU's hda-codec drops its whole 8 KiB output buffer when the host audio backend falls behind (host load):
    # trace it with timestamps so a host-side splice can be told apart from a driver bug (wav_check.explain_breaks).
    audio += ['-msg', 'timestamp=on', '-trace', 'hda_audio_overrun', '-trace', 'hda_audio_running'] if any('hda' in d for d in devices) else []
    for event in options.trace: audio += ['-trace', event]
    command = [QEMU, *build.desktop_cpu_options(options.tcg), '-m', '512', *machine,
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', *audio, *devices,
               *(['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}'] if cmdline else []),
               '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    (work / 'cmd.json').write_text(json.dumps([str(c) for c in command], indent=1))
    process = subprocess.Popen([str(c) for c in command], stdout=(work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    print(case, 'VM', process.pid, flush=True)
    try:
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            text = serial.read_bytes().decode('latin1') if serial.exists() else ''
            if re.search(r'\[64\] AUDIO CAMPAIGN PASSED|\[64\] AUDIO TEST FAILED|\[64\] FAIL:|KERNEL_FAULT', text) or process.poll() is not None:
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
        disk.unlink(missing_ok=True)
    text = serial.read_bytes().decode('latin1') if serial.exists() else ''
    lines = [line for line in text.splitlines() if any(k in line for k in ('AUDIO', 'SOUND', 'AC97', 'HDA', 'ALSA'))]
    print('\n'.join(lines), flush=True)
    assert 'KERNEL_FAULT' not in text and '[64] FAIL:' not in text, (case, 'kernel failure (see serial.log)')
    assert '[64] AUDIO TEST PASSED' in text and '[64] AUDIO CAMPAIGN PASSED' in text, (case, 'guest test failed (see serial.log)')
    return text, lines, (capture if capture.exists() else None)


class Attribution:
    """Splices in the capture vs. QEMU's traced hda-codec overruns. A splice is host-side (QEMU dropped its 8 KiB
    stream buffer) only if the signal after it is exactly the one before it advanced by 2048 frames AND a traced
    overrun not yet used lies near it on the capture timeline; every other splice is the guest's fault."""
    def __init__(self, capture, qemu_log):
        import numpy as np
        self.np = np; samples, self.rate, _ = wav_check.load(capture); self.x = samples[:, 0]
        self.overruns = wav_check.overrun_events(qemu_log); self.free = list(self.overruns); self.host = []; self.unexplained = []

    def segment(self, s, frequency, edges=0):
        """True when a tone segment has no unexplained break beyond `edges` expected ones (a pause's two gap edges)."""
        host, other = wav_check.explain_breaks(wav_check.segment_breaks(self.x, self.rate, s, [frequency]), self.free)
        self.host += host
        if len(other) > edges: self.unexplained += other
        return len(other) <= edges

    def mixture_windows(self, windows, n):
        """Windows of the two-client mixture whose residual is explained by a host-side drop are left out."""
        bad = [w for w in windows if w[3] >= 0.005]
        if not bad: return windows
        lo, hi = bad[0][0] - 10 * n, bad[-1][0] + 11 * n
        points = wav_check.splice_points(self.x, self.rate, [1000, 1500], max(lo, 0), min(hi, len(self.x)), 0.5)
        breaks = []
        for k, p in enumerate(points):
            a = points[k - 1] + 8 if k else max(lo, 0); b = points[k + 1] - 8 if k + 1 < len(points) else min(hi, len(self.x))
            breaks.append(wav_check.classify_splice(self.x, self.rate, p, [1000, 1500], a, b, exact=True))
        host, other = wav_check.explain_breaks(breaks, self.free)
        self.host += host; self.unexplained += [b for b in other if b]
        return [w for w in windows if not any(w[0] - n <= h['index'] < w[0] + 2 * n for h in host)]

    def report(self):
        print(f'host-side drops: {len(self.host)} splice(s) of exactly {wav_check.QEMU_HDA_DROP_FRAMES} frames matched to '
              f'{len(self.overruns)} traced QEMU hda_audio_overrun(s) {self.overruns}', [(h['t'], h['overrun_t']) for h in self.host], flush=True)
        return {'overruns': self.overruns, 'host_drops': self.host, 'unexplained': self.unexplained}


def check_full(capture, qemu_log):
    """The tests/test_x64_audio.py criteria: tones A-D, the forced-underrun gap, two mixed clients, pause/resume;
    splices that are QEMU's traced host-side buffer drops (Attribution) are reported, not failed."""
    if options.audiodev: return {'overruns': wav_check.overrun_events(qemu_log)}
    report = wav_check.analyze(capture); attribution = Attribution(capture, qemu_log)
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
            clean = attribution.segment(s, s['frequency_hz'])
            assert clean and not s['silence_runs_over_1ms'] and not s['dropout_windows_10ms'], ('glitch', s, attribution.unexplained)
    assert tones[2]['silence_runs_over_1ms'] >= 1, 'forced underrun should leave a silent gap, not stale audio'
    assert report['channels_identical'] and report['peak'] > 0.2
    import numpy as np
    x, rate = attribution.x, attribution.rate; n = rate // 50; t = np.arange(n)
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
    inner = attribution.mixture_windows(both[1:-1] or both, n)
    mixture = {'windows_with_both': len(both), 'longest_both_s': longest * n / rate,
               'amplitude_1000': [round(float(min(w[1] for w in inner)), 4), round(float(max(w[1] for w in inner)), 4)] if both else None,
               'amplitude_1500': [round(float(min(w[2] for w in inner)), 4), round(float(max(w[2] for w in inner)), 4)] if both else None,
               'max_residual': round(max((w[3] for w in inner), default=1.0), 5)}
    print('mixture', mixture)
    assert mixture['longest_both_s'] >= 1.8 and mixture['max_residual'] < 0.005, 'concurrent clients were not mixed cleanly'
    assert 0.23 < mixture['amplitude_1000'][0] and mixture['amplitude_1000'][1] < 0.27 and 0.23 < mixture['amplitude_1500'][0] and mixture['amplitude_1500'][1] < 0.27
    paused = [s for s in tones if abs(s['frequency_hz'] - 550) < 3]
    assert paused and paused[0]['silence_runs_over_1ms'] == 1 and 150 <= paused[0]['longest_silence_ms'] <= 450, paused
    assert attribution.segment(paused[0], 550, edges=2), ('pause/resume should only show the two gap edges', paused, attribution.unexplained)
    return {'wav': report, 'mixture': mixture, 'attribution': attribution.report()}


def check_tones(capture, qemu_log):
    """Mode 'q': 440 Hz then (after 3 s idle) 660 Hz, 1 s each at amplitude 0.5, without gaps, splices or level drops
    (QEMU's traced host-side buffer drops excepted, see Attribution)."""
    if options.audiodev: return {'overruns': wav_check.overrun_events(qemu_log)}
    report = wav_check.analyze(capture); attribution = Attribution(capture, qemu_log)
    tones = [s for s in report['segments'] if s['duration_s'] >= 0.3]
    print({k: report[k] for k in ('duration_s', 'peak', 'channels_identical')}, [(s['frequency_hz'], s['duration_s']) for s in tones])
    assert len(tones) == 2, tones
    for s, frequency in zip(tones, (440, 660)):
        assert abs(s['frequency_hz'] - frequency) < frequency * 0.005 and s['duration_s'] >= 0.9, s
        clean = attribution.segment(s, s['frequency_hz'])
        assert clean and not s['silence_runs_over_1ms'] and not s['dropout_windows_10ms'], ('glitch', s, attribution.unexplained)
    assert report['channels_identical'] and 0.45 < report['peak'] <= 0.5001, report['peak']
    return {'wav': report, 'attribution': attribution.report()}


def run_case(case):
    if case == 'pc':
        text, lines, capture = boot(case, ['-device', 'intel-hda', '-device', 'hda-output,audiodev=snd0'], '1')
        assert 'HDA 8086:2668 codec 0 vendor 1af40012 out pin 0x03 <- dac 0x02 (line-out)' in text, 'expected route'
        assert '[64] HDA native PCM ready: NexusOS HD Audio 8086:2668' in text and 'CORB/RIRB)' in text, 'HDA engine'
        assert '[64] AUDIO card0 name=NexusOS HDA' in text and 'AC97 native' not in text
        results[case] = {'guest': lines, **check_full(capture, WORK / case / 'qemu.log')}
    elif case == 'q35':
        text, lines, capture = boot(case, ['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0'], '1', ['-machine', 'q35'])
        assert 'HDA 8086:293E codec 0 vendor 1af40022 out pin 0x03 <- dac 0x02 (line-out)' in text, 'expected route'
        assert '[64] HDA native PCM ready: NexusOS HD Audio 8086:293E' in text and '[64] AUDIO card0 name=NexusOS HDA' in text
        results[case] = {'guest': lines, **check_full(capture, WORK / case / 'qemu.log')}
    elif case == 'micro':
        text, lines, capture = boot(case, ['-device', 'intel-hda', '-device', 'hda-micro,audiodev=snd0'], 'q', cmdline='hda.immediate')
        assert re.search(r'HDA 8086:2668 codec 0 vendor 1af400\w\w out pin 0x\w\w <- dac 0x\w\w \(speaker\)', text), 'expected speaker route'
        assert 'immediate commands)' in text, 'Immediate Command interface not used'
        # The 3 s gap lets the driver release the idle DMA (QEMU: stream stopped) and restart it for the second tone.
        running = re.findall(r'hda_audio_running st \w+, nr \d+, run (\d)', (WORK / case / 'qemu.log').read_text(errors='replace'))
        assert running[:3] == ['1', '0', '1'], ('DMA idle release and restart expected', running)
        results[case] = {'guest': lines, 'dma_runs': running, **check_tones(capture, WORK / case / 'qemu.log')}
    elif case == 'ac97':
        text, lines, capture = boot(case, ['-device', 'AC97,audiodev=snd0', '-device', 'intel-hda', '-device', 'hda-output,audiodev=snd0'], 'q')
        assert '[64] AC97 native PCM ready' in text and 'HDA native PCM ready' not in text and '[64] HDA' not in text, 'AC97 must win'
        assert '[64] AUDIO card0 name=NexusOS AC97' in text
        results[case] = {'guest': lines, **check_tones(capture, WORK / case / 'qemu.log')}
    elif case in ('nocard', 'nocodec'):
        text, lines, _ = boot(case, ['-device', 'intel-hda'] if case == 'nocodec' else [], 'n')
        assert 'native PCM ready' not in text, 'no PCM device expected'
        if case == 'nocodec':
            assert 'HDA 8086:2668 no codec attached (STATESTS=0), giving up' in text, 'driver must give up on a codec-less controller'
        assert text.count('[64] AUDIO nocard open') == 2, 'both opens must have been attempted'
        results[case] = {'guest': lines}


results = {}
try:
    for case in options.only:
        for attempt in (1, 2):
            try:
                run_case(case); break
            except AssertionError:
                log = WORK / case / 'qemu.log'
                overruns = log.read_text(errors='replace').count('hda_audio_overrun') if log.exists() else 0
                if attempt == 1 and overruns:   # only a drop QEMU itself reported (host load) earns one retry
                    print(f'{case}: QEMU reported {overruns} host-side hda-codec overrun(s) (audio dropped by the backend); retrying once', flush=True)
                    results.setdefault('retried', []).append(case)
                    continue
                raise
        print('PASS', case, flush=True)
finally:
    base.unlink(missing_ok=True)
(WORK / 'result.json').write_text(json.dumps(results, indent=1))
print('PASS native ALSA playback through Intel HDA:', ' '.join(options.only))
