#include "support/StereoMetrics.h"
#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}
int main()
{
    constexpr int frames = 65536, rate = 65536;
    constexpr double tau = 6.28318530717958647692;
    std::vector<float> left(frames), right(frames), bass(frames);
    for (int i = 0; i < frames; ++i)
    {
        // Exact FFT bins independently establish reference power and ratio.
        left[i] = right[i] = static_cast<float>(0.01 * std::sin(tau * 50 * i / rate)
                                             + 0.1 * std::sin(tau * 1000 * i / rate));
        bass[i] = static_cast<float>(0.1 * std::sin(tau * 50 * i / rate));
    }
    auto m = dsp_core::test::measure(left, right, bass, rate, 140);
    require(std::abs(m.correlation - 1.0) < 1.0e-9 && std::abs(m.foldLossDb) < 1.0e-9,
            "identical channels retain full correlation and zero fold loss");
    require(std::abs(m.separationDb + 20.0) < 1.0e-5
            && std::abs(m.lowBassDbfs - 10.0 * std::log10(0.005)) < 1.0e-5,
            "one-sided Parseval normalization and low-band ratio match analytic sines");
    require(std::abs(m.foldBandDbfs - 10.0 * std::log10(0.005)) < 1.0e-5,
            "fold-band absolute power excludes the low sine");
    for (auto& sample : right) sample = -sample;
    m = dsp_core::test::measure(left, right, bass, rate, 140);
    require(m.correlation < -0.999999 && m.foldLossDb > 200.0,
            "opposed channels cancel in host fold-down");
    for (int i = 0; i < frames; ++i)
    {
        left[i] = static_cast<float>(std::sin(tau * 1000 * i / rate));
        right[i] = static_cast<float>(std::cos(tau * 1000 * i / rate));
    }
    m = dsp_core::test::measure(left, right, bass, rate, 140);
    require(std::abs(m.correlation) < 1.0e-9 && std::abs(m.foldLossDb - 10.0 * std::log10(2.0)) < 1.0e-8,
            "quadrature gives zero correlation and 3.0103 dB fold loss");
    // Padding must preserve absolute energy for non-power-of-two renders.
    left.resize(48000); right = left; bass = left;
    m = dsp_core::test::measure(left, right, bass, 48000, 24000);
    double power = 0.0;
    for (auto value : left) power += value * static_cast<double>(value) / left.size();
    require(std::abs(m.lowBassDbfs - dsp_core::test::powerDb(power)) < 0.001,
            "zero padding does not change measured power");
    std::cout << "stereo_metrics analytic_phase_band_power_padding=passed\n";
}
