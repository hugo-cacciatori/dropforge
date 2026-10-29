#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <dsp_core/FixedDelay.h>
#include <dsp_core/LinearRamp.h>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/Svf.h>
#include "OutputLevel.h"

namespace dropforge
{
// Furnace's chain for one side, in 32-bit float, after every playable note is
// summed so chords interact in the drive (SPEC section 4):
//   drive(): Tight high-pass -> 2.8 kHz contour -> 4x asymmetric drive
//   [Engine: stereo-linked 3-band dynamics]
//   tone(): separation low-cut (48 dB/oct) -> low-mid dip -> air shelf
//   -> hash low-pass. gain() then carries the latency-aligned chop gate and
//   velocity restore, which Engine applies after its mid/side stage so no
//   later filter can ring past the chop.
// No 6 kHz low-pass and no 350 Hz high-pass: the top half of the spectrum and
// the chord fundamentals above Anvil's range stay in.
class RhythmBus
{
public:
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples;
    // With Anvil on, Furnace stays out of its range; alone it keeps its body.
    static constexpr double separationHz = 120.0, aloneHz = 40.0, separationGlideSeconds = 0.05;
    static constexpr double contourHz = 2800.0, contourDb = 3.0, contourQ = 0.7;
    static constexpr double lowMidHz = 300.0, lowMidDb = -2.0, lowMidQ = 0.8;
    static constexpr double airHz = 10000.0, airDb = 3.0;
    static constexpr double hashLowpassHz = 18000.0;
    static constexpr float outputGain = static_cast<float>(0.93 * outputTrim);
    static constexpr int controlInterval = 32;

    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
        smoothingSamples = static_cast<int>(std::ceil(rate * 0.010));
        oversampler.prepare();
        contour.set(dsp_core::Svf::Type::bell, contourHz, rate, contourQ, contourDb);
        lowMid.set(dsp_core::Svf::Type::bell, lowMidHz, rate, lowMidQ, lowMidDb);
        air.set(dsp_core::Svf::Type::highShelf, airHz, rate, 0.7071067811865476, airDb);
        hashLowpass.set(dsp_core::Svf::Type::lowpass, std::min(hashLowpassHz, 0.45 * rate), rate);
        currentTight = currentSeparation = -1.0; // recompute for the new rate
        reset();
    }
    void reset() noexcept
    {
        oversampler.reset();
        for (auto* filter : {&tightFilter, &contour, &lowMid, &air, &hashLowpass}) filter->reset();
        for (auto& filter : separation) filter.reset();
        lastGain = 0.0f;
        tight.reset(tightTarget); driveAmount.reset(driveTarget);
        separationLog.reset(std::log2(anvilPresent ? separationHz : aloneHz));
        alignedGate.reset(); alignedLevel.reset();
        countdown = 0;
    }
    // Diagnostic drive amount (0 is exactly linear) and the Tight macro.
    void setParameters(double amount, double tightness) noexcept
    {
        amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
        tightness = std::isfinite(tightness) ? std::clamp(tightness, 0.0, 1.0) : 0.5;
        if (amount != driveTarget) { driveTarget = amount; driveAmount.setTarget(amount, smoothingSamples); }
        if (tightness != tightTarget) { tightTarget = tightness; tight.setTarget(tightness, smoothingSamples); }
    }
    // With Anvil off, the separation cut glides down so Furnace alone has body.
    void setAnvilPresent(bool present) noexcept
    {
        if (present == anvilPresent) return;
        anvilPresent = present;
        separationLog.setTarget(std::log2(present ? separationHz : aloneHz),
                                static_cast<int>(std::ceil(rate * separationGlideSeconds)));
    }

    // The chain in two halves, so Engine can run the stereo-linked multiband
    // dynamics between them (SPEC section 4: drive -> dynamics -> tone).
    float drive(float input, float envelope = 1.0f, float level = 1.0f) noexcept
    {
        if (countdown-- == 0) updateFilters();
        // Fade filter ringing during the final millisecond of the fixed 6 ms
        // release, without extending its duration or clearing live history.
        const auto gate = alignedGate.next(std::clamp(envelope * 6.0f, 0.0f, 1.0f));
        const auto velocity = alignedLevel.next(level);
        lastGain = outputGain * gate * velocity;
        const auto amount = static_cast<float>(driveAmount.next());
        const auto sample = contour.next(tightFilter.next(input));
        return oversampler.next(sample, [amount](float value) noexcept { return shape(value, amount); });
    }
    float tone(float sample) noexcept
    {
        for (auto& filter : separation) sample = filter.next(sample);
        return hashLowpass.next(air.next(lowMid.next(sample)));
    }
    // One side without the dynamics, for tests of the chain itself.
    float next(float input, float envelope = 1.0f, float level = 1.0f) noexcept
    {
        return tone(drive(input, envelope, level));
    }
    // The output gain, chop gate and velocity for the sample next() returned.
    float gain() const noexcept { return lastGain; }

    // The drive curve: an asymmetric soft clip whose small-signal gain is one,
    // so it adds density and harmonics without changing the level of quiet
    // material. Amount 0 is exactly linear (for superposition tests); from
    // 0.25 the full curve applies, its gain rising with amount.
    static float shape(float value, float amount) noexcept
    {
        const auto gain = 1.0f + 7.0f * amount;
        constexpr float bias = 0.2f;
        const auto offset = std::tanh(bias);
        const auto slope = 1.0f - offset * offset; // tanh'(bias)
        const auto curved = (std::tanh(gain * value + bias) - offset) / (gain * slope);
        const auto blend = std::min(1.0f, 4.0f * amount);
        return value + blend * (curved - value);
    }

private:
    // Geometric mapping fixes the specified midpoint at 120 Hz.
    static double cutoff(double value) noexcept
    {
        return value <= 0.5 ? 60.0 * std::exp2(2.0 * value)
                            : 120.0 * std::pow(400.0 / 120.0, 2.0 * value - 1.0);
    }
    // Control rate: moving cutoffs step every 32 samples (0.7 ms or less).
    void updateFilters() noexcept
    {
        countdown = controlInterval - 1;
        const auto tightHz = cutoff(tight.value());
        if (tightHz != currentTight)
        {
            currentTight = tightHz;
            tightFilter.set(dsp_core::Svf::Type::highpass, tightHz, rate);
        }
        const auto separationNow = std::exp2(separationLog.value());
        if (separationNow != currentSeparation)
        {
            currentSeparation = separationNow;
            // Four sections with these Qs make an eighth-order Butterworth
            // (48 dB/oct): chord body from about 110 Hz up stays, while an E2
            // root and the drive's low beat tones fall 20 dB under Anvil.
            for (std::size_t i = 0; i < separation.size(); ++i)
                separation[i].set(dsp_core::Svf::Type::highpass, separationNow, rate, separationQ[i]);
        }
        for (int i = 0; i < controlInterval; ++i) { tight.next(); separationLog.next(); }
    }

    dsp_core::Oversampler4xFloat oversampler;
    static constexpr std::array<double, 4> separationQ{0.5097955, 0.6013449, 0.8999762, 2.5629154};
    dsp_core::Svf tightFilter, contour, lowMid, air, hashLowpass;
    std::array<dsp_core::Svf, 4> separation{};
    dsp_core::LinearRamp tight, driveAmount, separationLog;
    dsp_core::FixedDelay<float, latencySamples> alignedGate, alignedLevel;
    double rate = 44100.0, tightTarget = 0.5, driveTarget = 0.5;
    double currentTight = -1.0, currentSeparation = -1.0;
    float lastGain = 0.0f;
    bool anvilPresent = true;
    int smoothingSamples = 441, countdown = 0;
};
}
