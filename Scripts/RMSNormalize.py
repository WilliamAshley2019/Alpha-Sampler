"""RMS-normalize to a target dBFS (default -18 dBFS RMS). Length unchanged."""
import sys, wave, struct, math

def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 3:
        ints = [int.from_bytes(raw[i:i+3], 'little', signed=True) for i in range(0, len(raw), 3)]
    else:
        ints = list(struct.unpack('<%dh' % (len(raw)//2), raw))
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
    target_db = float(opts.get('target', -18.0))
    sum_sq = 0.0
    count = 0
    for c in chans:
        for v in c:
            sum_sq += v*v
            count += 1
    if count == 0 or sum_sq <= 0.0:
        print('warning: silent input')
        return rate, chans
    rms = math.sqrt(sum_sq / count)
    target = 10.0 ** (target_db / 20.0)
    g = target / rms
    # safety: don't let peak exceed 0 dBFS
    peak = max((abs(v) for c in chans for v in c), default=0.0)
    if peak * g > 1.0:
        g = 1.0 / peak
        print('note: gain limited to avoid clipping')
    print('rms %.3f -> %.3f  (%.2f dB)' % (rms, rms*g, 20*math.log10(g)))
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