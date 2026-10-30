#include "dsp/Engine.h"
#include <dsp_core/Svf.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

// Performance controls (SPEC section 3): Bend Range, polyphonic Glide, Impact
// and the mod wheel (Roar, Vibrato).
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

// Impact (SPEC section 3). Each layer alone through its tap: an E2 chord in a
// mix (Anvil E1, Furnace E2/B2/E3, Blade E3) held 300 ms, then released.
constexpr int impactHold = 14400, impactTail = 4800;
std::vector<float> impactRun(dropforge::LayerMask mask, bool impact, int velocity, int block = 512)
{
    // Each layer alone through the diagnostic mask; the others still run.
    std::vector<float> layer(impactHold + impactTail), right(layer.size());
    Engine masked;
    masked.setLeadEnabled(true);
    masked.setImpact(impact);
    masked.prepare(rate);
    for (const int n : {40, 47, 52}) masked.noteOn(1, n, velocity);
    for (int at = 0; at < impactHold; at += block)
        masked.process(layer.data() + at, right.data() + at, std::min(block, impactHold - at), mask);
    for (const int n : {40, 47, 52}) masked.noteOff(1, n);
    for (int at = impactHold; at < impactHold + impactTail; at += block)
        masked.process(layer.data() + at, right.data() + at, std::min(block, impactHold + impactTail - at), mask);
    return layer;
}
double rmsDb(const std::vector<float>& audio, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i) sum += static_cast<double>(audio[static_cast<std::size_t>(i)]) * audio[static_cast<std::size_t>(i)];
    return 10.0 * std::log10(std::max(sum / (to - from), 1e-30));
}
// Brightness: the level of the first difference (a gentle treble tilt)
// against the level itself.
double brightnessDb(const std::vector<float>& audio, int from, int to)
{
    std::vector<float> slope(audio.size());
    for (auto i = static_cast<std::size_t>(std::max(from, 1)); i < static_cast<std::size_t>(to); ++i) slope[i] = audio[i] - audio[i - 1];
    return rmsDb(slope, from, to) - rmsDb(audio, from, to);
}
// Energy above against below splitHz (12 dB/oct each side), from 100 ms on.
double tiltDb(const std::vector<float>& audio, double splitHz)
{
    dsp_core::Svf above, below;
    above.set(dsp_core::Svf::Type::highpass, splitHz, rate);
    below.set(dsp_core::Svf::Type::lowpass, splitHz, rate);
    std::vector<float> high(audio.size()), low(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) { high[i] = above.next(audio[i]); low[i] = below.next(audio[i]); }
    const auto end = static_cast<int>(audio.size());
    return rmsDb(high, 4800, end) - rmsDb(low, 4800, end);
}
int firstSound(const std::vector<float>& audio)
{
    for (std::size_t i = 0; i < audio.size(); ++i) if (std::abs(audio[i]) > 1e-6f) return static_cast<int>(i);
    return -1;
}
int lastSound(const std::vector<float>& audio)
{
    for (auto i = audio.size(); i-- > 0;) if (std::abs(audio[i]) > 1e-4f) return static_cast<int>(i);
    return -1;
}

void impactAccent()
{
    const int attack = Engine::latencySamples, window = attack + 960; // the first 20 ms
    const int steadyFrom = 7200, steadyTo = 13920;                     // 150-290 ms
    const struct { dropforge::LayerMask mask; const char* name; } layers[] = {
        {dropforge::LayerMask::bass, "anvil"}, {dropforge::LayerMask::rhythm, "furnace"}, {dropforge::LayerMask::lead, "blade"}};
    for (const auto& layer : layers)
    {
        const auto off = impactRun(layer.mask, false, 127), on = impactRun(layer.mask, true, 127);
        const auto softOff = impactRun(layer.mask, false, 40), softOn = impactRun(layer.mask, true, 40);
        const auto lift = rmsDb(on, attack, window) - rmsDb(off, attack, window);
        const auto softLift = rmsDb(softOn, attack, window) - rmsDb(softOff, attack, window);
        const auto steady = rmsDb(on, steadyFrom, steadyTo) - rmsDb(off, steadyFrom, steadyTo);
        const auto brighter = brightnessDb(on, attack, window) - brightnessDb(off, attack, window);
        std::cout << "impact " << layer.name << " lift_db=" << lift << " soft_lift_db=" << softLift
                  << " steady_db=" << steady << " brighter_db=" << brighter << '\n';
        require(lift > 1.0 && lift < 6.0, "Impact lifts a hard attack by a few dB");
        require(brighter > 0.2, "Impact brightens the attack (a harder drive)");
        require(softLift > 0.0 && softLift < 0.6 * lift, "a soft hit gets a much smaller accent");
        require(std::abs(steady) < 0.1, "the accent is gone well within 150 ms: the held sound is unchanged");
        require(firstSound(on) == firstSound(off), "Impact never moves the attack");
        require(std::abs(lastSound(on) - lastSound(off)) <= 4, "Impact never changes the release");
    }
    // Only a key press accents: legato moves have no attack, and a layer
    // switched on during a held chord is no strike.
    const auto legatoLift = [](dropforge::LayerMask mask, int note)
    {
        std::array<std::vector<float>, 2> runs;
        for (int impact = 0; impact < 2; ++impact)
        {
            Engine engine;
            engine.setLeadEnabled(true);
            engine.setGlide(true, 80.0f);
            engine.setLeadPortamento(Engine::Portamento::legato, 60.0f);
            engine.setImpact(impact == 1);
            engine.prepare(rate);
            engine.noteOn(1, 40, 127);
            engine.noteOn(1, 52, 127);
            std::vector<float> audio(19200), right(19200);
            engine.process(audio.data(), right.data(), 14400, mask);
            engine.noteOn(1, note, 127);
            engine.process(audio.data() + 14400, right.data() + 14400, 4800, mask);
            runs[static_cast<std::size_t>(impact)] = audio;
        }
        return rmsDb(runs[1], 14400, 15600) - rmsDb(runs[0], 14400, 15600);
    };
    const auto anvilLegato = legatoLift(dropforge::LayerMask::bass, 35), bladeLegato = legatoLift(dropforge::LayerMask::lead, 59);
    std::cout << "impact legato anvil_db=" << anvilLegato << " blade_db=" << bladeLegato << '\n';
    require(std::abs(anvilLegato) < 0.3 && std::abs(bladeLegato) < 0.3, "a legato move is no attack: no accent");
    std::array<std::vector<float>, 2> joined;
    for (int impact = 0; impact < 2; ++impact)
    {
        Engine engine;
        engine.setImpact(impact == 1);
        engine.prepare(rate);
        for (const int n : {40, 47, 52}) engine.noteOn(1, n, 127);
        std::vector<float> audio(9600), right(9600);
        engine.process(audio.data(), right.data(), 2400, dropforge::LayerMask::lead);
        engine.setLeadEnabled(true);
        engine.process(audio.data() + 2400, right.data() + 2400, 7200, dropforge::LayerMask::lead);
        joined[static_cast<std::size_t>(impact)] = audio;
    }
    require(firstSound(joined[1]) > 0 && joined[0] == joined[1], "switching Blade on under a held chord is no strike");
    // Block-size invariance with strikes during the run.
    const auto a = impactRun(dropforge::LayerMask::all, true, 110, 512);
    require(a == impactRun(dropforge::LayerMask::all, true, 110, 1) && a == impactRun(dropforge::LayerMask::all, true, 110, 73),
            "Impact renders identically at blocks 1, 73 and 512");
    std::cout << "impact passed\n";
}

// The mod wheel (SPEC section 3). A held E2 chord in a mix, Blade on, each
// layer through its mask; the wheel set before the chord or moved by CC1.
struct WheelRun { std::vector<float> left, right; };
WheelRun wheelRun(dropforge::LayerMask mask, Engine::WheelMode mode, float amount, int samples = 19200,
                  dropforge::BassComponent component = dropforge::BassComponent::all, int block = 512)
{
    Engine engine;
    engine.setLeadEnabled(true);
    engine.setWheelMode(mode);
    engine.setWheelAmount(amount);
    engine.prepare(rate);
    for (const int n : {40, 47, 52}) engine.noteOn(1, n, 110);
    WheelRun out{std::vector<float>(static_cast<std::size_t>(samples)), std::vector<float>(static_cast<std::size_t>(samples))};
    for (int at = 0; at < samples; at += block)
        engine.process(out.left.data() + at, out.right.data() + at, std::min(block, samples - at), mask, {}, component);
    return out;
}

void wheelControl()
{
    // The latest event wins; reset returns to the saved Amount.
    Engine engine;
    engine.prepare(rate);
    engine.setWheelAmount(0.5f);
    engine.modWheel(0);
    require(engine.wheelPosition() == 0.0f, "CC1 moves the wheel away from the saved Amount");
    engine.setWheelAmount(0.5f);
    require(engine.wheelPosition() == 0.0f, "an unchanged host value does not override CC1");
    engine.setWheelAmount(0.7f);
    require(std::abs(engine.wheelPosition() - 0.7f) < 1e-6f, "a host change wins over the last CC1");
    engine.modWheel(127);
    engine.resetWheel();
    require(std::abs(engine.wheelPosition() - 0.7f) < 1e-6f, "a controller reset returns to the saved Amount");
    engine.modWheel(127);
    engine.reset();
    require(std::abs(engine.wheelPosition() - 0.7f) < 1e-6f, "reset returns to the saved Amount");
}

void roar()
{
    // Anvil: every mode's grit, clean and destroyed, rises at full Roar to
    // about 6 dB under the sine, so the growl is heard over the sub whatever
    // the mode's resting grit level.
    for (int destroy = 0; destroy < 2; ++destroy)
        for (int mode = 0; mode < 4; ++mode)
        {
            const auto measure = [&](float amount, dropforge::BassComponent component)
            {
                Engine engine;
                engine.setAnvilMode(mode);
                engine.setAnvilDestruction(destroy == 1);
                engine.setWheelAmount(amount);
                engine.prepare(rate);
                for (const int n : {40, 47, 52}) engine.noteOn(1, n, 110);
                std::vector<float> left(19200), right(19200);
                for (int at = 0; at < 19200; at += 512)
                    engine.process(left.data() + at, right.data() + at, std::min(512, 19200 - at), dropforge::LayerMask::bass, {}, component);
                return rmsDb(left, 4800, 19200);
            };
            const auto sine = measure(0.0f, dropforge::BassComponent::clean);
            const auto rest = measure(0.0f, dropforge::BassComponent::grit) - sine, full = measure(1.0f, dropforge::BassComponent::grit) - sine;
            const auto louder = measure(1.0f, dropforge::BassComponent::all) - measure(0.0f, dropforge::BassComponent::all);
            std::cout << "roar anvil mode=" << "ABCD"[mode] << (destroy == 1 ? " destroyed" : "") << " grit_under_sine_db rest=" << -rest
                      << " full=" << -full << " layer_db=" << louder << '\n';
            require(full > -8.0 && full < -4.0, "full Roar brings Anvil's grit to about 6 dB under its sine");
            require(louder > 0.0 && louder < 3.0, "full Roar lifts Anvil by under 3 dB");
        }
    // Each layer: the formant sweeps up through its range. Measured as the
    // energy above against below the range's middle, which rises as the
    // peak climbs past it; the loudness stays within 3 dB.
    const struct { dropforge::LayerMask mask; const char* name; double low, high; } layers[] = {
        {dropforge::LayerMask::bass, "anvil", dropforge::BassVoice::roarLowHz, dropforge::BassVoice::roarHighHz},
        {dropforge::LayerMask::rhythm, "furnace", dropforge::FurnaceLayer::roarLowHz, dropforge::FurnaceLayer::roarHighHz},
        {dropforge::LayerMask::lead, "blade", dropforge::BladeVoice::roarLowHz, dropforge::BladeVoice::roarHighHz}};
    for (const auto& layer : layers)
    {
        const auto middle = std::sqrt(layer.low * layer.high);
        const auto base = wheelRun(layer.mask, Engine::WheelMode::roar, 0.0f).left;
        const auto half = wheelRun(layer.mask, Engine::WheelMode::roar, 0.5f).left;
        const auto full = wheelRun(layer.mask, Engine::WheelMode::roar, 1.0f).left;
        const auto level = rmsDb(full, 4800, 19200) - rmsDb(base, 4800, 19200);
        const auto halfLevel = rmsDb(half, 4800, 19200) - rmsDb(base, 4800, 19200);
        const auto tilt = tiltDb(full, middle) - tiltDb(base, middle);
        const auto halfTilt = tiltDb(half, middle) - tiltDb(base, middle);
        std::cout << "roar " << layer.name << " level_db half=" << halfLevel << " full=" << level
                  << " tilt_db half=" << halfTilt << " full=" << tilt << '\n';
        require(std::abs(level) < 3.0 && std::abs(halfLevel) < 3.0, "Roar moves the tone, not the loudness (within 3 dB)");
        require(tilt > 3.0 && tilt > halfTilt + 1.0, "the Roar formant sweeps up audibly as the wheel rises");
    }
    // Anvil's sine never changes: only its grit roars.
    require(wheelRun(dropforge::LayerMask::bass, Engine::WheelMode::roar, 1.0f, 9600, dropforge::BassComponent::clean).left
                == wheelRun(dropforge::LayerMask::bass, Engine::WheelMode::roar, 0.0f, 9600, dropforge::BassComponent::clean).left,
            "Roar leaves Anvil's sine untouched");
    // Amount 0 is exactly the base sound, also with Vibrato chosen.
    require(wheelRun(dropforge::LayerMask::all, Engine::WheelMode::vibrato, 0.0f).left
                == wheelRun(dropforge::LayerMask::all, Engine::WheelMode::roar, 0.0f).left,
            "a wheel at 0 is the base sound in either mode");
}

// Moves without clicks: the wheel thrown from 0 to full and back by CC1, and
// a mode change with the wheel up, never step further than the held sound.
void wheelMoves()
{
    const auto run = [](int block)
    {
        Engine engine;
        engine.setLeadEnabled(true);
        engine.prepare(rate);
        for (const int n : {40, 47, 52}) engine.noteOn(1, n, 110);
        std::vector<float> left(43200), right(43200);
        const auto play = [&](int from, int to)
        {
            for (int at = from; at < to; at += block)
                engine.process(left.data() + at, right.data() + at, std::min(block, to - at));
        };
        play(0, 9600);
        engine.modWheel(127);
        play(9600, 19200);
        engine.setWheelMode(Engine::WheelMode::vibrato);
        play(19200, 28800);
        engine.modWheel(0);
        play(28800, 43200);
        return Stereo{left, right};
    };
    const auto audio = run(512);
    const auto steady = std::max(largestStep(audio, 4800, 9600), largestStep(audio, 14400, 19200));
    const auto moves = std::max({largestStep(audio, 9600, 10600), largestStep(audio, 19200, 20600), largestStep(audio, 28800, 29800)});
    std::cout << "wheel moves step=" << moves << " steady=" << steady << '\n';
    require(moves <= 1.25f * steady, "wheel moves and a mode change while up do not click");
    // Back at 0, the sound returns to the base level once the moves settle.
    const auto base = wheelRun(dropforge::LayerMask::all, Engine::WheelMode::roar, 0.0f, 43200).left;
    require(std::abs(rmsDb(audio.left, 38400, 43200) - rmsDb(base, 38400, 43200)) < 0.1, "a wheel back at 0 sounds as before");
    const auto again = run(1), odd = run(73);
    require(audio.left == again.left && audio.left == odd.left, "the wheel renders identically at blocks 1, 73 and 512");
}

// Vibrato: Anvil's sine (exactly an octave under the key, so it shows the
// common pitch ratio) swings +-25 cents at 5 Hz at full Amount.
void vibrato()
{
    const auto sine = wheelRun(dropforge::LayerMask::bass, Engine::WheelMode::vibrato, 1.0f, 96000,
                               dropforge::BassComponent::clean).left;
    std::vector<double> crossings;
    for (std::size_t i = 4800; i < sine.size(); ++i)
        if (sine[i - 1] < 0.0f && sine[i] >= 0.0f)
            crossings.push_back(static_cast<double>(i - 1) + sine[i - 1] / (sine[i - 1] - sine[i]));
    double low = 1e9, high = -1e9;
    int turns = 0;
    double previous = 0.0;
    for (std::size_t i = 1; i < crossings.size(); ++i)
    {
        const auto deviation = cents(rate / (crossings[i] - crossings[i - 1]), hz(28));
        low = std::min(low, deviation); high = std::max(high, deviation);
        if (i > 1 && (deviation > 0.0) != (previous > 0.0)) ++turns;
        previous = deviation;
    }
    std::cout << "vibrato cents low=" << low << " high=" << high << " sign_changes=" << turns << '\n';
    require(high > 20.0 && high < 27.0 && low < -20.0 && low > -27.0, "full Vibrato swings about +-25 cents");
    // 1.9 s at 5 Hz: 9.5 cycles, so about 19 changes of sign.
    require(turns >= 17 && turns <= 21, "Vibrato runs at about 5 Hz");
}
}

int main()
{
    bendRange();
    furnaceGlide();
    anvilGlide();
    determinism();
    impactAccent();
    wheelControl();
    roar();
    wheelMoves();
    vibrato();
    std::cout << "performance passed\n";
    return 0;
}
