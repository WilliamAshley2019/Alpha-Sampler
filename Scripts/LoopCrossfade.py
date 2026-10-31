"""Crossfade the last xfade_ms into the tail so a loop wraps seamlessly (adds tail padding equal to the fade)."""
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
    ms = float(opts.get('xfade_ms', 50.0))
    fade = max(1, int(rate * ms / 1000.0))
    n = len(chans[0])
    fade = min(fade, n // 4)
    if fade < 2:
        print('warning: sample too short to crossfade')
        return rate, chans
    # extend the buffer by fade frames, and blend the tail into them
    out = []
    for c in chans:
        ext = list(c) + [0.0] * fade
        for i in range(fade):
            t = (i + 1) / (fade + 1)
            head = c[i]
            tail = c[n - fade + i]
            ext[n + i] = tail * (1.0 - t) + head * t
        out.append(ext)
    print('loop crossfade %.1f ms (%d new frames)' % (ms, fade))
    return rate, out

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')