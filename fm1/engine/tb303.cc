// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/tb303.rs. The sample loops keep every state in x87
// registers or qword locals (`real`) and store an f32 only at the block end; the output samples
// and the accent envelope (`+0xCBC`) are the only f32 stores inside the loops.
#include "tb303.h"

#include "seq.h"

namespace orb {

const uint8_t Tb303::DEFAULT_PARAMS[Tb303::PARAMS_LEN] = {1, 0, 0x40, 0x14, 0x5A, 0x14, 0x64, 0x40, 0, 0, 0, 0};

namespace {

// The control path (envelopes, cutoff, coefficient lookup: once per block, once per 4 samples
// in Vintage) in double in every build, soft-double on the FM-1: in float its rounding moves the
// cutoff across coefficient-table indices and the 303 loses ~20 dB against the reference. The
// per-sample voice (`real`) is not sensitive.
typedef double ctl;

// `as_i64` for the table indices and rows: in range by the cutoff clamp, NaN -> 0.
inline int64_t ctl_i64(ctl x)
{
    if (x != x) return 0;
    if (x <= (ctl)-2147483648.0f) return INT32_MIN;
    if (x >= (ctl)2147483648.0f) return INT32_MAX;
    return (int32_t)x;
}

// TB-303 tune offset in semitones, indexed by `tune >> 2` (`word_10033F90`, i16[32]).
const int16_t TB303_TUNE[32] = {
    -12, -12, -11, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 12,
};

// Note-on (0x10023CD4).
// `dbl_10153B28`: 1/12700.
const ctl ACC_RESO_SCALE = F64(0x3F14A4299AE3DC33ull);
// `dbl_10153B30`: 0.99.
const ctl ACC_RESO_BASE = F64(0x3FEFAE147AE147AEull);
// `dbl_10153B38`: 0.999.
const ctl ACC_RESO_MUL = F64(0x3FEFF7CED916872Bull);
// `dbl_10153B40`: 3/127.
const ctl ACC_AMOUNT_SCALE = F64(0x3F983060C183060Cull);
// `dbl_10153B48`.
const ctl DECAY_ONE = (ctl)1;
// `dbl_10153B50`: 0.0003.
const ctl DECAY_SPAN = F64(0x3F33A92A30553261ull);
// `dbl_10153B58`: 0.99999.
const ctl DECAY_BASE = F64(0x3FEFFFEB074A771Dull);
// `flt_10153B74`: f32 1/127.
const float DECAY_SCALE = F32(0x3C010204u);
// The accented envelope decay (`mov eax, 3F7FEBAFh` at 0x10023DB0): 0.99969.
const float ACC_DECAY = F32(0x3F7FEBAFu);

// The amp's input and feedback constants (immediates).
// Note-on, 2.0 sound (0x10023E3D: `esi` = 1.0, `edi` = 0).
const float AMP_ON_20_IN = 1.0f, AMP_ON_20_FB = 0.0f;
// Note-on and note-off, Vintage (0x10023E25, 0x10023C9B): 0.1 and 0.9.
const float AMP_VINTAGE_IN = F32(0x3DCCCCCDu), AMP_VINTAGE_FB = F32(0x3F666666u);
// Note-off, 2.0 sound (0x10023CB3): 0.063 and 0.937.
const float AMP_OFF_20_IN = F32(0x3D810625u), AMP_OFF_20_FB = F32(0x3F6FDF3Bu);

// The audio. The Vintage engine's constants are at 0x10153B78..0x10153BDC, the 2.0 engine's at
// 0x10153BE0..0x10153C50; the values are the same except the resonance scale.
// `dbl_10153B78` / `dbl_10153BE0`.
const ctl ONE = (ctl)1;
// `dbl_10153B80` / `dbl_10153BE8`: 0.0022834645669291337.
const ctl CUT_SCALE = F64(0x3F62B4C5B45E7F8Eull);
// `dbl_10153B88` / `dbl_10153BF0`: 0.46.
const ctl CUT_BASE = F64(0x3FDD70A3D70A3D71ull);
// `dbl_10153B90` / `dbl_10153BF8`: 0.0029921259842519685.
const ctl EM_SCALE = F64(0x3F6882F167EE957Dull);
// `dbl_10153B98` / `dbl_10153C00`: 0.08.
const ctl EM_BASE = F64(0x3FB47AE147AE147Bull);
// `dbl_10153BA0` / `dbl_10153C10`: 1/15.
const ctl KA_SLOPE = F64(0x3FB1111111111111ull);
// `dbl_10153BA8` / `dbl_10153C18`: 0.10166666666666667.
const ctl KA_BASE = F64(0x3FBA06D3A06D3A07ull);
// `dbl_10153BB0` / `dbl_10153C20`: 0.4.
const ctl ENV_BIAS = F64(0x3FD999999999999Aull);
// `flt_10153BB8` / `flt_10153C30`: the denormal offset added to the envelope and the amp.
const float OFFSET = F32(0x358637BDu);
// `flt_10153BBC`: the Vintage resonance scale, f32 1/127.
const float RESO_VINTAGE = F32(0x3C010204u);
// `dbl_10153C08`: the 2.0 resonance scale, f64 1/127.
const ctl RESO_20 = F64(0x3F80204081020408ull);
// `flt_10153C3C`: the 2.0 resonance row position scale (`7 * x * 127 < 7`): 0.0078732288.
const float RESO_ROW = F32(0x3C00FEB7u);
// `flt_10153A70`: rows per unit in `sub_10023800`.
const float ROWS = 7.0f;
// `flt_10153BC0`, `flt_10153BC4` (and `flt_10153C34`, `flt_10153C38`): the cutoff clamp.
const float C_MAX = 1.0f;
const float C_MIN = 0.0f;
// `flt_10153BC8`, `flt_10153A6C`: the cutoff index scale.
const float C_SCALE = 1023.0f;
// `dbl_10153C28`: the 2.0 ramp step, 1/32.
const ctl RAMP = F64(0x3FA0000000000000ull);
// `flt_10153BC0`: the phase wraps above 1 ...
const float PHASE_MAX = 1.0f;
// ... by `flt_10153BCC`.
const float PHASE_WRAP = 2.0f;
// `flt_10153BD0`: the DC blocker's feedback, 0.985853374.
const float DC = F32(0x3F7C60E3u);
// `flt_10153BD4`: the square wave's threshold, `flt_10153BD8`/`flt_10153BDC` its levels.
const float SQ_THRESH = F32(0x3D3B8000u);  // 0.0457763671875
const float SQ_HI = F32(0x3F0CA000u);      // 0.54931640625
const float SQ_LO = F32(0xBF0CA000u);

const uint32_t PITCH_LEN = 6144;

// `x^(2^n)` by repeated `fmul st, st`, from the stored f32 and stored back as f32.
float square_n(float x, uint32_t n)
{
    ctl v = (ctl)x;
    for (uint32_t i = 0; i < n; i++) v = v * v;
    return st(v);
}

// The cutoff clamp of both engines: `> 1` gives 1 (0x10024152), then `< 0` gives 0
// (0x10024173).
ctl clamp_c(ctl c)
{
    c = c > (ctl)C_MAX ? (ctl)C_MAX : c;
    return c < (ctl)C_MIN ? (ctl)C_MIN : c;
}

// The filter coefficients of one sample pair: c0, c1 the 2-pole feedback, g the input gain
// (c2 times the accent and amp gain), c3 the pre-filter's feedback.
// Float builds keep c0, c1 and c3 as their distances from 2, 1 and 1 (`NEAR`): next to those
// values a float resolves only ~1e-7, too coarse for the ramp's sums and for the pole radius at
// low cutoffs (the 303 lost 6..20 dB against the reference). The double build uses them as is.
const bool NEAR = sizeof(real) < 8;

struct Coefs {
    real c0, c1, g, c3;  // NEAR: 2 - c0, 1 - c1, g, 1 - c3
    static Coefs of(ctl c0, ctl c1, ctl g, ctl c3)
    {
        Coefs k = {(real)c0, (real)c1, (real)g, (real)c3};
        if (NEAR) k = {(real)((ctl)2 - c0), (real)((ctl)1 - c1), (real)g, (real)((ctl)1 - c3)};
        return k;
    }
    // per-sample-pair ramp steps of c0..c3, in the same representation
    static Coefs steps(ctl d0, ctl d1, ctl d2, ctl d3)
    {
        Coefs k = {(real)d0, (real)d1, (real)d2, (real)d3};
        if (NEAR) k = {(real)-d0, (real)-d1, (real)d2, (real)-d3};
        return k;
    }
    ctl get_c0() const { return NEAR ? (ctl)2 - (ctl)c0 : (ctl)c0; }
    ctl get_c1() const { return NEAR ? (ctl)1 - (ctl)c1 : (ctl)c1; }
    ctl get_c3() const { return NEAR ? (ctl)1 - (ctl)c3 : (ctl)c3; }
};

// The per-sample state the loops keep in registers.
struct Voice {
    real ph, s, y1, y2, o;
    // With `real` = float, ph + pl is the phase the Rust accumulates in f64 over a block: pl
    // collects each step's rounding error (two-sum). A float phase alone drifts off the
    // reference's waveform within a note. Unused in the double build.
    real pl;

    // One sample (0x100241A5..0x1002429A for the Vintage saw; the same expression in every
    // loop): the phase step and wrap, the oscillator, `s = c3*s + x`,
    // `y = (s*g + y1*c0) - y2*c1`, `o = (y - y1) + o*DC`; the output is `f32(o)`.
    inline float sample(real inc, bool square, const Coefs& k)
    {
        real p = ph + inc;
        if (sizeof(real) < 8) {
            real b = p - ph;
            pl += (ph - (p - b)) + (inc - b);
            if (p > (real)PHASE_MAX || (p == (real)PHASE_MAX && pl > 0)) p -= (real)PHASE_WRAP;
            real h = p + pl;
            pl -= h - p;
            p = h;
        } else if (p > (real)PHASE_MAX) {
            p -= (real)PHASE_WRAP;
        }
        ph = p;
        real x = !square ? p : p <= (real)SQ_THRESH ? (real)SQ_LO : (real)SQ_HI;
        real y;
        if (NEAR) {
            s = (s - s * k.c3) + x;
            y = (y1 + (y1 - y2)) + ((s * k.g - y1 * k.c0) + y2 * k.c1);
        } else {
            s = s * k.c3 + x;
            y = (s * k.g + y1 * k.c0) - y2 * k.c1;
        }
        o = (y - y1) + o * (real)DC;
        y2 = y1;
        y1 = y;
        return st(o);
    }
};

Voice voice_of(const Tb303& d)
{
    Voice v = {(real)d.phase, (real)d.s, (real)d.y1, (real)d.y2, (real)d.o, (real)0};
    return v;
}

void store_voice(Tb303& d, const Voice& v)
{
    d.phase = st(v.ph);
    d.s = st(v.s);
    d.y1 = st(v.y1);
    d.y2 = st(v.y2);
    d.o = st(v.o);
}

// `sub_10023800`: the coefficients at cutoff `c` (an f32), between the resonance rows
// `l = trunc(rf*7)` and `l + 1`, each `f32(lo + (hi - lo)*frac)`.
void interpolate(const Tables& t, CoefCache* cc, float c, ctl rf, float out[4])
{
    int32_t idx = (int32_t)ctl_i64((ctl)c * (ctl)C_SCALE);
    ctl r7 = rf * (ctl)ROWS;
    uint32_t l = (uint16_t)ctl_i64(r7);
    ctl frac = r7 - (ctl)l;
    if (l > 6) l = 6;  // resonance > 127: the Rust indexes past the table and panics
    const float* lo = t.tb303_coef(l, (uint32_t)idx, cc[0]);
    const float* hi = t.tb303_coef(l + 1, (uint32_t)idx, cc[1]);
    for (int k = 0; k < 4; k++) {
        ctl d = (ctl)hi[k] - (ctl)lo[k];
        out[k] = st((ctl)lo[k] + d * frac);
    }
}

}  // namespace

Tb303::Tb303()
    : pat_start(0), cur_pattern(0), enabled(1), wrapped(false), gate_off(-1), last_pos(0),
      retrig(false), slide(false), phase(0.0f), env(0.0f), env_decay(0.0f), pitch_target(0),
      pitch(0), acc_amount(0.0f), acc_env(0.0f), cs1(0.5f), cs2(0.5f), amp_in(0.0f),
      amp_fb(0.0f), gate(0.0f), amp(0.0f), y1(0.0f), y2(0.0f), s(0.0f), o(0.0f),
      acc_coef(0.0f), song_mode(false), vintage(false)
{
    memcpy(params, DEFAULT_PARAMS, PARAMS_LEN);
    memcpy(live_params, DEFAULT_PARAMS, PARAMS_LEN);
    memset(patterns, 0, sizeof patterns);
    for (int p = 0; p < PATTERNS; p++) {
        uint8_t* q = patterns + p * PATTERN_LEN;
        q[1] = 16;
        for (int i = 0; i < 16; i++) q[2 + 6 * i + 5] = 1;
    }
    for (int k = 0; k < 4; k++) coefs[k] = 0.0f;
}

const uint8_t* Tb303::pattern(uint8_t p) const
{
    if (p >= PATTERNS) p = PATTERNS - 1;  // the Rust panics past the 32 patterns
    return patterns + (uint32_t)p * PATTERN_LEN;
}

// Note-off `sub_10023C74`: skipped while the last step slides; else the gate closes and the
// amp gets its release constants.
void Tb303::note_off()
{
    if (slide) return;
    gate = 0.0f;
    if (vintage) {
        amp_in = AMP_VINTAGE_IN;
        amp_fb = AMP_VINTAGE_FB;
    } else {
        amp_in = AMP_OFF_20_IN;
        amp_fb = AMP_OFF_20_FB;
    }
}

// Note-on 0x10023CD4. A rest step still sets the coefficients, the pitch and the slide flag;
// only a played step opens the gate.
void Tb303::note_on(const uint8_t step[6])
{
    uint8_t note = step[0], slide_ = step[1], accent = step[2], up = step[3], down = step[4],
            rest = step[5];
    uint32_t n = vintage ? 2 : 6;
    // 0x10023CF2: fild reso; fmul 1/12700; fadd 0.99; fmul 0.999; fstp
    float c = st(((ctl)live_params[RESONANCE] * ACC_RESO_SCALE + ACC_RESO_BASE) * ACC_RESO_MUL);
    acc_coef = square_n(c, n);
    if (accent == 0) {
        acc_amount = 0.0f;
        // 0x10023D72: 0.99999 - (1 - decay*f32(1/127))^2 * 0.0003
        ctl d = DECAY_ONE - (ctl)live_params[DECAY] * (ctl)DECAY_SCALE;
        env_decay = st(DECAY_BASE - d * d * DECAY_SPAN);
    } else {
        env_decay = ACC_DECAY;
        acc_amount = st((ctl)live_params[ACCENT] * ACC_AMOUNT_SCALE);
    }
    env_decay = square_n(env_decay, n);
    if (rest == 0) {
        gate = 1.0f;
        if (vintage) {
            amp_in = AMP_VINTAGE_IN;
            amp_fb = AMP_VINTAGE_FB;
        } else {
            amp_in = AMP_ON_20_IN;
            amp_fb = AMP_ON_20_FB;
        }
        if (!slide) {
            env = 1.0f;
            retrig = true;
        }
    }
    // 0x10023E61: ((tune + (int16)(note + 12*up - 12*down)) * 64 + 3072) * 16
    int32_t semis = (int16_t)((int32_t)note + 12 * (int32_t)up - 12 * (int32_t)down);
    uint32_t ti = live_params[TUNE] >> 2;
    if (ti > 31) ti = 31;  // tune > 127: the Rust indexes past the table and panics
    int32_t tune = TB303_TUNE[ti];
    pitch_target = (((uint32_t)tune << 6) + ((uint32_t)semis << 6) + 0xC00u) << 4;
    if (!slide) pitch = pitch_target;
    // 0x10023ED5: dword_10124708 counts note-ons; nothing reads it
    slide = slide_ != 0;
}

// The live block from the pattern-mode copy (0x10025CD4, 0x10026B86).
void Tb303::params_live()
{
    memcpy(live_params, params, PARAMS_LEN);
}

// The glide `(11*pitch + target) / 12` in u32 (0x10023F9F, 0x100248EB) and the phase
// increment `PITCH[pitch >> 4]`.
ctl Tb303::glide(const Tables& t)
{
    pitch = (pitch * 11u + pitch_target) / 12u;
    uint32_t i = pitch >> 4;
    if (i >= PITCH_LEN) i = PITCH_LEN - 1;  // the Rust panics past the table
    return (ctl)t.tb303_pitch[i];
}

// The cutoff smoother (0x10023F73..0x10023FEC; 0x100248E7..0x1002495F):
// `cs = (A*cs1 + cut*g) - b2*cs2`, `cs1` stored as f32 and the old `cs1` moved to `cs2`.
ctl Tb303::smooth_cutoff(const Tables& t)
{
    const Smoother& k = t.tb303_smoother;
    ctl cut = (ctl)live_params[CUTOFF] * CUT_SCALE + CUT_BASE;
    ctl cs = ((ctl)k.a * (ctl)cs1 + cut * (ctl)k.g) - (ctl)k.b2 * (ctl)cs2;
    cs2 = cs1;
    cs1 = st(cs);
    return cs;
}

// The 2.0 sound, `sub_100248C4`.
void Tb303::process_20(Block& bus, const Tables& t)
{
    ctl cs = smooth_cutoff(t);
    ctl inc = glide(t);
    // 0x1002494C: amp = (amp + 1e-6)*fb + gate*in, once per block
    ctl amp0 = (ctl)amp + (ctl)OFFSET;
    amp = st(amp0);
    ctl amp1 = amp0 * (ctl)amp_fb + (ctl)gate * (ctl)amp_in;
    ctl e0 = (ctl)OFFSET + (ctl)env;
    env = st(e0);
    // 0x100249BB: acc = coef*acc + E*((1 - coef)*amount)
    ctl ac = (ctl)acc_coef;
    ctl acc = ac * (ctl)acc_env + e0 * ((ONE - ac) * (ctl)acc_amount);
    acc_env = st(acc);
    ctl g = amp1 * (acc + ONE);
    ctl em = (ctl)live_params[ENVMOD] * EM_SCALE + EM_BASE;
    ctl r = (ctl)live_params[RESONANCE] * RESO_20;
    ctl ka = KA_BASE - r * KA_SLOPE;
    ctl acc32 = (ctl)acc_env;
    ctl rf = (ctl)live_params[RESONANCE] * (ctl)RESO_ROW;
    if (retrig) {
        // 0x10024ABC: the coefficients at the unchanged envelope, c2*G starting at 0
        ctl c = clamp_c(ka * acc32 + ((e0 - ENV_BIAS) * em + cs));
        float k[4];
        interpolate(t, coef_cache, st(c), rf, k);
        coefs[0] = k[0];
        coefs[1] = k[1];
        coefs[2] = 0.0f;
        coefs[3] = k[3];
        retrig = false;
    }
    // 0x10024B65: the targets at the decayed envelope
    ctl e1 = e0 * (ctl)env_decay;
    ctl c = clamp_c(ka * acc32 + ((e1 - ENV_BIAS) * em + cs));
    float n[4];
    interpolate(t, coef_cache, st(c), rf, n);
    ctl k0 = (ctl)coefs[0], k1 = (ctl)coefs[1], k2 = (ctl)coefs[2], k3 = (ctl)coefs[3];
    // 0x10024BE8: the ramp steps, (target - start) * 1/32
    ctl d2 = ((ctl)n[2] * g - k2) * RAMP;
    ctl d0 = ((ctl)n[0] - k0) * RAMP;
    ctl d1 = ((ctl)n[1] - k1) * RAMP;
    ctl d3 = ((ctl)n[3] - k3) * RAMP;
    Coefs k = Coefs::of(k0, k1, k2, k3);
    const Coefs dk = Coefs::steps(d0, d1, d2, d3);
    const real step = (real)inc;
    bool square = live_params[WAVEFORM] != 0;
    Voice v = voice_of(*this);
    for (int i = 0; i < BLOCK; i += 2) {
        bus[i] = v.sample(step, square, k);
        bus[i + 1] = v.sample(step, square, k);
        k.c0 += dk.c0;
        k.c1 += dk.c1;
        k.g += dk.g;
        k.c3 += dk.c3;
    }
    coefs[0] = st(k.get_c0());
    coefs[1] = st(k.get_c1());
    coefs[2] = st(k.g);
    coefs[3] = st(k.get_c3());
    store_voice(*this, v);
    env = st(e1);
    amp = st(amp1);
}

// The Vintage sound, inline in 0x10023F10 (0x10023F38..0x10024897).
void Tb303::process_vintage(Block& bus, const Tables& t)
{
    ctl r = (ctl)live_params[RESONANCE] * (ctl)RESO_VINTAGE;
    ctl cs = smooth_cutoff(t);
    ctl inc = glide(t);
    // 0x10023FDA, 0x10024002: the amp and the envelope get the offset, are stored, and the
    // loop runs on the unrounded sums
    ctl a = (ctl)amp + (ctl)OFFSET;
    amp = st(a);
    ctl e = (ctl)OFFSET + (ctl)env;
    env = st(e);
    ctl ka = KA_BASE - r * KA_SLOPE;
    ctl em = (ctl)live_params[ENVMOD] * EM_SCALE + EM_BASE;
    ctl acc_in = (ONE - (ctl)acc_coef) * (ctl)acc_amount;
    ctl ac = (ctl)acc_coef;
    ctl decay = (ctl)env_decay;
    uint32_t ri = live_params[RESONANCE] >> 4;
    if (ri > 7) ri = 7;  // resonance > 127: the Rust indexes past the table and panics
    bool square = live_params[WAVEFORM] != 0;
    const real step = (real)inc;
    Voice v = voice_of(*this);
    for (int q = 0; q < BLOCK; q += 4) {
        // 0x100240DD
        ctl e1 = e * decay;
        ctl acc = ac * (ctl)acc_env + e * acc_in;
        acc_env = st(acc);
        a = a * (ctl)amp_fb + (ctl)gate * (ctl)amp_in;
        ctl c = clamp_c(((e1 - ENV_BIAS) * em + cs) + acc * ka);
        ctl g = (acc + ONE) * a;
        e = e1;
        const float* f = t.tb303_coef(ri, (uint32_t)(int32_t)ctl_i64(c * (ctl)C_SCALE), coef_cache[0]);
        Coefs k = Coefs::of((ctl)f[0], (ctl)f[1], g * (ctl)f[2], (ctl)f[3]);
        for (int i = 0; i < 4; i++) bus[q + i] = v.sample(step, square, k);
    }
    // 0x10024841
    store_voice(*this, v);
    env = st(e);
    amp = st(a);
}

const uint8_t* Tb303::live(uint32_t* len) const
{
    *len = PARAMS_LEN;
    return live_params;
}

// Slot 1, 0x100253A0.
void Tb303::step(int32_t tick, int32_t shuffled, int32_t tempo)
{
    (void)tempo;
    uint8_t pat = cur_pattern;
    const uint8_t* p0 = pattern(pat);
    uint8_t shuffle = p0[0], len = p0[1];
    int32_t t = shuffle != 0 ? shuffled : tick;
    int32_t base = song_mode ? pat_start : 0;
    uint32_t period = 48u * len;
    // `div` by zero in the original; no file has a pattern of length 0
    if (period == 0) return;
    // 0x10025407: unsigned (t - base) % (48*len)
    int32_t v = (int32_t)(((uint32_t)t - (uint32_t)base) % period);
    if ((int32_t)((uint32_t)last_pos - (uint32_t)v) > 24) {
        if (gate_off >= 0) {
            note_off();
            gate_off = -1;
        }
        wrapped = true;
    }
    last_pos = v;
    int32_t stp = (v / 48) & 0xFFFF;
    if (gate_off >= 0) {
        // 0x1002547E
        if (enabled == 0) slide = false;
        if (v >= gate_off) {
            note_off();
            gate_off = -1;
        }
        return;
    }
    if (stp == (int32_t)live_params[LAST_STEP] && !wrapped) return;
    // 0x100254D3
    gate_off = v + 24;
    wrapped = false;
    int32_t q = tick / 48;
    int32_t s16 = q < 0 ? -((-q) & 15) : q & 15;
    if (s16 == 0) {
        // the bar line: the pending pattern and the enable flag
        uint8_t np = live_params[PATTERN];
        if (np != cur_pattern) {
            pat_start = 768 * (tick / 768);
            cur_pattern = np;
            pat = np;
        }
        enabled = live_params[ENABLED];
    }
    if (enabled != 0) {
        uint32_t at = 2u + 6u * (uint32_t)stp;
        const uint8_t* p = pattern(pat);
        uint8_t sb[6];
        for (uint32_t i = 0; i < 6; i++) sb[i] = at + i < (uint32_t)PATTERN_LEN ? p[at + i] : 0;
        note_on(sb);
    } else {
        note_off();
    }
    live_params[LAST_STEP] = (uint8_t)stp;
}

// Slot 2, 0x100255E0: the argument is not read; the bar of the song's position, the pending
// pattern and the enable flag.
void Tb303::relocate(int32_t tick, int32_t now)
{
    (void)tick;
    pat_start = 768 * (now / 768);
    if (live_params[PATTERN] != cur_pattern) cur_pattern = live_params[PATTERN];
    enabled = live_params[ENABLED];
}

// Slot 3, 0x100255B0.
void Tb303::start(int32_t now)
{
    wrapped = true;
    pat_start = 768 * (now / 768);
}

// Slot 4, 0x100106C0: slot 6.
void Tb303::stop()
{
    silence();
}

// Slot 5, 0x10026CC0, then slot 6. The cutoff smoother restarts at 0.5; the 2.0 coefficients,
// the retrigger flag, the patterns and the pattern position are kept.
void Tb303::reset()
{
    wrapped = false;
    slide = false;
    pitch_target = 0;
    pitch = 0;
    last_pos = 0;
    acc_coef = 0.0f;
    o = 0.0f;
    s = 0.0f;
    y2 = 0.0f;
    y1 = 0.0f;
    amp = 0.0f;
    gate = 0.0f;
    amp_fb = 0.0f;
    amp_in = 0.0f;
    acc_env = 0.0f;
    acc_amount = 0.0f;
    env_decay = 0.0f;
    env = 0.0f;
    phase = 0.0f;
    cs2 = 0.5f;
    cs1 = 0.5f;
    gate_off = -1;
    silence();
}

// Slot 6, 0x10025630: the slide flag cleared, then note-off.
void Tb303::silence()
{
    slide = false;
    note_off();
}

// Slot 9, 0x10026B60: entering song mode saves the live block into the pattern-mode copy,
// leaving it restores it.
void Tb303::set_song_mode(bool on)
{
    if (on)
        memcpy(params, live_params, PARAMS_LEN);
    else
        params_live();
    song_mode = on;
}

// Msg 39 (0x10025CB0 -> `sub_10018EF0`): the parameter block into the pattern-mode copy and
// the patterns; in pattern mode the block also goes live (bytes 9..11 included).
void Tb303::data(const uint8_t* payload, uint32_t len, bool song_mode_)
{
    if (len < PAYLOAD_LEN) return;  // the Rust panics on a short payload
    memcpy(params, payload, PARAMS_LEN);
    memcpy(patterns, payload + PARAMS_LEN, sizeof patterns);
    if (!song_mode_) params_live();
}

// Slot 11, 0x10026180: controllers 0..8 set the live byte. (The "changed" flags at
// `+0xC88 + ctrl` are not modelled.)
void Tb303::set_param(uint8_t ctrl, uint8_t value)
{
    if (ctrl <= WAVEFORM) live_params[ctrl] = value;
}

// Msg 40 `{ctrl, v}` (0x10025CFC, sender `sub_43EC83`): controllers 0..8 as slot 11, 9
// nothing (0x10025EFB), 10 the unused live byte 10 (`+0xC86`, 0x10025EFD).
void Tb303::param_message(uint8_t ctrl, uint8_t value)
{
    if (ctrl == 10)
        live_params[10] = value;
    else
        set_param(ctrl, value);
}

void Tb303::set_sound_model(bool vintage_)
{
    vintage = vintage_;
}

// Slots 7 (0x10026C40) and 8 (0x10025650) from the song ctor: controllers 0 and 1 quantised
// to 32 * 24 ticks, and a tick-0 event of every controller 0..8 with the ctor's value.
void Tb303::init_track(TrackMut& track)
{
    track.set_quant((uint8_t)ENABLED, 32);
    track.set_quant((uint8_t)PATTERN, 32);
    for (int c = 0; c <= WAVEFORM; c++) track.insert(0, (uint8_t)c, params[c]);
}

// Slot 14, 0x10023F10.
void Tb303::process(Block& bus, const BlockContext& ctx)
{
    if (vintage)
        process_vintage(bus, *ctx.tables);
    else
        process_20(bus, *ctx.tables);
}

OptI32 Tb303::playing_pattern() const
{
    return OptI32::of(cur_pattern);
}

uint8_t* Tb303::patterns_mut(uint32_t* len)
{
    *len = sizeof patterns;
    return patterns;
}

// 0x10023F10 does not write its entry `esp - 0x1C` (task-072).
OptI32 Tb303::stack_word() const
{
    return OptI32::none();
}

}  // namespace orb
