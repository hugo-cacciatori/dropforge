# Dropforge

A heavy synth instrument for **metal breakdowns** and **bass-music drops** (dubstep, glitch hop, future bass) and anything in between.

Play chords with one hand and Dropforge turns them into a full part:

| Layer | What it plays | Sound |
|---|---|---|
| **Anvil** | The lowest note you hold, one octave down | A clean, round sub-bass with a growl on top, perfectly centred |
| **Furnace** | Every note you hold | The big, wide, aggressive chord wall: the part rhythm guitars play in a metal mix |
| **Blade** *(coming)* | The highest note you hold | A cutting mono lead that doubles your top note |

The rhythm comes from your MIDI: notes hit instantly, hold while the key is down and stop dead when you let go, so chugs, stabs and chops are as tight as you play them. Velocity sets loudness. There are no envelopes to set up and nothing to configure before it sounds good.

> **Status: early development, no release yet.** The current build is the starting point carried over from an earlier prototype. The new sound engine (Anvil and Furnace), the full one-screen interface, the Blade lead, performance controls (Impact, Glide, mod-wheel Roar and Vibrato) and the Space effects are being built in that order. Releases will appear on the [Releases page](https://github.com/hugo-cacciatori/dropforge/releases).

## What you need

- **Windows 10 or 11, 64-bit.**
- A host that loads **VST3 instruments**. Dropforge is developed and tested in **[REAPER](https://www.reaper.fm/)**. Other VST3 hosts (Ableton Live, Bitwig Studio, Cubase, Studio One, FL Studio…) should work but are **not tested yet**; FL Studio in particular is unverified.
- Or no host at all: the **Standalone** app runs on its own with a MIDI keyboard.
- macOS and Linux builds are not available yet.

Dropforge runs completely offline: no account, no licence server, no internet connection, no telemetry.

## Install (once a release is published)

1. Download `Dropforge-<version>-windows-x64.zip` from the [Releases page](https://github.com/hugo-cacciatori/dropforge/releases) and check it against `SHA256SUMS.txt` if you like.
2. Unzip it.
3. Copy the `Dropforge.vst3` folder into `C:\Program Files\Common Files\VST3\` (Windows asks for administrator permission).
4. Optional: put `Dropforge.exe` anywhere you like to use the Standalone app.

The binaries are not code-signed, so Windows SmartScreen may warn the first time you run the Standalone app; choose *More info → Run anyway* if you trust the download.

## First steps in REAPER

1. Open REAPER and let it scan for new plugins (*Options → Preferences → Plug-ins → VST → Re-scan* if Dropforge does not appear).
2. Insert a new track, click **FX**, search for **Dropforge** and add it as the track's instrument.
3. Arm the track for recording with your MIDI keyboard as input and enable monitoring, or draw MIDI notes in a MIDI item.
4. Play: low power chords for breakdowns, wider chords for drops. Turn layers on or off with their power switches and set their levels; **Output** sets the overall volume.

## Build from source

You need, all free:

- **Visual Studio 2022 Community** with *Desktop development with C++* (MSVC 14.44, Windows SDK 10.0.26100). `tools/windows.vsconfig` lists the exact components.
- **CMake 4.4.4** and **pluginval 1.0.4**: `tools/setup-windows.ps1` downloads both into the ignored `build/tools/` folder and checks their SHA-256.
- The **JUCE 9.0.4** framework is fetched automatically by CMake at a pinned commit and checked by SHA-256.

```powershell
powershell -NoProfile -File tools/setup-windows.ps1
powershell -NoProfile -File tools/build.ps1          # Release build + tests
powershell -NoProfile -File tools/check-plugin.ps1   # pluginval strictness 5
```

The VST3 and the Standalone app end up in `build/release-*/Dropforge_artefacts/Release/`. All automated tests run headless; no test window ever opens. `tools/storage.ps1` reports disk use and `-Apply` removes regenerable build output.

Project layout: `shared/dsp-core` (reusable DSP building blocks), `src/dsp` (the instrument's sound engine), `src/plugin` (plugin, parameters, MIDI), `src/ui` (interface), `tools/render_tool` (offline MIDI-to-WAV renderer), `tests` (CTest suites).

## Third-party software

Dropforge is built with [JUCE](https://juce.com/) (used under its free Starter licence) and the Steinberg VST3 SDK bundled with it (MIT). Their notices, and those of the libraries JUCE compiles in, are in [`packaging/licenses`](packaging/licenses) and ship inside every release. VST is a registered trademark of Steinberg Media Technologies GmbH. REAPER is a trademark of Cockos Incorporated. Dropforge is not affiliated with or endorsed by either.

## Licence

You may use the official Dropforge binaries free of charge for personal and commercial music, with no restrictions on what you make with them. The source code is public to read but is **not open source**: see [LICENSE.txt](LICENSE.txt).
