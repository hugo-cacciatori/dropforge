#include <dsp_core/PolyBlepSaw.h>
#include <dsp_core/PolyBlepPulse.h>
#include <complex>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

double magnitude(const std::vector<double>& audio, double hz)
{
    std::complex<double> result{};
    for (std::size_t i = 0; i < audio.size(); ++i)
        result += audio[i] * std::polar(1.0, -6.28318530717958647692 * hz * i / 48000.0);
    return std::abs(result) / audio.size();
}

int main()
{
    dsp_core::PolyBlepSaw reference, seeded;
    seeded.reset(1.25);
    for (int i = 0; i < 12; ++i) reference.next(1000, 48000);
    for (int i = 0; i < 2000; ++i)
        if (std::abs(reference.next(1000, 48000) - seeded.next(1000, 48000)) > 1.0e-12) return 1;
    for (const auto phase : {0.0, -1.0, 2.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        reference.reset(); seeded.reset(phase);
        for (int i = 0; i < 2000; ++i)
            if (reference.next(11000, 48000) != seeded.next(11000, 48000)) return 1;
    }
    std::cout << "phase_seed=wrapped invalid_phase=zero legacy_reset=exact\n";
    dsp_core::PolyBlepPulse pulse, repeat;
    pulse.reset(0.25); repeat.reset(1.25);
    for (int i = 0; i < 48000; ++i)
    {
        const auto width = 0.5 + 0.14 * std::sin(i * 0.00005);
        const auto value = pulse.next(440, 48000, width);
        if (!std::isfinite(value) || std::abs(value) > 1.3
            || value != repeat.next(440, 48000, width)) return 1;
    }
    pulse.reset();
    double mean = 0.0;
    for (int i = 0; i < 48000; ++i) mean += pulse.next(440, 48000, 0.36) / 48000.0;
    if (std::abs(mean) > 1e-6) return 1;
    std::vector<double> corrected(4800), naive(corrected.size());
    pulse.reset();
    for (std::size_t i = 0; i < corrected.size(); ++i)
    {
        corrected[i] = pulse.next(11000, 48000);
        const auto phase = std::fmod(i * (11000.0 / 48000.0), 1.0);
        naive[i] = phase < 0.5 ? -1.0 : 1.0;
    }
    double cleanAlias = 0.0, naiveAlias = 0.0;
    for (double hz : {15000.0, 7000.0, 19000.0, 3000.0})
    {
        cleanAlias += std::pow(magnitude(corrected, hz) / magnitude(corrected, 11000), 2);
        naiveAlias += std::pow(magnitude(naive, hz) / magnitude(naive, 11000), 2);
    }
    const auto reduction = 10.0 * std::log10(naiveAlias / cleanAlias);
    std::cout << "pulse_width_dc=" << mean << " selected_alias_reduction_db=" << reduction << '\n';
    if (reduction < 8.0) return 1;
}
