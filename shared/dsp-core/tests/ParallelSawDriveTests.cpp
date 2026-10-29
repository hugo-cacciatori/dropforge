#include <dsp_core/ParallelSawDrive.h>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>

// The grit drive curve Anvil's accepted grit (candidate A) runs at 4x.
namespace
{
constexpr double twoPi = 6.28318530717958647692;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main()
{
    std::complex<double> even{};
    double floatError = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        const auto angle = twoPi * 200.0 * i / 48000.0;
        const auto input = 0.6 * std::sin(angle);
        require(dsp_core::parallelSawDrive(input, 0.0) == input, "parallel drive has exact linear bypass");
        const auto output = dsp_core::parallelSawDrive(input, 0.5);
        even += output * std::polar(1.0, -2.0 * angle);
        for (const auto amount : {0.0, 0.5, 0.65, 1.0})
        {
            const auto value = dsp_core::parallelSawDrive(input, amount);
            require(std::isfinite(value) && std::abs(value) < 1.0, "parallel drive is bounded over exciter input");
            // The float curve Anvil runs is the same curve.
            floatError = std::max(floatError, std::abs(value - static_cast<double>(dsp_core::parallelSawDrive(
                                                   static_cast<float>(input), static_cast<float>(amount)))));
        }
        require(std::abs(dsp_core::parallelSawDrive(input, 0.65 - 1e-7)
                         - dsp_core::parallelSawDrive(input, 0.65 + 1e-7)) < 1e-6,
                "saw fold enters continuously");
    }
    const auto evenAmplitude = 2.0 * std::abs(even) / 48000.0;
    require(evenAmplitude > 0.001, "asymmetric grit creates even harmonics rather than only odd square-like partials");
    require(floatError < 1.0e-6, "float and double curves agree");
    for (const auto input : {1e308, -1e308, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        require(std::isfinite(dsp_core::parallelSawDrive(input, 1.0)), "parallel drive contains invalid and extreme inputs");
    std::cout << "parallel_saw_even_harmonic_amplitude=" << evenAmplitude << " float_error=" << floatError << " bypass=exact\n";
}
