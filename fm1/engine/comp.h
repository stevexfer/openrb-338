// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/comp.rs: the compressor insert effect (`CCompInsert`,
// mono 0x1000EC70, stereo 0x1000EDE0).
#pragma once

#include "orb_core.h"

namespace orb {

class Compressor : public InsertEffect {
public:
    // The controllers (bytes of `params`): enabled, amount, threshold; bytes 3 and 4 are the
    // gain and the input peak meters.
    enum { ENABLED = 0, AMOUNT = 1, THRESHOLD = 2, GAIN_METER = 3, PEAK_METER = 4, PARAMS = 5 };
    static const uint8_t DEFAULTS[PARAMS];
    static const uint16_t HOLD = 300;  // the hold reload, in groups

    uint8_t params[PARAMS];   // the live parameters (Rust `live`), which the audio reads
    uint8_t pattern[PARAMS];  // the pattern-mode copy
    uint16_t counter;         // the call counter; the update runs when counter & 15 == 0
    uint16_t last_counter;    // the counter at the last mixer update
    float x;                  // the gain factor X
    float off;                // the gain offset
    float env;                // the envelope
    float gain;               // the gain G; 0 at the ctor, so the compressor fades in
    float floor;              // the envelope's floor
    uint16_t hold;            // the hold counter
    float peak;               // the peak meter of the (left) input

    Compressor();

    const uint8_t* live(uint32_t* len) const override;
    void set_song_mode(bool on) override;
    void data(const uint8_t* payload, uint32_t len, bool song_mode) override;
    void set_param(uint8_t ctrl, uint8_t value) override;
    void init_track(TrackMut& track) override;

    void process(Block& bus, const BlockContext& ctx) override;
    void process_stereo(Block& l, Block& r, const BlockContext& ctx) override;
    void update(const BlockContext& ctx) override;
    OptI32 stack_word(OptI32 prev) const override;

    // The head of both blocks; false when disabled.
    bool begin(const Block& bus, const BlockContext& ctx);
    // The 16 groups of a block over `n` buses (one, or L and R linked).
    void run(Block* const* buses, int n);
};

}  // namespace orb
