// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/drums.rs.
#include "drums.h"

namespace orb {

// dbl_10153640: 2^-15, the int16 scale folded into the gain (0x1001E2CD).
static const real SCALE = (real)1 / (real)32768;
// The multiplier while held.
static const float HELD = 1.0f;

Voice::Voice() : sample(0), active(false), pos(0), left(0), hold(0), g(0.0f), m(0.0f)
{
    cur.reset(0);
}

void Voice::trigger(const Sample* s, float gain, float mul, uint32_t hold_blocks)
{
    if (s) sample = s;
    if (!sample) return;
    hold = hold_blocks;
    m = mul;
    g = st((real)gain * SCALE);
    pos = 0;
    cur.reset(sample);
    left = (int32_t)sample->frames;
    active = true;
}

void Voice::silence()
{
    active = false;
    g = 0.0f;
}

real Voice::mult()
{
    if (hold != 0) {
        hold -= 1;
        return (real)HELD;
    }
    return (real)m;
}

void Voice::end_block(real gain)
{
    if (gain < (real)DRUM_SILENT) active = false;
    g = st(gain);
    advance();
}

void Voice::advance()
{
    pos += 64;
    left -= 64;
    if (left <= 0) active = false;
}

void Simple::process(Block& out)
{
    if (!v.active) return;
    real mult = v.mult();
    real g = (real)v.g;
    uint32_t p = v.pos;
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t i = 2 * k, j = 2 * k + 1;
        real g2 = g * mult;
        real s1 = (real)sample_at(v.cur, p + j);
        real s0 = (real)sample_at(v.cur, p + i);
        out[j] = st(s1 * g + (real)out[j]);
        out[i] = st((real)out[i] + g * s0);
        g = g2;
    }
    v.end_block(g);
}

void Filter::trigger(const Sample* s, float gain, float m, uint32_t hold, float coef)
{
    v.trigger(s, gain, m, hold);
    y = 0.0f;
    c = coef;
}

void Filter::process(Block& out)
{
    if (!v.active) return;
    real mult = v.mult();
    real g = (real)v.g;
    real yy = (real)y;
    real cc = (real)c;
    uint32_t p = v.pos;
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t i = 2 * k, j = 2 * k + 1;
        real yc = yy * cc;
        real g2 = g * mult;
        real s1g = (real)sample_at(v.cur, p + j) * g;
        real y0 = yc + g * (real)sample_at(v.cur, p + i);
        real cy0 = cc * y0;
        out[i] = st((real)out[i] + y0);
        real y1 = cy0 + s1g;
        out[j] = st((real)out[j] + y1);
        yy = y1;
        g = g2;
    }
    y = st(yy);
    v.end_block(g);
}

Dual::Dual() : b(0), b_pos(0), g2(0.0f)
{
    b_cur.reset(0);
}

void Dual::trigger(const Sample* a, float gain, float m, uint32_t hold, const Sample* bs, float gain2)
{
    v.trigger(a, gain, m, hold);
    if (bs) b = bs;
    if (!b) return;
    g2 = gain2;
    b_pos = 0;
    b_cur.reset(b);
    int32_t la = v.sample ? (int32_t)v.sample->frames : 0;
    int32_t lb = (int32_t)b->frames;
    // 0x1001E883: `cmp lenA, lenB; jl` takes A's length, else B's
    v.left = la < lb ? la : lb;
    v.active = true;
}

void Dual::process(Block& out)
{
    if (!v.active) return;
    real mult = v.mult();
    real g1 = (real)v.g;
    real gb = (real)g2;
    uint32_t pa = v.pos, pb = b_pos;
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t i = 2 * k, j = 2 * k + 1;
        real g2n = gb * mult;
        real x0 = ((real)sample_at(v.cur, pa + i) + (real)sample_at(b_cur, pb + i) * gb) * g1;
        out[i] = st((real)out[i] + x0);
        real x1 = (gb * (real)sample_at(b_cur, pb + j) + (real)sample_at(v.cur, pa + j)) * g1;
        out[j] = st((real)out[j] + x1);
        gb = g2n;
    }
    g2 = st(gb);
    // 0x1001E7E2: the integer compare of g1's bits (stored back unchanged)
    if ((int32_t)bits(v.g) < (int32_t)DRUM_SILENT_BITS) v.active = false;
    b_pos += 64;
    v.advance();
}

void Pitched::trigger(const Sample* s, float gain, float m, uint32_t hold, uint32_t pstep)
{
    v.trigger(s, gain, m, hold);
    step = pstep;
    phase = 0;
    // the frame count of the sample argument (`mov ecx, ebp` at 0x1001EB17)
    uint32_t frames = s ? s->frames : 0;
    end = frames << 8;
}

static inline real pitched_sample_at(SampleCursor& c, uint32_t phase)
{
    int32_t f = (int32_t)(phase & 0xFF);
    uint32_t i = phase >> 8;
    int32_t a = sample_at(c, i);
    int32_t b = sample_at(c, i + 1);
    return (real)(int16_t)(a + ((f * (b - a)) >> 8));
}

void Pitched::process(Block& out)
{
    if (!v.active) return;
    real mult = v.mult();
    real g = (real)v.g;
    uint32_t ph = phase;
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t i = 2 * k, j = 2 * k + 1;
        real s0 = pitched_sample_at(v.cur, ph);
        ph += step;
        out[i] = st((real)out[i] + s0 * g);
        real g2 = g * mult;
        real s1 = pitched_sample_at(v.cur, ph);
        out[j] = st((real)out[j] + g * s1);
        ph += step;
        g = g2;
    }
    v.g = st(g);
    phase = ph;
    if (ph > end) v.active = false;
}

}  // namespace orb
