#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Two corrected edges, with zero DC at a fixed width. Slow width motion moves
// harmonic emphasis without changing the oscillator's pitch/phase increment.
class PolyBlepPulse
{
public:
    void reset(double initialPhase = 0.0) noexcept
    {
        phase = std::isfinite(initialPhase) ? initialPhase - std::floor(initialPhase) : 0.0;
        widthReady = false;
    }
    double next(double frequency, double sampleRate, double width = 0.5) noexcept
    {
        const auto increment = std::clamp(frequency / sampleRate, 1.0e-9, 0.45);
        width = std::isfinite(width) ? std::clamp(width, 0.2, 0.8) : 0.5;
        const auto edgeSpeed = std::clamp(increment - (widthReady ? width - previousWidth : 0.0),
                                         1.0e-9, 0.45);
        previousWidth = width;
        widthReady = true;
        auto shifted = phase - width;
        if (shifted < 0.0) shifted += 1.0;
        const auto output = (phase < width ? -1.0 : 1.0) + 2.0 * width - 1.0
                            - blep(phase, increment) + blep(shifted, edgeSpeed);
        phase += increment;
        if (phase >= 1.0) phase -= 1.0;
        return output;
    }
private:
    static double blep(double position, double increment) noexcept
    {
        if (position < increment)
        {
            const auto t = position / increment;
            return 2.0 * t - t * t - 1.0;
        }
        if (position > 1.0 - increment)
        {
            const auto t = (position - 1.0) / increment;
            return t * t + 2.0 * t + 1.0;
        }
        return 0.0;
    }
    double phase = 0.0, previousWidth = 0.5;
    bool widthReady = false;
};
}
