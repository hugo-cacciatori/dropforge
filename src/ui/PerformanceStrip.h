#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "NumericKnob.h"
#include "SuiteControls.h"
#include "plugin/Parameters.h"

namespace dropforge::ui
{
// The footer's performance strip (SPEC section 6), on every page: Impact,
// Glide and its time, Bend Range, and the mod wheel's mode and Amount. The
// Amount dial carries a dot where the wheel is now, so a hardware wheel (CC1)
// shows even though it never changes the saved value.
class PerformanceStrip final : public juce::Component
{
public:
    explicit PerformanceStrip(juce::AudioProcessorValueTreeState& state)
        : impact("Impact", SwitchButton::Look::option), glide("Glide", SwitchButton::Look::option),
          bend(*state.getParameter(parameters::bendRange), "Bend Range",
               "how far the pitch wheel bends, in semitones each way: 2 for expression, 12 or 24 for dive bombs and drops.",
               {"2", "5", "7", "12", "24"}, 0.0f),
          wheel(*state.getParameter(parameters::wheelMode), "Wheel",
                "what the mod wheel does: Roar sweeps a growl on every instrument (move it for wubs); Vibrato adds a 5 Hz pitch wobble.",
                {"Roar", "Vibrato"}, 0.0f),
          impactAttachment(state, parameters::impactOn, impact), glideAttachment(state, parameters::glideOn, glide),
          glideTimeAttachment(state, parameters::glideMs, glideTime), amountAttachment(state, parameters::wheelAmount, amount)
    {
        setTitle("Performance");
        setFocusContainerType(FocusContainerType::focusContainer);
        impact.setTooltip("Impact: harder hits crack harder, a short accent of drive and level on each key you strike, scaled by velocity.");
        glide.setTooltip("Glide: Anvil and Furnace slide to each new note or chord instead of jumping. Blade uses its own Portamento.");
        configure(glideTime, *state.getParameter(parameters::glideMs), "Glide time",
                  "Glide Time: how long a slide takes. Double-click resets; click the value to type.", " ms", 0);
        configure(amount, *state.getParameter(parameters::wheelAmount), "Wheel amount",
                  "Wheel Amount: where the mod wheel sits; a hardware wheel (CC1) moves it live, shown by the dot. "
                  "Double-click resets; click the value to type.", "", 2);
        for (auto* button : {&impact, &glide})
        {
            button->setAccent(colours::text);
            button->setHelpText(button->getTooltip());
        }
        for (auto* child : std::initializer_list<juce::Component*>{&impact, &glide, &glideTime, &bend, &wheel, &amount})
            addAndMakeVisible(child);
    }
    // The wheel's live position (0-1), from the processor at 30 Hz.
    void showWheel(float position) { amount.setMarker(std::abs(position - static_cast<float>(amount.getValue())) > 0.004f ? position : -1.0f); }

    SwitchButton& impactSwitch() noexcept { return impact; }
    SwitchButton& glideSwitch() noexcept { return glide; }
    NumericKnob& glideTimeKnob() noexcept { return glideTime; }
    ChoiceSelector& bendSelector() noexcept { return bend; }
    ChoiceSelector& wheelSelector() noexcept { return wheel; }
    NumericKnob& wheelAmountKnob() noexcept { return amount; }
    std::vector<NumericKnob*> knobs() { return {&glideTime, &amount}; }

    void paint(juce::Graphics& g) override
    {
        g.setColour(colours::dimText);
        g.setFont(titleFont(16.0f));
        for (const auto& [text, area] : captions)
            g.drawText(text, area, juce::Justification::centredRight, false);
    }
    void resized() override
    {
        auto row = getLocalBounds();
        const auto take = [&row](int width) { auto area = row.removeFromLeft(width); row.removeFromLeft(10); return area; };
        captions.clear();
        impact.setBounds(take(118));
        glide.setBounds(take(108));
        glideTime.setBounds(take(104));
        row.removeFromLeft(16);
        captions.push_back({"BEND", take(48)});
        bend.setBounds(take(190));
        row.removeFromLeft(16);
        captions.push_back({"WHEEL", take(58)});
        wheel.setBounds(take(176));
        amount.setBounds(take(92));
    }

private:
    static void configure(NumericKnob& knob, juce::RangedAudioParameter& parameter, const juce::String& title, const juce::String& help,
                          const juce::String& suffix, int decimals)
    {
        knob.setCompact(true);
        knob.setTitle(title);
        knob.setTooltip(help);
        knob.setHelpText(help);
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setNumDecimalPlacesToDisplay(decimals);
        knob.setTextValueSuffix(suffix);
        knob.setPopupDisplayEnabled(false, false, nullptr);
        knob.setWantsKeyboardFocus(true);
        knob.setColour(juce::Slider::rotarySliderFillColourId, colours::text.withAlpha(0.8f));
        knob.setDoubleClickReturnValue(true, parameter.convertFrom0to1(parameter.getDefaultValue()));
        knob.onEntryGesture = [&parameter](bool begin) { if (begin) parameter.beginChangeGesture(); else parameter.endChangeGesture(); };
    }
    SwitchButton impact, glide;
    ChoiceSelector bend, wheel;
    NumericKnob glideTime, amount;
    juce::AudioProcessorValueTreeState::ButtonAttachment impactAttachment, glideAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment glideTimeAttachment, amountAttachment;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> captions;
};
}
