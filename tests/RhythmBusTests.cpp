#include "dsp/Engine.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <complex>
#include <iostream>
#include <limits>

namespace
{
constexpr double pi = 3.14159265358979323846;
// Reference level for waveform shape after the fixed mix lift.
// Absolute peak/headroom and the dedicated Mono control-click gates stay fixed.
constexpr double waveformReferenceGain = (0.088 * dropforge::outputTrim) / dropforge::RhythmBus::outputGain;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
double response(double frequency, double rate, double tight)
{
    dropforge::RhythmBus bus;
    bus.setParameters(0.0, tight); bus.prepare(rate);
    double power = 0.0;
    for (int i = 0; i < static_cast<int>(rate); ++i)
    {
        const auto output = bus.next(std::sin(2.0 * pi * frequency * i / rate));
        if (i >= rate / 2) power += output * output;
    }
    return std::sqrt(4.0 * power / rate) / dropforge::RhythmBus::outputGain;
}
// Independent RBJ transfer function for the fixed pre-drive contour. At zero
// drive it multiplies the original chain response; retain the original HP/LP
// gates after removing this known factor rather than broadening their limits.
double contourGain(double frequency, double rate)
{
    const auto a = std::pow(10.0, 3.0 / 40.0);
    const auto omega = 2.0 * pi * 2800.0 / rate;
    const auto alpha = std::sin(omega) / (2.0 * 0.7);
    const auto z = std::polar(1.0, -2.0 * pi * frequency / rate);
    return std::abs(((1.0 + alpha * a) - 2.0 * std::cos(omega) * z + (1.0 - alpha * a) * z * z)
                  / ((1.0 + alpha / a) - 2.0 * std::cos(omega) * z + (1.0 - alpha / a) * z * z));
}
void filters()
{
    for (const auto rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        const auto body = response(1000.0, rate, 0.5) / contourGain(1000.0, rate);
        const auto low = response(50.0, rate, 0.5);
        const auto focus = response(1800.0, rate, 0.5) / contourGain(1800.0, rate);
        const auto edge = response(6000.0, rate, 0.5) / contourGain(6000.0, rate);
        const auto fizz = response(9000.0, rate, 0.5) / contourGain(9000.0, rate);
        const auto loose = response(120.0, rate, 0.0);
        const auto tight = response(120.0, rate, 1.0);
        require(body > 1.4 && body < 1.6 && focus > 1.9 && focus < 2.01,
                "dry ensemble has the intended broad +6 dB upper-mid focus");
        require(low < 0.003, "final HP plus Tight rejects deep rhythm lows");
        require(edge > 0.75 && edge < 0.9 && fizz < 0.5,
                "6 kHz final LP retains focused mids while rejecting upper fizz");
        require(tight < loose * 0.11, "Tight changes pre-drive low-frequency response");
        // Check the shared variable-Q extension without altering its old defaults.
        dsp_core::Butterworth a, b;
        a.prepare(dsp_core::Butterworth::Type::highpass, 150.0, rate, 0.541196100146197);
        b.prepare(dsp_core::Butterworth::Type::highpass, 150.0, rate, 1.306562964876377);
        double power = 0.0;
        for (int i = 0; i < static_cast<int>(rate); ++i)
        {
            const auto output = b.next(a.next(std::sin(2.0 * pi * 150.0 * i / rate)));
            if (i >= rate / 2) power += output * output;
        }
        require(std::abs(std::sqrt(4.0 * power / rate) - std::sqrt(0.5)) < 0.0001,
                "fourth-order Butterworth corner is -3 dB, rather than LR4 -6 dB");
        std::cout << "rhythm_filter_rate=" << rate << " 1k_gain=" << body << " 50hz_gain=" << low
                  << " 1k8_gain=" << focus << " 6k_gain=" << edge << " 9k_gain=" << fizz
                  << " tight_ratio=" << tight / loose << '\n';
    }
}
void topology()
{
    dropforge::Engine chord, first, second;
    for (auto* engine : {&chord, &first, &second}) engine->prepare(48000);
    chord.noteOn(1, 52, 127); chord.noteOn(1, 59, 127);
    first.noteOn(1, 52, 127); second.noteOn(1, 59, 127);
    double difference = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        float cl, cr, al, ar, bl, br;
        chord.process(&cl, &cr, 1, dropforge::LayerMask::rhythm);
        first.process(&al, &ar, 1, dropforge::LayerMask::rhythm);
        second.process(&bl, &br, 1, dropforge::LayerMask::rhythm);
        difference += std::abs(cl - al - bl) + std::abs(cr - ar - br);
    }
    require(difference / 96000 > 0.001, "played chords interact through a shared nonlinear bus per side");
    chord.reset();
    chord.noteOn(1, 52, 32);
    float previousLeft = 0.0f, previousRight = 0.0f;
    double step = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        if (i % 4800 == 1000) chord.noteOn(1, 59, 127);
        if (i % 4800 == 3000) chord.noteOff(1, 59);
        float left, right;
        chord.process(&left, &right, 1, dropforge::LayerMask::rhythm);
        step = std::max({step, std::abs(static_cast<double>(left - previousLeft)),
                        std::abs(static_cast<double>(right - previousRight))});
        previousLeft = left; previousRight = right;
    }
    std::cout << "rhythm_mixed_velocity_maximum_step=" << step
              << " reference_level_step=" << step * waveformReferenceGain << '\n';
    require(step * waveformReferenceGain < 0.04, "mixed-velocity waveform shape stays bounded at reference level");
}
void controls()
{
    double peak = 0.0, step = 0.0;
    bool bounded = true;
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        dropforge::RhythmBus bus;
        bus.prepare(rate);
        double previous = 0.0;
        for (int i = 0; i < static_cast<int>(rate / 2); ++i)
        {
            if (i % 997 == 0) bus.setParameters((i / 997) % 2, (i / 997) % 2);
            if (i % 1999 == 0) bus.setParameters(std::numeric_limits<double>::quiet_NaN(),
                                                 std::numeric_limits<double>::infinity());
            const auto output = bus.next(4.0 * std::sin(2.0 * pi * 220.0 * i / rate));
            require(std::isfinite(output) && std::fpclassify(output) != FP_SUBNORMAL, "extreme sweeps remain finite/normal");
            peak = std::max(peak, std::abs(output)); step = std::max(step, std::abs(output - previous));
            previous = output;
        }
        std::cout << "rhythm_control_rate=" << rate << " peak=" << peak << " maximum_step=" << step
                  << " reference_level_step=" << step * waveformReferenceGain << '\n';
        bounded = bounded && peak < 0.5 && step * waveformReferenceGain < 0.04;
        double output = 1.0;
        for (int i = 0; i < static_cast<int>(rate / 10); ++i) output = bus.next(0.0, false);
        require(output == 0.0, "release gate settles exactly with filter history retained");
    }
    std::cout << "rhythm_controls_peak=" << peak << " maximum_step=" << step << " latency=32\n";
    require(bounded, "driven input and parameter motion retain bounded output/steps");
}
void stagedHarmonics()
{
    static_assert(dropforge::RhythmBus::latencySamples == 32);
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        dropforge::RhythmBus driven, linear;
        driven.setParameters(0.5, 0.5); linear.setParameters(0.0, 0.5);
        driven.prepare(rate); linear.prepare(rate);
        std::array<double, 4> bins{};
        for (int i = 0; i < static_cast<int>(rate * 2); ++i)
        {
            const auto phase = 2.0 * pi * 220.0 * i / rate;
            const auto input = 0.4 * std::sin(phase);
            const auto wet = driven.next(input), dry = linear.next(input);
            if (i >= static_cast<int>(rate))
            {
                bins[0] += wet * std::cos(2.0 * phase); bins[1] += wet * std::sin(2.0 * phase);
                bins[2] += dry * std::cos(2.0 * phase); bins[3] += dry * std::sin(2.0 * phase);
            }
        }
        const auto second = 2.0 * std::hypot(bins[0], bins[1]) / rate;
        const auto linearSecond = 2.0 * std::hypot(bins[2], bins[3]) / rate;
        require(second > 1.0e-5 && linearSecond < 1.0e-9,
                "actual Rhythm bus uses asymmetric multistage drive while zero drive remains linear");
        std::cout << "rhythm_staged_rate=" << rate << " second_harmonic=" << second
                  << " zero_drive_second=" << linearSecond << '\n';
    }
}
}
int main() { filters(); topology(); controls(); stagedHarmonics(); }
