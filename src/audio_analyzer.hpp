#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

// Estimates which notes are sounding in an audio signal, for chord and scale
// detection. Only SampleRing::write runs on the audio thread; it is lock-free
// and allocation-free. Everything else runs on the analysis thread.
namespace audio {

// Recent mono samples. One writer (the audio thread) appends; one reader
// (the analysis thread) copies out the latest window.
class SampleRing {
public:
    // Not thread-safe: call while no one is writing or reading.
    void reset(size_t minCapacity) {
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        buf_.assign(cap, 0.0f);
        mask_ = cap - 1;
        writePos_.store(0, std::memory_order_relaxed);
    }

    // Appends the mono mix of `channels`.
    void write(const float* const* channels, uint32_t channelCount, uint32_t frames) {
        if (buf_.empty() || !channels || channelCount == 0) return;
        const float gain = 1.0f / static_cast<float>(channelCount);
        const uint64_t pos = writePos_.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i) {
            float s = 0.0f;
            for (uint32_t c = 0; c < channelCount; ++c) {
                if (channels[c]) s += channels[c][i];
            }
            buf_[(pos + i) & mask_] = s * gain;
        }
        writePos_.store(pos + frames, std::memory_order_release);
    }

    // Total samples written so far; stops advancing when the host stops
    // calling process.
    uint64_t writePos() const { return writePos_.load(std::memory_order_acquire); }

    // Copies the n most recent samples. False when fewer have been written,
    // or the writer overwrote part of the window while it was being copied.
    bool readLatest(float* dst, size_t n) const {
        if (n > buf_.size()) return false;
        const uint64_t end = writePos_.load(std::memory_order_acquire);
        if (end < n) return false;
        const uint64_t start = end - n;
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(start + i) & mask_];
        return writePos_.load(std::memory_order_acquire) - start <= buf_.size();
    }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    std::atomic<uint64_t> writePos_{0};
};

// In-place iterative radix-2 FFT for power-of-two sizes.
class Fft {
public:
    void prepare(size_t n) {
        n_ = n;
        twiddles_.resize(n / 2);
        for (size_t i = 0; i < n / 2; ++i) {
            double a = -2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n);
            twiddles_[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
        bitrev_.resize(n);
        int bits = 0;
        while ((size_t(1) << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            bitrev_[i] = r;
        }
    }

    void forward(std::vector<std::complex<float>>& x) const {
        for (size_t i = 0; i < n_; ++i) {
            if (i < bitrev_[i]) std::swap(x[i], x[bitrev_[i]]);
        }
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t half = len / 2;
            const size_t step = n_ / len;
            for (size_t i = 0; i < n_; i += len) {
                for (size_t j = 0; j < half; ++j) {
                    std::complex<float> t = x[i + j + half] * twiddles_[j * step];
                    x[i + j + half] = x[i + j] - t;
                    x[i + j] += t;
                }
            }
        }
    }

private:
    size_t n_ = 0;
    std::vector<std::complex<float>> twiddles_;
    std::vector<size_t> bitrev_;
};

// Estimates the notes in one window of audio. Candidates are scored by
// summing the spectral peaks at their harmonics; the strongest is accepted
// and its harmonics are subtracted, so its overtones (octave, fifth, third
// above) don't show up as notes of their own. Repeats until the remaining
// candidates are weak compared to the first.
class NoteEstimator {
public:
    static constexpr int   kLowKey       = 28;     // E1
    static constexpr int   kHighKey      = 96;     // C7
    static constexpr int   kMaxNotes     = 6;
    static constexpr int   kHarmonics    = 10;
    static constexpr float kGateRms      = 0.003f; // ~ -50 dBFS
    static constexpr float kPeakFloor    = 0.003f; // peaks below -50 dB of the loudest are ignored
    static constexpr float kMinFundamental = 0.05f; // a note's fundamental vs the loudest peak
    static constexpr float kRelSalience  = 0.2f;   // later notes vs the first
    // Fraction of a harmonic's expected level that is subtracted. Below 1 so
    // a chord tone on a bass harmonic (the fifth on the bass's third
    // harmonic, in almost every root-position voicing) survives.
    static constexpr float kSubtract = 0.5f;
    static constexpr float kToleranceCents = 40.0f;

    // ~150 ms or more: long enough to tell bass notes a semitone apart
    // through their harmonics, short enough to follow chord changes.
    static size_t windowSizeFor(double sampleRate) {
        size_t n = 1;
        while (static_cast<double>(n) < sampleRate * 0.15) n <<= 1;
        return n;
    }

    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        const size_t n = windowSizeFor(sampleRate);
        n_ = n;
        fft_.prepare(n);
        window_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            window_[i] = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * M_PI * i / n));
        }
        spectrum_.resize(n);
        mag_.resize(n / 2 + 1);
        peaks_.reserve(n / 4);
    }

    size_t windowSize() const { return n_; }

    // `samples` holds windowSize() samples. Replaces `keys` with the
    // estimated MIDI keys, strongest first.
    void estimate(const float* samples, std::vector<int>& keys) {
        keys.clear();

        double sumSq = 0.0;
        for (size_t i = 0; i < n_; ++i) sumSq += double(samples[i]) * samples[i];
        if (std::sqrt(sumSq / n_) < kGateRms) return;

        for (size_t i = 0; i < n_; ++i) spectrum_[i] = {samples[i] * window_[i], 0.0f};
        fft_.forward(spectrum_);
        for (size_t k = 0; k <= n_ / 2; ++k) mag_[k] = std::abs(spectrum_[k]);

        findPeaks();
        if (peaks_.empty()) return;
        float loudest = 0.0f;
        for (const Peak& p : peaks_) loudest = std::max(loudest, p.amp);

        float firstSalience = 0.0f;
        bool taken[128] = {};
        for (int iter = 0; iter < kMaxNotes; ++iter) {
            int bestKey = -1;
            float bestSalience = 0.0f;
            for (int key = kLowKey; key <= kHighKey; ++key) {
                if (taken[key]) continue;
                Match m;
                matchHarmonics(key, &m);
                if (m.peak[0] < 0 || m.amp[0] < loudest * kMinFundamental) continue;
                float s = salience(m);
                if (s > bestSalience) {
                    bestSalience = s;
                    bestKey = key;
                }
            }
            if (bestKey < 0) break;
            if (iter == 0) {
                firstSalience = bestSalience;
            } else if (bestSalience < firstSalience * kRelSalience) {
                break;
            }
            taken[bestKey] = true;
            keys.push_back(bestKey);
            subtractHarmonics(bestKey);
        }
    }

private:
    struct Peak {
        float freq;
        float amp;
    };

    // Per harmonic (index 0 = fundamental): matched peak index or -1.
    // Salience only looks at the first kHarmonics; subtraction clears all
    // of them so bright timbres don't leave high partials behind.
    static constexpr int kMatchHarmonics = 24;
    struct Match {
        int   peak[kMatchHarmonics];
        float amp[kMatchHarmonics];
    };

    static float keyFreq(int key) { return 440.0f * std::pow(2.0f, (key - 69) / 12.0f); }

    void findPeaks() {
        peaks_.clear();
        const float binHz = static_cast<float>(sampleRate_ / n_);
        const size_t lo = std::max<size_t>(2, static_cast<size_t>(25.0f / binHz));
        const size_t hi = std::min(n_ / 2 - 1, static_cast<size_t>(6000.0f / binHz));
        float maxMag = 0.0f;
        for (size_t k = lo; k <= hi; ++k) maxMag = std::max(maxMag, mag_[k]);
        if (maxMag <= 0.0f) return;
        const float floor = maxMag * kPeakFloor;

        for (size_t k = lo; k <= hi; ++k) {
            const float m = mag_[k];
            if (m < floor || m <= mag_[k - 1] || m < mag_[k + 1]) continue;
            // Parabolic interpolation on log magnitude.
            const float a = std::log(mag_[k - 1] + 1e-12f);
            const float b = std::log(m + 1e-12f);
            const float c = std::log(mag_[k + 1] + 1e-12f);
            const float den = a - 2.0f * b + c;
            const float p = den != 0.0f ? 0.5f * (a - c) / den : 0.0f;
            peaks_.push_back({(k + p) * binHz, std::exp(b - 0.25f * (a - c) * p)});
        }
    }

    // Loudest peak within the tolerance around f, or -1.
    int findPeakNear(float f) const {
        const float tol = f * (std::pow(2.0f, kToleranceCents / 1200.0f) - 1.0f);
        auto it = std::lower_bound(peaks_.begin(), peaks_.end(), f - tol,
                                   [](const Peak& p, float v) { return p.freq < v; });
        int best = -1;
        for (; it != peaks_.end() && it->freq <= f + tol; ++it) {
            if (best < 0 || it->amp > peaks_[best].amp) best = int(it - peaks_.begin());
        }
        return best;
    }

    void matchHarmonics(int key, Match* m) const {
        const float f0 = keyFreq(key);
        for (int h = 0; h < kMatchHarmonics; ++h) {
            m->peak[h] = findPeakNear(f0 * (h + 1));
            m->amp[h] = m->peak[h] >= 0 ? peaks_[m->peak[h]].amp : 0.0f;
        }
    }

    // Weighted harmonic sum; falling weights make the true fundamental beat
    // the octave below it (which only collects every other harmonic).
    static float salience(const Match& m) {
        float s = 0.0f;
        for (int h = 0; h < kHarmonics; ++h) s += m.amp[h] / (h + 1);
        return s;
    }

    // Mean amplitude of harmonics h-step and h+step (those that exist).
    static float neighbourLevel(const Match& m, int h, int step) {
        float sum = 0.0f;
        int n = 0;
        for (int j : {h - step, h + step}) {
            if (j < 0 || j >= kMatchHarmonics) continue;
            sum += m.amp[j];
            ++n;
        }
        return n ? sum / n : 0.0f;
    }

    // Removes (kSubtract of) the accepted note's harmonics from the peaks.
    // Each harmonic loses at most the level its neighbours suggest, so a peak that is much
    // louder (shared with another note, e.g. the fifth's fundamental on the
    // root's third harmonic) keeps the excess. Neighbours are both the
    // adjacent harmonics and those two away, whichever is louder, so
    // odd-harmonic timbres (square, clarinet) don't leave their third
    // harmonic behind as a phantom note.
    void subtractHarmonics(int key) {
        Match m;
        matchHarmonics(key, &m);
        float expected[kMatchHarmonics];
        for (int h = 0; h < kMatchHarmonics; ++h) {
            float level = std::max(neighbourLevel(m, h, 1), neighbourLevel(m, h, 2));
            expected[h] = std::min(m.amp[h], level);
        }
        for (int h = 0; h < kMatchHarmonics; ++h) {
            if (m.peak[h] < 0) continue;
            Peak& p = peaks_[m.peak[h]];
            p.amp = std::max(0.0f, p.amp - kSubtract * expected[h]);
        }
    }

    double sampleRate_ = 44100.0;
    size_t n_ = 0;
    Fft fft_;
    std::vector<float> window_;
    std::vector<std::complex<float>> spectrum_;
    std::vector<float> mag_;
    std::vector<Peak> peaks_;
};

// Debounces per-frame estimates into held notes: a key turns on after being
// seen in kOnFrames consecutive frames and off after kOffFrames without it.
class NoteTracker {
public:
    static constexpr int kOnFrames  = 3;
    static constexpr int kOffFrames = 3;

    // Returns true when the held set changed; `onsets` receives keys that
    // just turned on.
    bool update(const std::vector<int>& frameKeys, std::vector<int>& onsets) {
        onsets.clear();
        bool seen[128] = {};
        for (int k : frameKeys) {
            if (k >= 0 && k < 128) seen[k] = true;
        }
        bool changed = false;
        for (int k = 0; k < 128; ++k) {
            if (seen[k]) {
                missed_[k] = 0;
                if (!held_[k] && ++hits_[k] >= kOnFrames) {
                    held_[k] = true;
                    onsets.push_back(k);
                    changed = true;
                }
            } else {
                hits_[k] = 0;
                if (held_[k] && ++missed_[k] >= kOffFrames) {
                    held_[k] = false;
                    missed_[k] = 0;
                    changed = true;
                }
            }
        }
        return changed;
    }

    template <typename Fn>
    void forEachHeld(Fn fn) const {
        for (int k = 0; k < 128; ++k) {
            if (held_[k]) fn(k);
        }
    }

private:
    uint8_t hits_[128] = {};
    uint8_t missed_[128] = {};
    bool    held_[128] = {};
};

} // namespace audio
