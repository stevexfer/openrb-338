// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/tb303.rs.
//
// The TB-303 (`CTB303Device`, vtable 0x10033FD4, 0xD04 bytes, ctor `sub_10023920`). The step
// function (slot 1, 0x100253A0) plays the current pattern on the sequencer's ticks: note-on
// 0x10023CD4, note-off 0x10023C74. The audio (slot 14, 0x10023F10) runs one of two engines,
// chosen by the song's sound-model byte (`song+0x16D`, tested at 0x10023F2D):
// - zero, "ReBirth 2.0 sound": `sub_100248C4`, control values once per 64-sample block, the
//   filter coefficients interpolated between two resonance rows (`sub_10023800`) and ramped
//   every 2 samples;
// - non-zero, "Vintage": inline in 0x10023F10, control values every 4 samples from one row.
// Both feed the same per-sample voice: oscillator, one-pole pre-filter, 2-pole resonant
// low-pass and DC blocker.
#pragma once

#include "orb_core.h"

namespace orb {

class Tb303 : public ChannelDevice {
public:
    // The parameter block (`+0x30` pattern-mode copy, `+0xC7C` live): controllers 0..8, then
    // the step function's last step at byte 9 and two unused bytes.
    static const int PARAMS_LEN = 12;
    // Live/param byte indices.
    static const int ENABLED = 0;
    static const int PATTERN = 1;
    static const int TUNE = 2;
    static const int CUTOFF = 3;
    static const int RESONANCE = 4;
    static const int ENVMOD = 5;
    static const int DECAY = 6;
    static const int ACCENT = 7;
    static const int WAVEFORM = 8;
    // Byte 9 of the live block (`+0xC85`): the last step the step function played.
    static const int LAST_STEP = 9;
    // One pattern: shuffle flag, length, 16 steps of `{note, slide, accent, up, down, rest}`.
    static const int PATTERN_LEN = 98;
    static const int PATTERNS = 32;
    // The msg-41 payload: the parameter block, then the patterns.
    static const uint32_t PAYLOAD_LEN = PARAMS_LEN + PATTERNS * PATTERN_LEN;
    // `sub_10018C20` (ctor, both copies): enabled, pattern 0, tune 64, cutoff 20, resonance 90,
    // envmod 20, decay 100, accent 64, saw.
    static const uint8_t DEFAULT_PARAMS[PARAMS_LEN];

    // `+4` (CDevice): the tick of the bar the pattern started at (song mode).
    int32_t pat_start;
    // `+0x30`: the pattern-mode parameter copy.
    uint8_t params[PARAMS_LEN];
    // `+0x3C`: the 32 patterns, 98 bytes each, as msg 39 sends them.
    uint8_t patterns[PATTERNS * PATTERN_LEN];
    // `+0xC7C`: the live parameters (Rust `live`).
    uint8_t live_params[PARAMS_LEN];
    // `+0xC94`: the playing pattern.
    uint8_t cur_pattern;
    // `+0xC95`: the enable flag latched at the bar line.
    uint8_t enabled;
    // `+0xC96`: set by a pattern wrap and by play; forces the next step to trigger.
    bool wrapped;
    // `+0xC98`: the tick position (in the pattern) of the pending gate-off, -1 for none.
    int32_t gate_off;
    // `+0xC9C`: the pattern position of the last step call.
    int32_t last_pos;
    // `+0xCA0`: set by note-on without slide; the 2.0 engine then starts without a ramp, g = 0.
    bool retrig;
    // `+0xCA1`: the slide flag of the last step; a sliding step skips the note-off.
    bool slide;
    // `+0xCA4`: the oscillator phase, -1..1.
    float phase;
    // `+0xCA8`: the envelope (1 at note-on, decays by `env_decay`).
    float env;
    // `+0xCAC`: the envelope decay per control step.
    float env_decay;
    // `+0xCB0`: the target pitch, `+0xCB4` the gliding pitch (both 16 * table index).
    uint32_t pitch_target;
    uint32_t pitch;
    // `+0xCB8`: the accent amount, `+0xCBC` the accent envelope.
    float acc_amount;
    float acc_env;
    // `+0xCC0`, `+0xCC4`: the cutoff smoother's last two outputs.
    float cs1;
    float cs2;
    // `+0xCC8`, `+0xCCC`: the amp's input and feedback; `+0xCD0` the gate; `+0xCD4` the amp.
    float amp_in;
    float amp_fb;
    float gate;
    float amp;
    // `+0xCDC`, `+0xCE0`: the 2-pole's last two outputs; `+0xCE4` the pre-filter;
    // `+0xCE8` the DC blocker.
    float y1;
    float y2;
    float s;
    float o;
    // `+0xCEC`: the accent envelope's feedback.
    float acc_coef;
    // `+0xCF4`..`+0xD00`: the 2.0 engine's coefficients at the block end (c0, c1, c2*G, c3).
    float coefs[4];
    // The song's global byte 0 (`sub_1001FCB0`), followed through slot 9.
    bool song_mode;
    // The song's byte `+0x16D` (`sub_10013DA0()+0x15`), followed through msg 36.
    bool vintage;
    // The computed filter entries in use (without the table): the 2.0 engine's, and the Vintage
    // engine's two resonance rows.
    CoefCache coef_cache[2];

    // The ctor `sub_10023920`.
    Tb303();

    // Note-off `sub_10023C74`.
    void note_off();
    // Note-on 0x10023CD4 with the step's six bytes.
    void note_on(const uint8_t step[6]);

    // SongDevice
    const uint8_t* live(uint32_t* len) const override;
    void step(int32_t tick, int32_t shuffled, int32_t tempo) override;
    void relocate(int32_t tick, int32_t now) override;
    void start(int32_t now) override;
    void stop() override;
    void reset() override;
    void silence() override;
    void set_song_mode(bool on) override;
    void data(const uint8_t* payload, uint32_t len, bool song_mode) override;
    void set_param(uint8_t ctrl, uint8_t value) override;
    void param_message(uint8_t ctrl, uint8_t value) override;
    void set_sound_model(bool vintage) override;
    void init_track(TrackMut& track) override;

    // ChannelDevice
    void process(Block& bus, const BlockContext& ctx) override;
    OptI32 playing_pattern() const override;
    uint8_t* patterns_mut(uint32_t* len) override;
    OptI32 stack_word() const override;

private:
    const uint8_t* pattern(uint8_t p) const;
    void params_live();
    double glide(const Tables& t);
    double smooth_cutoff(const Tables& t);
    void process_20(Block& bus, const Tables& t);
    void process_vintage(Block& bus, const Tables& t);
};

}  // namespace orb
