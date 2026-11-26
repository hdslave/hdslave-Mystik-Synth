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
#include "../src/fx.h"    // the echo's reset, held to its set time in testEchoReset

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
    static const melodee_screen_host_t screen { [] (const void*, uint32_t) { ++paints; } };
    static const clap_host_preset_load presetLoad {
        [] (const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) { ++presetErrors; },
        [] (const clap_host*, uint32_t, const char*, const char*) { ++presetsLoaded; },
    };
    if (std::strcmp (id, MELODEE_SCREEN_CLAP_HOST) == 0) return &screen;
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
    std::vector<uint32_t> flags; // the header flags of each value, in step
    clap_output_events out { this, [] (const clap_output_events* o, const clap_event_header* h) {
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* v = reinterpret_cast<const clap_event_param_value*> (h);
            auto* self = static_cast<OutEvents*> (o->ctx);
            self->values.push_back ({ v->param_id, v->value });
            self->flags.push_back (h->flags);
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
    CHECK (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP) != nullptr);
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
            // A step's Pitch reads as a note of the live Scale from the live Tune, and that note's name reads back to
            // the degree it came from (the instance's Tune and Scale have not moved yet in this loop).
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
    CHECK (a.params->text_to_value (a.plugin, a.id ("Pitch 1"), "B2+40c", &v) && v == 1.0); // microtonal names read back too
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
    const uint32_t count = mystik::paramCount + 1; // one more pair than the state was saved with: it is really read
    std::memcpy (extra.data() + 8, &count, 4);
    const struct { uint32_t id; float v; } unknown { 9999, 1.0f };
    extra.insert (extra.end(), (const uint8_t*) &unknown, (const uint8_t*) &unknown + 8);
    Instance d;
    CHECK (load (d, extra) && d.value ("Body") == 6);
    // A state from a newer version is refused, changing nothing (it may mean things this build cannot know).
    auto future = saved;
    const uint32_t version2 = 2;
    std::memcpy (future.data() + 4, &version2, 4);
    Instance e;
    e.setup ({ { "Body", 3 } });
    CHECK (! load (e, future));
    CHECK (e.value ("Body") == 3);
    // A 0.1 state (no Human, which came in 0.2): loads with Human at 0, as 0.1 sounded, not at the 0.3 default; and
    // sounds exactly as the same state with Human 0 written in it.
    const clap_id humanId = b.id ("Human");
    std::vector<uint8_t> old01 (saved.begin(), saved.begin() + 12);
    uint32_t pairs = 0;
    for (size_t at = 12; at + 8 <= saved.size(); at += 8)
    {
        uint32_t id = 0;
        std::memcpy (&id, saved.data() + at, 4);
        if (id != humanId)
            old01.insert (old01.end(), saved.begin() + (long) at, saved.begin() + (long) at + 8), ++pairs;
    }
    std::memcpy (old01.data() + 8, &pairs, 4);
    Instance f;
    CHECK (load (f, old01));
    CHECK (f.value ("Human") == 0.0);
    auto explicit0 = old01;
    const struct { uint32_t id; float v; } human0 { humanId, 0.0f };
    explicit0.insert (explicit0.end(), (const uint8_t*) &human0, (const uint8_t*) &human0 + 8);
    ++pairs;
    std::memcpy (explicit0.data() + 8, &pairs, 4);
    Instance g, h;
    CHECK (load (g, old01) && load (h, explicit0));
    CHECK (g.play (24000) == h.play (24000));
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
    // CC 121 (Reset All Controllers, at any value) puts the bend back at rest; 120, 121 and 123 move no parameter.
    {
        Instance a;
        pure (a);
        std::vector<double> before;
        for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
        {
            clap_param_info info {};
            a.params->get_info (a.plugin, i, &info);
            double v = 0.0;
            a.params->get_value (a.plugin, info.id, &v);
            before.push_back (v);
        }
        std::vector<float> l, r;
        a.render (48000 * 2, 128, l, r, [] (uint64_t) { return stopped(); }, [] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) e.midi (0, 0x90, 60, 100), e.midi (1, 0xe0, 127, 127), e.midi (2, 0xb0, 1, 127); // bend +2, mod wheel up
            if (at == 48000) e.midi (0, 0xb0, 121, 127), e.midi (1, 0xb0, 120, 5), e.midi (2, 0xb0, 123, 9);
        });
        // Bent and folded (the fold's extra zero crossings read as a higher pitch), then back: the plain C2.
        CHECK (pitchOf (l, 12000, 47000, 48000.0) > 140.0);
        CHECK_NEAR (pitchOf (l, 60000, 95000, 48000.0), 130.81, 1.0);
        for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
        {
            clap_param_info info {};
            a.params->get_info (a.plugin, i, &info);
            double v = 0.0;
            a.params->get_value (a.plugin, info.id, &v);
            CHECK (v == before[i]);
        }
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

// Hold lights the gate only while a note (or a step's gate) is held: the further hits of a flam, a roll or a bounce that
// come after the note has ended, or after the step's gate, must not light it again (0.2.1 droned on for ever).
void testHold()
{
    const std::vector<std::pair<std::string, double>> dry { { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 }, { "Decay", 0.0 },
                                                            { "Ring", 0.0 }, { "Sequencer", 0.0 } };
    // Strokes: key 41 (Flam) let go before its main hit (22 ms), key 42 (Roll) before its second and its last (106 ms).
    for (auto [key, offAt] : { std::pair { 41, 480u }, std::pair { 42, 2000u }, std::pair { 42, 4800u } })
    {
        Instance a;
        a.setup (dry);
        a.setup ({ { "Notes", a.valueOf ("Notes", "Strokes") } });
        const auto out = a.keys (48000 * 3, { { 0, key, 0.9, true }, { offAt, key, 0.0, false } });
        CHECK (peakOf (out, 0, 4800) > 0.01);
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
    }
    // Held through its further hits, it stays lit while held, and ends when let go.
    {
        Instance a;
        a.setup (dry);
        a.setup ({ { "Notes", a.valueOf ("Notes", "Strokes") } });
        const auto out = a.keys (48000 * 3, { { 0, 42, 0.9, true }, { 24000, 42, 0.0, false } });
        CHECK (peakOf (out, 12000, 24000) > 1e-3);
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
    }
    // Steps: step 1 a Flam, its key let go before the main hit.
    {
        Instance a;
        a.setup (dry);
        a.setup ({ { "Notes", a.valueOf ("Notes", "Steps") }, { "Stroke 1", a.valueOf ("Stroke 1", "Flam") } });
        const auto out = a.keys (48000 * 3, { { 0, 36, 0.9, true }, { 480, 36, 0.0, false } });
        CHECK (peakOf (out, 0, 4800) > 0.01);
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
    }
    // The sequencer: Acid Gate stopped during step 12 (a Roll). The step starts at beat 2.76 (Swing 52 %), frame 66,240 at
    // 120 BPM; the roll's hits at 0, 2,236, 3,801 and 4,897 frames from there, its gate ends at 3,168.
    {
        Instance a;
        CHECK (a.loadPreset ("acid-gate"));
        a.setup ({ { "Echo", 0.0 }, { "Space", 0.0 } });
        const uint64_t stopAt = 67328; // the first block after the roll's first hit
        std::vector<float> l, r;
        a.render (48000 * 4, 128, l, r, [&] (uint64_t at) { return at < stopAt ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); });
        CHECK (peakOf (l, 66240, 67328) > 0.01);
        CHECK (peakOf (l, 48000 * 3) < 1e-5);
    }
    // Bounce: stopped while the ball still has bounces to come.
    {
        Instance a;
        a.setup (dry);
        a.setup ({ { "Sequencer", 1.0 }, { "Seq Mode", a.valueOf ("Seq Mode", "Bounce") } });
        std::vector<float> l, r;
        a.render (48000 * 4, 128, l, r, [&] (uint64_t at) { return at < 14400 ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); });
        CHECK (peakOf (l, 0, 4800) > 0.01);
        CHECK (peakOf (l, 48000 * 3) < 1e-5);
    }
    // The page's Shift + Push on SEQ plays step 1 stopped: lit for its gate, then it ends.
    {
        Instance a;
        a.setup (dry);
        const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
        melodee_screen_input_t next { MELODEE_SCREEN_STEP, 1, 0 }, play { MELODEE_SCREEN_PUSH, 0, 1 };
        CHECK (screen != nullptr && screen->input (a.plugin, &next) && screen->input (a.plugin, &play));
        const auto out = a.play (48000 * 3, false);
        CHECK (peakOf (out, 0, 4800) > 0.01);
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
    }
}

// --- Step pads -----------------------------------------------------------------------------------------------------
// With the sequencer on, keys 36-51 (pads 1-16 on a plug-in track at hdslaveOS's defaults) play steps 1-16, the host
// playing or stopped, as Notes: Steps plays them, without moving the sequence or the transposition. Other keys do what
// they did before; with the sequencer off, Notes decides.
struct PadEvent
{
    uint64_t frame;
    enum Kind { on, off, midi, param, choke } kind;
    int key;              // a note's key, a MIDI status
    double value = 0.9;   // a note's velocity, a MIDI message's data bytes as 256 * first + second, a parameter's value
    std::string name {};  // the parameter's
};
PadEvent padOn (uint64_t f, int key, double v = 0.9) { return { f, PadEvent::on, key, v }; }
PadEvent padOff (uint64_t f, int key) { return { f, PadEvent::off, key, 0.0 }; }
PadEvent midiEvent (uint64_t f, int status, int a, int b) { return { f, PadEvent::midi, status, (double) (a * 256 + b) }; }
PadEvent paramEvent (uint64_t f, const std::string& name, double v) { return { f, PadEvent::param, 0, v, name }; }

// Renders the events at their frames (in list order on a frame); left and right one after the other.
std::vector<float> perform (Instance& a, uint32_t frames, const std::vector<PadEvent>& list, const std::function<Transport (uint64_t)>& transport,
                            uint32_t block = 128, bool both = true)
{
    std::vector<float> l, r;
    a.render (frames, block, l, r, transport, [&] (uint64_t at, uint32_t n, Events& e) {
        for (const auto& v : list)
            if (v.frame >= at && v.frame < at + n)
            {
                const uint32_t t = (uint32_t) (v.frame - at);
                if (v.kind == PadEvent::on || v.kind == PadEvent::off || v.kind == PadEvent::choke)
                    e.note (t, v.kind == PadEvent::on ? CLAP_EVENT_NOTE_ON : (v.kind == PadEvent::off ? CLAP_EVENT_NOTE_OFF : CLAP_EVENT_NOTE_CHOKE),
                            (int16_t) v.key, v.value);
                else if (v.kind == PadEvent::midi)
                    e.midi (t, (uint8_t) v.key, (uint8_t) ((int) v.value >> 8), (uint8_t) ((int) v.value & 0xff));
                else
                    e.param (t, a.id (v.name), v.value);
            }
    });
    if (both)
        l.insert (l.end(), r.begin(), r.end());
    return l;
}
std::function<Transport (uint64_t)> stoppedAll() { return [] (uint64_t) { return stopped(); }; }
std::function<Transport (uint64_t)> playingAll (double bpm = 120.0, double from = 0.0) { return [=] (uint64_t at) { return playingFrom (from, bpm, 48000.0, at); }; }

// A sine at the drum's pitch through a gate held open: its pitch can be read at any time. Steps: all Hits, Open
// strokes, these degrees of the chromatic scale from C2 (130.81 Hz); Rate 1/4 (24,000 frames a step at 120 BPM).
constexpr int padDegree[16] = { 0, 2, 4, 5, 7, 9, 11, 12, -1, -3, -5, -7, 3, 6, 8, 10 };
double hzOf (double semis) { return 130.81 * std::pow (2.0, semis / 12.0); }
std::vector<std::pair<std::string, double>> dronePatch (Instance& a)
{
    std::vector<std::pair<std::string, double>> v { { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 }, { "Drift", 0.0 },
                                                    { "Bend", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Open", 1.0 }, { "Tune", 0.0 },
                                                    { "Human", 0.0 }, { "Scale", a.valueOf ("Scale", "Chromatic") }, { "Swing", 0.5 },
                                                    { "Mutate", 0.0 }, { "Density", 0.5 }, { "Hold", 0.0 }, { "Rate", a.valueOf ("Rate", "1/4") } };
    for (int s = 1; s <= 16; ++s)
    {
        v.push_back ({ "Trig " + std::to_string (s), 1.0 });
        v.push_back ({ "Stroke " + std::to_string (s), 0.0 });
        v.push_back ({ "Chance " + std::to_string (s), 1.0 });
        v.push_back ({ "Pitch " + std::to_string (s), (double) padDegree[s - 1] });
    }
    return v;
}
#define CHECK_PITCH(out, from, to, semis) CHECK_NEAR (pitchOf (out, (size_t) (from), (size_t) (to), 48000.0), hzOf (semis), hzOf (semis) * 0.012)

void testPads()
{
    const bool debug = std::getenv ("MYSTIK_DEBUG") != nullptr;
    // Each pad, stopped, plays its step exactly as Notes: Steps plays that key with the sequencer off: pitch, stroke
    // (the default steps' strokes, and a flam and two rolls), trig's accent, a hard hit's accent, Human.
    const std::vector<std::pair<std::string, double>> strokes { { "Stroke 2", 5.0 }, { "Stroke 6", 6.0 }, { "Stroke 10", 6.0 } };
    for (int s = 0; s < 16; ++s)
    {
        const double v = s % 2 ? 1.0 : 0.6;
        const std::vector<PadEvent> list { padOn (100, 36 + s, v), padOff (6000, 36 + s) };
        Instance a, b;
        a.setup (strokes);
        b.setup (strokes);
        b.setup ({ { "Sequencer", 0.0 }, { "Notes", b.valueOf ("Notes", "Steps") } });
        const auto pad = perform (a, 12000, list, stoppedAll());
        CHECK (pad == perform (b, 12000, list, stoppedAll()));
        CHECK (peakOf (pad) > 1e-3);
    }
    // In every mode it plays the step's own Pitch, Trig and Stroke (what Notes: Steps plays), whatever the mode does
    // with them; MIDI notes (hdslaveOS's dialect) as CLAP notes.
    for (const char* mode : { "Euclid", "Turing", "Automaton", "Bounce", "Rhythm" })
    {
        Instance a, b;
        a.setup ({ { "Seq Mode", a.valueOf ("Seq Mode", mode) } });
        b.setup ({ { "Seq Mode", b.valueOf ("Seq Mode", mode) }, { "Sequencer", 0.0 }, { "Notes", b.valueOf ("Notes", "Steps") } });
        const std::vector<PadEvent> list { midiEvent (100, 0x90, 47, 100), midiEvent (3000, 0x80, 47, 0), padOn (9000, 40, 0.5), padOff (12000, 40) };
        CHECK (perform (a, 24000, list, stoppedAll()) == perform (b, 24000, list, stoppedAll()));
    }
    // As the sequence plays it: an accented step struck at full velocity is the sequence's first step (Rotate) to the
    // sample, at 1/16 (a pad's roll spans what a 1/16 step's does), for each stroke.
    for (int stroke = 0; stroke < 8; ++stroke)
    {
        const int s = (stroke * 5 + 3) % 16;
        const std::vector<std::pair<std::string, double>> v { { "Trig " + std::to_string (s + 1), 2.0 }, { "Stroke " + std::to_string (s + 1), (double) stroke },
                                                              { "Chance " + std::to_string (s + 1), 1.0 }, { "Rotate", (double) s }, { "Rate", 5.0 },
                                                              { "Swing", 0.5 }, { "Hold", 0.0 } };
        Instance a, b;
        a.setup (v);
        b.setup (v);
        const auto pad = perform (a, 5900, { padOn (0, 36 + s, 1.0) }, stoppedAll());
        const auto step = perform (b, 5900, {}, playingAll());
        CHECK (pad == step);
    }
    // Stopped, each pad sets its step's pitch; Play then starts from step 1, untransposed.
    {
        Instance a;
        a.setup (dronePatch (a));
        std::vector<PadEvent> list;
        for (int s = 0; s < 16; ++s)
            list.push_back (padOn ((uint64_t) s * 12000 + 100, 36 + s, s % 3 ? 0.5 : 0.95)), list.push_back (padOff ((uint64_t) s * 12000 + 6000, 36 + s));
        const uint64_t play = 16 * 12000;
        const auto out = perform (a, (uint32_t) play + 48000, list, [&] (uint64_t at) { return at < play ? stopped() : playingFrom (0.0, 120.0, 48000.0, at - play); }, 128, false);
        for (int s = 0; s < 16; ++s)
            CHECK_PITCH (out, s * 12000 + 2000, s * 12000 + 11500, padDegree[s]);
        CHECK_PITCH (out, play + 2000, play + 22000, padDegree[0]);
        CHECK_PITCH (out, play + 26000, play + 46000, padDegree[1]);
    }
    // Playing: a pad plays its step until the sequence's next step, which is the one it would have played anyway (the
    // play head does not move, the transposition stays).
    {
        Instance a;
        a.setup (dronePatch (a));
        const auto out = perform (a, 24000 * 8, { padOn (36000, 44), padOff (40000, 44), padOn (84000, 51, 1.0), padOff (90000, 51) }, playingAll(), 128, false);
        CHECK_PITCH (out, 26000, 35000, padDegree[1]);
        CHECK_PITCH (out, 38000, 47000, padDegree[8]);
        CHECK_PITCH (out, 50000, 71000, padDegree[2]);
        CHECK_PITCH (out, 74000, 83000, padDegree[3]);
        CHECK_PITCH (out, 86000, 95000, padDegree[15]);
        CHECK_PITCH (out, 98000, 119000, padDegree[4]);
        CHECK_PITCH (out, 146000, 167000, padDegree[6]);
    }
    // Generative modes too: Turing's register moves on as it would have (each step's pitch as without the pads).
    {
        auto turing = [] (bool pads) {
            Instance a;
            auto v = dronePatch (a);
            v.push_back ({ "Seq Mode", a.valueOf ("Seq Mode", "Turing") });
            v.push_back ({ "Density", 1.0 });
            v.push_back ({ "Mutate", 0.35 });
            v.push_back ({ "Range", 1.0 });
            v.push_back ({ "Glide", 0.0 }); // its slides land within 5 ms
            a.setup (v);
            std::vector<PadEvent> list;
            if (pads)
                for (int k : { 1, 4, 7 }) list.push_back (padOn ((uint64_t) k * 24000 + 12000, 36 + k * 2)), list.push_back (padOff ((uint64_t) k * 24000 + 13000, 36 + k * 2));
            return perform (a, 24000 * 10, list, playingAll(), 128, false);
        };
        const auto with = turing (true), without = turing (false);
        int differ = 0;
        for (int k = 0; k < 10; ++k)
        {
            const double p = pitchOf (with, (size_t) k * 24000 + 2000, (size_t) k * 24000 + 11500, 48000.0);
            const double q = pitchOf (without, (size_t) k * 24000 + 2000, (size_t) k * 24000 + 11500, 48000.0);
            CHECK_NEAR (p, q, q * 0.005);
            differ += std::fabs (p - q) > q * 0.005;
        }
        CHECK (differ == 0);
    }
    // Other keys still transpose while it plays (35 and 52, either side of the pads, too); a pad plays its step with
    // the transposition and leaves it as it was.
    {
        Instance a;
        a.setup (dronePatch (a));
        const auto out = perform (a, 24000 * 6, { padOn (30000, 64), padOff (31000, 64), padOn (54000, 35), padOff (55000, 35), padOn (78000, 52),
                                                  padOff (79000, 52), padOn (102000, 36), padOff (103000, 36) }, playingAll(), 128, false);
        CHECK_PITCH (out, 50000, 71000, padDegree[2] + 4);
        CHECK_PITCH (out, 74000, 95000, padDegree[3] - 25);
        CHECK_PITCH (out, 98000, 101500, padDegree[4] - 8);
        CHECK_PITCH (out, 104000, 119000, padDegree[0] - 8);
        CHECK_PITCH (out, 122000, 143000, padDegree[5] - 8);
    }
    // Stopped, the other keys play the drum as Notes says (Pitch: from C3).
    {
        Instance a;
        a.setup (dronePatch (a));
        const auto out = perform (a, 36000, { padOn (100, 64), padOff (6000, 64), padOn (12000, 53), padOff (18000, 53), padOn (24000, 35), padOff (30000, 35) },
                                  stoppedAll(), 128, false);
        CHECK_PITCH (out, 2000, 11500, 4);
        CHECK_PITCH (out, 14000, 23500, -7);
        CHECK_PITCH (out, 26000, 35500, -25);
    }
    // Sequencer off: keys 36-51 are a chromatic keyboard again, stopped or playing; switched on, the pads are steps.
    for (bool playing : { false, true })
    {
        Instance a;
        auto v = dronePatch (a);
        v.push_back ({ "Sequencer", 0.0 });
        a.setup (v);
        const auto out = perform (a, 12000 * 6, { padOn (100, 48), padOff (6000, 48), padOn (12000, 51), padOff (18000, 51), padOn (24000, 44),
                                                  padOff (30000, 44), paramEvent (36000, "Sequencer", 1.0), padOn (36000, 44), padOff (42000, 44),
                                                  paramEvent (48000, "Sequencer", 0.0), padOn (48000, 40), padOff (54000, 40),
                                                  paramEvent (60000, "Sequencer", 1.0), padOn (60000, 40), padOff (66000, 40) },
                                  playing ? playingAll() : stoppedAll(), 128, false);
        CHECK_PITCH (out, 2000, 11500, -12);
        CHECK_PITCH (out, 14000, 23500, -9);
        CHECK_PITCH (out, 26000, 35500, -16);
        CHECK_PITCH (out, 38000, 47500, padDegree[8]);
        CHECK_PITCH (out, 50000, 59500, -20);
        CHECK_PITCH (out, 62000, 71500, padDegree[4]);
    }
    // Two pads on one frame: the louder plays, once (Human would show a second strike); on a tie the later. Stopped and
    // playing.
    for (bool playing : { false, true })
    {
        const uint64_t at = playing ? 30500 : 100;
        auto run = [&] (std::vector<PadEvent> list) {
            Instance a;
            for (auto& e : std::vector<PadEvent> (list)) if (e.kind == PadEvent::on) list.push_back (padOff (at + 3000, e.key));
            return perform (a, 48000, list, playing ? playingAll() : stoppedAll());
        };
        const auto louder = run ({ padOn (at, 47, 0.9) });
        CHECK (run ({ padOn (at, 40, 0.5), padOn (at, 47, 0.9) }) == louder);
        CHECK (run ({ padOn (at, 47, 0.9), padOn (at, 40, 0.5) }) == louder);
        CHECK (run ({ padOn (at, 40, 0.7), padOn (at, 47, 0.7) }) == run ({ padOn (at, 47, 0.7) }));
        CHECK (run ({ padOn (at, 47, 0.7), padOn (at, 40, 0.7) }) == run ({ padOn (at, 40, 0.7) }));
        CHECK (run ({ padOn (at, 40, 0.5), padOn (at, 47, 0.9) }) != run ({ padOn (at, 40, 0.5) }));
    }
    // On, on, off of one pad (a re-pressed pad): a new strike, not a second voice or a slide, and its Hold ends with the
    // one note off; stopped and playing. On one frame: the louder of the two once.
    {
        const std::vector<std::pair<std::string, double>> dry { { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 }, { "Decay", 0.0 }, { "Ring", 0.0 } };
        for (bool playing : { false, true })
        {
            auto run = [&] (const std::vector<PadEvent>& list, bool steps = false) {
                Instance a;
                a.setup (dry);
                if (steps)
                    a.setup ({ { "Sequencer", 0.0 }, { "Notes", a.valueOf ("Notes", "Steps") } });
                return perform (a, 48000 * 3, list, playing ? playingAll() : stoppedAll());
            };
            const uint64_t t = playing ? 3000 : 0;
            const auto again = run ({ padOn (t, 36), padOn (t + 12000, 36, 0.8), padOff (t + 24000, 36) });
            CHECK (again == run ({ padOn (t, 36), padOff (t + 12000, 36), padOn (t + 12000, 36, 0.8), padOff (t + 24000, 36) }));
            if (! playing)
            {
                CHECK (again == run ({ padOn (t, 36), padOn (t + 12000, 36, 0.8), padOff (t + 24000, 36) }, true));
                CHECK (peakOf (again, 12000, 24000) > 1e-3); // held, the gate stays lit
                CHECK (peakOf (again, 48000 * 2, 48000 * 3) < 1e-5);
            }
            const auto once = run ({ padOn (t + 100, 37, 0.6), padOn (t + 100, 37, 0.9), padOff (t + 100, 37) });
            CHECK (once == run ({ padOn (t + 100, 37, 0.9), padOff (t + 100, 37) }));
            if (! playing)
                CHECK (peakOf (once, 0, 4800) > 1e-3 && peakOf (once, 48000 * 2, 48000 * 3) < 1e-5);
        }
    }
    // A pad on Play's frame: the sequence's first step plays on that frame and wins (once); when the step rests, the pad
    // plays, and the sequence goes on from its second step. In MIDI too (hdslaveOS sends every MIDI event at frame 0).
    {
        Instance a, b, c;
        const auto alone = perform (b, 48000, {}, playingAll());
        CHECK (perform (a, 48000, { padOn (0, 44), padOff (3000, 44) }, playingAll()) == alone);
        CHECK (perform (c, 48000, { midiEvent (0, 0x90, 44, 127), midiEvent (3000, 0x80, 44, 0) }, playingAll()) == alone);
        Instance d;
        auto v = dronePatch (d);
        v.push_back ({ "Trig 1", 0.0 });
        d.setup (v);
        const auto rest = perform (d, 24000 * 3, { midiEvent (0, 0x90, 44, 90), midiEvent (0, 0x80, 44, 0) }, playingAll(), 128, false);
        CHECK_PITCH (rest, 2000, 23000, padDegree[8]);
        CHECK_PITCH (rest, 26000, 47000, padDegree[1]);
        CHECK_PITCH (rest, 50000, 71000, padDegree[2]);
    }
    // While the sequence plays with Hold: a pad held past the step's gate end stays lit until its note off; the steps
    // after it end their gates as before (the pad takes none of the sequence's gate ends away). And a pad's roll still to
    // come gives way to the next step.
    {
        // Steps at 1/4 (24,000 frames), their gates ending 6,000 frames in; the pad struck at 51,000, inside step 3's gate
        // (which ends at 54,000), held to 63,000 (or tapped).
        auto run = [] (uint64_t offAt) {
            Instance a;
            a.setup ({ { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 }, { "Decay", 0.0 }, { "Ring", 0.0 }, { "Rate", a.valueOf ("Rate", "1/4") },
                       { "Gate", 0.25 }, { "Swing", 0.5 }, { "Human", 0.0 } });
            for (int s = 1; s <= 16; ++s) a.setup ({ { "Trig " + std::to_string (s), 1.0 }, { "Stroke " + std::to_string (s), 0.0 }, { "Chance " + std::to_string (s), 1.0 } });
            std::vector<PadEvent> list;
            if (offAt > 0) list = { padOn (51000, 44), padOff (offAt, 44) };
            return perform (a, 24000 * 6, list, playingAll(), 128, false);
        };
        const auto held = run (63000), longer = run (71500), tapped = run (51200), without = run (0);
        if (debug)
        {
            std::printf ("  pad hold: held %.6f, tapped %.6f, without %.6f; later held %.6f %.6f, without %.6f %.6f\n", peakOf (held, 58000, 62000),
                         peakOf (tapped, 58000, 62000), peakOf (without, 58000, 62000), peakOf (held, 72000, 78000), peakOf (held, 84000, 95000),
                         peakOf (without, 72000, 78000), peakOf (without, 84000, 95000));
            for (size_t w = 62000; w < 72000; w += 1000)
                std::printf ("    %zu: held %.6f longer %.6f\n", w, peakOf (held, w, w + 1000), peakOf (longer, w, w + 1000));
        }
        CHECK (peakOf (held, 58000, 62000) > 4.0 * peakOf (tapped, 58000, 62000));
        CHECK (peakOf (held, 58000, 62000) > 4.0 * peakOf (without, 58000, 62000));
        CHECK (peakOf (held, 69000, 71000) < 0.5 * peakOf (longer, 69000, 71000));
        for (int k = 3; k < 6; ++k)
        {
            const size_t at = (size_t) k * 24000;
            CHECK (peakOf (held, at, at + 6000) > 1e-2);
            CHECK (peakOf (held, at + 12000, at + 23000) < 3.0 * peakOf (without, at + 12000, at + 23000) + 1e-4);
        }
        Instance r;
        auto v = dronePatch (r);
        v.push_back ({ "Stroke 9", r.valueOf ("Stroke 9", "Roll") });
        r.setup (v);
        const auto roll = perform (r, 24000 * 4, { padOn (47000, 44), padOff (47500, 44) }, playingAll(), 128, false);
        CHECK_PITCH (roll, 50000, 71000, padDegree[2]);
    }
    // All Sound Off and All Notes Off on every channel (Melodee's silence), a choke: a held pad stops; a pad and All
    // Sound Off on one frame play nothing; All Notes Off ends a pad's Hold as its note off would.
    {
        const std::vector<std::pair<std::string, double>> dry { { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 }, { "Decay", 0.0 }, { "Ring", 0.0 } };
        auto storm = [] (uint64_t at, std::vector<PadEvent> list) {
            for (int ch = 0; ch < 16; ++ch) list.push_back (midiEvent (at, 0xb0 | ch, 120, 0)), list.push_back (midiEvent (at, 0xb0 | ch, 123, 0));
            return list;
        };
        for (bool playing : { false, true })
        {
            Instance a;
            a.setup (dry);
            const auto out = perform (a, 48000, storm (12800, { padOn (100, 38) }), playing ? playingAll() : stoppedAll(), 128, false);
            CHECK (peakOf (out, 0, 12800) > 1e-3);
            if (! playing)
                CHECK (peakOf (out, 12800) < 1e-6);
        }
        Instance b;
        b.setup (dry);
        CHECK (peakOf (perform (b, 24000, storm (100, { padOn (100, 38) }), stoppedAll())) < 1e-6);
        Instance c, d;
        c.setup (dry), d.setup (dry);
        const auto notesOff = perform (c, 48000 * 3, { padOn (100, 38), midiEvent (12000, 0xb0, 123, 0) }, stoppedAll());
        CHECK (notesOff == perform (d, 48000 * 3, { padOn (100, 38), padOff (12000, 38) }, stoppedAll()));
        Instance e;
        e.setup (dry);
        e.setup ({ { "Stroke 1", e.valueOf ("Stroke 1", "Roll") } });
        const auto choked = perform (e, 24000, { padOn (0, 36), { 1000, PadEvent::choke, 36, 0.0 } }, stoppedAll(), 128, false);
        CHECK (peakOf (choked, 0, 1000) > 1e-3);
        CHECK (peakOf (choked, 1001) < 1e-6);
    }
    // The same output in blocks of 1, 37, 128 and 4096 frames, with pads on the sequence's step frames, two on a frame,
    // on/on/off, keys transposing, MIDI; playing, and stopped.
    for (bool playing : { true, false })
    {
        auto scene = [&] (uint32_t block) {
            Instance a;
            a.setup ({ { "Hold", 0.5 }, { "Echo", 0.3 }, { "Stroke 7", 6.0 }, { "Stroke 12", 5.0 } });
            const std::vector<PadEvent> list { padOn (12000, 39, 0.6), padOn (24000, 44, 0.9), padOn (24000, 45, 0.5), padOff (30011, 39), padOn (33337, 64),
                                               padOff (40000, 64), padOn (45001, 42, 1.0), padOn (52007, 47, 0.3), padOff (56003, 44), padOff (56003, 45),
                                               padOn (60000, 41), padOn (66011, 41, 0.7), padOff (70001, 41), midiEvent (78009, 0x90, 50, 120),
                                               midiEvent (80001, 0x90, 50, 0), padOn (90001, 36, 0.8), padOff (90001, 36), padOff (96000, 42),
                                               padOff (96000, 47), padOn (101009, 43), { 105003, PadEvent::choke, 43, 0.0 }, padOn (110000, 51, 0.95) };
            return perform (a, 48000 * 3, list, playing ? playingAll() : stoppedAll(), block);
        };
        const auto reference = scene (128);
        for (uint32_t block : { 1u, 37u, 4096u })
            CHECK (scene (block) == reference);
    }
}

// A key struck while keys play the drum (the host stopped, or the sequencer off) and let go once the sequence runs:
// its note off still ends its Hold. To 0.3.3 only a step pad's did, so the key's Hold stayed lit until a step's gate
// ended, and for good when no step played. Here no step plays (every step a Rest), so the drum must sound exactly as it
// does with the host stopped, or the sequencer off, throughout.
void testHoldAcrossPlay()
{
    auto patch = [] (Instance& a, double sequencer) {
        a.setup ({ { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 }, { "Decay", 0.0 }, { "Ring", 0.0 }, { "Density", 0.5 },
                   { "Sequencer", sequencer } });
        for (int s = 1; s <= 16; ++s) a.setup ({ { "Trig " + std::to_string (s), 0.0 } });
    };
    const auto playFrom = [] (uint64_t start) {
        return [=] (uint64_t at) { return at < start ? stopped() : playingFrom (0.0, 120.0, 48000.0, at - start); };
    };
    // Key 72 (no step pad) struck with the host stopped, Play at 12,032, the key let go at 24,000; and the key hit again
    // before Play as hdslaveOS sends it (on, on, off).
    for (bool again : { false, true })
    {
        Instance a, b;
        patch (a, 1.0), patch (b, 1.0);
        std::vector<PadEvent> list { padOn (100, 72), padOff (24000, 72) };
        if (again) list.insert (list.begin() + 1, padOn (6000, 72));
        const auto out = perform (a, 48000 * 3, list, playFrom (12032), 128, false);
        const auto ref = perform (b, 48000 * 3, list, stoppedAll(), 128, false);
        CHECK (peakOf (ref, 20000, 24000) > 1e-3); // stopped: lit while held,
        CHECK (peakOf (ref, 48000 * 2) < 1e-5);    // and it ends with the key
        CHECK (peakOf (out, 20000, 24000) > 1e-3);
        if (peakOf (out, 48000 * 2) >= 1e-5)
            std::printf ("  a key%s let go after Play: Hold stays lit, %.6f two seconds on\n", again ? " hit twice," : "", peakOf (out, 48000 * 2));
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
        CHECK (out == ref);
    }
    // The sequencer switched on under a held key while the host plays.
    {
        Instance a, b;
        patch (a, 0.0), patch (b, 0.0);
        const auto out = perform (a, 48000 * 3, { padOn (100, 72), paramEvent (6000, "Sequencer", 1.0), padOff (24000, 72) }, playingAll(), 128, false);
        const auto ref = perform (b, 48000 * 3, { padOn (100, 72), padOff (24000, 72) }, playingAll(), 128, false);
        CHECK (peakOf (ref, 20000, 24000) > 1e-3 && peakOf (ref, 48000 * 2) < 1e-5);
        if (peakOf (out, 48000 * 2) >= 1e-5)
            std::printf ("  a key let go after the sequencer came on: Hold stays lit, %.6f two seconds on\n", peakOf (out, 48000 * 2));
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
        CHECK (out == ref);
    }
    // A Stop, or the sequencer switched off, ends the sequence's gate, not a held key's Hold: the key still plays the
    // drum, and its note off ends it. To 0.3.4 both cut it while the key was held.
    struct Case { const char* what; double sequencer; std::function<Transport (uint64_t)> transport; std::vector<PadEvent> extra; };
    const Case cases[] = {
        { "the sequencer off, a key held while playing, then Stop", 0.0, [] (uint64_t at) { return at < 12032 ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); }, {} },
        { "a key struck stopped, Play, then Stop", 1.0, [] (uint64_t at) { return at >= 12032 && at < 18048 ? playingFrom (0.0, 120.0, 48000.0, at - 12032) : stopped(); }, {} },
        { "stopped, a key held, the sequencer switched off", 1.0, stoppedAll(), { paramEvent (12000, "Sequencer", 0.0) } },
    };
    for (const auto& c : cases)
    {
        Instance a, b;
        patch (a, c.sequencer), patch (b, c.sequencer);
        std::vector<PadEvent> list { padOn (100, 72), padOff (24000, 72) };
        list.insert (list.begin() + 1, c.extra.begin(), c.extra.end());
        const auto out = perform (a, 48000 * 3, list, c.transport, 128, false);
        // The same with nothing changed: the sequencer off playing throughout, on and stopped throughout.
        const auto ref = perform (b, 48000 * 3, { padOn (100, 72), padOff (24000, 72) }, c.sequencer < 0.5 ? playingAll() : stoppedAll(), 128, false);
        CHECK (peakOf (ref, 20000, 24000) > 1e-3 && peakOf (ref, 48000 * 2) < 1e-5);
        if (out != ref) std::printf ("  %s: %.6f while the key is held, not %.6f\n", c.what, peakOf (out, 20000, 24000), peakOf (ref, 20000, 24000));
        CHECK (out == ref);
    }
    // The page's audition (Shift + Push) takes the drum as a step does: its Hold ends at its gate, not with a key struck
    // before it and let go during it (here a key struck stopped, Play, the audition, the key let go); and a key struck
    // after an audition takes the drum from it: the audition's gate end, come after Play, leaves the held key's Hold.
    {
        const auto audition = [&] (Instance& a, uint64_t when, const std::vector<PadEvent>& list, const std::function<Transport (uint64_t)>& transport) {
            const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
            CHECK (screen != nullptr);
            std::vector<float> l, r;
            a.render (48000 * 3, 128, l, r, transport, [&] (uint64_t at, uint32_t n, Events& e) {
                if (screen != nullptr && when >= at && when < at + n)
                {
                    melodee_screen_input_t next { MELODEE_SCREEN_STEP, 1, 0 }, play { MELODEE_SCREEN_PUSH, 0, 1 };
                    CHECK (screen->input (a.plugin, &next) && screen->input (a.plugin, &play));
                }
                for (const auto& v : list)
                    if (v.frame >= at && v.frame < at + n)
                        e.note ((uint32_t) (v.frame - at), v.kind == PadEvent::on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, (int16_t) v.key, v.value);
            });
            return l;
        };
        {
            Instance a, b;
            patch (a, 1.0), patch (b, 1.0);
            const auto out = audition (a, 12800, { padOn (100, 72), padOff (14000, 72) }, playFrom (12032));
            const auto ref = audition (b, 12800, { padOn (100, 72), padOff (36000, 72) }, playFrom (12032)); // let go after the gate
            CHECK (peakOf (ref, 15000, 18000) > 1e-3 && peakOf (ref, 48000 * 2) < 1e-5);
            if (out != ref) std::printf ("  a key let go during an audition: %.6f in its gate, not %.6f\n", peakOf (out, 15000, 18000), peakOf (ref, 15000, 18000));
            CHECK (out == ref);
        }
        {
            Instance a, b;
            patch (a, 1.0), patch (b, 1.0);
            const auto out = audition (a, 0, { padOn (1000, 72), padOff (24000, 72) }, playFrom (1408));
            const auto ref = audition (b, 0, { padOn (1000, 72), padOff (24000, 72) }, stoppedAll());
            CHECK (peakOf (ref, 20000, 24000) > 1e-3 && peakOf (ref, 48000 * 2) < 1e-5);
            if (out != ref) std::printf ("  a key struck after an audition, then Play: %.6f while held, not %.6f\n", peakOf (out, 20000, 24000), peakOf (ref, 20000, 24000));
            CHECK (out == ref);
        }
        // A key struck stopped and held through an audition: the audition's gate ends at its time, and the key held on
        // past it changes nothing, with the host stopped throughout or Play before the key is let go. To 0.3.5's first
        // build the audition's gate end, with the key held, did not end Hold, and after Play the key's note off did not
        // either: Hold stayed lit for good on rests.
        {
            Instance a, b, c;
            patch (a, 1.0), patch (b, 1.0), patch (c, 1.0);
            const auto out = audition (a, 2048, { padOn (1000, 72), padOff (14000, 72) }, playFrom (12032));
            const auto ref = audition (b, 2048, { padOn (1000, 72), padOff (14000, 72) }, stoppedAll());
            const auto later = audition (c, 2048, { padOn (1000, 72), padOff (36000, 72) }, stoppedAll());
            CHECK (peakOf (ref, 2048, 4800) > 1e-3 && peakOf (ref, 48000 * 2) < 1e-5);
            if (peakOf (out, 48000 * 2) >= 1e-5)
                std::printf ("  a key held through an audition, Play, the key let go: Hold stays lit, %.6f two seconds on\n", peakOf (out, 48000 * 2));
            CHECK (peakOf (out, 48000 * 2) < 1e-5);
            CHECK (out == ref);
            if (later != ref) std::printf ("  a key held past an audition's gate: %.6f after it, not %.6f\n", peakOf (later, 15000, 36000), peakOf (ref, 15000, 36000));
            CHECK (later == ref);
        }
        // And a key struck before the audition and let go inside its gate, the host stopped (the Sequencer on or off):
        // the audition keeps its gate, as when the host plays. To 0.3.5's second build the note off ended Hold at once.
        for (double sequencer : { 1.0, 0.0 })
        {
            Instance a, b;
            patch (a, sequencer), patch (b, sequencer);
            const auto out = audition (a, 2048, { padOn (1000, 72), padOff (3000, 72) }, stoppedAll());
            const auto ref = audition (b, 2048, { padOn (1000, 72), padOff (36000, 72) }, stoppedAll());
            CHECK (peakOf (ref, 3000, 4800) > 1e-3);
            if (out != ref)
                std::printf ("  the Sequencer %s, a key let go during an audition: %.6f in its gate, not %.6f\n", sequencer > 0.5 ? "on" : "off",
                             peakOf (out, 3200, 4800), peakOf (ref, 3200, 4800));
            CHECK (out == ref);
        }
        // Two keys held (Notes: Pitch), the audition, the newer key let go inside its gate: the audition plays on, with
        // no glide back to the older key. To 0.3.5's third build the note off struck the older key, sliding.
        for (double sequencer : { 1.0, 0.0 })
        {
            Instance a, b;
            patch (a, sequencer), patch (b, sequencer);
            const auto out = audition (a, 2048, { padOn (1000, 72), padOn (1500, 74), padOff (3000, 74), padOff (40000, 72) }, stoppedAll());
            const auto ref = audition (b, 2048, { padOn (1000, 72), padOn (1500, 74), padOff (36000, 74), padOff (40000, 72) }, stoppedAll());
            CHECK (peakOf (ref, 3000, 4800) > 1e-3);
            if (out != ref)
                std::printf ("  the Sequencer %s, two keys held, the newer let go during an audition: %.6f in its gate, not %.6f\n",
                             sequencer > 0.5 ? "on" : "off", peakOf (out, 3200, 4800), peakOf (ref, 3200, 4800));
            CHECK (out == ref);
        }
        // The same, the newer key let go after the audition's gate while the drum still rings: no slide lights Hold
        // again, and nothing stays lit once the keys are let go. A slide there would light it for good, as no key
        // plays the drum to end it.
        {
            Instance a, b;
            patch (a, 1.0), patch (b, 1.0);
            const auto out = audition (a, 2048, { padOn (1000, 72), padOn (1500, 74), padOff (8000, 74), padOff (40000, 72) }, stoppedAll());
            const auto ref = audition (b, 2048, { padOn (1000, 72), padOn (1500, 74), padOff (36000, 74), padOff (40000, 72) }, stoppedAll());
            if (peakOf (out, 48000 * 2) >= 1e-5)
                std::printf ("  two keys held, the newer let go after an audition's gate: Hold stays lit, %.6f two seconds on\n", peakOf (out, 48000 * 2));
            CHECK (peakOf (out, 48000 * 2) < 1e-5);
            CHECK (out == ref);
        }
        // While the sequence plays: step 1 at 0 (its gate end due at 3,000), the audition at 2,048. The audition keeps
        // its gate to its own end (5,048), as with the steps at 1/8 (step 1's end due at 6,000), and a Stop at 2,560
        // inside it leaves it too, as playing on does (the steps after are rests). To 0.3.5's fourth build step 1's
        // gate end, and the Stop, ended the audition's Hold early.
        {
            const auto steps = [&] (Instance& a, double rate) {
                patch (a, 1.0);
                a.setup ({ { "Trig 1", 1.0 }, { "Stroke 1", 0.0 }, { "Swing", 0.5 }, { "Gate", 0.5 }, { "Rate", rate } });
            };
            Instance a, b, c;
            steps (a, 5.0), steps (b, 3.0), steps (c, 5.0);
            const auto sixteenths = audition (a, 2048, {}, playingAll());
            const auto eighths = audition (b, 2048, {}, playingAll());
            const auto stopIn = audition (c, 2048, {}, [] (uint64_t at) { return at < 2560 ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); });
            const auto cut = [] (const std::vector<float>& x) { return std::vector<float> (x.begin(), x.begin() + 90000); };
            CHECK (peakOf (sixteenths, 4600, 5000) > 1e-3);
            if (cut (sixteenths) != cut (eighths))
                std::printf ("  an audition while the sequence plays: %.6f before its gate end, not %.6f\n", peakOf (sixteenths, 4600, 5000),
                             peakOf (eighths, 4600, 5000));
            CHECK (cut (sixteenths) == cut (eighths));
            if (cut (stopIn) != cut (sixteenths))
                std::printf ("  an audition, then Stop inside its gate: %.6f before its gate end, not %.6f\n", peakOf (stopIn, 4600, 5000),
                             peakOf (sixteenths, 4600, 5000));
            CHECK (cut (stopIn) == cut (sixteenths));
        }
    }
    // Two keys struck stopped and held through Play, where a step takes the drum, then Stop; the newer key let go while
    // that step still rings slides nowhere (the keys no longer play the drum), so Hold does not light again and
    // nothing is left lit once both are let go. A slide there would light it for good, as no key plays the drum to end
    // it.
    {
        Instance a, b;
        patch (a, 1.0), patch (b, 1.0);
        for (int s = 1; s <= 16; ++s) a.setup ({ { "Trig " + std::to_string (s), 1.0 } }), b.setup ({ { "Trig " + std::to_string (s), 1.0 } });
        const auto playThenStop = [] (uint64_t at) { return at >= 6016 && at < 12032 ? playingFrom (0.0, 120.0, 48000.0, at - 6016) : stopped(); };
        const auto out = perform (a, 48000 * 3, { padOn (1000, 72), padOn (1500, 74), padOff (12416, 74), padOff (20000, 72) }, playThenStop, 128, false);
        const auto ref = perform (b, 48000 * 3, { padOn (1000, 72), padOn (1500, 74), padOff (30000, 74), padOff (36000, 72) }, playThenStop, 128, false);
        CHECK (peakOf (ref, 12416, 12544) > 1e-3); // the step at 12,016 still rings after Stop
        if (peakOf (out, 48000 * 2) >= 1e-5)
            std::printf ("  two keys held through Play, the newer let go after Stop: Hold stays lit, %.6f two seconds on\n", peakOf (out, 48000 * 2));
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
        CHECK (out == ref);
    }
    // A gate end held on for the next step's slide, and that step then not played: the default pattern at Swing 50 %,
    // step 11's gate end (63,000) held for step 12's slide (66,000), and Density turned to 0 between them. The gate ends
    // at step 12's clock. To 0.3.5's fourth build Hold stayed lit, a drone until Stop or the next step played.
    {
        Instance a;
        a.setup ({ { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 }, { "Decay", 0.0 }, { "Ring", 0.0 }, { "Swing", 0.5 } });
        const auto out = perform (a, 48000 * 3, { paramEvent (64000, "Density", 0.0) }, playingAll(), 128, false);
        CHECK (peakOf (out, 60000, 63000) > 1e-3);
        if (peakOf (out, 120000, 144000) >= 1e-5)
            std::printf ("  a slide that never comes: Hold stays lit, %.6f at 2.5 to 3 s\n", peakOf (out, 120000, 144000));
        CHECK (peakOf (out, 120000, 144000) < 1e-5);
    }
    // Notes: Pitch, Hold 0, two keys held until the drum has gone quiet, then the newer let go: nothing sounds. To 0.3.5's
    // fourth build the slide back onto the sleeping drum struck it anew at full force, louder than the keys' own hits.
    for (double sequencer : { 1.0, 0.0 })
    {
        Instance a;
        a.setup ({ { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.0 }, { "Sequencer", sequencer } });
        const auto out = perform (a, 48000 * 3, { padOn (1000, 60, 0.3), padOn (1500, 64, 0.3), padOff (120000, 64), padOff (130000, 60) },
                                  stoppedAll(), 128, false);
        CHECK (peakOf (out, 1000, 24000) > 0.01);
        CHECK (peakOf (out, 100000, 120000) < 1e-5); // quiet before the key is let go
        if (peakOf (out, 120000, 139000) >= 1e-5)
            std::printf ("  a key let go onto a sleeping drum: %.4f (the keys' own hits %.4f)\n", peakOf (out, 120000, 139000), peakOf (out, 1000, 24000));
        CHECK (peakOf (out, 120000, 139000) < 1e-5);
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
    bool told = false, released = false, notRecorded = false;
    for (size_t i = 0; i < out.values.size(); ++i)
    {
        const auto& [id, v] = out.values[i];
        told |= id == a.id ("Seed") && v == after;
        // The plug-in set Seed itself: a host must not record it as the player's own automation.
        notRecorded |= id == a.id ("Seed") && (out.flags[i] & CLAP_EVENT_DONT_RECORD) != 0;
        released |= id == a.id ("Regenerate") && v == 0.0;
    }
    CHECK (told && released);
    CHECK (notRecorded);
}

// The echo's loop high-passes at 20 Hz: at Repeats 97 % with Absorb 100 % (a loop gain just over 1 at DC) the folded
// Acid Gate's offset and rumble die away instead of building up for ever.
void testEchoDc()
{
    Instance a;
    CHECK (a.loadPreset ("acid-gate"));
    a.setup ({ { "Echo", 0.5 }, { "Space", 0.0 }, { "Repeats", 0.97 }, { "Absorb", 1.0 }, { "Hold", 0.0 } });
    std::vector<float> l, r;
    a.render (48000 * 40, 128, l, r, [] (uint64_t at) { return at < 48000 * 4 ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); });
    auto mean = [&] (size_t from, size_t to) { double s = 0.0; for (size_t i = from; i < to; ++i) s += l[i]; return s / (double) (to - from); };
    CHECK (rmsOf (l, 48000 * 10, 48000 * 11) > 0.01);
    CHECK (std::fabs (mean (48000 * 39, 48000 * 40)) < 1e-3);
    CHECK (rmsOf (l, 48000 * 39, 48000 * 40) < 0.5 * rmsOf (l, 48000 * 10, 48000 * 11));
}

// A loop wrap the host tells a block late (its blocks do not divide the loop) must sound exactly as one told on
// time: the step clocked on the wrap's frame is decided once, so Turing's register, Brownian's walk and the
// automaton's generations move on once, not twice. In each case the wrap's own step rests either way (the cases
// were picked for that), so only the state after it can differ.
void testWrap()
{
    auto scene = [] (const std::vector<std::pair<std::string, double>>& extra, uint32_t block) {
        Instance a;
        auto v = clickPatch();
        v.push_back ({ "Rate", a.valueOf ("Rate", "1/16") });
        v.push_back ({ "Length", 12 });
        for (auto& e : extra)
            v.push_back (e);
        a.setup (v);
        std::vector<float> l, r;
        a.render (199680, block, l, r, [] (uint64_t at) {
            return Transport { true, 120.0, std::fmod ((double) at * 2.0 / 48000.0, 3.0) }; // a 3-beat loop: 12 steps
        });
        l.insert (l.end(), r.begin(), r.end());
        return l;
    };
    auto check = [&] (const std::vector<std::pair<std::string, double>>& extra) {
        const auto onTime = scene (extra, 64); // the wrap (frame 72,000) is a block's first frame: told on time
        const auto late = scene (extra, 128);  // the block crossing it runs on: told 64 frames late, as Melodee does
        CHECK (peakOf (onTime) > 0.01);
        CHECK (late == onTime);
        if (late != onTime)
        {
            size_t at = 0;
            while (at < onTime.size() && onTime[at] == late[at])
                ++at;
            std::printf ("  wrap: first differs at %zu\n", at);
        }
    };
    Instance probe;
    check ({ { "Seq Mode", probe.valueOf ("Seq Mode", "Turing") }, { "Mutate", 0.3 }, { "Density", 0.6 }, { "Seed", 4 } });
    {
        std::vector<std::pair<std::string, double>> rests { { "Direction", probe.valueOf ("Direction", "Brownian") }, { "Seed", 1 } };
        for (int s = 1; s <= 16; ++s)
            if (s != 1 && s != 7)
                rests.push_back ({ "Trig " + std::to_string (s), 0.0 });
        check (rests);
    }
    check ({ { "Seq Mode", probe.valueOf ("Seq Mode", "Automaton") }, { "Rule", 90 }, { "Density", 0.4 }, { "Seed", 3 } });
    // The Sequencer switched off before the wrap: deciding the wrap's step again must stay as silent as the
    // clock was (the replay once struck it from the last sequencer-on step's state). Told late sounds exactly
    // as told on time, and nothing sounds after the switch's own ring.
    {
        auto sceneOff = [] (uint32_t block) {
            Instance a;
            auto v = clickPatch();
            v.push_back ({ "Rate", a.valueOf ("Rate", "1/16") });
            v.push_back ({ "Length", 12 });
            a.setup (v);
            std::vector<float> l, r;
            a.render (199680, block, l, r, [] (uint64_t at) { return Transport { true, 120.0, std::fmod ((double) at * 2.0 / 48000.0, 3.0) }; },
                      [&] (uint64_t at, uint32_t n, Events& e) {
                          if (60000 >= at && 60000 < at + n)
                              e.param ((uint32_t) (60000 - at), a.id ("Sequencer"), 0.0);
                      });
            l.insert (l.end(), r.begin(), r.end());
            return l;
        };
        const auto onTime = sceneOff (64), late = sceneOff (128);
        CHECK (peakOf (onTime, 0, 60000) > 0.01);
        CHECK (peakOf (late, 70000, 199680) < 1e-4);
        CHECK (late == onTime);
        if (late != onTime)
        {
            size_t at = 0;
            while (at < onTime.size() && onTime[at] == late[at])
                ++at;
            std::printf ("  wrap-off: first differs at %zu\n", at);
        }
    }
}

// A note on Play's own frame is read under the new transport, and one on Stop's frame under the stopped one:
// settings sent with Play apply first, then the transport, then notes.
void testFrameZero()
{
    // On Play's frame (sequencer on): a key outside the pads transposes the first step, it does not strike as a
    // note of a stopped drum. Whatever Notes says (Pitch, Strokes or Steps).
    for (const char* notes : { "Pitch", "Strokes", "Steps" })
    {
        Instance a;
        auto v = dronePatch (a);
        v.push_back ({ "Notes", a.valueOf ("Notes", notes) });
        a.setup (v);
        const auto out = perform (a, 48000, { padOn (0, 64), padOff (3000, 64) }, playingAll(), 128, false);
        CHECK_PITCH (out, 2000, 22000, padDegree[0] + 4);
        CHECK_PITCH (out, 26000, 46000, padDegree[1] + 4);
    }
    // On Stop's frame: the same key plays the drum (Pitch), it does not transpose a pattern that has stopped.
    {
        Instance a;
        a.setup (dronePatch (a));
        const uint64_t stop = 48000;
        const auto out = perform (a, 96000, { padOn (stop, 64), padOff (stop + 6000, 64) },
                                  [&] (uint64_t at) { return at < stop ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); }, 128, false);
        CHECK_PITCH (out, stop + 2000, stop + 22000, 4);
    }
    // A step pad on Play's frame: the step the sequence plays on that frame wins, the pad's own step (9 here)
    // does not sound, and the sequence walks on as it would have.
    {
        Instance a;
        a.setup (dronePatch (a));
        const auto out = perform (a, 48000, { padOn (0, 44), padOff (3000, 44) }, playingAll(), 128, false);
        CHECK_PITCH (out, 2000, 22000, padDegree[0]);
        CHECK_PITCH (out, 26000, 46000, padDegree[1]);
    }
    // A step pad on Stop's frame plays its step, as it does stopped.
    {
        Instance a;
        a.setup (dronePatch (a));
        const uint64_t stop = 48000;
        const auto out = perform (a, 96000, { padOn (stop, 44), padOff (stop + 6000, 44) },
                                  [&] (uint64_t at) { return at < stop ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); }, 128, false);
        CHECK_PITCH (out, stop + 2000, stop + 22000, padDegree[8]);
    }
}

// Stop (and the Sequencer switched off) takes the sequence's still-queued hits with it: no roll hit and no bounce
// lands after it. A live key's own further hits are the player's, not the sequence's: a flam struck by hand
// completes across a Stop.
void testStopQueue()
{
    auto rollPatch = [] (Instance& a) {
        auto v = clickPatch();
        v.push_back ({ "Rate", a.valueOf ("Rate", "1/4") });
        v.push_back ({ "Stroke 1", a.valueOf ("Stroke 1", "Roll") });
        return v;
    };
    const uint64_t stopAt = 12160; // a block boundary, after the roll's second hit (9,315), before its third (15,835)
    {
        Instance a;
        a.setup (rollPatch (a));
        const auto out = perform (a, 48000, {}, [&] (uint64_t at) { return at < stopAt ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); },
                                  128, false);
        const auto hits = onsets (out, 2e-3f, 2e-4f, 400);
        CHECK (hits.size() >= 2);
        for (long h : hits)
            CHECK (h < (long) stopAt);
    }
    {
        Instance a;
        a.setup (rollPatch (a));
        const auto out = perform (a, 48000, { paramEvent (stopAt, "Sequencer", 0.0) }, playingAll(), 128, false);
        const auto hits = onsets (out, 2e-3f, 2e-4f, 400);
        CHECK (hits.size() >= 2);
        for (long h : hits)
            CHECK (h < (long) stopAt);
    }
    {
        Instance a;
        auto v = clickPatch();
        v.push_back ({ "Rate", a.valueOf ("Rate", "1/16") });
        v.push_back ({ "Seq Mode", a.valueOf ("Seq Mode", "Bounce") });
        v.push_back ({ "Density", 0.2 });
        a.setup (v);
        const auto out = perform (a, 96000, {}, [&] (uint64_t at) { return at < stopAt ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); },
                                  128, false);
        const auto hits = onsets (out, 2e-3f, 2e-4f, 400);
        CHECK (! hits.empty());
        for (long h : hits)
            CHECK (h < (long) stopAt);
    }
    {
        // The flam's main hit (22 ms after its grace) is the key's own: a Stop in between changes nothing.
        auto flam = [] (bool stop) {
            Instance a;
            a.setup ({ { "Sequencer", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Decay", 0.0 }, { "Ring", 0.0 },
                       { "Notes", a.valueOf ("Notes", "Strokes") } });
            return perform (a, 24000, { padOn (11900, 41) },
                            [stop] (uint64_t at) { return ! stop || at < 12032 ? playingFrom (0.0, 120.0, 48000.0, at) : stopped(); }, 128, false);
        };
        const auto played = flam (false);
        CHECK (peakOf (played, 13000, 16000) > 0.005); // the main hit landed about 12,956
        CHECK (flam (true) == played);
    }
}

// After All Sound Off the echo's time is the set one from the first tap, not a glide up from nothing.
void testEchoReset()
{
    auto tapOf = [] (mystik::Effects& fx) {
        std::vector<float> l (24576, 0.0f), r (24576, 0.0f);
        l[0] = 0.5f;
        for (uint32_t at = 0; at + 128 <= 24576; at += 128)
            fx.process (l.data() + at, r.data() + at, 128, false);
        for (size_t i = 100; i < l.size(); ++i)
            if (std::fabs (l[i]) > 0.01f)
                return (long) i;
        return -1L;
    };
    mystik::Effects fx;
    fx.prepare (48000.0);
    mystik::FxKnobs k;
    k.echo = 0.5f, k.echoTime = 2, k.repeats = 0.0f, k.space = 0.0f; // 1/16 at 120 BPM: 6,000 frames
    fx.set (k);
    fx.tempo (120.0);
    const long first = tapOf (fx);
    CHECK (first >= 5996 && first <= 6004);
    fx.silence();
    CHECK (tapOf (fx) == first);

    // The same through the plug-in, as a host sends it (the part above compiles the header into this binary, so
    // it cannot see a broken CC 120 -> All Sound Off -> echo road): a click's one repeat (Repeats at 0) lands
    // the set 1/16 (6,000 frames at 120 BPM) after it, before a panic and after one alike.
    auto echoPatch = [] (Instance& a, const char* time) {
        return std::vector<std::pair<std::string, double>> { { "Source", 1.0 }, { "Snap", 0.0 }, { "Ring", 0.0 }, { "Decay", 0.0 },
                                                             { "Bend", 0.0 }, { "Drift", 0.0 }, { "Human", 0.0 }, { "Echo", 0.8 },
                                                             { "Repeats", 0.0 }, { "Space", 0.0 }, { "Sequencer", 0.0 },
                                                             { "Echo Time", a.valueOf ("Echo Time", time) } };
    };
    {
        Instance a;
        a.setup (echoPatch (a, "1/16"));
        const auto out = perform (a, 96000, { padOn (0, 60), padOff (3000, 60), midiEvent (24000, 0xb0, 120, 0),
                                              padOn (48000, 60), padOff (51000, 60) },
                                  stoppedAll(), 128, false);
        const auto hits = onsets (out, 4e-3f, 1e-4f, 400);
        CHECK (hits.size() == 4);
        if (hits.size() == 4)
        {
            CHECK (hits[1] - hits[0] >= 5984 && hits[1] - hits[0] <= 6016);
            CHECK (hits[3] - hits[2] >= hits[1] - hits[0] - 4 && hits[3] - hits[2] <= hits[1] - hits[0] + 4);
        }
        else
            std::printf ("  echo-cc120: %zu onsets\n", hits.size());
    }
    // A new Echo Time sent in the panic's own block (settings land before the block renders, so the effects
    // have not taken them when the panic is handled): the next hit's repeat is at the new time from its first
    // tap, not snapped to the old one and gliding.
    {
        Instance a;
        a.setup (echoPatch (a, "1/4"));
        const auto out = perform (a, 96000, { paramEvent (0, "Echo Time", a.valueOf ("Echo Time", "1/16")),
                                              midiEvent (0, 0xb0, 120, 0), padOn (24000, 60), padOff (27000, 60) },
                                  stoppedAll(), 128, false);
        const auto hits = onsets (out, 4e-3f, 1e-4f, 400);
        CHECK (hits.size() == 2);
        if (hits.size() == 2)
            CHECK (hits[1] - hits[0] >= 5984 && hits[1] - hits[0] <= 6016);
        else
            std::printf ("  echo-stale: %zu onsets\n", hits.size());
    }
}

// A state loaded into a sounding instance settles at once: its knobs are in place at the next control tick, not
// eased over the next millisecond and a half.
void testSettle()
{
    const std::vector<std::pair<std::string, double>> drone { { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 },
                                                                { "Drift", 0.0 }, { "Bend", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 },
                                                                { "Sequencer", 0.0 }, { "Open", 1.0 }, { "Human", 0.0 } };
    std::vector<uint8_t> saved;
    {
        Instance src;
        auto v = drone;
        v.push_back ({ "Tune", 36.0 });
        src.setup (v);
        clap_ostream out { &saved, [] (const clap_ostream* s, const void* data, uint64_t size) -> int64_t {
            auto* bytes = static_cast<std::vector<uint8_t>*> (s->ctx);
            bytes->insert (bytes->end(), (const uint8_t*) data, (const uint8_t*) data + size);
            return (int64_t) size;
        } };
        CHECK (src.state->save (src.plugin, &out));
    }
    Instance a;
    auto v = drone;
    v.push_back ({ "Tune", 0.0 });
    a.setup (v);
    (void) a.play (9600, false); // sounding at C2, its knobs long settled
    struct Reader { const std::vector<uint8_t>* data; size_t at; };
    Reader reader { &saved, 0 };
    clap_istream in { &reader, [] (const clap_istream* s, void* data, uint64_t size) -> int64_t {
        auto* r = static_cast<Reader*> (s->ctx);
        const size_t n = std::min<size_t> ((size_t) size, r->data->size() - r->at);
        std::memcpy (data, r->data->data() + r->at, n);
        r->at += n;
        return (int64_t) n;
    } };
    CHECK (a.state->load (a.plugin, &in));
    const auto after = a.play (4800, false);
    CHECK_NEAR (pitchOf (after, 16, 224, 48000.0), 1046.5, 30.0); // three octaves up, already there
    CHECK_NEAR (pitchOf (after, 2000, 4000, 48000.0), 1046.5, 12.0);
}

// Press bends the pitch up as the finger leans; another stroke interrupting it lets the bend fall back at the
// press's own rate instead of snapping to the unbent pitch mid-ring.
void testPress()
{
    Instance a;
    a.setup ({ { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 }, { "Drift", 0.0 }, { "Bend", 0.0 },
               { "Echo", 0.0 }, { "Space", 0.0 }, { "Sequencer", 0.0 }, { "Open", 1.0 }, { "Tune", 0.0 }, { "Human", 0.0 },
               { "Notes", a.valueOf ("Notes", "Strokes") } });
    // Key 43 is Press, key 40 Ghost, both at Tune.
    const auto out = a.keys (48000, { { 0, 43, 0.9, true }, { 24000, 40, 0.9, true } });
    CHECK_NEAR (pitchOf (out, 20000, 23000, 48000.0), 146.8, 2.0); // risen the press's two semitones
    CHECK (pitchOf (out, 24300, 26000, 48000.0) > 138.0);           // falling back, not snapped
    CHECK_NEAR (pitchOf (out, 40000, 47000, 48000.0), 130.81, 1.5);

    // The walk back is only over a head still ringing: a Press left to ring right out, then a fresh Open hit
    // from silence, starts at its own pitch (the bend once waited out the silence and bent the new hit).
    {
        Instance b;
        b.setup ({ { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 }, { "Drift", 0.0 }, { "Bend", 0.0 },
                   { "Snap", 0.0 }, { "Decay", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Sequencer", 0.0 }, { "Tune", 0.0 },
                   { "Human", 0.0 }, { "Notes", b.valueOf ("Notes", "Strokes") } });
        // Key 43 Press, key 36 Open, both at Tune; the gate (Decay at 0) has long closed between them.
        const auto rung = b.keys (144000, { { 0, 43, 0.9, true }, { 6000, 43, 0.0, false }, { 96000, 36, 0.9, true }, { 102000, 36, 0.0, false } });
        CHECK (pitchOf (rung, 2000, 5000, 48000.0) > 135.0);              // the press was leaning
        CHECK (peakOf (rung, 48000, 96000) < 1e-4);                       // and then silence
        CHECK_NEAR (pitchOf (rung, 96400, 99000, 48000.0), 130.81, 2.5);  // the fresh hit unbent from its start
    }
    // A choke ends the bend with the sound: Press against the held-open gate, a choke, then Open.
    {
        Instance b;
        b.setup ({ { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 }, { "Drift", 0.0 }, { "Bend", 0.0 },
                   { "Echo", 0.0 }, { "Space", 0.0 }, { "Sequencer", 0.0 }, { "Open", 1.0 }, { "Tune", 0.0 }, { "Human", 0.0 },
                   { "Notes", b.valueOf ("Notes", "Strokes") } });
        const auto choked = perform (b, 96000, { padOn (0, 43), PadEvent { 24000, PadEvent::choke, 43, 0.0 }, padOn (48000, 36) },
                                     stoppedAll(), 128, false);
        CHECK_NEAR (pitchOf (choked, 20000, 23000, 48000.0), 146.8, 2.0);
        CHECK (peakOf (choked, 26000, 48000) < 1e-4);
        CHECK_NEAR (pitchOf (choked, 48500, 52000, 48000.0), 130.81, 2.0);
        CHECK_NEAR (pitchOf (choked, 70000, 90000, 48000.0), 130.81, 1.5);
    }
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
    const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
    CHECK (screen != nullptr);
    if (screen == nullptr) return;
    std::vector<uint32_t> pixels (MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_PAGE_HEIGHT, 0);
    melodee_screen_frame_t frame {};
    frame.pixels = pixels.data();
    frame.width = MELODEE_SCREEN_WIDTH, frame.height = MELODEE_SCREEN_PAGE_HEIGHT, frame.stride = MELODEE_SCREEN_WIDTH;
    frame.area = MELODEE_SCREEN_PAGE;
    frame.colour = 0x00ff8a3du;
    for (int k = 0; k < 3; ++k) frame.pots[k] = -1, frame.pots_live[k] = 1;
    uint64_t now = 1000;
    auto paint = [&] (bool whole = false) { frame.redraw_all = whole ? 1u : 0u, frame.time_ms = now; return screen->draw (a.plugin, &frame); };
    auto shot = [&] (const std::string& name) { paint (true); CHECK (writePng ((folder + "/screen-" + name + ".png").c_str(), pixels.data(), 320, 218, 320, 2)); };
    auto in = [&] (uint32_t kind, int value, bool shift = false) { melodee_screen_input_t e { kind, value, shift ? 1u : 0u }; return screen->input (a.plugin, &e); };
    auto knobs = [&] () { std::vector<uint32_t> ids (3); screen->controls (a.plugin, ids.data()); return ids; };
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
    frame.playing = 1;
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
    CHECK (in (MELODEE_SCREEN_STEP, 1));
    CHECK (in (MELODEE_SCREEN_WHEEL, 2));
    CHECK ((knobs() == std::vector<uint32_t> { a.id ("Pitch 3"), a.id ("Stroke 3"), a.id ("Chance 3") }));
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
        CHECK (in (MELODEE_SCREEN_STEP, 1));
        play (12000, true, 8.0);
        shot (name);
    }
    CHECK ((knobs() == std::vector<uint32_t> { a.id ("Sequencer"), a.id ("Seq Mode"), a.id ("Rate") }));
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
    CHECK (in (MELODEE_SCREEN_STEP, 1));
    frame.shift = 1;
    shot ("shift");
    frame.shift = 0;
    // Every preset draws on every page (a whole paint each).
    for (const char* key : { "acid-gate", "singing-bowl", "feedback-witch", "tabla-teental" })
    {
        CHECK (a.loadPreset (key));
        for (int p = 0; p < 7; ++p)
        {
            in (MELODEE_SCREEN_STEP, 1);
            play (2400, true, 0.0);
            CHECK (paint (true));
        }
    }
    CHECK (a.loadPreset ("mystik"));
    for (int p = 0; p < 7; ++p) in (MELODEE_SCREEN_STEP, -1);
    play (24000, true, 0.0);
    shot ("play-default");

    // PLAY while a shorter cycle comes in: the values reach the page at once, the engine's cycle and place only at its
    // next step. Places past the new rhythm's end read as rests (make sanitize stops on a read past its timeline).
    CHECK ((knobs() == std::vector<clap_id> { a.id ("Timbre"), a.id ("Decay"), a.id ("Resonance") }));
    auto flush = [&] (const std::string& name, double v) {
        Events e;
        e.param (0, a.id (name), v);
        OutEvents out;
        a.params->flush (a.plugin, &e.in, &out.out);
    };
    a.setup ({ { "Seq Mode", a.valueOf ("Seq Mode", "Rhythm") }, { "Rhythm", a.valueOf ("Rhythm", "Teental") } });
    play (6000 * 15 + 3000, true, 0.0); // into Teental's 16th place
    flush ("Rhythm", a.valueOf ("Rhythm", "Dadra"));
    CHECK (paint (true));
    a.setup ({ { "Seq Mode", a.valueOf ("Seq Mode", "Pattern") }, { "Rhythm", a.valueOf ("Rhythm", "Dadra") } });
    play (6000 * 15 + 3000, true, 0.0);
    flush ("Seq Mode", a.valueOf ("Seq Mode", "Rhythm"));
    CHECK (paint (true));
    play (6000, true, 4.0);
    CHECK (paint (true));

    // The page asks for frames only while something moves. Left while it sounds (the scope live on PLAY), then stopped
    // and silent: no more frames asked for (0.2.1 asked for ever once it had left PLAY with the scope live).
    a.setup ({ { "Seq Mode", a.valueOf ("Seq Mode", "Pattern") } });
    play (24000, true, 0.0);
    now += 40;
    paint();
    CHECK (in (MELODEE_SCREEN_STEP, 1)); // to SEQ
    play (128, true, 1.0);
    now += 40;
    paint();
    for (int k = 0; k < 4; ++k)
    {
        play (48000, false);
        now += 40;
        paint();
    }
    int askedNow = paints;
    for (int k = 0; k < 3; ++k)
    {
        now += 100;
        paint();
    }
    CHECK (paints == askedNow);
    // A drone (Open) on PLAY keeps it moving while the host runs it; once the host stops running it (asleep), the light
    // it last told is not animated for ever.
    CHECK (in (MELODEE_SCREEN_STEP, -1)); // back to PLAY
    a.setup ({ { "Sequencer", 0.0 }, { "Open", 1.0 } });
    play (24000, false);
    now += 40;
    paint();
    askedNow = paints;
    for (int k = 0; k < 3; ++k)
    {
        play (1920, false);
        paint();
    }
    CHECK (paints > askedNow + 2);
    now += 2500;
    paint();
    askedNow = paints;
    for (int k = 0; k < 3; ++k)
    {
        now += 100;
        paint();
    }
    CHECK (paints == askedNow);
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
    run ("hold", testHold);
    run ("pads", testPads);
    run ("holdplay", testHoldAcrossPlay);
    run ("regenerate", testRegenerate);
    run ("echo-dc", testEchoDc);
    run ("wrap", testWrap);
    run ("framezero", testFrameZero);
    run ("stopqueue", testStopQueue);
    run ("echoreset", testEchoReset);
    run ("settle", testSettle);
    run ("press", testPress);
    run ("extremes", testExtremes);
    run ("screen", [&] { testScreen (argv[2]); });
    run ("demos", [&] { demos (argv[2]); });
    entry->deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
