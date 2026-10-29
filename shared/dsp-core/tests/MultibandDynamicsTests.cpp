#include <dsp_core/MultibandDynamics.h>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
constexpr double pi = 3.14159265358979323846;
int failures = 0;
void expect(bool condition, const char* what)
{
    if (!condition) { std::cout << "FAILED: " << what << '\n'; ++failures; }
}
double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

// Steady-state gain (dB) of a sine on the left (and `rightScale` x it on the
// right), measured after half a second; also returns the right/left ratio.
struct Result { double gainDb, rightOverLeftDb, appliedDb[3]; };
Result measure(dsp_core::MultibandDynamics::Settings settings, double hz, double amplitude,
               double rate = 48000.0, double rightScale = 1.0)
{
    dsp_core::MultibandDynamics dynamics;
    dynamics.prepare(settings, rate);
    double in = 0.0, left = 0.0, right = 0.0;
    const auto n = static_cast<int>(rate);
    for (int i = 0; i < n; ++i)
    {
        const auto x = amplitude * std::sin(2.0 * pi * hz * i / rate);
        float l = static_cast<float>(x), r = static_cast<float>(rightScale * x);
        dynamics.process(l, r);
        if (i >= n / 2) { in += x * x; left += static_cast<double>(l) * l; right += static_cast<double>(r) * r; }
    }
    return {db(std::sqrt(left / in)), db(std::sqrt(right / left)) - db(rightScale),
            {dynamics.bandGainDb(0), dynamics.bandGainDb(1), dynamics.bandGainDb(2)}};
}
}

int main()
{
    // At rest the bands sum flat: the low band's allpass matches the upper
    // crossover's phase, so nothing dips at either crossover.
    dsp_core::MultibandDynamics::Settings rest;
    rest.depth = 0.0f;
    double worstRipple = 0.0;
    for (const double rate : {44100.0, 48000.0, 192000.0})
        for (const double hz : {40.0, 120.0, 200.0, 450.0, 1000.0, 2500.0, 6000.0, 15000.0})
            worstRipple = std::max(worstRipple, std::abs(measure(rest, hz, 0.5, rate).gainDb));
    std::cout << "rest_ripple_db=" << worstRipple << '\n';
    expect(worstRipple < 0.05, "at rest the three bands sum flat");

    // One band's curve at a time: a tone also leaks into the neighbouring
    // bands through the crossover slopes, and those (quiet) shares would be
    // lifted, so the other two bands are made neutral here.
    const auto only = [](int band) {
        dsp_core::MultibandDynamics::Settings settings; // down -16 dB 4:1, up -20 dB 3:1, max +12
        for (int other = 0; other < dsp_core::MultibandDynamics::bands; ++other)
            if (other != band)
                settings.band[static_cast<std::size_t>(other)] = {100.0f, 4.0f, -200.0f, 3.0f, 12.0f, 0.0f};
        return settings;
    };
    const auto active = only(1);
    // The detector reads each band's peak. A 0 dB peak in the mid band:
    // (-16 - 0) x (1 - 1/4) = -12 dB. Slopes: 6 dB more input above the
    // threshold gives 6/4 dB more output; 4 dB below the upward one, 4/3 dB.
    const auto out = [&](double hz, double amplitude) { return measure(active, hz, amplitude).gainDb + db(amplitude); };
    const auto loud = measure(active, 707.0, 1.0).gainDb;
    const auto downSlope = out(707.0, 1.0) - out(707.0, 0.5);
    const auto upSlope = out(707.0, 0.05) - out(707.0, 0.05 / 1.5848931924611136);
    const auto cappedResult = measure(active, 707.0, 0.006); // -44 dB: lift would be 16 dB, above the floor fade
    std::cout << "capped applied_db=" << cappedResult.appliedDb[0] << ',' << cappedResult.appliedDb[1] << ',' << cappedResult.appliedDb[2] << '\n';
    const auto capped = cappedResult.gainDb;
    const auto silent = measure(active, 707.0, 0.0001).gainDb;  // -80 dB: under the floor
    const auto middle = measure(active, 707.0, 0.13).gainDb;    // between the thresholds
    std::cout << "loud_db=" << loud << " down_slope_db=" << downSlope << " up_slope_db=" << upSlope
              << " capped_db=" << capped << " floor_db=" << silent << " middle_db=" << middle << '\n';
    expect(std::abs(loud + 12.0) < 0.5, "a 0 dB peak is pushed down 12 dB");
    expect(std::abs(downSlope - 1.5) < 0.2, "loud material follows the 4:1 downward ratio");
    expect(std::abs(upSlope - 4.0 / 3.0) < 0.2, "quiet material follows the 3:1 upward ratio");
    expect(std::abs(capped - 12.0) < 0.3, "the lift stops at its maximum");
    expect(std::abs(silent) < 0.5, "material below the floor is never lifted");
    expect(std::abs(middle) < 0.5, "material between the thresholds passes unchanged");
    expect(std::abs(measure(only(0), 50.0, 1.0).gainDb + 12.0) < 1.0
           && std::abs(measure(only(2), 10000.0, 1.0).gainDb + 12.0) < 1.0, "low and high bands compress alike");
    // All bands together: a lone quiet band is lifted while a loud one is
    // pushed down, pulling the spectrum toward the dense middle.
    dsp_core::MultibandDynamics::Settings all;
    std::cout << "all_bands loud_1k_db=" << measure(all, 1000.0, 1.0).gainDb << '\n';

    // One detector per band for both channels: a quieter right side gets the
    // same gain, so the image never shifts.
    expect(std::abs(measure(all, 1000.0, 1.0, 48000.0, 0.05).rightOverLeftDb) < 0.01,
           "left and right share each band's gain");

    // Finite and bounded for any input.
    dsp_core::MultibandDynamics dynamics;
    dynamics.prepare(all, 48000.0);
    for (const float x : {1.0e6f, -1.0e6f, 0.0f, 1.0e-30f, 3.0f})
    {
        float l = x, r = -x;
        for (int i = 0; i < 1000; ++i) { l = x; r = -x; dynamics.process(l, r); }
        expect(std::isfinite(l) && std::isfinite(r), "finite for extreme input");
    }
    return failures ? 1 : 0;
}
