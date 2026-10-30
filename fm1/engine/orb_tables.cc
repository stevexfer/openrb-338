// SPDX-License-Identifier: GPL-3.0-or-later
// The entries of the TB-303 filter, PCF filter and distortion shaper tables, computed when they
// are needed instead of read (the FM-1 has no flash for the 260 KB they take). The formulas are
// those of crates/orb-engine/src/tables/mod.rs in single precision: the row constants once in
// double, every entry in float. Where the original's f64 sum cancels at low cutoffs (the gain
// term `c1 + (1 - c0)`), the same value is taken from a form that does not cancel:
// 1 - 2 e cos(2x) + e^2 = (1 - e)^2 + 4 e sin(x)^2.
#include <math.h>

#include "orb_core.h"

namespace orb {

namespace {

const float PI = 3.14159265358979f;

struct Row {
    float base_aw;  // pow(K, -top) * base * aw: w = base_aw * K^i
    float pm, m94;  // the resonance terms of d
    float c3k;      // 303: t*0.85 + 0.15
};

struct Calc {
    Row tb303[8], pcf[8];
    float log2k_303, log2k_pcf;
    Calc()
    {
        const double q_base = F64(0x3FFC7AE140000000ull);  // f32 1.78 widened
        const double sixth = F64(0x3FC5555555555555ull);
        const double seventh = (double)F32(0x3E124925u);
        double k303 = pow(1602.0, F64(0x3F50040100401004ull));  // pow(1602, 1/1023)
        double kpcf = F64(0x3FF03B93E0000000ull);
        double base303 = 0.4 / pow(k303, 1023.0), basepcf = 0.4 / pow(kpcf, 511.0);
        log2k_303 = (float)log2(k303);
        log2k_pcf = (float)log2(kpcf);
        for (int r = 0; r < 8; r++) {
            double rf = r, t = rf * seventh, aw = t * 0.4 + 0.6;
            double pm = pow(q_base, rf) * 0.52, m94 = rf * sixth + 0.5;
            Row a = {(float)(base303 * aw), (float)pm, (float)m94, (float)(t * 0.85 + 0.15)};
            Row b = {(float)(basepcf * aw), (float)pm, (float)m94, 0.0f};
            tb303[r] = a;
            pcf[r] = b;
        }
    }
};

const Calc& calc()
{
    static Calc c;
    return c;
}

// The terms both filters share: c0, c1, `c1 + (1 - c0)` and d.
struct Pole {
    float c0, c1, gain, d;
};

Pole pole(const Row& r, float w, float q)
{
    if (q > 1.0f) q = 1.0f;
    if (q < 0.0f) q = 0.0f;
    Pole p;
    p.d = q * r.pm + (1.0f - q) * r.m94;
    float a = PI * w / p.d;
    float e = expf(-a);
    float om = -expm1f(-a);
    float s = sinf(PI * w);
    p.c0 = e * (cosf(2.0f * PI * w) * 2.0f);
    p.c1 = expf(-2.0f * a);
    p.gain = om * om + 4.0f * e * (s * s);
    return p;
}

}  // namespace

void tb303_filter_entry(uint32_t row, uint32_t idx, float out[4])
{
    const Calc& c = calc();
    const Row& r = c.tb303[row];
    float fi = (float)idx;
    float w = r.base_aw * exp2f(fi * c.log2k_303);
    Pole p = pole(r, w, fi * fi * (0.000125f / 64.0f) - 0.3f);
    float x3 = (r.c3k * w) * (-2.0f * PI);
    float dq = p.d > 4.0f ? 3.0f : p.d;
    out[0] = p.c0;
    out[1] = p.c1;
    out[2] = (-expm1f(x3) * p.gain) / ((dq + 1.0f) * 0.8f);
    out[3] = expf(x3);
}

void pcf_filter_entry(bool bandpass, uint32_t row, uint32_t idx, float out[3])
{
    const Calc& c = calc();
    const Row& r = c.pcf[row];
    float fi = (float)idx;
    float w = r.base_aw * exp2f(fi * c.log2k_pcf);
    Pole p = pole(r, w, fi * fi * (0.000125f / 16.0f) - 0.3f);
    float num = bandpass ? cosf(PI * w) * sqrtf(p.gain) * 3.0f : p.gain * 2.0f;
    float dq = p.d > 4.0f ? 3.0f : p.d;
    out[0] = p.c0;
    out[1] = p.c1;
    out[2] = num / ((dq + 1.0f) * 0.8f);
}

void dist_shaper_row(uint32_t t, float out[256])
{
    float inc = ((float)(t * t) * F32(0x3A88642Au)) * F32(0x3EC90FD0u) + F32(0x3B490FD0u);
    for (int e = 128; e < 256; e++) {
        float j = (float)(e - 128), y = j * (1.0f / 128.0f);
        float v = ((sinf(j * inc) * 256.0f) * (1.0f - y) + (y * y) * 256.0f) * 0.7f;
        out[e] = v;
        out[256 - e] = -v;
    }
    out[0] = -out[255];
}

}  // namespace orb
