#include "plugin/PluginProcessor.h"
#include "plugin/MidiInput.h"
#include "ui/PluginEditor.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>
#include <locale>
#if JUCE_LINUX
#include "X11TestDisplay.h"
#endif

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void setOutput(DropforgeProcessor& processor, float db)
{
    auto* parameter = processor.parameterState().getParameter(dropforge::parameters::output);
    require(parameter != nullptr, "stable output parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(db));
}

float outputDb(DropforgeProcessor& processor)
{
    return processor.parameterState().getRawParameterValue(dropforge::parameters::output)->load();
}

void restoreXml(DropforgeProcessor& processor, const juce::String& text)
{
    auto xml = juce::parseXML(text);
    require(xml != nullptr, "test state XML parses");
    juce::MemoryBlock bytes;
    juce::AudioProcessor::copyXmlToBinary(*xml, bytes);
    processor.setStateInformation(bytes.getData(), static_cast<int>(bytes.getSize()));
}

void setToggle(DropforgeProcessor& processor, const char* id, bool enabled)
{
    auto* parameter = processor.parameterState().getParameter(id);
    require(parameter && parameter->isBoolean(), "host toggle exists and is boolean");
    parameter->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
}

bool toggle(DropforgeProcessor& processor, const char* id)
{
    return processor.parameterState().getRawParameterValue(id)->load() >= 0.5f;
}

void refreshUi()
{
    juce::Thread::sleep(60);
    juce::Timer::callPendingTimersSynchronously();
}

void rightClick(juce::Component& target, juce::Point<float> at = {60.0f, 20.0f})
{
    const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), at,
        juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        &target, &target, {}, at, {}, 1, false);
    target.mouseDown(event);
    target.mouseUp(event);
}

void leftClick(juce::Component& target, juce::Point<float> at)
{
    const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), at, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        &target, &target, {}, at, {}, 1, false);
    target.mouseDown(event);
    target.mouseUp(event);
}

int choice(DropforgeProcessor& processor, const char* id)
{
    return juce::roundToInt(processor.parameterState().getRawParameterValue(id)->load());
}

// Every component inside a parent, depth first (only the visible ones if asked).
void collect(juce::Component& parent, std::vector<juce::Component*>& out, bool visibleOnly = false)
{
    for (auto* child : parent.getChildren())
        if (!visibleOnly || child->isVisible()) { out.push_back(child); collect(*child, out, visibleOnly); }
}

void stateTests()
{
    DropforgeProcessor source, destination;
    struct HostChanges final : juce::AudioProcessorListener
    {
        int nonParameterChanges = 0, parameterChanges = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++parameterChanges; }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& changes) override
        {
            if (changes.nonParameterStateChanged) ++nonParameterChanges;
        }
    } changes;
    source.addListener(&changes);
    source.showPage(1);
    source.showPage(0);
    require(changes.nonParameterChanges == 0 && changes.parameterChanges == 0,
            "choosing an editor page is a view preference: no host change or automation");
    restoreXml(source, "<DropforgeState schemaVersion='1'/>");
    require(changes.nonParameterChanges == 0, "recall does not mark the project dirty");
    source.removeListener(&changes);
    require(source.getParameters().size() == 11, "output; power, level, mode and Destruction for each layer; Furnace Mono and Drop");
    for (const auto* id : {dropforge::parameters::bassMode, dropforge::parameters::rhythmMode})
    {
        const auto* mode = dynamic_cast<juce::AudioParameterChoice*>(source.parameterState().getParameter(id));
        require(mode != nullptr && mode->choices.size() == 4 && mode->getIndex() == 0,
                "each mode parameter offers A-D from the start and defaults to A");
    }
    for (const auto* retired : {"xover_hz", "destruction", "guitar_mono", "rhythm_mono", "output_db", "bass_enabled"})
        require(source.parameterState().getParameter(retired) == nullptr, "no Ironstack legacy parameter IDs");
    require(toggle(source, dropforge::parameters::bassEnabled) && toggle(source, dropforge::parameters::rhythmEnabled)
            && source.currentPage() == 0, "both layers on and the Anvil page by default");
    require(outputDb(source) == 0.0f, "output defaults to unity");
    for (const float value : {-12.0f, -9.876543f, -6.0f, 0.0f, 1.234567f, 6.0f})
    {
        setOutput(source, value);
        const auto expected = outputDb(source);
        juce::MemoryBlock saved;
        source.getStateInformation(saved);
        destination.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        require(outputDb(destination) == expected, "state restores exact parameter value");
        juce::MemoryBlock repeated;
        destination.getStateInformation(repeated);
        source.setStateInformation(repeated.getData(), static_cast<int>(repeated.getSize()));
        require(outputDb(source) == expected, "repeated state round-trip is exact");
    }
    for (int bits = 0; bits < 8; ++bits)
    {
        setToggle(source, dropforge::parameters::bassEnabled, (bits & 1) != 0);
        setToggle(source, dropforge::parameters::rhythmEnabled, (bits & 2) != 0);
        source.showPage((bits >> 2) & 1);
        destination.showPage(0);
        juce::MemoryBlock saved, repeated;
        source.getStateInformation(saved);
        destination.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        destination.getStateInformation(repeated);
        require(destination.currentPage() == 0, "the editor page is not part of the saved sound");
        for (const auto* id : {dropforge::parameters::bassEnabled, dropforge::parameters::rhythmEnabled})
            require(toggle(source, id) == toggle(destination, id), "all boolean values restore exactly");
        for (const auto* id : {dropforge::parameters::output, dropforge::parameters::bassLevel, dropforge::parameters::rhythmLevel})
            require(source.parameterState().getRawParameterValue(id)->load() == destination.parameterState().getRawParameterValue(id)->load(),
                    "levels restore exactly, including for powered-off layers");
        source.setStateInformation(repeated.getData(), static_cast<int>(repeated.getSize()));
        source.getStateInformation(saved);
        require(saved == repeated, "canonical repeated states are byte-exact");
    }
    setOutput(destination, -6.0f);
    const auto before = outputDb(destination);
    for (const auto* bad : {
        "<OtherState schemaVersion='1'/>",
        "<DropforgeState><PARAM id='output' value='3'/></DropforgeState>",
        "<DropforgeState schemaVersion='2'><PARAM id='output' value='3'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='3'/><PARAM id='bass_on' value='0.5'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='3'/><PARAM id='rhythm_on' value='nan'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='3'/><PARAM id='bass_on' value='0'/><PARAM id='bass_on' value='1'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='3'/><PARAM id='future' value='x'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='nan'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='inf'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='oops'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='2junk'/></DropforgeState>",
        "<DropforgeState schemaVersion='1'><PARAM id='output' value='2'/><PARAM id='output' value='3'/></DropforgeState>"})
    {
        juce::MemoryBlock initial, after;
        destination.getStateInformation(initial);
        restoreXml(destination, bad);
        destination.getStateInformation(after);
        require(initial == after, "invalid state cannot partially publish parameters");
        require(outputDb(destination) == before, "invalid/unknown state preserves current settings");
    }
    destination.setStateInformation(nullptr, 0);
    const char truncated[] = "bad state";
    destination.setStateInformation(truncated, sizeof(truncated));
    require(outputDb(destination) == before, "truncated/null state is ignored");
    restoreXml(destination, "<DropforgeState schemaVersion='1'><PARAM id='output' value='99'/></DropforgeState>");
    require(outputDb(destination) == 6.0f, "out-of-range finite state clamps safely");
    restoreXml(destination, "<DropforgeState schemaVersion='1'><PARAM id='future_knob' value='0.3'/>"
                            "<PARAM id='output' value='-2'/></DropforgeState>");
    require(outputDb(destination) == -2.0f, "unknown parameters from later versions are ignored");
    restoreXml(destination, "<DropforgeState schemaVersion='1'/>");
    require(outputDb(destination) == 0.0f && toggle(destination, dropforge::parameters::bassEnabled)
            && toggle(destination, dropforge::parameters::rhythmEnabled), "missing parameters take their defaults");
    struct CommaDecimal final : std::numpunct<char>
    {
        char do_decimal_point() const override { return ','; }
    };
    const auto previousLocale = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    restoreXml(destination, "<DropforgeState schemaVersion='1'><PARAM id='output' value='-3.25'/></DropforgeState>");
    const auto recalled = outputDb(destination);
    std::locale::global(previousLocale);
    require(recalled == -3.25f, "state decimals are independent of the host C++ locale");
    std::cout << "state round-trips, defaults, malformed and future-state checks passed\n";
}

void processorTests()
{
    DropforgeProcessor reference, changed;
    for (auto* processor : {&reference, &changed})
    {
        processor->setRateAndBufferSizeDetails(48000.0, 512);
        processor->prepareToPlay(48000.0, 512);
        require(processor->getLatencySamples() == dropforge::InstrumentCore::latencySamples, "voices plus master report a fixed 96-sample latency");
        require(std::abs(processor->getTailLengthSeconds() - (0.006 + 96.0 / 48000.0)) < 1.0e-9,
                "fixed release plus aligned delay reported");
    }
    juce::AudioBuffer<float> a(2, 512), b(2, 512);
    juce::MidiBuffer midiA, midiB;
    midiA.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127)), 0);
    midiB = midiA;
    reference.processBlock(a, midiA);
    changed.processBlock(b, midiB);
    require(midiA.isEmpty() && midiB.isEmpty(), "instrument consumes MIDI rather than echoing it");
    for (int i = 0; i < 512; ++i)
        require(a.getSample(0, i) == b.getSample(0, i), "default output gain leaves audio unchanged");
    setOutput(changed, -6.0f);
    reference.processBlock(a, midiA);
    changed.processBlock(b, midiB);
    double maximumGainError = 0.0;
    const auto target = std::pow(10.0, -6.0 / 20.0);
    for (int i = 0; i < 512; ++i)
    {
        const auto gain = i < 240 ? 1.0 + (target - 1.0) * i / 240.0 : target;
        maximumGainError = std::max(maximumGainError,
            std::abs(b.getSample(0, i) - a.getSample(0, i) * gain));
        maximumGainError = std::max(maximumGainError,
            std::abs(b.getSample(1, i) - a.getSample(1, i) * gain));
    }
    require(maximumGainError < 1.0e-7, "gain ramps over exactly 5 ms without an instantaneous step");

    // Recall while held uses the same smoothed parameter path.
    juce::MemoryBlock unity;
    reference.getStateInformation(unity);
    changed.setStateInformation(unity.getData(), static_cast<int>(unity.getSize()));
    reference.processBlock(a, midiA);
    changed.processBlock(b, midiB);
    for (int i = 240; i < 512; ++i)
        require(a.getSample(0, i) == b.getSample(0, i), "state recall reaches unity during a held note");
    changed.requestPanic();
    changed.processBlock(b, midiB);
    // 6 ms release (288 samples), the reported latency, and the master
    // clipper's oversampling filter ringing out for its own 32 samples.
    constexpr int silentFrom = 288 + dropforge::InstrumentCore::latencySamples + dsp_core::Oversampler4xFloat::latencySamples;
    require(b.getMagnitude(0, silentFrom, 512 - silentFrom) == 0.0f, "UI panic releases all voices within release plus latency");
    changed.reset();
    changed.processBlock(b, midiB);
    require(b.getMagnitude(0, 512) == 0.0f, "host reset clears held notes");

    for (const double rate : {44100.0, 96000.0, 192000.0})
    {
        changed.prepareToPlay(rate, 1);
        for (const int size : {1, 17, 0, 1024, 3})
        {
            b.setSize(2, size);
            setOutput(changed, size % 2 ? -12.0f : 6.0f);
            midiB.addEvent(juce::MidiMessage::noteOn(1, 127, static_cast<juce::uint8>(127)), 0);
            changed.processBlock(b, midiB);
            for (int i = 0; i < size; ++i)
                require(std::isfinite(b.getSample(0, i)), "parameter extremes/rates/changing blocks stay finite");
        }
        require(changed.getLatencySamples() == dropforge::InstrumentCore::latencySamples, "latency is stable across rates/block lengths");
    }
    std::cout << "output_ramp_max_error=" << maximumGainError << " latency_samples=" << dropforge::InstrumentCore::latencySamples << '\n';
}

void headlessParityTests()
{
    for (const double rate : {44100.0, 48000.0, 192000.0})
    {
        DropforgeProcessor processor;
        dropforge::InstrumentCore core;
        processor.prepareToPlay(rate, 512);
        core.setLayersEnabled(true, true);
        core.prepare(rate);
        int index = 0;
        for (const int count : {0, 1, 17, 512, 3, 1024, 4096})
        {
            juce::AudioBuffer<float> host(2, count), offline(2, count);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 45 + index, static_cast<juce::uint8>(127)), 0);
            midi.addEvent(juce::MidiMessage::pitchWheel(1, 10000), count / 2);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), count / 2);
            const auto db = index++ % 2 == 0 ? -9.0f : 6.0f;
            setOutput(processor, db);
            core.setOutputDb(outputDb(processor)); // Use the exact APVTS-normalised value.
            core.setBassParameters(140.0f, 0.5f);
            auto hostMidi = midi;
            processor.processBlock(host, hostMidi);
            dropforge::processMidiBlock(core, offline, midi);
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < count; ++sample)
                    require(host.getSample(channel, sample) == offline.getSample(channel, sample),
                            "headless core exactly matches plugin MIDI/gain across changing blocks");
        }
    }
    std::cout << "headless_plugin_sample_parity=exact\n";
}

void routingBridgeTests()
{
    for (const double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    for (int initial = 0; initial < 4; ++initial)
    {
        DropforgeProcessor processor;
        dropforge::InstrumentCore reference;
        const bool bassOn = (initial & 1) != 0, rhythmOn = (initial & 2) != 0;
        setToggle(processor, dropforge::parameters::bassEnabled, bassOn);
        setToggle(processor, dropforge::parameters::rhythmEnabled, rhythmOn);
        processor.prepareToPlay(rate, 512);
        reference.setLayersEnabled(bassOn, rhythmOn);
        reference.prepare(rate);
        juce::MemoryBlock saved;
        processor.getStateInformation(saved);
        int index = 0;
        for (const int count : {0, 1, 17, 73, 512, 3, 1024, 4096})
        {
            if (index == 3)
            {
                setToggle(processor, dropforge::parameters::bassEnabled, !bassOn);
                setToggle(processor, dropforge::parameters::rhythmEnabled, !rhythmOn);
                reference.setLayersEnabled(!bassOn, !rhythmOn);
            }
            if (index == 4)
            {
                // Restore through the real host state path while keys are held.
                processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
                reference.setLayersEnabled(bassOn, rhythmOn);
            }
            if (index == 5)
            {
                processor.reset();
                reference.reset(0.0f);
            }
            if (index == 6) { processor.requestPanic(); reference.panic(); }
            processor.showPage(index % 2); // Page has no sound counterpart.
            juce::AudioBuffer<float> host(2, count), expected(2, count);
            juce::MidiBuffer midi;
            if (index < 3 || index == 5)
                midi.addEvent(juce::MidiMessage::noteOn(1, index == 2 ? 57 : 69, static_cast<juce::uint8>(127)), 0);
            if (index == 3)
            {
                midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
                midi.addEvent(juce::MidiMessage::noteOff(1, 69), count / 2);
            }
            if (index == 4)
            {
                midi.addEvent(juce::MidiMessage::pitchWheel(1, 10000), 0);
                midi.addEvent(juce::MidiMessage::noteOn(2, 62, static_cast<juce::uint8>(96)), count / 2);
            }
            auto hostMidi = midi;
            processor.processBlock(host, hostMidi);
            dropforge::processMidiBlock(reference, expected, midi);
            require(processor.getLatencySamples() == dropforge::InstrumentCore::latencySamples, "enable and recall preserve fixed latency");
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < count; ++sample)
                    require(host.getSample(channel, sample) == expected.getSample(channel, sample),
                            "host enables, held recall and reset match explicit renderer policy exactly");
            if (index == 7) require(host.getMagnitude(count - 256, 256) == 0.0f, "panic clears held notes in all modes");
            ++index;
        }
    }
    // Host solo mode must match the explicit newest-note routing policy.
    DropforgeProcessor bassProcessor;
    setToggle(bassProcessor, dropforge::parameters::rhythmEnabled, false);
    bassProcessor.prepareToPlay(48000.0, 512);
    dropforge::InstrumentCore legacy;
    legacy.setLayersEnabled(true, false);
    legacy.prepare(48000.0);
    juce::AudioBuffer<float> bassAudio(2, 512), legacyAudio(2, 512);
    for (const auto& message : {juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127)),
                               juce::MidiMessage::noteOn(1, 57, static_cast<juce::uint8>(96)),
                               juce::MidiMessage::noteOff(1, 69), juce::MidiMessage::noteOff(1, 57)})
    {
        juce::MidiBuffer midi;
        midi.addEvent(message, 0);
        bassProcessor.processBlock(bassAudio, midi);
        dropforge::dispatchMidi(legacy, message.getRawData(), message.getRawDataSize());
        legacy.process(legacyAudio.getWritePointer(0), legacyAudio.getWritePointer(1), 512, dropforge::LayerMask::bass);
        for (int sample = 0; sample < 512; ++sample)
            require(bassAudio.getSample(0, sample) == legacyAudio.getSample(0, sample)
                    && bassAudio.getSample(0, sample) == bassAudio.getSample(1, sample),
                    "explicit host solo uses newest-note samples, including overlap/handover");
    }
    std::cout << "host_routing_matrix=24 rate/mode cases; live automation/held recall/reset/panic exact\n";
}

void editorTests(const juce::String& snapshot)
{
    namespace ids = dropforge::parameters;
    DropforgeProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> created(processor.createEditor());
    auto* editor = dynamic_cast<DropforgeEditor*>(created.get());
    require(editor && editor->getWidth() == 1100 && editor->getHeight() == 700, "suite editor opens at 1100 x 700");
    // Offscreen: the editor never gets a desktop window, so tests cannot
    // appear on screen or steal focus. Snapshots are painted into images.
    auto& anvil = editor->tab(0);
    auto& furnace = editor->tab(1);
    auto& anvilPage = editor->instrumentPage(0);
    auto& furnacePage = editor->instrumentPage(1);
    auto& output = editor->outputKnob();
    auto& footer = editor->helpLine();
    require(anvil.getButtonText() == "Anvil" && furnace.getButtonText() == "Furnace", "tabs in signal order");
    std::vector<juce::Component*> everything;
    collect(*editor, everything);
    for (auto* component : everything)
    {
        require(dynamic_cast<juce::ToggleButton*>(component) == nullptr, "no stock checkboxes");
        if (auto* button = dynamic_cast<juce::Button*>(component))
        {
            // Offscreen there is no window, so build the handler directly (public on Component).
            const auto handler = static_cast<juce::Component*>(button)->createAccessibilityHandler();
            require(button->getTooltip().isNotEmpty() && button->getWantsKeyboardFocus() && button->getTitle().isNotEmpty()
                && handler && handler->getHelp().isNotEmpty(), "all buttons have a name, help and keyboard focus");
        }
    }
    require(anvil.getToggleState() && anvilPage.isVisible() && !furnacePage.isVisible(), "Anvil page open initially");
    require(furnacePage.optionSwitch("Mono") && furnacePage.optionSwitch("Drop") && !anvilPage.optionSwitch("Mono"),
            "Mono and Drop live only on the Furnace page");

    // Navigation and power.
    const auto outputBounds = output.getBounds();
    furnace.onClick();
    require(processor.currentPage() == 1 && furnacePage.isVisible() && !anvilPage.isVisible(), "Furnace tab opens its page");
    require(output.getBounds() == outputBounds && toggle(processor, ids::bassEnabled), "page change keeps the header and power");
    rightClick(anvil, {150.0f, 30.0f});
    require(!toggle(processor, ids::bassEnabled) && processor.currentPage() == 1, "right-click anywhere on a tab toggles power without opening it");
    refreshUi();
    require(!anvil.isInstrumentEnabled() && !anvil.createAccessibilityHandler()->getCurrentState().isChecked(), "tab shows power off");
    require(anvil.createAccessibilityHandler()->getActions().invoke(juce::AccessibilityActionType::toggle)
            && toggle(processor, ids::bassEnabled), "accessible power toggle changes the same host parameter");
    auto& furnacePower = furnacePage.powerSwitch();
    require(furnacePower.getToggleState(), "page power switch shows the host state");
    furnacePower.setToggleState(false, juce::sendNotificationSync);
    require(!toggle(processor, ids::rhythmEnabled), "visible page power switch turns the instrument off");
    rightClick(furnace);
    require(toggle(processor, ids::rhythmEnabled) && furnacePower.getToggleState(), "tab and page power stay in step");
    anvil.onClick();
    require(anvilPage.isVisible() && anvilPage.levelKnob().isEnabled(), "a page stays editable while its instrument is off");

    // Modes, Destruction and options.
    for (int page = 0; page < 2; ++page)
    {
        auto& instrument = editor->instrumentPage(page);
        const auto* modeId = page == 0 ? ids::bassMode : ids::rhythmMode;
        const auto* destroyId = page == 0 ? ids::bassDestroy : ids::rhythmDestroy;
        auto& modes = instrument.modeSelector();
        require(modes.selectedMode() == 0 && modes.button(0).getToggleState(), "mode A selected by default");
        modes.button(2).onClick();
        require(choice(processor, modeId) == 2 && modes.selectedMode() == 2 && modes.button(2).getToggleState()
                && !modes.button(0).getToggleState(), "a mode button sets the mode parameter");
        auto* parameter = processor.parameterState().getParameter(modeId);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(3.0f));
        require(modes.selectedMode() == 3 && modes.button(3).getToggleState(), "host automation moves the mode selector");
        auto& destruction = instrument.destructionSwitch();
        destruction.setToggleState(true, juce::sendNotificationSync);
        require(toggle(processor, destroyId), "Destruction switch writes its parameter");
        refreshUi();
        require(editor->tab(page).getDescription().contains("D Alloy"), "the tab names its current mode");
        destruction.setToggleState(false, juce::sendNotificationSync);
        modes.button(0).onClick();
        require(choice(processor, modeId) == 0 && !toggle(processor, destroyId), "mode and Destruction return to defaults");
    }
    for (const auto& [name, id] : {std::pair{"Mono", ids::rhythmMono}, std::pair{"Drop", ids::rhythmDrop}})
    {
        auto& option = *furnacePage.optionSwitch(name);
        option.setToggleState(true, juce::sendNotificationSync);
        require(toggle(processor, id), "Furnace option writes its parameter");
        setToggle(processor, id, false);
        require(!option.getToggleState(), "host changes reach the option switch");
    }

    // Knobs: reset, typed values and the value field.
    auto& anvilLevel = anvilPage.levelKnob();
    auto& furnaceLevel = furnacePage.levelKnob();
    for (auto* knob : {&anvilLevel, &furnaceLevel, &output})
    {
        if (knob == &furnaceLevel) furnace.onClick(); else anvil.onClick();
        require(knob->getTextBoxPosition() == juce::Slider::NoTextBox && !knob->valueEditor().isVisible(), "no persistent numeric field");
        knob->setValue(-6.0, juce::sendNotificationSync);
        const juce::MouseEvent doubleClick(juce::Desktop::getInstance().getMainMouseSource(), {}, {},
            0, 0, 0, 0, 0, knob, knob, {}, {}, {}, 2, false);
        knob->mouseDoubleClick(doubleClick);
        require(knob->getValue() == 0.0 && !knob->valueEditor().isVisible(), "double-click resets default without opening entry");
        auto& entry = knob->valueEditor();
        leftClick(*knob, {knob->getWidth() * 0.5f, knob->getHeight() - 4.0f});
        require(entry.isVisible(), "clicking the value opens the entry field");
        entry.onEscapeKey();
        rightClick(*knob, {});
        require(entry.isVisible(), "right-click opens the entry field");
        require(entry.getHighlightedRegion().getLength() == entry.getText().length(), "entry selects the full current value");
        entry.setText("-3.125 dB", false); entry.onReturnKey();
        require(std::abs(knob->getValue() + 3.125) < 1e-5 && !entry.isVisible(), "Enter commits decimal units and hides field");
        knob->openEntry(); entry.onReturnKey();
        require(std::abs(knob->getValue() + 3.125) < 1e-5, "confirming the shown text keeps the exact value");
        knob->openEntry(); entry.setText("-4.25", false); entry.onFocusLost();
        require(std::abs(knob->getValue() + 4.25) < 1e-5 && !entry.isVisible(), "focus loss commits and hides field");
        knob->openEntry(); entry.setText("-5.5", false); knob->commitIfOutside(&editor->panicButton());
        require(std::abs(knob->getValue() + 5.5) < 1e-5 && !entry.isVisible(), "click-away commits even on a non-focus-taking control");
        knob->openEntry(); entry.setText("-10", false); entry.onEscapeKey();
        require(std::abs(knob->getValue() + 5.5) < 1e-5, "Escape cancels");
        for (const auto* invalid : {"garbage", "99999", "nan", "2.2.2"})
        {
            knob->openEntry(); entry.setText(invalid, false); entry.onReturnKey();
            require(std::abs(knob->getValue() + 5.5) < 1e-5 && !entry.isVisible(), "invalid values preserve setting and close field");
        }
        require(knob->getTooltip().contains("Enter a number from"), "the help line states the range after invalid input");
        knob->setValue(0.0, juce::sendNotificationSync);
    }
    anvilLevel.openEntry();
    furnace.onClick();
    require(!anvilLevel.valueEditor().isVisible(), "changing page closes a typing field");
    output.setValue(-9.0, juce::sendNotificationSync);
    require(outputDb(processor) == -9.0f, "output writes through APVTS");
    setOutput(processor, 3.0f);
    require(std::abs(output.getValue() - 3.0) < 1e-5, "host updates output knob");

    // Recall: the sound comes back, the page does not.
    anvilLevel.setValue(-4.0, juce::sendNotificationSync);
    furnaceLevel.setValue(-7.0, juce::sendNotificationSync);
    furnacePage.modeSelector().button(1).onClick();
    rightClick(furnace);
    furnace.onClick();
    juce::MemoryBlock settings;
    processor.getStateInformation(settings);
    DropforgeProcessor recalled;
    recalled.setStateInformation(settings.getData(), static_cast<int>(settings.getSize()));
    require(recalled.currentPage() == 0 && !toggle(recalled, ids::rhythmEnabled) && choice(recalled, ids::rhythmMode) == 1
        && recalled.parameterState().getRawParameterValue(ids::bassLevel)->load() == -4.0f
        && recalled.parameterState().getRawParameterValue(ids::rhythmLevel)->load() == -7.0f,
        "power, modes and levels restore; the page is a view preference only");
    furnacePage.modeSelector().button(0).onClick();
    setToggle(processor, ids::rhythmEnabled, true);

    // Layout: the canvas fills the window at 75-200 % and every part stays inside its parent.
    auto& canvas = editor->canvasComponent();
    for (const auto size : {juce::Point<int>{825, 525}, juce::Point<int>{2200, 1400}, juce::Point<int>{1300, 700}, juce::Point<int>{1100, 700}})
    {
        editor->setSize(size.x, size.y);
        const auto shown = editor->getLocalArea(&canvas, canvas.getLocalBounds());
        require(editor->getLocalBounds().expanded(1).contains(shown)
                && (std::abs(shown.getWidth() - size.x) <= 1 || std::abs(shown.getHeight() - size.y) <= 1),
                "the scaled canvas fills the window without cropping");
        for (int page = 0; page < 2; ++page)
        {
            editor->tab(page).onClick();
            std::vector<juce::Component*> parts;
            collect(canvas, parts, true);
            for (auto* part : parts)
            {
                const auto fits = part->getParentComponent()->getLocalBounds().contains(part->getBounds());
                if (!fits) std::cerr << "outside its parent: " << part->getTitle() << ' ' << part->getBounds().toString() << '\n';
                require(fits, "every control fits inside its parent");
            }
            require(output.getBounds() == outputBounds && footer.getHeight() <= 32, "stable header and one-line footer");
        }
    }
    require(editor->getConstrainer()->getFixedAspectRatio() > 1.57 && editor->getConstrainer()->getMinimumWidth() == 825
            && editor->getConstrainer()->getMaximumWidth() == 2200, "resizable 75-200 % at a fixed aspect ratio");
    refreshUi();
    require(footer.getText().isNotEmpty() && !footer.getText().containsChar('\n'), "footer is one help line");

    setToggle(processor, ids::bassEnabled, true);
    processor.prepareToPlay(48000, 512);
    juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127)), 0);
    processor.processBlock(audio, midi); editor->panicButton().onClick(); processor.processBlock(audio, midi);
    // Release, latency and the master clipper filter's ringing, as above.
    constexpr int quietFrom = 288 + dropforge::InstrumentCore::latencySamples + dsp_core::Oversampler4xFloat::latencySamples;
    require(audio.getMagnitude(0, quietFrom, 512 - quietFrom) == 0.0f, "Panic clears audio through realtime request");

    // Routing keyboard: the audio thread publishes held keys, the editor lights them.
    auto& keyboard = editor->routingKeyboard();
    const auto playChord = [&]
    {
        midi.clear();
        for (const int note : {40, 52, 55, 59})
            midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(audio, midi);
        refreshUi();
    };
    refreshUi();
    require(!keyboard.isHeld(40) && processor.heldKeys().bassNote == -1, "nothing lit after Panic");
    playChord();
    const auto keys = processor.heldKeys();
    require(keys.bassNote == 40 && keys.high == 0 && keys.low == ((1ull << 40) | (1ull << 52) | (1ull << 55) | (1ull << 59)),
            "the processor publishes held keys and Anvil's lowest key");
    require(keyboard.isHeld(40) && keyboard.isHeld(59) && !keyboard.isHeld(41), "the routing keyboard lights the held chord");
    setToggle(processor, ids::bassEnabled, false);
    midi.clear(); processor.processBlock(audio, midi);
    require(processor.heldKeys().bassNote == -1, "no Anvil key while Anvil is off");
    setToggle(processor, ids::bassEnabled, true);
    midi.clear(); processor.processBlock(audio, midi);
    require(processor.heldKeys().bassNote == 40, "Anvil's key returns with its power");
    if (snapshot.isNotEmpty())
    {
        setOutput(processor, 0);
        for (const auto* id : {ids::bassLevel, ids::rhythmLevel})
            processor.parameterState().getParameter(id)->setValueNotifyingHost(processor.parameterState().getParameter(id)->getDefaultValue());
        const juce::File base(snapshot);
        const auto save = [&](const juce::String& name)
        {
            refreshUi();
            juce::Image image(juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true, juce::SoftwareImageType{});
            { juce::Graphics g(image); editor->paintEntireComponent(g, false); }
            juce::MemoryOutputStream stream;
            const auto file = base.getSiblingFile("ui-" + name + ".png");
            require(juce::PNGImageFormat{}.writeImageToStream(image, stream) && file.replaceWithData(stream.getData(), stream.getDataSize()),
                    "UI snapshot saved");
        };
        for (const int percent : {100, 200})
        {
            editor->setSize(1100 * percent / 100, 700 * percent / 100);
            for (int page = 0; page < 2; ++page)
            {
                editor->tab(page).onClick();
                save(juce::String(page == 0 ? "anvil" : "furnace") + "-" + juce::String(percent));
            }
        }
        // Every mode finish once, with Destruction and an option lit, for review.
        editor->setSize(1100, 700);
        setToggle(processor, ids::rhythmDestroy, true);
        setToggle(processor, ids::rhythmMono, true);
        setToggle(processor, ids::bassEnabled, false);
        for (int mode = 1; mode < 4; ++mode)
        {
            for (const auto* id : {ids::bassMode, ids::rhythmMode})
            {
                auto* parameter = processor.parameterState().getParameter(id);
                parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(mode)));
            }
            for (int page = 0; page < 2; ++page)
            {
                editor->tab(page).onClick();
                save(juce::String(page == 0 ? "anvil" : "furnace") + "-mode-" + juce::String::charToString(juce::juce_wchar('a' + mode)));
            }
        }
    }
    std::cout << "suite pages, power, modes, Destruction, options, value entry, recall and layout passed\n";
}

void keyboardTests()
{
    DropforgeProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> created(processor.createEditor());
    auto* editor = dynamic_cast<DropforgeEditor*>(created.get());
    require(editor != nullptr, "suite editor for keyboard checks");
    auto& furnace = editor->tab(1);
    bool completed = false;
    juce::MessageManager::callAsync([&]
    {
        require(furnace.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)), "Enter opens a tab");
        juce::Timer::callAfterDelay(100, [&]
        {
            require(processor.currentPage() == 1 && toggle(processor, dropforge::parameters::rhythmEnabled), "keyboard page change is audio neutral");
            require(furnace.keyPressed(juce::KeyPress(juce::KeyPress::returnKey, juce::ModifierKeys::altModifier, 0)), "Alt+Enter toggles power");
            require(!toggle(processor, dropforge::parameters::rhythmEnabled) && processor.currentPage() == 1, "keyboard power leaves the page open");
            require(editor->keyPressed(juce::KeyPress('1', juce::ModifierKeys::ctrlModifier, 0)) && processor.currentPage() == 0, "Ctrl+1 opens Anvil");
            require(editor->keyPressed(juce::KeyPress('2', juce::ModifierKeys::ctrlModifier, 0)) && processor.currentPage() == 1, "Ctrl+2 opens Furnace");
            completed = true;
            juce::MessageManager::getInstance()->stopDispatchLoop();
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    require(completed, "keyboard checks completed");
}

}

int main(int argc, char** argv)
{
   #if JUCE_LINUX
    X11TestDisplay display;
   #endif
    juce::ScopedJuceInitialiser_GUI gui;
    stateTests();
    processorTests();
    headlessParityTests();
    routingBridgeTests();
    editorTests(argc > 1 ? juce::String(argv[1]) : juce::String{});
    keyboardTests();
    return 0;
}
