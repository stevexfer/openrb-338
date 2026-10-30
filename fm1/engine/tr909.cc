// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/tr909.rs.
#include "tr909.h"

#include "seq.h"

namespace orb {

// ---- crate::tables::data (TR909_*)

// TR-909 snare: the sample slot of tone sample A, indexed by `(tune*5) >> 7` (0x10034210).
static const int16_t TR909_SD_SAMPLE[5] = {4, 5, 6, 7, 8};
// SEQ909: the fixed order of the snare noise samples, indexed by the global counter
// (0x1003421C).
static const uint16_t TR909_SD_NOISE_SEQ[32] = {
    0, 1, 2, 0, 1, 0, 2, 1, 0, 1, 2, 1, 0, 2, 0, 1, 0, 2, 0, 2, 0, 1, 2, 0, 2, 1, 2, 0, 2, 1, 2, 1,
};
// TR-909 snare: the sample slot of noise sample B, indexed by the SEQ909 value (0..2; the fourth
// entry is 0 and unused) (0x1003425C).
static const uint16_t TR909_SD_NOISE_SAMPLE[4] = {9, 10, 11, 0};
// TR-909 BD low-pass coefficient C8, indexed by `attack >> 4` (0x10034264).
static const float TR909_BD_ATTACK[8] = {0.92f, 0.87f, 0.8f, 0.72f, 0.62f, 0.5f, 0.3f, 0.001f};

// ---- the step function and the triggers

static const real ACC_BASE = (real)1;               // dbl_10154348
static const float ACC_STEP = 0.03125f;             // flt_10154368: the accent step
static const real GAIN_ON = (real)0.8;              // dbl_10154358 (0x3FE999999999999A)
static const real GAIN_ACCENT = (real)2;            // dbl_10154360
static const real GAIN_FLAM = (real)1.5;            // dbl_10154350
static const int32_t FLAM_DIV = 700000;             // `mov ebx, 0AAE60h` at 0x1002ABBC
static const float K = 0.00787401572f;              // F32(0x3C010204): f32 1/127
static const float LEVEL_SCALE = 6.20001229e-05f;   // F32(0x38820610): 6.2e-5
static const float C_RS_OH = 1.10000002f;           // F32(0x3F8CCCCD): 1.1
static const float C_CP = 2.29999995f;              // F32(0x40133333): 2.3
static const float C_CH = 0.800000012f;             // F32(0x3F4CCCCD): 0.8
static const float CHOKE = 0.995999992f;            // F32(0x3F7EF9DB): 0.996
static const float KV_LT = 1.20000005f;             // F32(0x3F99999A): 1.2
static const float KV_MT = 1.10000002f;             // F32(0x3F8CCCCD): 1.1
static const float KV_HT = 1.39999998f;             // F32(0x3FB33333): 1.4
static const float KV_CY = 0.600000024f;            // F32(0x3F19999A): 0.6
static const real ONE = (real)1;                    // dbl_10154638: `1 - c` of the bass drum
static const float BD_FADE = 0.99000001f;           // F32(0x3F7D70A4): 0.99
static const float SNAPPY_SCALE = 0.00999999978f;   // F32(0x3C23D70A): 0.01
static const float SD_DECAY_M = 0.998000026f;       // F32(0x3F7F7CEE): 0.998
static const uint32_t SD_HOLD = 30;                 // `add ebx, 1Eh`: the snare's hold is tone + 30
static const uint8_t FIXED_DECAY = 127;             // the decay of RS, CP, crash and ride

// An index the Rust would panic past (a parameter byte above 127): clamped to the table.
static inline uint32_t clampi(uint32_t i, uint32_t n) { return i < n ? i : n - 1; }

const uint8_t Tr909::DEFAULT_PARAMS[Tr909::PARAMS] = {
    1, 0, 32, 100, 64, 64, 127, 100, 64, 64, 64, 100, 64, 127, 100, 64, 127, 100, 64, 127, 100,
    100, 100, 32, 127, 100, 64, 100, 64, 64, 0, 0, 0,
};

Tr909::Tr909(const SampleSet* samples_, ProcessState* process, const Tables* tables_)
    : pat_start(0), cur_pattern(0), enabled(1), wrapped(false), last_pos(0), flam_pending(false),
      flam_tick(0), accent(0.0f), gain(0.0f), song_mode(false), samples(samples_),
      process_state(process), tables(tables_)
{
    memcpy(params, DEFAULT_PARAMS, PARAMS);
    memcpy(live_params, DEFAULT_PARAMS, PARAMS);
    memset(patterns, 0, sizeof patterns);
    for (int p = 0; p < PATTERNS; p++) patterns[p * PATTERN_LEN + 1] = 16;
}

const uint8_t* Tr909::pattern(uint8_t p) const
{
    return &patterns[clampi(p, PATTERNS) * PATTERN_LEN];
}

const Sample* Tr909::sample(uint32_t n) const
{
    return samples->get((uint16_t)(1000 + n));
}

static inline int32_t bar_of(int32_t now) { return 768 * (now / 768); }

// DT[(d << 4) >> 7], the index the triggers compute from a decay byte.
float Tr909::dt(uint8_t d) const
{
    return tables->tr909_decay[clampi(((uint32_t)d << 4) >> 7, 16)];
}

// Instrument i at the gain in `gain`.
void Tr909::trigger(uint32_t i)
{
    const uint8_t* lv = live_params;
    switch (i) {
    case BD: bass(lv[BD_LEVEL], lv[BD_DECAY], lv[BD_TUNE], lv[BD_ATTACK]); break;
    case SD: snare(lv[SD_LEVEL], lv[SD_TONE], lv[SD_TUNE], lv[SD_SNAPPY]); break;
    case LT: trigger_pitched(i, SLOT_LT, lv[LT_LEVEL], lv[LT_LEVEL + 1], lv[LT_LEVEL + 2]); break;
    case MT: trigger_pitched(i, SLOT_MT, lv[MT_LEVEL], lv[MT_LEVEL + 1], lv[MT_LEVEL + 2]); break;
    case HT: trigger_pitched(i, SLOT_HT, lv[HT_LEVEL], lv[HT_LEVEL + 1], lv[HT_LEVEL + 2]); break;
    case CC: trigger_pitched(i, SLOT_CC, lv[CC_LEVEL], lv[CC_TUNE], FIXED_DECAY); break;
    case RC: trigger_pitched(i, SLOT_RC, lv[RC_LEVEL], lv[RC_TUNE], FIXED_DECAY); break;
    case RS: trigger_simple(i, SLOT_RS, lv[RS_LEVEL], FIXED_DECAY); break;
    case CP: trigger_simple(i, SLOT_CP, lv[CP_LEVEL], FIXED_DECAY); break;
    case CH: trigger_simple(i, SLOT_CH, lv[HH_LEVEL], lv[CH_DECAY]); break;
    case OH: trigger_simple(i, SLOT_OH, lv[HH_LEVEL], lv[OH_DECAY]); break;
    default: break;
    }
}

// sub_1002D8B4: RS, CP, CH and OH on their SIMPLE voices, g = f32((((6.2e-5f*L)*L)*c)*gain),
// hold 0. The multiplier is DT[d], or for CH f32(DT[d]^8); CH first chokes the open hi-hat.
void Tr909::trigger_simple(uint32_t i, uint32_t smp, uint8_t level, uint8_t decay)
{
    float dtv = dt(decay);
    uint32_t voice;
    float c, m;
    if (i == RS) {
        voice = SIMPLE_RS;
        c = C_RS_OH;
        m = dtv;
    } else if (i == CP) {
        voice = SIMPLE_CP;
        c = C_CP;
        m = dtv;
    } else if (i == CH) {
        // 0x1002D935: fld f32 DT; fmul st, st; fmul st, st; fmul st, st
        real x = (real)dtv;
        real x2 = x * x;
        real x4 = x2 * x2;
        real x8 = x4 * x4;
        // 0x1002D958: sub_1001E3D0(0.996) on the OH voice
        simple[SIMPLE_OH].set_m(CHOKE);
        voice = SIMPLE_CH;
        c = C_CH;
        m = st(x8);
    } else {
        voice = SIMPLE_OH;
        c = C_RS_OH;
        m = dtv;
    }
    real l = (real)level;
    float g = st(((((real)LEVEL_SCALE * l) * l) * (real)c) * (real)gain);
    simple[voice].trigger(sample(smp), g, m, 0);
}

// sub_1002DA24: a tom or cymbal on its PITCHED voice, g = f32(((L*(L*6.2e-5f))*kv)*gain),
// m = DT[decay], hold 0, step = T909[tune].
void Tr909::trigger_pitched(uint32_t i, uint32_t smp, uint8_t level, uint8_t tune, uint8_t decay)
{
    uint32_t voice;
    float kv;
    switch (i) {
    case LT: voice = 0; kv = KV_LT; break;
    case MT: voice = 1; kv = KV_MT; break;
    case HT: voice = 2; kv = KV_HT; break;
    case CC: voice = 3; kv = KV_CY; break;
    default: voice = 4; kv = KV_CY; break;
    }
    real l = (real)level;
    float g = st(((l * (l * (real)LEVEL_SCALE)) * (real)kv) * (real)gain);
    float m = dt(decay);
    // `mov dx, word_1014E54C[eax*2]`
    uint32_t step = (uint32_t)tables->tr909_tom_step[tune & 127];
    pitched[voice].trigger(sample(smp), g, m, 0, step);
}

// sub_1002DB24: the bass drum on a FILTER voice. The sample is bd1..bd4 by tune >> 5; the
// global toggle picks the voice and fades the other one.
void Tr909::bass(uint8_t level, uint8_t decay, uint8_t tune, uint8_t attack)
{
    // 0x1002DB3E: a signed divide by 32 of a byte
    uint32_t smp = SLOT_BD1 + ((uint32_t)tune >> 5);
    uint32_t voice;
    if (process_state->bd_909 != 0) {
        filter[FILTER_BD_A].set_m(BD_FADE);
        process_state->bd_909 = 0;
        voice = FILTER_BD_B;
    } else {
        filter[FILTER_BD_B].set_m(BD_FADE);
        process_state->bd_909 = 1;
        voice = FILTER_BD_A;
    }
    real l = (real)level;
    real x = (l * (l * (real)K)) * (real)K;
    // 0x1002DBBC: `(attack << 3) >> 7`
    float c = TR909_BD_ATTACK[clampi(((uint32_t)attack << 3) >> 7, 8)];
    float g = st((x * (real)gain) * (ONE - (real)c));
    float m = dt(decay);
    filter[voice].trigger(sample(smp), g, m, 0, c);
}

// sub_1002DC54: the snare on the DUAL voice. A is sd1..sd5 by (tune*5) >> 7, B the noise sample
// of SEQ909 at the global counter.
void Tr909::snare(uint8_t level, uint8_t tone, uint8_t tune, uint8_t snappy)
{
    float g2 = st((real)snappy * (real)SNAPPY_SCALE);
    real l = (real)level;
    float g1 = st(((l * (l * (real)K)) * (real)K) * (real)gain);
    uint32_t a = SLOT_BD1 + (uint32_t)TR909_SD_SAMPLE[clampi(((uint32_t)tune * 5) >> 7, 5)];
    uint16_t c = process_state->next_snare_909();
    uint32_t b = SLOT_BD1 + (uint32_t)TR909_SD_NOISE_SAMPLE[TR909_SD_NOISE_SEQ[c]];
    const Sample* sa = sample(a);
    const Sample* sb = sample(b);
    dual.trigger(sa, g1, SD_DECAY_M, (uint32_t)tone + SD_HOLD, sb, g2);
}

void Tr909::params_rbs_order(uint8_t out[PARAMS]) const
{
    for (uint32_t c = 0; c < (uint32_t)PARAMS; c++) out[c] = params[internal_index(c)];
}

const uint8_t* Tr909::live(uint32_t* len) const
{
    *len = PARAMS;
    return live_params;
}

// Slot 1, 0x1002AAC0.
void Tr909::step(int32_t tick, int32_t shuffled, int32_t tempo)
{
    uint8_t pat = cur_pattern;
    const uint8_t* p = pattern(pat);
    uint8_t shuffle = p[0], len = p[1];
    int32_t t = shuffle != 0 ? shuffled : tick;
    int32_t base = song_mode ? pat_start : 0;
    uint32_t period = 48u * (uint32_t)len;
    if (period == 0) return;  // `div` by zero in the original
    // 0x1002AB49: unsigned (t - base) % (48*len); the step is the signed quotient by 48
    int32_t v = (int32_t)(((uint32_t)t - (uint32_t)base) % period);
    if ((int32_t)((uint32_t)last_pos - (uint32_t)v) > 24) wrapped = true;
    last_pos = v;
    int32_t stp = (v / 48) & 0xFFFF;
    bool flam;
    if (stp == (int32_t)live_params[LAST_STEP] && !wrapped) {
        // 0x1002AB87: the same step: only an armed flam whose tick has passed (the unshuffled
        // tick, signed) goes on
        if (!(flam_pending && tick > flam_tick)) return;
        flam_pending = false;
        flam = true;
    } else {
        // 0x1002ABB0
        live_params[LAST_STEP] = (uint8_t)stp;
        // imul (32 bits), cdq, idiv 700000
        int32_t d = (int32_t)((uint32_t)live_params[FLAM] * (uint32_t)tempo) / FLAM_DIV;
        flam_tick = (int32_t)((uint32_t)tick + (uint32_t)d + 1u);
        wrapped = false;
        flam_pending = true;
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
        flam = false;
    }
    // 0x1002AC74
    if (enabled == 0) return;
    uint32_t at = 2 + 12 * (uint32_t)stp;
    uint8_t row[12];
    p = pattern(pat);
    for (uint32_t i = 0; i < 12; i++) row[i] = at + i < (uint32_t)PATTERN_LEN ? p[at + i] : 0;
    // 0x1002ACAF: f32(AC * accent level * 0.03125f + 1.0)
    accent = st(((real)row[AC] * (real)live_params[ACCENT]) * (real)ACC_STEP + ACC_BASE);
    for (uint32_t i = 11; i >= 1; i--) {
        uint8_t val = row[i];
        real k;
        if (flam) {
            // 0x1002B320: values above 2 retrigger at x1.5
            if (val <= 2) continue;
            k = GAIN_FLAM;
        } else if (val == 1 || val == 3) {
            k = GAIN_ON;
        } else if (val == 2 || val == 4) {
            k = GAIN_ACCENT;
        } else {
            continue;
        }
        gain = st((real)accent * k);
        trigger(i);
    }
}

// Slot 2, 0x1002B670: the argument is not read.
void Tr909::relocate(int32_t tick, int32_t now)
{
    (void)tick;
    pat_start = bar_of(now);
    if (live_params[PATTERN] != cur_pattern) cur_pattern = live_params[PATTERN];
    enabled = live_params[ENABLED];
}

// Slot 3, 0x1002B6C0.
void Tr909::start(int32_t now)
{
    wrapped = true;
    pat_start = bar_of(now);
}

void Tr909::stop() { silence(); }

void Tr909::reset() { silence(); }

// Slot 6, 0x1002DD40: slot 1 (0x1001EB30) on every voice.
void Tr909::silence()
{
    for (int v = 0; v < 4; v++) simple[v].silence();
    for (int v = 0; v < 2; v++) filter[v].silence();
    for (int v = 0; v < 5; v++) pitched[v].silence();
    dual.silence();
}

// Slot 9, 0x1002D510: entering song mode saves the live block into the pattern-mode copy,
// leaving it restores it (all 33 bytes).
void Tr909::set_song_mode(bool on)
{
    if (on) memcpy(params, live_params, PARAMS);
    else memcpy(live_params, params, PARAMS);
    song_mode = on;
}

// Msg 73: the parameter block (RBS order, permuted into the DLL's) into the pattern-mode copy,
// and the patterns; in pattern mode the block also goes live (bytes 30..32 included).
void Tr909::data(const uint8_t* payload, uint32_t len, bool song)
{
    if (len < (uint32_t)(PARAMS + PATTERNS * PATTERN_LEN)) return;
    for (uint32_t c = 0; c < (uint32_t)PARAMS; c++) params[internal_index(c)] = payload[c];
    memcpy(patterns, payload + PARAMS, PATTERNS * PATTERN_LEN);
    if (!song) memcpy(live_params, params, PARAMS);
}

// Slot 11, 0x1002C6D0: controllers 0..29 set the live byte (20..22 permuted).
void Tr909::set_param(uint8_t ctrl, uint8_t value)
{
    if (ctrl <= LAST_CTRL) live_params[internal_index(ctrl)] = value;
}

// Msg 74: controllers 0..29 as slot 11, 30 nothing, 31 the unused live byte 31.
void Tr909::param_message(uint8_t ctrl, uint8_t value)
{
    if (ctrl == 31) live_params[31] = value;
    else set_param(ctrl, value);
}

// Slots 7 and 8 from the song ctor: controllers 0 and 1 quantised to a bar, and a tick-0 event
// of every controller 0..29 with the pattern-mode copy's value in RBS order.
void Tr909::init_track(TrackMut& track)
{
    track.set_quant((uint8_t)ENABLED, 32);
    track.set_quant((uint8_t)PATTERN, 32);
    uint8_t p[PARAMS];
    params_rbs_order(p);
    for (uint32_t c = 0; c < (uint32_t)INIT_CTRLS; c++) track.insert(0, (uint8_t)c, p[c]);
}

OptI32 Tr909::playing_pattern() const { return OptI32::of(cur_pattern); }

uint8_t* Tr909::patterns_mut(uint32_t* len)
{
    *len = PATTERNS * PATTERN_LEN;
    return patterns;
}

// The return address 0x1002B8B4 slot 14's last call leaves at esp0 - 0x1C.
OptI32 Tr909::stack_word() const { return OptI32::of(0x1002B8B4); }

// Slot 14, 0x1002B6F0: the bus is zeroed, then the voices add in this order: BD A, BD B, SD,
// LT, MT, HT, RS, CP, CH, OH, crash, ride.
void Tr909::process(Block& bus, const BlockContext& ctx)
{
    tables = ctx.tables;
    fill(bus, 0.0f);
    for (int v = 0; v < 2; v++) filter[v].process(bus);
    dual.process(bus);
    pitched[0].process(bus);
    pitched[1].process(bus);
    pitched[2].process(bus);
    for (int v = 0; v < 4; v++) simple[v].process(bus);
    pitched[3].process(bus);
    pitched[4].process(bus);
}

}  // namespace orb
