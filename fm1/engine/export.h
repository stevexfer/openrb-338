// SPDX-License-Identifier: GPL-3.0-or-later
// C++ port of crates/orb-engine/src/frame/export.rs: the export driver, "Export Loop as Audio
// File" as Rebirth.exe runs it (`sub_427988` -> `sub_4280D5`, `docs/export_state.md` section 4)
// after N idle blocks, which is how `tools/rbhost` reproduces the app's exports.
//
// The export's position arithmetic is the exe's f64 on every build (like the sequencer's).
#pragma once

#include "frame.h"

namespace orb {

// The frames of one realtime MME call: the idle blocks are rendered 16 at a time.
static const uint32_t REALTIME_FRAMES = 1024;
// The frames of a full export chunk (`cmp [ebp+var_14], 800h` at 0x428186).
static const int32_t CHUNK_FRAMES = 2048;

// What the export queries from the engine before it starts (msg 20, ids 4, 12 and 16).
struct ExportRange {
    uint32_t tempo;  // BPM * 1000
    uint32_t loop_start, loop_end;  // in ticks
    static ExportRange of(const Globals& g)
    {
        ExportRange r = {g.tempo, g.loop_start, g.loop_end};
        return r;
    }
};

// The frame counts of the export's `SafeSoundCallback` calls, `sub_4280D5`
// (0x428139..0x4281DB), one per next():
//   pos = (double)start
//   do  n = ftol((end - pos) / (tempo * (192.0 / 2646000000.0)))
//       if n >= 2048: n = 2048 else last
//       step = (tempo * n) * (192.0 / 2646000000.0)
//       render n; pos = pos + step
//   until last
class Chunks {
public:
    explicit Chunks(const ExportRange& r);
    // The next chunk's frame count (may be negative); false after the last.
    bool next(int32_t* n);

private:
    double tempo_, end_, pos_;
    bool done_;
};

// The exported frame count: the sum of the chunks.
uint64_t frame_count(const ExportRange& r);

// Renders idle_blocks 64-frame blocks with the transport stopped: idle_blocks / 16 calls of
// 1024 frames, then one call for the remainder, into buf (at least 1024 stereo frames).
void render_idle(Engine& e, uint32_t idle_blocks, int16_t* buf);

// Takes each chunk of an export: n interleaved stereo frames.
typedef void (*ExportSink)(void* ctx, const int16_t* frames, uint32_t n);

// The export as rbhost runs it on a song the caller has loaded and activated: idle_blocks idle
// blocks, stop, locate to the loop start, play, the chunks, then stop, msg 33 and a 0-frame
// call (which drops the last chunk's leftover). One int16 buffer of CHUNK_FRAMES stereo frames,
// zeroed once, serves the idle calls and every chunk, as rbhost's `chunk_buf` does: a render
// call that faults writes nothing, so its chunk repeats what the buffer held.
void export_range(Engine& e, const ExportRange& r, uint32_t idle_blocks, int16_t* buf, ExportSink sink, void* ctx);

// rbhost's whole run on a new song (Rust export_song): load the song blob, activate it over the
// stopped "Untitled song", then export_range with the loaded globals' range. False on a bad blob.
bool export_song(Engine& e, const uint8_t* blob, uint32_t len, uint32_t idle_blocks, int16_t* buf, ExportSink sink,
                 void* ctx);

// The 44-byte header rbhost and the app write before the samples (16-bit stereo 44100 Hz PCM).
void wav_header(uint32_t frames, uint8_t h[44]);

}  // namespace orb
