// The Standalone application (SPEC section 10). It behaves exactly like
// JUCE's default Standalone app, plus one flag for release checks:
//
//   Dropforge.exe --self-test
//
// creates the processor and editor offscreen (never a window, never an audio
// device), plays short phrases through every mode pair with Blade on, checks the sound and
// the saved state, prints one line per check and exits 0 on success, 1 on
// any failure.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <cmath>
#include <iostream>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
class SelfTest
{
public:
    int run()
    {
        std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
        check(processor != nullptr, "processor created");
        if (processor == nullptr) return finish();
        editorRenders(*processor);
        processor->setRateAndBufferSizeDetails(rate, blockSize);
        processor->prepareToPlay(rate, blockSize);
        set(*processor, "lead_on", 1.0f); // every phrase with all three instruments
        for (const bool destroy : {false, true})
            for (int anvil = 0; anvil < 4; ++anvil)
                for (int furnace = 0; furnace < 4; ++furnace)
                {
                    set(*processor, "bass_mode", static_cast<float>(anvil));
                    set(*processor, "rhythm_mode", static_cast<float>(furnace));
                    set(*processor, "bass_destroy", destroy ? 1.0f : 0.0f);
                    set(*processor, "rhythm_destroy", destroy ? 1.0f : 0.0f);
                    processor->reset();
                    phrase(*processor, juce::String("mode ") + static_cast<char>('A' + anvil) + static_cast<char>('A' + furnace)
                                           + (destroy ? " destroyed" : ""));
                }
        stateRoundTrip(*processor);
        processor->releaseResources();
        return finish();
    }

private:
    static constexpr double rate = 48000.0;
    static constexpr int blockSize = 512;
    static constexpr float ceiling = 0.89125094f; // the master's -1 dBFS sample-peak ceiling

    void check(bool passed, const juce::String& what)
    {
        std::cout << (passed ? "PASS " : "FAIL ") << what << std::endl;
        if (!passed) ++failures;
    }
    int finish()
    {
        std::cout << (failures == 0 ? "self-test passed" : "self-test FAILED: " + juce::String(failures) + " check(s)") << std::endl;
        return failures == 0 ? 0 : 1;
    }
    static juce::RangedAudioParameter* parameter(juce::AudioProcessor& processor, const juce::String& id)
    {
        for (auto* candidate : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(candidate); ranged != nullptr && ranged->paramID == id)
                return ranged;
        return nullptr;
    }
    void set(juce::AudioProcessor& processor, const juce::String& id, float value)
    {
        auto* target = parameter(processor, id);
        if (target == nullptr) { check(false, "parameter " + id + " exists"); return; }
        target->setValueNotifyingHost(target->convertTo0to1(value));
    }

    // The editor is built and painted into an image only: no desktop window.
    void editorRenders(juce::AudioProcessor& processor)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditorIfNeeded());
        check(editor != nullptr && !editor->isOnDesktop() && editor->getWidth() > 0 && editor->getHeight() > 0,
              "editor created offscreen");
        if (editor == nullptr) return;
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds());
        int painted = 0;
        for (int y = 0; y < image.getHeight(); y += 7)
            for (int x = 0; x < image.getWidth(); x += 7)
                painted += image.getPixelAt(x, y).getBrightness() > 0.2f ? 1 : 0;
        check(painted > 100, "editor paints its interface");
        processor.editorBeingDeleted(editor.get());
    }

    // An E2 power chord, a change to B1, then release: the sound must be
    // finite, clearly audible, under the ceiling and silent after its tail.
    void phrase(juce::AudioProcessor& processor, const juce::String& name)
    {
        juce::AudioBuffer<float> audio(2, blockSize);
        juce::MidiBuffer midi;
        float peak = 0.0f, tailPeak = 0.0f;
        bool finite = true;
        constexpr int blocks = 120; // 1.28 s
        for (int block = 0; block < blocks; ++block)
        {
            midi.clear();
            if (block == 0)
                for (const int note : {40, 47, 52, 55}) midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(110)), 0);
            if (block == 50)
            {
                for (const int note : {40, 47, 52, 55}) midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
                for (const int note : {35, 42, 47, 50}) midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(110)), 0);
            }
            if (block == 90)
                for (const int note : {35, 42, 47, 50}) midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
            audio.clear();
            processor.processBlock(audio, midi);
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto sample = audio.getSample(channel, i);
                    finite = finite && std::isfinite(sample);
                    peak = std::max(peak, std::abs(sample));
                    if (block >= 100) tailPeak = std::max(tailPeak, std::abs(sample));
                }
        }
        check(finite && peak > 0.05f && peak <= ceiling && tailPeak < 1.0e-6f,
              name + ": peak " + juce::String(juce::Decibels::gainToDecibels(peak), 1) + " dBFS, silent after release");
    }

    void stateRoundTrip(juce::AudioProcessor& processor)
    {
        set(processor, "bass_mode", 2.0f);
        set(processor, "rhythm_mode", 3.0f);
        set(processor, "rhythm_drop", 1.0f);
        set(processor, "bass_level", -4.5f);
        juce::MemoryBlock saved;
        processor.getStateInformation(saved);
        std::unique_ptr<juce::AudioProcessor> recalled(createPluginFilter());
        recalled->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        bool same = true;
        for (auto* original : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(original))
                if (auto* copy = parameter(*recalled, ranged->paramID); copy == nullptr || copy->getValue() != ranged->getValue())
                    same = false;
        check(same && saved.getSize() > 0, "saved state restores every parameter");
    }

    int failures = 0;
};

// JUCE's default Standalone app (juce_audio_plugin_client_Standalone.cpp),
// with the self-test branch in initialise().
class DropforgeStandaloneApp final : public juce::JUCEApplication
{
public:
    DropforgeStandaloneApp()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = JucePlugin_Name;
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "";
        appProperties.setStorageParameters(options);
    }
    const juce::String getApplicationName() override { return JucePlugin_Name; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}

    void initialise(const juce::String& commandLine) override
    {
        if (commandLine.contains("--self-test"))
        {
            setApplicationReturnValue(SelfTest{}.run());
            quit();
            return;
        }
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            pluginHolder = createPluginHolder();
            return;
        }
        mainWindow = std::make_unique<juce::StandaloneFilterWindow>(
            getApplicationName(), juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
            createPluginHolder());
        mainWindow->setVisible(true);
    }
    void shutdown() override
    {
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }
    void systemRequestedQuit() override
    {
        if (pluginHolder != nullptr) pluginHolder->savePluginState();
        if (mainWindow != nullptr) mainWindow->pluginHolder->savePluginState();
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
            juce::Timer::callAfterDelay(100, [] { if (auto* app = JUCEApplicationBase::getInstance()) app->systemRequestedQuit(); });
        else
            quit();
    }

private:
    std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder()
    {
        return std::make_unique<juce::StandalonePluginHolder>(appProperties.getUserSettings(), false, juce::String{}, nullptr,
                                                              juce::Array<juce::StandalonePluginHolder::PluginInOuts>{}, false);
    }
    juce::ApplicationProperties appProperties;
    std::unique_ptr<juce::StandaloneFilterWindow> mainWindow;
    std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
};
}

juce::JUCEApplicationBase* juce_CreateApplication() { return new DropforgeStandaloneApp(); }
