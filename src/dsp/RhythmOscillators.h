#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/WavetableBank.h>

namespace dropforge
{
// One played Furnace note (SPEC section 4): per side a supersaw stack, an
// octave-up stack for presence, and a square body for mid weight, all read
// from the shared band-limited saw bank. Left and right stacks have their own
// phases and slightly different detune, so the width comes from synthesis.
// Pitch, detune and the octave fade are control-rate values set by setPitch().
// Mode D (SPEC section 12) swaps the main saws for 2-operator FM stacks.
class RhythmOscillators
{
public:
    struct Stereo { float left = 0.0f, right = 0.0f; };
    struct Design
    {
        double spreadCents = 30.0;   // outermost main saw, before the side scaling
        double octaveDb = -8.0;      // octave stack against the main stack
        double octaveSpreadCents = 12.0;
        double bodyDb = -12.0;       // square body against the main stack
        // FM main stacks (mode D) when either index is above zero: each main
        // oscillator becomes a sine carrier phase-modulated by itself (1:1)
        // and its octave (2:1). Integer ratios keep every partial harmonic.
        double fmIndex1 = 0.0, fmIndex2 = 0.0;
        bool fm() const noexcept { return fmIndex1 > 0.0 || fmIndex2 > 0.0; }
    };
    static constexpr int mainCount = 7, octaveCount = 5;
    // Final post-drive side blend (applied by Engine after all nonlinear stages).
    static constexpr double spread = 0.84;

    void setDesign(const Design& next) noexcept { design = next; }

    void reset(int channel, int note) noexcept
    {
        // Phases depend on the key only, never on the voice slot or block size.
        const auto key = [channel, note](std::uint32_t layer, std::uint32_t index) noexcept {
            return dsp_core::seededPhase(static_cast<std::uint32_t>(channel), static_cast<std::uint32_t>(note), layer, index);
        };
        for (std::uint32_t side = 0; side < 2; ++side)
        {
            for (std::uint32_t i = 0; i < mainCount; ++i)
            {
                main[side][i].reset(key(side, i));
                fmPhase[side][i] = key(side, i);
            }
            for (std::uint32_t i = 0; i < octaveCount; ++i) octave[side][i].reset(key(2 + side, i));
        }
        // One body for both sides keeps the mid weight centred and mono-solid.
        body.reset(key(4, 0));
    }

    // Control rate: pitch for every oscillator and the octave stack's fade.
    void setPitch(double frequency, double rate) noexcept
    {
        const auto& bank = dsp_core::WavetableBank::saw();
        for (std::size_t side = 0; side < 2; ++side)
        {
            // The right stack is mirrored and a little narrower, so the two
            // sides never beat in step: a wide, moving image without delay.
            const auto scale = side == 0 ? 1.0 : -0.85;
            for (std::size_t i = 0; i < mainCount; ++i)
            {
                const auto hz = frequency * cents(design.spreadCents * scale * mainOffsets[i]);
                main[side][i].setFrequency(bank, hz, rate);
                fmIncrement[side][i] = static_cast<std::uint32_t>(std::llround(std::clamp(hz / rate, 0.0, 0.5) * 4294967296.0));
            }
            for (std::size_t i = 0; i < octaveCount; ++i)
                octave[side][i].setFrequency(bank, 2.0 * frequency
                    * cents(design.octaveSpreadCents * scale * octaveOffsets[i]), rate);
        }
        body.setFrequency(bank, frequency, rate);
        // Fade the octave stack out before the band limit near the top key.
        const auto fade = std::clamp((0.45 - 2.0 * frequency / rate) / 0.15, 0.0, 1.0);
        octaveGain = static_cast<float>(std::pow(10.0, design.octaveDb / 20.0) / octaveCount
                                        * fade * fade * (3.0 - 2.0 * fade));
        bodyGain = static_cast<float>(std::pow(10.0, design.bodyDb / 20.0));
        if (design.fm())
        {
            // The FM partials of sin(p + a sin p + b sin 2p) fall below -55 dB
            // past about a + 2b + 6 harmonics; scale both indices down so that
            // many harmonics stay under 0.45 x rate. Low notes keep full depth.
            const auto room = 0.45 * rate / std::max(frequency, 1.0) - 6.0;
            const auto depth = design.fmIndex1 + 2.0 * design.fmIndex2;
            const auto scale = std::clamp(room / depth, 0.0, 1.0);
            // Indices in table cycles: one radian is 2^32 / (2 pi) phase units.
            // The right side runs at 75 % depth: its overtones differ from the
            // left's, which keeps even the low harmonics wide (detune alone
            // barely separates a sine-based tone's strong low partials).
            for (std::size_t side = 0; side < 2; ++side)
            {
                const auto sideScale = side == 0 ? 1.0 : 0.75;
                fmDepth1[side] = static_cast<float>(design.fmIndex1 * scale * sideScale * radian);
                fmDepth2[side] = static_cast<float>(design.fmIndex2 * scale * sideScale * radian);
            }
        }
    }

    // Diagnostic isolation only: choose octave presence once before a phrase.
    Stereo next(bool octavePresence = true) noexcept
    {
        // A pulse at width 0.5 is two saw reads half a cycle apart.
        const auto square = body.nextPulse(0.5);
        std::array<float, 2> sum{};
        for (std::size_t side = 0; side < 2; ++side)
        {
            if (design.fm())
                for (std::size_t i = 0; i < mainCount; ++i)
                {
                    const auto phase = fmPhase[side][i];
                    fmPhase[side][i] += fmIncrement[side][i];
                    const auto modulation = fmDepth1[side] * dsp_core::sine(phase) + fmDepth2[side] * dsp_core::sine(2u * phase);
                    sum[side] += mainWeights[i] * dsp_core::sine(phase + static_cast<std::uint32_t>(static_cast<std::int64_t>(modulation)));
                }
            else
                for (std::size_t i = 0; i < mainCount; ++i) sum[side] += mainWeights[i] * main[side][i].next();
            float presence = 0.0f;
            for (auto& oscillator : octave[side]) presence += oscillator.next();
            if (octavePresence) sum[side] += octaveGain * presence;
            sum[side] += bodyGain * 0.5f * square;
        }
        return {sum[0], sum[1]};
    }

private:
    static double cents(double value) noexcept { return std::exp2(value / 1200.0); }
    static constexpr double radian = 4294967296.0 / 6.283185307179586;
    // The JP-8000 supersaw's uneven detune positions, normalized to +-1:
    // dense near the centre, wide at the edges, never symmetric.
    static constexpr std::array<double, mainCount> mainOffsets{
        -1.0, -0.5716, -0.1775, 0.0, 0.1810, 0.5650, 0.9767};
    static constexpr std::array<double, octaveCount> octaveOffsets{-1.0, -0.45, 0.0, 0.5, 0.95};
    // A louder centre saw (about 4 dB over each side saw) keeps the pitch
    // focused; the weights sum to one so the stack's peak stays bounded.
    static constexpr std::array<float, mainCount> mainWeights{
        0.1313f, 0.1313f, 0.1313f, 0.2122f, 0.1313f, 0.1313f, 0.1313f};
    Design design{};
    std::array<std::array<dsp_core::WavetableOscillator, mainCount>, 2> main{};
    std::array<std::array<dsp_core::WavetableOscillator, octaveCount>, 2> octave{};
    dsp_core::WavetableOscillator body;
    float octaveGain = 0.0f, bodyGain = 0.0f;
    std::array<float, 2> fmDepth1{}, fmDepth2{};
    std::array<std::array<std::uint32_t, mainCount>, 2> fmPhase{}, fmIncrement{};
};
}
