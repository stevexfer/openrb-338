// SPDX-License-Identifier: GPL-3.0-only
// The engine behind a C interface (rb338_engine.h): int and pointer arguments only, so the
// generic C side and this FPU-built unit agree on the calling convention.
#include "rb338_engine.h"
#include "frame.h"

inline void* operator new(size_t, void* p) noexcept { return p; }

using namespace orb;

extern "C" const uint8_t orb_tables[], orb_tables_end[];

namespace {

Tables tables;
SampleSet samples;
alignas(8) unsigned char engine_mem[sizeof(Engine)];
Engine* engine;
int16_t pcm[2 * RB338_RENDER_MAX];

// The UI's edits, from the main loop (the only writer of `head`) to the audio ISR (of `tail`).
struct Cmd {
    uint8_t what, dev;
    uint16_t idx;
    int32_t value;
};
const uint32_t QUEUE = 64;
Cmd queue[QUEUE];
volatile uint32_t head, tail;

Tb303& tb303(int dev) { return *static_cast<Tb303*>(engine->frame.devices.channels[dev == DEV_TB303_1 ? 0 : 1]); }
Tr808& tr808() { return *static_cast<Tr808*>(engine->frame.devices.channels[2]); }
Tr909& tr909() { return *static_cast<Tr909*>(engine->frame.devices.channels[3]); }

// The live controller (RB_PARAM), null when there is none.
uint8_t* live_param(int dev, int idx)
{
    Devices& d = engine->frame.devices;
    if (idx < 0) return 0;
    switch (dev) {
    case DEV_MIXER: {
        MixerParams& m = engine->frame.mixer().live_params;
        if (idx == 0) return &m.master;
        if (idx == 1) return &m.comp_id;
        if (idx == 2) return &m.pcf_id;
        if (idx < 5 || idx >= 29 || (idx - 5) % 6 == 5) return 0;
        ChannelParams& ch = m.channels[(idx - 5) / 6];
        uint8_t* f[5] = {&ch.enabled, &ch.level, &ch.pan, &ch.send, &ch.dist};
        return f[(idx - 5) % 6];
    }
    case DEV_TB303_1:
    case DEV_TB303_2: return idx <= Tb303::WAVEFORM ? &tb303(dev).live_params[idx] : 0;
    case DEV_TR808: return idx <= Tr808::LAST_CTRL ? &tr808().live_params[idx] : 0;
    case DEV_TR909: return idx <= Tr909::LAST_CTRL ? &tr909().live_params[Tr909::internal_index((uint32_t)idx)] : 0;
    case DEV_DELAY: return idx < Delay::METER ? &static_cast<Delay*>(d.delay)->params[idx] : 0;
    case DEV_DIST: return idx < Distortion::METER ? &static_cast<Distortion*>(d.dist)->params[idx] : 0;
    case DEV_PCF: return idx < Pcf::METER ? &static_cast<Pcf*>(d.pcf)->params[idx] : 0;
    case DEV_COMP: return idx < Compressor::GAIN_METER ? &static_cast<Compressor*>(d.comp)->params[idx] : 0;
    default: return 0;
    }
}

uint8_t* pattern_byte(int dev, int idx)
{
    if (idx < 0) return 0;
    switch (dev) {
    case DEV_TB303_1:
    case DEV_TB303_2: return idx < (int)sizeof tb303(dev).patterns ? &tb303(dev).patterns[idx] : 0;
    case DEV_TR808: return idx < (int)sizeof tr808().patterns ? &tr808().patterns[idx] : 0;
    case DEV_TR909: return idx < (int)sizeof tr909().patterns ? &tr909().patterns[idx] : 0;
    default: return 0;
    }
}

void apply(const Cmd& c)
{
    Sequencer& seq = engine->frame.seq;
    uint8_t v = (uint8_t)c.value;
    switch (c.what) {
    case RB_TEMPO: seq.set_tempo((uint32_t)c.value); break;
    case RB_SHUFFLE: seq.set_shuffle(v); break;
    case RB_SONG_MODE: engine->set_song_mode(v); break;
    case RB_PARAM:
        if (c.dev == DEV_MIXER && c.idx == 0) engine->frame.mixer().set_master(v);
        else if (c.dev == DEV_MIXER && c.idx >= 5 && (c.idx - 5) % 6 == 0) engine->frame.mixer().set_enabled((c.idx - 5) / 6, v);
        else if (live_param(c.dev, c.idx)) engine->set_param((Device)c.dev, (uint8_t)c.idx, v);
        break;
    case RB_PATTERN_BYTE:
        if (uint8_t* p = pattern_byte(c.dev, c.idx)) *p = v;
        break;
    default: break;
    }
}

// A new song (no song, or one that did not load): every pattern 16 steps long, the 303's rests.
void blank()
{
    for (int dev = DEV_TB303_1; dev <= DEV_TB303_2; dev++)
        for (int p = 0; p < Tb303::PATTERNS; p++) {
            uint8_t* b = &tb303(dev).patterns[p * Tb303::PATTERN_LEN];
            b[1] = 16;
            for (int s = 0; s < 16; s++) b[2 + 6 * s + 5] = 1;
        }
    for (int p = 0; p < Tr808::PATTERNS; p++) tr808().patterns[p * Tr808::PATTERN_LEN + 1] = 16;
    for (int p = 0; p < Tr909::PATTERNS; p++) tr909().patterns[p * Tr909::PATTERN_LEN + 1] = 16;
}

}  // namespace

extern "C" int rb338_engine_init(const uint8_t* const* kit, const uint32_t* kit_len, int regions, const uint8_t* song,
                                 uint32_t song_len)
{
    int r;
    engine = 0;
    head = tail = 0;
    if (!tables_from_blob(tables, orb_tables, (uint32_t)(orb_tables_end - orb_tables))) return RB338_BAD_TABLES;
    r = kit_from_regions(samples, kit, kit_len, regions);
    if (r < 0) return RB338_BAD_KIT;
    Engine* e = new (engine_mem) Engine(&tables, &samples, &process_globals());
    if (song && !e->load_blob(song, song_len)) {
        e = new (engine_mem) Engine(&tables, &samples, &process_globals());
        r |= RB338_BAD_SONG;
    }
    e->activate(ACTIVATE_REPLACES_STOPPED);
    engine = e;
    if (!song || (r & RB338_BAD_SONG)) blank();
    return r;
}

extern "C" void rb338_engine_render(int32_t* out, uint32_t frames)
{
    uint32_t i;
    if (!engine || frames > RB338_RENDER_MAX) {
        for (i = 0; i < 2 * frames; i++) out[i] = 0;
        return;
    }
    for (uint32_t t = tail; t != head; t = (t + 1) % QUEUE) {
        apply(queue[t]);
        tail = (t + 1) % QUEUE;
    }
    engine->render(pcm, frames);
    for (i = 0; i < 2 * frames; i++) out[i] = (int32_t)pcm[i] * (1 << 7);
}

extern "C" void rb338_engine_play(int on)
{
    if (!engine) return;
    if (on) engine->play();
    else engine->stop();
}

extern "C" int rb338_engine_playing(void) { return engine && engine->playing(); }

extern "C" int rb338_get(int what, int dev, int idx)
{
    if (!engine) return 0;
    Frame& f = engine->frame;
    switch (what) {
    case RB_TEMPO: return (int)f.seq.globals.tempo;
    case RB_SHUFFLE: return f.seq.globals.shuffle;
    case RB_SONG_MODE: return f.seq.globals.song_mode;
    case RB_TICK: return f.seq.tick_pos();
    case RB_PARAM: {
        const uint8_t* p = live_param(dev, idx);
        return p ? *p : 0;
    }
    case RB_PATTERN_BYTE: {
        const uint8_t* p = pattern_byte(dev, idx);
        return p ? *p : 0;
    }
    case RB_CUR_PATTERN:
        if (dev == DEV_TB303_1 || dev == DEV_TB303_2) return tb303(dev).cur_pattern;
        return dev == DEV_TR808 ? tr808().cur_pattern : dev == DEV_TR909 ? tr909().cur_pattern : 0;
    case RB_STEP:
        if (dev == DEV_TB303_1 || dev == DEV_TB303_2) return tb303(dev).live_params[Tb303::LAST_STEP];
        return dev == DEV_TR808 ? tr808().live_params[Tr808::LAST_STEP]
                                : dev == DEV_TR909 ? tr909().live_params[Tr909::LAST_STEP] : 0;
    case RB_METER:
        if (idx >= 0 && idx < 4) return f.state.channels[idx].meter;
        return idx == 4 || idx == 5 ? f.state.master_meter[idx - 4] : 0;
    default: return 0;
    }
}

extern "C" void rb338_set(int what, int dev, int idx, int value)
{
    uint32_t h = head, n = (h + 1) % QUEUE;
    if (n == tail) return;  // full: 64 edits within one render cannot come from a person
    queue[h].what = (uint8_t)what;
    queue[h].dev = (uint8_t)dev;
    queue[h].idx = (uint16_t)idx;
    queue[h].value = value;
    __asm__ volatile("" ::: "memory");
    head = n;
}

extern "C" uint32_t rb338_song_blob(const uint8_t* old, uint32_t old_len, uint32_t off, uint8_t* out, uint32_t n)
{
    return engine ? engine->save_blob(old, old_len, off, out, n) : 0;
}
