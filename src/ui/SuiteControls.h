#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"
#include <cmath>

namespace dropforge::ui
{
// A latching switch with a lamp: power, Mono, Drop and the large Destruction
// switch. Toggles on click, Space or Enter; host gestures come from the
// ButtonAttachment that owns its value.
class SwitchButton final : public juce::Button
{
public:
    enum class Look { power, option, destruction };
    SwitchButton(const juce::String& name, Look switchLook) : Button(name), look(switchLook)
    {
        setClickingTogglesState(true);
        setWantsKeyboardFocus(true);
        setTitle(name);
    }
    void setAccent(juce::Colour colour) { if (accent != colour) { accent = colour; repaint(); } }
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto on = getToggleState();
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        const auto radius = look == Look::destruction ? 10.0f : 7.0f;
        const auto lampColour = look == Look::destruction ? colours::danger : accent;
        juce::ColourGradient body(colours::raised.brighter(highlighted ? 0.12f : 0.0f), 0.0f, bounds.getY(),
                                  colours::panel.darker(down ? 0.3f : 0.1f), 0.0f, bounds.getBottom(), false);
        if (on && look == Look::destruction)
            body = juce::ColourGradient(juce::Colour(0xff5a1a12), 0.0f, bounds.getY(), juce::Colour(0xff2a0c08), 0.0f, bounds.getBottom(), false);
        g.setGradientFill(body);
        g.fillRoundedRectangle(bounds, radius);
        g.setColour(on ? lampColour.withAlpha(0.9f) : colours::edge);
        g.drawRoundedRectangle(bounds, radius, on ? 2.0f : 1.0f);

        const auto lampSize = look == Look::destruction ? 16.0f : 11.0f;
        drawLamp(g, {bounds.getX() + 14.0f, bounds.getCentreY() - lampSize * 0.5f, lampSize, lampSize}, lampColour, on);
        auto textArea = bounds.withTrimmedLeft(lampSize * 2.0f + 14.0f).withTrimmedRight(8.0f);
        g.setColour(on ? colours::text : colours::dimText);
        if (look == Look::destruction)
        {
            g.setFont(titleFont(26.0f));
            g.drawFittedText(getButtonText().toUpperCase(), textArea.removeFromTop(textArea.getHeight() * 0.62f).toNearestInt(),
                             juce::Justification::bottomLeft, 1);
            g.setFont(valueFont(13.0f));
            g.setColour(on ? colours::danger.brighter(0.3f) : colours::faint);
            g.drawFittedText(on ? "ON: the extreme version of this mode" : "OFF", textArea.toNearestInt(), juce::Justification::topLeft, 1);
        }
        else
        {
            g.setFont(titleFont(look == Look::power ? 17.0f : 18.0f));
            const auto label = look == Look::power ? juce::String(on ? "ON" : "OFF") : getButtonText().toUpperCase();
            g.drawFittedText(label, textArea.toNearestInt(), juce::Justification::centredLeft, 1);
        }
        drawFocus(g, *this, radius);
    }
private:
    Look look;
    juce::Colour accent = colours::text;
};

// One page tab in the navigation: identity colour, mode letter, power light
// and a Destruction mark. Left-click opens the page; right-click anywhere on
// it, or Alt+Enter, toggles the instrument's power (the page's own power
// switch does the same, so power is never hidden behind right-click alone).
class InstrumentTab final : public juce::Button
{
public:
    explicit InstrumentTab(const InstrumentStyle& tabStyle)
        : Button(tabStyle.title), style(tabStyle)
    {
        setWantsKeyboardFocus(true);
        setTitle(getButtonText());
    }
    std::function<void()> onPowerToggle;
    void setStatus(bool powered, int modeIndex, bool destroyed)
    {
        if (powered == enabled && modeIndex == mode && destroyed == destroy) return;
        enabled = powered; mode = modeIndex; destroy = destroyed;
        repaint();
    }
    bool isInstrumentEnabled() const noexcept { return enabled; }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getModifiers().isAltDown() && key.isKeyCode(juce::KeyPress::returnKey))
        {
            if (onPowerToggle) onPowerToggle();
            return true;
        }
        return Button::keyPressed(key);
    }
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            suppressMouseUp = true;
            if (onPowerToggle) onPowerToggle();
            return;
        }
        suppressMouseUp = false;
        Button::mouseDown(event);
    }
    void mouseUp(const juce::MouseEvent& event) override
    {
        if (suppressMouseUp) { suppressMouseUp = false; return; }
        Button::mouseUp(event);
    }
    void paintButton(juce::Graphics& g, bool highlighted, bool) override
    {
        const auto selected = getToggleState();
        const auto finish = finishFor(style, mode);
        auto bounds = getLocalBounds().toFloat();
        juce::Path tab;
        tab.addRoundedRectangle(bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), 9.0f, 9.0f, true, true, false, false);
        g.setColour(selected ? colours::panel : highlighted ? colours::raised.darker(0.2f) : colours::base.brighter(0.04f));
        g.fillPath(tab);
        g.setColour(colours::edge.withAlpha(selected ? 1.0f : 0.6f));
        g.strokePath(tab, juce::PathStrokeType(1.0f));
        if (selected)
        {
            g.setColour(finish.accent);
            g.fillRoundedRectangle(bounds.getX() + 10.0f, bounds.getY(), bounds.getWidth() - 20.0f, 3.0f, 1.5f);
        }
        const auto alpha = enabled ? 1.0f : 0.45f;
        drawLamp(g, {18.0f, bounds.getCentreY() - 6.0f, 12.0f, 12.0f}, style.identity, enabled);
        g.setColour((selected ? colours::text : colours::dimText).withAlpha(alpha));
        g.setFont(titleFont(22.0f));
        g.drawText(style.name, juce::Rectangle<float>(42.0f, 6.0f, bounds.getWidth() - 100.0f, 26.0f), juce::Justification::centredLeft, false);
        g.setFont(valueFont(12.0f));
        g.setColour(enabled ? finish.accent.withAlpha(0.9f) : colours::faint);
        g.drawText(enabled ? juce::String(modeLetters[(size_t) mode]) + " " + modeNames[(size_t) mode] : juce::String("OFF"),
                   juce::Rectangle<float>(42.0f, 31.0f, bounds.getWidth() - 100.0f, 16.0f), juce::Justification::centredLeft, false);
        // The mode letter as a badge, readable at a glance from any page.
        const juce::Rectangle<float> badge(bounds.getRight() - 46.0f, bounds.getCentreY() - 15.0f, 30.0f, 30.0f);
        g.setColour(finish.accent.withAlpha(0.18f * alpha));
        g.fillRoundedRectangle(badge, 6.0f);
        g.setColour(finish.accent.withAlpha(alpha));
        g.drawRoundedRectangle(badge, 6.0f, 1.2f);
        g.setFont(titleFont(20.0f));
        g.drawText(modeLetters[(size_t) mode], badge, juce::Justification::centred, false);
        if (destroy)
        {
            // Destruction mark: a jagged bolt beside the badge.
            juce::Path bolt;
            const auto x = badge.getX() - 18.0f, y = badge.getY() + 3.0f;
            bolt.startNewSubPath(x + 8.0f, y); bolt.lineTo(x + 2.0f, y + 13.0f); bolt.lineTo(x + 7.0f, y + 13.0f);
            bolt.lineTo(x + 4.0f, y + 24.0f); bolt.lineTo(x + 12.0f, y + 9.0f); bolt.lineTo(x + 7.0f, y + 9.0f); bolt.closeSubPath();
            g.setColour(colours::danger.withAlpha(alpha));
            g.fillPath(bolt);
        }
        drawFocus(g, *this, 9.0f);
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Handler final : juce::AccessibilityHandler
        {
            explicit Handler(InstrumentTab& tab)
                : AccessibilityHandler(tab, juce::AccessibilityRole::radioButton,
                    juce::AccessibilityActions{}
                        .addAction(juce::AccessibilityActionType::press, [&tab] { tab.triggerClick(); })
                        .addAction(juce::AccessibilityActionType::toggle, [&tab] { if (tab.onPowerToggle) tab.onPowerToggle(); })), owner(tab) {}
            juce::AccessibleState getCurrentState() const override
            {
                auto state = AccessibilityHandler::getCurrentState().withCheckable().withSelectable();
                if (owner.isInstrumentEnabled()) state = state.withChecked();
                if (owner.getToggleState()) state = state.withSelected();
                return state;
            }
            InstrumentTab& owner;
        };
        return std::make_unique<Handler>(*this);
    }
private:
    const InstrumentStyle& style;
    int mode = 0;
    bool enabled = true, destroy = false, suppressMouseUp = false;
};

// The four large mode buttons of an instrument page, bound to its choice
// parameter. A click is one complete host gesture; the engine crossfades.
class ModeSelector final : public juce::Component
{
public:
    ModeSelector(juce::RangedAudioParameter& parameter, const InstrumentStyle& selectorStyle, const juce::String& instrument)
        : style(selectorStyle),
          attachment(parameter, [this](float value) { show(juce::roundToInt(value)); })
    {
        setTitle(instrument + " mode");
        setFocusContainerType(FocusContainerType::focusContainer);
        for (int i = 0; i < 4; ++i)
        {
            auto& button = buttons[(size_t) i];
            button.mode = i;
            button.style = &style;
            button.setButtonText(juce::String(modeLetters[(size_t) i]) + " " + modeNames[(size_t) i]);
            button.setTitle(instrument + " mode " + button.getButtonText());
            button.setTooltip(instrument + " mode " + button.getButtonText() + ". " + finishFor(style, i).character
                              + " Switching while playing crossfades without a click.");
            button.setHelpText(button.getTooltip());
            button.setRadioGroupId(1);
            button.setWantsKeyboardFocus(true);
            button.onClick = [this, i] { attachment.setValueAsCompleteGesture(static_cast<float>(i)); };
            addAndMakeVisible(button);
        }
        attachment.sendInitialUpdate();
    }
    int selectedMode() const noexcept { return current; }
    juce::Button& button(int mode) noexcept { return buttons[(size_t) mode]; }
    void resized() override
    {
        const auto gap = 8, width = (getWidth() - 3 * gap) / 4;
        for (int i = 0; i < 4; ++i) buttons[(size_t) i].setBounds(i * (width + gap), 0, width, getHeight());
    }
private:
    struct ModeButton final : juce::Button
    {
        ModeButton() : Button({}) {}
        int mode = 0;
        const InstrumentStyle* style = nullptr;
        void paintButton(juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto on = getToggleState();
            const auto finish = finishFor(*style, mode);
            auto bounds = getLocalBounds().toFloat().reduced(1.0f);
            g.setGradientFill(juce::ColourGradient(on ? finish.plateTop.brighter(0.25f) : colours::raised.brighter(highlighted ? 0.1f : 0.0f),
                                                   0.0f, bounds.getY(),
                                                   on ? finish.plateBottom : colours::panel.darker(down ? 0.3f : 0.15f),
                                                   0.0f, bounds.getBottom(), false));
            g.fillRoundedRectangle(bounds, 8.0f);
            g.setColour(on ? finish.accent : colours::edge);
            g.drawRoundedRectangle(bounds, 8.0f, on ? 2.0f : 1.0f);
            g.setColour(on ? finish.accent : colours::dimText);
            g.setFont(titleFont(28.0f));
            g.drawText(modeLetters[(size_t) mode], bounds.withWidth(40.0f).withTrimmedLeft(12.0f), juce::Justification::centredLeft, false);
            g.setColour(on ? colours::text : colours::dimText);
            g.setFont(titleFont(19.0f));
            g.drawFittedText(juce::String(modeNames[(size_t) mode]).toUpperCase(), bounds.withTrimmedLeft(44.0f).toNearestInt(),
                             juce::Justification::centredLeft, 1);
            drawFocus(g, *this, 8.0f);
        }
    };
    void show(int mode)
    {
        current = juce::jlimit(0, 3, mode);
        for (int i = 0; i < 4; ++i) buttons[(size_t) i].setToggleState(i == current, juce::dontSendNotification);
    }
    const InstrumentStyle& style;
    std::array<ModeButton, 4> buttons;
    juce::ParameterAttachment attachment;
    int current = 0;
};

// Stereo peak meter with clip and gain-reduction lights, fed at 30 Hz from
// the processor's lock-free meter taps. Repaints only when a reading moves.
class OutputMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    OutputMeter()
    {
        setTitle("Output meter");
        setTooltip("Output peak meter. CLIP lights when the output reaches full scale; LIMIT lights when the safety limiter is working.");
    }
    void push(float peakLeft, float peakRight, float limiterReductionDb)
    {
        constexpr float fallDbPerTick = 20.0f / 30.0f; // falls 20 dB per second
        const auto toDb = [](float peak) { return peak > 1.0e-6f ? 20.0f * std::log10(peak) : -120.0f; };
        const auto previous = std::array<float, 2>{levelDb[0], levelDb[1]};
        levelDb[0] = std::max(toDb(peakLeft), levelDb[0] - fallDbPerTick);
        levelDb[1] = std::max(toDb(peakRight), levelDb[1] - fallDbPerTick);
        // Lights hold for a second so short events stay visible.
        clipHold = std::max(peakLeft, peakRight) >= 0.999f ? 30 : std::max(0, clipHold - 1);
        limitHold = limiterReductionDb < -0.5f ? 30 : std::max(0, limitHold - 1);
        const auto lights = (clipHold > 0 ? 1 : 0) | (limitHold > 0 ? 2 : 0);
        if (std::abs(previous[0] - levelDb[0]) > 0.05f || std::abs(previous[1] - levelDb[1]) > 0.05f || lights != shownLights)
        {
            shownLights = lights;
            repaint();
        }
    }
    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        auto lights = bounds.removeFromRight(52.0f);
        const auto barHeight = (bounds.getHeight() - 6.0f) / 2.0f;
        for (int channel = 0; channel < 2; ++channel)
        {
            const juce::Rectangle<float> bar(bounds.getX(), bounds.getY() + 2.0f + channel * (barHeight + 2.0f), bounds.getWidth() - 8.0f, barHeight);
            g.setColour(colours::base);
            g.fillRoundedRectangle(bar, 2.0f);
            const auto fraction = juce::jlimit(0.0f, 1.0f, (levelDb[(size_t) channel] + 48.0f) / 48.0f);
            g.setGradientFill(juce::ColourGradient(juce::Colour(0xff3fae6a), bar.getX(), 0.0f, colours::danger, bar.getRight(), 0.0f, false));
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * fraction), 2.0f);
        }
        g.setFont(valueFont(10.0f));
        const auto drawLight = [&](juce::Rectangle<float> area, const char* label, juce::Colour colour, bool lit)
        {
            drawLamp(g, area.removeFromLeft(9.0f).withSizeKeepingCentre(8.0f, 8.0f), colour, lit);
            g.setColour(lit ? colours::text : colours::faint);
            g.drawText(label, area.withTrimmedLeft(4.0f), juce::Justification::centredLeft, false);
        };
        drawLight(lights.removeFromTop(lights.getHeight() / 2.0f), "CLIP", colours::danger, (shownLights & 1) != 0);
        drawLight(lights, "LIMIT", juce::Colour(0xffe8c22c), (shownLights & 2) != 0);
    }
private:
    std::array<float, 2> levelDb{-120.0f, -120.0f};
    int clipHold = 0, limitHold = 0, shownLights = 0;
};
}
