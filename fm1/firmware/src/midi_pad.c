/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 Mario: incoming MIDI notes, by pitch class. USB-MIDI (usb.c's ring)
 * and the TRS MIDI IN (UART1, PH8, 31250 baud), any channel, any octave:
 * bit c of midi_pc is set while any note of pitch class c (0 = C .. 11 = B)
 * is held. Polled from the TIMER5 ISR, the context usb_poll fills the ring in. */
#include "fm1_uart.h"

static uint32_t midi_held[4];                  /* 128 notes */
static uint8_t midi_pc_count[12];
static volatile uint32_t midi_pc;              /* held pitch classes */
static volatile uint32_t midi_pc_pressed;      /* press edges since midi_note_edges() */

static void midi_note(uint32_t note, int on)
{
    uint32_t w = note >> 5, bit = 1u << (note & 31u), pc = note % 12u;
    if (on) {
        if (midi_held[w] & bit)
            return;
        midi_held[w] |= bit;
        if (!midi_pc_count[pc]++) {
            midi_pc |= 1u << pc;
            midi_pc_pressed |= 1u << pc;
        }
    } else {
        if (!(midi_held[w] & bit))
            return;
        midi_held[w] &= ~bit;
        if (!--midi_pc_count[pc])
            midi_pc &= ~(1u << pc);
    }
}

static void midi_all_off(void)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        midi_held[i] = 0;
    for (i = 0; i < 12u; i++)
        midi_pc_count[i] = 0;
    midi_pc = 0;
}

static void midi_channel_msg(uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st >> 4) {
    case 0x9u:
        midi_note(d1, d2 != 0u);
        break;
    case 0x8u:
        midi_note(d1, 0);
        break;
    case 0xBu:
        if (d1 == 120u || d1 == 123u)          /* all sound off / all notes off */
            midi_all_off();
        break;
    default:
        break;
    }
}

static void midi_usb_drain(void)
{
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ];
        mi_r++;
        if ((pkt & 15u) >= 8u && (pkt & 15u) <= 0xEu)
            midi_channel_msg((pkt >> 8) & 0xFFu, (pkt >> 16) & 0x7Fu, (pkt >> 24) & 0x7Fu);
    }
}

#define UM_RING 128u
static volatile uint8_t um_ring[UM_RING] __attribute__((aligned(16)));
static struct {
    uint32_t rd;
    uint8_t st, got, d0, on;
} um;

static void midi_uart_init(void)               /* before fm1_timer5_start (PORTH RMW) */
{
    fm1_uart1_midi_init(um_ring, UM_RING);
    um.on = 1;
}

static void midi_uart_byte(uint32_t b)
{
    uint32_t need;
    if (b >= 0xF8u)
        return;                                /* realtime keeps running status */
    if (b & 0x80u) {
        um.st = b < 0xF0u ? (uint8_t)b : 0;    /* system common / SysEx cancel running status */
        um.got = 0;
        return;
    }
    if (!um.st)
        return;
    need = (um.st & 0xE0u) == 0xC0u ? 1u : 2u; /* Cx Dx: one data byte */
    if (need == 2u && !um.got) {
        um.d0 = (uint8_t)b;
        um.got = 1;
        return;
    }
    um.got = 0;
    midi_channel_msg(um.st, need == 2u ? um.d0 : b, need == 2u ? b : 0u);
}

static void midi_uart_poll(void)
{
    uint32_t n;
    if (!um.on)
        return;
    n = fm1_uart1_rx_take();
    if (n > UM_RING) {                         /* overrun: resync on the next status byte */
        um.rd = (um.rd + n - UM_RING) & (UM_RING - 1u);
        n = UM_RING;
        um.st = um.got = 0;
    }
    while (n--) {
        midi_uart_byte(um_ring[um.rd]);
        um.rd = (um.rd + 1u) & (UM_RING - 1u);
    }
}

static void midi_poll(void)                    /* TIMER5 ISR */
{
    midi_usb_drain();
    midi_uart_poll();
}

/* main loop: pitch classes pressed on MIDI since the last call */
static uint32_t midi_note_edges(void)
{
    uint32_t k = fm1__lock();
    uint32_t p = midi_pc_pressed;
    midi_pc_pressed = 0;
    fm1__unlock(k);
    return p;
}
