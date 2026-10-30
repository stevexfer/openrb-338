// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/delay.rs: the send delay (`CDelayEffect`, 0x1000F940).
#include "delay.h"

#include "seq.h"

namespace orb {

static const real FB_SCALE = (real)0.007874015748031496;  // dbl_101517B8, 1/127
static const float METER_DECAY = 0.92f;                   // flt_101517C8, 0x3F6B851F
static const float METER_SCALE = 127.0f;                  // flt_101517CC
static const real KEEP = (real)0.9;                       // dbl_101517D0, 0x3FECCCCCCCCCCCCD
static const real TAKE = (real)0.1;                       // dbl_101517D8, 0x3FB999999999999A
static const uint32_t STEP_SAMPLES = 26460000u;
static const uint32_t MAX_LENGTH = 0x1FFFFu;

const uint8_t Delay::DEFAULTS[Delay::PARAMS] = {1, 3, 1, 80, 64, 0};

static inline float abs_bits(float x) { return bits(x) > 0x80000000u ? -x : x; }

static inline uint8_t clamp_pan(uint8_t x) { return x < 127 ? x : 127; }

// delay::smooth: kept unrounded for the block's loop.
static inline real smooth(float gain, float target)
{
    return (real)gain * KEEP + (real)target * TAKE;
}

uint32_t Delay::length_of(uint32_t tempo, uint8_t steps, uint8_t mode)
{
    int32_t t = (int32_t)tempo;
    int32_t q = mode == 0 ? (int32_t)((uint32_t)t * 3u) / 100 : (int32_t)((uint32_t)t * 4u) / 100;
    uint32_t n = STEP_SAMPLES * (uint32_t)steps;
    uint32_t r = (uint32_t)q == 0 ? MAX_LENGTH : n / (uint32_t)q;
    return r < MAX_LENGTH ? r : MAX_LENGTH;
}

// The update 0x1000F754, every 16 enabled blocks.
static void param_update(Delay& d, const BlockContext& ctx)
{
    uint8_t p[Delay::PARAMS];
    memcpy(p, d.params, sizeof p);
    d.length = Delay::length_of(ctx.tempo, p[Delay::STEPS], p[Delay::MODE]);
    float pg[2];
    ctx.tables->pan(127, p[Delay::PAN], pg);
    d.target[0] = st((real)guard() + (real)pg[0]);
    d.target[1] = st((real)guard() + (real)pg[1]);
    d.feedback = st((real)p[Delay::FEEDBACK] * FB_SCALE);
    if ((int32_t)bits(d.peak) > (int32_t)0x3F800000)
        d.params[Delay::METER] = 0x7F;
    else
        d.params[Delay::METER] = (uint8_t)as_i64((real)d.peak * (real)METER_SCALE);
    d.peak = st((real)d.peak * (real)METER_DECAY);
}

Delay::Delay(ProcessState* process)
    : process_state(process), counter(0), feedback(0.0f), length(0), write(0), peak(0.0f)
{
    memcpy(params, DEFAULTS, PARAMS);
    memcpy(pattern, DEFAULTS, PARAMS);
    target[0] = target[1] = 0.0f;
    gain[0] = gain[1] = 0.0f;
}

const uint8_t* Delay::live(uint32_t* len) const
{
    *len = PARAMS;
    return params;
}

void Delay::reset() { silence(); }

void Delay::silence() { process_state->delay.refill(); }

void Delay::set_song_mode(bool on)
{
    if (on)
        memcpy(pattern, params, PARAMS);
    else
        memcpy(params, pattern, PARAMS);
}

void Delay::data(const uint8_t* payload, uint32_t len, bool song_mode)
{
    if (len < PARAMS) return;
    memcpy(pattern, payload, PARAMS);
    pattern[PAN] = clamp_pan(pattern[PAN]);
    if (!song_mode) memcpy(params, pattern, PARAMS);
}

void Delay::set_param(uint8_t ctrl, uint8_t value)
{
    if (ctrl == PAN)
        params[PAN] = clamp_pan(value);
    else if (ctrl <= FEEDBACK)
        params[ctrl] = value;
}

void Delay::init_track(TrackMut& track)
{
    for (int c = 0; c <= PAN; c++) track.insert(0, (uint8_t)c, pattern[c]);
}

OptI32 Delay::stack_word(OptI32 out_end, OptI32 prev) const
{
    return params[ENABLED] != 0 ? out_end : prev;
}

void Delay::process(Block& send, Block& l, Block& r, const BlockContext& ctx)
{
    DelayLine& buf = process_state->delay;
    if (params[ENABLED] == 0) {
        // 0x1000FAD1: the line refilled once (while the gate is clear), the send bus refilled,
        // the meter byte and the peak reset; nothing reaches the master
        if (!buf.gate) buf.refill();
        fill(send, guard());
        params[METER] = 0;
        peak = guard();
        return;
    }
    buf.gate = false;
    if ((counter & 15) == 0) param_update(*this, ctx);
    counter = (uint16_t)(counter + 1u);
    float v = abs_bits(send[ctx.meter_index]);
    if (v > peak) peak = v;
    real gl = smooth(gain[0], target[0]);
    real gr = smooth(gain[1], target[1]);
    gain[0] = st(gl);
    gain[1] = st(gr);
    real fb = (real)feedback;
    uint32_t rd = (write - length) & DELAY_MASK;
    uint32_t w = write;
    // 0x1000FA34: per sample, the read before the write (they meet when the length is 0)
    for (int i = 0; i < BLOCK; i++) {
        real x = (real)buf.get(rd);
        l[i] = st(x * gl + (real)l[i]);
        r[i] = st((real)r[i] + x * gr);
        real s = (real)send[i];
        send[i] = guard();
        buf.set(w, st(x * fb + s));
        rd = (rd + 1) & DELAY_MASK;
        w = (w + 1) & DELAY_MASK;
    }
    write = w;
}

}  // namespace orb
