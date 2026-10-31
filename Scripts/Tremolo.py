"""Alpha Sampler edit script: tempo-synced tremolo."""
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


_NOTE_DIV = {
    '1/1': 4.0, '1/2': 2.0, '1/4': 1.0, '1/8': 0.5, '1/16': 0.25, '1/32': 0.125,
    '1/4t': 2.0 / 3.0, '1/8t': 1.0 / 3.0, '1/16t': 1.0 / 6.0,
    '1/4d': 1.5, '1/8d': 0.75, '1/16d': 0.375,
}


def process(rate, chans, opts):
    n = len(chans[0])
    bpm = float(opts.get('bpm', 120.0))
    div = opts.get('rate', '1/8').strip()
    beats_per_cycle = _NOTE_DIV.get(div, 0.5)
    cycle_secs = 4.0 * 60.0 / bpm * beats_per_cycle  # one cycle = N beats
    if cycle_secs <= 0:
        return rate, chans
    depth = max(0.0, min(1.0, float(opts.get('depth', 0.7))))
    shape = opts.get('shape', 'sine').lower()

    for i in range(n):
        ph = (i / rate) / cycle_secs
        ph -= math.floor(ph)
        if shape == 'tri':
            u = 1.0 - abs(2.0 * ph - 1.0)
        elif shape == 'square':
            u = 1.0 if ph < 0.5 else 0.0
        else:
            u = 0.5 - 0.5 * math.cos(ph * 2.0 * math.pi)
        g = 1.0 - depth + depth * u
        for c in chans:
            c[i] *= g
    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')