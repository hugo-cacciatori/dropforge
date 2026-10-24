#include <dsp_core/DriveChain4x.h>
#include <dsp_core/Saturation.h>
#include <dsp_core/ParallelSawDrive.h>
#include <array>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace
{
bool guardAllocations = false;
constexpr double twoPi = 6.28318530717958647692;
using Chain = dsp_core::DriveChain4x;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}
void* operator new(std::size_t size)
{
    if (guardAllocations) std::abort();
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
void linearLatency(double rate)
{
    Chain chain; chain.prepare(rate);
    dsp_core::Oversampler4x reference; reference.prepare();
    int peakAt = 0;
    double peak = 0.0;
    for (int i = 0; i < 256; ++i)
    {
        const auto input = i == 0 ? 1.0 : 0.0;
        const auto output = chain.next(input, 0.0);
        require(output == reference.next(input, [](double value) noexcept { return value; }),
                "zero amount exactly preserves the existing linear FIR round trip");
        if (std::abs(output) > peak) { peak = std::abs(output); peakAt = i; }
    }
    require(peakAt == 32 && Chain::latencySamples == 32, "fixed linear FIR latency is 32 samples at every rate");
}

void spectrumAndDc()
{
    Chain chain; chain.prepare(48000);
    dsp_core::Oversampler4x legacy; legacy.prepare();
    std::array<double, 4> bins{};
    double mean = 0.0;
    for (int i = 0; i < 96000; ++i)
    {
        const auto phase = twoPi * 220.0 * i / 48000.0;
        const auto input = 0.4 * std::sin(phase);
        const auto output = chain.next(input, 0.5);
        const auto old = legacy.next(input, [](double value) noexcept { return dsp_core::saturate(value, 0.5); });
        if (i >= 48000)
        {
            mean += output;
            bins[0] += output * std::cos(phase * 2.0); bins[1] += output * std::sin(phase * 2.0);
            bins[2] += old * std::cos(phase * 2.0); bins[3] += old * std::sin(phase * 2.0);
        }
    }
    const auto second = 2.0 * std::hypot(bins[0], bins[1]) / 48000.0;
    const auto oldSecond = 2.0 * std::hypot(bins[2], bins[3]) / 48000.0;
    require(second > 0.001 && oldSecond < 1.0e-8, "new asymmetric drive generates even harmonics absent from the legacy odd curve");
    require(std::abs(mean / 48000.0) < 1.0e-6, "DC removal rejects sustained nonlinear bias");
    std::cout << "second_harmonic=" << second << " legacy_second=" << oldSecond << " dc=" << mean / 48000.0 << '\n';

    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        chain.prepare(rate);
        double output = 0.0;
        for (int i = 0; i < static_cast<int>(rate); ++i) output = chain.next(0.5, 1.0);
        require(std::abs(output) < 1.0e-12, "constant hot input settles to zero at every sample rate");
    }
}

void smallSignalResponse(double rate)
{
    // In the small-signal limit the derivatives of the two clips and the
    // fold are known. Cancel the FIR response with its independent linear
    // reference, then compare the remaining complex gain with analytic poles.
    for (const double amount : {0.5, 1.0})
    {
        Chain chain; chain.prepare(rate);
        dsp_core::Oversampler4x linear; linear.prepare();
        std::complex<double> measured{}, reference{};
        constexpr double frequency = 12000.0, amplitude = 1.0e-6;
        for (int i = 0; i < static_cast<int>(rate); ++i)
        {
            const auto phase = twoPi * frequency * i / rate;
            const auto input = amplitude * std::sin(phase);
            const auto output = chain.next(input, amount);
            const auto dry = linear.next(input, [](double value) noexcept { return value; });
            if (i >= static_cast<int>(rate / 2))
            {
                const auto basis = std::polar(1.0, -phase);
                measured += output * basis; reference += dry * basis;
            }
        }
        const auto internalRate = rate * 4.0;
        const auto z = std::polar(1.0, -twoPi * frequency / internalRate);
        const auto lpPole = std::exp(-twoPi * 12000.0 / internalRate);
        const auto dcPole = std::exp(-twoPi * 20.0 / internalRate);
        const auto lp = (1.0 - lpPole) / (1.0 - lpPole * z);
        const auto hp = dcPole * (1.0 - z) / (1.0 - dcPole * z);
        const auto softSlope = (1.0 + 3.0 * amount) / std::pow(std::cosh(0.15 * amount), 2.0);
        const auto foldSlope = amount == 1.0 ? 3.0 : 1.0;
        const auto expected = (1.0 - amount) + amount * softSlope * (1.0 + amount) * foldSlope * lp * hp;
        const auto error = std::abs(measured / reference - expected);
        require(error < 1.0e-6, "clip/fold slopes and inter-stage/DC poles match analytic 4x-rate transfer");
        std::cout << "small_signal_rate=" << rate << " amount=" << amount << " complex_error=" << error << '\n';
    }
}

void repeatAndControls(double rate)
{
    Chain chain, other; chain.prepare(rate); other.prepare(rate);
    std::vector<double> first(16384);
    double peak = 0.0;
    // Real-time calls, including reset, cannot allocate. A second instance
    // proves independent histories when calls are interleaved differently.
    guardAllocations = true;
    for (int pass = 0; pass < 2; ++pass)
    {
        chain.reset();
        for (std::size_t i = 0; i < first.size(); ++i)
        {
            const auto input = 8.0 * std::sin(twoPi * 997.0 * static_cast<double>(i) / rate);
            // Smooth excursions across the fold onset and both drive endpoints.
            const auto amount = 0.5 - 0.5 * std::cos(twoPi * static_cast<double>(i) / first.size());
            const auto output = chain.next(input, amount);
            require(std::isfinite(output) && std::fpclassify(output) != FP_SUBNORMAL && std::abs(output) < 9.0,
                    "driven range remains finite, normal and bounded across drive/fold motion");
            peak = std::max(peak, std::abs(output));
            if (pass == 0) { first[i] = output; other.next(-input, 1.0 - amount); }
            else require(output == first[i], "reset/replay is exact and other instances cannot contaminate state");
        }
    }
    guardAllocations = false;
    // Invalid controls recover to the documented midpoint; bad input cannot
    // poison later audio or the internal filter histories.
    for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
    {
        chain.reset(); other.reset();
        for (int i = 0; i < 256; ++i)
            require(chain.next(0.25, invalid) == other.next(0.25, 0.5), "invalid amount falls back to midpoint");
        require(std::isfinite(chain.next(invalid, 1.0)), "invalid input is contained");
        for (int i = 0; i < 256; ++i) require(std::isfinite(chain.next(0.25, 1.0)), "audio recovers after invalid input");
    }
    chain.reset();
    for (int i = 0; i < 256; ++i) require(chain.next(0.0, 1.0) == 0.0, "reset silence has no bias or stale history");
    std::cout << "drive_rate=" << rate << " stress_peak=" << peak << " allocation_free=1 repeat=exact\n";
}

void boundaries()
{
    Chain below, above;
    below.prepare(48000); above.prepare(48000);
    double maximumDifference = 0.0, hotPeak = 0.0;
    for (int i = 0; i < 4800; ++i)
    {
        const auto input = 2.0 * std::sin(twoPi * 220.0 * i / 48000.0);
        maximumDifference = std::max(maximumDifference,
            std::abs(below.next(input, 0.75 - 1.0e-7) - above.next(input, 0.75 + 1.0e-7)));
    }
    require(maximumDifference < 1.0e-5, "fold enters continuously at the high-drive boundary");
    for (const auto rate : {0.0, -1.0, 192001.0, std::numeric_limits<double>::quiet_NaN()})
    {
        below.prepare(rate); above.prepare(44100);
        for (int i = 0; i < 512; ++i)
            require(below.next(0.3, 0.5) == above.next(0.3, 0.5), "invalid prepare rate uses the 44.1 kHz fallback");
    }
    below.prepare(48000);
    for (int i = 0; i < 4800; ++i)
    {
        const auto output = below.next(1.0e12 * std::sin(twoPi * 220.0 * i / 48000.0), 1.0);
        hotPeak = std::max(hotPeak, std::abs(output));
        require(std::isfinite(output) && std::abs(output) < 2.0, "fully driven stage bounds gross overloads");
    }
    std::cout << "fold_boundary_difference=" << maximumDifference << " full_drive_overload_peak=" << hotPeak << '\n';
}
void parallelSawTexture()
{
    std::complex<double> even{};
    guardAllocations = true;
    for (int i = 0; i < 48000; ++i)
    {
        const auto angle = twoPi * 200.0 * i / 48000.0;
        const auto input = 0.6 * std::sin(angle);
        require(dsp_core::parallelSawDrive(input, 0.0) == input, "parallel drive has exact linear bypass");
        const auto output = dsp_core::parallelSawDrive(input, 0.5);
        even += output * std::polar(1.0, -2.0*angle);
        for (const auto amount : {0.0, 0.5, 0.65, 1.0})
        {
            const auto value = dsp_core::parallelSawDrive(input, amount);
            require(std::isfinite(value) && std::abs(value) < 1.0, "parallel drive is bounded over exciter input");
        }
        require(std::abs(dsp_core::parallelSawDrive(input, 0.65-1e-7)
                       - dsp_core::parallelSawDrive(input, 0.65+1e-7)) < 1e-6,
                "saw fold enters continuously");
    }
    guardAllocations = false;
    const auto evenAmplitude = 2.0*std::abs(even)/48000.0;
    require(evenAmplitude > 0.001, "asymmetric grit creates even harmonics rather than only odd square-like partials");
    for (const auto input : {1e308, -1e308, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        require(std::isfinite(dsp_core::parallelSawDrive(input, 1.0)), "parallel drive contains invalid and extreme inputs");
    std::cout << "parallel_saw_even_harmonic_amplitude=" << evenAmplitude << " allocation_free=1 bypass=exact\n";
}
}
int main()
{
    static_assert(std::is_nothrow_default_constructible_v<Chain>);
    static_assert(noexcept(std::declval<Chain&>().next(0.0, 0.5)));
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        linearLatency(rate);
        repeatAndControls(rate);
        smallSignalResponse(rate);
    }
    spectrumAndDc();
    boundaries();
    parallelSawTexture();
    std::cout << "Drive core only: instrument integration, swept alias acceptance and listening remain pending.\n";
}
