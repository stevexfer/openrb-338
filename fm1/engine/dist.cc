// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/dist.rs: the distortion insert effect (`CDistInsert`,
// 0x10010A50).
#include "dist.h"

#include "seq.h"

namespace orb {

// flt_10151AB8 (tables::data::DIST_GRID): the output gain grid GRID[shape][amount].
static const float GRID[5][5] = {
    {1.2f, 0.4f, 0.28f, 0.2f, 0.15f},
    {0.35f, 0.2f, 0.15f, 0.14f, 0.12f},
    {0.2f, 0.15f, 0.13f, 0.12f, 0.12f},
    {0.15f, 0.12f, 0.11f, 0.1f, 0.1f},
    {0.12f, 0.1f, 0.08f, 0.08f, 0.08f},
};

static const real GRID_SCALE = (real)3.999;       // dbl_10151B20, 0x400FFDF3B645A1CB
static const real IN_OFFSET = (real)0.0625;       // dbl_10151B28
static const real IN_SCALE = (real)2048;          // dbl_10151B30
static const real OUT_SCALE = (real)5;            // dbl_10151B38
static const real OUT_SCALE2 = (real)0.00390625;  // dbl_10151B40
static const float METER_DECAY = 0.92f;           // flt_10151B4C, 0x3F6B851F
static const float C127 = 0.007874015748031496f;  // flt_10151B50, 0x3C010204
static const float METER_SCALE = 127.0f;          // flt_10151B54
static const real ONE = (real)1;                  // dbl_10151B58
static const real KEEP = (real)0.9;               // dbl_10151B60, 0x3FECCCCCCCCCCCCD
static const real TAKE = (real)0.1;               // dbl_10151B68, 0x3FB999999999999A
static const real CLIP_SCALE = (real)16;          // dbl_10151B70
static const real CLIP_OFFSET = (real)1;          // dbl_10151B78
static const real NEG = (real)-1;                 // dbl_10151B80
static const uint8_t CLIP_KNEE = 90;
static const float SHAPER_LO = 0.0f;              // flt_10151B8C
static const float SHAPER_MID = 128.0f;           // flt_10151B90
static const float SHAPER_HI = 254.0f;            // flt_10151B94

const uint8_t Distortion::DEFAULTS[Distortion::PARAMS] = {1, 70, 0, 0};

static inline float abs_bits(float x) { return bits(x) > 0x80000000u ? -x : x; }

static inline uint8_t clamp_param(uint8_t x) { return x < 127 ? x : 127; }

static inline real coord(uint8_t x) { return ((real)x * (real)C127) * GRID_SCALE; }

// dist::grid: the bilinear interpolation of the output gain at (shape, amount).
static real grid(uint8_t shape, uint8_t amount)
{
    real s = coord(shape);
    real a = coord(amount);
    uint32_t i = (uint16_t)as_i64(s);
    uint32_t j = (uint16_t)as_i64(a);
    real fs = s - (real)i;
    real fa = a - (real)j;
    real top = ((real)GRID[i + 1][j + 1] - (real)GRID[i + 1][j]) * fa + (real)GRID[i + 1][j];
    real bot = ((real)GRID[i][j + 1] - (real)GRID[i][j]) * fa + (real)GRID[i][j];
    return fs * (top - bot) + bot;
}

// dist::shape: the waveshaper lookup on v = gIn*x + 128. In the interpolating branch v is in
// [0, 254] or a NaN, where as_i32 is the Rust `as i64`.
static inline real shape_lookup(const float* w, real v)
{
    if (v < (real)SHAPER_LO) return (real)w[0];
    if (v > (real)SHAPER_HI) return (real)w[254];
    int32_t i = as_i32(v);
    return (v - (real)i) * ((real)w[i + 1] - (real)w[i]) + (real)w[i];
}

static inline real clip_to(real y, real th, real nth)
{
    if (y > th) return th;
    if (y < nth) return nth;
    return y;
}

static inline float smooth(float gain, float target)
{
    return st((real)gain * KEEP + (real)target * TAKE);
}

// The update 0x10010894.
static void param_update(Distortion& d, const Tables& t)
{
    if ((int32_t)bits(d.peak) > (int32_t)0x3F800000)
        d.params[Distortion::METER] = 0x7F;
    else
        d.params[Distortion::METER] = (uint8_t)as_i64((real)d.peak * (real)METER_SCALE);
    d.peak = st((real)d.peak * (real)METER_DECAY);
    uint8_t shape = d.params[Distortion::SHAPE];
    uint8_t amount = d.params[Distortion::AMOUNT];
    d.clip = shape == 0;
    d.target_in = st(((real)t.level[amount] + IN_OFFSET) * IN_SCALE);
    d.row = (uint16_t)(shape >> 2);
    d.target_out = st(grid(shape, amount) * OUT_SCALE * OUT_SCALE2);
}

Distortion::Distortion()
    : counter(0), last_counter(0), clip(false), gain_in(0.0f), gain_out(0.0f), target_in(0.0f),
      target_out(0.0f), peak(0.0f), row(0)
{
    memcpy(params, DEFAULTS, PARAMS);
    memcpy(pattern, DEFAULTS, PARAMS);
}

const uint8_t* Distortion::live(uint32_t* len) const
{
    *len = PARAMS;
    return params;
}

void Distortion::set_song_mode(bool on)
{
    if (on)
        memcpy(pattern, params, PARAMS);
    else
        memcpy(params, pattern, PARAMS);
}

void Distortion::data(const uint8_t* payload, uint32_t len, bool song_mode)
{
    if (len < PARAMS) return;
    memcpy(pattern, payload, PARAMS);
    pattern[AMOUNT] = clamp_param(pattern[AMOUNT]);
    pattern[SHAPE] = clamp_param(pattern[SHAPE]);
    if (!song_mode) memcpy(params, pattern, PARAMS);
}

void Distortion::set_param(uint8_t ctrl, uint8_t value)
{
    switch (ctrl) {
    case ENABLED: params[ENABLED] = value; break;
    case AMOUNT:
    case SHAPE: params[ctrl] = clamp_param(value); break;
    default: break;
    }
}

void Distortion::init_track(TrackMut& track)
{
    for (int c = 0; c <= SHAPE; c++) track.insert(0, (uint8_t)c, pattern[c]);
}

void Distortion::process(Block& bus, const BlockContext& ctx)
{
    if (params[ENABLED] == 0) return;
    if ((counter & 15) == 0) param_update(*this, *ctx.tables);
    counter = (uint16_t)(counter + 1u);
    float v = abs_bits(bus[ctx.meter_index]);
    if (v > peak) peak = v;
    gain_in = smooth(gain_in, target_in);
    gain_out = smooth(gain_out, target_out);
    if (clip) {
        // 0x10010B01: g = 16*T[a] + 1, th = 1/(16*T[min(a, 90)] + 1); the gains are unused
        const float* t = ctx.tables->level;
        uint8_t a = params[AMOUNT];
        real g = CLIP_SCALE * (real)t[a] + CLIP_OFFSET;
        real h = a < CLIP_KNEE ? g : CLIP_SCALE * (real)t[CLIP_KNEE] + CLIP_OFFSET;
        real th = ONE / h;
        real nth = th * NEG;
        for (int i = 0; i < BLOCK; i++) bus[i] = st(clip_to(g * (real)bus[i], th, nth));
    } else {
        const float* w = ctx.tables->dist_row(row, row_cache);
        real gi = (real)gain_in;
        real go = (real)gain_out;
        for (int i = 0; i < BLOCK; i++) {
            real x = gi * (real)bus[i] + (real)SHAPER_MID;
            bus[i] = st(shape_lookup(w, x) * go);
        }
    }
}

void Distortion::update(const BlockContext& ctx)
{
    (void)ctx;
    if (counter == last_counter) params[METER] = 0;
    last_counter = counter;
}

}  // namespace orb
