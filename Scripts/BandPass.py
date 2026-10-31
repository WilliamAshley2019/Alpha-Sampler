"""Alpha Sampler edit script: BandPass.py

Isolates a selected frequency range with a resonant bandpass filter.
Zero-phase (forward-backward biquad) so transients and slice markers stay aligned.

Options (all optional):
  freq=<Hz>     center frequency (default 1000)
  q=<Q>         filter Q; higher = narrower band (default 1.0)
  bw=<octaves>  alternative to Q: -3 dB bandwidth in octaves (overrides q if set)
  gain=<dB>     output gain applied after filtering (default 0)
  mode=<rbj|fft>
                rbj = 2nd-order resonant bandpass (default, musical, cheap)
                fft = linear-phase brickwall-ish isolation with soft edges

Examples:
  freq=440 q=2
  freq=1200 bw=0.5 gain=-3
  freq=8000 bw=1.0 mode=fft
"""
import sys, wave, struct, math


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


# ---------- biquad ----------

def rbj_bandpass(rate, freq, q, gain_db=0.0):
    """RBJ cookbook constant 0 dB peak bandpass. Returns (b0,b1,b2,a1,a2)."""
    f = max(1.0, min(freq, rate * 0.4999))
    q = max(0.05, q)
    w0 = 2.0 * math.pi * f / rate
    cos_w0 = math.cos(w0)
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    b0 = alpha / a0
    b1 = 0.0
    b2 = -alpha / a0
    a1 = (-2.0 * cos_w0) / a0
    a2 = (1.0 - alpha) / a0
    g = 10.0 ** (gain_db / 20.0)
    return (b0 * g, b1 * g, b2 * g, a1, a2)


def biquad_inplace(x, coef):
    """Direct Form I biquad, in-place on list x."""
    b0, b1, b2, a1, a2 = coef
    x1 = x2 = y1 = y2 = 0.0
    for i in range(len(x)):
        xn = x[i]
        yn = b0 * xn + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2 = x1; x1 = xn
        y2 = y1; y1 = yn
        x[i] = yn


def zero_phase_bandpass(x, coef):
    biquad_inplace(x, coef)
    x.reverse()
    biquad_inplace(x, coef)
    x.reverse()


# ---------- FFT band (linear phase-ish) ----------

def fft_bandpass(x, rate, center, bw_oct):
    """Radix-2 FFT magnitude mask with soft (raised-cosine) band edges."""
    n = len(x)
    if n == 0:
        return
    # pad to next power of two >= n for speed; process only original length
    N = 1
    while N < n:
        N <<= 1
    buf = list(x) + [0.0] * (N - n)

    # iterative radix-2 FFT (in-place, complex as two float lists)
    re = buf
    im = [0.0] * N
    j = 0
    for i in range(1, N):
        bit = N >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            re[i], re[j] = re[j], re[i]
    length = 2
    while length <= N:
        ang = -2.0 * math.pi / length
        wr = math.cos(ang)
        wi = math.sin(ang)
        half = length >> 1
        for start in range(0, N, length):
            cr, ci = 1.0, 0.0
            for k in range(half):
                ar = re[start + k]
                ai = im[start + k]
                br = re[start + k + half] * cr - im[start + k + half] * ci
                bi = re[start + k + half] * ci + im[start + k + half] * cr
                re[start + k] = ar + br
                im[start + k] = ai + bi
                re[start + k + half] = ar - br
                im[start + k + half] = ai - bi
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        length <<= 1

    # build magnitude mask
    f_lo = center * (2.0 ** (-bw_oct / 2.0))
    f_hi = center * (2.0 ** (bw_oct / 2.0))
    edge = max(1.0, (f_hi - f_lo) * 0.15)  # soft edge width in Hz
    for k in range(N // 2 + 1):
        f = k * rate / N
        if f <= f_lo - edge or f >= f_hi + edge:
            m = 0.0
        elif f < f_lo:
            t = (f - (f_lo - edge)) / edge
            m = 0.5 - 0.5 * math.cos(math.pi * t)
        elif f > f_hi:
            t = ((f_hi + edge) - f) / edge
            m = 0.5 - 0.5 * math.cos(math.pi * t)
        else:
            m = 1.0
        if k == 0 or k == N // 2:
            re[k] *= m; im[k] *= m
        else:
            re[k] *= m; im[k] *= m
            re[N - k] *= m; im[N - k] *= m

    # inverse FFT via conjugate trick
    for k in range(N):
        im[k] = -im[k]
    j = 0
    for i in range(1, N):
        bit = N >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            re[i], re[j] = re[j], re[i]
            im[i], im[j] = im[j], im[i]
    length = 2
    while length <= N:
        ang = -2.0 * math.pi / length
        wr = math.cos(ang)
        wi = math.sin(ang)
        half = length >> 1
        for start in range(0, N, length):
            cr, ci = 1.0, 0.0
            for k in range(half):
                ar = re[start + k]
                ai = im[start + k]
                br = re[start + k + half] * cr - im[start + k + half] * ci
                bi = re[start + k + half] * ci + im[start + k + half] * cr
                re[start + k] = ar + br
                im[start + k] = ai + bi
                re[start + k + half] = ar - br
                im[start + k + half] = ai - bi
                cr, ci = cr * wr - ci * wi, cr * wi + ci * wr
        length <<= 1
    inv = 1.0 / N
    for i in range(n):
        x[i] = re[i] * inv


# ---------- process ----------

def process(rate, chans, opts):
    freq = float(opts.get('freq', 1000.0))
    q = float(opts.get('q', 1.0))
    gain_db = float(opts.get('gain', 0.0))
    mode = opts.get('mode', 'rbj').lower()

    if 'bw' in opts:
        bw_oct = max(0.05, float(opts['bw']))
        # convert octave bandwidth to Q at center frequency
        # Q ≈ sqrt(2**bw) / (2**bw - 1)
        q = math.sqrt(2.0 ** bw_oct) / (2.0 ** bw_oct - 1.0)
    else:
        bw_oct = 0.0

    if rate <= 0:
        raise SystemExit('bad sample rate')

    if mode == 'fft':
        if bw_oct <= 0.0:
            # derive octave bandwidth from Q: bw = 2*log2(sqrt(4Q^2+1)/(2Q))
            bw_oct = 2.0 * math.log2(math.sqrt(4.0 * q * q + 1.0) / (2.0 * q))
        for c in chans:
            fft_bandpass(c, rate, freq, bw_oct)
        if gain_db != 0.0:
            g = 10.0 ** (gain_db / 20.0)
            for c in chans:
                for i in range(len(c)):
                    c[i] *= g
    else:
        coef = rbj_bandpass(rate, freq, q, gain_db)
        for c in chans:
            zero_phase_bandpass(c, coef)

    # guard against clipping from resonant peaks / gain
    peak = max((abs(v) for c in chans for v in c), default=0.0)
    if peak > 0.99:
        s = 0.99 / peak
        for c in chans:
            for i in range(len(c)):
                c[i] *= s

    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')