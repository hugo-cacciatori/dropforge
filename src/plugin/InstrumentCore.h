#pragma once

#include "MidiDispatch.h"
#include "dsp/OutputGain.h"

namespace dropforge
{
// The plugin and offline renderer share MIDI, voice state and master smoothing.
// No host, JUCE, file I/O or GUI types enter this real-time path.
class InstrumentCore
{
public:
    void prepare(double sampleRate, float outputDb = 0.0f, float splitHz = 140.0f, float destruction = 0.5f) noexcept
    {
        engine.prepare(sampleRate);
        engine.setBassParameters(splitHz, destruction);
        engine.setRhythmParameters(destruction);
        engine.reset();
        output.prepare(sampleRate, outputDb);
    }
    void reset(float outputDb, float splitHz = 140.0f, float destruction = 0.5f) noexcept
    {
        engine.setBassParameters(splitHz, destruction);
        engine.setRhythmParameters(destruction);
        engine.reset();
        output.reset(outputDb);
    }
    void setOutputDb(float db) noexcept { output.setTarget(db); }
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
        float* channels[]{left, right};
        output.process(channels, 2, samples);
    }

private:
    Engine engine;
    OutputGain output;
};

inline void dispatchMidi(InstrumentCore& core, const std::uint8_t* bytes, int size) noexcept
{
    core.midi(bytes, size);
}
}
