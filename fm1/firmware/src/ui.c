/* SPDX-License-Identifier: GPL-3.0-only */
/* The RB-338 front panel, drawn in code: a rack overview (HOME), the two 303s, the 808, the 909,
 * the mixer and the effects, one module a page, in ReBirth's colours. Included by rb338_main.c
 * (after gfx.c and store.c; uses engine_on, play_req, song_save).
 *
 * Drawing: there is no frame buffer. A page is a static background plus widgets; a region of
 * the screen is drawn by running the whole page clipped to a canvas strip (cv_px) and blitting
 * it. Each widget has a signature of what it shows; when it changes, its box is redrawn.
 *
 * Controls: SELECT pages, HOME/GLO/FX jump to the rack/mixer/effects, ALGORITHM picks the bank
 * K1..K4 turn, PRESETS the pattern, PLAY starts/stops, SEQ toggles song/pattern mode, EDIT the
 * step edit, SAVE (twice) writes the song back to the song slot. Keys: see keys_*(). */

/* ---- palette (ReBirth RB-338) */
#define C_HDR RGB(12, 12, 14)
#define C_RAIL RGB(40, 40, 44)
#define C_TXT RGB(222, 226, 230)
#define C_DIMTXT RGB(120, 124, 130)
#define C_ORANGE RGB(255, 118, 26)
#define C_LEDRED RGB(255, 40, 20)
#define C_LEDOFF RGB(70, 16, 10)
#define C_SEGON RGB(255, 42, 18)
#define C_SEGOFF RGB(34, 8, 6)
#define C_SEGBG RGB(16, 4, 4)
#define C_GREEN RGB(70, 230, 90)
#define C_YELLOW RGB(255, 226, 70)
#define C_SILVER0 RGB(214, 217, 220)
#define C_SILVER1 RGB(166, 170, 175)
#define C_808 RGB(46, 43, 40)
#define C_808LO RGB(28, 26, 24)
#define C_909 RGB(206, 200, 184)
#define C_909LO RGB(60, 58, 54)
#define C_MIX0 RGB(104, 136, 150)
#define C_MIX1 RGB(70, 98, 112)
#define C_PCF RGB(52, 92, 140)
#define C_DLY RGB(46, 104, 98)
#define C_DST RGB(132, 46, 38)
#define C_CMP RGB(78, 80, 90)

/* ---- the region being drawn: canvas cv_px (cv_w x cv_h) at screen (rx0, ry0) */
static int32_t rx0, ry0;

static uint16_t mix(uint16_t o, uint16_t c, uint32_t a) /* a 0..256: o -> c */
{
    uint32_t r = ((o >> 11) * (256u - a) + (c >> 11) * a) >> 8;
    uint32_t g = (((o >> 5) & 63u) * (256u - a) + ((c >> 5) & 63u) * a) >> 8;
    uint32_t b = ((o & 31u) * (256u - a) + (c & 31u) * a) >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void pxa(int32_t x, int32_t y, uint16_t c, uint32_t a)
{
    uint16_t *p;
    x -= rx0;
    y -= ry0;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h || !a)
        return;
    p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    *p = swap16(a >= 256u ? c : mix(swap16(*p), c, a));
}

static int vis(int32_t x, int32_t y, int32_t w, int32_t h)
{
    return x < rx0 + (int32_t)cv_w && x + w > rx0 && y < ry0 + (int32_t)cv_h && y + h > ry0;
}

static void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w - rx0, y1 = y + h - ry0, i;
    uint16_t sc = swap16(c);
    x -= rx0;
    y -= ry0;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int32_t)cv_w)
        x1 = (int32_t)cv_w;
    if (y1 > (int32_t)cv_h)
        y1 = (int32_t)cv_h;
    for (; y < y1; y++)
        for (i = x; i < x1; i++)
            cv_px[(uint32_t)y * cv_w + (uint32_t)i] = sc;
}

static void vgrad(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c0, uint16_t c1)
{
    int32_t i, y0 = ry0 > y ? ry0 - y : 0, y1 = ry0 + (int32_t)cv_h - y;
    if (y1 > h)
        y1 = h;
    for (i = y0; i < y1; i++)
        fill(x, y + i, w, 1, mix(c0, c1, (uint32_t)(i * 256 / (h > 1 ? h - 1 : 1))));
}

static void bevel(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, uint16_t hi, uint16_t lo)
{
    fill(x, y, w, h, c);
    fill(x, y, w, 1, hi);
    fill(x, y, 1, h, hi);
    fill(x, y + h - 1, w, 1, lo);
    fill(x + w - 1, y, 1, h, lo);
}

static void frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

/* sin of d degrees, Q12 (Bhaskara: within 0.002) */
static int32_t isin(int32_t d)
{
    int32_t s = 1, x;
    d %= 360;
    if (d < 0)
        d += 360;
    if (d >= 180) {
        d -= 180;
        s = -1;
    }
    x = d * (180 - d);
    return s * (16384 * x / (40500 - x));
}

/* anti-aliased disc, centre and radius in 1/16 px, alpha 0..256 */
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c, uint32_t al)
{
    int32_t x0 = (cx - r) >> 4, x1 = (cx + r) >> 4, y0 = (cy - r) >> 4, y1 = (cy + r) >> 4, x, y;
    int32_t ri = r > 8 ? (r - 8) * (r - 8) : 0, ro = (r + 8) * (r + 8);
    if (!vis(x0, y0, x1 - x0 + 1, y1 - y0 + 1))
        return;
    for (y = y0; y <= y1; y++) {
        int32_t dy = y * 16 + 8 - cy;
        for (x = x0; x <= x1; x++) {
            int32_t dx = x * 16 + 8 - cx, d2 = dx * dx + dy * dy;
            if (d2 >= ro)
                continue;
            pxa(x, y, c, (d2 <= ri ? 256u : (uint32_t)(r + 8 - (int32_t)isqrt((uint32_t)d2)) * 16u) * al >> 8);
        }
    }
}
#define P16(v) ((v) * 16 + 8)                  /* a pixel's centre in 1/16 px */

/* anti-aliased segment (ends in 1/16 px), half width hw in 1/16 px */
static void seg(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t hw, uint16_t c)
{
    int32_t ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey, x, y;
    int32_t x0 = ((ax < bx ? ax : bx) - hw - 16) >> 4, x1 = ((ax > bx ? ax : bx) + hw + 16) >> 4;
    int32_t y0 = ((ay < by ? ay : by) - hw - 16) >> 4, y1 = ((ay > by ? ay : by) + hw + 16) >> 4;
    if (!vis(x0, y0, x1 - x0 + 1, y1 - y0 + 1))
        return;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            int32_t px = x * 16 + 8 - ax, py = y * 16 + 8 - ay, t = l2 ? px * ex + py * ey : 0, a;
            if (t < 0)
                t = 0;
            if (t > l2)
                t = l2;
            if (l2) {
                px -= (int32_t)((int64_t)ex * t / l2);
                py -= (int32_t)((int64_t)ey * t / l2);
            }
            a = hw + 8 - (int32_t)isqrt((uint32_t)(px * px + py * py));
            if (a > 0)
                pxa(x, y, c, (uint32_t)(a > 16 ? 16 : a) * 16u);
        }
}

/* filled triangle: dir 0 points right (s tall), 1 up / 2 down (s wide) */
static void tri(int32_t x, int32_t y, int32_t s, int dir, uint16_t c)
{
    int32_t i, n = (s + 1) / 2;
    if (dir == 0)
        for (i = 0; i < s; i++)
            fill(x, y + i, (i < s / 2 ? i : s - 1 - i) + 1, 1, c);
    else
        for (i = 0; i < n; i++) {
            int32_t row = dir == 1 ? i : n - 1 - i;
            fill(x + s / 2 - row, y + i, 2 * row + 1, 1, c);
        }
}

/* ---- text: the 5x7 panel font (ASCII 32..95, columns, bit 0 the top) and FONT_S */
static const uint8_t F5[64][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00},
    {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x73}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40},
};

static int32_t w5(const char *s) { return (int32_t)str_len(s) * 6 - 1; }

static int32_t text5(int32_t x, int32_t y, const char *s, uint16_t c)
{
    if (!vis(x, y, w5(s) + 1, 8))
        return x + w5(s) + 1;
    for (; *s; s++) {
        uint32_t ch = (uint8_t)*s, i, j;
        if (ch >= 'a' && ch <= 'z')
            ch -= 32u;
        if (ch < 32u || ch > 95u)
            ch = '?';
        for (i = 0; i < 5u; i++)
            for (j = 0; j < 8u; j++)
                if (F5[ch - 32u][i] >> j & 1u)
                    pxa(x + (int32_t)i, y + (int32_t)j, c, 256);
        x += 6;
    }
    return x;
}

static void text5c(int32_t cx, int32_t y, const char *s, uint16_t c) { text5(cx - w5(s) / 2, y, s, c); }

static int32_t texts(int32_t x, int32_t y, const char *s, uint16_t c) /* FONT_S, blended on what is there */
{
    const felucca_font_t *f = &FONT_S;
    if (!vis(x - 2, y, text_w(f, s) + 4, f->h))
        return x + text_w(f, s);
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v)
                    pxa(x - f->pad + (int32_t)gx, y + (int32_t)gy, c, v * 17u + (v == 15u));
            }
        x += f->adv[gi];
    }
    return x;
}

/* ---- seven-segment LEDs */
static uint32_t seg7_mask(char ch)
{
    static const uint8_t DIG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    static const char LET[] = "ABCDEFGHLNOPRSTU-";
    static const uint8_t LM[] = {0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71, 0x3D, 0x76, 0x38, 0x54, 0x5C, 0x73, 0x50,
                                 0x6D, 0x78, 0x3E, 0x40};
    uint32_t i;
    if (ch >= '0' && ch <= '9')
        return DIG[ch - '0'];
    for (i = 0; LET[i]; i++)
        if (LET[i] == ch)
            return LM[i];
    return 0;
}

static void seg7(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t m, uint16_t on, uint16_t off)
{
    int32_t t = h >= 20 ? 3 : 2, md = (h - t) / 2;
    const int32_t R[7][4] = {
        {x + t, y, w - 2 * t, t},                     /* a */
        {x + w - t, y + 1, t, md},                    /* b */
        {x + w - t, y + md + t - 1, t, h - md - t},   /* c */
        {x + t, y + h - t, w - 2 * t, t},             /* d */
        {x, y + md + t - 1, t, h - md - t},           /* e */
        {x, y + 1, t, md},                            /* f */
        {x + t, y + md, w - 2 * t, t},                /* g */
    };
    uint32_t i;
    for (i = 0; i < 7u; i++)
        fill(R[i][0], R[i][1], R[i][2], R[i][3], (m >> i & 1u) ? on : off);
}

/* a string of digits/letters, pitch apart; '.' lights the point after the digit before it */
static void seg7s(int32_t x, int32_t y, int32_t w, int32_t h, int32_t pitch, const char *s, uint16_t on, uint16_t off)
{
    for (; *s; s++) {
        int dp = s[1] == '.';
        seg7(x, y, w, h, seg7_mask(*s), on, off);
        if (pitch - w >= 3)
            fill(x + w + 1, y + h - 2, 2, 2, dp ? on : off);
        if (dp)
            s++;
        x += pitch;
    }
}

/* ---- panel parts */
static void led(int32_t cx, int32_t cy, int32_t r16, uint16_t c, int on)
{
    if (on) {
        disc(P16(cx), P16(cy), r16 + 24, c, 70);
        disc(P16(cx), P16(cy), r16, c, 256);
        disc(P16(cx) - r16 / 3, P16(cy) - r16 / 3, r16 / 3, C_WHITE, 150);
    } else {
        disc(P16(cx), P16(cy), r16 + 8, C_BLACK, 110);
        disc(P16(cx), P16(cy), r16, mix(c, C_BLACK, 190), 256);
    }
}

typedef struct {
    uint16_t body, cap, ptr, tick, label, focus;
} kstyle_t;

static void knob(int32_t cx, int32_t cy, int32_t r, uint32_t v, uint32_t lo, uint32_t hi, const kstyle_t *st)
{
    int32_t a = -135 + (int32_t)(hi > lo ? (v - lo) * 270u / (hi - lo) : 0), i, s, c;
    if (!vis(cx - r - 5, cy - r - 5, 2 * r + 11, 2 * r + 11))
        return;
    for (i = 0; i <= 10; i++) {
        int32_t ta = -135 + 27 * i;
        disc(P16(cx) + (r + 3) * isin(ta) / 256, P16(cy) - (r + 3) * isin(ta + 90) / 256, 10, st->tick, 256);
    }
    disc(P16(cx) + 16, P16(cy) + 24, r * 16, C_BLACK, 90);
    disc(P16(cx), P16(cy), r * 16, st->body, 256);
    disc(P16(cx), P16(cy), r * 16 - 32, st->cap, 256);
    disc(P16(cx) - r * 5, P16(cy) - r * 5, r * 6, C_WHITE, 40);
    s = isin(a);
    c = isin(a + 90);
    seg(P16(cx) + r * s / 1024, P16(cy) - r * c / 1024, P16(cx) + r * s * 14 / 16 / 256,
        P16(cy) - r * c * 14 / 16 / 256, r > 10 ? 18 : 12, st->ptr);
}

static void slide_sw(int32_t x, int32_t y, int32_t w, int32_t h, int up, uint16_t lever)
{
    bevel(x, y, w, h, RGB(20, 20, 20), RGB(8, 8, 8), RGB(90, 90, 90));
    bevel(x + 2, up ? y + 2 : y + h / 2, w - 4, h / 2 - 2, lever, mix(lever, C_WHITE, 120), mix(lever, C_BLACK, 120));
    fill(x + 3, (up ? y + 2 : y + h / 2) + (h / 2 - 2) / 2, w - 6, 1, mix(lever, C_BLACK, 150));
}

/* ---- the song as the pages see it */
enum { PG_HOME, PG_303A, PG_303B, PG_808, PG_909, PG_MIX, PG_FX, PAGES };   /* 303A..909 = their dev ids */
static const char *const PG_TITLE[PAGES] = {"RB-338", "TB-303 #1", "TB-303 #2", "TR-808", "TR-909", "MIXER",
                                            "EFFECTS"};
static const uint16_t PG_ACCENT[PAGES] = {RGB(235, 235, 235), RGB(255, 140, 50), RGB(255, 140, 50),
                                          RGB(255, 120, 40), RGB(255, 150, 60), RGB(140, 210, 240),
                                          RGB(130, 180, 255)};

static struct {
    uint8_t page, edit, full;
    uint8_t bank[PAGES];
    uint8_t inst[2];                            /* the 808's and the 909's selected column */
    uint8_t cursor[2];                          /* the 303s' edit step */
    uint32_t msg_until, save_armed, enc_ms[FM1_NENC], last_draw;
    char msg[40];
} ui;

static int dev_param(int dev, int idx) { return rb338_get(RB_PARAM, dev, idx); }
static uint32_t sel_pat(int dev) { return (uint32_t)dev_param(dev, 1) & 31u; }
static int pat_get(int dev, uint32_t off) { return rb338_get(RB_PATTERN_BYTE, dev, (int)(sel_pat(dev) * RB_PATTERN_LEN(dev) + off)); }
static void pat_set(int dev, uint32_t off, int v) { rb338_set(RB_PATTERN_BYTE, dev, (int)(sel_pat(dev) * RB_PATTERN_LEN(dev) + off), v); }
static int pat_len(int dev)
{
    int n = pat_get(dev, 1);
    return n < 1 ? 1 : n > 16 ? 16 : n;
}
/* the step playing now on dev's shown pattern, -1 none */
static int play_step(int dev)
{
    if (!rb338_engine_playing() || rb338_get(RB_CUR_PATTERN, dev, 0) != (int)sel_pat(dev))
        return -1;
    return rb338_get(RB_STEP, dev, 0) & 15;
}

static void pat_name(uint32_t p, char *b)
{
    b[0] = (char)('A' + p / 8u);
    b[1] = (char)('1' + p % 8u);
    b[2] = 0;
}

/* ---- controls: what K1..K4 turn in each bank of each page */
enum { S_NONE, S_PARAM, S_TEMPO, S_SHUF, S_MODE, S_LEN, S_PSHUF };
typedef struct {
    const char *name;
    const char *const *opt;                     /* names of the values lo..hi, or NULL: a number */
    uint8_t src, dev, idx, lo, hi, pan;
} ctl_t;

static const char *const O_OFFON[] = {"OFF", "ON"}, *const O_WAVE[] = {"SAW", "SQUARE"},
                  *const O_MODE[] = {"PATTERN", "SONG"}, *const O_PCFM[] = {"BAND", "LOW"},
                  *const O_DLYM[] = {"TRIPLET", "STRAIGHT"},
                  *const O_COMP[] = {"OFF", "MAST", "303-1", "303-2", "808", "909"},
                  *const O_PCFD[] = {"OFF", "303-1", "303-2", "808", "909"}, *const O_TOM[] = {"CONGA", "TOM"},
                  *const O_RS[] = {"CLAVES", "RIM"}, *const O_CP[] = {"MARACAS", "CLAP"};

typedef struct {
    const char *name, *tag;
    uint8_t n, sw, idx[4];
    const char *pn[4];
    const char *const *opt;
} inst_t;
static const inst_t I808[12] = {
    {"ACCENT", "AC", 1, 9, {2}, {"ACCENT"}, 0},
    {"BASS DRUM", "BD", 3, 9, {3, 4, 5}, {"LEVEL", "TONE", "DECAY"}, 0},
    {"SNARE DRUM", "SD", 3, 9, {6, 7, 8}, {"LEVEL", "TONE", "SNAPPY"}, 0},
    {"LOW TOM", "LT", 3, 2, {9, 10, 11}, {"LEVEL", "TUNING", "TOM"}, O_TOM},
    {"MID TOM", "MT", 3, 2, {12, 13, 14}, {"LEVEL", "TUNING", "TOM"}, O_TOM},
    {"HI TOM", "HT", 3, 2, {15, 16, 17}, {"LEVEL", "TUNING", "TOM"}, O_TOM},
    {"RIM SHOT", "RS", 2, 1, {18, 19}, {"LEVEL", "RIM"}, O_RS},
    {"HAND CLAP", "CP", 2, 1, {20, 21}, {"LEVEL", "CLAP"}, O_CP},
    {"COWBELL", "CB", 1, 9, {22}, {"LEVEL"}, 0},
    {"CYMBAL", "CY", 3, 9, {23, 24, 25}, {"LEVEL", "TONE", "DECAY"}, 0},
    {"OPEN HIHAT", "OH", 2, 9, {26, 27}, {"LEVEL", "DECAY"}, 0},
    {"CLOSED HIHAT", "CH", 1, 9, {28}, {"LEVEL"}, 0},
};
static const inst_t I909[12] = {
    {"ACCENT", "AC", 1, 9, {2}, {"ACCENT"}, 0},
    {"BASS DRUM", "BD", 4, 9, {3, 4, 5, 6}, {"LEVEL", "TUNE", "ATTACK", "DECAY"}, 0},
    {"SNARE DRUM", "SD", 4, 9, {7, 8, 9, 10}, {"LEVEL", "TUNE", "TONE", "SNAPPY"}, 0},
    {"LOW TOM", "LT", 3, 9, {11, 12, 13}, {"LEVEL", "TUNE", "DECAY"}, 0},
    {"MID TOM", "MT", 3, 9, {14, 15, 16}, {"LEVEL", "TUNE", "DECAY"}, 0},
    {"HI TOM", "HT", 3, 9, {17, 18, 19}, {"LEVEL", "TUNE", "DECAY"}, 0},
    {"RIM SHOT", "RS", 1, 9, {20}, {"LEVEL"}, 0},
    {"HAND CLAP", "CP", 1, 9, {21}, {"LEVEL"}, 0},
    {"CLOSED HIHAT", "CH", 2, 9, {22, 23}, {"HH LEVEL", "DECAY"}, 0},
    {"OPEN HIHAT", "OH", 2, 9, {22, 24}, {"HH LEVEL", "DECAY"}, 0},
    {"CRASH", "CC", 2, 9, {25, 26}, {"LEVEL", "TUNE"}, 0},
    {"RIDE", "RC", 2, 9, {27, 28}, {"LEVEL", "TUNE"}, 0},
};
static const inst_t *drum_inst(uint32_t page, uint32_t i) { return &(page == PG_808 ? I808 : I909)[i % 12u]; }

static const uint8_t BANKS[PAGES] = {1, 3, 3, 2, 2, 5, 5};

static int mk(ctl_t *c, const char *name, uint32_t src, uint32_t dev, uint32_t idx, uint32_t lo, uint32_t hi,
              const char *const *opt)
{
    c->name = name;
    c->opt = opt;
    c->src = (uint8_t)src;
    c->dev = (uint8_t)dev;
    c->idx = (uint8_t)idx;
    c->lo = (uint8_t)lo;
    c->hi = (uint8_t)hi;
    c->pan = 0;
    return 1;
}
#define PAR(name, dev, idx) mk(c, name, S_PARAM, dev, idx, 0, 127, 0)
#define MIXCH(ch, f) (5u + 6u * (ch) + (f))     /* the mixer's channel field (1 level 2 pan 3 send 4 dist) */

static int bank_ctl(uint32_t page, uint32_t g, uint32_t k, ctl_t *c)
{
    static const char *const MIXF[4] = {"LEVEL", "PAN", "DELAY", "DIST"};
    static const char *const S303[2][4] = {{"CUTOFF", "RESO", "ENV MOD", "DECAY"}, {"TUNE", "ACCENT", "WAVE", "VOLUME"}};
    static const uint8_t I303[2][4] = {{3, 4, 5, 6}, {2, 7, 8, 0}};
    int r;
    c->src = S_NONE;
    if (k > 3u)
        return 0;
    switch (page) {
    case PG_HOME:
        if (k == 0)
            return mk(c, "TEMPO", S_TEMPO, 0, 0, 40, 400, 0);
        if (k == 1)
            return mk(c, "SHUF", S_SHUF, 0, 0, 0, 127, 0);
        if (k == 2)
            return PAR("MASTER", 0, 0);
        return mk(c, "MODE", S_MODE, 0, 0, 0, 1, O_MODE);
    case PG_303A:
    case PG_303B:
        if (g == 2)
            return k == 0 ? mk(c, "LENGTH", S_LEN, page, 0, 1, 16, 0)
                 : k == 1 ? mk(c, "SHUFFLE", S_PSHUF, page, 0, 0, 1, O_OFFON) : 0;
        if (g == 1 && k == 3)
            return PAR("VOLUME", 0, MIXCH(page - 1u, 1u));
        if (g == 1 && k == 2)
            return mk(c, "WAVE", S_PARAM, page, 8, 0, 1, O_WAVE);
        return PAR(S303[g & 1u][k], page, I303[g & 1u][k]);
    case PG_808:
    case PG_909:
        if (g == 0) {
            const inst_t *in = drum_inst(page, ui.inst[page - PG_808]);
            if (k >= in->n)
                return 0;
            r = PAR(in->pn[k], page, in->idx[k]);
            if (k == in->sw) {
                c->opt = in->opt;
                c->hi = 1;
            }
            return r;
        }
        if (k == 0)
            return PAR("VOLUME", 0, MIXCH(page - 1u, 1u));
        if (k == 1) {
            if (page == PG_909)
                return PAR("FLAM", page, 29);
            r = PAR("PAN", 0, MIXCH(page - 1u, 2u));
            c->pan = 1;
            return r;
        }
        return k == 2 ? mk(c, "LENGTH", S_LEN, page, 0, 1, 16, 0) : mk(c, "SHUFFLE", S_PSHUF, page, 0, 0, 1, O_OFFON);
    case PG_MIX:
        if (g < 4u) {
            r = mk(c, MIXF[g], S_PARAM, 0, MIXCH(k, g + 1u), 0, g == 3u ? 1 : 127, g == 3u ? O_OFFON : 0);
            c->pan = g == 1u;
            return r;
        }
        if (k == 0)
            return PAR("MASTER", 0, 0);
        if (k == 1)
            return mk(c, "COMP", S_PARAM, 0, 1, 0, 5, O_COMP);
        return k == 2 ? mk(c, "PCF", S_PARAM, 0, 2, 0, 4, O_PCFD) : 0;
    case PG_FX:
        switch (g) {
        case 0: {
            static const char *const N[4] = {"FREQ", "RES", "AMT", "DCY"};
            static const uint8_t I[4] = {1, 2, 3, 5};
            return PAR(N[k], 7, I[k]);
        }
        case 1:
            return k == 0 ? mk(c, "WAVE", S_PARAM, 7, 4, 0, 55, 0)
                 : k == 1 ? mk(c, "MODE", S_PARAM, 7, 6, 0, 1, O_PCFM)
                 : k == 2 ? mk(c, "CHAN", S_PARAM, 0, 2, 0, 4, O_PCFD) : 0;
        case 2:
            if (k == 0)
                return mk(c, "STEPS", S_PARAM, 5, 1, 1, 16, 0);
            if (k == 1)
                return mk(c, "MODE", S_PARAM, 5, 2, 0, 1, O_DLYM);
            if (k == 2)
                return PAR("FEEDBACK", 5, 3);
            r = PAR("PAN", 5, 4);
            c->pan = 1;
            return r;
        case 3:
            return k == 0 ? PAR("AMOUNT", 6, 1) : k == 1 ? PAR("SHAPE", 6, 2) : 0;
        default:
            return k == 0 ? PAR("RATIO", 8, 1) : k == 1 ? PAR("THRESH", 8, 2)
                 : k == 2 ? mk(c, "CHAN", S_PARAM, 0, 1, 0, 5, O_COMP) : 0;
        }
    default:
        return 0;
    }
}

static int ctl_get(const ctl_t *c)
{
    switch (c->src) {
    case S_PARAM: return dev_param(c->dev, c->idx);
    case S_TEMPO: return (rb338_get(RB_TEMPO, 0, 0) + 500) / 1000;
    case S_SHUF: return rb338_get(RB_SHUFFLE, 0, 0);
    case S_MODE: return rb338_get(RB_SONG_MODE, 0, 0) != 0;
    case S_LEN: return pat_len(c->dev);
    case S_PSHUF: return pat_get(c->dev, 0) != 0;
    default: return 0;
    }
}

static void ctl_set(const ctl_t *c, int v)
{
    if (v < c->lo)
        v = c->lo;
    if (v > c->hi)
        v = c->hi;
    switch (c->src) {
    case S_PARAM: rb338_set(RB_PARAM, c->dev, c->idx, v); break;
    case S_TEMPO: rb338_set(RB_TEMPO, 0, 0, v * 1000); break;
    case S_SHUF: rb338_set(RB_SHUFFLE, 0, 0, v); break;
    case S_MODE: rb338_set(RB_SONG_MODE, 0, 0, v); break;
    case S_LEN: pat_set(c->dev, 1, v); break;
    case S_PSHUF: pat_set(c->dev, 0, v); break;
    default: break;
    }
}

static void ctl_text(const ctl_t *c, int v, char *b)
{
    if (c->opt && v >= c->lo && v <= c->hi)
        str_cpy(b, c->opt[v - c->lo], 12);
    else if (c->pan && v != 64) {
        b[0] = v < 64 ? 'L' : 'R';
        fmt_int(b + 1, v < 64 ? 64 - v : v - 64);
    } else if (c->pan)
        str_cpy(b, "C", 2);
    else
        fmt_int(b, v);
}

/* ---- widgets */
enum {
    W_HDR, W_FOOT, W_CTL, W_FADER, W_BTN, W_STEP3, W_STEPD, W_INST, W_INAME, W_PAT, W_LEN, W_PSHUF, W_EDIT,
    W_MUTE, W_METER, W_UNIT, W_TEMPO, W_PLAY, W_MODE, W_POS, W_HMUTE, W_HPAT, W_HSTEP, W_FXLED,
};
typedef struct {
    uint8_t kind, a, b, x, y, w, h;
} wdg_t;
#define NW 112u
static wdg_t wg[NW];
static uint32_t nwg, wsig[NW];

static void wadd(uint32_t kind, uint32_t a, uint32_t b, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    wdg_t *p;
    if (nwg >= NW)
        return;
    p = &wg[nwg++];
    p->kind = (uint8_t)kind;
    p->a = (uint8_t)a;
    p->b = (uint8_t)b;
    p->x = (uint8_t)x;
    p->y = (uint8_t)y;
    p->w = (uint8_t)w;
    p->h = (uint8_t)h;
}

static const kstyle_t K303 = {RGB(22, 22, 22), RGB(44, 44, 46), RGB(245, 245, 245), RGB(60, 62, 66),
                              RGB(30, 30, 32), RGB(214, 70, 20)};
static const kstyle_t K808 = {RGB(150, 144, 132), RGB(222, 216, 202), RGB(30, 30, 30), RGB(150, 144, 130),
                              RGB(222, 214, 196), RGB(255, 150, 40)};
static const kstyle_t K909 = {RGB(34, 34, 34), RGB(58, 58, 58), RGB(255, 130, 50), RGB(96, 92, 84),
                              RGB(40, 40, 40), RGB(214, 80, 20)};
static const kstyle_t KMIX = {RGB(22, 22, 24), RGB(46, 48, 52), RGB(250, 250, 250), RGB(36, 52, 60),
                              RGB(232, 240, 244), RGB(255, 200, 60)};

static const kstyle_t *page_style(uint32_t p)
{
    return p == PG_303A || p == PG_303B ? &K303 : p == PG_808 ? &K808 : p == PG_909 ? &K909 : &KMIX;
}

static uint32_t hsh(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static uint32_t blink(void) { return (fm1_ms / 250u) & 1u; }

static void note_text(uint32_t n, char *b)
{
    static const char NAMES[] = "C C#D D#E F F#G G#A A#B ";
    b[0] = NAMES[2u * (n % 12u)];
    b[1] = NAMES[2u * (n % 12u) + 1u] == ' ' ? 0 : '#';
    b[2] = 0;
}

static const uint16_t STEP808[4] = {RGB(222, 60, 40), RGB(246, 146, 40), RGB(244, 210, 64), RGB(236, 230, 210)};

/* signature (draw == 0) or drawing of a widget */
static uint32_t wdg(const wdg_t *w, int draw)
{
    int32_t x = w->x, y = w->y, ww = w->w, hh = w->h;
    uint32_t p = ui.page, h = w->kind * 7919u + w->a * 131u + w->b, i;
    const kstyle_t *st = page_style(p);
    char b[40];
    ctl_t c;
    switch (w->kind) {
    case W_HDR: {
        int play = rb338_engine_playing(), bpm = (rb338_get(RB_TEMPO, 0, 0) + 50) / 100;
        if (!draw)
            return hsh(hsh(h, (uint32_t)play), (uint32_t)bpm);
        fill(x, y, ww, hh, C_HDR);
        fill(x, y + hh - 1, ww, 1, C_RAIL);
        if (play)
            tri(x + 4, y + 4, 11, 0, C_GREEN);
        else
            fill(x + 5, y + 6, 8, 8, C_DIMTXT);
        fmt_fix(b, bpm, 1);
        while (str_len(b) < 5u) {
            for (i = str_len(b) + 1u; i > 0; i--)
                b[i] = b[i - 1u];
            b[0] = ' ';
        }
        fill(x + 20, y + 2, 66, 15, C_SEGBG);
        seg7s(x + 23, y + 3, 8, 13, 13, b, C_SEGON, C_SEGOFF);
        return 0;
    }
    case W_FOOT: {
        int msg = (int32_t)(ui.msg_until - fm1_ms) > 0;
        if (msg)
            str_cpy(b, ui.msg, sizeof b);
        else if (ui.edit && (p == PG_303A || p == PG_303B))
            str_cpy(b, "KEY NOTE OCT<> STEP ENV ACC LFO SLIDE", sizeof b);
        else if (ui.edit && (p == PG_808 || p == PG_909))
            str_cpy(b, "WHITE STEPS  BLACK/OCT INSTRUMENT", sizeof b);
        else {
            char *e = b;
            for (i = 0; i < 4u; i++)
                if (bank_ctl(p, ui.bank[p], i, &c)) {
                    const char *s = c.name;
                    if (e != b)
                        *e++ = ' ';
                    *e++ = (char)('1' + i);
                    *e++ = ':';
                    while (*s && e < b + 38)
                        *e++ = *s++;
                }
            *e = 0;
        }
        if (!draw) {
            for (i = 0; b[i]; i++)
                h = hsh(h, (uint8_t)b[i]);
            return hsh(h, (uint32_t)msg);
        }
        fill(x, y, ww, hh, C_HDR);
        fill(x, y, ww, 1, C_RAIL);
        if (BANKS[p] > 1u && !msg && !ui.edit) {
            fill(x + 2, y + 3, 11, 11, PG_ACCENT[p]);
            b[39] = 0;
            {
                char g[2] = {(char)('1' + ui.bank[p]), 0};
                text5(x + 5, y + 5, g, C_HDR);
            }
            text5(x + 17, y + 5, b, C_TXT);
        } else
            text5(x + 4, y + 5, b, msg ? C_YELLOW : C_TXT);
        return 0;
    }
    case W_CTL:
    case W_FADER:
    case W_BTN: {
        uint32_t g = w->a == 255u ? ui.bank[p] : w->a;
        int ok = bank_ctl(p, g, w->b, &c), v = ok ? ctl_get(&c) : 0, foc = g == ui.bank[p] && BANKS[p] > 1u;
        uint16_t lc = foc ? st->focus : st->label;
        if (!draw)
            return hsh(hsh(hsh(h, (uint32_t)ok), (uint32_t)v), (uint32_t)foc * 3u + g * 5u);
        if (w->a == 255u)
            fill(x, y, ww, hh, p == PG_808 ? C_808 : C_909);
        if (!ok)
            return 0;
        if (w->kind == W_FADER) {
            int32_t cx = x + ww / 2, top = y + 6, len = hh - 14, cy = top + len - (int32_t)((uint32_t)v * (uint32_t)len / 127u);
            for (i = 0; i <= 10u; i++)
                fill(cx - 12, top + (int32_t)i * len / 10, i % 5u ? 4 : 6, 1, mix(C_MIX1, C_BLACK, 120));
            fill(cx - 2, top - 2, 5, len + 4, RGB(14, 16, 18));
            fill(cx - 1, top - 1, 1, len + 2, RGB(50, 56, 60));
            bevel(cx - 9, cy - 5, 19, 10, RGB(40, 42, 46), RGB(110, 114, 120), RGB(14, 14, 16));
            fill(cx - 7, cy, 15, 1, foc ? st->focus : C_WHITE);
            return 0;
        }
        if (w->kind == W_BTN) {
            bevel(x + 3, y + 2, ww - 6, hh - 4, RGB(32, 34, 38), RGB(90, 94, 100), RGB(10, 10, 12));
            led(x + 10, y + hh / 2, 40, C_ORANGE, v != 0);
            text5(x + 17, y + hh / 2 - 3, c.name, lc);
            return 0;
        }
        if (c.opt && c.hi - c.lo == 1u) {
            int32_t sw = ww >= 40 ? 12 : 10, sh = sw * 2, sx = x + ww / 2 - sw / 2 - (ww >= 50 ? 10 : 0),
                    sy = y + (hh - 10) / 2 - sh / 2;
            slide_sw(sx, sy, sw, sh, v != 0, mix(st->cap, C_WHITE, 60));
            if (ww >= 50) {
                text5(sx + sw + 4, sy + 1, c.opt[1], v ? lc : mix(lc, p == PG_303A || p == PG_303B ? C_SILVER1 : C_MIX1, 150));
                text5(sx + sw + 4, sy + sh - 8, c.opt[0], !v ? lc : mix(lc, p == PG_303A || p == PG_303B ? C_SILVER1 : C_MIX1, 150));
            }
        } else if (c.opt) {
            /* a selector: its name above, the value below */
            int32_t r = (ww - 8 < hh - 26 ? ww - 8 : hh - 26) / 2 - 2;
            text5c(x + ww / 2, y + 1, c.name, lc);
            knob(x + ww / 2, y + 9 + (hh - 18) / 2, r, (uint32_t)v, c.lo, c.hi, st);
            ctl_text(&c, v, b);
            text5c(x + ww / 2, y + hh - 9, b, p == PG_MIX || p == PG_FX ? RGB(255, 210, 90) : C_ORANGE);
            return 0;
        } else {
            int32_t r = (ww - 8 < hh - 16 ? ww - 8 : hh - 16) / 2 - 2;
            knob(x + ww / 2, y + (hh - 10) / 2, r, (uint32_t)v, c.lo, c.hi, st);
        }
        text5c(x + ww / 2, y + hh - 9, c.name, lc);
        if (foc)
            fill(x + ww / 2 - w5(c.name) / 2, y + hh - 1, w5(c.name), 1, lc);
        return 0;
    }
    case W_STEP3: {
        int dev = (int)p, k = w->a, len = pat_len(dev), ps = play_step(dev), cur = ui.edit && ui.cursor[dev - 1] == k;
        uint8_t s[6], prev_slide = k > 0 && pat_get(dev, 2u + 6u * (uint32_t)(k - 1) + 1u);
        for (i = 0; i < 6u; i++)
            s[i] = (uint8_t)pat_get(dev, 2u + 6u * (uint32_t)k + i);
        if (!draw) {
            for (i = 0; i < 6u; i++)
                h = hsh(h, s[i]);
            return hsh(hsh(hsh(h, (uint32_t)(k < len)), (uint32_t)(ps == k) * 2u + (uint32_t)cur), prev_slide);
        }
        {
            int32_t cx = x + ww / 2, oct = (int32_t)s[3] - (int32_t)s[4] + s[0] / 12;
            uint16_t tc = k < len ? RGB(20, 20, 22) : RGB(120, 124, 128);
            fill(x, y, ww, hh, C_SILVER1);
            if (s[5])
                text5c(cx, y + 3, "-", tc);
            else {
                note_text(s[0], b);
                text5c(cx, y + 3, b, tc);
            }
            if (!s[5] && oct > 0)
                tri(cx - 3, y + 12, 7, 1, RGB(200, 40, 20));
            if (!s[5] && oct < 0)
                tri(cx - 3, y + 12, 7, 2, RGB(40, 60, 200));
            led(cx, y + 23, 32, C_ORANGE, s[2] != 0);
            if (s[1])
                fill(cx, y + 30, ww - ww / 2, 3, RGB(30, 60, 190));
            if (prev_slide)
                fill(x, y + 30, ww / 2 + 1, 3, RGB(30, 60, 190));
            bevel(x + 2, y + 36, ww - 4, hh - 38, k < len ? RGB(28, 28, 30) : RGB(90, 92, 96), RGB(80, 80, 84),
                  RGB(6, 6, 6));
            led(cx, y + 41, 30, C_LEDRED, ps == k);
            if (cur)
                frame(x, y, ww, hh, C_ORANGE);
        }
        return 0;
    }
    case W_STEPD: {
        int dev = (int)p, k = w->a, col = ui.inst[dev - PG_808], len = pat_len(dev), ps = play_step(dev);
        int v = pat_get(dev, 2u + 12u * (uint32_t)k + (uint32_t)col);
        if (!draw)
            return hsh(hsh(hsh(h, (uint32_t)v), (uint32_t)(k < len)), (uint32_t)(ps == k) + (uint32_t)ui.edit * 2u);
        {
            int32_t cx = x + ww / 2;
            uint16_t kc = p == PG_808 ? STEP808[k / 4] : RGB(214, 210, 200);
            fill(x, y, ww, hh, p == PG_808 ? C_808LO : C_909LO);
            if (k >= len)
                kc = mix(kc, C_BLACK, 150);
            if (ps == k)
                disc(P16(cx), P16(y + 7), 80, C_YELLOW, 90);
            led(cx, y + 7, 40, C_LEDRED, v != 0);
            if (v == 1 && p == PG_909)
                disc(P16(cx), P16(y + 7), 40, C_BLACK, 120);
            if (v == 3)
                disc(P16(cx), P16(y + 7), 14, C_WHITE, 256);
            bevel(x + 1, y + 15, ww - 2, 34, ps == k ? mix(kc, C_WHITE, 110) : kc, mix(kc, C_WHITE, 110),
                  mix(kc, C_BLACK, 140));
            if (ui.edit)
                fill(x + 3, y + 46, ww - 6, 1, mix(kc, C_BLACK, 90));
            fmt_int(b, k + 1);
            text5c(cx, y + 53, b, p == PG_808 ? RGB(200, 194, 180) : RGB(200, 196, 186));
        }
        return 0;
    }
    case W_INST: {
        const inst_t *in = drum_inst(p, w->a);
        int sel = ui.inst[p - PG_808] == w->a, v = dev_param((int)p, in->idx[0]);
        if (!draw)
            return hsh(hsh(h, (uint32_t)sel), (uint32_t)v);
        fill(x, y, ww, hh, p == PG_808 ? C_808 : C_909);
        if (w->a)
            fill(x, y + 6, 1, hh - 12, p == PG_808 ? RGB(70, 66, 60) : RGB(170, 164, 150));
        knob(x + ww / 2, y + 14, 6, (uint32_t)v, 0, 127, st);
        text5c(x + ww / 2, y + 28, in->tag, sel ? st->focus : st->label);
        led(x + ww / 2, y + 42, 40, C_ORANGE, sel);
        return 0;
    }
    case W_INAME: {
        uint32_t g = ui.bank[p];
        const char *s = g ? "MACHINE" : drum_inst(p, ui.inst[p - PG_808])->name;
        if (!draw)
            return hsh(hsh(h, g), ui.inst[p - PG_808]);
        fill(x, y, ww, hh, p == PG_808 ? C_808 : C_909);
        texts(x + 6, y, s, p == PG_808 ? RGB(255, 140, 40) : RGB(214, 80, 20));
        return 0;
    }
    case W_PAT: {
        int dev = (int)p;
        uint32_t sp = sel_pat(dev), cp = (uint32_t)rb338_get(RB_CUR_PATTERN, dev, 0) & 31u;
        int pend = sp != cp && rb338_engine_playing(), off = pend && blink();
        if (!draw)
            return hsh(hsh(h, sp), (uint32_t)off);
        fill(x, y, ww, hh, C_SEGBG);
        text5(x + 3, y + 5, "PAT", C_DIMTXT);
        pat_name(sp, b);
        seg7s(x + 25, y + 2, 8, 12, 12, b, off ? C_SEGOFF : C_SEGON, C_SEGOFF);
        return 0;
    }
    case W_LEN:
    case W_PSHUF:
    case W_EDIT: {
        int dev = (int)p, v = w->kind == W_LEN ? pat_len(dev) : w->kind == W_PSHUF ? pat_get(dev, 0) != 0 : ui.edit;
        int foc = w->kind != W_EDIT && ui.bank[p] == (p <= PG_303B ? 2u : 1u);
        uint16_t lc = foc ? C_ORANGE : C_DIMTXT;
        if (!draw)
            return hsh(hsh(h, (uint32_t)v), (uint32_t)foc);
        fill(x, y, ww, hh, C_SEGBG);
        if (w->kind == W_LEN) {
            text5(x + 3, y + 5, "LEN", lc);
            fmt_int(b, v);
            if (v < 10) {
                b[1] = b[0];
                b[0] = ' ';
                b[2] = 0;
            }
            seg7s(x + 25, y + 2, 8, 12, 12, b, C_SEGON, C_SEGOFF);
        } else {
            text5(x + 3, y + 5, w->kind == W_EDIT ? "EDIT" : "SHUF", lc);
            led(x + ww - 9, y + hh / 2, 44, w->kind == W_EDIT ? C_LEDRED : C_ORANGE, v);
        }
        return 0;
    }
    case W_MUTE:
    case W_HMUTE: {
        int on = dev_param(0, (int)MIXCH(w->a, 0u)) != 0;
        if (!draw)
            return hsh(h, (uint32_t)on);
        if (w->kind == W_MUTE) {
            fill(x, y, ww, hh, C_MIX1);
            bevel(x + 5, y + 1, ww - 10, hh - 2, RGB(30, 34, 38), RGB(96, 104, 110), RGB(8, 10, 12));
            led(x + 13, y + hh / 2, 40, C_GREEN, on);
            text5(x + 21, y + hh / 2 - 3, on ? "ON" : "OFF", on ? C_TXT : C_DIMTXT);
        } else {
            static const uint16_t RB[4] = {C_SILVER1, C_SILVER1, C_808, C_909};
            fill(x, y, ww, hh, RB[w->a]);
            led(x + 5, y + hh / 2, 36, C_GREEN, on);
            text5(x + 12, y + hh / 2 - 3, on ? "ON" : "MUTE", w->a == 2 ? C_TXT : RGB(30, 30, 30));
        }
        return 0;
    }
    case W_METER: {
        uint32_t v = (uint32_t)rb338_get(RB_METER, 0, w->a), n = (v * (uint32_t)(hh - 2) / 127u) & ~1u;
        if (!draw)
            return hsh(h, n);
        fill(x, y, ww, hh, RGB(10, 12, 14));
        for (i = 0; i + 2u < (uint32_t)hh; i += 2u)
            fill(x + 1, y + hh - 2 - (int32_t)i, ww - 2, 1,
                 i < n ? (i > (uint32_t)hh * 4u / 5u ? C_LEDRED : i > (uint32_t)hh * 3u / 5u ? C_YELLOW : C_GREEN)
                       : RGB(26, 30, 34));
        return 0;
    }
    case W_UNIT:
    case W_FXLED: {
        static const char *const NAME[4] = {"PCF", "DELAY", "DIST", "COMP"};
        static const uint8_t DEV[4] = {7, 5, 6, 8};
        static const uint16_t COL[4] = {C_PCF, C_DLY, C_DST, C_CMP};
        static const uint8_t UB[4] = {0, 2, 3, 4};
        int on = dev_param(DEV[w->a], 0) != 0, foc = w->kind == W_UNIT && (ui.bank[p] == UB[w->a] || (w->a == 0 && ui.bank[p] == 1u));
        if (!draw)
            return hsh(hsh(h, (uint32_t)on), (uint32_t)foc);
        if (w->kind == W_FXLED) {
            fill(x, y, ww, hh, C_MIX1);
            led(x + ww / 2, y + 6, 44, C_ORANGE, on);
            text5c(x + ww / 2, y + 14, w->a == 1 ? "DLY" : w->a == 2 ? "DST" : NAME[w->a], C_TXT);
            return 0;
        }
        bevel(x, y, ww, hh, mix(COL[w->a], C_BLACK, 70), mix(COL[w->a], C_WHITE, 70), mix(COL[w->a], C_BLACK, 160));
        texts(x + 4, y + 5, NAME[w->a], foc ? RGB(255, 210, 90) : C_WHITE);
        led(x + 9, y + hh - 11, 44, C_LEDRED, on);
        text5(x + 17, y + hh - 14, "ON", C_TXT);
        return 0;
    }
    case W_TEMPO: {
        int t = (rb338_get(RB_TEMPO, 0, 0) + 50) / 100;
        if (!draw)
            return hsh(h, (uint32_t)t);
        fill(x, y, ww, hh, C_SEGBG);
        frame(x, y, ww, hh, RGB(60, 20, 16));
        fmt_fix(b, t, 1);
        while (str_len(b) < 5u) {
            for (i = str_len(b) + 1u; i > 0; i--)
                b[i] = b[i - 1u];
            b[0] = ' ';
        }
        seg7s(x + 5, y + 5, 14, 24, 21, b, C_SEGON, C_SEGOFF);
        return 0;
    }
    case W_PLAY: {
        int play = rb338_engine_playing();
        if (!draw)
            return hsh(h, (uint32_t)play);
        fill(x, y, ww, hh, C_HDR);
        bevel(x, y, ww, hh, RGB(34, 34, 36), RGB(80, 80, 84), RGB(6, 6, 6));
        if (play)
            tri(x + ww / 2 - 4, y + 5, 13, 0, C_GREEN);
        else
            fill(x + ww / 2 - 5, y + 6, 11, 11, C_DIMTXT);
        return 0;
    }
    case W_MODE: {
        int m = rb338_get(RB_SONG_MODE, 0, 0) != 0;
        if (!draw)
            return hsh(h, (uint32_t)m);
        fill(x, y, ww, hh, C_HDR);
        led(x + 5, y + 4, 36, C_ORANGE, !m);
        text5(x + 12, y + 1, "PATTERN", m ? C_DIMTXT : C_TXT);
        led(x + 5, y + 15, 36, C_ORANGE, m);
        text5(x + 12, y + 12, "SONG", m ? C_TXT : C_DIMTXT);
        return 0;
    }
    case W_POS: {
        int t = rb338_get(RB_TICK, 0, 0), bar = t >= 0 ? t / 768 + 1 : 0, beat = t >= 0 ? t / 192 % 4 + 1 : 0;
        if (!draw)
            return hsh(hsh(h, (uint32_t)bar), (uint32_t)beat);
        fill(x, y, ww, hh, C_SEGBG);
        fmt_int(b, bar % 1000);
        while (str_len(b) < 3u) {
            for (i = str_len(b) + 1u; i > 0; i--)
                b[i] = b[i - 1u];
            b[0] = ' ';
        }
        b[3] = (char)('0' + beat);
        b[4] = 0;
        seg7s(x + 3, y + 3, 8, 13, 11, b, C_SEGON, C_SEGOFF);
        fill(x + 35, y + 13, 2, 2, C_SEGON);
        return 0;
    }
    case W_HPAT: {
        int dev = w->a + 1;
        uint32_t sp = sel_pat(dev), cp = (uint32_t)rb338_get(RB_CUR_PATTERN, dev, 0) & 31u;
        int off = sp != cp && rb338_engine_playing() && blink();
        if (!draw)
            return hsh(hsh(h, sp), (uint32_t)off);
        fill(x, y, ww, hh, C_SEGBG);
        pat_name(sp, b);
        seg7s(x + 3, y + 2, 8, 13, 11, b, off ? C_SEGOFF : C_SEGON, C_SEGOFF);
        return 0;
    }
    case W_HSTEP: {
        int dev = w->a + 1, k = w->b, on = 0, ps = play_step(dev), in = k < pat_len(dev);
        static const uint16_t BG[4] = {C_SILVER1, C_SILVER1, C_808, C_909};
        if (dev <= 2)
            on = !pat_get(dev, 2u + 6u * (uint32_t)k + 5u);
        else
            for (i = 1; i < 12u; i++)
                on |= pat_get(dev, 2u + 12u * (uint32_t)k + i) != 0;
        if (!draw)
            return hsh(hsh(h, (uint32_t)on + 2u * (uint32_t)in), (uint32_t)(ps == k));
        fill(x, y, ww, hh, BG[w->a]);
        {
            uint16_t kc = dev == 3 ? STEP808[k / 4] : dev == 4 ? RGB(226, 222, 212) : RGB(36, 36, 38);
            if (!in)
                kc = mix(kc, BG[w->a], 150);
            bevel(x + 1, y + 6, ww - 2, hh - 6, ps == k ? C_WHITE : kc, mix(kc, C_WHITE, 90), mix(kc, C_BLACK, 120));
            led(x + ww / 2, y + 2, 26, C_LEDRED, on && in);
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* ---- pages: static backgrounds and widget layouts */
static char ui_title[23];

static void title_load(void) /* st_data checks the song's CRC: once per full redraw, not per region */
{
    uint32_t len = 0, i;
    const uint8_t *song = st_data(AREA_SONG, &len);
    for (i = 0; i < 22u; i++)
        ui_title[i] = song && len >= 68u && song[36 + i] >= 0x20u && song[36 + i] < 0x7Fu ? (char)song[36 + i] : 0;
    ui_title[22] = 0;
    if (!ui_title[0])
        str_cpy(ui_title, song ? "SONG" : "NEW SONG", sizeof ui_title);
}

static void bg_draw(void)
{
    uint32_t p = ui.page, i;
    fill(0, 0, 240, 20, C_HDR);
    fill(0, 19, 240, 1, C_RAIL);
    texts(6, 2, PG_TITLE[p], PG_ACCENT[p]);
    for (i = 0; i < PAGES; i++)
        disc(P16(104 + 7 * (int32_t)i), P16(10), i == p ? 36 : 24, i == p ? PG_ACCENT[p] : RGB(80, 80, 86), 256);
    switch (p) {
    case PG_HOME: {
        static const char *const NM[4] = {"303 #1", "303 #2", "TR-808", "TR-909"};
        static const uint16_t BG[4] = {C_SILVER1, C_SILVER1, C_808, C_909};
        fill(0, 20, 240, 52, C_HDR);
        frame(0, 20, 240, 52, C_RAIL);
        text5(6, 24, "TEMPO", C_DIMTXT);
        text5(102, 24, "BAR.BEAT", C_DIMTXT);
        text5(102, 60, ui_title, RGB(150, 200, 230));
        for (i = 0; i < 4u; i++) {
            int32_t y0 = 72 + 26 * (int32_t)i;
            vgrad(0, y0, 240, 26, mix(BG[i], C_WHITE, 40), mix(BG[i], C_BLACK, 40));
            fill(0, y0 + 25, 240, 1, C_BLACK);
            text5(4, y0 + 3, NM[i], i == 2 ? C_TXT : RGB(30, 30, 30));
        }
        vgrad(0, 176, 240, 48, C_MIX0, C_MIX1);
        text5(4, 179, "MIXER", C_TXT);
        text5(76, 179, "EFFECTS", C_TXT);
        break;
    }
    case PG_303A:
    case PG_303B:
        vgrad(0, 20, 240, 122, C_SILVER0, C_SILVER1);
        fill(0, 20, 240, 15, RGB(30, 30, 32));
        text5(6, 24, "BASS LINE", RGB(255, 110, 30));
        text5(234 - w5(p == PG_303A ? "COMPUTER CONTROLLED  1" : "COMPUTER CONTROLLED  2"), 24,
              p == PG_303A ? "COMPUTER CONTROLLED  1" : "COMPUTER CONTROLLED  2", C_DIMTXT);
        fill(0, 87, 240, 1, mix(C_SILVER1, C_BLACK, 60));
        fill(0, 142, 240, 19, C_SEGBG);
        fill(0, 161, 240, 63, C_SILVER1);
        break;
    case PG_808:
    case PG_909: {
        int is8 = p == PG_808;
        fill(0, 20, 240, 122, is8 ? C_808 : C_909);
        fill(0, 73, 240, 2, is8 ? RGB(240, 110, 30) : RGB(214, 80, 20));
        if (!is8)
            fill(0, 76, 240, 1, RGB(170, 164, 150));
        fill(0, 142, 240, 19, C_SEGBG);
        fill(0, 161, 240, 63, is8 ? C_808LO : C_909LO);
        text5(234 - w5(is8 ? "RHYTHM COMPOSER" : "RHYTHM COMPOSER"), 80, "RHYTHM COMPOSER",
              is8 ? RGB(150, 144, 130) : RGB(110, 104, 94));
        break;
    }
    case PG_MIX: {
        static const char *const NM[5] = {"303 #1", "303 #2", "808", "909", "MASTER"};
        vgrad(0, 20, 240, 204, C_MIX0, C_MIX1);
        for (i = 0; i < 5u; i++) {
            if (i)
                fill(48 * (int32_t)i, 20, 1, 204, mix(C_MIX1, C_BLACK, 120));
            text5c(48 * (int32_t)i + 24, 23, NM[i], i == 4 ? RGB(255, 210, 90) : C_TXT);
        }
        break;
    }
    case PG_FX: {
        static const uint16_t COL[4] = {C_PCF, C_DLY, C_DST, C_CMP};
        for (i = 0; i < 4u; i++) {
            int32_t y0 = 20 + 51 * (int32_t)i;
            vgrad(0, y0, 240, 50, mix(COL[i], C_WHITE, 30), mix(COL[i], C_BLACK, 50));
            fill(0, y0 + 50, 240, 1, C_BLACK);
        }
        break;
    }
    default:
        break;
    }
}

static void page_build(void)
{
    uint32_t p = ui.page, k, m;
    nwg = 0;
    wadd(W_HDR, 0, 0, 150, 0, 90, 20);
    wadd(W_FOOT, 0, 0, 0, 224, 240, 16);
    switch (p) {
    case PG_HOME:
        wadd(W_TEMPO, 0, 0, 4, 33, 94, 34);
        wadd(W_POS, 0, 0, 102, 33, 50, 19);
        wadd(W_PLAY, 0, 0, 156, 31, 24, 23);
        wadd(W_MODE, 0, 0, 184, 33, 56, 22);
        for (m = 0; m < 4u; m++) {
            int32_t y0 = 72 + 26 * (int32_t)m;
            wadd(W_HMUTE, m, 0, 2, (uint32_t)y0 + 13, 36, 11);
            wadd(W_HPAT, m, 0, 42, (uint32_t)y0 + 4, 26, 18);
            for (k = 0; k < 16u; k++)
                wadd(W_HSTEP, m, k, 72 + 10 * k, (uint32_t)y0 + 3, 10, 20);
        }
        for (k = 0; k < 6u; k++)
            wadd(W_METER, k, 0, 6 + 9 * k + (k >= 4u ? 4 : 0), 190, 7, 31);
        for (k = 0; k < 4u; k++)
            wadd(W_FXLED, k, 0, 70 + 24 * k, 192, 24, 24);
        wadd(W_CTL, 0, 1, 168, 178, 36, 46);
        wadd(W_CTL, 0, 2, 204, 178, 36, 46);
        break;
    case PG_303A:
    case PG_303B:
        for (k = 0; k < 4u; k++) {
            wadd(W_CTL, 0, k, 60 * k, 36, 60, 51);
            wadd(W_CTL, 1, k, 60 * k, 89, 60, 52);
        }
        wadd(W_PAT, 0, 0, 2, 143, 54, 17);
        wadd(W_LEN, 0, 0, 62, 143, 54, 17);
        wadd(W_PSHUF, 0, 0, 122, 143, 54, 17);
        wadd(W_EDIT, 0, 0, 182, 143, 56, 17);
        for (k = 0; k < 16u; k++)
            wadd(W_STEP3, k, 0, 15 * k, 161, 15, 63);
        break;
    case PG_808:
    case PG_909:
        for (k = 0; k < 12u; k++)
            wadd(W_INST, k, 0, 20 * k, 20, 20, 53);
        wadd(W_INAME, 0, 0, 0, 77, 140, 16);
        for (k = 0; k < 4u; k++)
            wadd(W_CTL, 255, k, 60 * k, 93, 60, 49);
        wadd(W_PAT, 0, 0, 2, 143, 54, 17);
        wadd(W_LEN, 0, 0, 62, 143, 54, 17);
        wadd(W_PSHUF, 0, 0, 122, 143, 54, 17);
        wadd(W_EDIT, 0, 0, 182, 143, 56, 17);
        for (k = 0; k < 16u; k++)
            wadd(W_STEPD, k, 0, 15 * k, 161, 15, 63);
        break;
    case PG_MIX:
        for (k = 0; k < 4u; k++) {
            wadd(W_BTN, 3, k, 48 * k + 1, 32, 47, 18);
            wadd(W_CTL, 2, k, 48 * k + 1, 51, 47, 42);
            wadd(W_CTL, 1, k, 48 * k + 1, 93, 47, 42);
            wadd(W_FADER, 0, k, 48 * k + 1, 136, 36, 68);
            wadd(W_METER, k, 0, 48 * k + 38, 140, 6, 58);
            wadd(W_MUTE, k, 0, 48 * k + 1, 205, 47, 18);
        }
        wadd(W_CTL, 4, 1, 193, 32, 47, 50);
        wadd(W_CTL, 4, 2, 193, 84, 47, 50);
        wadd(W_FADER, 4, 0, 193, 136, 36, 68);
        wadd(W_METER, 4, 0, 230, 140, 4, 58);
        wadd(W_METER, 5, 0, 235, 140, 4, 58);
        break;
    case PG_FX: {
        static const uint8_t G[16] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 4};
        static const uint8_t K[16] = {0, 1, 2, 3, 0, 1, 2, 0, 1, 2, 3, 0, 1, 0, 1, 2};
        static const uint8_t U[16] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3};
        static const uint8_t X[16] = {0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 0, 1, 0, 1, 2};
        for (k = 0; k < 4u; k++)
            wadd(W_UNIT, k, 0, 2, 22 + 51 * k, 48, 46);
        for (k = 0; k < 16u; k++) {
            uint32_t pitch = U[k] == 0 ? 27u : 44u;
            wadd(W_CTL, G[k], K[k], 51 + pitch * X[k], 21 + 51u * U[k], pitch, 48);
        }
        break;
    }
    default:
        break;
    }
}

static void page_draw(void)
{
    uint32_t i;
    bg_draw();
    for (i = 0; i < nwg; i++)
        if (vis(wg[i].x, wg[i].y, wg[i].w, wg[i].h))
            wdg(&wg[i], 1);
}

static void region(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t y1 = y + h;
    if (!w || !h)
        return;
    while (y < y1) {
        uint32_t hs = CV_MAX / w;
        if (hs > y1 - y)
            hs = y1 - y;
        cv_begin(w, hs, C_BLACK);
        rx0 = (int32_t)x;
        ry0 = (int32_t)y;
        page_draw();
        cv_blit(x, y);
        y += hs;
    }
}

/* ---- input */
static void ui_msg(const char *s)
{
    str_cpy(ui.msg, s, sizeof ui.msg);
    ui.msg_until = fm1_ms + 1500u;
}

static void ui_page(uint32_t p)
{
    if (p >= PAGES)
        return;
    if (p != ui.page)
        ui.edit = 0;
    ui.page = (uint8_t)p;
    ui.full = 1;
}

static void msg_ctl(const ctl_t *c, int v)
{
    char b[40], *e = b;
    const char *s = c->name;
    while (*s)
        *e++ = *s++;
    *e++ = ' ';
    if (c->src == S_TEMPO) {
        fmt_int(e, v);
        str_cpy(e + str_len(e), " BPM", 5);
    } else
        ctl_text(c, v, e);
    ui_msg(b);
}

/* note keys: bit n = F3 + n semitones; the white keys are the 16 steps, the black ones 11 picks */
static const int8_t WHITE[27] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13,
                                 14, -1, 15};
static const int8_t BLACK[27] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5, -1, 6, -1, 7, -1, -1, 8, -1, 9, -1,
                                 -1, 10, -1};

static void toggle_mute(uint32_t ch)
{
    int on = dev_param(0, (int)MIXCH(ch, 0u)) != 0;
    rb338_set(RB_PARAM, 0, (int)MIXCH(ch, 0u), !on);
}

static void keys(uint32_t notes)
{
    uint32_t p = ui.page, n;
    for (n = 0; n < 27u; n++) {
        int wk = WHITE[n], bk = BLACK[n];
        if (!(notes >> n & 1u))
            continue;
        if ((p == PG_303A || p == PG_303B) && ui.edit) {
            /* the note at the cursor (C4 = C; below: down, above C5: up), then the next step */
            int dev = (int)p, s = (int)n - 7, k = ui.cursor[dev - 1], len = pat_len(dev);
            uint32_t at = 2u + 6u * (uint32_t)k;
            pat_set(dev, at + 0u, s < 0 ? s + 12 : s > 12 ? s - 12 : s);
            pat_set(dev, at + 3u, s > 12);
            pat_set(dev, at + 4u, s < 0);
            pat_set(dev, at + 5u, 0);
            ui.cursor[dev - 1] = (uint8_t)((k + 1) % len);
            continue;
        }
        if (bk >= 0 && (p == PG_808 || p == PG_909)) {
            ui.inst[p - PG_808] = (uint8_t)(bk + 1);
            ui.bank[p] = 0;
            continue;
        }
        if (wk < 0)
            continue;
        if ((p == PG_808 || p == PG_909) && ui.edit) {
            int dev = (int)p, col = ui.inst[p - PG_808];
            uint32_t at = 2u + 12u * (uint32_t)wk + (uint32_t)col;
            int v = pat_get(dev, at);
            if (p == PG_808)
                v = !v;
            else
                v = col == 0 ? (v ? 0 : 2) : v == 0 ? 2 : v == 2 ? 1 : 0;
            pat_set(dev, at, v);
        } else if (p >= PG_303A && p <= PG_909) {
            /* white keys 1..8: the pattern in the bank, 9..12: the bank */
            uint32_t sp = sel_pat((int)p);
            char b[16];
            if (wk < 8)
                sp = (sp & ~7u) | (uint32_t)wk;
            else if (wk < 12)
                sp = (uint32_t)(wk - 8) * 8u + (sp & 7u);
            else
                continue;
            rb338_set(RB_PARAM, (int)p, 1, (int)sp);
            str_cpy(b, "PATTERN ", sizeof b);
            pat_name(sp, b + 8);
            ui_msg(b);
        } else if ((p == PG_HOME || p == PG_MIX) && wk < 4)
            toggle_mute((uint32_t)wk);
        else if (p == PG_FX && wk < 4) {
            static const uint8_t DEV[4] = {7, 5, 6, 8};
            rb338_set(RB_PARAM, DEV[wk], 0, !dev_param(DEV[wk], 0));
        }
    }
}

enum { B_OCTM, B_OCTP, B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC };
enum { E_SELECT, E_ALG, E_K1, E_PRESETS = 6 };

static int song_save(void);

static void buttons(uint32_t pr)
{
    uint32_t p = ui.page;
    int is303 = p == PG_303A || p == PG_303B, isdrum = p == PG_808 || p == PG_909;
    if (pr & (1u << B_PLAY)) {
        play_req = (int8_t)!rb338_engine_playing();
        while (play_req >= 0 && engine_on)
            fm1_wdt_feed();
    }
    if (pr & (1u << B_HOME))
        ui_page(PG_HOME);
    if (pr & (1u << B_GLO))
        ui_page(PG_MIX);
    if (pr & (1u << B_FX))
        ui_page(PG_FX);
    if (pr & (1u << B_SEQ)) {
        int m = !rb338_get(RB_SONG_MODE, 0, 0);
        rb338_set(RB_SONG_MODE, 0, 0, m);
        ui_msg(m ? "SONG MODE" : "PATTERN MODE");
    }
    if ((pr & (1u << B_EDIT)) && (is303 || isdrum)) {
        ui.edit = !ui.edit;
        ui_msg(ui.edit ? "STEP EDIT" : "PLAY");
    }
    if (pr & (1u << B_SAVE)) {
        if ((uint32_t)(fm1_ms - ui.save_armed) < 2500u && ui.save_armed) {
            ui.save_armed = 0;
            ui_msg("SAVING...");
            region(0, 224, 240, 16);
            ui_msg(song_save() ? "SAVE FAILED" : "SAVED TO THE SONG SLOT");
            ui.full = 1;
        } else {
            ui.save_armed = fm1_ms ? fm1_ms : 1u;
            ui_msg("SAVE? PRESS SAVE AGAIN");
        }
    }
    if (is303 && ui.edit) {
        int dev = (int)p, k = ui.cursor[dev - 1], len = pat_len(dev);
        uint32_t at = 2u + 6u * (uint32_t)k;
        if (pr & (1u << B_OCTM))
            ui.cursor[dev - 1] = (uint8_t)((k + len - 1) % len);
        if (pr & (1u << B_OCTP))
            ui.cursor[dev - 1] = (uint8_t)((k + 1) % len);
        if (pr & (1u << B_ENV))
            pat_set(dev, at + 2u, !pat_get(dev, at + 2u));
        if (pr & (1u << B_LFO))
            pat_set(dev, at + 1u, !pat_get(dev, at + 1u));
        if (pr & (1u << B_SCL))
            pat_set(dev, at + 5u, !pat_get(dev, at + 5u));
        if (pr & (1u << B_REC)) {
            pat_set(dev, at + 5u, 1);
            ui.cursor[dev - 1] = (uint8_t)((k + 1) % len);
        }
    } else if (isdrum) {
        uint8_t *in = &ui.inst[p - PG_808];
        if (pr & (1u << B_OCTM))
            *in = (uint8_t)((*in + 11u) % 12u);
        if (pr & (1u << B_OCTP))
            *in = (uint8_t)((*in + 1u) % 12u);
        if (pr & ((1u << B_OCTM) | (1u << B_OCTP)))
            ui.bank[p] = 0;
    }
}

static int enc_steps(uint32_t e)
{
    int d = fm1_enc_take(e);
    uint32_t dt;
    if (!d)
        return 0;
    dt = fm1_ms - ui.enc_ms[e];
    ui.enc_ms[e] = fm1_ms;
    return dt < 35u ? d * 4 : dt < 80u ? d * 2 : d;
}

static void encoders(void)
{
    uint32_t p = ui.page, k;
    int d = fm1_enc_take(E_SELECT);
    if (d) {
        int np = (int)p + d;
        ui_page((uint32_t)(np < 0 ? 0 : np >= PAGES ? PAGES - 1 : np));
        p = ui.page;
    }
    d = fm1_enc_take(E_ALG);
    if (d && BANKS[p] > 1u) {
        int g = (int)ui.bank[p] + d;
        ui.bank[p] = (uint8_t)(g < 0 ? 0 : g >= BANKS[p] ? BANKS[p] - 1u : (uint32_t)g);
    }
    for (k = 0; k < 4u; k++) {
        ctl_t c;
        d = enc_steps(E_K1 + k);
        if (d && bank_ctl(p, ui.bank[p], k, &c)) {
            int v = ctl_get(&c);
            if (c.hi - c.lo <= 5u)
                v += d > 0 ? 1 : -1;
            else
                v += d;
            v = v < c.lo ? c.lo : v > c.hi ? c.hi : v;
            ctl_set(&c, v);
            msg_ctl(&c, v);
        }
    }
    d = fm1_enc_take(E_PRESETS);
    if (d && p >= PG_303A && p <= PG_909) {
        int sp = (int)sel_pat((int)p) + (d > 0 ? 1 : -1);
        char b[16];
        sp = sp < 0 ? 0 : sp > 31 ? 31 : sp;
        rb338_set(RB_PARAM, (int)p, 1, sp);
        str_cpy(b, "PATTERN ", sizeof b);
        pat_name((uint32_t)sp, b + 8);
        ui_msg(b);
    }
}

/* ---- the main-loop entry */
static void ui_redraw(void) { ui.full = 1; }
static void ui_poll(void)
{
    uint32_t released, pr = fm1_input_edges(&released), notes = fm1_input_note_edges(), i;
    (void)released;
    if (pr)
        buttons(pr);
    if (notes)
        keys(notes);
    encoders();
    fm1_led_key(B_PLAY, rb338_engine_playing());
    fm1_led_key(B_EDIT, ui.edit);
    fm1_led_key(B_SEQ, rb338_get(RB_SONG_MODE, 0, 0) != 0);
    if (ui.full) {
        ui.full = 0;
        title_load();
        page_build();
        for (i = 0; i < nwg; i++)
            wsig[i] = wdg(&wg[i], 0);
        region(0, 0, 240, 240);
        ui.last_draw = fm1_ms;
        return;
    }
    if ((uint32_t)(fm1_ms - ui.last_draw) < 25u)
        return;
    ui.last_draw = fm1_ms;
    for (i = 0; i < nwg; i++) {
        uint32_t s = wdg(&wg[i], 0);
        if (s != wsig[i]) {
            wsig[i] = s;
            region(wg[i].x, wg[i].y, wg[i].w, wg[i].h);
        }
    }
}
