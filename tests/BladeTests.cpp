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
Engine leadEngine(double rate = 48000.0, bool bass = true, bool rhythm = true)
{
    Engine engine;
    engine.setLayersEnabled(bass, rhythm);
    engine.setLeadEnabled(true);
    engine.prepare(rate);
    return engine;
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
    for (const double rate : {44100.0, 48000.0})
        for (const int note : {72, 84, 96})
        {
            Engine engine;
            engine.setLayersEnabled(false, false);
            engine.setLeadEnabled(true);
            engine.prepare(rate);
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
            const auto f0 = 440.0 * std::exp2((note - 69) / 12.0);
            double near = 0.0, far = 0.0;
            for (double hz = 40.0; hz < rate / 2.0; hz += 2.0)
            {
                const auto step = std::polar(1.0, -2.0 * pi * hz / rate);
                std::complex<double> sum{}, rotation{1.0, 0.0};
                for (const auto value : windowed) { sum += value * rotation; rotation *= step; }
                const auto power = std::norm(sum);
                const auto k = std::max(1.0, std::round(hz / f0));
                (std::abs(hz - k * f0) <= 0.25 * f0 ? near : far) += power;
            }
            const auto aliasDb = 10.0 * std::log10(std::max(2.0 * far, 1.0e-30) / near);
            worst = std::max(worst, aliasDb);
            std::cout << "blade rate=" << rate << " note=" << note << " off_harmonic_db=" << aliasDb << '\n';
            require(aliasDb < -50.0, "Blade's energy off its harmonics stays 50 dB down");
        }
    std::cout << "blade_worst_alias_db=" << worst << '\n';
}

// In front of the mix, never louder than Furnace (SPEC section 4).
void balance()
{
    const auto loudness = [](const std::vector<float>& left, const std::vector<float>& right)
    {
        auto meter = std::make_unique<dsp_core::LoudnessMeter>();
        meter->prepare(48000.0);
        for (std::size_t i = 0; i < left.size(); ++i) meter->process(left[i], right[i]);
        return meter->shortTermMaxLufs();
    };
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

int main()
{
    offByDefaultAndMono();
    noteChangesAndRelease();
    determinism();
    aliasing();
    balance();
    return 0;
}
