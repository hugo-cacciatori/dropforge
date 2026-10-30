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
        paintOrnament(g, plate, finish.accent);
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
    // Each mode's own hardware, like a different amp head: a texture over the
    // whole plate and an emblem in the free top-right corner, never under a control.
    void paintOrnament(juce::Graphics& g, juce::Rectangle<float> plate, juce::Colour accent) const
    {
        juce::Graphics::ScopedSaveState saved(g);
        juce::Path clip;
        clip.addRoundedRectangle(plate, 12.0f);
        g.reduceClipRegion(clip);
        const juce::Rectangle<float> emblem(plate.getRight() - 420.0f, plate.getY() + 22.0f, 380.0f, 100.0f);
        switch (shownMode)
        {
            case 1: // Riot: hazard stripes along the base and slashing chevrons.
            {
                const auto band = plate.withTop(plate.getBottom() - 10.0f);
                g.setColour(accent.withAlpha(0.22f));
                for (auto x = band.getX() - 20.0f; x < band.getRight(); x += 24.0f)
                {
                    juce::Path stripe;
                    stripe.addQuadrilateral(x, band.getBottom(), x + 10.0f, band.getBottom(), x + 20.0f, band.getY(), x + 10.0f, band.getY());
                    g.fillPath(stripe);
                }
                for (int i = 0; i < 4; ++i)
                {
                    juce::Path chevron;
                    const auto x = emblem.getRight() - 150.0f + i * 34.0f, y = emblem.getCentreY();
                    chevron.startNewSubPath(x, y - 30.0f); chevron.lineTo(x + 24.0f, y); chevron.lineTo(x, y + 30.0f);
                    g.setColour(accent.withAlpha(0.12f + 0.08f * i));
                    g.strokePath(chevron, juce::PathStrokeType(7.0f, juce::PathStrokeType::mitered));
                }
                break;
            }
            case 2: // Halo: soft concentric rings glowing from the corner.
            {
                const auto centre = juce::Point<float>(emblem.getRight() - 90.0f, emblem.getCentreY() + 6.0f);
                g.setGradientFill(juce::ColourGradient(accent.withAlpha(0.16f), centre, accent.withAlpha(0.0f), centre.translated(220.0f, 0.0f), true));
                g.fillEllipse(juce::Rectangle<float>(440.0f, 440.0f).withCentre(centre));
                for (int ring = 1; ring <= 5; ++ring)
                {
                    g.setColour(accent.withAlpha(0.22f - ring * 0.035f));
                    g.drawEllipse(juce::Rectangle<float>(ring * 30.0f, ring * 30.0f).withCentre(centre), 1.4f);
                }
                break;
            }
            case 3: // Alloy: a machined hex mesh and an FM operator diagram.
            {
                g.setColour(juce::Colours::white.withAlpha(0.025f));
                constexpr float size = 22.0f;
                int row = 0;
                for (auto y = plate.getY(); y < plate.getBottom() + size; y += size * 0.866f, ++row)
                    for (auto x = plate.getX() + (row % 2 != 0 ? size * 0.5f : 0.0f); x < plate.getRight() + size; x += size)
                    {
                        juce::Path hex;
                        for (int corner = 0; corner < 6; ++corner)
                        {
                            const auto angle = juce::MathConstants<float>::pi / 3.0f * corner + juce::MathConstants<float>::pi / 6.0f;
                            const auto point = juce::Point<float>(x, y).getPointOnCircumference(size * 0.5f, angle);
                            if (corner == 0) hex.startNewSubPath(point); else hex.lineTo(point);
                        }
                        hex.closeSubPath();
                        g.strokePath(hex, juce::PathStrokeType(0.8f));
                    }
                const auto modulator = juce::Point<float>(emblem.getRight() - 170.0f, emblem.getCentreY() + 10.0f);
                const auto carrier = modulator.translated(110.0f, 0.0f);
                g.setColour(accent.withAlpha(0.35f));
                g.drawArrow(juce::Line<float>(modulator.translated(24.0f, 0.0f), carrier.translated(-24.0f, 0.0f)), 2.0f, 9.0f, 9.0f);
                for (const auto centre : {modulator, carrier})
                    g.drawEllipse(juce::Rectangle<float>(46.0f, 46.0f).withCentre(centre), 2.0f);
                // The modulator's feedback loop.
                juce::Path loop;
                loop.startNewSubPath(modulator.translated(-12.0f, -21.0f));
                loop.cubicTo(modulator.translated(-30.0f, -55.0f), modulator.translated(30.0f, -55.0f), modulator.translated(12.0f, -21.0f));
                g.strokePath(loop, juce::PathStrokeType(1.6f));
                g.setFont(titleFont(16.0f));
                g.drawText("M", juce::Rectangle<float>(46.0f, 46.0f).withCentre(modulator), juce::Justification::centred, false);
                g.drawText("C", juce::Rectangle<float>(46.0f, 46.0f).withCentre(carrier), juce::Justification::centred, false);
                break;
            }
            default: // Forge: brushed steel and a glowing vent grille.
            {
                g.setColour(juce::Colours::white.withAlpha(0.018f));
                for (auto y = plate.getY() + 2.0f; y < plate.getBottom(); y += 3.0f)
                    g.drawHorizontalLine(static_cast<int>(y), plate.getX(), plate.getRight());
                for (int slot = 0; slot < 9; ++slot)
                {
                    const juce::Rectangle<float> vent(emblem.getRight() - 210.0f + slot * 22.0f, emblem.getY() + 14.0f, 9.0f, 70.0f);
                    g.setColour(juce::Colour(0xff0d0e10));
                    g.fillRoundedRectangle(vent, 4.5f);
                    g.setGradientFill(juce::ColourGradient(accent.withAlpha(0.0f), 0.0f, vent.getY(), accent.withAlpha(0.35f), 0.0f, vent.getBottom(), false));
                    g.fillRoundedRectangle(vent.reduced(2.0f), 3.0f);
                }
                break;
            }
        }
    }
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
