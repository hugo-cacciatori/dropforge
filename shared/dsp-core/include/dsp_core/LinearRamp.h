#pragma once

#include <algorithm>

namespace dsp_core
{
class LinearRamp
{
public:
    void reset(double value) noexcept { current = target = value; remaining = 0; step = 0.0; }
    void setTarget(double value, int samples) noexcept
    {
        target = value;
        remaining = std::max(1, samples);
        step = (target - current) / remaining;
    }
    double next() noexcept
    {
        const auto result = current;
        if (remaining > 0 && --remaining == 0)
            current = target; // Exact endpoint avoids indefinitely decaying denormals.
        else if (remaining > 0)
            current += step;
        return result;
    }
    bool moving() const noexcept { return remaining > 0; }
    double value() const noexcept { return current; }

private:
    double current = 0.0, target = 0.0, step = 0.0;
    int remaining = 0;
};
}
