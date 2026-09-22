#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "chord_detector.hpp"

#if defined(__APPLE__)
extern "C" {
    typedef void (*ChordProviderFn)(void* userData, char* buf, size_t bufSize);
    void* chordGuiCreate(uint32_t w, uint32_t h,
                         ChordProviderFn provider, void* userData);
    void  chordGuiDestroy(void* view);
    bool  chordGuiSetParent(void* view, void* parentNSView);
    void  chordGuiGetPreferredSize(uint32_t* w, uint32_t* h);
}
#endif

namespace {

constexpr uint32_t kGuiWidth  = 380;
constexpr uint32_t kGuiHeight = 200;

struct ChordAnalyzer {
    const clap_host_t* host = nullptr;

    std::mutex notesMutex;
    std::set<int> notes;

    void* guiView = nullptr;
    bool guiVisible = false;

    clap_plugin_t plugin{};
};

void fillChordString(void* userData, char* buf, size_t bufSize) {
    if (!buf || bufSize == 0) return;
    auto* self = static_cast<ChordAnalyzer*>(userData);
    std::string s;
    {
        std::lock_guard<std::mutex> lk(self->notesMutex);
        s = chord::detect(self->notes);
    }
    if (s.empty()) s = "—";
    size_t n = s.size() < bufSize - 1 ? s.size() : bufSize - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

// -------- helpers --------

void handleEvent(ChordAnalyzer* self, const clap_event_header_t* hdr) {
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (hdr->type) {
        case CLAP_EVENT_NOTE_ON: {
            auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
            std::lock_guard<std::mutex> lk(self->notesMutex);
            self->notes.insert(ev->key);
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
    delete self;
}

bool plugin_activate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void plugin_deactivate(const clap_plugin_t*) {}
bool plugin_start_processing(const clap_plugin_t*) { return true; }
void plugin_stop_processing(const clap_plugin_t*) {}

void plugin_reset(const clap_plugin_t* p) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);
    std::lock_guard<std::mutex> lk(self->notesMutex);
    self->notes.clear();
}

clap_process_status plugin_process(const clap_plugin_t* p, const clap_process_t* proc) {
    auto* self = static_cast<ChordAnalyzer*>(p->plugin_data);

    if (proc->in_events) {
        const uint32_t nev = proc->in_events->size(proc->in_events);
        for (uint32_t i = 0; i < nev; ++i) {
            handleEvent(self, proc->in_events->get(proc->in_events, i));
        }
    }

    if (proc->audio_outputs_count > 0 && proc->audio_outputs[0].data32) {
        const uint32_t frames = proc->frames_count;
        const uint32_t outCh  = proc->audio_outputs[0].channel_count;
        const bool hasIn      = proc->audio_inputs_count > 0
                              && proc->audio_inputs[0].data32 != nullptr;
        const uint32_t inCh   = hasIn ? proc->audio_inputs[0].channel_count : 0;

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

uint32_t note_ports_count(const clap_plugin_t*, bool is_input) {
    return is_input ? 1 : 0;
}

bool note_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                    clap_note_port_info_t* info) {
    if (!is_input || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Note In");
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
// Nothing meaningful to persist; save/load empty payload so hosts are happy.

bool state_save(const clap_plugin_t*, const clap_ostream_t*) { return true; }
bool state_load(const clap_plugin_t*, const clap_istream_t*) { return true; }

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
        self->guiView = chordGuiCreate(kGuiWidth, kGuiHeight, &fillChordString, self);
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
    "Displays the chord name of incoming MIDI notes.",
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
