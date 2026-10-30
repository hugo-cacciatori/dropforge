#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "NumericKnob.h"
#include "SuiteControls.h"
#include <memory>
#include <vector>

namespace dropforge::ui
{
// What one instrument page shows. Only controls whose sound exists appear
// (SPEC section 6): options are the instrument's own switches.
struct PageSpec
{
    struct Option { const char* id; const char* name; const char* help; };
    const InstrumentStyle& style;
    const char* powerId;
    const char* modeId;
    const char* levelId;
    const char* destroyId;
    std::vector<Option> options;
};

// An instrument page: header (name, power, role, mode selector) above the
// selected mode's faceplate. Every mode keeps the same controls in the same
// places; only the finish changes with the mode.
class InstrumentPage final : public juce::Component
{
public:
    InstrumentPage(juce::AudioProcessorValueTreeState& state, PageSpec pageSpec)
        : spec(std::move(pageSpec)),
          power(juce::String(spec.style.title) + " power", SwitchButton::Look::power),
          destruction("Destruction", SwitchButton::Look::destruction),
          modes(*state.getParameter(spec.modeId), spec.style, spec.style.title),
          powerAttachment(state, spec.powerId, power),
          destroyAttachment(state, spec.destroyId, destruction),
          levelAttachment(state, spec.levelId, level)
    {
        const juce::String title = spec.style.title;
        setTitle(title + " page");
        setFocusContainerType(FocusContainerType::focusContainer);
        power.setTooltip(title + " power: switches the whole instrument on or off. Right-clicking its tab does the same.");
        destruction.setTooltip(title + " Destruction: turns the current mode into a more extreme version of itself, at about the same loudness.");
        level.setTitle(title + " level");
        level.setTooltip(title + " level. Drag or scroll; Shift for fine; double-click resets to 0 dB; click the value to type. -inf silences it.");
        levelLabel.setText("LEVEL", juce::dontSendNotification);
        levelLabel.setJustificationType(juce::Justification::centred);
        levelLabel.setFont(titleFont(18.0f));
        levelLabel.setColour(juce::Label::textColourId, colours::dimText);
        levelLabel.setInterceptsMouseClicks(false, false);
        level.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        level.setNumDecimalPlacesToDisplay(1);
        level.setTextValueSuffix(" dB");
        level.setDoubleClickReturnValue(true, 0.0);
        level.setPopupDisplayEnabled(false, false, nullptr);
        level.setWantsKeyboardFocus(true);
        auto* parameter = state.getParameter(spec.levelId);
        level.onEntryGesture = [parameter](bool begin) { if (begin) parameter->beginChangeGesture(); else parameter->endChangeGesture(); };
        for (const auto& option : spec.options)
        {
            auto& button = *optionButtons.emplace_back(std::make_unique<SwitchButton>(option.name, SwitchButton::Look::option));
            button.setTooltip(title + " " + option.name + ": " + option.help);
            optionAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, option.id, button));
        }
        for (auto* button : buttons()) button->setHelpText(button->getTooltip());
        level.setHelpText(level.getTooltip());
        for (auto* child : std::initializer_list<juce::Component*>{&power, &modes, &level, &levelLabel, &destruction})
            addAndMakeVisible(child);
        for (auto& button : optionButtons) addAndMakeVisible(*button);
        showMode(modes.selectedMode());
    }

    const PageSpec& pageSpec() const noexcept { return spec; }
    NumericKnob& levelKnob() noexcept { return level; }
    ModeSelector& modeSelector() noexcept { return modes; }
    SwitchButton& powerSwitch() noexcept { return power; }
    SwitchButton& destructionSwitch() noexcept { return destruction; }
    SwitchButton* optionSwitch(const juce::String& name)
    {
        for (auto& button : optionButtons) if (button->getButtonText() == name) return button.get();
        return nullptr;
    }
    std::vector<juce::Button*> buttons()
    {
        std::vector<juce::Button*> all{&power, &destruction};
        for (int i = 0; i < 4; ++i) all.push_back(&modes.button(i));
        for (auto& button : optionButtons) all.push_back(button.get());
        return all;
    }

    // Called by the editor's timer: repaints the faceplate only when the mode changed.
    void showMode(int mode)
    {
        if (mode == shownMode) return;
        shownMode = mode;
        const auto finish = finishFor(spec.style, mode);
        power.setAccent(spec.style.identity);
        for (auto& button : optionButtons) button->setAccent(finish.accent);
        level.setColour(juce::Slider::rotarySliderFillColourId, finish.accent);
        level.repaint();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto finish = finishFor(spec.style, shownMode);
        g.setColour(colours::panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
        g.fillRect(getLocalBounds().removeFromTop(12)); // joins the selected tab to its page
        g.setColour(spec.style.identity);
        g.setFont(titleFont(40.0f));
        g.drawText(spec.style.name, juce::Rectangle<int>(24, 8, 220, 44), juce::Justification::centredLeft, false);
        g.setColour(colours::dimText);
        g.setFont(valueFont(13.0f));
        g.drawText(spec.style.role, juce::Rectangle<int>(24, 52, 400, 18), juce::Justification::centredLeft, true);

        // The faceplate: the mode's own material, accent rim and engraving.
        const auto plate = plateArea().toFloat();
        g.setGradientFill(juce::ColourGradient(finish.plateTop, 0.0f, plate.getY(), finish.plateBottom, 0.0f, plate.getBottom(), false));
        g.fillRoundedRectangle(plate, 12.0f);
        g.setColour(finish.accent.withAlpha(0.55f));
        g.drawRoundedRectangle(plate.reduced(1.0f), 12.0f, 1.5f);
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.drawHorizontalLine(static_cast<int>(plate.getY()) + 3, plate.getX() + 14.0f, plate.getRight() - 14.0f);
        for (const auto corner : {plate.getTopLeft(), plate.getTopRight(), plate.getBottomLeft(), plate.getBottomRight()})
        {
            // Screws: the faceplate reads as hardware bolted into the page.
            const auto centre = corner + juce::Point<float>(corner.x < plate.getCentreX() ? 18.0f : -18.0f,
                                                            corner.y < plate.getCentreY() ? 18.0f : -18.0f);
            g.setColour(juce::Colour(0xff0f1113));
            g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(centre));
            g.setColour(colours::edge);
            g.drawLine(juce::Line<float>(centre.translated(-3.0f, -3.0f), centre.translated(3.0f, 3.0f)), 1.2f);
        }
        g.setColour(finish.accent);
        g.setFont(titleFont(32.0f));
        g.drawText(juce::String(modeLetters[(size_t) shownMode]) + "  " + juce::String(modeNames[(size_t) shownMode]).toUpperCase(),
                   plate.toNearestInt().withTrimmedLeft(44).withTrimmedTop(20).withHeight(38), juce::Justification::centredLeft, false);
        g.setColour(colours::dimText);
        g.setFont(valueFont(14.0f));
        g.drawText(finish.character, plate.toNearestInt().withTrimmedLeft(46).withTrimmedTop(58).withHeight(20),
                   juce::Justification::centredLeft, false);
        if (!optionButtons.empty())
        {
            g.setColour(colours::dimText);
            g.setFont(titleFont(16.0f));
            g.drawText("OPTIONS", optionColumn().withHeight(22), juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        power.setBounds(250, 16, 104, 38);
        modes.setBounds(getWidth() - 24 - 600, 12, 600, 54);
        const auto plate = plateArea();
        levelLabel.setBounds(plate.getX() + 60, plate.getY() + 104, 220, 24);
        level.setBounds(plate.getX() + 60, plate.getY() + 130, 220, 230);
        auto column = optionColumn().withTrimmedTop(30);
        for (auto& button : optionButtons) button->setBounds(column.removeFromTop(60).withTrimmedBottom(12));
        destruction.setBounds(plate.getRight() - 360, plate.getY() + 150, 300, 110);
    }

private:
    juce::Rectangle<int> plateArea() const { return getLocalBounds().withTrimmedTop(82).reduced(16, 0).withTrimmedBottom(16); }
    juce::Rectangle<int> optionColumn() const
    {
        const auto plate = plateArea();
        return {plate.getX() + 360, plate.getY() + 134, 230, 160};
    }
    PageSpec spec;
    SwitchButton power, destruction;
    ModeSelector modes;
    NumericKnob level;
    juce::Label levelLabel;
    std::vector<std::unique_ptr<SwitchButton>> optionButtons;
    juce::AudioProcessorValueTreeState::ButtonAttachment powerAttachment, destroyAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment levelAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> optionAttachments;
    int shownMode = -1;
};
}
