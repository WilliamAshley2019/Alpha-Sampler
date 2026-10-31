"""Alpha Sampler edit script: TransientDetect.

Detects attack transients using a combination of:
  - short-term energy envelope derivative (attack slope)
  - peak detection with minimum inter-onset interval
Writes the audio unchanged to out.wav and prints detected onsets
plus a ready-to-paste slices= argument.

Optional opts (key=value):
  sensitivity=0.5    0..1, higher = more onsets (default 0.5)
  min_gap_ms=30      minimum spacing between onsets in ms (default 30)
  smooth_ms=2        envelope smoothing window in ms (default 2)
  threshold=0.15     absolute onset-strength threshold 0..1 (default 0.15)
  max_slices=128     cap on number of onsets reported (default 128)
"""
import sys, wave, struct


def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 3:
        ints = [int.from_bytes(raw[i:i + 3], 'little', signed=True) for i in range(0, len(raw), 3)]
    elif width == 2:
        ints = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
    else:
        raise SystemExit('unsupported sample width %d (plugin always sends 24-bit)' % width)
    scale = float(1 << (8 * width - 1))
    return rate, [[v / scale for v in ints[c::ch]] for c in range(ch)]


def write_wav(path, rate, chans):
    n = min(len(c) for c in chans)
    out = bytearray()
    for i in range(n):
        for c in chans:
            v = max(-8388608, min(8388607, int(round(c[i] * 8388608.0))))
            out += v.to_bytes(3, 'little', signed=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(len(chans)); w.setsampwidth(3); w.setframerate(rate)
        w.writeframes(bytes(out))


def _mono(chans):
    n = min(len(c) for c in chans)
    return [(sum(c[i] for c in chans) / len(chans)) for i in range(n)]


def _envelope(sig, smooth_len):
    """Rectified, smoothed amplitude envelope."""
    a = [abs(v) for v in sig]
    if smooth_len <= 1:
        return a
    # simple boxcar smoothing
    out = [0.0] * len(a)
    half = smooth_len // 2
    for i in range(len(a)):
        lo = max(0, i - half)
        hi = min(len(a), i + half + 1)
        s = 0.0
        for j in range(lo, hi):
            s += a[j]
        out[i] = s / (hi - lo)
    return out


def _onset_strength(env):
    """First-order difference of the envelope, half-wave rectified."""
    d = [0.0] * len(env)
    for i in range(1, len(env)):
        v = env[i] - env[i - 1]
        d[i] = v if v > 0.0 else 0.0
    return d


def _detect_peaks(strength, threshold, min_gap):
    """Local-maximum peak picker with absolute threshold and min spacing."""
    peaks = []
    n = len(strength)
    if n == 0:
        return peaks
    # find all local maxima above threshold
    cand = []
    for i in range(1, n - 1):
        if strength[i] >= threshold and strength[i] > strength[i - 1] and strength[i] >= strength[i + 1]:
            cand.append(i)
    # greedy: keep strongest first, suppress within min_gap
    cand.sort(key=lambda i: strength[i], reverse=True)
    taken = [False] * n
    for i in cand:
        if any(taken[max(0, i - min_gap):i + min_gap + 1]):
            continue
        taken[i] = True
        peaks.append(i)
    peaks.sort()
    return peaks


def process(rate, chans, opts):
    def f(key, default):
        try:
            return float(opts.get(key, default))
        except (TypeError, ValueError):
            return float(default)
    sens = max(0.0, min(1.0, f('sensitivity', 0.5)))
    min_gap_ms = max(1.0, f('min_gap_ms', 30.0))
    smooth_ms = max(0.0, f('smooth_ms', 2.0))
    thresh = max(0.0, min(1.0, f('threshold', 0.15)))
    max_slices = int(f('max_slices', 128))

    mono = _mono(chans)
    n = len(mono)
    if n == 0:
        print('empty sample')
        return rate, chans

    smooth_len = max(1, int(rate * smooth_ms / 1000.0))
    env = _envelope(mono, smooth_len)

    # normalise envelope to 0..1 so threshold is meaningful
    peak_env = max(env) if env else 0.0
    if peak_env > 0:
        env = [v / peak_env for v in env]

    strength = _onset_strength(env)

    # map sensitivity (0..1) to effective threshold:
    #   sens=0 -> threshold (conservative), sens=1 -> threshold/4 (aggressive)
    eff_thresh = thresh * (1.0 - 0.75 * sens)
    # also scale peak spacing with sensitivity: more sensitive -> allow closer hits
    gap_samples = max(1, int(rate * min_gap_ms / 1000.0 * (1.0 - 0.5 * sens)))

    peaks = _detect_peaks(strength, eff_thresh, gap_samples)

    # Always treat the very first non-silent sample as an onset candidate
    # (many one-shots don't have a derivative at t=0).
    first_nz = 0
    for i, v in enumerate(env):
        if v > 0.02:
            first_nz = i
            break
    if not peaks or first_nz < peaks[0] - gap_samples:
        peaks.insert(0, first_nz)

    if len(peaks) > max_slices:
        # keep strongest
        ranked = sorted(peaks, key=lambda i: strength[i] if i < len(strength) else 0.0, reverse=True)
        peaks = sorted(ranked[:max_slices])

    total = float(n) if n > 0 else 1.0
    fracs = [p / total for p in peaks]

    # Report to status line (last ~160 chars shown by plugin, so keep it tight)
    lines = []
    lines.append('TRANSIENTS %d detected @ %.1f BPM' % (len(peaks), f('bpm', 0.0)))
    # human-readable times (seconds)
    times_ms = [int(round(p / rate * 1000.0)) for p in peaks]
    lines.append('times(ms): ' + ','.join(str(t) for t in times_ms[:64])
                 + ('...' if len(times_ms) > 64 else ''))
    # paste-ready slices argument
    slice_arg = 'slices=' + ','.join('%.6f' % x for x in fracs)
    lines.append(slice_arg)
    print('\n'.join(lines))

    # Audio is unchanged
    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')