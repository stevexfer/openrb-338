/* SPDX-License-Identifier: GPL-3.0-only */
/* The engine (fm1/engine, C++ on the FPU) as the C firmware sees it: rb338_engine.cc. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RB338_RENDER_MAX 128u     /* frames per render call: one half of the audio buffer */
#define RB338_BAD_TABLES (-2)
#define RB338_BAD_KIT (-1)
#define RB338_BAD_SONG 0x100      /* or'ed into the result: the song did not load, an empty song plays */

/* (Re)builds the engine on the linked tables, the kit's regions (region 0 first; a NULL region
 * is missing) and the song blob (NULL: an empty song). Not while rb338_engine_render may run.
 * Returns the mask of kit regions missing or damaged (their samples silent) | RB338_BAD_SONG,
 * or RB338_BAD_KIT / RB338_BAD_TABLES (no engine: silence). */
int rb338_engine_init(const uint8_t *const *kit, const uint32_t *kit_len, int regions, const uint8_t *song,
                      uint32_t song_len);
/* frames (<= RB338_RENDER_MAX) of interleaved stereo for the codec: signed 24-bit, the engine's
 * 16-bit full scale at -6 dBFS (Felucca's ceiling: the analog stage is clean below it) */
void rb338_engine_render(int32_t *out, uint32_t frames);
void rb338_engine_play(int on);
int rb338_engine_playing(void);

/* The song as the UI sees it. dev: 0 mixer, 1 303 #1, 2 303 #2, 3 808, 4 909, 5 delay,
 * 6 distortion, 7 PCF, 8 compressor. */
enum {
    RB_TEMPO,         /* BPM * 1000 */
    RB_SHUFFLE,       /* 0..127 */
    RB_SONG_MODE,     /* 0 pattern mode, 1 song mode */
    RB_TICK,          /* get: the song position (48 ticks a step, 768 a bar) */
    RB_PARAM,         /* dev, idx: a live controller (the 909's in RBS order); the mixer's idx is its
                       * payload offset: 0 master, 1 compressor (0 off, 1 master, 2.. channel), 2 PCF
                       * (0 off, 1.. channel), 5 + 6c + {0 on, 1 level, 2 pan, 3 send, 4 dist} */
    RB_PATTERN_BYTE,  /* dev 1..4, idx = pattern * RB_PATTERN_LEN(dev) + byte */
    RB_CUR_PATTERN,   /* get, dev 1..4: the pattern playing */
    RB_STEP,          /* get, dev 1..4: the step last played */
    RB_METER,         /* get, idx 0..3 a channel, 4/5 the master's left/right: 0..127 */
};
#define RB_PATTERN_LEN(dev) ((dev) <= 2 ? 98 : 194)
int rb338_get(int what, int dev, int idx);
/* queued, applied by the next render (the audio ISR); rb338_engine_init drops what is queued */
void rb338_set(int what, int dev, int idx, int value);

/* SAVE: the song as a blob (old: the blob it was loaded from, or NULL), bytes [off, off + n)
 * into out (none when NULL); returns its length. */
uint32_t rb338_song_blob(const uint8_t *old, uint32_t old_len, uint32_t off, uint8_t *out, uint32_t n);

#ifdef __cplusplus
}
#endif
