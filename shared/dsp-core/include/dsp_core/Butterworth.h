#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Bilinear-transform biquad, defaulting to second-order Butterworth Q.
// Optional section Q also supports higher-order Butterworth cascades.
// prepare clears history; setCutoff retains it. Callers smooth cutoff movement.
class Butterworth
{
public:
    enum class Type { lowpass, highpass };
    void prepare(Type type, double cutoff, double sampleRate, double q = 0.7071067811865476) noexcept
    {
        setCutoff(type, cutoff, sampleRate, q);
        reset();
    }
    // Retain history when the caller moves a smoothed cutoff.
    void setCutoff(Type type, double cutoff, double sampleRate, double q = 0.7071067811865476) noexcept
    {
        const auto rate = std::isfinite(sampleRate) && sampleRate >= 44100.0
            && sampleRate <= 192000.0 ? sampleRate : 44100.0;
        const auto hz = std::clamp(std::isfinite(cutoff) ? cutoff : 140.0, 1.0, rate * 0.45);
        const auto k = std::tan(3.14159265358979323846 * hz / rate);
        const auto k2 = k * k;
        // Preserve the established default arithmetic for existing bass renders.
        const auto root2k = q == 0.7071067811865476 || !std::isfinite(q)
            ? std::sqrt(2.0) * k : k / std::clamp(q, 0.1, 10.0);
        const auto norm = 1.0 / (1.0 + root2k + k2);
        b0 = (type == Type::lowpass ? k2 : 1.0) * norm;
        b1 = (type == Type::lowpass ? 2.0 : -2.0) * b0;
        b2 = b0;
        a1 = 2.0 * (k2 - 1.0) * norm;
        a2 = (1.0 - root2k + k2) * norm;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
    double next(double input) noexcept
    {
        const auto output = b0 * input + z1;
        z1 = quiet(b1 * input - a1 * output + z2);
        z2 = quiet(b2 * input - a2 * output);
        return quiet(output);
    }

private:
    // Also protects standalone headless clients without a host's denormal guard.
    static double quiet(double value) noexcept { return std::abs(value) < 1.0e-30 ? 0.0 : value; }
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
};

// Two identical Butterworth sections give a fourth-order Linkwitz-Riley HP.
class LinkwitzRileyHighpass
{
public:
    void prepare(double cutoff, double rate) noexcept
    {
        first.prepare(Butterworth::Type::highpass, cutoff, rate);
        second.prepare(Butterworth::Type::highpass, cutoff, rate);
    }
    void reset() noexcept { first.reset(); second.reset(); }
    void setCutoff(double cutoff, double rate) noexcept
    {
        first.setCutoff(Butterworth::Type::highpass, cutoff, rate);
        second.setCutoff(Butterworth::Type::highpass, cutoff, rate);
    }
    double next(double input) noexcept { return second.next(first.next(input)); }
private:
    Butterworth first, second;
};
}
