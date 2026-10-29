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
    // Output trim so every mode lands within 1 LU of A (SPEC section 12).
    float levelGain = 1.0f;
    // Final side blend; the accepted 0.84 unless a mode needs more width.
    double sideGain = RhythmOscillators::spread;

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
        // The louder octave shimmer reads about 1 LU over A; trim -0.8 dB.
        design.levelGain = 0.91201083935590974f;
        return design;
    }
    // D Alloy: the FM wall (SPEC section 12). Glassy and metallic but
    // harmonic: sine carriers modulated 1:1 and 2:1, detuned per side like
    // the saws but a little narrower (FM detune beats harder), a saw octave for
    // air, no square body (the FM core already has the mids), a touch more
    // contour and a little less air than A, A's dynamics.
    static FurnaceDesign candidateD() noexcept
    {
        auto design = candidateA();
        design.voice = {30.0, -8.0, 12.0, -60.0, 1.8, 0.7};
        design.contourDb = 4.0;
        design.airDb = 2.0;
        // Sine-based stacks carry less upper energy than saws: +1.8 dB to
        // land within 1 LU of A (+1.2 dB with the wider side blend below).
        design.levelGain = 1.1481536214968828f;
        // A sine-based tone keeps more energy low, where the image is mono:
        // a stronger side blend restores A's width on low riffs.
        design.sideGain = 0.94;
        return design;
    }
    // 'a' to 'd' (either case); anything else is candidate A.
    static FurnaceDesign candidate(char letter) noexcept
    {
        switch (letter)
        {
            case 'b': case 'B': return candidateB();
            case 'c': case 'C': return candidateC();
            case 'd': case 'D': return candidateD();
            default: return candidateA();
        }
    }
};
}
