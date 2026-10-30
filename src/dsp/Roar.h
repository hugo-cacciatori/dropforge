#pragma once

#include <algorithm>
#include <cmath>
#include <dsp_core/Svf.h>

namespace dropforge
{
// Roar, the mod wheel's growl (SPEC section 3). One wheel position moves
// three things together, so the hand plays the growl like a wah pedal:
//   - drive: the layer's nonlinear stage is pushed harder (richer harmonics);
//   - a vocal formant peak sweeps up from "oo" towards "ee";
//   - the peak grows from flat to a strong resonance.
// Repeated wheel motion makes wubs; there is no LFO. Amount 0 skips the stage
// entirely, so the base sound stays bit-identical.
class RoarFormant
{
public:
    // Full wheel: the drive input up to +9.5 dB.
    static constexpr float driveBoost = 2.0f;
    static float drive(float amount) noexcept { return 1.0f + driveBoost * amount; }

    // lowHz/highHz: where the peak sweeps for this layer; peakDb at full
    // wheel; trim: level at full wheel, so the growl moves the tone rather
    // than the loudness.
    void prepare(double sampleRate, double lowHz, double highHz, double peakDb, float trim) noexcept
    {
        rate = sampleRate; low = lowHz; high = highHz; peak = peakDb; fullTrim = trim;
        reset();
    }
    void reset() noexcept { filter.reset(); active = false; current = -1.0f; }
    float process(float input, float amount) noexcept
    {
        if (amount <= 0.0f)
        {
            // The ramp reached zero: the peak was already under 0.05 dB.
            if (active) reset();
            return input;
        }
        // Retune only when the wheel has moved a step: the filter keeps its
        // state, so steps this small sweep smoothly without zipper noise.
        if (!active || std::abs(amount - current) >= 0.005f || (amount < 0.01f && amount != current))
        {
            current = amount;
            const auto hz = low * std::pow(high / low, static_cast<double>(amount));
            filter.set(dsp_core::Svf::Type::bell, hz, rate, 2.5, peak * amount);
            // Even in decibels: the wheel's travel lifts the level steadily.
            gain = std::pow(fullTrim, amount);
            active = true;
        }
        return gain * filter.next(input);
    }

private:
    dsp_core::Svf filter;
    double rate = 48000.0, low = 400.0, high = 1600.0, peak = 10.0;
    float fullTrim = 1.0f, gain = 1.0f, current = -1.0f;
    bool active = false;
};
}
