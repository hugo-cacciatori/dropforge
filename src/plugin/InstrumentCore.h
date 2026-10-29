#pragma once

#include "MidiDispatch.h"
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

    void prepare(double sampleRate, float outputDb = 0.0f, float splitHz = 140.0f, float destruction = 0.5f) noexcept
    {
        engine.prepare(sampleRate);
        engine.setBassParameters(splitHz, destruction);
        engine.setRhythmParameters(destruction);
        engine.reset();
        master.prepare(sampleRate);
        output.prepare(sampleRate, outputDb);
        meters = {};
    }
    void reset(float outputDb, float splitHz = 140.0f, float destruction = 0.5f) noexcept
    {
        engine.setBassParameters(splitHz, destruction);
        engine.setRhythmParameters(destruction);
        engine.reset();
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
    // Furnace Mono and Drop (SPEC section 3); real-time safe, click-free.
    void setFurnaceOptions(bool mono, bool drop) noexcept
    {
        engine.setFurnaceMono(mono);
        engine.setFurnaceDrop(drop);
    }
    void setLayerLevels(float bassDb, float rhythmDb) noexcept { engine.setLayerLevels(bassDb, rhythmDb); }
    // Enables choose playing policy; output masks below only isolate that policy.
    void setLayersEnabled(bool bass, bool rhythm) noexcept { engine.setLayersEnabled(bass, rhythm); }
    // Diagnostic voicing for renders and tests. The plugin keeps the fixed
    // 140 Hz split and 0.5 drive the owner chose; it never calls this.
    void setBassParameters(float splitHz, float destruction) noexcept
    {
        engine.setBassParameters(splitHz, destruction);
        engine.setRhythmParameters(destruction);
    }
    void panic() noexcept
    {
        for (int channel = 1; channel <= 16; ++channel)
            engine.allNotesOff(channel);
    }
    void midi(const std::uint8_t* bytes, int size) noexcept { dispatchMidi(engine, bytes, size); }
    void process(float* left, float* right, int samples,
                 LayerMask outputMask = LayerMask::both, LayerTaps taps = {},
                 BassComponent component = BassComponent::all) noexcept
    {
        engine.process(left, right, samples, outputMask, taps, component);
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

private:
    Engine engine;
    Master master;
    OutputGain output;
    Meters meters;
};

inline void dispatchMidi(InstrumentCore& core, const std::uint8_t* bytes, int size) noexcept
{
    core.midi(bytes, size);
}
}
