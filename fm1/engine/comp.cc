// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/comp.rs: the compressor insert effect (`CCompInsert`,
// mono 0x1000EC70, stereo 0x1000EDE0).
#include "comp.h"

#include "seq.h"

namespace orb {

static const float ATTACK = 0.01f;                    // +0x54, 0x3C23D70A
static const float RELEASE = 0.9984f;                 // +0x58, 0x3F7F9724
static const uint32_t RELEASE_BITS = 0x3F7F9724u;
static const float FLOOR_INIT = 1000.0f;              // +0x60
static const real GAIN_METER_SCALE = (real)18;        // dbl_10151628
static const float METER_FULL = 127.0f;               // flt_10151660
static const float C127 = 0.007874015748031496f;      // flt_10151664, 0x3C010204
static const real OFF_SCALE = (real)0.99;             // dbl_10151630, 0x3FEFAE147AE147AE
static const real ONE = (real)1;                      // dbl_10151638
static const real TH_SCALE = (real)0.03125;           // dbl_10151640
static const real X_SCALE = (real)0.3662109375;       // dbl_10151648
static const float FLOOR_SCALE = 80000.0f;            // flt_10151668
static const real FLOOR_SCALE2 = (real)0.000030517578125;  // dbl_10151650, 1/32768
static const real FLOOR_OFFSET = (real)0.01220703125;    // dbl_10151658
static const float METER_DECAY = 0.92f;               // flt_10151670, 0x3F6B851F
static const real RAMP = (real)0.25;                  // dbl_10151678, dbl_10151690
static const float ZERO = 0.0f;                       // flt_10151688

const uint8_t Compressor::DEFAULTS[Compressor::PARAMS] = {1, 70, 64, 127, 0};
const uint16_t Compressor::HOLD;

static inline float abs_bits(float x) { return bits(x) > 0x80000000u ? -x : x; }

// comp::params: X, the offset and the floor from the amount and the threshold.
static void comp_params(uint8_t amount, uint8_t threshold, float* x, float* off, float* floor)
{
    real af = (real)amount * (real)C127;
    real th = (real)threshold;
    real o = ONE - af * OFF_SCALE;
    real g = (af * (th * TH_SCALE + ONE)) * X_SCALE;
    real f = ((th * (real)C127) * (real)FLOOR_SCALE) * FLOOR_SCALE2 + FLOOR_OFFSET;
    *x = st(g);
    *off = st(o);
    *floor = st(f);
}

// comp::envelope: the detector and envelope of one group.
static inline real envelope(real env, real s, uint16_t* hold, float floor)
{
    real e;
    if (s > env) {
        *hold = Compressor::HOLD;
        e = env + (s - env) * (real)ATTACK;
    } else if (*hold == 0) {
        e = env * (real)RELEASE + (real)guard();
    } else {
        *hold = (uint16_t)(*hold - 1u);
        e = env;
    }
    if (e < (real)floor || e != e) return (real)floor;
    return e;
}

static inline real detect(real s) { return (s < (real)ZERO || s != s) ? -s : s; }

// The update 0x1000EB44: the gain meter, the parameters, then the peak meter.
static void param_update(Compressor& c)
{
    real g = (real)c.gain * GAIN_METER_SCALE;
    if (g > (real)METER_FULL) g = (real)METER_FULL;
    c.params[Compressor::GAIN_METER] = (uint8_t)as_i64(g);
    comp_params(c.params[Compressor::AMOUNT], c.params[Compressor::THRESHOLD], &c.x, &c.off, &c.floor);
    if ((int32_t)bits(c.peak) > (int32_t)0x3F800000)
        c.params[Compressor::PEAK_METER] = 0x7F;
    else
        c.params[Compressor::PEAK_METER] = (uint8_t)as_i64((real)METER_FULL * (real)c.peak);
    c.peak = st((real)c.peak * (real)METER_DECAY);
}

Compressor::Compressor()
    : counter(0), last_counter(0), x(1.0f), off(1.0f), env(1.0f), gain(0.0f), floor(FLOOR_INIT),
      hold(0), peak(0.0f)
{
    memcpy(params, DEFAULTS, PARAMS);
    memcpy(pattern, DEFAULTS, PARAMS);
}

bool Compressor::begin(const Block& bus, const BlockContext& ctx)
{
    if (params[ENABLED] == 0) return false;
    if ((counter & 15) == 0) param_update(*this);
    counter = (uint16_t)(counter + 1u);
    float v = abs_bits(bus[ctx.meter_index]);
    if (v > peak) peak = v;
    return true;
}

void Compressor::run(Block* const* buses, int n)
{
    real gx = (real)x;
    real go = (real)off;
    real e = (real)env;
    real g = (real)gain;
    for (int i = 0; i < BLOCK; i += 4) {
        real s = (real)(*buses[0])[i + 1] + (real)(*buses[0])[i + 3];
        for (int b = 1; b < n; b++) s = s + (real)(*buses[b])[i + 1] + (real)(*buses[b])[i + 3];
        e = envelope(e, detect(s), &hold, floor);
        real d = ((gx / e + go) - g) * RAMP;
        for (int k = 0; k < 4; k++) {
            g += d;
            for (int b = 0; b < n; b++) (*buses[b])[i + k] = st((real)(*buses[b])[i + k] * g);
        }
    }
    env = st(e);
    gain = st(g);
}

const uint8_t* Compressor::live(uint32_t* len) const
{
    *len = PARAMS;
    return params;
}

void Compressor::set_song_mode(bool on)
{
    if (on)
        memcpy(pattern, params, PARAMS);
    else
        memcpy(params, pattern, PARAMS);
}

void Compressor::data(const uint8_t* payload, uint32_t len, bool song_mode)
{
    if (len < PARAMS) return;
    memcpy(pattern, payload, PARAMS);
    if (!song_mode) memcpy(params, pattern, PARAMS);
}

void Compressor::set_param(uint8_t ctrl, uint8_t value)
{
    if (ctrl <= THRESHOLD) params[ctrl] = value;
}

void Compressor::init_track(TrackMut& track)
{
    for (int c = 0; c <= THRESHOLD; c++) track.insert(0, (uint8_t)c, pattern[c]);
}

OptI32 Compressor::stack_word(OptI32 prev) const
{
    if (params[ENABLED] != 0) return OptI32::of((int32_t)RELEASE_BITS);
    return prev;
}

void Compressor::process(Block& bus, const BlockContext& ctx)
{
    if (begin(bus, ctx)) {
        Block* b[1] = {&bus};
        run(b, 1);
    }
}

void Compressor::process_stereo(Block& l, Block& r, const BlockContext& ctx)
{
    if (begin(l, ctx)) {
        Block* b[2] = {&l, &r};
        run(b, 2);
    }
}

void Compressor::update(const BlockContext& ctx)
{
    (void)ctx;
    if (counter == last_counter) {
        params[GAIN_METER] = 0;
        params[PEAK_METER] = 0;
    }
    last_counter = counter;
}

}  // namespace orb
