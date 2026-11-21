// The sound's fingerprint: renders fixed scenarios through the CLAP entry, as a host plays them, and prints a 64-bit
// hash of every output sample's bits for each. tests/fingerprint.txt holds the hashes the current sound gives, and
// `make test` fails if one changes: work meant to keep the sound (a speed-up, a clean-up) must leave them all as they
// are. A change meant to change the sound updates the file with `make fingerprint-update`.
//   fingerprint PLUGIN.clap [EXPECTED.txt]
// Scenarios: every factory preset playing, each sequencer mode, keys in each Notes mode, MIDI, other rates, the
// transport (stop, Play again, a jump, a loop told a block late), random settings, and the same scene in blocks of 1,
// 37 and 4096 frames, which must hash as in blocks of 128. Hashes hold for this toolchain and these flags (Melodee's
// build image, -O3 -mcpu=cortex-a55 -ffp-contract=fast): another compiler may round differently.
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
const clap_plugin_factory* factory = nullptr;
const clap_host host { CLAP_VERSION, nullptr, "fingerprint", "", "", "1", [] (const clap_host*, const char*) -> const void* { return nullptr; },
                       [] (const clap_host*) {}, [] (const clap_host*) {}, [] (const clap_host*) {} };

struct Event
{
    uint64_t frame;
    enum { param, noteOn, noteOff, midi } kind;
    std::string name;      // the parameter's
    double value;          // its value, the note's key, or a MIDI status
    double velocity = 0.9; // a note's; a MIDI message's data bytes as 256 * first + second
};

struct Scenario
{
    std::string name;
    std::string preset;
    std::vector<std::pair<std::string, double>> values;
    std::vector<Event> events;
    double rate = 48000.0, seconds = 6.0, bpm = 120.0;
    uint32_t block = 128;
    std::function<std::pair<bool, double> (uint64_t, double, double)> transport; // {playing, beats} for a block at `at`
};

uint64_t fnv (uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*> (data);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t render (const Scenario& s)
{
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    const clap_plugin* plugin = factory->create_plugin (factory, &host, d->id);
    plugin->init (plugin);
    const auto* params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    std::map<std::string, clap_id> ids;
    for (uint32_t i = 0; i < params->count (plugin); ++i)
    {
        clap_param_info info {};
        params->get_info (plugin, i, &info);
        ids[info.name] = info.id;
    }
    if (! s.preset.empty())
    {
        const auto* presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        if (presets == nullptr || ! presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, s.preset.c_str()))
            std::printf ("no preset %s\n", s.preset.c_str());
    }
    plugin->activate (plugin, s.rate, 1, 4096);
    plugin->start_processing (plugin);

    struct In
    {
        std::vector<clap_event_param_value> params;
        std::vector<clap_event_note> notes;
        std::vector<clap_event_midi> midis;
        std::vector<const clap_event_header*> order;
        clap_input_events events { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const In*> (e->ctx)->order.size(); },
                                   [] (const clap_input_events* e, uint32_t i) { return static_cast<const In*> (e->ctx)->order[i]; } };
    };
    clap_output_events out { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };
    auto value = [&] (uint32_t time, const std::string& name, double v) {
        clap_event_param_value e {};
        e.header = { sizeof (e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = ids.count (name) ? ids[name] : CLAP_INVALID_ID;
        if (! ids.count (name))
            std::printf ("no parameter %s\n", name.c_str());
        e.note_id = e.port_index = e.channel = e.key = -1;
        e.value = v;
        return e;
    };

    const uint64_t frames = (uint64_t) (s.seconds * s.rate);
    std::vector<float> left (s.block), right (s.block);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint64_t at = 0; at < frames; at += s.block)
    {
        const uint32_t n = (uint32_t) std::min<uint64_t> (s.block, frames - at);
        In in;
        in.params.reserve (s.values.size() + s.events.size());
        in.notes.reserve (s.events.size());
        in.midis.reserve (s.events.size());
        if (at == 0)
            for (auto& [name, v] : s.values)
                in.params.push_back (value (0, name, v));
        for (const auto& e : s.events)
            if (e.frame >= at && e.frame < at + n)
            {
                const uint32_t t = (uint32_t) (e.frame - at);
                if (e.kind == Event::param)
                    in.params.push_back (value (t, e.name, e.value));
                else if (e.kind == Event::midi)
                {
                    clap_event_midi m {};
                    m.header = { sizeof (m), t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
                    const int data = (int) e.velocity;
                    m.port_index = 0, m.data[0] = (uint8_t) e.value, m.data[1] = (uint8_t) (data >> 8), m.data[2] = (uint8_t) (data & 0xff);
                    in.midis.push_back (m);
                }
                else
                {
                    clap_event_note note {};
                    note.header = { sizeof (note), t, CLAP_CORE_EVENT_SPACE_ID, (uint16_t) (e.kind == Event::noteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
                    note.note_id = -1, note.port_index = 0, note.channel = 0, note.key = (int16_t) e.value, note.velocity = e.velocity;
                    in.notes.push_back (note);
                }
            }
        for (auto& p : in.params) in.order.push_back (&p.header);
        for (auto& p : in.notes) in.order.push_back (&p.header);
        for (auto& p : in.midis) in.order.push_back (&p.header);
        std::stable_sort (in.order.begin(), in.order.end(), [] (const clap_event_header* a, const clap_event_header* b) { return a->time < b->time; });

        const auto [playing, beats] = s.transport ? s.transport (at, s.bpm, s.rate) : std::make_pair (true, (double) at * s.bpm / 60.0 / s.rate);
        clap_event_transport t {};
        t.header = { sizeof (t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
        t.tempo = s.bpm;
        t.song_pos_beats = (clap_beattime) std::llround (beats * (double) CLAP_BEATTIME_FACTOR);
        t.tsig_num = 4, t.tsig_denom = 4;
        float* outs[2] = { left.data(), right.data() };
        clap_audio_buffer buffer {};
        buffer.data32 = outs;
        buffer.channel_count = 2;
        clap_process p {};
        p.steady_time = (int64_t) at;
        p.frames_count = n;
        p.transport = &t;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = &in.events;
        p.out_events = &out;
        plugin->process (plugin, &p);
        for (uint32_t i = 0; i < n; ++i) // a frame at a time: the hash is the same whatever the block size
        {
            hash = fnv (hash, &left[i], sizeof (float));
            hash = fnv (hash, &right[i], sizeof (float));
        }
    }
    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    return hash;
}

// The parameters of Mystik 0.1, in its table's order: the random scenarios draw for these, so a parameter added later
// stays at its default there and these hashes still hold. Add another list for scenarios that use new ones.
std::vector<std::string> order01()
{
    std::vector<std::string> names { "Tune", "Source", "Body", "Ring", "Position", "Snap", "Bend", "Drift", "FM Index", "FM Ratio", "Feedback",
        "Timbre", "Symmetry", "Bloom", "Gate Mode", "Resonance", "Strike", "Decay", "Open", "Hold", "Memory", "Accent", "Glide", "Drive",
        "Crush", "Echo", "Echo Time", "Repeats", "Absorb", "Space", "Space Decay", "Volume", "Sequencer", "Seq Mode", "Rate", "Length",
        "Direction", "Swing", "Gate", "Scale", "Density", "Rotate", "Mutate", "Range", "Accents", "Rule", "Rhythm", "Curve", "Seed",
        "Regenerate", "Notes" };
    for (const char* lane : { "Pitch ", "Trig ", "Stroke ", "Chance " })
        for (int s = 1; s <= 16; ++s)
            names.push_back (lane + std::to_string (s));
    return names;
}

std::vector<std::pair<std::string, double>> randomValues (uint32_t seed, const std::vector<std::string>& order)
{
    std::vector<std::pair<std::string, double>> out;
    std::mt19937 random (seed);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    const clap_plugin* plugin = factory->create_plugin (factory, &host, d->id);
    plugin->init (plugin);
    const auto* params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    std::map<std::string, clap_param_info> byName;
    for (uint32_t i = 0; i < params->count (plugin); ++i)
    {
        clap_param_info info {};
        params->get_info (plugin, i, &info);
        byName[info.name] = info;
    }
    for (const auto& name : order)
    {
        if (byName.count (name) == 0)
        {
            std::printf ("no parameter %s\n", name.c_str());
            continue;
        }
        const clap_param_info info = byName[name];
        std::uniform_real_distribution<double> u (info.min_value, info.max_value);
        double v = u (random);
        if (info.flags & CLAP_PARAM_IS_STEPPED)
            v = std::round (v);
        if (name != "Regenerate" && name != "Volume")
            out.push_back ({ name, v });
    }
    plugin->destroy (plugin);
    return out;
}

Event noteAt (uint64_t frame, int key, double velocity = 0.9) { return { frame, Event::noteOn, "", (double) key, velocity }; }
Event noteOffAt (uint64_t frame, int key) { return { frame, Event::noteOff, "", (double) key, 0.0 }; }
Event midiAt (uint64_t frame, int status, int a, int b) { return { frame, Event::midi, "", (double) status, (double) (a * 256 + b) }; }
Event paramAt (uint64_t frame, const std::string& name, double v) { return { frame, Event::param, name, v }; }

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    for (const char* key : { "mystik", "west-bongo", "tabla-teental", "conga-tumbao", "udu-bloom", "acid-gate", "turing-acid", "singing-bowl",
                             "slit-euclid", "handpan-drift", "maqsum-night", "feedback-witch", "bouncing-ball", "gankogui-bell",
                             "goblet-maqsum" })
    {
        Scenario s;
        s.name = std::string ("preset-") + key;
        s.preset = key;
        s.bpm = 112.0;
        list.push_back (s);
    }
    // Each sequencer mode on the default patch, with the knobs that steer it.
    const std::pair<const char*, std::vector<std::pair<std::string, double>>> modes[] = {
        { "mode-pattern-brownian", { { "Direction", 3 }, { "Density", 0.8 }, { "Mutate", 0.5 } } },
        { "mode-pattern-pingpong", { { "Direction", 2 }, { "Length", 7 }, { "Rotate", 3 }, { "Swing", 0.66 } } },
        { "mode-euclid", { { "Seq Mode", 1 }, { "Density", 0.44 }, { "Accents", 0.5 }, { "Rotate", 2 }, { "Mutate", 0.2 } } },
        { "mode-turing", { { "Seq Mode", 2 }, { "Mutate", 0.35 }, { "Length", 11 }, { "Range", 0.8 }, { "Seed", 77 } } },
        { "mode-automaton", { { "Seq Mode", 3 }, { "Rule", 30 }, { "Length", 13 }, { "Mutate", 0.1 }, { "Seed", 5 } } },
        { "mode-bounce", { { "Seq Mode", 4 }, { "Density", 0.7 }, { "Curve", 0.3 }, { "Mutate", 0.4 } } },
        { "mode-rhythm-teental", { { "Seq Mode", 5 }, { "Rhythm", 11 }, { "Accents", 0.6 }, { "Mutate", 0.3 } } },
        { "mode-rhythm-bell", { { "Seq Mode", 5 }, { "Rhythm", 5 }, { "Rate", 4 } } },
        { "rate-1-32-flams", { { "Rate", 7 }, { "Stroke 1", 5 }, { "Stroke 5", 6 }, { "Stroke 9", 7 }, { "Hold", 0.7 }, { "Gate", 1.0 } } },
    };
    for (const auto& [name, values] : modes)
    {
        Scenario s;
        s.name = name;
        s.values = values;
        list.push_back (s);
    }
    // Keys, the sequencer off: Pitch with legato slides and held gates, Strokes, Steps; MIDI notes, the wheel, CC 120.
    {
        Scenario s;
        s.name = "notes-pitch";
        s.values = { { "Sequencer", 0 }, { "Hold", 0.8 }, { "Glide", 0.6 }, { "Gate Mode", 0.9 }, { "Resonance", 0.7 } };
        for (int i = 0; i < 24; ++i)
        {
            s.events.push_back (noteAt ((uint64_t) i * 11000, 48 + (i * 5) % 19, 0.4 + 0.6 * (i % 4) / 3.0));
            s.events.push_back (noteOffAt ((uint64_t) i * 11000 + (i % 3 == 0 ? 15000 : 7000), 48 + (i * 5) % 19));
        }
        list.push_back (s);
    }
    for (const char* mode : { "notes-strokes", "notes-steps" })
    {
        Scenario s;
        s.name = mode;
        s.values = { { "Sequencer", 0 }, { "Notes", std::string (mode) == "notes-strokes" ? 1.0 : 2.0 } };
        for (int i = 0; i < 24; ++i)
            s.events.push_back (noteAt ((uint64_t) i * 12000, 36 + (i * 3) % 16, 0.5 + 0.5 * (i % 2)));
        list.push_back (s);
    }
    {
        Scenario s;
        s.name = "midi";
        s.values = { { "Sequencer", 0 }, { "Echo", 0.4 }, { "Space", 0.4 } };
        for (int i = 0; i < 12; ++i)
            s.events.push_back (midiAt ((uint64_t) i * 20000 + 7, 0x90, 50 + i, 64 + i * 5));
        for (int i = 0; i < 40; ++i)
            s.events.push_back (midiAt ((uint64_t) i * 6000 + 3, 0xe0, (i * 37) & 127, (i * 13) & 127));
        for (int i = 0; i < 20; ++i)
            s.events.push_back (midiAt ((uint64_t) i * 12000 + 11, 0xb0, 1, (i * 29) & 127));
        s.events.push_back (midiAt (200000, 0xb0, 123, 0));
        s.events.push_back (midiAt (250000, 0xb0, 120, 0));
        list.push_back (s);
    }
    // Other rates.
    for (double rate : { 44100.0, 96000.0 })
    {
        Scenario s;
        s.name = rate == 44100.0 ? "rate-44100" : "rate-96000";
        s.rate = rate;
        s.preset = "west-bongo";
        list.push_back (s);
    }
    // The transport: stopped a while, Play from bar 2, a jump back, a loop of one bar told a block late (Melodee).
    {
        Scenario s;
        s.name = "transport-jumps";
        s.transport = [] (uint64_t at, double bpm, double rate) {
            const double t = (double) at / rate;
            if (t < 0.5) return std::make_pair (false, 0.0);
            if (t < 3.0) return std::make_pair (true, 4.0 + (t - 0.5) * bpm / 60.0);
            if (t < 3.3) return std::make_pair (false, 0.0);
            return std::make_pair (true, 1.0 + (t - 3.3) * bpm / 60.0);
        };
        list.push_back (s);
    }
    {
        Scenario s;
        s.name = "transport-loop";
        s.values = { { "Seq Mode", 2 }, { "Mutate", 0.3 } };
        s.transport = [] (uint64_t at, double bpm, double rate) {
            const double beats = (double) at * bpm / 60.0 / rate;
            return std::make_pair (true, std::fmod (beats, 4.0));
        };
        list.push_back (s);
    }
    // Knobs moving while it plays.
    {
        Scenario s;
        s.name = "knobs-moving";
        s.preset = "acid-gate";
        const char* names[] = { "Timbre", "Decay", "Resonance", "Gate Mode", "Tune", "FM Index", "Echo Time", "Body", "Rate", "Seq Mode" };
        for (int i = 0; i < 60; ++i)
            s.events.push_back (paramAt ((uint64_t) i * 4801, names[i % 10], (i % 10 == 4) ? -12.0 + i % 24 : (i % 10 == 6 || i % 10 == 7 || i % 10 == 9 ? (double) (i % 5) : (i % 7) / 6.0)));
        list.push_back (s);
    }
    for (uint32_t seed = 0; seed < 8; ++seed)
    {
        Scenario s;
        s.name = "random-" + std::to_string (seed);
        s.values = randomValues (seed * 7919 + 1, order01());
        s.bpm = 90.0 + 13.0 * seed;
        for (int i = 0; i < 10; ++i)
            s.events.push_back (noteAt ((uint64_t) i * 27000 + seed * 101, 40 + (i * 7 + (int) seed) % 40, 0.3 + 0.07 * i));
        list.push_back (s);
    }
    // The same scene in other block sizes: must hash as in blocks of 128 (checked in main()).
    for (uint32_t block : { 128u, 1u, 37u, 4096u })
    {
        Scenario s;
        s.name = "blocks-" + std::to_string (block);
        s.preset = "turing-acid";
        s.block = block;
        s.seconds = 4.0;
        s.values = { { "Space", 0.5 }, { "Crush", 0.15 }, { "Drift", 0.4 } };
        s.events = { paramAt (70001, "Timbre", 0.9), noteAt (50011, 64, 0.8), paramAt (120007, "Rate", 6) };
        // From a step's start: a Play landing just after one plays it at once if it is under a host block late
        // (Anvil's rule), so a start between steps would depend on the block size by design.
        s.transport = [] (uint64_t at, double bpm, double rate) { return std::make_pair (true, 0.25 + (double) at * bpm / 60.0 / rate); };
        s.bpm = 133.0;
        list.push_back (s);
    }
    return list;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: fingerprint PLUGIN.clap [EXPECTED.txt]\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    const auto* entry = library ? static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry")) : nullptr;
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL cannot load %s\n", argv[1]);
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    std::map<std::string, std::string> expected;
    if (argc > 2)
        if (FILE* f = std::fopen (argv[2], "r"))
        {
            char name[128], hash[32];
            while (std::fscanf (f, "%127s %31s", name, hash) == 2)
                expected[name] = hash;
            std::fclose (f);
        }
    int changed = 0, missing = 0, blockBreaks = 0;
    std::string blocks128;
    for (const auto& s : scenarios())
    {
        char hash[32];
        std::snprintf (hash, sizeof (hash), "%016llx", (unsigned long long) render (s));
        std::printf ("%s %s\n", s.name.c_str(), hash);
        if (s.name == "blocks-128")
            blocks128 = hash;
        else if (s.name.rfind ("blocks-", 0) == 0 && blocks128 != hash)
            ++blockBreaks, std::fprintf (stderr, "FAIL fingerprint: %s differs from blocks-128: the block size changes the sound\n", s.name.c_str());
        if (argc > 2)
        {
            const auto it = expected.find (s.name);
            if (it == expected.end())
                ++missing, std::fprintf (stderr, "fingerprint: %s is new (%s)\n", s.name.c_str(), hash);
            else if (it->second != hash)
                ++changed, std::fprintf (stderr, "FAIL fingerprint: %s was %s, is %s: the sound changed\n", s.name.c_str(), it->second.c_str(), hash);
        }
    }
    entry->deinit();
    if (argc > 2)
        std::fprintf (stderr, "fingerprint: %d changed, %d new, %d block-size breaks\n", changed, missing, blockBreaks);
    return changed == 0 && missing == 0 && blockBreaks == 0 ? 0 : 1;
}
