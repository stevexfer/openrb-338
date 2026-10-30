// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/pcf.rs: the PCF insert effect (`CPCFInsert`, 0x100166D0).
#pragma once

#include "orb_core.h"

namespace orb {

class Pcf : public InsertEffect {
public:
    // The controllers (bytes of `params`): enabled, frequency, resonance, amount, wave (the
    // pattern), decay, mode (1 low-pass, anything else band-pass); byte 7 is the meter.
    enum {
        ENABLED = 0, FREQUENCY = 1, RESONANCE = 2, AMOUNT = 3, WAVE = 4, DECAY = 5, MODE = 6,
        METER = 7, PARAMS = 8,
    };
    static const uint8_t DEFAULTS[PARAMS];

    uint8_t params[PARAMS];   // the live parameters (Rust `live`), which the audio reads
    uint8_t pattern[PARAMS];  // the pattern-mode copy
    uint16_t counter;         // the call counter; the meter update runs when counter & 15 == 0
    uint16_t last_counter;    // the counter at the last mixer update
    float lp[2];              // the low-pass state (y1, y2); zeroed by every band-pass block
    float bp[2];              // the band-pass state; zeroed by every low-pass block
    float trigger;            // the trigger level E
    float base;               // the cutoff base, the smoother's output
    float env;                // the envelope
    int32_t step;             // the last step, -1 at the ctor
    float attack;             // the attack's keep factor (0 instant, 0.995 slow)
    float decay;              // the decay per group
    float smooth[2];          // the cutoff smoother's last two outputs
    float peak;               // the peak meter of the input
    CoefCache coef_cache;     // the computed filter entry in use (without the table)
    int32_t quotient;         // the stack slot var_1C (see pcf.rs)
    bool has_fault;           // set by a block that divided by zero, cleared by faulted()
    Fault fault;

    Pcf();

    const uint8_t* live(uint32_t* len) const override;
    void set_song_mode(bool on) override;
    void data(const uint8_t* payload, uint32_t len, bool song_mode) override;
    void set_param(uint8_t ctrl, uint8_t value) override;
    void init_track(TrackMut& track) override;

    void process(Block& bus, const BlockContext& ctx) override;
    void update(const BlockContext& ctx) override;
    bool faulted(Fault* f) override;
    OptI32 stack_word(OptI32 prev) const override;
};

}  // namespace orb
