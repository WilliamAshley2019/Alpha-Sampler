# Alpha Sampler script format

Run from the TOOLS tab (SCRIPT... then RUN). One command per run:

    <python> script.py <in.wav> <out.wav> rate=<Hz> bpm=<host tempo> root=<MIDI note> slices=<f1,f2,...>

* Python: the path set under PYTHON..., otherwise `py -3`, `python`, `python3` in that order. A script is any `.py` file; the plugin lists `Documents/Alpha Sampler/Scripts/*.py`.
* argv[1] `in.wav`: 24-bit PCM stereo WAV copy of the loaded sample (any source format is converted first). The original file is never modified.
* argv[2] `out.wav`: where the script must write the result. WAV, any sample rate or length, mono or stereo (24-bit PCM recommended). It is loaded as the sample; REVERT restores the original.
* Extra args are `key=value` strings: `rate` (engine rate), `bpm` (host tempo), `root` (root note index), `slices` (comma list of slice start fractions 0..1, empty if none). Ignore unknown keys.
* Success = exit code 0 AND `out.wav` exists. Anything else shows the last ~160 characters of stdout/stderr in the status line, so print readable errors.
* Limits: 120 s timeout, no interactive input, no GUI. Standard library only unless that Python has numpy etc. installed.
* Slices are kept after an edit, so keep the length unchanged if the markers should still line up.
* Edison's own scripting API (`import edison` style calls inside FL Studio) is not available here; port the processing to plain Python lists/numpy.

`script_template.py` implements the whole contract (stdlib WAV read/write, argument parsing); edit `process()` only.
