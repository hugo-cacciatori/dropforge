#include "dsp/Engine.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>

// Furnace's per-side chain (SPEC section 4): tone shape, Tight, the
// separation cut and its glide, drive behaviour, chop-gate alignment and
// safety. Gains are relative to 1 kHz, so the output trim can be tuned freely.
namespace
{
constexpr double pi = 3.14159265358979323846;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

struct Setup { double rate = 48000.0, drive = 0.0, tight = 0.5; bool anvil = true; };
dropforge::RhythmBus make(const Setup& setup)
{
    dropforge::RhythmBus bus;
    bus.setParameters(setup.drive, setup.tight);
    bus.setAnvilPresent(setup.anvil);
    bus.prepare(setup.rate);
    return bus;
}
// Steady-state gain of a sine through the chain, in dB.
double gainDb(double frequency, const Setup& setup, double amplitude = 0.1)
{
    auto bus = make(setup);
    const auto settle = static_cast<int>(setup.rate / 2), length = static_cast<int>(setup.rate / 2);
    double in = 0.0, out = 0.0;
    for (int i = 0; i < settle + length; ++i)
    {
        const auto x = static_cast<float>(amplitude * std::sin(2.0 * pi * frequency * i / setup.rate));
        const auto filtered = bus.next(x); // next() before gain(): gain() describes that sample
        const auto y = filtered * bus.gain();
        if (i >= settle) { in += x * x; out += y * y; }
    }
    return db(std::sqrt(out / in));
}

void toneShape()
{
    for (const double rate : {44100.0, 48000.0, 192000.0})
    {
        Setup setup{rate};
        const auto reference = gainDb(1000.0, setup);
        const auto relative = [&](double hz) { return gainDb(hz, setup) - reference; };
        const auto deep = relative(40.0), body = relative(250.0), contour = relative(2800.0);
        const auto air = relative(12000.0), top = relative(16000.0);
        std::cout << "rate=" << rate << " gain_re_1k_db 40Hz=" << deep << " 250Hz=" << body << " 2k8=" << contour
                  << " 12k=" << air << " 16k=" << top << '\n';
        require(deep < -40.0, "with Anvil on, Furnace keeps out of the deep lows");
        require(body > -6.0 && body < 1.0, "chord body above Anvil's range stays in (no 350 Hz cut)");
        require(contour > 2.0 && contour < 6.0, "the 2.8 kHz contour pushes the upper mids");
        require(air > 1.0 && air < 5.0, "air shelf lifts the top: no 6 kHz low-pass");
        require(top > 0.0, "16 kHz is still lifted, not filtered away");
    }
}

void tightAndSeparation()
{
    const auto loose = gainDb(90.0, {48000.0, 0.0, 0.0}), tight = gainDb(90.0, {48000.0, 0.0, 1.0});
    require(loose - tight > 6.0, "Tight trades low body for chug");
    const auto withAnvil = gainDb(60.0, {48000.0, 0.0, 0.0, true});
    const auto alone = gainDb(60.0, {48000.0, 0.0, 0.0, false});
    std::cout << "60Hz with_anvil=" << withAnvil << " alone=" << alone << '\n';
    require(alone - withAnvil > 12.0, "without Anvil the separation cut moves down so Furnace keeps its body");

    // The glide: switching Anvil off mid-note moves the cut over about 50 ms,
    // with no step in a steady 60 Hz tone.
    auto bus = make({48000.0, 0.0, 0.0, true});
    float previous = 0.0f, largestStep = 0.0f;
    double early = 0.0, late = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        if (i == 24000) bus.setAnvilPresent(false);
        const auto filtered = bus.next(static_cast<float>(0.5 * std::sin(2.0 * pi * 60.0 * i / 48000.0)));
        const auto y = filtered * bus.gain();
        if (i > 1000) largestStep = std::max(largestStep, std::abs(y - previous));
        previous = y;
        if (i >= 20000 && i < 24000) early += y * y;
        if (i >= 44000) late += y * y;
    }
    std::cout << "glide largest_step=" << largestStep << " rise_db=" << db(std::sqrt(late / early)) << '\n';
    require(db(std::sqrt(late / early)) > 12.0, "the glide reaches the body setting");
    require(largestStep < 0.02f, "the glide moves smoothly, without a click");
}

void drive()
{
    // Small signals keep their level whatever the drive (no loudness jump).
    const auto clean = gainDb(1000.0, {48000.0, 0.0}, 0.001), driven = gainDb(1000.0, {48000.0, 1.0}, 0.001);
    require(std::abs(clean - driven) < 0.5, "drive has unity small-signal gain");
    // Two chord tones interact in the drive: a difference tone appears.
    double linear = 0.0, shaped = 0.0;
    for (const double amount : {0.0, 0.5})
    {
        auto bus = make({48000.0, amount});
        std::complex<double> bin{};
        for (int i = 0; i < 96000; ++i)
        {
            const auto t = i / 48000.0;
            const auto filtered = bus.next(static_cast<float>(0.6 * std::sin(2.0 * pi * 1000.0 * t) + 0.6 * std::sin(2.0 * pi * 1500.0 * t)));
            const auto y = filtered * bus.gain();
            if (i >= 48000) bin += static_cast<double>(y) * std::polar(1.0, -2.0 * pi * 500.0 * t);
        }
        (amount == 0.0 ? linear : shaped) = std::abs(bin) / 48000.0;
    }
    std::cout << "difference_tone linear=" << linear << " driven=" << shaped << '\n';
    require(linear < 1.0e-5 && shaped > 1.0e-3, "chord notes interact in the drive, and only there");
    // Shape is bounded and finite for any input.
    for (const float x : {-1.0e6f, -10.0f, -1.0f, 0.0f, 1.0f, 10.0f, 1.0e6f})
        require(std::isfinite(dropforge::RhythmBus::shape(x, 1.0f)) && std::abs(dropforge::RhythmBus::shape(x, 1.0f)) < 1.0f,
                "drive curve stays bounded");
}

void gateAlignment()
{
    // The chop gate follows the note envelope delayed by exactly the drive's
    // latency, so compression or filtering can never smear the chop.
    auto bus = make({48000.0, 0.5});
    const int stop = 4800;
    int lastSound = -1;
    for (int i = 0; i < 9600; ++i)
    {
        const auto envelope = i < stop ? 1.0f : 0.0f;
        const auto filtered = bus.next(static_cast<float>(0.5 * std::sin(2.0 * pi * 440.0 * i / 48000.0)), envelope, 1.0f);
        const auto y = filtered * bus.gain();
        if (y != 0.0f) lastSound = i;
    }
    std::cout << "gate last_sound=" << lastSound << " expected=" << stop + dropforge::RhythmBus::latencySamples - 1 << '\n';
    require(lastSound == stop + dropforge::RhythmBus::latencySamples - 1, "gate closes exactly one latency after the note");
}

void safety()
{
    for (const double rate : {44100.0, 192000.0})
    {
        auto bus = make({rate, 1.0, 1.0});
        float peak = 0.0f;
        for (int i = 0; i < static_cast<int>(rate); ++i)
        {
            const auto x = static_cast<float>(1000.0 * std::sin(2.0 * pi * 50.0 * i / rate));
            const auto filtered = bus.next(x, 1.0f, 1.0f);
            const auto y = filtered * bus.gain();
            require(std::isfinite(y), "finite under extreme input");
            peak = std::max(peak, std::abs(y));
        }
        require(peak < 4.0f, "bounded under extreme input");
    }
}
}

int main()
{
    toneShape();
    tightAndSeparation();
    drive();
    gateAlignment();
    safety();
    std::cout << "Furnace chain: tone, Tight, separation glide, drive, gate alignment and safety passed\n";
}
