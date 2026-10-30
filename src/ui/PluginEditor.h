#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "InstrumentPage.h"

class DropforgeProcessor;

// The synth suite (SPEC section 6): header, page navigation, the selected
// instrument page and a footer help line. Everything lives on a fixed
// 1100 x 700 design canvas that is scaled as a whole (75-200 %, fixed aspect).
class DropforgeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DropforgeEditor(DropforgeProcessor& processor);
    ~DropforgeEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

    // For the offscreen tests: the canvas and its parts, without a window.
    juce::Component& canvasComponent() noexcept { return canvas; }
    dropforge::ui::InstrumentTab& tab(int page) noexcept { return *tabs[(size_t) page]; }
    dropforge::ui::InstrumentPage& instrumentPage(int page) noexcept { return *pages[(size_t) page]; }
    NumericKnob& outputKnob() noexcept { return output; }
    juce::Button& panicButton() noexcept { return panic; }
    juce::Label& helpLine() noexcept { return hoverHelp; }
    dropforge::ui::RoutingKeyboard& routingKeyboard() noexcept { return keyboard; }
    static constexpr int pageCount = 3; // Anvil, Furnace, Blade: signal order

private:
    struct Canvas final : juce::Component
    {
        explicit Canvas(DropforgeEditor& editor) : owner(editor) {}
        void paint(juce::Graphics& g) override { owner.paintCanvas(g); }
        DropforgeEditor& owner;
    };
    void paintCanvas(juce::Graphics&);
    void layoutCanvas();
    void showAbout();
    void selectPage(int page);
    void timerCallback() override;
    void togglePower(int page);
    DropforgeProcessor& owner;
    Canvas canvas{*this};
    juce::Label outputLabel, hoverHelp;
    NumericKnob output;
    dropforge::ui::OutputMeter meter;
    dropforge::ui::RoutingKeyboard keyboard;
    juce::TextButton mark{"DROPFORGE"}, panic{"Panic"}, about{"i"};
    std::array<std::unique_ptr<dropforge::ui::InstrumentTab>, pageCount> tabs;
    std::array<std::unique_ptr<dropforge::ui::InstrumentPage>, pageCount> pages;
    juce::AudioProcessorValueTreeState::SliderAttachment outputAttachment;
    juce::ScopedMessageBox aboutBox;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DropforgeEditor)
};
