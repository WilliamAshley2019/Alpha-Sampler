"""Resample so the sample spans exactly `beats` beats at host `bpm`. Speed factor = target_duration / current_duration."""
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
    bpm = float(opts.get('bpm', 120.0))
    beats = float(opts.get('beats', 4.0))
    if bpm <= 0 or beats <= 0:
        raise SystemExit('bpm and beats must be positive')
    target_len = int(round(beats * 60.0 / bpm * rate))
    n = len(chans[0])
    if target_len < 2 or n < 2:
        raise SystemExit('sample too short to tempo-sync')
    speed = n / target_len  # >1 means we are squashing (faster playback)
    print('tempo sync: %d -> %d frames (speed %.3f)' % (n, target_len, speed))
    out = []
    for c in chans:
        res = [0.0] * target_len
        for i in range(target_len):
            p = i * speed
            i0 = int(p)
            i1 = min(n - 1, i0 + 1)
            f = p - i0
            res[i] = c[i0] + (c[i1] - c[i0]) * f
        out.append(res)
    # slices scale to new length so they line up
    slices = opts.get('slices', [])
    if slices:
        opts['slices'] = [max(0.0, min(0.9999, s)) for s in slices]
    return rate, out

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')