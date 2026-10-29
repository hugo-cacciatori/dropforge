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
// holds every sample peak at or under -1 dBFS. Output and meters follow in
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
        reset();
    }
    void reset() noexcept
    {
        glue.reset();
        for (auto& stage : clipper) stage.reset();
        limiter.reset();
        meters = {};
    }
    void process(float* left, float* right, int samples) noexcept
    {
        meters = {};
        for (int i = 0; i < samples; ++i)
        {
            auto l = trim * left[i], r = trim * right[i];
            glue.process(l, r);
            meters.glueReductionDb = std::min(meters.glueReductionDb, glue.gainReductionDb());
            l = clipper[0].next(l, softClip);
            r = clipper[1].next(r, softClip);
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

    dsp_core::GlueCompressor glue;
    std::array<dsp_core::Oversampler4xFloat, 2> clipper;
    dsp_core::PeakLimiter limiter;
    Meters meters;
};
}
