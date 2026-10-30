#include "plugin/InstrumentCore.h"
#include <dsp_core/LoudnessMeter.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

// Blade, the lead (SPEC sections 3, 4): highest key, exactly mono, off by
// default, click-free note changes, band-limited, balanced under Furnace.
namespace
{
using dropforge::Engine;
using dropforge::LayerMask;
constexpr double pi = 3.14159265358979323846;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Render { std::vector<float> left, right, lead, bass, rhythmLeft, rhythmRight; };
Render render(Engine& engine, int samples, int block = 512, LayerMask mask = LayerMask::all)
{
    Render out;
    for (auto* channel : {&out.left, &out.right, &out.lead, &out.bass, &out.rhythmLeft, &out.rhythmRight})
        channel->assign(static_cast<std::size_t>(samples), 0.0f);
    for (int at = 0; at < samples; at += block)
    {
        const auto i = static_cast<std::size_t>(at);
        engine.process(out.left.data() + i, out.right.data() + i, std::min(block, samples - at), mask,
                       {out.bass.data() + i, nullptr, out.rhythmLeft.data() + i, out.rhythmRight.data() + i, out.lead.data() + i});
    }
    return out;
}
float peak(const std::vector<float>& audio, std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    float result = 0.0f;
    for (auto i = from; i < std::min(to, audio.size()); ++i) result = std::max(result, std::abs(audio[i]));
    return result;
}
float largestStep(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    float step = 0.0f;
    for (auto i = std::max<std::size_t>(from, 1); i < std::min(to, audio.size()); ++i)
        step = std::max(step, std::abs(audio[i] - audio[i - 1]));
    return step;
}
Engine leadEngine(double rate = 48000.0, bool bass = true, bool rhythm = true, int mode = 0, bool destroy = false)
{
    Engine engine;
    engine.setLayersEnabled(bass, rhythm);
    engine.setLeadEnabled(true);
    engine.setLeadMode(mode);
    engine.setLeadDestruction(destroy);
    engine.prepare(rate);
    return engine;
}
double shortTermLufs(const std::vector<float>& left, const std::vector<float>& right)
{
    auto meter = std::make_unique<dsp_core::LoudnessMeter>();
    meter->prepare(48000.0);
    for (std::size_t i = 0; i < left.size(); ++i) meter->process(left[i], right[i]);
    return meter->shortTermMaxLufs();
}

void offByDefaultAndMono()
{
    Engine silent;
    silent.prepare(48000.0);
    for (const int note : {40, 52, 64}) silent.noteOn(1, note, 110);
    require(peak(render(silent, 9600).lead) == 0.0f, "Blade is off by default and costs nothing");

    auto engine = leadEngine();
    for (const int note : {40, 47, 52}) engine.noteOn(1, note, 110);
    require(engine.leadTarget().note == 52, "Blade takes the highest held key");
    const auto audio = render(engine, 24000, 512, LayerMask::lead);
    require(audio.left == audio.right && audio.left == audio.lead, "Blade alone is exactly mono, L == R");
    require(peak(audio.lead, 4800) > 0.05f, "Blade sounds on the highest key");
    for (const auto value : audio.lead) require(std::isfinite(value), "Blade output is finite");
}

// The selected key changes with a 2 ms crossfade: never a click, never a
// change for keys below it; releasing the top key falls back to the next.
void noteChangesAndRelease()
{
    auto steady = leadEngine();
    steady.noteOn(1, 60, 110);
    const auto natural = largestStep(render(steady, 24000).lead, 4800, 24000);
    steady.reset();
    steady.noteOn(1, 67, 110);
    const auto naturalHigh = largestStep(render(steady, 24000).lead, 4800, 24000);

    auto engine = leadEngine();
    engine.noteOn(1, 60, 110);
    auto first = render(engine, 12000).lead;
    engine.noteOn(1, 48, 110); // below the lead: no change
    require(engine.leadTarget().note == 60, "a lower key never moves Blade");
    engine.noteOn(1, 67, 110);
    require(engine.leadTarget().note == 67, "a higher key takes Blade");
    auto change = render(engine, 12000).lead;
    engine.noteOff(1, 67);
    require(engine.leadTarget().note == 60, "releasing the top key falls back to the next highest");
    auto back = render(engine, 12000).lead;
    first.insert(first.end(), change.begin(), change.end());
    first.insert(first.end(), back.begin(), back.end());
    const auto step = std::max(largestStep(first, 11900, 13500), largestStep(first, 23900, 25500));
    std::cout << "blade_change_step=" << step << " natural=" << std::max(natural, naturalHigh) << '\n';
    require(step <= 1.1f * std::max(natural, naturalHigh), "Blade note changes do not click");

    engine.noteOff(1, 60);
    engine.noteOff(1, 48);
    const auto tail = render(engine, 4800).lead;
    // Release (6 ms) after the 32-sample alignment, then exactly silent.
    const auto silentFrom = static_cast<std::size_t>(std::ceil(48000.0 * dropforge::Engine::releaseSeconds)) + 2 * dropforge::BladeVoice::latencySamples + 8;
    require(peak(tail, silentFrom) == 0.0f, "Blade is silent after its release");
}

// Same render at any block size, and after reset.
void determinism()
{
    const auto run = [](int block)
    {
        auto engine = leadEngine();
        for (const int note : {40, 52, 64}) engine.noteOn(1, note, 100);
        auto audio = render(engine, 9000, block).left;
        engine.noteOn(1, 71, 120);
        engine.pitchBend(1, 12000);
        const auto more = render(engine, 9000, block).left;
        audio.insert(audio.end(), more.begin(), more.end());
        return audio;
    };
    const auto a = run(512);
    require(a == run(1) && a == run(73), "Blade renders identically at blocks 1, 73 and 512");
}

// Energy between the harmonics of the played note: aliases fold to arbitrary
// frequencies, while the detune and the drive's intermodulation only add
// sidebands within a few percent of each harmonic. Only the middle half
// between neighbouring harmonics is counted, so its power is doubled to
// stand for the whole spectrum. Brute-force spectrum, Hann window, 2 Hz bins.
void aliasing()
{
    double worst = -300.0;
    for (int mode = 0; mode < 4; ++mode)
    for (const bool destroy : {false, true})
    for (const double rate : {44100.0, 48000.0})
        for (const int note : {72, 84, 96})
        {
            auto engine = leadEngine(rate, false, false, mode, destroy);
            engine.noteOn(1, note, 127);
            const auto length = static_cast<std::size_t>(rate / 2.0);
            const auto audio = render(engine, static_cast<int>(rate / 4.0 + length)).lead;
            std::vector<double> windowed(length);
            double windowSum = 0.0;
            for (std::size_t i = 0; i < length; ++i)
            {
                const auto w = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(length));
                windowed[i] = w * audio[audio.size() - length + i];
                windowSum += w;
            }
            // Halo (C) adds a sine an octave down: its series starts at half the note.
            const auto f0 = 440.0 * std::exp2((note - 69) / 12.0) * (mode == 2 ? 0.5 : 1.0);
            double near = 0.0, far = 0.0;
            for (double hz = 40.0; hz < rate / 2.0; hz += 2.0)
            {
                const auto step = std::polar(1.0, -2.0 * pi * hz / rate);
                std::complex<double> sum{}, rotation{1.0, 0.0};
                for (const auto value : windowed) { sum += value * rotation; rotation *= step; }
                const auto power = std::norm(sum);
                const auto k = std::max(1.0, std::round(hz / f0));
                // Detuned oscillators spread each harmonic by +-hz x detune
                // (doubled for the drive's intermodulation); the near zone
                // widens with it, keeping at least a fifth of each gap measured.
                const auto spread = 2.0 * hz * (std::exp2(dropforge::BladeVoice::design(mode, destroy).detuneCents / 1200.0) - 1.0);
                const auto zone = std::min(0.25 * f0 + spread, 0.4 * f0);
                (std::abs(hz - k * f0) <= zone ? near : far) += power;
            }
            const auto aliasDb = 10.0 * std::log10(std::max(2.0 * far, 1.0e-30) / near);
            worst = std::max(worst, aliasDb);
            std::cout << "blade mode=" << static_cast<char>('A' + mode) << " destroy=" << destroy << " rate=" << rate
                      << " note=" << note << " off_harmonic_db=" << aliasDb << '\n';
            require(aliasDb < -50.0, "Blade's energy off its harmonics stays 50 dB down");
        }
    std::cout << "blade_worst_alias_db=" << worst << '\n';
}

// In front of the mix, never louder than Furnace (SPEC section 4).
void balance()
{
    const auto loudness = shortTermLufs;
    auto engine = leadEngine();
    for (const int note : {40, 47, 52, 55, 64}) engine.noteOn(1, note, 127);
    const auto audio = render(engine, 48000 * 4);
    const auto lead = loudness(audio.lead, audio.lead);
    const auto furnace = loudness(audio.rhythmLeft, audio.rhythmRight);
    std::cout << "blade_lufs=" << lead << " furnace_lufs=" << furnace << " blade_vs_furnace_lu=" << lead - furnace << '\n';
    require(lead <= furnace + 0.5, "Blade is not louder than Furnace");
    require(lead >= furnace - 8.0, "Blade stays in front, within 8 LU of Furnace");
}
}

// Each mode lands within 1 LU of A; Destruction within +1/-1.5 LU of its
// clean mode (SPEC section 12): switching changes flavour, not volume.
void modeLoudness()
{
    const auto measure = [](int mode, bool destroy)
    {
        double sum = 0.0;
        for (const int note : {57, 64, 71}) // A3, E4, B4: typical lead range
        {
            auto engine = leadEngine(48000.0, false, false, mode, destroy);
            engine.noteOn(1, note, 127);
            const auto audio = render(engine, 48000 * 4);
            sum += shortTermLufs(audio.lead, audio.lead);
        }
        return sum / 3.0;
    };
    const auto reference = measure(0, false);
    for (int mode = 0; mode < 4; ++mode)
    {
        const auto clean = measure(mode, false), destroyed = measure(mode, true);
        std::cout << "blade mode=" << static_cast<char>('A' + mode) << " clean_vs_a_lu=" << clean - reference
                  << " destroyed_vs_clean_lu=" << destroyed - clean << '\n';
        require(std::abs(clean - reference) <= 1.0, "each Blade mode is within 1 LU of A");
        require(destroyed - clean <= 1.0 && destroyed - clean >= -1.5, "destroyed Blade stays within +1/-1.5 LU of its clean mode");
    }
}

// Switching mode and Destruction under a held note: click-free (no steeper
// than any mode's own steady sound) and identical at every block size.
void modeSwitching()
{
    float natural = 0.0f;
    for (int mode = 0; mode < 4; ++mode)
        for (const bool destroy : {false, true})
        {
            auto steady = leadEngine(48000.0, false, false, mode, destroy);
            steady.noteOn(1, 64, 120);
            natural = std::max(natural, largestStep(render(steady, 24000).lead, 4800, 24000));
        }
    const auto run = [](int block)
    {
        auto engine = leadEngine(48000.0, false, false);
        engine.noteOn(1, 64, 120);
        auto audio = render(engine, 9600, block).lead;
        for (int step = 1; step <= 7; ++step)
        {
            engine.setLeadMode(step % 4);
            engine.setLeadDestruction(step % 2 == 1);
            const auto more = render(engine, 4800, block).lead;
            audio.insert(audio.end(), more.begin(), more.end());
        }
        return audio;
    };
    const auto a = run(512);
    require(a == run(1) && a == run(73), "Blade mode switches render identically at blocks 1, 73 and 512");
    const auto step = largestStep(a, 9000, a.size());
    std::cout << "blade_switch_step=" << step << " natural_step=" << natural << '\n';
    require(step <= 1.1f * natural, "switching Blade's mode or Destruction does not click");
}

// Portamento and Octave (SPEC section 3). Pitches are read from the voice
// (before bend), so the checks are exact rather than spectral estimates.
void portamentoAndOctave()
{
    constexpr double rate = 48000.0;
    const auto hz = [](int note) { return 440.0 * std::exp2((note - 69) / 12.0); };
    const auto near = [](double a, double b) { return std::abs(std::log2(a / b)) < 1.0e-6; };
    using Portamento = Engine::Portamento;

    // Off: a new top note jumps at once, with a fresh attack.
    {
        auto engine = leadEngine(rate, false, false);
        engine.setLeadPortamento(Portamento::off, 200.0f);
        engine.noteOn(1, 60, 110);
        render(engine, 4800);
        engine.noteOn(1, 67, 110);
        render(engine, 64);
        require(near(engine.leadPitch(), hz(67)), "Portamento Off: the new note sounds at once");
    }
    // Legato: overlapping notes slide over the time, evenly in pitch; a note
    // after a gap starts fresh at its own pitch.
    for (const float ms : {60.0f, 250.0f})
    {
        auto engine = leadEngine(rate, false, false);
        engine.setLeadPortamento(Portamento::legato, ms);
        engine.noteOn(1, 60, 110);
        render(engine, 4800);
        engine.noteOn(1, 72, 110); // overlaps the held C4: one octave up
        const auto total = static_cast<int>(std::lround(rate * ms / 1000.0));
        render(engine, total / 2);
        const auto midway = std::log2(engine.leadPitch() / hz(60));
        require(std::abs(midway - 0.5) < 0.03, "Legato slides evenly in pitch: half the time, half the octave");
        render(engine, total / 2 + 64);
        require(near(engine.leadPitch(), hz(72)), "Legato lands exactly on the new note after its time");
        // No new attack while sliding: the output never dips to silence.
        engine.noteOff(1, 72);
        render(engine, 4800);
        engine.noteOff(1, 60);
        render(engine, 4800);
        engine.noteOn(1, 64, 110);
        render(engine, 64);
        require(near(engine.leadPitch(), hz(64)), "Legato: a note after a gap starts at its own pitch");
    }
    // Legato keeps the line going: the overlapping note never retriggers.
    {
        auto engine = leadEngine(rate, false, false);
        engine.setLeadPortamento(Portamento::legato, 120.0f);
        engine.noteOn(1, 60, 110);
        const auto before = render(engine, 9600).lead;
        engine.noteOn(1, 64, 110);
        const auto during = render(engine, 9600).lead;
        const auto held = peak(before, 4800);
        require(peak(during, 0, 480) > 0.5f * held, "Legato: no new attack, the sound carries straight on");
    }
    // Always: even after a gap, the new note slides from the last one.
    {
        auto engine = leadEngine(rate, false, false);
        engine.setLeadPortamento(Portamento::always, 100.0f);
        engine.noteOn(1, 60, 110);
        render(engine, 4800);
        engine.noteOff(1, 60);
        render(engine, 9600);
        engine.noteOn(1, 67, 110);
        render(engine, 64);
        const auto early = engine.leadPitch();
        require(early < hz(67) * 0.98 && early > hz(60), "Always: a note after a gap slides up from the last note");
        render(engine, 4800);
        require(near(engine.leadPitch(), hz(67)), "Always: and lands on it");
    }
    // Octave: -1 and +1 relative to the top key, also while it is held.
    {
        auto engine = leadEngine(rate, false, false);
        engine.setLeadOctave(1);
        engine.noteOn(1, 60, 110);
        render(engine, 64);
        require(near(engine.leadPitch(), hz(72)), "Octave +1 plays the top key an octave up");
        engine.setLeadOctave(-1);
        render(engine, 64);
        require(near(engine.leadPitch(), hz(48)), "Octave -1 moves the held note an octave down at once");
        require(engine.leadTarget().note == 60, "Octave changes Blade's pitch, not which key it follows");
    }
    // Slides render identically at every block size.
    const auto run = [](int block)
    {
        auto engine = leadEngine(48000.0, false, false);
        engine.setLeadPortamento(Portamento::always, 180.0f);
        engine.noteOn(1, 55, 100);
        auto audio = render(engine, 7000, block).lead;
        engine.noteOn(1, 67, 120);
        auto more = render(engine, 7000, block).lead;
        audio.insert(audio.end(), more.begin(), more.end());
        engine.noteOff(1, 67);
        engine.noteOff(1, 55);
        engine.noteOn(1, 48, 90);
        more = render(engine, 9000, block).lead;
        audio.insert(audio.end(), more.begin(), more.end());
        return audio;
    };
    const auto a = run(512);
    require(a == run(1) && a == run(73), "Portamento renders identically at blocks 1, 73 and 512");
    std::cout << "portamento and octave passed\n";
}

// The solo rule (SPEC section 3): Anvil the lowest and Blade the highest key
// whenever another instrument plays; an instrument alone follows the newest.
void soloRule()
{
    const auto press = [](Engine& engine) { for (const int note : {52, 40, 64, 47}) engine.noteOn(1, note, 100); };
    {
        // Anvil + Blade, Furnace off: still lowest and highest.
        Engine engine;
        engine.setLayersEnabled(true, false);
        engine.setLeadEnabled(true);
        engine.prepare(48000.0);
        press(engine);
        require(engine.bassTarget().note == 40 && engine.leadTarget().note == 64,
                "with Furnace off, Anvil still takes the lowest key and Blade the highest");
        // Blade off: Anvil plays alone and follows the newest key.
        engine.setLeadEnabled(false);
        require(engine.bassTarget().note == 47, "Anvil alone follows the newest key");
        engine.setLeadEnabled(true);
        require(engine.bassTarget().note == 40, "Blade joining returns Anvil to the lowest key");
        // Anvil off: Blade plays alone and follows the newest key.
        engine.setLayersEnabled(false, false);
        require(engine.leadTarget().note == 47, "Blade alone follows the newest key");
        engine.noteOn(1, 45, 100);
        require(engine.leadTarget().note == 45, "soloed Blade moves to each new key, even a lower one");
        engine.noteOff(1, 45);
        require(engine.leadTarget().note == 47, "and falls back to the previous newest on release");
        engine.setLayersEnabled(false, true);
        require(engine.leadTarget().note == 64, "Furnace joining returns Blade to the highest key");
    }
    {
        // Soloed Blade sounds the newest key (a real attack on the new note).
        auto engine = leadEngine(48000.0, false, false);
        engine.noteOn(1, 64, 100);
        render(engine, 4800);
        engine.noteOn(1, 60, 100);
        render(engine, 64);
        require(std::abs(engine.leadPitch() - 440.0 * std::exp2((60 - 69) / 12.0)) < 1.0e-6, "soloed Blade plays the newest key");
    }
    std::cout << "solo rule passed\n";
}

int main()
{
    offByDefaultAndMono();
    noteChangesAndRelease();
    determinism();
    aliasing();
    balance();
    modeLoudness();
    modeSwitching();
    portamentoAndOctave();
    soloRule();
    return 0;
}
