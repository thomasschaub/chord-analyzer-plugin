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
};

extern "C" {

typedef void (*ChordSnapshotProvider)(void* userData, ChordSnapshot* out);
typedef void (*ScaleChangedCallback)(void* userData, uint8_t root, uint8_t type);

void* chordGuiCreate(uint32_t w, uint32_t h,
                     ChordSnapshotProvider provider,
                     ScaleChangedCallback onScaleChanged,
                     void* userData);
void  chordGuiDestroy(void* view);
bool  chordGuiSetParent(void* view, void* parentNSView);
void  chordGuiGetPreferredSize(uint32_t* w, uint32_t* h);

}
