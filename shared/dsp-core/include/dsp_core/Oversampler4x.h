#pragma once

#include <array>
#include <cmath>
#include <cstddef>

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
        inputHistory[inputPosition] = safe(input);
        Sample result{};
        for (std::size_t phase = 0; phase < factor; ++phase)
        {
            Sample interpolated{};
            auto position = inputPosition;
            for (std::size_t tap = phase; tap < kernelSize; tap += factor)
            {
                interpolated += kernel[tap] * inputHistory[position];
                position = previous(position, inputHistory.size());
            }
            outputHistory[outputPosition] = safe(shape(interpolated * static_cast<Sample>(factor)));
            // Decimate at phase zero, not the last phase: this keeps the FIR
            // group delay an integer and avoids a fractional dry-path mismatch.
            if (phase == 0)
            {
                position = outputPosition;
                for (const auto coefficient : kernel)
                {
                    result += coefficient * outputHistory[position];
                    position = previous(position, outputHistory.size());
                }
            }
            if (++outputPosition == outputHistory.size()) outputPosition = 0;
        }
        if (++inputPosition == inputHistory.size()) inputPosition = 0;
        return safe(result);
    }
private:
    static std::size_t previous(std::size_t position, std::size_t size) noexcept
    {
        return position == 0 ? size - 1 : position - 1;
    }
    static Sample safe(Sample value) noexcept
    {
        return std::isfinite(value) && std::abs(value) >= static_cast<Sample>(1.0e-30) ? value : Sample{};
    }
    Kernel kernel{}, outputHistory{};
    std::array<Sample, (kernelSize + factor - 1) / factor> inputHistory{};
    std::size_t inputPosition = 0, outputPosition = 0;
};
using Oversampler4x = BasicOversampler4x<double>;
using Oversampler4xFloat = BasicOversampler4x<float>;
}
