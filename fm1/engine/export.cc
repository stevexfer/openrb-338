// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/frame/export.rs (see export.h).
#include "export.h"

namespace orb {

namespace {

// `dbl_4BBE08`: ticks per quarter note, 192.
inline double ppq() { return F64(0x4068000000000000ull); }
// `dbl_4BBE10`: 60 s * 44100 frames * 1000 (the tempo is BPM * 1000).
inline double frames_milli_minute() { return F64(0x41E3B6D830000000ull); }

void wr32(uint8_t* p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

}  // namespace

// The values are 32-bit `fild`s; `192/2646e6` is divided at run time; `__ftol` truncates.
Chunks::Chunks(const ExportRange& r)
    : tempo_((double)(int32_t)r.tempo), end_((double)(int32_t)r.loop_end), pos_((double)(int32_t)r.loop_start),
      done_(false)
{
}

bool Chunks::next(int32_t* out)
{
    if (done_) return false;
    int32_t n = trunc_pos((end_ - pos_) / (tempo_ * (ppq() / frames_milli_minute())));
    bool last = n < CHUNK_FRAMES;
    if (!last) n = CHUNK_FRAMES;
    double step = (tempo_ * (double)n) * (ppq() / frames_milli_minute());
    pos_ += step;
    done_ = last;
    *out = n;
    return true;
}

uint64_t frame_count(const ExportRange& r)
{
    Chunks c(r);
    uint64_t sum = 0;
    int32_t n;
    while (c.next(&n))
        if (n > 0) sum += (uint32_t)n;
    return sum;
}

void render_idle(Engine& e, uint32_t idle_blocks, int16_t* buf)
{
    for (uint32_t i = 0; i < idle_blocks / 16; i++) e.render(buf, REALTIME_FRAMES);
    uint32_t rest = (idle_blocks % 16) * BLOCK;
    if (rest > 0) e.render(buf, rest);
}

// Each message is applied by a 0-frame call that renders nothing, so the messages are applied
// here directly.
void export_range(Engine& e, const ExportRange& r, uint32_t idle_blocks, int16_t* buf, ExportSink sink, void* ctx)
{
    memset(buf, 0, 2 * (uint32_t)CHUNK_FRAMES * sizeof(int16_t));
    render_idle(e, idle_blocks, buf);
    e.stop();
    e.render(0, 0);
    e.locate(r.loop_start);
    e.play();
    e.render(0, 0);
    Chunks chunks(r);
    int32_t n;
    while (chunks.next(&n)) {
        // rbhost writes only positive counts
        uint32_t k = n > 0 ? (uint32_t)n : 0;
        e.render(buf, k);
        sink(ctx, buf, k);
    }
    e.stop();
    e.reset();
    e.render(0, 0);
}

bool export_song(Engine& e, const uint8_t* blob, uint32_t len, uint32_t idle_blocks, int16_t* buf, ExportSink sink,
                 void* ctx)
{
    if (!e.load_blob(blob, len)) return false;
    e.activate(ACTIVATE_REPLACES_STOPPED);
    // msg 20 ids 4, 12, 16 read the loaded globals, which nothing after msg 36 changes
    export_range(e, ExportRange::of(e.frame.seq.globals), idle_blocks, buf, sink, ctx);
    return true;
}

void wav_header(uint32_t frames, uint8_t h[44])
{
    uint32_t data = frames * 4;
    memcpy(h, "RIFF", 4);
    wr32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    h[20] = 1;
    h[21] = 0;
    h[22] = 2;
    h[23] = 0;
    wr32(h + 24, 44100);
    wr32(h + 28, 44100 * 4);
    h[32] = 4;
    h[33] = 0;
    h[34] = 16;
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    wr32(h + 40, data);
}

}  // namespace orb
