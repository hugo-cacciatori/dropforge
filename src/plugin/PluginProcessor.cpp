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
                        glideTime->load(std::memory_order_relaxed));
    core.setLeadPlaying(std::clamp(index(leadPortamento), 0, 2), leadPortamentoMs->load(std::memory_order_relaxed),
                        std::isfinite(octave) ? static_cast<int>(std::lround(octave)) : 0);
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
