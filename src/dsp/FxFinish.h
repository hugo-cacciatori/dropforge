#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>
#include <dsp_core/Svf.h>

namespace dropforge
{
// The FX page's finishing effects on the whole mix (SPEC section 8), in
// order Crush -> Chorus -> EQ. Each switches with a 30 ms crossfade between
// its input and its output, and costs nothing once off and faded. Saturate,
// the fourth, lives inside the master's oversampled stage (Master.h) so it
// adds no latency.
class FxFinish
{
public:
    struct Settings
    {
        bool crushOn = false;
        float crushBits = 8.0f, crushRateHz = 8000.0f, crushToneHz = 8000.0f, crushMix = 0.5f;
        bool chorusOn = false;
        float chorusRateHz = 0.6f, chorusDepth = 0.5f, chorusMix = 0.4f;
        bool eqOn = false;
        float eqLowCutHz = 20.0f, eqHighCutHz = 20000.0f, eqTiltDb = 0.0f;
    };

    // Not real-time: allocates the chorus line.
    void prepare(double sampleRate)
    {
        rate = sampleRate;
        for (auto& line : chorusLine) line.assign(static_cast<std::size_t>(std::ceil(0.05 * rate)) + 4, 0.0f);
        fadeStep = static_cast<float>(1.0 / std::ceil(rate * 0.030));
        smoothing = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * rate)));
        for (auto& filter : chorusLowCut) filter.set(dsp_core::Svf::Type::highpass, monoBelowHz, rate);
        for (auto& filter : chorusLowCut2) filter.set(dsp_core::Svf::Type::highpass, monoBelowHz, rate);
        tuned = false;
        set(settings);
        reset();
    }
    void reset() noexcept
    {
        crushLevel = settings.crushOn ? 1.0f : 0.0f;
        chorusLevel = settings.chorusOn ? 1.0f : 0.0f;
        eqLevel = settings.eqOn ? 1.0f : 0.0f;
        held = {};
        holdPhase = 1.0;
        for (auto& line : chorusLine) std::fill(line.begin(), line.end(), 0.0f);
        chorusAt = 0;
        chorusPhase = 0.0;
        for (auto* filter : {&crushTone[0], &crushTone[1], &chorusLowCut[0], &chorusLowCut[1], &chorusLowCut2[0], &chorusLowCut2[1], &eqLow[0], &eqLow[1], &eqHigh[0], &eqHigh[1],
                             &eqTilt[0], &eqTilt[1]})
            filter->reset();
        bits = settings.crushBits;
        depth = settings.chorusDepth;
    }
    void set(const Settings& chosen) noexcept
    {
        if (!tuned || chosen.crushToneHz != settings.crushToneHz)
            for (auto& filter : crushTone) filter.set(dsp_core::Svf::Type::lowpass, chosen.crushToneHz, rate);
        if (!tuned || chosen.eqLowCutHz != settings.eqLowCutHz || chosen.eqHighCutHz != settings.eqHighCutHz || chosen.eqTiltDb != settings.eqTiltDb)
        {
            for (int side = 0; side < 2; ++side)
            {
                eqLow[side].set(dsp_core::Svf::Type::highpass, chosen.eqLowCutHz, rate);
                eqHigh[side].set(dsp_core::Svf::Type::lowpass, chosen.eqHighCutHz, rate);
                // Tilt around 800 Hz: a high shelf of the full tilt, and the
                // whole curve lowered by half of it, so lows and highs move
                // in opposite directions by half the tilt each.
                eqTilt[side].set(dsp_core::Svf::Type::highShelf, 800.0, rate, 0.5, chosen.eqTiltDb);
            }
            tiltTrim = static_cast<float>(std::pow(10.0, -static_cast<double>(chosen.eqTiltDb) / 40.0));
        }
        tuned = true;
        settings = chosen;
    }
    // On, or still fading out.
    bool active() const noexcept
    {
        return settings.crushOn || settings.chorusOn || settings.eqOn || crushLevel > 0.0f || chorusLevel > 0.0f || eqLevel > 0.0f;
    }
    void process(float& left, float& right) noexcept
    {
        const auto fade = [this](float& level, bool on) noexcept
        {
            level = on ? std::min(1.0f, level + fadeStep) : std::max(0.0f, level - fadeStep);
        };
        fade(crushLevel, settings.crushOn);
        fade(chorusLevel, settings.chorusOn);
        fade(eqLevel, settings.eqOn);
        // While any finishing effect is active, the chorus line and the EQ
        // filters keep running even when their own effect is off, so
        // switching one on blends in a settled sound instead of a filter
        // ringing up from silence.
        if (crushLevel > 0.0f) crush(left, right);
        chorus(left, right);
        equalise(left, right);
    }

private:
    static constexpr double monoBelowHz = 150.0;
    // Crush: a sample-and-hold at the chosen rate (its aliasing is the
    // effect) and a quantiser of the chosen depth, then a low-pass Tone that
    // keeps the grit musical. Fractional bits move smoothly.
    void crush(float& left, float& right) noexcept
    {
        bits += smoothing * (settings.crushBits - bits);
        holdPhase += std::min(static_cast<double>(settings.crushRateHz), rate) / rate;
        if (holdPhase >= 1.0)
        {
            holdPhase -= std::floor(holdPhase);
            const auto step = std::exp2(1.0f - std::clamp(bits, 2.0f, 16.0f));
            held[0] = step * std::round(left / step);
            held[1] = step * std::round(right / step);
        }
        const auto wet = settings.crushMix * crushLevel;
        left += wet * (crushTone[0].next(held[0]) - left);
        right += wet * (crushTone[1].next(held[1]) - right);
    }
    // Chorus: three delayed copies (about 12 ms, swinging up to +-4 ms at
    // the Rate, a third of a cycle apart) panned left, centre and right. Only
    // the part above 150 Hz is chorused, so the low end stays mono and tight.
    void chorus(float& left, float& right) noexcept
    {
        depth += smoothing * (settings.chorusDepth - depth);
        const auto size = chorusLine[0].size();
        // 24 dB/oct, so even a 70 Hz root stays well under the wide part.
        chorusLine[0][chorusAt] = chorusLowCut2[0].next(chorusLowCut[0].next(left));
        chorusLine[1][chorusAt] = chorusLowCut2[1].next(chorusLowCut[1].next(right));
        chorusPhase += static_cast<double>(std::clamp(settings.chorusRateHz, 0.05f, 5.0f)) / rate;
        if (chorusPhase >= 1.0) chorusPhase -= 1.0;
        std::array<float, 3> voice{};
        for (std::size_t v = 0; v < 3; ++v)
        {
            const auto swing = std::sin(6.283185307179586 * (chorusPhase + static_cast<double>(v) / 3.0));
            const auto delay = (0.012 + 0.004 * static_cast<double>(depth) * swing) * rate;
            const auto whole = static_cast<std::size_t>(delay);
            const auto fraction = static_cast<float>(delay - static_cast<double>(whole));
            const auto side = v == 2 ? 1u : 0u; // left, centre (left line), right
            const auto at = (chorusAt + size - whole) % size, before = (at + size - 1) % size;
            voice[v] = (1.0f - fraction) * chorusLine[side][at] + fraction * chorusLine[side][before];
        }
        chorusAt = chorusAt + 1 == size ? 0 : chorusAt + 1;
        if (chorusLevel == 0.0f) return;
        const auto wet = 0.7f * settings.chorusMix * chorusLevel;
        left += wet * (voice[0] + 0.5f * voice[1]);
        right += wet * (voice[2] + 0.5f * voice[1]);
    }
    void equalise(float& left, float& right) noexcept
    {
        const auto shapedLeft = tiltTrim * eqTilt[0].next(eqHigh[0].next(eqLow[0].next(left)));
        const auto shapedRight = tiltTrim * eqTilt[1].next(eqHigh[1].next(eqLow[1].next(right)));
        if (eqLevel == 0.0f) return;
        left += eqLevel * (shapedLeft - left);
        right += eqLevel * (shapedRight - right);
    }

    Settings settings{};
    double rate = 48000.0, holdPhase = 1.0, chorusPhase = 0.0;
    std::array<float, 2> held{};
    std::array<std::vector<float>, 2> chorusLine;
    std::size_t chorusAt = 0;
    dsp_core::Svf crushTone[2], chorusLowCut[2], chorusLowCut2[2], eqLow[2], eqHigh[2], eqTilt[2];
    float crushLevel = 0.0f, chorusLevel = 0.0f, eqLevel = 0.0f, fadeStep = 0.001f, smoothing = 0.004f;
    float bits = 8.0f, depth = 0.5f, tiltTrim = 1.0f;
    bool tuned = false;
};
}
