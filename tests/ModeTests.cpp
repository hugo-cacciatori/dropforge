#include "plugin/InstrumentCore.h"
#include <dsp_core/LoudnessMeter.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

// Modes A-D per instrument (SPEC section 12): switching, loudness, combinations.
namespace
{
using dropforge::Engine;
using dropforge::LayerMask;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Stereo { std::vector<float> left, right; };
Stereo render(Engine& engine, int samples, int block, LayerMask mask = LayerMask::both)
{
    Stereo out{std::vector<float>(static_cast<std::size_t>(samples)), std::vector<float>(static_cast<std::size_t>(samples))};
    for (int at = 0; at < samples; at += block)
        engine.process(out.left.data() + at, out.right.data() + at, std::min(block, samples - at), mask);
    return out;
}
constexpr int chord[]{40, 47, 52, 55};
void play(Engine& engine) { for (const auto note : chord) engine.noteOn(1, note, 127); }
float largestStep(const Stereo& audio, std::size_t from, std::size_t to)
{
    float step = 0.0f;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i)
        step = std::max({step, std::abs(audio.left[i] - audio.left[i - 1]), std::abs(audio.right[i] - audio.right[i - 1])});
    return step;
}

void silentSwitchIsImmediate()
{
    for (const auto mode : {1, 2})
    {
        Engine switched, designed;
        switched.prepare(48000.0);
        switched.setFurnaceMode(mode);
        designed.setFurnaceDesign(dropforge::FurnaceDesign::candidate(static_cast<char>('a' + mode)));
        designed.prepare(48000.0);
        play(switched); play(designed);
        const auto a = render(switched, 9600, 512), b = render(designed, 9600, 512);
        require(a.left == b.left && a.right == b.right, "a Furnace mode chosen in silence plays exactly that design");
    }
    Engine switched, designed;
    switched.prepare(48000.0);
    switched.setAnvilMode(1);
    designed.setAnvilGrit(dropforge::AnvilGrit::b);
    designed.prepare(48000.0);
    play(switched); play(designed);
    const auto a = render(switched, 9600, 512), b = render(designed, 9600, 512);
    require(a.left == b.left, "an Anvil mode chosen in silence plays exactly that grit");
}

void switchWhilePlaying()
{
    for (const auto rate : {44100.0, 48000.0, 96000.0})
    {
        const auto fade = static_cast<int>(std::ceil(rate * Engine::modeFadeSeconds));
        // Natural steepness of the modes on their own, then the switch.
        float natural = 0.0f;
        for (const auto mode : {0, 1, 2})
        {
            Engine steady;
            steady.prepare(rate);
            steady.setFurnaceMode(mode);
            steady.setAnvilMode(mode == 1 ? 1 : 0);
            play(steady);
            const auto audio = render(steady, static_cast<int>(rate / 2), 512);
            natural = std::max(natural, largestStep(audio, audio.left.size() / 2, audio.left.size()));
        }
        const auto run = [&](int block)
        {
            Engine engine;
            engine.prepare(rate);
            play(engine);
            auto audio = render(engine, static_cast<int>(rate / 4), block);
            engine.setFurnaceMode(1);
            engine.setAnvilMode(1);
            require(engine.furnaceMode() == 1 && engine.anvilMode() == 1, "the new modes are reported at once");
            const auto after = render(engine, static_cast<int>(rate / 4), block);
            audio.left.insert(audio.left.end(), after.left.begin(), after.left.end());
            audio.right.insert(audio.right.end(), after.right.begin(), after.right.end());
            return audio;
        };
        const auto a = run(512), b = run(1), c = run(73);
        require(a.left == b.left && a.right == c.right, "a mode switch renders identically at block sizes 1, 73 and 512");
        const auto switchAt = static_cast<std::size_t>(rate / 4);
        const auto step = largestStep(a, switchAt - 64, switchAt + static_cast<std::size_t>(fade) + 64);
        std::cout << "rate=" << rate << " switch_step=" << step << " natural_step=" << natural << '\n';
        require(step <= 1.1f * natural, "a mode switch under a held chord steps no further than the modes themselves");
        float tail = 0.0f;
        for (std::size_t i = a.left.size() - 4800; i < a.left.size(); ++i) tail = std::max(tail, std::abs(a.left[i]));
        require(tail > 0.01f, "held notes keep sounding after the switch");
    }
    // Rapid switching queues the latest choice and stays finite.
    Engine engine;
    engine.prepare(48000.0);
    play(engine);
    for (int i = 0; i < 40; ++i)
    {
        engine.setFurnaceMode(i % 3);
        engine.setAnvilMode((i + 1) % 2);
        const auto audio = render(engine, 240, 64);
        for (std::size_t j = 0; j < audio.left.size(); ++j)
            require(std::isfinite(audio.left[j]) && std::abs(audio.left[j]) < 4.0f, "rapid switching stays finite and bounded");
    }
    engine.setFurnaceMode(2);
    render(engine, 9600, 512);
    require(engine.furnaceMode() == 2, "the last chosen mode wins after rapid switching");
}

double layerLoudness(int anvil, int furnace, LayerMask mask)
{
    Engine engine;
    engine.prepare(48000.0);
    engine.setAnvilMode(anvil);
    engine.setFurnaceMode(furnace);
    play(engine);
    const auto audio = render(engine, 4 * 48000, 512, mask);
    auto meter = std::make_unique<dsp_core::LoudnessMeter>();
    meter->prepare(48000.0);
    for (std::size_t i = 0; i < audio.left.size(); ++i) meter->process(audio.left[i], audio.right[i]);
    return meter->shortTermMaxLufs();
}

void loudnessMatched()
{
    const auto furnaceA = layerLoudness(0, 0, LayerMask::rhythm), anvilA = layerLoudness(0, 0, LayerMask::bass);
    for (const auto mode : {1, 2, 3})
    {
        const auto furnace = layerLoudness(0, mode, LayerMask::rhythm) - furnaceA;
        const auto anvil = layerLoudness(mode, 0, LayerMask::bass) - anvilA;
        std::cout << "mode=" << static_cast<char>('A' + mode) << " furnace_vs_a_lu=" << furnace << " anvil_vs_a_lu=" << anvil << '\n';
        require(std::abs(furnace) <= 1.0, "each Furnace mode is within 1 LU of A on the reference chord");
        require(std::abs(anvil) <= 1.0, "each Anvil mode is within 1 LU of A on the reference chord");
    }
}

void everyCombination()
{
    float worst = 0.0f;
    for (int anvil = 0; anvil < 4; ++anvil)
        for (int furnace = 0; furnace < 4; ++furnace)
        {
            dropforge::InstrumentCore core;
            core.setLayerLevels(6.0f, 6.0f);
            core.setModes(anvil, furnace);
            core.prepare(48000.0);
            for (const auto note : {28, 40, 47, 52, 55, 59, 64, 71})
            {
                const std::uint8_t on[]{0x90, static_cast<std::uint8_t>(note), 127};
                core.midi(on, 3);
            }
            std::vector<float> left(24000), right(24000), bassLeft(24000), bassRight(24000);
            core.process(left.data(), right.data(), 24000, LayerMask::both,
                         {bassLeft.data(), bassRight.data(), nullptr, nullptr});
            require(bassLeft == bassRight, "Anvil stays exactly mono in every combination");
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                require(std::isfinite(left[i]) && std::isfinite(right[i]), "every combination is finite");
                worst = std::max({worst, std::abs(left[i]), std::abs(right[i])});
            }
        }
    std::cout << "combinations=16 worst_peak_dbfs=" << 20.0 * std::log10(worst) << '\n';
    require(worst <= dropforge::Master::ceiling, "every combination stays under -1 dBFS at +6 dB Levels");
}
}

int main()
{
    silentSwitchIsImmediate();
    switchWhilePlaying();
    loudnessMatched();
    everyCombination();
}
