"""Alpha Sampler edit script: stereo -> mono (equal-power downmix)."""
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
    """Downmix to mono. opt 'mode' = 'sum' | 'avg' | 'left' | 'right'.
    Default 'sum' = equal-power (divide by sqrt(N)); 'avg' = divide by N."""
    n = len(chans[0])
    mode = opts.get('mode', 'sum').lower()
    if len(chans) == 1:
        return rate, chans
    left, right = chans[0], chans[1]
    if mode == 'left':
        mono = list(left)
    elif mode == 'right':
        mono = list(right)
    elif mode == 'avg':
        mono = [(left[i] + right[i]) * 0.5 for i in range(n)]
    else:  # 'sum' equal-power
        g = 1.0 / (2.0 ** 0.5)
        mono = [(left[i] + right[i]) * g for i in range(n)]
    # simple peak guard
    peak = max((abs(v) for v in mono), default=0.0)
    if peak > 0.999:
        s = 0.999 / peak
        mono = [v * s for v in mono]
    return rate, [mono]


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')