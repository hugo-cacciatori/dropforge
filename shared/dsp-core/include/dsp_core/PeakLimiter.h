#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dsp_core
{
// Stereo-linked look-ahead peak limiter with a hard sample-peak guarantee.
// Each sample asks for the gain that would put it exactly at the ceiling.
// That request is held (minimum) over the look-ahead window, released
// slowly, then averaged over the same window: the gain is already down
// when a peak arrives and glides there, with no step to click. Audio is
// delayed by latencySamples. The final clamp is a guard against float
// rounding; with this design it moves a sample by at most a few ulps.
class PeakLimiter
{
public:
    static constexpr int window = 33;                    // hold and average length
    static constexpr int latencySamples = window - 1;    // look-ahead, in samples

    void prepare(float ceiling, double sampleRate, double releaseSeconds = 0.120) noexcept
    {
        limit = std::isfinite(ceiling) ? std::clamp(ceiling, 1.0e-3f, 1.0f) : 1.0f;
        const auto rate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
        release = 1.0f - static_cast<float>(std::exp(-1.0 / (releaseSeconds * rate)));
        reset();
    }
    void reset() noexcept
    {
        delayLeft.fill(0.0f); delayRight.fill(0.0f);
        requested.fill(1.0f); envelope.fill(1.0f);
        position = 0;
        released = 1.0f;
        lastGain = 1.0f;
    }

    // Processes one stereo frame in place (output is latencySamples late).
    void process(float& left, float& right) noexcept
    {
        const auto peak = std::max(std::abs(left), std::abs(right));
        requested[position] = peak > limit ? limit / peak : 1.0f;
        delayLeft[position] = left;
        delayRight[position] = right;
        // Hold: the smallest request in the window, newest sample included.
        auto held = 1.0f;
        for (const auto value : requested) held = std::min(held, value);
        // Instant attack to the held request, smooth release back to unity.
        released = std::min(held, released + release * (1.0f - released));
        envelope[position] = released;
        // Average over the same window: every term is at or below the
        // request of the sample now leaving the delay, so the average is too.
        auto sum = 0.0f;
        for (const auto value : envelope) sum += value;
        lastGain = sum * (1.0f / window);
        const auto oldest = (position + 1) % window;
        left = std::clamp(delayLeft[oldest] * lastGain, -limit, limit);
        right = std::clamp(delayRight[oldest] * lastGain, -limit, limit);
        position = oldest;
    }
    float gainReductionDb() const noexcept { return 20.0f * std::log10(std::max(lastGain, 1.0e-6f)); }

private:
    std::array<float, window> delayLeft{}, delayRight{}, requested{}, envelope{};
    int position = 0;
    float limit = 1.0f, release = 0.0f, released = 1.0f, lastGain = 1.0f;
};
}
