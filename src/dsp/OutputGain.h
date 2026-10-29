#pragma once

#include <dsp_core/LinearRamp.h>
#include <algorithm>
#include <cmath>

namespace dropforge
{
class OutputGain
{
public:
    void prepare(double sampleRate, float db) noexcept
    {
        const auto rate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 44100.0, 192000.0) : 44100.0;
        rampSamples = static_cast<int>(std::ceil(rate * 0.005));
        reset(db);
    }
    void reset(float db) noexcept
    {
        lastDb = safeDb(db);
        gain.reset(std::pow(10.0, lastDb / 20.0));
    }
    void setTarget(float db) noexcept
    {
        db = safeDb(db);
        if (db != lastDb)
        {
            lastDb = db;
            gain.setTarget(std::pow(10.0, db / 20.0), rampSamples);
        }
    }
    void process(float* const* channels, int channelCount, int samples) noexcept
    {
        for (int sample = 0; sample < samples; ++sample)
        {
            const auto multiplier = static_cast<float>(gain.next());
            for (int channel = 0; channel < channelCount; ++channel)
                channels[channel][sample] *= multiplier;
        }
    }

private:
    static float safeDb(float db) noexcept
    {
        return std::isfinite(db) ? std::clamp(db, -24.0f, 6.0f) : 0.0f;
    }
    dsp_core::LinearRamp gain;
    float lastDb = 0.0f;
    int rampSamples = 221;
};
}
