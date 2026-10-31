import sys, wave
src, dst = sys.argv[1], sys.argv[2]
opts = dict(a.split('=', 1) for a in sys.argv[3:] if '=' in a)   # rate, bpm, root
with wave.open(src, 'rb') as w:
    p = w.getparams()
    data = w.readframes(p.nframes)
fs = p.sampwidth * p.nchannels
frames = [data[i:i + fs] for i in range(0, len(data), fs)]
frames.reverse()   # edit here
with wave.open(dst, 'wb') as o:
    o.setparams(p)
    o.writeframes(b''.join(frames))
