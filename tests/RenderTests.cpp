#include "plugin/InstrumentCore.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void add(juce::MidiMessageSequence& sequence, juce::MidiMessage message, double ticks)
{
    message.setTimeStamp(ticks);
    sequence.addEvent(message);
}

int run(const juce::String& tool, const juce::StringArray& arguments)
{
    juce::StringArray command{tool};
    command.addArray(arguments);
    juce::ChildProcess process;
    require(process.start(command), "renderer process starts");
    require(process.waitForProcessToFinish(30000), "renderer finishes within 30 seconds");
    const auto output = process.readAllProcessOutput();
    if (process.getExitCode() != 0) std::cout << output;
    return static_cast<int>(process.getExitCode());
}

bool identical(const juce::File& a, const juce::File& b)
{
    juce::MemoryBlock x, y;
    return a.loadFileAsData(x) && b.loadFileAsData(y) && x == y;
}

int standardSequences(const juce::String& tool, const juce::File& build, const juce::File& fixtures, bool bassOnly)
{
    const juce::String suite = bassOnly ? "bass-sequences" : "standard-sequences";
    require(build.getChildFile(suite + "-result.json").replaceWithText("{\"schema_version\":1,\"passed\":false}"),
            "invalidate previous suite receipt before testing");
    const auto specification = juce::JSON::parse(fixtures.loadFileAsString());
    require(static_cast<int>(specification["schema_version"]) == 1, "supported fixture schema");
    const auto* sequences = specification["sequences"].getArray();
    require(sequences && sequences->size() == 5, "all five standard regression sequences exist");
    const auto rate = static_cast<int>(specification["sample_rate"]);
    const auto block = static_cast<int>(specification["block_size"]);
    const auto ticks = static_cast<int>(specification["ticks_per_quarter"]);
    const auto bpm = static_cast<double>(specification["tempo_bpm"]);
    const auto tail = static_cast<double>(specification["tail_seconds"]);
    require(rate == 48000 && block == 512 && ticks == 960 && bpm == 120.0 && tail == 0.1
            && static_cast<double>(specification["output_db"]) == 0.0, "documented standard settings");
    const auto root = build.getChildFile(suite + "-" + juce::Uuid{}.toString());
    require(root.createDirectory().wasOk(), "unique standard render directory created");
    int lowest = 127, highest = 0, totalNotes = 0;
    juce::Array<juce::var> results;
    for (const auto& sequence : *sequences)
    {
        const auto id = sequence["id"].toString();
        require(id.isNotEmpty() && id.containsOnly("abcdefghijklmnopqrstuvwxyz-"), "safe fixture filename");
        const auto length = static_cast<double>(sequence["length_beats"]);
        const auto* notes = sequence["notes"].getArray();
        require(notes && !notes->isEmpty() && length > 0.0, "fixture contains notes and a duration");
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote(ticks);
        juce::MidiMessageSequence track;
        add(track, juce::MidiMessage::tempoMetaEvent(static_cast<int>(60000000.0 / bpm)), 0);
        for (const auto& row : *notes)
        {
            require(row.isArray() && row.size() == 5, "note row: beat, duration, channel, pitch, velocity");
            const auto beat = static_cast<double>(row[0]), duration = static_cast<double>(row[1]);
            const auto channel = static_cast<int>(row[2]), pitch = static_cast<int>(row[3]), velocity = static_cast<int>(row[4]);
            require(beat >= 0.0 && duration > 0.0 && beat + duration <= length && channel >= 1 && channel <= 16
                    && pitch >= 0 && pitch <= 127 && velocity == 127, "valid full-velocity note inside fixture");
            add(track, juce::MidiMessage::noteOn(channel, pitch, static_cast<juce::uint8>(velocity)), beat * ticks);
            add(track, juce::MidiMessage::noteOff(channel, pitch), (beat + duration) * ticks);
            lowest = std::min(lowest, pitch); highest = std::max(highest, pitch); ++totalNotes;
        }
        if (const auto* bends = sequence["bends"].getArray())
            for (const auto& row : *bends)
                add(track, juce::MidiMessage::pitchWheel(static_cast<int>(row[1]), static_cast<int>(row[2])),
                    static_cast<double>(row[0]) * ticks);
        if (const auto* controllers = sequence["controllers"].getArray())
            for (const auto& row : *controllers)
                add(track, juce::MidiMessage::controllerEvent(static_cast<int>(row[1]), static_cast<int>(row[2]),
                                                           static_cast<int>(row[3])), static_cast<double>(row[0]) * ticks);
        add(track, juce::MidiMessage::endOfTrack(), length * ticks);
        midi.addTrack(track);
        const auto input = root.getChildFile(id + ".mid"), wav = root.getChildFile(id + ".wav");
        const auto json = root.getChildFile(id + ".json");
        { auto out = input.createOutputStream(); require(out && midi.writeTo(*out, 0), "standard MIDI file generated"); }
        for (const bool repeat : {false, true})
        {
            const auto outputWav = repeat ? root.getChildFile(id + "-repeat.wav") : wav;
            const auto outputJson = repeat ? root.getChildFile(id + "-repeat.json") : json;
            require(run(tool, {input.getFullPathName(), outputWav.getFullPathName(), outputJson.getFullPathName(),
                               "--sample-rate", juce::String(rate), "--block-size", repeat ? "73" : juce::String(block),
                               "--output-db", "0", "--tail-seconds", juce::String(tail),
                               "--layer-mask", bassOnly ? "bass" : "both", "--rhythm-enabled", bassOnly ? "0" : "1"}) == 0,
                    "standard sequence renders successfully");
        }
        require(identical(wav, root.getChildFile(id + "-repeat.wav")), "standard WAV unchanged across blocks 512/73");
        const auto metrics = juce::JSON::parse(json.loadFileAsString());
        require(metrics["layer_mask"].toString() == (bassOnly ? "bass" : "both")
                && metrics["bass_enabled"].isBool() && static_cast<bool>(metrics["bass_enabled"])
                && metrics["rhythm_enabled"].isBool() && static_cast<bool>(metrics["rhythm_enabled"]) == !bassOnly
                && static_cast<double>(metrics["split_hz"]) == 140.0 && static_cast<double>(metrics["destruction"]) == 0.5
                && static_cast<int>(metrics["latency_samples"]) == dropforge::Engine::latencySamples,
                "render metrics identify exact diagnostic mask, bass controls and latency");
        const auto frames = static_cast<int>(std::llround(length * 60.0 / bpm * rate + tail * rate));
        require(static_cast<int>(metrics["frames"]) == frames && static_cast<int>(metrics["nonfinite_samples"]) == 0
                && static_cast<int>(metrics["clipped_samples"]) == 0
                && static_cast<double>(metrics["peak"]) > 0.01 && static_cast<double>(metrics["peak"]) < 1.0,
                "expected duration, audible numerical signal, finite and unclipped at defaults");
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::FileInputStream(wav), true));
        require(reader && reader->lengthInSamples == frames, "standard WAV length verified");
        juce::AudioBuffer<float> audio(2, frames);
        require(reader->read(&audio, 0, frames, 0, true, true), "standard WAV decodes");
        if (bassOnly)
        {
            require(static_cast<double>(metrics["left_minus_right_rms"]) == 0.0, "bass-only metrics are exactly mono");
            for (int i = 0; i < frames; ++i)
                require(audio.getSample(0, i) == audio.getSample(1, i), "every bass-only WAV frame has exact L == R");
        }
        require(audio.getMagnitude(frames - rate / 20, rate / 20) == 0.0f, "final 50 ms is exact silence, no stuck notes");
        for (const auto& row : *notes)
        {
            const auto onset = static_cast<int>(std::llround(static_cast<double>(row[0]) * 60.0 / bpm * rate));
            const auto duration = static_cast<int>(std::llround(static_cast<double>(row[1]) * 60.0 / bpm * rate));
            require(audio.getMagnitude(onset, duration) > 0.0001f, "every scheduled note interval contains signal");
        }
        if (id == "held-sweep")
        {
            for (int second = 1; second < 5; ++second)
                require(audio.getRMSLevel(0, second * rate, rate / 2) > 0.001f, "held note sustains through bend sweep");
            require(audio.getMagnitude(5 * rate + rate / 10, rate / 4) == 0.0f,
                    "note release is silent while sustain pedal is still down");
        }
        juce::DynamicObject::Ptr result = new juce::DynamicObject;
        result->setProperty("id", id);
        result->setProperty("notes", notes->size());
        result->setProperty("metrics", metrics);
        results.add(juce::var(result.get()));
        std::cout << "suite=" << suite << " sequence=" << id << " notes=" << notes->size() << " frames=" << frames
                  << " peak=" << static_cast<double>(metrics["peak"])
                  << " rms=" << static_cast<double>(metrics["rms"]) << " clipped=0 finite=yes repeat=exact\n";
    }
    require(lowest == 0 && highest == 127 && totalNotes == 168, "full MIDI range and full-velocity suite coverage retained");
    require(root.getChildFile("summary.json").replaceWithText(juce::JSON::toString(results)), "suite metrics saved");
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject;
    receipt->setProperty("schema_version", 1);
    receipt->setProperty("passed", true);
    receipt->setProperty("root", root.getFullPathName());
    require(build.getChildFile(suite + "-result.json").replaceWithText(juce::JSON::toString(juce::var(receipt.get()))),
            "successful suite publishes its artifact location only after all checks pass");
    std::cout << "suite=" << suite << " root=" << root.getFullPathName() << '\n';
    return 0;
}
}

int main(int argc, char** argv)
{
    if (argc == 4 || argc == 5)
    {
        require(argc == 4 || juce::String(argv[4]) == "--bass", "supported standard suite option");
        return standardSequences(argv[1], juce::File(argv[2]), juce::File(argv[3]), argc == 5);
    }
    require(argc == 3, "tool path and test output root supplied");
    const auto root = juce::File(argv[2]).getChildFile("render-" + juce::Uuid{}.toString());
    require(root.createDirectory().wasOk(), "unique test directory created");
    const auto input = root.getChildFile("tempo.mid");
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(480);
    juce::MidiMessageSequence tempo, notes, chord;
    add(tempo, juce::MidiMessage::tempoMetaEvent(500000), 0);
    add(tempo, juce::MidiMessage::tempoMetaEvent(250000), 480);
    add(tempo, juce::MidiMessage::endOfTrack(), 960);
    add(notes, juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127)), 240);
    add(notes, juce::MidiMessage::pitchWheel(1, 16383), 480);
    add(notes, juce::MidiMessage::controllerEvent(1, 64, 127), 480);
    add(notes, juce::MidiMessage::noteOff(1, 69), 720);
    add(notes, juce::MidiMessage::noteOn(2, 45, static_cast<juce::uint8>(96)), 900);
    add(chord, juce::MidiMessage::noteOn(1, 81, static_cast<juce::uint8>(127)), 240);
    add(chord, juce::MidiMessage::noteOn(1, 81, static_cast<juce::uint8>(0)), 720);
    midi.addTrack(tempo); midi.addTrack(notes); midi.addTrack(chord);
    {
        auto stream = input.createOutputStream();
        require(stream && midi.writeTo(*stream, 1), "type 1 tempo/chord fixture written");
    }
    const auto wav = root.getChildFile("a.wav"), json = root.getChildFile("a.json");
    const juce::StringArray paths{input.getFullPathName(), wav.getFullPathName(), json.getFullPathName()};
    require(run(argv[1], paths) == 0, "default render succeeds");
    for (const auto& name : {juce::String("repeat"), juce::String("one"), juce::String("odd")})
    {
        juce::StringArray args{input.getFullPathName(), root.getChildFile(name + ".wav").getFullPathName(),
                              root.getChildFile(name + ".json").getFullPathName()};
        if (name != "repeat") { args.add("--block-size"); args.add(name == "one" ? "1" : "73"); }
        require(run(argv[1], args) == 0, "repeat/alternate block render succeeds");
        require(identical(wav, root.getChildFile(name + ".wav")), "WAV is byte-identical across runs/block sizes");
        if (name == "repeat") require(identical(json, root.getChildFile("repeat.json")), "metrics are byte-identical");
    }
    const auto metrics = juce::JSON::parse(json.loadFileAsString());
    require(metrics["bass_enabled"].isBool() && static_cast<bool>(metrics["bass_enabled"])
            && metrics["rhythm_enabled"].isBool() && static_cast<bool>(metrics["rhythm_enabled"]),
            "renderer defaults to Both routing, independently of diagnostic masks");
    require(static_cast<int>(metrics["frames"]) == 40800 && static_cast<int>(metrics["midi_events"]) == 7,
            "tempo map, end marker and release tail set exact duration/event count");
    require(static_cast<double>(metrics["peak"]) > 0.01 && static_cast<double>(metrics["peak"]) < 1.0
            && static_cast<int>(metrics["clipped_samples"]) == 0
            && static_cast<double>(metrics["left_minus_right_rms"]) > 0.0, "finite, unclipped stereo metrics");
    require(static_cast<int>(metrics["last_nonzero_frame"]) < 36300 + dropforge::Engine::latencySamples,
            "hanging note released at file end plus reported latency");
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::FileInputStream(wav), true));
    require(reader && reader->usesFloatingPointData && reader->bitsPerSample == 32 && reader->numChannels == 2
            && reader->lengthInSamples == 40800 && reader->sampleRate == 48000, "float32 stereo WAV metadata");
    juce::AudioBuffer<float> audio(2, 40800), expected(2, 40800);
    require(reader->read(&audio, 0, 40800, 0, true, true), "WAV payload decodes");
    dropforge::InstrumentCore core;
    core.setLayersEnabled(true, true);
    core.prepare(48000);
    struct Event { int frame; juce::MidiMessage message; };
    // Independent expected times in samples: 500 ms/quarter then 250 ms/quarter.
    const std::vector<Event> events{
        {12000, juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127))},
        {12000, juce::MidiMessage::noteOn(1, 81, static_cast<juce::uint8>(127))},
        {24000, juce::MidiMessage::pitchWheel(1, 16383)},
        {24000, juce::MidiMessage::controllerEvent(1, 64, 127)},
        {30000, juce::MidiMessage::noteOff(1, 69)},
        {30000, juce::MidiMessage::noteOff(1, 81)},
        {34500, juce::MidiMessage::noteOn(2, 45, static_cast<juce::uint8>(96))}};
    int cursor = 0;
    for (const auto& event : events)
    {
        core.process(expected.getWritePointer(0, cursor), expected.getWritePointer(1, cursor), event.frame - cursor);
        core.midi(event.message.getRawData(), event.message.getRawDataSize());
        cursor = event.frame;
    }
    core.process(expected.getWritePointer(0, cursor), expected.getWritePointer(1, cursor), 36000 - cursor);
    core.panic();
    core.process(expected.getWritePointer(0, 36000), expected.getWritePointer(1, 36000), 4800);
    double squares = 0.0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 40800; ++sample)
        {
            require(audio.getSample(channel, sample) == expected.getSample(channel, sample),
                    "tempo/event placement and WAV samples exactly match headless core");
            const double value = audio.getSample(channel, sample);
            squares += value * value;
        }
    require(std::abs(std::sqrt(squares / 81600.0) - static_cast<double>(metrics["rms"])) < 1.0e-12,
            "metrics RMS matches decoded WAV");
    struct Diagnostic
    {
        const char* name;
        dropforge::LayerMask mask;
        const char* componentName = "all";
        dropforge::BassComponent component = dropforge::BassComponent::all;
    };
    for (const bool bassEnabled : {false, true})
    for (const bool rhythmEnabled : {false, true})
    for (const auto setting : {Diagnostic{"both", dropforge::LayerMask::both}, Diagnostic{"bass", dropforge::LayerMask::bass},
                               Diagnostic{"rhythm", dropforge::LayerMask::rhythm}, Diagnostic{"none", dropforge::LayerMask::none},
                               Diagnostic{"bass", dropforge::LayerMask::bass, "clean", dropforge::BassComponent::clean},
                               Diagnostic{"bass", dropforge::LayerMask::bass, "grit", dropforge::BassComponent::grit},
                               Diagnostic{"bass", dropforge::LayerMask::bass, "dry-grit", dropforge::BassComponent::dryGrit},
                               Diagnostic{"bass", dropforge::LayerMask::bass, "unison-grit", dropforge::BassComponent::unisonGrit}})
    {
        const auto name = juce::String(setting.name);
        const auto stem = name + (setting.component == dropforge::BassComponent::all ? "" : juce::String("-") + setting.componentName)
                              + (bassEnabled ? "-b1" : "-b0")
                              + (rhythmEnabled ? "-g1" : "-g0");
        const auto maskedWav = root.getChildFile(stem + ".wav"), maskedJson = root.getChildFile(stem + ".json");
        require(run(argv[1], {input.getFullPathName(), maskedWav.getFullPathName(), maskedJson.getFullPathName(),
                             "--layer-mask", name, "--split-hz", "80", "--destruction", "1", "--block-size", "73",
                             "--bass-enabled", bassEnabled ? "1" : "0",
                             "--rhythm-enabled", rhythmEnabled ? "1" : "0", "--bass-component", setting.componentName}) == 0,
                "all CLI masks and non-default bass settings render successfully");
        dropforge::InstrumentCore reference;
        reference.setLayersEnabled(bassEnabled, rhythmEnabled);
        reference.prepare(48000, 0.0f, 80.0f, 1.0f);
        juce::AudioBuffer<float> isolated(2, 40800), decoded(2, 40800);
        int offset = 0;
        for (const auto& event : events)
        {
            reference.process(isolated.getWritePointer(0, offset), isolated.getWritePointer(1, offset), event.frame - offset,
                              setting.mask, {}, setting.component);
            reference.midi(event.message.getRawData(), event.message.getRawDataSize());
            offset = event.frame;
        }
        reference.process(isolated.getWritePointer(0, offset), isolated.getWritePointer(1, offset), 36000 - offset,
                          setting.mask, {}, setting.component);
        reference.panic();
        reference.process(isolated.getWritePointer(0, 36000), isolated.getWritePointer(1, 36000), 4800,
                          setting.mask, {}, setting.component);
        std::unique_ptr<juce::AudioFormatReader> maskedReader(format.createReaderFor(new juce::FileInputStream(maskedWav), true));
        require(maskedReader && maskedReader->lengthInSamples == 40800 && maskedReader->read(&decoded, 0, 40800, 0, true, true),
                "masked WAV decodes at expected length");
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 40800; ++sample)
            {
                require(decoded.getSample(channel, sample) == isolated.getSample(channel, sample),
                        "every CLI mask/control sample exactly matches independent headless-core replay");
                if ((!bassEnabled && !rhythmEnabled) || setting.mask == dropforge::LayerMask::none
                    || (!bassEnabled && setting.mask == dropforge::LayerMask::bass)
                    || (!rhythmEnabled && setting.mask == dropforge::LayerMask::rhythm))
                    require(decoded.getSample(channel, sample) == 0.0f, "inactive contribution is exact silence");
                if (!rhythmEnabled || setting.mask == dropforge::LayerMask::bass)
                    require(decoded.getSample(0, sample) == decoded.getSample(1, sample),
                            "Bass-only renders are exactly centered");
            }
        const auto recorded = juce::JSON::parse(maskedJson.loadFileAsString());
        require(recorded["layer_mask"].toString() == name && static_cast<double>(recorded["split_hz"]) == 80.0
                && recorded["bass_component"].toString() == setting.componentName
                && recorded["bass_enabled"].isBool() && static_cast<bool>(recorded["bass_enabled"]) == bassEnabled
                && recorded["rhythm_enabled"].isBool() && static_cast<bool>(recorded["rhythm_enabled"]) == rhythmEnabled
                && static_cast<double>(recorded["destruction"]) == 1.0 && static_cast<int>(recorded["latency_samples"]) == 32,
                "CLI non-default controls and diagnostic mask are recorded accurately");
    }
    require(!identical(root.getChildFile("bass-b1-g0.wav"), root.getChildFile("bass-b1-g1.wav")),
            "higher newest key distinguishes solo last-note priority from combined lowest-root isolation");
    // Reconstruct unchanged Both routing from separately rendered masks. Disabling
    // rhythm instead would change this fixture's selected bass root.
    juce::AudioBuffer<float> full(2, 40800), bass(2, 40800), rhythm(2, 40800);
    for (auto item : {std::make_pair("both-b1-g1.wav", &full), std::make_pair("bass-b1-g1.wav", &bass),
                      std::make_pair("rhythm-b1-g1.wav", &rhythm)})
    {
        std::unique_ptr<juce::AudioFormatReader> decoded(format.createReaderFor(
            new juce::FileInputStream(root.getChildFile(item.first)), true));
        require(decoded && decoded->read(item.second, 0, 40800, 0, true, true), "isolation WAV decodes");
    }
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 40800; ++sample)
            require(full.getSample(channel, sample) == bass.getSample(channel, sample) + rhythm.getSample(channel, sample),
                    "Both output reconstructs exactly from masks without changing playing policy");
    require(run(argv[1], paths) != 0 && identical(wav, root.getChildFile("repeat.wav")), "existing outputs preserved");
    for (const auto& invalid : {juce::StringArray{"--sample-rate", "nan"}, juce::StringArray{"--block-size", "0"},
                               juce::StringArray{"--tail-seconds", "-1"}, juce::StringArray{"--output-db", "6.1"},
                               juce::StringArray{"--split-hz", "79"}, juce::StringArray{"--split-hz", "251"},
                               juce::StringArray{"--destruction", "nan"}, juce::StringArray{"--destruction", "1.1"},
                               juce::StringArray{"--rhythm-mono", "1"},
                               juce::StringArray{"--bass-enabled", "-1"}, juce::StringArray{"--bass-enabled", "0.5"},
                               juce::StringArray{"--bass-enabled", "nan"}, juce::StringArray{"--rhythm-enabled", "2"},
                               juce::StringArray{"--rhythm-enabled", "true"},
                               juce::StringArray{"--layer-mask", "solo"}, juce::StringArray{"--bass-component", "invalid"}})
    {
        juce::StringArray args{input.getFullPathName(), root.getChildFile("bad.wav").getFullPathName(),
                              root.getChildFile("bad.json").getFullPathName()};
        args.addArray(invalid);
        require(run(argv[1], args) != 0 && !root.getChildFile("bad.wav").exists(), "invalid options create no audio");
    }
    const auto bad = root.getChildFile("invalid.mid");
    require(bad.replaceWithText("not MIDI"), "invalid input fixture written");
    require(run(argv[1], {bad.getFullPathName(), root.getChildFile("invalid.wav").getFullPathName(),
                         root.getChildFile("invalid.json").getFullPathName()}) != 0, "malformed MIDI rejected");
    for (const int fps : {25, 29})
    {
        const auto stem = "smpte" + juce::String(fps);
        juce::MidiFile timed;
        timed.setSmpteTimeFormat(fps, 40);
        juce::MidiMessageSequence sequence;
        add(sequence, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(127)), 0);
        add(sequence, juce::MidiMessage::noteOff(1, 60), fps * 40);
        timed.addTrack(sequence);
        const auto file = root.getChildFile(stem + ".mid");
        { auto out = file.createOutputStream(); require(out && timed.writeTo(*out, 0), "type 0 SMPTE fixture written"); }
        require(run(argv[1], {file.getFullPathName(), root.getChildFile(stem + ".wav").getFullPathName(),
                             root.getChildFile(stem + ".json").getFullPathName(), "--sample-rate", "192000",
                             "--output-db", "-12"}) == 0, "type 0/SMPTE/high-rate render succeeds");
        const auto result = juce::JSON::parse(root.getChildFile(stem + ".json").loadFileAsString());
        const auto seconds = fps == 29 ? (29.0 * 1001.0 / 30000.0) : 1.0;
        require(static_cast<int>(result["frames"]) == static_cast<int>(std::llround(seconds * 192000)) + 19200,
                "SMPTE and 29.97 fps have correct frame counts");
    }
    const auto asynchronous = root.getChildFile("type2.mid");
    { auto out = asynchronous.createOutputStream(); require(out && midi.writeTo(*out, 2), "type 2 fixture written"); }
    require(run(argv[1], {asynchronous.getFullPathName(), root.getChildFile("type2.wav").getFullPathName(),
                         root.getChildFile("type2.json").getFullPathName()}) != 0, "asynchronous MIDI rejected");
    std::cout << "renderer_repeatability=byte_exact block_sizes=1,73,512 frames=40800 peak="
              << static_cast<double>(metrics["peak"]) << " rms=" << static_cast<double>(metrics["rms"])
              << " fixtures=" << root.getFullPathName() << '\n';
}
