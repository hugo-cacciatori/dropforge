#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>
#include <dsp_core/Svf.h>

namespace dropforge
{
// The FX page's reverb (SPEC section 8): an algorithmic room, no impulse
// responses. A pre-delay, four allpass diffusers per side that smear the
// input into a dense cloud, then an eight-line feedback delay network mixed
// by a Hadamard matrix (every line feeds every other with equal energy, so
// the tail is smooth and colourless). Each line loses exactly the level that
// makes the whole tail fall 60 dB in the Decay time, through a low-pass that
// darkens it as it fades (Damping). Two lines drift slowly in length
// (Modulation) so long tails shimmer instead of ringing metallic.
class FxReverb
{
public:
    static constexpr double maximumPreDelay = 0.5;
    struct Settings
    {
        float decaySeconds = 0.8f, preDelayMs = 20.0f, size = 0.6f, diffusion = 0.8f, damping = 0.4f, modulation = 0.3f;
        float width = 1.0f, lowCutHz = 250.0f, highCutHz = 7000.0f;
    };

    // Not real-time: allocates the lines.
    void prepare(double sampleRate)
    {
        rate = sampleRate;
        const auto longest = static_cast<std::size_t>(std::ceil(baseMs.back() * 1.6e-3 * rate)) + 64;
        for (auto& line : lines) line.assign(longest, 0.0f);
        for (auto& side : preDelay) side.assign(static_cast<std::size_t>(std::ceil(maximumPreDelay * rate)) + 2, 0.0f);
        for (std::size_t s = 0; s < 2; ++s)
            for (std::size_t d = 0; d < diffusers[s].size(); ++d)
                diffusers[s][d].buffer.assign(static_cast<std::size_t>(std::ceil(diffuserMs[s][d] * 1e-3 * rate)) + 1, 0.0f);
        smoothing = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * rate)));
        sizeSmoothing = static_cast<float>(1.0 - std::exp(-1.0 / (0.100 * rate)));
        filtersSet = false;
        reset();
    }
    void reset() noexcept
    {
        for (auto& line : lines) std::fill(line.begin(), line.end(), 0.0f);
        for (auto& side : preDelay) std::fill(side.begin(), side.end(), 0.0f);
        for (auto& side : diffusers) for (auto& d : side) { std::fill(d.buffer.begin(), d.buffer.end(), 0.0f); d.position = 0; }
        for (auto& f : lowpass) f = 0.0f;
        for (auto* filter : {&inputLow[0], &inputLow[1], &inputHigh[0], &inputHigh[1]}) filter->reset();
        writeAt = preDelayAt = 0;
        modulationPhase = 0.0;
        size = target.size;
        width = target.width;
        quiet = 0;
        controlCountdown = 0;
        recalculate();
    }
    void set(const Settings& settings) noexcept
    {
        if (settings.lowCutHz != target.lowCutHz || settings.highCutHz != target.highCutHz || !filtersSet)
        {
            for (int side = 0; side < 2; ++side)
            {
                inputLow[side].set(dsp_core::Svf::Type::highpass, settings.lowCutHz, rate);
                inputHigh[side].set(dsp_core::Svf::Type::lowpass, settings.highCutHz, rate);
            }
            filtersSet = true;
        }
        const auto changed = settings.decaySeconds != target.decaySeconds || settings.damping != target.damping
                          || settings.diffusion != target.diffusion;
        target = settings;
        target.size = std::clamp(settings.size, 0.0f, 1.0f);
        target.width = std::clamp(settings.width, 0.0f, 1.0f);
        if (changed) { recalculate(); controlCountdown = 0; }
    }
    struct Stereo { float left = 0.0f, right = 0.0f; };
    Stereo process(float inLeft, float inRight) noexcept
    {
        size += sizeSmoothing * (target.size - size);
        width += smoothing * (target.width - width);
        inLeft = inputHigh[0].next(inputLow[0].next(inLeft));
        inRight = inputHigh[1].next(inputLow[1].next(inRight));
        // Pre-delay, then the diffusers smear each side into a cloud.
        const auto preSize = preDelay[0].size();
        preDelay[0][preDelayAt] = inLeft;
        preDelay[1][preDelayAt] = inRight;
        const auto preSamples = static_cast<std::size_t>(std::clamp(static_cast<double>(target.preDelayMs) * 1e-3 * rate, 0.0, static_cast<double>(preSize - 1)));
        const auto readPre = preDelayAt >= preSamples ? preDelayAt - preSamples : preDelayAt + preSize - preSamples;
        std::array<float, 2> cloud{preDelay[0][readPre], preDelay[1][readPre]};
        preDelayAt = preDelayAt + 1 == preSize ? 0 : preDelayAt + 1;
        for (std::size_t s = 0; s < 2; ++s)
            for (auto& d : diffusers[s]) cloud[s] = d.next(cloud[s], diffuserGain);
        // Read the eight lines; size scales every length together.
        const auto scale = 0.5 + static_cast<double>(size);
        modulationPhase += 0.37 / rate;
        if (modulationPhase >= 1.0) modulationPhase -= 1.0;
        const auto wobble = static_cast<double>(target.modulation) * 12.0 * std::sin(6.283185307179586 * modulationPhase);
        std::array<float, 8> taps{};
        const auto length = lines[0].size();
        // Each line's loss follows its length (size) at control rate: a
        // pow per line per sample would cost more than the whole network.
        if (controlCountdown-- == 0)
        {
            controlCountdown = 31;
            for (std::size_t i = 0; i < 8; ++i)
                loss[i] = static_cast<float>(std::pow(10.0, -3.0 * baseMs[i] * 1e-3 * scale / static_cast<double>(decay)));
        }
        for (std::size_t i = 0; i < 8; ++i)
        {
            auto delay = baseMs[i] * 1e-3 * rate * scale;
            if (i == 1) delay += wobble;
            if (i == 6) delay -= wobble;
            delay = std::clamp(delay, 2.0, static_cast<double>(length - 2));
            const auto whole = static_cast<std::size_t>(delay);
            const auto fraction = static_cast<float>(delay - static_cast<double>(whole));
            const auto at = (writeAt + length - whole) % length, before = (at + length - 1) % length;
            taps[i] = (1.0f - fraction) * lines[i][at] + fraction * lines[i][before];
            // Loss and damping: the gain falls 60 dB in the Decay time for
            // this line's own length; the low-pass darkens what remains.
            lowpass[i] += dampingCoefficient * (taps[i] - lowpass[i]);
            taps[i] = loss[i] * lowpass[i];
        }
        // Hadamard mixing: energy preserving, every line into every line.
        auto mixed = taps;
        for (std::size_t span = 1; span < 8; span *= 2)
            for (std::size_t i = 0; i < 8; i += 2 * span)
                for (std::size_t j = i; j < i + span; ++j)
                {
                    const auto a = mixed[j], b = mixed[j + span];
                    mixed[j] = a + b; mixed[j + span] = a - b;
                }
        constexpr float normal = 0.35355339f; // 1 / sqrt(8)
        float loud = 0.0f;
        for (std::size_t i = 0; i < 8; ++i)
        {
            const auto input = (i % 2 == 0 ? cloud[0] : cloud[1]) * 0.5f;
            const auto value = flush(normal * mixed[i] + input);
            lines[i][writeAt] = value;
            loud = std::max(loud, std::abs(value));
        }
        writeAt = writeAt + 1 == length ? 0 : writeAt + 1;
        quiet = loud < 1.0e-7f && std::abs(inLeft) < 1.0e-7f && std::abs(inRight) < 1.0e-7f ? quiet + 1 : 0;
        // Odd and even lines make two decorrelated sides; Width folds them.
        const auto left = 0.5f * (taps[0] - taps[2] + taps[4] - taps[6]);
        const auto right = 0.5f * (taps[1] - taps[3] + taps[5] - taps[7]);
        const auto mid = 0.5f * (left + right), side = 0.5f * width * (left - right);
        return {mid + side, mid - side};
    }
    // The network and pre-delay hold nothing above -140 dBFS.
    bool silent() const noexcept { return quiet > preDelay[0].size() + lines[0].size(); }

private:
    struct Allpass
    {
        std::vector<float> buffer;
        std::size_t position = 0;
        float next(float input, float gain) noexcept
        {
            const auto delayed = buffer[position];
            const auto written = input + gain * delayed;
            buffer[position] = std::abs(written) < 1.0e-20f ? 0.0f : written;
            position = position + 1 == buffer.size() ? 0 : position + 1;
            return delayed - gain * written;
        }
    };
    void recalculate() noexcept
    {
        decay = std::clamp(target.decaySeconds, 0.1f, 15.0f);
        diffuserGain = 0.75f * std::clamp(target.diffusion, 0.0f, 1.0f);
        // Damping 0 leaves the tail bright (cutoff near 18 kHz); 1 darkens
        // it to about 1.5 kHz.
        const auto cutoff = 18000.0 * std::pow(1500.0 / 18000.0, static_cast<double>(std::clamp(target.damping, 0.0f, 1.0f)));
        dampingCoefficient = static_cast<float>(1.0 - std::exp(-6.283185307179586 * std::min(cutoff, 0.45 * rate) / rate));
    }
    static float flush(float value) noexcept { return std::abs(value) < 1.0e-20f ? 0.0f : value; }
    // Line lengths at size 0.5 (x1), mutually prime-ish so echoes never line up.
    static constexpr std::array<double, 8> baseMs{29.7, 37.1, 41.1, 43.7, 53.0, 59.9, 67.7, 73.1};
    static constexpr double diffuserMs[2][4]{{4.7, 3.6, 12.7, 9.3}, {5.3, 3.1, 11.3, 8.9}};
    std::array<std::vector<float>, 8> lines;
    std::array<std::vector<float>, 2> preDelay;
    std::array<std::array<Allpass, 4>, 2> diffusers;
    std::array<float, 8> lowpass{}, loss{};
    int controlCountdown = 0;
    dsp_core::Svf inputLow[2], inputHigh[2];
    Settings target{};
    double rate = 48000.0, modulationPhase = 0.0;
    std::size_t writeAt = 0, preDelayAt = 0, quiet = 0;
    float size = 0.6f, width = 1.0f, decay = 0.8f, diffuserGain = 0.6f, dampingCoefficient = 0.5f;
    float smoothing = 0.004f, sizeSmoothing = 0.0002f;
    bool filtersSet = false;
};
}
