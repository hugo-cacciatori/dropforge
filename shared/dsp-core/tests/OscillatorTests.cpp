#include <dsp_core/PolyBlepSaw.h>
#include <cmath>
#include <iostream>
#include <limits>

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
}
