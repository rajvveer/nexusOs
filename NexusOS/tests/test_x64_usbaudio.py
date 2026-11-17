"""USB Audio Class playback through the native xHCI isochronous pipes (arch/x86_64/uaudio.c) on QEMU's usb-audio (UAC1,
full speed, 48 kHz S16 stereo, feature unit with mute + volume), captured with QEMU's wav backend and analyzed.

Usage: python -I tests/test_x64_usbaudio.py [WORK] [--tcg] [--smp N] [--qmp-port N] [--only CASE ...]
Builds /bin/audio64 from arch/x86_64/uaudio_user.c against the unmodified Debian libasound.so.2 (out/chrome-root) and
boots the kernel's /nexus/test-audio campaign once per case:
  usb      q35 + qemu-xhci + usb-audio only: the USB device is card 0. 440 Hz 2 s (default), 660 Hz 1 s (hw:0,0 mmap),
           880 Hz at -12 dB (SET_CUR reached the device: QEMU's volume mapping), 550 Hz muted (absent), 330 Hz at
           44.1 kHz through the plug; hostile hw_params refused; consumption rate from the guest's delay fit
  both     ich9-intel-hda + hda-duplex (card 0) and usb-audio (card 1) playing at the same time, each capture only
           holds its own tone (880 Hz HDA, 440 + 660 Hz USB)
  stay     usb-audio plugged while the guest program runs and still attached when it exits (the kernel's per-program
           allocation audit must not count the driver's device memory)
  hotplug  usb-audio plugged after boot (QMP device_add), unplugged mid-stream (the guest's writei fails with
           ENODEV, the nodes and the card disappear, uevents), re-plugged (plays 660 Hz into a second capture),
           unplugged again; no kernel memory left behind (the campaign's page check)
  default  Aurora's default-device policy (aurora_sound.c, standalone) with HDA + a hot-plugged usb-audio: "default"
           follows /etc/asound.conf (auto = newest device, explicit choice, fallback after unplug, hand-written file
           kept); each tone must land in the right capture
QEMU's usb-audio drops a whole 192-byte packet when its 32-packet buffer is full (host-side; debug=2 reports
"output overrun"): a splice of exactly 48*k frames with as many reported overruns is attributed to the host.
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

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CASES = ['usb', 'both', 'hotplug', 'default', 'stay']
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\native64-usbaudio'))
parser.add_argument('--tcg', action='store_true'); parser.add_argument('--qmp-port', type=int, default=4750)
parser.add_argument('--smp', type=int, default=1)
parser.add_argument('--throttled', action='store_true', help='leave Windows power throttling on for the QEMU process')
parser.add_argument('--only', nargs='+', choices=CASES, default=CASES)
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland' / 'fatput.py')
wav_check = module('wav_check', HERE / 'wav_check.py')
root = OUT / 'chrome-root'
libasound = root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
if not libasound.exists():
    raise SystemExit('SKIP: no Debian libasound in out/chrome-root (install Chrome first)')
program = WORK / 'uaudio_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                '-DAURORA_SOUND_STANDALONE=1', str(HERE / 'uaudio_user.c'), str(HERE / 'aurora_sound.c'), str(libasound), '-lm', '-o', str(program)], check=True)
base = WORK / 'base.img'; shutil.copyfile(OUT / 'apps64.img', base)
install = ['fatput', str(base), 'put', str(program), 'bin/audio64', 'put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2']
for source in sorted((root / 'usr/share/alsa').rglob('*')):
    if source.is_file():
        install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(root / 'usr/share/alsa').as_posix()]
fatput.main(install)


class QMP:
    def __init__(self, port):
        deadline = time.monotonic() + 20
        while True:
            try: self.link = socket.create_connection(('127.0.0.1', port), timeout=10); break
            except OSError:
                if time.monotonic() > deadline: raise
                time.sleep(.2)
        self.stream = self.link.makefile('rwb'); self.stream.readline(); self.cmd('qmp_capabilities')

    def cmd(self, name, arguments=None):
        self.stream.write(json.dumps({'execute': name, **({'arguments': arguments} if arguments else {})}).encode() + b'\n'); self.stream.flush()
        while True:
            reply = json.loads(self.stream.readline())
            if 'return' in reply or 'error' in reply: return reply


def serial_text(path): return path.read_bytes().decode('latin1') if path.exists() else ''


def wait_for(serial, pattern, seconds, process):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        text = serial_text(serial)
        if re.search(pattern, text): return text
        if re.search(r'KERNEL_FAULT|\[64\] FAIL:|AUDIO CAMPAIGN PASSED', text) or process.poll() is not None: break
        time.sleep(.2)
    raise AssertionError(f'timeout waiting for {pattern!r}; serial tail:\n' + serial_text(serial)[-3000:])


def boot(case, mode, devices, captures, machine=('-machine', 'q35'), during=None):
    work = WORK / case; work.mkdir(exist_ok=True)
    disk = work / 'disk.img'; shutil.copyfile(base, disk)
    marker = work / 'marker'; marker.write_bytes(mode.encode())
    fatput.main(['fatput', str(disk), 'put', str(marker), 'nexus/test-audio'])
    serial = work / 'serial.log'; serial.unlink(missing_ok=True)
    audio = []
    for name in captures:
        (work / f'{name}.wav').unlink(missing_ok=True)
        audio += ['-audiodev', f'wav,id={name},path={work / (name + ".wav")},out.frequency=48000']
    command = [QEMU, *build.desktop_cpu_options(options.tcg), '-m', '1024', '-smp', str(options.smp), *machine,
               '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
               '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', *audio, '-device', 'qemu-xhci,id=xhci', *devices,
               '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    (work / 'cmd.json').write_text(json.dumps([str(c) for c in command], indent=1))
    process = subprocess.Popen([str(c) for c in command], stdout=(work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
    print(case, 'VM', process.pid, flush=True)
    # As the desktop launcher does (build.py unthrottle): Windows may run a hidden QEMU at 15.6 ms timer resolution, and
    # QEMU's xHCI then starts at most one Start-Isoch-ASAP TD per timer tick (~8x too slow for a 1 ms audio endpoint).
    if not options.throttled: build.unthrottle(process.pid)
    try:
        if during: during(serial, process)
        deadline = time.monotonic() + 240; last_size, last_change, dumped = -1, time.monotonic(), False
        while time.monotonic() < deadline:
            text = serial_text(serial)
            if re.search(r'\[64\] AUDIO CAMPAIGN PASSED|\[64\] AUDIO TEST FAILED|\[64\] FAIL:|KERNEL_FAULT', text) or process.poll() is not None: break
            if len(text) != last_size: last_size, last_change = len(text), time.monotonic()
            elif not dumped and time.monotonic() - last_change > 30:   # stuck: CPU state for the post-mortem
                dumped = True; q = QMP(options.qmp_port); dump = []
                for _ in range(3):
                    dump.append(q.cmd('human-monitor-command', {'command-line': 'info registers -a'}).get('return', '')); time.sleep(.5)
                (work / 'hang-registers.txt').write_text('\n=====\n'.join(dump)); print(case, 'guest stuck: registers in hang-registers.txt', flush=True)
            time.sleep(.2)
        time.sleep(1)
    finally:
        try: QMP(options.qmp_port).cmd('quit')
        except OSError: pass
        try: process.wait(30)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        disk.unlink(missing_ok=True)
    text = serial_text(serial)
    lines = [line for line in text.splitlines() if any(k in line for k in ('UAUDIO', 'AUDIO', 'SOUND', 'USB'))]
    print('\n'.join(lines), flush=True)
    assert 'KERNEL_FAULT' not in text and '[64] FAIL:' not in text and 'USER_FAULT' not in text, (case, 'kernel failure (see serial.log)')
    assert '[64] AUDIO TEST PASSED' in text and '[64] AUDIO CAMPAIGN PASSED' in text, (case, 'guest test failed (see serial.log)')
    return text, work


def host_overruns(work):
    """QEMU usb-audio (debug=2) reports every packet it could not store ("output overrun (N bytes)")."""
    log = (work / 'qemu.log').read_text(errors='replace') if (work / 'qemu.log').exists() else ''
    return [int(n) for n in re.findall(r'usb-audio: output overrun \((\d+) bytes\)', log)]


def tones(capture, work, expected):
    """expected: [(frequency, min duration, amplitude)]; splices must be host-side packet drops (48*k frames)."""
    report = wav_check.analyze(capture)
    found = [s for s in report['segments'] if s['duration_s'] >= 0.3]
    print(capture.name, json.dumps({k: report[k] for k in ('duration_s', 'peak', 'channels_identical')}),
          [(s['frequency_hz'], s['duration_s'], round(s['rms'] * 2 ** .5, 3), s['discontinuities']) for s in found], flush=True)
    assert [round(s['frequency_hz'] / 10) * 10 for s in found] == [f for f, _, _ in expected], ('tone sequence', capture.name, found)
    samples, rate, _ = wav_check.load(capture); x = samples[:, 0]
    drops = host_overruns(work); dropped_frames = sum(drops) // 4; explained = 0
    for s, (frequency, minimum, amplitude) in zip(found, expected):
        assert abs(s['frequency_hz'] - frequency) < frequency * 0.005 and s['duration_s'] >= minimum, (capture.name, s)
        assert abs(s['rms'] * 2 ** .5 - amplitude) < 0.02 * max(amplitude, 0.1) + 0.003, ('level', capture.name, s, amplitude)
        assert not s['silence_runs_over_1ms'] and not s['dropout_windows_10ms'], ('gap', capture.name, s)
        for b in wav_check.segment_breaks(x, rate, s, [s['frequency_hz']]):
            ok = False
            for k in range(1, 33):
                c = wav_check.classify_splice(x, rate, b['index'], [s['frequency_hz']], b['index'] - rate // 5, b['index'] + rate // 5, skip=48 * k)
                if c and c['host_drop'] and explained + 48 * k <= dropped_frames: explained += 48 * k; ok = True; break
            assert ok, ('splice not explained by a reported usb-audio overrun', capture.name, b, drops)
    assert report['channels_identical']
    return report


results = {}


def run_case(case):
    if case == 'usb':
        text, work = boot(case, 'u', ['-device', 'usb-audio,audiodev=snd1,bus=xhci.0,debug=2'], ['snd1'])
        assert re.search(r'USB xhci0 port \d+ full-speed 46f4:0002 slot \d+: audio playback: interface 1 alt 1 stereo 16-bit 48000 Hz, synchronous, volume, mute', text), 'UAC1 alt choice'
        assert '[64] SOUND card 0 added: QEMU USB Audio (USB Audio 1.0, full speed)' in text
        # QEMU maps the feature unit's 1/256 dB SET_CUR value linearly onto its 0..255 voice volume (not in dB) and logs
        # it (debug): the 880 Hz level relative to 440 Hz must follow the values our -12 dB / 0 dB requests produced.
        volumes = [int(v) for _, v in re.findall(r'usb-audio: mute (\d), vol\[0\] (\d+)', (work / 'qemu.log').read_text(errors='replace'))]
        distinct = [v for i, v in enumerate(volumes) if not i or v != volumes[i - 1]]
        assert len(distinct) >= 3 and distinct[1] < distinct[0] and distinct[2] == distinct[0], ('SET_CUR volume requests', volumes)
        ratio = distinct[1] / distinct[0]
        assert re.search(r'usb-audio: mute 1, vol', (work / 'qemu.log').read_text(errors='replace')), 'mute request'
        level = 0.5 * distinct[0] / 255        # QEMU's mixer scales by vol/255 (0 dB = 240)
        report = tones(work / 'snd1.wav', work, [(440, 1.9, level), (660, 0.9, level), (880, 0.9, level * ratio), (330, 0.9, level)])
        fit = float(re.search(r'UAUDIO played default 440 Hz .* rate_fit=([\d.]+)', text).group(1))
        results[case] = {'rate_fit': fit, 'volume_ratio_expected': ratio, 'wav': report, 'host_overruns': host_overruns(work)}
    elif case == 'both':
        text, work = boot(case, 'b', ['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=hda,use-timer=false',
                                      '-device', 'usb-audio,audiodev=snd1,bus=xhci.0,debug=2'], ['hda', 'snd1'])
        assert '[64] SOUND card 1 added: QEMU USB Audio' in text
        usb = tones(work / 'snd1.wav', work, [(440, 1.9, 0.5 * 240 / 255), (660, 0.9, 0.5 * 240 / 255)])   # QEMU default volume 240
        hda = tones(work / 'hda.wav', work, [(880, 1.9, 0.5)])
        results[case] = {'usb': usb, 'hda': hda}
    elif case == 'stay':      # plugged during the program and still attached at its exit: no allocation-audit failure
        def during(serial, process):
            q = QMP(options.qmp_port)
            wait_for(serial, r'\[64\] UAUDIO READY', 180, process)
            assert 'return' in q.cmd('device_add', {'driver': 'usb-audio', 'id': 'ua', 'audiodev': 'snd1', 'bus': 'xhci.0', 'debug': 2})
        text, work = boot(case, 's', [], ['snd1'], during=during)
        assert '[64] UAUDIO STAY exiting with the card attached' in text and 'allocation ownership leak' not in text
        results[case] = tones(work / 'snd1.wav', work, [(550, 0.9, 0.5 * 240 / 255)])
    elif case == 'hotplug':
        def during(serial, process):
            q = QMP(options.qmp_port)
            wait_for(serial, r'\[64\] UAUDIO READY', 180, process)
            assert 'return' in q.cmd('device_add', {'driver': 'usb-audio', 'id': 'ua', 'audiodev': 'snd1', 'bus': 'xhci.0', 'debug': 2})
            wait_for(serial, r'\[64\] UAUDIO PLAYING', 60, process)
            time.sleep(1.0)
            assert 'return' in q.cmd('device_del', {'id': 'ua'})
            wait_for(serial, r'\[64\] UAUDIO WAIT-REPLUG', 60, process)
            time.sleep(.5)
            assert 'return' in q.cmd('device_add', {'driver': 'usb-audio', 'id': 'ub', 'audiodev': 'snd2', 'bus': 'xhci.0', 'debug': 2})
            wait_for(serial, r'\[64\] UAUDIO WAIT-UNPLUG', 60, process)
            assert 'return' in q.cmd('device_del', {'id': 'ub'})
        text, work = boot(case, 'h', [], ['snd1', 'snd2'], during=during)
        assert text.count('[64] SOUND card 0 added: QEMU USB Audio') == 2 and text.count('[64] SOUND card 0 removed') == 2, 'two plug cycles'
        first = wav_check.analyze(work / 'snd1.wav')
        segments = [s for s in first['segments'] if s['duration_s'] >= 0.3]
        assert segments and abs(segments[0]['frequency_hz'] - 440) < 3 and segments[0]['duration_s'] >= 0.9, ('first plug', segments)
        second = tones(work / 'snd2.wav', work, [(660, 0.9, 0.5 * 240 / 255)])
        results[case] = {'first': first, 'second': second}


def run_default():
    def during(serial, process):
        q = QMP(options.qmp_port)
        wait_for(serial, r'\[64\] UAUDIO READY', 180, process)
        assert 'return' in q.cmd('device_add', {'driver': 'usb-audio', 'id': 'ua', 'audiodev': 'snd1', 'bus': 'xhci.0', 'debug': 2})
        wait_for(serial, r'\[64\] UAUDIO WAIT-UNPLUG', 60, process)
        assert 'return' in q.cmd('device_del', {'id': 'ua'})
    text, work = boot('default', 'd', ['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=hda,use-timer=false'], ['hda', 'snd1'], during=during)
    hda = tones(work / 'hda.wav', work, [(440, 0.9, 0.5), (880, 0.9, 0.5), (330, 0.9, 0.5)])
    usb = tones(work / 'snd1.wav', work, [(660, 0.9, 0.5 * 240 / 255)])
    results['default'] = {'hda': hda, 'usb': usb}


try:
    for case in options.only:
        if case == 'default': run_default()
        else: run_case(case)
        print('PASS', case, flush=True)
finally:
    base.unlink(missing_ok=True)
    for f in WORK.rglob('disk.img'): f.unlink(missing_ok=True)
(WORK / 'result.json').write_text(json.dumps(results, indent=1, default=str))
print('PASS USB Audio Class playback:', ' '.join(options.only))
