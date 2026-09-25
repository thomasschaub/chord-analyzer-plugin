#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
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

// Compact names for tight spaces (histogram labels, chord names).
inline constexpr const char* kShortNames[12] = {
    "C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B",
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

// ------------------------------------------------------------
// Diatonic triads
// ------------------------------------------------------------

enum class TriadQuality { Major, Minor, Diminished, Augmented, Other };

struct Triad {
    int offsets[3];       // semitones above the scale root, ascending
    int rootPc;           // absolute pitch class of the chord root
    TriadQuality quality;
};

// Semitones above the root for each note of a seven-note scale.
// Returns false for scales with a different note count.
inline bool sevenNoteSteps(int type, int steps[7]) {
    if (type <= 0 || type >= kTypeCount) return false;
    int n = 0;
    for (int i = 0; i < 12; ++i) {
        if ((kTypes[type].mask >> i) & 1) {
            if (n == 7) return false;
            steps[n++] = i;
        }
    }
    return n == 7;
}

// Offset of scale note `idx` (may run past the octave) above the root.
inline int stepOffset(const int steps[7], int idx) {
    return steps[idx % 7] + 12 * (idx / 7);
}

// Builds the triad on scale degree 0-6 (I-VII) by stacking every other
// scale note. Only defined for seven-note scales; returns false otherwise.
inline bool diatonicTriad(int root, int type, int degree, Triad* out) {
    if (root < 0 || root > 11 || degree < 0 || degree > 6) return false;
    int steps[7];
    if (!sevenNoteSteps(type, steps)) return false;

    for (int k = 0; k < 3; ++k) out->offsets[k] = stepOffset(steps, degree + 2 * k);
    out->rootPc = (root + out->offsets[0]) % 12;

    int third = out->offsets[1] - out->offsets[0];
    int fifth = out->offsets[2] - out->offsets[0];
    if      (third == 4 && fifth == 7) out->quality = TriadQuality::Major;
    else if (third == 3 && fifth == 7) out->quality = TriadQuality::Minor;
    else if (third == 3 && fifth == 6) out->quality = TriadQuality::Diminished;
    else if (third == 4 && fifth == 8) out->quality = TriadQuality::Augmented;
    else                               out->quality = TriadQuality::Other;
    return true;
}

// Roman numeral: upper case for major/augmented, lower case for
// minor/diminished, e.g. "V", "ii", "vii°", "III+".
inline std::string romanNumeral(int degree, TriadQuality q) {
    static const char* const upper[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    static const char* const lower[7] = {"i", "ii", "iii", "iv", "v", "vi", "vii"};
    bool minorish = q == TriadQuality::Minor || q == TriadQuality::Diminished;
    std::string s = minorish ? lower[degree] : upper[degree];
    if (q == TriadQuality::Diminished) s += "°";
    if (q == TriadQuality::Augmented)  s += "+";
    return s;
}

// ------------------------------------------------------------
// Pad chords: triad plus sus2 / sus4 / 7 / 9 variants
// ------------------------------------------------------------

// Order matches the GUI and the playChord callback.
enum class ChordVariant { Triad, Sus2, Sus4, Seventh, Ninth };
inline constexpr int kVariantCount = 5;
inline constexpr int kMaxChordNotes = 5;

// Pad chords are voiced upward from the scale root placed in C3..B3.
inline constexpr int kChordBaseKey = 48;
inline constexpr int kChordVelocity = 100; // out of 127

struct Chord {
    int offsets[kMaxChordNotes]; // semitones above the scale root, ascending
    int count;
    int rootPc;                  // absolute pitch class of the chord root
    std::string name;            // display symbol, e.g. "B♭maj7", "F♯m7♭5"
    std::string asciiName;       // file/clip-safe, e.g. "Bbmaj7", "F#m7b5"
};

namespace detail {

struct Suffix {
    const char* display;
    const char* ascii;
};

inline Suffix triadSuffix(int third, int fifth) {
    if (third == 4 && fifth == 7) return {"", ""};
    if (third == 3 && fifth == 7) return {"m", "m"};
    if (third == 3 && fifth == 6) return {"°", "dim"};
    if (third == 4 && fifth == 8) return {"+", "aug"};
    return {"?", "x"};
}

struct SeventhSuffix {
    int third, fifth, seventh;
    Suffix seventhName;
    Suffix ninthName;   // used when the ninth is major
};

// Every seventh chord the seven-note scales in kTypes can produce.
inline constexpr SeventhSuffix kSevenths[] = {
    {4, 7, 11, {"maj7",    "maj7"},    {"maj9",    "maj9"}},
    {3, 7, 10, {"m7",      "m7"},      {"m9",      "m9"}},
    {4, 7, 10, {"7",       "7"},       {"9",       "9"}},
    {3, 6, 10, {"m7♭5",    "m7b5"},    {"m9♭5",    "m9b5"}},
    {3, 6,  9, {"°7",      "dim7"},    {"°7(9)",   "dim7(9)"}},
    {3, 7, 11, {"m(maj7)", "m(maj7)"}, {"m(maj9)", "m(maj9)"}},
    {4, 8, 11, {"maj7♯5",  "maj7#5"},  {"maj9♯5",  "maj9#5"}},
    {4, 8, 10, {"7♯5",     "7#5"},     {"9♯5",     "9#5"}},
    {4, 6, 10, {"7♭5",     "7b5"},     {"9♭5",     "9b5"}},
};

inline const char* const kAsciiNames[12] = {
    "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
};

} // namespace detail

// Builds a pad chord on scale degree 0-6 from scale notes only:
//   Triad    1-3-5
//   Sus2     1-2-5, only if that is a real sus2 (major 2nd, perfect 5th)
//   Sus4     1-4-5, only if that is a real sus4 (perfect 4th and 5th)
//   Seventh  1-3-5-7
//   Ninth    1-3-5-7-9
// Returns false for scales without seven notes and for sus chords the
// scale can't form on that degree (e.g. E sus2 or F sus4 in C major).
inline bool diatonicChord(int root, int type, int degree, ChordVariant variant, Chord* out) {
    if (root < 0 || root > 11 || degree < 0 || degree > 6) return false;
    int steps[7];
    if (!sevenNoteSteps(type, steps)) return false;

    // Scale-note indices (relative to the chord root) for each variant.
    static const int kTriad[]   = {0, 2, 4};
    static const int kSus2[]    = {0, 1, 4};
    static const int kSus4[]    = {0, 3, 4};
    static const int kSeventh[] = {0, 2, 4, 6};
    static const int kNinth[]   = {0, 2, 4, 6, 8};
    const int* idx = kTriad;
    int count = 3;
    switch (variant) {
        case ChordVariant::Triad:   idx = kTriad;   count = 3; break;
        case ChordVariant::Sus2:    idx = kSus2;    count = 3; break;
        case ChordVariant::Sus4:    idx = kSus4;    count = 3; break;
        case ChordVariant::Seventh: idx = kSeventh; count = 4; break;
        case ChordVariant::Ninth:   idx = kNinth;   count = 5; break;
    }

    for (int k = 0; k < count; ++k) out->offsets[k] = stepOffset(steps, degree + idx[k]);
    out->count = count;
    out->rootPc = (root + out->offsets[0]) % 12;

    // Intervals above the chord root.
    int iv[kMaxChordNotes];
    for (int k = 0; k < count; ++k) iv[k] = out->offsets[k] - out->offsets[0];

    detail::Suffix suffix{"", ""};
    std::string extraDisplay, extraAscii;
    switch (variant) {
        case ChordVariant::Triad:
            suffix = detail::triadSuffix(iv[1], iv[2]);
            break;
        case ChordVariant::Sus2:
            if (iv[1] != 2 || iv[2] != 7) return false;
            suffix = {"sus2", "sus2"};
            break;
        case ChordVariant::Sus4:
            if (iv[1] != 5 || iv[2] != 7) return false;
            suffix = {"sus4", "sus4"};
            break;
        case ChordVariant::Seventh:
        case ChordVariant::Ninth: {
            const detail::SeventhSuffix* match = nullptr;
            for (const auto& e : detail::kSevenths) {
                if (e.third == iv[1] && e.fifth == iv[2] && e.seventh == iv[3]) match = &e;
            }
            if (!match) return false;
            if (variant == ChordVariant::Seventh) {
                suffix = match->seventhName;
            } else {
                int ninth = iv[4] - 12;
                if (ninth == 2) {
                    suffix = match->ninthName;
                } else if (ninth == 1 || ninth == 3) {
                    suffix = match->seventhName;
                    extraDisplay = ninth == 1 ? "♭9" : "♯9";
                    extraAscii   = ninth == 1 ? "b9" : "#9";
                } else {
                    return false;
                }
            }
            break;
        }
    }

    out->name      = std::string(kShortNames[out->rootPc]) + suffix.display + extraDisplay;
    out->asciiName = std::string(detail::kAsciiNames[out->rootPc]) + suffix.ascii + extraAscii;
    return true;
}

// MIDI keys for a pad chord of the scale rooted at `root`.
inline void chordKeys(int root, const Chord& c, int keys[kMaxChordNotes]) {
    for (int k = 0; k < c.count; ++k) keys[k] = kChordBaseKey + root + c.offsets[k];
}

} // namespace scale
