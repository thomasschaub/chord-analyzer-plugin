#pragma once

#include <set>
#include <string>
#include <vector>

namespace chord {

inline const char* pcName(int pc) {
    static const char* names[12] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };
    return names[((pc % 12) + 12) % 12];
}

struct Pattern {
    std::vector<int> intervals;
    const char* suffix;
    int priority;
};

inline const std::vector<Pattern>& patterns() {
    static const std::vector<Pattern> P = {
        // Triads
        {{0, 4, 7},         "",        100},
        {{0, 3, 7},         "m",       100},
        {{0, 3, 6},         "dim",      85},
        {{0, 4, 8},         "aug",      80},
        {{0, 5, 7},         "sus4",     75},
        {{0, 2, 7},         "sus2",     75},

        // Sixths
        {{0, 4, 7, 9},      "6",        80},
        {{0, 3, 7, 9},      "m6",       80},

        // Sevenths
        {{0, 4, 7, 10},     "7",        95},
        {{0, 4, 7, 11},     "maj7",     95},
        {{0, 3, 7, 10},     "m7",       95},
        {{0, 3, 7, 11},     "mMaj7",    75},
        {{0, 3, 6, 9},      "dim7",     80},
        {{0, 3, 6, 10},     "m7b5",     80},
        {{0, 4, 8, 10},     "aug7",     70},

        // Add9
        {{0, 2, 4, 7},      "add9",     80},
        {{0, 2, 3, 7},      "m(add9)",  75},

        // Ninths (4-note extended)
        {{0, 2, 4, 7, 10},  "9",        88},
        {{0, 2, 4, 7, 11},  "maj9",     88},
        {{0, 2, 3, 7, 10},  "m9",       88},

        // Suspended sevenths
        {{0, 5, 7, 10},     "7sus4",    72},
        {{0, 2, 7, 10},     "7sus2",    68},

        // Power chord
        {{0, 7},            "5",        50},
    };
    return P;
}

inline std::string intervalName(int semitones) {
    switch (semitones) {
        case 0:  return "unison";
        case 1:  return "m2";
        case 2:  return "M2";
        case 3:  return "m3";
        case 4:  return "M3";
        case 5:  return "P4";
        case 6:  return "tritone";
        case 7:  return "P5";
        case 8:  return "m6";
        case 9:  return "M6";
        case 10: return "m7";
        case 11: return "M7";
        default: return "?";
    }
}

// midiNotes is a sorted set of MIDI note numbers currently held.
// Returns an empty string when no notes are held.
inline std::string detect(const std::set<int>& midiNotes) {
    if (midiNotes.empty()) return "";

    if (midiNotes.size() == 1) {
        int n = *midiNotes.begin();
        std::string s = pcName(n % 12);
        s += std::to_string((n / 12) - 1);
        return s;
    }

    std::set<int> pcs;
    for (int n : midiNotes) pcs.insert(((n % 12) + 12) % 12);
    int bassPc = ((*midiNotes.begin() % 12) + 12) % 12;

    if (midiNotes.size() == 2 && pcs.size() == 2) {
        auto it = pcs.begin();
        int a = *it++;
        int b = *it;
        int upDist = (b - a + 12) % 12;
        int downDist = (a - b + 12) % 12;
        int lowPc = (upDist <= downDist) ? a : b;
        int gap = (upDist <= downDist) ? upDist : downDist;
        std::string s = pcName(lowPc);
        s += " + ";
        s += intervalName(gap);
        return s;
    }

    struct Match {
        int rootPc = -1;
        const char* suffix = "";
        int score = -1;
    };
    Match best;

    for (int root = 0; root < 12; ++root) {
        if (pcs.find(root) == pcs.end()) continue;

        std::set<int> intervals;
        for (int pc : pcs) intervals.insert((pc - root + 12) % 12);

        for (const auto& pat : patterns()) {
            std::set<int> patSet(pat.intervals.begin(), pat.intervals.end());
            if (intervals != patSet) continue;

            int score = pat.priority;
            if (root == bassPc) score += 15;
            if (score > best.score) {
                best = {root, pat.suffix, score};
            }
        }
    }

    if (best.rootPc < 0) {
        std::string s;
        bool first = true;
        for (int pc : pcs) {
            if (!first) s += " ";
            first = false;
            s += pcName(pc);
        }
        return s;
    }

    std::string result = pcName(best.rootPc);
    result += best.suffix;
    if (bassPc != best.rootPc) {
        result += "/";
        result += pcName(bassPc);
    }
    return result;
}

} // namespace chord
