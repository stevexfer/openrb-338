// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/frame/mod.rs (see frame.h). Every computation follows the
// crate's numerics rules: `real` arithmetic in the original's operand order, an f32 rounding
// (st) at every `fstp dword`/`fst dword`, no FMA.
#include "frame.h"

namespace orb {

namespace {

// The constants of the mixer block, update and conversion, decoded from their bytes.
// `dbl_10151F48`: the smoothing's "keep" factor, 0.93.
const real KEEP = R64(0x3FEDC28F5C28F5C3ull);
// `dbl_10151F50`: the smoothing's "take" factor, 0.07.
const real TAKE = R64(0x3FB1EB851EB851ECull);
// `dbl_10151F58`: 1/32, the per-pair ramp step factor.
const real RAMP = R64(0x3FA0000000000000ull);
// `dbl_10151F70`: the int16 scale.
const real SCALE = (real)32768;
// `flt_10151F78`, `flt_10151F7C`: the int16 clamp, 32767 and -32768.
const float CLAMP_HI = F32(0x46FFFE00u);
const float CLAMP_LO = F32(0xC7000000u);
// `dbl_10151F20`: the Vintage send's factor.
const real VINTAGE_SEND = R64(0x3FE0000000000000ull);  // 0.5
// `dbl_10151F28`: 1e-6 as an f32 widened to f64.
const real VINTAGE_OFFSET = R64(0x3EB0C6F7A0000000ull);
// `flt_10151F34`: the meters' full scale.
const float METER_FULL = 1.0f;
// `flt_10151F38`: the meters' decay per update, 0.92.
const float METER_DECAY = F32(0x3F6B851Fu);
// `flt_10151F3C`: the meter byte scale.
const float METER_SCALE = 127.0f;
// `flt_10151F40`: the master gain factor.
const float MASTER_SCALE = 2.0f;
// The meter index step per block (`add eax, 17h` at 0x10013481).
const uint16_t METER_STEP = 23;

// `cmp dword, 80000000h; jbe` then `fchs`: the sign cleared for every negative value.
inline float abs_bits(float x) { return F32(bits(x) & 0x7FFFFFFFu); }

// The meter byte: 0x7F above full scale, else `trunc(x * 127)` (the low byte stored).
inline uint8_t meter_byte(real x, bool full) { return full ? 0x7F : (uint8_t)as_i64(x * (real)METER_SCALE); }

inline uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

#ifdef ORB_DELAY_F16
uint16_t delay_cells[DELAY_LEN];
#else
float delay_cells[DELAY_LEN];
#endif
ProcessState the_process;

}  // namespace

ProcessState& process_globals()
{
    the_process.delay.buf = delay_cells;
    return the_process;
}

#ifdef ORB_TEST_SOLO
int test_solo = -1;
#endif

// ---- the mixer device

MixerParams MixerParams::defaults()
{
    MixerParams p;
    p.master = 100;
    p.comp_id = 0;
    p.pcf_id = 0;
    for (int c = 0; c < 4; c++) {
        ChannelParams& ch = p.channels[c];
        ch.enabled = 1;
        ch.level = 100;
        ch.pan = 64;
        ch.send = 0;
        ch.dist = 0;
    }
    return p;
}

MixerParams MixerParams::from_payload(const uint8_t* b)
{
    MixerParams p = defaults();
    p.master = b[0];
    p.comp_id = b[1];
    p.pcf_id = b[2];
    for (int c = 0; c < 4; c++) {
        const uint8_t* o = b + 5 + 6 * c;
        ChannelParams& ch = p.channels[c];
        ch.enabled = o[0];
        ch.level = o[1];
        ch.pan = o[2];
        ch.send = o[3];
        ch.dist = o[4];
    }
    return p;
}

MixerDevice::MixerDevice() : live_params(MixerParams::defaults()), pattern(MixerParams::defaults()) {}

// Slot 9, 0x10016280: entering song mode saves the live set into the pattern copy; leaving it
// carries the live master and channel enables into the pattern copy, then restores the live set.
void MixerDevice::set_song_mode(bool on)
{
    if (on) {
        pattern = live_params;
    } else {
        pattern.master = live_params.master;
        for (int c = 0; c < 4; c++) pattern.channels[c].enabled = live_params.channels[c].enabled;
        live_params = pattern;
    }
}

// Msg 143, 0x1001588F: the block goes into the pattern copy; then song mode copies only the
// master and the channel enables live, pattern mode all of it.
void MixerDevice::data(const uint8_t* payload, uint32_t len, bool song_mode)
{
    if (len < MixerParams::PAYLOAD_LEN) return;
    pattern = MixerParams::from_payload(payload);
    if (song_mode) {
        live_params.master = pattern.master;
        for (int c = 0; c < 4; c++) live_params.channels[c].enabled = pattern.channels[c].enabled;
    } else {
        live_params = pattern;
    }
}

// Slot 8, 0x100160F0, with tick 0 from the song ctor: the pattern copy's compressor id (1) and
// PCF id (2), then each channel's level, pan, send and dist (`6..9 + 6c`).
void MixerDevice::init_track(TrackMut& track)
{
    MixerParams p = pattern;
    track.insert(0, 1, p.comp_id);
    track.insert(0, 2, p.pcf_id);
    for (int c = 0; c < 4; c++) {
        const ChannelParams& ch = p.channels[c];
        uint8_t base = (uint8_t)(5 + 6 * c);
        track.insert(0, (uint8_t)(base + 1), ch.level);
        track.insert(0, (uint8_t)(base + 2), ch.pan);
        track.insert(0, (uint8_t)(base + 3), ch.send);
        track.insert(0, (uint8_t)(base + 4), ch.dist);
    }
}

// Slot 11, 0x10015C00: controller 1 is the compressor id, 2 the PCF id, and `5 + 6c + k` sets
// channel c's field k (1 level, 2 pan, 3 send, 4 dist).
void MixerDevice::set_param(uint8_t ctrl, uint8_t value)
{
    switch (ctrl) {
    case 1: live_params.comp_id = value; break;
    case 2: live_params.pcf_id = value; break;
    default: {
        int32_t x = (int32_t)ctrl - 5;
        uint32_t c = (uint8_t)(x / 6);
        uint8_t k = (uint8_t)(x % 6);
        if (c >= 4) break;
        ChannelParams& ch = live_params.channels[c];
        switch (k) {
        case 1: ch.level = value; break;
        case 2: ch.pan = value; break;
        case 3: ch.send = value; break;
        case 4: ch.dist = value; break;
        default: break;
        }
    }
    }
}

// ---- the frame state

ChannelState::ChannelState()
    : target_l(guard()), target_r(guard()), gain_l(guard()), gain_r(guard()), send(guard()),
      peak(0.0f), meter(0)
{
    routing.dist = routing.pcf = routing.comp = false;
}

MixerState::MixerState()
    : counter(COUNTER_INIT), meter_index(0), master_target(1.0f), master(1.0f), master_comp(false)
{
    master_peak[0] = master_peak[1] = 0.0f;
    master_meter[0] = master_meter[1] = 0;
    fill(send, guard());
}

OutputState::OutputState() : leftover_pos(0)
{
    fill(leftover[0], 0.0f);
    fill(leftover[1], 0.0f);
}

// ---- the frame

Frame::Frame(const Tables* tables, const Devices& d)
    : vintage(false), devices(d), blocks(0), stack_word(OptI32::none()), out_end(OptI32::none()),
      tables_(tables)
{
    fill(bus_, 0.0f);
}

void Frame::mixer_update()
{
    const Tables& tb = *tables_;
    const float* t = tb.level;
    MixerParams p = mixer().live_params;
    BlockContext ctx = {tables_, state.meter_index, seq.globals.tempo, seq.tick_pos(), OptI32::none()};
    for (uint32_t c = 0; c < 4; c++) {
        ChannelState& ch = state.channels[c];
        const ChannelParams& cp = p.channels[c];
        uint32_t lev = cp.enabled != 0 ? cp.level : 0;
        float pg[2];
        tb.pan((int)lev, cp.pan, pg);
        // 0x10012C8C..0x10012CBC: fld 1e-6; fadd PAN[0] -> +0xA8; fadd PAN[1] -> +0xA4
        ch.target_l = st((real)guard() + (real)pg[0]);
        ch.target_r = st((real)guard() + (real)pg[1]);
        real ts = (real)t[cp.send];
        if (!vintage) {
            // 0x10012CCD: T[lev] * T[send] + 1e-6
            ch.send = st((real)t[lev] * ts + (real)guard());
        } else {
            // 0x10012CF2: (PAN[0] + PAN[1]) * T[send] * 0.5 + 1e-6
            ch.send = st(((real)pg[0] + (real)pg[1]) * ts * VINTAGE_SEND + VINTAGE_OFFSET);
        }
        // 0x10012D1A..0x10012D96: the meter byte, then the peak decays
        real m = (real)t[lev] * (real)ch.peak;
        ch.meter = meter_byte(m, m > (real)METER_FULL);
        ch.peak = st((real)ch.peak * (real)METER_DECAY);
        ch.routing.dist = cp.dist != 0;
        ch.routing.pcf = p.pcf_id == c + 1;
        ch.routing.comp = p.comp_id == c + 2;
    }
    // 0x100130A4: slot 16 of every insert, in list order (`song+0x1EC`: dist, PCF, comp)
    devices.dist->update(ctx);
    devices.pcf->update(ctx);
    devices.comp->update(ctx);
    // 0x100130D7: 2 * T[master] + 1e-6
    state.master_target = st((real)MASTER_SCALE * (real)t[p.master] + (real)guard());
    state.master_comp = p.comp_id == 1;
    // 0x10013189..0x100131FD: the master meters (an integer compare of the float bits)
    for (int k = 0; k < 2; k++) {
        float pk = state.master_peak[k];
        if ((int32_t)bits(pk) > (int32_t)bits(1.0f)) {
            state.master_peak[k] = 1.0f;
            state.master_meter[k] = 0x7F;
        } else {
            state.master_meter[k] = meter_byte((real)pk, false);
        }
    }
    for (int k = 0; k < 2; k++) state.master_peak[k] = st((real)state.master_peak[k] * (real)METER_DECAY);
}

bool Frame::block(Block& l, Block& r, Fault* fault)
{
    blocks++;
    if ((state.counter & (UPDATE_PERIOD - 1)) == 0) {
        mixer_update();
        // 0x10012C3A: the compressor id (the dword at the mixer data's +4) into its local at the
        // slot's address
        stack_word = OptI32::of((int32_t)mixer().live_params.comp_id);
    }
    state.counter++;
    // 0x1001F3B3: the tempo (globals +4) the step loop passes, into its local there
    if (seq.tick(devices)) stack_word = OptI32::of((int32_t)seq.globals.tempo);
    uint16_t mi = (uint16_t)((state.meter_index + METER_STEP) & 63);
    state.meter_index = mi;
    fill(l, guard());
    fill(r, guard());
    BlockContext ctx = {tables_, mi, seq.globals.tempo, seq.tick_pos(), OptI32::none()};
    for (int c = 0; c < 4; c++) {
        ChannelState& ch = state.channels[c];
        devices.channels[c]->process(bus_, ctx);
        // the word the audio slot leaves at its esp0 - 0x1C (the 303s leave the slot as it was);
        // the distortion leaves it for the PCF after it
        OptI32 w = devices.channels[c]->stack_word();
        if (w.some) stack_word = w;
        InsertEffect* const ins[3] = {devices.dist, devices.pcf, devices.comp};
        const bool routed[3] = {ch.routing.dist, ch.routing.pcf, ch.routing.comp};
        for (int k = 0; k < 3; k++) {
            if (!routed[k]) continue;
            ctx.stack_word = stack_word;
            ins[k]->process(bus_, ctx);
            if (ins[k]->faulted(fault)) return false;
            stack_word = ins[k]->stack_word(stack_word);
        }
#ifdef ORB_TEST_SOLO
        if (test_solo >= 0 && c != test_solo) fill(bus_, 0.0f);
#endif
        // 0x100135FA: the channel's peak meter
        float v = abs_bits(bus_[mi]);
        if (v > ch.peak) ch.peak = v;
        // 0x10013650: send += bus * sendGain
        real sg = (real)ch.send;
        for (int i = 0; i < BLOCK; i++) state.send[i] = st((real)bus_[i] * sg + (real)state.send[i]);
        // 0x100136B7: smoothing, n = old*0.93 + target*0.07 stored as f32; the ramp steps from
        // the stored old value by (n_unrounded - old)/32 every two samples
        real old_l = (real)ch.gain_l;
        real old_r = (real)ch.gain_r;
        real new_l = old_l * KEEP + (real)ch.target_l * TAKE;
        real new_r = old_r * KEEP + (real)ch.target_r * TAKE;
        ch.gain_l = st(new_l);
        ch.gain_r = st(new_r);
        real dl = (new_l - old_l) * RAMP;
        real dr = (new_r - old_r) * RAMP;
        // 0x10013742: the pan and mix loop
        real gl = old_l, gr = old_r;
        for (int i = 0; i < BLOCK; i += 2) {
            for (int j = i; j < i + 2; j++) {
                real b = (real)bus_[j];
                l[j] = st(b * gl + (real)l[j]);
                r[j] = st(b * gr + (real)r[j]);
            }
            gl += dl;
            gr += dr;
        }
    }
    // 0x10013844: the send effect
    ctx.stack_word = OptI32::none();
    devices.delay->process(state.send, l, r, ctx);
    stack_word = devices.delay->stack_word(out_end, stack_word);
    // 0x10013883: the compressor on the master
    if (state.master_comp) devices.comp->process_stereo(l, r, ctx);
    // 0x100138AE: the master gain, constant over the block, unrounded
    real m = (real)state.master * KEEP + (real)state.master_target * TAKE;
    state.master = st(m);
    for (int i = 0; i < BLOCK; i++) l[i] = st((real)l[i] * m);
    for (int i = 0; i < BLOCK; i++) r[i] = st((real)r[i] * m);
    // 0x10013960: the master peak meters
    Block* const out[2] = {&l, &r};
    for (int k = 0; k < 2; k++) {
        float v = abs_bits((*out[k])[mi]);
        if (v > state.master_peak[k]) state.master_peak[k] = v;
    }
    return true;
}

// `x = f * 32768` in f64, clamped to the f32 constants 32767 and -32768 (a NaN takes the lower
// bound), truncated toward zero (`fistp` under a truncating control word).
int16_t to_i16(float f)
{
    real x = (real)f * SCALE;
    if (x > (real)CLAMP_HI) x = (real)CLAMP_HI;
    else if (x != x || x < (real)CLAMP_LO) x = (real)CLAMP_LO;
    return (int16_t)as_i64(x);
}

// ---- the engine

DeviceSet::DeviceSet(const SampleSet* samples, ProcessState* process, const Tables* tables)
    : tr808(samples, process, tables), tr909(samples, process, tables), delay(process)
{
}

Devices DeviceSet::devices(ProcessState* process)
{
    Devices d;
    d.mixer = &mixer;
    d.channels[0] = &tb303_1;
    d.channels[1] = &tb303_2;
    d.channels[2] = &tr808;
    d.channels[3] = &tr909;
    d.dist = &dist;
    d.pcf = &pcf;
    d.comp = &comp;
    d.delay = &delay;
    d.process = process;
    return d;
}

Engine::Engine(const Tables* tables, const SampleSet* samples, ProcessState* process)
    : set_(samples, process, tables), frame(tables, set_.devices(process)), has_fault_(false)
{
    memset(out_, 0, sizeof out_);
    fault_.first.wave = 0;
    fault_.block = fault_.calls = 0;
    for (int d = 0; d < DEVICES; d++) {
        TrackMut t = frame.seq.track_mut((uint32_t)d);
        frame.devices.device((Device)d).init_track(t);
    }
    frame.seq.set_tempo(140000);
    frame.seq.locate(0, frame.devices);
    frame.mixer_update();
}

// The blob: "ORBS", u32 version 1, u8 form (0 RBS, 1 ReBirth 1.x), u8 block count, u16 0; the
// globals (u8 song_mode, play, byte2, loop_on, u32 tempo, locate, loop_start, loop_end, u8
// shuffle, vintage, byte22, 0); char[32] title; the msg-41 blocks in load order (u8 device, 0,
// u16 length, payload, padded to 4); u32 node count of each of the 9 tracks; the nodes track by
// track (Event).
namespace {

const uint32_t HEAD = 36, TITLE = 32;

struct Parsed {
    Globals g;
    uint8_t form, blocks;
    uint32_t block_at;  // the first block's offset
    uint32_t count[TRACKS];
    const Event* nodes[TRACKS];
};

bool parse(const uint8_t* b, uint32_t len, Parsed& p)
{
    if (len < HEAD + TITLE || memcmp(b, "ORBS", 4) != 0 || rd32(b + 4) != 1 || ((uintptr_t)b & 1)) return false;
    p.form = b[8];
    p.blocks = b[9];
    if (p.form > 1) return false;
    p.g.song_mode = b[12];
    p.g.play = b[13];
    p.g.byte2 = b[14];
    p.g.loop_on = b[15];
    p.g.tempo = rd32(b + 16);
    p.g.locate = rd32(b + 20);
    p.g.loop_start = rd32(b + 24);
    p.g.loop_end = rd32(b + 28);
    p.g.shuffle = b[32];
    p.g.vintage = b[33];
    p.g.byte22 = b[34];
    uint32_t at = HEAD + TITLE;
    p.block_at = at;
    for (uint32_t i = 0; i < p.blocks; i++) {
        if (len - at < 4 || b[at] >= DEVICES) return false;
        uint32_t n = b[at + 2] | b[at + 3] << 8;
        if (len - at - 4 < n) return false;
        at = (at + 4 + n + 3) & ~3u;
        if (at > len) return false;
    }
    if (len - at < 4 * TRACKS) return false;
    uint32_t total = 0;
    for (int t = 0; t < TRACKS; t++) {
        p.count[t] = rd32(b + at + 4 * t);
        if (p.count[t] > (len - at) / sizeof(Event)) return false;
        total += p.count[t];
    }
    at += 4 * TRACKS;
    if (total > (len - at) / sizeof(Event)) return false;
    for (int t = 0; t < TRACKS; t++) {
        p.nodes[t] = (const Event*)(b + at);
        at += p.count[t] * (uint32_t)sizeof(Event);
    }
    return true;
}

}  // namespace

const char* Engine::blob_title(const uint8_t* blob, uint32_t len)
{
    Parsed p;
    if (!parse(blob, len, p) || blob[HEAD + TITLE - 1] != 0) return 0;
    return (const char*)blob + HEAD;
}

bool Engine::load_blob(const uint8_t* blob, uint32_t len)
{
    Parsed p;
    if (!parse(blob, len, p)) return false;
    set_globals(p.g);
    uint32_t at = p.block_at;
    for (uint32_t i = 0; i < p.blocks; i++) {
        uint32_t n = blob[at + 2] | blob[at + 3] << 8;
        device_data((Device)blob[at], blob + at + 4, n);
        at = (at + 4 + n + 3) & ~3u;
    }
    // 909 msg 74 {0, 0} (0x1002BF12): the live enabled byte, then the record hook, which does
    // nothing with record off
    if (p.form == 1) set_param(DEV_TR909, 0, 0);
    for (int t = 0; t < TRACKS; t++) frame.seq.adopt((uint32_t)t, p.nodes[t], p.count[t]);
    relocate();
    return true;
}

// The fields in this order: song mode, play or stop, record, loop on, tempo, locate, loop start,
// loop end, shuffle, then the stores of byte 21 (the sound model) and byte 22.
void Engine::set_globals(const Globals& g)
{
    Sequencer& seq = frame.seq;
    Devices& d = frame.devices;
    seq.set_song_mode(g.song_mode, d);
    if (g.play == 0) seq.stop(d);
    else seq.play(d);
    seq.set_record(g.byte2, d);
    seq.set_loop_on(g.loop_on);
    seq.set_tempo(g.tempo);
    seq.locate((int32_t)g.locate, d);
    seq.set_loop_start(g.loop_start);
    seq.set_loop_end(g.loop_end);
    seq.set_shuffle(g.shuffle);
    seq.globals.vintage = g.vintage;
    seq.globals.byte22 = g.byte22;
    frame.vintage = g.is_vintage();
    bool v = frame.vintage;
    d.for_each([v](SongDevice& dev) { dev.set_sound_model(v); });
}

void Engine::device_data(Device d, const uint8_t* payload, uint32_t len)
{
    bool song_mode = frame.seq.globals.song_mode != 0;
    frame.devices.device(d).data(payload, len, song_mode);
}

void Engine::relocate()
{
    int32_t t = frame.seq.tick_pos();
    frame.seq.locate(t, frame.devices);
}

void Engine::activate(Activation a)
{
    switch (a) {
    // 0x1001BDC7..0x1001BDCC: the current song is playing: no slot 6 and no switch
    case ACTIVATE_REPLACES_PLAYING: break;
    // 0x1001BDD0: the old song's slot 6, of which only the delay's 0x100100E0 reaches this song
    // (the refill of the process-global delay line); this song's delay stands in for it
    case ACTIVATE_REPLACES_STOPPED: frame.devices.delay->silence(); break;
    // 0x1001BDF3: slot 6 on this song's own devices
    case ACTIVATE_NONE_CURRENT: frame.devices.silence_all(); break;
    }
}

void Engine::render(int16_t* buf, uint32_t n)
{
    if (n > MAX_FRAMES) n = MAX_FRAMES;
    // 0x10013A5E: the song's try-lock, before the leftover and the first block
    frame.stack_word = OptI32::of(LOCK_WORD);
    int32_t lp = output.leftover_pos;
    uint32_t pos = 0;
    int32_t rem = (int32_t)n;
    if (lp > 0) {
        // 0x10013AE6: samples L..63 of the leftover block first, refilled with the guard
        for (int c = 0; c < 2; c++) {
            for (int i = lp; i < BLOCK; i++) {
                out_[c][i - lp] = output.leftover[c][i];
                output.leftover[c][i] = guard();
            }
        }
        pos = (uint32_t)(BLOCK - lp);
        rem -= (int32_t)pos;
    }
    while (rem > 0) {
        bool whole = rem > 63;
        Fault f;
        bool done;
        if (whole) {
            frame.out_end = OptI32::of((int32_t)((uint32_t)OUT_RIGHT + 4 * (pos + BLOCK - 1)));
            done = frame.block(*(Block*)(out_[0] + pos), *(Block*)(out_[1] + pos), &f);
        } else {
            // 0x10013BAA: the partial block is rendered whole into the leftover block
            frame.out_end = OptI32::none();
            done = frame.block(output.leftover[0], output.leftover[1], &f);
        }
        if (!done) {
            // the call ends here, before the leftover position is stored (0x10013BBA) and
            // before the int16 conversion, so buf keeps what the caller had in it
            if (!has_fault_) {
                has_fault_ = true;
                fault_.first = f;
                fault_.block = frame.blocks - 1;
                fault_.calls = 0;
            }
            fault_.calls++;
            return;
        }
        if (!whole) break;
        pos += BLOCK;
        rem -= BLOCK;
    }
    output.leftover_pos = rem;
    if (rem > 0) {
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < rem; i++) {
                out_[c][pos + i] = output.leftover[c][i];
                output.leftover[c][i] = guard();
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        buf[2 * i] = to_i16(out_[0][i]);
        buf[2 * i + 1] = to_i16(out_[1][i]);
    }
}

void Engine::stop() { frame.seq.stop(frame.devices); }

void Engine::locate(uint32_t tick) { frame.seq.locate((int32_t)tick, frame.devices); }

void Engine::play() { frame.seq.play(frame.devices); }

void Engine::halt() { frame.seq.halt(frame.devices); }

void Engine::set_song_mode(uint8_t mode) { frame.seq.set_song_mode(mode, frame.devices); }

void Engine::set_param(Device d, uint8_t ctrl, uint8_t value) { frame.devices.device(d).set_param(ctrl, value); }

// The three send buses are `song+0x160C + 0x100k`; the ctor registers only the delay, so buses
// 1 and 2 are never read and are not modelled.
void Engine::reset()
{
    MixerState& s = frame.state;
    s.master = 0.0f;
    s.master_peak[0] = s.master_peak[1] = 0.0f;
    fill(s.send, guard());
    fill(output.leftover[0], 0.0f);
    fill(output.leftover[1], 0.0f);
    frame.devices.reset_all();
}

int Engine::payload_byte(Device d, uint32_t i)
{
    bool live = frame.seq.globals.song_mode == 0;
    switch (d) {
    case DEV_MIXER: {
        // the master and the mutes are never automated: always the live ones
        const MixerParams& m = live ? set_.mixer.live_params : set_.mixer.pattern;
        if (i == 0) return set_.mixer.live_params.master;
        if (i == 1) return m.comp_id;
        if (i == 2) return m.pcf_id;
        if (i < 5 || i >= MixerParams::PAYLOAD_LEN) return -1;
        uint32_t c = (i - 5) / 6, k = (i - 5) % 6;
        const ChannelParams& ch = (k == 0 ? set_.mixer.live_params : m).channels[c];
        switch (k) {
        case 0: return ch.enabled;
        case 1: return ch.level;
        case 2: return ch.pan;
        case 3: return ch.send;
        case 4: return ch.dist;
        default: return -1;
        }
    }
    case DEV_TB303_1:
    case DEV_TB303_2: {
        const Tb303& t = d == DEV_TB303_1 ? set_.tb303_1 : set_.tb303_2;
        if (i <= Tb303::WAVEFORM) return live ? t.live_params[i] : t.params[i];
        if (i < Tb303::PARAMS_LEN) return -1;
        i -= Tb303::PARAMS_LEN;
        return i < sizeof t.patterns ? t.patterns[i] : -1;
    }
    case DEV_TR808: {
        const Tr808& t = set_.tr808;
        if (i <= Tr808::LAST_CTRL) return live ? t.live_params[i] : t.params[i];
        if (i < Tr808::PARAMS) return -1;
        i -= Tr808::PARAMS;
        return i < sizeof t.patterns ? t.patterns[i] : -1;
    }
    case DEV_TR909: {
        const Tr909& t = set_.tr909;
        if (i <= Tr909::LAST_CTRL) return (live ? t.live_params : t.params)[Tr909::internal_index(i)];
        if (i < Tr909::PARAMS) return -1;
        i -= Tr909::PARAMS;
        return i < sizeof t.patterns ? t.patterns[i] : -1;
    }
    case DEV_DELAY: return i < Delay::METER ? (live ? set_.delay.params : set_.delay.pattern)[i] : -1;
    case DEV_DIST: return i < Distortion::METER ? (live ? set_.dist.params : set_.dist.pattern)[i] : -1;
    case DEV_PCF: return i < Pcf::METER ? (live ? set_.pcf.params : set_.pcf.pattern)[i] : -1;
    default: return i < Compressor::GAIN_METER ? (live ? set_.comp.params : set_.comp.pattern)[i] : -1;
    }
}

uint32_t Engine::save_blob(const uint8_t* old, uint32_t old_len, uint32_t off, uint8_t* out, uint32_t n)
{
    static const uint16_t LEN[DEVICES] = {
        MixerParams::PAYLOAD_LEN, Tb303::PAYLOAD_LEN, Tb303::PAYLOAD_LEN,
        Tr808::PARAMS + Tr808::PATTERNS * Tr808::PATTERN_LEN, Tr909::PARAMS + Tr909::PATTERNS * Tr909::PATTERN_LEN,
        Delay::PARAMS, Distortion::PARAMS, Pcf::PARAMS, Compressor::PARAMS,
    };
    Parsed p;
    bool keep = old && parse(old, old_len, p);
    uint32_t blocks = keep ? p.blocks : DEVICES, at = HEAD + TITLE, total;
    if (keep) {
        total = old_len;
    } else {
        total = at + 4 * TRACKS;
        for (int d = 0; d < DEVICES; d++) total += (4u + LEN[d] + 3) & ~3u;
    }
    if (!out) return total;
    if (off >= total) return total;
    if (n > total - off) n = total - off;
    for (uint32_t i = 0; i < n; i++) out[i] = keep ? old[off + i] : 0;
    auto put = [&](uint32_t a, uint32_t v) {
        if (a >= off && a - off < n) out[a - off] = (uint8_t)v;
    };
    auto put32 = [&](uint32_t a, uint32_t v) {
        for (uint32_t k = 0; k < 4; k++) put(a + k, v >> (8 * k));
    };
    const Globals& g = frame.seq.globals;
    if (!keep) {
        for (uint32_t k = 0; k < 4; k++) put(k, "ORBS"[k]);
        put32(4, 1);
        put(9, DEVICES);
        put(15, g.loop_on);
        put32(24, g.loop_start);
        put32(28, g.loop_end);
        put(33, g.vintage);
        put(34, g.byte22);
        for (uint32_t k = 0; k < 8; k++) put(HEAD + k, "NEW SONG"[k]);
    }
    put(12, g.song_mode);
    put(13, 0);
    put32(16, g.tempo);
    put(32, g.shuffle);
    for (uint32_t b = 0; b < blocks; b++) {
        Device d = keep ? (Device)old[at] : (Device)b;
        uint32_t len = keep ? (uint32_t)(old[at + 2] | old[at + 3] << 8) : LEN[b];
        if (!keep) {
            put(at, d);
            put(at + 2, len & 0xFF);
            put(at + 3, len >> 8);
        }
        uint32_t s = at + 4, lo = s > off ? s : off, hi = s + len < off + n ? s + len : off + n;
        for (uint32_t j = lo; j < hi; j++) {
            int v = payload_byte(d, j - s);
            if (v >= 0) out[j - off] = (uint8_t)v;
        }
        at = (at + 4 + len + 3) & ~3u;
    }
    return total;
}

}  // namespace orb
