#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace dropforge
{
// A bounded press-order queue supports Bass's held-key lock and Both's lowest
// root, independently of rhythm steals. Equal lowest pitches use press order.
class BassNotes
{
public:
    enum class Policy { locked, lowest, newest };
    enum class Change { none, attack, release, retarget };
    struct Target
    {
        int channel = 0, note = -1, velocity = 0;
        double frequency = 0.0;
    };
    void reset() noexcept { count = 0; selected = 0; }
    Change setPolicy(Policy next) noexcept
    {
        const auto previous = target();
        policy = next;
        return reselect(previous);
    }
    template <typename Visitor>
    void forEachNewestHeldKey(std::size_t limit, Visitor&& visit) const noexcept
    {
        // Re-entry only needs the notes that fit in the rhythm bank. Avoid
        // replaying thousands of stolen keys on the audio thread.
        for (std::size_t i = count > limit ? count - limit : 0; i < count; ++i)
            visit(held[i].channel, held[i].note, held[i].velocity);
    }
    Target target() const noexcept
    {
        if (count == 0) return {};
        const auto& key = held[selected];
        // Compute below MIDI zero directly: note 0 still has a valid bass pitch.
        return {key.channel, key.note, key.velocity,
                440.0 * std::exp2((key.note - 81.0) / 12.0)};
    }
    Change noteOn(int channel, int note, int velocity) noexcept
    {
        if (!valid(channel, note)) return Change::none;
        if (velocity <= 0) return noteOff(channel, note);
        const auto previous = target();
        for (std::size_t i = 0; i < count; ++i)
            if (held[i].channel == channel && held[i].note == note)
            {
                if (policy == Policy::newest)
                {
                    // A repeated press becomes newest without growing the ledger.
                    erase(i);
                    held[count++] = {channel, note, std::min(velocity, 127)};
                    const auto change = reselect(previous);
                    return change == Change::none ? Change::attack : change;
                }
                held[i].velocity = std::min(velocity, 127);
                // Repeated selected note-ons articulate again; no stacked key counts.
                return i == selected ? Change::attack : Change::none;
            }
        held[count++] = {channel, note, std::min(velocity, 127)};
        return reselect(previous);
    }
    Change noteOff(int channel, int note) noexcept
    {
        if (!valid(channel, note)) return Change::none;
        const auto previous = target();
        for (std::size_t i = 0; i < count; ++i)
            if (held[i].channel == channel && held[i].note == note)
            {
                erase(i);
                return reselect(previous);
            }
        return Change::none;
    }
    Change allNotesOff(int channel) noexcept
    {
        const auto previous = target();
        // Remove the entire channel before selecting a replacement, so panic
        // never starts a transient attack on another key being cleared too.
        for (std::size_t i = 0; i < count;)
            if (held[i].channel == channel) erase(i); else ++i;
        return reselect(previous);
    }

private:
    Change reselect(const Target& previous) noexcept
    {
        selected = policy == Policy::newest && count > 0 ? count - 1 : 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (policy == Policy::lowest)
            {
                if (held[i].note < held[selected].note) selected = i;
            }
            else if (policy == Policy::locked && held[i].channel == previous.channel && held[i].note == previous.note)
            {
                selected = i;
                break;
            }
        }
        const auto next = target();
        if (next.note == -1) return previous.note == -1 ? Change::none : Change::release;
        if (next.note != previous.note) return Change::attack;
        // Same pitch on another channel changes its bend/velocity owner, but
        // must not add an attack when the lowest held pitch has not changed.
        return next.channel != previous.channel
            ? (policy == Policy::lowest ? Change::retarget : Change::attack) : Change::none;
    }
    struct Key { int channel = 0, note = 0, velocity = 0; };
    static bool valid(int channel, int note) noexcept
    {
        return channel >= 1 && channel <= 16 && note >= 0 && note <= 127;
    }
    void erase(std::size_t index) noexcept
    {
        for (auto i = index + 1; i < count; ++i) held[i - 1] = held[i];
        --count;
    }
    // At most one entry per MIDI channel/key; insertion cannot exceed capacity.
    std::array<Key, 16 * 128> held{};
    std::size_t count = 0;
    std::size_t selected = 0;
    Policy policy = Policy::locked;
};
}
