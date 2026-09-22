# Chord Analyzer

A CLAP plugin that displays the name of the chord formed by the MIDI notes currently
being played. Drop it on a track, route MIDI in, hold `C E G`, see **C**.

- Detects major, minor, diminished, augmented, sus2/sus4, 6, 7, maj7, m7,
  mMaj7, dim7, m7b5, aug7, add9, m(add9), 9, maj9, m9, 7sus4, 7sus2, power (5),
  intervals, and slash chords when the bass note isn't the root.
- macOS only (Cocoa GUI). The plugin core is portable — Linux/Windows builds
  need a native view added.

## Build

Requires CMake 3.20+, a C++17 compiler, and the Xcode command line tools.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build produces `build/ChordAnalyzer.clap` (a macOS bundle).

## Install

Copy the bundle to the standard CLAP location:

```sh
mkdir -p ~/Library/Audio/Plug-Ins/CLAP
cp -R build/ChordAnalyzer.clap ~/Library/Audio/Plug-Ins/CLAP/
```

Then rescan plugins in your DAW (Bitwig, Reaper, Studio One, FL Studio, etc.).

## Use

1. Insert **example → Chord Analyzer** on a MIDI/instrument track.
2. Route MIDI into it (many DAWs let you drop it directly on a MIDI track;
   others require an audio track with a MIDI insert).
3. Play notes — the chord name updates in the plugin window.

Audio passes through unchanged, so the plugin can sit anywhere in a chain.

## Project layout

```
CMakeLists.txt           build script (fetches CLAP SDK)
resources/Info.plist.in  macOS bundle metadata
src/chord_detector.hpp   note-set → chord-name logic
src/plugin.cpp           CLAP plugin implementation
src/gui_macos.mm         Cocoa NSView that draws the chord name
```
