#pragma once

#include <dsp_core/MultibandDynamics.h>
#include "RhythmOscillators.h"

namespace dropforge
{
// Everything that makes one Furnace candidate sound the way it does (SPEC
// section 4). Round 1 compares three on purpose clearly different designs;
// the owner's ears pick, then tune. Chosen before playing, not automated.
struct FurnaceDesign
{
    RhythmOscillators::Design voice{};
    float drivePushDb = 0.0f;   // extra level into the drive curve: harder, denser
    double contourDb = 3.0;     // the accepted 2.8 kHz pre-drive push
    double airDb = 3.0;         // 10 kHz shelf after the dynamics
    dsp_core::MultibandDynamics::Settings dynamics{};

    // A: the balanced wall. Starting dynamics come from the measured post-drive
    // band peaks of typical voicings (about -14 dB low, -9 dB mid, -13 dB
    // high on an E power chord): the low band held steady (a gentle lift of at
    // most 6 dB for high notes, never booming under Anvil), the mid core
    // compressed about 3 dB with 2 dB make-up, the top lifted about 3 dB.
    static FurnaceDesign candidateA() noexcept
    {
        FurnaceDesign design;
        //                   down dB, ratio, up dB, ratio, max lift, output dB
        design.dynamics.band = {{{-12.0f, 4.0f, -22.0f, 2.0f, 6.0f, 0.0f},
                                 {-14.0f, 3.0f, -20.0f, 3.0f, 12.0f, 2.0f},
                                 {-4.0f, 4.0f, -8.0f, 3.0f, 9.0f, 0.0f}}};
        return design;
    }
    // B: the dubstep side. Wider, more aggressive: 45-cent detune, a quieter
    // octave, a louder square body for bite, 4 dB more drive, a stronger
    // upper-mid push and air, the core squeezed harder and the top lifted more.
    static FurnaceDesign candidateB() noexcept
    {
        auto design = candidateA();
        design.voice = {45.0, -10.0, 12.0, -9.0};
        design.drivePushDb = 4.0f;
        design.contourDb = 5.0;
        design.airDb = 4.5;
        design.dynamics.band[1] = {-16.0f, 4.0f, -20.0f, 3.0f, 12.0f, 3.0f};
        design.dynamics.band[2] = {-2.0f, 4.0f, -5.0f, 3.0f, 12.0f, 0.0f};
        return design;
    }
    // C: the future-bass side. Smoother and more shimmering: 25-cent detune,
    // a louder, wider octave stack (the chorus-like shimmer), a quiet body,
    // 3 dB less drive, a softer contour and gentler compression in the core.
    static FurnaceDesign candidateC() noexcept
    {
        auto design = candidateA();
        design.voice = {25.0, -4.0, 20.0, -18.0};
        design.drivePushDb = -3.0f;
        design.contourDb = 1.5;
        design.airDb = 4.0;
        design.dynamics.band[1] = {-12.0f, 2.0f, -20.0f, 2.0f, 9.0f, 1.0f};
        design.dynamics.band[2] = {-4.0f, 3.0f, -10.0f, 2.0f, 9.0f, 0.0f};
        return design;
    }
    // 'a', 'b' or 'c' (either case); anything else is candidate A.
    static FurnaceDesign candidate(char letter) noexcept
    {
        switch (letter)
        {
            case 'b': case 'B': return candidateB();
            case 'c': case 'C': return candidateC();
            default: return candidateA();
        }
    }
};
}
