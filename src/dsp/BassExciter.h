#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/SmoothChaos.h>
#include <dsp_core/Svf.h>
#include <dsp_core/WavetableBank.h>

namespace dropforge
{
// Note-linked saturation-like overtones plus a quiet coloured texture.
// The separate protected sine remains the only clean fundamental source.
class BassExciter
{
public:
    struct Sources { float dry = 0.0f, unison = 0.0f; };
    struct Parts { Sources body, texture; };
    static constexpr float textureGain = 0.01f; // -40 dB before drive, relative to body scale.
    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
        for (auto& filter : bodyBand) filter.set(dsp_core::Svf::Type::lowpass, 2200.0, rate);
        for (auto& filter : edgeBand) filter.set(dsp_core::Svf::Type::lowpass, 5200.0, rate);
        dsp_core::sine(0); // build the shared table off the audio thread
        reset();
    }
    void reset() noexcept
    {
        body.reset(0.137619); edge.reset(0.731293);
        phase = 0;
        for (auto& filter : bodyBand) filter.reset();
        for (auto& filter : edgeBand) filter.reset();
    }
    float next(double hz, double sampleRate, double amount) noexcept { return nextSources(hz, sampleRate, amount).unison; }
    Sources nextSources(double hz, double sampleRate, double amount) noexcept
    {
        const auto parts = nextParts(hz, sampleRate, amount);
        return {parts.body.dry + parts.texture.dry, parts.body.unison + parts.texture.unison};
    }
    // Diagnostic decomposition before drive; no exposed sound control.
    Parts nextParts(double hz, double sampleRate, double amount) noexcept
    {
        const auto root = std::isfinite(hz) ? std::clamp(hz, 1.0, 24000.0) : 55.0;
        // A smoothly truncated odd-harmonic residual of a saturated sine,
        // excluding the fundamental. No saw, detuning or extra octave source.
        // Fade partials before Nyquist, without a nonlinear base-rate shaper.
        // One integer phase: harmonic k reads the sine at k x phase, so the
        // overtones stay exactly locked to the clean fundamental.
        // Everything that depends only on the pitch is recomputed only when the
        // pitch moves (a held note keeps it for thousands of samples).
        if (root != cachedRoot || sampleRate != cachedRate) updatePitch(root, sampleRate);
        const auto* table = dsp_core::sineTable();
        float overtones = 0.0f;
        for (std::size_t i = 0; i < weights.size(); ++i)
            overtones += weights[i] * fades[i] * dsp_core::sine(table, static_cast<std::uint32_t>(2 * i + 3) * phase);
        phase += increment;
        const auto speed = cachedSpeed;
        auto low = static_cast<float>(body.next(1733.0 * speed, sampleRate));
        auto high = static_cast<float>(edge.next(5171.0 * speed, sampleRate));
        for (auto& filter : bodyBand) low = filter.next(low);
        for (auto& filter : edgeBand) high = filter.next(high);
        const auto edgeShare = static_cast<float>(0.25 + 0.25 * std::clamp(2.0 * amount, 0.0, 1.0));
        const auto dry = 0.20f * textureGain * (0.75f * low + 0.25f * high);
        const auto wet = 0.20f * textureGain * ((1.0f - edgeShare) * low + edgeShare * high);
        // Legacy diagnostic field name retained; this is no longer unison.
        return {{0.20f * overtones, 0.20f * overtones}, {dry, wet}};
    }
private:
    static constexpr std::array<float, 4> weights{0.72f, 0.20f, 0.06f, 0.02f};
    void updatePitch(double root, double sampleRate) noexcept
    {
        cachedRoot = root; cachedRate = sampleRate;
        for (std::size_t i = 0; i < weights.size(); ++i)
        {
            const auto harmonic = static_cast<std::uint32_t>(2 * i + 3);
            fades[i] = static_cast<float>(std::clamp((0.45 - harmonic * root / sampleRate) / 0.05, 0.0, 1.0));
        }
        increment = static_cast<std::uint32_t>(std::llround(root / sampleRate * 4294967296.0));
        // Root changes bandwidth/roughness gently, never creates a pitched
        // oscillator. Cubic target transitions and two LP sections per stream
        // keep the chaotic texture coloured and remove sharp clock edges.
        cachedSpeed = std::clamp(1.0 + 0.08 * std::log2(root / 55.0), 0.7, 1.4);
    }
    double cachedRoot = -1.0, cachedRate = -1.0, cachedSpeed = 1.0;
    std::array<float, 4> fades{};
    std::uint32_t increment = 0;
    dsp_core::SmoothChaos body, edge;
    std::uint32_t phase = 0;
    double rate = 48000.0;
    std::array<dsp_core::Svf, 2> bodyBand, edgeBand;
};
}
