"""Loudness-match audition renders so candidates are compared by sound, not level.

For every phrase, measures the integrated loudness (ITU-R BS.1770-4: K-weighting,
400 ms blocks, absolute and relative gates) of the reference render and of each
candidate's render, then writes a matched copy of each candidate at the
reference's loudness and prints the gains. Standard library only.

  python -I tools/loudness_match.py build/auditions/r1 --reference baseline \
      --labels furnace-a furnace-b furnace-c [--part mix]

Inputs: <folder>/<label>_<phrase>_<part>.wav (32-bit float stereo, as written by
render_tool). Outputs: <label>_<phrase>_<part>_matched.wav next to them.
"""
import argparse
import array
import math
import pathlib
import struct
import sys


def read_wav(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAV file")
    position, fmt, samples = 12, None, None
    while position + 8 <= len(data):
        chunk, size = data[position:position + 4], struct.unpack("<I", data[position + 4:position + 8])[0]
        body = data[position + 8:position + 8 + size]
        if chunk == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif chunk == b"data":
            samples = array.array("f")
            samples.frombytes(body)
        position += 8 + size + (size & 1)
    if fmt is None or samples is None or fmt[0] not in (3, 0xFFFE) or fmt[5] != 32:
        raise ValueError(f"{path}: expected 32-bit float WAV")
    return fmt[2], fmt[1], samples


def write_wav(path, rate, channels, samples):
    body = samples.tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 3, channels, rate, rate * channels * 4, channels * 4, 32)
    path.write_bytes(header + b"data" + struct.pack("<I", len(body)) + body)


def biquad(signal, b, a):
    """Direct form I, a[0] == 1."""
    out = [0.0] * len(signal)
    x1 = x2 = y1 = y2 = 0.0
    b0, b1, b2 = b
    a1, a2 = a
    for i, x in enumerate(signal):
        y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, x, y1, y
        out[i] = y
    return out


def k_weighting(rate):
    """BS.1770 pre-filter (high shelf) and RLB high-pass for any sample rate."""
    k = math.tan(math.pi * 1681.974450955533 / rate)
    q = 0.7071752369554196
    vh = 10.0 ** (3.999843853973347 / 20.0)
    vb = vh ** 0.4996667741545416
    a0 = 1.0 + k / q + k * k
    shelf = ((vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0), \
            (2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0)
    k = math.tan(math.pi * 38.13547087602444 / rate)
    q = 0.5003270373238773
    a0 = 1.0 + k / q + k * k
    highpass = (1.0, -2.0, 1.0), (2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0)
    return shelf, highpass


def integrated_lufs(rate, channels, samples):
    shelf, highpass = k_weighting(rate)
    weighted = []
    for channel in range(channels):
        signal = list(samples[channel::channels])
        weighted.append(biquad(biquad(signal, *shelf), *highpass))
    block, step = int(0.4 * rate), int(0.1 * rate)
    powers = []
    for start in range(0, len(weighted[0]) - block + 1, step):
        powers.append(sum(sum(v * v for v in w[start:start + block]) / block for w in weighted))
    loudness = lambda power: -0.691 + 10.0 * math.log10(power) if power > 0.0 else -math.inf
    gated = [p for p in powers if loudness(p) > -70.0]
    if not gated:
        return -math.inf
    relative = loudness(sum(gated) / len(gated)) - 10.0
    gated = [p for p in gated if loudness(p) > relative]
    return loudness(sum(gated) / len(gated))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder", type=pathlib.Path)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--labels", nargs="+", required=True)
    parser.add_argument("--part", default="mix")
    options = parser.parse_args()
    suffix = f"_{options.part}.wav"
    phrases = sorted(p.name[len(options.reference) + 1:-len(suffix)]
                     for p in options.folder.glob(f"{options.reference}_*{suffix}"))
    if not phrases:
        sys.exit(f"No {options.reference}_<phrase>{suffix} renders in {options.folder}")
    print(f"{'phrase':12s} {options.reference + ' LUFS':>16s} " + " ".join(f"{label + ' gain dB':>18s}" for label in options.labels))
    for phrase in phrases:
        rate, channels, reference = read_wav(options.folder / f"{options.reference}_{phrase}{suffix}")
        target = integrated_lufs(rate, channels, reference)
        cells = []
        for label in options.labels:
            source = options.folder / f"{label}_{phrase}{suffix}"
            rate, channels, samples = read_wav(source)
            gain_db = target - integrated_lufs(rate, channels, samples)
            gain = 10.0 ** (gain_db / 20.0)
            matched = array.array("f", (value * gain for value in samples))
            write_wav(source.with_name(source.stem + "_matched.wav"), rate, channels, matched)
            cells.append(f"{gain_db:+18.1f}")
        print(f"{phrase:12s} {target:16.1f} " + " ".join(cells))


if __name__ == "__main__":
    main()
