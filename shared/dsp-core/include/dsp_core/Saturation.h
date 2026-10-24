#pragma once

#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Provisional upper-branch curve. Must run inside an oversampler; never use
// this on the clean sine foundation. The caller smooths amount before entry.
inline double saturate(double input, double amount) noexcept
{
    if (!std::isfinite(input)) return 0.0;
    amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
    if (amount == 0.0) return input;
    const auto driven = std::tanh((1.0 + 3.0 * amount) * input);
    return (1.0 - amount) * input + amount * driven;
}
}
