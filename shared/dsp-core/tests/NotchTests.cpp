#include <dsp_core/NotchFilter.h>
#include <cmath>
#include <complex>
#include <iostream>

int main()
{
    constexpr double pi = 3.14159265358979323846;
    double worst = 0.0;
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
        for (const auto center : {35.0, 400.0, 6000.0})
            for (const auto hz : {center*0.5, center, center*2.0})
            {
                dsp_core::NotchFilter filter;
                filter.prepare(center, rate);
                std::complex<double> sum{};
                // Two seconds leave whole periods in the measurement window.
                for (int i = 0; i < rate*2; ++i)
                {
                    const auto angle = 2.0*pi*hz*i/rate;
                    const auto value = filter.next(std::sin(angle));
                    if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL) return 1;
                    if (i >= rate) sum += value * std::polar(1.0, -angle);
                }
                const auto r = std::tan(pi*hz/rate)/std::tan(pi*center/rate);
                const auto expected = (1.0-r*r)/std::complex<double>(1.0-r*r, r/0.7);
                const auto error = std::abs(sum*std::complex<double>(0.0, 2.0/rate)-expected);
                worst = std::max(worst, error);
                if (error > 1e-7) { std::cerr << "notch_error=" << error << " rate=" << rate << '\n'; return 1; }
            }
    std::cout << "notch_complex_error=" << worst << " rates=6 roots=35,400,6000\n";
}
