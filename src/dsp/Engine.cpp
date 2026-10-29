#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace dropforge
{
void Engine::prepare(double sampleRate) noexcept
{
    rate = std::isfinite(sampleRate) && sampleRate >= 44100.0 && sampleRate <= 192000.0
        ? sampleRate : 44100.0;
    attackSamples = static_cast<int>(std::ceil(rate * 0.001));
    releaseSamples = static_cast<int>(std::ceil(rate * releaseSeconds));
    transitionSamples = static_cast<int>(std::ceil(rate * 0.002));
    fadeSamples = static_cast<int>(std::ceil(rate * 0.005));
    reset();
}

void Engine::reset() noexcept
{
    serial = 0;
    for (auto& wheel : bend)
        wheel.reset(1.0);
    bassVoice.prepare(rate);
    for (auto& bus : rhythmBus) bus.prepare(rate);
    clearRhythm();
    bassChannel = 1;
    bassBendOffset.reset(0.0);
    router.reset();
    router.setBassRule(rhythmEnabled ? NoteRouter::BassRule::lowest : NoteRouter::BassRule::newest);
    bassMixLevel.reset(bassMixTarget);
    rhythmMixLevel.reset(rhythmMixTarget);
    bassFade.reset(bassEnabled ? 1.0 : 0.0);
    rhythmFade.reset(rhythmEnabled ? 1.0 : 0.0);
    bassSuspended = !bassEnabled;
    rhythmSuspended = !rhythmEnabled;
}

void Engine::clearRhythm() noexcept
{
    for (auto& voice : voices)
    {
        voice = Voice{};
        voice.envelope.prepare(attackSamples, releaseSamples);
    }
    for (auto& bus : rhythmBus) bus.reset();
    rhythmLevel.reset(1.0); rhythmLevelTarget = 1.0; rhythmWasActive = false;
}

void Engine::setLayersEnabled(bool bass, bool rhythm) noexcept
{
    if (bass == bassEnabled && rhythm == rhythmEnabled) return;
    const auto bassWasEnabled = bassEnabled;
    const auto rhythmWasEnabled = rhythmEnabled;
    bassEnabled = bass;
    rhythmEnabled = rhythm;
    const auto change = router.setBassRule(rhythm ? NoteRouter::BassRule::lowest : NoteRouter::BassRule::newest);
    if (bass != bassWasEnabled)
    {
        bassFade.setTarget(bass ? 1.0 : 0.0, fadeSamples);
        if (bass)
        {
            bassSuspended = false;
            updateBass(router.bassKey().held() ? NoteRouter::Change::attack : NoteRouter::Change::release);
        }
    }
    else if (bass) updateBass(change);
    if (rhythm != rhythmWasEnabled)
    {
        rhythmFade.setTarget(rhythm ? 1.0 : 0.0, fadeSamples);
        if (rhythm)
        {
            rhythmSuspended = false;
            // Preserve continuity during a rapid reversal; a fully faded layer
            // already had its history cleared at zero gain.
            for (auto& voice : voices)
                if (voice.keyDown) { voice.keyDown = false; voice.envelope.stop(); }
            router.forEachNewestHeldKey(rhythmVoiceCount, [this](int channel, int note, int velocity) noexcept
            {
                startRhythm(channel, note, velocity);
            });
        }
    }
}

void Engine::noteOn(int channel, int note, int velocity) noexcept
{
    if (channel < 1 || channel > 16 || note < 0 || note > 127)
        return;
    if (velocity <= 0)
    {
        noteOff(channel, note);
        return;
    }
    // Blade's change waits for its voice (M2); the router already tracks it.
    updateBass(router.noteOn(channel, note, velocity).bass);
    if (rhythmEnabled) startRhythm(channel, note, velocity);
}

void Engine::startRhythm(int channel, int note, int velocity) noexcept
{
    auto& voice = voices[NoteRouter::pickVoice(voices, channel, note)];
    voice.transitionStart = voice.lastOutput;
    voice.transitionRemaining = transitionSamples;
    voice.oscillators.reset(channel, note);
    voice.envelope.start();
    voice.frequency = NoteRouter::frequency(note);
    voice.velocity = std::min(velocity, 127) / 127.0;
    voice.channel = channel;
    voice.note = note;
    voice.keyDown = true;
    voice.age = ++serial;
}

void Engine::noteOff(int channel, int note) noexcept
{
    updateBass(router.noteOff(channel, note).bass);
    for (auto& voice : voices)
        if (voice.keyDown && voice.channel == channel && voice.note == note)
        {
            voice.keyDown = false;
            voice.envelope.stop();
        }
}

void Engine::allNotesOff(int channel) noexcept
{
    updateBass(router.allNotesOff(channel).bass);
    for (auto& voice : voices)
        if (voice.channel == channel && voice.keyDown)
        {
            voice.keyDown = false;
            voice.envelope.stop();
        }
}

void Engine::updateBass(NoteRouter::Change change) noexcept
{
    if (!bassEnabled) return;
    const auto key = router.bassKey();
    if (change == NoteRouter::Change::attack)
    {
        bassChannel = key.channel;
        bassBendOffset.reset(0.0);
        bassVoice.start(NoteRouter::bassFrequency(key.note), key.velocity);
    }
    else if (change == NoteRouter::Change::retarget)
    {
        // Same pitch, new owner channel: glide from the old channel's bend.
        const auto oldRatio = bend[static_cast<std::size_t>(bassChannel - 1)].value() + bassBendOffset.value();
        bassChannel = key.channel;
        bassBendOffset.reset(oldRatio - bend[static_cast<std::size_t>(bassChannel - 1)].value());
        bassBendOffset.setTarget(0.0, transitionSamples);
        bassVoice.retargetVelocity(key.velocity);
    }
    else if (change == NoteRouter::Change::release)
    {
        bassVoice.stop();
    }
}

void Engine::pitchBend(int channel, int wheel) noexcept
{
    if (channel >= 1 && channel <= 16)
    {
        const auto position = std::clamp(wheel, 0, 16383) - 8192;
        const auto semitones = 2.0 * position / (position < 0 ? 8192.0 : 8191.0);
        bend[static_cast<std::size_t>(channel - 1)].setTarget(std::exp2(semitones / 12.0), transitionSamples);
    }
}

void Engine::setLayerLevels(float bassDb, float rhythmDb) noexcept
{
    const auto gain = [](float db) noexcept
    {
        db = std::isfinite(db) ? std::clamp(db, -60.0f, 6.0f) : 0.0f;
        return db <= -60.0f ? 0.0 : std::pow(10.0, db / 20.0);
    };
    const auto bass = gain(bassDb), rhythm = gain(rhythmDb);
    if (bass != bassMixTarget) { bassMixTarget = bass; bassMixLevel.setTarget(bass, fadeSamples); }
    if (rhythm != rhythmMixTarget) { rhythmMixTarget = rhythm; rhythmMixLevel.setTarget(rhythm, fadeSamples); }
}

void Engine::process(float* left, float* right, int numSamples, LayerMask outputMask, LayerTaps taps,
                     BassComponent component) noexcept
{
    const auto offset = [](float* tap, int at) noexcept { return tap ? tap + at : nullptr; };
    for (int at = 0; at < numSamples; at += controlBlock)
        processBlock(left + at, right + at, std::min(controlBlock, numSamples - at), outputMask,
                     {offset(taps.bassLeft, at), offset(taps.bassRight, at),
                      offset(taps.rhythmLeft, at), offset(taps.rhythmRight, at)},
                     component);
}

void Engine::processBlock(float* left, float* right, int count, LayerMask outputMask, LayerTaps taps,
                          BassComponent component) noexcept
{
    const auto samples = static_cast<std::size_t>(count);
    // Controls first: they never depend on the audio, so each ramp runs over
    // the whole block before the layers read it.
    for (std::size_t channel = 0; channel < bend.size(); ++channel)
        for (std::size_t i = 0; i < samples; ++i)
            bendRatio[channel][i] = bend[channel].next();
    for (std::size_t i = 0; i < samples; ++i)
    {
        rhythmGain[i] = rhythmFade.next() * rhythmMixLevel.next();
        bassGain[i] = bassFade.next() * bassMixLevel.next();
    }
    // A layer switched off stops synthesis at the sample its fade reaches
    // zero, discarding its histories. MIDI tracking continues, so re-entry
    // uses the held keys, never stale oscillator tails.
    const auto audibleUntil = [&](bool suspended, bool enabled, const Block& gain) noexcept
    {
        if (suspended) return std::size_t{0};
        if (!enabled)
            for (std::size_t i = 0; i < samples; ++i)
                if (gain[i] == 0.0) return i + 1;
        return samples;
    };
    const auto rhythmCount = audibleUntil(rhythmSuspended, rhythmEnabled, rhythmGain);
    renderRhythm(static_cast<int>(rhythmCount));
    if (!rhythmSuspended && !rhythmEnabled && rhythmCount > 0 && rhythmGain[rhythmCount - 1] == 0.0)
    {
        clearRhythm(); rhythmSuspended = true;
    }
    const auto bassCount = audibleUntil(bassSuspended, bassEnabled, bassGain);
    const auto& bassBend = bendRatio[static_cast<std::size_t>(bassChannel - 1)];
    std::array<float, controlBlock> bass{};
    for (std::size_t i = 0; i < bassCount; ++i)
        bass[i] = static_cast<float>(bassGain[i] * bassVoice.next(bassBend[i] + bassBendOffset.next(), component).output);
    if (!bassSuspended && !bassEnabled && bassCount > 0 && bassGain[bassCount - 1] == 0.0)
    {
        bassVoice.reset(); bassBendOffset.reset(0.0); bassSuspended = true;
    }

    const auto mask = static_cast<unsigned>(outputMask);
    for (std::size_t i = 0; i < samples; ++i)
    {
        const auto furnaceLeft = i < rhythmCount ? static_cast<float>(rhythmLeft[i] * rhythmGain[i]) : 0.0f;
        const auto furnaceRight = i < rhythmCount ? static_cast<float>(rhythmRight[i] * rhythmGain[i]) : 0.0f;
        if (taps.bassLeft) taps.bassLeft[i] = bass[i];
        if (taps.bassRight) taps.bassRight[i] = bass[i];
        if (taps.rhythmLeft) taps.rhythmLeft[i] = furnaceLeft;
        if (taps.rhythmRight) taps.rhythmRight[i] = furnaceRight;
        left[i] = ((mask & 1u) != 0 ? bass[i] : 0.0f) + ((mask & 2u) != 0 ? furnaceLeft : 0.0f);
        right[i] = ((mask & 1u) != 0 ? bass[i] : 0.0f) + ((mask & 2u) != 0 ? furnaceRight : 0.0f);
    }
}

void Engine::renderRhythm(int count) noexcept
{
    const auto samples = static_cast<std::size_t>(count);
    std::fill_n(sumLeft.begin(), samples, 0.0);
    std::fill_n(sumRight.begin(), samples, 0.0);
    std::fill_n(strongestEnvelope.begin(), samples, 0.0);
    std::fill_n(strongestVelocity.begin(), samples, 0.0);
    // Voice by voice over the block; the sums still add voices in bank order,
    // so every sample is bit-identical to a sample-by-sample loop.
    for (auto& voice : voices)
    {
        const auto& ratio = bendRatio[static_cast<std::size_t>(std::max(voice.channel, 1) - 1)];
        for (std::size_t i = 0; i < samples; ++i)
        {
            if (!voice.active())
            {
                voice.lastOutput = {};
                continue;
            }
            auto output = voice.oscillators.next(voice.frequency * ratio[i], rate);
            const auto envelope = voice.envelope.next();
            strongestVelocity[i] = std::max(strongestVelocity[i], voice.velocity);
            strongestEnvelope[i] = std::max(strongestEnvelope[i], envelope);
            const auto gain = envelope * voice.velocity;
            output.left *= gain;
            output.right *= gain;
            if (voice.transitionRemaining > 0)
            {
                // A convex blend from the last sample bounds even rapid steals,
                // without resetting a live waveform abruptly or adding a clipper.
                const auto oldWeight = static_cast<double>(voice.transitionRemaining--) / transitionSamples;
                strongestEnvelope[i] = std::max(strongestEnvelope[i], oldWeight);
                output.left = oldWeight * voice.transitionStart.left + (1.0 - oldWeight) * output.left;
                output.right = oldWeight * voice.transitionStart.right + (1.0 - oldWeight) * output.right;
            }
            voice.lastOutput = output;
            sumLeft[i] += output.left;
            sumRight[i] += output.right;
        }
    }
    for (std::size_t i = 0; i < samples; ++i)
    {
        // Factor the strongest note's velocity out of the nonlinear drive and
        // restore it afterwards: single-note velocity changes loudness only.
        // Other chord notes retain their relative weights in the summed bus.
        // Smooth reference changes when a louder/quieter note joins or leaves.
        const auto velocity = strongestVelocity[i];
        if (velocity > 0.0)
        {
            if (!rhythmWasActive) rhythmLevel.reset(velocity);
            else if (velocity != rhythmLevelTarget) rhythmLevel.setTarget(velocity, transitionSamples);
            rhythmLevelTarget = velocity;
        }
        rhythmWasActive = velocity > 0.0;
        const auto level = rhythmLevel.next();
        const auto processedLeft = rhythmBus[0].next(sumLeft[i] / level, strongestEnvelope[i], level);
        const auto processedRight = rhythmBus[1].next(sumRight[i] / level, strongestEnvelope[i], level);
        // Width still comes from independent synthesis. This final bounded
        // blend reserves common energy after nonlinear processing as well.
        const auto mid = 0.5 * (processedLeft + processedRight);
        const auto side = 0.5 * (processedLeft - processedRight) * RhythmOscillators::spread;
        rhythmLeft[i] = mid + side;
        rhythmRight[i] = mid - side;
    }
}

Engine::NoteTarget Engine::bassTarget() const noexcept
{
    const auto key = router.bassKey();
    if (!bassEnabled || !key.held()) return {};
    return {key.channel, key.note, key.velocity, NoteRouter::bassFrequency(key.note)};
}

Engine::NoteTarget Engine::leadTarget() const noexcept
{
    const auto key = router.leadKey();
    if (!key.held()) return {};
    return {key.channel, key.note, key.velocity, NoteRouter::frequency(key.note)};
}

std::array<Engine::RhythmTarget, Engine::rhythmVoiceCount> Engine::rhythmTargets() const noexcept
{
    std::array<RhythmTarget, rhythmVoiceCount> result{};
    for (std::size_t i = 0; i < voices.size(); ++i)
    {
        const auto& v = voices[i];
        result[i] = {v.channel, v.note, v.channel > 0 ? v.frequency * bend[static_cast<std::size_t>(v.channel - 1)].value() : 0.0,
                     v.keyDown, v.active()};
    }
    return result;
}
}
