#include "dsp/Engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
constexpr double pi = 3.14159265358979323846;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Audio
{
    std::vector<float> left, right;
    explicit Audio(int n) : left(static_cast<std::size_t>(n)), right(left.size()) {}
};
Audio render(dropforge::Engine& engine, int count, int block = 512)
{
    Audio audio(count);
    for (int at = 0; at < count; at += block)
        engine.process(audio.left.data() + at, audio.right.data() + at, std::min(block, count - at), dropforge::LayerMask::rhythm);
    return audio;
}
double magnitude(const std::vector<float>& samples, double frequency)
{
    std::complex<double> sum{};
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto window = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / (samples.size() - 1));
        sum += static_cast<double>(samples[i]) * window * std::polar(1.0, -2.0 * pi * frequency * static_cast<double>(i) / 48000.0);
    }
    return 4.0 * std::abs(sum) / static_cast<double>(samples.size());
}
void spectrum()
{
    dropforge::Engine engine;
    engine.prepare(48000);
    engine.noteOn(1, 69, 127);
    render(engine, 4800);
    const auto audio = render(engine, 192000);
    require(audio.left != audio.right, "rhythm is synthesized independently on both sides");
    for (const auto* side : {&audio.left, &audio.right})
    {
        require(magnitude(*side, 440.0) > 0.005, "exact input-pitch anchor is audible on both sides");
        // Inner supersaw saws: -5.3 and +5.4 cents left, +4.5 and -4.6 right.
        const auto inner = side == &audio.left ? std::array<double, 2>{-5.325, 5.43} : std::array<double, 2>{4.526, -4.616};
        require(magnitude(*side, 440.0 * std::exp2(inner[0] / 1200.0)) > 0.0005
                && magnitude(*side, 440.0 * std::exp2(inner[1] / 1200.0)) > 0.0005,
                "detuned supersaw sidebands, rather than extra chord notes, are present");
        require(magnitude(*side, 440.0 * std::exp2(7.0 / 12.0)) < 0.00001
                && magnitude(*side, 220.0) < 0.00001, "rhythm adds neither a fifth nor an octave-down note");
    }
    // The outer beating components must survive the real summed-bus drive,
    // rather than existing only in an isolated oscillator measurement.
    // Outermost saws: -30 cents left, +25.5 cents right (mirrored, narrower).
    require(magnitude(audio.left, 440.0 * std::exp2(-30.0 / 1200.0)) > 0.0001
            && magnitude(audio.right, 440.0 * std::exp2(25.5 / 1200.0)) > 0.0001,
            "outer supersaw saws survive the default drive on both sides");
    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (std::size_t i = 0; i < audio.left.size(); ++i)
    {
        ll += audio.left[i] * static_cast<double>(audio.left[i]);
        rr += audio.right[i] * static_cast<double>(audio.right[i]);
        lr += audio.left[i] * static_cast<double>(audio.right[i]);
    }
    std::cout << "rhythm_440_anchor_left=" << magnitude(audio.left, 440.0)
              << " right=" << magnitude(audio.right, 440.0) << " correlation=" << lr / std::sqrt(ll * rr)
              << " balance_db=" << 10.0 * std::log10(ll / rr) << '\n';
}
void octavePresence()
{
    double lowestAnchor = 1.0, lowestShoulder = 1.0;
    for (const auto rate : {44100, 48000, 192000})
    {
        dropforge::RhythmOscillators full, base;
        full.reset(1, 57); base.reset(1, 57);
        std::array<std::array<std::complex<double>, 7>, 2> bins{};
        std::array<std::array<double, 7>, 2> frequencies{};
        for (std::size_t side = 0; side < frequencies.size(); ++side)
        {
            frequencies[side] = {220, 330, 440};
            // The octave stack's four detuned saws: +-12 cents, mirrored and
            // 0.85x narrower on the right.
            constexpr std::array<double, 4> cents{-12.0, -5.4, 6.0, 11.4};
            for (std::size_t shoulder = 0; shoulder < cents.size(); ++shoulder)
                frequencies[side][shoulder + 3] = 440 * std::exp2(cents[shoulder]
                    * (side == 0 ? 1.0 : -0.85) / 1200.0);
        }
        for (int i = 0; i < rate*2; ++i)
        {
            if (i % 32 == 0) { full.setPitch(220.0, rate); base.setPitch(220.0, rate); }
            const auto a = full.next(), b = base.next(false);
            const std::array<double, 2> added{a.left-b.left, a.right-b.right};
            const auto window = 0.5 - 0.5*std::cos(2*pi*i/(rate*2));
            for (std::size_t side = 0; side < added.size(); ++side)
                for (std::size_t bin = 0; bin < bins[side].size(); ++bin)
                    bins[side][bin] += window*added[side]
                        * std::polar(1.0, -2*pi*frequencies[side][bin]*i/rate);
        }
        for (const auto& side : bins)
        {
            const auto anchor = std::abs(side[2])*2.0/rate;
            lowestAnchor = std::min(lowestAnchor, anchor);
            require(anchor > 0.01, "isolated presence layer has an exact octave-up anchor on each side");
            require(std::abs(side[0])*2.0/rate < 1e-6 && std::abs(side[1])*2.0/rate < 1e-6,
                    "added source supplies upper harmonics without a new root or fifth");
            for (std::size_t bin = 3; bin < side.size(); ++bin)
            {
                const auto shoulder = std::abs(side[bin])*2.0/rate;
                lowestShoulder = std::min(lowestShoulder, shoulder);
                require(shoulder > 0.008, "all four detuned octave saws are present on both sides");
            }
        }
        full.reset(1, 127); base.reset(1, 127);
        for (int i = 0; i < 4096; ++i)
        {
            if (i % 32 == 0) { full.setPitch(rate*0.24, rate); base.setPitch(rate*0.24, rate); }
            const auto a = full.next(), b = base.next(false);
            require(a.left == b.left && a.right == b.right,
                    "above-Nyquist octave components disappear instead of sounding a clamped pitch");
        }
    }
    std::cout << "rhythm_added_octave_min_anchor=" << lowestAnchor
              << " min_shoulder=" << lowestShoulder << " high_fade=exact\n";
}
void targets()
{
    dropforge::Engine engine;
    for (int note = 0; note < 128; ++note)
    {
        engine.prepare(48000);
        engine.noteOn(3, note, 127);
        int count = 0;
        for (const auto& target : engine.rhythmTargets())
            if (target.active)
            {
                ++count;
                require(target.note == note && target.channel == 3 && target.held
                        && std::abs(target.frequency - 440.0 * std::exp2((note - 69.0) / 12.0)) < 1.0e-9,
                        "every input MIDI pitch has exactly one unchanged-pitch rhythm note");
            }
        require(count == 1, "unison components do not consume playable note voices");
    }
    engine.prepare(48000);
    for (int note = 60; note < 68; ++note) engine.noteOn(1, note, 127);
    engine.noteOn(1, 80, 127);
    engine.noteOff(1, 60);
    auto current = engine.rhythmTargets();
    require(std::count_if(current.begin(), current.end(), [](const auto& t) { return t.active && t.held; }) == 8,
            "ninth note steals oldest, stale release preserves all eight replacement notes");
    require(std::none_of(current.begin(), current.end(), [](const auto& t) { return t.active && t.note == 60; }),
            "oldest held note was stolen");
    for (int note = 61; note < 68; ++note) engine.noteOff(1, note);
    engine.noteOff(1, 80);
    const auto released = render(engine, 2000);
    require(std::abs(released.left.back()) < 1.0e-8f && std::abs(released.right.back()) < 1.0e-8f,
            "both rhythm sides release including filter tails");
}
void bentAudio()
{
    dropforge::Engine original, otherChannel;
    // Drive 0 isolates pitch following: at the fixed drive a dense supersaw's
    // intermodulation skirt reaches two semitones away by design.
    for (auto* engine : {&original, &otherChannel})
    {
        engine->setRhythmParameters(0.0f); engine->prepare(48000); engine->noteOn(1, 69, 127);
    }
    otherChannel.pitchBend(2, 16383);
    const auto a = render(original, 4800), b = render(otherChannel, 4800, 73);
    require(a.left == b.left && a.right == b.right, "another channel's bend leaves both rhythm sides unchanged");
    otherChannel.pitchBend(1, 16383);
    render(otherChannel, 4800);
    const auto bent = render(otherChannel, 192000);
    const auto frequency = 440.0 * std::exp2(2.0 / 12.0);
    for (const auto* side : {&bent.left, &bent.right})
    {
        // Relative to the bent note: the wavetable alias floor (about -70 dB)
        // may leave a trace anywhere, but never the old pitch itself.
        const auto bentLevel = magnitude(*side, frequency), unbentLevel = magnitude(*side, 440.0);
        std::cout << "bent_magnitude=" << bentLevel << " unbent_db=" << 20.0 * std::log10(unbentLevel / bentLevel) << '\n';
        require(bentLevel > 0.005 && unbentLevel < 0.001 * bentLevel,
                "both output spectra actually follow the correct channel's bend");
    }
    for (const int note : {48, 72, 96})
    {
        original.prepare(48000); original.noteOn(1, note, 127);
        render(original, 4800);
        const auto audio = render(original, 192000);
        const auto hz = 440.0 * std::exp2((note - 69.0) / 12.0);
        for (const auto* side : {&audio.left, &audio.right})
            // The separation HP deliberately attenuates low fundamentals; the
            // exact pitch must still be present rather than a new chord note.
            require(magnitude(*side, hz) > (hz < 2.0 * dropforge::RhythmBus::separationHz ? 0.0001 : 0.005)
                    && magnitude(*side, hz * std::exp2(7.0 / 12.0)) < 0.00001,
                    "multiple played pitches produce their own spectral anchor without a fifth");
    }
}
void repeatability()
{
    dropforge::Engine a, b, reference;
    for (auto* engine : {&a, &b, &reference})
    {
        engine->setRhythmParameters(0.0f); engine->prepare(48000);
    }
    a.noteOn(1, 60, 127); a.noteOn(1, 69, 127); // 69 gets slot 1.
    b.noteOn(1, 69, 127); reference.noteOn(1, 60, 127); // Both get slot 0.
    const auto chord = render(a, 48000, 1), single = render(b, 48000, 73), other = render(reference, 48000, 512);
    // Voices sum in 32-bit float, so superposition holds to float rounding of
    // the chord level (about 1e-7 relative); a slot-dependent note would be off
    // by the size of the note itself.
    float peak = 0.0f, error = 0.0f;
    for (std::size_t i = 0; i < chord.left.size(); ++i)
    {
        peak = std::max({peak, std::abs(chord.left[i]), std::abs(chord.right[i])});
        error = std::max({error, std::abs(chord.left[i] - single.left[i] - other.left[i]),
                          std::abs(chord.right[i] - single.right[i] - other.right[i])});
    }
    std::cout << "chord_superposition_error=" << error << " chord_peak=" << peak << '\n';
    require(error < 1.0e-5f * peak, "at zero drive the filtered chord is the sum of slot-independent notes");
    a.prepare(48000); b.prepare(48000);
    for (auto* engine : {&a, &b})
    {
        engine->noteOn(1, 40, 127); engine->noteOn(2, 64, 90); engine->pitchBend(2, 15000);
    }
    const auto whole = render(a, 48000, 48000), split = render(b, 48000, 73);
    require(whole.left == split.left && whole.right == split.right, "both stereo channels repeat exactly across callback sizes");
    b.reset(); b.noteOn(1, 40, 127); b.noteOn(2, 64, 90); b.pitchBend(2, 15000);
    const auto reset = render(b, 48000, 1);
    require(reset.left == whole.left && reset.right == whole.right, "reset reproduces both channels exactly");
}
}
int main()
{
    targets(); spectrum(); bentAudio(); repeatability(); octavePresence();
    std::cout << "rhythm_notes=8 pitches=0..127 allocation=deterministic stereo_repeat=exact\n";
}
