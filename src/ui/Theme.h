#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

// The suite's visual language (SPEC section 6): a dark graphite base, one
// identity colour per instrument, and per-mode finishes within that colour.
namespace dropforge::ui
{
// Everything is laid out on this design canvas and scaled as a whole, so the
// look stays identical (and vector-crisp) from 75 % to 200 %.
inline constexpr int designWidth = 1100, designHeight = 700;

namespace colours
{
inline const juce::Colour base{0xff141619}, panel{0xff1d2025}, raised{0xff272b31}, edge{0xff3a3f47};
inline const juce::Colour text{0xffe9ecef}, dimText{0xff8b929b}, faint{0xff5a6068};
inline const juce::Colour danger{0xffe8452c}, lampOff{0xff2c3036};
}

struct InstrumentStyle
{
    const char* name;  // engraved on tabs and page headers
    const char* title; // spoken and written name
    const char* role;
    juce::Colour identity;
};

inline const InstrumentStyle anvilStyle{"ANVIL", "Anvil", "Bass: the lowest key an octave down, exactly mono.", juce::Colour(0xff5b8fd0)};
inline const InstrumentStyle furnaceStyle{"FURNACE", "Furnace", "Chord wall: every held key, wide and heavy.", juce::Colour(0xffef7a2a)};

inline const std::array<const char*, 4> modeLetters{"A", "B", "C", "D"};
inline const std::array<const char*, 4> modeNames{"Forge", "Riot", "Halo", "Alloy"};

// One finish per mode, like different amp heads: the same controls in the
// same places, a different material and accent within the identity colour.
struct Finish
{
    juce::Colour accent, plateTop, plateBottom;
    const char* character;
};

inline Finish finishFor(const InstrumentStyle& style, int mode)
{
    const auto id = style.identity;
    switch (mode)
    {
        case 1: return {id.withRotatedHue(-0.03f).withMultipliedSaturation(1.15f).brighter(0.15f),
                        juce::Colour(0xff2a2422), juce::Colour(0xff17120f), "Riot: harder, brighter, talking."};
        case 2: return {id.interpolatedWith(juce::Colours::white, 0.35f),
                        juce::Colour(0xff2b2f37), juce::Colour(0xff1a1d23), "Halo: smooth, rounded, shimmering."};
        case 3: return {id.withRotatedHue(0.06f).interpolatedWith(juce::Colour(0xffc8d2dc), 0.3f),
                        juce::Colour(0xff2c3134), juce::Colour(0xff15181a), "Alloy: FM, glassy and metallic."};
        default: return {id, juce::Colour(0xff2a2d33), juce::Colour(0xff181a1e), "Forge: balanced, lush, heavy."};
    }
}

// Condensed bold for names (no embedded font yet: SPEC section 9 registers one first).
inline juce::Font titleFont(float height)
{
    return juce::Font(juce::FontOptions(height).withStyle("Bold").withHorizontalScale(0.86f));
}

inline juce::Font valueFont(float height) { return juce::Font(juce::FontOptions(height)); }

inline void drawLamp(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, bool lit)
{
    if (lit)
    {
        g.setColour(colour.withAlpha(0.25f));
        g.fillEllipse(area.expanded(area.getWidth() * 0.45f));
    }
    g.setColour(lit ? colour : colours::lampOff);
    g.fillEllipse(area);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawEllipse(area, 1.0f);
}

inline void drawFocus(juce::Graphics& g, juce::Component& component, float radius)
{
    if (!component.hasKeyboardFocus(true)) return;
    g.setColour(colours::text.withAlpha(0.85f));
    g.drawRoundedRectangle(component.getLocalBounds().toFloat().reduced(2.0f), radius, 1.5f);
}
}
