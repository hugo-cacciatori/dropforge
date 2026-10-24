#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <dsp_core/Butterworth.h>
#include <dsp_core/FixedDelay.h>
#include <dsp_core/FixedEnvelope.h>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/PolyBlepSaw.h>
#include <dsp_core/ParallelSawDrive.h>
#include <dsp_core/NotchFilter.h>
#include "OutputLevel.h"
#include "BassExciter.h"

namespace dropforge
{
// Diagnostic render isolation only; select once before rendering a phrase.
enum class BassComponent { all, clean, grit, dryGrit, unisonGrit };
class BassVoice
{
public:
    struct Sample { double sine = 0.0, rich = 0.0, output = 0.0; };
    static constexpr int latencySamples = dsp_core::Oversampler4x::latencySamples;
    static constexpr double attackSeconds = 0.001, releaseSeconds = 0.006;
    // Restore 3 dB of clean foundation after the upper-texture audition. Keep
    // grit independent: raising the sine must never drive it into saturation.
    static constexpr double sineGain = 0.08 * 1.4125375446227544 * outputTrim;
    static constexpr double richGain = 0.10 * outputTrim;
    // A true dry/wet mix: changing wet share must not add an overall gain boost.
    static constexpr double unisonMix = 0.45, dryMix = 1.0 - unisonMix;
    static_assert(dryMix + unisonMix == 1.0, "Dry/wet mix must total 100 percent");

    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
        transitionSamples = static_cast<int>(std::ceil(rate * 0.002));
        smoothingSamples = static_cast<int>(std::ceil(rate * 0.020));
        envelope.prepare(static_cast<int>(std::ceil(rate * attackSeconds)),
                         static_cast<int>(std::ceil(rate * releaseSeconds)));
        rich.prepare(rate);
        for (auto& path : gritPaths) path.prepare(splitTarget, rate);
        finalLowpass.prepare(dsp_core::Butterworth::Type::lowpass, 5000.0, rate);
        reset();
    }
    void reset() noexcept
    {
        envelope.reset(); rich.reset(); aligned.reset(); finalLowpass.reset();
        for (auto& path : gritPaths) path.reset();
        currentNotchHz = -1.0;
        split.reset(splitTarget); destruction.reset(destructionTarget);
        currentSplit = -1.0;
        phase = frequency = lastOutput = transitionStart = 0.0;
        velocity.reset(0.0);
        transitionRemaining = 0;
        pendingAttack = false;
    }
    void setParameters(double splitHz, double amount) noexcept
    {
        splitHz = std::isfinite(splitHz) ? std::clamp(splitHz, 80.0, 250.0) : 140.0;
        amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
        if (splitHz != splitTarget) { splitTarget = splitHz; split.setTarget(splitHz, smoothingSamples); }
        if (amount != destructionTarget) { destructionTarget = amount; destruction.setTarget(amount, smoothingSamples); }
    }
    void start(double hz, int noteVelocity) noexcept
    {
        // Keep FIR/IIR history alive: clearing a live oversampler would erase
        // pending audio before its reported latency and click on repeated keys.
        phase = 0.0;
        rich.reset();
        frequency = hz;
        // Initialize even the FIR pre-ringing with the new pitch. Otherwise a
        // fully suspended voice could depend on its previous notch setting.
        for (auto& path : gritPaths) path.setPitch(hz, rate);
        currentNotchHz = hz;
        velocity.reset(std::clamp(noteVelocity, 1, 127) / 127.0);
        envelope.start();
        pendingAttack = true;
    }
    void stop() noexcept { envelope.stop(); }
    void retargetVelocity(int noteVelocity) noexcept
    {
        velocity.setTarget(std::clamp(noteVelocity, 1, 127) / 127.0, transitionSamples);
    }
    // Optional excitation is a test-only linear/alias measurement seam.
    // Ordinary rendering uses the harmonic body plus quiet coloured texture.
    Sample next(double bendRatio, BassComponent component = BassComponent::all,
                const BassExciter::Sources* excitation = nullptr) noexcept
    {
        const auto cutoff = split.next();
        const auto amount = destruction.next();
        if (cutoff != currentSplit)
        {
            currentSplit = cutoff;
            for (auto& path : gritPaths) path.setSplit(cutoff, rate);
        }
        double sine = 0.0;
        BassExciter::Sources sources;
        const auto hz = frequency * bendRatio;
        if (envelope.active())
        {
            sine = sineGain * std::sin(phase * 6.28318530717958647692);
            phase += hz / rate;
            if (phase >= 1.0) phase -= 1.0;
            sources = rich.nextSources(hz, rate, amount);
            if (excitation != nullptr) sources = *excitation;
        }
        const auto dryDriven = gritPaths[0].drive(sources.dry, amount);
        // Stronger wet saturation is level-compensated inside its 4x stage;
        // the user's normalized blend and overall grit gain remain unchanged.
        const auto unisonDriven = gritPaths[1].drive(sources.unison, amount, 6.0);
        // Sine, envelope and attack marker share the FIR's integer delay.
        // This delays release/retrigger events too, not just steady waveforms.
        const auto frame = aligned.next({sine, envelope.next() * velocity.next(), pendingAttack, hz});
        // Stronger grit must neither cancel the clean fundamental nor bring
        // competing sub harmonics back. Track its notch at FIR-aligned pitch.
        if (frame.frequency > 0.0 && frame.frequency != currentNotchHz)
        {
            currentNotchHz = frame.frequency;
            for (auto& path : gritPaths) path.setPitch(currentNotchHz, rate);
        }
        // Separate nonlinear paths preserve the original grit timbre. Blend
        // only after their drive/cleanup; a shared drive would colour the dry.
        const auto dry = richGain * gritPaths[0].cleanup(dryDriven);
        const auto unison = richGain * gritPaths[1].cleanup(unisonDriven);
        const auto upper = component == BassComponent::dryGrit ? dry
                         : component == BassComponent::unisonGrit ? unison : dryMix * dry + unisonMix * unison;
        pendingAttack = false;
        if (frame.attack)
        {
            transitionStart = lastOutput;
            transitionRemaining = transitionSamples;
        }
        // Final HP intentionally bypassed. Gain after the LP bounds filter
        // ringing by the fixed envelope, including delayed note-off release.
        const auto cleanInput = component == BassComponent::all || component == BassComponent::clean ? frame.sine : 0.0;
        const auto gritInput = component == BassComponent::clean ? 0.0 : upper;
        auto output = finalLowpass.next(cleanInput + gritInput) * frame.amplitude;
        if (transitionRemaining > 0)
        {
            const auto oldWeight = static_cast<double>(transitionRemaining--) / transitionSamples;
            output = oldWeight * transitionStart + (1.0 - oldWeight) * output;
        }
        lastOutput = output;
        return {frame.sine * frame.amplitude, upper * frame.amplitude, output};
    }
private:
    // Instrument-specific grit routing; both paths use the existing shared
    // primitives and identical latency. Their histories survive live retriggers.
    struct GritPath
    {
        void prepare(double splitHz, double rate) noexcept
        {
            oversampler.prepare(); preSplit.prepare(splitHz, rate); postSplit.prepare(splitHz, rate);
            notch.prepare(110.0, rate);
            highpass.prepare(dsp_core::Butterworth::Type::highpass, 260.0, rate);
            body.prepare(dsp_core::Butterworth::Type::lowpass, 3500.0, rate);
        }
        void reset() noexcept
        {
            oversampler.reset(); preSplit.reset(); postSplit.reset(); notch.reset(); highpass.reset(); body.reset();
        }
        void setSplit(double hz, double rate) noexcept { preSplit.setCutoff(hz, rate); postSplit.setCutoff(hz, rate); }
        void setPitch(double hz, double rate) noexcept
        {
            notch.setFrequency(hz);
            body.setCutoff(dsp_core::Butterworth::Type::lowpass, std::max(3500.0, 4.0 * hz), rate);
        }
        double drive(double input, double amount, double boost = 1.0) noexcept
        {
            return oversampler.next(preSplit.next(input),
                [amount, boost](double value) noexcept { return dsp_core::parallelSawDrive(value * boost, amount) / boost; });
        }
        double cleanup(double input) noexcept { return body.next(highpass.next(notch.next(postSplit.next(input)))); }
        dsp_core::Oversampler4x oversampler;
        dsp_core::LinkwitzRileyHighpass preSplit, postSplit;
        dsp_core::NotchFilter notch;
        dsp_core::Butterworth highpass, body;
    };
    struct Frame { double sine = 0.0, amplitude = 0.0; bool attack = false; double frequency = 0.0; };
    dsp_core::FixedEnvelope envelope;
    BassExciter rich;
    std::array<GritPath, 2> gritPaths;
    dsp_core::FixedDelay<Frame, latencySamples> aligned;
    dsp_core::Butterworth finalLowpass;
    dsp_core::LinearRamp split, destruction, velocity;
    double splitTarget = 140.0, destructionTarget = 0.5, currentSplit = -1.0;
    double currentNotchHz = -1.0;
    double rate = 44100.0, frequency = 0.0, phase = 0.0;
    double lastOutput = 0.0, transitionStart = 0.0;
    int transitionSamples = 89, smoothingSamples = 441, transitionRemaining = 0;
    bool pendingAttack = false;
};
}
