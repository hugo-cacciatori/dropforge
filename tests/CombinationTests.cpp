#include "plugin/InstrumentCore.h"
#include "support/StereoMetrics.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

// The gates for every mode combination (SPEC section 12): each of the 64
// Anvil x Furnace x Blade sets, clean and with every Destruction switch on,
// must keep separation, Anvil's exact mono, the -1 dBFS ceiling, finite
// output, identical renders at blocks 1, 73 and 512, and repeat exactly after
// reset. (Aliasing is gated per mode in bass_audio, rhythm_mix and
// blade_voice, CPU by the benchmark: neither depends on the other modes.)
namespace
{
using dropforge::InstrumentCore;
using dropforge::LayerMask;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

constexpr int rate = 48000, frames = 36000, changeAt = 18000;
struct Render { std::vector<float> left, right, bassLeft, bassRight, rhythmLeft, rhythmRight; };

void send(InstrumentCore& core, std::uint8_t status, int note, int value)
{
    const std::uint8_t bytes[]{status, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(value)};
    core.midi(bytes, 3);
}

// An E2 power chord with its octave and fifth, then a change to B1 halfway:
// Anvil takes the lowest key, so both chords put real energy under 100 Hz.
Render render(InstrumentCore& core, int length, int block)
{
    Render out;
    for (auto* channel : {&out.left, &out.right, &out.bassLeft, &out.bassRight, &out.rhythmLeft, &out.rhythmRight})
        channel->assign(static_cast<std::size_t>(length), 0.0f);
    for (const auto note : {40, 47, 52, 55}) send(core, 0x90, note, 112);
    for (int at = 0; at < length;)
    {
        if (at == changeAt)
        {
            for (const auto note : {40, 47, 52, 55}) send(core, 0x80, note, 0);
            for (const auto note : {35, 42, 47, 50}) send(core, 0x90, note, 112);
        }
        const auto count = std::min({block, length - at, at < changeAt ? changeAt - at : length});
        const auto i = static_cast<std::size_t>(at);
        core.process(out.left.data() + i, out.right.data() + i, count, LayerMask::all,
                     {out.bassLeft.data() + i, out.bassRight.data() + i, out.rhythmLeft.data() + i, out.rhythmRight.data() + i});
        at += count;
    }
    return out;
}

void configure(InstrumentCore& core, int anvil, int furnace, int blade, bool destroy)
{
    core.setModes(anvil, furnace);
    core.setDestruction(destroy, destroy);
    core.setLead(true, 0.0f); // all three layers: Blade joins the ceiling and determinism checks
    core.setLeadMode(blade, destroy);
    core.prepare(rate);
}

bool samePrefix(const Render& a, const Render& b, std::size_t length)
{
    return std::equal(a.left.begin(), a.left.begin() + length, b.left.begin())
        && std::equal(a.right.begin(), a.right.begin() + length, b.right.begin());
}
}

int main()
{
    double worstSeparation = -300.0;
    float worstPeak = 0.0f;
    for (const bool destroy : {false, true})
        for (int anvil = 0; anvil < 4; ++anvil)
            for (int furnace = 0; furnace < 4; ++furnace)
            for (int blade = 0; blade < 4; ++blade)
            {
                InstrumentCore core;
                configure(core, anvil, furnace, blade, destroy);
                const auto reference = render(core, frames, 512);
                require(reference.bassLeft == reference.bassRight, "Anvil is exactly mono in every combination");
                for (std::size_t i = 0; i < reference.left.size(); ++i)
                {
                    require(std::isfinite(reference.left[i]) && std::isfinite(reference.right[i]), "every combination is finite");
                    worstPeak = std::max({worstPeak, std::abs(reference.left[i]), std::abs(reference.right[i])});
                }
                // SPEC section 4 separation: below 100 Hz Furnace stays 20 dB under Anvil.
                const auto m = dsp_core::test::measure(reference.rhythmLeft, reference.rhythmRight, reference.bassLeft, rate, 100.0);
                std::cout << "anvil=" << static_cast<char>('A' + anvil) << " furnace=" << static_cast<char>('A' + furnace) << " blade=" << static_cast<char>('A' + blade)
                          << " destroy=" << destroy << " separation_db=" << m.separationDb << " low_bass_dbfs=" << m.lowBassDbfs << '\n';
                require(m.lowBassDbfs > -60.0, "Anvil carries the lows in every combination");
                require(m.separationDb <= -20.0, "Furnace stays 20 dB under Anvil below 100 Hz in every combination");
                worstSeparation = std::max(worstSeparation, m.separationDb);

                // Block-size invariance across the chord change, and exact repeat after reset.
                for (const int block : {1, 73})
                {
                    InstrumentCore other;
                    configure(other, anvil, furnace, blade, destroy);
                    require(samePrefix(reference, render(other, changeAt + 6000, block), static_cast<std::size_t>(changeAt + 6000)),
                            "every combination renders identically at blocks 1, 73 and 512");
                }
                core.panic();
                core.reset(0.0f);
                require(samePrefix(reference, render(core, frames, 512), static_cast<std::size_t>(frames)),
                        "every combination repeats exactly after reset");
            }
    std::cout << "combinations=128 (64 clean, 64 destroyed) worst_separation_db=" << worstSeparation
              << " worst_peak_dbfs=" << 20.0 * std::log10(worstPeak) << '\n';
    require(worstPeak <= dropforge::Master::ceiling, "every combination stays under -1 dBFS");
    return 0;
}
