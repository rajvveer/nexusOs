"""Raw audible-drop count of QEMU audio settings for the desktop launcher (build.py desktop_audio_options), measured.

QEMU's sound output in the launcher is DirectSound (it plays on the host speakers), so the measurement keeps the
launcher's device and audio-timer/buffer settings but swaps the backend for `wav` (same timer-period/buffer-length:
QEMU's audio timer drives the device either way) and counts every discontinuity in the 20 s 440 Hz tone that
/bin/audio64 mode 's' (audio_user.c probe_stability: hw:0,0, refilled every 4 ms) plays: splices, gaps and silence
runs inside the tone, plus how many were exactly QEMU's 2048-frame hda-codec drop (traced hda_audio_overrun).
Configurations run interleaved, round after round, while `--load N` host processes spin (moderate host load).

Usage: python -I tests/measure_audio_drops.py WORK [--rounds 4] [--load 2] [--base TREE] [--only NAME ...] [--qmp-port 4758]
Configurations (device / timer-period us / out.buffer-length us):
  ac97         AC97, 5000, 40000          the launcher's default (desktop_audio_options)
  ac97-10ms    AC97, 10000, 80000
  hda-timer    ich9-intel-hda + hda-duplex (QEMU default use-timer=true), 5000, 40000   (base launcher, NEXUS_AUDIO=hda)
  hda          ich9-intel-hda + hda-duplex use-timer=false, 5000, 40000                (current NEXUS_AUDIO=hda)
  hda-10ms     hda use-timer=false, 10000, 80000
  hda-2ms      hda use-timer=false, 2500, 40000
  base-ac97 / base-hda-timer   the same as ac97 / hda-timer with --base TREE's kernel (the unchanged build)
A stall (the guest saw no consumption for > 100 ms) counts as one raw event too; missing-s = tone absent from the capture.
Prints one line per run and a table: runs, raw events per run (mean, max), runs with any event, host drops."""
from pathlib import Path
import argparse, importlib.util, json, re, shutil, socket, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
CONFIGS = {
    'ac97': (['-device', 'AC97,audiodev=snd0'], 5000, 40000, False),
    'ac97-10ms': (['-device', 'AC97,audiodev=snd0'], 10000, 80000, False),
    'hda-timer': (['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0'], 5000, 40000, False),
    'hda': (['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0,use-timer=false'], 5000, 40000, False),
    'hda-10ms': (['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0,use-timer=false'], 10000, 80000, False),
    'hda-2ms': (['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0,use-timer=false'], 2500, 40000, False),
    'base-ac97': (['-device', 'AC97,audiodev=snd0'], 5000, 40000, True),
    'base-hda-timer': (['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0'], 5000, 40000, True),
}
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', type=Path); parser.add_argument('--rounds', type=int, default=4); parser.add_argument('--load', type=int, default=2)
parser.add_argument('--base', type=Path); parser.add_argument('--throttled', action='store_true', help='leave Windows power throttling on'); parser.add_argument('--only', nargs='+', choices=list(CONFIGS)); parser.add_argument('--qmp-port', type=int, default=4758)
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
names = options.only or [n for n in CONFIGS if not CONFIGS[n][3] or options.base]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


def prepare(tree):
    """apps image of TREE with /bin/audio64 (audio_user.c built against Debian's libasound) and the ALSA config."""
    here, out = tree / 'arch/x86_64', tree / 'arch/x86_64/out'
    build = module('nexus_build_' + str(abs(hash(str(tree)))), here / 'build.py'); fatput = module('fatput', ROOT / 'userland/fatput.py')
    root = ROOT / 'arch/x86_64/out/chrome-root'; libasound = root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
    program = WORK / ('audio_user_%d.elf' % abs(hash(str(tree))))
    subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                    str(here / 'audio_user.c'), str(libasound), '-lm', '-o', str(program)], check=True)
    image = WORK / ('apps_%d.img' % abs(hash(str(tree)))); shutil.copyfile(out / 'apps64.img', image)
    install = ['fatput', str(image), 'put', str(program), 'bin/audio64', 'put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2']
    for source in sorted((root / 'usr/share/alsa').rglob('*')):
        if source.is_file(): install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(root / 'usr/share/alsa').as_posix()]
    marker = WORK / 'marker'; marker.write_bytes(b's'); install += ['put', str(marker), 'nexus/test-audio']
    fatput.main(install)
    return build, out / 'nexus64.img', image


wav_check = module('wav_check', ROOT / 'arch/x86_64/wav_check.py')
trees = {False: prepare(ROOT)}
if options.base: trees[True] = prepare(options.base.resolve())


def run(name, index):
    devices, period, length, base = CONFIGS[name]
    build, kernel, image = trees[base]
    work = WORK / f'{name}_{index}'; work.mkdir(exist_ok=True)
    disk = work / 'disk.img'; shutil.copyfile(image, disk)
    capture, serial, log = work / 'audio.wav', work / 'serial.log', work / 'qemu.log'
    for f in (capture, serial): f.unlink(missing_ok=True)
    command = [QEMU, *build.desktop_cpu_options(False), '-m', '512', '-machine', 'q35',
               '-drive', f'file={kernel},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide', '-boot', 'a',
               '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-msg', 'timestamp=on', '-trace', 'hda_audio_overrun',
               '-audiodev', f'wav,id=snd0,path={capture},out.frequency=48000,timer-period={period},out.buffer-length={length}', *devices,
               '-serial', f'file:{serial}', '-no-reboot', '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    process = subprocess.Popen([str(c) for c in command], stdout=log.open('wb'), stderr=subprocess.STDOUT)
    if not options.throttled: build.unthrottle(process.pid)   # as the desktop launcher does (Windows power throttling off)
    try:
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            text = serial.read_bytes().decode('latin1') if serial.exists() else ''
            if re.search(r'AUDIO CAMPAIGN PASSED|AUDIO TEST FAILED|\[64\] FAIL:|KERNEL_FAULT', text) or process.poll() is not None: break
            time.sleep(.2)
        time.sleep(1)
    finally:
        try:
            with socket.create_connection(('127.0.0.1', options.qmp_port), timeout=5) as link:
                stream = link.makefile('rwb'); stream.readline()
                for request in ({'execute': 'qmp_capabilities'}, {'execute': 'quit'}):
                    stream.write(json.dumps(request).encode() + b'\n'); stream.flush(); stream.readline()
        except OSError: pass
        try: process.wait(30)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        disk.unlink(missing_ok=True)
    text = serial.read_bytes().decode('latin1') if serial.exists() else ''
    stability = re.search(r'AUDIO STABILITY rate=([\d.]+) jitter_ms=([-\d.]+)\.\.([-\d.]+) longest_no_consume_ms=([\d.]+) xruns=(\d+)', text)
    result = {'name': name, 'ok': 'AUDIO CAMPAIGN PASSED' in text, 'guest_xruns': int(stability.group(5)) if stability else None,
              'longest_no_consume_ms': float(stability.group(4)) if stability else None}
    if capture.exists():
        report = wav_check.analyze(capture); samples, rate, _ = wav_check.load(capture); x = samples[:, 0]
        tones = [s for s in report['segments'] if abs(s['frequency_hz'] - 440) < 5 and s['duration_s'] >= 0.2]
        breaks = [b for s in tones for b in wav_check.segment_breaks(x, rate, s, [440], exact=True)]
        silences = sum(s['silence_runs_over_1ms'] for s in tones)
        result.update(tone_s=round(sum(s['duration_s'] for s in tones), 2), pieces=len(tones), splices=len(breaks), silences=silences,
                      raw=len(breaks) + silences + max(len(tones) - 1, 0) + (1 if (result['longest_no_consume_ms'] or 0) > 100 else 0),
                      missing_s=round(max(0.0, 20.0 - sum(s['duration_s'] for s in tones)), 2), host_drops=sum(1 for b in breaks if b.get('host_drop')),
                      overruns=len(wav_check.overrun_events(log)))
        capture.unlink()
    print(json.dumps(result), flush=True)
    return result


load = [subprocess.Popen([sys.executable, '-I', '-c', 'while True: pass']) for _ in range(options.load)]   # moderate host load
results = []
try:
    for index in range(options.rounds):
        for name in names: results.append(run(name, index))
finally:
    for p in load: p.kill()
print('\nconfiguration     runs  ok  raw/run(mean,max)  runs-with-events  host-drops  overruns  longest-no-consume-ms(max)  missing-s(max)')
for name in names:
    rs = [r for r in results if r['name'] == name and 'raw' in r]
    if not rs: print(f'{name:16} no capture'); continue
    raw = [r['raw'] for r in rs]
    print(f'{name:16} {len(rs):5} {sum(r["ok"] for r in rs):3}  {sum(raw) / len(raw):7.2f} {max(raw):4}      {sum(1 for v in raw if v):10}        '
          f'{sum(r["host_drops"] for r in rs):5}  {sum(r["overruns"] for r in rs):8}  {max((r["longest_no_consume_ms"] or 0) for r in rs):8.1f}  {max(r["missing_s"] for r in rs):8.2f}')
(WORK / 'results.json').write_text(json.dumps(results, indent=1))
