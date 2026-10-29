#include <dsp_core/WavetableBank.h>
#include "support/StereoMetrics.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <vector>

namespace
{
constexpr double pi = 3.14159265358979323846;
int failures = 0;
void expect(bool condition, const char* what)
{
    if (!condition) { std::cout << "FAILED: " << what << '\n'; ++failures; }
}
double midiHz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }

struct Spectrum { double aliasDb = -300.0, topHarmonicHz = 0.0; };
// Blackman-Harris (-92 dB sidelobes) over a steady tone. Everything farther
// than six bins from a harmonic below Nyquist is an alias product.
template <typename Render>
Spectrum analyse(Render render, double hz, double rate, double idealAmplitude(int))
{
    constexpr std::size_t size = 32768;
    std::vector<float> audio(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        const auto x = 2.0 * pi * static_cast<double>(i) / (size - 1);
        const auto window = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
        audio[i] = static_cast<float>(render() * window);
    }
    const auto bins = dsp_core::test::spectrum(audio);
    const auto binHz = rate / size;
    const auto peakNear = [&](double at) {
        const auto centre = static_cast<long>(std::lround(at / binHz));
        double peak = 0.0;
        for (long b = std::max(0L, centre - 3); b <= std::min<long>(size / 2, centre + 3); ++b)
            peak = std::max(peak, std::abs(bins[static_cast<std::size_t>(b)]));
        return peak;
    };
    const auto fundamental = peakNear(hz);
    Spectrum result;
    for (std::size_t b = 1; b < size / 2; ++b)
    {
        const auto at = static_cast<double>(b) * binHz;
        const auto nearest = std::max(1.0, std::round(at / hz)) * hz;
        if (std::abs(at - nearest) <= 6.0 * binHz && nearest < rate / 2) continue;
        result.aliasDb = std::max(result.aliasDb, 20.0 * std::log10(std::abs(bins[b]) / fundamental + 1.0e-30));
    }
    // Brightness: the highest harmonic still within 12 dB of its ideal level.
    for (int k = 1; k * hz < rate / 2; ++k)
        if (idealAmplitude(k) > 0.0
            && peakNear(k * hz) / fundamental >= 0.25 * idealAmplitude(k) / idealAmplitude(1))
            result.topHarmonicHz = k * hz;
    return result;
}
double sawAmplitude(int k) { return 1.0 / k; }
double squareAmplitude(int k) { return k % 2 ? 1.0 / k : 0.0; }
double pulse25Amplitude(int k) { return std::abs(std::sin(pi * k * 0.25)) / k; }
}

int main()
{
    const auto started = std::chrono::steady_clock::now();
    const auto& bank = dsp_core::WavetableBank::saw();
    const auto buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "levels=" << bank.levelCount() << " build_ms=" << buildMs << '\n';

    // The full-band table is the ideal rising saw away from its edge.
    double shapeError = 0.0;
    const auto& top = bank.level(0);
    for (int i = dsp_core::WavetableBank::tableSize / 10; i < dsp_core::WavetableBank::tableSize * 9 / 10; ++i)
        shapeError = std::max(shapeError, std::abs(top.samples[static_cast<std::size_t>(i)]
                                                   - (2.0 * i / dsp_core::WavetableBank::tableSize - 1.0)));
    expect(shapeError < 0.01, "level 0 matches 2 x phase - 1");
    expect(top.samples.back() == top.samples.front(), "guard point repeats the first sample");

    // Each pitch gets the brightest level that cannot alias.
    for (double increment = 1.0e-5; increment <= 0.45; increment *= 1.01)
    {
        const auto index = bank.levelFor(increment);
        expect(bank.level(index).harmonics * increment <= 0.45, "chosen level stays under 0.45 x rate");
        if (index > 0) expect(bank.level(index - 1).harmonics * increment > 0.45, "chosen level is the brightest safe one");
    }

    // Alias and brightness gates (SPEC section 4): steady single notes up to MIDI 96.
    double worstAlias = -300.0, dullest = 1.0;
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0})
        for (int note = 24; note <= 96; note += note < 90 ? 6 : 1)
        {
            const auto hz = midiHz(note);
            dsp_core::WavetableOscillator saw, square, pulse;
            for (auto* osc : {&saw, &square, &pulse})
            {
                osc->reset(dsp_core::seededPhase(1, static_cast<std::uint32_t>(note), 0, 0));
                osc->setFrequency(bank, hz, rate);
            }
            const auto results = {analyse([&] { return saw.next(); }, hz, rate, sawAmplitude),
                                  analyse([&] { return square.nextPulse(0.5); }, hz, rate, squareAmplitude),
                                  analyse([&] { return pulse.nextPulse(0.25); }, hz, rate, pulse25Amplitude)};
            for (const auto& r : results)
            {
                worstAlias = std::max(worstAlias, r.aliasDb);
                // Rates above 48 kHz place 0.33 x rate beyond hearing; require 15.8 kHz there.
                dullest = std::min(dullest, r.topHarmonicHz / std::min(rate, 48000.0));
                if (r.aliasDb > -50.0 || r.topHarmonicHz < 0.33 * std::min(rate, 48000.0))
                    std::cout << "rate=" << rate << " note=" << note << " alias_db=" << r.aliasDb
                              << " top_harmonic_hz=" << r.topHarmonicHz << '\n';
            }
        }
    std::cout << "worst_alias_db=" << worstAlias << " dullest_top_harmonic_of_rate=" << dullest << '\n';
    expect(worstAlias <= -50.0, "alias products at least 50 dB below the fundamental");
    expect(dullest >= 0.33, "harmonics reach at least 0.33 x rate (15.8 kHz at 48 kHz)");

    // Pulse family: zero mean at every width; width 0.5 is a +-1 square.
    dsp_core::WavetableOscillator pulse;
    pulse.setFrequency(bank, 48000.0 / 64.0, 48000.0); // exactly 64 samples per cycle
    expect(pulse.phaseIncrement() == (1u << 26), "exact power-of-two increment");
    for (double width = 0.1; width < 0.95; width += 0.1)
    {
        pulse.reset();
        double mean = 0.0;
        for (int i = 0; i < 64; ++i) mean += pulse.nextPulse(width) / 64.0;
        expect(std::abs(mean) < 1.0e-4, "pulse has zero mean");
    }
    pulse.setFrequency(bank, 50.0, 48000.0);
    pulse.reset();
    double squareError = 0.0;
    for (int i = 0; i < 960; ++i)
    {
        const auto phase = (pulse.currentPhase() / 4294967296.0);
        const auto value = pulse.nextPulse(0.5);
        const auto edge = std::min({phase, std::abs(phase - 0.5), 1.0 - phase});
        if (edge > 0.05) squareError = std::max(squareError, std::abs(value - (phase < 0.5 ? 1.0 : -1.0)));
    }
    expect(squareError < 0.05, "width 0.5 is a +-1 square away from its edges");

    // Integer phase: exact pitch, exact wrap, exact repeats.
    dsp_core::WavetableOscillator a, b;
    a.setFrequency(bank, 1234.5678, 48000.0);
    b.setFrequency(bank, 1234.5678, 48000.0);
    expect(std::abs(a.phaseIncrement() / 4294967296.0 - 1234.5678 / 48000.0) <= 1.0 / 8589934592.0,
           "increment within half a phase step of the exact pitch");
    a.reset(0xdeadbeefu); b.reset(0xdeadbeefu);
    bool identical = true;
    for (int i = 0; i < 48000; ++i) identical = identical && a.next() == b.next();
    expect(identical, "same seed renders identically");
    expect(a.currentPhase() == static_cast<std::uint32_t>(0xdeadbeefu + 48000u * b.phaseIncrement()),
           "phase is start + n x increment modulo 2^32");

    // Non-finite input silences instead of producing NaN.
    a.setFrequency(bank, std::numeric_limits<double>::quiet_NaN(), 48000.0);
    expect(a.phaseIncrement() == 0 && std::isfinite(a.next()), "NaN frequency holds a finite value");

    // Seeded start phases: deterministic and spread evenly over the cycle.
    std::set<std::uint32_t> seen;
    std::vector<int> histogram(16);
    std::size_t count = 0;
    for (std::uint32_t channel = 1; channel <= 16; ++channel)
        for (std::uint32_t note = 0; note < 128; ++note)
            for (std::uint32_t slot = 0; slot < 8; ++slot)
                for (std::uint32_t unison = 0; unison < 16; ++unison, ++count)
                {
                    const auto seed = dsp_core::seededPhase(channel, note, slot, unison);
                    seen.insert(seed);
                    ++histogram[seed >> 28];
                }
    expect(dsp_core::seededPhase(3, 40, 2, 5) == dsp_core::seededPhase(3, 40, 2, 5), "seeds repeat");
    expect(seen.size() + 64 >= count, "seeds are practically all distinct");
    for (const auto bucket : histogram)
        expect(std::abs(bucket - static_cast<double>(count) / 16) < count / 16 * 0.05, "seeds cover the cycle evenly");
    std::cout << "seeds=" << count << " distinct=" << seen.size() << '\n';
    return failures ? 1 : 0;
}
