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
// Blade, the lead (SPEC section 3): off by default.
inline constexpr auto leadEnabled = "lead_on";
inline constexpr auto leadLevel = "lead_level";
inline constexpr auto leadMode = "lead_mode";
inline constexpr auto leadDestroy = "lead_destroy";
inline constexpr auto leadPortamento = "lead_porta";
inline constexpr auto leadPortamentoMs = "lead_porta_ms";
inline constexpr auto leadOctave = "lead_octave";
// Performance (SPEC section 3).
inline constexpr auto bendRange = "bend_range";
inline constexpr auto glideOn = "glide_on";
inline constexpr auto glideMs = "glide_ms";
inline constexpr auto impactOn = "impact_on";
inline constexpr auto wheelMode = "wheel_mode";
inline constexpr auto wheelAmount = "wheel_amount";
// FX page (SPEC section 8).
inline constexpr auto fxSendAnvil = "fx_send_anvil";
inline constexpr auto fxSendFurnace = "fx_send_furnace";
inline constexpr auto fxSendBlade = "fx_send_blade";
inline constexpr auto fxDuck = "fx_duck";
inline constexpr auto fxDelayOn = "fx_delay_on";
inline constexpr auto fxDelaySync = "fx_delay_sync";
inline constexpr auto fxDelayDivision = "fx_delay_division";
inline constexpr auto fxDelayFeel = "fx_delay_feel";
inline constexpr auto fxDelayMs = "fx_delay_ms";
inline constexpr auto fxDelayFeedback = "fx_delay_feedback";
inline constexpr auto fxDelayMode = "fx_delay_mode";
inline constexpr auto fxDelayWidth = "fx_delay_width";
inline constexpr auto fxDelayMix = "fx_delay_mix";
inline constexpr auto fxDelayLowCut = "fx_delay_low_cut";
inline constexpr auto fxDelayHighCut = "fx_delay_high_cut";
inline constexpr auto fxReverbOn = "fx_reverb_on";
inline constexpr auto fxReverbDecay = "fx_reverb_decay";
inline constexpr auto fxReverbPreDelay = "fx_reverb_predelay";
inline constexpr auto fxReverbSize = "fx_reverb_size";
inline constexpr auto fxReverbDiffusion = "fx_reverb_diffusion";
inline constexpr auto fxReverbDamping = "fx_reverb_damping";
inline constexpr auto fxReverbModulation = "fx_reverb_mod";
inline constexpr auto fxReverbWidth = "fx_reverb_width";
inline constexpr auto fxReverbMix = "fx_reverb_mix";
inline constexpr auto fxReverbLowCut = "fx_reverb_low_cut";
inline constexpr auto fxReverbHighCut = "fx_reverb_high_cut";
inline constexpr auto spaceAmount = "space_amount";
}
