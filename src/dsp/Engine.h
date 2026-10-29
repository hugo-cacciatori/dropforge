#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <dsp_core/FixedEnvelope.h>
#include <dsp_core/MultibandDynamics.h>
#include <dsp_core/Svf.h>
#include "BassVoice.h"
#include "FurnaceDesign.h"
#include "FurnaceLayer.h"
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
    static constexpr std::size_t rhythmVoiceCount = FurnaceLayer::voiceCount;
    // A mode change crossfades from the old voice to the new one over 30 ms
    // with equal power, while held notes keep sounding (SPEC section 12).
    static constexpr double modeFadeSeconds = 0.030;
    static constexpr int controlBlock = 32;
    static constexpr double releaseSeconds = 0.006;
    static constexpr int latencySamples = BassVoice::latencySamples;
    // Anvil's weight under Furnace at default Levels (SPEC section 4): +8 dB,
    // the "heavy" weight the owner found very satisfying in an early listen,
    // Anvil about 0-5 LU under Furnace. +4 dB would restore Ironstack's
    // 6-10 LU; round 1 still compares the two.
    static constexpr double anvilBalance = 2.5118864315095806; // +8 dB
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setLayersEnabled(bool bass, bool rhythm) noexcept;
    void setLayerLevels(float bassDb, float rhythmDb) noexcept;
    void setBassParameters(float splitHz, float drive) noexcept
    {
        splitTarget = splitHz; driveTarget = drive;
        for (auto& voice : bassVoices) voice.setParameters(splitHz, drive);
    }
    // Diagnostic drive amount (fixed in the product) and Tight. Amount 0 makes
    // the whole Furnace chain linear, its dynamics included, for tests.
    void setRhythmParameters(float drive, float tight = 0.5f) noexcept
    {
        rhythmDrive = drive; rhythmTight = tight;
        for (auto& furnace : furnaces) furnace.setParameters(drive, tight);
    }
    // Modes 0-3 = A-D (SPEC section 12), real-time safe: a change while notes
    // sound crossfades; a change while silent is immediate. Modes not built
    // yet (Anvil C/D, Furnace D) play A until M1.7.
    void setFurnaceMode(int mode) noexcept { furnaceTarget.mode = std::clamp(mode, 0, 3); updateFurnace(); }
    void setAnvilMode(int mode) noexcept { anvilTarget.mode = std::clamp(mode, 0, 3); updateAnvil(); }
    int furnaceMode() const noexcept { return furnaceTarget.mode; }
    // Furnace Drop (SPEC section 3): the wall an octave down, switched like a
    // mode (crossfade while sounding). Mono centres the wall over 20 ms.
    void setFurnaceDrop(bool on) noexcept { furnaceTarget.drop = on; updateFurnace(); }
    void setFurnaceMono(bool on) noexcept;
    // Destruction (SPEC section 12): each mode's extreme version, switched
    // like a mode change.
    void setFurnaceDestruction(bool on) noexcept { furnaceTarget.destroy = on; updateFurnace(); }
    void setAnvilDestruction(bool on) noexcept { anvilTarget.destroy = on; updateAnvil(); }

    int anvilMode() const noexcept { return anvilTarget.mode; }
    // Test hook, not real-time: a custom Furnace design for the playing slot,
    // taking effect at prepare() or reset().
    void setFurnaceDesign(const FurnaceDesign& design) noexcept { furnaces[furnaceSlot].setDesign(design); }
    // Which Anvil grit plays; not real-time, set before prepare().
    void setAnvilGrit(AnvilGrit grit) noexcept
    {
        anvilTarget = bassSetup[bassSlot] = {static_cast<int>(grit), false};
        bassVoices[bassSlot].setMode(grit, false);
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
    using RhythmTarget = FurnaceLayer::Target;
    std::array<RhythmTarget, rhythmVoiceCount> rhythmTargets() const noexcept { return furnaces[furnaceSlot].targets(bend); }
    // Furnace's multiband gain per band (0 low, 1 mid, 2 high), in dB, for
    // tests and meters.
    float furnaceBandGainDb(int band) const noexcept { return furnaces[furnaceSlot].bandGainDb(band); }
    void process(float* left, float* right, int numSamples,
                 LayerMask outputMask = LayerMask::both, LayerTaps taps = {},
                 BassComponent component = BassComponent::all) noexcept;

private:
    // Two of each layer: the playing slot and, during a mode crossfade, the
    // outgoing one. Outside a crossfade the other slot is silent and idle.
    std::array<FurnaceLayer, 2> furnaces;
    std::array<BassVoice, 2> bassVoices;
    std::size_t furnaceSlot = 0, bassSlot = 0;
    int furnaceFade = 0, bassFade = 0, modeFadeSamples = 1323;
    // What each Furnace slot plays, and what the controls ask for.
    struct FurnaceSetup { int mode = 0; bool drop = false, destroy = false; bool operator==(const FurnaceSetup&) const = default; };
    std::array<FurnaceSetup, 2> furnaceSetup{};
    FurnaceSetup furnaceTarget{};
    struct AnvilSetup { int mode = 0; bool destroy = false; bool operator==(const AnvilSetup&) const = default; };
    std::array<AnvilSetup, 2> bassSetup{};
    AnvilSetup anvilTarget{};
    void updateAnvil() noexcept;
    void configureAnvil(std::size_t slot) noexcept;
    dsp_core::LinearRamp monoBlend;
    bool monoOn = false;
    void updateFurnace() noexcept;
    void configureFurnace(std::size_t slot) noexcept;
    float splitTarget = 140.0f, driveTarget = 0.5f, rhythmDrive = 0.5f, rhythmTight = 0.5f;
    bool bassEnabled = true, rhythmEnabled = true;
    bool bassSuspended = false, rhythmSuspended = false;
    dsp_core::LinearRamp bassPower, rhythmPower, bassBendOffset;
    dsp_core::LinearRamp bassMixLevel, rhythmMixLevel;
    double bassMixTarget = 1.0, rhythmMixTarget = 1.0;
    static_assert(RhythmBus::latencySamples == latencySamples, "layer latency must agree");
    int bassChannel = 1; // Retain selected bend during the note-off release.
    NoteRouter router;
    void updateBass(NoteRouter::Change change) noexcept;
    void startBass(BassVoice& voice) noexcept;
    void startRhythm(int channel, int note, int velocity) noexcept;
    void startHeldRhythm(FurnaceLayer& furnace) noexcept;
    void clearRhythm() noexcept;
    void finishFurnaceFade() noexcept;
    void finishBassFade() noexcept;
    void processBlock(float* left, float* right, int count, LayerMask outputMask, LayerTaps taps,
                      BassComponent component) noexcept;
    // Equal-power weights for the incoming slot at fade sample remaining/total.
    static void fadeWeights(int remaining, int total, float& incoming, float& outgoing) noexcept;
    std::array<dsp_core::LinearRamp, 16> bend{};
    double rate = 44100.0;
    int attackSamples = 45, releaseSamples = 265, transitionSamples = 89, fadeSamples = 221;
    std::uint64_t serial = 0;

    // Per-block scratch, preallocated: bend ratios per channel, layer gains,
    // and the Furnace sum with its strongest envelope and velocity.
    using Block = std::array<double, controlBlock>;
    std::array<Block, 16> bendRatio{};
    Block rhythmGain{}, bassGain{}, rhythmLeft{}, rhythmRight{}, outgoingLeft{}, outgoingRight{};
};
}
