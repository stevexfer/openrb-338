/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-RB338: the Felucca shell around the RB-338 engine (engine/) and the front panel (ui.c). */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_audio.h"
#include "bootguard.h"
#include "rb338_engine.h"

bootguard_t bootguard __attribute__((section(".noinit")));
static volatile uint32_t fm1_ms;

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "libc.c"
#include "lcd.c"
#define CV_MAX (240u * 34u)    /* a full-width strip, tall enough for FONT_L */
#include "gfx.c"

#define FELUCCA_OTA 1
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_338"
#endif
#include "usb.c"
#include "midi_pad.c"

/* the song slot and kit regions (store.c), the settings sector, the OTA staging area */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, 0x97000u, 0xE0000u) || FL_IN(off, n, 0xE5000u, 0xFC000u) || \
                             FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
#include "fm1_flash.h"
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static int fl_erase4k_quiet(uint32_t off, uint32_t *took)
{
    uint32_t f = irq_save();
    int rc = FL_FAR(fl_erase4k_ram)(off, took);
    irq_restore(f);
    return rc;
}

static uint8_t recovery_active;
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)
#include "ota.c"
static void recovery_poll(void);
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void)
{
    fm1_wdt_feed();
    if (recovery_active) recovery_poll();
}
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k_quiet(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    if (recovery_active) return;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u)
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_WHITE, 1);
    else
        draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_WHITE, 1);
    (void)code;
}
static void ota_commit(const uint8_t *parm)
{
    bootguard_clear(&bootguard);
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
#include "recovery.c"
#include "store.c"

void fm1_timer5_irq(void)
{
    static uint32_t sub, last, acc;
    uint32_t t0 = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (sub % 5u == 0u)
        usb_poll();
    if (sub % 5u == 2u)
        midi_poll();
    if (++sub == 10u)
        sub = 0;
}
extern void isr_timer5(void);

#define HALF_FRAMES 128u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile uint8_t engine_on;              /* 0: the ISR writes silence and reads no flash data */
static volatile int8_t play_req = -1;           /* 0 stop / 1 play, applied by the ISR between renders */
#define HALF_TICKS (HALF_FRAMES * 544u)         /* TIMER4 ticks per half (24 MHz / 44.1 kHz) */
/* render time per half, per mille of HALF_TICKS: the running average (<< 8), the worst since PLAY */
static volatile uint32_t cpu_avg_q8, cpu_peak;

static void audio_silence(int32_t *out, unsigned n)
{
    while (n--)
        *out++ = 0;
}

void fm1_alnk0_irq(void)
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        int32_t *o = &abuf[fm1_audio_free_half() * HALF_WORDS];
        if (engine_on) {
            uint32_t t0 = fm1_ticks(), pm;
            if (play_req >= 0) {
                rb338_engine_play(play_req);
                play_req = -1;
                cpu_peak = 0;
            }
            rb338_engine_render(o, HALF_FRAMES);
            pm = (fm1_ticks() - t0) * 1000u / HALF_TICKS;
            cpu_avg_q8 = cpu_avg_q8 - (cpu_avg_q8 >> 6) + (pm << 2);    /* ~64 halves: 186 ms */
            if (pm > cpu_peak)
                cpu_peak = pm;
        } else
            audio_silence(o, HALF_WORDS);
        fm1_audio_ack_half();
    }
}
extern void isr_alnk0(void);

extern const uint8_t orb_kit0[], orb_kit0_end[];
static int engine_state;                        /* rb338_engine_init's result */

/* the engine on the kit (region 0 linked, 1 and 2 uploaded) and the song slot */
static void engine_load(void)
{
    const uint8_t *kit[3], *song;
    uint32_t len[3], song_len = 0;
    if (engine_on) {
        engine_on = 0;
        fm1_delay_ms(8);                        /* a render in progress has finished */
    }
    play_req = -1;
    kit[0] = orb_kit0;
    len[0] = (uint32_t)(orb_kit0_end - orb_kit0);
    kit[1] = st_data(AREA_KIT1, &len[1]);
    kit[2] = st_data(AREA_KIT2, &len[2]);
    song = st_data(AREA_SONG, &song_len);
    engine_state = rb338_engine_init(kit, len, 3, song, song_len);
    engine_on = engine_state >= 0;
}

static void rb338_splash(void)
{
    char title[33];
    uint32_t len = 0, i;
    const uint8_t *song = st_data(AREA_SONG, &len);
    int kit = engine_state >= 0 && !(engine_state & 0xFF);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 72, 240, &FONT_L, "OpenRB-338", C_WHITE, 1);
    for (i = 0; i < 32u; i++)
        title[i] = song && len >= 68u && song[36 + i] >= 0x20u && song[36 + i] < 0x7Fu ? (char)song[36 + i] : 0;
    title[32] = 0;
    if (song && engine_state >= 0 && (engine_state & RB338_BAD_SONG))
        draw_text_box(0, 120, 240, &FONT_S, "SONG DID NOT LOAD", RGB(255, 160, 0), 1);
    else
        draw_text_box(0, 120, 240, &FONT_S, song ? (title[0] ? title : "SONG") : "NO SONG", 0xBDF7, 1);
    draw_text_box(0, 144, 240, &FONT_S, engine_state < 0 ? "ENGINE ERROR" : kit ? "KIT OK" : "KIT: UPLOAD NEEDED",
                  kit ? 0xBDF7 : RGB(255, 160, 0), 1);
    draw_text_box(0, 180, 240, &FONT_S, rb338_engine_playing() ? "PLAYING" : "PLAY: START", C_WHITE, 1);
}

#include "ui.c"
static uint32_t ui_from;                        /* the panel takes over from the splash at this fm1_ms */

static void store_quiet(void)
{
    engine_on = 0;
    fm1_delay_ms(8);                            /* both halves of the buffer refilled with silence */
}
static void store_changed(uint32_t area)
{
    (void)area;
    engine_load();
    rb338_splash();
    ui_from = fm1_ms + 1200u;
    ui_redraw();
}

/* SAVE: the engine's song over the song slot, the blob it was loaded from patched in place */
static const uint8_t *save_old;
static uint32_t save_old_len;
static void save_fill(uint8_t *dst, uint32_t off, uint32_t n) { rb338_song_blob(save_old, save_old_len, off, dst, n); }

static int song_save(void)
{
    uint32_t len;
    int was = rb338_engine_playing(), rc;
    save_old_len = 0;
    save_old = st_data(AREA_SONG, &save_old_len);
    len = rb338_song_blob(save_old, save_old_len, 0, 0, 0);
    if (!len || !engine_on)
        return -1;
    lcd_sync();                                 /* cv_px is the sector buffer */
    store_quiet();
    rc = st_rewrite(AREA_SONG, len, save_fill, (uint8_t *)cv_px);
    engine_load();
    if (was && engine_on)
        play_req = 1;
    return rc;
}

static void fm1_service(void)
{
    fm1_wdt_feed();
    usb_retry(fm1_ms);
    store_service();
    ota_service();
    if ((int32_t)(fm1_ms - ui_from) >= 0)
        ui_poll();
    if (usb.ota_req) {
        usb.ota_req = 0;
        if (flash_ok)
            ota_session();
    }
    if (usb.uboot_req) {
        fm1_audio_stop();
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 110, 240, &FONT_S, "UBOOT (USB)", C_WHITE, 1);
        usb_detach();
        fm1_delay_ms(30);
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    if ((uint32_t)fm1_ms >= 30000u && bootguard.pending)
        bootguard_clear(&bootguard);
}

static void hex8(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[40];
    uint32_t t0;
    lcd_sync();
    lcd_fill(0, 0, 240, 240, RGB(120, 0, 0));
    draw_text_box(0, 20, 240, &FONT_S, "RB338 CRASH", C_WHITE, 1);
    hex8(b, c->vec);
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    hex8(b, c->pc);
    draw_text_box(10, 80, 220, &FONT_S, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 15000u * 1000u * FM1_TICKS_PER_US)
        fm1_wdt_feed();
    fm1_reboot();
}

void fm1_cstart(void)
{
    extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
    extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
    extern void (*const _init_start[])(void), (*const _init_end[])(void);
    void (*const *ctor)(void);
    uint32_t *s, *d, p3, boot_mode;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    fm1_wdt_arm(0x0D);
    if ((p3 & 1u) && !(p3 & (4u | 0x40u)))
        bootguard_clear(&bootguard);
    boot_mode = bootguard_begin(&bootguard);
    if (boot_mode == BOOT_ROM)
        fm1_enter_uboot();
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0;
    for (d = _pool_start; d < _pool_end; d++) *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    for (ctor = _init_start; ctor < _init_end; ctor++)
        (*ctor)();                              /* the engine's constants (C++ dynamic initialisation) */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    if (boot_mode == BOOT_RECOVERY || recovery_key())
        recovery_main();
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    fl_plain_window_init();
    ota_boot_cleanup();
    engine_load();
    lcd_init();
    fm1_input_init();
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    usb_start();
    midi_uart_init();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    rb338_splash();
    ui_from = fm1_ms + 1500u;
    ui_redraw();
    for (;;)
        fm1_service();
}
