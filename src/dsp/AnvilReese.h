#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/Svf.h>
#include <dsp_core/WavetableBank.h>

namespace dropforge
{
// Anvil mode D, Alloy, "reese" (SPEC section 12): two band-limited saws a
// few cents apart. Their overtones drift in and out of phase, so the bass
// moves on its own with no LFO: the classic neuro/drum-and-bass reese. A
// light 4x drive gives it an edge; it is band-limited to the mid-bass and
// sits mono above the clean sine, which still owns the lows.
class AnvilReese
{
public:
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples;
    static constexpr double detuneCents = 8.0, destroyedDetuneCents = 20.0;

    // Destruction (SPEC section 12): a wider detune (faster, deeper beats)
    // and a snarling drive, with a tighter 2.5 kHz top so the sub stays the
    // clean foundation. Takes effect at prepare().
    void setDestroyed(bool on) noexcept { destroyed = on; }
    void prepare(double splitHz, double rate) noexcept
    {
        oversampler.prepare();
        setSplit(splitHz, rate);
        // Band-limit before the drive (keeps 4x oversampling sufficient) and
        // after it (keeps the reese a mid-bass, never a fizz).
        for (auto& filter : sourceLowpass) filter.set(dsp_core::Svf::Type::lowpass, 3000.0, rate);
        for (auto& filter : topLowpass) filter.set(dsp_core::Svf::Type::lowpass, destroyed ? 2500.0 : 3000.0, rate);
        notch.set(dsp_core::Svf::Type::notch, 110.0, rate, 0.7);
        reset();
    }
    void reset() noexcept
    {
        start();
        sourceHz = -1.0;
        oversampler.reset(); notch.reset();
        for (auto* group : {&preSplit, &postSplit, &sourceLowpass, &topLowpass})
            for (auto& filter : *group) filter.reset();
    }
    // Fixed start phases a third of a cycle apart: every attack sounds the
    // same and the two saws never start in a full-strength unison.
    void start() noexcept { low.reset(0); high.reset(0x55555555u); }
    void setSplit(double hz, double rate) noexcept
    {
        for (auto& filter : preSplit) filter.set(dsp_core::Svf::Type::highpass, hz, rate);
        for (auto& filter : postSplit) filter.set(dsp_core::Svf::Type::highpass, hz, rate);
    }
    void setNotch(double hz, double rate) noexcept { notch.set(dsp_core::Svf::Type::notch, hz, rate, 0.7); }

    float source(double hz, double rate) noexcept
    {
        if (hz != sourceHz)
        {
            sourceHz = hz;
            const auto& bank = dsp_core::WavetableBank::saw();
            const auto cents = destroyed ? destroyedDetuneCents : detuneCents;
            low.setFrequency(bank, hz * std::exp2(-cents / 1200.0), rate);
            high.setFrequency(bank, hz * std::exp2(cents / 1200.0), rate);
        }
        auto value = 0.5f * (low.next() + high.next());
        for (auto& filter : preSplit) value = filter.next(value);
        for (auto& filter : sourceLowpass) value = filter.next(value);
        return value;
    }
    // Amount (Anvil's drive, 0..1) sets how much edge the drive adds.
    float drive(float input, float amount) noexcept
    {
        const auto gain = (1.2f + 2.8f * amount) * (destroyed ? 5.0f : 1.0f);
        return oversampler.next(input, [gain](float value) noexcept { return std::tanh(gain * value); });
    }
    float cleanup(float input) noexcept
    {
        for (auto& filter : postSplit) input = filter.next(input);
        input = notch.next(input);
        for (auto& filter : topLowpass) input = filter.next(input);
        return input;
    }

private:
    dsp_core::WavetableOscillator low, high;
    double sourceHz = -1.0;
    dsp_core::Oversampler4xFloat oversampler;
    std::array<dsp_core::Svf, 2> preSplit{}, postSplit{}, sourceLowpass{}, topLowpass{};
    dsp_core::Svf notch;
    bool destroyed = false;
};
}
