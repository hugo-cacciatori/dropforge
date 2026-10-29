#include "dsp/Engine.h"
#include "plugin/MidiDispatch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <vector>

namespace
{
bool forbidAllocation = false;
}
void* operator new(std::size_t size)
{
    if (forbidAllocation) { std::cerr << "FAIL: allocation in live routing\n"; std::abort(); }
    if (auto* memory = std::malloc(std::max(size, std::size_t{1}))) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using dropforge::Engine;
using dropforge::LayerMask;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
// Use the direct MIDI-minus-12 conversion for bit-exact audio oracles. Moving
// the factor of two outside exp2 is mathematically equal, but can round its
// fractional exponent differently and leave a sub-ULP filter-history residue.
double bassHz(int note) { return 440.0 * std::exp2((note - 81.0) / 12.0); }
std::set<int> heldRhythm(const Engine& engine)
{
    std::set<int> notes;
    for (const auto& voice : engine.rhythmTargets()) if (voice.held) notes.insert(voice.note);
    return notes;
}
struct Frame { float left, right, bass, rhythmLeft, rhythmRight; };
Frame next(Engine& engine, LayerMask mask = LayerMask::both)
{
    Frame frame{}; float bassRight = 0.0f;
    engine.process(&frame.left, &frame.right, 1, mask,
                   {&frame.bass, &bassRight, &frame.rhythmLeft, &frame.rhythmRight});
    require(frame.bass == bassRight, "bass taps exactly mono");
    require(std::isfinite(frame.left) && std::isfinite(frame.right), "finite routed output");
    require(std::abs(frame.left) < 1.0f && std::abs(frame.right) < 1.0f, "default routed headroom");
    return frame;
}
void advance(Engine& engine, int count) { while (count-- > 0) next(engine); }

void layerLevels()
{
    for (const double rate : {44100.0, 48000.0, 192000.0})
    {
        Engine actual, reference;
        for (auto* engine : {&actual, &reference})
        {
            engine->setLayersEnabled(true, true); engine->prepare(rate);
            engine->noteOn(1, 52, 127); advance(*engine, 2048);
        }
        forbidAllocation = true;
        actual.setLayerLevels(-6.0f, -60.0f);
        const auto fade = static_cast<int>(std::ceil(rate * 0.005));
        for (int i = 0; i < fade + 128; ++i)
        {
            const auto a = next(actual), b = next(reference);
            const auto phase = std::min(1.0, static_cast<double>(i) / fade);
            require(std::abs(a.bass - b.bass * (1.0 + (std::pow(10.0, -6.0 / 20.0) - 1.0) * phase)) < 3e-8,
                "Anvil level follows the prepared 5 ms ramp and stays exactly mono");
            require(std::abs(a.rhythmLeft - b.rhythmLeft * (1.0 - phase)) < 3e-8,
                "Furnace mute endpoint follows its independent level ramp");
            if (i >= fade) require(a.rhythmLeft == 0 && a.rhythmRight == 0, "minus infinity level reaches exact silence");
            require(a.left == a.bass + a.rhythmLeft && a.right == a.bass + a.rhythmRight, "level taps reconstruct output");
        }
        actual.setLayerLevels(0, 0);
        for (int i = 0; i < fade + 128; ++i)
        {
            const auto a = next(actual), b = next(reference);
            if (i >= fade) require(a.left == b.left && a.right == b.right, "unity level restores original samples without synthesis reset");
        }
        forbidAllocation = false;
    }
}

void modeExamples()
{
    for (const bool bass : {false, true}) for (const bool rhythm : {false, true})
    {
        Engine engine;
        engine.setLayersEnabled(bass, rhythm);
        engine.prepare(48000.0);
        engine.noteOn(1, 57, 127); // MIDI A3 -> bass A2, rhythm A3.
        require(engine.bassTarget().note == (bass ? 57 : -1), "single-note mode target");
        if (bass) require(engine.bassTarget().frequency == 110.0, "A3 maps to A2 exactly");
        require(heldRhythm(engine) == (rhythm ? std::set<int>{57} : std::set<int>{}), "exact single rhythm pitch");
        engine.noteOn(1, 60, 90); engine.noteOn(2, 52, 100);
        if (bass) require(engine.bassTarget().note == 52, "Both lowest and solo newest select the newly pressed low key");
        require(heldRhythm(engine) == (rhythm ? std::set<int>{52, 57, 60} : std::set<int>{}), "no automatic rhythm chord stacking");
        for (int i = 0; i < 1024; ++i)
        {
            const auto frame = next(engine);
            require(bass || frame.bass == 0.0f, "inactive bass silent");
            require(rhythm || (frame.rhythmLeft == 0.0f && frame.rhythmRight == 0.0f), "inactive rhythm silent");
        }
        engine.noteOff(2, 52);
        if (bass) require(engine.bassTarget().note == (rhythm ? 57 : 60), "release reveals lowest or newest remaining key");
        engine.noteOff(1, 57);
        if (bass) require(engine.bassTarget().note == 60, "last held key becomes root");
        engine.allNotesOff(1);
        advance(engine, 1024);
        const auto frame = next(engine);
        require(frame.left == 0.0f && frame.right == 0.0f, "all modes release to exact silence");
    }
}

void attackOracle(double rate)
{
    Engine engine; engine.setLayersEnabled(true, true); engine.prepare(rate);
    dropforge::BassVoice expected; expected.prepare(rate);
    auto attack = [&](int channel, int note, int velocity)
    {
        engine.noteOn(channel, note, velocity); expected.start(bassHz(note), velocity);
    };
    attack(1, 57, 90);
    for (int sample = 0; sample < 6000; ++sample)
    {
        if (sample == 700) engine.noteOn(1, 64, 50); // Higher: no bass attack.
        if (sample == 1000) attack(2, 52, 100);
        if (sample == 1500) engine.noteOff(1, 64); // Higher release: no attack.
        if (sample == 2000) { engine.noteOff(2, 52); expected.start(bassHz(57), 90); }
        if (sample == 2500) engine.noteOn(3, 57, 90); // Same pitch: no attack.
        if (sample == 3000) engine.noteOff(1, 57); // Same-pitch owner: no attack.
        if (sample == 3500) attack(3, 57, 70); // Explicit repeated key articulates.
        if (sample == 4000) { engine.noteOn(3, 57, 0); expected.stop(); }
        if (sample == 4500) attack(1, 0, 127);
        const auto frame = next(engine, LayerMask::bass);
        const auto oracle = static_cast<float>(expected.next(1.0).output);
        if (frame.left != oracle)
            std::cerr << "attack_oracle rate=" << rate << " sample=" << sample
                      << " audio_error=" << static_cast<double>(frame.left) - oracle
                      << " frequency_error=" << engine.bassTarget().frequency - bassHz(engine.bassTarget().note) << '\n';
        require(frame.left == oracle,
                "Both audio matches independent manually gated bass oracle sample-for-sample");
    }
}

double fadeTests(double rate)
{
    const auto length = static_cast<int>(std::ceil(rate * 0.005));
    double worstControlStep = 0.0;
    for (const bool disableBass : {false, true})
    {
        Engine faded, reference;
        for (auto* engine : {&faded, &reference})
        {
            engine->setLayersEnabled(true, true); engine->prepare(rate);
            engine->noteOn(1, 57, 127); advance(*engine, 2048);
        }
        faded.setLayersEnabled(!disableBass, disableBass);
        for (int i = 0; i < length + 20; ++i)
        {
            if (i == length / 2) faded.setLayersEnabled(!disableBass, disableBass); // No fade restart.
            const auto a = next(faded), b = next(reference);
            const auto gain = std::max(0.0, 1.0 - static_cast<double>(i) / length);
            const auto actual = disableBass ? a.bass : a.rhythmLeft;
            const auto baseline = disableBass ? b.bass : b.rhythmLeft;
            require(std::abs(actual - baseline * gain) < 3.0e-8, "disable follows exact 5 ms linear gain");
            if (i >= length) require(actual == 0.0f, "disabled layer is exactly silent at fade endpoint");
            const auto previousGain = std::max(0.0, 1.0 - std::max(0, i - 1) / static_cast<double>(length));
            worstControlStep = std::max(worstControlStep, std::abs(baseline * (gain - previousGain)));
            if (disableBass)
                require(a.rhythmLeft == b.rhythmLeft && a.rhythmRight == b.rhythmRight, "bass enable cannot alter rhythm");
            else require(a.bass == b.bass, "rhythm disable preserves the current bass root");
        }
    }
    // Reverse a fade before zero. No reset or gain jump at the reversal.
    Engine faded, reference;
    for (auto* engine : {&faded, &reference})
    {
        engine->setLayersEnabled(true, true); engine->prepare(rate);
        engine->noteOn(1, 57, 127); advance(*engine, 2048);
    }
    faded.setLayersEnabled(false, true);
    const int part = length / 3;
    for (int i = 0; i < part; ++i) { next(faded); next(reference); }
    faded.setLayersEnabled(true, true);
    // Re-entry intentionally attacks, so use a directly gated bass reference
    // with the same live history rather than expecting the old sustain phase.
    reference.noteOn(1, 57, 127);
    const auto startGain = 1.0 - static_cast<double>(part) / length;
    for (int i = 0; i < length + 20; ++i)
    {
        const auto a = next(faded), b = next(reference);
        const auto gain = std::min(1.0, startGain + (1.0 - startGain) * i / length);
        require(std::abs(a.bass - b.bass * gain) < 3.0e-8, "rapid re-entry preserves history and reverses gain smoothly");
    }
    return worstControlStep;
}

void heldReentry()
{
    Engine engine; engine.setLayersEnabled(false, false); engine.prepare(48000.0);
    for (int note = 40; note < 52; ++note) engine.noteOn(1, note, 90);
    engine.noteOff(1, 50); engine.noteOn(1, 43, 100); // Repeat preserves queue age.
    engine.setLayersEnabled(true, true);
    require(engine.bassTarget().note == 40, "Both re-entry recovers lowest beyond rhythm capacity");
    require(heldRhythm(engine) == std::set<int>({43, 44, 45, 46, 47, 48, 49, 51}), "rhythm re-entry keeps newest eight held keys");
    advance(engine, 1024);
    engine.setLayersEnabled(true, false);
    engine.noteOn(2, 30, 127);
    require(engine.bassTarget().note == 30, "solo mode follows a new key immediately");
    engine.noteOff(1, 40);
    require(engine.bassTarget().note == 30, "non-selected release leaves newest root alone");
    engine.setLayersEnabled(true, true);
    require(engine.bassTarget().note == 30, "returning to Both recomputes lower held root");
    engine.setLayersEnabled(false, false); advance(engine, 1024);
    require(heldRhythm(engine).empty(), "fully disabled layer clears active rhythm voices");
    for (const auto& voice : engine.rhythmTargets()) require(!voice.active, "disabled rhythm envelopes/history clear");
    engine.allNotesOff(1); engine.allNotesOff(2);
    engine.setLayersEnabled(true, true); advance(engine, 1024);
    require(next(engine).left == 0.0f && heldRhythm(engine).empty(), "off-mode panic cannot resurrect held notes");
    engine.noteOn(1, 57, 90); engine.setLayersEnabled(false, false); engine.reset();
    engine.setLayersEnabled(true, true); advance(engine, 1024);
    require(next(engine).left == 0.0f && engine.bassTarget().note == -1, "reset clears off-mode ledger");
    engine.setLayersEnabled(false, false); advance(engine, 1024);
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note) engine.noteOn(channel, note, 64);
    engine.setLayersEnabled(false, true);
    require(heldRhythm(engine) == std::set<int>({120, 121, 122, 123, 124, 125, 126, 127}),
            "full 2048-key ledger restores newest eight without replaying stolen keys");
    for (const auto& voice : engine.rhythmTargets()) require(voice.channel == 16 && voice.held, "full-ledger rhythm ownership");
    advance(engine, 1024);
}

void ownership()
{
    Engine actual, reference;
    for (auto* engine : {&actual, &reference})
    {
        engine->setLayersEnabled(true, true); engine->prepare(48000.0);
        engine->noteOn(1, 57, 127); engine->noteOn(2, 57, 32); advance(*engine, 2000);
    }
    actual.noteOff(1, 57);
    // Independent voice oracle: unchanged waveform/envelope, only smoothed velocity.
    dropforge::BassVoice oracle; oracle.prepare(48000.0); oracle.start(110.0, 127);
    for (int i = 0; i < 2000; ++i) oracle.next(1.0);
    oracle.retargetVelocity(32);
    for (int i = 0; i < 1000; ++i)
        require(next(actual, LayerMask::bass).left == static_cast<float>(oracle.next(1.0).output),
                "same-pitch owner velocity smooths without phase/envelope restart");
    require(actual.bassTarget().channel == 2 && actual.bassTarget().velocity == 32, "owner channel and velocity updated");
    actual.pitchBend(2, 16383); advance(actual, 2000);
    // Channel messages use the actual raw decoder, including ignored sustain.
    const std::uint8_t sustain[]{0xb1, 64, 127}, off[]{0x81, 57, 0};
    dropforge::dispatchMidi(actual, sustain, 3); dropforge::dispatchMidi(actual, off, 3);
    require(actual.bassTarget().note == -1, "sustain cannot hold Both root");
    advance(actual, 1024); require(next(actual).left == 0.0f, "bent owner releases without stuck note");

    Engine bent; bent.setLayersEnabled(true, true); bent.prepare(48000.0);
    bent.pitchBend(1, 0); bent.pitchBend(2, 16383); advance(bent, 256);
    bent.noteOn(1, 57, 90); bent.noteOn(2, 57, 90);
    dropforge::BassVoice bentOracle; bentOracle.prepare(48000.0); bentOracle.start(110.0, 90);
    const auto low = std::exp2(-2.0 / 12.0), high = std::exp2(2.0 / 12.0);
    for (int i = 0; i < 2000; ++i)
        require(next(bent, LayerMask::bass).left == static_cast<float>(bentOracle.next(low).output), "initial owner bend matches oracle");
    bent.noteOff(1, 57);
    dsp_core::LinearRamp offset; offset.reset(low - high); offset.setTarget(0.0, 96);
    for (int i = 0; i < 1000; ++i)
        require(next(bent, LayerMask::bass).left == static_cast<float>(bentOracle.next(high + offset.next()).output),
                "same-pitch owner bend crosses smoothly in 2 ms without restarting phase");
}

void freshReentry(double rate)
{
    Engine resumed; resumed.setLayersEnabled(true, true); resumed.prepare(rate);
    resumed.noteOn(1, 69, 127); advance(resumed, 2048);
    resumed.setLayersEnabled(false, false); advance(resumed, static_cast<int>(std::ceil(rate * 0.005)) + 16);
    resumed.noteOff(1, 69);
    for (const auto note : {52, 57, 60}) resumed.noteOn(2, note, 90);
    advance(resumed, 64);
    require(next(resumed).left == 0.0f, "held keys cannot leak from suspended layers");
    resumed.setLayersEnabled(true, true);
    Engine fresh; fresh.setLayersEnabled(true, true); fresh.prepare(rate);
    for (const auto note : {52, 57, 60}) fresh.noteOn(2, note, 90);
    const auto length = static_cast<int>(std::ceil(rate * 0.005));
    for (int i = 0; i < length + 2000; ++i)
    {
        if (i == length / 2) resumed.setLayersEnabled(true, true);
        const auto a = next(resumed), b = next(fresh);
        const auto gain = std::min(1.0, static_cast<double>(i) / length);
        require(std::abs(a.bass - b.bass * gain) < 3.0e-8
                && std::abs(a.rhythmLeft - b.rhythmLeft * gain) < 3.0e-8
                && std::abs(a.rhythmRight - b.rhythmRight * gain) < 3.0e-8,
                "re-entry is a fresh held chord with measured fade, no old histories or repeated-control attack");
    }
}

void allocationCheck()
{
    Engine engine; engine.setLayersEnabled(false, false); engine.prepare(48000.0);
    std::array<float, 512> left{}, right{};
    forbidAllocation = true;
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note) engine.noteOn(channel, note, 64);
    for (const auto bass : {false, true}) for (const auto rhythm : {false, true})
    {
        engine.setLayersEnabled(bass, rhythm);
        engine.process(left.data(), right.data(), 512);
        engine.pitchBend(16, 16383);
    }
    for (int channel = 1; channel <= 16; ++channel) engine.allNotesOff(channel);
    engine.setLayersEnabled(false, false); engine.process(left.data(), right.data(), 512);
    forbidAllocation = false;
}

void event(Engine& engine, int index)
{
    switch (index)
    {
        case 0: for (int note = 48; note < 60; ++note) engine.noteOn(1, note, 127); break;
        case 1: engine.setLayersEnabled(false, true); break;
        case 2: engine.setLayersEnabled(true, false); engine.noteOn(2, 30, 70); break;
        case 3: engine.setLayersEnabled(false, false); break;
        case 4: engine.noteOff(2, 30); engine.allNotesOff(1); engine.noteOn(3, 57, 90); break;
        case 5: engine.setLayersEnabled(true, true); break;
        case 6: engine.noteOn(4, 40, 127); engine.pitchBend(4, 0); break;
        case 7: engine.allNotesOff(4); engine.setLayersEnabled(false, true); break;
        case 8: engine.setLayersEnabled(true, true); break;
        case 9: engine.setLayersEnabled(false, false); break;
        case 10: engine.noteOff(3, 57); engine.setLayersEnabled(true, true); break;
        default: break;
    }
}
std::array<std::vector<float>, 2> sequence(double rate, int block, LayerMask mask = LayerMask::both)
{
    Engine engine; engine.setLayersEnabled(true, true); engine.prepare(rate);
    std::array<std::vector<float>, 2> audio{std::vector<float>(8000), std::vector<float>(8000)};
    const std::array<int, 12> times{0, 600, 1200, 1800, 2400, 3000, 3600, 4200, 4217, 4231, 5400, 8000};
    for (int index = 0; index < 11; ++index)
    {
        event(engine, index);
        engine.process(nullptr, nullptr, 0);
        int position = times[static_cast<std::size_t>(index)];
        while (position < times[static_cast<std::size_t>(index + 1)])
        {
            const auto count = std::min(block, times[static_cast<std::size_t>(index + 1)] - position);
            engine.process(audio[0].data() + position, audio[1].data() + position, count, mask);
            position += count;
        }
    }
    for (std::size_t i = 0; i < audio[0].size(); ++i)
    {
        require(std::isfinite(audio[0][i]) && std::isfinite(audio[1][i]), "mode/block matrix stays finite");
        require(std::abs(audio[0][i]) < 1.0f && std::abs(audio[1][i]) < 1.0f, "mode/block matrix headroom");
        if (i >= 7000) require(audio[0][i] == 0.0f && audio[1][i] == 0.0f, "rapid transitions leave no stuck notes");
    }
    return audio;
}
}

int main()
{
    layerLevels();
    modeExamples(); heldReentry(); ownership(); allocationCheck();
    double worst = 0.0;
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        attackOracle(rate);
        freshReentry(rate);
        worst = std::max(worst, fadeTests(rate));
        const auto reference = sequence(rate, 1);
        for (const auto block : {17, 73, 512})
            require(sequence(rate, block) == reference, "routing is sample-identical across block sizes");
        const auto bass = sequence(rate, 73, LayerMask::bass);
        const auto rhythm = sequence(rate, 73, LayerMask::rhythm);
        const auto silent = sequence(rate, 73, LayerMask::none);
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (std::size_t i = 0; i < reference[channel].size(); ++i)
            {
                require(reference[channel][i] == bass[channel][i] + rhythm[channel][i], "masks reconstruct mode-aware routing exactly");
                require(silent[channel][i] == 0.0f, "mask omission remains silent without changing mode state");
            }
        require(bass[0] == bass[1], "isolated routed bass exactly centered through mode changes");
    }
    std::cout << "Four modes, attack/ownership oracles, held re-entry, fades and 24 rate/block cases passed; "
              << "max fade control-only step=" << worst << '\n';
}
