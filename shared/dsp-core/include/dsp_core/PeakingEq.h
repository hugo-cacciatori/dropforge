#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Fixed broad tonal emphasis. Configure outside the audio callback; no control
// automation is implied by this API. reset keeps the prepared coefficients.
class PeakingEq
{
public:
    void prepare(double frequency, double gainDb, double q, double sampleRate) noexcept
    {
        const auto rate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 44100.0, 192000.0) : 48000.0;
        frequency = std::isfinite(frequency) ? std::clamp(frequency, 1.0, 0.45 * rate) : 1800.0;
        gainDb = std::isfinite(gainDb) ? std::clamp(gainDb, -12.0, 12.0) : 0.0;
        q = std::isfinite(q) ? std::clamp(q, 0.1, 10.0) : 0.7;
        bypass = gainDb == 0.0;
        const auto a = std::pow(10.0, gainDb / 40.0);
        const auto omega = 6.28318530717958647692 * frequency / rate;
        const auto alpha = std::sin(omega) / (2.0 * q);
        const auto denominator = 1.0 + alpha / a;
        b0 = (1.0 + alpha * a) / denominator;
        b1 = -2.0 * std::cos(omega) / denominator;
        b2 = (1.0 - alpha * a) / denominator;
        a1 = b1;
        a2 = (1.0 - alpha / a) / denominator;
        reset();
    }
    void reset() noexcept { z1 = z2 = 0.0; }
    double next(double input) noexcept
    {
        if (bypass) return input;
        const auto output = b0 * input + z1;
        z1 = quiet(b1 * input - a1 * output + z2);
        z2 = quiet(b2 * input - a2 * output);
        return quiet(output);
    }
private:
    static double quiet(double value) noexcept { return std::abs(value) < 1e-30 ? 0.0 : value; }
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
    bool bypass = true;
};
}
