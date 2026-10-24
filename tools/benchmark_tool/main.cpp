#include "dsp/Engine.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>

namespace
{
bool readInteger(std::string_view text, int& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
}

// Instrument workload only: no audio device, renderer, GUI or file I/O.
int main(int argc, char** argv)
{
    using Clock = std::chrono::steady_clock;
    constexpr int trials = 5, maximumBlockSize = 8192;
    int blockSize = 512, sampleRate = 48000;
    bool haveRate = false, haveBlock = false;
    for (int i = 1; i < argc; i += 2)
    {
        const std::string_view option = argv[i];
        if (i + 1 >= argc
            || (option != "--sample-rate" && option != "--block-size")
            || (option == "--sample-rate" && (haveRate || !readInteger(argv[i + 1], sampleRate)))
            || (option == "--block-size" && (haveBlock || !readInteger(argv[i + 1], blockSize))))
        {
            std::cerr << "Usage: benchmark_tool [--sample-rate 44100..192000] [--block-size 1..8192]\n";
            return 2;
        }
        if (option == "--sample-rate") haveRate = true;
        else haveBlock = true;
    }
    if (sampleRate < 44100 || sampleRate > 192000 || blockSize < 1 || blockSize > maximumBlockSize)
    {
        std::cerr << "Sample rate or block size outside supported benchmark range.\n";
        return 2;
    }
    const double rate = sampleRate;
    // Keep roughly ten measured/two warmup seconds at every setting. The default
    // remains exactly 938/188 blocks, preserving the original comparison workload.
    const int blocks = static_cast<int>(std::ceil(10.0 * rate / blockSize));
    const int warmupBlocks = static_cast<int>(std::ceil(2.0 * rate / blockSize));
    juce::ScopedNoDenormals noDenormals;
    std::array<float, maximumBlockSize> left{}, right{};
    std::array<double, trials> elapsed{};
    double peak = 0.0, energy = 0.0, worstBlockMs = 0.0;
    int overDeadline = 0;
    for (int trial = 0; trial < trials; ++trial)
    {
        dropforge::Engine engine;
        engine.setLayersEnabled(true, true);
        engine.prepare(rate);
        engine.setBassParameters(140.0f, 1.0f);
        engine.setRhythmParameters(1.0f, 0.5f);
        for (int note : {40, 47, 52, 55, 59, 64, 67, 71}) engine.noteOn(1, note, 127);
        const auto targets = engine.rhythmTargets();
        if (std::count_if(targets.begin(), targets.end(), [](auto v) { return v.held; }) != 8
            || engine.bassTarget().note != 40) return 1;
        for (int i = 0; i < warmupBlocks; ++i) engine.process(left.data(), right.data(), blockSize);
        double seconds = 0.0;
        for (int i = 0; i < blocks; ++i)
        {
            const auto start = Clock::now();
            engine.process(left.data(), right.data(), blockSize);
            const auto duration = std::chrono::duration<double>(Clock::now() - start).count();
            seconds += duration;
            worstBlockMs = std::max(worstBlockMs, duration * 1000.0);
            if (duration > blockSize / rate) ++overDeadline;
            // Validate and consume every sample outside the timed section.
            for (int j = 0; j < blockSize; ++j)
            {
                const double l = left[j], r = right[j];
                if (!std::isfinite(l) || !std::isfinite(r)) return 1;
                peak = std::max({peak, std::abs(l), std::abs(r)});
                energy += l * l + r * r;
            }
        }
        elapsed[trial] = seconds;
    }
    if (!(energy > 0.0) || !std::isfinite(energy)) return 1;
    const auto audioSeconds = blocks * blockSize / rate;
    std::cout << "{\n  \"sampleRate\": " << sampleRate << ", \"blockSize\": " << blockSize
        << ", \"rhythmVoices\": 8,"
        " \"bassVoices\": 1, \"destruction\": 1, \"tight\": 0.5, \"splitHz\": 140,"
        " \"velocity\": 127,\n  \"notes\": [40,47,52,55,59,64,67,71],"
        "\n  \"warmupBlocks\": " << warmupBlocks << ", \"blocksPerTrial\": " << blocks
        << ", \"audioSecondsPerTrial\": " << audioSeconds << ",\n  \"trialCpuPercent\": [";
    for (int i = 0; i < trials; ++i)
        std::cout << (i ? ", " : "") << elapsed[i] / audioSeconds * 100.0;
    std::sort(elapsed.begin(), elapsed.end());
    std::cout << "],\n  \"medianCpuPercent\": " << elapsed[trials / 2] / audioSeconds * 100.0
        << ", \"medianBlockMs\": " << elapsed[trials / 2] / blocks * 1000.0
        << ",\n  \"worstBlockMs\": " << worstBlockMs << ", \"blocksOverDeadline\": " << overDeadline
        << ", \"peak\": " << peak << ", \"energy\": " << energy << "\n}\n";
}
