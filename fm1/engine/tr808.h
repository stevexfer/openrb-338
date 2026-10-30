// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/tr808.rs: the TR-808 (CTR808Device).
//
// The step function plays the current pattern on the sequencer's ticks: on each new step it
// computes the accent and triggers the step's instruments in ascending column order. The audio
// zeroes the bus and adds the voices in a fixed order.
#pragma once

#include "drums.h"

namespace orb {

class Tr808 : public ChannelDevice {
public:
    // The parameter block (`+0x30` pattern-mode copy, `+0x1890` live): controllers 0..28 in
    // RBS42 order, then the step function's last step at byte 29 and two unused bytes.
    enum {
        PARAMS = 32,
        ENABLED = 0, PATTERN = 1, ACCENT = 2,
        BD_LEVEL = 3, BD_TONE = 4, BD_DECAY = 5,
        SD_LEVEL = 6, SD_TONE = 7, SD_SNAPPY = 8,
        // the toms' level, tuning and selector: low at 9, mid at 12, high at 15
        LT_LEVEL = 9, MT_LEVEL = 12, HT_LEVEL = 15,
        RS_LEVEL = 18, RS_SELECT = 19, CP_LEVEL = 20, CP_SELECT = 21, CB_LEVEL = 22,
        CY_LEVEL = 23, CY_TONE = 24, CY_DECAY = 25, OH_LEVEL = 26, OH_DECAY = 27, CH_LEVEL = 28,
        LAST_CTRL = CH_LEVEL,
        LAST_STEP = 29,  // byte 29 of the live block: the last step the step function played
    };
    // One pattern: shuffle flag, length, 16 steps of 12 columns.
    enum { PATTERN_LEN = 194, PATTERNS = 32 };
    // The step columns: 0 the accent, then the instruments in trigger order.
    enum { AC = 0, BD = 1, SD = 2, LT = 3, MT = 4, HT = 5, RS = 6, CP = 7, CB = 8, CY = 9, OH = 10, CH = 11 };
    // The sample slots: slot n = resource 999 + n.
    enum {
        SLOT_BD = 1, SLOT_CB = 11, SLOT_CH = 12, SLOT_CL = 13, SLOT_CP = 14, SLOT_CY = 15,
        SLOT_HC = 16, SLOT_HT = 17, SLOT_LC = 18, SLOT_LT = 19, SLOT_MA = 20, SLOT_MC = 21,
        SLOT_MT = 22, SLOT_OH = 23, SLOT_RS = 24,
    };
    enum { SIMPLE_RS = 0, SIMPLE_CP = 1, SIMPLE_CB = 2, SIMPLE_OH = 3, SIMPLE_CH = 4 };
    enum { FILTER_BD_A = 0, FILTER_BD_B = 1, FILTER_CY = 2 };

    static const uint8_t DEFAULT_PARAMS[PARAMS];

    // `tables` must be set (here or by assigning the member) before the first step: the toms
    // read tables->tr808_tom_step. process() also stores ctx.tables.
    Tr808(const SampleSet* samples, ProcessState* process, const Tables* tables = 0);

    int32_t pat_start;                          // +4: the tick of the bar the pattern started at
    uint8_t params[PARAMS];                     // +0x30: the pattern-mode copy
    uint8_t patterns[PATTERNS * PATTERN_LEN];   // +0x50: the 32 patterns, as msg 56 sends them
    uint8_t live_params[PARAMS];                // +0x1890: the live parameters
    uint8_t cur_pattern;                        // +0x18D0: the playing pattern
    uint8_t enabled;                            // +0x18D1: the enable flag latched at the bar line
    bool wrapped;                               // +0x18D2: forces the next step to trigger
    int32_t last_pos;                           // +0x18D4: the pattern position of the last step
    Simple simple[5];                           // RS/CL, CP/MA, CB, OH, CH
    Pitched pitched[3];                         // LT/LC, MT/MC, HT/HC
    Filter filter[3];                           // BD A, BD B, CY
    Dual dual;                                  // SD
    float accent;                               // +0x190C: the accent of the last step played
    bool song_mode;
    const SampleSet* samples;
    ProcessState* process_state;
    const Tables* tables;

    void step(int32_t tick, int32_t shuffled, int32_t tempo);
    void relocate(int32_t tick, int32_t now);
    void start(int32_t now);
    void stop();
    void reset();
    void silence();
    void set_song_mode(bool on);
    // Msg 56: the 32-byte parameter block, then the 32 patterns (len >= 32 + 32*194).
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
    real level(uint8_t l, float scale) const;
    void trigger(uint32_t i);
    void bass_or_cymbal(bool bd);
    void snare();
    void tom(uint32_t i);
};

}  // namespace orb
