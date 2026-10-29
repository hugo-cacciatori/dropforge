#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <dsp_core/FixedEnvelope.h>
#include <dsp_core/LinearRamp.h>
#include <dsp_core/MultibandDynamics.h>
#include <dsp_core/Svf.h>
#include "FurnaceDesign.h"
#include "NoteRouter.h"
#include "RhythmBus.h"
#include "RhythmOscillators.h"

namespace dropforge
{
// One complete Furnace (SPEC section 4) for one mode: eight stereo voices,
// the per-side drive and tone buses, multiband dynamics, mono-low width and
// the chop gate. The engine keeps two so a mode change can crossfade from
// the old Furnace to the new one while notes are held (SPEC section 12).
class FurnaceLayer
{
public:
    static constexpr std::size_t voiceCount = 8;
    static constexpr int controlBlock = 32;
    static constexpr double monoBelowHz = 150.0;
    using Block = std::array<double, controlBlock>;
    struct Target { int channel, note; double frequency; bool held, active; };

    void setDesign(const FurnaceDesign& chosen) noexcept { design = chosen; }
    const FurnaceDesign& currentDesign() const noexcept { return design; }

    void prepare(double sampleRate, int attack, int release, int transition, bool anvilPresent) noexcept
    {
        rate = sampleRate;
        attackSamples = attack; releaseSamples = release; transitionSamples = transition;
        for (auto& bus : buses)
        {
            bus.setVoicing(design.drivePushDb, design.contourDb, design.airDb);
            bus.setAnvilPresent(anvilPresent);
            bus.prepare(rate);
        }
        // Mono low end: the side signal loses everything below about 150 Hz.
        sideHighpass.set(dsp_core::Svf::Type::highpass, monoBelowHz, rate);
        dynamics.prepare(design.dynamics, rate);
        dynamics.setDepth(dynamicsDepth);
        clear();
    }
    // Silence: every voice, filter and dynamics history, as when first prepared.
    void clear() noexcept
    {
        for (auto& voice : voices)
        {
            voice = Voice{};
            voice.envelope.prepare(attackSamples, releaseSamples);
        }
        for (auto& bus : buses) bus.reset();
        dynamics.reset();
        sideHighpass.reset();
        level.reset(1.0); levelTarget = 1.0; wasActive = false;
    }
    // Diagnostic drive amount (fixed in the product) and Tight. Amount 0 makes
    // the whole chain linear, its dynamics included, for tests.
    void setParameters(float drive, float tight) noexcept
    {
        for (auto& bus : buses) bus.setParameters(drive, tight);
        dynamicsDepth = drive > 0.0f ? 1.0f : 0.0f;
        dynamics.setDepth(dynamicsDepth);
    }
    void setAnvilPresent(bool present, bool restart) noexcept
    {
        for (auto& bus : buses)
        {
            bus.setAnvilPresent(present);
            // A silent, cleared Furnace starts at its new separation cut, as a
            // freshly played chord would, instead of gliding in from the old one.
            if (restart) bus.reset();
        }
    }

    void start(int channel, int note, int velocity, double bendRatio, std::uint64_t age) noexcept
    {
        auto& voice = voices[NoteRouter::pickVoice(voices, channel, note)];
        voice.transitionStart = voice.lastOutput;
        voice.transitionRemaining = transitionSamples;
        voice.oscillators.setDesign(design.voice);
        voice.oscillators.reset(channel, note);
        voice.envelope.start();
        voice.frequency = NoteRouter::frequency(note);
        // Pitch now; the voice's control updates then keep it following the bend.
        voice.oscillators.setPitch(voice.frequency * bendRatio, rate);
        voice.controlCountdown = controlBlock;
        voice.velocity = std::min(velocity, 127) / 127.0;
        voice.channel = channel;
        voice.note = note;
        voice.keyDown = true;
        voice.age = age;
    }
    // Releases matching held keys; channel-only when note < 0.
    void release(int channel, int note) noexcept
    {
        for (auto& voice : voices)
            if (voice.keyDown && voice.channel == channel && (note < 0 || voice.note == note))
            {
                voice.keyDown = false;
                voice.envelope.stop();
            }
    }
    void releaseAll() noexcept
    {
        for (auto& voice : voices)
            if (voice.keyDown) { voice.keyDown = false; voice.envelope.stop(); }
    }
    bool sounding() const noexcept
    {
        return std::any_of(voices.begin(), voices.end(), [](const Voice& v) { return v.active(); });
    }

    // Renders count samples of the finished Furnace into left/right.
    void render(int count, const std::array<Block, 16>& bendRatio, Block& left, Block& right) noexcept
    {
        const auto samples = static_cast<std::size_t>(count);
        std::fill_n(sumLeft.begin(), samples, 0.0f);
        std::fill_n(sumRight.begin(), samples, 0.0f);
        std::fill_n(strongestEnvelope.begin(), samples, 0.0);
        std::fill_n(strongestVelocity.begin(), samples, 0.0);
        // Voice by voice over the block, in 32-bit float; the sums add voices in
        // bank order, so results never depend on which voices are free.
        for (auto& voice : voices)
        {
            const auto& ratio = bendRatio[static_cast<std::size_t>(std::max(voice.channel, 1) - 1)];
            for (std::size_t i = 0; i < samples; ++i)
            {
                if (!voice.active())
                {
                    voice.lastOutput = {};
                    continue;
                }
                // Pitch follows the bend at control rate: a 2 ms bend ramp moves
                // in steps of 0.7 ms or less, too fine to hear as stepping.
                if (voice.controlCountdown == 0)
                {
                    voice.oscillators.setPitch(voice.frequency * ratio[i], rate);
                    voice.controlCountdown = controlBlock;
                }
                --voice.controlCountdown;
                auto output = voice.oscillators.next();
                const auto envelope = voice.envelope.next();
                strongestVelocity[i] = std::max(strongestVelocity[i], voice.velocity);
                strongestEnvelope[i] = std::max(strongestEnvelope[i], envelope);
                const auto gain = static_cast<float>(envelope * voice.velocity);
                output.left *= gain;
                output.right *= gain;
                if (voice.transitionRemaining > 0)
                {
                    // A convex blend from the last sample bounds even rapid steals,
                    // without resetting a live waveform abruptly or adding a clipper.
                    const auto oldWeight = static_cast<double>(voice.transitionRemaining--) / transitionSamples;
                    strongestEnvelope[i] = std::max(strongestEnvelope[i], oldWeight);
                    const auto weight = static_cast<float>(oldWeight);
                    output.left = weight * voice.transitionStart.left + (1.0f - weight) * output.left;
                    output.right = weight * voice.transitionStart.right + (1.0f - weight) * output.right;
                }
                voice.lastOutput = output;
                sumLeft[i] += output.left;
                sumRight[i] += output.right;
            }
        }
        for (std::size_t i = 0; i < samples; ++i)
        {
            // Factor the strongest note's velocity out of the nonlinear drive and
            // restore it afterwards: single-note velocity changes loudness only.
            // Other chord notes retain their relative weights in the summed bus.
            // Smooth reference changes when a louder/quieter note joins or leaves.
            const auto velocity = strongestVelocity[i];
            if (velocity > 0.0)
            {
                if (!wasActive) level.reset(velocity);
                else if (velocity != levelTarget) level.setTarget(velocity, transitionSamples);
                levelTarget = velocity;
            }
            wasActive = velocity > 0.0;
            const auto current = level.next();
            const auto envelope = static_cast<float>(strongestEnvelope[i]);
            const auto restore = static_cast<float>(current), normalize = static_cast<float>(1.0 / current);
            auto drivenLeft = buses[0].drive(sumLeft[i] * normalize, envelope, restore);
            auto drivenRight = buses[1].drive(sumRight[i] * normalize, envelope, restore);
            // Dense and even: the multiband dynamics see the velocity-normalized
            // wall, so velocity still changes loudness only.
            dynamics.process(drivenLeft, drivenRight);
            const auto processedLeft = buses[0].tone(drivenLeft);
            const auto processedRight = buses[1].tone(drivenRight);
            // Width still comes from independent synthesis. This final bounded
            // blend reserves common energy after nonlinear processing as well.
            // Mono below about 150 Hz: chords stay solid on a club system or a
            // phone speaker, and the side signal never fights Anvil.
            const auto mid = 0.5f * (processedLeft + processedRight);
            const auto side = sideHighpass.next(0.5f * (processedLeft - processedRight)) * static_cast<float>(design.sideGain);
            // Both sides share one envelope and velocity, so one gain gates both,
            // after every filter: the chop stays dead-stop hard.
            const auto gain = buses[0].gain() * design.levelGain;
            left[i] = gain * (mid + side);
            right[i] = gain * (mid - side);
        }
    }

    std::array<Target, voiceCount> targets(const std::array<dsp_core::LinearRamp, 16>& bend) const noexcept
    {
        std::array<Target, voiceCount> result{};
        for (std::size_t i = 0; i < voices.size(); ++i)
        {
            const auto& v = voices[i];
            result[i] = {v.channel, v.note, v.channel > 0 ? v.frequency * bend[static_cast<std::size_t>(v.channel - 1)].value() : 0.0,
                         v.keyDown, v.active()};
        }
        return result;
    }
    // Multiband gain per band (0 low, 1 mid, 2 high), in dB.
    float bandGainDb(int band) const noexcept { return dynamics.bandGainDb(band); }

private:
    struct Voice
    {
        RhythmOscillators oscillators;
        dsp_core::FixedEnvelope envelope;
        double frequency = 0.0, velocity = 0.0;
        RhythmOscillators::Stereo lastOutput{}, transitionStart{};
        int channel = 0, note = -1, transitionRemaining = 0;
        // Each voice's own control clock starts at its note-on, so a note
        // sounds the same whenever it starts and whatever the host block size.
        int controlCountdown = 0;
        std::uint64_t age = 0;
        bool keyDown = false;
        bool active() const noexcept { return envelope.active() || transitionRemaining > 0; }
    };
    FurnaceDesign design = FurnaceDesign::candidateA();
    std::array<Voice, voiceCount> voices{};
    std::array<RhythmBus, 2> buses;
    dsp_core::LinearRamp level;
    double levelTarget = 1.0;
    bool wasActive = false;
    dsp_core::Svf sideHighpass;
    dsp_core::MultibandDynamics dynamics;
    float dynamicsDepth = 1.0f;
    double rate = 44100.0;
    int attackSamples = 45, releaseSamples = 265, transitionSamples = 89;
    Block strongestEnvelope{}, strongestVelocity{};
    std::array<float, controlBlock> sumLeft{}, sumRight{};
};
}
