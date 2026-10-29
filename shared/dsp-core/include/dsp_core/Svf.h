#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Topology-preserving (trapezoidal) state-variable filter in 32-bit float.
// Unlike a direct-form biquad it stays accurate in float even for a 40 Hz
// cutoff at 192 kHz, and its cutoff can move without clicks because the
// state is the integrators' own energy. Coefficients are computed in double.
// One class covers every Furnace tone stage: low-pass, high-pass, bell, high
// shelf and the allpass that phase-matches crossover bands, all second order.
class Svf
{
public:
    enum class Type { lowpass, highpass, bell, highShelf, allpass };

    // Keeps the integrator state, so a smoothed caller can move cutoff freely.
    void set(Type kind, double cutoff, double sampleRate, double q = 0.7071067811865476, double gainDb = 0.0) noexcept
    {
        const auto rate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
        const auto hz = std::clamp(std::isfinite(cutoff) ? cutoff : 1000.0, 1.0, rate * 0.49);
        q = std::isfinite(q) ? std::clamp(q, 0.1, 20.0) : 0.7071067811865476;
        const auto amplitude = std::pow(10.0, (std::isfinite(gainDb) ? gainDb : 0.0) / 40.0);
        auto g = std::tan(3.14159265358979323846 * hz / rate);
        auto k = 1.0 / q;
        // Output mix m0 x high + m1 x band + m2 x low (Simper's formulation).
        double high = 0.0, band = 0.0, low = 0.0;
        switch (kind)
        {
            case Type::lowpass: low = 1.0; break;
            case Type::highpass: high = 1.0; break;
            case Type::bell:
                k = 1.0 / (q * amplitude);
                high = 1.0; band = k * amplitude * amplitude; low = 1.0;
                break;
            case Type::highShelf:
                g *= std::sqrt(amplitude);
                high = amplitude * amplitude; band = k * amplitude; low = 1.0;
                break;
            // Flat magnitude, the phase of a Linkwitz-Riley pair at this cutoff
            // (with Q 0.7071): input - 2k x band = high - k x band + low.
            case Type::allpass: high = 1.0; band = -k; low = 1.0; break;
        }
        const auto a1 = 1.0 / (1.0 + g * (g + k));
        c1 = static_cast<float>(a1);
        c2 = static_cast<float>(g * a1);
        c3 = static_cast<float>(g * g * a1);
        damping = static_cast<float>(k);
        mixHigh = static_cast<float>(high); mixBand = static_cast<float>(band); mixLow = static_cast<float>(low);
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }

    float next(float input) noexcept
    {
        const auto v3 = input - ic2;
        const auto v1 = c1 * ic1 + c2 * v3;
        const auto v2 = ic2 + c2 * ic1 + c3 * v3;
        ic1 = quiet(2.0f * v1 - ic1);
        ic2 = quiet(2.0f * v2 - ic2);
        // high = input - k x band - low, written out so each mode costs the same.
        return mixHigh * (input - damping * v1 - v2) + mixBand * v1 + mixLow * v2;
    }
    void process(float* data, int count) noexcept
    {
        for (int i = 0; i < count; ++i) data[i] = next(data[i]);
    }

private:
    // Flush tails below audibility so idle integrators never go denormal.
    static float quiet(float value) noexcept { return std::abs(value) < 1.0e-20f ? 0.0f : value; }
    float c1 = 1.0f, c2 = 0.0f, c3 = 0.0f, damping = 1.4142135f;
    float mixHigh = 0.0f, mixBand = 0.0f, mixLow = 1.0f;
    float ic1 = 0.0f, ic2 = 0.0f;
};
}
