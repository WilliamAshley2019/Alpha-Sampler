"""Alpha Sampler script: mid/side stereo width control.

Scales the S component of M/S: M = 0.5*(L+R), S = 0.5*(L-R).
width=0 -> mono (both channels = M); 1.0 -> unchanged; >1 -> wider.

Length-preserving (slice markers stay aligned). Stdlib only.

Usage: stereo_width.py in.wav out.wav rate=<Hz> bpm=<tempo> root=<note> slices=<f1,f2,...> [width=1.5]
Optional keys:
  width   side gain multiplier (default 1.5; try 0.5 to narrow, 2.0 to widen)
"""
import sys, wave, struct


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
    width = float(opts.get('width', 1.5))
    if len(chans) < 2:
        print('note: mono input, stereo width is a no-op')
        return rate, chans
    L, R = chans[0], chans[1]
    n = min(len(L), len(R))
    outL = [0.0] * n
    outR = [0.0] * n
    for i in range(n):
        m = 0.5 * (L[i] + R[i])
        s = 0.5 * (L[i] - R[i]) * width
        outL[i] = m + s
        outR[i] = m - s
    return rate, [outL, outR]


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