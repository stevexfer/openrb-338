// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/dist.rs: the distortion insert effect (`CDistInsert`,
// 0x10010A50).
#pragma once

#include "orb_core.h"

namespace orb {

class Distortion : public InsertEffect {
public:
    // The controllers (bytes of `params`): enabled, amount, shape; byte 3 is the meter.
    enum { ENABLED = 0, AMOUNT = 1, SHAPE = 2, METER = 3, PARAMS = 4 };
    static const uint8_t DEFAULTS[PARAMS];

    uint8_t params[PARAMS];   // the live parameters (Rust `live`), which the audio reads
    uint8_t pattern[PARAMS];  // the pattern-mode copy
    uint16_t counter;         // the call counter; the update runs when counter & 15 == 0
    uint16_t last_counter;    // the counter at the last mixer update
    bool clip;                // shape 0, the hard clip
    float gain_in, gain_out;  // the smoothed input and output gains
    float target_in, target_out;
    float peak;               // the peak meter of the input
    uint16_t row;             // the waveshaper row, shape >> 2
    RowCache row_cache;       // the computed row in use (without the table)

    Distortion();

    const uint8_t* live(uint32_t* len) const override;
    void set_song_mode(bool on) override;
    void data(const uint8_t* payload, uint32_t len, bool song_mode) override;
    void set_param(uint8_t ctrl, uint8_t value) override;
    void init_track(TrackMut& track) override;

    void process(Block& bus, const BlockContext& ctx) override;
    void update(const BlockContext& ctx) override;
};

}  // namespace orb
