#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "NumericKnob.h"
#include "SuiteControls.h"
#include "plugin/Parameters.h"
#include <array>
#include <memory>
#include <vector>

namespace dropforge::ui
{
// The FX page's identity colour (SPEC section 6: FX teal).
inline const juce::Colour fxIdentity{0xff35c4b0};

// The FX tab: no mode, no power of its own; its lamp lights while any effect
// is on, and its second line says how many are.
class FxTab final : public juce::Button
{
public:
    FxTab() : Button("FX")
    {
        setWantsKeyboardFocus(true);
        setTitle("FX");
    }
    void setEffectsOn(int count)
    {
        if (count == on) return;
        on = count;
        repaint();
    }
    int effectsOn() const noexcept { return on; }
    void paintButton(juce::Graphics& g, bool highlighted, bool) override
    {
        const auto selected = getToggleState();
        auto bounds = getLocalBounds().toFloat();
        juce::Path tab;
        tab.addRoundedRectangle(bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), 9.0f, 9.0f, true, true, false, false);
        g.setColour(selected ? colours::panel : highlighted ? colours::raised.darker(0.2f) : colours::base.brighter(0.04f));
        g.fillPath(tab);
        g.setColour(colours::edge.withAlpha(selected ? 1.0f : 0.6f));
        g.strokePath(tab, juce::PathStrokeType(1.0f));
        if (selected)
        {
            g.setColour(fxIdentity);
            g.fillRoundedRectangle(bounds.getX() + 10.0f, bounds.getY(), bounds.getWidth() - 20.0f, 3.0f, 1.5f);
        }
        drawLamp(g, {18.0f, bounds.getCentreY() - 6.0f, 12.0f, 12.0f}, fxIdentity, on > 0);
        g.setColour(selected ? colours::text : colours::dimText);
        g.setFont(titleFont(22.0f));
        g.drawText("FX", juce::Rectangle<float>(42.0f, 6.0f, bounds.getWidth() - 60.0f, 26.0f), juce::Justification::centredLeft, false);
        g.setFont(valueFont(12.0f));
        g.setColour(on > 0 ? fxIdentity.withAlpha(0.9f) : colours::faint);
        g.drawText(on > 0 ? juce::String(on) + (on == 1 ? " effect on" : " effects on") : juce::String("all off"),
                   juce::Rectangle<float>(42.0f, 31.0f, bounds.getWidth() - 60.0f, 16.0f), juce::Justification::centredLeft, false);
        drawFocus(g, *this, 9.0f);
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Handler final : juce::AccessibilityHandler
        {
            explicit Handler(FxTab& tab)
                : AccessibilityHandler(tab, juce::AccessibilityRole::radioButton,
                    juce::AccessibilityActions{}.addAction(juce::AccessibilityActionType::press, [&tab] { tab.triggerClick(); })),
                  owner(tab) {}
            juce::AccessibleState getCurrentState() const override
            {
                auto state = AccessibilityHandler::getCurrentState().withSelectable();
                return owner.getToggleState() ? state.withSelected() : state;
            }
            FxTab& owner;
        };
        return std::make_unique<Handler>(*this);
    }
private:
    int on = 0;
};

// The FX page (SPEC sections 6 and 8): a strip per effect with its power and
// its few controls, laid out in signal order: the Delay and Reverb returns
// on top, then the sends that feed them and the finishing effects on the
// whole mix. Knobs are compact (a small dial with its value beside it); each
// caption carries the unit.
class FxPage final : public juce::Component
{
public:
    explicit FxPage(juce::AudioProcessorValueTreeState& state)
    {
        namespace p = dropforge::parameters;
        setTitle("FX page");
        setFocusContainerType(FocusContainerType::focusContainer);
        auto& delay = strip("DELAY", "Echoes of the sends, synced to the tempo. ", p::fxDelayOn, state);
        delay.add(toggle(state, p::fxDelaySync, "Sync", "Sync: the echo follows the song's tempo; off uses Time in milliseconds."), "SYNC", 84);
        delay.add(choice(state, p::fxDelayDivision, "Division", "the synced echo's note length.",
                         {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BARS"}), "DIVISION", 384);
        delay.newRow();
        delay.add(choice(state, p::fxDelayFeel, "Feel", "straight, dotted (one and a half) or triplet (two thirds) note lengths.",
                         {"Straight", "Dotted", "Triplet"}), "FEEL", 180);
        delay.add(choice(state, p::fxDelayMode, "Mode", "Stereo repeats each side on itself; Ping-pong bounces left and right, only while Furnace Mono is on or Blade or Anvil plays alone.",
                         {"Stereo", "Ping-pong"}), "MODE", 160);
        delay.add(knob(state, p::fxDelayMs, "Delay time", "Time: the echo length when Sync is off.", 0), "TIME ms", 96);
        delay.newRow();
        delay.add(knob(state, p::fxDelayFeedback, "Delay feedback", "Feedback: how many repeats; each comes back a little darker.", 0), "FEEDBACK %", 88);
        delay.add(knob(state, p::fxDelayWidth, "Delay width", "Width: from echoes in the centre to fully spread.", 0), "WIDTH %", 88);
        delay.add(knob(state, p::fxDelayMix, "Delay mix", "Mix: the echoes' level beside the dry sound.", 0), "MIX %", 88);
        delay.add(frequencyKnob(state, p::fxDelayLowCut, "Delay low cut", "Low Cut: keeps echoes out of the bass."), "LOW CUT Hz", 88);
        delay.add(frequencyKnob(state, p::fxDelayHighCut, "Delay high cut", "High Cut: darkens the echoes."), "HIGH CUT Hz", 88);

        auto& reverb = strip("REVERB", "A room around the sends. ", p::fxReverbOn, state);
        reverb.add(knob(state, p::fxReverbDecay, "Reverb decay", "Decay: how long the room rings, in seconds.", 1), "DECAY s", 88);
        reverb.add(knob(state, p::fxReverbPreDelay, "Reverb pre-delay", "Pre-delay: a gap before the room starts, keeping attacks clear.", 0),
                   "PRE-DLY ms", 88);
        reverb.add(knob(state, p::fxReverbSize, "Reverb size", "Size: a small room to a big hall.", 0), "SIZE %", 88);
        reverb.add(knob(state, p::fxReverbDiffusion, "Reverb diffusion", "Diffusion: from distinct early echoes to a smooth cloud.", 0), "DIFFUSE %", 88);
        reverb.add(knob(state, p::fxReverbDamping, "Reverb damping", "Damping: how fast the tail darkens.", 0), "DAMPING %", 88);
        reverb.newRow();
        reverb.add(knob(state, p::fxReverbModulation, "Reverb modulation", "Modulation: a slow shimmer that keeps long tails from ringing.", 0), "MOD %", 88);
        reverb.add(knob(state, p::fxReverbWidth, "Reverb width", "Width: from a centred room to fully spread.", 0), "WIDTH %", 88);
        reverb.add(knob(state, p::fxReverbMix, "Reverb mix", "Mix: the room's level beside the dry sound.", 0), "MIX %", 88);
        reverb.add(frequencyKnob(state, p::fxReverbLowCut, "Reverb low cut", "Low Cut: keeps the room out of the bass."), "LOW CUT Hz", 88);
        reverb.add(frequencyKnob(state, p::fxReverbHighCut, "Reverb high cut", "High Cut: darkens the room."), "HIGH CUT Hz", 88);

        auto& sends = strip("SENDS", "", nullptr, state);
        sends.add(knob(state, p::fxSendAnvil, "Anvil send", "Anvil send: how much bass reaches the Delay and Reverb; open only while Anvil plays alone, so a mix keeps a dry low end.", 0), "ANVIL %", 78);
        sends.add(knob(state, p::fxSendFurnace, "Furnace send", "Furnace send: how much of the chord wall reaches the Delay and Reverb.", 0), "FURNACE %", 78);
        sends.newRow();
        sends.add(knob(state, p::fxSendBlade, "Blade send", "Blade send: how much of the lead reaches the Delay and Reverb.", 0), "BLADE %", 78);
        sends.add(knob(state, p::fxDuck, "Ducking", "Ducking: the echoes and room dip under the playing and bloom in the gaps.", 0), "DUCKING %", 78);

        auto& saturate = strip("SATURATE", "Tape-style warmth and glue on the whole mix, at the same loudness. ", p::fxSaturateOn, state);
        saturate.add(knob(state, p::fxSaturateDrive, "Saturate drive", "Drive: from warm to thick and compressed.", 1), "DRIVE dB", 78);
        saturate.add(knob(state, p::fxSaturateMix, "Saturate mix", "Mix: blends the saturated mix with the clean one.", 0), "MIX %", 78);

        auto& crush = strip("CRUSH", "Glitch grit: fewer bits and a lower sample rate. ", p::fxCrushOn, state);
        crush.add(knob(state, p::fxCrushBits, "Crush bits", "Bits: fewer bits, rougher steps.", 1), "BITS", 78);
        crush.add(frequencyKnob(state, p::fxCrushRate, "Crush rate", "Rate: a lower sample rate folds in metallic aliasing."), "RATE Hz", 78);
        crush.newRow();
        crush.add(frequencyKnob(state, p::fxCrushTone, "Crush tone", "Tone: smooths the crushed signal's fizz."), "TONE Hz", 78);
        crush.add(knob(state, p::fxCrushMix, "Crush mix", "Mix: blends the crushed mix with the clean one.", 0), "MIX %", 78);

        auto& chorus = strip("CHORUS", "A lush, wide ensemble; the bass stays centred. ", p::fxChorusOn, state);
        chorus.add(knob(state, p::fxChorusRate, "Chorus rate", "Rate: how fast the voices drift.", 2), "RATE Hz", 78);
        chorus.add(knob(state, p::fxChorusDepth, "Chorus depth", "Depth: how far the voices drift.", 0), "DEPTH %", 78);
        chorus.newRow();
        chorus.add(knob(state, p::fxChorusMix, "Chorus mix", "Mix: how much ensemble joins the mix.", 0), "MIX %", 78);

        auto& eq = strip("EQ", "The final tone of the whole mix. ", p::fxEqOn, state);
        eq.add(frequencyKnob(state, p::fxEqLowCut, "EQ low cut", "Low Cut: removes rumble below it."), "LOW CUT Hz", 78);
        eq.add(frequencyKnob(state, p::fxEqHighCut, "EQ high cut", "High Cut: removes fizz above it."), "HIGH CUT Hz", 78);
        eq.newRow();
        eq.add(knob(state, p::fxEqTilt, "EQ tilt", "Tilt: darker (left) or brighter (right) around 800 Hz.", 1), "TILT dB", 78);
    }

    // Every knob, for closing typing fields when the page hides.
    std::vector<NumericKnob*> knobs()
    {
        std::vector<NumericKnob*> all;
        for (auto& k : knobControls) all.push_back(k.get());
        return all;
    }
    // Tests: a strip's power switch by its name ("DELAY"...), a control by
    // its parameter ID.
    SwitchButton* powerSwitch(const juce::String& name)
    {
        for (auto& s : strips) if (name == s->name && s->power != nullptr) return s->power.get();
        return nullptr;
    }
    NumericKnob* knobFor(const juce::String& id)
    {
        for (std::size_t i = 0; i < knobControls.size(); ++i) if (id == knobIds[i]) return knobControls[i].get();
        return nullptr;
    }
    ChoiceSelector* choiceFor(const juce::String& id)
    {
        for (std::size_t i = 0; i < choiceControls.size(); ++i) if (id == choiceIds[i]) return choiceControls[i].get();
        return nullptr;
    }
    // The owner's routing rules on screen (SPEC section 8): Anvil's send greys
    // out while closed, and Mode locks on Stereo while ping-pong is not allowed.
    void showRouting(bool anvilSendOpen, bool pingPongAllowed)
    {
        if (auto* send = knobFor(dropforge::parameters::fxSendAnvil))
        {
            send->setEnabled(anvilSendOpen);
            send->setAlpha(anvilSendOpen ? 1.0f : 0.4f);
        }
        if (auto* mode = choiceFor(dropforge::parameters::fxDelayMode)) mode->setForced(pingPongAllowed ? -1 : 0);
    }
    // How many effects are on (the tab shows it).
    static int effectsOn(juce::AudioProcessorValueTreeState& state)
    {
        namespace p = dropforge::parameters;
        int count = 0;
        for (const auto* id : {p::fxDelayOn, p::fxReverbOn, p::fxSaturateOn, p::fxCrushOn, p::fxChorusOn, p::fxEqOn})
            count += state.getRawParameterValue(id)->load() >= 0.5f ? 1 : 0;
        return count;
    }

    // The strips' outlines follow their power: repaint only when one changed.
    void refreshPower()
    {
        unsigned mask = 0, bit = 1;
        for (auto& s : strips) { if (s->power && s->power->getToggleState()) mask |= bit; bit <<= 1; }
        if (mask != lastPower) { lastPower = mask; repaint(); }
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(colours::panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 12.0f);
        g.setColour(fxIdentity);
        g.setFont(titleFont(40.0f));
        g.drawText("FX", juce::Rectangle<int>(24, 10, 200, 42), juce::Justification::centredLeft, false);
        g.setColour(colours::dimText);
        g.setFont(valueFont(13.0f));
        g.drawText("Echoes and room on the sends, then warmth, grit, width and tone on the whole mix. Space (top) sets both wets at once.",
                   juce::Rectangle<int>(24, 50, getWidth() - 48, 18), juce::Justification::centredLeft, false);
        for (auto& s : strips)
        {
            const auto area = s->area.toFloat();
            g.setColour(colours::raised.withAlpha(0.55f));
            g.fillRoundedRectangle(area, 10.0f);
            const auto lit = s->power == nullptr || s->power->getToggleState();
            g.setColour(lit ? fxIdentity.withAlpha(0.55f) : colours::edge);
            g.drawRoundedRectangle(area.reduced(0.5f), 10.0f, 1.0f);
            g.setColour(lit ? colours::text : colours::dimText);
            g.setFont(titleFont(19.0f));
            g.drawText(s->name, s->area.withHeight(34).withTrimmedLeft(14), juce::Justification::centredLeft, false);
            g.setColour(colours::dimText);
            g.setFont(titleFont(13.0f));
            for (const auto& [caption, control] : s->captions)
                g.drawText(caption, control->getBounds().withY(control->getY() - 16).withHeight(15).withWidth(std::max(control->getWidth(), 90)),
                           juce::Justification::centredLeft, false);
        }
    }
    void resized() override
    {
        // Two rows of strips: Delay and Reverb, then the sends and the four
        // finishing effects in the order the sound passes them.
        const std::array<juce::Rectangle<int>, 7> areas{
            juce::Rectangle<int>{16, 74, 504, 204}, {532, 74, 504, 204},
            {16, 288, 196, 150}, {220, 288, 196, 150}, {424, 288, 196, 150}, {628, 288, 196, 150}, {832, 288, 204, 150}};
        for (std::size_t i = 0; i < strips.size(); ++i)
        {
            auto& s = *strips[i];
            s.area = areas[i];
            if (s.power) s.power->setBounds(s.area.getRight() - 96, s.area.getY() + 6, 84, 26);
            int x = s.area.getX() + 14, y = s.area.getY() + 54;
            for (const auto& item : s.items)
            {
                if (item.control == nullptr) { x = s.area.getX() + 14; y += 54; continue; }
                item.control->setBounds(x, y, item.width, 30);
                x += item.width + 8;
            }
        }
    }

private:
    struct Strip
    {
        juce::String name;
        juce::Rectangle<int> area;
        std::unique_ptr<SwitchButton> power;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAttachment;
        struct Item { juce::Component* control; int width; };
        std::vector<Item> items;
        std::vector<std::pair<juce::String, juce::Component*>> captions;
        juce::Component* owner = nullptr;
        void add(juce::Component& control, const juce::String& caption, int width)
        {
            items.push_back({&control, width});
            captions.emplace_back(caption, &control);
            owner->addAndMakeVisible(control);
        }
        void newRow() { items.push_back({nullptr, 0}); }
    };
    Strip& strip(const char* name, const juce::String& help, const char* powerId, juce::AudioProcessorValueTreeState& state)
    {
        auto& s = *strips.emplace_back(std::make_unique<Strip>());
        s.name = name;
        s.owner = this;
        if (powerId != nullptr)
        {
            s.power = std::make_unique<SwitchButton>(juce::String(name).toLowerCase(), SwitchButton::Look::power);
            s.power->setTitle(juce::String(name).substring(0, 1) + juce::String(name).substring(1).toLowerCase() + " power");
            s.power->setAccent(fxIdentity);
            s.power->setTooltip(juce::String(name).substring(0, 1) + juce::String(name).substring(1).toLowerCase() + ": " + help
                                + "Click to switch it on or off; it fades in and out.");
            s.power->setHelpText(s.power->getTooltip());
            s.powerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, powerId, *s.power);
            addAndMakeVisible(*s.power);
        }
        return s;
    }
    NumericKnob& knob(juce::AudioProcessorValueTreeState& state, const char* id, const juce::String& title, const juce::String& help, int decimals)
    {
        auto& k = *knobControls.emplace_back(std::make_unique<NumericKnob>());
        knobIds.emplace_back(id);
        auto& parameter = *state.getParameter(id);
        k.setCompact(true);
        k.setTitle(title);
        k.setTooltip(help + " Double-click resets; click the value to type.");
        k.setHelpText(k.getTooltip());
        k.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.setNumDecimalPlacesToDisplay(decimals);
        k.setPopupDisplayEnabled(false, false, nullptr);
        k.setWantsKeyboardFocus(true);
        k.setColour(juce::Slider::rotarySliderFillColourId, fxIdentity);
        knobAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(state, id, k));
        k.setDoubleClickReturnValue(true, parameter.convertFrom0to1(parameter.getDefaultValue()));
        k.onEntryGesture = [&parameter](bool begin) { if (begin) parameter.beginChangeGesture(); else parameter.endChangeGesture(); };
        return k;
    }
    // Frequencies read "280" below 1 kHz and "6.5k" above, so they fit beside a small dial.
    NumericKnob& frequencyKnob(juce::AudioProcessorValueTreeState& state, const char* id, const juce::String& title, const juce::String& help)
    {
        auto& k = knob(state, id, title, help, 0);
        k.setKiloFormat(true);
        return k;
    }
    SwitchButton& toggle(juce::AudioProcessorValueTreeState& state, const char* id, const juce::String& name, const juce::String& help)
    {
        auto& button = *toggleControls.emplace_back(std::make_unique<SwitchButton>(name, SwitchButton::Look::option));
        button.setAccent(fxIdentity);
        button.setTooltip(help);
        button.setHelpText(help);
        toggleAttachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, id, button));
        return button;
    }
    ChoiceSelector& choice(juce::AudioProcessorValueTreeState& state, const char* id, const juce::String& name, const juce::String& help,
                           const std::vector<const char*>& labels)
    {
        auto& selector = *choiceControls.emplace_back(std::make_unique<ChoiceSelector>(*state.getParameter(id), name, help, labels, 0.0f));
        choiceIds.emplace_back(id);
        selector.setAccent(fxIdentity);
        return selector;
    }

    // Controls are created before the strips that lay them out are used, and
    // destroyed after them: strips only point at them.
    std::vector<std::unique_ptr<NumericKnob>> knobControls;
    std::vector<juce::String> knobIds, choiceIds;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> knobAttachments;
    std::vector<std::unique_ptr<SwitchButton>> toggleControls;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> toggleAttachments;
    std::vector<std::unique_ptr<ChoiceSelector>> choiceControls;
    std::vector<std::unique_ptr<Strip>> strips;
    unsigned lastPower = 0;
};
}
