#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

namespace dsp_core::test
{
// Offline-only measurement harness. A complete finite render is rectangularly
// windowed, zero-padded, then integrated in FFT bins; no weighting hides attacks.
inline std::vector<std::complex<double>> spectrum(const std::vector<float>& input)
{
    std::size_t size = 1;
    while (size < input.size()) size *= 2;
    std::vector<std::complex<double>> bins(size);
    std::copy(input.begin(), input.end(), bins.begin());
    for (std::size_t i = 1, j = 0; i < size; ++i)
    {
        auto bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(bins[i], bins[j]);
    }
    for (std::size_t length = 2; length <= size; length *= 2)
    {
        const auto step = std::polar(1.0, -6.28318530717958647692 / static_cast<double>(length));
        for (std::size_t start = 0; start < size; start += length)
        {
            std::complex<double> phase = 1.0;
            for (std::size_t i = 0; i < length / 2; ++i)
            {
                const auto a = bins[start + i], b = bins[start + i + length / 2] * phase;
                bins[start + i] = a + b; bins[start + i + length / 2] = a - b;
                phase *= step;
            }
        }
    }
    return bins;
}
struct StereoMetrics
{
    double correlation = 0.0, foldLossDb = 0.0, lowRhythmDbfs = -300.0, lowBassDbfs = -300.0;
    double separationDb = 0.0, stereoPower = 0.0;
    double foldBandDbfs = -300.0;
};
inline double powerDb(double power) { return 10.0 * std::log10(std::max(power, 1.0e-30)); }
inline StereoMetrics measure(const std::vector<float>& left, const std::vector<float>& right,
                             const std::vector<float>& bass, double rate, double split)
{
    if (left.empty() || left.size() != right.size() || left.size() != bass.size())
        throw std::invalid_argument("equal nonempty measurement buffers required");
    double l = 0.0, r = 0.0, ll = 0.0, rr = 0.0, lr = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        l += left[i]; r += right[i];
        ll += left[i] * static_cast<double>(left[i]); rr += right[i] * static_cast<double>(right[i]);
        lr += left[i] * static_cast<double>(right[i]);
    }
    const auto frames = static_cast<double>(left.size());
    StereoMetrics result;
    result.stereoPower = (ll + rr) / (2.0 * frames);
    const auto variance = std::max(0.0, (ll - l * l / frames) * (rr - r * r / frames));
    result.correlation = variance > 0.0 ? (lr - l * r / frames) / std::sqrt(variance) : 0.0;
    const auto L = spectrum(left), R = spectrum(right), B = spectrum(bass);
    double reference = 0.0, mono = 0.0, lowRhythm = 0.0, lowBass = 0.0;
    for (std::size_t bin = 0; bin <= L.size() / 2; ++bin)
    {
        const auto hz = static_cast<double>(bin) * rate / static_cast<double>(L.size());
        const auto weight = bin == 0 || bin == L.size() / 2 ? 1.0 : 2.0;
        if (hz >= 200.0 && hz <= 8000.0)
        {
            reference += weight * (std::norm(L[bin]) + std::norm(R[bin])) * 0.5;
            mono += weight * std::norm((L[bin] + R[bin]) * 0.5);
        }
        if (hz < split)
        {
            lowRhythm += weight * (std::norm(L[bin]) + std::norm(R[bin])) * 0.5;
            lowBass += weight * std::norm(B[bin]);
        }
    }
    const auto normalization = static_cast<double>(L.size()) * frames;
    result.foldBandDbfs = powerDb(reference / normalization);
    result.foldLossDb = powerDb(reference) - powerDb(mono);
    result.lowRhythmDbfs = powerDb(lowRhythm / normalization);
    result.lowBassDbfs = powerDb(lowBass / normalization);
    result.separationDb = result.lowRhythmDbfs - result.lowBassDbfs;
    return result;
}
}
