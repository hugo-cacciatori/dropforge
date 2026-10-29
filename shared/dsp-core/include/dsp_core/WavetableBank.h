#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace dsp_core
{
// A mipmapped, band-limited single-cycle wavetable, computed by additive
// synthesis at startup (no audio files). Each level keeps only the harmonics
// that stay below 0.45 x sample rate for the pitches it serves, so reading it
// never aliases. Levels are a quarter octave apart: with whole-octave levels
// the lowest pitch of each octave would lose its top octave of harmonics
// (a saw at the bottom of a level would stop near 0.225 x rate, about 11 kHz
// at 48 kHz); quarter octaves keep every pitch bright up to about 0.38 x rate.
//
// Build banks off the audio thread (they allocate); reading is lock-free.
class WavetableBank
{
public:
    static constexpr int tableBits = 11;
    static constexpr int tableSize = 1 << tableBits;          // 2048 points
    static constexpr int maxHarmonic = tableSize / 2 - 1;     // 1023
    static constexpr int levelsPerOctave = 4;
    static constexpr double bandLimit = 0.45;                 // of the sample rate

    // amplitude(k) is the sine amplitude of harmonic k >= 1 (sine phase).
    template <typename Amplitude>
    explicit WavetableBank(Amplitude amplitude)
    {
        for (int level = 0;; ++level)
        {
            const auto harmonics = static_cast<int>(maxHarmonic * std::exp2(-level / static_cast<double>(levelsPerOctave)));
            if (harmonics < 1) break;
            levels.push_back(build(amplitude, harmonics));
        }
    }

    // The ideal sawtooth rising from -1 to +1 over one cycle, truncated per
    // level. Its pulse family comes from two reads (see WavetableOscillator).
    static const WavetableBank& saw()
    {
        // 2 x phase - 1 = -(2 / pi) x sum of sin(2 pi k phase) / k.
        static const WavetableBank bank([](int k) { return -2.0 / (3.14159265358979323846 * k); });
        return bank;
    }

    struct Level
    {
        int harmonics = 0;
        std::vector<float> samples; // tableSize + 1 points: the last repeats the first
    };

    int levelCount() const noexcept { return static_cast<int>(levels.size()); }
    const Level& level(int index) const noexcept { return levels[static_cast<std::size_t>(index)]; }

    // The brightest level whose top harmonic stays under the band limit for a
    // fundamental of `increment` cycles per sample.
    int levelFor(double increment) const noexcept
    {
        const auto allowed = bandLimit / std::clamp(increment, 1.0e-9, bandLimit);
        auto index = allowed >= maxHarmonic ? 0
            : static_cast<int>(std::ceil(levelsPerOctave * std::log2(maxHarmonic / allowed)));
        index = std::clamp(index, 0, levelCount() - 1);
        // Guard the logarithm's rounding against the integer harmonic counts.
        while (index + 1 < levelCount() && levels[static_cast<std::size_t>(index)].harmonics > allowed) ++index;
        while (index > 0 && levels[static_cast<std::size_t>(index - 1)].harmonics <= allowed) --index;
        return index;
    }

private:
    template <typename Amplitude>
    static Level build(Amplitude amplitude, int harmonics)
    {
        // Inverse FFT of the sine spectrum: exact samples in N log N time.
        std::vector<std::complex<double>> bins(tableSize);
        for (int k = 1; k <= harmonics; ++k)
        {
            const auto a = amplitude(k) * 0.5;
            bins[static_cast<std::size_t>(k)] = {0.0, -a};
            bins[static_cast<std::size_t>(tableSize - k)] = {0.0, a};
        }
        inverseFft(bins);
        Level level{harmonics, std::vector<float>(tableSize + 1)};
        for (int i = 0; i < tableSize; ++i)
            level.samples[static_cast<std::size_t>(i)] = static_cast<float>(bins[static_cast<std::size_t>(i)].real());
        level.samples[tableSize] = level.samples[0];
        return level;
    }
    static void inverseFft(std::vector<std::complex<double>>& bins)
    {
        const auto size = bins.size();
        for (std::size_t i = 1, j = 0; i < size; ++i)
        {
            auto bit = size >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(bins[i], bins[j]);
        }
        for (std::size_t length = 2; length <= size; length *= 2)
            for (std::size_t start = 0; start < size; start += length)
                for (std::size_t i = 0; i < length / 2; ++i)
                {
                    // Direct twiddles: no accumulated rounding across a stage.
                    const auto twiddle = std::polar(1.0, 6.28318530717958647692 * static_cast<double>(i)
                                                             / static_cast<double>(length));
                    const auto a = bins[start + i], b = bins[start + i + length / 2] * twiddle;
                    bins[start + i] = a + b; bins[start + i + length / 2] = a - b;
                }
    }

    std::vector<Level> levels;
};

// Start phases for one oscillator from a (channel, note, voice slot, unison
// index) tuple. A fixed integer hash, not a random generator: renders repeat
// exactly, yet chord notes and unison voices never start phase-locked.
inline std::uint32_t seededPhase(std::uint32_t channel, std::uint32_t note,
                                 std::uint32_t slot, std::uint32_t unison) noexcept
{
    auto x = (channel & 0xffu) | (note & 0xffu) << 8 | (slot & 0xffu) << 16 | (unison & 0xffu) << 24;
    x ^= 0x9e3779b9u; // keep the all-zero tuple away from the hash's fixed point
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Reads a WavetableBank with a 32-bit phase accumulator: one cycle is exactly
// 2^32, so the phase wraps for free, identically on every platform and block
// size. Set the frequency at control rate; next() costs two table reads.
class WavetableOscillator
{
public:
    void reset(std::uint32_t startPhase = 0) noexcept { phase = startPhase; }

    void setFrequency(const WavetableBank& bank, double frequency, double sampleRate) noexcept
    {
        const auto ratio = std::isfinite(frequency) && std::isfinite(sampleRate) && sampleRate > 0.0
            ? std::clamp(frequency / sampleRate, 0.0, WavetableBank::bandLimit) : 0.0;
        increment = static_cast<std::uint32_t>(std::llround(ratio * 4294967296.0));
        table = bank.level(bank.levelFor(ratio)).samples.data();
    }

    float next() noexcept
    {
        const auto value = read(phase);
        phase += increment;
        return value;
    }

    // A band-limited pulse as the difference of two saws `width` of a cycle
    // apart: both reads come from the same alias-free level, so the pulse is
    // alias-free too and its width can move without clicks. High (2 - 2 width)
    // for the first `width` of the cycle, low (-2 width) after: zero mean at
    // every width, and width 0.5 is a +-1 square.
    float nextPulse(double width) noexcept
    {
        const auto value = read(phase - phaseOffset(width)) - read(phase);
        phase += increment;
        return value;
    }

    // saw - amount x pulse-shifted saw: a saw/pulse blend in two reads. With
    // amount a and offset phaseOffset(w), it equals (1 - a) x saw minus
    // a x nextPulse(w). Compute the offset at control rate.
    float nextBlend(std::uint32_t offset, float amount) noexcept
    {
        const auto value = read(phase) - amount * read(phase - offset);
        phase += increment;
        return value;
    }
    static std::uint32_t phaseOffset(double width) noexcept
    {
        return static_cast<std::uint32_t>(std::llround((std::isfinite(width) ? std::clamp(width, 0.0, 1.0) : 0.5)
                                                       * 4294967295.0));
    }

    std::uint32_t currentPhase() const noexcept { return phase; }
    std::uint32_t phaseIncrement() const noexcept { return increment; }

private:
    static constexpr int fractionBits = 32 - WavetableBank::tableBits;
    float read(std::uint32_t at) const noexcept
    {
        const auto index = at >> fractionBits;
        const auto fraction = static_cast<float>(at & ((1u << fractionBits) - 1u)) * (1.0f / (1u << fractionBits));
        const auto a = table[index], b = table[index + 1];
        return a + fraction * (b - a);
    }

    const float* table = WavetableBank::saw().level(0).samples.data();
    std::uint32_t phase = 0, increment = 0;
};
}
