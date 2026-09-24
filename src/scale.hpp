#pragma once

#include <cstdint>
#include <initializer_list>

namespace scale {

// Bit i set = the note i semitones above the root is in the scale.
constexpr uint16_t intervals(std::initializer_list<int> steps) {
    uint16_t m = 0;
    for (int s : steps) m = static_cast<uint16_t>(m | (1u << s));
    return m;
}

struct Type {
    const char* name;
    uint16_t    mask;
};

// Index 0 means "no scale selected". Order is persisted in plugin state,
// so only append new entries.
inline constexpr Type kTypes[] = {
    {"None",             0},
    {"Major",            intervals({0, 2, 4, 5, 7, 9, 11})},
    {"Minor",            intervals({0, 2, 3, 5, 7, 8, 10})},
    {"Harmonic Minor",   intervals({0, 2, 3, 5, 7, 8, 11})},
    {"Melodic Minor",    intervals({0, 2, 3, 5, 7, 9, 11})},
    {"Dorian",           intervals({0, 2, 3, 5, 7, 9, 10})},
    {"Phrygian",         intervals({0, 1, 3, 5, 7, 8, 10})},
    {"Lydian",           intervals({0, 2, 4, 6, 7, 9, 11})},
    {"Mixolydian",       intervals({0, 2, 4, 5, 7, 9, 10})},
    {"Locrian",          intervals({0, 1, 3, 5, 6, 8, 10})},
    {"Major Pentatonic", intervals({0, 2, 4, 7, 9})},
    {"Minor Pentatonic", intervals({0, 3, 5, 7, 10})},
    {"Blues",            intervals({0, 3, 5, 6, 7, 10})},
};

inline constexpr int kTypeCount = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));

inline constexpr const char* kRootNames[12] = {
    "C", "C♯/D♭", "D", "E♭", "E", "F", "F♯/G♭", "G", "A♭", "A", "B♭", "B",
};

// Mask over absolute pitch classes (bit 0 = C) for the given root and type.
inline uint16_t pitchClassMask(int root, int type) {
    if (type <= 0 || type >= kTypeCount || root < 0 || root > 11) return 0;
    uint32_t m = kTypes[type].mask;
    uint32_t rotated = (m << root) | (m >> (12 - root));
    return static_cast<uint16_t>(rotated & 0xFFF);
}

} // namespace scale
