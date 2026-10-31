"""ParametricEQ.py - multi-band parametric EQ for Alpha Sampler.

Band spec (passed in one key=value arg):
    bands=type,freq,gain_db,q;type,freq,gain_db,q;...

    type   : peak | lowshelf | highshelf   (aliases: peaking, ls, hs)
    freq   : centre / corner frequency in Hz
    gain_db: gain in dB (peak, shelves)
    q      : Q for peak bands  |  shelf slope S (0.1..1.0) for shelves

Example:
    bands=peak,200,3.0,1.0;peak,3000,-2.5,1.5;highshelf,8000,2.0,0.7

Coefficients are RBJ Audio EQ Cookbook biquads, run in series (direct form I).
Standard library only.
"""
import sys, wave, struct, math


# ---------------------------------------------------------------- WAV I/O ---
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


# ------------------------------------------------------- biquad cookbook ---
def _biquad_peak(fs, f0, gain_db, q):
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    b0 = 1 + alpha * A
    b1 = -2 * cw
    b2 = 1 - alpha * A
    a0 = 1 + alpha / A
    a1 = -2 * cw
    a2 = 1 - alpha / A
    return b0, b1, b2, a0, a1, a2


def _biquad_lowshelf(fs, f0, gain_db, S):
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / 2.0 * math.sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0)
    sq = 2.0 * math.sqrt(A) * alpha
    b0 = A * ((A + 1) - (A - 1) * cw + sq)
    b1 = 2 * A * ((A - 1) - (A + 1) * cw)
    b2 = A * ((A + 1) - (A - 1) * cw - sq)
    a0 = (A + 1) + (A - 1) * cw + sq
    a1 = -2 * ((A - 1) + (A + 1) * cw)
    a2 = (A + 1) + (A - 1) * cw - sq
    return b0, b1, b2, a0, a1, a2


def _biquad_highshelf(fs, f0, gain_db, S):
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / 2.0 * math.sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0)
    sq = 2.0 * math.sqrt(A) * alpha
    b0 = A * ((A + 1) + (A - 1) * cw + sq)
    b1 = -2 * A * ((A - 1) + (A + 1) * cw)
    b2 = A * ((A + 1) + (A - 1) * cw - sq)
    a0 = (A + 1) - (A - 1) * cw + sq
    a1 = 2 * ((A - 1) - (A + 1) * cw)
    a2 = (A + 1) - (A - 1) * cw - sq
    return b0, b1, b2, a0, a1, a2


def make_coeffs(kind, fs, freq, gain_db, q_or_s):
    """Return normalised (b0,b1,b2,a1,a2) with a0 divided out."""
    nyq = fs * 0.5
    freq = max(10.0, min(nyq * 0.98, freq))
    if kind in ('peak', 'peaking', 'bell'):
        b0, b1, b2, a0, a1, a2 = _biquad_peak(fs, freq, gain_db, max(0.1, q_or_s))
    elif kind in ('lowshelf', 'low_shelf', 'ls'):
        b0, b1, b2, a0, a1, a2 = _biquad_lowshelf(fs, freq, gain_db, max(0.1, min(1.0, q_or_s)))
    elif kind in ('highshelf', 'high_shelf', 'hs'):
        b0, b1, b2, a0, a1, a2 = _biquad_highshelf(fs, freq, gain_db, max(0.1, min(1.0, q_or_s)))
    else:
        raise ValueError('unknown band type %r' % kind)
    if a0 == 0:
        raise ValueError('degenerate biquad (a0 == 0)')
    return b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0


# -------------------------------------------------------------- filtering ---
def apply_biquad(x, b0, b1, b2, a1, a2):
    """Direct form I biquad. Returns a new list."""
    out = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, xi in enumerate(x):
        y = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, xi
        y2, y1 = y1, y
        out[i] = y
    return out


# ------------------------------------------------------------ band parser ---
def parse_bands(spec):
    """Parse 'type,freq,gain,q;...' -> list of (kind, freq, gain_db, q)."""
    bands = []
    if not spec:
        return bands
    for chunk in spec.split(';'):
        chunk = chunk.strip()
        if not chunk:
            continue
        parts = [p.strip() for p in chunk.split(',')]
        if len(parts) != 4:
            raise ValueError('band %r: expected type,freq,gain,q' % chunk)
        kind = parts[0].lower()
        try:
            freq = float(parts[1])
            gain = float(parts[2])
            q = float(parts[3])
        except ValueError as e:
            raise ValueError('band %r: %s' % (chunk, e))
        bands.append((kind, freq, gain, q))
    return bands


# ------------------------------------------------------------------ main ---
DEFAULT_BANDS = 'peak,200,0.0,1.0'   # effectively transparent; user should override


def process(rate, chans, opts):
    """opts: rate, bpm, root, slices, plus our 'bands' string."""
    fs = int(rate)
    spec = opts.get('bands', '') or DEFAULT_BANDS
    bands = parse_bands(spec)
    if not bands:
        return rate, chans

    # Build coefficient sets once for all channels (fs is common).
    coeffs = [make_coeffs(k, fs, f, g, q) for (k, f, g, q) in bands]

    # Apply in series, band by band, channel by channel.
    for (b0, b1, b2, a1, a2) in coeffs:
        for ci in range(len(chans)):
            chans[ci] = apply_biquad(chans[ci], b0, b1, b2, a1, a2)

    # Safety: kill any NaN / inf and prevent clipping via soft headroom.
    for c in chans:
        for i, v in enumerate(c):
            if v != v or v in (float('inf'), float('-inf')):
                c[i] = 0.0
    peak = max((abs(v) for c in chans for v in c), default=0.0)
    if peak > 0.999:
        g = 0.999 / peak
        for c in chans:
            for i in range(len(c)):
                c[i] *= g

    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    try:
        rate, chans = process(rate, chans, opts)
    except Exception as e:
        print('ParametricEQ error: %s' % e)
        raise SystemExit(1)
    write_wav(dst, rate, chans)
    print('ok')