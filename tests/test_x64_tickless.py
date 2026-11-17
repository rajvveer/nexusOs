"""Tickless idle (NO_HZ idle) end to end on QEMU: idle wake-ups, timer accuracy, hostile timer inputs, busy CPUs.

Usage: python -I tests/test_x64_tickless.py [WORK] [--smp 1,2] [--whpx] [--cases measure,accuracy,devices,nohz]
       [--qmp-port 4760] [--seconds 8] [--compare-kernel DIR]

The kernel's power campaign (/nexus/test-power "t <modes>") runs arch/x86_64/tickless_user.c as /bin/power64:
  measure   an idle machine for --seconds: per CPU the idle entries (wake-ups) and timer interrupts per second from
            /proc/interrupts, idle residency, /proc/timer_list; booted twice, tickless (default) and `nohz=off`
            (the periodic 1 kHz PIT on the boot CPU and 250 Hz LAPIC ticks on the others, as before); asserts that the
            tickless boot CPU takes < 60 timer interrupts/s and each application processor < 10 (vs ~1000 / ~250),
            and reports the QEMU process's host CPU time per idle second for both.
  accuracy  modes a (nanosleep, clock_nanosleep abs/realtime/boottime, poll, epoll, select, futex, ITIMER_REAL,
            alarm, timerfd/POSIX timers when present, long-idle wake, REALTIME vs MONOTONIC), h (hostile inputs,
            signal-interrupted sleeps, 24 concurrent sleepers) and b (sleepers beside busy-spinning threads).
  devices   the same accuracy run on every one-shot event device: tick=tsc-deadline (when the CPU has it),
            tick=lapic, tick=pit, and the legacy 8259 PIC mode (noapic: PIT one-shot only).
  nohz      nohz=off keeps the old periodic ticks and the accuracy run still passes.
  audio     a 440 Hz tone played through Debian's libasound (out/chrome-root) on AC'97 and on HD Audio (QEMU wav
            capture) for --seconds per phase, blocking 500 ms-buffer writes then poll-paced 100 ms-buffer writes,
            measured like `measure`: asserts the boot CPU's wake-ups/timer interrupts stay far below the 1 kHz the
            sound engine polled at before, no underruns, and a gap-free 440 Hz tone in the capture (wav_check.py).
  audio-sleep  (not in the default list) a player across suspend-to-idle and suspend-to-RAM on AC'97 and HD Audio:
            the 440/660/880 Hz tones appear in order (nothing stale replayed) and the audio after the S3 resume is clean.
--compare-kernel DIR: also measure another build's out/ (nexus64.img + apps64.img), e.g. the unchanged base tree.
Every case runs on each --smp value."""
from pathlib import Path
import argparse, ctypes, importlib.util, os, re, shutil, subprocess, sys, time

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'arch/x86_64'
OUT = HERE / 'out'
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('work', nargs='?', type=Path, default=Path(r'D:\nexustest\wave2\ws_power2_runs\tickless'))
parser.add_argument('--smp', default='1,2')
parser.add_argument('--whpx', action='store_true')
parser.add_argument('--cases', default='measure,accuracy,devices,nohz,audio')
parser.add_argument('--qmp-port', type=int, default=4760)
parser.add_argument('--seconds', type=int, default=8)
parser.add_argument('--compare-kernel', type=Path)
parser.add_argument('--modes', default='a h b', help='modes of the accuracy case (tickless_user.c)')
parser.add_argument('--audio-limit', type=float, default=150, help='boot CPU wake-ups (and timer irqs) per second allowed while playing')
options = parser.parse_args()
WORK = options.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded); return loaded


build = module('nexus_build', HERE / 'build.py')
fatput = module('fatput', ROOT / 'userland/fatput.py')
program = WORK / 'tickless_user.elf'
subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-pthread', '-Wall', '-Wextra', '-Werror',
                str(HERE / 'tickless_user.c'), '-o', str(program)], check=True)


class FILETIME(ctypes.Structure):
    _fields_ = [('low', ctypes.c_uint32), ('high', ctypes.c_uint32)]


def cpu_seconds(vm):
    """Host CPU time (user + kernel) of the QEMU process so far."""
    c, e, k, u = FILETIME(), FILETIME(), FILETIME(), FILETIME()
    if not ctypes.windll.kernel32.GetProcessTimes(ctypes.c_void_p(int(vm._handle)), ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
        return 0.0
    return ((k.high << 32 | k.low) + (u.high << 32 | u.low)) / 1e7


def run(tag, modes, smp, cmdline='', out=OUT, timeout=600, measure_host=False, cpu_extra='', extra=(), install=()):
    disk = WORK / f'{tag}.img'; serial = WORK / f'{tag}.log'; marker = WORK / f'{tag}.marker'
    shutil.copyfile(out / 'apps64.img', disk); marker.write_bytes(f't {modes}'.encode())
    fatput.main(['fatput', str(disk), 'put', str(program), 'bin/power64', 'put', str(marker), 'nexus/test-power', *install])
    serial.unlink(missing_ok=True)
    accel = ['-accel', 'whpx,kernel-irqchip=off'] if options.whpx else ['-accel', 'tcg']
    args = [QEMU, *accel, '-machine', 'q35', '-m', '512', '-smp', str(smp), '-display', 'none', '-vga', 'none',
            '-drive', f'file={out / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a',
            '-drive', f'if=none,id=root,file={disk},format=raw', '-device', 'nvme,serial=nexus-root,drive=root',
            '-serial', f'file:{serial}', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
            '-qmp', f'tcp:127.0.0.1:{options.qmp_port},server=on,wait=off']
    # the desktop launcher's CPU model under WHPX (RDRAND: the dynamic loader wants secure entropy); TCG: everything it has
    args += ['-cpu', ('qemu64,+rdrand' + build.host_cpu_flags() if options.whpx else 'max') + cpu_extra]
    if cmdline: args += ['-fw_cfg', f'name=opt/nexus/cmdline,string={cmdline}']
    args += list(extra)
    vm = subprocess.Popen(args, cwd=WORK, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    build.unthrottle(vm.pid)   # Windows 11 coalesces a hidden QEMU's host timers to 15.6 ms otherwise (guest ticks lost)
    host = None; begin = None; deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if measure_host:
                if begin is None and '[64] TICK MEASURE BEGIN' in text: begin = (time.time(), cpu_seconds(vm))
                if begin and host is None and '[64] TICK MEASURE END' in text:
                    wall = time.time() - begin[0]; host = (cpu_seconds(vm) - begin[1]) / wall if wall > 0 else 0.0
            if '[64] POWER CAMPAIGN STATUS=' in text or 'KERNEL_FAULT' in text or '[64] FAIL' in text: break
            if vm.poll() is not None: break
            time.sleep(0.2)
        else: raise AssertionError(f'{tag}: timeout\n' + text[-4000:])
        time.sleep(0.5); text = serial.read_text(errors='replace')
    finally:
        if vm.poll() is None: vm.kill()
        vm.wait()
        for f in (disk, marker): f.unlink(missing_ok=True)
    return text, host


def check(tag, text):
    fails = re.findall(r'\[64\] TICK FAIL: .*', text)
    if 'KERNEL_FAULT' in text or '[64] FAIL' in text or 'USER_FAULT' in text: raise AssertionError(f'{tag}: kernel/user fault\n' + text[-6000:])
    if fails or '[64] TICK DONE failures=0' not in text: raise AssertionError(f'{tag}: ' + '\n'.join(fails or ['no DONE line']) + '\n' + text[-6000:])


def measurement(text):
    rows = {}
    for m in re.finditer(r'\[64\] TICK MEASURE cpu(\d+) wakeups_per_s=([\d.]+) idle_residency=([\d.]+)% timer_irqs_per_s=([\d.]+|n/a)', text):
        rows[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), None if m.group(4) == 'n/a' else float(m.group(4)))
    return rows


def measurement_of(text, label):
    rows = {}
    for m in re.finditer(r'\[64\] TICK MEASURE ' + label + r' cpu(\d+) wakeups_per_s=([\d.]+) idle_residency=([\d.]+)% timer_irqs_per_s=([\d.]+|n/a)', text):
        rows[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), None if m.group(4) == 'n/a' else float(m.group(4)))
    return rows


def describe(rows):
    return ', '.join(f'cpu{c}: {w:.1f} wakeups/s {t if t is None else round(t, 1)} timer irq/s {r:.1f}% idle' for c, (w, r, t) in sorted(rows.items()))


results = []
cases = options.cases.split(',')
for smp in [int(n) for n in options.smp.split(',')]:
    if 'measure' in cases:
        measured = {}
        for label, cmdline, out in [('tickless', '', OUT), ('periodic', 'nohz=off', OUT)] + ([('compare', '', options.compare_kernel / 'arch/x86_64/out')] if options.compare_kernel else []):
            tag = f'measure-{label}-smp{smp}'
            text, host = run(tag, f'm{options.seconds}', smp, cmdline, out=out, measure_host=True)
            if label != 'compare': check(tag, text)
            rows = measurement(text); measured[label] = (rows, host)
            if len(rows) != smp: raise AssertionError(f'{tag}: {len(rows)} CPUs measured\n' + text[-4000:])
            print(f'MEASURE {tag}: {describe(rows)}; host CPU {host * 100 if host is not None else float("nan"):.1f}% of a core', flush=True)
        rows, host = measured['tickless']
        bsp_timer = rows[0][2]
        if bsp_timer is None or bsp_timer >= 60: raise AssertionError(f'smp{smp}: boot CPU still takes {bsp_timer} timer interrupts/s while idle')
        for cpu in range(1, smp):
            if rows[cpu][2] is None or rows[cpu][2] >= 10: raise AssertionError(f'smp{smp}: cpu{cpu} takes {rows[cpu][2]} timer interrupts/s while idle')
        periodic = measured['periodic'][0]
        if periodic[0][2] is None or periodic[0][2] < 500: raise AssertionError(f'smp{smp}: nohz=off boot CPU only {periodic[0][2]} timer irq/s')
        results.append(f'measure smp{smp}: tickless BSP {bsp_timer:.1f} timer irq/s (periodic {periodic[0][2]:.0f}); '
                       f'wake-ups {sum(r[0] for r in rows.values()):.1f}/s (periodic {sum(r[0] for r in periodic.values()):.1f}/s); '
                       f'host CPU {host * 100 if host is not None else float("nan"):.1f}% vs {measured["periodic"][1] * 100 if measured["periodic"][1] is not None else float("nan"):.1f}%')
    if 'accuracy' in cases:
        for label, out in [('', OUT)] + ([('-compare', options.compare_kernel / 'arch/x86_64/out')] if options.compare_kernel else []):
            tag = f'accuracy{label}-smp{smp}'; text, _ = run(tag, options.modes, smp, out=out)
            if not label: check(tag, text)
            results.append(f'{tag}: ' + '; '.join(f'{n} median {md} us max {mx} us' for n, md, mx in
                                                  re.findall(r'LATENCY (\S+) n=\d+ median_us=(\d+) mean_us=\d+ max_us=(\d+)', text)))
    if 'devices' in cases:
        for device, cmdline in [('tscdeadline', 'tick=tsc-deadline'), ('lapic', 'tick=lapic'), ('pit', 'tick=pit'), ('pic', 'noapic')]:
            tag = f'device-{device}-smp{smp}'
            text, _ = run(tag, 'a b m3', smp, cmdline, cpu_extra=',+tsc-deadline' if device == 'tscdeadline' else ''); check(tag, text)
            event = re.search(r'\[64\] TICK: [^\n]*', text)
            rows = measurement(text)
            if device in ('lapic', 'tscdeadline') and event and 'unavailable' not in event.group(0) and rows.get(0, (0, 0, 99))[2] >= 60:
                raise AssertionError(f'{tag}: {describe(rows)}')
            results.append(f'{tag}: {event.group(0) if event else "?"}; {describe(rows)}')
    if 'nohz' in cases:
        tag = f'nohz-off-smp{smp}'; text, _ = run(tag, 'a b', smp, 'nohz=off'); check(tag, text)
        results.append(f'{tag}: PASS')
    if 'audio' in cases:
        wav_check = module('wav_check', HERE / 'wav_check.py')
        root = OUT / 'chrome-root'; libasound = root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
        if not libasound.exists(): raise SystemExit('SKIP audio: no Debian libasound in out/chrome-root')
        install = ['put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2']
        for source in sorted((root / 'usr/share/alsa').rglob('*')):
            if source.is_file(): install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(root / 'usr/share/alsa').as_posix()]
        for device, qdev in [('ac97', ['-device', 'AC97,audiodev=snd0']), ('hda', ['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0'])]:
            tag = f'audio-{device}-smp{smp}'; wav = WORK / f'{tag}.wav'; wav.unlink(missing_ok=True)
            try:
                text, _ = run(tag, f'p{options.seconds}', smp, extra=['-audiodev', f'wav,id=snd0,path={wav},out.frequency=48000', *qdev], install=install)
                check(tag, text)
                summary = []
                for phase in ('blocking', 'poll'):
                    rows = measurement_of(text, 'audio-' + phase)
                    if len(rows) != smp: raise AssertionError(f'{tag}: {phase}: {len(rows)} CPUs measured\n' + text[-4000:])
                    print(f'MEASURE {tag} {phase}: {describe(rows)}', flush=True)
                    wakeups, _, timer = rows[0]
                    if timer is None or timer >= options.audio_limit or wakeups >= options.audio_limit:
                        raise AssertionError(f'{tag} {phase}: boot CPU {wakeups} wake-ups/s, {timer} timer irq/s while playing (limit {options.audio_limit})')
                    summary.append(f'{phase}: BSP {wakeups:.0f} wake-ups/s {timer:.0f} timer irq/s')
                report = wav_check.analyze(wav)
                tones = [s for s in report['segments'] if s['duration_s'] >= 0.3]
                print(f'  {tag} capture: ' + '; '.join(f"{s['frequency_hz']:.1f} Hz {s['duration_s']:.2f} s dropouts {s['dropout_windows_10ms']} silences {s['silence_runs_over_1ms']} discontinuities {s['discontinuities']}" for s in tones), flush=True)
                # both phases: one or two 440 Hz segments (QEMU records nothing while the DMA is stopped between them)
                if not 1 <= len(tones) <= 2 or sum(s['duration_s'] for s in tones) < 2 * options.seconds - 0.6:
                    raise AssertionError(f'{tag}: expected {2 * options.seconds} s of 440 Hz, got {tones}')
                # A looping DMA (HD Audio) records the silence between the phases (drain, close, reopen); any other
                # silent run is a glitch. AC'97 stops its DMA there, so its capture has no gap at all.
                import numpy as np
                samples, rate, _ = wav_check.load(wav); x = samples[:, 0]; loud = np.flatnonzero(np.abs(x) >= 1.0 / 32768)
                first, last = int(loud[0]), int(loud[-1])
                gaps = [(first + int(s), int(n)) for s, n in wav_check.runs(np.abs(x[first:last]) < 1.0 / 32768) if n >= rate // 1000]
                between = [(s, n) for s, n in gaps if abs(s - first - options.seconds * rate) < 0.8 * rate and n < rate // 2]
                if len(gaps) > 1 or gaps != between: raise AssertionError(f'{tag}: silent gaps (start s, ms) {[(round(s / rate, 3), round(n * 1000 / rate, 1)) for s, n in gaps]}')
                gap_windows = sum(n for _, n in gaps) // (rate // 100) + 2 * len(gaps)
                for s in tones:
                    if abs(s['frequency_hz'] - 440) > 3 or s['dropout_windows_10ms'] > gap_windows or s['discontinuities']:
                        raise AssertionError(f'{tag}: glitch in the captured tone {s}')
                summary.append('capture gap-free' + (f' (phase change {gaps[0][1] * 1000 / rate:.0f} ms)' if gaps else ''))
                results.append(f'{tag}: ' + '; '.join(summary))
            finally:
                wav.unlink(missing_ok=True)
    if 'audio-sleep' in cases:   # playback across suspend-to-idle and suspend-to-RAM (tickless_user.c mode P)
        wav_check = module('wav_check', HERE / 'wav_check.py')
        root = OUT / 'chrome-root'; libasound = root / 'usr/lib/x86_64-linux-gnu/libasound.so.2.0.0'
        install = ['put', str(libasound), 'lib/x86_64-linux-gnu/libasound.so.2']
        for source in sorted((root / 'usr/share/alsa').rglob('*')):
            if source.is_file(): install += ['put', str(source), 'usr/share/alsa/' + source.relative_to(root / 'usr/share/alsa').as_posix()]
        for device, qdev in [('ac97', ['-device', 'AC97,audiodev=snd0']), ('hda', ['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0'])]:
            tag = f'audio-sleep-{device}-smp{smp}'; wav = WORK / f'{tag}.wav'; wav.unlink(missing_ok=True)
            try:
                text, _ = run(tag, 'P', smp, extra=['-audiodev', f'wav,id=snd0,path={wav},out.frequency=48000', '-global', 'ICH9-LPC.disable_s3=0', *qdev], install=install)
                check(tag, text)
                for state in ('freeze', 'mem'):
                    if f'[64] TICK AUDIO system sleep {state}: resumed' not in text: raise AssertionError(f'{tag}: no {state} cycle\n' + text[-4000:])
                player = re.search(r'\[64\] TICK AUDIO sleep buffer=\d+ period=\d+ frames=(\d+) xruns=(\d+) interrupted=\d+ switches=(\d+)', text)
                if not player: raise AssertionError(f'{tag}: the player did not finish\n' + text[-4000:])
                report = wav_check.analyze(wav)
                tones = [s for s in report['segments'] if s['duration_s'] >= 0.2]
                order = []
                for s in tones:
                    hz = round(s['frequency_hz'] / 10) * 10
                    if not order or order[-1] != hz: order.append(hz)
                described = '; '.join(f"{s['frequency_hz']:.0f} Hz {s['start_s']:.2f}+{s['duration_s']:.2f} s silences {s['silence_runs_over_1ms']} discontinuities {s['discontinuities']}" for s in tones)
                print(f'  {tag} capture: {described}', flush=True)
                # in order, nothing stale after a resume (an old tone after a newer one), and clean audio after the last resume
                if order != [440, 660, 880]: raise AssertionError(f'{tag}: tone order {order}: {described}')
                last = tones[-1]
                if last['duration_s'] < 2.5 or last['silence_runs_over_1ms'] or last['discontinuities'] or last['dropout_windows_10ms'] > 1:
                    raise AssertionError(f'{tag}: playback after the S3 resume is not clean: {last}')
                results.append(f'{tag}: s2idle + S3 during playback, tones 440 -> 660 -> 880 Hz in order, {last["duration_s"]:.1f} s clean after the last resume, '
                               f'player xruns {player.group(2)} (one per sleep allowed)')
            finally:
                wav.unlink(missing_ok=True)
for line in results: print('RESULT', line, flush=True)
print('TICKLESS PASS', flush=True)
