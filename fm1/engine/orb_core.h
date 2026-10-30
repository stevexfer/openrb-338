// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of OpenRB-338's orb-engine (crates/orb-engine), shared types.
//
// Numerics: `real` is the type of the Rust code's f64 arithmetic. The host's reference build
// (ORB_REAL=double) must match `orb render` sample for sample; the FM-1 build (ORB_REAL=float)
// runs on the single-precision FPU. f32 in Rust is `float` here, and every Rust `x as f32` (the
// original's f32 stores) is `st(x)`. Build with -ffp-contract=off: no fused multiply-add.
#pragma once

#include <stdint.h>
#include <string.h>

#ifndef ORB_REAL
#define ORB_REAL double
#endif

namespace orb {

typedef ORB_REAL real;

// ---- bit-exact constants and conversions

inline float F32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
inline double F64(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
// A Rust `f64::from_bits` constant used in `real` arithmetic (never write a bare double literal
// into DSP code: on the FM-1 it would pull in soft-double).
inline real R64(uint64_t b) { return (real)F64(b); }
inline uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

inline float st(real x) { return (float)x; }

// Rust `as` casts from a float: truncation toward zero, saturating, NaN -> 0.
template <typename T> inline T sat_cast(real x, T lo, T hi)
{
    if (x != x) return 0;
    if (x <= (real)lo) return lo;
    if (x >= (real)hi) return hi;
    return (T)x;
}
inline int32_t as_i32(real x) { return sat_cast<int32_t>(x, INT32_MIN, INT32_MAX); }
inline int64_t as_i64(real x)
{
    // the FPU converts to 32 bits only; a 64-bit conversion is a library call
    if (x > (real)-2147483648.0f && x < (real)2147483648.0f) return (int32_t)x;
    if (x != x) return 0;
    if (x <= (real)-9.2233720368547758e18) return INT64_MIN;
    if (x >= (real)9.2233720368547758e18) return INT64_MAX;
    if (sizeof(real) == 4) {
        // a float of magnitude 2^31..2^63 is an integer: its mantissa shifted left by 8..39
        uint32_t b = bits((float)x);
        int64_t v = (int64_t)((b & 0x7FFFFFu) | 0x800000u) << (((b >> 23) & 0xFFu) - 150u);
        return b >> 31 ? -v : v;
    }
    return (int64_t)x;
}
inline uint8_t as_u8(real x) { return sat_cast<uint8_t>(x, 0, 255); }
inline uint16_t as_u16(real x) { return sat_cast<uint16_t>(x, 0, 65535); }
inline uint32_t as_u32(real x) { return sat_cast<uint32_t>(x, 0, UINT32_MAX); }

// ---- the block

static const int BLOCK = 64;
typedef float Block[BLOCK];
// The denormal guard every bus is filled with (0x358637BD, 1e-6).
inline float guard() { return F32(0x358637BDu); }
inline void fill(Block& b, float v) { for (int i = 0; i < BLOCK; i++) b[i] = v; }

// Rust Option<i32>.
struct OptI32 {
    bool some;
    int32_t v;
    static OptI32 none() { OptI32 o = {false, 0}; return o; }
    static OptI32 of(int32_t x) { OptI32 o = {true, x}; return o; }
};

// ---- song ids (crate::song::Device): the msg-41 device id and the track number

enum Device : uint8_t {
    DEV_MIXER = 0, DEV_TB303_1 = 1, DEV_TB303_2 = 2, DEV_TR808 = 3, DEV_TR909 = 4,
    DEV_DELAY = 5, DEV_DIST = 6, DEV_PCF = 7, DEV_COMP = 8,
};
static const int DEVICES = 9;

// ---- tables (crate::tables::Tables), generated on the host by tools/fm1pack

struct Smoother {
    float b2, a, g;
};

// One computed filter table entry, kept while its key (row, index) does not change.
struct CoefCache {
    uint32_t key;
    float c[4];
    CoefCache() : key(~0u) {}
};

// One computed distortion shaper row.
struct RowCache {
    uint32_t key;
    float w[256];
    RowCache() : key(~0u) {}
};

// Entries of the filter and shaper tables computed instead of read (orb_tables.cc): the FM-1
// has no flash for the 260 KB they take.
void tb303_filter_entry(uint32_t row, uint32_t idx, float out[4]);
void pcf_filter_entry(bool bandpass, uint32_t row, uint32_t idx, float out[3]);
void dist_shaper_row(uint32_t row, float out[256]);

struct Tables {
    const float* level;                    // LevelCurve [128]
    const float* tb303_pitch;              // PitchTable [6144]
    const float (*tb303_filter)[1024][4];  // Tb303Filter [8], or null: computed
    const float (*pcf_lowpass)[512][3];    // PcfFilter [8], or null: computed
    const float (*pcf_bandpass)[512][3];   // PcfFilter [8], or null: computed
    const float (*dist_shaper)[256];       // DistShaper [32], or null: computed
    const uint16_t* tr808_tom_step;        // [128]
    const uint16_t* tr909_tom_step;        // [128]
    const float* tr909_decay;              // [16]
    Smoother tb303_smoother;
    Smoother pcf_smoother;

    const float* tb303_coef(uint32_t row, uint32_t idx, CoefCache& cc) const
    {
        if (tb303_filter) return tb303_filter[row][idx];
        uint32_t key = row << 16 | idx;
        if (cc.key != key) {
            tb303_filter_entry(row, idx, cc.c);
            cc.key = key;
        }
        return cc.c;
    }
    const float* pcf_coef(bool bandpass, uint32_t row, uint32_t idx, CoefCache& cc) const
    {
        const float (*t)[512][3] = bandpass ? pcf_bandpass : pcf_lowpass;
        if (t) return t[row][idx];
        uint32_t key = (uint32_t)bandpass << 24 | row << 16 | idx;
        if (cc.key != key) {
            pcf_filter_entry(bandpass, row, idx, cc.c);
            cc.key = key;
        }
        return cc.c;
    }
    const float* dist_row(uint32_t row, RowCache& rc) const
    {
        if (dist_shaper) return dist_shaper[row];
        if (rc.key != row) {
            dist_shaper_row(row, rc.w);
            rc.key = row;
        }
        return rc.w;
    }

    // PanTable PAN[a][b] = [L, R] (crate::tables::pan_table), computed instead of stored.
    void pan(int a, int b, float out[2]) const
    {
        int d = b - 64, q = (d * d) >> 5, cut = a - q;
        if (cut < 0) cut = 0;
        if (cut > 127) cut = 127;
        out[0] = level[d > 0 ? cut : a];
        out[1] = level[d < 0 ? cut : a];
    }
};

// The tables blob of fm1/pack: "ORBT", u32 version 1, u32 flags, then the arrays in the field
// order above, little endian, each 4-byte aligned. Flag 1: the filter and shaper tables are
// there (the host's bit-exact build); without it they are computed. False on a bad blob.
bool tables_from_blob(Tables& t, const uint8_t* blob, uint32_t len);

// ---- what a block may read (crate::frame::BlockContext)

struct BlockContext {
    const Tables* tables;
    uint32_t meter_index;
    uint32_t tempo;     // BPM * 1000
    int32_t tick;
    OptI32 stack_word;  // see the Rust docs: the PCF's uninitialised local
};

// ---- the drum samples (crate::samples)

static const uint16_t SAMPLE_FIRST_ID = 1000;
static const uint16_t SAMPLE_LAST_ID = 1045;
static const int SAMPLE_COUNT = 46;
static const uint16_t SAMPLE_STARTUP_ID = 1024;

// One sample, in one of three codecs (orb_samples.cc):
//   PCM   (host reference) `data` is `frames` int16;
//   FLAC  (FM-1, the tonal drums) `data` is FLAC frames of FLAC_BLOCK samples (mono, 16 bit,
//         fixed block size, no metadata), `index[k]` the byte offset of frame k;
//   ADPCM (FM-1, cymbals, hats and noise) `data` is blocks of 32 samples in 17 bytes.
// Reads past `frames` are 0 (the 512 zero samples of padding, and beyond).
enum Codec : uint8_t { CODEC_PCM = 0, CODEC_FLAC = 1, CODEC_ADPCM = 2 };
static const uint32_t FLAC_BLOCK = 256;
static const uint32_t ADPCM_BLOCK = 32;

struct Sample {
    uint32_t frames;
    uint8_t codec;
    const uint8_t* data;
    const uint32_t* index;  // FLAC only
};

struct SampleSet {
    Sample s[SAMPLE_COUNT];  // by resource id - 1000
    const Sample* get(uint16_t id) const { return &s[id - SAMPLE_FIRST_ID]; }
};

// The kit of fm1/pack, in up to KIT_REGIONS flash regions: region 0 is "ORBK" (the header and
// the samples that fit there), the others hold samples only and are checked against the size
// and CRC-32 the header records. Points the samples into the regions; a sample in a region that
// is missing or does not match is left empty (silent). Returns the mask of such regions, or -1
// when region 0 is not a kit.
static const int KIT_REGIONS = 4;
int kit_from_regions(SampleSet& k, const uint8_t* const* region, const uint32_t* len, int regions);
uint32_t crc32(const uint8_t* p, uint32_t n, uint32_t crc = 0);

// A voice's read head on one sample: it decodes whole codec blocks into a ring as reads move
// forward. The ring holds the last RING frames decoded; a read further back decodes again, from
// its own block for FLAC (the blocks are independent), from the start for ADPCM (the voices read
// forward from 0 after a trigger, which resets the cursor, so this is rare).
struct SampleCursor {
    static const uint32_t RING = 2 * FLAC_BLOCK;
    const Sample* s;
    uint32_t next;   // the next frame to decode
    int32_t h1, h2;  // ADPCM history
    int16_t ring[RING];
    void reset(const Sample* smp) { s = smp; next = 0; h1 = h2 = 0; }
};

// Decodes the codec block holding frame c.next into the ring and advances c.next past it.
void sample_decode_next(SampleCursor& c);

// Sample i of the cursor's sample as Rust's `at(d, i)`: 0 past the end (or without a sample).
inline int32_t sample_at(SampleCursor& c, uint32_t i)
{
    const Sample* s = c.s;
    if (!s || i >= s->frames) return 0;
    if (s->codec == CODEC_PCM) return ((const int16_t*)s->data)[i];
    if (i + SampleCursor::RING < c.next) {
        c.next = s->codec == CODEC_FLAC ? i / FLAC_BLOCK * FLAC_BLOCK : 0;
        c.h1 = c.h2 = 0;
    }
    while (i >= c.next) sample_decode_next(c);
    return c.ring[i & (SampleCursor::RING - 1)];
}

// ---- the process globals (crate::process::ProcessState)

static const uint32_t DELAY_LEN = 1u << 17;
static const uint32_t DELAY_MASK = DELAY_LEN - 1;

// The global delay line, 131072 cells. The host keeps f32 cells; the FM-1 (ORB_DELAY_F16) keeps
// IEEE half floats (512 KB of f32 is the FM-1's whole RAM): the error stays ~60 dB below the
// signal at any level, so the echo tails stay clean. Below 2^-14 a cell is 0.

inline uint16_t f32_to_f16(float x)
{
    uint32_t b = bits(x), sign = (b >> 16) & 0x8000u;
    uint32_t a = b & 0x7FFFFFFFu;
    if (a < 0x38800000u) return (uint16_t)sign;          // below 2^-14 (and denormals): 0
    if (a >= 0x477FF000u) return (uint16_t)(sign | 0x7BFFu);  // saturate at 65504 (no inf, no NaN)
    a += 0x00000FFFu + ((a >> 13) & 1u);                 // round to nearest even
    return (uint16_t)(sign | ((a - 0x38000000u) >> 13));
}
inline float f16_to_f32(uint16_t h)
{
    uint32_t e = h & 0x7C00u;
    if (!e) return 0.0f;
    return F32(((uint32_t)(h & 0x8000u) << 16) | (((uint32_t)(h & 0x7FFFu) << 13) + 0x38000000u));
}

struct DelayLine {
#ifdef ORB_DELAY_F16
    uint16_t* buf;
    float get(uint32_t i) const { return f16_to_f32(buf[i]); }
    void set(uint32_t i, float x) { buf[i] = f32_to_f16(x); }
#else
    float* buf;
    float get(uint32_t i) const { return buf[i]; }
    void set(uint32_t i, float x) { buf[i] = x; }
#endif
    bool gate;
    // The refill of 0x100100E0: every cell 1e-6, the gate set.
    void refill()
    {
        for (uint32_t i = 0; i < DELAY_LEN; i++) set(i, guard());
        gate = true;
    }
};

struct ProcessState {
    uint16_t snare_808, snare_909, bd_808, bd_909;
    DelayLine delay;
    // The snare hits: c = (c + 1) & 31, stored and returned.
    uint16_t next_snare_808() { snare_808 = (uint16_t)((snare_808 + 1u) & 31u); return snare_808; }
    uint16_t next_snare_909() { snare_909 = (uint16_t)((snare_909 + 1u) & 31u); return snare_909; }
};

// ---- the device slots (crate::frame::SongDevice and the role traits)

class TrackMut;  // seq.h: bool insert(int32_t tick, uint8_t ctrl, uint8_t value);
                 //         void set_quant(uint8_t ctrl, uint8_t q);

class SongDevice {
public:
    virtual ~SongDevice() {}
    virtual void step(int32_t tick, int32_t shuffled, int32_t tempo) { (void)tick; (void)shuffled; (void)tempo; }
    virtual void relocate(int32_t tick, int32_t now) { (void)tick; (void)now; }
    virtual void start(int32_t now) { (void)now; }
    virtual void stop() {}
    virtual void reset() {}
    virtual void silence() {}
    virtual void set_song_mode(bool on) { (void)on; }
    virtual void data(const uint8_t* payload, uint32_t len, bool song_mode) { (void)payload; (void)len; (void)song_mode; }
    virtual void set_param(uint8_t ctrl, uint8_t value) { (void)ctrl; (void)value; }
    virtual void param_message(uint8_t ctrl, uint8_t value) { set_param(ctrl, value); }
    virtual void set_sound_model(bool vintage) { (void)vintage; }
    virtual void init_track(TrackMut& track) { (void)track; }
    // The live parameter block (empty: len 0).
    virtual const uint8_t* live(uint32_t* len) const { *len = 0; return 0; }
};

class ChannelDevice : public SongDevice {
public:
    virtual void process(Block& bus, const BlockContext& ctx) = 0;
    virtual OptI32 playing_pattern() const { return OptI32::none(); }
    virtual uint8_t* patterns_mut(uint32_t* len) { *len = 0; return 0; }
    virtual OptI32 stack_word() const { return OptI32::none(); }
};

// crate::frame::Fault: the PCF's division by a zero pattern length.
struct Fault {
    uint8_t wave;
};

class InsertEffect : public SongDevice {
public:
    virtual void process(Block& bus, const BlockContext& ctx) = 0;
    virtual void process_stereo(Block& l, Block& r, const BlockContext& ctx) { (void)l; (void)r; (void)ctx; }
    virtual void update(const BlockContext& ctx) { (void)ctx; }
    // The fault of the last process, if any; reading it clears it.
    virtual bool faulted(Fault* f) { (void)f; return false; }
    virtual OptI32 stack_word(OptI32 prev) const { return prev; }
};

class SendEffect : public SongDevice {
public:
    virtual void process(Block& send, Block& l, Block& r, const BlockContext& ctx) = 0;
    virtual OptI32 stack_word(OptI32 out_end, OptI32 prev) const { (void)out_end; return prev; }
};

// crate::frame::Devices: the song's devices in their mixer roles. `mixer` is the frame's
// MixerDevice (frame.h).
struct Devices {
    SongDevice* mixer;
    ChannelDevice* channels[4];  // 0 TB-303 #1, 1 TB-303 #2, 2 TR-808, 3 TR-909
    InsertEffect* dist;
    InsertEffect* pcf;
    InsertEffect* comp;
    SendEffect* delay;
    ProcessState* process;

    // device_mut: the device with id d.
    SongDevice& device(Device d)
    {
        switch (d) {
        case DEV_MIXER: return *mixer;
        case DEV_TB303_1: return *channels[0];
        case DEV_TB303_2: return *channels[1];
        case DEV_TR808: return *channels[2];
        case DEV_TR909: return *channels[3];
        case DEV_DELAY: return *delay;
        case DEV_DIST: return *dist;
        case DEV_PCF: return *pcf;
        default: return *comp;
        }
    }
    // for_each: the song's device order (mixer, 303, 303, 808, 909, delay, dist, PCF, comp).
    template <typename F> void for_each(F f)
    {
        f(*mixer);
        for (int c = 0; c < 4; c++) f(*channels[c]);
        f(*delay);
        f(*dist);
        f(*pcf);
        f(*comp);
    }
    void stop_all() { for_each([](SongDevice& d) { d.stop(); }); }
    void reset_all() { for_each([](SongDevice& d) { d.reset(); }); }
    void silence_all() { for_each([](SongDevice& d) { d.silence(); }); }
};

}  // namespace orb
