// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/frame/mod.rs: the engine frame (the 64-frame block loop, the
// mixer, the int16 output conversion) and the song object that drives it.
//
// The DLL's song object (ctor `sub_10011350`) renders through `sub_100139E0` (one
// `SafeSoundCallback` worth of frames, the leftover buffer and the int16 conversion), which calls
// the mixer block `sub_10013444` once per 64 frames; every 16 blocks that block runs the mixer
// update `sub_10012C14`. Engine is that song object: Engine::render is `sub_100139E0`,
// Frame::block is `sub_10013444`, Frame::mixer_update is `sub_10012C14`.
//
// Not ported: the sample hot-swap (msg 19, Retired), the EngineState snapshots, the panel's edit
// messages (frame/edit.rs) and the msg-37 batch load (Engine::load_blob adopts the batches'
// result from the song blob instead).
#pragma once

#include "comp.h"
#include "delay.h"
#include "dist.h"
#include "pcf.h"
#include "seq.h"
#include "tb303.h"
#include "tr808.h"
#include "tr909.h"

// The most frames one Engine::render call renders (`cmp eax, 4000h` at 0x100139ED). The engine
// keeps a float buffer of this many stereo frames; a smaller value (the FM-1's) clamps longer
// calls, which then differ from the original.
#ifndef ORB_MAX_FRAMES
#define ORB_MAX_FRAMES 0x4000
#endif

namespace orb {

#ifdef ORB_TEST_SOLO
// host tests: >= 0 mixes only that channel (0, 1 the 303s, 2 the 808, 3 the 909); the others still run
extern int test_solo;
#endif

static const uint32_t MAX_FRAMES = ORB_MAX_FRAMES;
// The mixer runs its update when `counter & 15 == 0` (0x10013453).
static const uint32_t UPDATE_PERIOD = 16;
// The block counter's constructor value (0x1001145A).
static const uint32_t COUNTER_INIT = 10;
// `unk_100F012C`: the right channel's float output of a render call (0x10013A3A), at the
// preferred base 0x10000000.
static const int32_t OUT_RIGHT = 0x100F012C;
// What the render call's try-lock (`sub_10009450` at 0x10013A5E) leaves at Frame::stack_word's
// address: the system DLLs' code below it writes there, last a 0 (task-072).
static const int32_t LOCK_WORD = 0;

// One mixer channel's parameters (the mixer device's channel block, `sub_10015D10`).
struct ChannelParams {
    uint8_t enabled, level, pan, send, dist;
};

// The mixer device's parameters, which the mixer update reads (`sub_10015D30`: mixer `+0x30`).
struct MixerParams {
    uint8_t master;
    uint8_t comp_id;  // 0 off, 1 the master, 2..5 channel 0..3
    uint8_t pcf_id;   // 0 off, 1..4 channel 0..3
    ChannelParams channels[4];

    static const uint32_t PAYLOAD_LEN = 29;
    // The mixer device ctor's values (`sub_10015400` -> `sub_10019CC0`, `sub_10019D50`): master
    // 100, comp and PCF off; every channel enabled, level 100, centered, no send, no distortion.
    static MixerParams defaults();
    // The msg 41 `0/143` block (`sub_10019DE0`): master, comp id, PCF id, two unused bytes, then
    // per channel enabled, level, pan, send, dist and an unused byte.
    static MixerParams from_payload(const uint8_t* b);
};

// The mixer device (`CMixerDevice`, vtable 0x10033214, ctor `sub_10015400`): the live parameters
// at `+0x30`, which the mixer update reads, and the pattern-mode copy at `+0x58`.
class MixerDevice : public SongDevice {
public:
    MixerParams live_params;
    MixerParams pattern;

    MixerDevice();

    void set_song_mode(bool on) override;
    void data(const uint8_t* payload, uint32_t len, bool song_mode) override;
    void init_track(TrackMut& track) override;
    void set_param(uint8_t ctrl, uint8_t value) override;
    // The mixer has no `{ctrl, v}` message: its live messages are 145 and 147.
    void param_message(uint8_t ctrl, uint8_t value) override { (void)ctrl; (void)value; }

    // Msg 145 `{0, v}` (0x10015928): the live master level.
    void set_master(uint8_t v) { live_params.master = v; }
    // Msg 147 `{c, 0, v}` (0x10015A12): channel c's live enabled byte; above 3 ignored.
    void set_enabled(uint32_t c, uint8_t v)
    {
        if (c < 4) live_params.channels[c].enabled = v;
    }
};

// The insert slots a mixer channel routes its bus through, in processing order (`+0xC4`,
// `+0xC8`, `+0xCC` of the channel block).
struct Routing {
    bool dist, pcf, comp;
};

// One mixer channel's state: the 44-byte block at `song+0xA4 + 44c`. The song ctor's values
// (0x100113AF..0x100113D1): every gain 1e-6, no peak, no insert.
struct ChannelState {
    float target_l;  // +0xA8
    float target_r;  // +0xA4
    float gain_l;    // +0xB0: the left gain at the start of the next block
    float gain_r;    // +0xAC
    float send;      // +0xB4: the send gain (no smoothing)
    float peak;      // +0xB8: the peak meter (display only)
    uint8_t meter;   // the meter byte the update writes into the mixer device's channel block
    Routing routing;
    ChannelState();
};

// The mixer's state in the song object; the song ctor's values, before its one mixer update
// (0x100119D4).
struct MixerState {
    uint32_t counter;      // song+0x188: the block counter
    uint16_t meter_index;  // song+0xA0: `(i + 23) & 63` per block
    ChannelState channels[4];
    float master_target;   // song+0x17C
    float master;          // song+0x180
    bool master_comp;      // song+0x184 non-null: the compressor on the master
    float master_peak[2];  // song+0, song+4 (display only)
    uint8_t master_meter[2];
    Block send;            // song+0x160C: the send bus (the delay's input)
    MixerState();
};

// The output side of `sub_100139E0`: the leftover block and its position (song ctor: 0, zeros).
struct OutputState {
    // song+0x208: `L > 0` means samples `L..64` of the leftover block are still to be output.
    int32_t leftover_pos;
    // song+0x20C, channels 0 (L) and 1 (R) of the 18 (the others are never enabled).
    Block leftover[2];
    OutputState();
};

// The faults of an Engine's render calls: the first one and how many calls faulted.
struct FaultRecord {
    Fault first;
    uint64_t block;  // the block it ended, counted from 0 over every block started
    uint64_t calls;  // the render calls that faulted (each left its int16 buffer unwritten)
};

// The mixer and its devices: everything one block touches.
class Frame {
public:
    MixerState state;
    // song+0x16D, the sound-model byte non-zero: the pan-dependent send.
    bool vintage;
    // `devices.mixer` is a MixerDevice.
    Devices devices;
    // song+8, run by every block.
    Sequencer seq;
    // Not in the original: the blocks started so far, for FaultRecord::block.
    uint64_t blocks;
    // The dword at the mixer block's `esp - 0x24`, the PCF's uninitialised `var_1C`
    // (BlockContext::stack_word); none where the original leaves something unknown.
    OptI32 stack_word;
    // The address of the last right-channel sample the current block renders into
    // (`OUT_RIGHT + 4*(pos + 63)`), none for the partial block. Set by Engine::render.
    OptI32 out_end;

    Frame(const Tables* tables, const Devices& devices);
    const Tables& tables() const { return *tables_; }
    MixerDevice& mixer() { return *static_cast<MixerDevice*>(devices.mixer); }

    // The mixer update `sub_10012C14`, every 16 blocks and once at the end of the song ctor.
    void mixer_update();
    // The mixer block `sub_10013444`: one block into the master buses l and r. A fault ends it
    // right after the faulting insert, as the original's exception does: false, *fault set.
    bool block(Block& l, Block& r, Fault* fault);

private:
    const Tables* tables_;
    // `unk_1010012C`: the channel bus every device renders into in turn.
    Block bus_;
};

// The float-to-int16 conversion of `sub_100139E0` (0x10013C4E).
int16_t to_i16(float f);

// What top-level msg 53 finds when it makes a song current (`sub_1001BDA4`).
enum Activation {
    // Another, stopped song was current (the app's "Untitled song", rbhost by default): the
    // old song's slot 6, of which only the delay's refill reaches this song.
    ACTIVATE_REPLACES_STOPPED,
    // Another, playing song was current: nothing (the switch is made later).
    ACTIVATE_REPLACES_PLAYING,
    // No song was current (rbhost `--no-untitled`): slot 6 on this song's devices.
    ACTIVATE_NONE_CURRENT,
};

// The process globals (crate::process): the drum counters and the delay line, shared by every
// song of the process. A static instance, all zero as when the DLL is loaded.
ProcessState& process_globals();

// The song's device instances (Devices::implemented): one of each.
struct DeviceSet {
    MixerDevice mixer;
    Tb303 tb303_1, tb303_2;
    Tr808 tr808;
    Tr909 tr909;
    Delay delay;
    Distortion dist;
    Pcf pcf;
    Compressor comp;
    DeviceSet(const SampleSet* samples, ProcessState* process, const Tables* tables);
    Devices devices(ProcessState* process);
};

// The song object as `SafeSoundCallback` drives it. Holds every device; no heap.
class Engine {
    DeviceSet set_;

public:
    Frame frame;
    OutputState output;

    // A new song as the ctor `sub_10011350` leaves it: the constructor state, every device's
    // slot 7 then slot 8 with its own track (0x10011941, 0x10011985), the tempo 140000
    // (0x100119BF), a locate to 0 (0x100119CC), then one mixer update (0x100119D4). `samples`
    // and `process` must outlive the engine.
    Engine(const Tables* tables, const SampleSet* samples, ProcessState* process);

    // The faults of the render calls so far (null: none).
    const FaultRecord* fault() const { return has_fault_ ? &fault_ : 0; }

    // The song load (Rust Engine::load) from a song blob of fm1/pack ("ORBS"): msg 36, the
    // msg-41 device blocks, for a 1.x song the 909's msg 74, then the track lists the msg-37
    // batches made (adopted from the blob, which must stay valid and unchanged while the engine
    // uses it; 2-byte aligned) and msg 32. False on a bad blob, which may leave the song half
    // loaded.
    bool load_blob(const uint8_t* blob, uint32_t len);
    // The blob's title (NUL terminated, up to 31 characters), or null on a bad blob.
    static const char* blob_title(const uint8_t* blob, uint32_t len);

    // Song msg 36, 0x100126B2: the global struct, applied field by field.
    void set_globals(const Globals& g);
    // Song msg 41 (0x1001285E): device d's data block, with the current song mode.
    void device_data(Device d, const uint8_t* payload, uint32_t len);
    // Song msg 32 (0x10012526): locate to the current tick.
    void relocate();
    // Top-level msg 53, `sub_1001BDA4`.
    void activate(Activation a);

    // `sub_100139E0(song, buf, n)`: renders n frames (clamped to MAX_FRAMES) of interleaved int16
    // stereo into buf[0..2n], through the leftover block. A call with n = 0 (buf may be null)
    // renders nothing but drops a pending leftover. A faulting call leaves buf as it was.
    void render(int16_t* buf, uint32_t n);

    // Song msg 35 `{1,0}` (`sub_1001E100`).
    void stop();
    // Song msg 35 `{8,tick}` (`sub_1001BF70`; the u32 is used as a signed tick).
    void locate(uint32_t tick);
    // Song msg 35 `{1,1}` (`sub_1001BF50`).
    void play();
    // The song's play flag (`song+0x159`).
    bool playing() const { return frame.seq.globals.play != 0; }
    // `sub_100218A0`: the stop of the song switch, without the bar locate.
    void halt();
    // Song msg 35 `{0,m}` (`sub_1001FBE0`).
    void set_song_mode(uint8_t mode);
    // Slot 11 of device d (`sub_10012B40`): what a track event does.
    void set_param(Device d, uint8_t ctrl, uint8_t value);
    // Song msg 33, `sub_10014140`: the master's gain and peaks to 0, the send bus to the guard,
    // the leftover blocks to 0 (not its position), then slot 5 on every device.
    void reset();

    // Not in the original: the song as a blob load_blob reads back, bytes [off, off + n) into
    // out (nothing when out is null); the result is the blob's length. With `old` (the valid
    // blob the song was loaded from) its layout and what the engine does not keep (title, track
    // lists, loop) are kept; without, a new song "NEW SONG" with every device's block and empty
    // tracks. The device blocks are the pattern-mode copies, in pattern mode taken from the live
    // parameters (the knobs as they are); the song mode, tempo and shuffle are the current ones.
    uint32_t save_blob(const uint8_t* old, uint32_t old_len, uint32_t off, uint8_t* out, uint32_t n);

private:
    // Byte i of device d's msg-41 payload as save_blob writes it; -1 for a byte it keeps.
    int payload_byte(Device d, uint32_t i);

    // `unk_100E012C`, `unk_100F012C`: the float output of the current call.
    float out_[2][MAX_FRAMES];
    bool has_fault_;
    FaultRecord fault_;
};

}  // namespace orb
