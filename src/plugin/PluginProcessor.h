#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "InstrumentCore.h"
#include "Parameters.h"
#include <atomic>

class DropforgeProcessor final : public juce::AudioProcessor
{
public:
    DropforgeProcessor();
    const juce::String getName() const override { return JucePlugin_Name; }
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return dropforge::Engine::releaseSeconds + dropforge::InstrumentCore::latencySamples / preparedRate; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int size) override;
    juce::AudioProcessorValueTreeState& parameterState() noexcept { return state; }
    void requestPanic() noexcept { panicRequested.store(true, std::memory_order_relaxed); }
    // Meter readings for the editor (any thread, lock-free): the highest
    // sample peaks and deepest master gain reduction since the last call.
    dropforge::InstrumentCore::Meters takeMeters() noexcept
    {
        return {meterPeakLeft.exchange(0.0f, std::memory_order_relaxed), meterPeakRight.exchange(0.0f, std::memory_order_relaxed),
                meterGlueDb.exchange(0.0f, std::memory_order_relaxed), meterLimiterDb.exchange(0.0f, std::memory_order_relaxed)};
    }
    // Which layer page the editor shows. A per-session view preference only:
    // it is not saved and never changes the sound.
    bool isRhythmPageSelected() const noexcept { return rhythmPageSelected.load(std::memory_order_relaxed); }
    void selectRhythmPage(bool selected) noexcept { rhythmPageSelected.store(selected, std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();
    float readOutputDb() const noexcept;
    static bool readToggle(const std::atomic<float>* parameter, bool fallback) noexcept;
    dropforge::InstrumentCore core;
    juce::AudioProcessorValueTreeState state;
    std::atomic<float>* outputDb = nullptr;
    std::atomic<float>* bassLevel = nullptr;
    std::atomic<float>* rhythmLevel = nullptr;
    std::atomic<float>* bassEnabled = nullptr;
    std::atomic<float>* rhythmEnabled = nullptr;
    double preparedRate = 44100.0;
    std::atomic<bool> panicRequested{false};
    std::atomic<float> meterPeakLeft{0.0f}, meterPeakRight{0.0f}, meterGlueDb{0.0f}, meterLimiterDb{0.0f};
    void publishMeters() noexcept;
    std::atomic<bool> rhythmPageSelected{false};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DropforgeProcessor)
};
