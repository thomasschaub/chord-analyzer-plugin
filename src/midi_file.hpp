#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace midifile {

inline constexpr int kTicksPerQuarter = 480;
inline constexpr int kTicksPerBar     = kTicksPerQuarter * 4; // 4/4

inline void putVarLen(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t buf[4];
    int n = 0;
    buf[n++] = v & 0x7F;
    while (v >>= 7) buf[n++] = static_cast<uint8_t>((v & 0x7F) | 0x80);
    while (n > 0) out.push_back(buf[--n]);
}

inline void putBE(std::vector<uint8_t>& out, uint32_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// Standard MIDI File (format 0, one track) holding a single block chord:
// all keys start at tick 0 and end together after `lengthTicks`.
// No tempo or time signature is written, so hosts keep their own.
inline std::vector<uint8_t> chordFile(const int* keys, int count, uint8_t velocity,
                                      const std::string& trackName,
                                      uint32_t lengthTicks = kTicksPerBar) {
    std::vector<uint8_t> track;

    track.push_back(0x00);                       // delta
    track.push_back(0xFF);                       // meta: track name
    track.push_back(0x03);
    putVarLen(track, static_cast<uint32_t>(trackName.size()));
    track.insert(track.end(), trackName.begin(), trackName.end());

    for (int i = 0; i < count; ++i) {
        track.push_back(0x00);
        track.push_back(0x90);                   // note on, channel 1
        track.push_back(static_cast<uint8_t>(keys[i] & 0x7F));
        track.push_back(velocity & 0x7F);
    }
    for (int i = 0; i < count; ++i) {
        putVarLen(track, i == 0 ? lengthTicks : 0);
        track.push_back(0x80);                   // note off, channel 1
        track.push_back(static_cast<uint8_t>(keys[i] & 0x7F));
        track.push_back(0x00);
    }

    track.push_back(0x00);                       // end of track
    track.push_back(0xFF);
    track.push_back(0x2F);
    track.push_back(0x00);

    std::vector<uint8_t> file;
    const char mthd[] = {'M', 'T', 'h', 'd'};
    file.insert(file.end(), mthd, mthd + 4);
    putBE(file, 6, 4);
    putBE(file, 0, 2);                           // format 0
    putBE(file, 1, 2);                           // one track
    putBE(file, kTicksPerQuarter, 2);

    const char mtrk[] = {'M', 'T', 'r', 'k'};
    file.insert(file.end(), mtrk, mtrk + 4);
    putBE(file, static_cast<uint32_t>(track.size()), 4);
    file.insert(file.end(), track.begin(), track.end());
    return file;
}

} // namespace midifile
