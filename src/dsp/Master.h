#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <dsp_core/GlueCompressor.h>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/PeakLimiter.h>

namespace dropforge
{
// The master after the layer buses (SPEC section 4): a fixed trim that sets
// the mix-ready loudness, a gentle glue compressor, a 4x oversampled soft
// clipper that rounds off stab transients, and a look-ahead limiter that
// holds every sample peak at or under -1 dBFS. The FX page's Saturate runs
// inside the clipper's 4x stage, before the clip: tape-like warmth on the
// whole mix with no added latency. Output and meters follow in
// InstrumentCore. At default settings only peaks are touched.
class Master
{
public:
    static constexpr float ceiling = 0.89125093813374556f; // -1 dBFS
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples
                                        + dsp_core::PeakLimiter::latencySamples;
    // A sustained 4-note chord at velocity 127 with defaults reaches about
    // -16 LUFS short-term (owner, 2026-10-24: mix-ready level). Measured
    // with Furnace A, grit A and the heavy Anvil weight: -1.25 dB.
    static constexpr float trim = 0.86596432336006535f;

    struct Meters { float glueReductionDb = 0.0f, limiterReductionDb = 0.0f; };

    void prepare(double sampleRate) noexcept
    {
        // Threshold -12 dBFS, 2:1, 6 dB knee, 10 ms / 150 ms: a dB or two on
        // the dense default wall, more only when Levels push it.
        glue.prepare({-12.0f, 2.0f, 6.0f, 0.010, 0.150}, sampleRate);
        for (auto& stage : clipper) stage.prepare();
        limiter.prepare(ceiling, sampleRate);
        fadeStep = static_cast<float>(1.0 / std::ceil(sampleRate * 0.030));
        smoothing = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
        reset();
    }
    void reset() noexcept
    {
        glue.reset();
        for (auto& stage : clipper) stage.reset();
        limiter.reset();
        meters = {};
        saturateLevel = saturateOn ? saturateMix : 0.0f;
        saturateGain = saturateTarget;
        saturateBalance = balanceFor(saturateGain);
    }
    // Saturate (SPEC section 8): drive 0-24 dB, mix 0-1; switching on or
    // off fades the mix over 30 ms.
    void setSaturate(bool on, float driveDb, float mix) noexcept
    {
        saturateOn = on;
        saturateMix = std::isfinite(mix) ? std::clamp(mix, 0.0f, 1.0f) : 1.0f;
        saturateTarget = std::pow(10.0f, (std::isfinite(driveDb) ? std::clamp(driveDb, 0.0f, 24.0f) : 6.0f) / 20.0f);
    }
    void process(float* left, float* right, int samples) noexcept
    {
        meters = {};
        for (int i = 0; i < samples; ++i)
        {
            auto l = trim * left[i], r = trim * right[i];
            glue.process(l, r);
            meters.glueReductionDb = std::min(meters.glueReductionDb, glue.gainReductionDb());
            const auto mixTarget = saturateOn ? saturateMix : 0.0f;
            if (saturateLevel != mixTarget || saturateLevel > 0.0f)
            {
                saturateLevel = mixTarget > saturateLevel ? std::min(mixTarget, saturateLevel + fadeStep)
                                                          : std::max(mixTarget, saturateLevel - fadeStep);
                if (saturateGain != saturateTarget)
                {
                    saturateGain += smoothing * (saturateTarget - saturateGain);
                    if (std::abs(saturateGain - saturateTarget) < 1e-5f) saturateGain = saturateTarget;
                    saturateBalance = balanceFor(saturateGain);
                }
                const auto shape = [mix = saturateLevel, gain = saturateGain, balance = saturateBalance](float x) noexcept
                {
                    return softClip(x + mix * (balance * (std::tanh(gain * x + saturateBias) - biasOffset) - x));
                };
                l = clipper[0].next(l, shape);
                r = clipper[1].next(r, shape);
            }
            else
            {
                l = clipper[0].next(l, softClip);
                r = clipper[1].next(r, softClip);
            }
            limiter.process(l, r);
            meters.limiterReductionDb = std::min(meters.limiterReductionDb, limiter.gainReductionDb());
            left[i] = l;
            right[i] = r;
        }
    }
    // The deepest gain reduction in the last processed block.
    const Meters& lastMeters() const noexcept { return meters; }

private:
    // Linear up to -3 dBFS, then a tanh curve that approaches the ceiling:
    // the slope stays 1 at the knee, so quiet material passes untouched and
    // loud transients are rounded instead of being left to the limiter.
    static float softClip(float x) noexcept
    {
        constexpr float knee = 0.70794578438413791f; // -3 dBFS
        constexpr float room = ceiling - knee;
        const auto magnitude = std::abs(x);
        if (magnitude <= knee) return x;
        const auto shaped = knee + room * std::tanh((magnitude - knee) / room);
        return x < 0.0f ? -shaped : shaped;
    }

    // Saturate's curve: a slightly biased tanh (the bias adds the even
    // harmonics of tape and tubes), rescaled so a -18 dBFS signal (the
    // mix's typical level) comes out at -18 dBFS whatever the drive: more
    // drive thickens and compresses the mix instead of making it louder.
    static constexpr float saturateBias = 0.1f, saturateReference = 0.125f;
    static inline const float biasOffset = std::tanh(saturateBias);
    static float balanceFor(float gain) noexcept
    {
        return saturateReference / (std::tanh(gain * saturateReference + saturateBias) - biasOffset);
    }
    bool saturateOn = false;
    float saturateMix = 1.0f, saturateTarget = 2.0f, saturateGain = 2.0f, saturateBalance = 1.0f, saturateLevel = 0.0f;
    float fadeStep = 0.001f, smoothing = 0.004f;
    dsp_core::GlueCompressor glue;
    std::array<dsp_core::Oversampler4xFloat, 2> clipper;
    dsp_core::PeakLimiter limiter;
    Meters meters;
};
}
