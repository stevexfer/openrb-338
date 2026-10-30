// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/seq.rs: the sequencer (the song position, the track events,
// the chase, the loop and the transport; `docs/initial_investigation.md` section 5).
//
// The DLL's sequencer object sits at `song+8` (ctor `sub_1001EC50`): the position as a double at
// `+0`, the last tick at `+8`, the song-mode tick saved by pattern mode at `+0x10`, the track
// count at `+0xC` and the 32 track pointers at `+0x14` (nine are used, one per device). The
// song's global struct (`song+0x158`, Globals) holds the transport state the sequencer reads.
// Sequencer::tick is `sub_1001EF20`, which the mixer block `sub_10013444` runs once per block
// before any device renders (0x1001346A): every device call made here lands between two blocks.
//
// A track (ctor `sub_1001EDA0`, 0x22C bytes) is a sorted circular list (list ctor `sub_100144B0`
// at `track+0x108`), a pointer to the last applied node (`+0x228`), the chase state (`+0x84`), a
// per-controller hold mask (`+4`) and the device id (`+0x104`). A second list at `track+0x198`
// holds what is being recorded (Sequencer::record_hook) until the merge `sub_1001F520` moves it
// into the first.
//
// The node pool: the DLL takes every list node from one pool of POOL_NODES nodes
// (`dword_10100230`, `sub_100142C0`); `sub_10014434` refuses a node when one or none is free.
// Pool is one fixed buffer of POOL_NODES nodes holding the song's 18 lists back to back.
//
// Numerics: the position is a double on every build (a float cannot hold ticks up to 767231
// with the sub-tick fraction); it runs once per block, so soft-double on the FM-1 is fine.
//
// Capacity (not in the Rust): the Rust's unchecked insert (Track::insert, the song ctor's) can
// grow its Vec past POOL_NODES; here the buffer holds exactly POOL_NODES nodes and an unchecked
// insert into a full pool stores nothing and returns false. Every checked path (msg 37, the
// record hook, the merge) leaves at least one node free, as in the Rust.
#pragma once

#include "orb_core.h"

#ifndef ORB_POOL_NODES
#define ORB_POOL_NODES 100000
#endif
// The merge buffer's nodes. The merge's result does not depend on it (a full buffer is moved
// into the list before the next node goes in), only how often the pool's nodes move.
#ifndef ORB_MERGE_CHUNK
#define ORB_MERGE_CHUNK 4096
#endif

namespace orb {

// `0x186A0` at 0x1001431B: the nodes of the DLL's event pool (100000 on the host).
static const uint32_t POOL_NODES = ORB_POOL_NODES;
// The tracks of a song, one per device.
static const int TRACKS = 9;
// The lists of a song's Pool: list k < 9 is track k's (`+0x108`), list 9 + k its recording
// (`+0x198`).
static const int LISTS = 2 * TRACKS;
static const uint32_t MERGE_CHUNK = ORB_MERGE_CHUNK;

// `dbl_10153808`: the position step per block per BPM*1000, `64*192 / (60*44100*1000)`.
inline double tempo_step() { return F64(0x3ED37A7281B22126ull); }  // 4.6439909297052154e-06
// `dbl_10153810`: the song ends when the position passes this (999 bars minus a tick).
inline double song_end_pos() { return F64(0x412769FE00000000ull); }  // 767231.0
// `sub_10014750` drops an event whose quantised tick is above this (`cmp eax, 0BB4FFh`).
static const int32_t LAST_EVENT_TICK = 767231;

// song::TrackEvent: one event of a msg-37 batch.
struct TrackEvent {
    uint32_t tick;  // a multiple of 24 below MAX_TICK
    uint8_t ctrl;
    uint8_t value;
};

// song::Globals, the song's global struct (DLL `song+0x158`), as song msg 36 sets it. Each byte
// offset is also the msg 35 id that sets the field.
struct Globals {
    uint8_t song_mode;    // byte 0: 0 pattern mode, 1 song mode
    uint8_t play;         // byte 1
    uint8_t byte2;        // byte 2: record (`0x10021194`)
    uint8_t loop_on;      // byte 3
    uint32_t tempo;       // bytes 4..7: BPM * 1000
    uint32_t locate;      // bytes 8..11
    uint32_t loop_start;  // bytes 12..15, in ticks
    uint32_t loop_end;    // bytes 16..19, in ticks
    uint8_t shuffle;      // byte 20: 0..127
    uint8_t vintage;      // byte 21, `song+0x16D`: the sound model (non-zero: Vintage)
    uint8_t byte22;       // byte 22, `song+0x16E`: 1 at load

    static const uint32_t PAYLOAD_LEN = 23;
    // 0x10023F2D, 0x10012C9F.
    bool is_vintage() const { return vintage != 0; }
    // The msg 36 payload (multi-byte values big-endian).
    void payload(uint8_t b[PAYLOAD_LEN]) const;
};

// `trunc(pos)`: `fistp qword` under a truncating control word, of which the low dword is used
// (0x1001EF88, `sub_1001F870`). Rust `pos as i64 as i32`.
int32_t trunc_pos(double pos);

// The shuffled tick of 0x1001F3D0..0x1001F429: `96*(t/96) + max(0, t%96 - shuffle/6)`.
int32_t shuffled(int32_t tick, uint8_t shuffle);

// One node of a track's list: `+0` next, `+4` tick, `+8` controller, `+9` value. Packed to 6
// bytes (2-byte aligned, little endian: a song blob's node layout); do not take the address of
// `tick`.
#pragma pack(push, 2)
struct Event {
    int32_t tick;
    uint8_t ctrl;
    uint8_t value;
};
#pragma pack(pop)
static_assert(sizeof(Event) == 6, "the blob's node layout");

// A list of the pool (the Rust `&[Event]`).
struct EventList {
    const Event* data;
    uint32_t len;
    const Event& operator[](uint32_t i) const { return data[i]; }
    const Event* begin() const { return data; }
    const Event* end() const { return data + len; }
    bool empty() const { return len == 0; }
};

// A song's node pool: its LISTS lists, each in list order, back to back in one buffer of
// POOL_NODES nodes. The tracks' lists come first, so a recorded node moves only the recording
// lists after it.
//
// Not in the Rust: a track's list (k < TRACKS) may instead be external, read-only nodes (a song
// blob in flash, Sequencer::adopt), which take no node of the buffer. A change of an external
// list first copies it into the buffer (own); when it does not fit, the change is dropped as
// on a full pool.
class Pool {
public:
    Pool();
    // List k.
    EventList list(int k) const;
    EventList operator[](int k) const { return list(k); }
    // The nodes of all lists in the buffer (not the external ones).
    uint32_t used() const { return len_; }
    // The nodes free (`[pool+4]`, what `sub_10014434` reads): the buffer's.
    uint32_t free() const { return len_ < POOL_NODES ? POOL_NODES - len_ : 0; }
    // This pool becomes a copy of src's track lists (0..9), with empty recording lists; an
    // external list stays external.
    void copy_tracks_from(const Pool& src);
    // Whether list k is external.
    bool external(int k) const { return k < TRACKS && ext_[k] != 0; }
    // The external lists copied into the buffer so far, and the copies that did not fit.
    uint32_t copies() const { return copies_; }
    uint32_t copy_failures() const { return copy_failures_; }

private:
    friend class Track;
    friend class Sequencer;
    uint32_t start(int k) const { return k == 0 ? 0 : ends_[k - 1]; }
    // Node e at index `at` of list k (not external); false (nothing stored) when the buffer is
    // full.
    bool insert(int k, uint32_t at, const Event& e);
    void remove(int k, uint32_t at);
    void clear(int k);
    // List k (< TRACKS) becomes the n nodes at ext, its nodes in the buffer freed.
    void adopt(int k, const Event* ext, uint32_t n);
    // Copies list k into the buffer when it is external; false when it does not fit.
    bool own(int k);

    Event nodes_[POOL_NODES];
    uint32_t len_;
    // The end of each list in nodes_; list k starts where list k-1 ends.
    uint32_t ends_[LISTS];
    // The external lists (null: in the buffer) and their lengths.
    const Event* ext_[TRACKS];
    uint32_t ext_len_[TRACKS];
    uint32_t copies_, copy_failures_;
};

// A merge's new nodes, in the list's order, in a buffer of MERGE_CHUNK nodes. Empty between
// merges.
struct MergeBuf {
    Event e[MERGE_CHUNK];
    uint32_t len;
};

// One track: the events of one device. Its two lists are in the song's Pool: list device id
// and its recording, list 9 + device id.
class Track {
public:
    // `+0x104`: the device whose set-param (slot 11) the events go to.
    Device device;
    // `+4`: a controller whose byte is set is skipped by the clock's apply loops (not by the
    // chase). Set by the record hook (0x10021402), cleared by the merge (0x1001F767).
    bool hold[128];

    explicit Track(Device d = DEV_MIXER);

    // The track's list in pool.
    EventList events(const Pool& pool) const { return pool.list(list_); }
    // The recording list (`+0x198`): what the record hook took since the last merge.
    EventList recorded(const Pool& pool) const { return pool.list(rec()); }
    // The index of the next event the clock applies.
    uint32_t next() const { return next_; }

    // `sub_10014600(list, ctrl, q)`: controller ctrl's quantisation becomes q 24-tick units.
    void set_quant(uint8_t ctrl, uint8_t q) { quant_[ctrl & 127] = q; }

    // `sub_10014750`, without the pool's free check (the song ctor's inserts): the tick rounded
    // up to the controller's quantisation, dropped above LAST_EVENT_TICK; a node of the same tick
    // and controller takes the value, else a new node goes before the first node whose tick is
    // not below it. Returns whether the event was kept.
    bool insert(Pool& pool, int32_t tick, uint8_t ctrl, uint8_t value);

    // The chase `sub_100151F0(list, state, p)`: the value of the last event at or before p for
    // each controller (0xFF for none) into state, and the applied pointer moved to that event.
    void chase(const Pool& pool, int32_t p, uint8_t state[128]);

    // The clock's apply loop (0x1001F2C6, 0x1001F032, 0x1001FADB): every event after the
    // applied one with `tick <= t`, in list order, skipping held controllers.
    template <typename F> void advance(const Pool& pool, int32_t t, F apply)
    {
        EventList events = pool.list(list_);
        while (next_ < events.len && t >= events[next_].tick) {
            Event e = events[next_];
            if (!hold[e.ctrl & 127]) apply(e);
            next_++;
        }
    }

    // `sub_100149F0`, run on every track by stop: an event whose value equals the previous value
    // of its controller (0xFF before the first) is removed, unless it is at tick 0.
    void prune(Pool& pool);

private:
    friend class Sequencer;
    // Where find found a node.
    struct Found {
        enum Kind { NONE, PENDING, NODE } kind;
        uint32_t at;  // PENDING: index in the merge buffer; NODE: index in the pool's nodes
    };
    // What insert_into did: a new node at *at, a node of the same tick and controller took the
    // value, or dropped (above LAST_EVENT_TICK, or no node free).
    enum Inserted { INS_AT, INS_REPLACED, INS_DROPPED };

    static Inserted insert_into(Pool& pool, int k, const uint8_t* quant, int32_t tick, uint8_t ctrl,
                                uint8_t value, bool pooled, uint32_t* at);
    int rec() const { return TRACKS + list_; }
    int32_t quantise(int32_t tick, uint8_t ctrl) const;
    bool insert_to(Pool& pool, int32_t tick, uint8_t ctrl, uint8_t value, bool pooled);
    bool insert_pooled(Pool& pool, int32_t tick, uint8_t ctrl, uint8_t value)
    {
        return insert_to(pool, tick, ctrl, value, true);
    }
    // The record hook's list part (0x100213EF..0x10021506). `wrap`: loop on, with
    // (loop start, loop end).
    void record(Pool& pool, int32_t now, bool playing, bool wrap, int32_t wrap_start, int32_t wrap_end,
                uint8_t ctrl, uint8_t value);
    // `sub_1001F520(track, t)`, the merge of the recording into the track.
    void merge(Pool& pool, MergeBuf& buf, int32_t t);
    Found find(const Pool& pool, const MergeBuf& buf, int32_t t, uint8_t ctrl) const;
    void add(Pool& pool, MergeBuf& buf, int32_t tick, uint8_t ctrl, uint8_t value);
    void flush(Pool& pool, MergeBuf& buf);

    // Its list in the pool; the recording list is TRACKS + list_.
    uint8_t list_;
    // The index of the node after the one `+0x228` points at (0 when it points at the head).
    uint32_t next_;
    // List `+0xD`: each controller's quantisation in 24-tick units, 1 from the list ctor.
    uint8_t quant_[128];
};

// A track with the song's pool: what a device's slots 7 and 8 get from the song ctor
// (SongDevice::init_track).
class TrackMut {
public:
    Track* track;
    Pool* pool;
    TrackMut(Track& t, Pool& p) : track(&t), pool(&p) {}
    bool insert(int32_t tick, uint8_t ctrl, uint8_t value) { return track->insert(*pool, tick, ctrl, value); }
    void set_quant(uint8_t ctrl, uint8_t q) { track->set_quant(ctrl, q); }
    EventList events() const { return track->events(*pool); }
};

// The song's sequencer and transport state.
class Sequencer {
public:
    // `seq+0`: the position in ticks.
    double pos;
    // `seq+8`: the tick of the last block that did work.
    int32_t last_tick;
    // `seq+0x10`: the song-mode tick saved when pattern mode was entered.
    int32_t saved_tick;
    // The tracks of devices 0..8 (`sub_1001EEF0(i, i, n)` in the song ctor).
    Track tracks[TRACKS];
    // The nodes of the tracks' lists (`dword_10100230`).
    Pool pool;
    // `song+0x158`: song mode, play, record, loop, tempo, loop points, shuffle, sound model.
    Globals globals;
    // Not in the original: counts the changes of the tracks' event lists after the song ctor
    // (a msg 37 batch, a merge, a stop's prune).
    uint64_t revision;
    // Not in the original: the merges that moved recorded events into the tracks.
    uint64_t merges;

    // The ctors' values: `sub_1001EC50` (position, ticks 0) and the global struct's
    // `sub_10018AC0` (tempo 120000, loop end 768, shuffle 64, byte 22 = 1, the rest 0).
    Sequencer();

    // The tick of the position (`sub_1001F870`).
    int32_t tick_pos() const { return trunc_pos(pos); }
    // Track i's list.
    EventList events(uint32_t i) const { return tracks[i].events(pool); }
    // Track i's recording list.
    EventList recorded(uint32_t i) const { return tracks[i].recorded(pool); }
    // Track i with the pool, to insert into.
    TrackMut track_mut(uint32_t i) { return TrackMut(tracks[i], pool); }

    // `sub_1001EF20`, the clock, once per block before the devices render. Returns whether it
    // reached the step loop (whose tempo argument it stores in a local at 0x1001F3B3).
    bool tick(Devices& devices);
    // `sub_1001F984`, locate (song msg 35 `{8,p}`, msg 32).
    void locate(int32_t p, Devices& devices);
    // `sub_100218A0`: when playing, clear the play flag and stop every device (slot 4); prune
    // every track; in song mode chase every track to the current tick.
    void halt(Devices& devices);
    // Song msg 35 `{1,0}`, `sub_1001E100`: halt, then locate to the start of the current bar.
    void stop(Devices& devices);
    // Song msg 35 `{1,1}`, `sub_1001BE74` -> 0x1001F7E4: when stopped, set the play flag and
    // start every device (slot 3).
    void play(Devices& devices);
    // Song msg 35 `{0,m}`, `sub_1001FBE0`.
    void set_song_mode(uint8_t mode, Devices& devices);
    // Song msg 35 `{2,v}`, `sub_10021194`.
    void set_record(uint8_t v, Devices& devices);
    // The nodes this song's pool has free.
    uint32_t free_nodes() const { return pool.free(); }
    // The record hook `sub_10012BC0` -> `sub_10021340`: only in song mode with record on
    // (0x10021352, 0x10021367), device d's track records ctrl = value at the current tick.
    void record_hook(Device d, uint8_t ctrl, uint8_t value);
    // Song msg 35 `{3,v}` (`sub_10021110`).
    void set_loop_on(uint8_t v) { globals.loop_on = v; }
    // Song msg 35 `{4,v}` (`sub_1001FCD0`), BPM*1000.
    void set_tempo(uint32_t v) { globals.tempo = v; }
    // Song msg 35 `{12,v}` (`sub_10021130`).
    void set_loop_start(uint32_t v) { globals.loop_start = v; }
    // Song msg 35 `{16,v}` (`sub_10021150`).
    void set_loop_end(uint32_t v) { globals.loop_end = v; }
    // Song msg 35 `{20,v}` (`sub_10021170`).
    void set_shuffle(uint8_t v) { globals.shuffle = v; }
    // Song msg 37 (`sub_10021010`): a batch of more events than the pool has free nodes is
    // refused whole (0x1002105B); else each event is inserted into its track from the pool.
    // Returns whether the batch was taken.
    bool add_events(uint32_t track, const TrackEvent* events, uint32_t n);
    // Not in the original: track i's list becomes the n external nodes at `nodes` (kept, not
    // copied; Pool), in place of what it held: the list msg-37 batches would make from it. The
    // applied node keeps its place among the new nodes, as the batches' inserts keep it.
    void adopt(uint32_t i, const Event* nodes, uint32_t n);

private:
    bool song_mode() const { return globals.song_mode != 0; }
    bool playing() const { return globals.play != 0; }
    void chase_all(int32_t p, Devices& devices);
    void advance_all(int32_t t, Devices& devices);
    void song_end(Devices& devices);
    void merge_all(int32_t t);

    // The merge's new nodes.
    MergeBuf buf_;
};

}  // namespace orb
