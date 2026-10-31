"""Alpha Sampler edit script: crossfade over [start, end) with 'curve'."""
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


def process(rate, chans, opts):
    """start/end in seconds (fractions of the sample if <1). 'curve' = lin|cos|log."""
    n = len(chans[0])
    if n == 0:
        return rate, chans

    def to_samp(x):
        f = float(x)
        return max(0, min(n, int(round(f * rate)) if f >= 1 else int(round(f * n))))

    start = to_samp(opts.get('start', 0.0))
    end = to_samp(opts.get('end', 1.0))
    if end <= start:
        return rate, chans
    curve = opts.get('curve', 'cos').lower()
    ln = end - start

    for c in chans:
        for i in range(ln):
            t = i / max(1, ln - 1)
            if curve == 'lin':
                w = t
            elif curve == 'log':
                w = t * t
            else:  # cosine
                w = 0.5 - 0.5 * __import__('math').cos(t * 3.141592653589793)
            # fade in from silence at start, full at end
            c[start + i] *= w
    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')