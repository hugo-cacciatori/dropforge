#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <dsp_core/FixedEnvelope.h>
#include <dsp_core/MultibandDynamics.h>
#include <dsp_core/Svf.h>
#include "BassVoice.h"
#include "NoteRouter.h"
#include "RhythmOscillators.h"
#include "RhythmBus.h"

namespace dropforge
{
// Diagnostic contribution masks, not user-facing routing/enables. All synthesis
// and MIDI state advances even when a contribution is omitted from the output.
enum class LayerMask : unsigned { none = 0, bass = 1, rhythm = 2, both = 3 };
struct LayerTaps
{
    // Optional, caller-owned non-aliasing buffers of numSamples floats each.
    // These observe both contributions before the mask and master output gain.
    float* bassLeft = nullptr;
    float* bassRight = nullptr;
    float* rhythmLeft = nullptr;
    float* rhythmRight = nullptr;
};

// Eight stereo Furnace voices, the mono Anvil voice and Blade's note slot
// (its voice arrives in M2), fed by one NoteRouter. Audio runs in control
// blocks of up to 32 samples, layer by layer, with per-sample ramps where they
// are audible. MIDI decoding stays in plugin; no JUCE/GUI dependency here.
class Engine
{
public:
    static constexpr std::size_t rhythmVoiceCount = 8;
    static constexpr int controlBlock = 32;
    static constexpr double releaseSeconds = 0.006;
    static constexpr int latencySamples = BassVoice::latencySamples;
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setLayersEnabled(bool bass, bool rhythm) noexcept;
    void setLayerLevels(float bassDb, float rhythmDb) noexcept;
    void setBassParameters(float splitHz, float destruction) noexcept { bassVoice.setParameters(splitHz, destruction); }
    // Diagnostic drive amount (fixed in the product) and Tight. Amount 0 makes
    // the whole Furnace chain linear, its dynamics included, for tests.
    void setRhythmParameters(float destruction, float tight = 0.5f) noexcept
    {
        for (auto& bus : rhythmBus) bus.setParameters(destruction, tight);
        furnaceDynamicsDepth = destruction > 0.0f ? 1.0f : 0.0f;
        furnaceDynamics.setDepth(furnaceDynamicsDepth);
    }
    void noteOn(int channel, int note, int velocity) noexcept;
    void noteOff(int channel, int note) noexcept;
    void allNotesOff(int channel) noexcept;
    void pitchBend(int channel, int wheel) noexcept;
    struct NoteTarget { int channel = 0, note = -1, velocity = 0; double frequency = 0.0; };
    // Anvil's selected key and its octave-down pitch; empty while Anvil is off.
    NoteTarget bassTarget() const noexcept;
    // Blade's selected key: the highest held key, tracked whether or not it sounds.
    NoteTarget leadTarget() const noexcept;
    struct RhythmTarget { int channel, note; double frequency; bool held, active; };
    std::array<RhythmTarget, rhythmVoiceCount> rhythmTargets() const noexcept;
    // Furnace's multiband gain per band (0 low, 1 mid, 2 high), in dB, for
    // tests and meters.
    float furnaceBandGainDb(int band) const noexcept { return furnaceDynamics.bandGainDb(band); }
    void process(float* left, float* right, int numSamples,
                 LayerMask outputMask = LayerMask::both, LayerTaps taps = {},
                 BassComponent component = BassComponent::all) noexcept;

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
    std::array<Voice, rhythmVoiceCount> voices{};
    BassVoice bassVoice;
    std::array<RhythmBus, 2> rhythmBus;
    dsp_core::LinearRamp rhythmLevel;
    double rhythmLevelTarget = 1.0;
    bool rhythmWasActive = false;
    bool bassEnabled = true, rhythmEnabled = true;
    bool bassSuspended = false, rhythmSuspended = false;
    static constexpr double monoBelowHz = 150.0;
    dsp_core::Svf sideHighpass;
    dsp_core::MultibandDynamics furnaceDynamics;
    float furnaceDynamicsDepth = 1.0f;
    // Furnace A's starting dynamics (SPEC section 4), set from the measured
    // post-drive band peaks of typical voicings (about -14 dB low, -9 dB mid,
    // -13 dB high on an E power chord); tuned by listening.
    //   low:  held steady on chords, a gentle lift (at most 6 dB) for high notes,
    //         so it never booms under Anvil;
    //   mid:  about 3 dB of compression with 2 dB make-up: the dense core;
    //   high: lifted about 3 dB toward the top (at most 9 dB): the shimmer.
    static dsp_core::MultibandDynamics::Settings furnaceDynamicsSettings() noexcept
    {
        dsp_core::MultibandDynamics::Settings settings;
        //            down dB, ratio, up dB, ratio, max lift, output dB
        settings.band = {{{-12.0f, 4.0f, -22.0f, 2.0f, 6.0f, 0.0f},
                          {-14.0f, 3.0f, -20.0f, 3.0f, 12.0f, 2.0f},
                          {-4.0f, 4.0f, -8.0f, 3.0f, 9.0f, 0.0f}}};
        return settings;
    }
    dsp_core::LinearRamp bassFade, rhythmFade, bassBendOffset;
    dsp_core::LinearRamp bassMixLevel, rhythmMixLevel;
    double bassMixTarget = 1.0, rhythmMixTarget = 1.0;
    static_assert(RhythmBus::latencySamples == latencySamples, "layer latency must agree");
    int bassChannel = 1; // Retain selected bend during the note-off release.
    NoteRouter router;
    void updateBass(NoteRouter::Change change) noexcept;
    void startRhythm(int channel, int note, int velocity) noexcept;
    void clearRhythm() noexcept;
    void processBlock(float* left, float* right, int count, LayerMask outputMask, LayerTaps taps,
                      BassComponent component) noexcept;
    void renderRhythm(int count) noexcept;
    std::array<dsp_core::LinearRamp, 16> bend{};
    double rate = 44100.0;
    int attackSamples = 45, releaseSamples = 265, transitionSamples = 89, fadeSamples = 221;
    std::uint64_t serial = 0;

    // Per-block scratch, preallocated: bend ratios per channel, layer gains,
    // and the Furnace sum with its strongest envelope and velocity.
    using Block = std::array<double, controlBlock>;
    std::array<Block, 16> bendRatio{};
    Block rhythmGain{}, bassGain{}, rhythmLeft{}, rhythmRight{};
    Block strongestEnvelope{}, strongestVelocity{};
    std::array<float, controlBlock> sumLeft{}, sumRight{};
};
}
