#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/FixedDelay.h>
#include <dsp_core/FixedEnvelope.h>
#include <dsp_core/LinearRamp.h>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/ParallelSawDrive.h>
#include <dsp_core/Svf.h>
#include <dsp_core/WavetableBank.h>
#include "OutputLevel.h"
#include "AnvilGrowl.h"
#include "AnvilReese.h"
#include "AnvilRound.h"
#include "BassExciter.h"

namespace dropforge
{
// Diagnostic render isolation only; select once before rendering a phrase.
// With modes B-D, dryGrit and unisonGrit both isolate that mode's voice.
enum class BassComponent { all, clean, grit, dryGrit, unisonGrit };
// Which grit candidate rides on the sine (SPEC section 4): A is the
// owner-accepted harmonic grit, B the growl, C the round, D the reese.
// The engine crossfades between two voices to change it while playing.
enum class AnvilGrit { a, b, c, d };
// Anvil (SPEC section 4), in 32-bit float: a clean sine exactly an octave
// down, never saturated, plus a grit candidate: A (ported from Ironstack
// with its gains, filters and drive unchanged) or the growl B.
class BassVoice
{
public:
    struct Sample { float sine = 0.0f, rich = 0.0f, output = 0.0f; };
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples;
    static_assert(AnvilGrowl::latencySamples == latencySamples, "both grit candidates share one alignment delay");
    static constexpr double attackSeconds = 0.001, releaseSeconds = 0.006;
    // Restore 3 dB of clean foundation after the upper-texture audition. Keep
    // grit independent: raising the sine must never drive it into saturation.
    static constexpr float sineGain = static_cast<float>(0.08 * 1.4125375446227544 * outputTrim);
    static constexpr float richGain = static_cast<float>(0.10 * outputTrim);
    // Growl B sits about 13 dB under the sine's RMS on a held E1: clearly
    // audible mid-bass over the sub, within 1 LU of grit A (SPEC section 12).
    static constexpr float growlGain = static_cast<float>(0.042 * outputTrim);
    // Round C and reese D: set so each lands within 1 LU of grit A.
    static constexpr float roundGain = static_cast<float>(0.06 * outputTrim);
    static constexpr float reeseGain = static_cast<float>(0.04 * outputTrim);
    // Destruction trims per mode A-D, so destroyed lands near its clean mode.
    // A: the subtle grit lifted 15.6 dB so Destruction is clearly heard; B
    // trimmed 1.4 dB so its folded clip is not louder than the clean growl.
    static constexpr std::array<float, 4> destroyedGain{6.0f, 0.85f, 1.0f, 1.0f};
    // A true dry/wet mix: changing wet share must not add an overall gain boost.
    static constexpr float unisonMix = 0.45f, dryMix = 1.0f - unisonMix;
    static_assert(dryMix + unisonMix == 1.0f, "Dry/wet mix must total 100 percent");

    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
        transitionSamples = static_cast<int>(std::ceil(rate * 0.002));
        smoothingSamples = static_cast<int>(std::ceil(rate * 0.020));
        envelope.prepare(static_cast<int>(std::ceil(rate * attackSeconds)),
                         static_cast<int>(std::ceil(rate * releaseSeconds)));
        rich.prepare(rate);
        for (auto& path : gritPaths) path.prepare(splitTarget, rate);
        growl.prepare(splitTarget, rate);
        round.prepare(splitTarget, rate);
        reese.prepare(splitTarget, rate);
        finalLowpass.set(dsp_core::Svf::Type::lowpass, 5000.0, rate);
        reset();
    }
    void reset() noexcept
    {
        envelope.reset(); rich.reset(); growl.reset(); round.reset(); reese.reset(); aligned.reset(); finalLowpass.reset();
        for (auto& path : gritPaths) path.reset();
        currentNotchHz = -1.0;
        split.reset(splitTarget); driveAmount.reset(driveAmountTarget);
        currentSplit = -1.0;
        phase = 0;
        frequency = 0.0;
        lastOutput = transitionStart = 0.0f;
        velocity.reset(0.0);
        transitionRemaining = 0;
        pendingAttack = false;
    }
    // Not real-time: select before prepare() or between renders.
    void setGrit(AnvilGrit candidate) noexcept { setMode(candidate, destroyed); }
    // Mode and Destruction (SPEC section 12); not real-time, re-prepares the
    // filters at the current rate. The engine crossfades between voices.
    void setMode(AnvilGrit candidate, bool destroy) noexcept
    {
        grit = candidate;
        destroyed = destroy;
        growl.setDestroyed(destroy); round.setDestroyed(destroy); reese.setDestroyed(destroy);
        prepare(rate);
    }
    void setParameters(double splitHz, double amount) noexcept
    {
        splitHz = std::isfinite(splitHz) ? std::clamp(splitHz, 80.0, 250.0) : 140.0;
        amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
        if (splitHz != splitTarget) { splitTarget = splitHz; split.setTarget(splitHz, smoothingSamples); }
        if (amount != driveAmountTarget) { driveAmountTarget = amount; driveAmount.setTarget(amount, smoothingSamples); }
    }
    void start(double hz, int noteVelocity) noexcept
    {
        // Keep FIR/IIR history alive: clearing a live oversampler would erase
        // pending audio before its reported latency and click on repeated keys.
        phase = 0;
        rich.reset();
        growl.start(); round.start(); reese.start();
        frequency = hz;
        // Initialize even the FIR pre-ringing with the new pitch. Otherwise a
        // fully suspended voice could depend on its previous notch setting.
        for (auto& path : gritPaths) path.setPitch(hz, rate);
        growl.setNotch(hz, rate); round.setNotch(hz, rate); reese.setNotch(hz, rate);
        currentNotchHz = hz;
        velocity.reset(std::clamp(noteVelocity, 1, 127) / 127.0);
        envelope.start();
        pendingAttack = true;
    }
    void stop() noexcept { envelope.stop(); }
    // Still producing sound: held, releasing, or draining its aligned delay.
    bool sounding() const noexcept { return envelope.active() || pendingAttack || lastOutput != 0.0f; }
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
        // Destroyed grit A runs at full drive, deep in its fold region.
        const auto amount = destroyed && grit == AnvilGrit::a ? 1.0f : static_cast<float>(driveAmount.next());
        if (destroyed && grit == AnvilGrit::a) driveAmount.next();
        if (cutoff != currentSplit)
        {
            currentSplit = cutoff;
            for (auto& path : gritPaths) path.setSplit(cutoff, rate);
            growl.setSplit(cutoff, rate); round.setSplit(cutoff, rate); reese.setSplit(cutoff, rate);
        }
        float sine = 0.0f, growlSource = 0.0f;
        BassExciter::Sources sources;
        const auto hz = frequency * bendRatio;
        if (envelope.active())
        {
            sine = sineGain * dsp_core::sine(phase);
            phase += static_cast<std::uint32_t>(std::llround(std::clamp(hz / rate, 0.0, 0.5) * 4294967296.0));
            if (grit != AnvilGrit::a) growlSource = withMode([&](auto& voice) noexcept { return voice.source(hz, rate); });
            else
            {
                sources = rich.nextSources(hz, rate, amount);
                if (excitation != nullptr) sources = *excitation;
            }
        }
        // Only the selected candidate runs; the other's state stays idle.
        float dryDriven = 0.0f, unisonDriven = 0.0f, growlDriven = 0.0f;
        if (grit != AnvilGrit::a) growlDriven = withMode([&](auto& voice) noexcept { return voice.drive(growlSource, amount); });
        else
        {
            dryDriven = gritPaths[0].drive(sources.dry, amount);
            // Stronger wet saturation is level-compensated inside its 4x stage;
            // the user's normalized blend and overall grit gain remain unchanged.
            unisonDriven = gritPaths[1].drive(sources.unison, amount, 6.0f);
        }
        // Sine, envelope and attack marker share the FIR's integer delay.
        // This delays release/retrigger events too, not just steady waveforms.
        const auto frame = aligned.next({sine, static_cast<float>(envelope.next() * velocity.next()), pendingAttack, hz});
        // Stronger grit must neither cancel the clean fundamental nor bring
        // competing sub harmonics back. Track its notch at FIR-aligned pitch.
        if (frame.frequency > 0.0 && frame.frequency != currentNotchHz)
        {
            currentNotchHz = frame.frequency;
            for (auto& path : gritPaths) path.setPitch(currentNotchHz, rate);
            growl.setNotch(currentNotchHz, rate); round.setNotch(currentNotchHz, rate); reese.setNotch(currentNotchHz, rate);
        }
        float upper = 0.0f;
        if (grit != AnvilGrit::a)
            upper = (grit == AnvilGrit::b ? growlGain : grit == AnvilGrit::c ? roundGain : reeseGain)
                  * withMode([&](auto& voice) noexcept { return voice.cleanup(growlDriven); });
        else
        {
            // Separate nonlinear paths preserve the original grit timbre. Blend
            // only after their drive/cleanup; a shared drive would colour the dry.
            const auto dry = richGain * gritPaths[0].cleanup(dryDriven);
            const auto unison = richGain * gritPaths[1].cleanup(unisonDriven);
            upper = component == BassComponent::dryGrit ? dry
                  : component == BassComponent::unisonGrit ? unison : dryMix * dry + unisonMix * unison;
        }
        if (destroyed) upper *= destroyedGain[static_cast<std::size_t>(grit)];
        pendingAttack = false;
        if (frame.attack)
        {
            transitionStart = lastOutput;
            transitionRemaining = transitionSamples;
        }
        // Final HP intentionally bypassed. Gain after the LP bounds filter
        // ringing by the fixed envelope, including delayed note-off release.
        const auto cleanInput = component == BassComponent::all || component == BassComponent::clean ? frame.sine : 0.0f;
        const auto gritInput = component == BassComponent::clean ? 0.0f : upper;
        auto output = finalLowpass.next(cleanInput + gritInput) * frame.amplitude;
        if (transitionRemaining > 0)
        {
            const auto oldWeight = static_cast<float>(transitionRemaining--) / static_cast<float>(transitionSamples);
            output = oldWeight * transitionStart + (1.0f - oldWeight) * output;
        }
        lastOutput = output;
        return {frame.sine * frame.amplitude, upper * frame.amplitude, output};
    }
private:
    // Instrument-specific grit routing; both paths share the oversampler type
    // and so the same latency. Their histories survive live retriggers.
    struct GritPath
    {
        void prepare(double splitHz, double rate) noexcept
        {
            oversampler.prepare();
            setSplit(splitHz, rate);
            notch.set(dsp_core::Svf::Type::notch, 110.0, rate, 0.7);
            highpass.set(dsp_core::Svf::Type::highpass, 260.0, rate);
            body.set(dsp_core::Svf::Type::lowpass, 3500.0, rate);
        }
        void reset() noexcept
        {
            oversampler.reset(); notch.reset(); highpass.reset(); body.reset();
            for (auto& filter : preSplit) filter.reset();
            for (auto& filter : postSplit) filter.reset();
        }
        // Linkwitz-Riley 24 dB/oct: two Butterworth sections, before and after.
        void setSplit(double hz, double rate) noexcept
        {
            for (auto& filter : preSplit) filter.set(dsp_core::Svf::Type::highpass, hz, rate);
            for (auto& filter : postSplit) filter.set(dsp_core::Svf::Type::highpass, hz, rate);
        }
        void setPitch(double hz, double rate) noexcept
        {
            notch.set(dsp_core::Svf::Type::notch, hz, rate, 0.7);
            // Capped at 0.45 x rate, as in the accepted Ironstack grit.
            body.set(dsp_core::Svf::Type::lowpass, std::min(std::max(3500.0, 4.0 * hz), 0.45 * rate), rate);
        }
        float drive(float input, float amount, float boost = 1.0f) noexcept
        {
            for (auto& filter : preSplit) input = filter.next(input);
            return oversampler.next(input,
                [amount, boost](float value) noexcept { return dsp_core::parallelSawDrive(value * boost, amount) / boost; });
        }
        float cleanup(float input) noexcept
        {
            for (auto& filter : postSplit) input = filter.next(input);
            return body.next(highpass.next(notch.next(input)));
        }
        dsp_core::Oversampler4xFloat oversampler;
        std::array<dsp_core::Svf, 2> preSplit{}, postSplit{};
        dsp_core::Svf notch, highpass, body;
    };
    struct Frame { float sine = 0.0f, amplitude = 0.0f; bool attack = false; double frequency = 0.0; };
    dsp_core::FixedEnvelope envelope;
    BassExciter rich;
    AnvilGrowl growl;
    AnvilRound round;
    AnvilReese reese;
    AnvilGrit grit = AnvilGrit::a;
    bool destroyed = false;
    // Runs one step on the selected B-D voice; only that one ever runs.
    template <typename Step>
    float withMode(Step&& step) noexcept
    {
        return grit == AnvilGrit::b ? step(growl) : grit == AnvilGrit::c ? step(round) : step(reese);
    }
    std::array<GritPath, 2> gritPaths;
    dsp_core::FixedDelay<Frame, latencySamples> aligned;
    dsp_core::Svf finalLowpass;
    dsp_core::LinearRamp split, driveAmount, velocity;
    double splitTarget = 140.0, driveAmountTarget = 0.5, currentSplit = -1.0;
    double currentNotchHz = -1.0;
    double rate = 44100.0, frequency = 0.0;
    std::uint32_t phase = 0;
    float lastOutput = 0.0f, transitionStart = 0.0f;
    int transitionSamples = 89, smoothingSamples = 441, transitionRemaining = 0;
    bool pendingAttack = false;
};
}
