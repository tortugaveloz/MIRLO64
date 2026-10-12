/* MRDP triangle setup -- the RSP's job: three screen-space vertices in, one
 * N64-layout triangle command out (mrdp.h has the formats).
 *
 * Header-only and integer-only (32x32 -> 64 multiplies, no divides), so the
 * geom core (rv32i + Zmmul, no FPU, no divider) and the host simulator run
 * this exact code and produce the same bits.
 *
 * Vertex attributes, in their MRDP formats:
 *   x, y       s15.16 screen pixels (y down)
 *   a[0..3]    R, G, B, A  s15.16 (0..255)
 *   a[4..6]    S, T, W     s15.16 (S = s * W with perspective)
 *   a[7]       Z           s16.15
 */
#ifndef MRDP_SETUP_H
#define MRDP_SETUP_H

#include <stdint.h>

#define MRDP_SETUP_SHADE 4u
#define MRDP_SETUP_TEX   2u
#define MRDP_SETUP_Z     1u

typedef struct {
    int32_t x, y;
    int32_t a[8];
} mrdp_vtx_t;

/* Count leading zeros without builtins or libcalls (the geom core's libgcc
 * helpers are bit loops); 32 for 0. */
static inline int mrdp_clz32(uint32_t v)
{
    int n = 0;
    if (!v) return 32;
    if (!(v & 0xFFFF0000u)) { n += 16; v <<= 16; }
    if (!(v & 0xFF000000u)) { n += 8;  v <<= 8; }
    if (!(v & 0xF0000000u)) { n += 4;  v <<= 4; }
    if (!(v & 0xC0000000u)) { n += 2;  v <<= 2; }
    if (!(v & 0x80000000u)) { n += 1; }
    return n;
}

static inline int mrdp_clz64(uint64_t v)
{
    uint32_t hi = (uint32_t)(v >> 32);
    return hi ? mrdp_clz32(hi) : 32 + mrdp_clz32((uint32_t)v);
}

/* (a * b) >> 16 for s15.16 values, 64-bit intermediate */
static inline int32_t mrdp_mul16(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 16);
}

/* 2^62 / d for d in [2^31, 2^32): a result in (2^30, 2^31], Q30. As the
 * N64 does it: a 512-entry reciprocal ROM shaped like the RSP's VRCP table,
 * indexed by the 9 bits under d's leading one, reads 1 + rom / 2^16
 * (~10 bits); then ONE Newton step y (2 - d y), 32x32 -> 64. The ROM alone
 * shifted sprites' texels (they are triangles here, texture rectangles on
 * the N64); one step matches two to the eye, and no more precision is used.
 * Entry i = min(0xFFFF, ((2^34 / (512 + i) + 1) >> 8) - 2^16); tools/mrdp_ucode.py
 * checks the table against that formula. */
static const uint16_t mrdp_rcp_rom[512] = {
    0xffff, 0xff00, 0xfe01, 0xfd04, 0xfc07, 0xfb0c, 0xfa11, 0xf918, 0xf81f, 0xf727, 0xf631, 0xf53b,
    0xf446, 0xf352, 0xf25f, 0xf16d, 0xf07c, 0xef8b, 0xee9c, 0xedae, 0xecc0, 0xebd3, 0xeae8, 0xe9fd,
    0xe913, 0xe829, 0xe741, 0xe65a, 0xe573, 0xe48d, 0xe3a9, 0xe2c5, 0xe1e1, 0xe0ff, 0xe01e, 0xdf3d,
    0xde5d, 0xdd7e, 0xdca0, 0xdbc2, 0xdae6, 0xda0a, 0xd92f, 0xd854, 0xd77b, 0xd6a2, 0xd5ca, 0xd4f3,
    0xd41d, 0xd347, 0xd272, 0xd19e, 0xd0cb, 0xcff8, 0xcf26, 0xce55, 0xcd85, 0xccb5, 0xcbe6, 0xcb18,
    0xca4b, 0xc97e, 0xc8b2, 0xc7e7, 0xc71c, 0xc652, 0xc589, 0xc4c0, 0xc3f8, 0xc331, 0xc26b, 0xc1a5,
    0xc0e0, 0xc01c, 0xbf58, 0xbe95, 0xbdd2, 0xbd10, 0xbc4f, 0xbb8f, 0xbacf, 0xba10, 0xb951, 0xb894,
    0xb7d6, 0xb71a, 0xb65e, 0xb5a2, 0xb4e8, 0xb42e, 0xb374, 0xb2bb, 0xb203, 0xb14b, 0xb094, 0xafde,
    0xaf28, 0xae73, 0xadbe, 0xad0a, 0xac57, 0xaba4, 0xaaf1, 0xaa40, 0xa98e, 0xa8de, 0xa82e, 0xa77e,
    0xa6d0, 0xa621, 0xa574, 0xa4c6, 0xa41a, 0xa36e, 0xa2c2, 0xa217, 0xa16d, 0xa0c3, 0xa01a, 0x9f71,
    0x9ec8, 0x9e21, 0x9d79, 0x9cd3, 0x9c2d, 0x9b87, 0x9ae2, 0x9a3d, 0x9999, 0x98f6, 0x9852, 0x97b0,
    0x970e, 0x966c, 0x95cb, 0x952b, 0x948b, 0x93eb, 0x934c, 0x92ad, 0x920f, 0x9172, 0x90d4, 0x9038,
    0x8f9c, 0x8f00, 0x8e65, 0x8dca, 0x8d30, 0x8c96, 0x8bfc, 0x8b64, 0x8acb, 0x8a33, 0x899c, 0x8904,
    0x886e, 0x87d8, 0x8742, 0x86ad, 0x8618, 0x8583, 0x84f0, 0x845c, 0x83c9, 0x8336, 0x82a4, 0x8212,
    0x8181, 0x80f0, 0x8060, 0x7fd0, 0x7f40, 0x7eb1, 0x7e22, 0x7d93, 0x7d05, 0x7c78, 0x7beb, 0x7b5e,
    0x7ad2, 0x7a46, 0x79ba, 0x792f, 0x78a4, 0x781a, 0x7790, 0x7706, 0x767d, 0x75f5, 0x756c, 0x74e4,
    0x745d, 0x73d5, 0x734f, 0x72c8, 0x7242, 0x71bc, 0x7137, 0x70b2, 0x702e, 0x6fa9, 0x6f26, 0x6ea2,
    0x6e1f, 0x6d9c, 0x6d1a, 0x6c98, 0x6c16, 0x6b95, 0x6b14, 0x6a94, 0x6a13, 0x6993, 0x6914, 0x6895,
    0x6816, 0x6798, 0x6719, 0x669c, 0x661e, 0x65a1, 0x6524, 0x64a8, 0x642c, 0x63b0, 0x6335, 0x62ba,
    0x623f, 0x61c5, 0x614b, 0x60d1, 0x6058, 0x5fdf, 0x5f66, 0x5eed, 0x5e75, 0x5dfd, 0x5d86, 0x5d0f,
    0x5c98, 0x5c22, 0x5bab, 0x5b35, 0x5ac0, 0x5a4b, 0x59d6, 0x5961, 0x58ed, 0x5879, 0x5805, 0x5791,
    0x571e, 0x56ac, 0x5639, 0x55c7, 0x5555, 0x54e3, 0x5472, 0x5401, 0x5390, 0x5320, 0x52af, 0x5240,
    0x51d0, 0x5161, 0x50f2, 0x5083, 0x5015, 0x4fa6, 0x4f38, 0x4ecb, 0x4e5e, 0x4df1, 0x4d84, 0x4d17,
    0x4cab, 0x4c3f, 0x4bd3, 0x4b68, 0x4afd, 0x4a92, 0x4a27, 0x49bd, 0x4953, 0x48e9, 0x4880, 0x4817,
    0x47ae, 0x4745, 0x46dc, 0x4674, 0x460c, 0x45a5, 0x453d, 0x44d6, 0x446f, 0x4408, 0x43a2, 0x433c,
    0x42d6, 0x4270, 0x420b, 0x41a6, 0x4141, 0x40dc, 0x4078, 0x4014, 0x3fb0, 0x3f4c, 0x3ee8, 0x3e85,
    0x3e22, 0x3dc0, 0x3d5d, 0x3cfb, 0x3c99, 0x3c37, 0x3bd6, 0x3b74, 0x3b13, 0x3ab2, 0x3a52, 0x39f1,
    0x3991, 0x3931, 0x38d2, 0x3872, 0x3813, 0x37b4, 0x3755, 0x36f7, 0x3698, 0x363a, 0x35dc, 0x357f,
    0x3521, 0x34c4, 0x3467, 0x340a, 0x33ae, 0x3351, 0x32f5, 0x3299, 0x323e, 0x31e2, 0x3187, 0x312c,
    0x30d1, 0x3076, 0x301c, 0x2fc2, 0x2f68, 0x2f0e, 0x2eb4, 0x2e5b, 0x2e02, 0x2da9, 0x2d50, 0x2cf8,
    0x2c9f, 0x2c47, 0x2bef, 0x2b97, 0x2b40, 0x2ae8, 0x2a91, 0x2a3a, 0x29e4, 0x298d, 0x2937, 0x28e0,
    0x288b, 0x2835, 0x27df, 0x278a, 0x2735, 0x26e0, 0x268b, 0x2636, 0x25e2, 0x258d, 0x2539, 0x24e5,
    0x2492, 0x243e, 0x23eb, 0x2398, 0x2345, 0x22f2, 0x22a0, 0x224d, 0x21fb, 0x21a9, 0x2157, 0x2105,
    0x20b4, 0x2063, 0x2012, 0x1fc1, 0x1f70, 0x1f1f, 0x1ecf, 0x1e7f, 0x1e2e, 0x1ddf, 0x1d8f, 0x1d3f,
    0x1cf0, 0x1ca1, 0x1c52, 0x1c03, 0x1bb4, 0x1b66, 0x1b17, 0x1ac9, 0x1a7b, 0x1a2d, 0x19e0, 0x1992,
    0x1945, 0x18f8, 0x18ab, 0x185e, 0x1811, 0x17c4, 0x1778, 0x172c, 0x16e0, 0x1694, 0x1648, 0x15fd,
    0x15b1, 0x1566, 0x151b, 0x14d0, 0x1485, 0x143b, 0x13f0, 0x13a6, 0x135c, 0x1312, 0x12c8, 0x127f,
    0x1235, 0x11ec, 0x11a3, 0x1159, 0x1111, 0x10c8, 0x107f, 0x1037, 0x0fef, 0x0fa6, 0x0f5e, 0x0f17,
    0x0ecf, 0x0e87, 0x0e40, 0x0df9, 0x0db2, 0x0d6b, 0x0d24, 0x0cdd, 0x0c97, 0x0c50, 0x0c0a, 0x0bc4,
    0x0b7e, 0x0b38, 0x0af2, 0x0aad, 0x0a68, 0x0a22, 0x09dd, 0x0998, 0x0953, 0x090f, 0x08ca, 0x0886,
    0x0842, 0x07fd, 0x07b9, 0x0776, 0x0732, 0x06ee, 0x06ab, 0x0668, 0x0624, 0x05e1, 0x059e, 0x055c,
    0x0519, 0x04d6, 0x0494, 0x0452, 0x0410, 0x03ce, 0x038c, 0x034a, 0x0309, 0x02c7, 0x0286, 0x0245,
    0x0204, 0x01c3, 0x0182, 0x0141, 0x0101, 0x00c0, 0x0080, 0x0040
};
#ifdef MRDP_RECIP_HOOK
/* The includer's reciprocal (the geom core: GeomSetupUnit RECIPN, bit-exact
 * with its C on the host): MRDP_RECIP_HOOK(d) ~ 2^62 / d for d in [2^31, 2^32). */
static inline uint32_t mrdp_recip_norm(uint32_t d) { return MRDP_RECIP_HOOK(d); }
#else
static inline uint32_t mrdp_recip_norm(uint32_t d)
{
    uint32_t y = 0x40000000u | ((uint32_t)mrdp_rcp_rom[(d >> 22) & 0x1FFu] << 14);
    uint32_t xy = (uint32_t)(((uint64_t)d * y) >> 32);          /* x*y, Q30 */
    return (uint32_t)(((uint64_t)y * (0x80000000u - xy)) >> 30);  /* y (2 - x y) */
}
#endif

/* v normalised to [2^31, 2^32): v ~ n * 2^(msb - 31); returns n, sets *msb */
static inline uint32_t mrdp_norm64(uint64_t v, int *msb)
{
    int m = 63 - mrdp_clz64(v);
    *msb = m;
    return m >= 31 ? (uint32_t)(v >> (m - 31)) : (uint32_t)v << (31 - m);
}

/* x slope of an edge per scanline, s15.16 */
static inline int32_t mrdp_slope(int32_t dx, int32_t dy)
{
    if (dy <= 0 || dx == 0) return 0;
    int s = mrdp_clz32((uint32_t)dy);
    uint32_t r = mrdp_recip_norm((uint32_t)dy << s);         /* 2^62 / (dy << s) */
    uint32_t adx = dx < 0 ? (uint32_t)-dx : (uint32_t)dx;
    uint64_t v = ((uint64_t)adx * r) >> (46 - s);              /* |dx| * 2^16 / dy */
    if (v > 0x7FFFFFFFu) v = 0x7FFFFFFFu;
    return dx < 0 ? -(int32_t)v : (int32_t)v;
}

/* dx / dy with 30 fraction bits (0 < dx <= dy: W' = w_min / w, in (0, 1]).
 * W' is 2.30, not 16.16: with 1.0 = 2^16 a far vertex's W' had a dozen
 * significant bits, and its per-pixel gradient (~160 units) was rounded to
 * whole units -- over the ~240 rows from a big triangle's off-screen anchor
 * the error reached 0.25 % of W, 1.5 texels of S at S ~ 600: walls, floors
 * and cliffs swam on large scenes. The N64 keeps W large too: its RDP
 * normalises W from the integer bits. */
static inline int32_t mrdp_slope30(int32_t dx, int32_t dy)
{
    if (dy <= 0 || dx == 0) return 0;
    int s = mrdp_clz32((uint32_t)dy);
    uint32_t r = mrdp_recip_norm((uint32_t)dy << s);           /* 2^62 / (dy << s) */
    uint32_t adx = dx < 0 ? (uint32_t)-dx : (uint32_t)dx;
    uint64_t v = ((uint64_t)adx * r) >> (32 - s);              /* |dx| * 2^30 / dy */
    if (v > 0x7FFFFFFFu) v = 0x7FFFFFFFu;
    return dx < 0 ? -(int32_t)v : (int32_t)v;
}

/* 1 / area, normalised: |area| ~ an * 2^(e - 31), r = 2^62 / an. The
 * gradient of an attribute is then P / area * 2^16 for P = da1*dy2 - da2*dy1
 * (or the x form), computed by mrdp_grad(). */
typedef struct { uint32_t r; int e; int neg; } mrdp_rinv_t;

static inline mrdp_rinv_t mrdp_rinv(int64_t area)
{
    mrdp_rinv_t ri;
    ri.neg = area < 0;
    ri.r = mrdp_recip_norm(mrdp_norm64(ri.neg ? (uint64_t)-area : (uint64_t)area, &ri.e));
    return ri;
}

static inline int32_t mrdp_grad(int64_t p, mrdp_rinv_t ri)
{
    if (p == 0) return 0;
    int neg = (p < 0) ^ ri.neg, f;
    uint32_t pn = mrdp_norm64(p < 0 ? (uint64_t)-p : (uint64_t)p, &f);
    uint64_t prod = (uint64_t)pn * ri.r;                       /* < 2^63 */
    int sh = 46 + ri.e - f;                                    /* p / area * 2^16 */
    uint64_t v = sh <= 0 ? 0x7FFFFFFFu : sh >= 64 ? 0 : prod >> sh;
    if (v > 0x7FFFFFFFu) v = 0x7FFFFFFFu;
    return neg ? -(int32_t)v : (int32_t)v;
}

/* 1/area as four plane factors sharing one exponent: f = d * 2^16 / area,
 * for d = dy2, dy1, dx1, dx2, is F * 2^-e with |F| < 2^30. A gradient is then
 * (da * Fa - db * Fb) >> e -- two multiplies and one shift, where a
 * normalisation, a multiply and a variable shift per gradient (mrdp_grad)
 * were the bulk of the geom core's setup. */
typedef struct { int32_t f[4]; int e; } mrdp_fac_t;

static inline mrdp_fac_t mrdp_factors(int64_t area, int32_t dy2, int32_t dy1, int32_t dx1, int32_t dx2)
{
    mrdp_fac_t F;
    int neg = area < 0, ea;
    /* 1 / |area| = r * 2^(-31 - ea) */
    uint32_t r = mrdp_recip_norm(mrdp_norm64(neg ? (uint64_t)-area : (uint64_t)area, &ea));
    int32_t d[4] = { dy2, dy1, dx1, dx2 };
    uint32_t dor = 0;
    for (int k = 0; k < 4; k++) dor |= d[k] < 0 ? 0u - (uint32_t)d[k] : (uint32_t)d[k];
    int s = 33 - mrdp_clz32(dor);               /* |d| * r < 2^(msb + 32): >> msb + 2 */
    if (s < 0) s = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t ad = d[k] < 0 ? 0u - (uint32_t)d[k] : (uint32_t)d[k];
        int32_t p = (int32_t)(((uint64_t)ad * r) >> s);
        F.f[k] = (neg ^ (d[k] < 0)) ? -p : p;
    }
    F.e = 15 + ea - s;
    return F;
}

static inline int32_t mrdp_fgrad(int32_t da, int32_t fa, int32_t db, int32_t fb, int e)
{
    int64_t p = (int64_t)da * fa - (int64_t)db * fb;       /* |p| < 2^62 */
    if (p == 0) return 0;
    if (e >= 0) {
        int64_t v = e >= 63 ? (p < 0 ? -1 : 0) : p >> e;
        return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFF ? -0x7FFFFFFF : (int32_t)v;
    }
    /* a sliver: the plane is steeper than int32 can hold unless p is small */
    uint64_t a = p < 0 ? (uint64_t)-p : (uint64_t)p;
    int32_t sat = p < 0 ? -0x7FFFFFFF : 0x7FFFFFFF;
    if (-e >= 32 || (a >> (31 + e)) != 0) return sat;    /* a << -e >= 2^31 */
    return p < 0 ? -(int32_t)(a << -e) : (int32_t)(a << -e);
}

/* ---- the edge half of a triangle (both command forms) -------------------- */
typedef struct {
    const mrdp_vtx_t *v0, *v1, *v2;     /* sorted: top, middle, bottom */
    int32_t dx1, dy1, dx2, dy2;
    int64_t area;                        /* > 0: v1 right of v0->v2 */
    int32_t yh, ym, yl, y0i;             /* s11.2; y0i: YH's row, 16.16 */
    int32_t xh, dxh, xm, dxm, xl, dxl;
    unsigned lft;
} mrdp_edges_t;

/* Snap y to quarter pixels (in place: the caller's vertices are scratch),
 * sort, and the three edges. Returns 0 for a degenerate triangle.
 *
 * The snap: YH, YM, YL carry quarter pixels, and the walker switches from XM
 * to XL at the snapped YM; an XL computed from the exact y instead was off
 * by (y - YM) * DxLDy, and a sub-triangle a fraction of a row tall has a
 * huge DxLDy -- Goddard's head got 40-pixel one-row streaks. */
/* the edges of three vertices taken as given: v0 top, v1 middle, v2 bottom
 * (MRDP's TRI_R does exactly this; it neither snaps nor sorts) */
#ifndef MRDP_EDGES_ATTR
#define MRDP_EDGES_ATTR inline   /* the geom core: noinline (its ROM B is full) */
#endif
static MRDP_EDGES_ATTR int mrdp_edges_of(mrdp_edges_t *E, const mrdp_vtx_t *v0, const mrdp_vtx_t *v1,
                                const mrdp_vtx_t *v2)
{
    E->v0 = v0; E->v1 = v1; E->v2 = v2;
    E->dx1 = v1->x - v0->x; E->dy1 = v1->y - v0->y;
    E->dx2 = v2->x - v0->x; E->dy2 = v2->y - v0->y;
    E->area = (int64_t)E->dx1 * E->dy2 - (int64_t)E->dx2 * E->dy1;
    if (E->area == 0 || E->dy2 == 0) return 0;
    E->yh = v0->y >> 14; E->ym = v1->y >> 14; E->yl = v2->y >> 14;
    E->y0i = (E->yh >> 2) * 65536;
    int32_t ymi = (E->ym >> 2) * 65536;
    E->dxh = mrdp_slope(E->dx2, E->dy2);
    E->dxm = mrdp_slope(E->dx1, E->dy1);
    E->dxl = mrdp_slope(v2->x - v1->x, v2->y - v1->y);
    E->xh = v0->x + mrdp_mul16(E->dxh, E->y0i - v0->y);
    E->xm = v0->x + mrdp_mul16(E->dxm, E->y0i - v0->y);
    E->xl = v1->x + mrdp_mul16(E->dxl, ymi - v1->y);
    E->lft = E->area > 0;
    return 1;
}

/* Crack grow: an AA_EN triangle's edges move out a
 * little so neighbours overlap instead of leaving T-junction cracks -- but
 * only the edges the triangle is about a pixel thick across (2 * area >= |dx|
 * + |dy|): a sliver's grown band would extrapolate colour and depth along it
 * (on the Pocket, a green line across the castle grounds). Bit 0 H (v0 v2),
 * 1 M (v0 v1), 2 L (v1 v2). What TRI_R's microcode computes (GRW), bit for bit:
 * 32-bit |dx| + |dy| (wrapping), shifted to 32.32 as a signed 64-bit value,
 * against |area| as a signed 64-bit value. */
static inline unsigned mrdp_grow_mask(const mrdp_edges_t *E)
{
    uint64_t ar = (uint64_t)E->area;
    if (E->area < 0) ar = 0u - ar;
    int64_t a = (int64_t)ar;
    const int32_t dx[3] = { E->dx2, E->dx1, (int32_t)((uint32_t)E->dx2 - (uint32_t)E->dx1) };
    const int32_t dy[3] = { E->dy2, E->dy1, (int32_t)((uint32_t)E->dy2 - (uint32_t)E->dy1) };
    unsigned m = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t ax = (uint32_t)dx[k], ay = (uint32_t)dy[k];
        if (dx[k] < 0) ax = 0u - ax;
        if (dy[k] < 0) ay = 0u - ay;
        int64_t l = (int64_t)(int32_t)(ax + ay) * 65536;
        if (!(a < l)) m |= 1u << k;
    }
    return m;
}

/* How far an edge grows, horizontally in 16.16: 1/8 px across it -- (|a| +
 * |b|) >> 3 in edge units -- is (1 + |DxDy|) / 8 along a row, capped at 1/4
 * px: a row walker has no third edge to stop a near-horizontal one's shift,
 * which would run on by |DxDy| / 8. At pixel centres 1/16 px left gaps
 * between surfaces 0.22 px apart; 1/8 closes them. */
static inline int32_t mrdp_grow_amount(int32_t dxdy)
{
    uint32_t a = (uint32_t)dxdy;
    if (dxdy < 0) a = 0u - a;
    a >>= 3;
    return a >= 0x2000u ? 0x4000 : (int32_t)(0x2000u + a);
}

static inline int mrdp_setup_edges(mrdp_edges_t *E, mrdp_vtx_t *va, mrdp_vtx_t *vb, mrdp_vtx_t *vc)
{
    va->y = (int32_t)(((uint32_t)va->y + 0x2000u) & ~0x3FFFu);
    vb->y = (int32_t)(((uint32_t)vb->y + 0x2000u) & ~0x3FFFu);
    vc->y = (int32_t)(((uint32_t)vc->y + 0x2000u) & ~0x3FFFu);
    const mrdp_vtx_t *v0 = va, *v1 = vb, *v2 = vc, *t;
    if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
    if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; }
    if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
    return mrdp_edges_of(E, v0, v1, v2);
}

/* the 8 edge words; w0's bits 22 and up above the tile come from `hi` */
static inline int mrdp_put_edges(uint32_t *out, const mrdp_edges_t *E, uint32_t hi, unsigned tile)
{
    out[0] = hi | (E->lft << 23) | ((tile & 7u) << 16) | ((uint32_t)E->yl & 0x3FFF);
    out[1] = (((uint32_t)E->ym & 0x3FFF) << 16) | ((uint32_t)E->yh & 0x3FFF);
    out[2] = (uint32_t)E->xl; out[3] = (uint32_t)E->dxl;
    out[4] = (uint32_t)E->xh; out[5] = (uint32_t)E->dxh;
    out[6] = (uint32_t)E->xm; out[7] = (uint32_t)E->dxm;
    return 8;
}

/* N64 attribute blocks (ints then fracs, 16 bits each, two per word) after
 * the edge words; returns the new word count */
static inline int mrdp_put_attrs(uint32_t *out, int n, unsigned flags, const int32_t *val,
                                 const int32_t *ddx, const int32_t *dde, const int32_t *ddy)
{
#define MRDP_HI(v) ((uint32_t)(v) >> 16)
#define MRDP_LO(v) ((uint32_t)(v) & 0xFFFF)
#define MRDP_PAIR(f, a, b) ((f(a)) << 16 | (f(b)))
    if (flags & MRDP_SETUP_SHADE) {
        out[n++] = MRDP_PAIR(MRDP_HI, val[0], val[1]); out[n++] = MRDP_PAIR(MRDP_HI, val[2], val[3]);
        out[n++] = MRDP_PAIR(MRDP_HI, ddx[0], ddx[1]); out[n++] = MRDP_PAIR(MRDP_HI, ddx[2], ddx[3]);
        out[n++] = MRDP_PAIR(MRDP_LO, val[0], val[1]); out[n++] = MRDP_PAIR(MRDP_LO, val[2], val[3]);
        out[n++] = MRDP_PAIR(MRDP_LO, ddx[0], ddx[1]); out[n++] = MRDP_PAIR(MRDP_LO, ddx[2], ddx[3]);
        out[n++] = MRDP_PAIR(MRDP_HI, dde[0], dde[1]); out[n++] = MRDP_PAIR(MRDP_HI, dde[2], dde[3]);
        out[n++] = MRDP_PAIR(MRDP_HI, ddy[0], ddy[1]); out[n++] = MRDP_PAIR(MRDP_HI, ddy[2], ddy[3]);
        out[n++] = MRDP_PAIR(MRDP_LO, dde[0], dde[1]); out[n++] = MRDP_PAIR(MRDP_LO, dde[2], dde[3]);
        out[n++] = MRDP_PAIR(MRDP_LO, ddy[0], ddy[1]); out[n++] = MRDP_PAIR(MRDP_LO, ddy[2], ddy[3]);
    }
    if (flags & MRDP_SETUP_TEX) {             /* S, T, W and a zero fourth */
        out[n++] = MRDP_PAIR(MRDP_HI, val[4], val[5]); out[n++] = MRDP_HI(val[6]) << 16;
        out[n++] = MRDP_PAIR(MRDP_HI, ddx[4], ddx[5]); out[n++] = MRDP_HI(ddx[6]) << 16;
        out[n++] = MRDP_PAIR(MRDP_LO, val[4], val[5]); out[n++] = MRDP_LO(val[6]) << 16;
        out[n++] = MRDP_PAIR(MRDP_LO, ddx[4], ddx[5]); out[n++] = MRDP_LO(ddx[6]) << 16;
        out[n++] = MRDP_PAIR(MRDP_HI, dde[4], dde[5]); out[n++] = MRDP_HI(dde[6]) << 16;
        out[n++] = MRDP_PAIR(MRDP_HI, ddy[4], ddy[5]); out[n++] = MRDP_HI(ddy[6]) << 16;
        out[n++] = MRDP_PAIR(MRDP_LO, dde[4], dde[5]); out[n++] = MRDP_LO(dde[6]) << 16;
        out[n++] = MRDP_PAIR(MRDP_LO, ddy[4], ddy[5]); out[n++] = MRDP_LO(ddy[6]) << 16;
    }
    if (flags & MRDP_SETUP_Z) {
        out[n++] = (uint32_t)val[7]; out[n++] = (uint32_t)ddx[7];
        out[n++] = (uint32_t)dde[7]; out[n++] = (uint32_t)ddy[7];
    }
#undef MRDP_PAIR
#undef MRDP_HI
#undef MRDP_LO
    return n;
}

static inline int mrdp_attr_on(unsigned flags, int i)
{
    return (flags & (i < 4 ? MRDP_SETUP_SHADE : i < 7 ? MRDP_SETUP_TEX : MRDP_SETUP_Z)) != 0;
}

/* Writes an N64 triangle command into out[] (up to 44 words) and returns the
 * number of words, or 0 for a degenerate triangle. flags = MRDP_SETUP_*. The
 * vertices' y are snapped in place. */
static inline int mrdp_setup_triangle(uint32_t *out, mrdp_vtx_t *va, mrdp_vtx_t *vb,
                                      mrdp_vtx_t *vc, unsigned flags, unsigned tile)
{
    mrdp_edges_t E;
    if (!mrdp_setup_edges(&E, va, vb, vc)) return 0;
    int n = mrdp_put_edges(out, &E, (uint32_t)(0x08 | (flags & 7)) << 24, tile);
    mrdp_fac_t fc = mrdp_factors(E.area, E.dy2, E.dy1, E.dx1, E.dx2);
    int32_t val[8], ddx[8], dde[8], ddy[8];
    for (int i = 0; i < 8; i++) {
        if (!mrdp_attr_on(flags, i)) { val[i] = ddx[i] = dde[i] = ddy[i] = 0; continue; }
        int32_t da1 = E.v1->a[i] - E.v0->a[i], da2 = E.v2->a[i] - E.v0->a[i];
        /* plane: dA/dx = (da1*dy2 - da2*dy1) / area, dA/dy = (da2*dx1 - da1*dx2) / area */
        int32_t gx = mrdp_fgrad(da1, fc.f[0], da2, fc.f[1], fc.e);
        int32_t gy = mrdp_fgrad(da2, fc.f[2], da1, fc.f[3], fc.e);
        ddx[i] = gx;
        ddy[i] = gy;
        dde[i] = gy + mrdp_mul16(gx, E.dxh);
        val[i] = E.v0->a[i] + mrdp_mul16(gx, E.xh - E.v0->x) + mrdp_mul16(gy, E.y0i - E.v0->y);
    }
    return mrdp_put_attrs(out, n, flags, val, ddx, dde, ddy);
}

/* ---- TRI_V / TRI_G: the per-attribute half in MRDP --------- *
 *
 * The geom core computed the gradients of every attribute in C: ~1000 of the
 * ~2350 cycles a triangle cost it. TRI_V carries the edges, four plane
 * factors F = d * 2^46 / area (d = dy2, dy1, dx1, dx2; |F| < 2^31, a FIXED
 * exponent, so MRDP needs no shifter), the offsets to YH's row on the H edge
 * (kx, ky), and each attribute's value at the three sorted vertices. MRDP
 * turns them into gradients with the multiplier it already has
 * (mrdp_vattr() is its arithmetic: the model's and the RTL's contract).
 *
 *   w0     0x10|flags <<24 | lft <<23 | tile <<16 | YL
 *   w1     YM <<16 | YH;  w2..w7  XL DxLDy XH DxHDy XM DxMDy (as the N64's)
 *   w8..11 F0 F1 F2 F3;   w12 kx = XH - x0;  w13 ky = YH row - y0 (16.16)
 *   w14..  per attribute on (R G B A, S T W, Z): a(v0) a(v1) a(v2)
 *   padded to an even count
 *
 * A sliver whose factors do not fit goes as TRI_G, gradients set up in C:
 *   w0..w7 as TRI_V with 0x18|flags; w8.. per attribute: value DaDx DaDe
 * (MRDP takes no N64-form triangles: their half-word attribute layout cost
 * more logic than both of these.)
 *
 * Decals are pulled toward the camera by 2 (|dZdx| + |dZdy|) + 2 LSB of
 * depth16 here, in the geom core: TRI_V's three vertex Z move by it (the
 * plane's slopes do not change), TRI_G's start value does. */
#define MRDP_OP_TRI_V 0x10u
#define MRDP_OP_TRI_G 0x18u

static inline unsigned mrdp_tri_v_attrs(unsigned flags)
{
    return ((flags & MRDP_SETUP_SHADE) ? 4u : 0u) + ((flags & MRDP_SETUP_TEX) ? 3u : 0u)
         + ((flags & MRDP_SETUP_Z) ? 1u : 0u);
}
static inline unsigned mrdp_tri_v_words(unsigned flags)
{
    return (14u + 3u * mrdp_tri_v_attrs(flags) + 1u) & ~1u;
}
static inline unsigned mrdp_tri_g_words(unsigned flags)
{
    return (8u + 3u * mrdp_tri_v_attrs(flags) + 1u) & ~1u;
}

/* gx or gy: (da * fa - db * fb) >> 30, saturated to +-0x7FFFFFFF */
static inline int32_t mrdp_vgrad(int32_t da, int32_t fa, int32_t db, int32_t fb)
{
    int64_t v = ((int64_t)da * fa - (int64_t)db * fb) >> 30;
    return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFF ? -0x7FFFFFFF : (int32_t)v;
}

/* one attribute from its three vertex values (all arithmetic wraps mod 2^32) */
static inline void mrdp_vattr(const int32_t *F, int32_t kx, int32_t ky, int32_t dxh,
                              int32_t a0, int32_t a1, int32_t a2,
                              int32_t *val, int32_t *dx, int32_t *de, int32_t *dy)
{
    int32_t da1 = (int32_t)((uint32_t)a1 - (uint32_t)a0), da2 = (int32_t)((uint32_t)a2 - (uint32_t)a0);
    int32_t gx = mrdp_vgrad(da1, F[0], da2, F[1]);
    int32_t gy = mrdp_vgrad(da2, F[2], da1, F[3]);
    *dx = gx;
    *dy = gy;
    *de = (int32_t)((uint32_t)gy + (uint32_t)mrdp_mul16(gx, dxh));
    *val = (int32_t)((uint32_t)a0 + (uint32_t)mrdp_mul16(gx, kx) + (uint32_t)mrdp_mul16(gy, ky));
}

/* the decal bias for a Z plane of slopes dzdx, dzdy */
static inline int64_t mrdp_decal_bias(int32_t dzdx, int32_t dzdy)
{
    return 2 * ((int64_t)(dzdx < 0 ? -(int64_t)dzdx : dzdx) + (dzdy < 0 ? -(int64_t)dzdy : dzdy)) + (2 << 15);
}
static inline int32_t mrdp_decal_z(int32_t z0, int32_t dzdx, int32_t dzdy)
{
    int64_t z = (int64_t)z0 - mrdp_decal_bias(dzdx, dzdy);
    return (int32_t)(z < 0 ? 0 : z);
}

/* F = d * 2^46 / area; 0 when one does not fit 31 bits (a sliver: TRI_G) */
static inline int mrdp_factors46(int64_t area, const int32_t *d, int32_t *F)
{
    int neg = area < 0, ea;
    /* 1 / |area| = r * 2^(-31 - ea), so F = |d| r 2^(15 - ea) */
    uint32_t r = mrdp_recip_norm(mrdp_norm64(neg ? (uint64_t)-area : (uint64_t)area, &ea));
    int s = ea - 15;
    if (s < 0) return 0;
    for (int k = 0; k < 4; k++) {
        uint32_t ad = d[k] < 0 ? 0u - (uint32_t)d[k] : (uint32_t)d[k];
        uint64_t p = s >= 64 ? 0 : ((uint64_t)ad * r) >> s;
        if (p >= 0x80000000u) return 0;
        F[k] = (neg ^ (d[k] < 0)) ? -(int32_t)p : (int32_t)p;
    }
    return 1;
}

/* TRI_G from edges set up: gradients in C (mrdp_fgrad), decal on the start */
static inline int mrdp_put_tri_g(uint32_t *out, const mrdp_edges_t *E, unsigned flags, unsigned tile, int decal)
{
    int n = mrdp_put_edges(out, E, (MRDP_OP_TRI_G | (flags & 7)) << 24, tile);
    mrdp_fac_t fc = mrdp_factors(E->area, E->dy2, E->dy1, E->dx1, E->dx2);
    for (int i = 0; i < 8; i++) {
        if (!mrdp_attr_on(flags, i)) continue;
        int32_t da1 = E->v1->a[i] - E->v0->a[i], da2 = E->v2->a[i] - E->v0->a[i];
        int32_t gx = mrdp_fgrad(da1, fc.f[0], da2, fc.f[1], fc.e);
        int32_t gy = mrdp_fgrad(da2, fc.f[2], da1, fc.f[3], fc.e);
        int32_t val = E->v0->a[i] + mrdp_mul16(gx, E->xh - E->v0->x) + mrdp_mul16(gy, E->y0i - E->v0->y);
        if (i == 7 && decal) val = mrdp_decal_z(val, gx, gy);
        out[n++] = (uint32_t)val;
        out[n++] = (uint32_t)gx;
        out[n++] = (uint32_t)(gy + mrdp_mul16(gx, E->dxh));
    }
    if (n & 1) out[n++] = 0;
    return n;
}

/* A TRI_V command, or (a sliver) TRI_G, into out[] (<= 38 words). Returns
 * the word count, 0 for a degenerate triangle. */
static inline int mrdp_setup_triangle_v(uint32_t *out, mrdp_vtx_t *va, mrdp_vtx_t *vb,
                                        mrdp_vtx_t *vc, unsigned flags, unsigned tile, int decal)
{
    mrdp_edges_t E;
    if (!mrdp_setup_edges(&E, va, vb, vc)) return 0;
    int32_t d[4] = { E.dy2, E.dy1, E.dx1, E.dx2 }, F[4];
    if (!mrdp_factors46(E.area, d, F)) return mrdp_put_tri_g(out, &E, flags, tile, decal);
    int32_t zb = 0;
    if (decal && (flags & MRDP_SETUP_Z)) {
        /* the slopes MRDP will compute (mrdp_vattr), and every vertex Z
         * moved by the bias: the plane's value drops by it, its slopes stay */
        int32_t z0 = E.v0->a[7];
        int32_t dz1 = (int32_t)((uint32_t)E.v1->a[7] - (uint32_t)z0), dz2 = (int32_t)((uint32_t)E.v2->a[7] - (uint32_t)z0);
        int64_t b = mrdp_decal_bias(mrdp_vgrad(dz1, F[0], dz2, F[1]), mrdp_vgrad(dz2, F[2], dz1, F[3]));
        zb = b > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)b;
    }
    int n = mrdp_put_edges(out, &E, (MRDP_OP_TRI_V | (flags & 7)) << 24, tile);
    out[n++] = (uint32_t)F[0]; out[n++] = (uint32_t)F[1];
    out[n++] = (uint32_t)F[2]; out[n++] = (uint32_t)F[3];
    out[n++] = (uint32_t)(E.xh - E.v0->x);
    out[n++] = (uint32_t)(E.y0i - E.v0->y);
    for (int i = 0; i < 8; i++) {
        if (!mrdp_attr_on(flags, i)) continue;
        uint32_t sub = i == 7 ? (uint32_t)zb : 0u;
        out[n++] = (uint32_t)E.v0->a[i] - sub; out[n++] = (uint32_t)E.v1->a[i] - sub; out[n++] = (uint32_t)E.v2->a[i] - sub;
    }
    if (n & 1) out[n++] = 0;
    return n;
}

/* ---- TRI_R: all of the setup in MRDP -------------------------------------- *
 *
 * TRI_V still left the edges, slopes and plane factors to the geom core's C
 * (~1300 cycles a triangle with the rest of the command). TRI_R carries the
 * three vertices, y snapped to quarter pixels and sorted top to bottom, and
 * each attribute's three values; MRDP does the rest -- mrdp_tri_r() and
 * mrdp_tri_r_attr() are its arithmetic: mrdp_setup_edges() without the snap
 * and sort it was given, mrdp_recip_norm()'s reciprocal, mrdp_factors()
 * (a per-triangle exponent) and mrdp_fgrad().
 *
 *   w0     0x20 <<24 | flags <<19 | tile <<16
 *   w1..6  x0 y0 x1 y1 x2 y2  (S15.16; y snapped, y0 <= y1 <= y2)
 *   w7..   per attribute on: a(v0) a(v1) a(v2);  padded to an even count
 *
 * The attribute values are the geom core's raw vertex fields; MRDP converts
 * them first (mrdp_tri_r_fields()): shade R G B A 0..1 -> x 255; texture
 * s, t in texels and w -> S = s W', T = t W', W' = w_min / w (<= 1, the
 * division an edge slope's, mrdp_slope()); Z 0..1 -> x 65535 / 2 (s16.15).
 *
 * The geom core sends TRI_R when the factors' exponent is >= 0 (e = 15 +
 * msb(|area|) - msb(OR of |d|) - 2, two clz), else TRI_G; decals go as TRI_V
 * (their bias needs Z's slopes). MRDP takes e < 0 as 0. */
#define MRDP_OP_TRI_R 0x20u

static inline unsigned mrdp_tri_r_words(unsigned flags)
{
    return (7u + 3u * mrdp_tri_v_attrs(flags) + 1u) & ~1u;
}

/* MRDP's TRI_R: edges, factors, offsets from the sorted vertices; returns
 * 0 if degenerate (then nothing is drawn) */
static inline int mrdp_tri_r(const int32_t *xy, mrdp_edges_t *E, mrdp_fac_t *fc, int32_t *kx, int32_t *ky)
{
    mrdp_vtx_t v[3];
    for (int k = 0; k < 3; k++) { v[k].x = xy[2 * k]; v[k].y = xy[2 * k + 1]; }
    if (!mrdp_edges_of(E, &v[0], &v[1], &v[2])) return 0;
    *fc = mrdp_factors(E->area, E->dy2, E->dy1, E->dx1, E->dx2);
    if (fc->e < 0) fc->e = 0;
    *kx = E->xh - v[0].x;
    *ky = E->y0i - v[0].y;
    return 1;
}

/* TRI_R's attribute triplets (a[i][k]: attribute i at sorted vertex k) from
 * the raw fields to MRDP's formats, in place */
static inline void mrdp_tri_r_fields(int32_t a[8][3], unsigned flags)
{
    if (flags & MRDP_SETUP_SHADE)
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 3; k++)
                a[i][k] = (int32_t)(((uint32_t)a[i][k] << 8) - (uint32_t)a[i][k]);
    if (flags & MRDP_SETUP_TEX) {
        int32_t wref = a[6][0];
        if (a[6][1] < wref) wref = a[6][1];
        if (a[6][2] < wref) wref = a[6][2];
        for (int k = 0; k < 3; k++) {
            int32_t w = a[6][k];
            int32_t iw = (w != wref && w >= (1 << 17)) ? mrdp_slope30(wref, w) : 0x40000000;
            a[4][k] = (int32_t)(((int64_t)a[4][k] * iw) >> 30);
            a[5][k] = (int32_t)(((int64_t)a[5][k] * iw) >> 30);
            a[6][k] = iw;
        }
    }
    if (flags & MRDP_SETUP_Z)
        for (int k = 0; k < 3; k++)
            a[7][k] = (int32_t)(((int64_t)a[7][k] * 65535) >> 1);
}

/* one attribute of a TRI_R: mrdp_vattr() with the triangle's own exponent */
static inline void mrdp_tri_r_attr(const mrdp_fac_t *fc, int32_t kx, int32_t ky, int32_t dxh,
                                   int32_t a0, int32_t a1, int32_t a2,
                                   int32_t *val, int32_t *dx, int32_t *de, int32_t *dy)
{
    int32_t da1 = (int32_t)((uint32_t)a1 - (uint32_t)a0), da2 = (int32_t)((uint32_t)a2 - (uint32_t)a0);
    int32_t gx = mrdp_fgrad(da1, fc->f[0], da2, fc->f[1], fc->e);
    int32_t gy = mrdp_fgrad(da2, fc->f[2], da1, fc->f[3], fc->e);
    *dx = gx;
    *dy = gy;
    *de = (int32_t)((uint32_t)gy + (uint32_t)mrdp_mul16(gx, dxh));
    *val = (int32_t)((uint32_t)a0 + (uint32_t)mrdp_mul16(gx, kx) + (uint32_t)mrdp_mul16(gy, ky));
}

/* raw vertex fields -> MRDP's formats (mrdp_tri_r_fields()), for the forms
 * the geom core sets up itself */
static inline void mrdp_vtx_fields(mrdp_vtx_t *va, mrdp_vtx_t *vb, mrdp_vtx_t *vc, unsigned flags)
{
    mrdp_vtx_t *v[3] = { va, vb, vc };
    int32_t a[8][3];
    for (int i = 0; i < 8; i++) for (int k = 0; k < 3; k++) a[i][k] = v[k]->a[i];
    mrdp_tri_r_fields(a, flags);
    for (int i = 0; i < 8; i++) for (int k = 0; k < 3; k++) v[k]->a[i] = a[i][k];
}

/* The geom core's side, from raw vertex fields: snap, sort, and TRI_R -- or
 * TRI_G if the exponent would be negative, TRI_V for a decal. <= 38 words. */
static inline int mrdp_setup_triangle_r(uint32_t *out, mrdp_vtx_t *va, mrdp_vtx_t *vb,
                                        mrdp_vtx_t *vc, unsigned flags, unsigned tile, int decal)
{
    if (decal && (flags & MRDP_SETUP_Z)) {
        mrdp_vtx_fields(va, vb, vc, flags);
        return mrdp_setup_triangle_v(out, va, vb, vc, flags, tile, 1);
    }
    va->y = (int32_t)(((uint32_t)va->y + 0x2000u) & ~0x3FFFu);
    vb->y = (int32_t)(((uint32_t)vb->y + 0x2000u) & ~0x3FFFu);
    vc->y = (int32_t)(((uint32_t)vc->y + 0x2000u) & ~0x3FFFu);
    const mrdp_vtx_t *v0 = va, *v1 = vb, *v2 = vc, *t;
    if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
    if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; }
    if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
    int32_t dx1 = v1->x - v0->x, dy1 = v1->y - v0->y, dx2 = v2->x - v0->x, dy2 = v2->y - v0->y;
    int64_t area = (int64_t)dx1 * dy2 - (int64_t)dx2 * dy1;
    if (area == 0 || dy2 == 0) return 0;
    /* mrdp_factors()'s exponent: 15 + msb(|area|) - (msb(OR |d|) + 2) */
    uint32_t dor = 0, d[4] = { (uint32_t)dy2, (uint32_t)dy1, (uint32_t)dx1, (uint32_t)dx2 };
    for (int k = 0; k < 4; k++) dor |= (int32_t)d[k] < 0 ? 0u - d[k] : d[k];
    int e = 15 + (63 - mrdp_clz64(area < 0 ? (uint64_t)-area : (uint64_t)area)) - (33 - mrdp_clz32(dor));
    if (e < 0) {                                         /* extreme sliver: set up here */
        mrdp_edges_t E;
        mrdp_vtx_t s0 = *v0, s1 = *v1, s2 = *v2;
        mrdp_vtx_fields(&s0, &s1, &s2, flags);
        if (!mrdp_setup_edges(&E, &s0, &s1, &s2)) return 0;
        return mrdp_put_tri_g(out, &E, flags, tile, 0);
    }
    int n = 0;
    out[n++] = (MRDP_OP_TRI_R << 24) | ((flags & 7u) << 19) | ((tile & 7u) << 16);
    out[n++] = (uint32_t)v0->x; out[n++] = (uint32_t)v0->y;
    out[n++] = (uint32_t)v1->x; out[n++] = (uint32_t)v1->y;
    out[n++] = (uint32_t)v2->x; out[n++] = (uint32_t)v2->y;
    for (int i = 0; i < 8; i++) {
        if (!mrdp_attr_on(flags, i)) continue;
        out[n++] = (uint32_t)v0->a[i]; out[n++] = (uint32_t)v1->a[i]; out[n++] = (uint32_t)v2->a[i];
    }
    if (n & 1) out[n++] = 0;
    return n;
}

/* always TRI_G (tests) */
static inline int mrdp_setup_triangle_g(uint32_t *out, mrdp_vtx_t *va, mrdp_vtx_t *vb,
                                        mrdp_vtx_t *vc, unsigned flags, unsigned tile, int decal)
{
    mrdp_edges_t E;
    if (!mrdp_setup_edges(&E, va, vb, vc)) return 0;
    return mrdp_put_tri_g(out, &E, flags, tile, decal);
}

#endif
