"""Trim leading/trailing silence below a threshold. Slices rescaled to keep positions relative."""
import sys, wave, struct

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
    thr = 10.0 ** (float(opts.get('thr_db', -60.0)) / 20.0)
    pad_ms = float(opts.get('pad_ms', 0.0))
    pad = int(rate * pad_ms / 1000.0)
    n = len(chans[0])
    def level(i):
        return max(abs(c[i]) for c in chans)
    a = 0
    while a < n and level(a) < thr: a += 1
    b = n - 1
    while b > a and level(b) < thr: b -= 1
    a = max(0, a - pad)
    b = min(n - 1, b + pad)
    new_n = b - a + 1
    print('trim: %d -> %d frames (%.3f s)' % (n, new_n, new_n / rate))
    # rescale slices so markers stay at the same relative time
    slices = opts.get('slices', [])
    if slices:
        opts['slices'] = [max(0.0, min(0.9999, (s * n - a) / new_n)) for s in slices]
    return rate, [c[a:b+1] for c in chans]

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')