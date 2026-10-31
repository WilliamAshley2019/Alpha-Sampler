"""Alpha Sampler script: Haas stereo widener.

Delays one channel by ms milliseconds (default 12 ms, right channel).
Length is unchanged: the delayed channel is filled with silence at the tail.
Stdlib only. Mono input is dual-monoed first.

Usage: haas.py in.wav out.wav rate=<Hz> bpm=<tempo> root=<note> slices=<f1,f2,...> [ms=12] [side=1]
Optional keys:
  ms    delay in milliseconds (default 12; useful range 2-30)
  side  1 = delay right (default), 0 = delay left
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
    ms = float(opts.get('ms', 12.0))
    side = int(float(opts.get('side', 1)))
    if len(chans) < 2:
        print('note: mono input, dual-monoed before Haas delay')
        chans = [list(chans[0]), list(chans[0])]
    d = int(round(ms * 0.001 * rate))
    if d < 1:
        print('note: delay < 1 sample, no-op')
        return rate, chans
    L, R = chans[0], chans[1]
    n = min(len(L), len(R))
    if n <= d:
        print('note: file shorter than delay, no-op')
        return rate, chans
    if side:
        newR = [0.0] * d + R[:n - d]
        return rate, [list(L[:n]), newR]
    else:
        newL = [0.0] * d + L[:n - d]
        return rate, [newL, list(R[:n])]


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