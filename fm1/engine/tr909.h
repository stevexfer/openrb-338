// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/tr909.rs: the TR-909 (CTR909Device).
//
// The step function plays the current pattern on the sequencer's ticks: on each new step it
// computes the accent and triggers the step's instruments in descending column order, each at
// the accent times 0.8 (step value 1 or 3) or 2.0 (value 2 or 4). A new step also arms the
// flam: the first later call whose tick is past the flam tick retriggers every instrument whose
// value is above 2 at the accent times 1.5.
//
// The parameter blocks keep the DLL's internal order, which differs from the RBS and controller
// order in bytes 20..22: the DLL holds the hi-hat level at 20, the rim shot's at 21 and the
// clap's at 22 (see internal_index).
#pragma once

#include "drums.h"

namespace orb {

class Tr909 : public ChannelDevice {
public:
    // The parameter block (`+0x30` pattern-mode copy, `+0x1891` live) in the DLL's order:
    // controllers 0..29 (bytes 20..22 permuted), the last step at byte 30 and two unused bytes.
    enum {
        PARAMS = 33,
        ENABLED = 0, PATTERN = 1, ACCENT = 2,
        BD_LEVEL = 3, BD_TUNE = 4, BD_ATTACK = 5, BD_DECAY = 6,
        SD_LEVEL = 7, SD_TUNE = 8, SD_TONE = 9, SD_SNAPPY = 10,
        // the toms' level, tuning and decay: low at 11, mid at 14, high at 17
        LT_LEVEL = 11, MT_LEVEL = 14, HT_LEVEL = 17,
        HH_LEVEL = 20,  // the hi-hat level (controller 22)
        RS_LEVEL = 21,  // the rim shot's level (controller 20)
        CP_LEVEL = 22,  // the clap's level (controller 21)
        CH_DECAY = 23, OH_DECAY = 24, CC_LEVEL = 25, CC_TUNE = 26, RC_LEVEL = 27, RC_TUNE = 28,
        FLAM = 29,  // the flam interval (controller 29)
        LAST_STEP = 30,
        LAST_CTRL = FLAM,
        INIT_CTRLS = 30,  // the controllers whose tick-0 events init_track inserts
    };
    enum { PATTERN_LEN = 194, PATTERNS = 32 };
    // The step columns: 0 the accent, then the instruments (triggered from 11 down to 1).
    enum { AC = 0, BD = 1, SD = 2, LT = 3, MT = 4, HT = 5, RS = 6, CP = 7, CH = 8, OH = 9, CC = 10, RC = 11 };
    // The sample slots: slot n = resource 1000 + n. bd1..bd4 (25..28), sd1..sd5 (29..33),
    // sdn1..sdn3 (34..36).
    enum {
        SLOT_BD1 = 25, SLOT_LT = 37, SLOT_MT = 38, SLOT_HT = 39, SLOT_RS = 40, SLOT_CP = 41,
        SLOT_CH = 42, SLOT_OH = 43, SLOT_CC = 44, SLOT_RC = 45,
    };
    enum { SIMPLE_RS = 0, SIMPLE_CP = 1, SIMPLE_CH = 2, SIMPLE_OH = 3 };
    enum { FILTER_BD_A = 0, FILTER_BD_B = 1 };

    static const uint8_t DEFAULT_PARAMS[PARAMS];

    // The byte of the DLL's block that controller (and RBS byte) c maps to: 20..22 permuted.
    static uint32_t internal_index(uint32_t c)
    {
        return c == 20 ? (uint32_t)RS_LEVEL : c == 21 ? (uint32_t)CP_LEVEL : c == 22 ? (uint32_t)HH_LEVEL : c;
    }

    // `tables` must be set (here or by assigning the member) before the first step: the
    // triggers read tables->tr909_tom_step and tables->tr909_decay. process() also stores
    // ctx.tables.
    Tr909(const SampleSet* samples, ProcessState* process, const Tables* tables = 0);

    int32_t pat_start;                          // +4: the tick of the bar the pattern started at
    uint8_t params[PARAMS];                     // +0x30: the pattern-mode copy (DLL order)
    uint8_t patterns[PATTERNS * PATTERN_LEN];   // +0x51: the 32 patterns, as msg 73 sends them
    uint8_t live_params[PARAMS];                // +0x1891: the live parameters (DLL order)
    uint8_t cur_pattern;                        // +0x18D3: the playing pattern
    uint8_t enabled;                            // +0x18D4: the enable flag latched at the bar line
    bool wrapped;                               // +0x18D5: forces the next step to trigger
    int32_t last_pos;                           // +0x18D8: the pattern position of the last step
    Simple simple[4];                           // RS, CP, CH, OH
    Pitched pitched[5];                         // LT, MT, HT, crash, ride
    Filter filter[2];                           // BD A, BD B
    Dual dual;                                  // SD
    bool flam_pending;                          // +0x1910: a new step armed the flam
    int32_t flam_tick;                          // +0x1914: the tick the flam waits to pass
    float accent;                               // +0x1918: the accent of the last step played
    float gain;                                 // +0x191C: the gain of the instrument triggered
    bool song_mode;
    const SampleSet* samples;
    ProcessState* process_state;
    const Tables* tables;

    // The pattern-mode block in RBS (controller) order (sub_10019624).
    void params_rbs_order(uint8_t out[PARAMS]) const;

    void step(int32_t tick, int32_t shuffled, int32_t tempo);
    void relocate(int32_t tick, int32_t now);
    void start(int32_t now);
    void stop();
    void reset();
    void silence();
    void set_song_mode(bool on);
    // Msg 73: the 33-byte parameter block in RBS order, then the 32 patterns
    // (len >= 33 + 32*194).
    void data(const uint8_t* payload, uint32_t len, bool song_mode);
    void set_param(uint8_t ctrl, uint8_t value);
    void param_message(uint8_t ctrl, uint8_t value);
    void init_track(TrackMut& track);
    const uint8_t* live(uint32_t* len) const;

    void process(Block& bus, const BlockContext& ctx);
    OptI32 playing_pattern() const;
    uint8_t* patterns_mut(uint32_t* len);
    OptI32 stack_word() const;

private:
    const uint8_t* pattern(uint8_t p) const;
    const Sample* sample(uint32_t n) const;
    float dt(uint8_t d) const;
    void trigger(uint32_t i);
    void trigger_simple(uint32_t i, uint32_t smp, uint8_t level, uint8_t decay);
    void trigger_pitched(uint32_t i, uint32_t smp, uint8_t level, uint8_t tune, uint8_t decay);
    void bass(uint8_t level, uint8_t decay, uint8_t tune, uint8_t attack);
    void snare(uint8_t level, uint8_t tone, uint8_t tune, uint8_t snappy);
};

}  // namespace orb
