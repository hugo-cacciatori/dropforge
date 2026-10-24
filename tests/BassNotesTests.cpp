#include "dsp/BassNotes.h"
#include "plugin/MidiDispatch.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
using Change = dropforge::BassNotes::Change;
}

int main()
{
    {
        dropforge::BassNotes newest;
        newest.setPolicy(dropforge::BassNotes::Policy::newest);
        newest.noteOn(1, 60, 70); newest.noteOn(2, 48, 110); newest.noteOn(1, 72, 90);
        require(newest.target().note == 72, "newest key wins in either pitch direction");
        newest.noteOff(2, 48);
        require(newest.target().note == 72, "non-selected release leaves newest note unchanged");
        newest.noteOff(1, 72);
        require(newest.target().note == 60, "release falls back to newest remaining held key");
        newest.noteOn(2, 60, 110);
        require(newest.target().channel == 2, "newest same-pitch channel owns velocity and bend");
        newest.noteOn(1, 72, 90); newest.noteOn(1, 60, 127);
        require(newest.target().note == 60 && newest.target().channel == 1, "repeat press moves existing key to newest without stacking");
        newest.noteOff(1, 60);
        require(newest.target().note == 72, "one note-off clears repeated key");
        newest.allNotesOff(1);
        require(newest.target().note == 60 && newest.target().channel == 2, "panic removes only its channel before fallback");
        newest.noteOn(2, 60, 0);
        require(newest.target().note == -1, "velocity-zero newest note releases");
        for (int channel = 1; channel <= 16; ++channel) for (int note = 0; note < 128; ++note)
            newest.noteOn(channel, note, 100);
        require(newest.target().channel == 16 && newest.target().note == 127, "newest full ledger stays bounded");
        newest.noteOn(1, 0, 127);
        require(newest.target().channel == 1 && newest.target().note == 0, "full-ledger repeated oldest becomes newest");
        newest.noteOff(1, 0);
        require(newest.target().channel == 16 && newest.target().note == 127, "full-ledger repeat retains deterministic fallback");
    }
    dropforge::BassNotes notes;
    require(notes.target().note == -1, "empty selection");
    require(notes.noteOn(1, 52, 100) == Change::attack, "first key attacks");
    require(notes.noteOn(1, 57, 40) == Change::none, "higher overlap cannot retrigger");
    require(notes.noteOn(2, 40, 80) == Change::none, "lower overlap cannot interrupt lock");
    require(notes.target().note == 52 && notes.target().velocity == 100, "locked pitch and velocity");
    require(notes.noteOff(1, 52) == Change::attack && notes.target().note == 57,
            "handover uses oldest remaining key, not lowest or newest");
    require(notes.target().velocity == 40, "handover recalls that key's velocity");
    require(notes.noteOn(1, 57, 70) == Change::attack, "selected repeated note articulates");
    require(notes.noteOn(2, 40, 110) == Change::none, "background repeat does not articulate");
    require(notes.noteOn(1, 57, 0) == Change::attack && notes.target().channel == 2,
            "velocity zero releases and hands over across channels");
    require(notes.target().velocity == 110, "background repeat updates remembered velocity");
    require(notes.noteOff(1, 40) == Change::none && notes.target().note == 40,
            "same pitch on another channel cannot release selection");
    require(notes.allNotesOff(2) == Change::release && notes.target().note == -1,
            "last held key releases");
    require(notes.noteOn(1, 60, 127) == Change::attack, "new phrase attacks");
    notes.noteOn(1, 62, 127);
    notes.noteOn(3, 64, 90);
    require(notes.allNotesOff(1) == Change::attack && notes.target().note == 64,
            "channel panic removes all matching keys before handover");
    require(notes.allNotesOff(1) == Change::none, "unrelated panic leaves selection alone");
    notes.noteOn(4, 65, 50);
    require(notes.noteOff(4, 65) == Change::none && notes.target().note == 64,
            "unselected key release preserves pitch and attack");
    notes.reset();
    require(notes.target().note == -1, "reset clears every key");
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note)
        {
            require(notes.noteOn(channel, note, 127) == Change::attack, "every MIDI key playable");
            const auto frequency = notes.target().frequency;
            const auto expected = 220.0 * std::pow(2.0, (note - 69.0) / 12.0);
            require(std::isfinite(frequency) && std::abs(frequency / expected - 1.0) < 1.0e-12,
                    "all notes exactly one octave down including below MIDI zero");
            notes.noteOff(channel, note);
        }
    notes.noteOn(1, 0, 127);
    require(notes.target().frequency > 4.08 && notes.target().frequency < 4.09,
            "lowest input maps to sub-audible 4.0879 Hz without clamping");
    notes.reset();
    require(notes.noteOn(0, 60, 127) == Change::none && notes.noteOn(17, 60, 127) == Change::none
            && notes.noteOn(1, -1, 127) == Change::none && notes.noteOn(1, 128, 127) == Change::none,
            "invalid inputs rejected");
    // Fill every possible channel/key, then release in press order. Bass storage
    // must not inherit the rhythm bank's eight-note stealing limit.
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note) notes.noteOn(channel, note, 64);
    notes.noteOn(16, 127, 500); // Full-capacity duplicate must not append.
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note)
        {
            require(notes.target().channel == channel && notes.target().note == note,
                    "full-capacity deterministic handover");
            notes.noteOff(channel, note);
        }
    require(notes.target().note == -1, "full-capacity queue drains");

    using Policy = dropforge::BassNotes::Policy;
    require(notes.setPolicy(Policy::lowest) == Change::none, "empty policy change stays silent");
    require(notes.noteOn(1, 57, 90) == Change::attack, "Both single A3 attacks A2");
    require(notes.target().frequency == 110.0, "Both A3 exactly one octave down");
    require(notes.noteOn(1, 60, 40) == Change::none, "Both higher note preserves attack");
    require(notes.noteOn(2, 52, 70) == Change::attack && notes.target().note == 52,
            "Both E3 A3 C4 selects E2");
    require(notes.noteOff(1, 60) == Change::none, "higher release preserves attack");
    require(notes.noteOff(2, 52) == Change::attack && notes.target().note == 57,
            "lowest release retriggers revealed higher root");
    require(notes.noteOn(3, 57, 60) == Change::none && notes.target().channel == 1,
            "equal-pitch tie keeps oldest key");
    require(notes.noteOff(1, 57) == Change::retarget && notes.target().channel == 3
            && notes.target().velocity == 60, "same-pitch channel handover updates owner without attack");
    require(notes.noteOn(3, 57, 100) == Change::attack, "Both repeated selected key articulates");
    notes.noteOn(1, 64, 20);
    notes.noteOn(1, 40, 80);
    require(notes.allNotesOff(1) == Change::attack && notes.target().note == 57,
            "Both channel panic removes every key before root selection");
    require(notes.noteOn(3, 57, 0) == Change::release, "Both velocity zero releases last root");
    notes.noteOn(1, 70, 100);
    notes.noteOn(2, 50, 90);
    require(notes.setPolicy(Policy::locked) == Change::none && notes.target().note == 50,
            "entering Bass locks currently selected held root");
    require(notes.noteOn(3, 30, 80) == Change::none, "new lower key cannot interrupt Bass lock");
    require(notes.noteOff(2, 50) == Change::attack && notes.target().note == 70,
            "Bass handover still uses oldest key after policy switch");
    require(notes.setPolicy(Policy::lowest) == Change::attack && notes.target().note == 30,
            "entering Both recomputes lowest held pitch");
    require(notes.setPolicy(Policy::lowest) == Change::none, "same policy does not retrigger");
    notes.reset();
    notes.noteOn(1, 70, 100);
    notes.noteOn(2, 50, 90);
    require(notes.target().note == 50, "reset clears held notes and preserves chosen policy");
    notes.reset();
    // All channel/key pairs, descending pitches: each new lower root attacks,
    // equal pitches retain the oldest channel. Drain ascending to expose every
    // same-pitch handover and every upward root change, including MIDI zero.
    for (int note = 127; note >= 0; --note)
        for (int channel = 1; channel <= 16; ++channel)
        {
            const auto change = notes.noteOn(channel, note, 64);
            require(change == (channel == 1 ? Change::attack : Change::none),
                    "full lowest-policy ledger attacks only on pitch change");
            require(notes.target().note == note && notes.target().channel == 1,
                    "lowest policy ignores press order except equal-pitch ties");
            const auto expected = 220.0 * std::exp2((note - 69.0) / 12.0);
            require(std::abs(notes.target().frequency / expected - 1.0) < 1.0e-12,
                    "lowest policy preserves all octave-down frequencies");
        }
    require(notes.noteOn(16, 127, 500) == Change::none, "full lowest ledger duplicate stays bounded");
    for (int note = 0; note < 128; ++note)
        for (int channel = 1; channel <= 16; ++channel)
        {
            require(notes.target().note == note && notes.target().channel == channel,
                    "all 2048 lowest-policy roots and ties hand over deterministically");
            const auto expected = channel != 16 ? Change::retarget
                : (note != 127 ? Change::attack : Change::release);
            require(notes.noteOff(channel, note) == expected, "full lowest ledger change classification");
        }
    require(notes.target().note == -1, "lowest ledger drains completely");
    require(notes.noteOn(0, 60, 127) == Change::none && notes.noteOff(17, 0) == Change::none,
            "lowest policy rejects invalid keys");
    notes.setPolicy(Policy::locked);
    notes.noteOn(1, 60, 90); notes.noteOn(2, 60, 50);
    require(notes.noteOff(1, 60) == Change::attack, "Bass same-pitch channel handover keeps fresh attack");

    dropforge::Engine engine;
    engine.prepare(48000.0);
    auto midi = [&](std::uint8_t status, std::uint8_t a, std::uint8_t b)
    {
        const std::uint8_t bytes[]{status, a, b};
        dropforge::dispatchMidi(engine, bytes, 3);
    };
    midi(0x90, 52, 127);
    for (std::uint8_t note = 60; note < 80; ++note) midi(0x90, note, 64);
    require(engine.bassTarget().note == 52, "rhythm steals do not steal bass lock");
    midi(0xb0, 64, 127);
    midi(0x80, 52, 0);
    require(engine.bassTarget().note == 60, "sustain ignored by real MIDI path");
    midi(0xb0, 123, 0);
    require(engine.bassTarget().note == -1, "all notes off clears real MIDI path");
    midi(0x91, 0, 127);
    midi(0x91, 0, 0);
    require(engine.bassTarget().note == -1, "raw velocity-zero note-on releases");
    midi(0x91, 57, 127);
    midi(0xb1, 120, 0);
    require(engine.bassTarget().note == -1, "all sound off clears selection");
    midi(0x90, 69, 127);
    require(engine.bassTarget().frequency == 220.0, "engine routes A4 to A3");
    midi(0xff, 0, 0);
    require(engine.bassTarget().note == -1, "system reset clears selection");
    engine.noteOn(16, 127, 127);
    engine.reset();
    require(engine.bassTarget().note == -1, "engine reset clears selection");
    std::cout << "Bass lock, handover, 2048 pitches/held keys and MIDI resets passed\n";
}
