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
#include "Impact.h"
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
    // Drop (SPEC section 3): every voice an octave down, in unison with
    // Anvil. The separation cut opens to 40 Hz (as without Anvil) so the
    // dropped roots really sound with Anvil; the voicing digs the low mids
    // 2 dB deeper and lifts the contour 1.5 dB so dropped chords stay
    // readable. Takes effect at prepare().
    void setDrop(bool on) noexcept { drop = on; }
    static constexpr double dropLowMidDb = -2.0, dropContourDb = 1.5, dropLowBoostDb = 7.0;
    const FurnaceDesign& currentDesign() const noexcept { return design; }

    void prepare(double sampleRate, int attack, int release, int transition, bool anvilPresent) noexcept
    {
        rate = sampleRate;
        attackSamples = attack; releaseSamples = release; transitionSamples = transition;
        for (auto& bus : buses)
        {
            bus.setVoicing(design.drivePushDb, design.contourDb + (drop ? dropContourDb : 0.0), design.airDb,
                           design.lowMidDb + (drop ? dropLowMidDb : 0.0));
            bus.setExtremes(design.foldAmount, design.hashHz);
            bus.setTightScale(drop ? 0.5 : 1.0);
            bus.setLowBoost(drop ? dropLowBoostDb : 0.0);
            bus.setAnvilPresent(anvilPresent && !drop);
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
            voice.impact.prepare(rate);
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
            bus.setAnvilPresent(present && !drop);
            // A silent, cleared Furnace starts at its new separation cut, as a
            // freshly played chord would, instead of gliding in from the old one.
            if (restart) bus.reset();
        }
    }

    // glideSamples > 0 (Glide on, SPEC section 3): the new note takes over the
    // releasing voice nearest in pitch and slides from it without a new
    // attack, so a chord change glides note for note; with no releasing voice
    // it starts fresh, sliding from the nearest sounding pitch. accent > 0
    // (Impact, SPEC section 3) accents a fresh attack; a legato takeover has none.
    void start(int channel, int note, int velocity, double bendRatio, std::uint64_t age, int glideSamples = 0,
               int accent = 0) noexcept
    {
        if (glideSamples > 0)
        {
            const auto target = NoteRouter::frequency(note) * (drop ? 0.5 : 1.0);
            Voice* releasing = nullptr;
            Voice* nearest = nullptr;
            const auto distance = [target](const Voice& v) noexcept { return std::abs(std::log2(v.pitch() / target)); };
            for (auto& v : voices)
            {
                if (!v.active() || v.frequency <= 0.0 || (v.channel == channel && v.note == note)) continue;
                if (!v.keyDown && (releasing == nullptr || distance(v) < distance(*releasing))) releasing = &v;
                if (nearest == nullptr || distance(v) < distance(*nearest)) nearest = &v;
            }
            if (releasing != nullptr)
            {
                releasing->slideTo(target, glideSamples);
                releasing->envelope.resume();
                releasing->velocity = std::min(velocity, 127) / 127.0;
                releasing->channel = channel;
                releasing->note = note;
                releasing->keyDown = true;
                releasing->age = age;
                return;
            }
            const auto from = nearest != nullptr ? nearest->pitch() : 0.0;
            startVoice(channel, note, velocity, bendRatio, age, accent);
            if (from > 0.0)
            {
                auto& fresh = voices[NoteRouter::pickVoice(voices, channel, note)];
                fresh.frequency = from;
                fresh.slideTo(target, glideSamples);
                fresh.oscillators.setPitch(from * bendRatio, rate);
            }
            return;
        }
        startVoice(channel, note, velocity, bendRatio, age, accent);
    }
    void startVoice(int channel, int note, int velocity, double bendRatio, std::uint64_t age, int accent = 0) noexcept
    {
        auto& voice = voices[NoteRouter::pickVoice(voices, channel, note)];
        voice.impact.strike(accent);
        voice.sliding = false;
        voice.transitionStart = voice.lastOutput;
        voice.transitionRemaining = transitionSamples;
        voice.oscillators.setDesign(design.voice);
        voice.oscillators.reset(channel, note);
        voice.envelope.start();
        voice.frequency = NoteRouter::frequency(note) * (drop ? 0.5 : 1.0);
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
        std::fill_n(strongestAccent.begin(), samples, 0.0f);
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
                // A glide advances every sample and retunes at control rate.
                if (voice.sliding) voice.advanceSlide();
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
                if (voice.impact.active()) strongestAccent[i] = std::max(strongestAccent[i], voice.impact.next());
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
            auto restore = static_cast<float>(current);
            const auto normalize = static_cast<float>(1.0 / current);
            auto inputLeft = sumLeft[i] * normalize, inputRight = sumRight[i] * normalize;
            // Impact: the newest strike drives the wall harder for a moment, and
            // its punch joins the restored level, which the bus delays to land
            // on the driven attack and applies after the dynamics, unsquashed.
            if (const auto accent = strongestAccent[i]; accent > 0.0f)
            {
                const auto boost = 1.0f + ImpactAccent::driveBoost * accent;
                inputLeft *= boost; inputRight *= boost;
                restore *= 1.0f + ImpactAccent::punchBoost * accent;
            }
            auto drivenLeft = buses[0].drive(inputLeft, envelope, restore);
            auto drivenRight = buses[1].drive(inputRight, envelope, restore);
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
        ImpactAccent impact;
        bool active() const noexcept { return envelope.active() || transitionRemaining > 0; }
        // Glide: frequency follows the slide, landing exactly on the target.
        dsp_core::LinearRamp slide;
        double slideTarget = 0.0;
        bool sliding = false;
        double pitch() const noexcept { return frequency; }
        void slideTo(double hz, int samples) noexcept
        {
            slide.reset(std::log2(frequency));
            slide.setTarget(std::log2(hz), samples);
            slideTarget = hz;
            sliding = true;
        }
        void advanceSlide() noexcept
        {
            slide.next();
            frequency = slide.moving() ? std::exp2(slide.value()) : slideTarget;
            sliding = slide.moving();
        }
    };
    FurnaceDesign design = FurnaceDesign::candidateA();
    bool drop = false;
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
    std::array<float, controlBlock> sumLeft{}, sumRight{}, strongestAccent{};
};
}
