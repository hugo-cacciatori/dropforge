#include "dsp/Engine.h"
#include "support/StereoMetrics.h"
#include <juce_core/juce_core.h>
#include <iostream>

namespace
{
struct Event { int sample, kind, channel, note, value; };
struct Audio { std::vector<float> left, right, bass; };
// The Furnace mode under test (SPEC section 12): 'a'-'d' clean, 'A'-'D'
// with Destruction.
char candidate = 'a';
dropforge::FurnaceDesign design()
{
    return candidate >= 'a' ? dropforge::FurnaceDesign::candidate(candidate)
                            : dropforge::FurnaceDesign::destroyed(candidate - 'A');
}
double reconstructionError = 0.0;
int isolatedFixtures = 0;
Audio render(std::vector<Event> events, int frames, int rate, float split, float drive, float tight,
             bool verifyMasks = false)
{
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.sample < b.sample; });
    dropforge::Engine engine;
    engine.setFurnaceDesign(design());
    engine.setLayersEnabled(true, true);
    engine.setBassParameters(split, drive); engine.setRhythmParameters(drive, tight); engine.prepare(rate);
    std::array<dropforge::Engine, 3> isolated;
    if (verifyMasks)
        for (auto& copy : isolated)
        {
            copy.setFurnaceDesign(design());
            copy.setLayersEnabled(true, true);
            copy.setBassParameters(split, drive); copy.setRhythmParameters(drive, tight); copy.prepare(rate);
        }
    Audio audio{std::vector<float>(frames), std::vector<float>(frames), std::vector<float>(frames)};
    std::array<float, 512> mixedLeft{}, mixedRight{};
    std::size_t event = 0;
    for (int at = 0; at < frames;)
    {
        while (event < events.size() && events[event].sample == at)
        {
            const auto& e = events[event++];
            if (e.kind == 0) engine.noteOn(e.channel, e.note, e.value);
            else if (e.kind == 1) engine.noteOff(e.channel, e.note);
            else if (e.kind == 2) engine.pitchBend(e.channel, e.value);
            if (verifyMasks)
                for (auto& copy : isolated)
                {
                    if (e.kind == 0) copy.noteOn(e.channel, e.note, e.value);
                    else if (e.kind == 1) copy.noteOff(e.channel, e.note);
                    else if (e.kind == 2) copy.pitchBend(e.channel, e.value);
                }
            // CC64 deliberately ignored, matching the live MIDI bridge.
        }
        const auto next = event < events.size() ? events[event].sample : frames;
        const auto count = std::min({512, frames - at, next - at});
        engine.process(mixedLeft.data(), mixedRight.data(), count, dropforge::LayerMask::both,
                       {audio.bass.data() + at, nullptr, audio.left.data() + at, audio.right.data() + at});
        if (verifyMasks)
        {
            std::array<std::array<float, 512>, 6> masked{};
            for (std::size_t i = 0; i < isolated.size(); ++i)
                isolated[i].process(masked[i * 2].data(), masked[i * 2 + 1].data(), count,
                                    static_cast<dropforge::LayerMask>(i));
            for (int i = 0; i < count; ++i)
            {
                const auto index = static_cast<std::size_t>(i);
                const auto tap = static_cast<std::size_t>(at + i);
                const float bass = masked[2][index], left = masked[4][index], right = masked[5][index];
                const auto error = std::max(std::abs(static_cast<double>(mixedLeft[index]) - (bass + left)),
                                            std::abs(static_cast<double>(mixedRight[index]) - (bass + right)));
                reconstructionError = std::max(reconstructionError, error);
                if (!std::isfinite(error) || error != 0.0 || masked[0][index] != 0.0f || masked[1][index] != 0.0f
                    || bass != masked[3][index] || bass != audio.bass[tap]
                    || left != audio.left[tap] || right != audio.right[tap])
                    throw std::runtime_error("Both masks/taps must reconstruct exactly with centered bass and silent None");
            }
        }
        at += count;
    }
    if (verifyMasks) ++isolatedFixtures;
    return audio;
}
juce::Array<juce::var> rows;
int failures = 0;
void check(const juce::String& id, const Audio& audio, int rate, float split, float drive, float tight)
{
    // SPEC section 4 separation gate: below 100 Hz, Furnace stays 20 dB under
    // Anvil. (split is Anvil's own grit voicing, not the Furnace boundary.)
    constexpr double separationGateHz = 100.0;
    const auto m = dsp_core::test::measure(audio.left, audio.right, audio.bass, rate, separationGateHz);
    // Below -90 dBFS the bass band is effectively silent: retain absolute
    // rhythm-floor evidence instead of dividing by a near-zero reference.
    // With Anvil sounding below 100 Hz, the SPEC gate: Furnace 20 dB under it.
    // When Anvil has no lows (high roots), Furnace may not add rumble of its
    // own: its lows stay 40 dB under its own level (the drive's beat tones from
    // the detuned saws measure about -54 dB).
    const auto furnaceDbfs = dsp_core::test::powerDb(m.stereoPower);
    const bool lowPass = m.lowBassDbfs > -90.0 ? m.separationDb <= -20.0
                                               : m.lowRhythmDbfs <= std::max(-90.0, furnaceDbfs - 40.0);
    const bool widthPass = m.correlation >= 0.0 && m.correlation <= 0.5;
    const bool foldPass = m.foldLossDb <= 3.0;
    const bool pass = widthPass && foldPass && lowPass && m.stereoPower > 1.0e-12;
    auto row = std::make_unique<juce::DynamicObject>();
    row->setProperty("id", id); row->setProperty("furnace", juce::String::charToString(candidate)); row->setProperty("sample_rate", rate); row->setProperty("split", split);
    row->setProperty("drive", drive); row->setProperty("tight", tight);
    row->setProperty("correlation", m.correlation); row->setProperty("fold_loss_db", m.foldLossDb);
    row->setProperty("low_rhythm_dbfs", m.lowRhythmDbfs); row->setProperty("low_bass_dbfs", m.lowBassDbfs);
    row->setProperty("rhythm_vs_bass_low_db", m.separationDb); row->setProperty("passed", pass);
    row->setProperty("width_passed", widthPass); row->setProperty("low_passed", lowPass);
    row->setProperty("window", id.startsWith("hold-") ? "settled_hann_10s" : "complete_fixture");
    row->setProperty("fold_band_dbfs", m.foldBandDbfs); row->setProperty("fold_passed", foldPass);
    rows.add(juce::var(row.release()));
    std::cout << id << " furnace=" << candidate << " rate=" << rate << " split=" << split << " drive=" << drive << " tight=" << tight
              << " correlation=" << m.correlation << " fold_loss_db=" << m.foldLossDb
              << " low_ratio_db=" << m.separationDb << " low_bass_dbfs=" << m.lowBassDbfs
              << " fold_band_dbfs=" << m.foldBandDbfs
              << " pass=" << pass << std::endl;
    if (!pass) ++failures;
}
void fixtures(const juce::File& file, int rate, float split, float drive, float tight, bool verifyMasks)
{
    const auto specification = juce::JSON::parse(file.loadFileAsString());
    const auto* sequences = specification["sequences"].getArray();
    if (!sequences || sequences->size() != 5) throw std::runtime_error("five standard fixtures required");
    const auto samplesPerBeat = rate * 60.0 / static_cast<double>(specification["tempo_bpm"]);
    for (const auto& sequence : *sequences)
    {
        std::vector<Event> events;
        for (const auto& note : *sequence["notes"].getArray())
        {
            const auto at = static_cast<double>(note[0]), duration = static_cast<double>(note[1]);
            const auto channel = static_cast<int>(note[2]), pitch = static_cast<int>(note[3]);
            events.push_back({static_cast<int>(std::llround(at * samplesPerBeat)), 0, channel, pitch, static_cast<int>(note[4])});
            events.push_back({static_cast<int>(std::llround((at + duration) * samplesPerBeat)), 1, channel, pitch, 0});
        }
        if (const auto* bends = sequence["bends"].getArray())
            for (const auto& bend : *bends)
                events.push_back({static_cast<int>(std::llround(static_cast<double>(bend[0]) * samplesPerBeat)),
                                  2, static_cast<int>(bend[1]), 0, static_cast<int>(bend[2])});
        const auto frames = static_cast<int>(std::llround(static_cast<double>(sequence["length_beats"]) * samplesPerBeat + rate * 0.1));
        check(sequence["id"].toString(), render(events, frames, rate, split, drive, tight, verifyMasks), rate, split, drive, tight);
    }
}
void settledWindow(Audio& audio, int rate)
{
    // Integrate the settled part after one second, suppressing artificial LF
    // leakage from non-integer periods; RMS normalization preserves stationary power.
    for (auto* side : {&audio.left, &audio.right, &audio.bass})
    {
        *side = std::vector<float>(side->begin() + rate, side->end());
        for (std::size_t i = 0; i < side->size(); ++i)
            (*side)[i] *= static_cast<float>(std::sqrt(8.0 / 3.0) * 0.5
                * (1.0 - std::cos(6.28318530717958647692 * i / side->size())));
    }
}
// The settled window must span about two cycles of Furnace's slowest
// pulse-width motion (0.23 Hz, 4.3 s): width, fold-down and correlation swing
// with it, so a shorter window measures wherever the cycle happens to be.
constexpr int holdSeconds = 10;
using Holds = std::vector<std::vector<int>>;
// Every register boundary and the chords that stress the low split.
const Holds allHolds{{0}, {24}, {36}, {45}, {48}, {60}, {96}, {127},
                     {28, 35, 40}, {40, 47, 52}, {24, 31, 36, 40, 43, 48, 52, 55}};
// The extremes only, for the parameter corners and the highest sample rate.
const Holds edgeHolds{{0}, {127}, {28, 35, 40}, {24, 31, 36, 40, 43, 48, 52, 55}};
void holds(const Holds& chords, int rate, float split, float drive, float tight)
{
    for (const auto& notes : chords)
    {
        std::vector<Event> events;
        juce::String id = "hold";
        for (auto note : notes) { events.push_back({0, 0, 1, note, 127}); id += "-" + juce::String(note); }
        auto audio = render(events, rate * holdSeconds, rate, split, drive, tight);
        settledWindow(audio, rate);
        check(id, audio, rate, split, drive, tight);
    }
}
}
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const juce::File output(argv[2]);
    if (!output.replaceWithText("{\"passed\":false}")) return 2;
    // The lowest and highest supported rates bracket the default 48 kHz; mask
    // reconstruction is structural, so its four-engine check runs once.
    for (int rate : {44100, 48000, 192000})
    {
        fixtures(juce::File(argv[1]), rate, 140, 0.5f, 0.5f, rate == 48000);
        holds(rate == 192000 ? edgeHolds : allHolds, rate, 140, 0.5f, 0.5f);
    }
    // Every mode, clean and destroyed, passes the same gates at the default rate.
    for (const char other : {'b', 'c', 'd', 'A', 'B', 'C', 'D'})
    {
        candidate = other;
        fixtures(juce::File(argv[1]), 48000, 140, 0.5f, 0.5f, false);
        holds(edgeHolds, 48000, 140, 0.5f, 0.5f);
    }
    candidate = 'a';
    // Corners of what can actually change: Anvil's split voicing and the
    // Tight range. Furnace's drive is fixed in the product (SPEC section 2).
    for (float split : {80.0f, 250.0f})
        for (float tight : {0.0f, 1.0f})
            holds(edgeHolds, 48000, split, 0.5f, tight);
    auto result = std::make_unique<juce::DynamicObject>();
    result->setProperty("schema_version", 1); result->setProperty("source_commit", DROPFORGE_COMMIT);
    result->setProperty("source_dirty", DROPFORGE_DIRTY); result->setProperty("passed", failures == 0);
    result->setProperty("routing", "both_lowest_root");
    result->setProperty("isolated_fixtures", isolatedFixtures);
    result->setProperty("max_reconstruction_error", reconstructionError);
    result->setProperty("cases", juce::var(rows));
    if (!output.replaceWithText(juce::JSON::toString(juce::var(result.release())))) return 2;
    std::cout << "Both isolated_fixtures=" << isolatedFixtures
              << " max_reconstruction_error=" << reconstructionError << " matrix_cases=" << rows.size() << '\n';
    return failures ? 1 : 0;
}
