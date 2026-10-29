#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// A gentle stereo-linked bus compressor: the "glue" that makes layers move
// together. Feed-forward on the louder channel's peak, attack and release
// on the level, a soft knee and a modest ratio, so it only leans on dense
// passages by a dB or two. Gain is a slow multiplier, so no oversampling.
class GlueCompressor
{
public:
    struct Settings
    {
        float thresholdDb = -10.0f, ratio = 2.0f, kneeDb = 6.0f;
        double attackSeconds = 0.010, releaseSeconds = 0.150;
    };

    void prepare(const Settings& chosen, double sampleRate) noexcept
    {
        settings = chosen;
        const auto rate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
        attack = static_cast<float>(1.0 - std::exp(-1.0 / (settings.attackSeconds * rate)));
        release = static_cast<float>(1.0 - std::exp(-1.0 / (settings.releaseSeconds * rate)));
        slope = 1.0f / std::max(settings.ratio, 1.0f) - 1.0f;
        reset();
    }
    void reset() noexcept { level = 0.0f; reductionDb = 0.0f; }

    // Processes one stereo frame in place.
    void process(float& left, float& right) noexcept
    {
        const auto peak = std::max(std::abs(left), std::abs(right));
        level += (peak > level ? attack : release) * (peak - level);
        if (level < 1.0e-20f) level = 0.0f; // no denormals in a silent tail
        reductionDb = gainDb(level);
        if (reductionDb < 0.0f)
        {
            const auto gain = std::pow(10.0f, 0.05f * reductionDb);
            left *= gain;
            right *= gain;
        }
    }
    // The current gain change in dB (0 or negative), for meters and tests.
    float gainReductionDb() const noexcept { return reductionDb; }

private:
    // Soft-knee gain computer, in dB.
    float gainDb(float amplitude) const noexcept
    {
        if (amplitude <= 0.0f) return 0.0f;
        const auto over = 20.0f * std::log10(amplitude) - settings.thresholdDb;
        const auto half = 0.5f * settings.kneeDb;
        if (over <= -half) return 0.0f;
        if (over < half) return slope * (over + half) * (over + half) / (2.0f * settings.kneeDb);
        return slope * over;
    }
    Settings settings{};
    float attack = 0.0f, release = 0.0f, slope = -0.5f, level = 0.0f, reductionDb = 0.0f;
};
}
