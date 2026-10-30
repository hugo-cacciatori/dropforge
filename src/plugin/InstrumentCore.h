#pragma once

#include "MidiDispatch.h"
#include "dsp/FxChain.h"
#include "dsp/Master.h"
#include "dsp/OutputGain.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace dropforge
{
// The plugin and offline renderer share MIDI, voice state and master smoothing.
// No host, JUCE, file I/O or GUI types enter this real-time path.
class InstrumentCore
{
public:
    // Voices and master together: what the host is told and compensates.
    static constexpr int latencySamples = Engine::latencySamples + Master::latencySamples;
    // Meter taps for the editor: sample peaks after Output and the deepest
    // master gain reductions since the last takeMeters().
    struct Meters { float peakLeft = 0.0f, peakRight = 0.0f, glueReductionDb = 0.0f, limiterReductionDb = 0.0f; };

    void prepare(double sampleRate, float outputDb = 0.0f, float splitHz = 140.0f, float drive = 0.5f) noexcept
    {
        engine.prepare(sampleRate);
        engine.setBassParameters(splitHz, drive);
        engine.setRhythmParameters(drive);
        engine.reset();
        fx.prepare(sampleRate);
        master.prepare(sampleRate);
        output.prepare(sampleRate, outputDb);
        meters = {};
    }
    void reset(float outputDb, float splitHz = 140.0f, float drive = 0.5f) noexcept
    {
        engine.setBassParameters(splitHz, drive);
        engine.setRhythmParameters(drive);
        engine.reset();
        fx.reset();
        master.reset();
        output.reset(outputDb);
        meters = {};
    }
    void setOutputDb(float db) noexcept { output.setTarget(db); }
    // Modes 0-3 = A-D for Anvil and Furnace (SPEC section 12); real-time
    // safe, crossfading while notes sound.
    void setModes(int anvil, int furnace) noexcept
    {
        engine.setAnvilMode(anvil);
        engine.setFurnaceMode(furnace);
    }
    // Destruction per instrument (SPEC section 12); crossfades like a mode.
    void setDestruction(bool anvil, bool furnace) noexcept
    {
        engine.setAnvilDestruction(anvil);
        engine.setFurnaceDestruction(furnace);
    }
    // Furnace Mono and Drop (SPEC section 3); real-time safe, click-free.
    void setFurnaceOptions(bool mono, bool drop) noexcept
    {
        engine.setFurnaceMono(mono);
        engine.setFurnaceDrop(drop);
        if (mono != furnaceMono) { furnaceMono = mono; applyFx(); }
    }
    void setLayerLevels(float bassDb, float rhythmDb) noexcept { engine.setLayerLevels(bassDb, rhythmDb); }
    // Enables choose playing policy; output masks below only isolate that policy.
    void setLayersEnabled(bool bass, bool rhythm) noexcept
    {
        engine.setLayersEnabled(bass, rhythm);
        powered[0] = bass; powered[1] = rhythm;
        clearTailsWhenAllOff();
        applyFx();
    }
    // Blade (SPEC section 3): power and Level.
    void setLead(bool on, float levelDb) noexcept
    {
        engine.setLeadEnabled(on);
        engine.setLeadLevel(levelDb);
        powered[2] = on;
        clearTailsWhenAllOff();
        applyFx();
    }
    // The FX page (SPEC section 8) at the host's tempo (BPM; anything
    // outside 20-300, such as no host tempo, plays at 120).
    void setFx(const FxChain::Settings& settings, double bpm) noexcept
    {
        fxChosen = settings;
        fxBpm = bpm;
        applyFx();
    }
    // Blade's mode (0-3 = A-D) and Destruction; real-time safe, crossfading.
    void setLeadMode(int mode, bool destroy) noexcept { engine.setLeadMode(mode); engine.setLeadDestruction(destroy); }
    // Performance (SPEC section 3): Bend Range in semitones; Glide on/off and
    // time; Impact on/off.
    void setPerformance(int bendRange, bool glide, float glideMs, bool impact) noexcept
    {
        engine.setImpact(impact);
        engine.setBendRange(bendRange);
        engine.setGlide(glide, glideMs);
    }
    // The mod wheel (SPEC section 3): mode 0 Roar, 1 Vibrato; the saved
    // Amount, taken when it changes (CC1 moves the wheel in between).
    void setWheel(int mode, float amount) noexcept
    {
        engine.setWheelMode(mode == 1 ? Engine::WheelMode::vibrato : Engine::WheelMode::roar);
        engine.setWheelAmount(amount);
    }
    // Where the wheel is now (0-1): the saved Amount or the latest CC1.
    float wheelPosition() const noexcept { return engine.wheelPosition(); }
    // Blade's Portamento (0 Off, 1 Legato, 2 Always) and time, and Octave.
    void setLeadPlaying(int portamento, float milliseconds, int octave) noexcept
    {
        engine.setLeadPortamento(static_cast<Engine::Portamento>(std::clamp(portamento, 0, 2)), milliseconds);
        engine.setLeadOctave(octave);
    }
    // Diagnostic voicing for renders and tests. The plugin keeps the fixed
    // 140 Hz split and 0.5 drive the owner chose; it never calls this.
    void setBassParameters(float splitHz, float drive) noexcept
    {
        engine.setBassParameters(splitHz, drive);
        engine.setRhythmParameters(drive);
    }
    // Diagnostic Roar depth for listening rounds; the plugin never calls it.
    void setRoarScale(float scale) noexcept { engine.setRoarScale(scale); }
    void panic() noexcept
    {
        for (int channel = 1; channel <= 16; ++channel)
            engine.allNotesOff(channel);
        fx.reset(); // Panic clears the echoes too.
    }
    void midi(const std::uint8_t* bytes, int size) noexcept { dispatchMidi(engine, bytes, size); }
    void process(float* left, float* right, int samples,
                 LayerMask outputMask = LayerMask::all, LayerTaps taps = {},
                 BassComponent component = BassComponent::all) noexcept
    {
        if (!fx.active()) engine.process(left, right, samples, outputMask, taps, component);
        else
        {
            // The FX sends need each instrument's signal: render in short
            // runs through scratch taps (the engine is block-size invariant).
            for (int at = 0; at < samples; at += fxRun)
            {
                const auto count = std::min(fxRun, samples - at);
                LayerTaps scratch{tapBassLeft.data(), tapBassRight.data(), tapRhythmLeft.data(), tapRhythmRight.data(), tapLead.data()};
                engine.process(left + at, right + at, count, outputMask, scratch, component);
                const auto forward = [count](const std::array<float, fxRun>& from, float* to, int offset) noexcept
                {
                    if (to != nullptr) std::copy_n(from.begin(), count, to + offset);
                };
                forward(tapBassLeft, taps.bassLeft, at); forward(tapBassRight, taps.bassRight, at);
                forward(tapRhythmLeft, taps.rhythmLeft, at); forward(tapRhythmRight, taps.rhythmRight, at);
                forward(tapLead, taps.lead, at);
                fx.process(left + at, right + at, scratch, outputMask, count);
            }
        }
        master.process(left, right, samples);
        float* channels[]{left, right};
        output.process(channels, 2, samples);
        meters.glueReductionDb = std::min(meters.glueReductionDb, master.lastMeters().glueReductionDb);
        meters.limiterReductionDb = std::min(meters.limiterReductionDb, master.lastMeters().limiterReductionDb);
        for (int i = 0; i < samples; ++i)
        {
            meters.peakLeft = std::max(meters.peakLeft, std::abs(left[i]));
            meters.peakRight = std::max(meters.peakRight, std::abs(right[i]));
        }
    }
    Meters takeMeters() noexcept { return std::exchange(meters, Meters{}); }
    // The routing keyboard's view (SPEC section 6): one bit per held MIDI
    // note, the key Anvil plays (-1 while it plays none) and Blade's top key.
    struct Keys { std::uint64_t low = 0, high = 0; int bassNote = -1, leadNote = -1; };
    Keys heldKeys() const noexcept
    {
        Keys keys;
        engine.forEachHeldKey([&keys](int, int note, int)
        {
            (note < 64 ? keys.low : keys.high) |= std::uint64_t{1} << (note & 63);
        });
        keys.bassNote = engine.bassTarget().note;
        keys.leadNote = engine.leadTarget().note;
        return keys;
    }

private:
    // The FX as chosen, with the owner's routing rules applied for the
    // instruments on now: Anvil's send closed in a mix, ping-pong only on a
    // mono source (the stored choice returns when allowed again).
    void applyFx() noexcept
    {
        auto settings = fxChosen;
        if (!FxChain::anvilSendOpen(powered[0], powered[1], powered[2])) settings.sendAnvil = 0.0f;
        if (!FxChain::pingPongAllowed(powered[0], powered[1], powered[2], furnaceMono)) settings.delayPingPong = false;
        fx.set(settings, fxBpm);
        master.setSaturate(settings.saturateOn, settings.saturateDriveDb, settings.saturateMix);
    }
    // Every instrument switched off clears the echoes (SPEC section 8).
    void clearTailsWhenAllOff() noexcept
    {
        const auto allOff = !powered[0] && !powered[1] && !powered[2];
        if (allOff && !wasAllOff) fx.reset();
        wasAllOff = allOff;
    }
    Engine engine;
    FxChain fx;
    static constexpr int fxRun = 256;
    std::array<float, fxRun> tapBassLeft{}, tapBassRight{}, tapRhythmLeft{}, tapRhythmRight{}, tapLead{};
    std::array<bool, 3> powered{true, true, false};
    bool wasAllOff = false, furnaceMono = false;
    FxChain::Settings fxChosen{};
    double fxBpm = 120.0;
    Master master;
    OutputGain output;
    Meters meters;
};

inline void dispatchMidi(InstrumentCore& core, const std::uint8_t* bytes, int size) noexcept
{
    core.midi(bytes, size);
}
}
