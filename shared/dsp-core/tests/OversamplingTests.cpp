#include <dsp_core/Oversampler4x.h>
#include <dsp_core/FixedDelay.h>
#include <dsp_core/Butterworth.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace dsp_core
{
// The test's nonlinear stage (formerly dsp_core::saturate): odd, bounded,
// with an exact linear bypass at amount 0.
inline double saturate(double input, double amount) noexcept
{
    if (!std::isfinite(input)) return 0.0;
    amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
    if (amount == 0.0) return input;
    const auto driven = std::tanh((1.0 + 3.0 * amount) * input);
    return (1.0 - amount) * input + amount * driven;
}
}

namespace
{
bool countAllocations = false;
std::size_t allocations = 0;
constexpr double pi = 3.14159265358979323846;
using Stage = dsp_core::Oversampler4x;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
double identity(double value) noexcept { return value; }
}

void* operator new(std::size_t size)
{
    if (countAllocations) ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace
{
void filterAndLatency()
{
    Stage stage;
    stage.prepare();
    const auto kernel = stage.coefficients();
    double total = 0.0, symmetryError = 0.0;
    for (std::size_t i = 0; i < kernel.size(); ++i)
    {
        total += kernel[i];
        symmetryError = std::max(symmetryError, std::abs(kernel[i] - kernel[kernel.size() - 1 - i]));
    }
    require(std::abs(total - 1.0) < 1.0e-14 && symmetryError < 1.0e-15,
            "FIR kernel has unity DC and symmetric linear phase");
    auto magnitude = [&](double baseFrequency)
    {
        std::complex<double> sum{};
        for (std::size_t i = 0; i < kernel.size(); ++i)
            sum += kernel[i] * std::polar(1.0, -2.0 * pi * baseFrequency * static_cast<double>(i) / Stage::factor);
        return std::abs(sum);
    };
    double passError = 0.0, stopMagnitude = 0.0;
    for (int i = 0; i <= 2000; ++i)
    {
        passError = std::max(passError, std::abs(1.0 - magnitude(i * 0.25 / 2000)));
        stopMagnitude = std::max(stopMagnitude, magnitude(0.5 + i * 1.5 / 2000));
    }
    require(passError < 0.0002, "kernel passband ripple below 0.0002 through 0.25 * base sample rate");
    require(stopMagnitude < 0.001, "kernel stopband above base Nyquist below -60 dB");

    std::array<double, 128> impulse{};
    int peakPosition = 0;
    for (std::size_t n = 0; n < impulse.size(); ++n)
    {
        impulse[n] = stage.next(n == 0 ? 1.0 : 0.0, identity);
        double expected = 0.0;
        const auto highIndex = n * Stage::factor;
        for (std::size_t i = 0; i < kernel.size(); ++i)
            if (highIndex >= i && highIndex - i < kernel.size())
                expected += Stage::factor * kernel[i] * kernel[highIndex - i];
        require(std::abs(impulse[n] - expected) < 1.0e-14,
                "polyphase/decimator result matches independently convolved FIR impulse");
        if (std::abs(impulse[n]) > std::abs(impulse[static_cast<std::size_t>(peakPosition)]))
            peakPosition = static_cast<int>(n);
    }
    require(peakPosition == Stage::latencySamples && peakPosition == 32, "measured identity group delay is exactly 32 samples");
    for (std::size_t i = 0; i <= 64; ++i)
        require(std::abs(impulse[i] - impulse[64 - i]) < 1.0e-14, "complete 4x linear round trip has symmetric phase");
    require(std::all_of(impulse.begin() + 65, impulse.end(), [](double value) { return value == 0.0; }),
            "FIR tail ends exactly rather than leaving recursive denormals");
    stage.reset();
    int shapeCalls = 0;
    for (int i = 0; i < 10; ++i)
        stage.next(0.0, [&](double value) noexcept { ++shapeCalls; return value; });
    require(shapeCalls == 40, "nonlinear callback runs exactly four times per base-rate sample, including silence");
    for (const auto amount : {0.0, 0.5, 1.0})
    {
        stage.reset();
        int drivenPeakPosition = 0;
        double drivenPeak = 0.0;
        for (int i = 0; i < 128; ++i)
        {
            const auto value = stage.next(i == 0 ? 1.0 : 0.0,
                [=](double input) noexcept { return dsp_core::saturate(input, amount); });
            if (std::abs(value) > drivenPeak) { drivenPeak = std::abs(value); drivenPeakPosition = i; }
        }
        require(drivenPeakPosition == Stage::latencySamples, "impulse peak delay stays fixed at zero/default/maximum amount");
        stage.reset();
        double actual = 0.0;
        for (int i = 0; i < 1000; ++i)
            actual = stage.next(0.25, [=](double input) noexcept { return dsp_core::saturate(input, amount); });
        require(std::abs(actual - dsp_core::saturate(0.25, amount)) < 1.0e-6,
                "interpolation/scaling/decimation preserve steady DC at all tested drives");
    }
    std::cout << "latency_samples=" << peakPosition << " kernel_passband_max_error=" << passError
              << " kernel_stopband_max_db=" << 20.0 * std::log10(stopMagnitude) << '\n';
}

void fft(std::vector<std::complex<double>>& values)
{
    const auto count = values.size();
    for (std::size_t i = 1, j = 0; i < count; ++i)
    {
        auto bit = count >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t size = 2; size <= count; size <<= 1)
    {
        const auto rotation = std::polar(1.0, -2.0 * pi / static_cast<double>(size));
        for (std::size_t offset = 0; offset < count; offset += size)
        {
            std::complex<double> weight{1.0, 0.0};
            for (std::size_t i = 0; i < size / 2; ++i)
            {
                const auto even = values[offset + i], odd = weight * values[offset + i + size / 2];
                values[offset + i] = even + odd;
                values[offset + i + size / 2] = even - odd;
                weight *= rotation;
            }
        }
    }
}

struct Spectrum { double aliasPower = 0.0, strongestHarmonic = 0.0; };
Spectrum spectrum(std::vector<std::complex<double>> values, int fundamental)
{
    fft(values);
    Spectrum result;
    for (int bin = 1; bin < static_cast<int>(values.size() / 2); ++bin)
    {
        const auto harmonic = bin % fundamental == 0 && (bin / fundamental) % 2 == 1;
        const auto power = std::norm(values[static_cast<std::size_t>(bin)]);
        if (harmonic) result.strongestHarmonic = std::max(result.strongestHarmonic, power);
        else result.aliasPower += power;
    }
    return result;
}

void aliasMeasurements()
{
    constexpr int count = 8192, warmup = 4096;
    double worstAliasDb = -1000.0, leastReduction = 1000.0;
    for (const auto rate : {44100, 48000, 88200, 96000, 176400, 192000})
        for (const auto wantedHz : {7100.0, 11000.0})
            for (const auto amount : {0.5, 1.0})
            {
                const auto bin = static_cast<int>(std::round(wantedHz * count / rate));
                Stage stage;
                stage.prepare();
                std::vector<std::complex<double>> oversampled, direct;
                for (int sample = -warmup; sample < count; ++sample)
                {
                    const auto input = 0.8 * std::sin(2.0 * pi * bin * sample / count);
                    const auto output = stage.next(input, [=](double value) noexcept { return dsp_core::saturate(value, amount); });
                    if (sample >= 0)
                    {
                        oversampled.emplace_back(output, 0.0);
                        direct.emplace_back(dsp_core::saturate(input, amount), 0.0);
                    }
                }
                const auto filtered = spectrum(oversampled, bin), naive = spectrum(direct, bin);
                const auto aliasDb = 10.0 * std::log10(filtered.aliasPower / filtered.strongestHarmonic);
                const auto reduction = 10.0 * std::log10(naive.aliasPower / filtered.aliasPower);
                worstAliasDb = std::max(worstAliasDb, aliasDb);
                // At high host rates direct alias is already below numerical
                // significance, so compare relative reduction only when measurable.
                if (naive.aliasPower / naive.strongestHarmonic > 1.0e-10)
                {
                    leastReduction = std::min(leastReduction, reduction);
                    require(reduction > 20.0, "4x stage reduces measurable alias energy by at least 20 dB versus base-rate saturation");
                }
                require(aliasDb < -50.0, "isolated sine fixtures have total alias energy at least 50 dB below strongest harmonic");
            }
    std::cout << "sine_alias_worst_dbc=" << worstAliasDb << " minimum_measurable_alias_reduction_db=" << leastReduction
              << " rates=6 tones_hz=7100,11000 amplitudes=0.8 amounts=0.5,1\n";
}

void realTimeAndDeterminism()
{
    dsp_core::FixedDelay<int, 32> delay;
    for (int i = 1; i <= 100; ++i)
        require(delay.next(i) == (i <= 32 ? 0 : i - 32), "fixed alignment delay preserves exact sample provenance");
    delay.reset();
    require(delay.next(99) == 0, "delay reset clears retained samples");
    dsp_core::Butterworth filter;
    filter.prepare(dsp_core::Butterworth::Type::lowpass, 1000.0, 48000.0);
    for (int i = 0; i < 1000; ++i) filter.next(1.0);
    auto identicalFilter = filter;
    filter.setCutoff(dsp_core::Butterworth::Type::lowpass, 1000.0, 48000.0);
    require(filter.next(1.0) == identicalFilter.next(1.0), "unchanged cutoff update keeps exact recursive history");
    filter.setCutoff(dsp_core::Butterworth::Type::lowpass, 1001.0, 48000.0);
    require(filter.next(1.0) > 0.9, "moving cutoff retains state rather than restarting the filter");
    static_assert(std::is_trivially_copyable_v<Stage>, "stage storage must have value semantics and no owning heap state");
    Stage whole, partitioned;
    whole.prepare(); partitioned.prepare();
    constexpr int count = 10000;
    std::array<double, count> input{}, expected{}, actual{};
    for (int i = 0; i < count; ++i) input[static_cast<std::size_t>(i)] = std::sin(i * 0.17);
    auto shape = [](double value) noexcept { return dsp_core::saturate(value, 0.75); };
    for (int i = 0; i < count; ++i) expected[static_cast<std::size_t>(i)] = whole.next(input[static_cast<std::size_t>(i)], shape);
    int position = 0;
    for (const auto block : {0, 1, 17, 512, 3, 1024, 0, 7, 65536})
        for (int j = 0; j < block && position < count; ++j, ++position)
            actual[static_cast<std::size_t>(position)] = partitioned.next(input[static_cast<std::size_t>(position)], shape);
    require(position == count && actual == expected, "0/1/changing/large callback partitions produce bit-identical samples");
    partitioned.reset();
    for (int i = 0; i < count; ++i)
        require(partitioned.next(input[static_cast<std::size_t>(i)], shape) == expected[static_cast<std::size_t>(i)],
                "reset exactly reproduces fresh stage history");
    auto copy = partitioned;
    for (int i = 0; i < 1000; ++i)
        require(copy.next(0.0, shape) == partitioned.next(0.0, shape), "copies own all filter state independently");

    allocations = 0;
    countAllocations = true;
    whole.prepare();
    for (int i = 0; i < 20000; ++i)
    {
        const auto value = whole.next(i % 2 == 0 ? 1.0 : -1.0, shape);
        delay.next(i);
        filter.setCutoff(dsp_core::Butterworth::Type::lowpass, i % 2 == 0 ? 1000.0 : 1001.0, 48000.0);
        filter.next(value);
        if (!std::isfinite(value)) std::abort();
        if (i == 123) whole.reset();
    }
    countAllocations = false;
    require(allocations == 0, "prepare/reset/process perform no heap allocations");
    // Prove the allocation counter itself observes an allocation.
    countAllocations = true;
    auto* probe = ::operator new(7);
    countAllocations = false;
    ::operator delete(probe);
    require(allocations == 1, "allocation guard detects an actual heap allocation");

    whole.reset();
    for (const auto value : {0.0, 1.0e-310, 1.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()})
        for (int i = 0; i < 200; ++i)
        {
            const auto output = whole.next(value, shape);
            require(std::isfinite(output) && std::fpclassify(output) != FP_SUBNORMAL && std::abs(output) < 1.5,
                    "finite/normal bounded output with invalid and subnormal inputs");
        }
    for (const auto amount : {-100.0, 0.0, 0.5, 1.0, 100.0, std::numeric_limits<double>::quiet_NaN()})
        for (const auto inputValue : {-1.0, -0.01, 0.0, 0.01, 1.0})
        {
            const auto value = dsp_core::saturate(inputValue, amount);
            require(std::isfinite(value) && std::abs(value) <= 1.0, "saturation curve bounds finite nominal input at parameter extremes");
            require(value == -dsp_core::saturate(-inputValue, amount), "symmetric curve has no imposed DC/asymmetry");
            require(dsp_core::saturate(inputValue, 0.0) == inputValue, "zero amount is exactly linear without disabling 4x processing");
        }
    std::cout << "block_partition=exact reset=exact copy=independent processing_allocations=0 finite_extremes=passed\n";
}
}

int main()
{
    filterAndLatency();
    aliasMeasurements();
    realTimeAndDeterminism();
}
