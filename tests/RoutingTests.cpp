#include "plugin/InstrumentCore.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Stereo
{
    std::vector<float> left, right;
    explicit Stereo(int samples) : left(static_cast<std::size_t>(samples)), right(left.size()) {}
};

void midi(dropforge::InstrumentCore& core, std::array<std::uint8_t, 3> bytes)
{
    core.midi(bytes.data(), static_cast<int>(bytes.size()));
}

void phraseEvent(dropforge::InstrumentCore& core, int block)
{
    if (block == 0)
        for (const auto note : {45, 52, 57, 60, 64, 69, 96, 127})
            midi(core, {0x90, static_cast<std::uint8_t>(note), 127});
    if (block == 1) { core.setOutputDb(-9.0f); midi(core, {0xe0, 127, 127}); midi(core, {0x90, 28, 127}); }
    if (block == 3) { midi(core, {0x90, 28, 64}); midi(core, {0x9f, 28, 127}); }
    if (block == 5) { core.setOutputDb(6.0f); midi(core, {0x90, 28, 0}); }
    if (block == 6) midi(core, {0x8f, 28, 0}); // Reveal the original higher root again.
    if (block == 7) { midi(core, {0xb0, 64, 127}); core.panic(); }
}

Stereo phrase(double rate, dropforge::LayerMask mask, bool changingMasks = false, bool useTaps = false)
{
    constexpr std::array<int, 10> sizes{1, 17, 0, 512, 3, 1024, 7, 257, 2048, 2048};
    int total = 0;
    for (const auto size : sizes) total += size;
    Stereo output(total);
    dropforge::InstrumentCore core;
    core.prepare(rate, 0.0f, 140.0f, 0.5f);
    int position = 0;
    for (std::size_t block = 0; block < sizes.size(); ++block)
    {
        phraseEvent(core, static_cast<int>(block));
        const auto count = sizes[block];
        Stereo bass(count), rhythm(count);
        const auto effectiveMask = changingMasks && block < 6 ? dropforge::LayerMask::none : mask;
        core.process(output.left.data() + position, output.right.data() + position, count, effectiveMask,
                     useTaps ? dropforge::LayerTaps{bass.left.data(), bass.right.data(), rhythm.left.data(), rhythm.right.data()}
                             : dropforge::LayerTaps{});
        if (useTaps)
            for (int i = 0; i < count; ++i)
                require(bass.left[static_cast<std::size_t>(i)] == bass.right[static_cast<std::size_t>(i)],
                        "audible bass taps stay exactly centered");
        position += count;
    }
    return output;
}

void isolationTests()
{
    double maxError = 0.0;
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        const auto full = phrase(rate, dropforge::LayerMask::both);
        const auto bass = phrase(rate, dropforge::LayerMask::bass);
        const auto rhythm = phrase(rate, dropforge::LayerMask::rhythm);
        const auto silent = phrase(rate, dropforge::LayerMask::none);
        const auto tapped = phrase(rate, dropforge::LayerMask::both, false, true);
        const auto resumed = phrase(rate, dropforge::LayerMask::both, true, false);
        require(full.left == tapped.left && full.right == tapped.right, "enabling taps is audio-neutral");
        for (std::size_t i = 0; i < full.left.size(); ++i)
        {
            maxError = std::max({maxError,
                std::abs(static_cast<double>(full.left[i]) - (bass.left[i] + rhythm.left[i])),
                std::abs(static_cast<double>(full.right[i]) - (bass.right[i] + rhythm.right[i]))});
            require(std::abs(full.left[i] - (bass.left[i] + rhythm.left[i])) < 2.0e-7f
                    && std::abs(full.right[i] - (bass.right[i] + rhythm.right[i])) < 2.0e-7f,
                    "isolated contributions reconstruct full output within float rounding after master gain");
            require(bass.left[i] == bass.right[i] && silent.left[i] == 0.0f && silent.right[i] == 0.0f,
                    "bass is centered and omitted contributions are exactly silent");
            if (i >= 1557) // The first six blocks were hidden, but all state must have advanced.
                require(resumed.left[i] == full.left[i] && resumed.right[i] == full.right[i],
                        "mask changes preserve voice, bend, envelope and gain-ramp history");
        }
        require(std::any_of(full.left.begin(), full.left.end(), [](float sample) { return sample != 0.0f; }),
                "sum/isolation exercises an active rhythm prototype");
        require(std::any_of(bass.left.begin(), bass.left.end(), [](float sample) { return sample != 0.0f; }),
                "sum/isolation exercises audible bass");
    }
    std::cout << "isolation_cases=12 rates=6 policies=legacy,Both max_post_master_error=" << maxError << '\n';

    dropforge::Engine engine;
    engine.prepare(48000);
    engine.noteOn(1, 69, 127);
    Stereo full(512), bass(512), rhythm(512);
    engine.process(full.left.data(), full.right.data(), 512, dropforge::LayerMask::both,
                   {bass.left.data(), bass.right.data(), rhythm.left.data(), rhythm.right.data()});
    for (std::size_t i = 0; i < full.left.size(); ++i)
        require(full.left[i] == bass.left[i] + rhythm.left[i] && full.right[i] == bass.right[i] + rhythm.right[i],
                "pre-master taps exactly reconstruct engine output");
    auto reference = engine;
    Stereo hidden(512), expected(512), partial(512);
    engine.process(hidden.left.data(), hidden.right.data(), 512, dropforge::LayerMask::none,
                   {nullptr, nullptr, nullptr, partial.right.data()});
    reference.process(expected.left.data(), expected.right.data(), 512, dropforge::LayerMask::rhythm);
    require(partial.right == expected.right, "partial taps observe the contribution even when output is masked");
    require(std::all_of(hidden.left.begin(), hidden.left.end(), [](float sample) { return sample == 0.0f; }),
            "a diagnostic tap does not leak the masked contribution into output");
}

void finiteMatrix()
{
    constexpr std::array<int, 9> sizes{0, 1, 3, 17, 64, 127, 512, 1024, 7};
    constexpr std::array<dropforge::LayerMask, 4> masks{dropforge::LayerMask::none, dropforge::LayerMask::bass,
                                                    dropforge::LayerMask::rhythm, dropforge::LayerMask::both};
    const std::array<float, 7> gains{-12.0f, 0.0f, 6.0f, -100.0f, 100.0f,
                                    std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    std::size_t cases = 0;
    double peak = 0.0;
    dropforge::InstrumentCore core;
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
        for (const auto mask : masks)
            for (const auto initialDb : gains)
            {
                core.prepare(rate, initialDb);
                for (int note = 0; note <= 127; ++note)
                {
                    const auto count = sizes[static_cast<std::size_t>(note) % sizes.size()];
                    const auto channel = note % 2 == 0 ? 0 : 15;
                    const auto velocity = note % 3 == 0 ? 1 : (note % 3 == 1 ? 64 : 127);
                    midi(core, {static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note),
                                static_cast<std::uint8_t>(velocity)});
                    const auto wheel = note % 3 == 0 ? 0 : (note % 3 == 1 ? 8192 : 16383);
                    midi(core, {static_cast<std::uint8_t>(0xe0 | channel), static_cast<std::uint8_t>(wheel & 127),
                                static_cast<std::uint8_t>(wheel >> 7)});
                    core.setOutputDb(gains[static_cast<std::size_t>(note) % gains.size()]);
                    std::array<std::array<float, 1026>, 6> buffers;
                    for (auto& buffer : buffers) buffer.fill(123.0f);
                    // Guard every tap buffer as well as the mixed output.
                    core.process(buffers[0].data() + 1, buffers[1].data() + 1, count, mask,
                                 {buffers[2].data() + 1, buffers[3].data() + 1, buffers[4].data() + 1, buffers[5].data() + 1});
                    for (const auto& buffer : buffers)
                    {
                        require(buffer.front() == 123.0f && buffer[static_cast<std::size_t>(count + 1)] == 123.0f,
                                "output and taps write only the requested samples, including zero/one callbacks");
                        for (int i = 1; i <= count; ++i)
                        {
                            const auto value = buffer[static_cast<std::size_t>(i)];
                            require(std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL && std::abs(value) < 2.0f,
                                    "finite, normal, bounded samples across notes/rates/gains/bends/masks");
                            peak = std::max(peak, std::abs(static_cast<double>(value)));
                        }
                    }
                    midi(core, {static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(note), 0});
                    ++cases;
                }
                core.panic();
                Stereo tail(4096);
                core.process(tail.left.data(), tail.right.data(), 4096, mask);
                require(tail.left.back() == 0.0f && tail.right.back() == 0.0f, "panic settles to silence at every rate/mask/gain");
            }
    std::cout << "finite_matrix_cases=" << cases << " rates=6 masks=4 notes=0..127 velocities=1,64,127 peak=" << peak << '\n';
}
}

int main()
{
    isolationTests();
    finiteMatrix();
    std::cout << "pre_master_sum=exact post_master_sum_error<2e-7 taps=audio_neutral mask_resume=exact bass=audible_centered\n";
}
