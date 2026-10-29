#include "dsp/NoteRouter.h"
#include "plugin/MidiDispatch.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
using Router = dropforge::NoteRouter;
using Change = Router::Change;
using Rule = Router::BassRule;

void newestRule()
{
    Router newest;
    newest.setBassRule(Rule::newest);
    newest.noteOn(1, 60, 70); newest.noteOn(2, 48, 110); newest.noteOn(1, 72, 90);
    require(newest.bassKey().note == 72, "newest key wins in either pitch direction");
    newest.noteOff(2, 48);
    require(newest.bassKey().note == 72, "non-selected release leaves newest note unchanged");
    require(newest.noteOff(1, 72).bass == Change::attack && newest.bassKey().note == 60,
            "release falls back to newest remaining held key with an attack");
    newest.noteOn(2, 60, 110);
    require(newest.bassKey().channel == 2, "newest same-pitch channel owns velocity and bend");
    newest.noteOn(1, 72, 90);
    require(newest.noteOn(1, 60, 127).bass == Change::attack && newest.bassKey().note == 60
            && newest.bassKey().channel == 1, "repeat press moves existing key to newest without stacking");
    newest.noteOff(1, 60);
    require(newest.bassKey().note == 72, "one note-off clears repeated key");
    newest.allNotesOff(1);
    require(newest.bassKey().note == 60 && newest.bassKey().channel == 2, "panic removes only its channel before fallback");
    require(newest.noteOn(2, 60, 0).bass == Change::release && !newest.bassKey().held(), "velocity-zero newest note releases");
    for (int channel = 1; channel <= 16; ++channel) for (int note = 0; note < 128; ++note)
        newest.noteOn(channel, note, 100);
    require(newest.heldCount() == 16 * 128, "every channel and key fits the ledger");
    require(newest.bassKey().channel == 16 && newest.bassKey().note == 127, "newest full ledger stays bounded");
    newest.noteOn(1, 0, 127);
    require(newest.heldCount() == 16 * 128 && newest.bassKey().channel == 1 && newest.bassKey().note == 0,
            "full-ledger repeated oldest becomes newest without growing");
    newest.noteOff(1, 0);
    require(newest.bassKey().channel == 16 && newest.bassKey().note == 127, "full-ledger repeat retains deterministic fallback");
    newest.reset();
    newest.noteOn(1, 60, 90); newest.noteOn(2, 60, 50);
    require(newest.noteOff(2, 60).bass == Change::attack, "newest same-pitch channel handover attacks: another key takes over");
}

void lowestRule()
{
    Router notes;
    require(!notes.bassKey().held() && !notes.leadKey().held(), "empty selection");
    require(notes.setBassRule(Rule::lowest) == Change::none, "empty rule change stays silent");
    require(notes.noteOn(1, 57, 90).bass == Change::attack, "single A3 attacks");
    require(Router::bassFrequency(notes.bassKey().note) == 110.0, "A3 sounds exactly one octave down");
    require(notes.noteOn(1, 60, 40).bass == Change::none, "higher note preserves attack");
    require(notes.noteOn(2, 52, 70).bass == Change::attack && notes.bassKey().note == 52, "E3 A3 C4 selects E3");
    require(notes.noteOff(1, 60).bass == Change::none, "higher release preserves attack");
    require(notes.noteOff(2, 52).bass == Change::attack && notes.bassKey().note == 57,
            "lowest release retriggers the revealed higher root");
    require(notes.noteOn(3, 57, 60).bass == Change::none && notes.bassKey().channel == 1, "equal-pitch tie keeps the earlier press");
    require(notes.noteOff(1, 57).bass == Change::retarget && notes.bassKey().channel == 3
            && notes.bassKey().velocity == 60, "same-pitch channel handover moves the owner without attack");
    require(notes.noteOn(3, 57, 100).bass == Change::attack, "repeated selected key articulates");
    require(notes.noteOn(3, 70, 100).bass == Change::none, "background press does not articulate");
    notes.noteOn(1, 64, 20);
    notes.noteOn(1, 40, 80);
    require(notes.allNotesOff(1).bass == Change::attack && notes.bassKey().note == 57,
            "channel panic removes every key before root selection");
    notes.noteOff(3, 70);
    require(notes.noteOn(3, 57, 0).bass == Change::release, "velocity zero releases last root");
    notes.noteOn(1, 70, 100);
    notes.noteOn(2, 50, 90);
    require(notes.setBassRule(Rule::newest) == Change::none && notes.bassKey().note == 50,
            "switching to newest keeps a root that is also newest");
    notes.noteOn(3, 80, 80);
    require(notes.setBassRule(Rule::lowest) == Change::attack && notes.bassKey().note == 50,
            "switching back to lowest recomputes the lowest held pitch");
    require(notes.setBassRule(Rule::lowest) == Change::none, "same rule does not retrigger");
    notes.reset();
    require(!notes.bassKey().held(), "reset clears every key");
    // All channel/key pairs, descending pitches: each new lower root attacks,
    // equal pitches retain the earliest channel. Drain ascending to expose every
    // same-pitch handover and every upward root change, including MIDI zero.
    for (int note = 127; note >= 0; --note)
        for (int channel = 1; channel <= 16; ++channel)
        {
            require(notes.noteOn(channel, note, 64).bass == (channel == 1 ? Change::attack : Change::none),
                    "full lowest ledger attacks only on pitch change");
            require(notes.bassKey().note == note && notes.bassKey().channel == 1,
                    "lowest rule ignores press order except equal-pitch ties");
            const auto expected = 220.0 * std::exp2((note - 69.0) / 12.0);
            require(std::abs(Router::bassFrequency(note) / expected - 1.0) < 1.0e-12,
                    "every note sounds exactly one octave down, below MIDI zero too");
        }
    require(Router::bassFrequency(0) > 4.08 && Router::bassFrequency(0) < 4.09, "MIDI 0 maps to 4.09 Hz without clamping");
    for (int note = 0; note < 128; ++note)
        for (int channel = 1; channel <= 16; ++channel)
        {
            require(notes.bassKey().note == note && notes.bassKey().channel == channel,
                    "all 2048 lowest roots and ties hand over deterministically");
            const auto expected = channel != 16 ? Change::retarget : (note != 127 ? Change::attack : Change::release);
            require(notes.noteOff(channel, note).bass == expected, "full lowest ledger change classification");
        }
    require(!notes.bassKey().held(), "lowest ledger drains completely");
    require(notes.noteOn(0, 60, 127).bass == Change::none && notes.noteOn(17, 60, 127).bass == Change::none
            && notes.noteOn(1, -1, 127).bass == Change::none && notes.noteOn(1, 128, 127).bass == Change::none
            && notes.noteOff(17, 0).bass == Change::none, "invalid keys rejected");
}

void leadSlot()
{
    // Blade: the highest held key, whatever Anvil's rule.
    for (const auto rule : {Rule::lowest, Rule::newest})
    {
        Router notes;
        notes.setBassRule(rule);
        require(notes.noteOn(1, 40, 100).lead == Change::attack && notes.leadKey().note == 40, "single key is also the lead");
        require(notes.noteOn(1, 47, 100).lead == Change::attack && notes.leadKey().note == 47, "higher key takes the lead");
        require(notes.noteOn(1, 52, 100).lead == Change::attack, "E2 B2 E3: lead E3");
        require(notes.bassKey().note == (rule == Rule::lowest ? 40 : 52) && notes.leadKey().note == 52,
                "Anvil and Blade select independently");
        require(notes.noteOn(1, 45, 100).lead == Change::none, "inner key leaves the lead alone");
        require(notes.noteOff(1, 45).lead == Change::none, "inner release leaves the lead alone");
        require(notes.noteOn(2, 52, 60).lead == Change::none && notes.leadKey().channel == 1, "equal-pitch tie keeps the earlier press");
        require(notes.noteOff(1, 52).lead == Change::retarget && notes.leadKey().channel == 2,
                "same-pitch handover moves the lead owner without attack");
        require(notes.noteOff(2, 52).lead == Change::attack && notes.leadKey().note == 47, "lead release falls back to next highest");
        require(notes.noteOn(1, 47, 90).lead == Change::attack, "repeated lead key articulates");
        notes.allNotesOff(1);
        require(!notes.leadKey().held(), "panic clears the lead");
    }
    Router notes;
    notes.noteOn(1, 127, 100);
    notes.noteOn(1, 0, 100);
    require(notes.leadKey().note == 127 && notes.bassKey().note == 0, "full range: lead 127, Anvil 0");
}

struct TestVoice
{
    int channel = 0, note = -1;
    bool keyDown = false, sounding = false;
    std::uint64_t age = 0;
    bool active() const noexcept { return sounding; }
};

void voiceStealing()
{
    std::vector<TestVoice> voices(8);
    std::uint64_t serial = 0;
    const auto press = [&](int channel, int note) {
        const auto index = Router::pickVoice(voices, channel, note);
        voices[index] = {channel, note, true, true, ++serial};
        return index;
    };
    for (int i = 0; i < 8; ++i) require(press(1, 40 + i) == static_cast<std::size_t>(i), "free voices fill in order");
    require(press(1, 43) == 3, "a sounding key retriggers its own voice");
    require(press(2, 43) == 0, "same note on another channel is another key: oldest held voice is stolen");
    voices[5].keyDown = false; // released, still in its tail
    require(press(1, 60) == 5, "a released voice is taken before any held one");
    voices[6].sounding = false;
    require(press(1, 61) == 6, "a silent voice is taken first");
    require(press(1, 62) == 1, "otherwise the oldest held voice");
}

void midiPath()
{
    dropforge::Engine engine;
    engine.prepare(48000.0);
    auto midi = [&](std::uint8_t status, std::uint8_t a, std::uint8_t b)
    {
        const std::uint8_t bytes[]{status, a, b};
        dropforge::dispatchMidi(engine, bytes, 3);
    };
    midi(0x90, 52, 127);
    for (std::uint8_t note = 60; note < 80; ++note) midi(0x90, note, 64);
    require(engine.bassTarget().note == 52, "Furnace steals do not steal Anvil's root");
    require(engine.leadTarget().note == 79, "Blade follows the highest of all held keys, beyond Furnace's capacity");
    midi(0xb0, 64, 127);
    midi(0x80, 52, 0);
    require(engine.bassTarget().note == 60, "sustain ignored by the real MIDI path");
    midi(0xb0, 123, 0);
    require(engine.bassTarget().note == -1 && engine.leadTarget().note == -1, "all notes off clears the real MIDI path");
    midi(0x91, 0, 127);
    midi(0x91, 0, 0);
    require(engine.bassTarget().note == -1, "raw velocity-zero note-on releases");
    midi(0x91, 57, 127);
    midi(0xb1, 120, 0);
    require(engine.bassTarget().note == -1, "all sound off clears selection");
    midi(0x90, 69, 127);
    require(engine.bassTarget().frequency == 220.0 && engine.leadTarget().frequency == 440.0, "A4: Anvil A3, Blade A4");
    midi(0xff, 0, 0);
    require(engine.bassTarget().note == -1, "system reset clears selection");
    engine.noteOn(16, 127, 127);
    engine.reset();
    require(engine.bassTarget().note == -1 && engine.leadTarget().note == -1, "engine reset clears selection");
}
}

int main()
{
    newestRule();
    lowestRule();
    leadSlot();
    voiceStealing();
    midiPath();
    std::cout << "Anvil newest/lowest, Blade highest, Furnace stealing, 2048 keys and MIDI resets passed\n";
}
