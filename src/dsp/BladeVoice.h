#pragma once

#include <algorithm>
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
// Blade, the lead (SPEC section 4): one mono voice on the highest held key.
// Mode A "singing": a saw and a pulse a few cents apart, a warm 4x drive, a
// presence peak near 2.6 kHz and a low-pass near 11 kHz. Exactly mono: the
// engine writes the same sample to both outputs.
//
// The oscillators and drive run ahead; the envelope, velocity and an attack
// marker travel through a delay equal to the oversampler's latency, so the
// gain and the 2 ms retrigger crossfade land exactly on the sound they shape
// (the same scheme as Anvil). The drive sees the velocity-free signal, so
// velocity changes loudness only.
class BladeVoice
{
public:
    static constexpr int latencySamples = dsp_core::Oversampler4xFloat::latencySamples;
    static constexpr int controlBlock = 32;

    void prepare(double sampleRate, int attackSamples, int releaseSamples, int transition) noexcept
    {
        rate = sampleRate;
        transitionSamples = std::max(1, transition);
        envelope.prepare(attackSamples, releaseSamples);
        oversampler.prepare();
        // Thin the lows so the lead never crowds Anvil or Furnace's roots.
        highpass.set(dsp_core::Svf::Type::highpass, 160.0, rate);
        presence.set(dsp_core::Svf::Type::bell, 2600.0, rate, 0.9, 4.0);
        lowpass.set(dsp_core::Svf::Type::lowpass, 11000.0, rate);
        pulseOffset = dsp_core::WavetableOscillator::phaseOffset(pulseWidth);
        dsp_core::sineTable(); // build the shared tables off the audio thread
        reset();
    }
    void reset() noexcept
    {
        envelope.reset();
        velocity.reset(0.0);
        oversampler.reset();
        aligned.reset();
        highpass.reset(); presence.reset(); lowpass.reset();
        saw.reset(0x2b7e1516u); pulse.reset(0x9e3779b9u);
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
        // pulse = shifted saw - saw: one bank level serves both, alias-free.
        const auto source = sawWeight * saw.next() - pulseWeight * pulse.nextBlend(pulseOffset, 1.0f);
        const auto driven = oversampler.next(source, [](float x) noexcept
        {
            // Warm and slightly asymmetric: even harmonics give the lead its
            // vocal "singing" edge; unity gain at small signals.
            constexpr float bias = 0.15f;
            return std::tanh(drive * x + bias) - std::tanh(bias);
        });
        const auto frame = aligned.next({static_cast<float>(envelope.next() * velocity.next()), pendingAttack});
        pendingAttack = false;
        if (frame.attack)
        {
            transitionStart = lastOutput;
            transitionRemaining = transitionSamples;
        }
        auto output = outputGain * lowpass.next(presence.next(highpass.next(driven))) * frame.amplitude;
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
        void reset() noexcept { delay.reset(); }
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
    void setPitch(double hz) noexcept
    {
        // A held note without bend keeps its pitch: skip the table lookups.
        if (hz == pitchHz) return;
        pitchHz = hz;
        const auto& bank = dsp_core::WavetableBank::saw();
        saw.setFrequency(bank, hz * detuneDown, rate);
        pulse.setFrequency(bank, hz * detuneUp, rate);
    }
    // Starting voicing, tuned by listening (round 3).
    static constexpr double pulseWidth = 0.38;
    static constexpr float sawWeight = 0.6f, pulseWeight = 0.4f;
    static constexpr float drive = 2.5f;
    static constexpr float outputGain = 0.5f;
    static constexpr double detuneDown = 0.99769, detuneUp = 1.00231; // -4 and +4 cents

    double rate = 48000.0, frequency = 0.0, pitchHz = -1.0;
    int transitionSamples = 96, transitionRemaining = 0, controlCountdown = 0;
    bool pendingAttack = false;
    float lastOutput = 0.0f, transitionStart = 0.0f;
    std::uint32_t pulseOffset = 0;
    dsp_core::FixedEnvelope envelope;
    dsp_core::LinearRamp velocity;
    dsp_core::WavetableOscillator saw, pulse;
    dsp_core::Oversampler4xFloat oversampler;
    AlignedFrames aligned;
    dsp_core::Svf highpass, presence, lowpass;
};
}
