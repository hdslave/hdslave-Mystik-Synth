// Hold in the desktop copy: the engine and glue of src/ (the inner plug-in, as the desktop bundle runs it), played as a
// host plays it, with keys, the page's Shift + Push on SEQ (the audition) and the transport at block boundaries. It
// holds the Hold rules the MPC build's tests/host.cpp holds for the same code: Hold follows the key that struck the
// drum last, a step's Hold is not cut by an older strike's gate end, a gate end is never lost, Stop takes the
// sequence's queued hits, a block's first-frame notes and presses come after its transport, a pre-roll's steps have
// their gate ends, and a late wrap's step plays as told on time.
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
// SEQ's Shift + Push before the block it lands in, on step 1 or `cursor` steps on from it (the wheel turned first)
Event audition (uint64_t f, int cursor = 0) { return { f, Event::audition, cursor }; }

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
                    melodee_screen_input_t next { MELODEE_SCREEN_STEP, 1, 0 }, wheel { MELODEE_SCREEN_WHEEL, e.key, 0 },
                        push { MELODEE_SCREEN_PUSH, 0, 1 };
                    if (! onSeq)
                        CHECK (screen->input (plugin, &next));
                    onSeq = true;
                    if (e.key != 0)
                        CHECK (screen->input (plugin, &wheel));
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

double rmsOf (const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += (double) x[i] * x[i];
    return to > from ? std::sqrt (sum / (double) (to - from)) : 0.0;
}
double floorOf (const std::vector<float>& x, size_t from, size_t to) // the quietest 10 ms (480 frames) in [from, to)
{
    double low = 1e9;
    for (size_t w = from; w + 480 <= to; w += 480)
        low = std::min (low, rmsOf (x, w, w + 480));
    return low;
}
double dbOf (double v) { return 20.0 * std::log10 (v + 1e-12); }
std::function<Transport (uint64_t)> looping (double beats) // 120 BPM, a loop of `beats` from the song's top
{
    return [=] (uint64_t at) { return Transport { true, std::fmod ((double) at * 2.0 / 48000.0, beats) }; };
}

// A step that plays takes the drum from every older strike, and their gate ends still to come no longer end its Hold.
// A sine through the gate (the oscillator alone), so the output's quietest 10 ms shows the light: lit at Hold's level
// it stays near the hit's, unlit it falls to silence. To 0.2.5's first build (and 0.2.4) an older strike's end cut the
// newer one's Hold: Bounce at 1/2 in a one-bar loop (from bar 8 an older ball's end lands 0.6 into every bar), a 3-beat
// loop at 1/2 whose step 2's gate runs past the wrap (Gate 50 %; with Gate 100 % and the wrap told late, step 1's end
// lands in step 2), and a Rate change inside a step's gate (here to a faster one).
void testGateEnds()
{
    auto tone = [] (Instance& a, const std::vector<std::pair<std::string, double>>& more) {
        std::vector<std::pair<std::string, double>> v { { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 },
                                                         { "Feedback", 0.0 }, { "Drift", 0.0 }, { "Bend", 0.0 }, { "Snap", 0.0 }, { "Human", 0.0 },
                                                         { "Decay", 0.0 }, { "Ring", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Hold", 0.6 } };
        v.insert (v.end(), more.begin(), more.end());
        a.setup (v);
    };
    auto everyStep = [] (Instance& a) {
        for (int s = 1; s <= 16; ++s)
            a.setup ({ { "Trig " + std::to_string (s), 1.0 }, { "Chance " + std::to_string (s), 1.0 } });
    };
    const double lit = std::pow (10.0, -30.0 / 20.0);
    {
        Instance a;
        tone (a, { { "Seq Mode", a.valueOf ("Seq Mode", "Bounce") }, { "Rate", a.valueOf ("Rate", "1/2") }, { "Density", 1.0 } });
        const auto out = a.run (96000 * 20, {}, looping (4.0));
        double low = 1.0;
        for (int bar = 10; bar < 20; ++bar)
            low = std::min (low, floorOf (out, (size_t) bar * 96000 + 57600, (size_t) (bar + 1) * 96000));
        CHECK (peakOf (out, 0, 4800) > 0.1);
        if (low <= lit)
            std::printf ("  Bounce at 1/2 in a one-bar loop: the last 0.4 of bars 10-19 falls to %.1f dB\n", dbOf (low));
        CHECK (low > lit);
    }
    for (double gate : { 0.5, 1.0 })
        for (uint32_t block : { 64u, 128u }) // the wrap (frame 72,000) a block's first frame, or told 64 frames late
        {
            Instance a;
            tone (a, { { "Rate", a.valueOf ("Rate", "1/2") }, { "Gate", gate } });
            everyStep (a);
            const auto out = a.run (72000 * 8, {}, looping (3.0), block);
            // Each pass's step 1 from 0.2 beat to 0.06 before its gate ends (Gate of its 2.16 beats), step 2 at 2.3-3.0.
            const size_t firstEnd = (size_t) std::lround (gate * 51840.0) - 1440;
            double first = 1.0, second = 1.0;
            for (int pass = 0; pass < 8; ++pass)
            {
                const size_t at = (size_t) pass * 72000;
                first = std::min (first, floorOf (out, at + 4800, at + firstEnd));
                second = std::min (second, floorOf (out, at + 55200, at + 72000));
            }
            if (first <= lit || second <= lit)
                std::printf ("  a 3-beat loop, Gate %.0f %%, the wrap told %s: step 1 falls to %.1f dB, step 2 to %.1f dB\n", gate * 100.0,
                             block == 64 ? "on time" : "late", dbOf (first), dbOf (second));
            CHECK (first > lit);
            CHECK (second > lit);
        }
    {
        // Swing 50 %, Gate 75 %: step 1 at 0 (1/2, its gate to 1.5 beats), Rate 1/4 from beat 0.5: the next step plays
        // at beat 1, its gate to 1.75.
        Instance a;
        tone (a, { { "Rate", a.valueOf ("Rate", "1/2") }, { "Swing", 0.5 }, { "Gate", 0.75 } });
        everyStep (a);
        const auto out = a.run (48000, { { 12000, Event::param, 0, a.valueOf ("Rate", "1/4"), "Rate" } }, looping (1000.0));
        const double low = floorOf (out, 38400, 42000); // 1.6-1.75 beats
        if (low <= lit)
            std::printf ("  a Rate change in a step's gate: the next step falls to %.1f dB\n", dbOf (low));
        CHECK (low > lit);
    }
}

double ceilOf (const std::vector<float>& x, size_t from, size_t to) // the loudest 10 ms (480 frames) in [from, to)
{
    double high = 0.0;
    for (size_t w = from; w + 480 <= to; w += 480)
        high = std::max (high, rmsOf (x, w, w + 480));
    return high;
}
double pitchOf (const std::vector<float>& x, size_t from, size_t to) // Hz, from the upward zero crossings
{
    long first = -1, last = -1, count = 0;
    for (size_t i = from + 1; i < std::min (to, x.size()); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            if (first < 0) first = (long) i;
            last = (long) i, ++count;
        }
    return count > 1 ? (double) (count - 1) * 48000.0 / (double) (last - first) : 0.0;
}

// Edges of the clock, the same sine through the gate:
// - A host's pre-roll (the song from beat -2) plays its steps before the top with their gate ends. To 0.2.5's second
//   build a clock below 0 was taken for a key's strike: step 16 at beat -1 lit Hold for good.
// - A loop's wrap told a block late: the step the sequence decides again really began on the wrap's frame, inside the
//   block before, so it plays as the transport is told, before that frame's notes and presses, as told on time, and
//   from the state its silent step was decided from (Turing's register and Brownian's walk move once). 0.2.5's third
//   build played it after them (an audition there lost its strike, a key transposed the step), and to its second
//   build it was decided again from the state the silent step had moved: Turing and Brownian moved twice.
// - A call of no frames that tells the wrap, then a Stop: nothing strikes after the Stop.
// - A step drops the audition's flam and roll hits still to come: to 0.2.5's third build a Roll audition on a step's
//   frame played its later hits, at its own pitch, over the step.
void testClockEdges()
{
    auto tone = [] (Instance& a, int step) {
        a.setup ({ { "Source", 0.0 }, { "FM Index", 0.0 }, { "Timbre", 0.0 }, { "Bloom", 0.0 }, { "Feedback", 0.0 }, { "Drift", 0.0 },
                   { "Bend", 0.0 }, { "Snap", 0.0 }, { "Human", 0.0 }, { "Decay", 0.0 }, { "Ring", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 },
                   { "Hold", 0.6 }, { "Swing", 0.5 }, { "Gate", 0.5 }, { "Rate", a.valueOf ("Rate", "1/4") } });
        for (int s = 1; s <= 16; ++s)
            a.setup ({ { "Trig " + std::to_string (s), s == step ? 1.0 : 0.0 }, { "Chance " + std::to_string (s), 1.0 } });
        a.setup ({ { "Pitch 5", 5.0 } }); // step 5 a fifth up, to tell its strikes apart
    };
    const double lit = std::pow (10.0, -30.0 / 20.0), bright = std::pow (10.0, -12.0 / 20.0);
    {
        Instance a;
        tone (a, 16);
        const auto out = a.run (60000, {}, [] (uint64_t at) { return Transport { true, -2.0 + (double) at * 2.0 / 48000.0 }; });
        const double during = floorOf (out, 26400, 33600), after = peakOf (out, 43200, 48000); // beats -0.9 to -0.6, -0.2 to 0
        if (during <= lit || after >= 1e-3)
            std::printf ("  a pre-roll: step 16 at beat -1 %.1f dB in its gate, %.1f dB at -0.2 to 0\n", dbOf (during), dbOf (after));
        CHECK (during > lit);
        CHECK (after < 1e-3);
    }
    // A 3-beat loop at 1/4, step 1 a Hit: the wrap (frame 72,000) told on time with 64-frame blocks, at 72,064 with
    // 128. Events at 72,064 do the same either way: Hold's level (within 6 dB of its -6 dB here) or dark (under -30 dB)
    // through the same stretches, and the same pitch.
    struct Case
    {
        const char* what;
        std::vector<Event> events;
        size_t litFrom, litTo, darkFrom, darkTo, pitchFrom, pitchTo; // 0: no such check
    };
    const Case cases[] = {
        { "the audition (step 1)", { audition (72064) }, 72600, 74800, 82000, 92000, 0, 0 },
        { "key 65 (it transposes the steps after)", { keyOn (72064, 65), keyOff (73000, 65) }, 74000, 83500, 0, 0, 73000, 83000 },
    };
    for (const auto& c : cases)
    {
        std::vector<float> run[2];
        for (int late = 0; late < 2; ++late)
        {
            Instance a;
            tone (a, 1);
            run[late] = a.run (96000, c.events, looping (3.0), late ? 128 : 64);
        }
        for (int late = 0; late < 2; ++late)
        {
            const auto& x = run[late];
            const char* told = late ? "told late" : "on time";
            if (c.litTo > 0 && floorOf (x, c.litFrom, c.litTo) <= bright)
                std::printf ("  the wrap %s, %s: under Hold's level at %zu-%zu\n", told, c.what, c.litFrom, c.litTo);
            if (c.litTo > 0)
                CHECK (floorOf (x, c.litFrom, c.litTo) > bright);
            if (c.darkTo > 0 && ceilOf (x, c.darkFrom, c.darkTo) >= lit)
                std::printf ("  the wrap %s, %s: lit at %zu-%zu\n", told, c.what, c.darkFrom, c.darkTo);
            if (c.darkTo > 0)
                CHECK (ceilOf (x, c.darkFrom, c.darkTo) < lit);
        }
        if (c.pitchTo > 0)
        {
            const double onTime = pitchOf (run[0], c.pitchFrom, c.pitchTo), told = pitchOf (run[1], c.pitchFrom, c.pitchTo);
            if (std::fabs (told - onTime) > onTime * 0.01)
                std::printf ("  %s, the wrap told late: %.1f Hz, on time %.1f Hz\n", c.what, told, onTime);
            CHECK (onTime > 20.0);
            CHECK (std::fabs (told - onTime) <= onTime * 0.01);
        }
    }
    // Turing, and Brownian with most steps resting, at 1/16 with Length 12 in the 3-beat loop: told late sounds exactly
    // as told on time (the step at the wrap's frame is step 1 either way).
    for (int brownian = 0; brownian < 2; ++brownian)
    {
        std::vector<float> run[2];
        for (int late = 0; late < 2; ++late)
        {
            Instance a;
            a.setup ({ { "Source", 1.0 }, { "Snap", 0.0 }, { "Ring", 0.0 }, { "Decay", 0.0 }, { "Bend", 0.0 }, { "Drift", 0.0 }, { "Bloom", 0.0 },
                       { "Timbre", 0.0 }, { "Echo", 0.0 }, { "Space", 0.0 }, { "Memory", 0.0 }, { "Swing", 0.5 }, { "Density", 0.5 },
                       { "Position", 0.0 }, { "Human", 0.0 }, { "Rate", a.valueOf ("Rate", "1/16") }, { "Length", 12.0 } });
            for (int s = 1; s <= 16; ++s)
                a.setup ({ { "Trig " + std::to_string (s), brownian && s != 1 && s != 7 ? 0.0 : 1.0 }, { "Stroke " + std::to_string (s), 0.0 },
                           { "Chance " + std::to_string (s), 1.0 }, { "Pitch " + std::to_string (s), 0.0 } });
            if (brownian)
                a.setup ({ { "Direction", a.valueOf ("Direction", "Brownian") }, { "Seed", 1.0 } });
            else
                a.setup ({ { "Seq Mode", a.valueOf ("Seq Mode", "Turing") }, { "Mutate", 0.3 }, { "Density", 0.6 }, { "Seed", 4.0 } });
            run[late] = a.run (199680, {}, looping (3.0), late ? 128 : 64);
        }
        CHECK (peakOf (run[0], 0, run[0].size()) > 0.01);
        if (run[1] != run[0])
        {
            size_t at = 0;
            while (at < run[0].size() && run[0][at] == run[1][at])
                ++at;
            std::printf ("  %s, the wrap told late: first differs from on time at %zu\n", brownian ? "Brownian" : "Turing", at);
        }
        CHECK (run[1] == run[0]);
    }
    // A call of no frames tells the wrap, then Stop on the same frame: the step's Hold ends at the Stop.
    {
        Instance a;
        tone (a, 1);
        std::vector<float> left (96000, 0.0f), right (96000, 0.0f);
        uint64_t at = 0;
        for (int call = 0; at < 96000; ++call)
        {
            const uint32_t n = call == 563 ? 0 : (uint32_t) std::min<uint64_t> (128, 96000 - at);
            const Transport tr = call <= 563 ? looping (3.0) (at) : stopped();
            clap_event_transport t {};
            t.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (tr.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            t.tempo = 120.0;
            t.song_pos_beats = (clap_beattime) std::llround (tr.beats * (double) CLAP_BEATTIME_FACTOR);
            clap_input_events in { nullptr, [] (const clap_input_events*) { return 0u; },
                                   [] (const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; } };
            clap_output_events out { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };
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
            CHECK (a.plugin->process (a.plugin, &p) != CLAP_PROCESS_ERROR);
            at += n;
        }
        if (ceilOf (left, 79000, 90000) >= lit)
            std::printf ("  a call of no frames tells the wrap, then Stop: lit at 79,000-90,000\n");
        CHECK (peakOf (left, 72064, 74000) > 0.1);
        CHECK (ceilOf (left, 79000, 90000) < lit);
    }
    // Step 5 a Roll, auditioned on step 1's frame (Play from the top): the step drops the roll's later hits.
    {
        std::vector<float> run[2];
        for (int heard = 0; heard < 2; ++heard)
        {
            Instance a;
            tone (a, 1);
            a.setup ({ { "Stroke 5", a.valueOf ("Stroke 5", "Roll") } });
            run[heard] = a.run (24000, heard ? std::vector<Event> { audition (0, 4) } : std::vector<Event> {}, looping (1000.0));
        }
        const double ref = pitchOf (run[0], 6000, 11000), out = pitchOf (run[1], 6000, 11000);
        if (std::fabs (out - ref) > ref * 0.01)
            std::printf ("  a Roll audition on step 1's frame: %.1f Hz after its hits, step 1 alone %.1f Hz\n", out, ref);
        CHECK (ref > 20.0);
        CHECK (std::fabs (out - ref) <= ref * 0.01);
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
    const int before3 = failures;
    testGateEnds();
    std::printf ("gate ends    %s\n", failures > before3 ? "FAILED" : "ok");
    const int before4 = failures;
    testClockEdges();
    std::printf ("clock edges  %s\n", failures > before4 ? "FAILED" : "ok");
    hdslave_inner_clap_entry.deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
