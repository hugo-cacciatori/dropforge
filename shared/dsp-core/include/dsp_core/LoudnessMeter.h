#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dsp_core
{
// ITU-R BS.1770-4 / EBU R 128 stereo loudness: K-weighting, momentary
// (400 ms), short-term (3 s) and gated integrated loudness, in LUFS.
// Fixed storage only: energies arrive in 100 ms steps, short-term from a
// ring of 30 steps, and the integrated gate from a 0.05 LU histogram of
// momentary blocks, accurate to well under 0.1 LU. Real-time safe.
class LoudnessMeter
{
public:
    static constexpr double silence = -1000.0;

    void prepare(double sampleRate) noexcept
    {
        const auto rate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
        // Pre-filter (high shelf about +4 dB above 1.5 kHz) and RLB high-pass,
        // designed for any rate from the standard's analogue parameters.
        auto k = std::tan(pi * 1681.974450955533 / rate);
        auto q = 0.7071752369554196;
        const auto vh = std::pow(10.0, 3.999843853973347 / 20.0), vb = std::pow(vh, 0.4996667741545416);
        auto a0 = 1.0 + k / q + k * k;
        shelf = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
        k = std::tan(pi * 38.13547087602444 / rate);
        q = 0.5003270373238773;
        a0 = 1.0 + k / q + k * k;
        highpass = {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
        stepLength = std::max(1, static_cast<int>(std::lround(0.1 * rate)));
        reset();
    }
    void reset() noexcept
    {
        for (auto& channel : state) channel = {};
        steps.fill(0.0);
        histogramEnergy.fill(0.0);
        histogramCount.fill(0);
        stepEnergy = 0.0;
        stepFill = 0;
        stepsSeen = 0;
        momentaryMax = shortTermMax = silence;
    }

    void process(float left, float right) noexcept
    {
        stepEnergy += weigh(state[0], left) + weigh(state[1], right);
        if (++stepFill < stepLength) return;
        steps[static_cast<std::size_t>(stepsSeen % steps.size())] = stepEnergy / stepLength;
        ++stepsSeen;
        stepEnergy = 0.0;
        stepFill = 0;
        if (stepsSeen >= 4)
        {
            const auto momentary = mean(4);
            momentaryMax = std::max(momentaryMax, lufs(momentary));
            const auto bin = binOf(lufs(momentary));
            if (bin >= 0)
            {
                histogramEnergy[static_cast<std::size_t>(bin)] += momentary;
                ++histogramCount[static_cast<std::size_t>(bin)];
            }
        }
        if (stepsSeen >= 30) shortTermMax = std::max(shortTermMax, lufs(mean(30)));
    }

    double momentaryMaxLufs() const noexcept { return momentaryMax; }
    double shortTermMaxLufs() const noexcept { return shortTermMax; }
    // Gated integrated loudness: absolute gate -70 LUFS, then relative -10 LU.
    double integratedLufs() const noexcept
    {
        const auto absolute = gatedMean(0);
        if (absolute <= 0.0) return silence;
        const auto relative = binOf(lufs(absolute) - 10.0);
        const auto gated = gatedMean(std::max(relative + 1, 0));
        return gated > 0.0 ? lufs(gated) : silence;
    }

private:
    struct Biquad { double b0, b1, b2, a1, a2; };
    struct Channel { double x1, x2, y1, y2, z1, z2; };
    static constexpr double pi = 3.14159265358979323846;
    static constexpr double floorLufs = -70.0, binWidth = 0.05;
    static constexpr int bins = 1800; // -70 to +20 LUFS

    double weigh(Channel& c, float input) const noexcept
    {
        const auto x = std::isfinite(input) ? static_cast<double>(input) : 0.0;
        const auto y = shelf.b0 * x + shelf.b1 * c.x1 + shelf.b2 * c.x2 - shelf.a1 * c.y1 - shelf.a2 * c.y2;
        c.x2 = c.x1; c.x1 = x;
        const auto z = highpass.b0 * y + highpass.b1 * c.y1 + highpass.b2 * c.y2 - highpass.a1 * c.z1 - highpass.a2 * c.z2;
        c.y2 = c.y1; c.y1 = y;
        c.z2 = c.z1; c.z1 = z;
        return z * z;
    }
    double mean(int count) const noexcept
    {
        double sum = 0.0;
        for (int i = 1; i <= count; ++i)
            sum += steps[static_cast<std::size_t>((stepsSeen - i) % steps.size())];
        return sum / count;
    }
    static double lufs(double power) noexcept { return power > 0.0 ? -0.691 + 10.0 * std::log10(power) : silence; }
    // Block energies at or above floorLufs land in 0.05 LU bins; -1 below.
    static int binOf(double loudness) noexcept
    {
        if (!(loudness > floorLufs)) return -1;
        return std::min(bins - 1, static_cast<int>((loudness - floorLufs) / binWidth));
    }
    double gatedMean(int firstBin) const noexcept
    {
        double energy = 0.0;
        std::uint64_t count = 0;
        for (int bin = firstBin; bin < bins; ++bin)
        {
            energy += histogramEnergy[static_cast<std::size_t>(bin)];
            count += histogramCount[static_cast<std::size_t>(bin)];
        }
        return count > 0 ? energy / static_cast<double>(count) : 0.0;
    }

    Biquad shelf{}, highpass{};
    std::array<Channel, 2> state{};
    std::array<double, 30> steps{};
    std::array<double, bins> histogramEnergy{};
    std::array<std::uint64_t, bins> histogramCount{};
    double stepEnergy = 0.0, momentaryMax = silence, shortTermMax = silence;
    int stepLength = 4800, stepFill = 0;
    std::int64_t stepsSeen = 0;
};
}
