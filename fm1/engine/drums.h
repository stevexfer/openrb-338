// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/drums.rs: the drum voices shared by the TR-808 and the
// TR-909, the classes SIMPLE_VOICE, FILTER_VOICE, DUAL_VOICE and PITCHED_VOICE.
//
// A voice plays one sample (two for Dual) of raw i16 values through a SampleCursor; a voice
// without a sample (nullptr) reads 0. Every restart at frame 0 resets the cursor.
#pragma once

#include "orb_core.h"

namespace orb {

// flt_1015364C: a gain below it ends a SIMPLE, FILTER or DUAL voice.
static const uint32_t DRUM_SILENT_BITS = 0x3300D959u;  // 3e-8
static const float DRUM_SILENT = 2.99999989e-08f;      // F32(0x3300D959)

// The part every class shares (the SIMPLE_VOICE fields).
struct Voice {
    const Sample* sample;  // +4: a trigger without one keeps the last
    bool active;           // +8
    uint32_t pos;          // +0xC: the read position of the next block
    int32_t left;          // +0x10: the frames left
    uint32_t hold;         // +0x14: the blocks the gain is held (multiplier 1.0)
    float g;               // +0x18: the gain (the DUAL voice's g1)
    float m;               // +0x1C: the gain multiplier per pair (the DUAL voice's applies to g2)
    SampleCursor cur;      // the read head on `sample`

    Voice();
    // sub_1001E2B4(sample, gain, m, hold).
    void trigger(const Sample* s, float gain, float mul, uint32_t hold_blocks);
    void silence();
    real mult();
    void end_block(real gain);
    void advance();
};

// SIMPLE_VOICE: out += g*s.
struct Simple {
    Voice v;
    void trigger(const Sample* s, float gain, float m, uint32_t hold) { v.trigger(s, gain, m, hold); }
    void set_hold(uint32_t hold) { v.hold = hold; }
    void set_m(float m) { v.m = m; }
    void silence() { v.silence(); }
    void process(Block& out);
};

// FILTER_VOICE: a one-pole low-pass, y = y*c + g*s, out += y.
struct Filter {
    Voice v;
    float c;  // +0x20
    float y;  // +0x24
    Filter() : c(0.0f), y(0.0f) {}
    void trigger(const Sample* s, float gain, float m, uint32_t hold, float coef);
    void set_m(float m) { v.m = m; }
    void silence() { v.silence(); }
    void process(Block& out);
};

// DUAL_VOICE: two samples, out += g1*(A + g2*B). The multiplier and the hold apply to g2.
struct Dual {
    Voice v;
    const Sample* b;  // +0x28
    uint32_t b_pos;   // +0x20
    float g2;         // +0x24
    SampleCursor b_cur;
    Dual();
    void trigger(const Sample* a, float gain, float m, uint32_t hold, const Sample* bs, float gain2);
    void silence() { v.silence(); }
    void process(Block& out);
};

// PITCHED_VOICE: the sample resampled with a 24.8 fixed-point phase and integer linear
// interpolation, s = i16(a + ((f*(b - a)) >> 8)).
struct Pitched {
    Voice v;
    uint32_t step;   // +0x24
    uint32_t phase;  // +0x28
    uint32_t end;    // +0x2C: frames << 8
    Pitched() : step(0), phase(0), end(0) {}
    void trigger(const Sample* s, float gain, float m, uint32_t hold, uint32_t pstep);
    void silence() { v.silence(); }
    void process(Block& out);
};

}  // namespace orb
