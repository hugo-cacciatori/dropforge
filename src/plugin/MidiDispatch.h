#pragma once

#include "dsp/Engine.h"
#include <cstdint>

namespace dropforge
{
inline void dispatchMidi(Engine& engine, const std::uint8_t* bytes, int size) noexcept
{
    if (size < 1)
        return;
    if (bytes[0] == 0xff) // System reset releases every channel without a waveform jump.
    {
        for (int channel = 1; channel <= 16; ++channel)
        {
            engine.allNotesOff(channel);
            engine.pitchBend(channel, 8192);
        }
        engine.resetWheel();
        return;
    }
    if (size < 3 || bytes[1] > 127 || bytes[2] > 127)
        return;
    const auto channel = (bytes[0] & 0x0f) + 1;
    switch (bytes[0] & 0xf0)
    {
        case 0x80: engine.noteOff(channel, bytes[1]); break;
        case 0x90: engine.noteOn(channel, bytes[1], bytes[2]); break;
        case 0xe0: engine.pitchBend(channel, bytes[1] | (bytes[2] << 7)); break;
        case 0xb0:
            if (bytes[1] == 1)
                engine.modWheel(bytes[2]); // from any channel: one global wheel
            else if (bytes[1] == 120 || bytes[1] == 123)
                engine.allNotesOff(channel);
            else if (bytes[1] == 121)
            {
                engine.pitchBend(channel, 8192);
                engine.resetWheel();
            }
            break; // CC64 is deliberately ignored, including after a note-off.
        default: break;
    }
}
}
