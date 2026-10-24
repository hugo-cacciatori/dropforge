#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// A bounded chaotic recurrence selects targets at a control clock. Smooth
// interpolation removes step edges; callers band-limit/colour the audio.
// No random generator, note-frequency oscillator, allocation or shared state.
class SmoothChaos
{
public:
    void reset(double seed = 0.137619) noexcept
    {
        state = std::isfinite(seed) ? std::clamp(seed, -0.9, 0.9) : 0.137619;
        for (int i = 0; i < 32; ++i) advance();
        from = advance(); to = advance(); phase = 0.0;
    }
    double next(double clockHz, double sampleRate) noexcept
    {
        const auto rate = std::isfinite(sampleRate) && sampleRate >= 44100.0
            && sampleRate <= 192000.0 ? sampleRate : 44100.0;
        const auto clock = std::clamp(std::isfinite(clockHz) ? clockHz : 1733.0, 1.0, 7000.0);
        const auto weight = phase * phase * (3.0 - 2.0 * phase);
        const auto value = from + weight * (to - from);
        phase += clock / rate;
        if (phase >= 1.0) { phase -= 1.0; from = to; to = advance(); }
        return value;
    }
private:
    double advance() noexcept { state = 1.0 - 1.97 * state * state; return state; }
    double state = 0.137619, from = 0.0, to = 0.0, phase = 0.0;
};
}
