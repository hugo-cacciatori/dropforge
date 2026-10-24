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
    bassNotes.reset();
    bassNotes.setPolicy(!routingConfigured ? BassNotes::Policy::locked
        : rhythmEnabled ? BassNotes::Policy::lowest : BassNotes::Policy::newest);
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
    if (routingConfigured && bass == bassEnabled && rhythm == rhythmEnabled) return;
    routingConfigured = true;
    const auto bassWasEnabled = bassEnabled;
    const auto rhythmWasEnabled = rhythmEnabled;
    bassEnabled = bass;
    rhythmEnabled = rhythm;
    const auto change = bassNotes.setPolicy(rhythm ? BassNotes::Policy::lowest : BassNotes::Policy::newest);
    if (bass != bassWasEnabled)
    {
        bassFade.setTarget(bass ? 1.0 : 0.0, fadeSamples);
        if (bass)
        {
            bassSuspended = false;
            updateBass(bassNotes.target().note >= 0 ? BassNotes::Change::attack : BassNotes::Change::release);
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
            bassNotes.forEachNewestHeldKey(rhythmVoiceCount, [this](int channel, int note, int velocity) noexcept
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
    updateBass(bassNotes.noteOn(channel, note, velocity));
    if (rhythmEnabled) startRhythm(channel, note, velocity);
}

void Engine::startRhythm(int channel, int note, int velocity) noexcept
{
    auto selected = voices.end();
    for (auto it = voices.begin(); it != voices.end(); ++it)
        if (it->active() && it->channel == channel && it->note == note)
        {
            selected = it;
            break;
        }
    if (selected == voices.end())
        selected = std::find_if(voices.begin(), voices.end(), [](const Voice& v) { return !v.active(); });
    if (selected == voices.end())
        selected = std::min_element(voices.begin(), voices.end(), [](const Voice& a, const Voice& b)
        {
            // Reuse released voices before interrupting a held note, then oldest first.
            if (a.keyDown != b.keyDown)
                return !a.keyDown;
            return a.age < b.age;
        });
    auto& voice = *selected;
    voice.transitionStart = voice.lastOutput;
    voice.transitionRemaining = transitionSamples;
    voice.oscillators.reset(channel, note);
    voice.envelope.start();
    voice.frequency = 440.0 * std::exp2((note - 69.0) / 12.0);
    voice.velocity = std::min(velocity, 127) / 127.0;
    voice.channel = channel;
    voice.note = note;
    voice.keyDown = true;
    voice.age = ++serial;
}

void Engine::noteOff(int channel, int note) noexcept
{
    updateBass(bassNotes.noteOff(channel, note));
    for (auto& voice : voices)
        if (voice.keyDown && voice.channel == channel && voice.note == note)
        {
            voice.keyDown = false;
            voice.envelope.stop();
        }
}

void Engine::allNotesOff(int channel) noexcept
{
    updateBass(bassNotes.allNotesOff(channel));
    for (auto& voice : voices)
        if (voice.channel == channel && voice.keyDown)
        {
            voice.keyDown = false;
            voice.envelope.stop();
        }
}

void Engine::updateBass(BassNotes::Change change) noexcept
{
    if (!bassEnabled) return;
    if (change == BassNotes::Change::attack)
    {
        const auto target = bassNotes.target();
        bassChannel = target.channel;
        bassBendOffset.reset(0.0);
        bassVoice.start(target.frequency, target.velocity);
    }
    else if (change == BassNotes::Change::retarget)
    {
        const auto target = bassNotes.target();
        const auto oldRatio = bend[static_cast<std::size_t>(bassChannel - 1)].value() + bassBendOffset.value();
        bassChannel = target.channel;
        bassBendOffset.reset(oldRatio - bend[static_cast<std::size_t>(bassChannel - 1)].value());
        bassBendOffset.setTarget(0.0, transitionSamples);
        bassVoice.retargetVelocity(target.velocity);
    }
    else if (change == BassNotes::Change::release)
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
    for (int sample = 0; sample < numSamples; ++sample)
    {
        std::array<double, 16> ratios{};
        for (std::size_t channel = 0; channel < bend.size(); ++channel)
            ratios[channel] = bend[channel].next();
        const auto rhythm = rhythmSuspended ? RhythmOscillators::Stereo{} : nextRhythm(ratios);
        const auto rhythmGain = rhythmFade.next() * rhythmMixLevel.next();
        const auto rhythmLeft = static_cast<float>(rhythm.left * rhythmGain);
        const auto rhythmRight = static_cast<float>(rhythm.right * rhythmGain);
        const auto bassGain = bassFade.next() * bassMixLevel.next();
        const auto bass = bassSuspended ? 0.0f : static_cast<float>(bassGain *
            bassVoice.next(ratios[static_cast<std::size_t>(bassChannel - 1)] + bassBendOffset.next(), component).output);
        // Once inaudible, discard histories and stop synthesis. MIDI tracking
        // continues so re-entry uses held keys, never stale oscillator tails.
        if (!bassEnabled && bassGain == 0.0 && !bassSuspended)
        {
            bassVoice.reset(); bassBendOffset.reset(0.0); bassSuspended = true;
        }
        if (!rhythmEnabled && rhythmGain == 0.0 && !rhythmSuspended)
        {
            clearRhythm(); rhythmSuspended = true;
        }
        if (taps.bassLeft) taps.bassLeft[sample] = bass;
        if (taps.bassRight) taps.bassRight[sample] = bass;
        if (taps.rhythmLeft) taps.rhythmLeft[sample] = rhythmLeft;
        if (taps.rhythmRight) taps.rhythmRight[sample] = rhythmRight;
        const auto mask = static_cast<unsigned>(outputMask);
        left[sample] = ((mask & 1u) != 0 ? bass : 0.0f) + ((mask & 2u) != 0 ? rhythmLeft : 0.0f);
        right[sample] = ((mask & 1u) != 0 ? bass : 0.0f) + ((mask & 2u) != 0 ? rhythmRight : 0.0f);
    }
}

RhythmOscillators::Stereo Engine::nextRhythm(const std::array<double, 16>& ratios) noexcept
{
    RhythmOscillators::Stereo sum;
    double rhythmEnvelope = 0.0;
    double rhythmVelocity = 0.0;
    for (auto& voice : voices)
    {
        if (!voice.active())
        {
            voice.lastOutput = {};
            continue;
        }
        auto output = voice.oscillators.next(voice.frequency * ratios[static_cast<std::size_t>(voice.channel - 1)], rate);
        const auto envelope = voice.envelope.next();
        rhythmVelocity = std::max(rhythmVelocity, voice.velocity);
        rhythmEnvelope = std::max(rhythmEnvelope, envelope);
        const auto gain = envelope * voice.velocity;
        output.left *= gain;
        output.right *= gain;
        if (voice.transitionRemaining > 0)
        {
            // A convex blend from the last sample bounds even rapid steals,
            // without resetting a live waveform abruptly or adding a clipper.
            const auto oldWeight = static_cast<double>(voice.transitionRemaining--) / transitionSamples;
            rhythmEnvelope = std::max(rhythmEnvelope, oldWeight);
            output.left = oldWeight * voice.transitionStart.left + (1.0 - oldWeight) * output.left;
            output.right = oldWeight * voice.transitionStart.right + (1.0 - oldWeight) * output.right;
        }
        voice.lastOutput = output;
        sum.left += output.left;
        sum.right += output.right;
    }
    // Factor the strongest note's velocity out of the nonlinear drive and
    // restore it afterwards: single-note velocity changes loudness only.
    // Other chord notes retain their relative weights in the summed bus.
    // Smooth reference changes when a louder/quieter note joins or leaves.
    if (rhythmVelocity > 0.0)
    {
        if (!rhythmWasActive) rhythmLevel.reset(rhythmVelocity);
        else if (rhythmVelocity != rhythmLevelTarget) rhythmLevel.setTarget(rhythmVelocity, transitionSamples);
        rhythmLevelTarget = rhythmVelocity;
    }
    rhythmWasActive = rhythmVelocity > 0.0;
    const auto level = rhythmLevel.next();
    const auto processedLeft = rhythmBus[0].next(sum.left / level, rhythmEnvelope, level);
    const auto processedRight = rhythmBus[1].next(sum.right / level, rhythmEnvelope, level);
    // Width still comes from independent synthesis. This final bounded
    // blend reserves common energy after nonlinear processing as well.
    const auto rhythmMid = 0.5 * (processedLeft + processedRight);
    const auto rhythmSide = 0.5 * (processedLeft - processedRight) * RhythmOscillators::spread;
    return {rhythmMid + rhythmSide, rhythmMid - rhythmSide};
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
