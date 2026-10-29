#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace dsp_core
{
// Fixed 4x FIR interpolation -> caller's nonlinear stage -> FIR decimation.
// The two symmetric 129-tap filters contribute exactly 32 base-rate samples.
// No buffer size dependence, dynamic storage, or host/GUI dependency.
// Sample is double for the bit-stable legacy paths, float for 32-bit chains;
// the kernel is always designed in double, then stored as Sample.
template <typename Sample>
class BasicOversampler4x
{
public:
    static constexpr std::size_t factor = 4, kernelSize = 129;
    static constexpr int latencySamples = static_cast<int>((kernelSize - 1) / factor);
    using Kernel = std::array<Sample, kernelSize>;

    void prepare() noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        // Cutoff is 0.4 * base rate (0.1 * 4x rate). The transition band
        // trades ultrasonic bandwidth for measured rejection above base Nyquist.
        std::array<double, kernelSize> design{};
        double sum = 0.0;
        for (std::size_t i = 0; i < kernelSize; ++i)
        {
            const auto offset = static_cast<double>(i) - (kernelSize - 1) / 2.0;
            const auto position = 2.0 * pi * static_cast<double>(i) / (kernelSize - 1);
            const auto window = 0.42 - 0.5 * std::cos(position) + 0.08 * std::cos(2.0 * position);
            const auto sinc = offset == 0.0 ? 0.2 : std::sin(0.2 * pi * offset) / (pi * offset);
            design[i] = i == 0 || i == kernelSize - 1 ? 0.0 : window * sinc;
            sum += design[i];
        }
        for (std::size_t i = 0; i < kernelSize; ++i) kernel[i] = static_cast<Sample>(design[i] / sum);
        reset();
    }
    void reset() noexcept
    {
        inputHistory.fill(Sample{});
        outputHistory.fill(Sample{});
        inputPosition = outputPosition = 0;
    }
    const Kernel& coefficients() const noexcept { return kernel; }

    // Shape must be allocation-free, lock-free and noexcept. It is called for
    // every one of the four interpolated samples, including at zero drive.
    template <typename Shape>
    Sample next(Sample input, Shape&& shape) noexcept
    {
        // Each history is stored twice, back to back, so every filter window
        // is one straight run of memory: no wrap test per tap. The sums keep
        // their original order, so results are unchanged bit for bit.
        const auto sample = safe(input);
        inputHistory[inputPosition] = inputHistory[inputPosition + inputLength] = sample;
        const auto* newestInput = inputHistory.data() + inputPosition + inputLength;
        Sample result{};
        for (std::size_t phase = 0; phase < factor; ++phase)
        {
            Sample interpolated{};
            if constexpr (std::is_same_v<Sample, float>)
            {
                // Same four-lane split as the decimation below (float only).
                Sample sums[4]{};
                std::size_t j = 0, tap = phase;
                for (; tap + 3 * factor < kernelSize; tap += 4 * factor, j += 4)
                    for (std::size_t lane = 0; lane < 4; ++lane)
                        sums[lane] += kernel[tap + lane * factor] * newestInput[-static_cast<std::ptrdiff_t>(j + lane)];
                for (; tap < kernelSize; tap += factor, ++j) sums[0] += kernel[tap] * newestInput[-static_cast<std::ptrdiff_t>(j)];
                interpolated = (sums[0] + sums[1]) + (sums[2] + sums[3]);
            }
            else
            {
                const auto* older = newestInput;
                for (std::size_t tap = phase; tap < kernelSize; tap += factor)
                    interpolated += kernel[tap] * *older--;
            }
            const auto shaped = safe(shape(interpolated * static_cast<Sample>(factor)));
            outputHistory[outputPosition] = outputHistory[outputPosition + kernelSize] = shaped;
            // Decimate at phase zero, not the last phase: this keeps the FIR
            // group delay an integer and avoids a fractional dry-path mismatch.
            if (phase == 0)
            {
                const auto* newestOutput = outputHistory.data() + outputPosition + kernelSize;
                if constexpr (std::is_same_v<Sample, float>)
                {
                    // Four partial sums let the CPU overlap the multiply-adds
                    // (float only; the double path keeps its exact order).
                    Sample sums[4]{};
                    std::size_t i = 0;
                    for (; i + 4 <= kernelSize; i += 4)
                        for (std::size_t lane = 0; lane < 4; ++lane)
                            sums[lane] += kernel[i + lane] * newestOutput[-static_cast<std::ptrdiff_t>(i + lane)];
                    for (; i < kernelSize; ++i) sums[0] += kernel[i] * newestOutput[-static_cast<std::ptrdiff_t>(i)];
                    result = (sums[0] + sums[1]) + (sums[2] + sums[3]);
                }
                else
                    for (std::size_t i = 0; i < kernelSize; ++i)
                        result += kernel[i] * newestOutput[-static_cast<std::ptrdiff_t>(i)];
            }
            if (++outputPosition == kernelSize) outputPosition = 0;
        }
        if (++inputPosition == inputLength) inputPosition = 0;
        return safe(result);
    }
private:
    static constexpr std::size_t inputLength = (kernelSize + factor - 1) / factor;
    static Sample safe(Sample value) noexcept
    {
        return std::isfinite(value) && std::abs(value) >= static_cast<Sample>(1.0e-30) ? value : Sample{};
    }
    Kernel kernel{};
    std::array<Sample, 2 * kernelSize> outputHistory{};
    std::array<Sample, 2 * inputLength> inputHistory{};
    std::size_t inputPosition = 0, outputPosition = 0;
};
using Oversampler4x = BasicOversampler4x<double>;
using Oversampler4xFloat = BasicOversampler4x<float>;
}
