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
}
