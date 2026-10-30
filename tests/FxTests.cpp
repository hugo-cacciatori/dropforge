#include "dsp/FxChain.h"
#include "plugin/InstrumentCore.h"
#include <dsp_core/Svf.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

// The FX page's chain (SPEC section 8): sends, the ducked Delay and Reverb,
// Space, the finishing effects, tempo, tails.
namespace
{
using dropforge::FxChain;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
constexpr double rate = 48000.0;
struct Stereo { std::vector<float> left, right; };
// What the instruments play: Furnace's tap carries furnace, the dry mix dry.
struct Input { std::vector<float> furnace, dry; };

// Runs the chain over the input in blocks; returns dry plus returns.
Stereo run(FxChain& chain, const Input& input, int block = 512)
{
    const auto size = input.furnace.size();
    Stereo out{input.dry, input.dry};
    if (out.left.empty()) out = {std::vector<float>(size), std::vector<float>(size)};
    std::vector<float> zero(size), furnace = input.furnace;
    for (std::size_t at = 0; at < size; at += static_cast<std::size_t>(block))
    {
        const auto count = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), size - at));
        dropforge::LayerTaps taps{zero.data() + at, zero.data() + at, furnace.data() + at, furnace.data() + at, zero.data() + at};
        chain.process(out.left.data() + at, out.right.data() + at, taps, dropforge::LayerMask::all, count);
    }
    return out;
}
FxChain::Settings echoOnly()
{
    FxChain::Settings s;
    s.delayOn = true;
    s.delayFeedback = 0.0f;
    s.delayMix = 1.0f;
    s.duck = 0.0f;
    s.delayPingPong = false;
    s.delayLowCutHz = 20.0f;
    s.delayHighCutHz = 20000.0f;
    return s;
}
std::size_t loudest(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    std::size_t best = from;
    for (auto i = from; i < std::min(to, audio.size()); ++i) if (std::abs(audio[i]) > std::abs(audio[best])) best = i;
    return best;
}
double rmsDb(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (auto i = from; i < to; ++i) sum += static_cast<double>(audio[i]) * audio[i];
    return 10.0 * std::log10(std::max(sum / static_cast<double>(to - from), 1e-30));
}
float largestStep(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    float step = 0.0f;
    for (auto i = std::max<std::size_t>(from, 1); i < to; ++i) step = std::max(step, std::abs(audio[i] - audio[i - 1]));
    return step;
}

// Energy above against below splitHz, from 200 ms on.
double tiltDb(const std::vector<float>& audio, double splitHz)
{
    dsp_core::Svf above, below;
    above.set(dsp_core::Svf::Type::highpass, splitHz, rate);
    below.set(dsp_core::Svf::Type::lowpass, splitHz, rate);
    std::vector<float> high(audio.size()), low(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) { high[i] = above.next(audio[i]); low[i] = below.next(audio[i]); }
    return rmsDb(high, 9600, audio.size()) - rmsDb(low, 9600, audio.size());
}
std::vector<float> sine(double hz, std::size_t size, float level = 0.3f)
{
    std::vector<float> out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = level * static_cast<float>(std::sin(6.283185307179586 * hz * static_cast<double>(i) / rate));
    return out;
}
std::vector<float> noise(std::size_t size, float level = 0.3f)
{
    std::vector<float> out(size);
    std::uint32_t state = 12345u;
    for (auto& v : out) { state = state * 1664525u + 1013904223u; v = level * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f); }
    return out;
}

void timing()
{
    struct Case { int division; FxChain::Feel feel; bool sync; float ms; double bpm, seconds; const char* what; };
    const Case cases[] = {
        {3, FxChain::Feel::dotted, true, 0.0f, 120.0, 0.375, "1/8 dotted at 120 BPM"},
        {4, FxChain::Feel::triplet, true, 0.0f, 120.0, 1.0 / 3.0, "1/4 triplet at 120 BPM"},
        {2, FxChain::Feel::straight, true, 0.0f, 150.0, 0.1, "1/16 at 150 BPM"},
        {0, FxChain::Feel::straight, true, 0.0f, 120.0, 0.03125, "1/64 at 120 BPM"},
        {3, FxChain::Feel::dotted, true, 0.0f, std::nan(""), 0.375, "no host tempo plays at 120 BPM"},
        {3, FxChain::Feel::dotted, true, 0.0f, 0.0, 0.375, "a zero tempo plays at 120 BPM"},
        {7, FxChain::Feel::straight, true, 0.0f, 60.0, 6.0, "2 bars at 60 BPM clamp to 6 s"},
        {3, FxChain::Feel::dotted, false, 250.0f, 120.0, 0.25, "free time ignores the tempo"},
    };
    for (const auto& c : cases)
    {
        auto s = echoOnly();
        s.delayDivision = c.division; s.delayFeel = c.feel; s.delaySync = c.sync; s.delayMs = c.ms;
        require(std::abs(FxChain::delaySeconds(s, c.bpm) - c.seconds) < 1e-9, c.what);
        FxChain chain;
        chain.set(s, c.bpm);
        chain.prepare(rate);
        const auto expected = static_cast<std::size_t>(std::lround(c.seconds * rate));
        Input input{std::vector<float>(expected + 4800), {}};
        input.furnace[0] = 1.0f;
        const auto out = run(chain, input);
        const auto peak = loudest(out.left, 1, out.left.size());
        require(peak + 4 >= expected && peak <= expected + 4, c.what);
    }
    std::cout << "sync divisions, feels, fallback tempo, clamp and free time passed\n";
}

void pingPong()
{
    auto s = echoOnly();
    s.delayPingPong = true;
    s.delayFeedback = 0.6f;
    FxChain chain;
    chain.set(s, 120.0);
    chain.prepare(rate);
    const auto t = static_cast<std::size_t>(std::lround(0.375 * rate));
    Input input{std::vector<float>(3 * t + 2400), {}};
    input.furnace[0] = 1.0f;
    const auto out = run(chain, input);
    const auto window = [&](const std::vector<float>& side, std::size_t at) { return rmsDb(side, at - 200, at + 400); };
    require(window(out.left, t) - window(out.right, t) > 40.0, "ping-pong: the first repeat on the left");
    require(window(out.right, 2 * t) - window(out.left, 2 * t) > 40.0, "ping-pong: the second repeat crosses to the right");
}

void feedbackAndFilters()
{
    // 95 % feedback: bounded and decaying, never running away.
    auto s = echoOnly();
    s.delayFeedback = 0.95f;
    s.delayLowCutHz = 280.0f; s.delayHighCutHz = 6500.0f;
    FxChain chain;
    chain.set(s, 120.0);
    chain.prepare(rate);
    Input input{noise(static_cast<std::size_t>(rate * 40.0)), {}};
    std::fill(input.furnace.begin() + static_cast<std::ptrdiff_t>(rate), input.furnace.end(), 0.0f);
    const auto out = run(chain, input);
    float peak = 0.0f;
    for (const auto v : out.left) { require(std::isfinite(v), "finite at maximum feedback"); peak = std::max(peak, std::abs(v)); }
    const auto early = rmsDb(out.left, static_cast<std::size_t>(rate * 1.0), static_cast<std::size_t>(rate * 3.0));
    const auto late = rmsDb(out.left, static_cast<std::size_t>(rate * 37.0), static_cast<std::size_t>(rate * 39.0));
    std::cout << "feedback 95% peak=" << peak << " early_db=" << early << " late_db=" << late << '\n';
    require(peak < 2.0f && late < early - 6.0, "maximum feedback stays bounded and decays");
    // The filters: lows and highs echo far quieter than the mids.
    const auto echoLevel = [](double hz)
    {
        auto f = echoOnly();
        f.delayLowCutHz = 280.0f; f.delayHighCutHz = 6500.0f;
        FxChain c;
        c.set(f, 120.0);
        c.prepare(rate);
        const auto out = run(c, {sine(hz, 48000), {}});
        return rmsDb(out.left, 24000, 48000);
    };
    const auto low = echoLevel(50.0), mid = echoLevel(1000.0), high = echoLevel(15000.0);
    std::cout << "echo levels db 50Hz=" << low << " 1kHz=" << mid << " 15kHz=" << high << '\n';
    require(mid - low > 20.0 && mid - high > 12.0, "the low and high cuts keep echoes out of the bass and the fizz");
}

void ducking()
{
    // A loud dry part with Furnace sending: full Ducking pulls the echoes far
    // down while it plays; after it stops they bloom back.
    const auto size = static_cast<std::size_t>(rate * 3.0);
    Input input{noise(size), std::vector<float>(size)};
    for (std::size_t i = 0; i < static_cast<std::size_t>(rate); ++i) input.dry[i] = 0.5f;
    const auto wet = [&](float duck)
    {
        auto s = echoOnly();
        s.duck = duck;
        FxChain chain;
        chain.set(s, 120.0);
        chain.prepare(rate);
        auto out = run(chain, input);
        for (std::size_t i = 0; i < size; ++i) out.left[i] -= input.dry[i];
        return out.left;
    };
    const auto open = wet(0.0f), ducked = wet(1.0f), half = wet(0.5f);
    const auto during = rmsDb(open, 24000, 47000) - rmsDb(ducked, 24000, 47000);
    const auto halfDuring = rmsDb(open, 24000, 47000) - rmsDb(half, 24000, 47000);
    const auto after = rmsDb(open, 120000, 144000) - rmsDb(ducked, 120000, 144000);
    std::cout << "ducking db full=" << during << " half=" << halfDuring << " after_1.5s=" << after << '\n';
    require(during > 15.0 && halfDuring > 3.0 && halfDuring < during, "Ducking pulls the echoes under a loud dry part");
    require(after < 0.5, "the echoes bloom back once the dry part stops");
}

void retimeAndTails()
{
    // A held tone echoing; the division changes while it sounds.
    auto s = echoOnly();
    s.delayFeedback = 0.4f;
    FxChain chain;
    chain.set(s, 120.0);
    chain.prepare(rate);
    const auto size = static_cast<std::size_t>(rate * 4.0);
    Input input{sine(220.0, size), {}};
    std::vector<float> zero(size);
    Stereo out{std::vector<float>(size), std::vector<float>(size)};
    const auto play = [&](std::size_t from, std::size_t to)
    {
        for (auto at = from; at < to; at += 512)
        {
            const auto count = static_cast<int>(std::min<std::size_t>(512, to - at));
            dropforge::LayerTaps taps{zero.data() + at, zero.data() + at, input.furnace.data() + at, input.furnace.data() + at, zero.data() + at};
            chain.process(out.left.data() + at, out.right.data() + at, taps, dropforge::LayerMask::all, count);
        }
    };
    const auto switchAt = static_cast<std::size_t>(rate * 2.0);
    play(0, switchAt);
    s.delayDivision = 4; s.delayFeel = FxChain::Feel::triplet;
    chain.set(s, 120.0);
    play(switchAt, size);
    const auto steady = largestStep(out.left, switchAt - 24000, switchAt);
    const auto change = largestStep(out.left, switchAt, switchAt + 4800);
    std::cout << "retime step=" << change << " steady=" << steady << '\n';
    require(change <= 1.25f * steady, "a time change crossfades without a click");

    // Off: the input fades but the tail drains; then the chain goes idle.
    s.delayOn = false;
    chain.set(s, 120.0);
    std::vector<float> l(48000), r(48000), z(48000);
    dropforge::LayerTaps taps{z.data(), z.data(), input.furnace.data(), input.furnace.data(), z.data()};
    chain.process(l.data(), r.data(), taps, dropforge::LayerMask::all, 48000);
    require(rmsDb(l, 12000, 24000) > -60.0, "switching off lets the tail ring on");
    for (int i = 0; i < 200 && chain.active(); ++i)
        chain.process(l.data(), r.data(), taps, dropforge::LayerMask::all, 48000);
    require(!chain.active(), "a drained tail returns the chain to bypass");
}


FxChain::Settings roomOnly(float decay)
{
    FxChain::Settings s;
    s.reverbOn = true;
    s.reverbMix = 1.0f;
    s.duck = 0.0f;
    s.reverb.decaySeconds = decay;
    s.reverb.preDelayMs = 0.0f;
    s.reverb.damping = 0.0f;
    s.reverb.lowCutHz = 20.0f;
    s.reverb.highCutHz = 20000.0f;
    return s;
}
// RT60 from an impulse: the time the 10 ms level takes to fall from 10 dB to
// 40 dB under its loudest, doubled.
double decayTime(const std::vector<float>& audio)
{
    std::vector<double> levels;
    for (std::size_t at = 0; at + 480 <= audio.size(); at += 480) levels.push_back(rmsDb(audio, at, at + 480));
    const auto top = *std::max_element(levels.begin(), levels.end());
    const auto peakAt = static_cast<std::size_t>(std::max_element(levels.begin(), levels.end()) - levels.begin());
    double first = -1.0, second = -1.0;
    for (auto i = peakAt; i < levels.size(); ++i)
    {
        if (first < 0.0 && levels[i] < top - 10.0) first = static_cast<double>(i);
        if (second < 0.0 && levels[i] < top - 40.0) { second = static_cast<double>(i); break; }
    }
    return second > 0.0 && first > 0.0 ? 2.0 * (second - first) * 0.01 : -1.0;
}

void reverbRoom()
{
    for (const float decay : {0.8f, 3.0f})
    {
        FxChain chain;
        chain.set(roomOnly(decay), 120.0);
        chain.prepare(rate);
        Input input{std::vector<float>(static_cast<std::size_t>(rate * (decay * 1.5 + 0.5))), {}};
        input.furnace[0] = 1.0f;
        const auto measured = decayTime(run(chain, input).left);
        std::cout << "reverb decay set=" << decay << " measured=" << measured << '\n';
        require(measured > 0.7 * decay && measured < 1.3 * decay, "the tail falls 60 dB in about the Decay time");
    }
    // Pre-delay: silence first, then the room.
    {
        auto s = roomOnly(0.8f);
        s.reverb.preDelayMs = 100.0f;
        FxChain chain;
        chain.set(s, 120.0);
        chain.prepare(rate);
        Input input{std::vector<float>(24000), {}};
        input.furnace[0] = 1.0f;
        const auto out = run(chain, input);
        float early = 0.0f;
        for (std::size_t i = 0; i < 4560; ++i) early = std::max(early, std::abs(out.left[i]));
        require(early < 1e-6f && rmsDb(out.left, 4800, 9600) > -60.0, "the room starts after its pre-delay");
    }
    // The longest, brightest, most modulated room stays bounded and finite.
    {
        auto s = roomOnly(15.0f);
        s.reverb.size = 1.0f; s.reverb.modulation = 1.0f; s.reverb.diffusion = 1.0f;
        FxChain chain;
        chain.set(s, 120.0);
        chain.prepare(rate);
        Input input{noise(static_cast<std::size_t>(rate * 20.0)), {}};
        std::fill(input.furnace.begin() + static_cast<std::ptrdiff_t>(rate), input.furnace.end(), 0.0f);
        const auto out = run(chain, input);
        float peak = 0.0f;
        for (const auto v : out.left) { require(std::isfinite(v), "finite at 15 s decay"); peak = std::max(peak, std::abs(v)); }
        std::cout << "reverb 15 s peak=" << peak << '\n';
        require(peak < 4.0f, "a 15 s room stays bounded");
    }
    // Width: 0 folds the room to the centre; 100 % keeps two different sides.
    const auto sides = [](float width)
    {
        auto s = roomOnly(1.5f);
        s.reverb.width = width;
        FxChain chain;
        chain.set(s, 120.0);
        chain.prepare(rate);
        Input input{noise(9600), {}};
        input.furnace.resize(48000);
        return run(chain, input);
    };
    const auto mono = sides(0.0f), wide = sides(1.0f);
    require(mono.left == mono.right, "Width 0 is exactly centred");
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (std::size_t i = 12000; i < 48000; ++i) { lr += wide.left[i] * wide.right[i]; ll += wide.left[i] * wide.left[i]; rr += wide.right[i] * wide.right[i]; }
    std::cout << "reverb correlation=" << lr / std::sqrt(ll * rr) << '\n';
    require(std::abs(lr / std::sqrt(ll * rr)) < 0.5, "full Width gives two decorrelated sides");
    std::cout << "reverb decay, pre-delay, stability and width passed\n";
}

// Space scales both wets; the reverb never hears the delay.
void spaceAndRouting()
{
    const auto size = static_cast<std::size_t>(rate * 2.0);
    Input input{noise(size), sine(110.0, size, 0.2f)};
    for (std::size_t i = size / 2; i < size; ++i) input.furnace[i] = 0.0f;
    const auto wet = [&](bool delayOn, bool reverbOn, float space)
    {
        FxChain::Settings s;
        s.delayOn = delayOn; s.reverbOn = reverbOn; s.space = space; s.duck = 0.0f;
        FxChain chain;
        chain.set(s, 120.0);
        chain.prepare(rate);
        auto out = run(chain, input);
        for (std::size_t i = 0; i < size; ++i) out.left[i] -= input.dry[i];
        return out.left;
    };
    FxChain::Settings dryOnly;
    dryOnly.delayOn = dryOnly.reverbOn = true; dryOnly.space = 0.0f;
    FxChain silentChain;
    silentChain.set(dryOnly, 120.0);
    silentChain.prepare(rate);
    const auto none = run(silentChain, input);
    require(none.left == input.dry && none.right == input.dry, "Space 0 leaves only the dry sound");
    const auto half = wet(true, true, 0.5f), full = wet(true, true, 1.0f);
    const auto lift = rmsDb(full, 0, size) - rmsDb(half, 0, size);
    std::cout << "space full vs half db=" << lift << '\n';
    require(std::abs(lift - 6.02) < 0.1, "Space at 100 % doubles both wets");
    const auto both = wet(true, true, 0.5f), delayOnly = wet(true, false, 0.5f), roomWet = wet(false, true, 0.5f);
    double worst = 0.0;
    for (std::size_t i = 0; i < size; ++i) worst = std::max(worst, static_cast<double>(std::abs(both[i] - delayOnly[i] - roomWet[i])));
    require(worst < 1e-5, "the reverb and the delay run side by side: the reverb never hears the echoes");
}

// The finishing effects through the chain alone: dry in, finished out.
Stereo finished(const FxChain::Settings& s, const std::vector<float>& left, const std::vector<float>& right, int block = 512)
{
    FxChain chain;
    chain.set(s, 120.0);
    chain.prepare(rate);
    Stereo out{left, right};
    std::vector<float> zero(left.size());
    for (std::size_t at = 0; at < left.size(); at += static_cast<std::size_t>(block))
    {
        const auto count = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), left.size() - at));
        dropforge::LayerTaps taps{zero.data() + at, zero.data() + at, zero.data() + at, zero.data() + at, zero.data() + at};
        chain.process(out.left.data() + at, out.right.data() + at, taps, dropforge::LayerMask::all, count);
    }
    return out;
}
double sideDb(const Stereo& audio, std::size_t from, std::size_t to)
{
    std::vector<float> side(audio.left.size()), mid(audio.left.size());
    for (std::size_t i = 0; i < side.size(); ++i) { side[i] = 0.5f * (audio.left[i] - audio.right[i]); mid[i] = 0.5f * (audio.left[i] + audio.right[i]); }
    return rmsDb(side, from, to) - rmsDb(mid, from, to);
}

void crushChorusEq()
{
    const auto size = static_cast<std::size_t>(rate);
    // Crush: a 997 Hz tone held at 3 kHz folds a strong alias tone back.
    {
        FxChain::Settings s;
        s.finish.crushOn = true; s.finish.crushBits = 16.0f; s.finish.crushRateHz = 3000.0f; s.finish.crushToneHz = 20000.0f; s.finish.crushMix = 1.0f;
        const auto tone = sine(997.0, size);
        const auto out = finished(s, tone, tone);
        std::vector<float> residual(size);
        for (std::size_t i = 0; i < size; ++i) residual[i] = out.left[i] - tone[i];
        const auto grit = rmsDb(residual, 4800, size) - rmsDb(tone, 4800, size);
        s.finish.crushRateHz = 24000.0f; s.finish.crushBits = 3.0f;
        const auto coarse = finished(s, tone, tone);
        for (std::size_t i = 0; i < size; ++i) residual[i] = coarse.left[i] - tone[i];
        const auto steps = rmsDb(residual, 4800, size) - rmsDb(tone, 4800, size);
        std::cout << "crush grit_db rate=" << grit << " bits=" << steps << '\n';
        require(grit > -15.0 && steps > -25.0, "Crush's rate and bits both add audible grit");
        for (const auto v : out.left) require(std::isfinite(v), "Crush stays finite");
    }
    // Chorus: wide above 150 Hz, the low end stays centred.
    {
        FxChain::Settings s;
        s.finish.chorusOn = true; s.finish.chorusMix = 1.0f; s.finish.chorusDepth = 1.0f;
        const auto low = sine(70.0, size), high = sine(2000.0, size);
        const auto lowOut = finished(s, low, low), highOut = finished(s, high, high);
        const auto lowSide = sideDb(lowOut, 9600, size), highSide = sideDb(highOut, 9600, size);
        std::cout << "chorus side_db 70Hz=" << lowSide << " 2kHz=" << highSide << '\n';
        require(lowSide < -20.0 && highSide > -15.0, "Chorus widens the mids and highs and keeps the lows mono");
    }
    // EQ: the tilt turns lows down and highs up by half its amount each; the
    // cuts remove what is outside them.
    {
        const auto level = [&](double hz, FxChain::Settings s)
        {
            const auto tone = sine(hz, size);
            const auto out = finished(s, tone, tone);
            return rmsDb(out.left, 9600, size) - rmsDb(tone, 9600, size);
        };
        FxChain::Settings tilt;
        tilt.finish.eqOn = true; tilt.finish.eqTiltDb = 6.0f;
        const auto lows = level(60.0, tilt), highs = level(12000.0, tilt);
        FxChain::Settings cuts;
        cuts.finish.eqOn = true; cuts.finish.eqLowCutHz = 300.0f; cuts.finish.eqHighCutHz = 2000.0f;
        const auto rumble = level(40.0, cuts), fizz = level(15000.0, cuts);
        std::cout << "eq tilt+6 60Hz=" << lows << " 12kHz=" << highs << " cuts 40Hz=" << rumble << " 15kHz=" << fizz << '\n';
        require(std::abs(lows + 3.0) < 0.6 && std::abs(highs - 3.0) < 0.6, "a +6 dB tilt is -3 dB in the lows, +3 dB in the highs");
        require(rumble < -25.0 && fizz < -25.0, "the EQ's low and high cuts remove rumble and fizz");
    }
    // Switching an effect on and off fades: no click, and back to bypass.
    {
        FxChain::Settings s;
        const auto tone = sine(220.0, size);
        FxChain chain;
        chain.set(s, 120.0);
        chain.prepare(rate);
        Stereo out{tone, tone};
        std::vector<float> zero(size);
        const auto play = [&](std::size_t from, std::size_t to)
        {
            dropforge::LayerTaps taps{zero.data(), zero.data(), zero.data(), zero.data(), zero.data()};
            for (auto at = from; at < to; at += 512)
                chain.process(out.left.data() + at, out.right.data() + at, taps, dropforge::LayerMask::all, static_cast<int>(std::min<std::size_t>(512, to - at)));
        };
        play(0, 12000);
        s.finish.eqOn = true; s.finish.eqTiltDb = -6.0f; s.finish.eqHighCutHz = 3000.0f;
        chain.set(s, 120.0);
        play(12000, 30000);
        s.finish.eqOn = false;
        chain.set(s, 120.0);
        play(30000, size);
        // Against the louder of the plain and the equalised tone (the tilt
        // lifts 220 Hz by about 2 dB).
        const auto steady = std::max(largestStep(out.left, 2000, 12000), largestStep(out.left, 18000, 30000));
        const auto switching = std::max(largestStep(out.left, 12000, 15000), largestStep(out.left, 30000, 33000));
        std::cout << "eq switch step=" << switching << " steady=" << steady << '\n';
        require(switching <= 1.1f * steady, "switching an effect fades without a click");
        require(!chain.active() && out.left[size - 1] == tone[size - 1], "off and faded, the chain is bypassed again");
    }
}

// Saturate through the whole instrument: thicker, about as loud, never over
// the ceiling, and fading in and out without a click.
void saturate()
{
    const auto play = [](bool on, float drive, int toggleAt = -1)
    {
        dropforge::InstrumentCore core;
        dropforge::FxChain::Settings fx;
        fx.saturateOn = on; fx.saturateDriveDb = drive;
        core.setFx(fx, 120.0);
        core.prepare(rate);
        const std::uint8_t on1[][3]{{0x90, 40, 120}, {0x90, 47, 120}, {0x90, 52, 120}, {0x90, 55, 120}};
        for (const auto& message : on1) core.midi(message, 3);
        std::vector<float> left(72000), right(72000);
        for (int at = 0; at < 72000; at += 480)
        {
            if (at == toggleAt) { fx.saturateOn = !fx.saturateOn; core.setFx(fx, 120.0); }
            core.process(left.data() + at, right.data() + at, 480);
        }
        return Stereo{left, right};
    };
    const auto dry = play(false, 6.0f);
    const auto dryLevel = rmsDb(dry.left, 9600, 72000);
    // Harmonics: a 200 Hz tone at -12 dBFS through the master alone; the
    // energy above 600 Hz is what the drive adds.
    const auto harmonics = [](bool on, float drive)
    {
        dropforge::Master master;
        master.setSaturate(on, drive, 1.0f);
        master.prepare(rate);
        auto left = sine(200.0, 48000, 0.25f), right = left;
        master.process(left.data(), right.data(), 48000);
        // 24 dB/oct above 500 Hz: the 200 Hz tone itself falls 32 dB, its
        // third and higher harmonics pass.
        std::array<dsp_core::Svf, 2> above;
        for (auto& f : above) f.set(dsp_core::Svf::Type::highpass, 500.0, rate);
        std::vector<float> high(left.size());
        for (std::size_t i = 0; i < left.size(); ++i) high[i] = above[1].next(above[0].next(left[i]));
        return rmsDb(high, 9600, left.size()) - rmsDb(left, 9600, left.size());
    };
    const auto clean = harmonics(false, 0.0f);
    double previous = 0.0;
    for (const float drive : {0.0f, 6.0f, 12.0f, 24.0f})
    {
        const auto wet = play(true, drive);
        float peak = 0.0f;
        for (const auto v : wet.left) { require(std::isfinite(v), "Saturate stays finite"); peak = std::max(peak, std::abs(v)); }
        const auto level = rmsDb(wet.left, 9600, 72000) - dryLevel, brighter = harmonics(true, drive) - clean;
        std::cout << "saturate drive=" << drive << " level_db=" << level << " brighter_db=" << brighter << " peak=" << peak << '\n';
        require(std::abs(level) < 2.0, "Saturate is loudness-compensated (within 2 dB at any drive)");
        require(peak <= 0.8913f, "Saturate stays under the -1 dBFS ceiling");
        if (drive >= 12.0f) require(brighter > 3.0 && brighter > previous, "more drive adds more harmonics");
        previous = brighter;
    }
    const auto toggled = play(false, 12.0f, 24000);
    const auto steady = largestStep(toggled.left, 9600, 24000);
    const auto change = largestStep(toggled.left, 24000, 27000);
    std::cout << "saturate switch step=" << change << " steady=" << steady << '\n';
    require(change <= 1.25f * std::max(steady, largestStep(toggled.left, 40000, 72000)), "Saturate fades in without a click");
}

// The owner's routing rules (SPEC section 8, 2026-10-30): Anvil's send is
// closed in a mix and open when it plays alone; ping-pong only on a mono
// source (Furnace Mono, or Blade or Anvil alone), otherwise the Delay is stereo.
void routingRules()
{
    const auto play = [](dropforge::FxChain::Settings fx, bool anvil, bool furnace, bool blade, bool mono)
    {
        dropforge::InstrumentCore core;
        core.setLayersEnabled(anvil, furnace);
        core.setLead(blade, 0.0f);
        core.setFurnaceOptions(mono, false);
        core.setFx(fx, 120.0);
        core.prepare(rate);
        const std::uint8_t on[][3]{{0x90, 40, 120}, {0x90, 47, 120}, {0x90, 52, 120}};
        for (const auto& message : on) core.midi(message, 3);
        std::vector<float> left(48000), right(48000);
        core.process(left.data(), right.data(), 12000);
        const std::uint8_t off[][3]{{0x80, 40, 0}, {0x80, 47, 0}, {0x80, 52, 0}};
        for (const auto& message : off) core.midi(message, 3);
        core.process(left.data() + 12000, right.data() + 12000, 36000);
        return Stereo{left, right};
    };
    dropforge::FxChain::Settings anvilOnly;
    anvilOnly.delayOn = true; anvilOnly.sendAnvil = 1.0f; anvilOnly.sendFurnace = anvilOnly.sendBlade = 0.0f;
    dropforge::FxChain::Settings none;
    const auto mixDry = play(none, true, true, true, false), mixAnvil = play(anvilOnly, true, true, true, false);
    require(mixDry.left == mixAnvil.left && mixDry.right == mixAnvil.right, "in a mix Anvil's send is closed");
    const auto soloDry = play(none, true, false, false, false), soloAnvil = play(anvilOnly, true, false, false, false);
    require(rmsDb(soloAnvil.left, 14400, 48000) > rmsDb(soloDry.left, 14400, 48000) + 20.0, "soloed, Anvil sends to the Delay");
    require(dropforge::FxChain::Settings{}.sendAnvil == 0.6f, "Anvil's send defaults to 60 % (used only when soloed)");

    dropforge::FxChain::Settings ping, stereo;
    ping.delayOn = stereo.delayOn = true;
    ping.delayPingPong = true; stereo.delayPingPong = false;
    struct Case { bool anvil, furnace, blade, mono, allowed; const char* what; };
    const Case cases[] = {
        {true, true, false, false, false, "the stereo wall with Anvil: no ping-pong"},
        {false, true, true, false, false, "the stereo wall with Blade: no ping-pong"},
        {false, true, false, false, false, "the stereo wall alone: no ping-pong"},
        {true, true, true, true, true, "Furnace Mono allows ping-pong"},
        {false, false, true, false, true, "Blade alone allows ping-pong"},
        {true, false, false, false, true, "Anvil alone allows ping-pong"},
        {true, false, true, false, false, "Anvil with Blade: no ping-pong"},
    };
    for (const auto& c : cases)
    {
        require(dropforge::FxChain::pingPongAllowed(c.anvil, c.furnace, c.blade, c.mono) == c.allowed, c.what);
        const auto a = play(ping, c.anvil, c.furnace, c.blade, c.mono), b = play(stereo, c.anvil, c.furnace, c.blade, c.mono);
        const auto same = a.left == b.left && a.right == b.right;
        require(same != c.allowed, c.what);
    }
    std::cout << "routing rules passed\n";
}

void blockInvariance()
{
    const auto render = [](int block)
    {
        auto s = echoOnly();
        s.delayFeedback = 0.5f; s.duck = 0.5f; s.delayPingPong = true; s.delayWidth = 0.7f;
        s.reverbOn = true; s.reverb.modulation = 0.8f; s.reverb.preDelayMs = 37.0f;
        s.finish.crushOn = s.finish.chorusOn = s.finish.eqOn = true; s.finish.eqTiltDb = 3.0f;
        FxChain chain;
        chain.set(s, 128.0);
        chain.prepare(rate);
        Input input{noise(48000), sine(110.0, 48000, 0.4f)};
        return run(chain, input, block);
    };
    const auto a = render(512);
    const auto b = render(1), c = render(73);
    require(a.left == b.left && a.right == b.right && a.left == c.left && a.right == c.right, "identical at blocks 1, 73 and 512");
}

// The whole instrument: with sends at 0 the dry sound is bit-identical; panic
// and switching every instrument off clear the echoes.
void instrument()
{
    const auto play = [](dropforge::FxChain::Settings fx, int mode)
    {
        dropforge::InstrumentCore core;
        core.setLead(true, 0.0f);
        core.setFx(fx, 140.0);
        core.prepare(rate);
        const std::uint8_t on[][3]{{0x90, 40, 120}, {0x90, 47, 120}, {0x90, 52, 120}};
        for (const auto& message : on) core.midi(message, 3);
        std::vector<float> left(48000), right(48000);
        core.process(left.data(), right.data(), 9600);
        const std::uint8_t off[][3]{{0x80, 40, 0}, {0x80, 47, 0}, {0x80, 52, 0}};
        for (const auto& message : off) core.midi(message, 3);
        core.process(left.data() + 9600, right.data() + 9600, 4800);
        if (mode == 1) core.panic();
        if (mode == 2) { core.setLayersEnabled(false, false); core.setLead(false, 0.0f); }
        core.process(left.data() + 14400, right.data() + 14400, 33600);
        return Stereo{left, right};
    };
    dropforge::FxChain::Settings off;
    auto silentSends = off;
    silentSends.delayOn = true; silentSends.sendAnvil = silentSends.sendFurnace = silentSends.sendBlade = 0.0f;
    const auto dry = play(off, 0), quiet = play(silentSends, 0);
    require(dry.left == quiet.left && dry.right == quiet.right, "the Delay with every send at 0 leaves the dry sound bit-identical");
    auto on = off;
    on.delayOn = true;
    const auto echoes = play(on, 0);
    require(rmsDb(echoes.left, 24000, 48000) > rmsDb(dry.left, 24000, 48000) + 20.0, "the Delay's tail rings after the chord");
    const auto panicked = play(on, 1), switchedOff = play(on, 2);
    require(rmsDb(panicked.left, 24000, 48000) < -200.0 && rmsDb(switchedOff.left, 24000, 48000) < -200.0,
            "panic and every instrument off clear the echoes");
}
}

int main()
{
    timing();
    pingPong();
    feedbackAndFilters();
    ducking();
    retimeAndTails();
    reverbRoom();
    spaceAndRouting();
    crushChorusEq();
    saturate();
    blockInvariance();
    instrument();
    routingRules();
    std::cout << "fx passed\n";
    return 0;
}
