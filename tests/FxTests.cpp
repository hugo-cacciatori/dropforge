#include "dsp/FxChain.h"
#include "plugin/InstrumentCore.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

// The FX page's chain (SPEC section 8): sends, the ducked Delay and Reverb,
// Space, tempo, tails.
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

void blockInvariance()
{
    const auto render = [](int block)
    {
        auto s = echoOnly();
        s.delayFeedback = 0.5f; s.duck = 0.5f; s.delayPingPong = true; s.delayWidth = 0.7f;
        s.reverbOn = true; s.reverb.modulation = 0.8f; s.reverb.preDelayMs = 37.0f;
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
    blockInvariance();
    instrument();
    std::cout << "fx passed\n";
    return 0;
}
