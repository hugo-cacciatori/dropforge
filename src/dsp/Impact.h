#pragma once

#include <algorithm>
#include <cmath>

namespace dropforge
{
// Impact (SPEC section 3): a short accent on each struck key, scaled by its
// velocity. It pushes the layer's drive harder (more harmonics, a brighter
// bite) and lifts the level a few dB, then decays in a few tens of
// milliseconds, so a hard hit cracks and a soft one barely changes. It rides
// on top of the fixed envelope and never changes its timings.
class ImpactAccent
{
public:
    // Full velocity: the drive input up to +6 dB, the level up to +4 dB.
    static constexpr float driveBoost = 1.0f;   // input x (1 + 1.0 a)
    static constexpr float punchBoost = 0.585f; // level x (1 + 0.585 a), +4 dB
    // A 10 ms time constant: about -26 dB of the accent left at 30 ms.
    static constexpr double decaySeconds = 0.010;

    void prepare(double rate) noexcept
    {
        decay = static_cast<float>(std::exp(-1.0 / (rate * decaySeconds)));
        reset();
    }
    void reset() noexcept { value = 0.0f; }
    // A new strike restarts the accent; a quieter one never cuts a louder
    // accent still ringing.
    void strike(int velocity) noexcept { value = std::max(value, std::clamp(velocity, 0, 127) / 127.0f); }
    bool active() const noexcept { return value > 0.0f; }
    // The accent for this sample, 0-1; exactly 0 once it has died away, so
    // the layers skip it and sound bit-identical to Impact off.
    float next() noexcept
    {
        const auto current = value;
        value *= decay;
        if (value < 1.0e-3f) value = 0.0f;
        return current;
    }

private:
    float value = 0.0f, decay = 0.0f;
};
}
