#include "dsp/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

// Performance controls (SPEC section 3): Bend Range and polyphonic Glide.
namespace
{
using dropforge::Engine;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
constexpr double rate = 48000.0;
double hz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }
double cents(double a, double b) { return 1200.0 * std::log2(a / b); }
struct Stereo { std::vector<float> left, right; };
Stereo render(Engine& engine, int samples, int block = 512)
{
    Stereo out{std::vector<float>(static_cast<std::size_t>(samples)), std::vector<float>(static_cast<std::size_t>(samples))};
    for (int at = 0; at < samples; at += block)
        engine.process(out.left.data() + at, out.right.data() + at, std::min(block, samples - at));
    return out;
}
float largestStep(const Stereo& audio, std::size_t from, std::size_t to)
{
    float step = 0.0f;
    for (auto i = std::max<std::size_t>(from, 1); i < std::min(to, audio.left.size()); ++i)
        step = std::max({step, std::abs(audio.left[i] - audio.left[i - 1]), std::abs(audio.right[i] - audio.right[i - 1])});
    return step;
}
// The sounding Furnace pitches (with bend), lowest first.
std::vector<double> furnacePitches(const Engine& engine)
{
    std::vector<double> result;
    for (const auto& target : engine.rhythmTargets()) if (target.held) result.push_back(target.frequency);
    std::sort(result.begin(), result.end());
    return result;
}

void bendRange()
{
    for (const int range : {2, 5, 7, 12, 24})
    {
        Engine engine;
        engine.prepare(rate);
        engine.setBendRange(range);
        engine.noteOn(1, 52, 100);
        engine.pitchBend(1, 16383);
        render(engine, 2400);
        require(std::abs(cents(furnacePitches(engine)[0], hz(52)) - 100.0 * range) < 0.01, "full bend up reaches the range");
        engine.pitchBend(1, 0);
        render(engine, 2400);
        require(std::abs(cents(furnacePitches(engine)[0], hz(52)) + 100.0 * range) < 0.01, "full bend down reaches the range");
        engine.pitchBend(1, 8192);
        render(engine, 2400);
        require(std::abs(cents(furnacePitches(engine)[0], hz(52))) < 1.0e-6, "centre restores exact tuning");
    }
    // A wheel held up follows a range change, smoothed, never a jump.
    Engine engine;
    engine.prepare(rate);
    engine.noteOn(1, 52, 100);
    engine.pitchBend(1, 16383);
    render(engine, 2400);
    engine.setBendRange(12);
    render(engine, 2400);
    require(std::abs(cents(furnacePitches(engine)[0], hz(52)) - 1200.0) < 0.01, "a held bend follows a new range");
    engine.setBendRange(3); // not a choice: back to the default 2
    render(engine, 2400);
    require(std::abs(cents(furnacePitches(engine)[0], hz(52)) - 200.0) < 0.01, "an unsupported range falls back to 2");
    std::cout << "bend ranges 2/5/7/12/24 passed\n";
}

// A chord change: note-offs then note-ons at the same instant, as a host sends.
void changeChord(Engine& engine, std::initializer_list<int> from, std::initializer_list<int> to)
{
    for (const auto note : from) engine.noteOff(1, note);
    for (const auto note : to) engine.noteOn(1, note, 110);
}

void furnaceGlide()
{
    const int glideSamples = static_cast<int>(std::lround(rate * 0.2));
    // Off: the new chord sounds at once.
    {
        Engine engine;
        engine.prepare(rate);
        for (const int n : {40, 47, 52}) engine.noteOn(1, n, 110);
        render(engine, 4800);
        changeChord(engine, {40, 47, 52}, {42, 49, 54});
        render(engine, 64);
        const auto pitches = furnacePitches(engine);
        require(pitches.size() == 3 && std::abs(cents(pitches[0], hz(42))) < 1e-6, "Glide off: chords change at once");
    }
    // On: each note slides from the old note nearest it, evenly in pitch.
    Engine engine;
    engine.setGlide(true, 200.0f);
    engine.prepare(rate);
    for (const int n : {40, 47, 52}) engine.noteOn(1, n, 110);
    const auto before = render(engine, 9600);
    changeChord(engine, {40, 47, 52}, {42, 49, 54});
    auto during = render(engine, glideSamples / 2);
    auto pitches = furnacePitches(engine);
    require(pitches.size() == 3, "a gliding chord keeps its three voices (no new attacks)");
    for (std::size_t i = 0; i < 3; ++i)
    {
        const auto travelled = cents(pitches[i], hz(std::array{40, 47, 52}[i]));
        require(std::abs(travelled - 100.0) < 3.0, "halfway through the time, each note is halfway (a semitone of two)");
    }
    const auto rest = render(engine, glideSamples / 2 + 64);
    pitches = furnacePitches(engine);
    for (std::size_t i = 0; i < 3; ++i)
        require(std::abs(cents(pitches[i], hz(std::array{42, 49, 54}[i]))) < 1e-6, "each note lands exactly on the new chord");
    // No click and no dip: the glide is as smooth as the chord itself.
    Stereo whole = before;
    whole.left.insert(whole.left.end(), during.left.begin(), during.left.end());
    whole.right.insert(whole.right.end(), during.right.begin(), during.right.end());
    const auto natural = largestStep(before, 4800, before.left.size());
    const auto step = largestStep(whole, before.left.size() - 200, whole.left.size());
    std::cout << "furnace_glide_step=" << step << " natural=" << natural << '\n';
    require(step <= 1.25f * natural, "a chord glide does not click");
    // A growing chord: the extra note starts fresh, sliding from the nearest old pitch.
    changeChord(engine, {42, 49, 54}, {40, 47, 52, 59});
    render(engine, glideSamples + 64);
    pitches = furnacePitches(engine);
    require(pitches.size() == 4 && std::abs(cents(pitches[3], hz(59))) < 1e-6, "a growing chord adds its new note and lands");
}

void anvilGlide()
{
    const int glideSamples = static_cast<int>(std::lround(rate * 0.1));
    Engine engine;
    engine.setGlide(true, 100.0f);
    engine.prepare(rate);
    engine.noteOn(1, 40, 110);
    render(engine, 4800);
    engine.noteOn(1, 35, 110); // a lower key while 40 is held: Anvil moves, legato
    render(engine, glideSamples / 2);
    const auto midway = cents(engine.bassPitch(), hz(28)); // Anvil sounds an octave down
    require(std::abs(midway + 250.0) < 5.0, "Anvil glides evenly from the old key down to the new one");
    render(engine, glideSamples / 2 + 64);
    require(std::abs(cents(engine.bassPitch(), hz(23))) < 1e-6, "Anvil lands exactly an octave under the new key");
    engine.noteOff(1, 35);
    engine.noteOff(1, 40);
    render(engine, 4800);
    engine.noteOn(1, 45, 110);
    render(engine, 64);
    require(std::abs(cents(engine.bassPitch(), hz(33))) < 1e-6, "after a gap Anvil starts fresh at its own pitch");
}

void determinism()
{
    const auto run = [](int block)
    {
        Engine engine;
        engine.setGlide(true, 150.0f);
        engine.setBendRange(12);
        engine.prepare(rate);
        for (const int n : {40, 47, 52}) engine.noteOn(1, n, 110);
        auto audio = render(engine, 6000, block).left;
        changeChord(engine, {40, 47, 52}, {43, 50, 55});
        engine.pitchBend(1, 12000);
        const auto more = render(engine, 12000, block).left;
        audio.insert(audio.end(), more.begin(), more.end());
        return audio;
    };
    const auto a = run(512);
    require(a == run(1) && a == run(73), "Glide and bend render identically at blocks 1, 73 and 512");
}
}

int main()
{
    bendRange();
    furnaceGlide();
    anvilGlide();
    determinism();
    std::cout << "performance passed\n";
    return 0;
}
