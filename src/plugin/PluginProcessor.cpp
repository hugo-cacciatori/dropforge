#include "PluginProcessor.h"
#include "MidiInput.h"
#include "ui/PluginEditor.h"
#include <cmath>
#include <locale>
#include <map>
#include <sstream>
#include <utility>

static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free
              && std::atomic<std::uint64_t>::is_always_lock_free,
              "Audio parameter, panic and held-key exchanges must be lock-free");

namespace
{
// Bump only with a migration in setStateInformation; unknown versions are refused.
constexpr int stateSchemaVersion = 1;

// Strict "C" locale parsing: a saved state must never depend on the user's
// decimal separator, and trailing garbage rejects the value.
bool parseFloat(const juce::String& text, float& value)
{
    std::istringstream parser(text.toStdString());
    parser.imbue(std::locale::classic());
    parser >> std::noskipws >> value;
    return parser && parser.peek() == std::char_traits<char>::eof() && std::isfinite(value);
}
}

juce::AudioProcessorValueTreeState::ParameterLayout DropforgeProcessor::createParameters()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{dropforge::parameters::output, 1}, "Output",
        juce::NormalisableRange<float>{-24.0f, 6.0f}, 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")
            .withStringFromValueFunction([](float value, int) { return juce::String(value, 2); })
            .withValueFromStringFunction([](const juce::String& text) { return text.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::bassEnabled, 1}, "Anvil enabled", true));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::rhythmEnabled, 1}, "Furnace enabled", true));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::leadEnabled, 1}, "Blade enabled", false));
    for (const auto& layer : {std::pair{dropforge::parameters::bassLevel, "Anvil Level"},
                              std::pair{dropforge::parameters::rhythmLevel, "Furnace Level"},
                              std::pair{dropforge::parameters::leadLevel, "Blade Level"}})
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{layer.first, 1}, layer.second,
            juce::NormalisableRange<float>{-60.0f, 6.0f}, 0.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("dB")));
    const juce::StringArray modes{"A Forge", "B Riot", "C Halo", "D Alloy"};
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{dropforge::parameters::bassMode, 1}, "Anvil Mode", modes, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{dropforge::parameters::rhythmMode, 1}, "Furnace Mode", modes, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{dropforge::parameters::leadMode, 1}, "Blade Mode", modes, 0));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::rhythmMono, 1}, "Furnace Mono", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::rhythmDrop, 1}, "Furnace Drop", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::bassDestroy, 1}, "Anvil Destruction", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::rhythmDestroy, 1}, "Furnace Destruction", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::leadDestroy, 1}, "Blade Destruction", false));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{dropforge::parameters::leadPortamento, 1}, "Blade Portamento", juce::StringArray{"Off", "Legato", "Always"}, 0));
    // Skewed so the musical short times get most of the travel: 150 ms sits mid-way.
    juce::NormalisableRange<float> portamentoRange{0.0f, 1000.0f};
    portamentoRange.setSkewForCentre(150.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{dropforge::parameters::leadPortamentoMs, 1}, "Blade Portamento Time", portamentoRange, 60.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{dropforge::parameters::leadOctave, 1}, "Blade Octave", -1, 1, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{dropforge::parameters::bendRange, 1}, "Bend Range", juce::StringArray{"2", "5", "7", "12", "24"}, 0));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::glideOn, 1}, "Glide", false));
    juce::NormalisableRange<float> glideRange{0.0f, 500.0f};
    glideRange.setSkewForCentre(100.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{dropforge::parameters::glideMs, 1}, "Glide Time", glideRange, 80.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::impactOn, 1}, "Impact", false));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{dropforge::parameters::wheelMode, 1}, "Wheel", juce::StringArray{"Roar", "Vibrato"}, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{dropforge::parameters::wheelAmount, 1}, "Wheel Amount", juce::NormalisableRange<float>{0.0f, 1.0f}, 0.0f));
    // FX page (SPEC section 8): every strip off by default.
    namespace fx = dropforge::parameters;
    const auto percent = [&layout](const char* id, const char* name, float max, float fallback)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>{0.0f, max}, fallback,
                                                               juce::AudioParameterFloatAttributes{}.withLabel("%")));
    };
    const auto skewed = [&layout](const char* id, const char* name, float low, float high, float centre, float fallback, const char* unit)
    {
        juce::NormalisableRange<float> range{low, high};
        range.setSkewForCentre(centre);
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id, 1}, name, range, fallback,
                                                               juce::AudioParameterFloatAttributes{}.withLabel(unit)));
    };
    percent(fx::fxSendAnvil, "Anvil Send", 100.0f, 60.0f);
    percent(fx::fxSendFurnace, "Furnace Send", 100.0f, 100.0f);
    percent(fx::fxSendBlade, "Blade Send", 100.0f, 60.0f);
    percent(fx::fxDuck, "Ducking", 100.0f, 50.0f);
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{fx::fxDelayOn, 1}, "Delay", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{fx::fxDelaySync, 1}, "Delay Sync", true));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{fx::fxDelayDivision, 1}, "Delay Division",
        juce::StringArray{"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars"}, 3));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{fx::fxDelayFeel, 1}, "Delay Feel",
        juce::StringArray{"Straight", "Dotted", "Triplet"}, 1));
    skewed(fx::fxDelayMs, "Delay Time", 1.0f, 2000.0f, 300.0f, 375.0f, "ms");
    percent(fx::fxDelayFeedback, "Delay Feedback", 95.0f, 25.0f);
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{fx::fxDelayMode, 1}, "Delay Mode",
        juce::StringArray{"Stereo", "Ping-pong"}, 1));
    percent(fx::fxDelayWidth, "Delay Width", 100.0f, 100.0f);
    percent(fx::fxDelayMix, "Delay Mix", 100.0f, 25.0f);
    skewed(fx::fxDelayLowCut, "Delay Low Cut", 20.0f, 2000.0f, 250.0f, 280.0f, "Hz");
    skewed(fx::fxDelayHighCut, "Delay High Cut", 1000.0f, 20000.0f, 6000.0f, 6500.0f, "Hz");
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{fx::fxReverbOn, 1}, "Reverb", false));
    skewed(fx::fxReverbDecay, "Reverb Decay", 0.1f, 15.0f, 2.0f, 0.8f, "s");
    skewed(fx::fxReverbPreDelay, "Reverb Pre-delay", 0.0f, 500.0f, 80.0f, 20.0f, "ms");
    percent(fx::fxReverbSize, "Reverb Size", 100.0f, 60.0f);
    percent(fx::fxReverbDiffusion, "Reverb Diffusion", 100.0f, 80.0f);
    percent(fx::fxReverbDamping, "Reverb Damping", 100.0f, 40.0f);
    percent(fx::fxReverbModulation, "Reverb Modulation", 100.0f, 30.0f);
    percent(fx::fxReverbWidth, "Reverb Width", 100.0f, 100.0f);
    percent(fx::fxReverbMix, "Reverb Mix", 100.0f, 20.0f);
    skewed(fx::fxReverbLowCut, "Reverb Low Cut", 20.0f, 2000.0f, 250.0f, 250.0f, "Hz");
    skewed(fx::fxReverbHighCut, "Reverb High Cut", 1000.0f, 20000.0f, 6000.0f, 7000.0f, "Hz");
    percent(fx::spaceAmount, "Space", 100.0f, 50.0f);
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{fx::fxSaturateOn, 1}, "Saturate", false));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{fx::fxSaturateDrive, 1}, "Saturate Drive",
        juce::NormalisableRange<float>{0.0f, 24.0f}, 6.0f, juce::AudioParameterFloatAttributes{}.withLabel("dB")));
    percent(fx::fxSaturateMix, "Saturate Mix", 100.0f, 100.0f);
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{fx::fxCrushOn, 1}, "Crush", false));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{fx::fxCrushBits, 1}, "Crush Bits",
        juce::NormalisableRange<float>{2.0f, 16.0f}, 8.0f, juce::AudioParameterFloatAttributes{}.withLabel("bits")));
    skewed(fx::fxCrushRate, "Crush Rate", 1000.0f, 24000.0f, 6000.0f, 8000.0f, "Hz");
    skewed(fx::fxCrushTone, "Crush Tone", 1000.0f, 20000.0f, 6000.0f, 8000.0f, "Hz");
    percent(fx::fxCrushMix, "Crush Mix", 100.0f, 50.0f);
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{fx::fxChorusOn, 1}, "Chorus", false));
    skewed(fx::fxChorusRate, "Chorus Rate", 0.05f, 5.0f, 0.6f, 0.6f, "Hz");
    percent(fx::fxChorusDepth, "Chorus Depth", 100.0f, 50.0f);
    percent(fx::fxChorusMix, "Chorus Mix", 100.0f, 40.0f);
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{fx::fxEqOn, 1}, "EQ", false));
    skewed(fx::fxEqLowCut, "EQ Low Cut", 20.0f, 300.0f, 80.0f, 20.0f, "Hz");
    skewed(fx::fxEqHighCut, "EQ High Cut", 2000.0f, 20000.0f, 8000.0f, 20000.0f, "Hz");
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{fx::fxEqTilt, 1}, "EQ Tilt",
        juce::NormalisableRange<float>{-6.0f, 6.0f}, 0.0f, juce::AudioParameterFloatAttributes{}.withLabel("dB")));
    return layout;
}

DropforgeProcessor::DropforgeProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      state(*this, nullptr, "DropforgeState", createParameters())
{
    outputDb = state.getRawParameterValue(dropforge::parameters::output);
    bassLevel = state.getRawParameterValue(dropforge::parameters::bassLevel);
    rhythmLevel = state.getRawParameterValue(dropforge::parameters::rhythmLevel);
    bassEnabled = state.getRawParameterValue(dropforge::parameters::bassEnabled);
    rhythmEnabled = state.getRawParameterValue(dropforge::parameters::rhythmEnabled);
    bassMode = state.getRawParameterValue(dropforge::parameters::bassMode);
    rhythmMode = state.getRawParameterValue(dropforge::parameters::rhythmMode);
    rhythmMono = state.getRawParameterValue(dropforge::parameters::rhythmMono);
    rhythmDrop = state.getRawParameterValue(dropforge::parameters::rhythmDrop);
    bassDestroy = state.getRawParameterValue(dropforge::parameters::bassDestroy);
    rhythmDestroy = state.getRawParameterValue(dropforge::parameters::rhythmDestroy);
    leadEnabled = state.getRawParameterValue(dropforge::parameters::leadEnabled);
    leadLevel = state.getRawParameterValue(dropforge::parameters::leadLevel);
    leadMode = state.getRawParameterValue(dropforge::parameters::leadMode);
    leadDestroy = state.getRawParameterValue(dropforge::parameters::leadDestroy);
    leadPortamento = state.getRawParameterValue(dropforge::parameters::leadPortamento);
    leadPortamentoMs = state.getRawParameterValue(dropforge::parameters::leadPortamentoMs);
    leadOctave = state.getRawParameterValue(dropforge::parameters::leadOctave);
    bendRangeChoice = state.getRawParameterValue(dropforge::parameters::bendRange);
    glideEnabled = state.getRawParameterValue(dropforge::parameters::glideOn);
    glideTime = state.getRawParameterValue(dropforge::parameters::glideMs);
    impactEnabled = state.getRawParameterValue(dropforge::parameters::impactOn);
    wheelModeChoice = state.getRawParameterValue(dropforge::parameters::wheelMode);
    wheelAmountValue = state.getRawParameterValue(dropforge::parameters::wheelAmount);
    {
        namespace fx = dropforge::parameters;
        const std::array<const char*, 43> ids{fx::fxSendAnvil, fx::fxSendFurnace, fx::fxSendBlade, fx::fxDuck, fx::fxDelayOn, fx::fxDelaySync,
                                              fx::fxDelayDivision, fx::fxDelayFeel, fx::fxDelayMs, fx::fxDelayFeedback, fx::fxDelayMode,
                                              fx::fxDelayWidth, fx::fxDelayMix, fx::fxDelayLowCut, fx::fxDelayHighCut,
                                              fx::fxReverbOn, fx::fxReverbDecay, fx::fxReverbPreDelay, fx::fxReverbSize, fx::fxReverbDiffusion,
                                              fx::fxReverbDamping, fx::fxReverbModulation, fx::fxReverbWidth, fx::fxReverbMix,
                                              fx::fxReverbLowCut, fx::fxReverbHighCut, fx::spaceAmount,
                                              fx::fxSaturateOn, fx::fxSaturateDrive, fx::fxSaturateMix, fx::fxCrushOn, fx::fxCrushBits,
                                              fx::fxCrushRate, fx::fxCrushTone, fx::fxCrushMix, fx::fxChorusOn, fx::fxChorusRate,
                                              fx::fxChorusDepth, fx::fxChorusMix, fx::fxEqOn, fx::fxEqLowCut, fx::fxEqHighCut, fx::fxEqTilt};
        for (std::size_t i = 0; i < ids.size(); ++i) fxValues[i] = state.getRawParameterValue(ids[i]);
    }
    core.setLayersEnabled(true, true);
    core.reset(0.0f);
}

void DropforgeProcessor::prepareToPlay(double sampleRate, int)
{
    preparedRate = std::isfinite(sampleRate) && sampleRate >= 44100.0 && sampleRate <= 192000.0 ? sampleRate : 44100.0;
    core.setLayersEnabled(readToggle(bassEnabled, true), readToggle(rhythmEnabled, true));
    core.setLayerLevels(bassLevel->load(), rhythmLevel->load());
    core.setLead(readToggle(leadEnabled, false), leadLevel->load());
    applyModes();
    core.prepare(preparedRate, readOutputDb());
    reset();
    setLatencySamples(dropforge::InstrumentCore::latencySamples);
}

void DropforgeProcessor::releaseResources() { reset(); }

float DropforgeProcessor::readOutputDb() const noexcept
{
    const auto value = outputDb->load(std::memory_order_relaxed);
    return std::isfinite(value) ? std::clamp(value, -24.0f, 6.0f) : 0.0f;
}

void DropforgeProcessor::reset()
{
    core.setLayersEnabled(readToggle(bassEnabled, true), readToggle(rhythmEnabled, true));
    core.setLayerLevels(bassLevel->load(), rhythmLevel->load());
    core.setLead(readToggle(leadEnabled, false), leadLevel->load());
    applyModes();
    applyFx();
    core.reset(readOutputDb());
    panicRequested.store(false, std::memory_order_relaxed);
}

bool DropforgeProcessor::readToggle(const std::atomic<float>* parameter, bool fallback) noexcept
{
    const auto value = parameter->load(std::memory_order_relaxed);
    return std::isfinite(value) ? value >= 0.5f : fallback;
}

bool DropforgeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet().isDisabled()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DropforgeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    if (panicRequested.exchange(false, std::memory_order_relaxed))
        core.panic();
    core.setOutputDb(readOutputDb());
    core.setLayerLevels(bassLevel->load(), rhythmLevel->load());
    core.setLead(readToggle(leadEnabled, false), leadLevel->load());
    core.setLayersEnabled(readToggle(bassEnabled, true), readToggle(rhythmEnabled, true));
    applyModes();
    applyFx();
    dropforge::processMidiBlock(core, buffer, midi);
    midi.clear();
    publishMeters();
}

void DropforgeProcessor::applyModes() noexcept
{
    // Choice parameters hold their index as a float; anything unreadable is A.
    const auto index = [](const std::atomic<float>* parameter) noexcept
    {
        const auto value = parameter->load(std::memory_order_relaxed);
        return std::isfinite(value) ? std::clamp(static_cast<int>(std::lround(value)), 0, 3) : 0;
    };
    core.setModes(index(bassMode), index(rhythmMode));
    core.setFurnaceOptions(readToggle(rhythmMono, false), readToggle(rhythmDrop, false));
    core.setDestruction(readToggle(bassDestroy, false), readToggle(rhythmDestroy, false));
    core.setLeadMode(index(leadMode), readToggle(leadDestroy, false));
    const auto octave = leadOctave->load(std::memory_order_relaxed);
    constexpr std::array<int, 5> ranges{2, 5, 7, 12, 24};
    core.setPerformance(ranges[static_cast<std::size_t>(std::clamp(index(bendRangeChoice), 0, 4))], readToggle(glideEnabled, false),
                        glideTime->load(std::memory_order_relaxed), readToggle(impactEnabled, false));
    core.setWheel(std::clamp(index(wheelModeChoice), 0, 1), wheelAmountValue->load(std::memory_order_relaxed));
    core.setLeadPlaying(std::clamp(index(leadPortamento), 0, 2), leadPortamentoMs->load(std::memory_order_relaxed),
                        std::isfinite(octave) ? static_cast<int>(std::lround(octave)) : 0);
}

// The FX page in the order of fxValues; the tempo comes from the host each
// block (120 BPM when it gives none).
void DropforgeProcessor::applyFx() noexcept
{
    const auto value = [this](std::size_t i, float fallback) noexcept
    {
        const auto v = fxValues[i]->load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : fallback;
    };
    dropforge::FxChain::Settings settings;
    settings.sendAnvil = value(0, 60.0f) / 100.0f;
    settings.sendFurnace = value(1, 100.0f) / 100.0f;
    settings.sendBlade = value(2, 60.0f) / 100.0f;
    settings.duck = value(3, 50.0f) / 100.0f;
    settings.delayOn = value(4, 0.0f) >= 0.5f;
    settings.delaySync = value(5, 1.0f) >= 0.5f;
    settings.delayDivision = std::clamp(static_cast<int>(std::lround(value(6, 3.0f))), 0, 7);
    settings.delayFeel = static_cast<dropforge::FxChain::Feel>(std::clamp(static_cast<int>(std::lround(value(7, 1.0f))), 0, 2));
    settings.delayMs = value(8, 375.0f);
    settings.delayFeedback = value(9, 25.0f) / 100.0f;
    settings.delayPingPong = value(10, 1.0f) >= 0.5f;
    settings.delayWidth = value(11, 100.0f) / 100.0f;
    settings.delayMix = value(12, 25.0f) / 100.0f;
    settings.delayLowCutHz = value(13, 280.0f);
    settings.delayHighCutHz = value(14, 6500.0f);
    settings.reverbOn = value(15, 0.0f) >= 0.5f;
    settings.reverb = {value(16, 0.8f), value(17, 20.0f), value(18, 60.0f) / 100.0f, value(19, 80.0f) / 100.0f, value(20, 40.0f) / 100.0f,
                       value(21, 30.0f) / 100.0f, value(22, 100.0f) / 100.0f, value(24, 250.0f), value(25, 7000.0f)};
    settings.reverbMix = value(23, 20.0f) / 100.0f;
    settings.space = value(26, 50.0f) / 100.0f;
    settings.saturateOn = value(27, 0.0f) >= 0.5f;
    settings.saturateDriveDb = value(28, 6.0f);
    settings.saturateMix = value(29, 100.0f) / 100.0f;
    auto& finish = settings.finish;
    finish.crushOn = value(30, 0.0f) >= 0.5f;
    finish.crushBits = value(31, 8.0f);
    finish.crushRateHz = value(32, 8000.0f);
    finish.crushToneHz = value(33, 8000.0f);
    finish.crushMix = value(34, 50.0f) / 100.0f;
    finish.chorusOn = value(35, 0.0f) >= 0.5f;
    finish.chorusRateHz = value(36, 0.6f);
    finish.chorusDepth = value(37, 50.0f) / 100.0f;
    finish.chorusMix = value(38, 40.0f) / 100.0f;
    finish.eqOn = value(39, 0.0f) >= 0.5f;
    finish.eqLowCutHz = value(40, 20.0f);
    finish.eqHighCutHz = value(41, 20000.0f);
    finish.eqTiltDb = value(42, 0.0f);
    double bpm = 120.0;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto hostBpm = position->getBpm()) bpm = *hostBpm;
    core.setFx(settings, bpm);
}

void DropforgeProcessor::publishMeters() noexcept
{
    // Keep the extreme until the editor reads it: peaks rise, reductions
    // deepen. One writer, one reader, so the exchange loops end at once.
    const auto taken = core.takeMeters();
    const auto keep = [](std::atomic<float>& meter, float value, bool larger) noexcept
    {
        auto current = meter.load(std::memory_order_relaxed);
        while ((larger ? value > current : value < current)
               && !meter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
    };
    keep(meterPeakLeft, taken.peakLeft, true);
    keep(meterPeakRight, taken.peakRight, true);
    keep(meterGlueDb, taken.glueReductionDb, false);
    keep(meterLimiterDb, taken.limiterReductionDb, false);
    const auto keys = core.heldKeys();
    keysLow.store(keys.low, std::memory_order_relaxed);
    keysHigh.store(keys.high, std::memory_order_relaxed);
    bassKey.store(keys.bassNote, std::memory_order_relaxed);
    leadKey.store(keys.leadNote, std::memory_order_relaxed);
    wheelNow.store(core.wheelPosition(), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* DropforgeProcessor::createEditor()
{
    return new DropforgeEditor(*this);
}

void DropforgeProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto saved = state.copyState();
    saved.setProperty("schemaVersion", stateSchemaVersion, nullptr);
    if (auto xml = saved.createXml())
        copyXmlToBinary(*xml, destination);
}

void DropforgeProcessor::setStateInformation(const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return;
    auto xml = getXmlFromBinary(data, size);
    if (!xml || !xml->hasTagName(state.state.getType()))
        return;
    if (xml->getStringAttribute("schemaVersion") != juce::String(stateSchemaVersion))
        return;

    // Validate everything before publishing any of it: one duplicated or
    // unreadable entry refuses the whole recall rather than half-applying it.
    std::map<juce::String, float> recalled;
    for (auto* child : xml->getChildIterator())
    {
        if (!child->hasTagName("PARAM"))
            continue;
        const auto id = child->getStringAttribute("id");
        float value = 0.0f;
        if (!child->hasAttribute("value") || recalled.count(id) != 0
            || !parseFloat(child->getStringAttribute("value"), value))
            return;
        recalled[id] = value;
    }

    // Missing parameters take their defaults; unknown ones are ignored, so
    // states saved by later versions still load their known settings.
    juce::ValueTree clean(state.state.getType());
    clean.setProperty("schemaVersion", stateSchemaVersion, nullptr);
    for (auto* parameter : getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter);
        if (ranged == nullptr)
            continue;
        const auto& range = ranged->getNormalisableRange();
        auto value = range.convertFrom0to1(ranged->getDefaultValue());
        if (const auto found = recalled.find(ranged->paramID); found != recalled.end())
        {
            value = found->second;
            if (dynamic_cast<juce::AudioParameterBool*>(ranged) != nullptr && value != 0.0f && value != 1.0f)
                return;
            value = std::clamp(value, range.start, range.end);
        }
        juce::ValueTree entry("PARAM");
        entry.setProperty("id", ranged->paramID, nullptr);
        entry.setProperty("value", value, nullptr);
        clean.appendChild(entry, nullptr);
    }
    // Loading a project must not mark that project dirty again.
    state.replaceState(clean);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DropforgeProcessor();
}
