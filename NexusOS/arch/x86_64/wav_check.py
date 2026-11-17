"""Analyze a QEMU `-audiodev wav` capture: duration, peak, non-silent fraction, tone segments and glitches.

Usage: python -I wav_check.py capture.wav [--json out.json]
A capture that QEMU did not finalize (killed or isa-debug-exit) is read up to the end of the file.
Tone segments are found from 20 ms zero-crossing windows; inside each segment the analyzer reports the
FFT frequency, level, dropout windows, digital-silence runs and sample discontinuities (second-difference
residual of an ideal sine; a gap, splice or skipped block shows up as a spike far above resampler noise).
"""
from pathlib import Path
import argparse
import json
import re
import struct

import numpy as np


def load(path):
    data = Path(path).read_bytes()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('not a RIFF/WAVE file')
    position, fmt, payload, finalized = 12, None, b'', False
    while position + 8 <= len(data):
        chunk, size = data[position:position + 4], struct.unpack_from('<I', data, position + 4)[0]
        body = position + 8
        if chunk == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', data, body)
        elif chunk == b'data':
            finalized = size != 0 and body + size <= len(data)
            payload = data[body:body + size] if finalized else data[body:]
            break
        position = body + size + (size & 1)
    if not fmt or fmt[0] != 1 or fmt[5] != 16:
        raise ValueError('expected 16-bit PCM')
    channels, rate = fmt[1], fmt[2]
    usable = len(payload) // (2 * channels) * 2 * channels
    samples = np.frombuffer(payload[:usable], '<i2').reshape(-1, channels).astype(np.float64) / 32768.0
    return samples, rate, finalized


def tone_frequency(x, rate):
    if len(x) < 64:
        return 0.0
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x)), n=max(len(x), 1 << 16)))
    k = int(np.argmax(spectrum[1:])) + 1
    if 1 <= k < len(spectrum) - 1:
        a, b, c = np.log(spectrum[k - 1:k + 2] + 1e-12)
        k = k + 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) else k
    return float(k * rate / (2 * (len(spectrum) - 1)))


def runs(mask):
    """(start, length) of True runs."""
    if not len(mask):
        return []
    padded = np.concatenate(([False], mask, [False]))
    edges = np.flatnonzero(padded[1:] != padded[:-1])
    return list(zip(edges[::2], edges[1::2] - edges[::2]))


def segment_report(x, rate, start, end, frequency_hint):
    part = x[start:end]
    frequency = tone_frequency(part[: min(len(part), rate * 4)], rate) if len(part) else 0.0
    rms = float(np.sqrt(np.mean(part ** 2))) if len(part) else 0.0
    amplitude = rms * np.sqrt(2)
    window = rate // 100
    levels = np.array([np.sqrt(np.mean(part[i:i + window] ** 2)) for i in range(0, len(part) - window + 1, window)])
    dropouts = int(np.sum(levels < 0.5 * rms)) if len(levels) else 0
    # Digital silence touching the segment's first or last sample is the tone's own start/end (a window that holds
    # the end of the previous tone can join this segment), not a gap inside it.
    zero_runs = [(int(s), int(n)) for s, n in runs(np.abs(part) < 1.0 / 32768) if n >= rate // 1000 and 0 < s and s + n < len(part)]
    w = 2 * np.pi * (frequency or frequency_hint) / rate
    residual = np.abs(part[2:] + part[:-2] - 2 * np.cos(w) * part[1:-1]) if len(part) > 2 else np.zeros(0)
    limit = max(0.1 * amplitude, 4.0 / 32768)
    spikes = residual > limit
    events = runs(spikes)
    worst = float(residual.max() / amplitude) if len(residual) and amplitude else 0.0
    return {
        'start_s': round(start / rate, 4), 'end_s': round(end / rate, 4), 'duration_s': round((end - start) / rate, 4),
        'frequency_hz': round(frequency, 2), 'rms': round(rms, 5), 'peak': round(float(np.max(np.abs(part))) if len(part) else 0.0, 5),
        'dropout_windows_10ms': dropouts, 'silence_runs_over_1ms': len(zero_runs),
        'longest_silence_ms': round(max((n for _, n in zero_runs), default=0) * 1000 / rate, 2),
        'discontinuities': len(events),
        'discontinuity_times_s': [round((start + 1 + s) / rate, 4) for s, _ in events[:20]],
        'worst_residual_rel': round(worst, 4),
        'residual_p999_rel': round(float(np.quantile(residual, 0.999) / amplitude), 5) if len(residual) and amplitude else 0.0,
    }


# QEMU's hda-codec (use-timer=on, its default) pulls the guest's DMA into an 8 KiB stream buffer and, when the
# audio backend finds that buffer completely full (QEMU's main loop stalled ~20 ms under host load), throws the whole
# buffer away and traces hda_audio_overrun: exactly 2048 S16-stereo frames vanish between the guest's DMA ring and the
# capture. The guest cannot see or prevent it (the frames were fetched normally). Such a splice is recognizable: the
# signal after it is the signal before it advanced by exactly 2048 frames.
QEMU_HDA_DROP_FRAMES = 2048


def annihilator(frequencies, rate):
    """FIR filter that cancels a sum of sinusoids at `frequencies` exactly; a splice leaves a spike."""
    h = np.array([1.0])
    for f in frequencies:
        h = np.convolve(h, [1.0, -2.0 * np.cos(2 * np.pi * f / rate), 1.0])
    return h


def splice_points(x, rate, frequencies, start, end, amplitude):
    """Sample indices in [start, end) where the tone mixture breaks (annihilator spike above 10% of the amplitude)."""
    h = annihilator(frequencies, rate)
    if end - start <= len(h):
        return []
    residual = np.abs(np.convolve(x[start:end], h, mode='valid'))
    limit = max(0.1 * amplitude * (np.sum(np.abs(h)) / 4.0), 8.0 / 32768)
    return [start + int(s) + len(h) // 2 for s, _ in runs(residual > limit)]


def _basis(t, rate, frequencies):
    columns = [np.ones(len(t))]
    for f in frequencies:
        columns += [np.sin(2 * np.pi * f * t / rate), np.cos(2 * np.pi * f * t / rate)]
    return np.stack(columns, axis=1)


def _fit(x, rate, t, frequencies):
    basis = _basis(t, rate, frequencies)
    c, *_ = np.linalg.lstsq(basis, x[t], rcond=None)
    return c, float(np.sqrt(np.mean((x[t] - basis @ c) ** 2)))


def refine_frequencies(x, rate, t, frequencies, span=1.5):
    """Least-squares refinement of each frequency (golden section, one at a time) over the sample indices t."""
    frequencies = list(frequencies)
    for k in range(len(frequencies)):
        a, b, g = frequencies[k] - span, frequencies[k] + span, (np.sqrt(5) - 1) / 2
        def cost(f):
            trial = frequencies[:k] + [f] + frequencies[k + 1:]
            return _fit(x, rate, t, trial)[1]
        c, d = b - g * (b - a), a + g * (b - a)
        fc, fd = cost(c), cost(d)
        for _ in range(40):
            if fc < fd:
                b, d, fd = d, c, fc; c = b - g * (b - a); fc = cost(c)
            else:
                a, c, fc = c, d, fd; d = a + g * (b - a); fd = cost(d)
        frequencies[k] = (a + b) / 2
    return frequencies


def classify_splice(x, rate, index, frequencies, lo, hi, skip=QEMU_HDA_DROP_FRAMES, exact=False):
    """Is the break at sample `index` (inside the clean span [lo, hi)) a downstream drop of exactly `skip` frames?
    The tone(s) are fitted on up to 0.2 s on the longer side of the break and extrapolated across it with and
    without the skip; returns relative RMS residuals of the other side's 20 ms. `exact`: frequencies are exact
    (48 kHz client streams), otherwise each is refined first (resampled streams: alsa-lib's fixed-point pitch)."""
    guard = 8
    before = np.arange(max(lo, index - guard - rate // 5), index - guard)
    after = np.arange(index + guard, min(hi, index + guard + rate // 5))
    if len(before) < rate // 100 and len(after) < rate // 100:
        return None
    fit_side, other, sign = (before, after, 1) if len(before) >= len(after) else (after, before, -1)
    other = other[:rate // 50] if sign > 0 else other[-(rate // 50):]
    if not exact:
        frequencies = refine_frequencies(x, rate, fit_side, frequencies)
    c, _ = _fit(x, rate, fit_side, frequencies)
    amplitude = float(np.sqrt(np.sum(c[1:] ** 2)))
    if not amplitude or not len(other):
        return None
    def residual(shift):
        model = _basis(other + sign * shift, rate, frequencies) @ c
        return float(np.sqrt(np.mean((x[other] - model) ** 2)) / amplitude)
    with_skip, without = residual(skip), residual(0)
    return {'index': int(index), 't': round(index / rate, 4), 'frequencies': [round(f, 3) for f in frequencies],
            'residual_with_skip': round(with_skip, 5), 'residual_without': round(without, 5),
            'host_drop': with_skip < 0.03 and without > 0.1}


def segment_breaks(x, rate, segment, frequencies=None, exact=False):
    """classify_splice for every discontinuity of a segment_report, each fitted only up to its neighbours."""
    start, end = int(round(segment['start_s'] * rate)), int(round(segment['end_s'] * rate))
    points = [int(round(t * rate)) for t in segment['discontinuity_times_s']]
    frequencies = frequencies or [segment['frequency_hz']]
    result = []
    for k, p in enumerate(points):
        lo = points[k - 1] + 8 if k else start
        hi = points[k + 1] - 8 if k + 1 < len(points) else end
        result.append(classify_splice(x, rate, p, frequencies, lo, hi, exact=exact) or
                      {'index': p, 't': round(p / rate, 4), 'host_drop': False, 'note': 'too short to classify'})
    return result


def overrun_events(qemu_log):
    """Parse QEMU's hda trace (-msg timestamp=on -trace hda_audio_running -trace hda_audio_overrun): returns the
    overrun times on the capture's timeline (seconds of active stream before each overrun, minus the frames dropped
    by earlier overruns), or plain counts when the log has no timestamps."""
    text = Path(qemu_log).read_text(errors='replace') if Path(qemu_log).exists() else ''
    events = re.findall(r'(?:\d+@(\d+\.\d+):)?hda_audio_(running|overrun) st \w+(?:, nr \d+, run (\d))?', text)
    timeline, active_since, elapsed, dropped, timed = [], None, 0.0, 0, all(stamp for stamp, _, _ in events)
    for stamp, kind, run in events:
        now = float(stamp) if stamp else 0.0
        if kind == 'running':
            if run == '1' and active_since is None: active_since = now
            elif run == '0' and active_since is not None: elapsed += now - active_since; active_since = None
        else:
            position = elapsed + (now - active_since if active_since is not None else 0.0)
            timeline.append(round(position - dropped * QEMU_HDA_DROP_FRAMES / 48000.0, 3) if timed else None)
            dropped += 1
    return timeline


def explain_breaks(breaks, free, tolerance_s=0.35):
    """Attribute classified splices (classify_splice results) to QEMU overruns: a splice is host-side only if it is an
    exact 2048-frame drop and an unused traced overrun lies within `tolerance_s` of it on the capture timeline (any
    unused one when the log has no timestamps). `free` (overrun_events entries) loses the overruns used.
    Returns (host, unexplained)."""
    host, unexplained = [], []
    for b in breaks:
        match = None
        if b and b['host_drop']:
            for k, t in enumerate(free):
                if t is None or abs(t - b['t']) <= tolerance_s:
                    match = k; break
        if match is None:
            unexplained.append(b)
        else:
            host.append(dict(b, overrun_t=free.pop(match)))
    return host, unexplained


def analyze(path, min_segment_s=0.25):
    samples, rate, finalized = load(path)
    left = samples[:, 0]
    channels_equal = bool(samples.shape[1] == 1 or np.array_equal(samples[:, 0], samples[:, -1]))
    peak = float(np.max(np.abs(samples))) if len(samples) else 0.0
    nonsilent = float(np.mean(np.abs(left) >= 0.01)) if len(left) else 0.0
    # 20 ms windows: level and zero-crossing frequency.
    window = rate // 50
    count = len(left) // window
    blocks = left[:count * window].reshape(count, window) if count else np.zeros((0, window))
    level = np.sqrt(np.mean(blocks ** 2, axis=1)) if count else np.zeros(0)
    crossings = np.sum(np.abs(np.diff(np.signbit(blocks).astype(np.int8), axis=1)), axis=1) if count else np.zeros(0)
    zcr_frequency = crossings * rate / (2.0 * window)
    active = level >= 0.01
    raw, i = [], 0
    while i < count:
        if not active[i]:
            i += 1
            continue
        j = i + 1
        reference = np.median(zcr_frequency[i:min(i + 5, count)])
        while j < count and active[j] and abs(zcr_frequency[j] - reference) <= max(0.08 * reference, 60):
            j += 1
        raw.append([i, j, reference])
        i = j
    # A glitch (gap, splice) briefly breaks the zero-crossing estimate; re-join same-tone pieces closer than
    # half a second so the glitch is measured inside the segment instead of hiding at a segment boundary.
    # "Same tone" allows one zero crossing per window of quantization (25 Hz at 20 ms): 550 Hz reads as 525 or 550.
    merged = []
    for piece in raw:
        if merged and abs(piece[2] - merged[-1][2]) <= max(0.03 * merged[-1][2], rate / window / 2 + 5) and (piece[0] - merged[-1][1]) * window < rate // 2:
            merged[-1][1] = piece[1]
        elif (piece[1] - piece[0]) * window >= rate // 50 * 3:
            merged.append(list(piece))
    segments = [segment_report(left, rate, (i + 1) * window, (j - 1) * window, reference)
                for i, j, reference in merged if (j - i) * window >= min_segment_s * rate]
    return {
        'file': str(path), 'finalized_header': finalized, 'rate': rate, 'channels': int(samples.shape[1]),
        'duration_s': round(len(samples) / rate, 4), 'peak': round(peak, 5), 'nonsilent_fraction': round(nonsilent, 4),
        'channels_identical': channels_equal, 'segments': segments,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav'); parser.add_argument('--json', type=Path)
    options = parser.parse_args()
    report = analyze(options.wav)
    text = json.dumps(report, indent=1)
    if options.json:
        options.json.write_text(text)
    print(text)
