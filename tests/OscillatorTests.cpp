#include "plugin/MidiInput.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
constexpr double pi = 3.14159265358979323846;

void require(bool condition, const char* description)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}

std::vector<float> render(dropforge::Engine& engine, int count)
{
    std::vector<float> left(static_cast<std::size_t>(count)), right(left.size());
    engine.process(left.data(), right.data(), count, dropforge::LayerMask::rhythm);

    for (auto value : left)
        require(std::isfinite(value) && std::abs(value) <= 0.641f
                && std::fpclassify(value) != FP_SUBNORMAL, "bounded finite normal output");
    for (auto value : right)
        require(std::isfinite(value) && std::abs(value) <= 0.641f
                && std::fpclassify(value) != FP_SUBNORMAL, "right channel bounded finite normal output");
    return left;
}

double energy(const std::vector<float>& audio)
{
    double sum = 0.0;
    for (auto sample : audio)
        sum += sample * static_cast<double>(sample);
    return sum / static_cast<double>(audio.size());
}

void requireSilence(dropforge::Engine& engine)
{
    render(engine, 2000); // Longer than both the release and transition at 192 kHz.
    require(energy(render(engine, 512)) == 0.0, "release must end in exact silence");
}

double targetPitch(dropforge::Engine& engine)
{
    render(engine, 2400); // Settle the same channel bend ramp as the audible path.
    // Unison has intentional sidebands; zero crossings no longer measure its
    // MIDI pitch. RhythmTests separately measures the synthesized spectral lines.
    for (const auto& target : engine.rhythmTargets())
        if (target.active) return target.frequency;
    require(false, "pitch diagnostic needs an active note");
    return 0.0;
}
void envelopeAndPitch()
{
    dropforge::Engine engine;
    engine.prepare(48000.0);
    engine.noteOn(1, 69, 127);
    const auto first = render(engine, 1);
    require(first[0] == 0.0f, "attack begins at zero");
    const auto pitch = targetPitch(engine);
    require(std::abs(pitch - 440.0) < 0.1, "A4 must remain 440 Hz");
    const auto held = energy(render(engine, 48000));
    require(held > 0.0005, "held note sustains");
    engine.pitchBend(2, 16383);
    require(std::abs(targetPitch(engine) - 440.0) < 0.1, "bend is channel-specific");
    engine.pitchBend(1, 16383);
    const auto upper = targetPitch(engine);
    require(std::abs(upper - 440.0 * std::exp2(2.0 / 12.0)) < 0.1, "upper bend reaches +2 semitones");
    engine.pitchBend(1, 0);
    const auto lower = targetPitch(engine);
    require(std::abs(lower - 440.0 * std::exp2(-2.0 / 12.0)) < 0.1, "lower bend reaches -2 semitones");
    engine.pitchBend(1, 8192);
    require(std::abs(targetPitch(engine) - 440.0) < 0.1, "bend returns to center");
    engine.noteOff(1, 69);
    const auto release = render(engine, 290 + dropforge::Engine::latencySamples);
    require(release.back() == 0.0f, "6 ms release reaches zero at 48 kHz");
    requireSilence(engine);

    engine.prepare(48000.0);
    engine.noteOn(1, 69, 64);
    render(engine, 2400);
    const auto quiet = energy(render(engine, 48000));
    require(std::abs(std::sqrt(quiet / held) - 64.0 / 127.0) < 0.001, "velocity is linear loudness gain");
    engine.noteOn(1, 69, 0);
    requireSilence(engine);
    for (const int note : {0, 127})
    {
        engine.noteOn(1, note, 127);
        const auto expected = 440.0 * std::exp2((note - 69.0) / 12.0);
        require(std::abs(targetPitch(engine) - expected) < 0.2, "boundary MIDI notes keep their exact pitch");
        engine.noteOff(1, note);
        requireSilence(engine);
    }
    std::cout << "target_pitch_hz=" << pitch << " bend_upper_hz=" << upper << " bend_lower_hz=" << lower
              << " held_rms=" << std::sqrt(held) << " velocity_ratio=" << std::sqrt(quiet / held) << '\n';
}

void midiBridge()
{
    dropforge::Engine engine;
    engine.prepare(48000.0);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127)), 73);
    dropforge::processMidiBlock(engine, audio, midi);
    for (int i = 0; i <= 73; ++i)
        require(audio.getSample(0, i) == 0.0f, "note-on is sample accurate and starts at zero");
    require(audio.getMagnitude(0, 74, 438) > 0.01f, "note sounds after its timestamp");
    midi.clear();
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 69), 29);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 400);
    dropforge::processMidiBlock(engine, audio, midi);
    for (int i = 318 + dropforge::Engine::latencySamples; i < 512; ++i)
        require(audio.getSample(0, i) == 0.0f, "CC64 cannot extend the 6 ms release");

    for (const int controller : {120, 123})
    {
        engine.noteOn(1, 60, 127);
        engine.noteOn(2, 64, 127);
        render(engine, 512);
        const std::array<juce::uint8, 3> panic{0xb0, static_cast<juce::uint8>(controller), 0};
        dropforge::dispatchMidi(engine, panic.data(), 3);
        render(engine, 512);
        require(energy(render(engine, 512)) > 0.0001, "channel panic leaves other channels playing");
        const juce::uint8 reset = 0xff;
        dropforge::dispatchMidi(engine, &reset, 1);
        requireSilence(engine);
    }
    const std::array<juce::uint8, 3> on{0x9f, 69, 127}, off{0x9f, 69, 0};
    dropforge::dispatchMidi(engine, on.data(), 3);
    render(engine, 512);
    dropforge::dispatchMidi(engine, off.data(), 3);
    requireSilence(engine);
    // Large SysEx/short/malformed messages are ignored without MidiMessage copies.
    const std::array<juce::uint8, 4> sysex{0xf0, 1, 2, 0xf7};
    const std::array<juce::uint8, 3> invalid{0x90, 255, 127};
    dropforge::dispatchMidi(engine, sysex.data(), 4);
    dropforge::dispatchMidi(engine, invalid.data(), 3);
    dropforge::dispatchMidi(engine, on.data(), 2);
    requireSilence(engine);

    // Zero-sized host callbacks still apply MIDI without accessing audio pointers.
    juce::AudioBuffer<float> empty(2, 0);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127)), 0);
    dropforge::processMidiBlock(engine, empty, midi);
    require(energy(render(engine, 512)) > 0.0001, "zero-sized callback applies MIDI");
    engine.noteOff(1, 69);
    requireSilence(engine);

    engine.noteOn(1, 69, 127);
    const std::array<juce::uint8, 3> upperBend{0xe0, 127, 127};
    dropforge::dispatchMidi(engine, upperBend.data(), 3);
    require(std::abs(targetPitch(engine) - 440.0 * std::exp2(2.0 / 12.0)) < 0.1, "raw wheel bytes decode correctly");
    const std::array<juce::uint8, 3> controllersReset{0xb0, 121, 0};
    dropforge::dispatchMidi(engine, controllersReset.data(), 3);
    require(std::abs(targetPitch(engine) - 440.0) < 0.1, "reset controllers centers bend");
    engine.noteOff(1, 69);
    requireSilence(engine);
}

void polyphonyAndRetriggers()
{
    dropforge::Engine engine;
    engine.prepare(48000.0);
    engine.noteOn(1, 60, 127);
    engine.noteOn(1, 64, 127);
    render(engine, 512);
    engine.noteOff(1, 60);
    render(engine, 512);
    require(energy(render(engine, 512)) > 0.0001, "releasing one chord note preserves others");
    engine.noteOff(1, 64);
    requireSilence(engine);
    for (int note = 60; note < 68; ++note)
        engine.noteOn(1, note, 127);
    render(engine, 512);
    engine.noteOn(1, 80, 127); // Ninth note steals oldest held note (60).
    render(engine, 512);
    engine.noteOff(1, 60); // Stale release must not stop the replacement note.
    for (int note = 61; note < 68; ++note)
        engine.noteOff(1, note);
    render(engine, 512);
    require(energy(render(engine, 512)) > 0.0001, "stale stolen-key release preserves its replacement");
    engine.noteOff(1, 80);
    requireSilence(engine);

    for (int note = 60; note < 68; ++note)
        engine.noteOn(1, note, 127);
    render(engine, 512);
    engine.noteOff(1, 67);
    engine.noteOn(1, 80, 127); // Released 67 must be preferred over held 60.
    for (int note = 61; note < 67; ++note)
        engine.noteOff(1, note);
    engine.noteOff(1, 80);
    render(engine, 512);
    require(energy(render(engine, 512)) > 0.0001, "released voice is stolen before oldest held key");
    engine.noteOff(1, 60);
    requireSilence(engine);

    engine.prepare(48000.0);
    for (int channel = 1; channel <= 8; ++channel)
        engine.noteOn(channel, 33, 127);
    const auto fullVelocity = render(engine, 48000);
    double peak = 0.0;
    for (auto sample : fullVelocity)
        peak = std::max(peak, std::abs(static_cast<double>(sample)));
    std::cout << "eight_note_full_velocity_peak=" << peak << '\n';
    require(peak > 0.1 && peak < 0.641, "eight aligned full-velocity notes retain headroom");

    engine.prepare(48000.0);
    engine.noteOn(1, 33, 127);
    auto previous = render(engine, 512).back();
    double maximumStep = 0.0;
    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        engine.noteOn(1, 33, 127);
        previous = render(engine, dropforge::Engine::latencySamples).back();
        const auto block = render(engine, 3);
        maximumStep = std::max(maximumStep, std::abs(static_cast<double>(block.front() - previous)));
        previous = block.back();
    }
    // The post-sum FIR/IIR stages continue evolving through the retrigger;
    // equality with the prior output sample no longer describes continuity.
    require(maximumStep < 0.01, "filtered rapid retriggers retain a bounded boundary step");
    engine.noteOff(1, 33);
    requireSilence(engine);
    std::cout << "rapid_retrigger_boundary_step=" << maximumStep << '\n';
}

void ratesAndBlocks()
{
    double peak = 0.0;
    for (const double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    {
        dropforge::Engine engine;
        engine.prepare(rate);
        std::array<float, 1026> left{}, right{};
        for (int channel = 1; channel <= 16; ++channel)
            for (int note = 0; note < 128; ++note)
            {
                engine.noteOn(channel, note, 127);
                engine.pitchBend(channel, note % 2 == 0 ? 0 : 16383);
                for (const int size : {1, 17, 0, 512, 1024, 3})
                {
                    left.fill(2.0f);
                    right.fill(-2.0f);
                    engine.process(left.data() + 1, right.data() + 1, size, dropforge::LayerMask::rhythm);
                    require(left[0] == 2.0f && right[0] == -2.0f
                            && left[static_cast<std::size_t>(size + 1)] == 2.0f
                            && right[static_cast<std::size_t>(size + 1)] == -2.0f, "block boundary guards survive");
                    for (int sample = 1; sample <= size; ++sample)
                    {
                        const auto value = left[static_cast<std::size_t>(sample)];
                        require(std::isfinite(value) && std::abs(value) <= 0.641f
                                && std::fpclassify(value) != FP_SUBNORMAL
                                && std::isfinite(right[static_cast<std::size_t>(sample)])
                                && std::abs(right[static_cast<std::size_t>(sample)]) <= 0.641f
                                && std::fpclassify(right[static_cast<std::size_t>(sample)]) != FP_SUBNORMAL, "stereo matrix stays bounded/finite");
                        peak = std::max(peak, std::abs(static_cast<double>(value)));
                    }
                }
                engine.noteOff(channel, note);
            }
        for (int channel = 1; channel <= 16; ++channel)
            engine.allNotesOff(channel);
        requireSilence(engine);
        engine.prepare(rate);
        requireSilence(engine);
    }
    std::cout << "all_notes_rates_blocks_peak=" << peak << '\n';
}

void blockInvariance()
{
    dropforge::Engine whole, split;
    whole.prepare(48000.0);
    split.prepare(48000.0);
    whole.noteOn(1, 69, 127);
    split.noteOn(1, 69, 127);
    const auto expected = render(whole, 4096);
    std::vector<float> actual;
    for (int offset = 0; offset < 4096;)
    {
        const auto count = std::min(4096 - offset, offset % 29 + 1);
        const auto part = render(split, count);
        actual.insert(actual.end(), part.begin(), part.end());
        offset += count;
    }
    require(actual == expected, "output is bit-identical across block partitions");
}

void bandLimiting()
{
    constexpr int count = 4800;
    dsp_core::PolyBlepSaw oscillator;
    std::vector<double> corrected, naive;
    for (int i = 0; i < count; ++i)
    {
        corrected.push_back(oscillator.next(11000.0, 48000.0));
        const auto phase = std::fmod(i * 11000.0 / 48000.0, 1.0);
        naive.push_back(2.0 * phase - 1.0);
    }
    auto aliasEnergy = [=](const std::vector<double>& audio)
    {
        double sum = 0.0;
        // Integer-period input: allowed harmonics only at bins 1100 and 2200.
        for (int bin = 1; bin <= count / 2; ++bin)
        {
            if (bin == 1100 || bin == 2200)
                continue;
            std::complex<double> coefficient{};
            for (int sample = 0; sample < count; ++sample)
                coefficient += audio[static_cast<std::size_t>(sample)]
                    * std::polar(1.0, -2.0 * pi * bin * sample / count);
            sum += std::norm(coefficient);
        }
        return sum;
    };
    const auto reduction = 10.0 * std::log10(aliasEnergy(naive) / aliasEnergy(corrected));
    require(reduction > 10.0, "PolyBLEP must reduce high-note alias energy versus naive saw");
    std::cout << "11khz_alias_reduction_db=" << reduction << '\n';
}
}

int main()
{
    envelopeAndPitch();
    midiBridge();
    polyphonyAndRetriggers();
    ratesAndBlocks();
    blockInvariance();
    bandLimiting();
    return 0;
}
