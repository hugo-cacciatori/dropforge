#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
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
    // Compact: a small dial with its value beside it, for the footer strip.
    void setCompact(bool shouldBeCompact) { compact = shouldBeCompact; resized(); repaint(); }
    // A dot on the arc at a live position (0-1), e.g. the mod wheel moved by
    // CC1 away from the saved value; below 0 hides it.
    void setMarker(float proportion)
    {
        if (proportion != marker) { marker = proportion; repaint(); }
    }
    float markerPosition() const noexcept { return marker; }
    // Frequencies: "280" below 1000 and "6.5k" above, to fit a compact
    // knob; typing accepts either form.
    void setKiloFormat(bool shouldUseKilo) { kilo = shouldUseKilo; repaint(); }
    void mouseDown(const juce::MouseEvent& event) override
    {
        // The value under the dial is the place to type (right-click works anywhere).
        if (event.mods.isPopupMenu() || valueArea().contains(event.getPosition())) { openEntry(); typedClick = true; return; }
        typedClick = false;
        juce::Slider::mouseDown(event);
    }
    void mouseUp(const juce::MouseEvent& event) override
    {
        if (typedClick) { typedClick = false; return; }
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
        entry.setBounds(valueArea().withSizeKeepingCentre(std::min(getWidth(), 132), valueArea().getHeight()));
    }
    void paint(juce::Graphics& g) override
    {
        using namespace dropforge::ui;
        const auto accent = findColour(juce::Slider::rotarySliderFillColourId);
        const auto dial = compact ? getLocalBounds().removeFromLeft(getHeight()).toFloat()
                                  : getLocalBounds().withTrimmedBottom(valueArea().getHeight() + 4).toFloat();
        const auto size = std::min(dial.getWidth(), dial.getHeight()) - (compact ? 2.0f : 8.0f);
        const auto centre = dial.getCentre();
        const auto radius = size * 0.5f;
        constexpr auto start = juce::MathConstants<float>::pi * 1.25f, end = juce::MathConstants<float>::pi * 2.75f;
        const auto position = static_cast<float>(valueToProportionOfLength(getValue()));
        const auto angle = start + position * (end - start);
        const auto track = std::max(3.0f, radius * 0.11f);
        juce::Path arc;
        arc.addCentredArc(centre.x, centre.y, radius - track, radius - track, 0.0f, start, end, true);
        g.setColour(colours::base);
        g.strokePath(arc, juce::PathStrokeType(track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, radius - track, radius - track, 0.0f, start, angle, true);
        g.setColour(accent);
        g.strokePath(value, juce::PathStrokeType(track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // The knob body: brushed steel with a pointer line.
        const auto body = radius - track * 2.4f;
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff454b54), centre.x, centre.y - body,
                                               juce::Colour(0xff1b1e22), centre.x, centre.y + body, false));
        g.fillEllipse(juce::Rectangle<float>(body * 2.0f, body * 2.0f).withCentre(centre));
        g.setColour(colours::edge.brighter(0.2f));
        g.drawEllipse(juce::Rectangle<float>(body * 2.0f, body * 2.0f).withCentre(centre), 1.0f);
        g.setColour(colours::text);
        g.drawLine(juce::Line<float>(centre.getPointOnCircumference(body * 0.25f, angle), centre.getPointOnCircumference(body * 0.88f, angle)),
                   std::max(2.0f, body * 0.07f));
        if (marker >= 0.0f)
        {
            const auto spot = centre.getPointOnCircumference(radius - track, start + juce::jlimit(0.0f, 1.0f, marker) * (end - start));
            g.setColour(colours::text);
            g.fillEllipse(juce::Rectangle<float>(track * 1.8f, track * 1.8f).withCentre(spot));
        }
        if (!entry.isVisible())
        {
            g.setColour(colours::text);
            g.setFont(valueFont(compact ? std::min(16.0f, getHeight() * 0.55f) : std::min(17.0f, valueArea().getHeight() * 0.8f)));
            g.drawText(getTextFromValue(getValue()), valueArea(), compact ? juce::Justification::centredLeft : juce::Justification::centred, false);
        }
        drawFocus(g, *this, 8.0f);
    }
    void openEntry()
    {
        entry.setTitle(getTitle() + " value");
        entry.setHelpText("Enter applies; Escape cancels; clicking away applies and closes.");
        // Full precision while typing, so confirming unchanged text never nudges the value.
        const auto exact = getMinimum() == -60.0 && getValue() <= -60.0 ? juce::String("-inf dB")
                         : juce::String(getValue(), 3) + getTextValueSuffix();
        entry.setText(exact, false);
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
        repaint();
    }
    void commitIfOutside(juce::Component* target)
    {
        if (target != this && target != &entry && !entry.isParentOf(target)) finishEntry(true);
    }
    juce::TextEditor& valueEditor() noexcept { return entry; }
    juce::String getTextFromValue(double value) override
    {
        if (getMinimum() == -60.0 && value <= -60.0) return "-inf dB";
        if (kilo && value >= 1000.0) return juce::String(value / 1000.0, value >= 9950.0 ? 0 : 1) + "k" + getTextValueSuffix();
        return juce::String(value, getNumDecimalPlacesToDisplay()) + getTextValueSuffix();
    }
    double getValueFromText(const juce::String& text) override
    {
        auto stripped = text.trim();
        if (getMinimum() == -60.0 && (stripped == "-inf" || stripped == "-inf dB")) return -60.0;
        const auto suffix = getTextValueSuffix().trim();
        if (suffix.isNotEmpty() && stripped.endsWithIgnoreCase(suffix))
            stripped = stripped.dropLastCharacters(suffix.length()).trim();
        auto factor = 1.0;
        if (kilo && (stripped.endsWithChar('k') || stripped.endsWithChar('K'))) { factor = 1000.0; stripped = stripped.dropLastCharacters(1).trim(); }
        std::istringstream parser(stripped.toStdString());
        parser.imbue(std::locale::classic());
        double value = 0.0;
        parser >> std::noskipws >> value;
        value *= factor;
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
    juce::Rectangle<int> valueArea() const
    {
        return compact ? getLocalBounds().withTrimmedLeft(getHeight() + 4) : getLocalBounds().removeFromBottom(std::max(18, getHeight() / 7));
    }
    void clearEntryError()
    {
        if (normalTooltip.isNotEmpty()) { setTooltip(normalTooltip); normalTooltip.clear(); }
    }
    juce::TextEditor entry;
    juce::String normalTooltip;
    bool editing = false, typedClick = false, compact = false, kilo = false;
    float marker = -1.0f;
};
