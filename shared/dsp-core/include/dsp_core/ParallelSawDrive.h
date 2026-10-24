#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Run at >=4x. Preserve at least 55% of the source ramp even at full drive;
// symmetric hard flattening would turn a saw towards a square. A small curved
// bias adds even partials; continuous folding enters only near full drive.
inline double parallelSawDrive(double input, double amount) noexcept
{
    if (!std::isfinite(input)) return 0.0;
    amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
    if (amount == 0.0) return input;
    // Numerical containment well outside the exciter's musical range.
    input = std::clamp(input, -8.0, 8.0);
    const auto shaped = std::tanh((1.0 + 2.0 * amount) * (input + 0.25 * amount * input * input));
    const auto fold = std::max(0.0, (amount - 0.65) / 0.35);
    const auto texture = (1.0 - 0.25 * fold) * shaped + 0.25 * fold * std::sin(4.0 * input);
    return (1.0 - 0.45 * amount) * input + 0.45 * amount * texture;
}
}
