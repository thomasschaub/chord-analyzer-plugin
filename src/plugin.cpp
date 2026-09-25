#include <clap/clap.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio_analyzer.hpp"
#include "chord_detector.hpp"
#include "scale.hpp"

#if defined(__APPLE__)
#include "gui_api.h"
#endif

namespace {

constexpr uint32_t kGuiWidth  = 440;
constexpr uint32_t kGuiHeight = 520;
constexpr int kHistorySize = 100;
// How often the analysis thread looks at the audio input.
constexpr auto kAnalysisHop = std::chrono::milliseconds(25);
// Hops without new audio (host stopped calling process) before the audio
// notes are released instead of re-analysing the last window forever.
constexpr int kAnalysisIdleHops = 8;

struct ChordAnalyzer {
    const clap_host_t* host = nullptr;

    std::mutex notesMutex;
    std::set<int> notes;       // held MIDI notes
    std::set<int> audioNotes;  // notes estimated from the audio input

    // Ring buffer of the pitch classes of the last kHistorySize note-ons,
    // plus a running per-pitch-class count. Guarded by notesMutex.
    uint8_t  history[kHistorySize] = {};
    int      historyHead = 0;
    int      historyCount = 0;
    uint16_t pcCounts[12] = {};

    std::atomic<uint8_t> scaleRoot{0};
    std::atomic<uint8_t> scaleType{0};

    // Chord pad output. The GUI sets the desired chord (empty = released)
    // and bumps padGen on every press/release; guarded by outMutex.
    std::mutex outMutex;
    int      padKeys[scale::kMaxChordNotes] = {};
    int      padKeyCount = 0;
    uint32_t padGen = 0;

    // What the audio thread has actually sent. Audio thread only.
    int      sentKeys[scale::kMaxChordNotes] = {};
    int      sentKeyCount = 0;
    uint32_t sentGen = 0;

    // Audio analysis. process() copies the input into `ring`; the analysis
    // thread estimates the sounding notes and publishes them to audioNotes.
    // Runs between activate and deactivate; the audio itself passes through
    // undelayed.
    audio::SampleRing ring;
    double sampleRate = 44100.0;
    std::thread analysisThread;
    std::mutex analysisMutex;
    std::condition_variable analysisWake;
    bool analysisStop = false; // guarded by analysisMutex

    void* guiView = nullptr;
    bool guiVisible = false;

    clap_plugin_t plugin{};
};

#if defined(__APPLE__)
void fillChordSnapshot(void* userData, ChordSnapshot* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    auto* self = static_cast<ChordAnalyzer*>(userData);

    chord::Result r;
    {
        std::lock_guard<std::mutex> lk(self->notesMutex);
        std::set<int> held = self->notes;
        held.insert(self->audioNotes.begin(), self->audioNotes.end());
        r = chord::detect(held);
        uint8_t count = 0;
        for (int n : held) {
            if (count >= sizeof(out->notes)) break;
            if (n < 0 || n > 127) continue;
            out->notes[count++] = static_cast<uint8_t>(n);
        }
        out->noteCount = count;

        std::memcpy(out->histogram, self->pcCounts, sizeof(out->histogram));
        out->historyCount = static_cast<uint16_t>(self->historyCount);
    }
    scale::Detection d = scale::detect(out->histogram);
    out->detectedRoot = static_cast<uint8_t>(d.root);
    out->detectedType = static_cast<uint8_t>(d.type);
    out->detectedEarly = d.early ? 1 : 0;
    if (r.name.empty()) r.name = "—";
    size_t n = r.name.size() < sizeof(out->name) - 1 ? r.name.size() : sizeof(out->name) - 1;
    std::memcpy(out->name, r.name.data(), n);
    out->name[n] = '\0';
    out->isChord = r.isChord ? 1 : 0;
    out->scaleRoot = self->scaleRoot.load();
    out->scaleType = self->scaleType.load();
}

void onPlayChord(void* userData, int degree, int variant) {
    auto* self = static_cast<ChordAnalyzer*>(userData);
    if (variant < 0 || variant >= scale::kVariantCount) return;
    int root = self->scaleRoot.load();
    scale::Chord c;
    if (!scale::diatonicChord(root, self->scaleType.load(), degree,
                              static_cast<scale::ChordVariant>(variant), &c)) {
        return;
    }

    std::lock_guard<std::mutex> lk(self->outMutex);
    scale::chordKeys(root, c, self->padKeys);
    self->padKeyCount = c.count;
    self->padGen++;
}

void onReleaseChord(void* userData) {
    auto* self = static_cast<ChordAnalyzer*>(userData);
    std::lock_guard<std::mutex> lk(self->outMutex);
    if (self->padKeyCount == 0) return;
    self->padKeyCount = 0;
    self->padGen++;
}

void onResetHistory(void* userData) {
    auto* self = static_cast<ChordAnalyzer*>(userData);
    std::lock_guard<std::mutex> lk(self->notesMutex);
    self->historyHead = 0;
    self->historyCount = 0;
    std::memset(self->pcCounts, 0, sizeof(self->pcCounts));
}

void onScaleChanged(void* userData, uint8_t root, uint8_t type) {
    auto* self = static_cast<ChordAnalyzer*>(userData);
    if (root > 11 || type >= scale::kTypeCount) return;
    if (self->scaleRoot.load() == root && self->scaleType.load() == type) return;
    self->scaleRoot.store(root);
    self->scaleType.store(type);

    // Let the host know the project has unsaved changes.
    auto* hostState = static_cast<const clap_host_state_t*>(
        self->host->get_extension(self->host, CLAP_EXT_STATE));
    if (hostState && hostState->mark_dirty) hostState->mark_dirty(self->host);
}
#endif

// -------- helpers --------

void pushNote(const clap_output_events_t* out, int key, bool on) {
    clap_event_note_t ev{};
    ev.header.size     = sizeof(ev);
    ev.header.time     = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type     = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
    ev.header.flags    = 0;
    ev.note_id    = -1;
    ev.port_index = 0;
    ev.channel    = 0;
    ev.key        = static_cast<int16_t>(key);
    ev.velocity   = on ? scale::kChordVelocity / 127.0 : 0.0;
    out->try_push(out, &ev.header);
}

// Brings the sent pad chord in line with what the GUI wants. Emitted at
// time 0, before pass-through events, so the output stays time-sorted.
// try_lock: never block the audio thread; a missed update goes out next block.
void emitPadChord(ChordAnalyzer* self, const clap_output_events_t* out) {
    if (!self->outMutex.try_lock()) return;
    if (self->padGen != self->sentGen) {
        for (int i = 0; i < self->sentKeyCount; ++i) pushNote(out, self->sentKeys[i], false);
        for (int i = 0; i < self->padKeyCount; ++i) pushNote(out, self->padKeys[i], true);
        std::memcpy(self->sentKeys, self->padKeys, sizeof(self->sentKeys));
        self->sentKeyCount = self->padKeyCount;
        self->sentGen = self->padGen;
    }
    self->outMutex.unlock();
}

// Events passed through unchanged from the note input to the note output.
bool isNoteEvent(const clap_event_header_t* hdr) {
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
    switch (hdr->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
        case CLAP_EVENT_NOTE_EXPRESSION:
        case CLAP_EVENT_MIDI:
        case CLAP_EVENT_MIDI_SYSEX:
        case CLAP_EVENT_MIDI2:
            return true;
        default:
            return false;
    }
}

// Call with notesMutex held.
void recordNoteOn(ChordAnalyzer* self, int key) {
    uint8_t pc = static_cast<uint8_t>(((key % 12) + 12) % 12);
    if (self->historyCount == kHistorySize) {
        self->pcCounts[self->history[self->historyHead]]--;
    } else {
        self->historyCount++;
    }
    self->history[self->historyHead] = pc;
    self->historyHead = (self->historyHead + 1) % kHistorySize;
    self->pcCounts[pc]++;
}

void runAnalysis(ChordAnalyzer* self) {
    audio::NoteEstimator estimator;
    estimator.prepare(self->sampleRate);
    audio::NoteTracker tracker;
    std::vector<float> window(estimator.windowSize());
    std::vector<int> frameKeys;
    std::vector<int> onsets;
    uint64_t lastPos = 0;
    int idleHops = 0;

    std::unique_lock<std::mutex> lk(self->analysisMutex);
    while (!self->analysisStop) {
        lk.unlock();

        const uint64_t pos = self->ring.writePos();
        idleHops = pos == lastPos ? idleHops + 1 : 0;
        lastPos = pos;

        frameKeys.clear();
        if (idleHops < kAnalysisIdleHops && self->ring.readLatest(window.data(), window.size())) {
            estimator.estimate(window.data(), frameKeys);
        }
        if (tracker.update(frameKeys, onsets)) {
            std::lock_guard<std::mutex> nl(self->notesMutex);
            self->audioNotes.clear();
            tracker.forEachHeld([&](int key) { self->audioNotes.insert(key); });
            for (int key : onsets) recordNoteOn(self, key);
        }

        lk.lock();
        self->analysisWake.wait_for(lk, kAnalysisHop, [self] { return self->analysisStop; });
    }
}

void startAnalysis(ChordAnalyzer* self, double sampleRate) {
    self->sampleRate = sampleRate;
    // Room for the window plus several blocks, so the writer never laps a
    // copy in progress.
    self->ring.reset(audio::NoteEstimator::windowSizeFor(sampleRate) * 4);
    self->analysisStop = false;
    self->analysisThread = std::thread(runAnalysis, self);
}

void stopAnalysis(ChordAnalyzer* self) {
    if (!self->analysisThread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(self->analysisMutex);
        self->analysisStop = true;
    }
    self->analysisWake.notify_all();
    self->analysisThread.join();

    std::lock_guard<std::mutex> lk(self->notesMutex);
    self->audioNotes.clear();
}

void handleEvent(ChordAnalyzer* self, const clap_event_header_t* hdr) {
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (hdr->type) {
        case CLAP_EVENT_NOTE_ON: {
            auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
            std::lock_guard<std::mutex> lk(self->notesMutex);
            self->notes.insert(ev->key);
            recordNoteOn(self, ev->key);
            break;
        }
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
            std::lock_guard<std::mutex> lk(self->notesMutex);
            self->notes.erase(ev->key);
            break;
        }
        case CLAP_EVENT_MIDI: {
            auto* ev = reinterpret_cast<const clap_event_midi_t*>(hdr);
            uint8_t status = ev->data[0] & 0xF0;
            uint8_t key    = ev->data[1] & 0x7F;
            uint8_t vel    = ev->data[2] & 0x7F;
            std::lock_guard<std::mutex> lk(self->notesMutex);
            if (status == 0x90 && vel > 0) {
                self->notes.insert(key);
                recordNoteOn(self, key);
            } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
                self->notes.erase(key);
            }
            break;
        }
        default:
            break;
    }
}

// -------- plugin methods --------

bool plugin_init(const clap_plugin_t*) { return true; }

void plugin_destroy(const clap_plugin_t* p) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    stopAnalysis(self);
    delete self;
}

bool plugin_activate(const clap_plugin_t* p, double sampleRate, uint32_t, uint32_t) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    stopAnalysis(self);
    startAnalysis(self, sampleRate);
    return true;
}

void plugin_deactivate(const clap_plugin_t* p) {
    stopAnalysis(static_cast<ChordAnalyzer*>(p->plugin_data));
}
bool plugin_start_processing(const clap_plugin_t*) { return true; }
void plugin_stop_processing(const clap_plugin_t*) {}

void plugin_reset(const clap_plugin_t* p) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    std::lock_guard<std::mutex> lk(self->notesMutex);
    self->notes.clear();
}

clap_process_status plugin_process(const clap_plugin_t* p, const clap_process_t* proc) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);

    if (proc->out_events) emitPadChord(self, proc->out_events);

    if (proc->in_events) {
        const uint32_t nev = proc->in_events->size(proc->in_events);
        for (uint32_t i = 0; i < nev; ++i) {
            const clap_event_header_t* hdr = proc->in_events->get(proc->in_events, i);
            handleEvent(self, hdr);
            if (proc->out_events && isNoteEvent(hdr)) {
                proc->out_events->try_push(proc->out_events, hdr);
            }
        }
    }

    const uint32_t frames = proc->frames_count;
    const bool hasIn      = proc->audio_inputs_count > 0
                          && proc->audio_inputs[0].data32 != nullptr;
    const uint32_t inCh   = hasIn ? proc->audio_inputs[0].channel_count : 0;

    if (hasIn) self->ring.write(proc->audio_inputs[0].data32, inCh, frames);

    if (proc->audio_outputs_count > 0 && proc->audio_outputs[0].data32) {
        const uint32_t outCh  = proc->audio_outputs[0].channel_count;

        for (uint32_t c = 0; c < outCh; ++c) {
            float* dst = proc->audio_outputs[0].data32[c];
            if (!dst) continue;
            if (c < inCh) {
                const float* src = proc->audio_inputs[0].data32[c];
                if (src && src != dst) std::memcpy(dst, src, frames * sizeof(float));
            } else {
                std::memset(dst, 0, frames * sizeof(float));
            }
        }
    }

    return CLAP_PROCESS_CONTINUE;
}

void plugin_on_main_thread(const clap_plugin_t*) {}

// -------- extensions: note ports --------

uint32_t note_ports_count(const clap_plugin_t*, bool) { return 1; }

bool note_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                    clap_note_port_info_t* info) {
    if (index != 0) return false;
    info->id = is_input ? 0u : 1u;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), is_input ? "Note In" : "Note Out");
    return true;
}

constexpr clap_plugin_note_ports_t kNotePorts = {
    note_ports_count,
    note_ports_get,
};

// -------- extensions: audio ports --------

uint32_t audio_ports_count(const clap_plugin_t*, bool) { return 1; }

bool audio_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                     clap_audio_port_info_t* info) {
    if (index != 0) return false;
    info->id            = is_input ? 0u : 1u;
    info->channel_count = 2;
    info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type     = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    std::snprintf(info->name, sizeof(info->name), is_input ? "Audio In" : "Audio Out");
    return true;
}

constexpr clap_plugin_audio_ports_t kAudioPorts = {
    audio_ports_count,
    audio_ports_get,
};

// -------- extensions: state --------
// Layout: [version][scaleRoot][scaleType]

constexpr uint8_t kStateVersion = 1;

bool state_save(const clap_plugin_t* p, const clap_ostream_t* stream) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    const uint8_t data[3] = { kStateVersion, self->scaleRoot.load(), self->scaleType.load() };
    int64_t written = 0;
    while (written < (int64_t)sizeof(data)) {
        int64_t n = stream->write(stream, data + written, sizeof(data) - written);
        if (n <= 0) return false;
        written += n;
    }
    return true;
}

bool state_load(const clap_plugin_t* p, const clap_istream_t* stream) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    uint8_t data[3] = {};
    int64_t got = 0;
    while (got < (int64_t)sizeof(data)) {
        int64_t n = stream->read(stream, data + got, sizeof(data) - got);
        if (n < 0) return false;
        if (n == 0) break;
        got += n;
    }
    // Older sessions saved an empty state; keep defaults.
    if (got == 0) return true;
    if (got < (int64_t)sizeof(data) || data[0] != kStateVersion) return false;
    if (data[1] > 11 || data[2] >= scale::kTypeCount) return false;
    self->scaleRoot.store(data[1]);
    self->scaleType.store(data[2]);
    return true;
}

constexpr clap_plugin_state_t kState = { state_save, state_load };

// -------- extensions: gui --------

#if defined(__APPLE__)
constexpr const char* kNativeGuiApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kNativeGuiApi = "";
#endif

bool gui_is_api_supported(const clap_plugin_t*, const char* api, bool is_floating) {
    if (is_floating) return false;
    return std::strcmp(api, kNativeGuiApi) == 0;
}

bool gui_get_preferred_api(const clap_plugin_t*, const char** api, bool* is_floating) {
    *api = kNativeGuiApi;
    *is_floating = false;
    return kNativeGuiApi[0] != '\0';
}

bool gui_create(const clap_plugin_t* p, const char* api, bool is_floating) {
    if (is_floating) return false;
    if (std::strcmp(api, kNativeGuiApi) != 0) return false;
#if defined(__APPLE__)
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    if (!self->guiView) {
        ChordGuiCallbacks cb{};
        cb.userData     = self;
        cb.getSnapshot  = &fillChordSnapshot;
        cb.setScale     = &onScaleChanged;
        cb.resetHistory = &onResetHistory;
        cb.playChord    = &onPlayChord;
        cb.releaseChord = &onReleaseChord;
        self->guiView = chordGuiCreate(kGuiWidth, kGuiHeight, cb);
        if (!self->guiView) return false;
    }
    return true;
#else
    (void)p;
    return false;
#endif
}

void gui_destroy(const clap_plugin_t* p) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
#if defined(__APPLE__)
    if (self->guiView) {
        chordGuiDestroy(self->guiView);
        self->guiView = nullptr;
    }
    // Don't leave a pad chord hanging if the editor closes mid-press.
    onReleaseChord(self);
#endif
    self->guiVisible = false;
}

bool gui_set_scale(const clap_plugin_t*, double) { return true; }

bool gui_get_size(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    *w = kGuiWidth;
    *h = kGuiHeight;
    return true;
}

bool gui_can_resize(const clap_plugin_t*) { return false; }

bool gui_get_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }

bool gui_adjust_size(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    *w = kGuiWidth;
    *h = kGuiHeight;
    return true;
}

bool gui_set_size(const clap_plugin_t*, uint32_t, uint32_t) { return true; }

bool gui_set_parent(const clap_plugin_t* p, const clap_window_t* window) {
#if defined(__APPLE__)
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    if (!self->guiView || !window) return false;
    return chordGuiSetParent(self->guiView, window->cocoa);
#else
    (void)p; (void)window;
    return false;
#endif
}

bool gui_set_transient(const clap_plugin_t*, const clap_window_t*) { return false; }
void gui_suggest_title(const clap_plugin_t*, const char*) {}

bool gui_show(const clap_plugin_t* p) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    self->guiVisible = true;
    return true;
}

bool gui_hide(const clap_plugin_t* p) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    self->guiVisible = false;
    return true;
}

constexpr clap_plugin_gui_t kGui = {
    gui_is_api_supported,
    gui_get_preferred_api,
    gui_create,
    gui_destroy,
    gui_set_scale,
    gui_get_size,
    gui_can_resize,
    gui_get_resize_hints,
    gui_adjust_size,
    gui_set_size,
    gui_set_parent,
    gui_set_transient,
    gui_suggest_title,
    gui_show,
    gui_hide,
};

// -------- extension dispatch --------

const void* plugin_get_extension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS)     == 0) return &kNotePorts;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS)    == 0) return &kAudioPorts;
    if (std::strcmp(id, CLAP_EXT_STATE)          == 0) return &kState;
    if (std::strcmp(id, CLAP_EXT_GUI)            == 0) return &kGui;
    return nullptr;
}

// -------- descriptor --------

const char* const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_ANALYZER,
    CLAP_PLUGIN_FEATURE_NOTE_EFFECT,
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    nullptr,
};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.example.chord-analyzer",
    "Chord Analyzer",
    "example",
    "",
    "",
    "",
    "0.1.0",
    "Displays the chord name of incoming MIDI notes or audio.",
    kFeatures,
};

// -------- factory --------

const clap_plugin_t* create_plugin(const clap_plugin_factory_t*,
                                   const clap_host_t* host,
                                   const char* plugin_id) {
    if (!plugin_id || std::strcmp(plugin_id, kDescriptor.id) != 0) return nullptr;
    if (!clap_version_is_compatible(host->clap_version)) return nullptr;

    auto* self = new ChordAnalyzer();
    self->host = host;

    self->plugin.desc              = &kDescriptor;
    self->plugin.plugin_data       = self;
    self->plugin.init              = plugin_init;
    self->plugin.destroy           = plugin_destroy;
    self->plugin.activate          = plugin_activate;
    self->plugin.deactivate        = plugin_deactivate;
    self->plugin.start_processing  = plugin_start_processing;
    self->plugin.stop_processing   = plugin_stop_processing;
    self->plugin.reset             = plugin_reset;
    self->plugin.process           = plugin_process;
    self->plugin.get_extension     = plugin_get_extension;
    self->plugin.on_main_thread    = plugin_on_main_thread;

    return &self->plugin;
}

uint32_t factory_get_plugin_count(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factory_get_plugin_descriptor(
    const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

constexpr clap_plugin_factory_t kFactory = {
    factory_get_plugin_count,
    factory_get_plugin_descriptor,
    create_plugin,
};

// -------- entry --------

bool entry_init(const char*) { return true; }
void entry_deinit() {}

const void* entry_get_factory(const char* factory_id) {
    if (std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &kFactory;
    return nullptr;
}

} // namespace

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory,
};
}
