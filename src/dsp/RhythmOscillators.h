#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/PolyBlepSaw.h>
#include <dsp_core/PolyBlepPulse.h>

namespace dropforge
{
// Components belong to one played note: the owner's octave-up presence layer
// is synthesized inside the voice, without allocating additional MIDI notes.
class RhythmOscillators
{
public:
    struct Stereo { double left = 0.0, right = 0.0; };
    static constexpr double detuneCents = 8.0;
    // Retain the common/mono signal while allowing more of the independent
    // groups' difference through after drive. No delay or new pitch movement.
    static constexpr double spread = 0.84;
    static constexpr double octaveMix = 0.20;
    void reset(int channel, int note) noexcept
    {
        std::uint32_t seed = 0x49524f4eu ^ (static_cast<std::uint32_t>(channel) << 16)
                                            ^ static_cast<std::uint32_t>(note);
        double anchorPhase = 0.0;
        for (std::size_t side = 0; side < oscillators.size(); ++side)
            for (std::size_t component = 0; component < 3; ++component)
            {
                // Fixed integer PRNG, independent of voice slot and callback size.
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                auto phase = static_cast<double>(seed) / 4294967296.0;
                if (side == 0 && component == 1) anchorPhase = phase;
                // Exact-pitch anchors must not land almost in phase or opposed
                // by chance. A quarter cycle retains independent synthesis and
                // a stable common/side balance, without delaying either bus.
                if (side == 1 && component == 1) phase = anchorPhase + 0.25;
                oscillators[side][component].reset(phase);
                pulses[side][component].reset(phase);
            }
        // Preserve the original anchor/inner-pair phases for a controlled
        // audition. Quieter outer shoulders add beating without new intervals,
        // a delayed duplicate, or modulation of the played pitch anchor.
        for (std::size_t side = 0; side < oscillators.size(); ++side)
            for (std::size_t component = 3; component < oscillators[side].size(); ++component)
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                const auto phase = static_cast<double>(seed) / 4294967296.0;
                oscillators[side][component].reset(phase);
                pulses[side][component].reset(phase);
            }
        motionPhase[0] = static_cast<double>(seed) / 4294967296.0;
        motionPhase[1] = motionPhase[0] + 0.25;
        if (motionPhase[1] >= 1.0) motionPhase[1] -= 1.0;
        for (std::size_t side = 0; side < ratios.size(); ++side)
        {
            const auto cents = detuneCents * (side == 0 ? 1.0 : 0.77);
            const auto scale = side == 0 ? 1.0 : 0.77;
            ratios[side] = {std::exp2(-cents / 1200.0), 1.0, std::exp2(cents / 1200.0),
                           std::exp2(-14.0 * scale / 1200.0), std::exp2(14.0 * scale / 1200.0),
                           std::exp2(-22.0 * scale / 1200.0), std::exp2(22.0 * scale / 1200.0)};
        }
        // Seed after the existing groups/motion so their phase relationship
        // stays identical for a controlled octave-layer comparison.
        double octaveAnchor = 0.0;
        for (std::size_t side = 0; side < octave.size(); ++side)
            for (std::size_t component = 0; component < 3; ++component)
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                auto phase = static_cast<double>(seed) / 4294967296.0;
                if (side == 0 && component == 1) octaveAnchor = phase;
                if (side == 1 && component == 1) phase = octaveAnchor + 0.25;
                octave[side][component].reset(phase);
            }
        // Keep the existing inner-pair/anchor phases. Spread the detuned half
        // over six voices for more beating without raising the octave layer.
        for (auto& side : octave)
            for (std::size_t component = 3; component < side.size(); ++component)
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                side[component].reset(static_cast<double>(seed) / 4294967296.0);
            }
    }
    // Diagnostic isolation only: choose octave presence once before a phrase.
    Stereo next(double frequency, double rate, bool octavePresence = true) noexcept
    {
        std::array<double, 2> sum{};
        for (std::size_t side = 0; side < oscillators.size(); ++side)
        {
            // Gentle, independent harmonic motion supplies dry ensemble colour;
            // it never moves the exact-pitch targets or lengthens the envelope.
            const auto width = 0.5 + 0.14 * std::sin(6.28318530717958647692 * motionPhase[side]);
            motionPhase[side] += (side == 0 ? 0.23 : 0.31) / rate;
            if (motionPhase[side] >= 1.0) motionPhase[side] -= 1.0;
            for (std::size_t component = 0; component < oscillators[side].size(); ++component)
                sum[side] += weights[component]
                    * (0.7 * oscillators[side][component].next(frequency * ratios[side][component], rate)
                       + 0.3 * pulses[side][component].next(frequency * ratios[side][component], rate, width));
            constexpr std::array<double, 7> octaveWeights{1.0/12.0, 0.5, 1.0/12.0,
                1.0/12.0, 1.0/12.0, 1.0/12.0, 1.0/12.0};
            for (std::size_t component = 0; component < octave[side].size(); ++component)
            {
                const auto hz = 2.0 * frequency * ratios[side][component];
                // Smoothly remove the added layer before oscillator clamping
                // could introduce an unrelated tone at the keyboard's top.
                const auto fade = std::clamp((0.45 - hz / rate) / 0.15, 0.0, 1.0);
                const auto weight = fade * fade * (3.0 - 2.0 * fade);
                const auto value = octave[side][component].next(hz, rate);
                if (octavePresence) sum[side] += octaveMix * octaveWeights[component] * weight * value;
            }
        }
        // Keep the independent groups separate through summed-bus drive.
        // Engine applies Spread after filtering so drive cannot narrow it.
        return {sum[0], sum[1]};
    }
private:
    // Unity weight sum keeps the ensemble mix fixed; the anchor retains its
    // original weight while energy is distributed over six detuned components.
    static constexpr std::array<double, 7> weights{1.0/4.0, 1.0/3.0, 1.0/4.0,
                                                1.0/24.0, 1.0/24.0, 1.0/24.0, 1.0/24.0};
    std::array<std::array<dsp_core::PolyBlepSaw, 7>, 2> oscillators{};
    std::array<std::array<dsp_core::PolyBlepPulse, 7>, 2> pulses{};
    std::array<std::array<dsp_core::PolyBlepSaw, 7>, 2> octave{};
    std::array<std::array<double, 7>, 2> ratios{};
    std::array<double, 2> motionPhase{};
};
}
