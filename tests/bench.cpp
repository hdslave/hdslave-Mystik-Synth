// Mystik's cost: renders through the CLAP entry for SECONDS at 48 kHz in 128-frame blocks, as Melodee plays it, and
// prints the time a block takes here. tools/bench.sh runs it under callgrind for instruction counts (exact and
// repeatable, unlike timings on a computer that is not the MPC).
//   bench [worst|acid|typical|stopped|paint] [SECONDS]
// worst: everything on at once (both sources, folding, feedback FM, resonance limiting, held gates, 1/32 rolls, every
// effect); acid: the Acid Gate preset; typical: the default groove; stopped: the default, transport stopped; paint:
// the PLAY page painted 25 times a second while the default groove plays (tools/bench.sh counts only screenPaint).
#include "../src/mystik.cpp"

#include <chrono>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "worst";
    const double seconds = argc > 2 ? std::atof (argv[2]) : 10.0;
    constexpr double rate = 48000.0;
    constexpr uint32_t block = 128;

    static const clap_host host { CLAP_VERSION, nullptr, "bench", "", "", "1", [] (const clap_host*, const char*) -> const void* { return nullptr; },
                                  [] (const clap_host*) {}, [] (const clap_host*) {}, [] (const clap_host*) {} };
    clap_entry.init ("");
    const auto* factory = static_cast<const clap_plugin_factory*> (clap_entry.get_factory (CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin* plugin = factory->create_plugin (factory, &host, "com.hdslave.mystik");
    plugin->init (plugin);
    const auto* presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
    if (mode == "acid" || mode == "worst")
        presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "acid-gate");
    plugin->activate (plugin, rate, 1, block);
    plugin->start_processing (plugin);

    std::vector<std::pair<uint32_t, double>> values;
    using namespace mystik::ParamId;
    if (mode == "worst")
    {
        values = { { source, 0.5 }, { fmIndex, 0.8 }, { feedback, 0.6 }, { timbre, 0.8 }, { symmetry, 0.3 }, { bloom, 1.0 }, { drift, 0.6 },
                   { resonance, 1.0 }, { hold, 1.0 }, { gate, 1.0 }, { memory, 1.0 }, { rate, 7 }, { echo, 0.6 }, { repeats, 0.8 },
                   { space, 0.7 }, { spaceDecay, 0.9 }, { drive, 0.6 }, { crush, 0.3 }, { snap, 0.6 }, { ring, 0.9 } };
        for (int s = 0; s < 16; ++s)
            values.push_back ({ stroke1 + (uint32_t) s, s % 4 == 0 ? 6.0 : (s % 4 == 2 ? 5.0 : 0.0) }), values.push_back ({ trig1 + (uint32_t) s, 2.0 });
    }

    struct Events
    {
        std::vector<clap_event_param_value> params;
        std::vector<const clap_event_header*> order;
        clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->order.size(); },
                               [] (const clap_input_events* e, uint32_t i) { return static_cast<const Events*> (e->ctx)->order[i]; } };
    } first, none;
    for (auto& [id, v] : values)
    {
        clap_event_param_value e {};
        e.header = { sizeof (e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = id;
        e.note_id = e.port_index = e.channel = e.key = -1;
        e.value = v;
        first.params.push_back (e);
    }
    for (auto& p : first.params)
        first.order.push_back (&p.header);
    clap_output_events out { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };

    std::vector<float> left (block), right (block);
    float* outs[2] = { left.data(), right.data() };
    clap_audio_buffer buffer {};
    buffer.data32 = outs;
    buffer.channel_count = 2;
    uint64_t b = 0;
    auto process = [&] (bool playing) {
        clap_event_transport t {};
        t.header = { sizeof (t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
        t.tempo = 120.0;
        t.song_pos_beats = (clap_beattime) std::llround ((double) (b * block) * 2.0 / rate * (double) CLAP_BEATTIME_FACTOR);
        clap_process p {};
        p.frames_count = block;
        p.transport = &t;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = b == 0 ? &first.in : &none.in;
        p.out_events = &out;
        plugin->process (plugin, &p);
        ++b;
    };

    // The page: one whole paint first, then a frame every 40 ms (15 blocks).
    const auto* screen = static_cast<const clap_plugin_melodee_screen*> (plugin->get_extension (plugin, CLAP_EXT_MELODEE_SCREEN));
    std::vector<uint32_t> pixels (MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_HEIGHT, 0);
    melodee_screen_canvas_t canvas {};
    canvas.pixels = pixels.data();
    canvas.width = MELODEE_SCREEN_WIDTH, canvas.height = MELODEE_SCREEN_HEIGHT, canvas.stride = MELODEE_SCREEN_WIDTH;
    canvas.playing = true;
    for (auto& k : canvas.knobs)
        k = { -1, true };
    int painted = 0;

    const uint64_t blocks = (uint64_t) (seconds * rate / block);
    double sum = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    if (mode == "paint")
        for (int i = 0; i < 40; ++i) // a lit groove before the frames are counted
            process (true);
    for (uint64_t n = 0; n < blocks; ++n)
    {
        process (mode != "stopped");
        sum += left[0];
        if (mode == "paint" && n % 15 == 0)
        {
            canvas.now_ms = 1000 + n * block * 1000 / (uint64_t) rate;
            canvas.whole = n == 0;
            painted += screen->paint (plugin, &canvas) ? 1 : 0;
        }
    }
    const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("%s: %llu blocks, %.2f us a block here (checksum %.6f)%s\n", mode.c_str(), (unsigned long long) blocks, us / (double) std::max<uint64_t> (1, blocks), sum,
                 mode == "paint" ? (", " + std::to_string (painted) + " frames drew").c_str() : "");
    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    clap_entry.deinit();
    return 0;
}
