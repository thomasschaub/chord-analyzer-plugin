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
    // Start sending the diatonic triad on scale degree 0-6 (I-VII) of the
    // current scale; releaseChord sends the matching note-offs.
    void (*playChord)(void* userData, int degree);
    void (*releaseChord)(void* userData);
};

void* chordGuiCreate(uint32_t w, uint32_t h, ChordGuiCallbacks callbacks);
void  chordGuiDestroy(void* view);
bool  chordGuiSetParent(void* view, void* parentNSView);
void  chordGuiGetPreferredSize(uint32_t* w, uint32_t* h);

}
