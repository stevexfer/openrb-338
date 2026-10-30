// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/delay.rs: the send delay (`CDelayEffect`, 0x1000F940).
#pragma once

#include "orb_core.h"

namespace orb {

class Delay : public SendEffect {
public:
    // The controllers (bytes of `params`): enabled, steps (1..32), mode (0 triplets, else
    // sixteenths), feedback, pan; byte 5 is the meter.
    enum { ENABLED = 0, STEPS = 1, MODE = 2, FEEDBACK = 3, PAN = 4, METER = 5, PARAMS = 6 };
    static const uint8_t DEFAULTS[PARAMS];

    ProcessState* process_state;  // the process-global delay line
    uint8_t params[PARAMS];       // the live parameters (Rust `live`), which the audio reads
    uint8_t pattern[PARAMS];      // the pattern-mode copy
    uint16_t counter;             // the block counter; the update runs when counter & 15 == 0
    float target[2];              // the pan gains' targets
    float gain[2];                // the smoothed pan gains
    float feedback;
    uint32_t length;              // the delay in samples
    uint32_t write;               // the write position, 0 at the ctor and never reset
    float peak;                   // the peak meter of the send bus

    explicit Delay(ProcessState* process);

    const uint8_t* live(uint32_t* len) const override;
    void reset() override;
    void silence() override;
    void set_song_mode(bool on) override;
    void data(const uint8_t* payload, uint32_t len, bool song_mode) override;
    void set_param(uint8_t ctrl, uint8_t value) override;
    void init_track(TrackMut& track) override;

    void process(Block& send, Block& l, Block& r, const BlockContext& ctx) override;
    OptI32 stack_word(OptI32 out_end, OptI32 prev) const override;

    // delay::length: the delay length in samples for a tempo (BPM*1000), steps and mode.
    static uint32_t length_of(uint32_t tempo, uint8_t steps, uint8_t mode);
};

}  // namespace orb
