#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace dropforge
{
namespace
{
// Mode letters to designs (SPEC section 12).
FurnaceDesign furnaceForMode(int mode, bool destroy = false) noexcept
{
    if (destroy) return FurnaceDesign::destroyed(mode);
    switch (mode)
    {
        case 1: return FurnaceDesign::candidateB();
        case 2: return FurnaceDesign::candidateC();
        case 3: return FurnaceDesign::candidateD();
        default: return FurnaceDesign::candidateA();
    }
}
AnvilGrit gritForMode(int mode) noexcept { return static_cast<AnvilGrit>(std::clamp(mode, 0, 3)); }
}

void Engine::prepare(double sampleRate) noexcept
{
    rate = std::isfinite(sampleRate) && sampleRate >= 44100.0 && sampleRate <= 192000.0
        ? sampleRate : 44100.0;
    attackSamples = static_cast<int>(std::ceil(rate * 0.001));
    releaseSamples = static_cast<int>(std::ceil(rate * releaseSeconds));
    transitionSamples = static_cast<int>(std::ceil(rate * 0.002));
    fadeSamples = static_cast<int>(std::ceil(rate * 0.005));
    modeFadeSamples = static_cast<int>(std::ceil(rate * modeFadeSeconds));
    reset();
}

void Engine::reset() noexcept
{
    serial = 0;
    for (auto& wheel : bend)
        wheel.reset(1.0);
    wheels.fill(8192);
    bassKeyHeld = false;
    glideSamples = static_cast<int>(std::lround(rate * glideMs / 1000.0));
    // The wheel returns to the saved Amount; Vibrato's phase restarts so a
    // render always sounds the same.
    wheelTarget = wheelSaved;
    roarGoal = wheelMode == WheelMode::roar ? wheelTarget : 0.0;
    vibratoGoal = wheelMode == WheelMode::vibrato ? wheelTarget : 0.0;
    roarDepth.reset(roarGoal);
    vibratoDepth.reset(vibratoGoal);
    vibratoPhase = 0.0;
    // A pending change is applied now: nothing sounds after a reset.
    if (furnaceSetup[furnaceSlot] != furnaceTarget) { furnaceSetup[furnaceSlot] = furnaceTarget; furnaces[furnaceSlot].setDesign(furnaceForMode(furnaceTarget.mode, furnaceTarget.destroy)); }
    for (std::size_t slot = 0; slot < 2; ++slot) furnaces[slot].setDrop(furnaceSetup[slot].drop);
    if (bassSetup[bassSlot] != anvilTarget) configureAnvil(bassSlot);
    monoBlend.reset(monoOn ? 1.0 : 0.0);
    furnaceFade = bassFade = 0;
    for (auto& voice : bassVoices) voice.prepare(rate);
    for (auto& furnace : furnaces)
    {
        furnace.prepare(rate, attackSamples, releaseSamples, transitionSamples, bassEnabled);
        furnace.setParameters(rhythmDrive, rhythmTight);
    }
    bassChannel = 1;
    bassBendOffset.reset(0.0);
    router.reset();
    router.setBassRule(bassRule());
    router.setLeadRule(leadRule());
    bassMixLevel.reset(bassMixTarget);
    rhythmMixLevel.reset(rhythmMixTarget);
    bassPower.reset(bassEnabled ? 1.0 : 0.0);
    rhythmPower.reset(rhythmEnabled ? 1.0 : 0.0);
    bassSuspended = !bassEnabled;
    rhythmSuspended = !rhythmEnabled;
    leadSetup[leadSlot] = leadChoice;
    leadFade = 0;
    for (std::size_t slot = 0; slot < 2; ++slot)
    {
        blades[slot].setMode(leadSetup[slot].mode, leadSetup[slot].destroy);
        blades[slot].prepare(rate, attackSamples, releaseSamples, transitionSamples);
    }
    leadChannel = 1;
    leadKeyHeld = false;
    lastLeadHz = 0.0;
    portamentoSamples = static_cast<int>(std::lround(rate * portamentoMs / 1000.0));
    leadBendOffset.reset(0.0);
    leadMixLevel.reset(leadMixTarget);
    leadPower.reset(leadEnabled ? 1.0 : 0.0);
    leadSuspended = !leadEnabled;
}

void Engine::clearRhythm() noexcept
{
    for (auto& furnace : furnaces) furnace.clear();
    if (furnaceFade > 0) finishFurnaceFade();
}

void Engine::configureFurnace(std::size_t slot) noexcept
{
    furnaceSetup[slot] = furnaceTarget;
    furnaces[slot].setDesign(furnaceForMode(furnaceTarget.mode, furnaceTarget.destroy));
    furnaces[slot].setDrop(furnaceTarget.drop);
    furnaces[slot].prepare(rate, attackSamples, releaseSamples, transitionSamples, bassEnabled);
    furnaces[slot].setParameters(rhythmDrive, rhythmTight);
}

// Brings the playing Furnace to the chosen mode and Drop. During a fade the
// change waits; the fade's end calls this again with the latest choice.
void Engine::updateFurnace() noexcept
{
    if (furnaceFade > 0 || furnaceSetup[furnaceSlot] == furnaceTarget) return;
    if (rhythmSuspended || !furnaces[furnaceSlot].sounding())
    {
        // Silent: switch in place, as if it had always been chosen.
        configureFurnace(furnaceSlot);
        return;
    }
    furnaceSlot = 1 - furnaceSlot;
    configureFurnace(furnaceSlot);
    startHeldRhythm(furnaces[furnaceSlot]);
    furnaceFade = modeFadeSamples;
}

void Engine::setFurnaceMono(bool on) noexcept
{
    if (on == monoOn) return;
    monoOn = on;
    monoBlend.setTarget(on ? 1.0 : 0.0, static_cast<int>(std::ceil(rate * 0.020)));
}

void Engine::configureAnvil(std::size_t slot) noexcept
{
    bassSetup[slot] = anvilTarget;
    bassVoices[slot].setParameters(splitTarget, driveTarget);
    bassVoices[slot].setMode(gritForMode(anvilTarget.mode), anvilTarget.destroy);
}

// Brings the playing Anvil to the chosen mode and Destruction, like
// updateFurnace(): in place when silent, else a crossfade to a fresh voice
// that starts the held key (or stays silent under a release tail).
void Engine::updateAnvil() noexcept
{
    if (bassFade > 0 || bassSetup[bassSlot] == anvilTarget) return;
    if (bassSuspended || !bassVoices[bassSlot].sounding())
    {
        configureAnvil(bassSlot);
        return;
    }
    bassSlot = 1 - bassSlot;
    configureAnvil(bassSlot);
    if (bassEnabled && router.bassKey().held()) startBass(bassVoices[bassSlot]);
    bassFade = modeFadeSamples;
}

void Engine::finishFurnaceFade() noexcept
{
    furnaceFade = 0;
    furnaces[1 - furnaceSlot].clear();
    updateFurnace();
}

void Engine::finishBassFade() noexcept
{
    bassFade = 0;
    bassVoices[1 - bassSlot].reset();
    updateAnvil();
}

void Engine::fadeWeights(int remaining, int total, float& incoming, float& outgoing) noexcept
{
    const auto position = 1.0 - static_cast<double>(remaining) / total;
    incoming = static_cast<float>(std::sin(1.5707963267948966 * position));
    outgoing = static_cast<float>(std::cos(1.5707963267948966 * position));
}

void Engine::setLayersEnabled(bool bass, bool rhythm) noexcept
{
    if (bass == bassEnabled && rhythm == rhythmEnabled) return;
    const auto bassWasEnabled = bassEnabled;
    const auto rhythmWasEnabled = rhythmEnabled;
    bassEnabled = bass;
    rhythmEnabled = rhythm;
    const auto change = router.setBassRule(bassRule());
    // Turning Anvil or Furnace on or off can solo Blade or end its solo.
    updateLead(router.setLeadRule(leadRule()));
    for (auto& furnace : furnaces) furnace.setAnvilPresent(bass, rhythmSuspended);
    if (bass != bassWasEnabled)
    {
        bassPower.setTarget(bass ? 1.0 : 0.0, fadeSamples);
        if (bass)
        {
            bassSuspended = false;
            bassKeyHeld = false; // re-entry is a fresh attack, never a glide
            updateBass(router.bassKey().held() ? NoteRouter::Change::attack : NoteRouter::Change::release);
        }
    }
    else if (bass) updateBass(change);
    if (rhythm != rhythmWasEnabled)
    {
        rhythmPower.setTarget(rhythm ? 1.0 : 0.0, fadeSamples);
        if (rhythm)
        {
            rhythmSuspended = false;
            // Preserve continuity during a rapid reversal; a fully faded layer
            // already had its history cleared at zero gain.
            for (auto& furnace : furnaces) furnace.releaseAll();
            startHeldRhythm(furnaces[furnaceSlot]);
            if (furnaceFade > 0) startHeldRhythm(furnaces[1 - furnaceSlot]);
        }
    }
}

void Engine::startHeldRhythm(FurnaceLayer& furnace) noexcept
{
    router.forEachNewestHeldKey(rhythmVoiceCount, [this, &furnace](int channel, int note, int velocity) noexcept
    {
        furnace.start(channel, note, velocity, bend[static_cast<std::size_t>(channel - 1)].value(), ++serial);
    });
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
    const auto changes = router.noteOn(channel, note, velocity);
    striking = true;
    updateBass(changes.bass);
    updateLead(changes.lead);
    if (rhythmEnabled) startRhythm(channel, note, velocity);
    striking = false;
}

void Engine::startRhythm(int channel, int note, int velocity) noexcept
{
    const auto ratio = bend[static_cast<std::size_t>(channel - 1)].value();
    const auto age = ++serial;
    const auto accent = strikeVelocity(velocity);
    furnaces[furnaceSlot].start(channel, note, velocity, ratio, age, activeGlide(), accent);
    // The outgoing mode plays along until its fade ends, so the crossfade
    // carries the same notes on both sides.
    if (furnaceFade > 0) furnaces[1 - furnaceSlot].start(channel, note, velocity, ratio, age, activeGlide(), accent);
}

void Engine::noteOff(int channel, int note) noexcept
{
    const auto changes = router.noteOff(channel, note);
    updateBass(changes.bass);
    updateLead(changes.lead);
    for (auto& furnace : furnaces) furnace.release(channel, note);
}

void Engine::allNotesOff(int channel) noexcept
{
    const auto changes = router.allNotesOff(channel);
    updateBass(changes.bass);
    updateLead(changes.lead);
    for (auto& furnace : furnaces) furnace.release(channel, -1);
}

void Engine::startBass(BassVoice& voice) noexcept
{
    const auto key = router.bassKey();
    voice.start(NoteRouter::bassFrequency(key.note), key.velocity);
    voice.strike(strikeVelocity(key.velocity));
}

void Engine::updateBass(NoteRouter::Change change) noexcept
{
    if (!bassEnabled) return;
    const auto key = router.bassKey();
    if (change == NoteRouter::Change::attack)
    {
        bassChannel = key.channel;
        bassBendOffset.reset(0.0);
        if (glideOn && bassKeyHeld)
        {
            // Glide: Anvil slides to its new key as one line, no new attack.
            for (std::size_t i = 0; i < (bassFade > 0 ? 2u : 1u); ++i)
            {
                auto& voice = bassVoices[i == 0 ? bassSlot : 1 - bassSlot];
                voice.legato(NoteRouter::bassFrequency(key.note), activeGlide());
                voice.retargetVelocity(key.velocity);
            }
        }
        else
        {
            startBass(bassVoices[bassSlot]);
            if (bassFade > 0) startBass(bassVoices[1 - bassSlot]);
        }
        bassKeyHeld = true;
    }
    else if (change == NoteRouter::Change::retarget)
    {
        // Same pitch, new owner channel: glide from the old channel's bend.
        const auto oldRatio = bend[static_cast<std::size_t>(bassChannel - 1)].value() + bassBendOffset.value();
        bassChannel = key.channel;
        bassBendOffset.reset(oldRatio - bend[static_cast<std::size_t>(bassChannel - 1)].value());
        bassBendOffset.setTarget(0.0, transitionSamples);
        for (auto& voice : bassVoices) voice.retargetVelocity(key.velocity);
    }
    else if (change == NoteRouter::Change::release)
    {
        for (auto& voice : bassVoices) voice.stop();
        bassKeyHeld = false;
    }
}

// Blade follows its key like Anvil: a hard attack (2 ms crossfade) on a new
// note, a bend-owner glide on a retarget, the release when no key is left.
void Engine::updateLead(NoteRouter::Change change) noexcept
{
    if (!leadEnabled) return;
    const auto key = router.leadKey();
    if (change == NoteRouter::Change::attack)
    {
        const auto hz = leadHz(key.note);
        leadChannel = key.channel;
        leadBendOffset.reset(0.0);
        const auto slides = portamento != Portamento::off;
        for (std::size_t i = 0; i < (leadFade > 0 ? 2u : 1u); ++i)
        {
            auto& voice = blades[i == 0 ? leadSlot : 1 - leadSlot];
            if (slides && leadKeyHeld)
            {
                // Overlapping notes: one continuous line, no new attack.
                voice.legato(hz, portamentoSamples);
                voice.retargetVelocity(key.velocity);
            }
            else
            {
                if (portamento == Portamento::always && lastLeadHz > 0.0)
                    voice.start(hz, key.velocity, portamentoSamples, lastLeadHz);
                else
                    voice.start(hz, key.velocity);
                voice.strike(strikeVelocity(key.velocity));
            }
        }
        leadKeyHeld = true;
        lastLeadHz = hz;
    }
    else if (change == NoteRouter::Change::retarget)
    {
        const auto oldRatio = bend[static_cast<std::size_t>(leadChannel - 1)].value() + leadBendOffset.value();
        leadChannel = key.channel;
        leadBendOffset.reset(oldRatio - bend[static_cast<std::size_t>(leadChannel - 1)].value());
        leadBendOffset.setTarget(0.0, transitionSamples);
        for (auto& voice : blades) voice.retargetVelocity(key.velocity);
    }
    else if (change == NoteRouter::Change::release)
    {
        for (auto& voice : blades) voice.stop();
        leadKeyHeld = false;
    }
}

void Engine::setLeadPortamento(Portamento mode, float milliseconds) noexcept
{
    portamento = mode;
    portamentoMs = std::isfinite(milliseconds) ? std::clamp(milliseconds, 0.0f, 1000.0f) : 60.0f;
    portamentoSamples = static_cast<int>(std::lround(rate * portamentoMs / 1000.0));
}

// Octave moves the held note at once (oscillators keep their phase: no
// click), still in tune with Furnace's top note an octave apart.
void Engine::setLeadOctave(int octave) noexcept
{
    octave = std::clamp(octave, -1, 1);
    if (octave == leadOctave) return;
    leadOctave = octave;
    const auto key = router.leadKey();
    if (!leadEnabled || !key.held() || !leadKeyHeld) return;
    lastLeadHz = leadHz(key.note);
    for (auto& voice : blades) voice.legato(lastLeadHz, 0);
}

void Engine::configureBlade(std::size_t slot) noexcept
{
    leadSetup[slot] = leadChoice;
    blades[slot].setMode(leadChoice.mode, leadChoice.destroy);
    blades[slot].reset();
}

// Like updateAnvil(): in place when silent, else a crossfade to a fresh voice
// that starts the held top key. A change during a fade waits for its end.
void Engine::updateBlade() noexcept
{
    if (leadFade > 0 || leadSetup[leadSlot] == leadChoice) return;
    if (leadSuspended || !blades[leadSlot].sounding())
    {
        configureBlade(leadSlot);
        return;
    }
    leadSlot = 1 - leadSlot;
    configureBlade(leadSlot);
    const auto key = router.leadKey();
    // The incoming mode takes over the held note at its current pitch (even
    // mid-slide) and finishes any slide toward the key.
    if (leadEnabled && key.held())
    {
        const auto from = blades[1 - leadSlot].currentPitch(), hz = leadHz(key.note);
        blades[leadSlot].start(hz, key.velocity, from > 1.0 && from != hz ? portamentoSamples : 0, from);
    }
    leadFade = modeFadeSamples;
}

void Engine::finishLeadFade() noexcept
{
    leadFade = 0;
    blades[1 - leadSlot].reset();
    updateBlade();
}

void Engine::setLeadEnabled(bool on) noexcept
{
    if (on == leadEnabled) return;
    leadEnabled = on;
    leadPower.setTarget(on ? 1.0 : 0.0, fadeSamples);
    // Blade joining or leaving can solo Anvil or end its solo.
    updateBass(router.setBassRule(bassRule()));
    const auto leadChange = router.setLeadRule(leadRule());
    if (on)
    {
        leadSuspended = false;
        leadKeyHeld = false; // re-entry is a fresh attack, never a slide
        updateLead(router.leadKey().held() ? NoteRouter::Change::attack : NoteRouter::Change::release);
    }
    else updateLead(leadChange);
}

void Engine::setLeadLevel(float db) noexcept
{
    db = std::isfinite(db) ? std::clamp(db, -60.0f, 6.0f) : 0.0f;
    const auto gain = db <= -60.0f ? 0.0 : std::pow(10.0, db / 20.0);
    if (gain != leadMixTarget) { leadMixTarget = gain; leadMixLevel.setTarget(gain, fadeSamples); }
}

void Engine::pitchBend(int channel, int wheel) noexcept
{
    if (channel >= 1 && channel <= 16)
    {
        wheels[static_cast<std::size_t>(channel - 1)] = std::clamp(wheel, 0, 16383);
        const auto position = std::clamp(wheel, 0, 16383) - 8192;
        const auto semitones = bendRange * position / (position < 0 ? 8192.0 : 8191.0);
        bend[static_cast<std::size_t>(channel - 1)].setTarget(std::exp2(semitones / 12.0), transitionSamples);
    }
}

void Engine::setBendRange(int semitones) noexcept
{
    constexpr std::array<int, 5> allowed{2, 5, 7, 12, 24};
    if (std::find(allowed.begin(), allowed.end(), semitones) == allowed.end()) semitones = 2;
    if (semitones == bendRange) return;
    bendRange = semitones;
    // A wheel held off centre follows the new range (smoothed, no jump).
    for (int channel = 1; channel <= 16; ++channel)
        if (wheels[static_cast<std::size_t>(channel - 1)] != 8192) pitchBend(channel, wheels[static_cast<std::size_t>(channel - 1)]);
}

void Engine::setGlide(bool on, float milliseconds) noexcept
{
    glideOn = on;
    glideMs = std::isfinite(milliseconds) ? std::clamp(milliseconds, 0.0f, 500.0f) : 80.0f;
    glideSamples = static_cast<int>(std::lround(rate * glideMs / 1000.0));
}

void Engine::aimWheel(int samples) noexcept
{
    const auto roar = wheelMode == WheelMode::roar ? static_cast<double>(wheelTarget) : 0.0;
    const auto vibrato = wheelMode == WheelMode::vibrato ? static_cast<double>(wheelTarget) : 0.0;
    if (roar != roarGoal) { roarGoal = roar; roarDepth.setTarget(roar, samples); }
    if (vibrato != vibratoGoal) { vibratoGoal = vibrato; vibratoDepth.setTarget(vibrato, samples); }
}

void Engine::setWheelMode(WheelMode mode) noexcept
{
    if (mode == wheelMode) return;
    wheelMode = mode;
    // One depth fades out while the other fades in: no jump, no retrigger.
    aimWheel(static_cast<int>(std::ceil(rate * 0.020)));
}

void Engine::setWheelAmount(float amount) noexcept
{
    amount = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
    if (amount == wheelSaved) return;
    wheelSaved = wheelTarget = amount;
    aimWheel(fadeSamples);
}

void Engine::modWheel(int value) noexcept
{
    wheelTarget = std::clamp(value, 0, 127) / 127.0f;
    aimWheel(fadeSamples);
}

void Engine::resetWheel() noexcept
{
    wheelTarget = wheelSaved;
    aimWheel(fadeSamples);
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
                      offset(taps.rhythmLeft, at), offset(taps.rhythmRight, at), offset(taps.lead, at)},
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
    // The wheel: Roar's depth for the layers, and Vibrato as a common pitch
    // ratio on every channel's bend (so Anvil keeps its octave). Its phase
    // runs from reset() on and never restarts with a note.
    const auto increment = vibratoHz / rate;
    for (std::size_t i = 0; i < samples; ++i)
    {
        roarBlock[i] = static_cast<float>(roarDepth.next());
        const auto depth = vibratoDepth.next();
        if (depth > 0.0)
        {
            const auto ratio = std::exp2(vibratoCents * depth * std::sin(6.283185307179586 * vibratoPhase) / 1200.0);
            for (auto& channel : bendRatio) channel[i] *= ratio;
        }
        vibratoPhase += increment;
        if (vibratoPhase >= 1.0) vibratoPhase -= 1.0;
    }
    for (std::size_t i = 0; i < samples; ++i)
    {
        rhythmGain[i] = rhythmPower.next() * rhythmMixLevel.next();
        bassGain[i] = bassPower.next() * bassMixLevel.next() * anvilBalance;
        leadGain[i] = leadPower.next() * leadMixLevel.next() * bladeBalance;
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
    furnaces[furnaceSlot].render(static_cast<int>(rhythmCount), bendRatio, rhythmLeft, rhythmRight, roarBlock);
    if (furnaceFade > 0)
    {
        furnaces[1 - furnaceSlot].render(static_cast<int>(rhythmCount), bendRatio, outgoingLeft, outgoingRight, roarBlock);
        for (std::size_t i = 0; i < rhythmCount && furnaceFade > 0; ++i)
        {
            float in = 1.0f, out = 0.0f;
            fadeWeights(furnaceFade--, modeFadeSamples, in, out);
            rhythmLeft[i] = in * rhythmLeft[i] + out * outgoingLeft[i];
            rhythmRight[i] = in * rhythmRight[i] + out * outgoingRight[i];
        }
        if (furnaceFade == 0) finishFurnaceFade();
    }
    // Mono: the right output crossfades to the left, a complete finished
    // wall on its own. Summing the two sides instead would phase: their
    // stacks are detuned differently on purpose. One side carries the same
    // power as the stereo pair, so no make-up is needed. Skipped while fully
    // off so the stereo wall stays bit-identical.
    if (monoOn || monoBlend.value() > 0.0)
        for (std::size_t i = 0; i < samples; ++i)
        {
            const auto blend = monoBlend.next();
            if (i < rhythmCount) rhythmRight[i] += blend * (rhythmLeft[i] - rhythmRight[i]);
        }
    if (!rhythmSuspended && !rhythmEnabled && rhythmCount > 0 && rhythmGain[rhythmCount - 1] == 0.0)
    {
        clearRhythm(); rhythmSuspended = true;
    }
    const auto bassCount = audibleUntil(bassSuspended, bassEnabled, bassGain);
    const auto& bassBend = bendRatio[static_cast<std::size_t>(bassChannel - 1)];
    std::array<float, controlBlock> bass{};
    for (std::size_t i = 0; i < bassCount; ++i)
    {
        const auto ratio = bassBend[i] + bassBendOffset.next();
        for (auto& voice : bassVoices) voice.setRoar(roarBlock[i]);
        auto value = bassVoices[bassSlot].next(ratio, component).output;
        if (bassFade > 0)
        {
            float in = 1.0f, out = 0.0f;
            fadeWeights(bassFade--, modeFadeSamples, in, out);
            value = in * value + out * bassVoices[1 - bassSlot].next(ratio, component).output;
            if (bassFade == 0) finishBassFade();
        }
        bass[i] = static_cast<float>(bassGain[i] * value);
    }
    if (!bassSuspended && !bassEnabled && bassCount > 0 && bassGain[bassCount - 1] == 0.0)
    {
        for (auto& voice : bassVoices) voice.reset();
        if (bassFade > 0) finishBassFade();
        bassBendOffset.reset(0.0); bassSuspended = true;
    }

    const auto leadCount = audibleUntil(leadSuspended, leadEnabled, leadGain);
    const auto& leadBend = bendRatio[static_cast<std::size_t>(leadChannel - 1)];
    std::array<float, controlBlock> lead{};
    for (std::size_t i = 0; i < leadCount; ++i)
    {
        const auto ratio = leadBend[i] + leadBendOffset.next();
        for (auto& voice : blades) voice.setRoar(roarBlock[i]);
        auto value = blades[leadSlot].next(ratio);
        if (leadFade > 0)
        {
            float in = 1.0f, out = 0.0f;
            fadeWeights(leadFade--, modeFadeSamples, in, out);
            value = in * value + out * blades[1 - leadSlot].next(ratio);
            if (leadFade == 0) finishLeadFade();
        }
        lead[i] = static_cast<float>(leadGain[i] * value);
    }
    if (!leadSuspended && !leadEnabled && leadCount > 0 && leadGain[leadCount - 1] == 0.0)
    {
        for (auto& voice : blades) voice.reset();
        if (leadFade > 0) finishLeadFade();
        leadBendOffset.reset(0.0); leadSuspended = true;
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
        if (taps.lead) taps.lead[i] = lead[i];
        // Blade is added last and only when present, so an Anvil + Furnace
        // mix stays bit-identical to the M1 engine.
        left[i] = ((mask & 1u) != 0 ? bass[i] : 0.0f) + ((mask & 2u) != 0 ? furnaceLeft : 0.0f);
        right[i] = ((mask & 1u) != 0 ? bass[i] : 0.0f) + ((mask & 2u) != 0 ? furnaceRight : 0.0f);
        if ((mask & 4u) != 0 && i < leadCount) { left[i] += lead[i]; right[i] += lead[i]; }
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
}
