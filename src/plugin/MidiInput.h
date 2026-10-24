#pragma once

#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>
#include "InstrumentCore.h"

namespace dropforge
{
template <typename Core>
inline void processMidiBlock(Core& engine, juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) noexcept
{
    const auto samples = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2)
    {
        buffer.clear();
        return;
    }
    if (samples == 0)
    {
        for (const auto metadata : midi)
            dispatchMidi(engine, metadata.data, metadata.numBytes);
        return;
    }
    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(1);
    int cursor = 0;
    for (const auto metadata : midi)
    {
        const auto position = std::clamp(metadata.samplePosition, cursor, samples);
        engine.process(left + cursor, right + cursor, position - cursor);
        // Read raw bytes: MidiMessage construction can allocate for large SysEx.
        dispatchMidi(engine, metadata.data, metadata.numBytes);
        cursor = position;
    }
    engine.process(left + cursor, right + cursor, samples - cursor);
}
}
