// Mystik's page on Melodee's screen (melodee_screen.h): 320 x 218 under the soft keys.
//
//   PLAY  the vactrol's light as a glowing orb (the model's own light level), the ring of steps around it (accents,
//         slides, the step playing), a phosphor scope; the wheel picks which three controls K1-K3 hold, Push starts and
//         stops the sequencer, Shift + Push regenerates.
//   SEQ   the 16 steps as lanes (pitch, trig, stroke, chance): the wheel picks a step, Push cycles its trig, Shift +
//         Push plays it; K1-K3 are its Pitch, Stroke and Chance.
//   SKIN OSC GATE FX MODE   lists (the wheel moves, Push edits, K1-K3 hold the row and the next two) beside a picture
//         of what they do: the membrane's modes, the folded wave, the gate's response at its present light and the
//         light's trail, the effects, the sequencer mode's own machine.
// Minus and Plus turn the pages; Shift shows the controls. Each region is redrawn only when what it shows changed, and
// animation (the orb, the scope, the playhead, the gate's curve) is held to 25 frames a second.
#pragma once

#include "engine.h"
#include "params.h"
#include "ui/page.h"

#include <atomic>
#include <iterator>

namespace mystik
{
// The page's own button: Shift + Push on SEQ plays the step picked (ids from here on are the screen's).
constexpr uint32_t auditionId = 1000;

class Screen final
{
    using Raster = hdui::Raster;
    using Host = hdui::Host;
    using Ink = hdui::Ink;

public:
    Screen (const std::atomic<float>* values, const Telemetry& telemetry) : values (values), tel (telemetry) {}

    enum Page { play, seq, skin, osc, gate, fx, mode, pageCount };

    void knobs (uint32_t ids[3]) const
    {
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t id = cardField (k);
            ids[k] = id != hdui::noParam && paramTable[paramIndex (id)].kind != Kind::button ? id : hdui::noParam;
        }
    }

    bool animating() const { return animated; }

    bool input (hdui::Key key, int amount, bool shift, hdui::Edits& out)
    {
        using hdui::Key;
        touched = true;
        if (key == Key::nudge && ! editing)
        {
            page = (page + (amount > 0 ? 1 : -1) + pageCount) % pageCount;
            return true;
        }
        if (page == play)
        {
            if (key == Key::wheel)
            {
                trio = ((trio + amount) % trioCount + trioCount) % trioCount;
                return true;
            }
            if (key == Key::push)
            {
                if (shift) out.press (ParamId::regenerate);
                else out.set (ParamId::seqOn, live (ParamId::seqOn) >= 0.5f ? 0.0f : 1.0f);
                return true;
            }
            return false;
        }
        if (page == seq)
        {
            if (key == Key::wheel)
            {
                cursor = std::clamp (cursor + amount, 0, steps - 1);
                return true;
            }
            if (key == Key::push)
            {
                if (shift) out.press (auditionId + (uint32_t) cursor);
                else out.set (ParamId::trig1 + (uint32_t) cursor, (float) (((int) live (ParamId::trig1 + (uint32_t) cursor) + 1) % 5));
                return true;
            }
            return false;
        }
        const List& l = list();
        int& row = rows[page];
        row = std::clamp (row, 0, l.count - 1);
        const uint32_t id = l.ids[row];
        const Param& p = paramTable[paramIndex (id)];
        if (key == Key::push)
        {
            if (p.kind == Kind::button) out.press (id);
            else editing = ! editing;
            return true;
        }
        if (! editing && key == Key::wheel)
        {
            row = std::clamp (row + amount, 0, l.count - 1);
            return true;
        }
        if (editing && (key == Key::wheel || key == Key::nudge))
        {
            const float delta = p.kind == Kind::continuous ? (p.max - p.min) / (key == Key::nudge ? 16.0f : (shift ? 1024.0f : 128.0f))
                                                           : (key == Key::nudge ? std::max (1.0f, std::round ((p.max - p.min) / 8.0f)) : 1.0f);
            const float before = live (id);
            const float next = clampParam (p, before + delta * (float) amount);
            if (next != before) out.set (id, next);
            return true;
        }
        return false;
    }

    bool paint (const Host& h, uint32_t* pixels, int stride, bool whole)
    {
        whole |= first;
        first = false;
        // What changed: values, the audio thread's report, the pots, the clock.
        bool params = false;
        for (uint32_t i = 0; i < paramCount; ++i)
        {
            const float v = values[i].load (std::memory_order_relaxed);
            float& s = seen[paramTable[i].id];
            params |= v != s;
            s = v;
        }
        const float light = tel.light.load (std::memory_order_relaxed);
        const int place = tel.place.load (std::memory_order_relaxed);
        const uint32_t strikes = tel.strikes.load (std::memory_order_acquire);
        const uint32_t generation = tel.generationAt.load (std::memory_order_acquire);
        const bool running = tel.running.load (std::memory_order_relaxed);
        if (strikes != seenStrikes)
        {
            seenStrikes = strikes;
            if (! whole || h.nowMs > 0)
                strikeAt = h.nowMs, strikeAccent = tel.accent.load (std::memory_order_relaxed), struck = true;
        }
        const bool rippling = struck && h.nowMs >= strikeAt && h.nowMs - strikeAt < 260;
        const bool glowing = light > 0.0015f || lightShown > 0.0015f;
        animated = glowing || rippling || running || scopeLive;
        const bool frame = whole || params || touched || h.nowMs - frameAt >= 40 || h.nowMs < frameAt;
        bool pots = false;
        for (int k = 0; k < 3; ++k)
            pots |= h.pot[k] != lastHost.pot[k] || h.engaged[k] != lastHost.engaged[k];
        const bool chrome = whole || page != shownPage || touched;
        if (! frame && ! pots && ! chrome && h.shift == lastHost.shift)
            return false;
        if (frame)
            frameAt = h.nowMs;
        bool drew = false;

        const bool moved = place != shownPlace || running != shownRunning;
        if (chrome) // a new page: what the last one drew between this one's regions goes
            drew |= picture.region<320, 142> (pixels, stride, whole, 0, 26, [] (Raster&) {});
        if (chrome || params)
            drew |= picture.region<206, 26> (pixels, stride, whole, 0, 0, [&] (Raster& r) { header (r); });
        if (chrome || params || moved)
            drew |= picture.region<114, 26> (pixels, stride, whole, 206, 0, [&] (Raster& r) { status (r, place, running); });

        switch (page)
        {
            case play:
                if (chrome || params || moved || (frame && (glowing || rippling)))
                    drew |= picture.region<160, 142> (pixels, stride, whole, 0, 26, [&] (Raster& r) { ringAndOrb (r, h, light, place, running); });
                if (chrome || frame)
                    drew |= picture.region<158, 66> (pixels, stride, whole, 162, 26, [&] (Raster& r) { scope (r); });
                if (chrome || params || moved || (frame && glowing))
                    drew |= picture.region<158, 74> (pixels, stride, whole, 162, 94, [&] (Raster& r) { info (r, light); });
                break;
            case seq:
                if (chrome || params || moved)
                    drew |= picture.region<320, 142> (pixels, stride, whole, 0, 26, [&] (Raster& r) { lanes (r, place, running); });
                break;
            default:
                if (chrome || params)
                    drew |= picture.region<186, 142> (pixels, stride, whole, 0, 26, [&] (Raster& r) { listRows (r); });
                if (chrome || params || moved || generation != shownGeneration || (frame && glowing && (page == gate)))
                    drew |= picture.region<132, 142> (pixels, stride, whole, 188, 26, [&] (Raster& r) { picturePanel (r, light, place, running); });
                break;
        }
        if (chrome || params || pots || h.shift != lastHost.shift || h.accent != lastHost.accent)
            drew |= picture.region<320, 48> (pixels, stride, whole, 0, 170, [&] (Raster& r) {
                if (h.shift) legend (r);
                else cards (r, h);
            });

        shownPage = page;
        shownPlace = place;
        shownRunning = running;
        shownGeneration = generation;
        lightShown = light;
        lastHost = h;
        touched = false;
        if (! rippling)
            struck = false;
        return drew;
    }

private:
    // --- Lists ------------------------------------------------------------------------------------------------------
    struct List { const uint32_t* ids; int count; };
    static constexpr uint32_t skinIds[] = { ParamId::tune, ParamId::source, ParamId::body, ParamId::ring, ParamId::position,
                                            ParamId::snap, ParamId::bend, ParamId::drift, ParamId::human };
    static constexpr uint32_t oscIds[] = { ParamId::fmIndex, ParamId::fmRatio, ParamId::feedback, ParamId::timbre, ParamId::symmetry,
                                           ParamId::bloom };
    static constexpr uint32_t gateIds[] = { ParamId::gateMode, ParamId::resonance, ParamId::strike, ParamId::decay, ParamId::open,
                                            ParamId::hold, ParamId::memory, ParamId::accent, ParamId::glide };
    static constexpr uint32_t fxIds[] = { ParamId::drive, ParamId::crush, ParamId::echo, ParamId::echoTime, ParamId::repeats,
                                          ParamId::absorb, ParamId::space, ParamId::spaceDecay, ParamId::volume };
    static constexpr uint32_t modeIds[] = { ParamId::seqOn, ParamId::seqMode, ParamId::rate, ParamId::length, ParamId::direction,
                                            ParamId::swing, ParamId::gate, ParamId::scale, ParamId::density, ParamId::rotate,
                                            ParamId::mutate, ParamId::range, ParamId::accents, ParamId::rule, ParamId::rhythm,
                                            ParamId::curve, ParamId::seed, ParamId::regenerate, ParamId::notes };
    List list() const
    {
        switch (page)
        {
            case skin: return { skinIds, (int) std::size (skinIds) };
            case osc: return { oscIds, (int) std::size (oscIds) };
            case gate: return { gateIds, (int) std::size (gateIds) };
            case fx: return { fxIds, (int) std::size (fxIds) };
            default: return { modeIds, (int) std::size (modeIds) };
        }
    }

    // PLAY's trios for K1-K3.
    static constexpr int trioCount = 6;
    static constexpr const char* trioNames[trioCount] = { "TONE", "HIT", "SKIN", "WEIRD", "GROOVE", "SPACE" };
    static constexpr uint32_t trioIds[trioCount][3] = {
        { ParamId::timbre, ParamId::decay, ParamId::resonance }, { ParamId::strike, ParamId::bloom, ParamId::accent },
        { ParamId::source, ParamId::ring, ParamId::bend }, { ParamId::fmIndex, ParamId::feedback, ParamId::symmetry },
        { ParamId::density, ParamId::mutate, ParamId::swing }, { ParamId::echo, ParamId::space, ParamId::drive },
    };

    uint32_t cardField (int k) const
    {
        if (page == play) return trioIds[trio][k];
        if (page == seq)
        {
            const uint32_t ids[3] = { ParamId::pitch1, ParamId::stroke1, ParamId::chance1 };
            return ids[k] + (uint32_t) cursor;
        }
        const List l = list();
        const int row = std::clamp (rows[page], 0, l.count - 1);
        return row + k < l.count ? l.ids[row + k] : hdui::noParam;
    }

    float live (uint32_t id) const { return values[paramIndex (id)].load (std::memory_order_relaxed); }
    float val (uint32_t id) const { return seen[id]; }
    int ival (uint32_t id) const { return (int) seen[id]; }
    template <size_t N> void formatted (uint32_t id, char (&s)[N]) const
    {
        TextContext c;
        c.tune = val (ParamId::tune), c.scale = val (ParamId::scale);
        formatParam (paramTable[paramIndex (id)], val (id), s, N, c);
    }
    int modeNow() const { return std::clamp (ival (ParamId::seqMode), 0, 5); }
    int cycleLength() const
    {
        if (modeNow() == SeqMode::rhythm) return rhythmLength (std::clamp (ival (ParamId::rhythm), 0, rhythmCount - 1));
        return std::clamp (ival (ParamId::length), 1, steps);
    }
    // Whether a MODE row matters for the sequencer mode in use (others are dimmed).
    bool rowUsed (uint32_t id) const
    {
        const int m = modeNow();
        switch (id)
        {
            case ParamId::rule: return m == SeqMode::automaton;
            case ParamId::rhythm: return m == SeqMode::rhythm;
            case ParamId::curve: return m == SeqMode::bounce;
            case ParamId::range: return m == SeqMode::turing;
            case ParamId::length: return m != SeqMode::rhythm;
            case ParamId::rotate: case ParamId::direction: return m != SeqMode::turing && m != SeqMode::bounce;
            default: return true;
        }
    }

    // --- Header -----------------------------------------------------------------------------------------------------
    void header (const Raster& r) const
    {
        using namespace hdui;
        r.large (6, 2, "MYSTIK", ink::hot, ink::ember);
        // A crescent: the moon over the drum.
        r.disc (104, 13, 6, ink::amber);
        r.disc (107, 11, 6, ink::bg);
        static const char* const names[pageCount] = { "PLAY", "SEQ", "SKIN", "OSC", "GATE", "FX", "MODE" };
        r.text (118, 4, names[page], ink::paper);
        for (int i = 0; i < pageCount; ++i)
            i == page ? r.box (118 + i * 8, 19, 5, 3, ink::amber) : r.box (119 + i * 8, 20, 3, 1, ink::rule);
        r.box (4, 25, 202, 1, ink::rule);
    }

    // The header's right end: the sequencer's mode, rate and the step it is on (redrawn as steps go by).
    void status (const Raster& r, int place, bool running) const
    {
        using namespace hdui;
        r.right (108, 4, seqModeChoices[modeNow()], ink::soft);
        char s[24];
        if (val (ParamId::seqOn) < 0.5f)
            std::snprintf (s, sizeof (s), "SEQ OFF");
        else if (! running || place < 0)
            std::snprintf (s, sizeof (s), "%s  --/%02d", rateChoices[std::clamp (ival (ParamId::rate), 0, 9)], cycleLength());
        else
            std::snprintf (s, sizeof (s), "%s  %02d/%02d", rateChoices[std::clamp (ival (ParamId::rate), 0, 9)], place + 1, cycleLength());
        r.right (108, 14, s, running ? ink::amber : ink::muted);
        r.box (0, 25, 110, 1, ink::rule);
    }

    // --- PLAY -------------------------------------------------------------------------------------------------------
    // What the ring shows at place i of the cycle: whether it plays, accent, slide, stroke, pitch (degree).
    struct Glyph { bool on, accent, slide; int stroke, degree; };
    Glyph glyphAt (int i, int len) const
    {
        Glyph g {};
        const int s = i % steps;
        const int trig = ival (ParamId::trig1 + (uint32_t) s);
        g.on = trig != Trig::rest;
        g.accent = trig == Trig::accent || trig == Trig::accentSlide;
        g.slide = trig == Trig::slide || trig == Trig::accentSlide;
        g.stroke = ival (ParamId::stroke1 + (uint32_t) s);
        g.degree = ival (ParamId::pitch1 + (uint32_t) s);
        switch (modeNow())
        {
            case SeqMode::euclid:
            {
                const int pulses = (int) std::lround (val (ParamId::density) * (float) len);
                g.on = pulses > 0 && ((i * pulses) % len) < pulses;
                break;
            }
            case SeqMode::turing:
                g.on = ((tel.turing.load (std::memory_order_relaxed) >> i) & 1u) != 0;
                g.slide = false;
                break;
            case SeqMode::automaton: g.on = ((tel.cells.load (std::memory_order_relaxed) >> i) & 1u) != 0; break;
            case SeqMode::bounce: g.on = i == 0, g.slide = false; break;
            case SeqMode::rhythm:
            {
                const int stroke = rhythmStroke (std::clamp (ival (ParamId::rhythm), 0, rhythmCount - 1), i);
                g.on = stroke >= 0, g.stroke = std::max (0, stroke), g.slide = false, g.accent = stroke == Stroke::bass;
                break;
            }
            default: break;
        }
        return g;
    }
    // Where the steps of a cycle of `len` sit on a unit circle (step 1 at the top), worked out once a length.
    struct Circle { float x[steps], y[steps]; int len = 0; };
    const Circle& circleFor (int len) const
    {
        if (circle.len != len)
        {
            for (int i = 0; i < len && i < steps; ++i)
            {
                const double a = -M_PI / 2.0 + 2.0 * M_PI * (double) i / (double) len;
                circle.x[i] = (float) std::cos (a), circle.y[i] = (float) std::sin (a);
            }
            circle.len = len;
        }
        return circle;
    }

    static Ink strokeInk (int stroke)
    {
        using namespace hdui;
        switch (stroke)
        {
            case Stroke::slap: return ink::paper;
            case Stroke::bass: return ink::amber;
            case Stroke::mute: return ink::muted;
            case Stroke::ghost: return ink::parked;
            case Stroke::flam: case Stroke::roll: return ink::violet;
            case Stroke::press: return ink::teal;
            default: return ink::soft;
        }
    }

    // The orb's inks: night, ember, amber, white heat (64 steps, dithered between neighbours).
    static Ink orbInk (int i)
    {
        using namespace hdui;
        static Ink ramp[65];
        static bool made = false;
        if (! made)
        {
            const Ink anchors[5] = { ink::bg, 0xff2c0b26, 0xffc2461a, 0xffffad45, 0xfffff0cc };
            for (int k = 0; k <= 64; ++k)
            {
                const int seg = std::min (3, k / 16), t = (k - seg * 16) * 16;
                ramp[k] = mix (anchors[seg], anchors[seg + 1], t);
            }
            made = true;
        }
        return ramp[std::clamp (i, 0, 64)];
    }

    void ringAndOrb (const Raster& r, const Host& h, float light, int place, bool running) const
    {
        using namespace hdui;
        constexpr int cx = 80, cy = 68, track = 50;
        const float lit = std::clamp (light, 0.0f, 1.0f);
        // The orb: a core whose size and heat follow the light, a soft halo, an ember always lit.
        const float glow = std::sqrt (lit);
        const int core = 7 + (int) std::lround (24.0f * glow), halo = core + 9;
        const float heat = 0.28f + 0.72f * glow;
        uint8_t level[46 * 46 * 2 + 1];
        const int reach = halo * halo;
        const float invCore = 1.0f / (float) (core * core), invReach = 1.0f / (float) reach;
        for (int d2 = 0; d2 <= reach; ++d2)
        {
            const float t = (float) d2 * invCore;
            const float inner = t < 1.0f ? (1.0f - t) * (1.0f - t) * (1.0f - 0.25f * t) : 0.0f;
            const float u = 1.0f - (float) d2 * invReach, outer = u * u * u; // about (1 - d/halo)^2, without a root
            level[d2] = (uint8_t) std::clamp ((int) ((heat * inner + 0.33f * heat * outer) * 1023.0f), 0, 255);
        }
        Ink* const pixels = r.px;
        for (int y = -halo; y <= halo; ++y)
        {
            const int span = (int) std::sqrt ((float) (reach - y * y)); // the circle's own width on this row
            Ink* const row = pixels + (cy + y) * r.w + cx;
            const uint8_t* const dither = bayer[(cy + y) & 3];
            for (int x = -span; x <= span; ++x)
            {
                const int k = (level[x * x + y * y] * 4 + dither[(cx + x) & 3]) >> 4; // 0..63, dithered
                if (k > 0) row[x] = orbInk (k);
            }
        }
        // A strike's ripple: the head's wave running out to the ring.
        if (struck && h.nowMs >= strikeAt && h.nowMs - strikeAt < 260)
        {
            const float age = (float) (h.nowMs - strikeAt) / 260.0f;
            const int radius = halo + (int) ((float) (track - 4 - halo) * age);
            r.ring (cx, cy, radius, mix (strikeAccent ? ink::magenta : ink::amber, ink::bg, (int) (age * 230.0f)));
        }
        // The ring: a track, the steps of the cycle, slides as arcs inside, the step playing lit with a ray to the orb.
        const int len = std::max (1, running && place >= 0 ? tel.cycle.load (std::memory_order_relaxed) : cycleLength());
        r.ring (cx, cy, track, ink::grid);
        const Circle& unitCircle = circleFor (len);
        float px[steps], py[steps];
        Glyph glyphs[steps];
        for (int i = 0; i < len && i < steps; ++i)
        {
            const Glyph g = glyphs[i] = glyphAt (i, len);
            const float rad = (float) track + std::clamp ((float) g.degree * 0.7f, -7.0f, 7.0f) * (g.on ? 1.0f : 0.0f);
            px[i] = (float) cx + rad * unitCircle.x[i], py[i] = (float) cy + rad * unitCircle.y[i];
        }
        for (int i = 0; i < len && i < steps; ++i)
        {
            const Glyph g = glyphs[i];
            const int x = (int) std::lround (px[i]), y = (int) std::lround (py[i]);
            const bool now = running && place == i;
            if (g.slide && g.on)
            {
                // An arc inside the ring from the step before: its chord's points pushed out to the arc's radius.
                const int j = (i + len - 1) % len;
                for (int n = 0; n <= 8; ++n)
                {
                    const float t = (float) n / 8.0f;
                    float ux = unitCircle.x[j] + (unitCircle.x[i] - unitCircle.x[j]) * t, uy = unitCircle.y[j] + (unitCircle.y[i] - unitCircle.y[j]) * t;
                    const float norm = (float) (track - 9) / std::sqrt (ux * ux + uy * uy);
                    r.dot (cx + (int) std::lround (ux * norm), cy + (int) std::lround (uy * norm), ink::teal);
                }
            }
            if (now)
            {
                r.dotted (cx + (int) std::lround ((float) (halo + 2) * (px[i] - (float) cx) / (float) track),
                          cy + (int) std::lround ((float) (halo + 2) * (py[i] - (float) cy) / (float) track), x, y, ink::amber, 2);
                r.disc (x, y, 6, mix (ink::bg, ink::amber, 90));
            }
            if (! g.on)
                r.box (x, y, 2, 2, now ? ink::amber : ink::parked);
            else if (g.accent)
                r.disc (x, y, 4, now ? ink::hot : ink::magenta);
            else
            {
                r.ring (x, y, 3, now ? ink::hot : strokeInk (g.stroke));
                r.dot (x, y, now ? ink::hot : strokeInk (g.stroke));
            }
        }
        // Step 1's tick, outside.
        r.box (cx, cy - track - 10, 1, 4, ink::muted);
        // Under the ring: the step picked or playing, as words.
        char s[40];
        const int shown = running && place >= 0 ? place : 0;
        const Glyph g = glyphAt (shown, len);
        char note[16];
        noteName (val (ParamId::tune) + degreeSemis (g.degree, ival (ParamId::scale)), note, sizeof (note));
        std::snprintf (s, sizeof (s), "%s %s", g.on ? strokeChoices[std::clamp (g.stroke, 0, 7)] : "rest", note);
        r.centre (cx, 129, s, running ? ink::soft : ink::parked);
    }

    // The phosphor scope: the voice's last 1024 samples from a rising zero crossing, min and max a column, the two
    // traces before it fading behind.
    void scope (const Raster& r)
    {
        using namespace hdui;
        r.box (0, 2, 156, 64, ink::field);
        r.frame (0, 2, 156, 64, ink::grid);
        for (int x = 2; x < 154; x += 3) r.dot (x, 34, ink::grid);
        constexpr int span = 1024, columns = 150;
        constexpr int size = 1536; // a trace and room to find its trigger: the newest of the ring
        const uint32_t end = tel.scopeAt.load (std::memory_order_acquire) - (uint32_t) size;
        float buffer[size];
        float peak = 0.0f;
        for (int i = 0; i < size; ++i)
        {
            buffer[i] = tel.scope[(end + (uint32_t) i) & (Telemetry::scopeSize - 1)].load (std::memory_order_relaxed);
            peak = std::max (peak, std::fabs (buffer[i]));
        }
        scopeLive = peak > 1e-4f;
        int start = size - span;
        for (int i = size - span; i > 1; --i)
            if (buffer[i - 1] < 0.0f && buffer[i] >= 0.0f) { start = i; break; }
        start = std::min (start, size - span);
        gainShown = std::max (peak, gainShown * 0.85f);
        const float scale = 28.0f / std::max (0.05f, gainShown);
        int8_t lo[columns], hi[columns];
        for (int c = 0; c < columns; ++c)
        {
            float a = 1e9f, b = -1e9f;
            for (int i = start + c * span / columns; i < start + (c + 1) * span / columns; ++i)
                a = std::min (a, buffer[i]), b = std::max (b, buffer[i]);
            lo[c] = (int8_t) std::clamp ((int) std::lround (-b * scale), -30, 30);
            hi[c] = (int8_t) std::clamp ((int) std::lround (-a * scale), -30, 30);
        }
        for (int age = 2; age >= 1; --age)
        {
            const Ink c = age == 2 ? 0xff2a1c12 : 0xff6a4520;
            for (int x = 0; x < columns; ++x)
                r.box (3 + x, 34 + trail[age - 1][x][0], 1, trail[age - 1][x][1] - trail[age - 1][x][0] + 1, c);
        }
        for (int x = 0; x < columns; ++x)
            r.box (3 + x, 34 + lo[x], 1, hi[x] - lo[x] + 1, ink::amber);
        for (int x = 0; x < columns; ++x)
        {
            trail[1][x][0] = trail[0][x][0], trail[1][x][1] = trail[0][x][1];
            trail[0][x][0] = lo[x], trail[0][x][1] = hi[x];
        }
        // Its name in a notch of its own, over the trace.
        r.box (1, 3, 40, 13, ink::field);
        r.box (1, 15, 40, 1, ink::grid);
        r.box (40, 3, 1, 13, ink::grid);
        r.text (5, 4, "SCOPE", ink::muted);
    }

    void info (const Raster& r, float light) const
    {
        using namespace hdui;
        r.box (0, 0, 156, 72, ink::field);
        char s[48];
        // The trio the pots hold.
        std::snprintf (s, sizeof (s), "< %s >", trioNames[trio]);
        r.text (6, 4, s, ink::paper);
        r.right (150, 4, "WHEEL", ink::parked);
        // The gate's light and where it lets sound through.
        const GateResponse g = gateResponse (ldrOhms (light), gateCircuit (val (ParamId::gateMode), val (ParamId::resonance)));
        const double hz = dominantHz (g);
        if (light < 2e-4f) std::snprintf (s, sizeof (s), "GATE  closed");
        else if (hz >= 1000.0) std::snprintf (s, sizeof (s), "GATE  %.1f kHz", hz / 1000.0);
        else std::snprintf (s, sizeof (s), "GATE  %.0f Hz", hz);
        r.text (6, 20, s, light > 2e-4f ? ink::amber : ink::muted);
        const float level = std::clamp ((std::log10 (std::max (light, 1e-4f)) + 4.0f) / 4.0f, 0.0f, 1.0f);
        r.box (6, 34, 144, 3, ink::grid);
        const int fill = (int) std::lround (level * 144.0f);
        for (int x = 0; x < fill; ++x)
            r.box (6 + x, 34, 1, 3, orbInk (16 + x * 48 / 144));
        // Scale and seed.
        formatted (ParamId::tune, s);
        char line[96];
        std::snprintf (line, sizeof (line), "%s from %s", scaleChoices[std::clamp (ival (ParamId::scale), 0, 11)], s);
        r.text (6, 42, line, ink::soft);
        std::snprintf (line, sizeof (line), "SEED %04d   %s", ival (ParamId::seed), bodyChoices[std::clamp (ival (ParamId::body), 0, bodyCount - 1)]);
        r.text (6, 56, line, ink::muted);
    }

    static double dominantHz (const GateResponse& g)
    {
        const double disc = g.a2 * g.a2 - 4.0 * g.a1 * g.a3;
        if (disc >= 0.0)
            return 2.0 * g.a1 / (g.a2 + std::sqrt (disc)) / (2.0 * M_PI);
        return g.w0 / (2.0 * M_PI);
    }

    // --- SEQ --------------------------------------------------------------------------------------------------------
    void lanes (const Raster& r, int place, bool running) const
    {
        using namespace hdui;
        constexpr int left = 8, wide = 19, zero = 47;
        const int len = cycleLength();
        int widest = 7; // the pattern's own range sets the scale (at least a 7-note octave)
        for (int i = 0; i < steps; ++i) widest = std::max (widest, std::abs (ival (ParamId::pitch1 + (uint32_t) i)));
        const float perDegree = 30.0f / (float) widest;
        r.box (4, 14, 312, 66, ink::field);
        for (int x = 6; x < 314; x += 3) r.dot (x, zero, ink::grid);
        for (int i = 0; i < steps; ++i)
        {
            const int x = left + i * wide, mid = x + wide / 2;
            const int trig = ival (ParamId::trig1 + (uint32_t) i), stroke = ival (ParamId::stroke1 + (uint32_t) i);
            const int degree = ival (ParamId::pitch1 + (uint32_t) i);
            const bool inCycle = i < len || modeNow() == SeqMode::rhythm;
            const bool now = running && place == i;
            const bool picked = cursor == i;
            const Ink base = ! inCycle ? ink::grid
                           : trig == Trig::rest ? ink::parked
                           : (trig == Trig::accent || trig == Trig::accentSlide) ? ink::magenta
                           : (trig == Trig::slide ? ink::teal : ink::soft);
            char n[12];
            std::snprintf (n, sizeof (n), "%d", i + 1);
            r.centre (mid + 1, 1, n, now ? ink::amber : (picked ? ink::paper : ink::muted));
            if (now) r.box (x + 1, 14, wide - 2, 66, mix (ink::field, ink::amber, 40));
            // Pitch: a bar from the zero line, a cap at the note.
            const int dy = std::clamp ((int) std::lround ((float) degree * perDegree), -30, 30);
            const int top = zero - std::max (dy, 0), bottom = zero - std::min (dy, 0);
            if (trig != Trig::rest) r.box (mid - 3, top, 7, bottom - top + 1, mix (ink::field, base, 70));
            r.box (mid - 4, zero - dy - 1, 9, 3, base);
            // Trig: dot, accent diamond, slide arc from the step before.
            constexpr int trigY = 86;
            if (trig == Trig::rest) r.dot (mid, trigY, ink::parked);
            if (trig == Trig::hit) r.disc (mid, trigY, 2, base);
            if (trig == Trig::accent || trig == Trig::accentSlide)
                for (int k = 0; k <= 3; ++k) r.box (mid - k, trigY - 3 + k, 2 * k + 1, 1, ink::magenta), r.box (mid - k, trigY + 3 - k, 2 * k + 1, 1, ink::magenta);
            if (trig == Trig::slide || trig == Trig::accentSlide)
                for (int k = -wide + 3; k <= 0; ++k)
                    r.dot (mid + k, trigY + 6 - (int) std::lround (3.0 * std::sin ((double) (k + wide - 3) / (wide - 3) * M_PI)), ink::teal);
            // Stroke: its letter.
            const char letter[2] = { "OSBMGFRP"[std::clamp (stroke, 0, 7)], 0 };
            r.centre (mid + 1, 96, letter, inCycle ? strokeInk (stroke) : ink::grid);
            // Chance: a short bar.
            const int h = (int) std::lround (val (ParamId::chance1 + (uint32_t) i) * 9.0f);
            r.box (mid - 4, 120 - h, 9, h, inCycle ? mix (ink::bg, ink::violet, 150) : ink::grid);
            r.box (mid - 4, 120, 9, 1, ink::grid);
            if (picked) r.frame (x, 12, wide, 112, ink::paper);
        }
        // The step picked, in words.
        const int c = cursor;
        char note[16], s[80];
        formatted (ParamId::pitch1 + (uint32_t) c, note);
        std::snprintf (s, sizeof (s), "STEP %02d  %s  %s  %s  %.0f%%", c + 1, note, trigChoices[std::clamp (ival (ParamId::trig1 + (uint32_t) c), 0, 4)],
                       strokeChoices[std::clamp (ival (ParamId::stroke1 + (uint32_t) c), 0, 7)], val (ParamId::chance1 + (uint32_t) c) * 100.0f);
        r.text (8, 128, s, ink::paper);
    }

    // --- Lists ------------------------------------------------------------------------------------------------------
    static constexpr int visible = 8, rowH = 17;
    void listRows (const Raster& r) const
    {
        using namespace hdui;
        const List l = list();
        const int row = std::clamp (rows[page], 0, l.count - 1);
        const int top = std::clamp (row - 3, 0, std::max (0, l.count - visible));
        for (int i = top; i < std::min (l.count, top + visible); ++i)
        {
            const int y = 3 + (i - top) * rowH;
            const uint32_t id = l.ids[i];
            const bool focus = i == row;
            const bool used = page != mode || rowUsed (id);
            const bool held = i >= row && i < row + 3;
            if (focus)
            {
                r.box (4, y, 176, rowH - 1, ink::selected);
                r.box (4, y, 2, rowH - 1, ink::amber);
            }
            else if (held)
                r.box (4, y + rowH - 2, 2, 1, ink::rule);
            char s[32];
            formatted (id, s);
            const auto& p = paramTable[paramIndex (id)];
            r.text (10, y + 3, p.name, ! used ? ink::parked : (focus ? ink::paper : ink::muted));
            const int end = 176;
            if (focus && editing)
            {
                const int w = (int) std::strlen (s) * 7;
                r.frame (end - w - 3, y + 1, w + 6, rowH - 3, ink::amber);
            }
            r.right (end, y + 3, s, ! used ? ink::parked : (focus ? ink::paper : ink::soft));
        }
        if (l.count > visible)
        {
            const int extent = visible * rowH;
            r.box (182, 3, 1, extent, ink::grid);
            r.box (182, 3 + top * extent / l.count, 1, visible * extent / l.count, ink::muted);
        }
    }

    void picturePanel (const Raster& r, float light, int place, bool running) const
    {
        using namespace hdui;
        r.box (0, 3, 128, 136, ink::field);
        switch (page)
        {
            case skin: membrane (r); break;
            case osc: wave (r, light); break;
            case gate: response (r, light); break;
            case fx: effects (r); break;
            default: machine (r, place, running); break;
        }
    }

    // SKIN: the body's eight modes (a bar each at its ratio, as loud as the strike at Position makes it), the drum head
    // with where it is struck.
    void membrane (const Raster& r) const
    {
        using namespace hdui;
        const Body& b = bodies[std::clamp (ival (ParamId::body), 0, bodyCount - 1)];
        const float place = val (ParamId::position);
        r.text (6, 7, "MODES", ink::muted);
        r.right (122, 7, bodyChoices[std::clamp (ival (ParamId::body), 0, bodyCount - 1)], ink::paper);
        float total = 0.0f, amp[8];
        for (int i = 0; i < 8; ++i)
            amp[i] = b.centre[i] + (b.edge[i] - b.centre[i]) * place, total += amp[i];
        const float top = *std::max_element (amp, amp + 8);
        r.box (6, 74, 116, 1, ink::rule);
        for (int i = 0; i < 8; ++i)
        {
            // From 0.4 (a body's air, under the head) to 14 times the fundamental, on a log scale.
            const int x = 6 + (int) std::lround ((std::log2 (b.ratio[i]) - std::log2 (0.4f)) / std::log2 (14.0f / 0.4f) * 112.0f);
            const int h = (int) std::lround (amp[i] / std::max (top, 1e-3f) * 48.0f);
            const Ink c = mix (ink::ember, ink::hot, (int) (b.decay[i] * 255.0f));
            r.box (x, 74 - h, 3, h, c);
        }
        (void) total;
        char s[32];
        std::snprintf (s, sizeof (s), "%.2f %.2f %.2f", b.ratio[0], b.ratio[1], b.ratio[2]);
        r.text (6, 78, s, ink::parked);
        // The head, and the strike.
        constexpr int hx = 40, hy = 112, hr = 20;
        r.disc (hx, hy, hr, ink::card);
        r.ring (hx, hy, hr, ink::rule);
        r.ring (hx, hy, hr / 2, ink::grid);
        r.disc (hx + (int) std::lround (place * (hr - 3)), hy, 3, ink::amber);
        formatted (ParamId::ring, s);
        r.text (70, 98, "RING", ink::muted);
        r.text (70, 110, s, ink::paper);
        formatted (ParamId::bend, s);
        r.text (70, 124, s, ink::soft);
    }

    // OSC: one cycle of the principal, phase-modulated and folded, at the light given (Bloom moves it).
    void wave (const Raster& r, float light) const
    {
        using namespace hdui;
        r.text (6, 7, "WAVE", ink::muted);
        char s[24];
        std::snprintf (s, sizeof (s), "x%s", ratioChoices[std::clamp (ival (ParamId::fmRatio), 0, 15)]);
        r.right (122, 7, s, ink::paper);
        const float lit = std::clamp (light, 0.0f, 1.0f);
        const float bloomLit = val (ParamId::bloom) * lit;
        const float index = val (ParamId::fmIndex) * (1.0f + 2.0f * bloomLit);
        const float pm = std::min (6.0f * index * index, 8.0f) / (2.0f * (float) M_PI);
        const float depth = std::clamp (val (ParamId::timbre) + bloomLit, 0.0f, 1.0f);
        const float g = (1.0f + 11.0f * depth * depth) / (2.0f * (float) M_PI), bias = val (ParamId::symmetry) * 0.25f;
        const float foldMix = std::clamp (20.0f * (depth + std::fabs (val (ParamId::symmetry))), 0.0f, 1.0f);
        const float ratio = ratioValues[std::clamp (ival (ParamId::fmRatio), 0, 15)];
        constexpr int x0 = 6, cy = 58, amp = 34;
        r.box (x0, cy, 116, 1, ink::grid);
        int last = cy;
        for (int x = 0; x < 116; ++x)
        {
            const float ph = (float) x / 115.0f;
            float y = sinTurns (ph + pm * sinTurns (ph * ratio));
            y += foldMix * (sinTurns (g * y + bias) - sinTurns (bias) - y);
            const int py = cy - (int) std::lround (y * (float) amp);
            r.line (x0 + x - (x > 0), last, x0 + x, py, ink::amber);
            last = py;
        }
        formatted (ParamId::timbre, s);
        r.text (6, 100, "FOLD", ink::muted);
        r.right (122, 100, s, ink::paper);
        formatted (ParamId::fmIndex, s);
        r.text (6, 114, "INDEX", ink::muted);
        r.right (122, 114, s, ink::paper);
        formatted (ParamId::feedback, s);
        r.text (6, 126, "FEEDBACK", ink::muted);
        r.right (122, 126, s, ink::paper);
    }

    // GATE: the 292's response now (20 Hz to 20 kHz, -48 to +12 dB) and the light's last 1.4 s.
    void response (const Raster& r, float light) const
    {
        using namespace hdui;
        const GateCircuit circuit = gateCircuit (val (ParamId::gateMode), val (ParamId::resonance));
        const float shown = std::max (light, openLight (val (ParamId::open)));
        const GateResponse g = gateResponse (ldrOhms (std::max (shown, 0.002f)), circuit);
        r.text (6, 7, "RESPONSE", ink::muted);
        char s[24];
        formatted (ParamId::gateMode, s);
        r.right (122, 7, s, ink::paper);
        constexpr int x0 = 6, y0 = 22, w = 116, hgt = 56;
        r.frame (x0 - 1, y0 - 1, w + 2, hgt + 2, ink::grid);
        for (int f : { 100, 1000, 10000 })
        {
            const int x = x0 + (int) std::lround (std::log10 ((double) f / 20.0) / 3.0 * (w - 1));
            for (int y = y0; y < y0 + hgt; y += 3) r.dot (x, y, ink::grid);
        }
        const int zeroY = y0 + (int) std::lround (12.0 / 60.0 * hgt);
        for (int x = x0; x < x0 + w; x += 2) r.dot (x, zeroY, ink::rule);
        int last = -1;
        for (int x = 0; x < w; ++x)
        {
            const double hz = 20.0 * std::pow (1000.0, (double) x / (w - 1));
            const double db = gateDb (g, hz);
            const int y = y0 + std::clamp ((int) std::lround ((12.0 - db) / 60.0 * hgt), 0, hgt - 1);
            r.box (x0 + x, y, 1, y0 + hgt - y, mix (ink::field, ink::ember, 60));
            if (last >= 0) r.line (x0 + x - 1, last, x0 + x, y, light > 2e-4f ? ink::amber : ink::muted);
            last = y;
        }
        // The light's trail: newest on the right.
        constexpr int ty = 86, th = 36;
        r.text (6, ty - 4, "LIGHT", ink::muted);
        const uint32_t at = tel.lightAt.load (std::memory_order_acquire);
        for (int x = 0; x < w; ++x)
        {
            const int i = (int) ((at + (uint32_t) Telemetry::lightSize - (uint32_t) w + (uint32_t) x) & (Telemetry::lightSize - 1));
            const float v = tel.lightTrail[i].load (std::memory_order_relaxed);
            const float level = std::clamp ((std::log10 (std::max (v, 1e-4f)) + 4.0f) / 4.0f, 0.0f, 1.0f);
            const int hgtBar = (int) std::lround (level * th);
            for (int y = 0; y < hgtBar; ++y)
                r.dot (x0 + x, ty + 10 + th - 1 - y, orbInk (8 + (y * 56) / th));
        }
        r.box (x0, ty + 10 + th, w, 1, ink::rule);
    }

    // FX: the chain, each stage lit by how much of it is on; the echo's taps fading by Repeats.
    void effects (const Raster& r) const
    {
        using namespace hdui;
        r.text (6, 7, "CHAIN", ink::muted);
        const char* names[4] = { "DRIVE", "CRUSH", "ECHO", "SPACE" };
        const uint32_t ids[4] = { ParamId::drive, ParamId::crush, ParamId::echo, ParamId::space };
        for (int k = 0; k < 4; ++k)
        {
            const int y = 22 + k * 18;
            const float v = val (ids[k]);
            r.frame (6, y, 116, 15, v > 0.0f ? ink::rule : ink::grid);
            r.box (7, y + 1, (int) std::lround (v * 114.0f), 13, mix (ink::field, ink::amber, 40 + (int) (v * 90.0f)));
            r.text (11, y + 2, names[k], v > 0.0f ? ink::paper : ink::parked);
        }
        // The echo: a tap every Echo Time, each Repeats as loud, alternating sides.
        const float fb = 1.05f * val (ParamId::repeats);
        float level = 1.0f;
        for (int n = 0; n < 7 && level > 0.03f; ++n)
        {
            const int x = 12 + n * 16, h = (int) std::lround (level * 24.0f);
            r.box (x, 124 - h, 5, h, n % 2 ? ink::teal : ink::amber);
            level *= std::min (fb, 1.0f);
        }
        char s[24];
        formatted (ParamId::echoTime, s);
        r.right (122, 98, s, ink::soft);
        r.box (6, 125, 116, 1, ink::rule);
    }

    // MODE: the machine at work.
    void machine (const Raster& r, int place, bool running) const
    {
        using namespace hdui;
        const int m = modeNow(), len = cycleLength();
        r.text (6, 7, seqModeChoices[m], ink::paper);
        char s[32];
        switch (m)
        {
            case SeqMode::euclid:
            case SeqMode::pattern:
            case SeqMode::rhythm:
            {
                constexpr int cx = 64, cy = 74, rad = 42;
                r.ring (cx, cy, rad, ink::grid);
                const int pulses = (int) std::lround (val (ParamId::density) * (float) len);
                int count = 0;
                for (int i = 0; i < len; ++i)
                {
                    const float a = -1.5707963f + 6.2831853f * (float) i / (float) len;
                    const int x = cx + (int) std::lround ((float) rad * std::cos (a)), y = cy + (int) std::lround ((float) rad * std::sin (a));
                    const Glyph g = glyphAt (i, len);
                    count += g.on;
                    const bool now = running && place == i;
                    if (m == SeqMode::rhythm && g.on)
                    {
                        const char letter[2] = { "OSBMG"[std::clamp (g.stroke, 0, 4)], 0 };
                        r.disc (x, y, 6, now ? ink::amber : ink::card);
                        r.centre (x + 1, y - 5, letter, now ? ink::bg : strokeInk (g.stroke));
                    }
                    else if (g.on)
                        r.disc (x, y, g.accent ? 5 : 4, now ? ink::hot : (g.accent ? ink::magenta : ink::soft));
                    else
                        r.disc (x, y, 1, now ? ink::amber : ink::parked);
                    if (now) r.line (cx, cy, x, y, mix (ink::field, ink::amber, 120));
                }
                if (m == SeqMode::euclid) std::snprintf (s, sizeof (s), "E(%d,%d)", pulses, len);
                else if (m == SeqMode::rhythm) std::snprintf (s, sizeof (s), "%s", rhythmChoices[std::clamp (ival (ParamId::rhythm), 0, rhythmCount - 1)]);
                else std::snprintf (s, sizeof (s), "%d/%d", count, len);
                r.centre (cx, cy - 6, s, ink::soft);
                break;
            }
            case SeqMode::turing:
            {
                // The register: a cell a bit, the one going round on the right.
                const uint32_t bits = tel.turing.load (std::memory_order_relaxed);
                for (int i = 0; i < len; ++i)
                {
                    const int x = 6 + (i % 8) * 15, y = 34 + (i / 8) * 18;
                    const bool on = ((bits >> (len - 1 - i)) & 1u) != 0;
                    r.box (x, y, 12, 14, on ? ink::amber : ink::card);
                    if (i == len - 1) r.frame (x - 1, y - 1, 14, 16, ink::paper);
                }
                const float lock = 1.0f - val (ParamId::mutate);
                std::snprintf (s, sizeof (s), "LOCK %3.0f%%", lock * 100.0f);
                r.text (6, 84, s, ink::soft);
                std::snprintf (s, sizeof (s), "VALUE %3u", (unsigned) ((bits * 2654435761u) >> 24));
                r.text (6, 98, s, ink::muted);
                break;
            }
            case SeqMode::automaton:
            {
                // The last 16 generations, newest at the foot: Rule's own drawing.
                const uint32_t newest = tel.generationAt.load (std::memory_order_acquire);
                const int seen = (int) tel.generationsSeen.load (std::memory_order_relaxed);
                const int cell = std::max (3, 112 / std::max (1, len));
                for (int row = Telemetry::history - seen; row < Telemetry::history; ++row)
                {
                    const uint32_t g = newest - (uint32_t) (Telemetry::history - 1 - row);
                    const uint32_t cells = tel.generations[g % Telemetry::history].load (std::memory_order_relaxed);
                    for (int i = 0; i < len; ++i)
                        if ((cells >> i) & 1u)
                            r.box (6 + i * cell, 24 + row * 6, cell - 1, 5, row == Telemetry::history - 1 ? ink::amber : mix (ink::field, ink::violet, 60 + row * 12));
                }
                if (running && place >= 0) r.box (6 + place * cell, 122, cell - 1, 2, ink::paper);
                std::snprintf (s, sizeof (s), "RULE %d", ival (ParamId::rule));
                r.text (6, 126, s, ink::soft);
                break;
            }
            case SeqMode::bounce:
            {
                // The ball's arcs: each bounce Curve as long and lower.
                const int count = 2 + (int) std::lround (val (ParamId::density) * 14.0f);
                const float ratio = bounceRatio (val (ParamId::curve));
                float total = 0.0f, gap = 1.0f;
                for (int j = 0; j < count; ++j) total += gap, gap *= ratio;
                float x = 0.0f;
                gap = 1.0f;
                for (int j = 0; j < count; ++j)
                {
                    const float wdt = gap / total * 116.0f, h = 70.0f * gap;
                    int lastY = 110;
                    for (int k = 0; k <= (int) wdt; ++k)
                    {
                        const float t = (float) k / std::max (1.0f, wdt);
                        const int y = 110 - (int) std::lround (h * 4.0f * t * (1.0f - t));
                        r.line (6 + (int) (x + (float) k) - (k > 0), lastY, 6 + (int) (x + (float) k), y, j == 0 ? ink::amber : ink::soft);
                        lastY = y;
                    }
                    r.disc (6 + (int) x, 110, 2, ink::paper);
                    x += wdt, gap *= ratio;
                }
                std::snprintf (s, sizeof (s), "%d bounces x%.2f", count, ratio);
                r.text (6, 120, s, ink::soft);
                break;
            }
            default: break;
        }
    }

    // --- K1-K3 ------------------------------------------------------------------------------------------------------
    void cards (const Raster& r, const Host& h) const
    {
        using namespace hdui;
        const Ink track = h.accent ? (h.accent | 0xff000000u) : ink::amber;
        for (int k = 0; k < 3; ++k)
        {
            const int x = 4 + 105 * k;
            const uint32_t id = cardField (k);
            r.box (x, 3, 102, 42, ink::card);
            const char tag[] = { 'K', char ('1' + k), 0 };
            r.text (x + 4, 6, tag, ink::muted);
            if (id == noParam) continue;
            const auto& p = paramTable[paramIndex (id)];
            r.text (x + 22, 6, p.name, ink::paper);
            if (p.kind == Kind::button)
            {
                r.text (x + 4, 21, "Push", ink::soft);
                continue;
            }
            char s[32];
            formatted (id, s);
            r.text (x + 4, 20, s, ink::hot);
            const int start = x + 5, w = 92, rail = 37;
            const float unit = (val (id) - p.min) / (p.max - p.min);
            const int at = start + (int) std::lround (unit * (float) (w - 1));
            r.box (start, rail, w, 2, ink::rule);
            const int origin = p.min < 0 ? start + (w - 1) / 2 : start;
            r.box (std::min (at, origin), rail, std::abs (at - origin) + 1, 2, track);
            r.box (at, rail - 2, 2, 6, track);
            if (h.pot[k] >= 0 && ! h.engaged[k])
            {
                const int pos = start + std::clamp ((int) h.pot[k], 0, 127) * (w - 1) / 127;
                r.box (pos - 2, rail - 3, 5, 7, ink::card);
                r.frame (pos - 2, rail - 3, 5, 7, ink::paper);
            }
        }
    }

    void legend (const Raster& r) const
    {
        using namespace hdui;
        // Each card: a control, what it does, and (under) what it does with Shift.
        const char* top[3] = { "WHEEL", "PUSH", "MINUS / PLUS" };
        const char* plain[3] = { "Move", "Edit", "Page" };
        const char* shifted[3] = { "", "", "" };
        if (page == play) plain[0] = "Pick trio", plain[1] = "Seq on/off", shifted[1] = "+SH Regenerate";
        else if (page == seq) plain[0] = "Pick step", plain[1] = "Cycle trig", shifted[1] = "+SH Play step";
        else if (editing) plain[0] = "Value", shifted[0] = "+SH Fine", plain[1] = "Done", plain[2] = "Coarse value";
        else shifted[1] = "Buttons press";
        for (int k = 0; k < 3; ++k)
        {
            const int x = 4 + 105 * k;
            r.box (x, 3, 102, 42, ink::field);
            r.box (x, 3, 2, 42, ink::amber);
            r.text (x + 6, 6, top[k], ink::muted);
            r.text (x + 6, 19, plain[k], ink::paper);
            r.text (x + 6, 31, shifted[k], ink::soft);
        }
    }

    const std::atomic<float>* values;
    const Telemetry& tel;
    hdui::Picture picture;
    Host lastHost {};
    float seen[ParamId::count] {};
    int page = play, trio = 0, cursor = 0, rows[pageCount] {};
    int shownPage = -1, shownPlace = -2;
    bool shownRunning = false, editing = false, first = true, touched = false, animated = false, struck = false, strikeAccent = false;
    bool scopeLive = false;
    uint32_t seenStrikes = 0, shownGeneration = 0;
    uint64_t strikeAt = 0, frameAt = 0;
    float lightShown = 0.0f, gainShown = 0.05f;
    int8_t trail[2][150][2] {};
    mutable Circle circle;
};
} // namespace mystik
