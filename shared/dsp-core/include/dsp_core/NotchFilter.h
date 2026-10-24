#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Remove one tracked fundamental from a texture branch, leaving the separate
// clean oscillator to own it. Frequency movement retains filter history.
class NotchFilter
{
public:
    void prepare(double frequency, double sampleRate) noexcept
    {
        rate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 44100.0, 192000.0) : 48000.0;
        setFrequency(frequency);
        reset();
    }
    void setFrequency(double frequency) noexcept
    {
        frequency = std::isfinite(frequency) ? std::clamp(frequency, 1.0, rate * 0.45) : 110.0;
        const auto omega = 6.28318530717958647692 * frequency / rate;
        const auto alpha = std::sin(omega) / (2.0 * 0.7);
        b0 = 1.0 / (1.0 + alpha);
        b1 = -2.0 * std::cos(omega) * b0;
        a2 = (1.0 - alpha) * b0;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
    double next(double input) noexcept
    {
        const auto output = b0 * input + z1;
        z1 = quiet(b1 * input - b1 * output + z2);
        z2 = quiet(b0 * input - a2 * output);
        return quiet(output);
    }
private:
    static double quiet(double value) noexcept { return std::abs(value) < 1e-30 ? 0.0 : value; }
    double rate = 48000.0, b0 = 1.0, b1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
};
}
