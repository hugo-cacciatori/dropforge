#pragma once

#include "LinearRamp.h"

namespace dsp_core
{
// Linear attack, indefinitely held sustain, finite release. No decay stage.
class FixedEnvelope
{
public:
    void prepare(int attackSamples, int releaseSamples) noexcept
    {
        attack = attackSamples;
        release = releaseSamples;
        reset();
    }
    void reset() noexcept { level.reset(0.0); }
    void start() noexcept { level.reset(0.0); level.setTarget(1.0, attack); }
    void stop() noexcept { level.setTarget(0.0, release); }
    // Back to full from the current level (a releasing voice taken over by a
    // legato slide): no restart from zero, so no new attack.
    void resume() noexcept { level.setTarget(1.0, attack); }
    double next() noexcept { return level.next(); }
    bool active() const noexcept { return level.moving() || level.value() != 0.0; }

private:
    LinearRamp level;
    int attack = 1, release = 1;
};
}
