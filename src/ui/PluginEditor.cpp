#include "PluginEditor.h"
#include "plugin/PluginProcessor.h"

DropforgeEditor::DropforgeEditor(DropforgeProcessor& processor)
    : AudioProcessorEditor(processor), owner(processor),
      anvilAttachment(processor.parameterState(), dropforge::parameters::bassLevel, anvilLevel),
      furnaceAttachment(processor.parameterState(), dropforge::parameters::rhythmLevel, furnaceLevel),
      outputAttachment(processor.parameterState(), dropforge::parameters::output, output)
{
    title.setText(JucePlugin_Name, juce::dontSendNotification);
    title.setFont(juce::FontOptions(28.0f));
    anvilTab.onClick = [this] { selectPage(false); };
    furnaceTab.onClick = [this] { selectPage(true); };
    anvilTab.onPowerToggle = [this] { toggleLayer(dropforge::parameters::bassEnabled); };
    furnaceTab.onPowerToggle = [this] { toggleLayer(dropforge::parameters::rhythmEnabled); };
    anvilTab.setTooltip("Anvil: select tab; right-click icon to enable or disable. Alt+Enter also toggles power.");
    furnaceTab.setTooltip("Furnace: select tab; right-click icon to enable or disable. Alt+Enter also toggles power.");
    for (auto* tab : {&anvilTab, &furnaceTab})
    {
        tab->setRadioGroupId(1);
        tab->setClickingTogglesState(true);
    }
    levelLabel.setText("Level", juce::dontSendNotification);
    outputLabel.setText("Output", juce::dontSendNotification);
    for (auto* label : {&levelLabel, &outputLabel})
    {
        label->setJustificationType(juce::Justification::centred);
        label->setFont(juce::FontOptions(15.0f));
    }
    hoverHelp.setTitle("Control description");
    hoverHelp.setFont(juce::FontOptions(12.0f));
    hoverHelp.setJustificationType(juce::Justification::centredLeft);
    hoverHelp.setMinimumHorizontalScale(1.0f);
    anvilLevel.setTitle("Anvil level");
    furnaceLevel.setTitle("Furnace level");
    output.setTitle("Output gain");
    anvilLevel.setTooltip("Anvil level. Double-click resets; right-click types dB. -60 dB silences this layer.");
    furnaceLevel.setTooltip("Furnace level. Double-click resets; right-click types dB. -60 dB silences this layer.");
    output.setTooltip("Master output level. Double-click resets to 0 dB; right-click types an exact value.");
    outputLabel.setTooltip(output.getTooltip());
    const std::array<const char*, 3> ids{dropforge::parameters::bassLevel, dropforge::parameters::rhythmLevel,
                                      dropforge::parameters::output};
    const std::array<NumericKnob*, 3> knobs{&anvilLevel, &furnaceLevel, &output};
    for (std::size_t i = 0; i < knobs.size(); ++i)
    {
        auto& knob = *knobs[i];
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        knob.setNumDecimalPlacesToDisplay(3);
        knob.setTextValueSuffix(" dB");
        knob.setDoubleClickReturnValue(true, 0.0);
        knob.setPopupDisplayEnabled(false, false, nullptr);
        knob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffe9a153));
        knob.setHelpText(knob.getTooltip());
        knob.setWantsKeyboardFocus(true);
        auto* parameter = processor.parameterState().getParameter(ids[i]);
        knob.onEntryGesture = [parameter](bool begin)
        {
            if (begin) parameter->beginChangeGesture(); else parameter->endChangeGesture();
        };
    }
    panic.setTooltip("Stop all sounding notes.");
    panic.onClick = [&processor] { processor.requestPanic(); };
    help.setTooltip("About Dropforge.");
    help.onClick = [this] { showAbout(); };
    for (auto* child : std::array<juce::Component*, 11>{&title, &levelLabel, &outputLabel, &hoverHelp,
            &anvilLevel, &furnaceLevel, &output, &anvilTab, &furnaceTab, &panic, &help})
        addAndMakeVisible(child);
    for (auto* button : std::array<juce::Button*, 4>{&anvilTab, &furnaceTab, &panic, &help})
    {
        button->setWantsKeyboardFocus(true);
        button->setTitle(button->getButtonText());
        button->setHelpText(button->getTooltip());
    }
    anvilTab.setExplicitFocusOrder(1); furnaceTab.setExplicitFocusOrder(2);
    anvilLevel.setExplicitFocusOrder(3); furnaceLevel.setExplicitFocusOrder(3);
    output.setExplicitFocusOrder(4); panic.setExplicitFocusOrder(5); help.setExplicitFocusOrder(6);
    addMouseListener(this, true);
    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(560, 360, 1000, 600);
    setSize(680, 400);
    timerCallback();
    startTimerHz(30);
}

DropforgeEditor::~DropforgeEditor() { removeMouseListener(this); }

void DropforgeEditor::mouseDown(const juce::MouseEvent& event)
{
    for (auto* knob : {&anvilLevel, &furnaceLevel, &output}) knob->commitIfOutside(event.eventComponent);
}

void DropforgeEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff17191d));
    g.setColour(juce::Colour(0xff292d33));
    g.fillRoundedRectangle(24.0f, 132.0f, getWidth() - 48.0f, getHeight() - 174.0f, 8.0f);
    // Connect the selected tab to the page rather than drawing separate buttons.
    g.fillRect(24, 132, getWidth() - 48, 12);
    g.setColour(juce::Colour(0xff101216));
    g.fillRect(0, getHeight() - 30, getWidth(), 30);
    g.setColour(juce::Colour(0xff353a42));
    g.drawHorizontalLine(getHeight() - 30, 0.0f, static_cast<float>(getWidth()));
}

void DropforgeEditor::resized()
{
    const auto width = getWidth();
    title.setBounds(24, 20, width - 140, 40);
    help.setBounds(width - 84, 25, 60, 30);
    const auto tabWidth = (width - 48) / 2;
    anvilTab.setBounds(24, 88, tabWidth, 46);
    furnaceTab.setBounds(24 + tabWidth, 88, width - 48 - tabWidth, 46);
    const auto leftCentre = width / 4 + 12, rightCentre = width * 3 / 4 - 12;
    levelLabel.setBounds(leftCentre - 90, 153, 180, 26);
    outputLabel.setBounds(rightCentre - 90, 153, 180, 26);
    const auto knobHeight = getHeight() - 266;
    anvilLevel.setBounds(leftCentre - 84, 180, 168, knobHeight);
    furnaceLevel.setBounds(anvilLevel.getBounds());
    output.setBounds(rightCentre - 84, 180, 168, knobHeight);
    panic.setBounds(width - 128, getHeight() - 78, 88, 28);
    hoverHelp.setBounds(16, getHeight() - 28, width - 32, 26);
}

void DropforgeEditor::toggleLayer(const char* id)
{
    auto* parameter = owner.parameterState().getParameter(id);
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->getValue() >= 0.5f ? 0.0f : 1.0f);
    parameter->endChangeGesture();
    timerCallback();
}

void DropforgeEditor::selectPage(bool furnace)
{
    if (furnace != owner.isRhythmPageSelected())
    {
        anvilLevel.finishEntry(true);
        furnaceLevel.finishEntry(true);
    }
    owner.selectRhythmPage(furnace);
    timerCallback();
}

void DropforgeEditor::timerCallback()
{
    const auto furnace = owner.isRhythmPageSelected();
    anvilTab.setToggleState(!furnace, juce::dontSendNotification);
    furnaceTab.setToggleState(furnace, juce::dontSendNotification);
    anvilTab.setLayerEnabled(owner.parameterState().getRawParameterValue(dropforge::parameters::bassEnabled)->load() >= 0.5f);
    furnaceTab.setLayerEnabled(owner.parameterState().getRawParameterValue(dropforge::parameters::rhythmEnabled)->load() >= 0.5f);
    for (auto* tab : {&anvilTab, &furnaceTab})
        tab->setDescription(tab->getButtonText() + (tab->getToggleState() ? " page selected. " : " page. ")
                            + (tab->isLayerEnabled() ? "Layer on." : "Layer off; still editable."));
    if (anvilLevel.isVisible() == furnace) anvilLevel.finishEntry(true);
    if (furnaceLevel.isVisible() != furnace) furnaceLevel.finishEntry(true);
    anvilLevel.setVisible(!furnace); furnaceLevel.setVisible(furnace);
    levelLabel.setTooltip(furnace ? furnaceLevel.getTooltip() : anvilLevel.getTooltip());
    const auto describe = [this](juce::Component* target) -> juce::String
    {
        if (target == nullptr || (target != this && !isParentOf(target))) return {};
        for (auto* component = target; component != nullptr && component != this; component = component->getParentComponent())
            if (auto* tooltip = dynamic_cast<juce::TooltipClient*>(component))
                if (const auto text = tooltip->getTooltip(); text.isNotEmpty()) return text;
        return {};
    };
    auto description = describe(juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse());
    if (description.isEmpty()) description = describe(juce::Component::getCurrentlyFocusedComponent());
    hoverHelp.setText(description.replaceCharacter('\n', ' '), juce::dontSendNotification);
}

void DropforgeEditor::showAbout()
{
    aboutBox = juce::NativeMessageBox::showScopedAsync(
        juce::MessageBoxOptions{}.withTitle("About Dropforge")
            .withMessage(juce::String(JucePlugin_Name) + " " + JucePlugin_VersionString + "\n\nHugo Cacciatori")
            .withButton("Close").withAssociatedComponent(this), {});
}
