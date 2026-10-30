// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/seq.rs (see seq.h).
#include "seq.h"

namespace orb {

namespace {

// Ticks per bar: stop relocates to `768 * (tick / 768)` (`sub_1001E100`).
const int32_t BAR = 768;
// The shuffle period: one 8th note (`mov edx, 60h` at 0x1001F3E8).
const int32_t SHUFFLE_PERIOD = 96;
// The shuffle amount's divisor (`mov ecx, 6` at 0x1001F3D7).
const int32_t SHUFFLE_DIV = 6;

inline int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
inline int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

// Rust `partition_point(|e| t > e.tick)` on a list sorted by tick.
uint32_t lower_bound(const Event* l, uint32_t n, int32_t t)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (t > l[mid].tick) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Rust `partition_point(|e| e.tick <= p)`.
uint32_t upper_bound(const Event* l, uint32_t n, int32_t p)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (l[mid].tick <= p) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// The index of the node of tick t and controller ctrl in l, or n when there is none.
uint32_t of_tick(const Event* l, uint32_t n, int32_t t, uint8_t ctrl)
{
    for (uint32_t j = lower_bound(l, n, t); j < n && l[j].tick == t; j++)
        if (l[j].ctrl == ctrl) return j;
    return n;
}

// The quantisation unit of ctrl: `24*q` ticks. A controller above 127 reads past the 128-byte
// array in the DLL; no message the panel sends records one, and 1 stands in. A null quant is
// the recording list's (1 for every controller).
int32_t quant_of(const uint8_t* quant, uint8_t ctrl)
{
    return 24 * (int32_t)(quant && ctrl < 128 ? quant[ctrl] : 1);
}

// `sub_100146E4`: up to a multiple of unit (`lea ebx, [edx+eax*8-1]; cdq; idiv; imul`).
int32_t round_up(int32_t tick, int32_t unit) { return wsub(wadd(tick, unit), 1) / unit * unit; }

// `sub_10014720`: down to a multiple of unit (`cdq; idiv; imul`, truncating).
int32_t round_down(int32_t tick, int32_t unit) { return tick / unit * unit; }

}  // namespace

void Globals::payload(uint8_t b[PAYLOAD_LEN]) const
{
    const uint32_t w[4] = {tempo, locate, loop_start, loop_end};
    b[0] = song_mode;
    b[1] = play;
    b[2] = byte2;
    b[3] = loop_on;
    for (int i = 0; i < 4; i++) {
        b[4 + 4 * i] = (uint8_t)(w[i] >> 24);
        b[5 + 4 * i] = (uint8_t)(w[i] >> 16);
        b[6 + 4 * i] = (uint8_t)(w[i] >> 8);
        b[7 + 4 * i] = (uint8_t)w[i];
    }
    b[20] = shuffle;
    b[21] = vintage;
    b[22] = byte22;
}

int32_t trunc_pos(double pos)
{
    int64_t v;
    if (pos != pos) v = 0;
    else if (pos <= F64(0xC3E0000000000000ull)) v = INT64_MIN;  // -2^63
    else if (pos >= F64(0x43E0000000000000ull)) v = INT64_MAX;  // 2^63
    else v = (int64_t)pos;
    return (int32_t)(uint32_t)(uint64_t)v;
}

int32_t shuffled(int32_t tick, uint8_t shuffle)
{
    int32_t s = (int32_t)shuffle / SHUFFLE_DIV;
    int32_t r = tick % SHUFFLE_PERIOD - s;
    if (r < 0) r = 0;
    return SHUFFLE_PERIOD * (tick / SHUFFLE_PERIOD) + r;
}

// ---- Pool

Pool::Pool() : len_(0), copies_(0), copy_failures_(0)
{
    for (int k = 0; k < LISTS; k++) ends_[k] = 0;
    for (int k = 0; k < TRACKS; k++) {
        ext_[k] = 0;
        ext_len_[k] = 0;
    }
}

EventList Pool::list(int k) const
{
    EventList l;
    if (external(k)) {
        l.data = ext_[k];
        l.len = ext_len_[k];
        return l;
    }
    l.data = nodes_ + start(k);
    l.len = ends_[k] - start(k);
    return l;
}

void Pool::adopt(int k, const Event* ext, uint32_t n)
{
    ext_[k] = 0;
    clear(k);
    if (n == 0) return;
    ext_[k] = ext;
    ext_len_[k] = n;
}

bool Pool::own(int k)
{
    if (!external(k)) return true;
    uint32_t n = ext_len_[k], s = start(k);  // the list's part of the buffer is empty
    if (n > POOL_NODES - len_) {
        copy_failures_++;
        return false;
    }
    memmove(nodes_ + s + n, nodes_ + s, (len_ - s) * sizeof(Event));
    memcpy(nodes_ + s, ext_[k], n * sizeof(Event));
    len_ += n;
    for (int j = k; j < LISTS; j++) ends_[j] += n;
    ext_[k] = 0;
    ext_len_[k] = 0;
    copies_++;
    return true;
}

bool Pool::insert(int k, uint32_t at, const Event& e)
{
    if (len_ >= POOL_NODES) return false;
    uint32_t g = start(k) + at;
    memmove(nodes_ + g + 1, nodes_ + g, (len_ - g) * sizeof(Event));
    nodes_[g] = e;
    len_++;
    for (int j = k; j < LISTS; j++) ends_[j]++;
    return true;
}

void Pool::remove(int k, uint32_t at)
{
    uint32_t g = start(k) + at;
    memmove(nodes_ + g, nodes_ + g + 1, (len_ - g - 1) * sizeof(Event));
    len_--;
    for (int j = k; j < LISTS; j++) ends_[j]--;
}

void Pool::clear(int k)
{
    if (external(k)) {
        ext_[k] = 0;
        ext_len_[k] = 0;
        return;
    }
    uint32_t s = start(k), e = ends_[k];
    memmove(nodes_ + s, nodes_ + e, (len_ - e) * sizeof(Event));
    len_ -= e - s;
    for (int j = k; j < LISTS; j++) ends_[j] -= e - s;
}

void Pool::copy_tracks_from(const Pool& src)
{
    uint32_t n = src.ends_[TRACKS - 1];
    memcpy(nodes_, src.nodes_, n * sizeof(Event));
    len_ = n;
    for (int k = 0; k < TRACKS; k++) {
        ends_[k] = src.ends_[k];
        ext_[k] = src.ext_[k];
        ext_len_[k] = src.ext_len_[k];
    }
    for (int k = TRACKS; k < LISTS; k++) ends_[k] = n;
}

// ---- Track

Track::Track(Device d) : device(d), list_((uint8_t)d), next_(0)
{
    for (int c = 0; c < 128; c++) {
        hold[c] = false;
        quant_[c] = 1;
    }
}

// `sub_10014750` on list k of the pool, of quantisation quant: the tick rounded up to a multiple
// of `24*q` (`sub_100146E4`), dropped above LAST_EVENT_TICK; a node of the same tick and
// controller takes the value, else a new node (pooled: when the pool has more than one free,
// `sub_10014434`) goes before the first node whose tick is not below it.
Track::Inserted Track::insert_into(Pool& pool, int k, const uint8_t* quant, int32_t tick, uint8_t ctrl,
                                   uint8_t value, bool pooled, uint32_t* at_out)
{
    int32_t t = round_up(tick, quant_of(quant, ctrl));
    if (t > LAST_EVENT_TICK) return INS_DROPPED;
    EventList list = pool.list(k);
    uint32_t at = lower_bound(list.data, list.len, t);
    for (uint32_t j = at; j < list.len && list[j].tick == t; j++) {
        if (list[j].ctrl == ctrl) {
            if (list[j].value != value) {
                if (!pool.own(k)) return INS_DROPPED;
                pool.nodes_[pool.start(k) + j].value = value;
            }
            return INS_REPLACED;
        }
    }
    if (!pool.own(k)) return INS_DROPPED;
    // 0x1001443A: cmp [pool+4], 1; jle (no node)
    if (pooled && pool.free() <= 1) return INS_DROPPED;
    Event e;
    e.tick = t;
    e.ctrl = ctrl;
    e.value = value;
    // only an unchecked insert finds the buffer full (seq.h)
    if (!pool.insert(k, at, e)) return INS_DROPPED;
    *at_out = at;
    return INS_AT;
}

// `sub_100146E4`: the tick rounded up to a multiple of `24*q`.
int32_t Track::quantise(int32_t tick, uint8_t ctrl) const { return round_up(tick, quant_of(quant_, ctrl)); }

bool Track::insert(Pool& pool, int32_t tick, uint8_t ctrl, uint8_t value)
{
    return insert_to(pool, tick, ctrl, value, false);
}

bool Track::insert_to(Pool& pool, int32_t tick, uint8_t ctrl, uint8_t value, bool pooled)
{
    uint32_t at = 0;
    switch (insert_into(pool, list_, quant_, tick, ctrl, value, pooled, &at)) {
    case INS_AT:
        // a node inserted before the applied one moves it; one right after it becomes its next
        if (at < next_) next_++;
        return true;
    case INS_REPLACED:
        return true;
    default:
        return false;
    }
}

// For a controller below 128: the hold byte is set; the tick of the position is rounded to the
// controller's quantisation of the first list, up while playing (`sub_100146E0` ->
// `sub_100146E4`) and down when stopped (`sub_10014720`); with the loop on, a tick equal to the
// loop end becomes the loop start; then it is inserted into the recording list (`sub_10014750`
// on `+0x198`, whose own quantisation is 1).
void Track::record(Pool& pool, int32_t now, bool playing, bool wrap, int32_t wrap_start, int32_t wrap_end,
                   uint8_t ctrl, uint8_t value)
{
    hold[ctrl & 127] = true;
    int32_t unit = quant_of(quant_, ctrl);
    int32_t t = playing ? round_up(now, unit) : round_down(now, unit);
    // 0x100214B9: loop on and t == loop end (+0x10) -> loop start (+0xC)
    if (wrap && t == wrap_end) t = wrap_start;
    uint32_t at;
    insert_into(pool, rec(), 0, t, ctrl, value, true, &at);
}

// - the first list is walked while its tick is at most t; every recorded node whose tick is at
//   most the walked node's marks its controller; the walked node's value is the controller's
//   value so far, and a walked node of a marked controller is removed (`sub_10014B30`) unless
//   its tick is 0; the recorded nodes up to t mark theirs too (0x1001F649);
// - every marked controller c, ascending, gets its value so far back at `round_up(t + 1)` of its
//   quantisation unless the track has a node of c there (`sub_10014840`);
// - every recorded node up to t is inserted into the first list;
// - the whole recording list is freed (`sub_100149A0`, OB-18) and the hold mask cleared
//   (0x1001F767).
// The applied pointer `+0x228` dangles in the DLL when its node is removed (OB-19); here it
// moves to the node before. As in the Rust (task-084), the new nodes collect in buf and move
// into the list in one pass (flush), with sub_10014750's rules applied by add.
void Track::merge(Pool& pool, MergeBuf& buf, int32_t t)
{
    int ev = list_, rec = this->rec();
    if (pool.list(rec).empty()) {
        // nothing marked, removed or inserted
        for (int c = 0; c < 128; c++) hold[c] = false;
        return;
    }
    if (!pool.own(ev)) {
        // an external list without room for its copy: the recording is dropped
        pool.clear(rec);
        for (int c = 0; c < 128; c++) hold[c] = false;
        return;
    }
    bool marked[128];
    uint8_t value[128];
    for (int c = 0; c < 128; c++) {
        marked[c] = false;
        value[c] = 0xFF;
    }
    Event* nodes = pool.nodes_;
    uint32_t s = pool.start(ev), end = pool.ends_[ev];
    uint32_t rs = pool.start(rec), rn = pool.list(rec).len;
    // the applied node's index before the walk: a removed node before it moves it back
    uint32_t applied = s + next_;
    uint32_t r = 0;
    uint32_t i = s, w = s;
    while (i < end && t >= nodes[i].tick) {
        Event m = nodes[i];
        while (r < rn && nodes[rs + r].tick <= m.tick) {
            marked[nodes[rs + r].ctrl & 127] = true;
            r++;
        }
        int c = m.ctrl & 127;
        value[c] = m.value;
        if (marked[c] && m.tick != 0) {
            // the node goes back to the pool
            if (i < applied) next_--;
        } else {
            nodes[w] = m;
            w++;
        }
        i++;
    }
    if (w < i) {
        memmove(nodes + w, nodes + i, (pool.len_ - i) * sizeof(Event));
        pool.len_ -= i - w;
        for (int k = ev; k < LISTS; k++) pool.ends_[k] -= i - w;
    }
    EventList recorded = pool.list(rec);
    while (r < recorded.len && t >= recorded[r].tick) {
        marked[recorded[r].ctrl & 127] = true;
        r++;
    }
    buf.len = 0;
    // 0x1001F675: the return to the automation, controllers ascending
    for (int c = 0; c < 128; c++) {
        if (!marked[c]) continue;
        int32_t back = quantise(wadd(t, 1), (uint8_t)c);
        if (find(pool, buf, back, (uint8_t)c).kind == Found::NONE) add(pool, buf, back, (uint8_t)c, value[c]);
    }
    // 0x1001F722: the recorded nodes up to t, in list order; their nodes stay taken until the
    // whole recording list is freed after them
    for (uint32_t k = 0; k < r; k++) {
        Event e = pool.list(rec)[k];
        add(pool, buf, e.tick, e.ctrl, e.value);
    }
    flush(pool, buf);
    pool.clear(rec);
    for (int c = 0; c < 128; c++) hold[c] = false;
}

// The node of tick t and controller ctrl in the list as the merge has it so far: the new nodes
// in buf first (of one tick they go before the list's), then the list's.
Track::Found Track::find(const Pool& pool, const MergeBuf& buf, int32_t t, uint8_t ctrl) const
{
    Found f;
    uint32_t j = of_tick(buf.e, buf.len, t, ctrl);
    if (j < buf.len) {
        f.kind = Found::PENDING;
        f.at = j;
        return f;
    }
    EventList l = pool.list(list_);
    j = of_tick(l.data, l.len, t, ctrl);
    if (j < l.len) {
        f.kind = Found::NODE;
        f.at = pool.start(list_) + j;
        return f;
    }
    f.kind = Found::NONE;
    f.at = 0;
    return f;
}

// insert_pooled during a merge: a new node goes into buf (before the nodes of its tick, as
// `sub_10014750` puts it before the list's), its pool node counted as taken; a full buf is
// flushed first.
void Track::add(Pool& pool, MergeBuf& buf, int32_t tick, uint8_t ctrl, uint8_t value)
{
    int32_t t = round_up(tick, quant_of(quant_, ctrl));
    if (t > LAST_EVENT_TICK) return;
    Found f = find(pool, buf, t, ctrl);
    if (f.kind == Found::PENDING) {
        buf.e[f.at].value = value;
    } else if (f.kind == Found::NODE) {
        pool.nodes_[f.at].value = value;
    } else {
        // 0x1001443A: cmp [pool+4], 1; jle (no node), the held nodes taken
        uint32_t taken = pool.len_ + buf.len;
        if ((taken < POOL_NODES ? POOL_NODES - taken : 0) <= 1) return;
        if (buf.len == MERGE_CHUNK) flush(pool, buf);
        uint32_t at = lower_bound(buf.e, buf.len, t);
        memmove(buf.e + at + 1, buf.e + at, (buf.len - at) * sizeof(Event));
        buf.e[at].tick = t;
        buf.e[at].ctrl = ctrl;
        buf.e[at].value = value;
        buf.len++;
    }
}

// Moves the new nodes of buf into the list: the pool's nodes after the list move once, then the
// list and buf merge from the back, a new node before the list's nodes of its tick. The applied
// node keeps its place: the new nodes of a tick up to its tick go before it.
void Track::flush(Pool& pool, MergeBuf& buf)
{
    uint32_t n = buf.len;
    if (n == 0) return;
    Event* nodes = pool.nodes_;
    uint32_t s = pool.start(list_), e = pool.ends_[list_], used = pool.len_;
    if (next_ > 0) {
        int32_t p = nodes[s + next_ - 1].tick;
        next_ += upper_bound(buf.e, buf.len, p);
    }
    // within the capacity: every new node found more than one free
    memmove(nodes + e + n, nodes + e, (used - e) * sizeof(Event));
    pool.len_ = used + n;
    uint32_t i = e, j = n, w = e + n;
    while (j > 0) {
        w--;
        if (i > s && nodes[i - 1].tick >= buf.e[j - 1].tick) {
            i--;
            nodes[w] = nodes[i];
        } else {
            j--;
            nodes[w] = buf.e[j];
        }
    }
    for (int k = list_; k < LISTS; k++) pool.ends_[k] += n;
    buf.len = 0;
}

void Track::chase(const Pool& pool, int32_t p, uint8_t state[128])
{
    EventList events = this->events(pool);
    for (int c = 0; c < 128; c++) state[c] = 0xFF;
    uint32_t i = 0;
    while (i < events.len && p >= events[i].tick) {
        Event e = events[i];
        state[e.ctrl & 127] = e.value;
        i++;
    }
    next_ = i;
}

void Track::prune(Pool& pool)
{
    uint8_t last[128];
    for (int c = 0; c < 128; c++) last[c] = 0xFF;
    uint32_t i = 0;
    if (pool.external(list_)) {
        // copied only when a node goes; without room the list stays as it is
        EventList l = pool.list(list_);
        for (; i < l.len; i++) {
            Event e = l[i];
            if (e.value != last[e.ctrl & 127]) last[e.ctrl & 127] = e.value;
            else if (e.tick != 0) break;
        }
        if (i == l.len || !pool.own(list_)) return;
    }
    while (i < pool.list(list_).len) {
        Event e = pool.list(list_)[i];
        int c = e.ctrl & 127;
        if (e.value != last[c]) {
            last[c] = e.value;
            i++;
        } else if (e.tick != 0) {
            pool.remove(list_, i);
            // the DLL frees the node; the applied pointer then moves back to its predecessor
            if (i < next_) next_--;
        } else {
            i++;
        }
    }
}

// ---- Sequencer

Sequencer::Sequencer() : pos(0.0), last_tick(0), saved_tick(0), revision(0), merges(0)
{
    for (int i = 0; i < TRACKS; i++) tracks[i] = Track((Device)i);
    globals.song_mode = 0;
    globals.play = 0;
    globals.byte2 = 0;
    globals.loop_on = 0;
    globals.tempo = 120000;
    globals.locate = 0;
    globals.loop_start = 0;
    globals.loop_end = 768;
    globals.shuffle = 64;
    globals.vintage = 0;
    globals.byte22 = 1;
    buf_.len = 0;
}

// Chases every track to p and applies each controller that has a value, ascending, to its
// device (the loop at 0x1001FA2C; `sub_10012B40` finds the device by id).
void Sequencer::chase_all(int32_t p, Devices& devices)
{
    for (int k = 0; k < TRACKS; k++) {
        Track& tr = tracks[k];
        uint8_t state[128];
        tr.chase(pool, p, state);
        SongDevice& d = devices.device(tr.device);
        for (int c = 0; c < 128; c++)
            if (state[c] != 0xFF) d.set_param((uint8_t)c, state[c]);
    }
}

// The apply loop over every track, in track order.
void Sequencer::advance_all(int32_t t, Devices& devices)
{
    for (int k = 0; k < TRACKS; k++) {
        Track& tr = tracks[k];
        SongDevice& d = devices.device(tr.device);
        tr.advance(pool, t, [&d](const Event& e) { d.set_param(e.ctrl, e.value); });
    }
}

// if !play: return
// inc = tempo * TEMPO_STEP; pos = pos + inc; t = trunc(pos)
// if t > lastTick:
//     if song mode:
//         if loop on and loopEnd <= pos and !(loopEnd <= pos - inc):
//             apply events up to loopEnd - 1
//             pos = pos + (loopStart - loopEnd); t = trunc(pos)
//             chase to loopStart, relocate every device to loopStart
//         apply events up to t
//         if pos > 767231: song end (stop, locate 0), return
//     every device: step(t, shuffled(t), tempo)
// lastTick = t
// The tempo of the position step is the current song's (`sub_1001BCC0` without MIDI sync).
bool Sequencer::tick(Devices& devices)
{
    if (!playing()) return false;
    int32_t tempo = (int32_t)globals.tempo;
    // 0x1001EF51: fild tempo; fmul; fld pos; fadd st, st(1)
    double inc = (double)tempo * tempo_step();
    pos += inc;
    int32_t t = trunc_pos(pos);
    if (t > last_tick) {
        if (song_mode()) {
            int32_t le = (int32_t)globals.loop_end;
            // 0x1001EFD6: loopEnd <= pos; 0x1001EFF3: loopEnd > pos - inc
            if (globals.loop_on != 0 && (double)le <= pos && (double)le > pos - inc) {
                advance_all(wsub(le, 1), devices);
                // 0x1001F0C7: with record on, every track's recording is merged up to
                // loopEnd - 1 (`sub_1001F520`, 0x1001F0FD)
                if (globals.byte2 != 0) merge_all(wsub(le, 1));
                int32_t ls = (int32_t)globals.loop_start;
                // 0x1001F135: fld pos; fild (start - end); faddp
                pos += (double)wsub(ls, le);
                t = trunc_pos(pos);
                // 0x1001F182 tests song mode again, which holds here
                chase_all(ls, devices);
                int32_t now = t;
                devices.for_each([ls, now](SongDevice& d) { d.relocate(ls, now); });
            }
            advance_all(t, devices);
            // 0x1001F35F: pos > 767231
            if (pos > song_end_pos()) {
                song_end(devices);
                return false;
            }
        }
        int32_t sp = shuffled(t, globals.shuffle);
        int32_t tempo2 = (int32_t)globals.tempo;
        int32_t now = t;
        devices.for_each([now, sp, tempo2](SongDevice& d) { d.step(now, sp, tempo2); });
        last_tick = t;
        return true;
    }
    last_tick = t;
    return false;
}

// 0x1001F484: record off (`sub_10021194(0)`, with its merge and chase), the host is told (msg 35
// `{2,0}` and `{1,0}`), stop (`sub_100218A0`, without the bar locate) and locate to 0.
void Sequencer::song_end(Devices& devices)
{
    set_record(0, devices);
    halt(devices);
    locate(0, devices);
}

// A negative tick becomes 0; in song mode every track is chased to it (with record on, stopped
// and a later tick, the events up to it are applied instead); then the position and the last
// tick become it and every device is relocated (slot 2).
void Sequencer::locate(int32_t p, Devices& devices)
{
    if (p < 0) p = 0;
    if (globals.byte2 != 0 && !playing() && p > tick_pos()) {
        advance_all(p, devices);
    } else if (song_mode()) {
        chase_all(p, devices);
    }
    pos = (double)p;
    devices.for_each([p](SongDevice& d) { d.relocate(p, p); });
    last_tick = p;
}

void Sequencer::halt(Devices& devices)
{
    if (playing()) {
        globals.play = 0;
        devices.for_each([](SongDevice& d) { d.stop(); });
    }
    for (int k = 0; k < TRACKS; k++) tracks[k].prune(pool);
    revision++;
    if (song_mode()) {
        int32_t t = tick_pos();
        chase_all(t, devices);
    }
}

void Sequencer::stop(Devices& devices)
{
    halt(devices);
    int32_t t = tick_pos();
    locate(BAR * (t / BAR), devices);
}

// The position is not touched.
void Sequencer::play(Devices& devices)
{
    if (!playing()) {
        globals.play = 1;
        int32_t now = tick_pos();
        devices.for_each([now](SongDevice& d) { d.start(now); });
    }
}

// On a change, store the byte and tell every device (slot 9, `sub_10013FB0`); then song mode
// locates to the saved tick, pattern mode relocates every device to 0 and saves the current tick.
void Sequencer::set_song_mode(uint8_t mode, Devices& devices)
{
    if (globals.song_mode != mode) {
        globals.song_mode = mode;
        bool on = mode != 0;
        devices.for_each([on](SongDevice& d) { d.set_song_mode(on); });
    }
    if (mode != 0) {
        locate(saved_tick, devices);
    } else {
        int32_t now = tick_pos();
        devices.for_each([now](SongDevice& d) { d.relocate(0, now); });
        saved_tick = tick_pos();
    }
}

// Turning record off (v == 0) while it is on merges every track's recording up to the current
// tick (`sub_1001F520`, 0x10021206) and, in song mode, chases every track to that tick
// (0x10021294); then the byte is stored as it is.
void Sequencer::set_record(uint8_t v, Devices& devices)
{
    if (globals.byte2 != 0 && v == 0) {
        int32_t t = tick_pos();
        merge_all(t);
        if (song_mode()) chase_all(t, devices);
    }
    globals.byte2 = v;
}

// `sub_1001F520` on every track, in track order.
void Sequencer::merge_all(int32_t t)
{
    for (int i = 0; i < TRACKS; i++) {
        if (!recorded((uint32_t)i).empty()) {
            merges++;
            break;
        }
    }
    for (int k = 0; k < TRACKS; k++) tracks[k].merge(pool, buf_, t);
    revision++;
}

// Not modelled: the warning the DLL sends the host when its pool has 99, 50 or fewer than 10
// nodes free (top-level msg 79, `sub_1001D440`, 0x10021378), and a controller above 127.
void Sequencer::record_hook(Device d, uint8_t ctrl, uint8_t value)
{
    if (!song_mode() || globals.byte2 == 0 || ctrl > 127) return;
    int32_t now = tick_pos();
    bool is_playing = playing();
    bool wrap = globals.loop_on != 0;
    int32_t ws = (int32_t)globals.loop_start, we = (int32_t)globals.loop_end;
    // 0x100213BF: the track whose device id (+0x104) is d's
    for (int k = 0; k < TRACKS; k++) {
        if (tracks[k].device == d) {
            tracks[k].record(pool, now, is_playing, wrap, ws, we, ctrl, value);
            return;
        }
    }
}

// The batch's count (`last - first` at 0x10021042 against `sub_1000DEE0`, `cmp ebx, eax; ja` at
// 0x1002105B, unsigned); a refused batch inserts nothing and the reply is `['K', 22]`
// (`sub_1001DE00`, 0x100210ED) instead of `['I']` (`sub_1001DF80`, 0x100210CA). A taken batch
// can still lose its last event: with as many events as free nodes, the last insert finds one
// node free and stores nothing (`sub_10014434`).
bool Sequencer::add_events(uint32_t track, const TrackEvent* events, uint32_t n)
{
    if (n > pool.free()) return false;
    Track& tr = tracks[track];
    for (uint32_t i = 0; i < n; i++) tr.insert_pooled(pool, (int32_t)events[i].tick, events[i].ctrl, events[i].value);
    revision++;
    return true;
}

// An insert never removes a node and keeps the node of a tick and controller unique, so the
// applied node is the new list's node of its tick and controller.
void Sequencer::adopt(uint32_t i, const Event* nodes, uint32_t n)
{
    Track& tr = tracks[i];
    uint32_t next = 0;
    if (tr.next_ > 0) {
        Event a = tr.events(pool)[tr.next_ - 1];
        uint32_t j = of_tick(nodes, n, a.tick, a.ctrl);
        next = j < n ? j + 1 : upper_bound(nodes, n, a.tick);
    }
    pool.adopt(tr.list_, nodes, n);
    tr.next_ = next;
    revision++;
}

}  // namespace orb
