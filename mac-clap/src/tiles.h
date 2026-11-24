#pragma once

#include "hdslave_tiles.h"
#include "screen.h"

namespace mystik
{

struct TilesAccess
{
    // 8 tiles (4x2 grid): PLAY, SEQ, SKIN, OSC, GATE, FX, MODE (1-10), MODE (11-19)
    static constexpr int kTileCount = 8;
    static constexpr int kTilePage[kTileCount] = { 0, 1, 2, 3, 4, 5, 6, 6 };
    static constexpr int kTileTop[kTileCount]  = { 0, 0, 0, 0, 0, 0, 0, 10 };
    static constexpr int kTileVis[kTileCount]  = { 0, 0, 9, 6, 9, 9, 10, 9 };

    static uint32_t CLAP_ABI count (const clap_plugin_t*, int* out_cols, int* out_rows)
    {
        if (out_cols) *out_cols = 4;
        if (out_rows) *out_rows = 2;
        return kTileCount;
    }

    static const char* CLAP_ABI title (const clap_plugin_t*, uint32_t tile)
    {
        static constexpr const char* kTitles[kTileCount] = {
            "01 // PLAY (ORB & SCOPE)",
            "02 // 16-STEP SEQUENCER",
            "03 // MEMBRANE SKIN",
            "04 // WEST-COAST OSC",
            "05 // LOW-PASS GATE",
            "06 // CRUSH / ECHO / SPACE",
            "07 // SEQ MODE (1-10)",
            "08 // SEQ MODE (11-19)"
        };
        return tile < kTileCount ? kTitles[tile] : "";
    }

    static bool CLAP_ABI paint (const clap_plugin_t* plugin, uint32_t tile, const melodee_screen_frame_t* frame, clap_id knobs_out[3])
    {
        if (tile >= kTileCount || !frame || !frame->pixels) return false;
        auto& p = of (plugin);
        const int pg = kTilePage[tile];
        const int savedPage = p.screen.page;
        const int savedRow = p.screen.rows[pg];
        const int savedTop = p.screen.pinnedTop;
        const int savedVis = p.screen.pinnedVis;

        p.screen.page = pg;
        p.screen.pinnedTop = kTileTop[tile];
        p.screen.pinnedVis = kTileVis[tile];
        if (pg >= Screen::skin)
            p.screen.rows[pg] = std::clamp (p.screen.tileRows[tile], kTileTop[tile], p.screen.list().count - 1);
        p.screen.shownPage = -1;

        melodee_screen_frame_t c = *frame;
        c.redraw_all = 1;
        const hdui::Host h = hostOf (&c);
        const bool drew = p.screen.paint (h, c.pixels, (int) c.stride, true);
        if (knobs_out)
            screenControls (plugin, knobs_out);

        p.screen.page = savedPage;
        p.screen.rows[pg] = savedRow;
        p.screen.pinnedTop = savedTop;
        p.screen.pinnedVis = savedVis;
        return drew;
    }

    static bool CLAP_ABI hit_test (const clap_plugin_t* plugin, uint32_t tile, int x, int y, hdslave_tile_hotspot_t* out_spot)
    {
        if (!out_spot || tile >= kTileCount) return false;
        *out_spot = {};
        out_spot->param_id = CLAP_INVALID_ID;
        out_spot->param_id_y = CLAP_INVALID_ID;

        auto& p = of (plugin);
        const int pg = kTilePage[tile];

        // Header right status (x: 206..320, y: 0..25): Seq Mode & Rate / Seq On
        if (x >= 206 && x < 320 && y >= 0 && y < 26)
        {
            out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
            out_spot->param_id = y < 13 ? ParamId::seqMode : ParamId::seqOn;
            out_spot->x = 206; out_spot->y = (int16_t) (y < 13 ? 1 : 13);
            out_spot->w = 112; out_spot->h = 12;
            return true;
        }

        const int by = y - 26;
        if (by < 0 || by >= 142) return false;

        if (pg == Screen::play)
        {
            // Left: Ring & Vactrol Orb (x: 0..160, by: 0..141)
            if (x < 160)
            {
                if (by >= 125)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::length;
                    out_spot->x = 2; out_spot->y = 151; out_spot->w = 156; out_spot->h = 16;
                    return true;
                }
                // Central orb click toggles Seq On (or Shift+click Regenerates)
                out_spot->kind = HDSLAVE_HOTSPOT_ACTION;
                out_spot->custom_id = 1; // Orb toggle / regen
                out_spot->x = 12; out_spot->y = 28; out_spot->w = 136; out_spot->h = 120;
                return true;
            }
            // Right top: Scope (x: 162..320, by: 0..66) -> Note pad audition
            if (by < 67)
            {
                out_spot->kind = HDSLAVE_HOTSPOT_NOTE_PAD;
                out_spot->custom_id = 60;
                out_spot->x = 162; out_spot->y = 28; out_spot->w = 156; out_spot->h = 64;
                return true;
            }
            // Right bottom: Info & Trio selector (x: 162..320, by: 68..141)
            const int iy = by - 68;
            if (iy < 18)
            {
                out_spot->kind = HDSLAVE_HOTSPOT_ACTION;
                out_spot->custom_id = 2; // Cycle trio
                out_spot->x = 162; out_spot->y = 94; out_spot->w = 156; out_spot->h = 16;
                return true;
            }
            if (iy < 40)
            {
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = ParamId::open;
                out_spot->x = 162; out_spot->y = 112; out_spot->w = 156; out_spot->h = 22;
                return true;
            }
            if (iy < 55)
            {
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = x < 240 ? ParamId::scale : ParamId::tune;
                out_spot->x = 162; out_spot->y = 134; out_spot->w = 156; out_spot->h = 15;
                return true;
            }
            out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
            out_spot->param_id = x < 240 ? ParamId::seed : ParamId::body;
            out_spot->x = 162; out_spot->y = 149; out_spot->w = 156; out_spot->h = 16;
            return true;
        }

        if (pg == Screen::seq)
        {
            // 16 step columns (left = 8, wide = 19)
            if (x >= 8 && x < 8 + 16 * 19 && by < 125)
            {
                const int step = std::clamp ((x - 8) / 19, 0, 15);
                const int sx = 8 + step * 19;
                if (by < 13)
                {
                    // Step number badge: select & audition step
                    out_spot->kind = HDSLAVE_HOTSPOT_ACTION;
                    out_spot->custom_id = 10 + step;
                    out_spot->x = (int16_t) sx; out_spot->y = 26; out_spot->w = 19; out_spot->h = 13;
                    return true;
                }
                if (by < 80)
                {
                    // Pitch bar
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::pitch1 + (uint32_t) step;
                    out_spot->x = (int16_t) sx; out_spot->y = 39; out_spot->w = 19; out_spot->h = 66;
                    return true;
                }
                if (by < 93)
                {
                    // Trig dot/diamond/slide
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::trig1 + (uint32_t) step;
                    out_spot->x = (int16_t) sx; out_spot->y = 105; out_spot->w = 19; out_spot->h = 13;
                    return true;
                }
                if (by < 107)
                {
                    // Stroke letter
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::stroke1 + (uint32_t) step;
                    out_spot->x = (int16_t) sx; out_spot->y = 118; out_spot->w = 19; out_spot->h = 14;
                    return true;
                }
                // Chance bar
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = ParamId::chance1 + (uint32_t) step;
                out_spot->x = (int16_t) sx; out_spot->y = 132; out_spot->w = 19; out_spot->h = 18;
                out_spot->is_absolute = true;
                out_spot->invert_y = true;
                return true;
            }
            return false;
        }

        // List pages (SKIN, OSC, GATE, FX, MODE 1/2, MODE 2/2)
        const int vis = kTileVis[tile];
        const int rh = vis >= 10 ? 14 : (vis == 9 ? 15 : Screen::rowH);
        const int y0 = vis >= 10 ? 1 : 3;
        const int top = kTileTop[tile];

        if (x >= 2 && x < 184 && by >= y0 && by < y0 + vis * rh)
        {
            const int visIdx = (by - y0) / rh;
            const int rowIdx = top + visIdx;
            const int savedPage = p.screen.page;
            p.screen.page = pg;
            const Screen::List l = p.screen.list();
            p.screen.page = savedPage;
            if (rowIdx >= 0 && rowIdx < l.count)
            {
                const uint32_t id = l.ids[rowIdx];
                const auto& param = paramTable[paramIndex (id)];
                out_spot->kind = param.kind == Kind::button ? HDSLAVE_HOTSPOT_BUTTON : HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = id;
                out_spot->x = 3;
                out_spot->y = (int16_t) (26 + y0 + visIdx * rh);
                out_spot->w = 177;
                out_spot->h = (int16_t) (rh - 1);
                return true;
            }
        }

        // Right picture panel (x: 188..318, by: 3..139) -> 2D interactive plot!
        if (x >= 188 && x < 318 && by >= 3 && by < 139)
        {
            out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
            out_spot->x = 188; out_spot->y = 29; out_spot->w = 128; out_spot->h = 136;
            out_spot->invert_y = true;
            switch (pg)
            {
                case Screen::skin: out_spot->param_id = ParamId::position; out_spot->param_id_y = ParamId::ring; break;
                case Screen::osc:  out_spot->param_id = ParamId::timbre;   out_spot->param_id_y = ParamId::fmIndex; break;
                case Screen::gate: out_spot->param_id = ParamId::strike;   out_spot->param_id_y = ParamId::decay; break;
                case Screen::fx:   out_spot->param_id = ParamId::echo;     out_spot->param_id_y = ParamId::space; break;
                default:           out_spot->param_id = ParamId::density;  out_spot->param_id_y = ParamId::mutate; break;
            }
            return true;
        }
        return false;
    }

    static void CLAP_ABI focus (const clap_plugin_t* plugin, uint32_t tile, int x, int y, bool)
    {
        if (tile >= kTileCount) return;
        auto& p = of (plugin);
        const int pg = kTilePage[tile];
        p.screen.page = pg;
        const int by = y - 26;
        if (pg == Screen::seq && x >= 8 && x < 8 + 16 * 19 && by >= 0 && by < 125)
        {
            p.screen.cursor = std::clamp ((x - 8) / 19, 0, 15);
            p.screen.touched = true;
        }
        else if (pg >= Screen::skin && x >= 2 && x < 184 && by >= 0 && by < 142)
        {
            const int vis = kTileVis[tile];
            const int rh = vis >= 10 ? 14 : (vis == 9 ? 15 : Screen::rowH);
            const int y0 = vis >= 10 ? 1 : 3;
            const int rowIdx = kTileTop[tile] + std::max (0, (by - y0) / rh);
            const Screen::List l = p.screen.list();
            if (rowIdx >= 0 && rowIdx < l.count)
            {
                p.screen.tileRows[tile] = rowIdx;
                p.screen.rows[pg] = rowIdx;
                p.screen.touched = true;
            }
        }
    }

    static void CLAP_ABI action (const clap_plugin_t* plugin, uint32_t, int32_t custom_id, int32_t wheel_delta, bool shift)
    {
        auto& p = of (plugin);
        if (custom_id == 1)
        {
            // PLAY orb click: toggle Seq On (or Shift: Regenerate)
            melodee_screen_input_t in { MELODEE_SCREEN_PUSH, 1, shift ? 1u : 0u };
            p.screen.page = Screen::play;
            screenInput (plugin, &in);
        }
        else if (custom_id == 2)
        {
            // PLAY trio cycle
            const int dir = wheel_delta != 0 ? (wheel_delta > 0 ? 1 : -1) : (shift ? -1 : 1);
            p.screen.trio = ((p.screen.trio + dir) % Screen::trioCount + Screen::trioCount) % Screen::trioCount;
            p.screen.touched = true;
            requestPaint (p);
        }
        else if (custom_id >= 10 && custom_id < 26)
        {
            // SEQ step audition
            const int step = custom_id - 10;
            p.screen.cursor = step;
            p.pressedAt.store (steadyMs(), std::memory_order_release);
            p.audition.store (step, std::memory_order_release);
            if (p.host && p.host->request_process)
                p.host->request_process (p.host);
            requestPaint (p);
        }
    }
};

const clap_plugin_hdslave_tiles_t tilesExtension {
    TilesAccess::count,
    TilesAccess::title,
    TilesAccess::paint,
    TilesAccess::hit_test,
    TilesAccess::focus,
    TilesAccess::action,
    169,
};

} // namespace mystik
