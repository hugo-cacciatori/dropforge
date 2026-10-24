#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "NumericKnob.h"
#include "LayerTab.h"

class DropforgeProcessor;
class DropforgeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DropforgeEditor(DropforgeProcessor& processor);
    ~DropforgeEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
private:
    void showAbout();
    void selectPage(bool furnace);
    void timerCallback() override;
    void toggleLayer(const char* id);
    DropforgeProcessor& owner;
    juce::Label title, levelLabel, outputLabel, hoverHelp;
    NumericKnob anvilLevel, furnaceLevel, output;
    LayerTab anvilTab{"Anvil"}, furnaceTab{"Furnace", true};
    juce::TextButton panic{"Panic"}, help{"Help"};
    juce::AudioProcessorValueTreeState::SliderAttachment anvilAttachment, furnaceAttachment, outputAttachment;
    juce::ScopedMessageBox aboutBox;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DropforgeEditor)
};
