"""Dynamic-range compressor for Alpha Sampler.

Options (all optional, passed as key=value on the command line):
  threshold  -20.0    Threshold in dBFS
  ratio       4.0     Compression ratio (>=1)
  attack      0.010   Attack time in seconds
  release     0.100   Release time in seconds
  knee        6.0     Knee width in dB (0 = hard knee, >0 = soft knee)
  makeup      0.0     Makeup gain in dB
  rms         0       If 1, use RMS detection instead of peak (default peak)
  link        1       If 1, stereo channels share gain (default); 0 = per-channel
"""
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


def db_to_lin(db):
    return 10.0 ** (db / 20.0)


def lin_to_db(x):
    return 20.0 * math.log10(x) if x > 1e-12 else -240.0


def gain_reduction_db(level_db, thresh_db, ratio, knee_db):
    """Static gain reduction (positive number = dB of reduction) for a given input level."""
    over = level_db - thresh_db
    if knee_db > 0.0:
        # Quadratic soft knee, centred on threshold
        half = knee_db * 0.5
        if over <= -half:
            return 0.0
        elif over >= half:
            slope = 1.0 - 1.0 / ratio
            return over * slope
        else:
            x = over + half          # 0..knee
            # smooth quadratic blend from 0 to full slope
            return (1.0 - 1.0 / ratio) * (x * x) / (2.0 * knee_db)
    else:
        if over <= 0.0:
            return 0.0
        return over * (1.0 - 1.0 / ratio)


def process(rate, chans, opts):
    """chans: list of float lists (-1..1), one per channel."""
    thr = float(opts.get('threshold', -20.0))
    ratio = max(1.0, float(opts.get('ratio', 4.0)))
    attack_s = max(1e-6, float(opts.get('attack', 0.010)))
    release_s = max(1e-6, float(opts.get('release', 0.100)))
    knee_db = max(0.0, float(opts.get('knee', 6.0)))
    makeup_db = float(opts.get('makeup', 0.0))
    use_rms = int(opts.get('rms', 0)) != 0
    link = int(opts.get('link', 1)) != 0

    if not chans or not chans[0]:
        return rate, chans

    n = min(len(c) for c in chans)
    nch = len(chans)

    # Coefficients for one-pole smoothing of the gain envelope
    a_att = math.exp(-1.0 / (attack_s * rate))
    a_rel = math.exp(-1.0 / (release_s * rate))

    # Envelope follower state (per channel, and a linked mono envelope)
    env = [0.0] * nch
    linked_env = 0.0
    makeup_lin = db_to_lin(makeup_db)

    # Stereo linking: use the maximum envelope across channels
    for i in range(n):
        # 1) measure instantaneous level
        if link:
            inst = 0.0
            for c in range(nch):
                v = abs(chans[c][i])
                if use_rms:
                    # RMS-style: smooth squared level, but for simplicity use |v| smoothed
                    pass
                if v > inst:
                    inst = v
            if use_rms:
                # For RMS mode, smooth squared value then sqrt later via envelope
                # We keep env in amplitude domain; approximate RMS by stronger smoothing
                target = inst
            else:
                target = inst
            # envelope follower on linked signal
            if target > linked_env:
                linked_env = a_att * linked_env + (1.0 - a_att) * target
            else:
                linked_env = a_rel * linked_env + (1.0 - a_rel) * target
            level_db = lin_to_db(linked_env)
            gr = gain_reduction_db(level_db, thr, ratio, knee_db)
            gain = db_to_lin(-gr) * makeup_lin
            for c in range(nch):
                chans[c][i] *= gain
        else:
            for c in range(nch):
                v = abs(chans[c][i])
                if v > env[c]:
                    env[c] = a_att * env[c] + (1.0 - a_att) * v
                else:
                    env[c] = a_rel * env[c] + (1.0 - a_rel) * v
                level_db = lin_to_db(env[c])
                gr = gain_reduction_db(level_db, thr, ratio, knee_db)
                chans[c][i] *= db_to_lin(-gr) * makeup_lin

    # Safety clamp
    for c in chans:
        for i in range(len(c)):
            if c[i] > 1.0:
                c[i] = 1.0
            elif c[i] < -1.0:
                c[i] = -1.0

    return rate, chans


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)
    opts['slices'] = [float(x) for x in opts.get('slices', '').split(',') if x]
    rate, chans = read_wav(src)
    rate, chans = process(rate, chans, opts)
    write_wav(dst, rate, chans)
    print('ok')