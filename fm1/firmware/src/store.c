/* SPDX-License-Identifier: GPL-3.0-only */
/* Data-flash areas written over USB SysEx (tools/rb338_upload.py): the song slot
 * and kit regions 1 and 2. Region 0 of the kit is linked into the app image.
 *
 * Area layout: a 16-byte header {"RBA1", u32 len, u32 crc32, u32 0} and the data
 * at +16. The header is programmed last (END), after the CRC of the data checks
 * out, so an interrupted upload leaves the area empty, never half valid.
 *
 * Frames (pack7 = groups of 7 bytes behind their MSB byte, as Felucca's editor):
 *   7D 46 4C 40                                INFO  -> 40 01 {state len28 crc32} x3
 *   7D 46 4C 41 area len28                     BEGIN (erase) -> 41 area rc
 *   7D 46 4C 42 area off28 pack7(<= 256 B)     WRITE -> 42 area off28 rc
 *   7D 46 4C 43 area len28 crc32               END   -> 43 area rc
 * len28 = 4 x 7 bits, crc32 = 5 x 7 bits, both LSB first; rc 0 = ok. */

#define ST_HDR 16u
#define ST_MAGIC 0x31414252u                    /* "RBA1" */
enum { AREA_SONG, AREA_KIT1, AREA_KIT2, AREAS };
static const struct { uint32_t off, cap; } st_area[AREAS] = {
    {0x97000u, 0x20000u},                       /* song slot */
    {0xB7000u, 0x29000u},                       /* kit region 1 */
    {0xE5000u, 0x16000u},                       /* kit region 2 (OTA staging 0xE0000..0xE5000 between) */
};
#define ST_SETTINGS 0xFB000u

enum { ST_OK, ST_EARG, ST_EFLASH, ST_ECRC, ST_ESTATE };

static struct {
    uint8_t open;                               /* area being written + 1; 0 = none */
    uint32_t len;
} st_up;

static uint32_t st_crc32(const uint8_t *p, uint32_t n, uint32_t c)
{
    c = ~c;
    while (n--) {
        uint32_t k;
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t st_rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* a valid area's data (XIP) and length; NULL when empty or damaged */
static const uint8_t *st_data(uint32_t a, uint32_t *len)
{
    const uint8_t *h = (const uint8_t *)FL_XIP(st_area[a].off);
    uint32_t n = st_rd32(h + 4);
    if (st_up.open == a + 1u || st_rd32(h) != ST_MAGIC || n > st_area[a].cap - ST_HDR ||
        st_crc32(h + ST_HDR, n, 0) != st_rd32(h + 8))
        return 0;
    *len = n;
    return h + ST_HDR;
}

static uint32_t st_get7(const uint8_t *p, uint32_t k)
{
    uint32_t v = 0, i;
    for (i = 0; i < k; i++)
        v |= (uint32_t)(p[i] & 0x7Fu) << (7u * i);
    return v;
}
static void st_put7(uint8_t *p, uint32_t v, uint32_t k)
{
    while (k--) {
        *p++ = v & 0x7Fu;
        v >>= 7;
    }
}

static uint32_t st_unpack7(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t o = 0;
    while (n) {
        uint32_t m = *p++, k = n - 1u < 7u ? n - 1u : 7u, i;
        n -= k + 1u;
        for (i = 0; i < k; i++) {
            if (o == cap)
                return cap + 1u;
            out[o++] = (uint8_t)(p[i] | (m >> i & 1u) << 7);
        }
        p += k;
    }
    return o;
}

static void st_reply(const uint8_t *b, uint32_t n)
{
    uint8_t f[48];
    uint32_t i;
    f[0] = 0xF0;
    f[1] = 0x7D;
    f[2] = 0x46;
    f[3] = 0x4C;
    for (i = 0; i < n && i < sizeof f - 5u; i++)
        f[4 + i] = b[i] & 0x7Fu;
    f[4 + i] = 0xF7;
    ota_wire_send(f, i + 5u);
}

static void store_changed(uint32_t area);       /* rb338_main.c: reload the engine's data */
static void store_quiet(void);                  /* rb338_main.c: stop reading the areas */

static int st_begin(uint32_t a, uint32_t len)
{
    uint32_t o, took;
    if (a >= AREAS || len > st_area[a].cap - ST_HDR)
        return ST_EARG;
    store_quiet();
    st_up.open = (uint8_t)(a + 1u);
    st_up.len = len;
    for (o = 0; o < ST_HDR + len; o += 0x1000u) {
        fm1_wdt_feed();
        if (fl_erase4k_quiet(st_area[a].off + o, &took))
            return ST_EFLASH;
    }
    return ST_OK;
}

static int st_write(uint32_t a, uint32_t off, const uint8_t *p, uint32_t n)
{
    if (st_up.open != a + 1u)
        return ST_ESTATE;
    if (off > st_up.len || n > st_up.len - off)
        return ST_EARG;
    return fl_write(st_area[a].off + ST_HDR + off, p, n) ? ST_EFLASH : ST_OK;
}

static int st_header(uint32_t a, uint32_t len, uint32_t crc)
{
    uint8_t h[ST_HDR];
    uint32_t i;
    for (i = 0; i < ST_HDR; i++)
        h[i] = 0;
    for (i = 0; i < 4u; i++) {
        h[i] = (uint8_t)(ST_MAGIC >> 8u * i);
        h[4 + i] = (uint8_t)(len >> 8u * i);
        h[8 + i] = (uint8_t)(crc >> 8u * i);
    }
    return fl_write(st_area[a].off, h, ST_HDR) ? ST_EFLASH : ST_OK;
}

static int st_end(uint32_t a, uint32_t len, uint32_t crc)
{
    const uint8_t *d = (const uint8_t *)FL_XIP(st_area[a].off + ST_HDR);
    int rc;
    if (st_up.open != a + 1u)
        return ST_ESTATE;
    st_up.open = 0;
    if (len != st_up.len)
        return ST_EARG;
    if (st_crc32(d, len, 0) != crc)
        return ST_ECRC;
    rc = st_header(a, len, crc);
    if (rc == ST_OK)
        store_changed(a);
    return rc;
}

/* Rewrites area a with len bytes that fill() supplies a sector at a time, into buf (4 KB): it
 * may read the area's old data at the offsets it fills, as each sector is filled before it is
 * erased. Sector 0 goes first with the header unwritten and the header last, as an upload: an
 * interrupted rewrite leaves the area empty. The caller has stopped every reader (store_quiet). */
static int st_rewrite(uint32_t a, uint32_t len, void (*fill)(uint8_t *dst, uint32_t off, uint32_t n), uint8_t *buf)
{
    uint32_t o, i, took;
    if (a >= AREAS || st_up.open || len > st_area[a].cap - ST_HDR)
        return ST_EARG;
    for (o = 0; o < ST_HDR + len; o += 0x1000u) {
        uint32_t lo = o < ST_HDR ? ST_HDR : o, hi = o + 0x1000u < ST_HDR + len ? o + 0x1000u : ST_HDR + len;
        fm1_wdt_feed();
        for (i = 0; i < 0x1000u; i++)
            buf[i] = 0xFF;
        fill(buf + (lo - o), lo - ST_HDR, hi - lo);
        if (fl_erase4k_quiet(st_area[a].off + o, &took) || fl_write(st_area[a].off + o, buf, 0x1000u))
            return ST_EFLASH;
    }
    return st_header(a, len, st_crc32((const uint8_t *)FL_XIP(st_area[a].off + ST_HDR), len, 0));
}

/* main loop: one pending RB338 frame (header 7D 46 4C, commands 0x40..0x4F) */
static void store_service(void)
{
    static uint8_t data[256];
    uint8_t r[2u + 10u * AREAS];
    const uint8_t *p;
    uint32_t n, a, i;
    if (!ota_frame_get(&p, &n))
        return;
    if (n < 4u || p[0] != 0x7D || p[1] != 0x46 || p[2] != 0x4C || (p[3] & 0xF0u) != 0x40u)
        return;                                 /* not ours: ota_service() takes it */
    r[0] = p[3];
    a = n > 4u ? p[4] : 0u;
    switch (p[3]) {
    case 0x40: {
        ota_frame_done();
        r[1] = 1;                               /* protocol version */
        for (i = 0; i < AREAS; i++) {
            uint32_t len = 0;
            const uint8_t *d = st_data(i, &len);
            uint8_t *q = r + 2 + 10u * i;
            q[0] = d != 0;
            st_put7(q + 1, len, 4);
            st_put7(q + 5, d ? st_rd32(d - ST_HDR + 8) : 0u, 5);
        }
        st_reply(r, 2u + 10u * AREAS);
        return;
    }
    case 0x41:
        r[2] = (uint8_t)(n == 9u ? st_begin(a, st_get7(p + 5, 4)) : ST_EARG);
        ota_frame_done();
        break;
    case 0x42: {
        uint32_t off = n >= 9u ? st_get7(p + 5, 4) : 0u;
        uint32_t k = n >= 9u ? st_unpack7(p + 9, n - 9u, data, sizeof data) : 0u;
        int rc;
        ota_frame_done();
        rc = n < 10u || k > sizeof data ? ST_EARG : st_write(a, off, data, k);
        r[1] = (uint8_t)a;
        st_put7(r + 2, off, 4);
        r[6] = (uint8_t)rc;
        st_reply(r, 7);
        return;
    }
    case 0x43:
        r[2] = (uint8_t)(n == 14u ? st_end(a, st_get7(p + 5, 4), st_get7(p + 9, 5)) : ST_EARG);
        ota_frame_done();
        break;
    default:
        ota_frame_done();
        r[2] = ST_EARG;
        break;
    }
    r[1] = (uint8_t)a;
    st_reply(r, 3);
}
