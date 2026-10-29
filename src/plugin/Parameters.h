#pragma once

namespace dropforge::parameters
{
// Stable host IDs. They name the musical role (bass, rhythm, lead), not the
// visible layer names, so the UI can rename layers without breaking projects.
// Never rename or reuse an ID once released; add each one only with its sound.
inline constexpr auto output = "output";
inline constexpr auto bassEnabled = "bass_on";
inline constexpr auto bassLevel = "bass_level";
inline constexpr auto rhythmEnabled = "rhythm_on";
inline constexpr auto rhythmLevel = "rhythm_level";
// Modes A-D (SPEC section 12); all four choices exist from the first release.
inline constexpr auto bassMode = "bass_mode";
inline constexpr auto rhythmMode = "rhythm_mode";
// Furnace options (SPEC section 3): a centred image, and the wall an octave down.
inline constexpr auto rhythmMono = "rhythm_center"; // Ironstack used "rhythm_mono"
inline constexpr auto rhythmDrop = "rhythm_drop";
// Destruction per instrument (SPEC section 12).
inline constexpr auto bassDestroy = "bass_destroy";
inline constexpr auto rhythmDestroy = "rhythm_destroy";
}
