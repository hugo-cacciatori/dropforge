#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Run at >=4x. Preserve at least 55% of the source ramp even at full drive;
// symmetric hard flattening would turn a saw towards a square. A small curved
// bias adds even partials; continuous folding enters only near full drive.
// Sample is double or float; both run the same curve.
template <typename Sample>
inline Sample parallelSawDrive(Sample input, Sample amount) noexcept
{
    using T = Sample;
    if (!std::isfinite(input)) return T(0);
    amount = std::isfinite(amount) ? std::clamp(amount, T(0), T(1)) : T(0.5);
    if (amount == T(0)) return input;
    // Numerical containment well outside the exciter's musical range.
    input = std::clamp(input, T(-8), T(8));
    const auto shaped = std::tanh((T(1) + T(2) * amount) * (input + T(0.25) * amount * input * input));
    const auto fold = std::max(T(0), (amount - T(0.65)) / T(0.35));
    const auto texture = (T(1) - T(0.25) * fold) * shaped + T(0.25) * fold * std::sin(T(4) * input);
    return (T(1) - T(0.45) * amount) * input + T(0.45) * amount * texture;
}
}
