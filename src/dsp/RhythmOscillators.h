#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/WavetableBank.h>

namespace dropforge
{
// One played Furnace note: independent left and right groups of seven
// detuned components, each a saw/pulse blend, plus the owner's octave-up
// presence layer synthesized inside the voice (no extra MIDI notes).
//
// All of it reads the shared band-limited saw bank. The 0.7 saw + 0.3 pulse
// blend collapses to saw(phase) - 0.3 x saw(phase - width), so a component
// costs two table reads, and an octave component one. Pitch, pulse width and
// the octave fade are control-rate values set by setPitch().
class RhythmOscillators
{
public:
    struct Stereo { float left = 0.0f, right = 0.0f; };
    static constexpr int components = 7;
    static constexpr double detuneCents = 8.0;
    // Retain the common/mono signal while allowing more of the independent
    // groups' difference through after drive. No delay or new pitch movement.
    static constexpr double spread = 0.84;
    static constexpr double octaveMix = 0.20;

    void reset(int channel, int note) noexcept
    {
        std::uint32_t seed = 0x49524f4eu ^ (static_cast<std::uint32_t>(channel) << 16)
                                            ^ static_cast<std::uint32_t>(note);
        // Fixed integer PRNG, independent of voice slot and callback size.
        const auto nextSeed = [&seed]() noexcept { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        std::uint32_t anchor = 0;
        for (std::size_t side = 0; side < 2; ++side)
            for (std::size_t component = 0; component < 3; ++component)
            {
                auto phase = nextSeed();
                if (side == 0 && component == 1) anchor = phase;
                // Exact-pitch anchors must not land almost in phase or opposed
                // by chance. A quarter cycle retains independent synthesis and
                // a stable common/side balance, without delaying either bus.
                if (side == 1 && component == 1) phase = anchor + 0x40000000u;
                main[side][component].reset(phase);
            }
        // Quieter outer shoulders add beating without new intervals, a delayed
        // duplicate, or modulation of the played pitch anchor.
        for (auto& side : main)
            for (std::size_t component = 3; component < components; ++component)
                side[component].reset(nextSeed());
        motionPhase[0] = static_cast<double>(seed) / 4294967296.0;
        motionPhase[1] = motionPhase[0] + 0.25;
        if (motionPhase[1] >= 1.0) motionPhase[1] -= 1.0;
        for (std::size_t side = 0; side < 2; ++side)
        {
            const auto cents = detuneCents * (side == 0 ? 1.0 : 0.77);
            const auto scale = side == 0 ? 1.0 : 0.77;
            ratios[side] = {std::exp2(-cents / 1200.0), 1.0, std::exp2(cents / 1200.0),
                            std::exp2(-14.0 * scale / 1200.0), std::exp2(14.0 * scale / 1200.0),
                            std::exp2(-22.0 * scale / 1200.0), std::exp2(22.0 * scale / 1200.0)};
        }
        for (std::size_t side = 0; side < 2; ++side)
            for (std::size_t component = 0; component < 3; ++component)
            {
                auto phase = nextSeed();
                if (side == 0 && component == 1) anchor = phase;
                if (side == 1 && component == 1) phase = anchor + 0x40000000u;
                octave[side][component].reset(phase);
            }
        // Spread the detuned half of the octave layer over six voices for more
        // beating without raising its level.
        for (auto& side : octave)
            for (std::size_t component = 3; component < components; ++component)
                side[component].reset(nextSeed());
    }

    // Control rate: pitch for every component, the slow pulse-width motion
    // advanced by `samples`, and the octave layer's fade near the top.
    void setPitch(double frequency, double rate, int samples) noexcept
    {
        const auto& bank = dsp_core::WavetableBank::saw();
        for (std::size_t side = 0; side < 2; ++side)
        {
            // Gentle, independent harmonic motion supplies dry ensemble colour;
            // it never moves the exact-pitch targets or lengthens the envelope.
            pulseOffset[side] = dsp_core::WavetableOscillator::phaseOffset(
                0.5 + 0.14 * std::sin(6.28318530717958647692 * motionPhase[side]));
            motionPhase[side] += (side == 0 ? 0.23 : 0.31) * samples / rate;
            motionPhase[side] -= std::floor(motionPhase[side]);
            for (std::size_t component = 0; component < components; ++component)
            {
                const auto hz = frequency * ratios[side][component];
                main[side][component].setFrequency(bank, hz, rate);
                octave[side][component].setFrequency(bank, 2.0 * hz, rate);
                // Smoothly remove the added layer before the band limit could
                // fold it into an unrelated tone at the keyboard's top.
                const auto fade = std::clamp((0.45 - 2.0 * hz / rate) / 0.15, 0.0, 1.0);
                octaveGain[side][component] = static_cast<float>(octaveMix * octaveWeights[component]
                                                                 * fade * fade * (3.0 - 2.0 * fade));
            }
        }
    }

    // Diagnostic isolation only: choose octave presence once before a phrase.
    Stereo next(bool octavePresence = true) noexcept
    {
        std::array<float, 2> sum{};
        for (std::size_t side = 0; side < 2; ++side)
        {
            for (std::size_t component = 0; component < components; ++component)
                sum[side] += weights[component] * main[side][component].nextBlend(pulseOffset[side], 0.3f);
            if (octavePresence)
                for (std::size_t component = 0; component < components; ++component)
                    sum[side] += octaveGain[side][component] * octave[side][component].next();
            else
                for (auto& oscillator : octave[side]) oscillator.next();
        }
        // Keep the independent groups separate through summed-bus drive.
        // Engine applies Spread after filtering so drive cannot narrow it.
        return {sum[0], sum[1]};
    }

private:
    // Unity weight sum keeps the ensemble mix fixed; the anchor retains its
    // original weight while energy is distributed over six detuned components.
    static constexpr std::array<float, components> weights{1.0f/4.0f, 1.0f/3.0f, 1.0f/4.0f,
        1.0f/24.0f, 1.0f/24.0f, 1.0f/24.0f, 1.0f/24.0f};
    static constexpr std::array<double, components> octaveWeights{1.0/12.0, 0.5, 1.0/12.0,
        1.0/12.0, 1.0/12.0, 1.0/12.0, 1.0/12.0};
    std::array<std::array<dsp_core::WavetableOscillator, components>, 2> main{}, octave{};
    std::array<std::array<double, components>, 2> ratios{};
    std::array<std::array<float, components>, 2> octaveGain{};
    std::array<double, 2> motionPhase{};
    std::array<std::uint32_t, 2> pulseOffset{0x80000000u, 0x80000000u};
};
}
