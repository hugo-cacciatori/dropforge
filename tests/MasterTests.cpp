#include "plugin/InstrumentCore.h"
#include <dsp_core/LoudnessMeter.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Render { std::vector<float> left, right; };
Render render(dropforge::InstrumentCore& core, int samples, int block)
{
    Render out{std::vector<float>(static_cast<std::size_t>(samples)), std::vector<float>(static_cast<std::size_t>(samples))};
    for (int at = 0; at < samples; at += block)
        core.process(out.left.data() + at, out.right.data() + at, std::min(block, samples - at));
    return out;
}
void noteOn(dropforge::InstrumentCore& core, int note, int velocity)
{
    const std::uint8_t bytes[]{0x90, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity)};
    core.midi(bytes, 3);
}
// The loudness reference (SPEC section 4): E minor, E2 B2 E3 G3, held.
constexpr int referenceChord[]{40, 47, 52, 55};

void loudnessTarget()
{
    dropforge::InstrumentCore core;
    core.prepare(48000.0);
    for (const auto note : referenceChord) noteOn(core, note, 127);
    const auto audio = render(core, 5 * 48000, 512);
    auto meter = std::make_unique<dsp_core::LoudnessMeter>();
    meter->prepare(48000.0);
    float peak = 0.0f;
    for (std::size_t i = 0; i < audio.left.size(); ++i)
    {
        meter->process(audio.left[i], audio.right[i]);
        peak = std::max({peak, std::abs(audio.left[i]), std::abs(audio.right[i])});
    }
    const auto meters = core.takeMeters();
    std::cout << "reference_chord short_term_max_lufs=" << meter->shortTermMaxLufs()
              << " peak_dbfs=" << 20.0 * std::log10(peak) << " last_block_glue_db=" << meters.glueReductionDb
              << " last_block_limiter_db=" << meters.limiterReductionDb << '\n';
    require(std::abs(meter->shortTermMaxLufs() - (-16.0)) < 0.5, "reference chord reaches -16 LUFS short-term within 0.5 LU");
}

void ceilingAndDeterminism()
{
    float worst = 0.0f;
    for (const auto rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        // Everything at maximum: eight notes, both layer Levels +6 dB, hard
        // retriggered stabs. Output 0 dB must still never pass -1 dBFS.
        const auto stress = [rate](int block)
        {
            dropforge::InstrumentCore core;
            core.setLayerLevels(6.0f, 6.0f);
            core.prepare(rate);
            Render all;
            for (int hit = 0; hit < 6; ++hit)
            {
                for (const auto note : {28, 35, 40, 47, 52, 55, 59, 64}) noteOn(core, note, 127);
                auto part = render(core, static_cast<int>(rate / 8), block);
                all.left.insert(all.left.end(), part.left.begin(), part.left.end());
                all.right.insert(all.right.end(), part.right.begin(), part.right.end());
                core.panic();
                part = render(core, static_cast<int>(rate / 50), block);
                all.left.insert(all.left.end(), part.left.begin(), part.left.end());
                all.right.insert(all.right.end(), part.right.begin(), part.right.end());
            }
            return all;
        };
        const auto a = stress(512), b = stress(1), c = stress(73);
        require(a.left == b.left && a.right == b.right && a.left == c.left && a.right == c.right,
                "master output is identical at block sizes 1, 73 and 512");
        for (std::size_t i = 0; i < a.left.size(); ++i)
        {
            require(std::isfinite(a.left[i]) && std::isfinite(a.right[i]), "master output is finite");
            worst = std::max({worst, std::abs(a.left[i]), std::abs(a.right[i])});
        }
    }
    std::cout << "stress_worst_peak_dbfs=" << 20.0 * std::log10(worst) << '\n';
    require(worst <= dropforge::Master::ceiling, "sample peak never above -1 dBFS at Output 0 dB");
}

void metersAndOutput()
{
    dropforge::InstrumentCore core;
    core.prepare(48000.0, -24.0f);
    for (const auto note : referenceChord) noteOn(core, note, 127);
    render(core, 24000, 512);
    core.takeMeters();
    const auto audio = render(core, 4800, 4800);
    const auto meters = core.takeMeters();
    float left = 0.0f, right = 0.0f;
    for (std::size_t i = 0; i < audio.left.size(); ++i)
    {
        left = std::max(left, std::abs(audio.left[i]));
        right = std::max(right, std::abs(audio.right[i]));
    }
    require(meters.peakLeft == left && meters.peakRight == right, "meter peaks are the block's output peaks");
    require(left > 0.0f && left < std::pow(10.0f, -20.0f / 20.0f), "Output reaches -24 dB");
    const auto cleared = core.takeMeters();
    require(cleared.peakLeft == 0.0f && cleared.limiterReductionDb == 0.0f, "taking the meters clears them");
    std::cout << "output_minus_24_peak_dbfs=" << 20.0 * std::log10(left) << '\n';
}
}

int main()
{
    loudnessTarget();
    ceilingAndDeterminism();
    metersAndOutput();
}
