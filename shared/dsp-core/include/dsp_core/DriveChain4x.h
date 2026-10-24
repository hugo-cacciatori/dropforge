#pragma once

#include "Oversampler4x.h"
#include <algorithm>
#include <cmath>

namespace dsp_core
{
// Shared multistage drive, independent of instrument gain/envelopes/filters.
// Callers smooth amount at the base rate. Every nonlinear operation and both
// internal filters run at 4x; the clean bass foundation must never enter here.
class DriveChain4x
{
public:
    static constexpr int latencySamples = Oversampler4x::latencySamples;
    void prepare(double sampleRate) noexcept
    {
        const auto rate = std::isfinite(sampleRate) && sampleRate >= 44100.0 && sampleRate <= 192000.0
            ? sampleRate : 44100.0;
        const auto internalRate = rate * Oversampler4x::factor;
        // A gentle inter-stage rolloff limits what hits the harder stage.
        // Compute at the actual internal rate (up to 768 kHz).
        lowpassCoefficient = -std::expm1(-twoPi * 12000.0 / internalRate);
        dcCoefficient = -std::expm1(-twoPi * 20.0 / internalRate);
        oversampler.prepare();
        reset();
    }
    void reset() noexcept
    {
        oversampler.reset();
        lowpass = dc = 0.0;
    }
    double next(double input, double amount) noexcept
    {
        amount = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.5;
        const auto drive = 1.0 + 3.0 * amount;
        const auto bias = 0.15 * amount;
        const auto biasOutput = std::tanh(bias);
        // Folding enters continuously only in the last quarter of Destruction.
        const auto fold = std::max(0.0, 4.0 * amount - 3.0);
        return oversampler.next(input, [this, amount, drive, bias, biasOutput, fold](double value) noexcept
        {
            const auto soft = std::tanh(drive * value + bias) - biasOutput;
            lowpass = quiet(lowpass + lowpassCoefficient * (soft - lowpass));
            const auto hard = std::clamp((1.0 + amount) * lowpass, -1.0, 1.0);
            // Continuous triangular reflection adds a distinct high-drive
            // texture without a discontinuous wrap or unbounded fuzz gain.
            const auto reflected = 1.0 - std::abs(std::remainder(hard * (1.0 + 2.0 * fold) - 1.0, 4.0));
            const auto shaped = hard + fold * (reflected - hard);
            dc = quiet(dc + dcCoefficient * (shaped - dc));
            // Bias can generate DC under sustained playing; remove it before
            // decimation. Hidden histories advance even at zero amount.
            return (1.0 - amount) * value + amount * (shaped - dc);
        });
    }
private:
    static double quiet(double value) noexcept { return std::abs(value) < 1.0e-30 ? 0.0 : value; }
    static constexpr double twoPi = 6.28318530717958647692;
    Oversampler4x oversampler;
    double lowpass = 0.0, dc = 0.0;
    double lowpassCoefficient = 0.1, dcCoefficient = 0.001;
};
}
