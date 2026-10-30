#include "PluginEditor.h"
#include "plugin/PluginProcessor.h"

namespace
{
using namespace dropforge::ui;
namespace ids = dropforge::parameters;

// Header buttons drawn in the suite's own style instead of the stock look.
struct SuiteLookAndFeel final : juce::LookAndFeel_V4
{
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down) override
    {
        if (button.getProperties()["mark"]) return;
        const auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        g.setGradientFill(juce::ColourGradient(colours::raised.brighter(highlighted ? 0.12f : 0.0f), 0.0f, bounds.getY(),
                                               colours::panel.darker(down ? 0.35f : 0.1f), 0.0f, bounds.getBottom(), false));
        g.fillRoundedRectangle(bounds, 7.0f);
        g.setColour(colours::edge);
        g.drawRoundedRectangle(bounds, 7.0f, 1.0f);
        drawFocus(g, button, 7.0f);
    }
    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool highlighted, bool) override
    {
        const auto isMark = static_cast<bool>(button.getProperties()["mark"]);
        g.setFont(isMark ? titleFont(34.0f) : titleFont(18.0f));
        g.setColour(isMark ? (highlighted ? colours::text : colours::text.withAlpha(0.9f)) : colours::text);
        const auto text = button.getButtonText();
        g.drawText(text.length() > 1 ? text.toUpperCase() : text, button.getLocalBounds(),
                   isMark ? juce::Justification::centredLeft : juce::Justification::centred, false);
    }
};

SuiteLookAndFeel& suiteLook()
{
    static SuiteLookAndFeel look;
    return look;
}

const juce::Rectangle<int> headerArea{0, 0, designWidth, 84};
const juce::Rectangle<int> pageArea{24, 148, designWidth - 48, 446};
// Footer: the performance strip, the routing keyboard and the help line.
const juce::Rectangle<int> footerArea{0, 604, designWidth, designHeight - 604};
}

DropforgeEditor::DropforgeEditor(DropforgeProcessor& processor)
    : AudioProcessorEditor(processor), owner(processor), strip(processor.parameterState()),
      outputAttachment(processor.parameterState(), ids::output, output)
{
    setLookAndFeel(&suiteLook());
    auto& state = processor.parameterState();
    pages[0] = std::make_unique<InstrumentPage>(state, PageSpec{anvilStyle, ids::bassEnabled, ids::bassMode, ids::bassLevel, ids::bassDestroy, {}});
    pages[1] = std::make_unique<InstrumentPage>(state, PageSpec{furnaceStyle, ids::rhythmEnabled, ids::rhythmMode, ids::rhythmLevel, ids::rhythmDestroy,
        {{ids::rhythmMono, "Mono", "folds the wall to the centre, for a tight, focused chord without phasing."},
         {ids::rhythmDrop, "Drop", "plays the wall an octave down, in unison with Anvil, for the heaviest drops."}}});
    pages[2] = std::make_unique<InstrumentPage>(state, PageSpec{bladeStyle, ids::leadEnabled, ids::leadMode, ids::leadLevel, ids::leadDestroy, {},
        {{ids::leadPortamento, "Portamento", "Off jumps to each note; Legato slides between overlapping notes without a new attack; "
                                               "Always also slides into a new note from the last one.", {"Off", "Legato", "Always"}, 0.0f},
         {ids::leadOctave, "Octave", "plays the top key an octave down or up, still in tune with Furnace.", {"-1", "0", "+1"}, -1.0f}},
        {{ids::leadPortamentoMs, "Time", "how long Portamento takes to reach the new note.", " ms"}}});
    for (int page = 0; page < pageCount; ++page)
    {
        auto& tab = *(tabs[(size_t) page] = std::make_unique<InstrumentTab>(pages[(size_t) page]->pageSpec().style));
        const juce::String title = tab.getButtonText();
        tab.setTooltip(title + ": click to open its page; right-click (or Alt+Enter) switches " + title + " on or off. Ctrl+"
                       + juce::String(page + 1) + " opens it from anywhere.");
        tab.setHelpText(tab.getTooltip());
        tab.setRadioGroupId(1);
        tab.onClick = [this, page] { selectPage(page); };
        tab.onPowerToggle = [this, page] { togglePower(page); };
        tab.setExplicitFocusOrder(1 + page);
        canvas.addAndMakeVisible(tab);
        canvas.addChildComponent(*pages[(size_t) page]);
        pages[(size_t) page]->setExplicitFocusOrder(10);
    }

    mark.getProperties().set("mark", true);
    mark.setTooltip("About Dropforge: version, credits and licence.");
    about.setTooltip(mark.getTooltip());
    panic.setTooltip("Panic: stops every sounding note at once.");
    panic.onClick = [&processor] { processor.requestPanic(); };
    for (auto* button : {&mark, &about}) button->onClick = [this] { showAbout(); };
    for (auto* button : {&mark, &panic, &about})
    {
        button->setWantsKeyboardFocus(true);
        button->setTitle(button == &about ? juce::String("About") : button->getButtonText());
        button->setHelpText(button->getTooltip());
    }
    mark.setExplicitFocusOrder(20); panic.setExplicitFocusOrder(21); output.setExplicitFocusOrder(22); about.setExplicitFocusOrder(23);

    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    outputLabel.setFont(titleFont(16.0f));
    outputLabel.setColour(juce::Label::textColourId, colours::dimText);
    outputLabel.setJustificationType(juce::Justification::centredRight);
    outputLabel.setInterceptsMouseClicks(false, false);
    output.setTitle("Output gain");
    output.setTooltip("Output: master level after the safety limiter. Double-click resets to 0 dB; click the value to type.");
    output.setHelpText(output.getTooltip());
    output.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    output.setNumDecimalPlacesToDisplay(1);
    output.setTextValueSuffix(" dB");
    output.setDoubleClickReturnValue(true, 0.0);
    output.setPopupDisplayEnabled(false, false, nullptr);
    output.setWantsKeyboardFocus(true);
    output.setColour(juce::Slider::rotarySliderFillColourId, colours::text.withAlpha(0.8f));
    auto* outputParameter = state.getParameter(ids::output);
    output.onEntryGesture = [outputParameter](bool begin) { if (begin) outputParameter->beginChangeGesture(); else outputParameter->endChangeGesture(); };

    hoverHelp.setTitle("Control description");
    hoverHelp.setFont(valueFont(14.0f));
    hoverHelp.setColour(juce::Label::textColourId, colours::dimText);
    hoverHelp.setJustificationType(juce::Justification::centredLeft);
    hoverHelp.setMinimumHorizontalScale(1.0f);
    strip.setExplicitFocusOrder(30);
    for (auto* child : std::initializer_list<juce::Component*>{&mark, &panic, &outputLabel, &output, &meter, &about, &strip, &keyboard, &hoverHelp})
        canvas.addAndMakeVisible(child);
    addAndMakeVisible(canvas);
    layoutCanvas();

    addMouseListener(this, true);
    setWantsKeyboardFocus(true);
    setResizable(true, true);
    setResizeLimits(designWidth * 3 / 4, designHeight * 3 / 4, designWidth * 2, designHeight * 2);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(designWidth) / designHeight);
    setSize(designWidth, designHeight);
    timerCallback();
    startTimerHz(30);
}

DropforgeEditor::~DropforgeEditor()
{
    removeMouseListener(this);
    setLookAndFeel(nullptr);
}

void DropforgeEditor::mouseDown(const juce::MouseEvent& event)
{
    output.commitIfOutside(event.eventComponent);
    for (auto& page : pages) for (auto* knob : page->knobs()) knob->commitIfOutside(event.eventComponent);
}

bool DropforgeEditor::keyPressed(const juce::KeyPress& key)
{
    // Ctrl+1... opens the pages in signal order.
    if (key.getModifiers().isCtrlDown())
        for (int page = 0; page < pageCount; ++page)
            if (key.getKeyCode() == '1' + page) { selectPage(page); return true; }
    return false;
}

void DropforgeEditor::paint(juce::Graphics& g)
{
    // Only visible outside the canvas when a host forces another aspect ratio.
    g.fillAll(colours::base);
}

void DropforgeEditor::paintCanvas(juce::Graphics& g)
{
    g.fillAll(colours::base);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff22252b), 0.0f, 0.0f, colours::base, 0.0f, static_cast<float>(headerArea.getBottom()), false));
    g.fillRect(headerArea);
    g.setColour(colours::edge.withAlpha(0.6f));
    g.drawHorizontalLine(headerArea.getBottom(), 0.0f, static_cast<float>(designWidth));
    // A thin rule in every instrument's identity colour under the product mark.
    const std::array<juce::Colour, 3> identities{anvilStyle.identity, furnaceStyle.identity, bladeStyle.identity};
    for (size_t i = 0; i < identities.size(); ++i)
    {
        g.setColour(identities[i]);
        g.fillRect(24 + static_cast<int>(i) * 34, 60, 30, 3);
    }
    g.setColour(colours::faint);
    g.setFont(valueFont(12.0f));
    g.drawText("HEAVY SYNTH SUITE", juce::Rectangle<int>(134, 52, 160, 18), juce::Justification::centredLeft, false);
    g.setColour(juce::Colour(0xff0f1113));
    g.fillRect(footerArea);
    g.setColour(colours::edge.withAlpha(0.6f));
    g.drawHorizontalLine(footerArea.getY(), 0.0f, static_cast<float>(designWidth));
}

void DropforgeEditor::layoutCanvas()
{
    canvas.setBounds(0, 0, designWidth, designHeight);
    mark.setBounds(20, 10, 230, 46);
    panic.setBounds(600, 24, 96, 36);
    outputLabel.setBounds(704, 30, 66, 24);
    output.setBounds(776, 4, 80, 76);
    meter.setBounds(870, 24, 166, 36);
    about.setBounds(1046, 26, 32, 32);
    for (int page = 0; page < pageCount; ++page)
    {
        tabs[(size_t) page]->setBounds(pageArea.getX() + page * 240, 94, 232, 56);
        pages[(size_t) page]->setBounds(pageArea);
    }
    strip.setBounds(footerArea.getX() + 24, footerArea.getY() + 6, footerArea.getWidth() - 48, 32);
    keyboard.setBounds(footerArea.getX() + 24, footerArea.getY() + 44, footerArea.getWidth() - 48, 26);
    hoverHelp.setBounds(footerArea.getX() + 24, footerArea.getY() + 72, footerArea.getWidth() - 48, 22);
}

void DropforgeEditor::resized()
{
    // Scale the whole canvas: vector drawing stays crisp at every size, and a
    // host that ignores the aspect ratio gets the canvas centred, never cropped.
    const auto scale = std::min(getWidth() / static_cast<float>(designWidth), getHeight() / static_cast<float>(designHeight));
    const auto x = (getWidth() - designWidth * scale) * 0.5f, y = (getHeight() - designHeight * scale) * 0.5f;
    canvas.setTransform(juce::AffineTransform::scale(scale).translated(x, y));
}

void DropforgeEditor::togglePower(int page)
{
    auto* parameter = owner.parameterState().getParameter(pages[(size_t) page]->pageSpec().powerId);
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->getValue() >= 0.5f ? 0.0f : 1.0f);
    parameter->endChangeGesture();
    timerCallback();
}

void DropforgeEditor::selectPage(int page)
{
    if (page != owner.currentPage()) for (auto* knob : pages[(size_t) owner.currentPage()]->knobs()) knob->finishEntry(true);
    owner.showPage(page);
    timerCallback();
}

void DropforgeEditor::timerCallback()
{
    const auto meters = owner.takeMeters();
    meter.push(meters.peakLeft, meters.peakRight, meters.limiterReductionDb);
    strip.showWheel(owner.wheelPosition());
    const auto current = juce::jlimit(0, pageCount - 1, owner.currentPage());
    auto& state = owner.parameterState();
    const auto keys = owner.heldKeys();
    keyboard.show(keys.low, keys.high, keys.bassNote, keys.leadNote, state.getRawParameterValue(ids::bassEnabled)->load() >= 0.5f,
                  state.getRawParameterValue(ids::rhythmEnabled)->load() >= 0.5f, state.getRawParameterValue(ids::leadEnabled)->load() >= 0.5f);
    for (int index = 0; index < pageCount; ++index)
    {
        auto& tab = *tabs[(size_t) index];
        auto& page = *pages[(size_t) index];
        const auto& spec = page.pageSpec();
        const auto enabled = state.getRawParameterValue(spec.powerId)->load() >= 0.5f;
        const auto mode = juce::jlimit(0, 3, juce::roundToInt(state.getRawParameterValue(spec.modeId)->load()));
        tab.setStatus(enabled, mode, state.getRawParameterValue(spec.destroyId)->load() >= 0.5f);
        tab.setToggleState(index == current, juce::dontSendNotification);
        tab.setDescription(tab.getButtonText() + (index == current ? " page open. " : " page. ")
                           + (enabled ? "On, mode " + juce::String(modeLetters[(size_t) mode]) + " " + modeNames[(size_t) mode] + "."
                                      : juce::String("Off; still editable.")));
        // Hidden pages neither repaint nor keep a typing field open.
        if (index != current && page.isVisible()) for (auto* knob : page.knobs()) knob->finishEntry(true);
        page.setVisible(index == current);
        if (index == current) page.showMode(mode);
    }
    const auto describe = [this](juce::Component* target) -> juce::String
    {
        if (target == nullptr || !canvas.isParentOf(target)) return {};
        for (auto* component = target; component != nullptr && component != &canvas; component = component->getParentComponent())
            if (auto* tooltip = dynamic_cast<juce::TooltipClient*>(component))
                if (const auto text = tooltip->getTooltip(); text.isNotEmpty()) return text;
        return {};
    };
    auto description = describe(juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse());
    if (description.isEmpty()) description = describe(juce::Component::getCurrentlyFocusedComponent());
    if (description.isEmpty()) description = "Play chords: Anvil takes the lowest key an octave down, Furnace plays the wall, Blade the top key. Hover any control for help.";
    hoverHelp.setText(description.replaceCharacter('\n', ' '), juce::dontSendNotification);
}

void DropforgeEditor::showAbout()
{
    aboutBox = juce::NativeMessageBox::showScopedAsync(
        juce::MessageBoxOptions{}.withTitle("About Dropforge")
            .withMessage(juce::String(JucePlugin_Name) + " " + JucePlugin_VersionString
                         + "\nHeavy synth suite for breakdowns and drops.\n\nBy Hugo Cacciatori. Free to use for music; see the licence"
                           " file that comes with it.\nBuilt with JUCE (juce.com), used under its licence.")
            .withButton("Close").withAssociatedComponent(this), {});
}
