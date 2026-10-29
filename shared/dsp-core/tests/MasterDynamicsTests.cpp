#include <dsp_core/GlueCompressor.h>
#include <dsp_core/LoudnessMeter.h>
#include <dsp_core/PeakLimiter.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>

namespace
{
constexpr double pi = 3.14159265358979323846;
int failures = 0;
void expect(bool condition, const char* what)
{
    if (!condition) { std::cout << "FAILED: " << what << '\n'; ++failures; }
}
// Deterministic white noise in [-1, 1].
struct Noise
{
    std::uint32_t state = 0x12345678u;
    float next() noexcept
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return static_cast<float>(state) / 2147483648.0f - 1.0f;
    }
};

void limiterCeiling()
{
    constexpr float ceiling = 0.89125093813374556f;
    float worst = 0.0f, deepestDb = 0.0f;
    for (const auto rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        dsp_core::PeakLimiter limiter;
        limiter.prepare(ceiling, rate);
        Noise noise;
        const auto n = static_cast<int>(rate);
        for (int i = 0; i < n; ++i)
        {
            // Bursts up to +26 dB over the ceiling, single-sample spikes,
            // silence and a loud low sine, each a quarter of the second.
            const auto part = 4 * i / n;
            float l = 0.0f, r = 0.0f;
            if (part == 0) { l = 20.0f * noise.next(); r = 20.0f * noise.next(); }
            else if (part == 1 && i % 97 == 0) { l = 8.0f; r = -3.0f; }
            else if (part == 3)
            {
                l = 3.0f * static_cast<float>(std::sin(2.0 * pi * 50.0 * i / rate));
                r = -l;
            }
            limiter.process(l, r);
            expect(std::isfinite(l) && std::isfinite(r), "limiter output is finite");
            worst = std::max({worst, std::abs(l), std::abs(r)});
            deepestDb = std::min(deepestDb, limiter.gainReductionDb());
        }
    }
    expect(worst <= ceiling, "no sample ever exceeds the ceiling");
    std::cout << "limiter_worst_peak=" << worst << " ceiling=" << ceiling << " deepest_reduction_db=" << deepestDb << '\n';

    // Below the ceiling it is a pure delay of exactly latencySamples.
    dsp_core::PeakLimiter quiet;
    quiet.prepare(ceiling, 48000.0);
    Noise noise;
    float history[dsp_core::PeakLimiter::latencySamples + 1]{};
    for (int i = 0; i < 48000; ++i)
    {
        const auto x = 0.8f * noise.next();
        float l = x, r = -x;
        quiet.process(l, r);
        const auto expected = i >= dsp_core::PeakLimiter::latencySamples
            ? history[(i - dsp_core::PeakLimiter::latencySamples) % (dsp_core::PeakLimiter::latencySamples + 1)] : 0.0f;
        history[i % (dsp_core::PeakLimiter::latencySamples + 1)] = x;
        expect(l == expected && r == -expected, "under the ceiling the limiter is an exact delay");
    }

    // A single peak dips the gain smoothly: no step larger than one window's share.
    dsp_core::PeakLimiter smooth;
    smooth.prepare(ceiling, 48000.0);
    float previous = 1.0f, largestStep = 0.0f;
    for (int i = 0; i < 9600; ++i)
    {
        float l = i == 4000 ? 4.0f : 0.5f, r = l;
        smooth.process(l, r);
        const auto gain = std::pow(10.0f, 0.05f * smooth.gainReductionDb());
        largestStep = std::max(largestStep, std::abs(gain - previous));
        previous = gain;
    }
    expect(largestStep <= (1.0f - ceiling / 4.0f) / dsp_core::PeakLimiter::window + 1.0e-6f,
           "gain glides into a peak over the whole look-ahead window");
}

void glue()
{
    // Quiet material passes untouched; a loud steady tone is reduced by
    // the soft-knee 2:1 law; the two channels always get the same gain.
    for (const auto rate : {44100.0, 192000.0})
    {
        dsp_core::GlueCompressor quiet, loud;
        quiet.prepare({}, rate);
        loud.prepare({}, rate);
        double reduction = 0.0;
        const auto n = static_cast<int>(rate);
        for (int i = 0; i < n; ++i)
        {
            const auto s = static_cast<float>(std::sin(2.0 * pi * 220.0 * i / rate));
            float ql = 0.1f * s, qr = 0.05f * s;
            quiet.process(ql, qr);
            expect(ql == 0.1f * s && qr == 0.05f * s, "material 6 dB under the knee is untouched");
            float ll = s, lr = 0.5f * s;
            loud.process(ll, lr);
            if (std::abs(s) > 0.5f)
                expect(std::abs(lr / ll - 0.5f) < 1.0e-6f, "glue is stereo-linked");
            reduction = loud.gainReductionDb();
        }
        // 0 dBFS peak, threshold -10, ratio 2: about -5 dB once settled
        // (the level detector rides slightly under the peak between cycles).
        expect(reduction < -4.0 && reduction > -5.5, "a 0 dBFS tone is held about 5 dB down");
        std::cout << "glue_rate=" << rate << " zero_dbfs_reduction_db=" << reduction << '\n';
    }
}

void loudness()
{
    // EBU Tech 3341 style checks: a 997 Hz sine at full scale in one channel
    // reads -3.01 LUFS; at -23 dBFS in both channels it reads -23.0 LUFS.
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        for (const auto both : {false, true})
        {
            auto meter = std::make_unique<dsp_core::LoudnessMeter>();
            meter->prepare(rate);
            const auto amplitude = both ? std::pow(10.0, -23.0 / 20.0) : 1.0;
            for (int i = 0; i < static_cast<int>(5 * rate); ++i)
            {
                const auto s = static_cast<float>(amplitude * std::sin(2.0 * pi * 997.0 * i / rate));
                meter->process(s, both ? s : 0.0f);
            }
            const auto target = both ? -23.0 : -3.01;
            const auto integrated = meter->integratedLufs(), shortTerm = meter->shortTermMaxLufs();
            std::cout << "loudness_rate=" << rate << " target=" << target << " integrated=" << integrated
                      << " short_term_max=" << shortTerm << " momentary_max=" << meter->momentaryMaxLufs() << '\n';
            expect(std::abs(integrated - target) < 0.1, "integrated loudness within 0.1 LU");
            expect(std::abs(shortTerm - target) < 0.1, "short-term loudness within 0.1 LU");
        }
    // The relative gate: 10 s at -20 then 10 s at -40 (more than 10 LU down)
    // integrates to the louder part only.
    auto meter = std::make_unique<dsp_core::LoudnessMeter>();
    meter->prepare(48000.0);
    for (int i = 0; i < 20 * 48000; ++i)
    {
        const auto amplitude = i < 10 * 48000 ? std::pow(10.0, -20.0 / 20.0) : std::pow(10.0, -40.0 / 20.0);
        const auto s = static_cast<float>(amplitude * std::sin(2.0 * pi * 997.0 * i / 48000.0));
        meter->process(s, s);
    }
    std::cout << "gated_integrated=" << meter->integratedLufs() << '\n';
    expect(std::abs(meter->integratedLufs() - (-20.0)) < 0.1, "relative gate drops the quiet half");
}
}

int main()
{
    limiterCeiling();
    glue();
    loudness();
    return failures == 0 ? 0 : 1;
}
