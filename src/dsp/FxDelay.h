#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include <dsp_core/Svf.h>

namespace dropforge
{
// The FX page's delay (SPEC section 8): a stereo or ping-pong echo on the
// instruments' sends. Its input and its feedback loop are band-limited
// (low cut and high cut), so repeats never muddy the low end or fizz, and
// each repeat gets a little darker and thinner, like a tape or analogue
// echo. A time change crossfades between two read heads (equal power, 30 ms)
// instead of jumping or pitch-sliding, so retiming never clicks.
class FxDelay
{
public:
    // The longest echo: 6 s holds 2 bars at 80 BPM, or 1 bar dotted at 60.
    // Longer synced times are clamped to it.
    static constexpr double maximumSeconds = 6.0;
    static constexpr double fadeSeconds = 0.030;
    struct Settings
    {
        double seconds = 0.375;
        float feedback = 0.25f, width = 1.0f, lowCutHz = 280.0f, highCutHz = 6500.0f;
        bool pingPong = true;
    };

    // Not real-time: allocates the lines.
    void prepare(double sampleRate)
    {
        rate = sampleRate;
        const auto size = static_cast<std::size_t>(std::ceil(maximumSeconds * rate)) + 2;
        for (auto& line : lines) line.assign(size, 0.0f);
        fadeSamples = std::max(1, static_cast<int>(std::ceil(rate * fadeSeconds)));
        smoothing = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * rate)));
        filtersSet = false; // retuned for this rate by the next set()
        reset();
    }
    // Silence, as after prepare(): only the part of the lines written since
    // the last clear is zeroed, so a panic stays cheap on the audio thread.
    void reset() noexcept
    {
        const auto size = lines[0].size();
        if (size > 0)
            for (std::size_t n = 0, at = position; n < std::min(written, size); ++n)
            {
                at = at == 0 ? size - 1 : at - 1;
                lines[0][at] = lines[1][at] = 0.0f;
            }
        written = 0;
        for (auto* filter : {&inputLow[0], &inputLow[1], &inputHigh[0], &inputHigh[1], &loopLow[0], &loopLow[1], &loopHigh[0], &loopHigh[1]})
            filter->reset();
        fade = 0;
        quiet = 0;
        feedback = target.feedback;
        width = target.width;
        current = next = timeInSamples(target.seconds);
    }
    // Settings may change every block; times retime with a crossfade.
    void set(const Settings& settings) noexcept
    {
        if (settings.lowCutHz != target.lowCutHz || settings.highCutHz != target.highCutHz || !filtersSet)
        {
            for (int side = 0; side < 2; ++side)
            {
                inputLow[side].set(dsp_core::Svf::Type::highpass, settings.lowCutHz, rate);
                inputHigh[side].set(dsp_core::Svf::Type::lowpass, settings.highCutHz, rate);
                loopLow[side].set(dsp_core::Svf::Type::highpass, settings.lowCutHz, rate);
                loopHigh[side].set(dsp_core::Svf::Type::lowpass, settings.highCutHz, rate);
            }
            filtersSet = true;
        }
        target = settings;
        target.feedback = std::clamp(settings.feedback, 0.0f, 0.95f);
        target.width = std::clamp(settings.width, 0.0f, 1.0f);
        const auto wanted = timeInSamples(settings.seconds);
        // A time change while silent takes effect at once; while sounding it
        // crossfades, and a change during a fade waits for its end.
        if (wanted != next && fade == 0) { next = wanted; if (written == 0) current = next; else fade = fadeSamples; }
    }
    struct Stereo { float left = 0.0f, right = 0.0f; };
    Stereo process(float inLeft, float inRight) noexcept
    {
        feedback += smoothing * (target.feedback - feedback);
        width += smoothing * (target.width - width);
        inLeft = inputHigh[0].next(inputLow[0].next(inLeft));
        inRight = inputHigh[1].next(inputLow[1].next(inRight));
        Stereo out{read(0, current), read(1, current)};
        if (fade > 0)
        {
            // Equal power: the old and new echoes are different moments of
            // the music, so their powers add.
            const auto position01 = 1.0f - static_cast<float>(fade) / static_cast<float>(fadeSamples);
            const auto in = std::sin(1.5707963f * position01), outWeight = std::cos(1.5707963f * position01);
            out.left = outWeight * out.left + in * read(0, next);
            out.right = outWeight * out.right + in * read(1, next);
            if (--fade == 0) current = next;
        }
        const auto backLeft = feedback * loopHigh[0].next(loopLow[0].next(out.left));
        const auto backRight = feedback * loopHigh[1].next(loopLow[1].next(out.right));
        // Ping-pong: the input enters on the left and each repeat crosses
        // sides; stereo: each side repeats on itself.
        const auto writeLeft = target.pingPong ? 0.5f * (inLeft + inRight) + backRight : inLeft + backLeft;
        const auto writeRight = target.pingPong ? backLeft : inRight + backRight;
        lines[0][position] = flush(writeLeft);
        lines[1][position] = flush(writeRight);
        position = position + 1 == lines[0].size() ? 0 : position + 1;
        written = std::min(written + 1, lines[0].size());
        const auto loud = std::max({std::abs(writeLeft), std::abs(writeRight), std::abs(out.left), std::abs(out.right)});
        quiet = loud < 1.0e-7f ? quiet + 1 : 0;
        // Width on the echoes alone: 0 folds them to the centre.
        const auto mid = 0.5f * (out.left + out.right), side = 0.5f * width * (out.left - out.right);
        return {mid + side, mid - side};
    }
    // Nothing left in the lines worth hearing: every sample written over the
    // longest current read time was below -140 dBFS.
    bool silent() const noexcept { return written == 0 || quiet > static_cast<std::size_t>(std::max(current, next)) + static_cast<std::size_t>(fadeSamples); }
    int delaySamples() const noexcept { return current; }

private:
    int timeInSamples(double seconds) const noexcept
    {
        seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.001, maximumSeconds) : 0.375;
        return std::clamp(static_cast<int>(std::lround(seconds * rate)), 1, static_cast<int>(lines[0].empty() ? 1 : lines[0].size() - 1));
    }
    float read(int side, int delay) const noexcept
    {
        const auto size = lines[0].size();
        const auto at = position >= static_cast<std::size_t>(delay) ? position - static_cast<std::size_t>(delay)
                                                                    : position + size - static_cast<std::size_t>(delay);
        return lines[static_cast<std::size_t>(side)][at];
    }
    static float flush(float value) noexcept { return std::abs(value) < 1.0e-20f ? 0.0f : value; }
    std::vector<float> lines[2];
    dsp_core::Svf inputLow[2], inputHigh[2], loopLow[2], loopHigh[2];
    Settings target{};
    double rate = 48000.0;
    std::size_t position = 0, written = 0, quiet = 0;
    int current = 1, next = 1, fade = 0, fadeSamples = 1440;
    float feedback = 0.25f, width = 1.0f, smoothing = 0.004f;
    bool filtersSet = false;
};
}
