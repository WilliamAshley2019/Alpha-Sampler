"""Alpha Sampler script: constant-power pan / balance.

pan = -1..1 (-1 hard L, 0 centre, +1 hard R). Constant-power law means
the sum of squares stays constant; mono-in becomes stereo-out. Length
preserved; markers stay aligned. Stdlib only.

Usage: pan.py in.wav out.wav rate=<Hz> bpm=<tempo> root=<note> slices=<f1,f2,...> [pan=0.0]
Optional keys:
  pan   -1.0 (L) .. 0.0 (centre) .. +1.0 (R), default 0.0
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
    pan = max(-1.0, min(1.0, float(opts.get('pan', 0.0))))
    # Constant power: theta 0..pi/2
    theta = (pan + 1.0) * 0.25 * math.pi
    gl = math.cos(theta)
    gr = math.sin(theta)
    if len(chans) == 1:
        c = chans[0]
        return rate, [[v * gl * math.sqrt(2.0) for v in c],
                      [v * gr * math.sqrt(2.0) for v in c]]
    if len(chans) == 0:
        return rate, chans
    L = chans[0]
    R = chans[1] if len(chans) > 1 else chans[0]
    # Balance law for stereo: scale each side, preserving centre at 1.0
    if pan <= 0.0:
        # attenuate R, keep L
        sL = 1.0
        sR = math.cos(pan * 0.5 * math.pi)  # pan=0 -> 1, pan=-1 -> 0
    else:
        sL = math.cos(pan * 0.5 * math.pi)
        sR = 1.0
    return rate, [[v * sL for v in L], [v * sR for v in R]]


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('usage: pan.py in.wav out.wav [key=value ...]', file=sys.stderr)
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