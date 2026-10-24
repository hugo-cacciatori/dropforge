#include <dsp_core/PeakingEq.h>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>

int main()
{
    constexpr double pi = 3.14159265358979323846;
    double worst = 0.0;
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
        for (const auto db : {-6.0, 0.0, 6.0})
            for (const auto hz : {100.0, 500.0, 1800.0, 4000.0, 9000.0})
            {
                dsp_core::PeakingEq eq;
                eq.prepare(1800.0, db, 0.7, rate);
                std::complex<double> sum{};
                for (int i = 0; i < rate; ++i)
                {
                    const auto angle = 2.0 * pi * hz * i / rate;
                    const auto input = std::sin(angle);
                    const auto output = eq.next(input);
                    if (db == 0.0 && output != input) return 1;
                    if (i >= rate / 2) sum += output * std::polar(1.0, -angle);
                }
                // Independently evaluate the prewarped analog pole/zero ratio.
                const auto r = std::tan(pi * hz / rate) / std::tan(pi * 1800.0 / rate);
                const auto a = std::pow(10.0, db / 40.0);
                const auto expected = std::complex<double>(1.0-r*r, r*a/0.7)
                                    / std::complex<double>(1.0-r*r, r/(a*0.7));
                const auto error = std::abs(sum * std::complex<double>(0.0, 4.0/rate) - expected);
                worst = std::max(worst, error);
                if (error > 1e-8)
                {
                    std::cerr << "response rate=" << rate << " db=" << db << " hz=" << hz
                              << " error=" << error << " expected=" << expected
                              << " measured=" << sum * std::complex<double>(0.0, 4.0/rate) << '\n';
                    return 1;
                }
                eq.reset();
                for (int i = 0; i < 10000; ++i)
                {
                    const auto output = eq.next(i == 0 ? 1.0 : 0.0);
                    if (!std::isfinite(output) || std::fpclassify(output) == FP_SUBNORMAL) return 1;
                }
                // Quiet-state rounding may leave a bounded 1e-30 residue;
                // the instrument's existing release gate supplies exact zero.
                if (std::abs(eq.next(0.0)) > 1e-28) return 1;
            }
    std::cout << "peaking_eq_complex_error=" << worst << " neutral=exact rates=6\n";
}
