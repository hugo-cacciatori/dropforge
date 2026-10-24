#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <dsp_core/FixedEnvelope.h>
#include <dsp_core/PolyBlepSaw.h>
#include "BassNotes.h"
#include "BassVoice.h"
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

// Eight stereo rhythm notes plus monophonic bass. Explicit layer enables enter
// mode-aware routing; older diagnostic callers retain their locked-bass baseline.
// MIDI decoding stays in plugin; this interface has no JUCE/GUI dependency.
class Engine
{
public:
    static constexpr std::size_t rhythmVoiceCount = 8;
    static constexpr double releaseSeconds = 0.006;
    static constexpr int latencySamples = BassVoice::latencySamples;
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setLayersEnabled(bool bass, bool rhythm) noexcept;
    void setLayerLevels(float bassDb, float rhythmDb) noexcept;
    void setBassParameters(float splitHz, float destruction) noexcept { bassVoice.setParameters(splitHz, destruction); }
    void setRhythmParameters(float destruction, float tight = 0.5f) noexcept
    {
        for (auto& bus : rhythmBus) bus.setParameters(destruction, tight);
    }
    void noteOn(int channel, int note, int velocity) noexcept;
    void noteOff(int channel, int note) noexcept;
    void allNotesOff(int channel) noexcept;
    void pitchBend(int channel, int wheel) noexcept;
    BassNotes::Target bassTarget() const noexcept { return bassEnabled ? bassNotes.target() : BassNotes::Target{}; }
    struct RhythmTarget { int channel, note; double frequency; bool held, active; };
    std::array<RhythmTarget, rhythmVoiceCount> rhythmTargets() const noexcept;
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
    bool bassEnabled = true, rhythmEnabled = true, routingConfigured = false;
    bool bassSuspended = false, rhythmSuspended = false;
    dsp_core::LinearRamp bassFade, rhythmFade, bassBendOffset;
    dsp_core::LinearRamp bassMixLevel, rhythmMixLevel;
    double bassMixTarget = 1.0, rhythmMixTarget = 1.0;
    static_assert(RhythmBus::latencySamples == latencySamples, "layer latency must agree");
    int bassChannel = 1; // Retain selected bend during the note-off release.
    BassNotes bassNotes;
    void updateBass(BassNotes::Change change) noexcept;
    void startRhythm(int channel, int note, int velocity) noexcept;
    void clearRhythm() noexcept;
    RhythmOscillators::Stereo nextRhythm(const std::array<double, 16>& ratios) noexcept;
    std::array<dsp_core::LinearRamp, 16> bend{};
    double rate = 44100.0;
    int attackSamples = 45, releaseSamples = 265, transitionSamples = 89, fadeSamples = 221;
    std::uint64_t serial = 0;
};
}
