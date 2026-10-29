#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/Svf.h>
#include <dsp_core/WavetableBank.h>

namespace dropforge
{
// Anvil mode C, Halo, "round" (SPEC section 12): a sine at the bass pitch
// through gentle asymmetric (tube-like) saturation, so its overtones are
// soft, mostly low and include the even ones, then rolled off at 1.5 kHz.
// Felt more than heard, smoother than grit A, yet readable on small
// speakers. Pitch-locked to the clean sine, which still owns the lows.
class AnvilRound
{
public:
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples;

    // Destruction (SPEC section 12): overdriven like a distorted 808, still
    // rounded: the roll-off moves down to 1 kHz. Takes effect at prepare().
    void setDestroyed(bool on) noexcept { destroyed = on; }
    void prepare(double splitHz, double rate) noexcept
    {
        oversampler.prepare();
        setSplit(splitHz, rate);
        notch.set(dsp_core::Svf::Type::notch, 110.0, rate, 0.7);
        for (auto& filter : roll) filter.set(dsp_core::Svf::Type::lowpass, destroyed ? 1000.0 : 1500.0, rate);
        dsp_core::sine(0); // build the shared table off the audio thread
        reset();
    }
    void reset() noexcept
    {
        phase = 0;
        oversampler.reset(); notch.reset();
        for (auto& filter : postSplit) filter.reset();
        for (auto& filter : roll) filter.reset();
    }
    // Each note restarts in phase with the clean sine, which also starts at 0.
    void start() noexcept { phase = 0; }
    // The source is a pure sine, so only the drive's overtones pass: the
    // Split high-pass (Linkwitz-Riley 24 dB/oct) sits after the drive only.
    void setSplit(double hz, double rate) noexcept
    {
        for (auto& filter : postSplit) filter.set(dsp_core::Svf::Type::highpass, hz, rate);
    }
    void setNotch(double hz, double rate) noexcept { notch.set(dsp_core::Svf::Type::notch, hz, rate, 0.7); }

    float source(double hz, double rate) noexcept
    {
        const auto value = dsp_core::sine(phase);
        phase += static_cast<std::uint32_t>(std::llround(std::clamp(hz / rate, 0.0, 0.5) * 4294967296.0));
        return value;
    }
    // Amount (Anvil's drive, 0..1) sets how hard the saturation leans.
    float drive(float input, float amount) noexcept
    {
        const auto gain = (1.5f + 2.5f * amount) * (destroyed ? 7.0f : 1.0f);
        return oversampler.next(input, [gain](float value) noexcept { return shape(gain * value); });
    }
    float cleanup(float input) noexcept
    {
        for (auto& filter : postSplit) input = filter.next(input);
        input = notch.next(input);
        for (auto& filter : roll) input = filter.next(input);
        return input;
    }

private:
    // tanh with an offset: one side saturates earlier than the other, which
    // adds even overtones (the warm "tube" character) to the odd ones.
    static float shape(float value) noexcept
    {
        constexpr float bias = 0.35f, offset = 0.33637554433f; // tanh(0.35)
        return std::tanh(value + bias) - offset;
    }
    std::uint32_t phase = 0;
    dsp_core::Oversampler4xFloat oversampler;
    std::array<dsp_core::Svf, 2> postSplit{}, roll{};
    dsp_core::Svf notch;
    bool destroyed = false;
};
}
