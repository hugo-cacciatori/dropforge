#include "PluginProcessor.h"
#include "MidiInput.h"
#include "ui/PluginEditor.h"
#include <cmath>
#include <locale>
#include <map>
#include <sstream>
#include <utility>

static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "Audio parameter and panic reads must be lock-free");

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
        juce::NormalisableRange<float>{-12.0f, 6.0f}, 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("dB")
            .withStringFromValueFunction([](float value, int) { return juce::String(value, 2); })
            .withValueFromStringFunction([](const juce::String& text) { return text.getFloatValue(); })));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::bassEnabled, 1}, "Anvil enabled", true));
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{dropforge::parameters::rhythmEnabled, 1}, "Furnace enabled", true));
    for (const auto& layer : {std::pair{dropforge::parameters::bassLevel, "Anvil Level"},
                              std::pair{dropforge::parameters::rhythmLevel, "Furnace Level"}})
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{layer.first, 1}, layer.second,
            juce::NormalisableRange<float>{-60.0f, 6.0f}, 0.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("dB")));
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
    core.setLayersEnabled(true, true);
    core.reset(0.0f);
}

void DropforgeProcessor::prepareToPlay(double sampleRate, int)
{
    preparedRate = std::isfinite(sampleRate) && sampleRate >= 44100.0 && sampleRate <= 192000.0 ? sampleRate : 44100.0;
    core.setLayersEnabled(readToggle(bassEnabled, true), readToggle(rhythmEnabled, true));
    core.setLayerLevels(bassLevel->load(), rhythmLevel->load());
    core.prepare(preparedRate, readOutputDb());
    reset();
    setLatencySamples(dropforge::Engine::latencySamples);
}

void DropforgeProcessor::releaseResources() { reset(); }

float DropforgeProcessor::readOutputDb() const noexcept
{
    const auto value = outputDb->load(std::memory_order_relaxed);
    return std::isfinite(value) ? std::clamp(value, -12.0f, 6.0f) : 0.0f;
}

void DropforgeProcessor::reset()
{
    core.setLayersEnabled(readToggle(bassEnabled, true), readToggle(rhythmEnabled, true));
    core.setLayerLevels(bassLevel->load(), rhythmLevel->load());
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
    core.setLayersEnabled(readToggle(bassEnabled, true), readToggle(rhythmEnabled, true));
    dropforge::processMidiBlock(core, buffer, midi);
    midi.clear();
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
