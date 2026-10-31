"""Alpha Sampler edit script: exact DC offset removal (whole file or per slice)."""
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
    n = len(chans[0])
    if n == 0:
        return rate, chans
    # If slices given, de-DC each slice independently (and inter-slice region)
    fracs = opts.get('slices') or []
    if isinstance(fracs, str):
        fracs = [float(x) for x in fracs.split(',') if x]
    bounds = [0] + [int(round(f * n)) for f in fracs] + [n]
    bounds = sorted(set(b for b in bounds if 0 <= b <= n))
    for a, b in zip(bounds[:-1], bounds[1:]):
        if b <= a:
            continue
        ln = b - a
        for c in chans:
            mean = sum(c[a:b]) / ln
            for i in range(a, b):
                c[i] -= mean
    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')