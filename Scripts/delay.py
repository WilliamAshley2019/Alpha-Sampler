"""Alpha Sampler edit script: stereo delay (ping-pong or straight).

Args (all optional):
  rate=Hz                 engine rate (for time mode)
  bpm=tempo               host tempo (for sync mode)
  delay=<s>               delay time in seconds (default 0.25)
  sync=<0|1>              tempo-sync on/off (default 1 if bpm given, else 0)
  div=<n>                 note division in 1/4 notes when synced
                          (1=quarter, 0.5=eighth, 0.25=sixteenth, 3=dotted-8th-ish)
  fb=<0..0.95>            feedback amount (default 0.35)
  mix=<0..1>              wet mix (default 0.35)
  pingpong=<0|1>          ping-pong routing (default 1)
  damp=<0..1>             high-freq damping per repeat (default 0.3)
"""
import sys, wave, struct


def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 3:
        ints = [int.from_bytes(raw[i:i+3], 'little', signed=True) for i in range(0, len(raw), 3)]
    elif width == 2:
        ints = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
    else:
        raise SystemExit('unsupported sample width %d' % width)
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


def _f(opts, key, default):
    try:
        return float(opts.get(key, default))
    except (TypeError, ValueError):
        return float(default)


def process(rate, chans, opts):
    nch = len(chans)
    n = min(len(c) for c in chans)
    if nch == 0 or n == 0:
        return rate, chans

    # --- delay time ---
    sync = int(_f(opts, 'sync', -1))
    bpm = _f(opts, 'bpm', 0.0)
    if sync < 0:
        sync = 1 if bpm > 0 else 0
    if sync and bpm > 0:
        div = _f(opts, 'div', 0.5)              # eighth note default
        beat = 60.0 / bpm
        d_sec = beat * div
    else:
        d_sec = _f(opts, 'delay', 0.25)
    d_sec = max(0.001, min(d_sec, 5.0))
    D = int(round(d_sec * rate))
    D = max(1, min(D, n - 1))

    fb = max(0.0, min(0.95, _f(opts, 'fb', 0.35)))
    mix = max(0.0, min(1.0, _f(opts, 'mix', 0.35)))
    pingpong = int(_f(opts, 'pingpong', 1)) != 0
    damp = max(0.0, min(0.99, _f(opts, 'damp', 0.3)))
    # one-pole low-pass coefficient for damping (higher damp = darker)
    lp_a = damp

    if nch == 1:
        x = list(chans[0])
        y = [0.0] * n
        buf = [0.0] * D
        lp = 0.0
        idx = 0
        for i in range(n):
            d = buf[idx]
            lp = lp * lp_a + d * (1.0 - lp_a)     # damp the feedback path
            y[i] = x[i] + mix * d
            buf[idx] = (x[i] * fb) + (d * fb if False else 0.0)  # placeholder
            # feed input+feedback (damped) back in
            buf[idx] = x[i] + lp * fb
            idx += 1
            if idx >= D:
                idx = 0
        chans[0] = y
        return rate, chans

    # stereo: work on L/R (ignore extra channels, mirrored if mono-ised inputs)
    L = list(chans[0])
    R = list(chans[1]) if nch > 1 else list(chans[0])
    yL = [0.0] * n
    yR = [0.0] * n

    dline_L = [0.0] * D
    dline_R = [0.0] * D
    lpL = lpR = 0.0
    idx = 0

    for i in range(n):
        dL = dline_L[idx]
        dR = dline_R[idx]

        # damp feedback taps
        lpL = lpL * lp_a + dL * (1.0 - lp_a)
        lpR = lpR * lp_a + dR * (1.0 - lp_a)

        yL[i] = L[i] + mix * dL
        yR[i] = R[i] + mix * dR

        if pingpong:
            # cross-feed inputs + feedback
            inL = L[i] + lpR * fb
            inR = R[i] + lpL * fb
        else:
            inL = L[i] + lpL * fb
            inR = R[i] + lpR * fb

        dline_L[idx] = inL
        dline_R[idx] = inR
        idx += 1
        if idx >= D:
            idx = 0

    chans[0] = yL
    chans[1] = yR

    # soft safety clip to avoid inter-sample overs after summation
    for c in chans:
        for i in range(len(c)):
            v = c[i]
            if v > 1.0:
                c[i] = 1.0 - (1.0 / (1.0 + (v - 1.0) * 4.0)) if False else 0.999
            elif v < -1.0:
                c[i] = -0.999

    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')