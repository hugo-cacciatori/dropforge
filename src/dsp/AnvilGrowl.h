#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/Svf.h>
#include <dsp_core/WavetableBank.h>

namespace dropforge
{
// Anvil grit candidate B, "growl" (SPEC section 4): the modern, aggressive
// alternative to the accepted harmonic grit A. A hollow saw/square blend at
// the bass pitch, band-limited to the mid-bass, folded and clipped at 4x,
// then shaped by two vowel formants so it talks like a dubstep growl.
// Exactly mono and pitch-locked to Anvil's clean sine, which owns the lows.
class AnvilGrowl
{
public:
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples;

    void prepare(double splitHz, double rate) noexcept
    {
        oversampler.prepare();
        setSplit(splitHz, rate);
        // Band-limit the source before the folder: folding a full-range saw
        // would spray partials far past what 4x oversampling can contain.
        for (auto& filter : sourceLowpass) filter.set(dsp_core::Svf::Type::lowpass, 3000.0, rate);
        notch.set(dsp_core::Svf::Type::notch, 110.0, rate, 0.7);
        // An open "ah": first formant 700 Hz, second 1150 Hz. Roar (M2)
        // sweeps them together; until then they sit at this vowel.
        formants[0].set(dsp_core::Svf::Type::bell, 700.0, rate, 3.0, 9.0);
        formants[1].set(dsp_core::Svf::Type::bell, 1150.0, rate, 4.0, 6.0);
        for (auto& filter : topLowpass) filter.set(dsp_core::Svf::Type::lowpass, 4000.0, rate);
        dsp_core::sine(0); // build the shared table off the audio thread
        reset();
    }
    void reset() noexcept
    {
        oscillator.reset();
        sourceHz = -1.0;
        oversampler.reset(); notch.reset();
        for (auto* group : {&preSplit, &postSplit, &sourceLowpass, &topLowpass})
            for (auto& filter : *group) filter.reset();
        for (auto& filter : formants) filter.reset();
    }
    // Each note restarts the cycle in phase with the clean sine, so attacks
    // repeat exactly; filter and oversampler histories carry on unclicked.
    void start() noexcept { oscillator.reset(); }
    // Linkwitz-Riley 24 dB/oct at Split, before and after the folder, as grit A.
    void setSplit(double hz, double rate) noexcept
    {
        for (auto& filter : preSplit) filter.set(dsp_core::Svf::Type::highpass, hz, rate);
        for (auto& filter : postSplit) filter.set(dsp_core::Svf::Type::highpass, hz, rate);
    }
    // Folding adds difference tones at the root; the notch keeps them from
    // fighting the clean sine. Set at the FIR-aligned pitch.
    void setNotch(double hz, double rate) noexcept { notch.set(dsp_core::Svf::Type::notch, hz, rate, 0.7); }

    // Source at the played (bent) pitch; silent while the envelope is idle.
    float source(double hz, double rate) noexcept
    {
        if (hz != sourceHz)
        {
            sourceHz = hz;
            oscillator.setFrequency(dsp_core::WavetableBank::saw(), hz, rate);
        }
        // Saw minus half a half-cycle-shifted saw: odd partials 1.5x, even
        // 0.5x. Hollow like a square, with enough even partials to snarl.
        auto value = 0.6f * oscillator.nextBlend(halfCycle, 0.5f);
        for (auto& filter : preSplit) value = filter.next(value);
        for (auto& filter : sourceLowpass) value = filter.next(value);
        return value;
    }
    // Amount (Anvil's grit drive, 0..1) sets how many times the wave folds.
    float drive(float input, float amount) noexcept
    {
        const auto gain = 1.5f + 4.5f * amount;
        return oversampler.next(input, [gain](float value) noexcept { return shape(gain * value); });
    }
    float cleanup(float input) noexcept
    {
        for (auto& filter : postSplit) input = filter.next(input);
        input = notch.next(input);
        for (auto& filter : formants) input = filter.next(input);
        for (auto& filter : topLowpass) input = filter.next(input);
        return input;
    }

private:
    // Sine wavefolder with a small offset (the offset adds even partials, the
    // "throat"), then a soft clipper that rounds the folds' sharpest peaks.
    // The fold reads the shared sine table: unit input is a quarter cycle.
    static float shape(float value) noexcept
    {
        if (!(std::abs(value) < 64.0f)) value = 0.0f;
        constexpr float bias = 0.15f, quarterCycle = 1073741824.0f;
        const auto fold = [](float x) noexcept
        {
            return dsp_core::sine(static_cast<std::uint32_t>(static_cast<std::int64_t>(x * quarterCycle)));
        };
        const auto folded = fold(value + bias) - fold(bias);
        return std::tanh(1.5f * folded) * (1.0f / 0.9051482536f); // tanh(1.5)
    }

    static constexpr std::uint32_t halfCycle = 0x80000000u;
    dsp_core::WavetableOscillator oscillator;
    double sourceHz = -1.0;
    dsp_core::Oversampler4xFloat oversampler;
    std::array<dsp_core::Svf, 2> preSplit{}, postSplit{}, sourceLowpass{}, topLowpass{}, formants{};
    dsp_core::Svf notch;
};
}
