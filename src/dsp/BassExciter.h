#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <dsp_core/Butterworth.h>
#include <dsp_core/SmoothChaos.h>

namespace dropforge
{
// Note-linked saturation-like overtones plus a quiet coloured texture.
// The separate protected sine remains the only clean fundamental source.
class BassExciter
{
public:
    struct Sources { double dry = 0.0, unison = 0.0; };
    struct Parts { Sources body, texture; };
    static constexpr double textureGain = 0.01; // -40 dB before drive, relative to body scale.
    void prepare(double rate) noexcept
    {
        for (auto& filter : bodyBand) filter.prepare(dsp_core::Butterworth::Type::lowpass, 2200.0, rate);
        for (auto& filter : edgeBand) filter.prepare(dsp_core::Butterworth::Type::lowpass, 5200.0, rate);
        reset();
    }
    void reset() noexcept
    {
        body.reset(0.137619); edge.reset(0.731293);
        phase = 0.0;
        for (auto& filter : bodyBand) filter.reset();
        for (auto& filter : edgeBand) filter.reset();
    }
    double next(double hz, double rate, double amount) noexcept { return nextSources(hz, rate, amount).unison; }
    Sources nextSources(double hz, double rate, double amount) noexcept
    {
        const auto parts = nextParts(hz, rate, amount);
        return {parts.body.dry + parts.texture.dry, parts.body.unison + parts.texture.unison};
    }
    // Diagnostic decomposition before drive; no exposed sound control.
    Parts nextParts(double hz, double rate, double amount) noexcept
    {
        const auto root = std::isfinite(hz) ? std::clamp(hz, 1.0, 24000.0) : 55.0;
        constexpr double twoPi = 6.28318530717958647692;
        // A smoothly truncated odd-harmonic residual of a saturated sine,
        // excluding the fundamental. No saw, detuning or extra octave source.
        // Fade partials before Nyquist, without a nonlinear base-rate shaper.
        double overtones = 0.0;
        constexpr std::array<double, 4> weights{0.72, 0.20, 0.06, 0.02};
        for (std::size_t i = 0; i < weights.size(); ++i)
        {
            const auto harmonic = static_cast<double>(2 * i + 3);
            const auto fade = std::clamp((0.45 - harmonic * root / rate) / 0.05, 0.0, 1.0);
            overtones += weights[i] * fade * std::sin(twoPi * harmonic * phase);
        }
        phase += root / rate;
        phase -= std::floor(phase);
        // Root changes bandwidth/roughness gently, never creates a pitched
        // oscillator. Cubic target transitions and two LP sections per stream
        // keep the chaotic texture coloured and remove sharp clock edges.
        const auto speed = std::clamp(1.0 + 0.08 * std::log2(root / 55.0), 0.7, 1.4);
        auto low = body.next(1733.0 * speed, rate);
        auto high = edge.next(5171.0 * speed, rate);
        for (auto& filter : bodyBand) low = filter.next(low);
        for (auto& filter : edgeBand) high = filter.next(high);
        const auto edgeShare = 0.25 + 0.25 * std::clamp(2.0 * amount, 0.0, 1.0);
        const auto dry = 0.20 * textureGain * (0.75 * low + 0.25 * high);
        const auto wet = 0.20 * textureGain * ((1.0 - edgeShare) * low + edgeShare * high);
        // Legacy diagnostic field name retained; this is no longer unison.
        return {{0.20 * overtones, 0.20 * overtones}, {dry, wet}};
    }
private:
    dsp_core::SmoothChaos body, edge;
    double phase = 0.0;
    std::array<dsp_core::Butterworth, 2> bodyBand, edgeBand;
};
}
