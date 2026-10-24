#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

class LayerTab final : public juce::TextButton
{
public:
    explicit LayerTab(const juce::String& name, bool ensemble = false) : TextButton(name), furnace(ensemble) {}
    std::function<void()> onPowerToggle;
    void setLayerEnabled(bool enabled) { if (layerEnabled != enabled) { layerEnabled = enabled; repaint(); } }
    bool isLayerEnabled() const noexcept { return layerEnabled; }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getModifiers().isAltDown() && key.isKeyCode(juce::KeyPress::returnKey))
        {
            if (onPowerToggle) onPowerToggle();
            return true;
        }
        return TextButton::keyPressed(key);
    }
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            suppressMouseUp = true;
            if (event.x >= 0 && event.x < iconWidth && onPowerToggle) onPowerToggle();
            return;
        }
        suppressMouseUp = false;
        TextButton::mouseDown(event);
    }
    void mouseUp(const juce::MouseEvent& event) override
    {
        if (suppressMouseUp) { suppressMouseUp = false; return; }
        if (!event.mods.isPopupMenu()) TextButton::mouseUp(event);
    }
    void paintButton(juce::Graphics& g, bool highlighted, bool) override
    {
        const auto accent = juce::Colour(0xffe9a153);
        auto bounds = getLocalBounds().toFloat();
        juce::Path tab;
        tab.addRoundedRectangle(bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
                                8.0f, 8.0f, true, true, false, false);
        g.setColour(getToggleState() ? juce::Colour(0xff292d33)
                                    : highlighted ? juce::Colour(0xff22262c) : juce::Colour(0xff1e2126));
        g.fillPath(tab);
        g.setColour(juce::Colours::white.withAlpha(layerEnabled ? 0.95f : 0.55f));
        g.setFont(juce::FontOptions(17.0f));
        g.drawFittedText(getButtonText(), iconWidth + 4, 0, getWidth() - iconWidth - 16, getHeight(),
                        juce::Justification::centredLeft, 1);
        g.setColour(layerEnabled ? accent : juce::Colour(0xff999999));
        if (furnace)
        {
            for (int row = 0; row < 3; ++row)
            {
                juce::Path wave;
                const auto y = 15.0f + row * 7.0f;
                wave.startNewSubPath(10.0f, y);
                wave.lineTo(15.0f, y - 3.0f); wave.lineTo(22.0f, y + 3.0f); wave.lineTo(29.0f, y);
                g.strokePath(wave, juce::PathStrokeType(1.5f));
            }
        }
        else
        {
            for (int row = 0; row < 3; ++row)
                g.fillRoundedRectangle(10.0f + row * 3.0f, 14.0f + row * 7.0f, 20.0f - row * 6.0f, 3.0f, 1.5f);
        }
        if (!layerEnabled) g.drawLine(9.0f, 36.0f, 30.0f, 9.0f, 2.0f);
        if (getToggleState()) { g.setColour(accent); g.fillRect(12, 0, getWidth() - 24, 2); }
        if (hasKeyboardFocus(true)) { g.setColour(juce::Colours::white); g.drawRect(getLocalBounds().reduced(3), 1); }
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Handler final : juce::AccessibilityHandler
        {
            explicit Handler(LayerTab& tab)
                : AccessibilityHandler(tab, juce::AccessibilityRole::radioButton,
                    juce::AccessibilityActions{}
                        .addAction(juce::AccessibilityActionType::press, [&tab] { tab.triggerClick(); })
                        .addAction(juce::AccessibilityActionType::toggle, [&tab] { if (tab.onPowerToggle) tab.onPowerToggle(); })), owner(tab) {}
            juce::AccessibleState getCurrentState() const override
            {
                auto state = AccessibilityHandler::getCurrentState().withCheckable().withSelectable();
                if (owner.isLayerEnabled()) state = state.withChecked();
                if (owner.getToggleState()) state = state.withSelected();
                return state;
            }
            LayerTab& owner;
        };
        return std::make_unique<Handler>(*this);
    }
private:
    static constexpr int iconWidth = 38;
    bool layerEnabled = true, suppressMouseUp = false, furnace = false;
};
