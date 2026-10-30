// SPDX-License-Identifier: GPL-3.0-or-later
// The blobs of tools/fm1pack (tables, kit) and the sample codecs: a streaming FLAC frame decoder
// (mono, 16 bit, fixed block size, no metadata) and the 4-bit block ADPCM.
#include "orb_core.h"

namespace orb {

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// ---- tables

bool tables_from_blob(Tables& t, const uint8_t* b, uint32_t len)
{
    static const uint32_t BIG = 4 * (8 * 1024 * 4 + 2 * 8 * 512 * 3 + 32 * 256);
    static const uint32_t SMALL = 12 + 4 * (128 + 6144) + 2 * 2 * 128 + 4 * 16 + 2 * 12;
    if (len < 12 || memcmp(b, "ORBT", 4) || rd32(b + 4) != 1 || ((uintptr_t)b & 3)) return false;
    bool full = rd32(b + 8) & 1;
    if (len < SMALL + (full ? BIG : 0)) return false;
    const uint8_t* p = b + 12;
    t.level = (const float*)p; p += 4 * 128;
    t.tb303_pitch = (const float*)p; p += 4 * 6144;
    t.tb303_filter = 0;
    t.pcf_lowpass = t.pcf_bandpass = 0;
    t.dist_shaper = 0;
    if (full) {
        t.tb303_filter = (const float(*)[1024][4])p; p += 4 * 8 * 1024 * 4;
        t.pcf_lowpass = (const float(*)[512][3])p; p += 4 * 8 * 512 * 3;
        t.pcf_bandpass = (const float(*)[512][3])p; p += 4 * 8 * 512 * 3;
        t.dist_shaper = (const float(*)[256])p; p += 4 * 32 * 256;
    }
    t.tr808_tom_step = (const uint16_t*)p; p += 2 * 128;
    t.tr909_tom_step = (const uint16_t*)p; p += 2 * 128;
    t.tr909_decay = (const float*)p; p += 4 * 16;
    memcpy(&t.tb303_smoother, p, 12); p += 12;
    memcpy(&t.pcf_smoother, p, 12);
    return true;
}

// ---- kit: region 0 starts "ORBK", u32 version 2, u32 sample count 46, u32 region count R,
//      R x { u32 size, u32 crc32 } (region 0's crc unused), then per sample
//      { u32 frames, u8 codec, u8 region, u16 0, u32 data_off, u32 data_len, u32 index_off,
//        u32 index_count }, offsets within the sample's region (4-aligned)

uint32_t crc32(const uint8_t* p, uint32_t n, uint32_t crc)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

int kit_from_regions(SampleSet& k, const uint8_t* const* region, const uint32_t* len, int regions)
{
    const uint8_t* b = region[0];
    if (regions < 1 || !b || len[0] < 16 || memcmp(b, "ORBK", 4) || rd32(b + 4) != 2 ||
        rd32(b + 8) != (uint32_t)SAMPLE_COUNT || ((uintptr_t)b & 3))
        return -1;
    uint32_t nreg = rd32(b + 12);
    uint32_t head = 16 + 8 * nreg + 24u * SAMPLE_COUNT;
    if (nreg < 1 || nreg > (uint32_t)KIT_REGIONS || len[0] < head) return -1;
    int missing = 0;
    uint32_t size[KIT_REGIONS];
    for (uint32_t r = 0; r < nreg; r++) {
        size[r] = rd32(b + 16 + 8 * r);
        bool ok = r == 0 ? size[0] <= len[0]
                         : (int)r < regions && region[r] && !((uintptr_t)region[r] & 3) && size[r] <= len[r] &&
                               crc32(region[r], size[r]) == rd32(b + 20 + 8 * r);
        if (!ok) missing |= 1 << r;
    }
    if (missing & 1) return -1;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        const uint8_t* e = b + 16 + 8 * nreg + 24 * i;
        uint32_t frames = rd32(e), codec = e[4], r = e[5], off = rd32(e + 8), n = rd32(e + 12);
        uint32_t ioff = rd32(e + 16), icount = rd32(e + 20);
        Sample& s = k.s[i];
        s.frames = 0;
        s.codec = CODEC_PCM;
        s.data = 0;
        s.index = 0;
        if (r >= nreg) return -1;
        if (missing & (1 << r)) continue;
        uint32_t sz = size[r];
        if (codec > CODEC_ADPCM || off > sz || n > sz - off || ioff > sz || icount > (sz - ioff) / 4 ||
            (codec == CODEC_FLAC && icount * FLAC_BLOCK < frames) || (ioff & 3))
            return -1;
        s.frames = frames;
        s.codec = (uint8_t)codec;
        s.data = region[r] + off;
        s.index = codec == CODEC_FLAC ? (const uint32_t*)(region[r] + ioff) : 0;
    }
    return missing;
}

// ---- ADPCM: per 32 samples one byte (filter << 4 | shift) and 16 bytes of 4-bit codes, low
//      nibble first; s = clamp16(((code << 12) >> shift) + ((h1 * F0 + h2 * F1 + 32) >> 6))

static const int32_t ADPCM_F[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static void adpcm_block(const uint8_t* b, int32_t& h1, int32_t& h2, int16_t* out)
{
    uint32_t f = b[0] >> 4, sh = b[0] & 15;
    if (f > 4) f = 0;
    const int32_t f0 = ADPCM_F[f][0], f1 = ADPCM_F[f][1];
    for (int i = 0; i < 32; i++) {
        int32_t code = (b[1 + (i >> 1)] >> ((i & 1) * 4)) & 15;
        code = (code ^ 8) - 8;
        int32_t s = ((code * 4096) >> sh) + ((h1 * f0 + h2 * f1 + 32) >> 6);
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        out[i] = (int16_t)s;
        h2 = h1;
        h1 = s;
    }
}

// ---- FLAC frames

struct Bits {
    const uint8_t* p;
    uint32_t acc;  // left-aligned
    int n;         // valid bits in acc
    uint32_t get(int k)  // 1 <= k <= 32
    {
        if (k > 24) {
            uint32_t hi = get(k - 16);
            return hi << 16 | get(16);
        }
        while (n < k) {
            acc |= (uint32_t)*p++ << (24 - n);
            n += 8;
        }
        uint32_t v = acc >> (32 - k);
        acc <<= k;
        n -= k;
        return v;
    }
    int32_t sget(int k)
    {
        uint32_t v = get(k);
        return (int32_t)(v << (32 - k)) >> (32 - k);
    }
    uint32_t unary()  // zeros before a one
    {
        uint32_t z = 0;
        for (;;) {
            if (!n) {
                acc = (uint32_t)*p++ << 24;
                n = 8;
            }
            if (acc) {
                uint32_t lz = (uint32_t)__builtin_clz(acc);
                if ((int)lz < n) {
                    acc <<= lz + 1;
                    n -= (int)lz + 1;
                    return z + lz;
                }
            }
            z += (uint32_t)n;
            acc = 0;
            n = 0;
        }
    }
    void align() { int r = n & 7; acc <<= r; n -= r; }
};

// The predictor of a FIXED or LPC subframe.
struct Predictor {
    uint32_t order;
    bool lpc;
    int shift;
    int32_t coef[32];
    int32_t at(const int16_t* o, uint32_t i) const
    {
        if (!lpc) {
            switch (order) {
            case 1: return o[i - 1];
            case 2: return 2 * o[i - 1] - o[i - 2];
            case 3: return 3 * o[i - 1] - 3 * o[i - 2] + o[i - 3];
            case 4: return 4 * o[i - 1] - 6 * o[i - 2] + 4 * o[i - 3] - o[i - 4];
            default: return 0;
            }
        }
        int64_t sum = 0;
        for (uint32_t j = 0; j < order; j++) sum += (int64_t)coef[j] * o[i - 1 - j];
        return (int32_t)(sum >> shift);
    }
};

// The residual of samples order..n, each added to its prediction as it is read.
static bool flac_residual(Bits& br, int16_t* out, uint32_t n, const Predictor& pr)
{
    uint32_t method = br.get(2);
    if (method > 1) return false;
    int pbits = method ? 5 : 4;
    uint32_t esc = method ? 31u : 15u;
    uint32_t porder = br.get(4), parts = 1u << porder, i = pr.order;
    if ((n >> porder) < pr.order) return false;
    for (uint32_t p = 0; p < parts; p++) {
        uint32_t cnt = (n >> porder) - (p ? 0 : pr.order);
        uint32_t k = br.get(pbits);
        if (k == esc) {
            uint32_t raw = br.get(5);
            for (uint32_t j = 0; j < cnt; j++, i++) out[i] = (int16_t)((raw ? br.sget((int)raw) : 0) + pr.at(out, i));
        } else {
            for (uint32_t j = 0; j < cnt; j++, i++) {
                uint32_t u = (br.unary() << k) | (k ? br.get((int)k) : 0);
                out[i] = (int16_t)(((int32_t)(u >> 1) ^ -(int32_t)(u & 1)) + pr.at(out, i));
            }
        }
    }
    return true;
}

// One frame at `p` into out[0..n). Returns n, 0 on a damaged frame.
static uint32_t flac_frame(const uint8_t* p, int16_t* out)
{
    Bits br = {p, 0, 0};
    if (br.get(14) != 0x3FFE) return 0;
    br.get(2);
    uint32_t bs = br.get(4), sr = br.get(4);
    if (br.get(4) != 0 || br.get(3) != 4) return 0;  // mono, 16 bit
    br.get(1);
    uint32_t c = br.get(8);  // UTF-8 frame number: one more byte per leading 1 after the first
    if (c & 0x80)
        for (uint32_t m = 0x40; c & m; m >>= 1) br.get(8);
    uint32_t n;
    if (bs == 6) n = br.get(8) + 1;
    else if (bs == 7) n = br.get(16) + 1;
    else if (bs == 8) n = 256;
    else if (bs >= 9) n = 256u << (bs - 8);
    else return 0;
    if (n > FLAC_BLOCK) return 0;
    if (sr == 12) br.get(8);
    else if (sr == 13 || sr == 14) br.get(16);
    br.get(8);  // CRC-8
    // the subframe, in the domain of the wasted bits (|value| < 2^(16 - wasted))
    br.get(1);
    uint32_t type = br.get(6), wasted = 0;
    if (br.get(1)) wasted = br.unary() + 1;
    if (wasted > 15) return 0;
    int bps = 16 - (int)wasted;
    if (type == 0) {
        int16_t v = (int16_t)br.sget(bps);
        for (uint32_t i = 0; i < n; i++) out[i] = v;
    } else if (type == 1) {
        for (uint32_t i = 0; i < n; i++) out[i] = (int16_t)br.sget(bps);
    } else if ((type >= 8 && type <= 12) || type >= 32) {
        Predictor pr;
        pr.lpc = type >= 32;
        pr.order = pr.lpc ? type - 31 : type - 8;
        pr.shift = 0;
        for (uint32_t i = 0; i < pr.order; i++) out[i] = (int16_t)br.sget(bps);
        if (pr.lpc) {
            int prec = (int)br.get(4) + 1;
            if (prec == 16) return 0;
            pr.shift = br.sget(5);
            if (pr.shift < 0) return 0;
            for (uint32_t i = 0; i < pr.order; i++) pr.coef[i] = br.sget(prec);
        }
        if (!flac_residual(br, out, n, pr)) return 0;
    } else {
        return 0;
    }
    if (wasted)
        for (uint32_t i = 0; i < n; i++) out[i] = (int16_t)(out[i] * (1 << wasted));
    return n;
}

void sample_decode_next(SampleCursor& c)
{
    const Sample* s = c.s;
    int16_t* dst = &c.ring[c.next & (SampleCursor::RING - 1)];
    if (s->codec == CODEC_ADPCM) {
        adpcm_block(s->data + (c.next / ADPCM_BLOCK) * 17u, c.h1, c.h2, dst);
        c.next += ADPCM_BLOCK;
        return;
    }
    uint32_t n = flac_frame(s->data + s->index[c.next / FLAC_BLOCK], dst);
    for (uint32_t i = n; i < FLAC_BLOCK; i++) dst[i] = 0;
    c.next += FLAC_BLOCK;
}

}  // namespace orb
