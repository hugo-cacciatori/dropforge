#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include "Engine.h"
#include "FxDelay.h"
#include "FxFinish.h"
#include "FxReverb.h"

namespace dropforge
{
// The FX page's chain between the instruments and the master (SPEC section 8).
// Each instrument sends its finished signal (after power and Level) to the
// delay and the reverb side by side (the reverb never hears the echoes); both
// return in parallel with the dry mix and duck under it, so
// chops stay hard and tails bloom in the gaps. While every effect is off and
// its tail has drained the chain does nothing at all: the dry sound is
// bit-identical to a build without it.
class FxChain
{
public:
    // Note-value divisions in beats (4/4) and feels (SPEC section 8).
    static constexpr std::array<double, 8> divisionBeats{0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
    static constexpr std::array<double, 3> feelScale{1.0, 1.5, 2.0 / 3.0};
    enum class Feel { straight, dotted, triplet };
    struct Settings
    {
        float sendAnvil = 0.6f, sendFurnace = 1.0f, sendBlade = 0.6f, duck = 0.5f;
        bool delayOn = false, delaySync = true, delayPingPong = true;
        int delayDivision = 3; // 1/8
        Feel delayFeel = Feel::dotted;
        float delayMs = 375.0f, delayFeedback = 0.25f, delayWidth = 1.0f, delayMix = 0.5f;
        float delayLowCutHz = 280.0f, delayHighCutHz = 6500.0f;
        bool reverbOn = false;
        FxReverb::Settings reverb{};
        float reverbMix = 0.4f;
        // Space (0-1): both wets at once; 0.5 plays them as set, 0 dry,
        // 1 doubles them.
        float space = 0.5f;
        // Crush, Chorus and EQ run here; Saturate in the master (Master.h).
        FxFinish::Settings finish{};
        bool saturateOn = false;
        float saturateDriveDb = 6.0f, saturateMix = 1.0f;
    };
    // The synced or free delay time in seconds at this tempo.
    static double delaySeconds(const Settings& settings, double bpm) noexcept
    {
        if (!settings.delaySync) return std::clamp(static_cast<double>(settings.delayMs), 1.0, 2000.0) / 1000.0;
        const auto beats = divisionBeats[static_cast<std::size_t>(std::clamp(settings.delayDivision, 0, 7))]
                         * feelScale[static_cast<std::size_t>(settings.delayFeel)];
        return std::min(beats * 60.0 / tempo(bpm), FxDelay::maximumSeconds);
    }
    // The owner's routing rules (SPEC section 8, 2026-10-30): Anvil feeds the
    // Delay and Reverb only while it plays alone, so the low end stays dry
    // in a mix; the Delay may ping-pong only on a mono source (Furnace Mono,
    // or Blade or Anvil alone), never on the stereo wall.
    static bool anvilSendOpen(bool anvil, bool furnace, bool blade) noexcept { return anvil && !furnace && !blade; }
    static bool pingPongAllowed(bool anvil, bool furnace, bool blade, bool furnaceMono) noexcept
    {
        return furnaceMono || (blade && !anvil && !furnace) || anvilSendOpen(anvil, furnace, blade);
    }
    // The host's tempo, 20-300 BPM; anything else (no host tempo) is 120.
    static double tempo(double bpm) noexcept { return std::isfinite(bpm) && bpm >= 20.0 && bpm <= 300.0 ? bpm : 120.0; }

    // Not real-time: allocates the delay lines.
    void prepare(double sampleRate)
    {
        rate = sampleRate;
        delay.prepare(rate);
        reverb.prepare(rate);
        finish.prepare(rate);
        fadeStep = static_cast<float>(1.0 / std::ceil(rate * 0.030));
        smoothing = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * rate)));
        // Ducking: a 1 ms attack so a chop pulls the echoes down at once, a
        // 250 ms release so they bloom back in the gap after it.
        attack = static_cast<float>(1.0 - std::exp(-1.0 / (0.001 * rate)));
        release = static_cast<float>(1.0 - std::exp(-1.0 / (0.250 * rate)));
        set(settings, bpm); // settings chosen before prepare() take this rate
        reset();
    }
    // Tails cleared (panic, every instrument off, transport reset).
    void reset() noexcept
    {
        delay.reset();
        reverb.reset();
        finish.reset();
        delayInput = settings.delayOn ? 1.0f : 0.0f;
        reverbInput = settings.reverbOn ? 1.0f : 0.0f;
        reverbMix = settings.reverbMix;
        space = settings.space;
        sends = {settings.sendAnvil, settings.sendFurnace, settings.sendBlade};
        mix = settings.delayMix;
        duck = settings.duck;
        follower = 0.0f;
        running = false;
    }
    void set(const Settings& chosen, double hostBpm) noexcept
    {
        settings = chosen;
        bpm = hostBpm;
        delay.set({delaySeconds(settings, bpm), settings.delayFeedback, settings.delayWidth, settings.delayLowCutHz,
                   settings.delayHighCutHz, settings.delayPingPong});
        reverb.set(settings.reverb);
        finish.set(settings.finish);
    }
    // Something to do: an effect on, or a tail still draining.
    bool active() const noexcept { return returnsActive() || finish.active(); }

    // Adds the returns to the dry mix in left/right. The taps hold each
    // instrument's signal for the same samples; mask says which instruments
    // are in the output (a stem render sends only its own instrument).
    void process(float* left, float* right, const LayerTaps& taps, LayerMask mask, int count) noexcept
    {
        if (!active()) return;
        const auto returns = returnsActive();
        running = running || returns;
        const auto bits = static_cast<unsigned>(mask);
        const auto target = settings.delayOn ? 1.0f : 0.0f, reverbTarget = settings.reverbOn ? 1.0f : 0.0f;
        const auto fade = [this](float& value, float goal) noexcept
        {
            value = goal > value ? std::min(goal, value + fadeStep) : std::max(goal, value - fadeStep);
        };
        for (int i = 0; i < count && returns; ++i)
        {
            fade(delayInput, target);
            fade(reverbInput, reverbTarget);
            sends[0] += smoothing * (settings.sendAnvil - sends[0]);
            sends[1] += smoothing * (settings.sendFurnace - sends[1]);
            sends[2] += smoothing * (settings.sendBlade - sends[2]);
            mix += smoothing * (settings.delayMix - mix);
            reverbMix += smoothing * (settings.reverbMix - reverbMix);
            space += smoothing * (settings.space - space);
            duck += smoothing * (settings.duck - duck);
            auto sendLeft = 0.0f, sendRight = 0.0f;
            if ((bits & 1u) != 0) { sendLeft += sends[0] * taps.bassLeft[i]; sendRight += sends[0] * taps.bassRight[i]; }
            if ((bits & 2u) != 0) { sendLeft += sends[1] * taps.rhythmLeft[i]; sendRight += sends[1] * taps.rhythmRight[i]; }
            if ((bits & 4u) != 0) { sendLeft += sends[2] * taps.lead[i]; sendRight += sends[2] * taps.lead[i]; }
            const auto echo = delay.process(delayInput * sendLeft, delayInput * sendRight);
            const auto room = reverb.process(reverbInput * sendLeft, reverbInput * sendRight);
            // The follower hears the dry mix; at full Ducking a loud dry
            // signal (about -12 dBFS and up) pulls the returns down 20 dB.
            const auto level = std::max(std::abs(left[i]), std::abs(right[i]));
            follower += (level > follower ? attack : release) * (level - follower);
            const auto ducked = 2.0f * space * (1.0f - 0.9f * duck * std::min(1.0f, 4.0f * follower));
            left[i] += ducked * (mix * echo.left + reverbMix * room.left);
            right[i] += ducked * (mix * echo.right + reverbMix * room.right);
        }
        // Off, faded and drained: back to the bypass, with the lines cleared
        // so the next start is exactly like the first.
        if (returns && !settings.delayOn && !settings.reverbOn && delayInput == 0.0f && reverbInput == 0.0f && delay.silent() && reverb.silent())
        {
            delay.reset();
            reverb.reset();
            running = false;
        }
        if (finish.active())
            for (int i = 0; i < count; ++i) finish.process(left[i], right[i]);
    }

private:
    bool returnsActive() const noexcept { return settings.delayOn || settings.reverbOn || running; }
    FxDelay delay;
    FxReverb reverb;
    FxFinish finish;
    Settings settings{};
    std::array<float, 3> sends{};
    double rate = 48000.0, bpm = 120.0;
    float delayInput = 0.0f, reverbInput = 0.0f, mix = 0.25f, reverbMix = 0.2f, space = 0.5f, duck = 0.5f, follower = 0.0f;
    float fadeStep = 0.001f, smoothing = 0.004f, attack = 0.02f, release = 1e-4f;
    bool running = false;
};
}
