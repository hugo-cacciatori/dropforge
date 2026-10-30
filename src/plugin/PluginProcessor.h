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
    // Held keys for the routing keyboard, published after every block.
    dropforge::InstrumentCore::Keys heldKeys() const noexcept
    {
        return {keysLow.load(std::memory_order_relaxed), keysHigh.load(std::memory_order_relaxed), bassKey.load(std::memory_order_relaxed),
                leadKey.load(std::memory_order_relaxed)};
    }
    // Which suite page the editor shows (0 Anvil, 1 Furnace, 2 Blade, in signal order).
    // A per-session view preference only: it is not saved and never changes the sound.
    int currentPage() const noexcept { return page.load(std::memory_order_relaxed); }
    // Where the mod wheel is now (0-1), CC1 included, for the footer.
    float wheelPosition() const noexcept { return wheelNow.load(std::memory_order_relaxed); }
    void showPage(int index) noexcept { page.store(index, std::memory_order_relaxed); }

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
    std::atomic<float>* bassMode = nullptr;
    std::atomic<float>* rhythmMode = nullptr;
    std::atomic<float>* rhythmMono = nullptr;
    std::atomic<float>* rhythmDrop = nullptr;
    std::atomic<float>* bassDestroy = nullptr;
    std::atomic<float>* rhythmDestroy = nullptr;
    std::atomic<float>* leadEnabled = nullptr;
    std::atomic<float>* leadLevel = nullptr;
    std::atomic<float>* leadMode = nullptr;
    std::atomic<float>* leadDestroy = nullptr;
    std::atomic<float>* leadPortamento = nullptr;
    std::atomic<float>* leadPortamentoMs = nullptr;
    std::atomic<float>* leadOctave = nullptr;
    std::atomic<float>* bendRangeChoice = nullptr;
    std::atomic<float>* glideEnabled = nullptr;
    std::atomic<float>* glideTime = nullptr;
    std::atomic<float>* impactEnabled = nullptr;
    std::atomic<float>* wheelModeChoice = nullptr;
    std::atomic<float>* wheelAmountValue = nullptr;
    std::atomic<float> wheelNow{0.0f};
    // The FX page's values, read every block (SPEC section 8).
    std::array<std::atomic<float>*, 43> fxValues{};
    void applyFx() noexcept;
    void applyModes() noexcept;
    double preparedRate = 44100.0;
    std::atomic<bool> panicRequested{false};
    std::atomic<float> meterPeakLeft{0.0f}, meterPeakRight{0.0f}, meterGlueDb{0.0f}, meterLimiterDb{0.0f};
    void publishMeters() noexcept;
    std::atomic<int> page{0};
    std::atomic<std::uint64_t> keysLow{0}, keysHigh{0};
    std::atomic<int> bassKey{-1}, leadKey{-1};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DropforgeProcessor)
};
