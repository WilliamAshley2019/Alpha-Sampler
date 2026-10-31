"""Alpha Sampler edit script: downward expander / gate (stereo-linked)."""
import sys, wave, struct, math


def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 3:
        ints = [int.from_bytes(raw[i:i + 3], 'little', signed=True) for i in range(0, len(raw), 3)]
    elif width == 2:
        ints = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
    else:
        raise SystemExit('unsupported sample width %d (plugin always sends 24-bit)' % width)
    scale = float(1 << (8 * width - 1))
    return rate, [[v / scale for v in ints[c::ch]] for c in range(ch)]


def write_wav(path, rate, chans):
    n = min(len(c) for c in chans)
    out = bytearray()
    for i in range(n):
        for c in chans:
            v = max(-8388608, min(8388607, int(round(c[i] * 8388608.0))))
            out += v.to_bytes(3, 'little', signed=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(len(chans)); w.setsampwidth(3); w.setframerate(rate)
        w.writeframes(bytes(out))


def process(rate, chans, opts):
    n = len(chans[0])
    thr = float(opts.get('threshold', 0.02))
    atk = max(1, int(rate * float(opts.get('attack_ms', 1.0)) / 1000.0))
    rel = max(1, int(rate * float(opts.get('release_ms', 80.0)) / 1000.0))
    att_coef = 1.0 - math.exp(-1.0 / atk)
    rel_coef = 1.0 - math.exp(-1.0 / rel)

    gain = 0.0  # 0..1
    for i in range(n):
        # stereo-linked detector
        amp = 0.0
        for c in chans:
            a = abs(c[i])
            if a > amp:
                amp = a
        target = 1.0 if amp > thr else 0.0
        coef = att_coef if target > gain else rel_coef
        gain += (target - gain) * coef
        for c in chans:
            c[i] *= gain
    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')