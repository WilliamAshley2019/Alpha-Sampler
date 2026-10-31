"""Peak-normalize to a target dBFS (default -1.0). Length unchanged."""
import sys, wave, struct

def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 3:
        ints = [int.from_bytes(raw[i:i+3], 'little', signed=True) for i in range(0, len(raw), 3)]
    elif width == 2:
        ints = list(struct.unpack('<%dh' % (len(raw)//2), raw))
    else:
        raise SystemExit('unsupported sample width %d' % width)
    scale = float(1 << (8*width - 1))
    return rate, [[v/scale for v in ints[c::ch]] for c in range(ch)]

def write_wav(path, rate, chans):
    n = min(len(c) for c in chans)
    out = bytearray()
    for i in range(n):
        for c in chans:
            v = max(-8388608, min(8388607, int(round(c[i]*8388608.0))))
            out += v.to_bytes(3, 'little', signed=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(len(chans)); w.setsampwidth(3); w.setframerate(rate)
        w.writeframes(bytes(out))

def process(rate, chans, opts):
    target_db = float(opts.get('target', -1.0))
    peak = max((abs(v) for c in chans for v in c), default=0.0)
    if peak < 1e-9:
        print('warning: silent input, nothing to normalize')
        return rate, chans
    target = 10.0 ** (target_db / 20.0)
    g = target / peak
    print('peak %.3f -> %.3f  (%.2f dB gain)' % (peak, target, 20 * __import__('math').log10(g)))
    for c in chans:
        for i in range(len(c)):
            c[i] *= g
            if c[i] > 1.0: c[i] = 1.0
            elif c[i] < -1.0: c[i] = -1.0
    return rate, chans

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')