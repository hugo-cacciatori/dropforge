"""Writes the musical listening phrases as standard MIDI files.

Every candidate in a listening round is rendered from exactly these files, so
the owner compares sounds, never different playing. Standard library only.

    python -I tools/make_phrases.py <output-folder>
"""
import struct
import sys
from pathlib import Path

TICKS = 960  # per quarter note


def phrase_chords():
    """The power-chords test fixture raised an octave (MIDI 35-79), the
    register the owner preferred for chord auditions."""
    import json
    fixtures = json.loads((Path(__file__).resolve().parent.parent / 'tests' / 'fixtures' / 'sequences.json')
                          .read_text(encoding='utf-8'))
    source = next(s for s in fixtures['sequences'] if s['id'] == 'power-chords')
    rows = [(start, duration, note + 12) for start, duration, _, note, _ in source['notes']]
    return fixtures['tempo_bpm'], float(source['length_beats']), rows


def phrase_breakdown():
    """A half-time metal breakdown in drop C: chugs, rests, stabs and a slide
    up the neck. Anvil plays an octave under the lowest note."""
    root, fifth, octave = 36, 43, 48
    rows = []
    pattern = [0.0, 0.25, 0.75, 1.0, 1.5, 2.25, 2.5, 3.0, 3.25, 3.5]
    for bar in range(2):
        for step in pattern:
            start = bar * 4 + step
            rows += [(start, 0.2, root), (start, 0.2, fifth), (start, 0.2, octave)]
    for i, base in enumerate([39, 41, 42, 43]):  # rising power-chord stabs
        start = 8 + i * 0.5
        rows += [(start, 0.35, base), (start, 0.35, base + 7), (start, 0.35, base + 12)]
    rows += [(10.0, 2.0, root), (10.0, 2.0, fifth), (10.0, 2.0, octave)]
    return 140, 12.0, rows


def phrase_stabs():
    """Glitch-hop/future-bass style chord stabs in F# minor with a held
    resolution: tests chord readability, width and hard chops."""
    chords = [[54, 57, 61, 64], [50, 54, 57, 62], [52, 56, 59, 64], [49, 54, 57, 61]]
    rhythm = [0.0, 0.75, 1.5, 2.0, 2.75, 3.25]
    rows = []
    for bar, chord in enumerate(chords[:3]):
        for step in rhythm:
            rows += [(bar * 4 + step, 0.3, n) for n in chord]
    rows += [(12.0, 4.0, n) for n in chords[3]]
    return 110, 16.0, rows


def phrase_held():
    """One sustained wide chord for lushness, movement and stereo image."""
    return 120, 9.0, [(0.0, 8.0, n) for n in [40, 47, 52, 55, 59, 64]]


def phrase_anthem():
    """A lead melody over power chords in E minor (Em, C, G, D), all three
    instruments together: Anvil takes the roots, Furnace the chords and Blade
    the melody, the highest key. Bars 1-2 are played legato (each note
    overlaps the next), bars 3-4 detached, so Portamento's modes differ."""
    rows = []
    for bar, root in enumerate([40, 36, 43, 38]):
        rows += [(bar * 4.0, 4.0, n) for n in (root, root + 7, root + 12)]
    legato = [(0, 64), (1, 67), (2, 69), (3, 71), (4, 74), (5, 71), (6, 69), (7, 67)]
    rows += [(beat, 1.1, note) for beat, note in legato]
    detached = [(8, 64), (9, 66), (10, 67), (11, 71), (12, 69), (13.5, 67), (14, 66)]
    rows += [(beat, 0.6, note) for beat, note in detached]
    rows += [(14.5, 1.5, 64)]
    return 120, 16.0, rows


def phrase_line():
    """A lead line for Blade on its own: a legato run up (overlapping notes),
    a rest, then detached notes back down, for hearing Portamento."""
    rows = [(beat, 0.6, note) for beat, note in [(0, 64), (0.5, 67), (1, 71), (1.5, 74)]]
    rows += [(2.0, 1.5, 76)]
    rows += [(beat, 0.4, note) for beat, note in [(4, 74), (4.5, 71), (5, 67)]]
    rows += [(5.5, 2.0, 64)]
    return 110, 8.0, rows


PHRASES = {
    'chords': phrase_chords,
    'breakdown': phrase_breakdown,
    'stabs': phrase_stabs,
    'held': phrase_held,
    'anthem': phrase_anthem,
    'line': phrase_line,
}


def variable_length(value):
    data = [value & 0x7F]
    value >>= 7
    while value:
        data.insert(0, 0x80 | (value & 0x7F))
        value >>= 7
    return bytes(data)


def write_midi(path, bpm, length_beats, rows, velocity=127):
    events = []  # (tick, order, bytes): note-offs sort before note-ons at equal ticks
    for start, duration, note in rows:
        on = round(start * TICKS)
        off = round((start + duration) * TICKS)
        events.append((on, 1, bytes([0x90, note, velocity])))
        events.append((off, 0, bytes([0x80, note, 0])))
    end = round(length_beats * TICKS)
    events.sort(key=lambda e: (e[0], e[1]))
    tempo = round(60_000_000 / bpm)
    track = bytearray(b'\x00\xff\x51\x03' + tempo.to_bytes(3, 'big'))
    now = 0
    for tick, _, message in events:
        track += variable_length(tick - now) + message
        now = tick
    track += variable_length(max(0, end - now)) + b'\xff\x2f\x00'
    header = b'MThd' + struct.pack('>IHHH', 6, 0, 1, TICKS)
    path.write_bytes(header + b'MTrk' + struct.pack('>I', len(track)) + bytes(track))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    for name, build in PHRASES.items():
        bpm, length, rows = build()
        write_midi(folder / f'{name}.mid', bpm, length, rows)
        print(f'{name}.mid  {bpm} BPM  {length:g} beats  {len(rows)} notes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
