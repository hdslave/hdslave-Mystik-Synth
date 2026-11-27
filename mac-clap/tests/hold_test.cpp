// Hold in the desktop copy: the engine and glue of src/ (the inner plug-in, as the desktop bundle runs it), played as a
// host plays it, with keys, the page's Shift + Push on SEQ (the audition) and the transport at block boundaries. It
// holds the Hold rules the MPC build's tests/host.cpp holds for the same code: Hold follows the key that struck the
// drum last, a gate end is never lost, Stop takes the sequence's queued hits, and a block's first-frame notes and
// presses come after its transport.
//   hold_test
#include "melodee_screen.h"

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

extern "C" const clap_plugin_entry hdslave_inner_clap_entry; // src/mystik.cpp's entry, renamed for the bundle

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

const clap_host host {
    CLAP_VERSION, nullptr, "Mystik hold test", "tests", "", "1",
    [] (const clap_host*, const char*) -> const void* { return nullptr; }, [] (const clap_host*) {}, [] (const clap_host*) {},
    [] (const clap_host*) {},
};
const clap_plugin_factory* factory = nullptr;

struct Transport
{
    bool playing = false;
    double beats = 0.0;
};
Transport stopped() { return {}; }
Transport playingFrom (uint64_t start, uint64_t at) { return { true, (double) (at - start) * 2.0 / 48000.0 }; } // 120 BPM

struct Event
{
    uint64_t frame;
    enum Kind { on, off, audition, param } kind;
    int key = 0;
    double value = 0.9;
    std::string name {};
};
Event keyOn (uint64_t f, int key, double v = 0.9) { return { f, Event::on, key, v }; }
Event keyOff (uint64_t f, int key) { return { f, Event::off, key, 0.0 }; }
Event audition (uint64_t f) { return { f, Event::audition }; } // SEQ's Shift + Push on step 1, before the block it lands in

struct Instance
{
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const melodee_screen_t* screen = nullptr;
    std::map<std::string, clap_id> ids;
    bool onSeq = false;

    Instance()
    {
        plugin = factory->create_plugin (factory, &host, factory->get_plugin_descriptor (factory, 0)->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        screen = static_cast<const melodee_screen_t*> (plugin->get_extension (plugin, MELODEE_SCREEN_CLAP));
        CHECK (params != nullptr && screen != nullptr);
        for (uint32_t i = 0; params && i < params->count (plugin); ++i)
        {
            clap_param_info info {};
            params->get_info (plugin, i, &info);
            ids[info.name] = info.id;
        }
        CHECK (plugin->activate (plugin, 48000.0, 1, 4096));
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
    double valueOf (const std::string& name, const char* text)
    {
        double v = 0.0;
        CHECK (params->text_to_value (plugin, id (name), text, &v));
        return v;
    }

    // Renders `frames` in blocks, the events at their frames (a page press before its block), the left channel.
    std::vector<float> run (uint32_t frames, const std::vector<Event>& events, const std::function<Transport (uint64_t)>& transport,
                            uint32_t block = 128)
    {
        std::vector<float> left (frames, 0.0f), right (frames, 0.0f);
        struct Any { union { clap_event_header header; clap_event_param_value param; clap_event_note note; }; };
        for (uint64_t at = 0; at < frames; at += block)
        {
            const uint32_t n = (uint32_t) std::min<uint64_t> (block, frames - at);
            std::vector<Any> list;
            for (const auto& e : events)
            {
                if (e.frame < at || e.frame >= at + n)
                    continue;
                const uint32_t t = (uint32_t) (e.frame - at);
                Any a {};
                if (e.kind == Event::audition)
                {
                    melodee_screen_input_t next { MELODEE_SCREEN_STEP, 1, 0 }, push { MELODEE_SCREEN_PUSH, 0, 1 };
                    if (! onSeq)
                        CHECK (screen->input (plugin, &next));
                    onSeq = true;
                    CHECK (screen->input (plugin, &push));
                    continue;
                }
                if (e.kind == Event::param)
                    a.param = { { sizeof (clap_event_param_value), t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id (e.name), nullptr,
                                -1, -1, -1, -1, e.value };
                else
                    a.note = { { sizeof (clap_event_note), t, CLAP_CORE_EVENT_SPACE_ID, (uint16_t) (e.kind == Event::on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 },
                               -1, 0, 0, (int16_t) e.key, e.value };
                list.push_back (a);
            }
            std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; });
            clap_input_events in { &list, [] (const clap_input_events* e) { return (uint32_t) static_cast<const std::vector<Any>*> (e->ctx)->size(); },
                                   [] (const clap_input_events* e, uint32_t i) { return &(*static_cast<const std::vector<Any>*> (e->ctx))[i].header; } };
            clap_output_events out { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };
            const Transport tr = transport (at);
            clap_event_transport t {};
            t.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (tr.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            t.tempo = 120.0;
            t.song_pos_beats = (clap_beattime) std::llround (tr.beats * (double) CLAP_BEATTIME_FACTOR);
            float* outs[2] = { left.data() + at, right.data() + at };
            clap_audio_buffer buffer {};
            buffer.data32 = outs;
            buffer.channel_count = 2;
            clap_process p {};
            p.steady_time = (int64_t) at;
            p.frames_count = n;
            p.transport = &t;
            p.audio_outputs = &buffer;
            p.audio_outputs_count = 1;
            p.in_events = &in;
            p.out_events = &out;
            CHECK (plugin->process (plugin, &p) != CLAP_PROCESS_ERROR);
        }
        return left;
    }
    // Settings before anything plays: one stopped frame.
    void setup (const std::vector<std::pair<std::string, double>>& values)
    {
        std::vector<Event> list;
        for (const auto& [name, v] : values)
            list.push_back ({ 0, Event::param, 0, v, name });
        run (1, list, [] (uint64_t) { return stopped(); });
    }
};

double peakOf (const std::vector<float>& x, size_t from, size_t to = SIZE_MAX)
{
    double p = 0.0;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        p = std::max (p, (double) std::fabs (x[i]));
    return p;
}

// Hold lit (0.6) and nothing else ringing on: the gate closes within a few ms of Hold's end. No step strikes.
void dry (Instance& a, double sequencer)
{
    a.setup ({ { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 }, { "Decay", 0.0 }, { "Ring", 0.0 }, { "Density", 0.5 },
               { "Sequencer", sequencer } });
    for (int s = 1; s <= 16; ++s)
        a.setup ({ { "Trig " + std::to_string (s), 0.0 } });
}
std::function<Transport (uint64_t)> stoppedAll() { return [] (uint64_t) { return stopped(); }; }

// Hold follows the key that struck the drum last: it ends when that key is let go, whatever older key is still held.
// To 0.2.4 a key held from before (here 72, Notes: Strokes) kept it lit until it was let go too.
void testLastKey()
{
    struct Case { const char* what; double sequencer; std::vector<Event> out, ref; };
    const Case cases[] = {
        { "two keys, the newer let go", 0.0,
          { keyOn (1000, 72), keyOn (8000, 68), keyOff (9000, 68), keyOff (40000, 72) },
          { keyOn (1000, 72), keyOff (8000, 72), keyOn (8000, 68), keyOff (9000, 68) } },
        { "a key, the audition, a newer key let go", 1.0,
          { keyOn (1000, 72), audition (2048), keyOn (8000, 68), keyOff (9000, 68), keyOff (40000, 72) },
          { keyOn (1000, 72), audition (2048), keyOff (7000, 72), keyOn (8000, 68), keyOff (9000, 68) } },
    };
    for (const auto& c : cases)
    {
        Instance a, b;
        dry (a, c.sequencer), dry (b, c.sequencer);
        a.setup ({ { "Notes", a.valueOf ("Notes", "Strokes") } }), b.setup ({ { "Notes", b.valueOf ("Notes", "Strokes") } });
        const auto out = a.run (48000 * 3, c.out, stoppedAll());
        const auto ref = b.run (48000 * 3, c.ref, stoppedAll());
        CHECK (peakOf (ref, 8000, 9000) > 1e-3 && peakOf (ref, 48000 * 2) < 1e-5);
        if (out != ref)
            std::printf ("  %s: %.6f at 12,000-36,000, not %.6f\n", c.what, peakOf (out, 12000, 36000), peakOf (ref, 12000, 36000));
        CHECK (out == ref);
    }
}

// A gate end is never lost. Bounce at 1/2 drops a ball of 15 hits still to come over 14 s: five Plays and Stops
// in 8 s left them all queued to 0.2.4, the queue (48) filled, and the audition's gate end after them was dropped:
// Hold stayed lit. Stop now takes the sequence's hits with it; and with the queue full of the keys' own (a burst of
// rolls), a gate end takes the place of a hit.
void testFullQueue()
{
    {
        Instance a;
        dry (a, 1.0);
        a.setup ({ { "Seq Mode", a.valueOf ("Seq Mode", "Bounce") }, { "Rate", a.valueOf ("Rate", "1/2") }, { "Density", 1.0 },
                   { "Trig 1", 1.0 } });
        const auto transport = [] (uint64_t at) { // Play for 0.8 s from every 1.6 s up to 8 s
            const uint64_t period = 76800, start = at / period * period;
            return at < 384000 && at - start < 38400 ? playingFrom (start, at) : stopped();
        };
        const auto out = a.run (48000 * 11, { audition (408000) }, transport);
        CHECK (peakOf (out, 0, 4800) > 0.01);
        CHECK (peakOf (out, 408000, 410000) > 1e-3); // the audition, lit for its gate (3,000 frames)
        if (peakOf (out, 456000) >= 1e-5)
            std::printf ("  five Plays and Stops of Bounce, then the audition: %.6f at 9.5 s on\n", peakOf (out, 456000));
        CHECK (peakOf (out, 456000) < 1e-5);
    }
    {
        // Sequencer off, Notes: Strokes: 11 rolls (keys 42, 50 ... 122) twice in one frame, 66 hits to come within
        // 106 ms, then the audition.
        Instance a;
        dry (a, 0.0);
        a.setup ({ { "Notes", a.valueOf ("Notes", "Strokes") } });
        std::vector<Event> list;
        for (int round = 0; round < 2; ++round)
            for (int key = 42; key < 128; key += 8)
                list.push_back (keyOn (1000, key, 0.5));
        for (int key = 42; key < 128; key += 8)
            list.push_back (keyOff (1100, key));
        list.push_back (audition (2048));
        const auto out = a.run (48000 * 3, list, stoppedAll());
        CHECK (peakOf (out, 2048, 4800) > 1e-3);
        if (peakOf (out, 48000 * 2) >= 1e-5)
            std::printf ("  a burst of rolls fills the queue, then the audition: Hold stays lit, %.6f two seconds on\n", peakOf (out, 48000 * 2));
        CHECK (peakOf (out, 48000 * 2) < 1e-5);
    }
}

// A block's first-frame events in the MPC glue's order: settings, then the transport, then notes, then the page's
// presses. To 0.2.4 notes and presses came before the transport: a key on Play's frame struck the drum as if stopped,
// and one on Stop's frame transposed a pattern that had stopped, sounding nothing. (Every step here is a rest.)
void testFrameZero()
{
    const uint64_t edge = 24064; // a block's first frame
    const auto playFrom = [&] (uint64_t at) { return at < edge ? stopped() : playingFrom (edge, at); };
    const auto stopAt = [&] (uint64_t at) { return at < edge ? playingFrom (0, at) : stopped(); };
    {
        Instance a;
        dry (a, 1.0);
        const auto out = a.run (48000, { keyOn (edge, 64), keyOff (edge + 3000, 64) }, playFrom);
        if (peakOf (out, edge) >= 1e-5)
            std::printf ("  a key on Play's frame strikes: %.6f\n", peakOf (out, edge));
        CHECK (peakOf (out, edge) < 1e-5);
    }
    {
        Instance a, b;
        dry (a, 1.0), dry (b, 1.0);
        const auto out = a.run (48000, { keyOn (edge, 64), keyOff (edge + 3000, 64) }, stopAt);
        const auto ref = b.run (48000, { keyOn (edge, 64), keyOff (edge + 3000, 64) }, stoppedAll());
        CHECK (peakOf (ref, edge, edge + 3000) > 1e-3);
        if (out != ref)
            std::printf ("  a key on Stop's frame: %.6f while held, not %.6f\n", peakOf (out, edge, edge + 3000), peakOf (ref, edge, edge + 3000));
        CHECK (out == ref);
    }
    // The audition sent with Stop holds its gate as one sent stopped does.
    {
        Instance a, b;
        dry (a, 1.0), dry (b, 1.0);
        const auto out = a.run (48000, { audition (edge) }, stopAt);
        const auto ref = b.run (48000, { audition (edge) }, stoppedAll());
        CHECK (peakOf (ref, edge + 2000, edge + 2900) > 1e-3);
        if (out != ref)
            std::printf ("  an audition on Stop's block: %.6f in its gate, not %.6f\n", peakOf (out, edge + 2000, edge + 2900),
                         peakOf (ref, edge + 2000, edge + 2900));
        CHECK (out == ref);
    }
}
} // namespace

int main()
{
    if (! hdslave_inner_clap_entry.init ("hold_test"))
        return 1;
    factory = static_cast<const clap_plugin_factory*> (hdslave_inner_clap_entry.get_factory (CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr || factory->get_plugin_count (factory) < 1)
        return 1;
    testLastKey();
    std::printf ("last key     %s\n", failures ? "FAILED" : "ok");
    const int before = failures;
    testFullQueue();
    std::printf ("full queue   %s\n", failures > before ? "FAILED" : "ok");
    const int before2 = failures;
    testFrameZero();
    std::printf ("frame zero   %s\n", failures > before2 ? "FAILED" : "ok");
    hdslave_inner_clap_entry.deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
