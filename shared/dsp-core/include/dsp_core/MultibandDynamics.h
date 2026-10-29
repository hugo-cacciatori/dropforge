#pragma once

#include "Svf.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace dsp_core
{
// Three-band upward and downward compression ("OTT-style"), stereo-linked,
// in 32-bit float. It pulls every band toward a dense middle: loud material
// is pushed down, quiet harmonics and detail are pulled up, which is what
// makes a driven wall sound thick, even and "expensive".
//
// Bands split at two Linkwitz-Riley (24 dB/oct) crossovers; the low band
// passes an allpass matching the upper crossover, so at rest the three bands
// sum back to a flat magnitude (an allpass in phase). Gains move per sample
// from one detector per band shared by both channels, so the stereo image
// never wanders. Upward gain fades out below a floor, so silence, noise and
// release tails are never lifted.
class MultibandDynamics
{
public:
    static constexpr int bands = 3;
    struct Band
    {
        float downThresholdDb = -16.0f, downRatio = 4.0f;  // above: pushed down
        float upThresholdDb = -20.0f, upRatio = 3.0f;      // below: pulled up
        float maxUpDb = 12.0f, outputDb = 0.0f;
    };
    struct Settings
    {
        double lowCrossoverHz = 200.0, highCrossoverHz = 2500.0;
        double attackMs = 2.0, releaseMs = 60.0;
        float floorDb = -60.0f; // no upward gain below; full from 10 dB above
        float depth = 1.0f;     // scales every band's compression (0 = off)
        std::array<Band, bands> band{};
    };

    void prepare(const Settings& next, double sampleRate) noexcept
    {
        settings = next;
        const auto rate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
        for (auto& side : split)
        {
            for (auto& filter : side.lowLow) filter.set(Svf::Type::lowpass, settings.lowCrossoverHz, rate);
            for (auto& filter : side.lowHigh) filter.set(Svf::Type::highpass, settings.lowCrossoverHz, rate);
            for (auto& filter : side.highLow) filter.set(Svf::Type::lowpass, settings.highCrossoverHz, rate);
            for (auto& filter : side.highHigh) filter.set(Svf::Type::highpass, settings.highCrossoverHz, rate);
            side.phaseMatch.set(Svf::Type::allpass, settings.highCrossoverHz, rate);
        }
        attack = static_cast<float>(-std::expm1(-1000.0 / (settings.attackMs * rate)));
        release = static_cast<float>(-std::expm1(-1000.0 / (settings.releaseMs * rate)));
        peakDecay = static_cast<float>(std::exp(-1000.0 / (settings.releaseMs * rate)));
        reset();
    }
    void reset() noexcept
    {
        for (auto& side : split)
        {
            for (auto* group : {&side.lowLow, &side.lowHigh, &side.highLow, &side.highHigh})
                for (auto& filter : *group) filter.reset();
            side.phaseMatch.reset();
        }
        envelope.fill(0.0f);
        gainDb.fill(0.0f);
    }

    void process(float& left, float& right) noexcept
    {
        std::array<std::array<float, bands>, 2> parts{};
        const float input[2]{left, right};
        for (std::size_t side = 0; side < 2; ++side)
        {
            auto& s = split[side];
            const auto low = s.lowLow[1].next(s.lowLow[0].next(input[side]));
            const auto rest = s.lowHigh[1].next(s.lowHigh[0].next(input[side]));
            parts[side][0] = s.phaseMatch.next(low);
            parts[side][1] = s.highLow[1].next(s.highLow[0].next(rest));
            parts[side][2] = s.highHigh[1].next(s.highHigh[0].next(rest));
        }
        float outLeft = 0.0f, outRight = 0.0f;
        for (std::size_t band = 0; band < bands; ++band)
        {
            const auto gain = bandGain(band, std::max(std::abs(parts[0][band]), std::abs(parts[1][band])));
            outLeft += gain * parts[0][band];
            outRight += gain * parts[1][band];
        }
        left = outLeft;
        right = outRight;
    }

    // Scales every band's compression; 0 leaves only the bands' output gains.
    void setDepth(float depth) noexcept { settings.depth = std::isfinite(depth) ? std::clamp(depth, 0.0f, 1.0f) : 1.0f; }

    // The gain each band applied to the last sample, for tests and meters.
    float bandGainDb(int band) const noexcept { return gainDb[static_cast<std::size_t>(band)]; }

private:
    float bandGain(std::size_t band, float level) noexcept
    {
        // Detector: the band's peak, rising at once and falling with the
        // release. It is steady on a held tone, so the gain never wobbles
        // inside a cycle (which would distort, worst in the low band).
        auto& peak = envelope[band];
        peak = std::max(level, peak * peakDecay);
        if (peak < 1.0e-20f) peak = 0.0f;
        const auto& b = settings.band[band];
        const auto db = 20.0f * std::log10(peak + 1.0e-9f);
        float change = 0.0f;
        if (db > b.downThresholdDb)
            change = (b.downThresholdDb - db) * (1.0f - 1.0f / b.downRatio);
        else if (db < b.upThresholdDb)
        {
            const auto lift = std::min(b.maxUpDb, (b.upThresholdDb - db) * (1.0f - 1.0f / b.upRatio));
            change = lift * std::clamp((db - settings.floorDb) / 10.0f, 0.0f, 1.0f);
        }
        // The gain itself moves with the attack when it falls (more
        // compression) and the release when it rises: fast, but click-free.
        const auto target = settings.depth * change + b.outputDb;
        auto& current = gainDb[band];
        current += (target < current ? attack : release) * (target - current);
        // Land exactly on the target: an endless approach to 0 dB would
        // decay into denormals, which stall some CPUs.
        if (std::abs(target - current) < 1.0e-6f) current = target;
        return std::pow(10.0f, current / 20.0f);
    }

    struct Split
    {
        // Linkwitz-Riley 24 dB/oct = two Butterworth sections in series.
        std::array<Svf, 2> lowLow{}, lowHigh{}, highLow{}, highHigh{};
        Svf phaseMatch;
    };
    Settings settings{};
    std::array<Split, 2> split{};
    std::array<float, bands> envelope{}, gainDb{};
    float attack = 0.1f, release = 0.001f, peakDecay = 0.999f;
};
}
