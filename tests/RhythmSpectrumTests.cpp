#include "dsp/RhythmBus.h"
#include "dsp/RhythmOscillators.h"
#include <dsp_core/FixedEnvelope.h>
#include "support/StereoMetrics.h"
#include <array>
#include <iomanip>
#include <iostream>

namespace
{
constexpr int stages = 8;
using Frame = std::array<double, stages>;
struct ObservedDrive
{
    dsp_core::Oversampler4x oversampler;
    double lowpass = 0.0, dc = 0.0, lpCoefficient, dcCoefficient;
    std::array<std::array<double, 6>, 4> internal{};
    explicit ObservedDrive(double rate)
    {
        lpCoefficient = -std::expm1(-6.28318530717958647692 * 12000.0 / (4.0 * rate));
        dcCoefficient = -std::expm1(-6.28318530717958647692 * 20.0 / (4.0 * rate));
        oversampler.prepare();
    }
    static double quiet(double v) { return std::abs(v) < 1.0e-30 ? 0.0 : v; }
    double next(double input, double amount)
    {
        const auto gain = 1.0 + 3.0 * amount, bias = 0.15 * amount;
        const auto biasOutput = std::tanh(bias);
        const auto fold = std::max(0.0, 4.0 * amount - 3.0);
        int sample = 0;
        return oversampler.next(input, [&](double value) {
            auto& f = internal[sample++];
            f[0] = value;
            f[1] = std::tanh(gain * value + bias) - biasOutput;
            lowpass = quiet(lowpass + lpCoefficient * (f[1] - lowpass));
            f[2] = lowpass;
            f[3] = std::clamp((1.0 + amount) * lowpass, -1.0, 1.0);
            const auto reflected = 1.0 - std::abs(std::remainder(f[3] * (1.0 + 2.0 * fold) - 1.0, 4.0));
            f[4] = f[3] + fold * (reflected - f[3]);
            dc = quiet(dc + dcCoefficient * (f[4] - dc));
            f[5] = f[4] - dc;
            return (1.0 - amount) * value + amount * f[5];
        });
    }
};
// Offline reconstruction exposes filter boundaries without adding observation
// work to the instrument. Every sample is checked against the production bus.
struct ObservedBus
{
    dsp_core::Butterworth tight, hp[2], lp;
    dsp_core::PeakingEq presence, preDriveContour;
    dsp_core::DriveChain4x drive;
    ObservedDrive observedDrive;
    dsp_core::FixedDelay<double, dropforge::RhythmBus::latencySamples> gate;
    dropforge::RhythmBus actual;
    double amount;
    ObservedBus(double rate, double destruction) : observedDrive(rate), amount(destruction)
    {
        using Type = dsp_core::Butterworth::Type;
        tight.prepare(Type::highpass, 120.0, rate);
        preDriveContour.prepare(2800.0, 3.0, 0.7, rate);
        hp[0].prepare(Type::highpass, dropforge::RhythmBus::separationHz, rate, 0.541196100146197);
        hp[1].prepare(Type::highpass, dropforge::RhythmBus::separationHz, rate, 1.306562964876377);
        presence.prepare(1800.0, 6.0, 0.7, rate);
        lp.prepare(Type::lowpass, dropforge::RhythmBus::finalLowpassHz, rate);
        drive.prepare(rate);
        actual.setParameters(amount, 0.5); actual.prepare(rate);
    }
    Frame next(double input, double envelope, double& error)
    {
        Frame f{};
        f[0] = input;
        f[1] = tight.next(input);
        f[2] = preDriveContour.next(f[1]);
        f[3] = drive.next(f[2], 0.85 * amount);
        const auto reconstructedDrive = observedDrive.next(f[2], 0.85 * amount);
        if (!std::isfinite(reconstructedDrive) || !std::isfinite(f[3]))
            throw std::runtime_error("nonfinite drive");
        error = std::max(error, std::abs(reconstructedDrive - f[3]));
        f[4] = hp[1].next(hp[0].next(f[3]));
        f[5] = presence.next(f[4]);
        f[6] = lp.next(f[5]);
        const auto compensation = 1.0 / (1.0 + 0.8 * std::max(0.0, 4.0 * amount - 3.0));
        f[7] = dropforge::RhythmBus::outputGain * compensation * f[6]
            * gate.next(std::clamp(envelope * 6.0, 0.0, 1.0));
        const auto reference = actual.next(input, envelope);
        for (auto value : f) if (!std::isfinite(value)) throw std::runtime_error("nonfinite stage");
        if (!std::isfinite(reference)) throw std::runtime_error("nonfinite bus");
        error = std::max(error, std::abs(f[7] - reference));
        return f;
    }
};
struct Phrase { const char* name; std::array<int, 8> notes; int count; bool chopped; };
constexpr std::array<Phrase, 5> phrases{{
    {"low", {40}, 1, false}, {"mid", {64}, 1, false}, {"high", {88}, 1, false},
    {"dense", {40, 47, 52, 55, 59, 64, 67, 71}, 8, false},
    {"chops", {40, 47, 52, 55}, 4, true}}};
constexpr std::array<const char*, stages> names{
    "source", "tight", "pre_contour", "drive", "separation", "presence", "lowpass", "width_output"};
constexpr std::array<const char*, 6> internalNames{
    "upsampled4x", "soft4x", "interstage4x", "hard4x", "fold4x", "dc4x"};

void measure(double rate, double amount, const Phrase& phrase, double& error)
{
    std::array<dropforge::RhythmOscillators, 8> voices;
    dsp_core::FixedEnvelope envelope;
    envelope.prepare(static_cast<int>(std::ceil(rate * 0.001)), static_cast<int>(std::ceil(rate * 0.006)));
    auto start = [&] {
        for (int v = 0; v < phrase.count; ++v) voices[v].reset(1, phrase.notes[v]);
        envelope.start();
    };
    start();
    ObservedBus left(rate, amount), right(rate, amount);
    std::array<std::array<std::vector<float>, 2>, stages> audio;
    std::array<std::array<std::vector<float>, 2>, 6> internalAudio;
    const bool captureInternal = rate == 48000.0 && amount > 0.0;
    const int count = static_cast<int>(rate);
    const int begin = phrase.chopped ? 0 : count / 2;
    for (auto& stage : audio) for (auto& side : stage) side.reserve(count - begin);
    for (int i = 0; i < count; ++i)
    {
        const int beat = i % (count / 4);
        if (phrase.chopped && i > 0 && beat == 0) start();
        if (phrase.chopped && beat == count / 8) envelope.stop();
        const auto env = envelope.next();
        dropforge::RhythmOscillators::Stereo sum;
        // Same full-velocity, channel-1 note sum as Engine, before per-side drive.
        for (int v = 0; v < phrase.count; ++v)
        {
            if (!envelope.active()) continue;
            if (i % 32 == 0) voices[v].setPitch(440.0 * std::exp2((phrase.notes[v] - 69) / 12.0), rate, 32);
            const auto s = voices[v].next();
            sum.left += s.left * env; sum.right += s.right * env;
        }
        auto l = left.next(sum.left, env, error), r = right.next(sum.right, env, error);
        const auto mid = 0.5 * (l[7] + r[7]);
        const auto side = 0.5 * (l[7] - r[7]) * dropforge::RhythmOscillators::spread;
        l[7] = mid + side; r[7] = mid - side;
        if (i < begin) continue;
        // Hann on held segments avoids interpreting cut-window leakage as air.
        // Chops include their real attack/release energy, rectangularly windowed.
        const auto window = phrase.chopped ? 1.0 : 0.5 - 0.5 * std::cos(
            6.28318530717958647692 * (i - begin) / (count - begin - 1));
        for (int s = 0; s < stages; ++s)
        {
            audio[s][0].push_back(static_cast<float>(l[s] * window));
            audio[s][1].push_back(static_cast<float>(r[s] * window));
        }
        if (captureInternal)
            for (int sub = 0; sub < 4; ++sub)
                for (int s = 0; s < 6; ++s)
                {
                    internalAudio[s][0].push_back(static_cast<float>(left.observedDrive.internal[sub][s] * window));
                    internalAudio[s][1].push_back(static_cast<float>(right.observedDrive.internal[sub][s] * window));
                }
    }
    constexpr std::array<double, 5> edges{350.0, 1200.0, 4000.0, 9000.0, 16000.0};
    for (int s = 0; s < stages + (captureInternal ? 6 : 0); ++s)
    {
        const auto& signal = s < stages ? audio[s] : internalAudio[s - stages];
        const auto measurementRate = s < stages ? rate : rate * 4.0;
        const auto L = dsp_core::test::spectrum(signal[0]);
        const auto R = dsp_core::test::spectrum(signal[1]);
        std::array<double, 4> power{};
        double total = 0.0;
        for (std::size_t k = 0; k <= L.size() / 2; ++k)
        {
            const auto hz = k * measurementRate / L.size();
            const auto p = (k == 0 || k == L.size() / 2 ? 1.0 : 2.0)
                * 0.5 * (std::norm(L[k]) + std::norm(R[k]));
            total += p;
            for (int b = 0; b < 4; ++b) if (hz >= edges[b] && hz < edges[b + 1]) power[b] += p;
        }
        // Ratios to each stage's entire spectrum separate spectral balance from
        // bus gain. Absolute band power also permits stage-to-stage loss checks.
        const auto norm = static_cast<double>(L.size()) * signal[0].size();
        double windowPower = 1.0;
        if (!phrase.chopped) windowPower = 0.375 * (audio[s][0].size() - 1.0) / audio[s][0].size();
        std::cout << rate << ',' << amount << ',' << phrase.name << ','
            << (s < stages ? names[s] : internalNames[s - stages]);
        for (const auto p : power) std::cout << ',' << dsp_core::test::powerDb(p / (norm * windowPower));
        for (const auto p : power) std::cout << ',' << dsp_core::test::powerDb(p / std::max(total, 1.0e-30));
        std::cout << '\n';
    }
}
}
int main()
{
    std::cout << std::setprecision(10)
        << "rate,drive,phrase,stage,body_dbfs,identity_dbfs,brilliance_dbfs,air_dbfs,body_relative_db,identity_relative_db,brilliance_relative_db,air_relative_db\n";
    double error = 0.0;
    for (const auto rate : {44100.0, 48000.0, 96000.0, 192000.0})
        for (const auto amount : {0.0, 0.5, 1.0})
            for (const auto& phrase : phrases) measure(rate, amount, phrase, error);
    std::cerr << "maximum_bus_reconstruction_error=" << error << '\n';
    return std::isfinite(error) && error <= 1.0e-12 ? 0 : 1;
}
