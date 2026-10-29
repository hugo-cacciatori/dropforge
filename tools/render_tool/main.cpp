#include "plugin/InstrumentCore.h"
#include <dsp_core/LoudnessMeter.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
struct Options
{
    juce::File input, wav, metrics;
    int rate = 48000, block = 512;
    double tail = 0.1;
    float outputDb = 0.0f, anvilLevelDb = 0.0f;
    float splitHz = 140.0f, drive = 0.5f;
    bool bassEnabled = true, rhythmEnabled = true;
    dropforge::LayerMask mask = dropforge::LayerMask::both;
    std::string maskName = "both";
    dropforge::BassComponent bassComponent = dropforge::BassComponent::all;
    std::string componentName = "all";
    std::string furnace = "a", anvil = "a";
    bool furnaceMono = false, furnaceDrop = false, anvilDestroy = false, furnaceDestroy = false;
};

double number(const char* text)
{
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> std::noskipws >> value;
    if (!stream || stream.peek() != std::char_traits<char>::eof() || !std::isfinite(value))
        throw std::runtime_error("Expected a finite number");
    return value;
}

int integer(const char* text, int minimum, int maximum)
{
    const auto value = number(text);
    if (value < minimum || value > maximum || std::floor(value) != value)
        throw std::runtime_error("Integer option outside its supported range");
    return static_cast<int>(value);
}

Options parse(int argc, char** argv)
{
    if (argc < 4 || (argc - 4) % 2 != 0)
        throw std::runtime_error("Expected MIDI, WAV and JSON paths followed by option/value pairs");
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    Options options{cwd.getChildFile(argv[1]), cwd.getChildFile(argv[2]), cwd.getChildFile(argv[3])};
    for (int i = 4; i < argc; i += 2)
    {
        const std::string flag(argv[i]);
        if (flag == "--sample-rate") options.rate = integer(argv[i + 1], 44100, 192000);
        else if (flag == "--bass-enabled") options.bassEnabled = integer(argv[i + 1], 0, 1) != 0;
        else if (flag == "--rhythm-enabled") options.rhythmEnabled = integer(argv[i + 1], 0, 1) != 0;
        else if (flag == "--block-size") options.block = integer(argv[i + 1], 1, 65536);
        else if (flag == "--tail-seconds")
        {
            options.tail = number(argv[i + 1]);
            if (options.tail < 0.01 || options.tail > 60.0)
                throw std::runtime_error("Tail must be 0.01 to 60 seconds");
        }
        else if (flag == "--output-db")
        {
            const auto db = number(argv[i + 1]);
            if (db < -24.0 || db > 6.0) throw std::runtime_error("Output must be -24 to +6 dB");
            options.outputDb = static_cast<float>(db);
        }
        else if (flag == "--furnace-mono") options.furnaceMono = integer(argv[i + 1], 0, 1) != 0;
        else if (flag == "--furnace-drop") options.furnaceDrop = integer(argv[i + 1], 0, 1) != 0;
        else if (flag == "--anvil-destroy") options.anvilDestroy = integer(argv[i + 1], 0, 1) != 0;
        else if (flag == "--furnace-destroy") options.furnaceDestroy = integer(argv[i + 1], 0, 1) != 0;
        else if (flag == "--anvil-level-db")
        {
            const auto db = number(argv[i + 1]);
            if (db < -60.0 || db > 6.0) throw std::runtime_error("Anvil Level must be -60 to +6 dB");
            options.anvilLevelDb = static_cast<float>(db);
        }
        else if (flag == "--split-hz" || flag == "--drive")
        {
            const auto value = number(argv[i + 1]);
            if (flag == "--split-hz")
            {
                if (value < 80.0 || value > 250.0) throw std::runtime_error("Split must be 80 to 250 Hz");
                options.splitHz = static_cast<float>(value);
            }
            else
            {
                if (value < 0.0 || value > 1.0) throw std::runtime_error("Drive must be 0 to 1");
                options.drive = static_cast<float>(value);
            }
        }
        else if (flag == "--bass-component")
        {
            options.componentName = argv[i + 1];
            if (options.componentName == "all") options.bassComponent = dropforge::BassComponent::all;
            else if (options.componentName == "clean") options.bassComponent = dropforge::BassComponent::clean;
            else if (options.componentName == "grit") options.bassComponent = dropforge::BassComponent::grit;
            else if (options.componentName == "dry-grit") options.bassComponent = dropforge::BassComponent::dryGrit;
            else if (options.componentName == "unison-grit") options.bassComponent = dropforge::BassComponent::unisonGrit;
            else throw std::runtime_error("Bass component must be all, clean, grit, dry-grit or unison-grit");
        }
        else if (flag == "--furnace")
        {
            options.furnace = argv[i + 1];
            if (options.furnace.size() != 1 || options.furnace[0] < 'a' || options.furnace[0] > 'd')
                throw std::runtime_error("Furnace mode must be a, b, c or d");
        }
        else if (flag == "--anvil")
        {
            options.anvil = argv[i + 1];
            if (options.anvil.size() != 1 || options.anvil[0] < 'a' || options.anvil[0] > 'd')
                throw std::runtime_error("Anvil mode must be a, b, c or d");
        }
        else if (flag == "--layer-mask")
        {
            options.maskName = argv[i + 1];
            if (options.maskName == "both") options.mask = dropforge::LayerMask::both;
            else if (options.maskName == "bass") options.mask = dropforge::LayerMask::bass;
            else if (options.maskName == "rhythm") options.mask = dropforge::LayerMask::rhythm;
            else if (options.maskName == "none") options.mask = dropforge::LayerMask::none;
            else throw std::runtime_error("Layer mask must be both, bass, rhythm or none");
        }
        else throw std::runtime_error("Unknown option: " + flag);
    }
    // Never truncate an input or an existing render, including through a symlink.
    if (options.wav == options.metrics || options.wav.exists() || options.metrics.exists()
        || !options.wav.getParentDirectory().isDirectory() || !options.metrics.getParentDirectory().isDirectory())
        throw std::runtime_error("Output paths must be distinct new files in existing directories");
    return options;
}

struct Event
{
    juce::int64 frame;
    juce::MidiMessage message;
};

void render(const Options& options)
{
    auto input = options.input.createInputStream();
    juce::MidiFile midi;
    int type = -1;
    // Do not let the reader manufacture note-offs: preserve the actual MIDI.
    if (!input || !midi.readFrom(*input, false, &type) || type < 0 || type > 1 || midi.getTimeFormat() == 0)
        throw std::runtime_error("Expected a readable standard MIDI type 0 or 1 file with valid timing");
    const auto timing = midi.getTimeFormat();
    if (timing > 0)
        midi.convertTimestampTicksToSeconds();
    else
    {
        const auto fps = -(timing >> 8), ticksPerFrame = timing & 255;
        if ((fps != 24 && fps != 25 && fps != 29 && fps != 30) || ticksPerFrame == 0)
            throw std::runtime_error("Invalid SMPTE MIDI timing");
        // MIDI's -29 code denotes 29.97 fps, rather than exactly 29 fps.
        const auto ticksPerSecond = (fps == 29 ? 30000.0 / 1001.0 : fps) * ticksPerFrame;
        for (int track = 0; track < midi.getNumTracks(); ++track)
            for (auto* event : *midi.getTrack(track))
                event->message.setTimeStamp(event->message.getTimeStamp() / ticksPerSecond);
    }
    const auto duration = midi.getLastTimestamp();
    // Bound memory/disk use and keep well below RIFF's 4 GiB size limit at 192 kHz.
    if (!std::isfinite(duration) || duration < 0.0 || duration + options.tail > 600.0)
        throw std::runtime_error("MIDI plus tail must be at most 600 seconds");
    const auto endFrame = static_cast<juce::int64>(std::llround(duration * options.rate));
    const auto frames = endFrame + static_cast<juce::int64>(std::ceil(options.tail * options.rate));
    std::vector<Event> events;
    for (int track = 0; track < midi.getNumTracks(); ++track)
        for (const auto* event : *midi.getTrack(track))
        {
            const auto& message = event->message;
            const auto time = message.getTimeStamp();
            if (!std::isfinite(time) || time < 0.0 || time > duration)
                throw std::runtime_error("Invalid MIDI event time");
            // Meta 0xff is a file event, not the live MIDI system-reset byte.
            if (!message.isMetaEvent())
                events.push_back({static_cast<juce::int64>(std::llround(time * options.rate)), message});
        }
    const auto midiEventCount = static_cast<juce::int64>(events.size());
    // Release hanging keys after the file's last event, without truncating release.
    for (int channel = 1; channel <= 16; ++channel)
        events.push_back({endFrame, juce::MidiMessage::allNotesOff(channel)});
    // Preserve file order within a track, then track-index order on equal frames.
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });

    std::unique_ptr<juce::OutputStream> stream = options.wav.createOutputStream();
    if (!stream) throw std::runtime_error("Cannot open WAV output");
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
        .withSampleRate(options.rate).withNumChannels(2).withBitsPerSample(32)
        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (!writer) throw std::runtime_error("Cannot create WAV output");
    dropforge::InstrumentCore core;
    core.setLayersEnabled(options.bassEnabled, options.rhythmEnabled);
    core.setModes(options.anvil[0] - 'a', options.furnace[0] - 'a');
    core.setFurnaceOptions(options.furnaceMono, options.furnaceDrop);
    core.setDestruction(options.anvilDestroy, options.furnaceDestroy);
    core.setLayerLevels(options.anvilLevelDb, 0.0f);
    core.prepare(options.rate, options.outputDb, options.splitHz, options.drive);
    juce::AudioBuffer<float> audio(2, options.block);
    juce::ScopedNoDenormals noDenormals;
    std::size_t nextEvent = 0;
    double peak = 0.0, sumSquares = 0.0, sideSquares = 0.0;
    juce::int64 clipped = 0, lastNonzero = -1;
    auto loudness = std::make_unique<dsp_core::LoudnessMeter>();
    loudness->prepare(options.rate);
    for (juce::int64 position = 0; position < frames;)
    {
        const auto count = static_cast<int>(std::min<juce::int64>(options.block, frames - position));
        int cursor = 0;
        while (nextEvent < events.size() && events[nextEvent].frame < position + count)
        {
            const auto& event = events[nextEvent++];
            const auto offset = static_cast<int>(event.frame - position);
            core.process(audio.getWritePointer(0, cursor), audio.getWritePointer(1, cursor), offset - cursor,
                         options.mask, {}, options.bassComponent);
            core.midi(event.message.getRawData(), event.message.getRawDataSize());
            cursor = offset;
        }
        core.process(audio.getWritePointer(0, cursor), audio.getWritePointer(1, cursor), count - cursor,
                     options.mask, {}, options.bassComponent);
        for (int sample = 0; sample < count; ++sample)
        {
            const double left = audio.getSample(0, sample), right = audio.getSample(1, sample);
            if (!std::isfinite(left) || !std::isfinite(right)) throw std::runtime_error("Non-finite audio");
            peak = std::max({peak, std::abs(left), std::abs(right)});
            loudness->process(static_cast<float>(left), static_cast<float>(right));
            sumSquares += left * left + right * right;
            sideSquares += (left - right) * (left - right);
            clipped += (std::abs(left) >= 1.0 ? 1 : 0) + (std::abs(right) >= 1.0 ? 1 : 0);
            if (left != 0.0 || right != 0.0) lastNonzero = position + sample;
        }
        if (!writer->writeFromAudioSampleBuffer(audio, 0, count)) throw std::runtime_error("WAV write failed");
        position += count;
    }
    if (!writer->flush()) throw std::runtime_error("WAV flush failed");
    writer.reset();
    juce::DynamicObject::Ptr metrics = new juce::DynamicObject;
    metrics->setProperty("schema_version", 1);
    metrics->setProperty("product_version", DROPFORGE_VERSION);
    metrics->setProperty("source_commit", DROPFORGE_COMMIT);
    metrics->setProperty("source_dirty", DROPFORGE_DIRTY);
    metrics->setProperty("sample_rate", options.rate);
    metrics->setProperty("block_size", options.block);
    metrics->setProperty("output_db", options.outputDb);
    metrics->setProperty("layer_mask", juce::String(options.maskName));
    metrics->setProperty("furnace", juce::String(options.furnace));
    metrics->setProperty("anvil", juce::String(options.anvil));
    metrics->setProperty("furnace_mono", options.furnaceMono);
    metrics->setProperty("furnace_drop", options.furnaceDrop);
    metrics->setProperty("anvil_destroy", options.anvilDestroy);
    metrics->setProperty("furnace_destroy", options.furnaceDestroy);
    metrics->setProperty("anvil_level_db", options.anvilLevelDb);
    metrics->setProperty("bass_component", juce::String(options.componentName));
    metrics->setProperty("split_hz", options.splitHz);
    metrics->setProperty("drive", options.drive);
    metrics->setProperty("bass_enabled", options.bassEnabled);
    metrics->setProperty("rhythm_enabled", options.rhythmEnabled);
    metrics->setProperty("latency_samples", dropforge::InstrumentCore::latencySamples);
    metrics->setProperty("tail_seconds", options.tail);
    metrics->setProperty("frames", frames);
    metrics->setProperty("midi_events", midiEventCount);
    metrics->setProperty("peak", peak);
    // BS.1770 loudness; -1000 when the file is shorter than the window.
    metrics->setProperty("integrated_lufs", loudness->integratedLufs());
    metrics->setProperty("short_term_max_lufs", loudness->shortTermMaxLufs());
    metrics->setProperty("momentary_max_lufs", loudness->momentaryMaxLufs());
    metrics->setProperty("rms", std::sqrt(sumSquares / (2.0 * static_cast<double>(frames))));
    metrics->setProperty("left_minus_right_rms", std::sqrt(sideSquares / static_cast<double>(frames)));
    metrics->setProperty("clipped_samples", clipped);
    metrics->setProperty("nonfinite_samples", 0);
    metrics->setProperty("last_nonzero_frame", lastNonzero);
    const auto json = juce::JSON::toString(juce::var(metrics.get()), true) + "\n";
    auto output = options.metrics.createOutputStream();
    if (!output || !output->writeText(json, false, false, nullptr)) throw std::runtime_error("Metrics write failed");
    output->flush();
    if (output->getStatus().failed()) throw std::runtime_error("Metrics flush failed");
    std::cout << json;
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help")
    {
        std::cout << "render_tool input.mid output.wav metrics.json [--sample-rate 48000] [--block-size 512]\n"
                     "  [--output-db 0] [--tail-seconds 0.1] [--split-hz 140] [--drive 0.5]\n"
                     "  [--layer-mask both|bass|rhythm|none] (diagnostic output mask, not a playing mode)\n"
                     "  [--bass-component all|clean|grit|dry-grit|unison-grit] (diagnostic isolation, same final filter/envelope)\n"
                     "  [--bass-enabled 0|1] [--rhythm-enabled 0|1] (default 1/1; selects Both/Bass/Rhythm/Off routing)\n"
                     "  [--furnace a|b|c|d] (Furnace mode; default a)\n"
                     "  [--anvil a|b|c|d] (Anvil mode; default a)\n"
                     "  [--furnace-mono 0|1] [--furnace-drop 0|1] (Furnace options; default 0)\n"
                     "  [--anvil-destroy 0|1] [--furnace-destroy 0|1] (Destruction; default 0)\n"
                     "  [--anvil-level-db 0] (Anvil Level, -60 to +6 dB, as the plugin parameter)\n"
                     "MIDI type 0/1; stereo float32 WAV; new output files only; maximum 600 seconds.\n";
        return 0;
    }
    try { render(parse(argc, argv)); }
    catch (const std::exception& error)
    {
        std::cerr << "render_tool: " << error.what() << "\nUse --help for usage.\n";
        return 1;
    }
    return 0;
}
