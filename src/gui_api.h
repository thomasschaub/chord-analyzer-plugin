#pragma once

#include <cstddef>
#include <cstdint>

struct ChordSnapshot {
    char    name[64];
    uint8_t notes[32];
    uint8_t noteCount;
    uint8_t isChord;   // 1 when name is a recognized chord, 0 otherwise
    uint8_t scaleRoot; // pitch class 0-11 (0 = C)
    uint8_t scaleType; // index into scale::kTypes, 0 = none

    // Pitch-class histogram of the most recent notes (index 0 = C).
    uint16_t histogram[12];
    uint16_t historyCount;
    uint8_t  detectedRoot;
    uint8_t  detectedType;  // 0 = not enough notes to guess
    uint8_t  detectedEarly; // 1 = tentative guess from few notes
};

extern "C" {

struct ChordGuiCallbacks {
    void* userData;
    void (*getSnapshot)(void* userData, ChordSnapshot* out);
    void (*setScale)(void* userData, uint8_t root, uint8_t type);
    void (*resetHistory)(void* userData);
    // Start sending the chord on scale degree 0-6 (I-VII) of the current
    // scale; variant is a scale::ChordVariant (triad, sus2, sus4, 7, 9).
    // releaseChord sends the matching note-offs.
    void (*playChord)(void* userData, int degree, int variant);
    void (*releaseChord)(void* userData);
};

void* chordGuiCreate(uint32_t w, uint32_t h, ChordGuiCallbacks callbacks);
void  chordGuiDestroy(void* view);
// parent is an NSView* on macOS and an HWND on Windows.
bool  chordGuiSetParent(void* view, void* parent);
void  chordGuiGetPreferredSize(uint32_t* w, uint32_t* h);

#if defined(_WIN32)
// Windows hosts size the editor in physical pixels: the view is w x h
// logical units drawn at `scale` pixels per unit.
void   chordGuiSetScale(void* view, double scale);
// Scale of the primary monitor, for hosts that never call set_scale.
double chordGuiSystemScale();
#endif

}
