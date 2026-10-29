#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dropforge
{
// The auto-split (SPEC section 3): one press-order list of held keys decides
// which note each layer plays. Selection uses MIDI note numbers, never bent
// frequencies, so hosts that label octaves differently all route alike.
//   Anvil: the newest held key, or the lowest while Furnace is on.
//   Blade: the highest held key, whatever else is on.
//   Furnace: every held key, through pickVoice() into a fixed voice bank.
// Equal pitches on two channels are two keys; the earlier press keeps the
// lowest/highest selection until it is released, then the other takes over
// without a new attack (only its bend and velocity owner changes).
class NoteRouter
{
public:
    enum class BassRule { newest, lowest };
    enum class Change { none, attack, release, retarget };
    struct Changes { Change bass = Change::none, lead = Change::none; };
    struct Key
    {
        int channel = 0, note = -1, velocity = 0;
        bool held() const noexcept { return note >= 0; }
    };

    void reset() noexcept { count = 0; bassIndex = leadIndex = 0; }
    // Returns Anvil's change when switching Furnace on or off moves its note.
    Change setBassRule(BassRule rule) noexcept
    {
        const auto previous = bassKey();
        bassRule = rule;
        return reselect(previous, leadKey()).bass;
    }

    Changes noteOn(int channel, int note, int velocity) noexcept
    {
        if (!valid(channel, note)) return {};
        if (velocity <= 0) return noteOff(channel, note);
        const auto previousBass = bassKey(), previousLead = leadKey();
        const Key key{channel, note, std::min(velocity, 127)};
        // A repeated press becomes the newest press without stacking.
        for (std::size_t i = 0; i < count; ++i)
            if (held[i].channel == channel && held[i].note == note)
            {
                // Under the pitch rules a repeat keeps its place among equal
                // pitches; only its velocity changes. Newest moves it last.
                if (bassRule == BassRule::newest) { erase(i); held[count++] = key; }
                else held[i].velocity = key.velocity;
                auto changes = reselect(previousBass, previousLead);
                // A repeated press of a selected key articulates it again.
                if (same(bassKey(), key)) changes.bass = Change::attack;
                if (same(leadKey(), key)) changes.lead = Change::attack;
                return changes;
            }
        held[count++] = key;
        return reselect(previousBass, previousLead);
    }
    Changes noteOff(int channel, int note) noexcept
    {
        if (!valid(channel, note)) return {};
        const auto previousBass = bassKey(), previousLead = leadKey();
        for (std::size_t i = 0; i < count; ++i)
            if (held[i].channel == channel && held[i].note == note)
            {
                erase(i);
                return reselect(previousBass, previousLead);
            }
        return {};
    }
    Changes allNotesOff(int channel) noexcept
    {
        const auto previousBass = bassKey(), previousLead = leadKey();
        // Remove the whole channel before selecting a replacement, so panic
        // never starts an attack on another key being cleared too.
        for (std::size_t i = 0; i < count;)
            if (held[i].channel == channel) erase(i); else ++i;
        return reselect(previousBass, previousLead);
    }

    Key bassKey() const noexcept { return count ? held[bassIndex] : Key{}; }
    Key leadKey() const noexcept { return count ? held[leadIndex] : Key{}; }
    std::size_t heldCount() const noexcept { return count; }

    // Re-entry when Furnace switches on mid-chord: the newest keys that fit
    // its voice bank, oldest first. Never replays thousands of stolen keys.
    template <typename Visitor>
    void forEachNewestHeldKey(std::size_t limit, Visitor&& visit) const noexcept
    {
        for (std::size_t i = count > limit ? count - limit : 0; i < count; ++i)
            visit(held[i].channel, held[i].note, held[i].velocity);
    }

    // The Furnace voice for a new press: the same key's own sounding voice,
    // else a silent voice, else a released one, else the oldest held one.
    // Voice needs active(), keyDown, channel, note and an increasing age.
    template <typename Voices>
    static std::size_t pickVoice(const Voices& voices, int channel, int note) noexcept
    {
        for (std::size_t i = 0; i < voices.size(); ++i)
            if (voices[i].active() && voices[i].channel == channel && voices[i].note == note) return i;
        for (std::size_t i = 0; i < voices.size(); ++i)
            if (!voices[i].active()) return i;
        std::size_t chosen = 0;
        for (std::size_t i = 1; i < voices.size(); ++i)
        {
            const auto& a = voices[i];
            const auto& b = voices[chosen];
            if (a.keyDown != b.keyDown ? !a.keyDown : a.age < b.age) chosen = i;
        }
        return chosen;
    }

    static double frequency(int note) noexcept { return 440.0 * std::exp2((note - 69.0) / 12.0); }
    // Anvil sounds exactly an octave down, computed directly so MIDI 0 still
    // gets a valid 4.09 Hz pitch instead of a clamp or wrap.
    static double bassFrequency(int note) noexcept { return 440.0 * std::exp2((note - 81.0) / 12.0); }

private:
    static bool valid(int channel, int note) noexcept
    {
        return channel >= 1 && channel <= 16 && note >= 0 && note <= 127;
    }
    static bool same(const Key& a, const Key& b) noexcept { return a.channel == b.channel && a.note == b.note; }
    void erase(std::size_t index) noexcept
    {
        for (auto i = index + 1; i < count; ++i) held[i - 1] = held[i];
        --count;
    }
    static Change classify(const Key& previous, const Key& next, bool newOwnerAttacks) noexcept
    {
        if (!next.held()) return previous.held() ? Change::release : Change::none;
        if (next.note != previous.note) return Change::attack;
        if (next.channel == previous.channel) return Change::none;
        return newOwnerAttacks ? Change::attack : Change::retarget;
    }
    Changes reselect(const Key& previousBass, const Key& previousLead) noexcept
    {
        bassIndex = bassRule == BassRule::newest && count > 0 ? count - 1 : 0;
        leadIndex = 0;
        for (std::size_t i = 1; i < count; ++i)
        {
            if (bassRule == BassRule::lowest && held[i].note < held[bassIndex].note) bassIndex = i;
            if (held[i].note > held[leadIndex].note) leadIndex = i;
        }
        // Under the newest rule a same-pitch handover is a different key
        // taking over, so it attacks; pitch rules only move bend ownership.
        return {classify(previousBass, bassKey(), bassRule == BassRule::newest),
                classify(previousLead, leadKey(), false)};
    }

    // At most one entry per MIDI channel and key, so this can never overflow.
    std::array<Key, 16 * 128> held{};
    std::size_t count = 0, bassIndex = 0, leadIndex = 0;
    BassRule bassRule = BassRule::lowest;
};
}
