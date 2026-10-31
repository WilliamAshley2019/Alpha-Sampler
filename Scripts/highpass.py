"""Alpha Sampler script: DC removal + one-pole high-pass.

Subtracts the channel mean (DC removal), then applies a cascaded one-pole
high-pass at freq Hz (default 20 Hz = DC/rumble filter). Length preserved.
Stdlib only.

Usage: highpass.py in.wav out.wav rate=<Hz> bpm=<tempo> root=<note> slices=<f1,f2,...> [freq=20]
Optional keys:
  freq  high-pass corner in Hz (default 20)
"""
import sys, wave, struct, math


def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 1:
        ints = [b - 128 for b in raw]
        scale = 128.0
    elif width == 2:
        ints = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
        scale = 32768.0
    elif width == 3:
        ints = [int.from_bytes(raw[i:i + 3], 'little', signed=True) for i in range(0, len(raw), 3)]
        scale = 8388608.0
    elif width == 4:
        ints = list(struct.unpack('<%di' % (len(raw) // 4), raw))
        scale = 2147483648.0
    else:
        raise SystemExit('unsupported sample width %d' % width)
    chans = [[v / scale for v in ints[c::ch]] for c in range(ch)]
    return rate, chans


def write_wav(path, rate, chans, bits=24):
    n = min(len(c) for c in chans)
    nch = len(chans)
    if bits == 24:
        out = bytearray()
        for i in range(n):
            for c in chans:
                v = int(round(max(-1.0, min(1.0, c[i])) * 8388607.0))
                out += v.to_bytes(3, 'little', signed=True)
        width = 3
    elif bits == 16:
        out = bytearray()
        for i in range(n):
            for c in chans:
                v = int(round(max(-1.0, min(1.0, c[i])) * 32767.0))
                out += v.to_bytes(2, 'little', signed=True)
        width = 2
    else:
        raise SystemExit('unsupported output width %d' % bits)
    with wave.open(path, 'wb') as w:
        w.setnchannels(nch)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(bytes(out))


def parse_args(argv):
    opts = {}
    for a in argv[3:]:
        if '=' in a:
            k, v = a.split('=', 1)
            opts[k] = v
    opts['rate'] = int(float(opts.get('rate', 0))) or None
    opts['bpm'] = float(opts.get('bpm', 0.0)) or None
    opts['root'] = int(float(opts.get('root', 60)))
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    return opts


def process(rate, chans, opts):
    freq = max(1.0, float(opts.get('freq', 20.0)))
    rc = 1.0 / (2.0 * math.pi * freq)
    dt = 1.0 / rate
    alpha = rc / (rc + dt)
    out = []
    for c in chans:
        n = len(c)
        if n == 0:
            out.append([])
            continue
        mean = sum(c) / n
        x1 = 0.0
        y1 = 0.0
        x2 = 0.0
        y2 = 0.0
        r = [0.0] * n
        for i in range(n):
            x = c[i] - mean
            y1 = alpha * (y1 + x - x1)
            x1 = x
            y2 = alpha * (y2 + y1 - x2)
            x2 = y1
            r[i] = y2
        out.append(r)
    return rate, out


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('usage: highpass.py in.wav out.wav [key=value ...]', file=sys.stderr)
        sys.exit(2)
    src, dst = sys.argv[1], sys.argv[2]
    opts = parse_args(sys.argv)
    try:
        rate, chans = read_wav(src)
        rate, chans = process(rate, chans, opts)
        write_wav(dst, rate, chans)
        print('ok')
    except SystemExit:
        raise
    except Exception as e:
        print('error: %s' % e, file=sys.stderr)
        sys.exit(1)