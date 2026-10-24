# Test sequences

`sequences.json` defines deterministic MIDI phrases for automated tests. They are development fixtures, not patterns inside the instrument.

The `standard_sequences` and `bass_sequences` CTest suites convert each phrase to a type-0 MIDI file, render it with the real `render_tool` at 512- and 73-sample blocks, and require byte-identical WAV output. Renders are written under the build tree and are disposable.

All phrases: 120 BPM, 960 ticks per quarter note, channel 1, velocity 127, 48 kHz stereo float WAV, Output 0 dB, 100 ms tail.

| ID | Content | Purpose |
|---|---|---|
| `low-riff` | 13 notes, MIDI 22–31, 4 s | Short gates, repeated low roots, rests and pitch changes |
| `power-chords` | 20 notes, 5 s | Root/fifth/octave chords, then one eight-note chord |
| `chromatic` | MIDI 0–127, 16.25 s | Every input pitch |
| `boundary-notes` | MIDI 0, 1, 11, 12, 126, 127, 6 s | Holds at the keyboard extremes; the bass octave below MIDI 0 must stay exact |
| `held-sweep` | MIDI 45 held, 6 s | Sustain, pitch-wheel positions, release while the sustain pedal is down (it is ignored) |

Rows: notes `[startBeat, durationBeats, channel, midiNote, velocity]`, bends `[beat, channel, wheel0to16383]`, controllers `[beat, channel, ccNumber, value]`.
