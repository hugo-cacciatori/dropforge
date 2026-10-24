#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <locale>
#include <sstream>

class NumericKnob final : public juce::Slider
{
public:
    NumericKnob()
    {
        setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        setDoubleClickReturnValue(true, 0.0);
        addChildComponent(entry);
        entry.setJustification(juce::Justification::centred);
        entry.setSelectAllWhenFocused(true);
        entry.onReturnKey = [this] { finishEntry(true); grabKeyboardFocus(); };
        entry.onEscapeKey = [this] { finishEntry(false); grabKeyboardFocus(); };
        entry.onFocusLost = [this] { finishEntry(true); };
    }
    ~NumericKnob() override { entry.onFocusLost = nullptr; }
    std::function<void(bool)> onEntryGesture;
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu()) { openEntry(); return; }
        juce::Slider::mouseDown(event);
    }
    void mouseUp(const juce::MouseEvent& event) override
    {
        if (!event.mods.isPopupMenu()) juce::Slider::mouseUp(event);
    }
    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        finishEntry(false);
        clearEntryError();
        juce::Slider::mouseDoubleClick(event);
    }
    void valueChanged() override
    {
        clearEntryError();
        juce::Slider::valueChanged();
    }
    void resized() override
    {
        juce::Slider::resized();
        entry.setBounds(getLocalBounds().withSizeKeepingCentre(132, 28).translated(0, getHeight() / 3));
    }
    void openEntry()
    {
        entry.setTitle(getTitle() + " value");
        entry.setHelpText("Enter applies; Escape cancels; clicking away applies and closes.");
        entry.setText(getTextFromValue(getValue()), false);
        editing = true;
        entry.setVisible(true);
        entry.toFront(true);
        entry.grabKeyboardFocus();
        entry.selectAll();
    }
    void finishEntry(bool apply)
    {
        if (!editing) return;
        editing = false;
        if (apply)
        {
            const auto value = getValueFromText(entry.getText());
            if (value != getValue())
            {
                if (onEntryGesture) onEntryGesture(true);
                setValue(value, juce::sendNotificationSync);
                if (onEntryGesture) onEntryGesture(false);
            }
        }
        entry.setVisible(false);
    }
    void commitIfOutside(juce::Component* target)
    {
        if (target != this && target != &entry && !entry.isParentOf(target)) finishEntry(true);
    }
    juce::TextEditor& valueEditor() noexcept { return entry; }
    juce::String getTextFromValue(double value) override
    {
        if (getMinimum() == -60.0 && value <= -60.0) return "-inf dB";
        return juce::String(value, getNumDecimalPlacesToDisplay()) + getTextValueSuffix();
    }
    double getValueFromText(const juce::String& text) override
    {
        auto stripped = text.trim();
        if (getMinimum() == -60.0 && (stripped == "-inf" || stripped == "-inf dB")) return -60.0;
        const auto suffix = getTextValueSuffix().trim();
        if (suffix.isNotEmpty() && stripped.endsWithIgnoreCase(suffix))
            stripped = stripped.dropLastCharacters(suffix.length()).trim();
        std::istringstream parser(stripped.toStdString());
        parser.imbue(std::locale::classic());
        double value = 0.0;
        parser >> std::noskipws >> value;
        if (!parser || parser.peek() != std::char_traits<char>::eof() || !std::isfinite(value)
            || value < getMinimum() || value > getMaximum())
        {
            if (normalTooltip.isEmpty()) normalTooltip = getTooltip();
            setTooltip("Enter a number from " + juce::String(getMinimum()) + " to " + juce::String(getMaximum())
                       + getTextValueSuffix() + ". Value unchanged.");
            return getValue();
        }
        if (normalTooltip.isNotEmpty()) { setTooltip(normalTooltip); normalTooltip.clear(); }
        return value;
    }
private:
    void clearEntryError()
    {
        if (normalTooltip.isNotEmpty()) { setTooltip(normalTooltip); normalTooltip.clear(); }
    }
    juce::TextEditor entry;
    juce::String normalTooltip;
    bool editing = false;
};
