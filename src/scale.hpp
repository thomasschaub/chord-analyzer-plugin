#pragma once

#include <cstdint>
#include <initializer_list>
#include <utility>

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

struct Detection {
    int  root = 0;
    int  type = 0;        // 0 = not enough data
    bool early = false;   // guessed from half steps before the full threshold
};

inline constexpr int kTypeMajor = 1;
inline constexpr int kTypeMinor = 2;
static_assert(kTypes[kTypeMajor].mask == intervals({0, 2, 4, 5, 7, 9, 11}), "kTypeMajor");
static_assert(kTypes[kTypeMinor].mask == intervals({0, 2, 3, 5, 7, 8, 10}), "kTypeMinor");

// Minimum history before the full scorer runs.
inline constexpr int kMinDetectNotes    = 8;
inline constexpr int kMinDetectDistinct = 4;

// How well a scale explains the note shares f[pc] (see detect()).
inline double fitScore(const uint16_t counts[12], const double f[12], int root, int type) {
    uint16_t mask = pitchClassMask(root, type);
    double in = 0, out = 0;
    int unused = 0;
    for (int pc = 0; pc < 12; ++pc) {
        bool member = (mask >> pc) & 1;
        if (member) {
            in += f[pc];
            if (counts[pc] == 0) unused++;
        } else {
            out += f[pc];
        }
    }
    return in - 2.0 * out - 0.04 * unused
         + 0.3 * f[root] + 0.1 * f[(root + 7) % 12];
}

// Early guess assuming plain major/minor. A major scale has exactly two
// half steps, a fourth apart (C major: B-C and E-F). If the played pitch
// classes contain exactly two half steps in that arrangement, the notes
// fit exactly one major/relative-minor pair: any foreign note would add
// another half step. Root/fifth emphasis then picks major vs. minor.
inline Detection detectFromHalfSteps(const uint16_t counts[12], const double f[12]) {
    Detection d;
    int lower[12];
    int pairs = 0;
    for (int pc = 0; pc < 12; ++pc) {
        if (counts[pc] > 0 && counts[(pc + 1) % 12] > 0) lower[pairs++] = pc;
    }
    if (pairs != 2) return d;

    // Order so that the second half step lies a fourth above the first.
    int a = lower[0], b = lower[1];
    if ((b - a + 12) % 12 != 5) std::swap(a, b);
    if ((b - a + 12) % 12 != 5) return d;

    // With a = B (11) and b = E (4): major tonic is C, relative minor A.
    int majorRoot = (a + 1) % 12;
    int minorRoot = (majorRoot + 9) % 12;
    double majorScore = fitScore(counts, f, majorRoot, kTypeMajor);
    double minorScore = fitScore(counts, f, minorRoot, kTypeMinor);
    if (minorScore > majorScore + 1e-9) {
        d.root = minorRoot;
        d.type = kTypeMinor;
    } else {
        d.root = majorRoot;
        d.type = kTypeMajor;
    }
    d.early = true;
    return d;
}

// Picks the scale that best explains a pitch-class histogram (index 0 = C).
//
// Below kMinDetectNotes, only the half-step early guess is attempted.
// Otherwise every root/type is scored, with f[pc] = share of notes on
// that pitch class:
//   + share of notes inside the scale
//   - 2 x share of notes outside it (wrong notes weigh heavily)
//   - 0.04 per scale degree never played (prefers the tighter scale,
//     e.g. pentatonic when only its five notes occur)
//   + 0.3 x f[root] + 0.1 x f[fifth] (tonic emphasis separates relative
//     keys such as C major / A minor, which share the same notes)
// Ties keep the earlier entry in kTypes, so Major/Minor win over modes.
inline Detection detect(const uint16_t counts[12]) {
    Detection best;
    int total = 0, distinct = 0;
    for (int pc = 0; pc < 12; ++pc) {
        total += counts[pc];
        if (counts[pc] > 0) distinct++;
    }
    if (total == 0) return best;

    double f[12];
    for (int pc = 0; pc < 12; ++pc) f[pc] = static_cast<double>(counts[pc]) / total;

    if (total < kMinDetectNotes || distinct < kMinDetectDistinct) {
        return detectFromHalfSteps(counts, f);
    }

    double bestScore = -1e9;
    for (int type = 1; type < kTypeCount; ++type) {
        for (int root = 0; root < 12; ++root) {
            double score = fitScore(counts, f, root, type);
            if (score > bestScore + 1e-9) {
                bestScore = score;
                best.root = root;
                best.type = type;
            }
        }
    }
    return best;
}

} // namespace scale
