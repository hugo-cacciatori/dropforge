#pragma once

#include <dsp_core/Butterworth.h>
#include <dsp_core/LinearRamp.h>
#include <dsp_core/FixedDelay.h>
#include <dsp_core/DriveChain4x.h>
#include <dsp_core/PeakingEq.h>
#include "OutputLevel.h"

namespace dropforge
{
// One instance per side, after all playable notes are summed. Chords therefore
// interact in the drive; no shared nonlinear stage can alter the bass sine.
class RhythmBus
{
public:
    static constexpr int latencySamples = dsp_core::DriveChain4x::latencySamples;
    static constexpr double separationHz = 350.0;
    // Fixed Furnace balance: +6 dB over the original bus, after tone/drive.
    static constexpr double outputGain = 0.088 * 1.9952623149688795 * outputTrim;
    static constexpr double finalLowpassHz = 6000.0;
    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
        smoothingSamples = static_cast<int>(std::ceil(rate * 0.010));
        driveChain.prepare(rate);
        tightFilter.prepare(dsp_core::Butterworth::Type::highpass, cutoff(tightTarget), rate);
        // Feed upper-mid harmonics into the shared drive before its stages
        // interact, rather than relying solely on the final presence boost.
        preDriveContour.prepare(2800.0, 3.0, 0.7, rate);
        // These unequal Qs form a true fourth-order Butterworth, rather than
        // two identical sections (which would be Linkwitz-Riley).
        highpass[0].prepare(dsp_core::Butterworth::Type::highpass, separationHz, rate, 0.541196100146197);
        highpass[1].prepare(dsp_core::Butterworth::Type::highpass, separationHz, rate, 1.306562964876377);
        // Bring the ensemble's upper-mid harmonics forward while rolling off
        // fizz. This shapes the dry voice without changing its hard envelope.
        presence.prepare(1800.0, 6.0, 0.7, rate);
        lowpass.prepare(dsp_core::Butterworth::Type::lowpass, finalLowpassHz, rate);
        reset();
    }
    void reset() noexcept
    {
        driveChain.reset(); tightFilter.reset(); lowpass.reset(); presence.reset(); preDriveContour.reset();
        for (auto& filter : highpass) filter.reset();
        tight.reset(tightTarget); drive.reset(driveTarget); currentCutoff = -1.0;
        alignedGate.reset(); alignedLevel.reset();
    }
    void setParameters(double amount, double tightness) noexcept
    {
        amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
        tightness = std::isfinite(tightness) ? std::clamp(tightness, 0.0, 1.0) : 0.5;
        if (amount != driveTarget) { driveTarget = amount; drive.setTarget(amount, smoothingSamples); }
        if (tightness != tightTarget) { tightTarget = tightness; tight.setTarget(tightness, smoothingSamples); }
    }
    double next(double input, double envelope = 1.0, double level = 1.0) noexcept
    {
        // Fade filter ringing during the final millisecond of the fixed 6 ms
        // release, without extending its duration or clearing live history.
        const auto gate = alignedGate.next(std::clamp(envelope * 6.0, 0.0, 1.0));
        const auto velocity = alignedLevel.next(level);
        const auto hz = cutoff(tight.next());
        if (hz != currentCutoff)
        {
            currentCutoff = hz;
            tightFilter.setCutoff(dsp_core::Butterworth::Type::highpass, hz, rate);
        }
        const auto amount = drive.next();
        // Drive the unscaled note sum. Restrict the core range to keep high-note
        // fold products out of the bass band and bound default chord steps.
        auto sample = driveChain.next(preDriveContour.next(tightFilter.next(input)), 0.85 * amount);
        for (auto& filter : highpass) sample = filter.next(sample);
        // Full folding sharpens transitions. Trim only its upper-quarter range
        // using the already-smoothed drive; zero/default drive retain their gain.
        const auto foldDepth = std::max(0.0, 4.0 * amount - 3.0);
        const auto compensation = 1.0 / (1.0 + 0.8 * foldDepth);
        return outputGain * compensation * lowpass.next(presence.next(sample)) * gate * velocity;
    }
private:
    // Geometric mapping fixes the specified midpoint at 120 Hz.
    static double cutoff(double value) noexcept
    {
        return value <= 0.5 ? 60.0 * std::exp2(2.0 * value)
                            : 120.0 * std::pow(400.0 / 120.0, 2.0 * value - 1.0);
    }
    dsp_core::DriveChain4x driveChain;
    dsp_core::Butterworth tightFilter, highpass[2], lowpass;
    dsp_core::PeakingEq presence, preDriveContour;
    dsp_core::LinearRamp tight, drive;
    dsp_core::FixedDelay<double, latencySamples> alignedGate, alignedLevel;
    double rate = 44100.0, tightTarget = 0.5, driveTarget = 0.5, currentCutoff = -1.0;
    int smoothingSamples = 441;
};
}
