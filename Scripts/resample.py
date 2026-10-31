"""Alpha Sampler script: resample to a new sample rate (Lanczos-3 windowed sinc).

Resamples the input to opts['rate'] (engine rate) by default, or to an
explicit target= Hz. LENGTH CHANGES - a note is printed and slice markers
may need re-detection. Good quality (Lanczos-3); stdlib only, but O(N*K)
so a 60 s stereo file takes a few seconds.

Usage: resample.py in.wav out.wav rate=<Hz> bpm=<tempo> root=<note> slices=<f1,f2,...> [target=48000]
Optional keys:
  target  desired output rate in Hz (default: engine rate)
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


def _lanczos(x, a):
    if x == 0.0:
        return 1.0
    if x <= -a or x >= a:
        return 0.0
    return a * math.sin(math.pi * x) * math.sin(math.pi * x / a) / (math.pi * math.pi * x * x)


def resample_channel(src, ratio, a=3):
    n_in = len(src)
    if n_in == 0:
        return []
    n_out = max(1, int(round(n_in * ratio)))
    out = [0.0] * n_out
    step = 1.0 / ratio
    for i in range(n_out):
        pos = i * step
        i0 = int(math.floor(pos))
        frac = pos - i0
        acc = 0.0
        wsum = 0.0
        for k in range(i0 - a + 1, i0 + a + 1):
            if 0 <= k < n_in:
                w = _lanczos((k - i0) - frac, a)
                acc += src[k] * w
                wsum += w
        out[i] = acc / wsum if wsum != 0.0 else 0.0
    return out


def process(rate, chans, opts):
    target = int(float(opts.get('target', 0) or 0)) or opts['rate'] or rate
    if target == rate:
        print('note: target rate equals source rate; passing through unchanged')
        return rate, chans
    ratio = target / rate
    print('note: length changed %d -> %d frames; slice markers may need re-detection'
          % (len(chans[0]) if chans else 0, int(round((len(chans[0]) if chans else 0) * ratio))))
    out = [resample_channel(c, ratio) for c in chans]
    return target, out


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('usage: resample.py in.wav out.wav [key=value ...]', file=sys.stderr)
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