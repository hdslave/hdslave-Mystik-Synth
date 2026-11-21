// Loads Mystik.clap as a host does and checks it: description and ports, parameters and their texts, state, presets,
// the gate against the 292's equations, the vactrol's timing, the sequencer against the host's transport (timing to
// the sample, every mode, stop, restart, a loop that wraps inside a block), the same output whatever the block size,
// notes and MIDI, Regenerate, bounded output at extreme settings, and the screen page. Writes demo renders (WAV) and
// the page's pictures (PNG) into the output folder.
//   host Mystik.clap OUT_FOLDER
#include "wav.h"
#include "png.h"
#include "../src/melodee_screen.h"
#include "../src/voice.h" // the gate's analog response (gateResponse, gateDb), to hold the sound to it

#include <clap/clap.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++checks; const double va = (a), vb = (b); if (! (std::fabs (va - vb) <= (tol))) { ++failures; \
    std::printf ("FAIL %s:%d  %s = %g, %s = %g (tolerance %g)\n", __FILE__, __LINE__, #a, va, #b, vb, (double) (tol)); } } while (0)

const clap_plugin_entry* entry = nullptr;
const clap_plugin_factory* factory = nullptr;

int rescans = 0, textRescans = 0, callbacks = 0, flushes = 0, paints = 0, processRequests = 0, presetsLoaded = 0, presetErrors = 0;
const void* hostExtension (const clap_host*, const char* id)
{
    static const clap_host_log log { [] (const clap_host*, clap_log_severity s, const char* m) { std::printf ("  [plug-in log %d] %s\n", (int) s, m); } };
    static const clap_host_params params { [] (const clap_host*, clap_param_rescan_flags f) {
                                               rescans += (f & CLAP_PARAM_RESCAN_VALUES) != 0;
                                               textRescans += (f & CLAP_PARAM_RESCAN_TEXT) != 0;
                                           },
                                           [] (const clap_host*, clap_id, clap_param_clear_flags) {}, [] (const clap_host*) { ++flushes; } };
    static const clap_host_melodee_screen screen { [] (const clap_host*) { ++paints; } };
    static const clap_host_preset_load presetLoad {
        [] (const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) { ++presetErrors; },
        [] (const clap_host*, uint32_t, const char*, const char*) { ++presetsLoaded; },
    };
    if (std::strcmp (id, CLAP_EXT_MELODEE_SCREEN_HOST) == 0) return &screen;
    if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &log;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0) return &presetLoad;
    return nullptr;
}

clap_host host {
    CLAP_VERSION, nullptr, "Mystik test host", "tests", "", "1",
    hostExtension, [] (const clap_host*) {}, [] (const clap_host*) { ++processRequests; }, [] (const clap_host*) { ++callbacks; },
};

struct Events
{
    struct Any { union { clap_event_header header; clap_event_param_value param; clap_event_note note; clap_event_midi midi; }; };
    std::vector<Any> list;
    clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->list.size(); },
                           [] (const clap_input_events* e, uint32_t i) { return &static_cast<const Events*> (e->ctx)->list[i].header; } };
    void param (uint32_t time, clap_id id, double value)
    {
        Any a {};
        a.param = { { sizeof (clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        list.push_back (a);
    }
    void note (uint32_t time, uint16_t type, int16_t key, double velocity)
    {
        Any a {};
        a.note = { { sizeof (clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 }, -1, 0, 0, key, velocity };
        list.push_back (a);
    }
    void midi (uint32_t time, uint8_t status, uint8_t d1, uint8_t d2)
    {
        Any a {};
        a.midi.header = { sizeof (clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        a.midi.port_index = 0, a.midi.data[0] = status, a.midi.data[1] = d1, a.midi.data[2] = d2;
        list.push_back (a);
    }
    void sort() { std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; }); }
};

struct OutEvents
{
    std::vector<std::pair<clap_id, double>> values;
    clap_output_events out { this, [] (const clap_output_events* o, const clap_event_header* h) {
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* v = reinterpret_cast<const clap_event_param_value*> (h);
            static_cast<OutEvents*> (o->ctx)->values.push_back ({ v->param_id, v->value });
        }
        return true;
    } };
};

struct Transport
{
    bool playing = true;
    double bpm = 120.0, beats = 0.0;
};
Transport stopped() { return { false, 120.0, 0.0 }; }
Transport playingFrom (double startBeats, double bpm, double rate, uint64_t at) { return { true, bpm, startBeats + (double) at * bpm / 60.0 / rate }; }

struct Instance
{
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;
    const clap_plugin_preset_load* presets = nullptr;
    std::map<std::string, clap_id> ids;
    double rate = 48000.0;

    explicit Instance (double sampleRate = 48000.0, uint32_t maxFrames = 4096) : rate (sampleRate)
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        plugin = factory->create_plugin (factory, &host, d->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        for (uint32_t i = 0; params && i < params->count (plugin); ++i)
        {
            clap_param_info info {};
            params->get_info (plugin, i, &info);
            ids[info.name] = info.id;
        }
        CHECK (plugin->activate (plugin, rate, 1, maxFrames));
        CHECK (plugin->start_processing (plugin));
    }
    ~Instance()
    {
        plugin->stop_processing (plugin);
        plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
    clap_id id (const std::string& name)
    {
        const auto it = ids.find (name);
        if (it == ids.end())
        {
            std::printf ("FAIL no parameter named \"%s\"\n", name.c_str());
            ++failures;
            return CLAP_INVALID_ID;
        }
        return it->second;
    }
    bool loadPreset (const char* key) { return presets != nullptr && presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key); }
    double value (const std::string& name)
    {
        double v = 0.0;
        params->get_value (plugin, id (name), &v);
        return v;
    }
    double valueOf (const std::string& name, const std::string& text)
    {
        double v = 0.0;
        CHECK (params->text_to_value (plugin, id (name), text.c_str(), &v));
        return v;
    }
    std::string text (const std::string& name, double v)
    {
        char s[128] = {};
        params->value_to_text (plugin, id (name), v, s, sizeof (s));
        return s;
    }

    void render (uint32_t frames, uint32_t block, std::vector<float>& left, std::vector<float>& right,
                 const std::function<Transport (uint64_t)>& transport, const std::function<void (uint64_t, uint32_t, Events&)>& events = {},
                 OutEvents* out = nullptr)
    {
        left.assign (frames, 0.0f);
        right.assign (frames, 0.0f);
        OutEvents ignored;
        for (uint64_t at = 0; at < frames; at += block)
        {
            const uint32_t n = (uint32_t) std::min<uint64_t> (block, frames - at);
            const auto t = transport (at);
            clap_event_transport tr {};
            tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | (t.beats >= 0.0 ? CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0) | (t.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            tr.tempo = t.bpm;
            tr.song_pos_beats = (clap_beattime) std::llround (t.beats * (double) CLAP_BEATTIME_FACTOR);
            Events e;
            if (events)
                events (at, n, e);
            e.sort();
            float* outs[2] = { left.data() + at, right.data() + at };
            clap_audio_buffer buffer {};
            buffer.data32 = outs;
            buffer.channel_count = 2;
            clap_process p {};
            p.steady_time = (int64_t) at;
            p.frames_count = n;
            p.transport = &tr;
            p.audio_outputs = &buffer;
            p.audio_outputs_count = 1;
            p.in_events = &e.in;
            p.out_events = out ? &out->out : &ignored.out;
            CHECK (plugin->process (plugin, &p) != CLAP_PROCESS_ERROR);
        }
    }
    void set (Events& e, const std::string& name, double value) { e.param (0, id (name), value); }
    // Parameter changes before anything plays: one silent, stopped block.
    void setup (const std::vector<std::pair<std::string, double>>& values)
    {
        std::vector<float> l, r;
        render (1, 1, l, r, [] (uint64_t) { return stopped(); }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0)
                for (auto& [name, v] : values)
                    set (e, name, v);
        });
    }
    // Plays `frames` from the song's top (or stopped).
    std::vector<float> play (uint32_t frames, bool playing = true, double bpm = 120.0, uint32_t block = 128)
    {
        std::vector<float> l, r;
        render (frames, block, l, r, [&] (uint64_t at) { return playing ? playingFrom (0.0, bpm, rate, at) : stopped(); });
        return l;
    }
    // Keys at frames (on, off), stopped.
    std::vector<float> keys (uint32_t frames, const std::vector<std::tuple<uint32_t, int, double, bool>>& list, std::vector<float>* rightOut = nullptr)
    {
        std::vector<float> l, r;
        render (frames, 128, l, r, [] (uint64_t) { return stopped(); }, [&] (uint64_t at, uint32_t n, Events& e) {
            for (auto& [frame, key, velocity, on] : list)
                if (frame >= at && frame < at + n)
                    e.note ((uint32_t) (frame - at), on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, (int16_t) key, velocity);
        });
        if (rightOut) *rightOut = r;
        return l;
    }
};

// Hits: where the signal passes `threshold` in a rise that began after at least `gap` samples below `quiet`.
std::vector<long> onsets (const std::vector<float>& x, float threshold = 1e-3f, float quiet = 1e-4f, long gap = 96)
{
    std::vector<long> found;
    long silent = gap;
    bool armed = true, counted = false;
    for (long i = 0; i < (long) x.size(); ++i)
    {
        const float a = std::fabs (x[(size_t) i]);
        if (a < quiet)
        {
            ++silent;
            continue;
        }
        if (silent > 0)
            armed = silent >= gap, counted = false;
        silent = 0;
        if (armed && ! counted && a > threshold)
            found.push_back (i), counted = true;
    }
    return found;
}

// Where a hit expected at `at` really starts, against a reference hit at `ref`: the lag (within +-maxLag) whose window
// best matches the reference's (a hit's noise burst varies, its shape does not).
long lagOf (const std::vector<float>& x, long ref, long at, long window = 480, long maxLag = 24)
{
    long best = 0;
    double bestScore = -1e30;
    for (long d = -maxLag; d <= maxLag; ++d)
    {
        double score = 0.0;
        for (long n = 0; n < window; ++n)
        {
            const long i = ref + n, j = at + d + n;
            if (i >= 0 && j >= 0 && i < (long) x.size() && j < (long) x.size())
                score += (double) x[(size_t) i] * x[(size_t) j];
        }
        if (score > bestScore) bestScore = score, best = d;
    }
    return best;
}

double peakOf (const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX)
{
    double p = 0.0;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        p = std::max (p, (double) std::fabs (x[i]));
    return p;
}
double rmsOf (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0.0;
    to = std::min (to, x.size());
    for (size_t i = from; i < to; ++i)
        s += (double) x[i] * x[i];
    return to > from ? std::sqrt (s / (double) (to - from)) : 0.0;
}
bool finiteAndBounded (const std::vector<float>& x, float limit = 1.0f)
{
    return std::all_of (x.begin(), x.end(), [limit] (float v) { return std::isfinite (v) && std::fabs (v) <= limit; });
}
// The fundamental by zero crossings (rising), in Hz.
double pitchOf (const std::vector<float>& x, size_t from, size_t to, double rate)
{
    long first = -1, last = -1, count = 0;
    for (size_t i = from + 1; i < std::min (to, x.size()); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double frac = (double) -x[i - 1] / (double) (x[i] - x[i - 1]);
            const long at = (long) i; (void) frac;
            if (first < 0) first = at;
            last = at, ++count;
        }
    return count > 1 ? (double) (count - 1) * rate / (double) (last - first) : 0.0;
}

// A clean patch for timing: the membrane alone, a short ring and gate, no effects, every step a plain hit.
std::vector<std::pair<std::string, double>> clickPatch()
{
    std::vector<std::pair<std::string, double>> v { { "Source", 1.0 }, { "Snap", 0.0 }, { "Ring", 0.0 }, { "Decay", 0.0 }, { "Bend", 0.0 },
                                                    { "Drift", 0.0 }, { "Bloom", 0.0 }, { "Timbre", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 },
                                                    { "Memory", 0.0 }, { "Swing", 0.5 }, { "Density", 0.5 }, { "Position", 0.0 },
                                                    { "Human", 0.0 } };
    for (int s = 1; s <= 16; ++s)
    {
        v.push_back ({ "Trig " + std::to_string (s), 1.0 });
        v.push_back ({ "Stroke " + std::to_string (s), 0.0 });
        v.push_back ({ "Chance " + std::to_string (s), 1.0 });
        v.push_back ({ "Pitch " + std::to_string (s), 0.0 });
    }
    return v;
}

// --- Description, parameters, state, presets ---------------------------------------------------------------------
void testDescription()
{
    CHECK (factory->get_plugin_count (factory) == 1);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    CHECK (std::strcmp (d->id, "com.hdslave.mystik") == 0);
    CHECK (std::strcmp (d->name, "hdslave Mystik") == 0);
    Instance a;
    const auto* audio = static_cast<const clap_plugin_audio_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_AUDIO_PORTS));
    const auto* notes = static_cast<const clap_plugin_note_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_NOTE_PORTS));
    CHECK (audio && audio->count (a.plugin, false) == 1 && audio->count (a.plugin, true) == 0);
    clap_audio_port_info info {};
    CHECK (audio->get (a.plugin, 0, false, &info) && info.channel_count == 2);
    CHECK (notes && notes->count (a.plugin, true) == 1);
    CHECK (a.plugin->get_extension (a.plugin, CLAP_EXT_MELODEE_SCREEN) != nullptr);
    // Activation refuses a rate it cannot run at.
    const clap_plugin* p = factory->create_plugin (factory, &host, d->id);
    CHECK (p->init (p));
    CHECK (! p->activate (p, 0.0, 1, 128));
    CHECK (! p->activate (p, NAN, 1, 128));
    p->destroy (p);
}

void testParameters()
{
    Instance a;
    const uint32_t n = a.params->count (a.plugin);
    CHECK (n == 116);
    std::vector<clap_id> seen;
    for (uint32_t i = 0; i < n; ++i)
    {
        clap_param_info info {};
        CHECK (a.params->get_info (a.plugin, i, &info));
        CHECK (std::find (seen.begin(), seen.end(), info.id) == seen.end());
        seen.push_back (info.id);
        CHECK (info.min_value < info.max_value);
        CHECK (info.default_value >= info.min_value && info.default_value <= info.max_value);
        double v = 0.0;
        CHECK (a.params->get_value (a.plugin, info.id, &v) && v == info.default_value);
        // Every value's text reads back to (about) the value, at the ends and between.
        for (double t : { 0.0, 0.13, 0.5, 0.77, 1.0 })
        {
            double x = info.min_value + t * (info.max_value - info.min_value);
            if (info.flags & CLAP_PARAM_IS_STEPPED) x = std::round (x);
            if (info.flags & CLAP_PARAM_IS_ENUM && std::string (info.name) == "Regenerate") continue;
            char text[64] = {};
            CHECK (a.params->value_to_text (a.plugin, info.id, x, text, sizeof (text)));
            double back = -1e9;
            const bool parsed = a.params->text_to_value (a.plugin, info.id, text, &back);
            const bool degree = std::string (info.name).rfind ("Pitch ", 0) == 0;
            if (degree) continue; // a step's note names depend on the scale; numbers are read instead
            if (! parsed || std::fabs (back - x) > 0.02 * (info.max_value - info.min_value) + 1e-6)
            {
                std::printf ("  text round trip: %s %g -> \"%s\" -> %g\n", info.name, x, text, back);
                CHECK (false);
            }
        }
    }
    // No number is refused; padded and lower-case names are read.
    double v = 0.0;
    CHECK (! a.params->text_to_value (a.plugin, a.id ("Decay"), "nan", &v));
    CHECK (! a.params->text_to_value (a.plugin, a.id ("Decay"), "inf", &v));
    CHECK (a.params->text_to_value (a.plugin, a.id ("Body"), " udu ", &v) && v == 3.0);
    CHECK (a.params->text_to_value (a.plugin, a.id ("Tune"), "G2", &v) && v == 7.0);
    CHECK (a.params->text_to_value (a.plugin, a.id ("Gate Mode"), "Combo", &v) && v == 0.5);
    CHECK (a.params->text_to_value (a.plugin, a.id ("Gate Mode"), "LP 50%", &v) && std::fabs (v - 0.75) < 1e-6);
    CHECK (a.params->text_to_value (a.plugin, a.id ("Pitch 3"), "4", &v) && v == 4.0);
    // A step's Pitch reads as a note of Scale from Tune: Hijaz degree 2 from A2 is C#3; a Tune or Scale change asks the
    // host to read the texts again.
    CHECK (a.text ("Pitch 1", 2.0) == "C#3");
    CHECK (a.text ("Pitch 1", -7.0) == "A1");
    const int before = textRescans, callbacksBefore = callbacks;
    a.setup ({ { "Scale", a.valueOf ("Scale", "Slendro") } });
    CHECK (callbacks > callbacksBefore);
    a.plugin->on_main_thread (a.plugin);
    CHECK (textRescans > before);
    CHECK (a.text ("Pitch 1", 1.0) == "B2+40c");
}

void testState()
{
    std::vector<uint8_t> saved;
    clap_ostream out { &saved, [] (const clap_ostream* s, const void* data, uint64_t size) -> int64_t {
        auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
        v->insert (v->end(), (const uint8_t*) data, (const uint8_t*) data + size);
        return (int64_t) size;
    } };
    {
        Instance a;
        a.setup ({ { "Body", 6 }, { "Decay", 0.81 }, { "Pitch 7", -5 }, { "Seq Mode", 3 }, { "Rule", 110 }, { "Seed", 4242 } });
        CHECK (a.state->save (a.plugin, &out));
    }
    struct Reader { const std::vector<uint8_t>* data; size_t at; };
    auto load = [] (Instance& b, const std::vector<uint8_t>& bytes) {
        Reader reader { &bytes, 0 };
        clap_istream in { &reader, [] (const clap_istream* s, void* data, uint64_t size) -> int64_t {
            auto* r = static_cast<Reader*> (s->ctx);
            const size_t n = std::min<size_t> ((size_t) size, r->data->size() - r->at);
            std::memcpy (data, r->data->data() + r->at, n);
            r->at += n;
            return (int64_t) n;
        } };
        return b.state->load (b.plugin, &in);
    };
    Instance b;
    CHECK (load (b, saved));
    CHECK (b.value ("Body") == 6 && std::fabs (b.value ("Decay") - 0.81) < 1e-6 && b.value ("Pitch 7") == -5);
    CHECK (b.value ("Seq Mode") == 3 && b.value ("Rule") == 110 && b.value ("Seed") == 4242);
    // Another tag, or a short stream, is refused and changes nothing.
    auto bad = saved;
    bad[0] = 'X';
    Instance c;
    CHECK (! load (c, bad));
    CHECK (c.value ("Body") == 1);
    auto shortened = saved;
    shortened.resize (20);
    CHECK (! load (c, shortened));
    // An unknown id is skipped; a value out of range is held to its range.
    auto extra = saved;
    const uint32_t count = 116;
    std::memcpy (extra.data() + 8, &count, 4);
    const struct { uint32_t id; float v; } unknown { 9999, 1.0f };
    extra.insert (extra.end(), (const uint8_t*) &unknown, (const uint8_t*) &unknown + 8);
    Instance d;
    CHECK (load (d, extra) && d.value ("Body") == 6);
}

void testPresets()
{
    const auto* discovery = static_cast<const clap_preset_discovery_factory*> (entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    CHECK (discovery != nullptr && discovery->count (discovery) == 1);
    const char* keys[] = { "mystik", "west-bongo", "tabla-teental", "conga-tumbao", "udu-bloom", "acid-gate", "turing-acid",
                           "singing-bowl", "slit-euclid", "handpan-drift", "maqsum-night", "feedback-witch", "bouncing-ball", "gankogui-bell",
                           "goblet-maqsum" };
    for (const char* key : keys)
    {
        Instance a;
        const int loaded = presetsLoaded;
        CHECK (a.loadPreset (key));
        CHECK (presetsLoaded == loaded + 1);
        const auto out = a.play (48000 * 4);
        if (! finiteAndBounded (out) || peakOf (out) < 0.02)
            std::printf ("  preset %s: peak %.4f\n", key, peakOf (out));
        CHECK (finiteAndBounded (out));
        CHECK (peakOf (out) > 0.02);
    }
    Instance a;
    const int errors = presetErrors;
    CHECK (! a.loadPreset ("no-such-preset"));
    CHECK (presetErrors == errors + 1);
}

// --- The gate ------------------------------------------------------------------------------------------------------
// The gate held open by Open (a steady light), the oscillator a pure sine: the output is the sine times |H| at its
// pitch, as the 292's transfer function gives it (and Volume's gain). This holds the SVF, its prewarping, the poles'
// mapping and the decimator to the analog response.
void testGateResponse()
{
    struct Case { double mode, resonance, tune; };
    const Case cases[] = { { 0.5, 0.0, 0.0 }, { 0.5, 0.0, 24.0 }, { 0.0, 0.0, 12.0 }, { 1.0, 0.0, 0.0 }, { 1.0, 0.5, 12.0 }, { 0.75, 0.0, 36.0 },
                           { 0.25, 0.0, 30.0 } };
    for (const auto& c : cases)
    {
        Instance a;
        a.setup ({ { "Source", 0.0 }, { "FM Index", 0.0 }, { "Feedback", 0.0 }, { "Timbre", 0.0 }, { "Symmetry", 0.0 }, { "Bloom", 0.0 },
                   { "Drift", 0.0 }, { "Bend", 0.0 }, { "Open", 1.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Sequencer", 0.0 },
                   { "Gate Mode", c.mode }, { "Resonance", c.resonance }, { "Tune", c.tune }, { "Volume", 0.7 }, { "Accent", 0.0 }, { "Memory", 0.0 }, { "Strike", 0.0 } });
        const auto out = a.keys (48000, { { 0, 60, 1.0, true } });
        const double hz = 130.812783 * std::pow (2.0, c.tune / 12.0);
        const double measured = peakOf (out, 30000, 48000);
        const auto g = mystik::gateResponse (mystik::ldrOhms (0.1), mystik::gateCircuit (c.mode, c.resonance));
        const double expected = std::pow (10.0, mystik::gateDb (g, hz) / 20.0) * 0.7 * 0.7 * 1.4;
        if (std::fabs (20.0 * std::log10 (measured / expected)) > 0.35)
            std::printf ("  gate mode %.2f res %.2f at %.0f Hz: %.2f dB measured, %.2f dB expected\n", c.mode, c.resonance, hz,
                         20.0 * std::log10 (measured), 20.0 * std::log10 (expected));
        CHECK (std::fabs (20.0 * std::log10 (measured / expected)) <= 0.35);
    }
    // The model itself: Combo fully open passes; dark it is silent; Resonance 0 is the 292 as built (Lowpass's small
    // bump, Q near 1), and at full Resonance it rings without running away.
    const auto open = mystik::gateResponse (mystik::ldrOhms (1.0), mystik::gateCircuit (0.5, 0.0));
    CHECK (mystik::gateDb (open, 1000.0) > -0.2 && mystik::gateDb (open, 10000.0) > -1.5);
    const auto dark = mystik::gateResponse (mystik::ldrOhms (0.0), mystik::gateCircuit (0.5, 0.0));
    CHECK (mystik::gateDb (dark, 100.0) < -60.0);
    const auto lp = mystik::gateResponse (mystik::ldrOhms (0.02), mystik::gateCircuit (1.0, 0.0));
    CHECK (lp.q > 0.8 && lp.q < 1.2);
    const auto ringing = mystik::gateResponse (mystik::ldrOhms (0.02), mystik::gateCircuit (1.0, 1.0));
    CHECK (ringing.q > 10.0 && ringing.q < 60.0);
}

// The vactrol: quick to open, slow to close; Decay sets the fall; Memory makes a gate hit often fall slower; harder hits
// open it further and longer; the sound darkens as it fades.
void testVactrol()
{
    auto ping = [] (std::vector<std::pair<std::string, double>> extra, std::vector<std::tuple<uint32_t, int, double, bool>> keys, uint32_t frames = 96000) {
        Instance a;
        std::vector<std::pair<std::string, double>> v { { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.5 }, { "Bloom", 0.0 }, { "Drift", 0.0 },
                                                         { "Bend", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Sequencer", 0.0 }, { "Memory", 0.0 } };
        for (auto& e : extra) v.push_back (e);
        a.setup (v);
        return a.keys (frames, keys);
    };
    // The envelope: 5 ms windows of RMS.
    auto envelope = [] (const std::vector<float>& x, size_t from) {
        std::vector<double> e;
        for (size_t at = from; at + 240 <= x.size(); at += 240)
            e.push_back (rmsOf (x, at, at + 240));
        return e;
    };
    const auto one = ping ({}, { { 0, 60, 1.0, true } });
    const auto env = envelope (one, 0);
    const size_t peakAt = (size_t) (std::max_element (env.begin(), env.end()) - env.begin());
    const double peak = env[peakAt];
    size_t fall = peakAt;
    while (fall < env.size() && env[fall] > peak * 0.1) ++fall;
    CHECK (peakAt * 5 <= 30);                     // open within 30 ms
    CHECK ((fall - peakAt) * 5 >= 150);           // and 20 dB down only after 150 ms or more
    CHECK ((fall - peakAt) > 5 * std::max<size_t> (peakAt, 1));
    // Decay longer: the fall longer.
    const auto longer = ping ({ { "Decay", 0.75 } }, { { 0, 60, 1.0, true } });
    const auto envLong = envelope (longer, 0);
    size_t fallLong = peakAt;
    while (fallLong < envLong.size() && envLong[fallLong] > peak * 0.1) ++fallLong;
    CHECK (fallLong > fall + (fall - peakAt));
    // Harder: louder and longer.
    const auto soft = ping ({}, { { 0, 60, 0.3, true } });
    const auto envSoft = envelope (soft, 0);
    CHECK (*std::max_element (envSoft.begin(), envSoft.end()) < peak * 0.9);
    // Darker as it fades: the share of the first difference's energy (the highs) falls.
    auto brightness = [] (const std::vector<float>& x, size_t from, size_t to) {
        double hi = 0.0, all = 1e-30;
        for (size_t i = from + 1; i < to; ++i) hi += (double) (x[i] - x[i - 1]) * (x[i] - x[i - 1]), all += (double) x[i] * x[i];
        return hi / all;
    };
    const auto bright = ping ({ { "Tune", 24.0 }, { "Timbre", 0.9 } }, { { 0, 60, 1.0, true } });
    // A hard ping holds the gate wide open a while (as the 292's does), then it darkens as it closes.
    CHECK (brightness (bright, 480, 4800) > 2.0 * brightness (bright, 28800, 33600));
    // Memory: eight quick hits, then the tail of the last against a single hit's.
    std::vector<std::tuple<uint32_t, int, double, bool>> roll;
    for (int i = 0; i < 8; ++i) roll.push_back ({ (uint32_t) (i * 2400), 60, 1.0, true });
    const auto withMemory = ping ({ { "Memory", 1.0 } }, roll, 144000);
    const auto without = ping ({ { "Memory", 0.0 } }, roll, 144000);
    const size_t tail = 7 * 2400 + 24000;
    CHECK (rmsOf (withMemory, tail, tail + 4800) > 1.5 * rmsOf (without, tail, tail + 4800));
}

// --- Sequencer -----------------------------------------------------------------------------------------------------
void testTiming()
{
    // 1/8 at 120 BPM: a hit every 12000 frames, from the song's top.
    Instance a;
    auto v = clickPatch();
    v.push_back ({ "Rate", a.valueOf ("Rate", "1/8") });
    a.setup (v);
    const auto out = a.play (48000 * 3);
    const auto hits = onsets (out, 2e-3f, 2e-4f, 400);
    CHECK (hits.size() == 12);
    // Each hit starts exactly a step after the first (its shape matched to the sample).
    for (long i = 1; i < 12; ++i)
    {
        const long lag = lagOf (out, 0, 12000 * i);
        if (lag != 0 && std::getenv ("MYSTIK_DEBUG")) std::printf ("  hit %ld lag %ld\n", i, lag);
        CHECK (lag == 0);
    }
    CHECK (! hits.empty() && hits[0] < 300);
    // Swing 60 %: every second hit late by 10 % of the pair.
    Instance b;
    v.push_back ({ "Swing", 0.6 });
    b.setup (v);
    const auto swung = onsets (b.play (48000 * 2), 2e-3f, 2e-4f, 400);
    CHECK (swung.size() == 8);
    const auto swungOut = b.play (48000 * 2);
    (void) swungOut;
    Instance b2;
    b2.setup (v);
    const auto again = b2.play (48000 * 2);
    CHECK (lagOf (again, 0, 14400) == 0);  // the second of a pair 60 % of the pair late
    CHECK (lagOf (again, 0, 24000) == 0);
    CHECK (lagOf (again, 0, 38400) == 0);
    // Sequencer off: nothing while playing; stopped: nothing.
    Instance c;
    auto off = clickPatch();
    off.push_back ({ "Sequencer", 0.0 });
    c.setup (off);
    CHECK (peakOf (c.play (48000)) < 1e-6);
    Instance d;
    d.setup (clickPatch());
    CHECK (peakOf (d.play (24000, false)) < 1e-6);
}

void testModes()
{
    auto run = [] (std::vector<std::pair<std::string, double>> extra, uint32_t frames, double bpm = 120.0) {
        Instance a;
        auto v = clickPatch();
        v.push_back ({ "Rate", a.valueOf ("Rate", "1/4") });
        for (auto& e : extra) v.push_back (e);
        a.setup (v);
        return onsets (a.play (frames, true, bpm), 2e-3f, 2e-4f, 400);
    };
    auto places = [] (const std::vector<long>& hits, long step, long cycle) {
        std::vector<long> p;
        for (long h : hits) p.push_back (((h + 300) / step) % cycle);
        return p;
    };
    // Euclid E(3,8): the tresillo's places 0, 3, 6.
    {
        Instance probe;
        const auto hits = run ({ { "Seq Mode", probe.valueOf ("Seq Mode", "Euclid") }, { "Length", 8 }, { "Density", 3.0 / 8.0 } }, 48000 * 8);
        CHECK (hits.size() == 6);
        const auto p = places (hits, 24000, 8);
        CHECK ((p == std::vector<long> { 0, 3, 6, 0, 3, 6 }));
    }
    // Rhythm: Maqsum's places 0 1 3 4 6 of 8.
    {
        Instance probe;
        const auto hits = run ({ { "Seq Mode", probe.valueOf ("Seq Mode", "Rhythm") }, { "Rhythm", probe.valueOf ("Rhythm", "Maqsum") },
                                 { "Accents", 0.0 }, { "Mutate", 0.0 } }, 48000 * 8);
        CHECK ((places (hits, 24000, 8) == std::vector<long> { 0, 1, 3, 4, 6, 0, 1, 3, 4, 6 }));
        if (std::getenv ("MYSTIK_DEBUG")) { for (long h : hits) std::printf (" %ld", h); std::printf ("  (maqsum)\n"); }
    }
    // Pattern: a rest and a step at Chance 0 are silent; Density 0 drops everything, 1 fills every rest.
    {
        const auto hits = run ({ { "Length", 4 }, { "Trig 2", 0 }, { "Chance 4", 0.0 } }, 48000 * 4);
        CHECK ((places (hits, 24000, 4) == std::vector<long> { 0, 2, 0, 2 }));
        CHECK (run ({ { "Density", 0.0 } }, 48000 * 2).empty());
        const auto full = run ({ { "Length", 4 }, { "Trig 2", 0 }, { "Trig 3", 0 }, { "Density", 1.0 } }, 48000 * 4);
        CHECK (full.size() == 8);
        if (std::getenv ("MYSTIK_DEBUG")) { for (long h : full) std::printf (" %ld", h); std::printf ("  (full)\n"); }
        if (std::getenv ("MYSTIK_DEBUG"))
        {
            Instance a;
            auto v = clickPatch();
            v.push_back ({ "Rate", a.valueOf ("Rate", "1/4") });
            for (auto& e : std::vector<std::pair<std::string, double>> { { "Length", 4 }, { "Trig 2", 0 }, { "Trig 3", 0 }, { "Density", 1.0 } }) v.push_back (e);
            a.setup (v);
            const auto out = a.play (48000 * 4);
            for (int k = 0; k < 8; ++k)
                std::printf ("  step %d: peak %.5f, quiet before %.6f\n", k, peakOf (out, (size_t) k * 24000, (size_t) k * 24000 + 4800),
                             k > 0 ? peakOf (out, (size_t) k * 24000 - 1000, (size_t) k * 24000) : 0.0);
        }
    }
    // Turing, locked (Mutate 0): its pattern repeats every Length steps.
    {
        Instance probe;
        const auto hits = run ({ { "Seq Mode", probe.valueOf ("Seq Mode", "Turing") }, { "Length", 6 }, { "Mutate", 0.0 }, { "Density", 0.6 },
                                 { "Seed", 7 } }, 48000 * 9);
        // The places hit in each 6-step loop are the same loop after loop.
        std::vector<int> loops[4];
        for (long h : hits) loops[std::min<long> (3, (h + 300) / 144000)].push_back ((int) (((h + 300) / 24000) % 6));
        CHECK (! loops[0].empty() && loops[0] == loops[1] && loops[1] == loops[2]);
        if (std::getenv ("MYSTIK_DEBUG")) { for (long h : hits) std::printf (" %ld", h); std::printf ("  (turing)\n"); }
    }
    // Automaton: rule 90 from one cell spreads (its hits each pass differ from the first).
    {
        Instance probe;
        const auto hits = run ({ { "Seq Mode", probe.valueOf ("Seq Mode", "Automaton") }, { "Length", 8 }, { "Rule", 90 }, { "Density", 0.25 },
                                 { "Seed", 3 } }, 48000 * 16);
        std::vector<int> passes[4];
        for (long h : hits) passes[std::min<long> (3, (h + 300) / 192000)].push_back ((int) (((h + 300) / 24000) % 8));
        CHECK (! passes[0].empty());
        CHECK (passes[0] != passes[1] || passes[1] != passes[2]);
    }
    // Bounce: within a cycle the gaps shrink.
    {
        Instance probe;
        Instance a;
        auto v = clickPatch();
        v.push_back ({ "Rate", a.valueOf ("Rate", "1/4") });
        v.push_back ({ "Seq Mode", a.valueOf ("Seq Mode", "Bounce") });
        v.push_back ({ "Length", 16 });
        v.push_back ({ "Density", 0.2 });
        v.push_back ({ "Curve", 0.8 });
        a.setup (v);
        const auto out = a.play (48000 * 8);
        const auto hits = onsets (out, 1e-3f, 1e-4f, 200);
        CHECK (hits.size() >= 4);
        bool shrinking = true;
        for (size_t i = 2; i < std::min<size_t> (hits.size(), 5); ++i)
            shrinking &= hits[i] - hits[i - 1] < hits[i - 1] - hits[i - 2];
        CHECK (shrinking);
    }
}

// Stop and play again from the top: the same sound; a loop that wraps inside a block (the host tells at the next
// block, as Melodee does) plays its downbeat once.
void testTransport()
{
    auto patch = [] (Instance& a) {
        auto v = clickPatch();
        v.push_back ({ "Rate", a.valueOf ("Rate", "1/16") });
        v.push_back ({ "Seq Mode", a.valueOf ("Seq Mode", "Turing") });
        v.push_back ({ "Mutate", 0.3 });
        return v;
    };
    Instance a;
    a.setup (patch (a));
    const auto first = a.play (48000);
    a.play (4800, false);
    a.plugin->reset (a.plugin);
    const auto again = a.play (48000);
    CHECK (first == again);
    if (first != again)
    {
        const auto x = onsets (first, 2e-3f, 2e-4f, 400), y = onsets (again, 2e-3f, 2e-4f, 400);
        for (long h : x) std::printf (" %ld", h);
        std::printf ("\n");
        for (long h : y) std::printf (" %ld", h);
        std::printf ("\n");
    }
    // A one-bar loop (2 s at 120 BPM) told a block late: hits at the bar's 16ths, never two at the wrap.
    Instance b;
    b.setup (patch (b));
    std::vector<float> l, r;
    const uint64_t bar = 96000;
    b.render (bar * 3, 128, l, r, [&] (uint64_t at) {
        // Melodee tells the place at its own blocks: a block that crosses the loop's end runs on past it.
        const uint64_t blockStart = at;
        const uint64_t wrapped = blockStart % bar;
        return Transport { true, 120.0, (double) wrapped * 2.0 / 48000.0 };
    });
    const auto hits = onsets (l, 2e-3f, 2e-4f, 400);
    for (size_t i = 1; i < hits.size(); ++i)
        CHECK (hits[i] - hits[i - 1] >= 5900);
}

void testBlockSizes()
{
    auto scene = [] (uint32_t block) {
        Instance a;
        a.loadPreset ("acid-gate");
        a.setup ({ { "Echo", 0.3 }, { "Space", 0.4 }, { "Crush", 0.2 }, { "Drift", 0.3 } });
        std::vector<float> l, r;
        a.render (48000 * 3, block, l, r, [] (uint64_t at) { return playingFrom (0.25, 133.0, 48000.0, at); }, [&] (uint64_t at, uint32_t n, Events& e) {
            const uint64_t when = 70001;
            if (when >= at && when < at + n) e.param ((uint32_t) (when - at), a.id ("Timbre"), 0.8);
            const uint64_t key = 50011;
            if (key >= at && key < at + n) e.note ((uint32_t) (key - at), CLAP_EVENT_NOTE_ON, 64, 0.8);
        });
        l.insert (l.end(), r.begin(), r.end());
        return l;
    };
    const auto reference = scene (128);
    for (uint32_t block : { 1u, 37u, 4096u })
        CHECK (scene (block) == reference);
}

// --- Notes and MIDI ------------------------------------------------------------------------------------------------
void testNotes()
{
    auto pure = [] (Instance& a) {
        a.setup ({ { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 }, { "Drift", 0.0 }, { "Bend", 0.0 }, { "Echo", 0.0 },
                   { "Space", 0.0 }, { "Sequencer", 0.0 }, { "Open", 1.0 }, { "Tune", 0.0 } });
    };
    // Pitch: key 72 an octave over key 60 (C2, 130.8 Hz).
    {
        Instance a;
        pure (a);
        const auto out = a.keys (48000, { { 0, 60, 0.8, true }, { 24000, 72, 0.8, true } });
        CHECK_NEAR (pitchOf (out, 12000, 23000, 48000.0), 130.81, 1.0);
        CHECK_NEAR (pitchOf (out, 40000, 47000, 48000.0), 261.63, 2.0);
    }
    // Glide: an overlapping key slides (Glide's time); a key on its own jumps.
    {
        Instance a;
        pure (a);
        a.setup ({ { "Glide", 1.0 } });
        const auto out = a.keys (48000, { { 0, 60, 0.8, true }, { 12000, 72, 0.8, true } });
        const double during = pitchOf (out, 13000, 16000, 48000.0);
        CHECK (during > 135.0 && during < 250.0);
    }
    // Strokes: keys 36-43 each play; MIDI notes too.
    {
        Instance a;
        a.setup ({ { "Notes", a.valueOf ("Notes", "Strokes") }, { "Sequencer", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 } });
        std::vector<std::tuple<uint32_t, int, double, bool>> keys;
        for (int k = 0; k < 8; ++k) keys.push_back ({ (uint32_t) (k * 12000), 36 + k, 0.9, true });
        const auto out = a.keys (12000 * 8, keys);
        for (int k = 0; k < 8; ++k)
            CHECK (peakOf (out, (size_t) (k * 12000), (size_t) (k * 12000 + 6000)) > 0.01);
    }
    // MIDI: note on plays, All Sound Off (CC 120 at 0) silences now, CC 123 lets held notes go; a choke stops.
    {
        Instance a;
        a.setup ({ { "Sequencer", 0.0 }, { "Open", 0.5 }, { "Echo", 0.3 }, { "Space", 0.3 } });
        std::vector<float> l, r;
        a.render (48000, 128, l, r, [] (uint64_t) { return stopped(); }, [] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) e.midi (0, 0x90, 60, 100);
            if (at == 128 * 100) e.midi (5, 0xb0, 120, 0);
        });
        CHECK (peakOf (l, 0, 12800) > 0.01);
        CHECK (peakOf (l, 12800 + 128, 48000) < 0.02); // Open still lets a little of the oscillator through
    }
    {
        Instance a;
        a.setup ({ { "Sequencer", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Decay", 1.0 } });
        std::vector<float> l, r;
        a.render (48000, 128, l, r, [] (uint64_t) { return stopped(); }, [] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) e.note (0, CLAP_EVENT_NOTE_ON, 60, 1.0);
            if (at == 128 * 50) e.note (0, CLAP_EVENT_NOTE_CHOKE, 60, 0.0);
        });
        CHECK (peakOf (l, 6400 + 64, 48000) < 1e-3);
    }
    // With the sequencer playing, a key transposes the pattern instead of playing.
    {
        Instance a;
        auto v = clickPatch();
        v.push_back ({ "Source", 0.0 });
        v.push_back ({ "Open", 1.0 });
        v.push_back ({ "FM Index", 0.0 });
        v.push_back ({ "Tune", 0.0 });
        a.setup (v);
        std::vector<float> l, r;
        a.render (48000 * 2, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); }, [] (uint64_t at, uint32_t, Events& e) {
            if (at == 128 * 375) e.note (0, CLAP_EVENT_NOTE_ON, 72, 1.0);
        });
        CHECK_NEAR (pitchOf (l, 20000, 47000, 48000.0), 130.81, 1.5);
        CHECK_NEAR (pitchOf (l, 60000, 95000, 48000.0), 261.63, 3.0);
    }
}

void testRegenerate()
{
    Instance a;
    const double before = a.value ("Seed");
    OutEvents out;
    std::vector<float> l, r;
    a.render (256, 128, l, r, [] (uint64_t) { return stopped(); }, [&] (uint64_t at, uint32_t, Events& e) {
        if (at == 0) e.param (0, a.id ("Regenerate"), 1.0);
    }, &out);
    const double after = a.value ("Seed");
    CHECK (after != before);
    bool told = false, released = false;
    for (auto& [id, v] : out.values)
    {
        told |= id == a.id ("Seed") && v == after;
        released |= id == a.id ("Regenerate") && v == 0.0;
    }
    CHECK (told && released);
}

void testExtremes()
{
    std::mt19937 random (12345);
    for (int trial = 0; trial < 24; ++trial)
    {
        Instance a (trial % 3 == 0 ? 44100.0 : (trial % 3 == 1 ? 48000.0 : 96000.0));
        std::vector<std::pair<std::string, double>> v;
        for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
        {
            clap_param_info info {};
            a.params->get_info (a.plugin, i, &info);
            if (std::string (info.name) == "Regenerate") continue;
            std::uniform_real_distribution<double> u (0.0, 1.0);
            const double pick = u (random);
            double x = pick < 0.25 ? info.min_value : (pick < 0.5 ? info.max_value : info.min_value + u (random) * (info.max_value - info.min_value));
            if (info.flags & CLAP_PARAM_IS_STEPPED) x = std::round (x);
            v.push_back ({ info.name, x });
        }
        v.push_back ({ "Volume", 1.0 });
        a.setup (v);
        std::vector<float> l, r;
        a.render ((uint32_t) (a.rate * 3), 128, l, r, [&] (uint64_t at) { return playingFrom (0.0, 40.0 + trial * 17.0, a.rate, at); },
                  [&] (uint64_t at, uint32_t, Events& e) {
                      if (at % 4096 == 0) e.note (0, CLAP_EVENT_NOTE_ON, (int16_t) (24 + (at / 4096) % 80), 1.0);
                  });
        CHECK (finiteAndBounded (l) && finiteAndBounded (r));
    }
    // Extreme tempos: no hang, no NaN.
    for (double bpm : { 1e-3, 1.0, 1000.0, 1e9, (double) INFINITY, (double) NAN })
    {
        Instance a;
        a.setup ({ { "Rate", a.valueOf ("Rate", "1/64") } });
        std::vector<float> l, r;
        a.render (24000, 128, l, r, [&] (uint64_t at) { return Transport { true, bpm, std::isfinite (bpm) ? std::min (1e6, (double) at * bpm / 60.0 / 48000.0) : 0.0 }; });
        CHECK (finiteAndBounded (l));
    }
}

// --- The page ------------------------------------------------------------------------------------------------------
void testScreen (const std::string& folder)
{
    Instance a;
    const auto* screen = static_cast<const clap_plugin_melodee_screen*> (a.plugin->get_extension (a.plugin, CLAP_EXT_MELODEE_SCREEN));
    CHECK (screen != nullptr);
    if (screen == nullptr) return;
    std::vector<uint32_t> pixels (MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_HEIGHT, 0);
    melodee_screen_canvas_t canvas {};
    canvas.pixels = pixels.data();
    canvas.width = MELODEE_SCREEN_WIDTH, canvas.height = MELODEE_SCREEN_HEIGHT, canvas.stride = MELODEE_SCREEN_WIDTH;
    canvas.accent = 0xffff8a3d;
    for (auto& k : canvas.knobs) k = { -1, true };
    uint64_t now = 1000;
    auto paint = [&] (bool whole = false) { canvas.whole = whole, canvas.now_ms = now; return screen->paint (a.plugin, &canvas); };
    auto shot = [&] (const std::string& name) { paint (true); CHECK (writePng ((folder + "/screen-" + name + ".png").c_str(), pixels.data(), 320, 218, 320, 2)); };
    auto in = [&] (uint32_t kind, int value, bool shift = false) { melodee_screen_input_t e { kind, value, shift }; return screen->input (a.plugin, &e); };
    auto knobs = [&] () { std::vector<clap_id> ids (3); screen->knobs (a.plugin, ids.data()); return ids; };
    auto play = [&] (uint32_t frames, bool playing, double from = 0.0) {
        std::vector<float> l, r;
        a.render (frames, 128, l, r, [&] (uint64_t at) { return playing ? playingFrom (from, 120.0, 48000.0, at) : stopped(); });
        now += frames * 1000 / 48000;
        return l;
    };

    // Still and silent: one whole paint, then nothing to draw and no paints asked for.
    CHECK (paint (true));
    play (4800, false);
    now += 100;
    CHECK (! paint());
    int asked = paints;
    now += 100;
    CHECK (! paint());
    CHECK (paints == asked);
    // PLAY: K1-K3 hold the TONE trio; the wheel turns to the next.
    CHECK ((knobs() == std::vector<clap_id> { a.id ("Timbre"), a.id ("Decay"), a.id ("Resonance") }));
    CHECK (in (MELODEE_SCREEN_WHEEL, 1));
    CHECK ((knobs() == std::vector<clap_id> { a.id ("Strike"), a.id ("Bloom"), a.id ("Accent") }));
    CHECK (in (MELODEE_SCREEN_WHEEL, -1));
    // Playing: the page asks to be painted as the steps go by and the light glows.
    canvas.playing = true;
    asked = paints;
    play (48000, true);
    CHECK (paints > asked + 4);
    shot ("play");
    // Push stops the sequencer (a value told to the host), Push again starts it.
    const int processBefore = processRequests, rescanBefore = rescans;
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    CHECK (processRequests > processBefore && rescans > rescanBefore);
    CHECK (a.value ("Sequencer") == 0.0);
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    CHECK (a.value ("Sequencer") == 1.0);
    // SEQ: the wheel picks step 3; Push cycles its trig; K1-K3 are its Pitch, Stroke and Chance.
    CHECK (in (MELODEE_SCREEN_NUDGE, 1));
    CHECK (in (MELODEE_SCREEN_WHEEL, 2));
    CHECK ((knobs() == std::vector<clap_id> { a.id ("Pitch 3"), a.id ("Stroke 3"), a.id ("Chance 3") }));
    const double trig = a.value ("Trig 3");
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    CHECK (a.value ("Trig 3") == std::fmod (trig + 1.0, 5.0));
    play (24000, true, 4.0);
    shot ("seq");
    // Shift + Push plays the step (stopped); a press kept waiting over half a second is dropped.
    a.setup ({ { "Echo", 0.0 }, { "Space", 0.0 }, { "Decay", 0.0 }, { "Ring", 0.0 } });
    play (96000, false);
    CHECK (in (MELODEE_SCREEN_PUSH, 0, true));
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    CHECK (peakOf (play (4800, false)) < 1e-4);
    CHECK (in (MELODEE_SCREEN_PUSH, 0, true));
    CHECK (peakOf (play (9600, false)) > 1e-3);
    // The lists: SKIN, OSC, GATE, FX, MODE; Push edits, the wheel changes the value, Push ends.
    const char* names[] = { "skin", "osc", "gate", "fx", "mode" };
    for (const char* name : names)
    {
        CHECK (in (MELODEE_SCREEN_NUDGE, 1));
        play (12000, true, 8.0);
        shot (name);
    }
    CHECK ((knobs() == std::vector<clap_id> { a.id ("Sequencer"), a.id ("Seq Mode"), a.id ("Rate") }));
    CHECK (in (MELODEE_SCREEN_WHEEL, 1));
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    CHECK (in (MELODEE_SCREEN_WHEEL, 2));
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    CHECK (a.value ("Seq Mode") == 2.0);
    for (const char* mode : { "Euclid", "Turing", "Automaton", "Bounce", "Rhythm" })
    {
        a.setup ({ { "Seq Mode", a.valueOf ("Seq Mode", mode) } });
        play (48000, true, 0.0);
        std::string n = "mode-";
        for (const char* c = mode; *c; ++c) n += (char) std::tolower (*c);
        shot (n);
    }
    // Minus and Plus go round; Shift shows the controls.
    CHECK (in (MELODEE_SCREEN_NUDGE, 1));
    canvas.shift = true;
    shot ("shift");
    canvas.shift = false;
    // Every preset draws on every page (a whole paint each).
    for (const char* key : { "acid-gate", "singing-bowl", "feedback-witch", "tabla-teental" })
    {
        CHECK (a.loadPreset (key));
        for (int p = 0; p < 7; ++p)
        {
            in (MELODEE_SCREEN_NUDGE, 1);
            play (2400, true, 0.0);
            CHECK (paint (true));
        }
    }
    CHECK (a.loadPreset ("mystik"));
    for (int p = 0; p < 7; ++p) in (MELODEE_SCREEN_NUDGE, -1);
    play (24000, true, 0.0);
    shot ("play-default");
}

// --- Demos ---------------------------------------------------------------------------------------------------------
void demos (const std::string& folder)
{
    const char* keys[] = { "mystik", "west-bongo", "tabla-teental", "conga-tumbao", "udu-bloom", "acid-gate", "turing-acid",
                           "singing-bowl", "slit-euclid", "handpan-drift", "maqsum-night", "feedback-witch", "bouncing-ball", "gankogui-bell",
                           "goblet-maqsum" };
    for (const char* key : keys)
    {
        Instance a;
        a.loadPreset (key);
        std::vector<float> l, r;
        a.render (48000 * 10, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 112.0, 48000.0, at); });
        CHECK (writeWav ((folder + "/demo-" + key + ".wav").c_str(), l, r, 48000));
        std::printf ("  demo %-15s peak %5.2f dB  rms %6.2f dB\n", key, 20.0 * std::log10 (std::max (1e-9, peakOf (l))),
                     20.0 * std::log10 (std::max (1e-9, rmsOf (l, 0, l.size()))));
    }
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: host Mystik.clap OUT_FOLDER\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        std::printf ("FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    entry = static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry"));
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL no clap_entry\n");
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    const std::string only = std::getenv ("MYSTIK_ONLY") ? std::getenv ("MYSTIK_ONLY") : "";
    auto run = [&] (const char* name, const std::function<void()>& test) {
        if (only.empty() || only.find (name) != std::string::npos)
        {
            const int before = failures;
            test();
            std::printf ("%-12s %s\n", name, failures == before ? "ok" : "FAILED");
        }
    };
    run ("description", testDescription);
    run ("parameters", testParameters);
    run ("state", testState);
    run ("presets", testPresets);
    run ("gate", testGateResponse);
    run ("vactrol", testVactrol);
    run ("timing", testTiming);
    run ("modes", testModes);
    run ("transport", testTransport);
    run ("blocks", testBlockSizes);
    run ("notes", testNotes);
    run ("regenerate", testRegenerate);
    run ("extremes", testExtremes);
    run ("screen", [&] { testScreen (argv[2]); });
    run ("demos", [&] { demos (argv[2]); });
    entry->deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
