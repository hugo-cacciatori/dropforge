#include "dsp/Engine.h"
#include <dsp_core/Butterworth.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
// A band-limited periodic probe for filter and FIR response checks: the
// expected values are computed from whatever this probe contains.
struct ProbeSaw
{
    dsp_core::WavetableOscillator oscillator;
    void reset(double phase) noexcept { oscillator.reset(static_cast<std::uint32_t>(phase * 4294967296.0)); }
    double next(double hz, double rate) noexcept
    {
        oscillator.setFrequency(dsp_core::WavetableBank::saw(), hz, rate);
        return oscillator.next();
    }
};
constexpr double pi = 3.14159265358979323846;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::complex<double> coefficient(const std::vector<double>& audio, double hz, double rate)
{
    std::complex<double> sum{};
    for (std::size_t i = 0; i < audio.size(); ++i)
        sum += audio[i] * std::polar(1.0, -2.0 * pi * hz * static_cast<double>(i) / rate);
    return sum * (2.0 / static_cast<double>(audio.size()));
}
// Power of one sinusoidal partial (amplitude squared over two) in audio that
// was multiplied by a window whose samples sum to windowSum; a rotating
// phasor instead of polar() per sample keeps hundreds of partials quick.
double partialPower(const std::vector<double>& audio, double hz, double rate, double windowSum)
{
    const auto step = std::polar(1.0, -2.0 * pi * hz / rate);
    std::complex<double> sum{}, rotation{1.0, 0.0};
    for (const auto value : audio) { sum += value * rotation; rotation *= step; }
    const auto amplitude = std::abs(sum) * 2.0 / windowSum;
    return 0.5 * amplitude * amplitude;
}
std::complex<double> butterworthResponse(bool highpass, double hz, double cutoff, double rate)
{
    // Independent analog prototype evaluated at the bilinear prewarped frequency.
    const auto r = std::tan(pi * hz / rate) / std::tan(pi * cutoff / rate);
    return (highpass ? -r * r : 1.0) / std::complex<double>(1.0 - r * r, std::sqrt(2.0) * r);
}
std::complex<double> notchResponse(double hz, double center, double rate)
{
    const auto r = std::tan(pi * hz / rate) / std::tan(pi * center / rate);
    return (1.0-r*r) / std::complex<double>(1.0-r*r, r/0.7);
}

void filterResponses()
{
    double worstError = 0.0;
    for (const auto rate : {44100, 48000, 88200, 96000, 176400, 192000})
        for (const auto cutoff : {80.0, 140.0, 250.0})
            for (const auto hz : {35.0, 70.0, 140.0, 280.0, 5000.0, 10000.0})
            {
                dsp_core::LinkwitzRileyHighpass pre, post;
                dsp_core::Butterworth lowpass, highpass;
                pre.prepare(cutoff, rate); post.prepare(cutoff, rate);
                lowpass.prepare(dsp_core::Butterworth::Type::lowpass, 5000.0, rate);
                highpass.prepare(dsp_core::Butterworth::Type::highpass, cutoff, rate);
                std::array<std::complex<double>, 4> sums{};
                for (int i = 0; i < rate * 3 / 2; ++i)
                {
                    const auto phase = 2.0 * pi * hz * i / rate;
                    const auto input = std::sin(phase);
                    const auto first = pre.next(input);
                    const std::array<double, 4> values{first, post.next(first), lowpass.next(input), highpass.next(input)};
                    if (i >= rate / 2)
                        for (std::size_t j = 0; j < sums.size(); ++j)
                            sums[j] += values[j] * std::polar(1.0, -phase);
                }
                const auto hp = butterworthResponse(true, hz, cutoff, rate);
                const std::array<std::complex<double>, 4> expected{hp * hp, hp * hp * hp * hp,
                    butterworthResponse(false, hz, 5000.0, rate), hp};
                for (std::size_t j = 0; j < sums.size(); ++j)
                {
                    const auto measured = sums[j] * std::complex<double>(0.0, 2.0 / rate);
                    const auto error = std::abs(measured - expected[j]);
                    worstError = std::max(worstError, error);
                    require(error < 1.0e-7, "HP/LP magnitude AND phase match prewarped Butterworth/LR4 response");
                }
            }
    dsp_core::Butterworth clamped, reference;
    clamped.prepare(dsp_core::Butterworth::Type::lowpass, 1.0e9, 44100);
    reference.prepare(dsp_core::Butterworth::Type::lowpass, 44100 * 0.45, 44100);
    for (int i = 0; i < 100000; ++i)
    {
        const auto value = clamped.next(i == 0 ? 1.0 : 0.0);
        require(value == reference.next(i == 0 ? 1.0 : 0.0), "filter clamps below Nyquist deterministically");
        require(std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL, "filter decay stays finite/normal");
    }
    std::cout << "filter_complex_response_max_error=" << worstError << " rates=6 split_hz=80,140,250\n";
}

void branchMeasurements()
{
    dsp_core::Oversampler4x fir;
    fir.prepare();
    double worstHarmonic = 0.0, worstLeakage = 0.0, worstPhaseError = 0.0, worstSineError = 0.0;
    for (const auto rate : {44100, 48000, 88200, 96000, 176400, 192000})
        for (const auto hz : {4.0, 35.0, 70.0, 140.0, 400.0, 6000.0})
        {
            dropforge::BassVoice voice;
            ProbeSaw richReference, upperReference;
            upperReference.reset(0.41);
            // Inject the retained periodic probe into both source inputs to
            // test filter/FIR gain and phase independently of aperiodic grit.
            const auto fade = std::clamp((0.45 - 8.0*hz/rate) / 0.15, 0.0, 1.0);
            const auto upperWeight = fade*fade*(3.0-2.0*fade);
            voice.prepare(rate);
            voice.setParameters(140.0, 0.0); // Preserve the linear-path reference checks.
            voice.start(hz, 127);
            for (int i = 0; i < rate / 2; ++i)
            {
                const auto value = 0.65*richReference.next(hz, rate)
                                 + 0.2*upperWeight*upperReference.next(8.0*hz, rate);
                const dropforge::BassExciter::Sources probe{value,value};
                voice.next(1.0, dropforge::BassComponent::all, &probe);
            }
            std::vector<double> sine, rich, output, rawRich;
            for (int i = 0; i < rate; ++i)
            {
                const auto value = 0.65*richReference.next(hz, rate)
                                 + 0.2*upperWeight*upperReference.next(8.0*hz, rate);
                const dropforge::BassExciter::Sources probe{value,value};
                const auto sample = voice.next(1.0, dropforge::BassComponent::all, &probe);
                sine.push_back(sample.sine); rich.push_back(sample.rich); output.push_back(sample.output);
                rawRich.push_back(value);
                require(std::isfinite(sample.output) && std::abs(sample.output) < 0.3, "bass is finite with conservative headroom");
            }
            // The sine plays the nearest 32-bit phase step to hz (within
            // 0.00002 Hz); analyse it there, so the window does not leak.
            const auto played = std::llround(hz / rate * 4294967296.0) * rate / 4294967296.0;
            const auto fundamental = coefficient(sine, played, rate);
            // 32-bit float and the table sine: within 1e-5 (0.0001 dB; slow reads
            // of the table at 4 Hz measure 3e-6).
            const auto sineError = std::abs(std::abs(fundamental) - dropforge::BassVoice::sineGain) / dropforge::BassVoice::sineGain;
            worstSineError = std::max(worstSineError, sineError);
            if (sineError >= 1.0e-5) std::cerr << "clean sine rate=" << rate << " hz=" << hz << " relative_error=" << sineError << '\n';
            require(sineError < 1.0e-5,
                    "clean sine retains full fundamental below and above Split, including sub-audible notes");
            for (int harmonic = 2; harmonic <= 10 && hz * harmonic < rate * 0.5; ++harmonic)
            {
                const auto ratio = std::abs(coefficient(sine, played * harmonic, rate) / fundamental);
                worstHarmonic = std::max(worstHarmonic, ratio);
                if (ratio >= 1.0e-6) std::cerr << "sine harmonic rate=" << rate << " hz=" << hz << " k=" << harmonic << " dbc=" << 20.0 * std::log10(ratio) << std::endl;
                require(ratio < 1.0e-6, "isolated sine harmonics below -120 dBc (32-bit float floor)");
            }
            const auto upper = coefficient(rich, hz, rate);
            const auto hp = butterworthResponse(true, hz, 140.0, rate);
            std::complex<double> firResponse{};
            for (std::size_t i = 0; i < fir.coefficients().size(); ++i)
                firResponse += fir.coefficients()[i] * std::polar(1.0, -2.0 * pi * hz * static_cast<double>(i) / (rate * 4.0));
            // At zero Destruction both source paths coincide; a normalized
            // mix retains their original linear gain and phase.
            const auto expectedUpper = static_cast<double>(dropforge::BassVoice::richGain) * coefficient(rawRich, hz, rate)
                * hp * hp * hp * hp * firResponse * firResponse
                * notchResponse(hz, hz, rate) * butterworthResponse(true, hz, 260.0, rate)
                * butterworthResponse(false, hz, std::min(std::max(3500.0, 4.0*hz), rate*0.45), rate);
            // 32-bit float filters and kernel: within 1e-4 of the 64-bit model
            // (-80 dB); a wrong filter or notch would be off by percent.
            if (std::abs(upper - expectedUpper) >= 1.0e-7 + 1.0e-4 * std::abs(expectedUpper))
                std::cerr << "grit root rate=" << rate << " root=" << hz << " expected=" << std::abs(expectedUpper)
                          << " error=" << std::abs(upper - expectedUpper) << '\n';
            require(std::abs(upper - expectedUpper) < 1.0e-7 + 1.0e-4 * std::abs(expectedUpper),
                    "grit root notch leaves the clean oscillator to own the fundamental");
            double harmonicPower = 0.0;
            for (const auto partial : {2.0 * hz, 3.0 * hz})
            {
                if (partial >= rate * 0.5) continue;
                const auto splitResponse = butterworthResponse(true, partial, 140.0, rate);
                std::complex<double> firPartial{};
                for (std::size_t i = 0; i < fir.coefficients().size(); ++i)
                    firPartial += fir.coefficients()[i] * std::polar(1.0, -2.0*pi*partial*i/(rate*4.0));
                const auto expected = static_cast<double>(dropforge::BassVoice::richGain) * coefficient(rawRich, partial, rate)
                    * std::pow(splitResponse, 4) * firPartial * firPartial
                    * notchResponse(partial, hz, rate) * butterworthResponse(true, partial, 260.0, rate)
                    * butterworthResponse(false, partial, std::min(std::max(3500.0, 4.0*hz), rate*0.45), rate);
                const auto measured = coefficient(rich, partial, rate);
                if (std::abs(measured-expected) >= 1.0e-7 + 1.0e-4 * std::abs(expected))
                    std::cerr << "grit partial rate=" << rate << " root=" << hz << " partial=" << partial
                              << " expected=" << std::abs(expected) << " error=" << std::abs(measured-expected) << '\n';
                require(std::abs(measured-expected) < 1.0e-7 + 1.0e-4 * std::abs(expected),
                        "grit harmonics retain measured gain/phase through Split, root notch and LF cleanup");
                harmonicPower += std::norm(measured);
            }
            const auto summed = coefficient(output, hz, rate);
            const auto finalResponse = butterworthResponse(false, hz, 5000.0, rate);
            const auto error = std::abs(summed - finalResponse * (fundamental + upper));
            worstPhaseError = std::max(worstPhaseError, error);
            require(error < 1.0e-7 + 1.0e-4 * std::abs(finalResponse * (fundamental + upper)),
                    "recombined fundamental has the measured branch phase/level and independent final LP");
            require(std::abs(summed) > 0.75 * std::abs(finalResponse * fundamental),
                    "branch recombination cannot cancel the sine foundation");
            if (hz >= 400.0) require(std::sqrt(harmonicPower) > 0.01, "grit supplies upper harmonics rather than a competing root");
            if (hz <= 70.0)
            {
                const auto leakage = std::abs(upper / fundamental);
                worstLeakage = std::max(worstLeakage, leakage);
                require(leakage < 0.001, "rich fundamental leakage below 70 Hz stays at least 60 dB below clean sine");
            }
            // Sustain, independent fixed envelope and exact finite release.
            voice.stop();
            for (int i = 0; i <= static_cast<int>(std::ceil(rate * dropforge::BassVoice::releaseSeconds)) + dropforge::BassVoice::latencySamples; ++i)
                voice.next(1.0);
            require(voice.next(1.0).output == 0.0, "bass release reaches exact silence within 6 ms plus endpoint");
        }
    std::cout << "sine_max_harmonic_dbc=" << 20.0 * std::log10(worstHarmonic)
              << " upper_fundamental_leakage_below_70hz_dbc=" << 20.0 * std::log10(worstLeakage)
              << " recombination_complex_error=" << worstPhaseError << " clean_sine_relative_error=" << worstSineError << '\n';
}

std::vector<float> render(dropforge::Engine& engine, int samples, int block = 512)
{
    std::vector<float> left(static_cast<std::size_t>(samples)), right(left.size());
    for (int offset = 0; offset < samples;)
    {
        const auto count = std::min(block, samples - offset);
        engine.process(left.data() + offset, right.data() + offset, count, dropforge::LayerMask::bass);
        offset += count;
    }
    require(left == right, "bass engine output is bit-exact L == R");
    // Pre-master engine output: 0.3 of Anvil itself, scaled by the engine's
    // Anvil balance lift; the master's limiter holds the final ceiling.
    for (auto value : left)
        require(std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL
                && std::abs(value) < 0.3f * static_cast<float>(dropforge::Engine::anvilBalance),
                "audible bass output stays finite, normal and bounded");
    return left;
}
double pitch(dropforge::Engine& engine, double rate, bool subAudible = false)
{
    std::array<dsp_core::Butterworth, 4> analysisLowpass;
    for (auto& filter : analysisLowpass)
        filter.prepare(dsp_core::Butterworth::Type::lowpass, subAudible ? 20.0 : 70.0, rate);
    const auto warmup = render(engine, static_cast<int>(rate / 2));
    for (auto sample : warmup)
        for (auto& filter : analysisLowpass) sample = static_cast<float>(filter.next(sample));
    auto audio = render(engine, static_cast<int>(rate));
    // Strong grit adds crossings; isolate the 49-62 Hz (or sub-audible)
    // fundamental in this measurement only, retaining the 0.01 Hz limit.
    for (auto& sample : audio)
        for (auto& filter : analysisLowpass) sample = static_cast<float>(filter.next(sample));
    int crossings = 0;
    double first = 0.0, last = 0.0;
    for (std::size_t i = 1; i < audio.size(); ++i)
        if (audio[i - 1] < 0.0f && audio[i] >= 0.0f)
        {
            const auto position = static_cast<double>(i - 1) - audio[i - 1] / (audio[i] - audio[i - 1]);
            if (crossings++ == 0) first = position;
            last = position;
        }
    require(crossings > 1, "bass has sufficient fundamental crossings");
    return (crossings - 1) * rate / (last - first);
}
void audibleRouting()
{
    double worstPitchError = 0.0, worstTransitionStep = 0.0;
    for (const auto rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        dropforge::Engine engine;
        engine.prepare(rate);
        engine.noteOn(2, 45, 127); // 55 Hz bass.
        auto reference = engine;
        require(render(engine, 4096, 1) == render(reference, 4096, 73), "bass is block-partition invariant");
        engine.pitchBend(1, 16383);
        for (const auto wheel : {8192, 0, 16383})
        {
            engine.pitchBend(2, wheel);
            const auto expected = 55.0 * std::exp2((wheel == 0 ? -2.0 : wheel == 8192 ? 0.0 : 2.0) / 12.0);
            const auto error = std::abs(pitch(engine, rate) - expected);
            worstPitchError = std::max(worstPitchError, error);
            require(error < 0.01, "bass follows selected channel bend and exact octave-down target");
        }
        engine.noteOn(1, 69, 64); // Unselected; no audio restart or bend change.
        reference = engine;
        engine.noteOn(1, 69, 32);
        require(render(engine, 1024) == render(reference, 1024), "background repeated keys do not affect audible bass");
        engine.noteOff(2, 45); // Handover to channel 1, with its remembered bend/velocity.
        require(std::abs(pitch(engine, rate) - 220.0 * std::exp2(2.0 / 12.0)) < 0.01,
                "handover uses new selected channel's bend");
        auto previous = render(engine, 512).back();
        for (int i = 0; i < 1000; ++i)
        {
            engine.noteOn(1, 69, (i % 2) == 0 ? 127 : 1);
            previous = render(engine, dropforge::Engine::latencySamples).back();
            const auto audio = render(engine, 3);
            worstTransitionStep = std::max(worstTransitionStep, std::abs(static_cast<double>(audio.front() - previous)));
            previous = audio.back();
        }
        require(worstTransitionStep == 0.0, "rapid bass retriggers begin at the exact previous sample");
        engine.allNotesOff(1);
        const auto tail = render(engine, static_cast<int>(rate / 50));
        require(tail.back() == 0.0f, "panic/release ends exactly");
        engine.prepare(rate);
        engine.noteOn(1, 45, 127);
        previous = render(engine, static_cast<int>(rate / 4)).back();
        engine.noteOn(2, 52, 64);
        engine.noteOff(1, 45);
        previous = render(engine, dropforge::Engine::latencySamples).back();
        require(render(engine, 1).front() == previous, "selected-key handover starts without a discontinuity");
        render(engine, 4096);
        engine.noteOff(2, 52);
        render(engine, 4096);
        engine.noteOn(1, 45, 127);
        previous = render(engine, static_cast<int>(rate / 4)).back();
        engine.noteOff(1, 45);
        const auto release = render(engine, static_cast<int>(rate / 50));
        for (const auto value : release)
        {
            if (std::abs(value - previous) >= 0.005f)
                std::cerr << "release_rate=" << rate << " sample_step=" << std::abs(value - previous) << '\n';
            require(std::abs(value - previous) < 0.005f, "low-note release has no abrupt waveform cut");
            previous = value;
        }
        for (const auto note : {0, 1, 12, 69, 127})
        {
            engine.prepare(rate);
            engine.noteOn(16, note, 127);
            require(render(engine, static_cast<int>(rate / 2)).back() != 0.0f, "boundary notes produce actual audio");
            if (note == 0)
                require(std::abs(pitch(engine, rate, true) - 440.0 * std::exp2(-81.0 / 12.0)) < 0.01,
                        "MIDI zero generates unclamped 4.0879 Hz octave-down bass");
        }
    }
    dropforge::Engine loud, quiet;
    loud.prepare(48000); quiet.prepare(48000);
    loud.noteOn(1, 45, 127); quiet.noteOn(1, 45, 64);
    const auto a = render(loud, 48000), b = render(quiet, 48000);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(std::abs(b[i] - a[i] * (64.0f / 127.0f)) < 3.0e-8f, "bass velocity changes gain without envelope timing");
    std::cout << "bass_pitch_max_error_hz=" << worstPitchError << " retrigger_boundary_step=" << worstTransitionStep
              << " mono=exact block_invariance=exact velocity=linear\n";
}
void drivenBassTests(dropforge::AnvilGrit grit)
{
    double maximumResidualStep = 0.0, worstLowWeightDb = 0.0, worstLeakageDb = -1000.0, naturalStep = 0.0, ownStep = 0.0;
    for (const auto rate : {44100, 48000, 88200, 96000, 176400, 192000})
    {
        dropforge::BassVoice changed, reference, harshest;
        for (auto* voice : {&changed, &reference, &harshest})
        {
            voice->setGrit(grit); voice->prepare(rate);
        }
        harshest.setParameters(80.0, 1.0);
        harshest.reset();
        for (auto* voice : {&changed, &reference, &harshest}) voice->start(55.0, 127);
        // The growl's own steepest step at its harshest steady setting: its
        // folds are much steeper than grit A, so they set its click threshold.
        float previousHarsh = 0.0f;
        for (int i = 0; i < rate / 2; ++i)
        {
            const auto value = harshest.next(1.0).output;
            if (i > rate / 4) naturalStep = std::max(naturalStep, static_cast<double>(std::abs(value - previousHarsh)));
            previousHarsh = value;
        }
        double previousResidual = 0.0;
        float previousOutput = 0.0f;
        for (int i = 0; i < rate; ++i)
        {
            if (i % 777 == 0)
            {
                auto beforeChange = changed;
                changed.setParameters((i / 777) % 2 == 0 ? 80.0 : 250.0, (i / 777) % 2 == 0 ? 0.0 : 1.0);
                auto verify = changed;
                require(verify.next(1.0).output == beforeChange.next(1.0).output,
                        "new parameter targets start at current values without a sample discontinuity");
            }
            const auto actual = changed.next(1.0), clean = reference.next(1.0);
            require(actual.sine == clean.sine, "Split/Destruction automation leaves clean sine bit-exact");
            require(std::isfinite(actual.output) && std::fpclassify(actual.output) != FP_SUBNORMAL && std::abs(actual.output) < 0.3,
                    "driven/automated bass remains bounded finite and normal");
            const auto residual = actual.output - clean.output;
            maximumResidualStep = std::max(maximumResidualStep, std::abs(residual - previousResidual));
            previousResidual = residual;
            ownStep = std::max(ownStep, static_cast<double>(std::abs(actual.output - previousOutput)));
            previousOutput = actual.output;
        }
    }
    std::cout << "automation_residual_step_probe=" << maximumResidualStep << " own_step=" << ownStep
              << " harshest_steady_step=" << naturalStep << '\n';
    if (grit == dropforge::AnvilGrit::a)
        require(maximumResidualStep < 0.01, "low-note automation residual has no step larger than 0.01");
    else
        // The growl's folds are far steeper than grit A, so the residual of two
        // settings is no click measure. Instead the automated voice must never
        // step further than the growl does at its harshest steady setting.
        require(ownStep < 1.1 * naturalStep, "growl automation steps no further than its harshest steady sound");
    for (const auto rate : {44100, 48000, 192000})
        for (const auto hz : {35.0, 70.0})
            for (const auto cutoff : {80.0, 250.0})
                for (const auto amount : {0.0, 1.0})
                {
                    dropforge::BassVoice voice;
                    voice.setGrit(grit);
                    voice.prepare(rate);
                    voice.setParameters(cutoff, amount);
                    voice.reset();
                    voice.start(hz, 127);
                    for (int i = 0; i < rate / 2; ++i) voice.next(1.0);
                    std::vector<double> sine, upper, full;
                    for (int i = 0; i < rate; ++i)
                    {
                        const auto frame = voice.next(1.0);
                        sine.push_back(frame.sine); upper.push_back(frame.rich); full.push_back(frame.output);
                    }
                    const auto played = std::llround(hz / rate * 4294967296.0) * rate / 4294967296.0;
                    const auto fundamental = coefficient(sine, played, rate);
                    require(std::abs(std::abs(fundamental) - dropforge::BassVoice::sineGain) < 1.0e-5 * dropforge::BassVoice::sineGain,
                            "drive preserves the independently selected clean foundation level");
                    const auto reference = fundamental * butterworthResponse(false, hz, 5000.0, rate);
                    const auto weightDb = 20.0 * std::log10(std::abs(coefficient(full, hz, rate) / reference));
                    worstLowWeightDb = std::max(worstLowWeightDb, std::abs(weightDb));
                    require(std::abs(weightDb) < 1.0, "low fundamental weight stays within 1 dB of clean filtered foundation");
                    double leakagePower = 0.0;
                    for (auto partial = hz; partial < 80.0; partial += hz)
                        leakagePower += std::norm(coefficient(upper, partial, rate));
                    const auto leakageDb = 10.0 * std::log10(leakagePower / std::norm(fundamental));
                    worstLeakageDb = std::max(worstLeakageDb, leakageDb);
                    require(leakageDb < -20.0, "rich energy below 80 Hz stays 20 dB below sine at Split/drive boundaries");
                }
    dropforge::Engine engine;
    engine.setAnvilGrit(grit);
    engine.prepare(48000);
    engine.setBassParameters(80.0f, 1.0f);
    engine.noteOn(1, 45, 127);
    const auto first = render(engine, dropforge::Engine::latencySamples + 1, 1);
    require(std::all_of(first.begin(), first.end(), [](float sample) { return sample == 0.0f; }),
            "note-on and attack are aligned to exactly 32 samples");
    engine.setBassParameters(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    render(engine, 8192);
    engine.reset();
    require(render(engine, 512) == std::vector<float>(512, 0.0f), "reset clears alignment/FIR/filter/voice history");
    std::cout << "grit=" << (grit == dropforge::AnvilGrit::b ? 'b' : 'a') << " bass_automation_residual_max_step=" << maximumResidualStep
              << " low_weight_worst_abs_db=" << worstLowWeightDb << " upper_energy_below_80hz_dbc=" << worstLeakageDb
              << " protected_sine=bit_exact latency_samples=32\n";
}
void componentIsolation(dropforge::AnvilGrit candidate)
{
    double worstError = 0.0, parallelError = 0.0;
    for (const auto rate : {44100, 48000, 96000, 192000})
    {
        dropforge::BassVoice full, clean, grit, dry, unison;
        for (auto* voice : {&full, &clean, &grit, &dry, &unison}) { voice->setGrit(candidate); voice->prepare(rate); }
        for (int i = 0; i < rate; ++i)
        {
            if (i % (rate/4) == 0)
                for (auto* voice : {&full, &clean, &grit, &dry, &unison}) voice->start(i < rate/2 ? 55.0 : 82.406889, 100);
            if (i % (rate/4) == rate/5)
                for (auto* voice : {&full, &clean, &grit, &dry, &unison}) voice->stop();
            if (i == rate/3)
                for (auto* voice : {&full, &clean, &grit, &dry, &unison}) voice->setParameters(80.0, 1.0);
            const auto bend = std::exp2(0.1 * std::sin(2.0*pi*i/rate));
            const auto mixed = grit.next(bend, dropforge::BassComponent::grit).output;
            const auto parallel = 0.55 * dry.next(bend, dropforge::BassComponent::dryGrit).output
                                + 0.45 * unison.next(bend, dropforge::BassComponent::unisonGrit).output;
            parallelError = std::max(parallelError, std::abs(mixed - parallel));
            const auto sum = clean.next(bend, dropforge::BassComponent::clean).output + mixed;
            worstError = std::max(worstError, static_cast<double>(std::abs(sum - full.next(bend).output)));
        }
    }
    // 32-bit float: the parts rebuild the whole within 1e-6 (-120 dB).
    require(worstError < 1.0e-6, "clean plus grit reconstruct Bass through its final filter, releases and retriggers");
    require(parallelError < 1.0e-6, "independently driven dry and unison reconstruct parallel grit through bends/controls/releases");
    std::cout << "grit=" << (candidate == dropforge::AnvilGrit::b ? 'b' : 'a') << " bass_component_reconstruction_error=" << worstError << " parallel_grit_error=" << parallelError << '\n';
}
void harmonicGritTexture()
{
    double lowestRms = 1.0, worstTextureRatio = 0.0;
    for (const auto rate : {44100, 48000, 192000})
        for (const auto root : {55.0, 110.0})
            for (const auto amount : {0.0, 0.5, 1.0})
            {
                dropforge::BassExciter source; source.prepare(rate);
                dropforge::BassVoice full, body;
                for (auto* voice : {&full, &body})
                {
                    voice->prepare(rate); voice->setParameters(140, amount);
                    voice->reset(); voice->start(root, 127);
                }
                double bodyEnergy = 0.0, differenceEnergy = 0.0;
                std::vector<double> driven;
                driven.reserve(rate);
                for (int i = 0; i < rate * 3 / 2; ++i)
                {
                    const auto parts = source.nextParts(root, rate, amount);
                    const auto withTexture = full.next(1, dropforge::BassComponent::grit).output;
                    const auto bodyOnly = body.next(1, dropforge::BassComponent::grit, &parts.body).output;
                    require(std::isfinite(withTexture) && std::isfinite(bodyOnly), "harmonic grit remains finite");
                    if (i < rate / 2) continue;
                    bodyEnergy += bodyOnly * bodyOnly;
                    differenceEnergy += (withTexture - bodyOnly) * (withTexture - bodyOnly);
                    driven.push_back(withTexture);
                }
                const auto rms = std::sqrt(bodyEnergy / driven.size());
                const auto ratio = std::sqrt(differenceEnergy / bodyEnergy);
                lowestRms = std::min(lowestRms, rms);
                worstTextureRatio = std::max(worstTextureRatio, ratio);
                require(rms > 0.0005 && rms < 0.05, "main harmonic grit sustains independently of noise");
                require(ratio > 0.0 && ratio < 0.0316227766, "texture contribution remains below -30 dB relative to body after drive");
                const auto anchor = std::max(std::abs(coefficient(driven, 3 * root, rate)),
                                             std::abs(coefficient(driven, 5 * root, rate)));
                require(anchor > 0.3 * rms, "grit follows the played note through saturation-like overtones");
                source.reset(); dropforge::BassExciter replay; replay.prepare(rate);
                for (int i = 0; i < 4096; ++i)
                    require(source.next(root, rate, amount) == replay.next(root, rate, amount), "harmonic/texture excitation resets exactly");
            }
    // A high-root source contains only partials below the fade boundary.
    dropforge::BassExciter source; source.prepare(48000);
    std::vector<double> high;
    for (int i = 0; i < 48000; ++i) high.push_back(source.nextParts(5500, 48000, 1).body.dry);
    require(std::abs(coefficient(high, 16500, 48000)) > 0.1, "permitted high harmonic remains");
    require(std::abs(coefficient(high, 20500, 48000)) < 1.0e-8, "out-of-band fifth harmonic cannot fold into source");
    std::cout << "bass_harmonic_min_body_rms=" << lowestRms << " max_post_drive_texture_dbr="
              << 20.0 * std::log10(worstTextureRatio) << '\n';
}
void injectedDriveAliasEvidence()
{
    // Retain a controlled periodic drive probe: broadband chaotic energy in
    // arbitrary folded bins is intentional texture, not an alias measurement.
    for (const auto rate : {48000, 96000, 192000})
      for (const auto amount : {0.0, 1.0})
    {
        dropforge::BassVoice voice;
        voice.prepare(rate);
        voice.setParameters(140.0, amount);
        voice.reset();
        voice.start(5500.0, 127);
        auto probe = [&](int i)
        {
            const auto value = 0.4 * std::sin(2 * pi * 5500.0 * i / rate);
            const dropforge::BassExciter::Sources input{value,value};
            return voice.next(1.0, dropforge::BassComponent::all, &input).output;
        };
        for (int i = 0; i < rate / 2; ++i) probe(i);
        std::vector<double> audio;
        for (int i = 0; i < rate; ++i) audio.push_back(probe(i));
        const auto strongest = std::abs(coefficient(audio, 5500.0, rate));
        double worst = 0.0;
        std::vector<double> bins;
        for (int harmonic = 5; harmonic <= 40; ++harmonic)
        {
            auto hz = std::fmod(harmonic * 5500.0, rate);
            if (hz > rate / 2.0) hz = rate - hz;
            if (hz <= 0.0 || std::abs(hz / 5500.0 - std::round(hz / 5500.0)) < 1.0e-9
                || std::find(bins.begin(), bins.end(), hz) != bins.end()) continue;
            bins.push_back(hz);
            worst = std::max(worst, std::abs(coefficient(audio, hz, rate)) / strongest);
        }
        require(std::isfinite(worst) && strongest > 0.01, "actual high-note alias probe measures a finite sustained voice");
        std::cout << "injected_bass_drive_alias_probe_rate=" << rate << " note_hz=5500 amount=" << amount << " selected_folded_bins="
                  << bins.size() << " worst_selected_alias_dbc=" << 20.0 * std::log10(worst) << '\n';
    }
}
// Grit candidate B, the growl (SPEC section 4): its source is periodic, so
// aliasing is measured on the real voice: everything that is not a harmonic
// of the played pitch is alias or noise.
void growlCandidate()
{
    using dropforge::AnvilGrit;
    double worstAliasDb = -1000.0, worstTopDb = -1000.0;
    for (const auto rate : {44100, 48000, 96000})
        for (const auto hz : {41.203, 110.0, 261.626})
            for (const auto amount : {0.0, 1.0})
            {
                dropforge::BassVoice growl, harmonic;
                growl.setGrit(AnvilGrit::b);
                for (auto* voice : {&growl, &harmonic})
                {
                    voice->prepare(rate); voice->setParameters(140.0, amount);
                    voice->reset(); voice->start(hz, 127);
                }
                for (int i = 0; i < rate / 2; ++i)
                    require(growl.next(1.0).sine == harmonic.next(1.0).sine, "growl leaves the clean sine bit-exact");
                std::vector<double> upper;
                for (int i = 0; i < rate; ++i) upper.push_back(growl.next(1.0, dropforge::BassComponent::grit).output);
                const auto played = std::llround(hz / rate * 4294967296.0) * rate / 4294967296.0;
                // A Hann window: one second never holds a whole number of
                // cycles, and unwindowed, neighbouring partials would leak
                // into each other at about -26 dB and read as alias.
                double total = 0.0, harmonics = 0.0, top = 0.0, windowSum = 0.0, windowPower = 0.0;
                for (std::size_t i = 0; i < upper.size(); ++i)
                {
                    const auto w = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(upper.size()));
                    upper[i] *= w;
                    windowSum += w; windowPower += w * w;
                    total += upper[i] * upper[i];
                }
                total /= windowPower;
                // Partials above 12 kHz are 60 dB down after the 4 kHz low-pass;
                // leaving them out only makes the alias figure more pessimistic.
                for (int k = 1; k * played < std::min(12000.0, rate / 2.0); ++k)
                {
                    const auto power = partialPower(upper, k * played, rate, windowSum);
                    harmonics += power;
                    if (k * played > 8000.0) top += power;
                }
                const auto aliasDb = 10.0 * std::log10(std::max(total - harmonics, 1.0e-30) / total);
                const auto topDb = 10.0 * std::log10(std::max(top, 1.0e-30) / total);
                worstAliasDb = std::max(worstAliasDb, aliasDb);
                worstTopDb = std::max(worstTopDb, topDb);
                std::cout << "growl rate=" << rate << " hz=" << hz << " amount=" << amount
                          << " alias_db=" << aliasDb << " above_8k_db=" << topDb << '\n';
                require(aliasDb < -50.0, "growl energy off the harmonic series stays 50 dB down");
                require(topDb < -40.0, "growl stays a mid-bass sound: energy above 8 kHz 40 dB down");
            }
    // Level against grit A on a held E1 at the default drive, for the record.
    double levels[2] = {};
    for (const auto grit : {AnvilGrit::a, AnvilGrit::b})
    {
        dropforge::BassVoice voice;
        voice.setGrit(grit);
        voice.prepare(48000);
        voice.start(41.203, 127);
        double energy = 0.0;
        for (int i = 0; i < 96000; ++i)
        {
            const auto value = voice.next(1.0, dropforge::BassComponent::grit).output;
            if (i >= 48000) energy += value * value;
        }
        levels[grit == AnvilGrit::b ? 1 : 0] = 10.0 * std::log10(energy / 48000.0);
    }
    // Mono, block-size invariance and linear velocity through the engine.
    dropforge::Engine loud, quiet;
    for (auto* engine : {&loud, &quiet}) { engine->setAnvilGrit(AnvilGrit::b); engine->prepare(48000); }
    loud.noteOn(1, 40, 127); quiet.noteOn(1, 40, 64);
    auto reference = loud;
    require(render(loud, 4096, 1) == render(reference, 4096, 97), "growl is block-partition invariant");
    const auto a = render(loud, 48000), b = render(quiet, 4096 + 48000, 333);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(std::abs(b[i + 4096] - a[i] * (64.0f / 127.0f)) < 3.0e-8f, "growl velocity changes gain only");
    std::cout << "growl_worst_alias_db=" << worstAliasDb << " growl_above_8k_db=" << worstTopDb
              << " grit_a_rms_db=" << levels[0] << " growl_rms_db=" << levels[1] << '\n';
}
}
int main()
{
    filterResponses();
    branchMeasurements();
    audibleRouting();
    for (const auto grit : {dropforge::AnvilGrit::a, dropforge::AnvilGrit::b})
    {
        drivenBassTests(grit);
        componentIsolation(grit);
    }
    harmonicGritTexture();
    injectedDriveAliasEvidence();
    growlCandidate();
}
