#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
class PolyBlepSaw
{
public:
    void reset(double initialPhase = 0.0) noexcept
    {
        phase = std::isfinite(initialPhase) ? initialPhase - std::floor(initialPhase) : 0.0;
    }
    double next(double frequency, double sampleRate) noexcept
    {
        // Leave headroom below Nyquist; callers supply finite positive rates.
        const auto increment = std::clamp(frequency / sampleRate, 1.0e-9, 0.45);
        auto result = 2.0 * phase - 1.0;
        if (phase < increment)
        {
            const auto t = phase / increment;
            result -= 2.0 * t - t * t - 1.0;
        }
        else if (phase > 1.0 - increment)
        {
            const auto t = (phase - 1.0) / increment;
            result -= t * t + 2.0 * t + 1.0;
        }
        phase += increment;
        if (phase >= 1.0)
            phase -= 1.0;
        return result;
    }

private:
    double phase = 0.0;
};
}
