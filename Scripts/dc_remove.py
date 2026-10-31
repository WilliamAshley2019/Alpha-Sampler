"""Alpha Sampler script: DC offset removal.

Default: subtract each channel's mean (fast, exact for constant DC).
hp=1:    also apply a 20 Hz one-pole high-pass to remove drifting DC/rumble.

Length-preserving. Stdlib only.

Usage: dc_remove.py in.wav out.wav rate=<Hz> bpm=<tempo> root=<note> slices=<f1,f2,...> [hp=0] [hp_hz=20]
Optional keys:
  hp      1 = enable 20 Hz high-pass after mean subtraction (default 0)
  hp_hz   high-pass corner in Hz (default 20)
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
    hp = int(float(opts.get('hp', 0)))
    hp_hz = float(opts.get('hp_hz', 20.0))
    out = []
    for c in chans:
        n = len(c)
        if n == 0:
            out.append([])
            continue
        mean = sum(c) / n
        y = [c[i] - mean for i in range(n)]
        if hp:
            # one-pole high-pass: y = x - lp; lp += a*(x - lp)
            a = 1.0 - math.exp(-2.0 * math.pi * hp_hz / max(1.0, float(rate)))
            lp = 0.0
            z = [0.0] * n
            for i in range(n):
                lp += a * (y[i] - lp)
                z[i] = y[i] - lp
            y = z
        out.append(y)
    if hp:
        print('note: applied 20 Hz high-pass in addition to mean subtraction')
    return rate, out


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('usage: script.py in.wav out.wav [key=value ...]', file=sys.stderr)
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