#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <dsp_core/FixedDelay.h>
#include <dsp_core/FixedEnvelope.h>
#include <dsp_core/LinearRamp.h>
#include <dsp_core/Oversampler4x.h>
#include <dsp_core/Svf.h>
#include <dsp_core/WavetableBank.h>

namespace dropforge
{
// Blade, the lead (SPEC sections 4 and 12): one mono voice on the highest
// held key, in four modes, each with its own Destruction design.
//   A Forge "singing": saw and pulse a few cents apart, warm drive, presence.
//   B Riot "scream":   two bright saws, hard drive, an "ee" vowel peak.
//   C Halo "air":      three soft saws and a sine an octave down, gentle
//                      even-harmonic saturation, a soft top.
//   D Alloy "FM":      a sine phase-modulated at 1:1 and 2:1 with feedback,
//                      the same ratio family as Furnace D.
// Exactly mono: the engine writes the same sample to both outputs.
//
// The sources and drive run ahead; the envelope, velocity and an attack
// marker travel through a delay equal to the oversampler's latency, so the
// gain and the 2 ms retrigger crossfade land exactly on the sound they shape
// (the same scheme as Anvil). The drive sees the velocity-free signal, so
// velocity changes loudness only.
class BladeVoice
{
public:
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples;
    static constexpr int controlBlock = 32;

    // One complete voicing: what a mode (clean or destroyed) sets.
    struct Design
    {
        int source = 0;                    // 0 saw+pulse, 1 two saws, 2 three saws + sub, 3 FM
        double detuneCents = 4.0;          // outer oscillators, +- this
        float drive = 2.5f, bias = 0.15f;  // tanh(drive x + bias) - tanh(bias)
        float fold = 0.0f;                 // wavefolder mix after the drive (Destruction)
        double preLowpassHz = 0.0;         // tames the source before a hard drive; 0 = off
        bool keyTrack = false;             // drive and fold ease off near the top of the keyboard
        double bellHz = 2600.0, bellDb = 4.0, bellQ = 0.9;
        double bell2Hz = 3200.0, bell2Db = 0.0, bell2Q = 2.0;
        double lowpassHz = 11000.0;
        double fmIndex1 = 0.0, fmIndex2 = 0.0, fmFeedback = 0.0;
        float gain = 0.5f;                 // loudness match against mode A
    };
    static Design design(int mode, bool destroy) noexcept
    {
        Design d;
        switch (std::clamp(mode, 0, 3))
        {
            case 1: // Riot: brighter, harder, the "ee" formants (F2 2.3 kHz, F3 3.2 kHz).
                d.source = 1; d.detuneCents = 7.0; d.drive = 5.0f; d.bias = 0.1f;
                d.bellHz = 2300.0; d.bellDb = 7.0; d.bellQ = 1.5;
                d.bell2Hz = 3200.0; d.bell2Db = 4.0; d.bell2Q = 2.0;
                d.lowpassHz = 12000.0; d.gain = 0.382f;
                if (destroy) { d.drive = 9.0f; d.fold = 0.6f; d.preLowpassHz = 3500.0; d.keyTrack = true; d.bellHz = 2650.0; d.bell2Hz = 3700.0; d.lowpassHz = 9000.0; d.gain = 0.384f; }
                break;
            case 2: // Halo: smooth and airy, even harmonics, soft top.
                d.source = 2; d.detuneCents = 9.0; d.drive = 1.5f; d.bias = 0.3f;
                d.bellHz = 1800.0; d.bellDb = -2.0; d.bellQ = 0.7;
                d.lowpassHz = 7000.0; d.gain = 1.16f;
                if (destroy) { d.detuneCents = 4.0; d.drive = 6.0f; d.lowpassHz = 6000.0; d.gain = 0.604f; }
                break;
            case 3: // Alloy: glassy FM; integer ratios keep it harmonic.
                d.source = 3; d.drive = 2.0f; d.bias = 0.1f;
                d.bellHz = 3000.0; d.bellDb = 3.0; d.bellQ = 1.0;
                d.lowpassHz = 12000.0;
                d.fmIndex1 = 1.6; d.fmIndex2 = 0.6; d.fmFeedback = 0.25; d.gain = 0.766f;
                if (destroy) { d.fmIndex1 = 3.0; d.fmIndex2 = 1.4; d.fmFeedback = 0.7; d.drive = 4.0f; d.gain = 0.411f; }
                break;
            default: // Forge: the singing lead.
                if (destroy) { d.drive = 8.0f; d.fold = 0.3f; d.keyTrack = true; d.bellQ = 2.0; d.bellDb = 3.0; d.gain = 0.454f; }
                break;
        }
        return d;
    }

    void prepare(double sampleRate, int attackSamples, int releaseSamples, int transition) noexcept
    {
        rate = sampleRate;
        transitionSamples = std::max(1, transition);
        envelope.prepare(attackSamples, releaseSamples);
        oversampler.prepare();
        pulseOffset = dsp_core::WavetableOscillator::phaseOffset(pulseWidth);
        dsp_core::sineTable(); // build the shared tables off the audio thread
        configure();
        reset();
    }
    // Choose the voicing; set it while the voice is silent (the engine
    // crossfades to a fresh voice when a mode changes while it sounds).
    void setMode(int mode, bool destroy) noexcept
    {
        current = design(mode, destroy);
        configure();
    }
    void reset() noexcept
    {
        envelope.reset();
        velocity.reset(0.0);
        oversampler.reset();
        aligned.reset();
        highpass.reset(); bell.reset(); bell2.reset(); lowpass.reset(); preLowpass.reset();
        for (std::size_t i = 0; i < oscillators.size(); ++i) oscillators[i].reset(startPhases[i]);
        subPhase = fmPhase = 0;
        fmLast = 0.0f;
        frequency = 0.0;
        pitchHz = -1.0;
        controlCountdown = 0;
        pendingAttack = false;
        lastOutput = transitionStart = 0.0f;
        transitionRemaining = 0;
    }
    // A hard attack. A sounding voice keeps its oscillators running (no phase
    // jump) and crossfades from its last output once the new note arrives.
    void start(double hz, int noteVelocity) noexcept
    {
        frequency = hz;
        controlCountdown = 0; // take the new pitch on the next sample
        velocity.reset(std::clamp(noteVelocity, 1, 127) / 127.0);
        envelope.start();
        pendingAttack = true;
    }
    void stop() noexcept { envelope.stop(); }
    void retargetVelocity(int noteVelocity) noexcept
    {
        velocity.setTarget(std::clamp(noteVelocity, 1, 127) / 127.0, transitionSamples);
    }
    // Held, releasing or draining its delay.
    bool sounding() const noexcept { return envelope.active() || pendingAttack || lastOutput != 0.0f || aligned.pending(); }

    float next(double bendRatio) noexcept
    {
        if (controlCountdown == 0)
        {
            setPitch(frequency * bendRatio);
            controlCountdown = controlBlock;
        }
        --controlCountdown;
        const auto drive = activeDrive, bias = current.bias, fold = activeFold;
        const auto offset = biasOffset;
        auto input = source();
        if (current.preLowpassHz > 0.0) input = preLowpass.next(input);
        const auto driven = oversampler.next(input, [drive, bias, fold, offset, table = dsp_core::sineTable()](float x) noexcept
        {
            // Slightly asymmetric: even harmonics give each lead a vocal edge;
            // unity gain at small signals. Destruction adds a sine fold on top,
            // read from the shared table (y is within +-1, 2.5 radians a unit).
            auto y = std::tanh(drive * x + bias) - offset;
            if (fold > 0.0f)
            {
                constexpr float cycle = 683565275.57643158f; // 2^32 / (2 pi)
                const auto phase = static_cast<std::uint32_t>(static_cast<std::int64_t>(2.5f * y * cycle));
                y = (1.0f - fold) * y + fold * dsp_core::sine(table, phase);
            }
            return y;
        });
        const auto frame = aligned.next({static_cast<float>(envelope.next() * velocity.next()), pendingAttack});
        pendingAttack = false;
        if (frame.attack)
        {
            transitionStart = lastOutput;
            transitionRemaining = transitionSamples;
        }
        auto output = current.gain * lowpass.next(bell2.next(bell.next(highpass.next(driven)))) * frame.amplitude;
        if (transitionRemaining > 0)
        {
            const auto oldWeight = static_cast<float>(transitionRemaining--) / static_cast<float>(transitionSamples);
            output = oldWeight * transitionStart + (1.0f - oldWeight) * output;
        }
        lastOutput = std::abs(output) < 1.0e-20f ? 0.0f : output;
        return lastOutput;
    }

private:
    struct Frame { float amplitude = 0.0f; bool attack = false; };
    struct AlignedFrames
    {
        void reset() noexcept { delay.reset(); busy = 0; }
        Frame next(const Frame& in) noexcept
        {
            const auto out = delay.next(in);
            // Counts frames still in flight so sounding() stays true while a
            // release or attack drains through the delay.
            busy = in.amplitude != 0.0f || in.attack ? latencySamples : std::max(0, busy - 1);
            return out;
        }
        bool pending() const noexcept { return busy > 0; }
        dsp_core::FixedDelay<Frame, latencySamples> delay;
        int busy = 0;
    };

    float source() noexcept
    {
        switch (current.source)
        {
            case 1: return 0.5f * (oscillators[0].next() + oscillators[1].next());
            case 2:
            {
                // Three soft saws and a sine an octave down for body.
                const auto saws = (oscillators[0].next() + oscillators[1].next() + oscillators[2].next()) * (1.0f / 3.0f);
                const auto sub = dsp_core::sine(sineTable, subPhase);
                subPhase += subIncrement;
                return 0.7f * saws + 0.45f * sub;
            }
            case 3:
            {
                // sin(p + a sin p + b sin 2p + feedback x previous output).
                const auto* table = sineTable;
                const auto phase = fmPhase;
                fmPhase += fmIncrement;
                const auto modulation = fmDepth1 * dsp_core::sine(table, phase) + fmDepth2 * dsp_core::sine(table, 2u * phase)
                                      + fmFeedbackDepth * fmLast;
                fmLast = dsp_core::sine(table, phase + static_cast<std::uint32_t>(static_cast<std::int64_t>(modulation)));
                return 0.8f * fmLast;
            }
            default: // pulse = shifted saw - saw: one bank level, alias-free.
                return sawWeight * oscillators[0].next() - pulseWeight * oscillators[1].nextBlend(pulseOffset, 1.0f);
        }
    }
    void configure() noexcept
    {
        // Thin the lows so the lead never crowds Anvil or Furnace's roots.
        highpass.set(dsp_core::Svf::Type::highpass, 160.0, rate);
        bell.set(dsp_core::Svf::Type::bell, current.bellHz, rate, current.bellQ, current.bellDb);
        bell2.set(dsp_core::Svf::Type::bell, current.bell2Hz, rate, current.bell2Q, current.bell2Db);
        lowpass.set(dsp_core::Svf::Type::lowpass, std::min(current.lowpassHz, 0.45 * rate), rate);
        if (current.preLowpassHz > 0.0) preLowpass.set(dsp_core::Svf::Type::lowpass, current.preLowpassHz, rate);
        biasOffset = std::tanh(current.bias);
        activeDrive = current.drive;
        activeFold = current.fold;
        pitchHz = -1.0;
    }
    void setPitch(double hz) noexcept
    {
        // A held note without bend keeps its pitch: skip the table lookups.
        if (hz == pitchHz) return;
        pitchHz = hz;
        const auto& bank = dsp_core::WavetableBank::saw();
        const auto spread = std::exp2(current.detuneCents / 1200.0);
        oscillators[0].setFrequency(bank, hz / spread, rate);
        oscillators[1].setFrequency(bank, hz * spread, rate);
        oscillators[2].setFrequency(bank, hz, rate);
        const auto increment = [this](double f) noexcept
        {
            return static_cast<std::uint32_t>(std::llround(std::clamp(f / rate, 0.0, 0.45) * 4294967296.0));
        };
        subIncrement = increment(0.5 * hz);
        fmIncrement = increment(hz);
        if (current.keyTrack)
        {
            // A hard drive makes harmonics far past what 4x oversampling can
            // hold on the highest notes; there the destroyed edge eases back
            // toward a clean drive (full heat up to about C6 at 48 kHz).
            const auto heat = static_cast<float>(std::clamp((0.45 * rate / std::max(hz, 1.0) - 8.0) / 12.0, 0.0, 1.0));
            activeDrive = 1.0f + (current.drive - 1.0f) * heat;
            activeFold = current.fold * heat;
        }
        if (current.source == 3)
        {
            // Keep the FM partials (about a + 2b + feedback + 6 harmonics)
            // under 0.45 x rate: high notes lose depth, low notes keep it all.
            const auto room = 0.45 * rate / std::max(hz, 1.0) - 6.0;
            const auto depth = current.fmIndex1 + 2.0 * current.fmIndex2 + current.fmFeedback;
            const auto scale = std::clamp(room / depth, 0.0, 1.0);
            fmDepth1 = static_cast<float>(current.fmIndex1 * scale * radian);
            fmDepth2 = static_cast<float>(current.fmIndex2 * scale * radian);
            fmFeedbackDepth = static_cast<float>(current.fmFeedback * scale * radian);
        }
    }

    static constexpr double radian = 4294967296.0 / 6.283185307179586;
    static constexpr double pulseWidth = 0.38;
    static constexpr float sawWeight = 0.6f, pulseWeight = 0.4f;
    static constexpr std::array<std::uint32_t, 3> startPhases{0x2b7e1516u, 0x9e3779b9u, 0x5bd1e995u};

    Design current{};
    double rate = 48000.0, frequency = 0.0, pitchHz = -1.0;
    int transitionSamples = 96, transitionRemaining = 0, controlCountdown = 0;
    bool pendingAttack = false;
    float lastOutput = 0.0f, transitionStart = 0.0f;
    std::uint32_t pulseOffset = 0, subPhase = 0, subIncrement = 0, fmPhase = 0, fmIncrement = 0;
    float fmDepth1 = 0.0f, fmDepth2 = 0.0f, fmFeedbackDepth = 0.0f, fmLast = 0.0f;
    float activeDrive = 2.5f, activeFold = 0.0f, biasOffset = 0.0f;
    const float* sineTable = dsp_core::sineTable();
    dsp_core::FixedEnvelope envelope;
    dsp_core::LinearRamp velocity;
    std::array<dsp_core::WavetableOscillator, 3> oscillators{};
    dsp_core::Oversampler4xFloat oversampler;
    AlignedFrames aligned;
    dsp_core::Svf highpass, bell, bell2, lowpass, preLowpass;
};
}
