// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/tr808.rs.
#include "tr808.h"

#include "seq.h"

namespace orb {

// ---- crate::tables::data (TR808_*)

// TR-808 snare: the sample slot of tone sample A, indexed by `(tune*5) >> 7` (0x10034090).
static const int16_t TR808_SD_SAMPLE[5] = {1, 2, 3, 4, 5};
// SEQ808: the fixed order of the snare noise samples, indexed by the global counter
// (0x1003409C).
static const uint16_t TR808_SD_NOISE_SEQ[32] = {
    0, 1, 2, 3, 1, 0, 3, 1, 0, 3, 2, 1, 0, 2, 3, 1, 0, 2, 3, 2, 0, 1, 2, 0, 3, 1, 2, 0, 1, 3, 2, 1,
};
// TR-808 snare: the sample slot of noise sample B, indexed by the SEQ808 value (0x100340DC).
static const uint16_t TR808_SD_NOISE_SAMPLE[4] = {6, 7, 8, 9};
// TR-808 BD and CY low-pass coefficient, indexed by `tone >> 4` (0x100340E4).
static const float TR808_TONE[8] = {0.9939f, 0.9757f, 0.906f, 0.822f, 0.675f, 0.456f, 0.208f, 0.001f};
// TR-808 BD and CY gain multiplier per pair, indexed by `decay >> 3` (0x10034104).
static const float TR808_DECAY[16] = {
    0.999f, 0.9993f, 0.9995f, 0.99955f, 0.9996089f, 0.9996524f, 0.9996958f, 0.9997393f,
    0.9997828f, 0.99982f, 0.99986f, 0.999913f, 0.99993f, 0.999956f, 0.99997f, 1.0f,
};

// ---- the step function and the triggers

static const float ACC_STEP = 0.03125f;             // flt_10153F48: the accent step
static const real ACC_BASE = (real)1;               // dbl_10153F40
static const float K = 0.00787401572f;              // F32(0x3C010204): f32 1/127
static const float LEVEL_SCALE = 0.00499999989f;    // F32(0x3BA3D70A): 0.005
static const float SD_SCALE = 0.0250000004f;        // F32(0x3CCCCCCD): 0.025
static const real ONE = (real)1;                    // dbl_10154218: `1 - c` of the bass drum
static const float BD_FADE = 0.99000001f;           // F32(0x3F7D70A4): 0.99
static const float OH_DECAY_M = 0.998000026f;       // F32(0x3F7F7CEE): 0.998
static const float NO_DECAY = 1.0f;

// An index the Rust would panic past (a parameter byte above 127): clamped to the table.
static inline uint32_t clampi(uint32_t i, uint32_t n) { return i < n ? i : n - 1; }

const uint8_t Tr808::DEFAULT_PARAMS[Tr808::PARAMS] = {
    1, 0, 40, 100, 64, 64, 100, 64, 64, 100, 64, 0, 100, 64, 0, 100, 64, 0, 100, 0, 100, 0, 100,
    100, 0, 100, 100, 100, 100, 0, 0, 0,
};

Tr808::Tr808(const SampleSet* samples_, ProcessState* process, const Tables* tables_)
    : pat_start(0), cur_pattern(0), enabled(1), wrapped(false), last_pos(0), accent(0.0f),
      song_mode(false), samples(samples_), process_state(process), tables(tables_)
{
    memcpy(params, DEFAULT_PARAMS, PARAMS);
    memcpy(live_params, DEFAULT_PARAMS, PARAMS);
    memset(patterns, 0, sizeof patterns);
    for (int p = 0; p < PATTERNS; p++) patterns[p * PATTERN_LEN + 1] = 16;
}

const uint8_t* Tr808::pattern(uint8_t p) const
{
    return &patterns[clampi(p, PATTERNS) * PATTERN_LEN];
}

const Sample* Tr808::sample(uint32_t n) const
{
    return samples->get((uint16_t)(999 + n));
}

static inline int32_t bar_of(int32_t now) { return 768 * (now / 768); }

// ((L*k)*L)*scale*accent as the SIMPLE instruments, BD and CY compute it (unrounded).
real Tr808::level(uint8_t l, float scale) const
{
    real lr = (real)l;
    return ((((real)K * lr) * lr) * (real)scale) * (real)accent;
}

void Tr808::trigger(uint32_t i)
{
    const uint8_t* lv = live_params;
    switch (i) {
    case BD: bass_or_cymbal(true); break;
    case CY: bass_or_cymbal(false); break;
    case SD: snare(); break;
    case LT:
    case MT:
    case HT: tom(i); break;
    case RS:
    case CP:
    case CB: {
        uint32_t voice, smp;
        uint8_t lvl;
        if (i == RS) {
            voice = SIMPLE_RS;
            lvl = lv[RS_LEVEL];
            smp = lv[RS_SELECT] != 0 ? SLOT_RS : SLOT_CL;
        } else if (i == CP) {
            voice = SIMPLE_CP;
            lvl = lv[CP_LEVEL];
            smp = lv[CP_SELECT] != 0 ? SLOT_CP : SLOT_MA;
        } else {
            voice = SIMPLE_CB;
            lvl = lv[CB_LEVEL];
            smp = SLOT_CB;
        }
        float g = st(level(lvl, LEVEL_SCALE));
        simple[voice].trigger(sample(smp), g, NO_DECAY, 0);
        break;
    }
    case OH: {
        // 0x10027DDB: level >> 1, hold 4*decay, m 0.998
        float g = st(level((uint8_t)(lv[OH_LEVEL] >> 1), LEVEL_SCALE));
        simple[SIMPLE_OH].trigger(sample(SLOT_OH), g, OH_DECAY_M, 4u * (uint32_t)lv[OH_DECAY]);
        break;
    }
    case CH: {
        // 0x10027E90: (2*level)/3, then every SIMPLE voice's hold is cleared (the choke)
        float g = st(level((uint8_t)((2 * (int32_t)lv[CH_LEVEL]) / 3), LEVEL_SCALE));
        simple[SIMPLE_CH].trigger(sample(SLOT_CH), g, NO_DECAY, 0);
        for (int v = 0; v < 5; v++) simple[v].set_hold(0);
        break;
    }
    default: break;
    }
}

// sub_1002A0B4: the bass drum (`bd`) or the cymbal on a FILTER voice.
void Tr808::bass_or_cymbal(bool bd)
{
    const uint8_t* lv = live_params;
    uint8_t lvl, decay, tone;
    uint32_t smp;
    if (bd) {
        lvl = lv[BD_LEVEL];
        decay = lv[BD_DECAY];
        tone = lv[BD_TONE];
        smp = SLOT_BD;
    } else {
        lvl = lv[CY_LEVEL];
        decay = lv[CY_DECAY];
        tone = lv[CY_TONE];
        smp = SLOT_CY;
    }
    uint32_t voice;
    if (bd) {
        // 0x1002A0DA: the global toggle picks the voice and fades the other one
        if (process_state->bd_808 != 0) {
            filter[FILTER_BD_A].set_m(BD_FADE);
            process_state->bd_808 = 0;
            voice = FILTER_BD_B;
        } else {
            filter[FILTER_BD_B].set_m(BD_FADE);
            process_state->bd_808 = 1;
            voice = FILTER_BD_A;
        }
    } else {
        voice = FILTER_CY;
    }
    float c = TR808_TONE[clampi(tone >> 4, 8)];
    float m = TR808_DECAY[clampi(decay >> 3, 16)];
    real x = level(lvl, LEVEL_SCALE);
    float g = bd ? st((ONE - (real)c) * x) : st(x);
    filter[voice].trigger(sample(smp), g, m, 0, c);
}

// sub_1002A1E4: the snare on the DUAL voice; the noise sample follows SEQ808 at the global
// counter.
void Tr808::snare()
{
    const uint8_t* lv = live_params;
    real sk = (real)lv[SD_SNAPPY] * (real)K;
    real l = (real)lv[SD_LEVEL];
    float g1 = st((((l * (real)K) * l) * (real)SD_SCALE) * (real)accent);
    float g2 = st(sk * sk);
    uint32_t a = (uint32_t)TR808_SD_SAMPLE[clampi(((uint32_t)lv[SD_TONE] * 5) >> 7, 5)] + 1;
    uint16_t c = process_state->next_snare_808();
    uint32_t b = (uint32_t)TR808_SD_NOISE_SAMPLE[TR808_SD_NOISE_SEQ[c]] + 1;
    const Sample* sa = sample(a);
    const Sample* sb = sample(b);
    dual.trigger(sa, g1, NO_DECAY, 0, sb, g2);
}

// sub_1002A004: a tom (selector set) or conga on its PITCHED voice.
void Tr808::tom(uint32_t i)
{
    uint32_t base = LT_LEVEL + 3 * (i - LT);
    uint8_t lvl = live_params[base], tune = live_params[base + 1], select = live_params[base + 2];
    uint32_t smp;
    if (i == LT) smp = select != 0 ? SLOT_LT : SLOT_LC;
    else if (i == MT) smp = select != 0 ? SLOT_MT : SLOT_MC;
    else smp = select != 0 ? SLOT_HT : SLOT_HC;
    real l = (real)lvl;
    float g = st((((l * (real)K) * l) * (real)K) * (real)accent);
    uint32_t step = (uint32_t)tables->tr808_tom_step[tune & 127];
    pitched[i - LT].trigger(sample(smp), g, NO_DECAY, 0, step);
}

const uint8_t* Tr808::live(uint32_t* len) const
{
    *len = PARAMS;
    return live_params;
}

// Slot 1, 0x10027690.
void Tr808::step(int32_t tick, int32_t shuffled, int32_t tempo)
{
    (void)tempo;
    uint8_t pat = cur_pattern;
    const uint8_t* p = pattern(pat);
    uint8_t shuffle = p[0], len = p[1];
    int32_t t = shuffle != 0 ? shuffled : tick;
    int32_t base = song_mode ? pat_start : 0;
    uint32_t period = 48u * (uint32_t)len;
    if (period == 0) return;  // `div` by zero in the original
    // 0x10027713: unsigned (t - base) % (48*len); the step is the signed quotient by 48
    int32_t v = (int32_t)(((uint32_t)t - (uint32_t)base) % period);
    int32_t stp = (v / 48) & 0xFF;
    if ((int32_t)((uint32_t)last_pos - (uint32_t)v) > 24) wrapped = true;
    last_pos = v;
    if (stp == (int32_t)live_params[LAST_STEP] && !wrapped) return;
    // 0x10027772
    live_params[LAST_STEP] = (uint8_t)stp;
    wrapped = false;
    int32_t q = tick / 48;
    int32_t s16 = q < 0 ? -((-q) & 15) : (q & 15);
    if (s16 == 0) {
        // the bar line: the pending pattern and the enable flag
        uint8_t np = live_params[PATTERN];
        if (np != cur_pattern) {
            cur_pattern = np;
            pat_start = 768 * (tick / 768);
            pat = np;
        }
        enabled = live_params[ENABLED];
    }
    if (enabled == 0) return;
    uint32_t at = 2 + 12 * (uint32_t)stp;
    uint8_t row[12];
    p = pattern(pat);
    for (uint32_t i = 0; i < 12; i++) row[i] = at + i < (uint32_t)PATTERN_LEN ? p[at + i] : 0;
    // 0x10027821: f32(AC * accent level * 0.03125f + 1.0)
    accent = st(((real)row[AC] * (real)live_params[ACCENT]) * (real)ACC_STEP + ACC_BASE);
    for (uint32_t i = 1; i < 12; i++) {
        if (row[i] != 0) trigger(i);
    }
}

// Slot 2, 0x10028030: the argument is not read.
void Tr808::relocate(int32_t tick, int32_t now)
{
    (void)tick;
    pat_start = bar_of(now);
    if (live_params[PATTERN] != cur_pattern) cur_pattern = live_params[PATTERN];
    enabled = live_params[ENABLED];
}

// Slot 3, 0x10028080.
void Tr808::start(int32_t now)
{
    wrapped = true;
    pat_start = bar_of(now);
}

void Tr808::stop() { silence(); }

void Tr808::reset() { silence(); }

// Slot 6, 0x1002A2D0: slot 1 (0x1001EB30) on every voice.
void Tr808::silence()
{
    for (int v = 0; v < 5; v++) simple[v].silence();
    for (int v = 0; v < 3; v++) filter[v].silence();
    for (int v = 0; v < 3; v++) pitched[v].silence();
    dual.silence();
}

// Slot 9, 0x10029E30: entering song mode saves the live block into the pattern-mode copy,
// leaving it restores it (all 32 bytes).
void Tr808::set_song_mode(bool on)
{
    if (on) memcpy(params, live_params, PARAMS);
    else memcpy(live_params, params, PARAMS);
    song_mode = on;
}

// Msg 56: the parameter block into the pattern-mode copy and the patterns; in pattern mode the
// block also goes live (bytes 29..31 included).
void Tr808::data(const uint8_t* payload, uint32_t len, bool song)
{
    if (len < (uint32_t)(PARAMS + PATTERNS * PATTERN_LEN)) return;
    memcpy(params, payload, PARAMS);
    memcpy(patterns, payload + PARAMS, PATTERNS * PATTERN_LEN);
    if (!song) memcpy(live_params, params, PARAMS);
}

// Slot 11, 0x10029050: controllers 0..28 set the live byte.
void Tr808::set_param(uint8_t ctrl, uint8_t value)
{
    if (ctrl <= LAST_CTRL) live_params[ctrl] = value;
}

// Msg 57: controllers 0..28 as slot 11, 29 nothing, 30 the unused live byte 30.
void Tr808::param_message(uint8_t ctrl, uint8_t value)
{
    if (ctrl == 30) live_params[30] = value;
    else set_param(ctrl, value);
}

// Slots 7 and 8 from the song ctor: controllers 0 and 1 quantised to a bar, and a tick-0 event
// of every controller 0..28 with the pattern-mode copy's value.
void Tr808::init_track(TrackMut& track)
{
    track.set_quant((uint8_t)ENABLED, 32);
    track.set_quant((uint8_t)PATTERN, 32);
    for (uint32_t c = 0; c <= LAST_CTRL; c++) track.insert(0, (uint8_t)c, params[c]);
}

OptI32 Tr808::playing_pattern() const { return OptI32::of(cur_pattern); }

uint8_t* Tr808::patterns_mut(uint32_t* len)
{
    *len = PATTERNS * PATTERN_LEN;
    return patterns;
}

// The return address 0x10028278 slot 14's last call leaves at esp0 - 0x1C.
OptI32 Tr808::stack_word() const { return OptI32::of(0x10028278); }

// Slot 14, 0x100280B0: the bus is zeroed, then the voices add in this order: BD A, BD B, SD,
// LT, MT, HT, RS/CL, CP/MA, CB, OH, CH, CY.
void Tr808::process(Block& bus, const BlockContext& ctx)
{
    tables = ctx.tables;
    fill(bus, 0.0f);
    filter[FILTER_BD_A].process(bus);
    filter[FILTER_BD_B].process(bus);
    dual.process(bus);
    for (int v = 0; v < 3; v++) pitched[v].process(bus);
    for (int v = 0; v < 5; v++) simple[v].process(bus);
    filter[FILTER_CY].process(bus);
}

}  // namespace orb
