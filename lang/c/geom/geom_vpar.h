/* CFU intrinsics for rtl/vpu/Vpu4DFixed.v -- the parallel fixed-point
 * matrix-vector engine. Custom-0 R-type
 * instructions, function id = {funct7[6:0], funct3[2:0]} (10 bits),
 * matching the wiring of the SoC and the RTL's own function-id map:
 *
 *   0x00  M[rs2&0xF] = rs1   resident 4x4 matrix element, row-major, S17.10
 *                            -- MATVEC4 only.
 *   0x01  A[rs2&0x3] = rs1   resident input vector element, S17.10
 *   0x02  B[rs2&0x3] = rs1   resident B vector element, S17.10 -- VMUL4/VFMA4 only.
 *   0x03  C[rs2&0x3] = rs1   resident C (seed) vector element, S17.10 -- VFMA4 only.
 *   0x30  MATVEC4            out[j] = sum_i A[i]*M[i*4+j] for all 4 j
 *   0x31  VMUL4              out[i] = A[i]*B[i] for all 4 i (element-wise)
 *   0x32  VFMA4              out[i] = A[i]*B[i] + C[i] for all 4 i
 *   0x08  rd = out[rs2&0x3]  read one output component
 *
 * VMUL4/VFMA4 (2026-09-22): the perspective-divide and viewport-mapping
 * shapes load_vertices() still ran as sequential scalar geom_fixed.c ops
 * after MATVEC4 -- these two move that onto the same 4 parallel lanes,
 * reusing the same DSP-mapped multiply-accumulate units (see
 * rtl/vpu/Vpu4DFixed.v's header for the standalone-synthesis area numbers
 * and the three real RTL bugs found bringing them up). B/C are their OWN
 * register files, NOT M[0..3]/M[4..7] -- the real per-vertex call sequence
 * is MATVEC4 (needs the full MVP matrix resident) immediately followed by
 * VMUL4/VFMA4 for that same vertex, and a design that shared M's storage
 * would force the ENTIRE 16-element matrix to be reloaded before every
 * vertex's MATVEC4 (see Vpu4DFixed.v's header for how this was caught,
 * before it ever reached firmware). Per-vertex lighting (up to 4 light
 * directions dotted against one normal) needs no new opcode: load M's 4
 * columns with 4 light directions and A with the normal (A[3]=0), MATVEC4
 * computes all 4 dot products at once -- see geom_vpar_light4() below.
 *
 * Used by geom_pipeline.c's load_vertices() (MATVEC4: the per-vertex MVP
 * transform) and light_vertices() (MATVEC4 with object-space lights as M's
 * columns). VMUL4/VFMA4 are NOT used by the pipeline -- see the perspective
 * divide comment in load_vertices() for the measured precision reasons. The
 * *_f float wrappers below are for test/test_vpar.c.
 */
#ifndef GEOM_VPAR_H
#define GEOM_VPAR_H

#include <stdint.h>
#include "geom_fixed.h"

#define GEOM_VPAR_FRAC_BITS 10
#define GEOM_VPAR_ONE_Q     (1 << GEOM_VPAR_FRAC_BITS)
#define GEOM_VPAR_MAX_RAW   ((int32_t)((1 << 26) - 1))
#define GEOM_VPAR_MIN_RAW   ((int32_t)(-(1 << 26)))

/* float (IEEE-754 bit pattern) <-> S17.10 raw (27-bit two's complement
 * carried in a 32-bit int) conversion, PURE INTEGER -- no hardware or
 * soft-float multiply/divide. Matches sim/vpu/tb_vpu4dfixed.cpp's
 * to_fixed()/from_fixed() and the RTL's own round_sat() -- see that file for
 * the precision study this format was validated against. This used to do
 * `x * (float)GEOM_VPAR_ONE_Q` (a real fmul.s / soft-float multiply); once
 * the geom core drops the F extension entirely (see geom_fixed.h's header),
 * that line would either fail to link (no soft-float in this freestanding
 * build) or silently pull in libgcc soft-float bloat -- same class of
 * problem geom_fixed.h solves for scalar math, applied here to the CFU's
 * own S17.10 boundary. Same bit-decode algorithm as geom_fixed.c's
 * fx_from_bits()/fx_to_bits(), just at 10 fractional bits and this format's
 * own 27-bit raw range instead of geom_fixed.h's S15.16 -- routed through
 * fx_from_bits_n()/fx_to_bits_n() (geom_fixed.c's shared generic core) so
 * this stays a thin wrapper instead of a second ~90-line copy of the same
 * logic (that duplication briefly existed and cost enough ROM, on top of
 * geom_fixed.h's own per-TU-inline duplication, to overflow the geom core's
 * 16KB ROM -- see geom_fixed.h's header). */
#ifdef GEOM_FX_CONV_CFU
int32_t geom_vpar_to_fixed_bits(uint32_t bits);
#endif
static inline int32_t geom_vpar_to_fixed(float xf) {
    union { float f; uint32_t u; } c = { .f = xf };
#ifdef GEOM_FX_CONV_CFU
    return geom_vpar_to_fixed_bits(c.u);   /* geom_fixed.c: CFU if present, else the C below */
#else
    return fx_from_bits_n(c.u, GEOM_VPAR_FRAC_BITS, GEOM_VPAR_MAX_RAW, GEOM_VPAR_MIN_RAW);
#endif
}
static inline float geom_vpar_from_fixed(int32_t x) {
    union { float f; uint32_t u; } c = { .u = fx_to_bits_n(x, GEOM_VPAR_FRAC_BITS) };
    return c.f;
}

#if defined(__mips__) && !defined(GEOM_HOST_TEST)

#include "mips_cfu.h"             /* the geom core on MIPS: fid = insn[9:0] */
#define GEOM_VPAR_INSN(function_id, rd_expr, rs1_expr, rs2_expr) \
    ((rd_expr) = MCFU(function_id, rs1_expr, rs2_expr))

static inline void geom_vpar_load_m(int idx, int32_t raw) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x00, rd, raw, idx);
}
static inline void geom_vpar_load_a(int idx, int32_t raw) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x01, rd, raw, idx);
}
static inline void geom_vpar_load_b(int idx, int32_t raw) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x02, rd, raw, idx);
}
static inline void geom_vpar_load_c(int idx, int32_t raw) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x03, rd, raw, idx);
}
/* 0x33: A = (x, y, z, 1) from packed int16 object coordinates, then
 * MATVEC4 -- xy = y << 16 | (x & 0xFFFF) (a geom_vtx_t's first word). */
static inline void geom_vpar_matvec_ob(uint32_t xy, int32_t z) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x33, rd, xy, z);
    (void)rd;
}
/* 0x34: as 0x33 with x, y, z at S17.4 (the constant 1 at S17.10): the
 * x/y/z rows of M then carry 6 more fraction bits for the same output. */
static inline void geom_vpar_matvec_ob_hp(uint32_t xy, int32_t z) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x34, rd, xy, z);
    (void)rd;
}
static inline void geom_vpar_matvec4(void) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x30, rd, 0, 0);
}
static inline void geom_vpar_vmul4(void) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x31, rd, 0, 0);
}
static inline void geom_vpar_vfma4(void) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x32, rd, 0, 0);
}
static inline int32_t geom_vpar_read_out(int idx) {
    int32_t rd;
    GEOM_VPAR_INSN(0x08, rd, 0, idx);
    return rd;
}

/* Triangle-setup helpers (rtl/vpu/GeomSetupUnit.v): target only. Their
 * host counterparts are the C they replace (geom_triangle.c blend_attr(),
 * geom_fixed.c fx_recip_norm()), which the host build keeps running, so the
 * host stays the bit-exact reference sim/geom_full/check_host_vs_rtl.sh
 * compares the RTL against.
 *   0x04  W[rs2] = rs1 (0..5: W1 W2 X1 X2 Y1 Y2)
 *   0x40  a1 = rs1, a2 = rs2
 *   0x41  rd = fxr((a0<<16) + d1*W1 + d2*W2, rs2) for rs1 = a0; X/Y computed too.
 *         fxr(p, s) = p / 2^s rounded (halves away from zero), saturated:
 *         a fixed-point attribute word (s = 8 colour, 2 depth, 4 tex)
 *   0x42  rd = fxr(d1*X1 + d2*X2)     0x43  rd = fxr(d1*Y1 + d2*Y2)
 *   0x44  like 0x41 but the 3-term blend_fx() form: W6..W8 = wN[0], wXN[0],
 *         wYN[0] (0x04 idx 6..8) with W0..W5 as above
 *   0x50  rd = fx_recip_norm(rs1).r   0x51  rd = its .e
 *   0x52  rd = fx_mul(rs1, rs2)       0x53  rd = fx_div_norm(rs1, last 0x50)
 *   0x54  rd = len2 ? (1<<24) / isqrt_u32(len2 << 8) : 0  (light_vertices) */
#define GEOM_SETUP_HW 1
/* The unit's edge-setup ops (0x62, 0x64..0x67) are not used: MRDP takes N64
 * edge slopes (mrdp_setup.h), computed in C -- the cull verdict then comes
 * from the same code the host runs -- and the gateware is built SU_NO_EDGE
 * (~540 ALMs). */
/* 0x5A: out[idx] of the output buffer -- a blend's X/Y deposits, PROJECT's
 * results */
static inline uint32_t geom_su_read(unsigned idx) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x5A, rd, 0, idx);
    return rd;
}
static inline void geom_setup_load_w(int idx, int32_t v) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x04, rd, v, idx);
}
static inline void geom_setup_blend_ld(int32_t a1, int32_t a2) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x40, rd, a1, a2);
}
static inline uint32_t geom_setup_blend_go(int32_t a0, int sh) {   /* sh: 8 colour, 2 depth, 4 texture */
    uint32_t rd;
    GEOM_VPAR_INSN(0x41, rd, a0, sh);
    return rd;
}
/* 0x46 / 0x47: the blends above that also write V, X, Y into the CFU's
 * output buffer at iv, ix, iy (GeomSetupUnit.v); 0x60 writes one word there,
 * 0x61 streams out[0 .. n-1] into MRDP's command FIFO. */
static inline uint32_t geom_setup_blend_out(int32_t a0, int sh, unsigned iv, unsigned ix, unsigned iy) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x46, rd, a0, (uint32_t)sh | iv << 8 | ix << 16 | iy << 24);
    return rd;
}
static inline void geom_setup_dwr(uint32_t v, unsigned idx) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x60, rd, v, idx);
    (void)rd;
}
/* 0x63 APPEND2: out[p] = a, out[p + 1] = b, p += 2; PUSH clears p */
static inline void geom_setup_append2(uint32_t a, uint32_t b) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x63, rd, a, b);
    (void)rd;
}
static inline void geom_setup_push(unsigned n) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x61, rd, n, 0);
    (void)rd;
}
/* Edge setup (GeomSetupUnit 0x62, 0x64..0x67): geom_triangle.c's
 * edges_of() and the integer half of geom_triangle_setup_e() -- edge-space
 * coordinates, guard, cull, bounding box, edge functions, 1/area and the
 * normalised weights (left in W0..W8), descriptor words 1..11 into the
 * output buffer. */
static inline void geom_edge_cfg(unsigned idx, uint32_t v) {   /* 0..3 scissor, 4 cull */
    uint32_t rd;
    GEOM_VPAR_INSN(0x62, rd, v, idx);
    (void)rd;
}
static inline void geom_edge_v0(int32_t x, int32_t y) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x64, rd, x, y);
    (void)rd;
}
static inline void geom_edge_v1(int32_t x, int32_t y) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x65, rd, x, y);
    (void)rd;
}
static inline uint32_t geom_edge_v2(int32_t x, int32_t y) {   /* 1: rejected */
    uint32_t rd;
    GEOM_VPAR_INSN(0x66, rd, x, y);
    return rd;
}
static inline uint32_t geom_edge_setup(void) {                /* 0: empty box */
    uint32_t rd;
    GEOM_VPAR_INSN(0x67, rd, 0, 0);
    return rd;
}
static inline int32_t geom_setup_normf(uint32_t len2) {      /* 0x54 */
    uint32_t rd;
    GEOM_VPAR_INSN(0x54, rd, len2, 0);
    return (int32_t)rd;
}
static inline uint32_t geom_setup_recipn(uint32_t a) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x50, rd, a, 0);
    return rd;
}
static inline int geom_setup_recipn_e(void) {
    uint32_t rd;
    GEOM_VPAR_INSN(0x51, rd, 0, 0);
    return (int)rd;
}
/* PROJECT (rtl/vpu/GeomSetupUnit.v 0x58..0x5A): load_vertices()'s
 * projection of the last MATVEC4 in one op, bit-exact with its C. */
#ifndef SU_NO_PROJECT   /* a bitstream built without it: the C does the projection */
#define GEOM_PROJECT_HW 1
#endif
static inline void geom_proj_set(int idx, int32_t v) {    /* 0 sx 1 tx 2 sy 3 ty 4 shifts */
    uint32_t rd;
    GEOM_VPAR_INSN(0x58, rd, v, idx);
    (void)rd;
}
static inline int32_t geom_proj(void) {                    /* rd = w (S15.16) */
    int32_t rd;
    GEOM_VPAR_INSN(0x59, rd, 0, 0);
    return rd;
}
static inline int32_t geom_proj_read(int idx) {            /* 0 x 1 y 2 z 3 cx 4 cy 5 cz */
    return (int32_t)geom_su_read(56u + (unsigned)idx);     /* PROJECT leaves them in out[56..61] */
}

#else /* host / non-RISC-V build: plain-C model matching the RTL exactly
       * (same rounding, same saturation) -- lets host tests exercise the
       * SAME call sequence firmware will use, without real hardware. */

static int32_t g_vpar_m[16];
static int32_t g_vpar_a[4];
static int32_t g_vpar_b[4];
static int32_t g_vpar_c[4];
static int32_t g_vpar_out[4];

static inline void geom_vpar_load_m(int idx, int32_t raw) { g_vpar_m[idx & 0xF] = raw; }
static inline void geom_vpar_load_a(int idx, int32_t raw) { g_vpar_a[idx & 0x3] = raw; }
static inline void geom_vpar_load_b(int idx, int32_t raw) { g_vpar_b[idx & 0x3] = raw; }
static inline void geom_vpar_load_c(int idx, int32_t raw) { g_vpar_c[idx & 0x3] = raw; }
static inline int32_t geom_vpar_round_sat_(int64_t acc) {
    int64_t rounded = acc + (1LL << (GEOM_VPAR_FRAC_BITS - 1));
    rounded >>= GEOM_VPAR_FRAC_BITS;
    if (rounded > GEOM_VPAR_MAX_RAW) rounded = GEOM_VPAR_MAX_RAW;
    if (rounded < GEOM_VPAR_MIN_RAW) rounded = GEOM_VPAR_MIN_RAW;
    return (int32_t)rounded;
}
static inline void geom_vpar_matvec4(void);
static inline void geom_vpar_matvec_ob(uint32_t xy, int32_t z) {
    g_vpar_a[0] = (int32_t)(int16_t)(xy & 0xFFFFu) * GEOM_VPAR_ONE_Q;
    g_vpar_a[1] = (int32_t)(int16_t)(xy >> 16) * GEOM_VPAR_ONE_Q;
    g_vpar_a[2] = (int32_t)(int16_t)z * GEOM_VPAR_ONE_Q;
    g_vpar_a[3] = GEOM_VPAR_ONE_Q;
    geom_vpar_matvec4();
}
static inline void geom_vpar_matvec_ob_hp(uint32_t xy, int32_t z) {
    g_vpar_a[0] = (int32_t)(int16_t)(xy & 0xFFFFu) * (GEOM_VPAR_ONE_Q >> 6);
    g_vpar_a[1] = (int32_t)(int16_t)(xy >> 16) * (GEOM_VPAR_ONE_Q >> 6);
    g_vpar_a[2] = (int32_t)(int16_t)z * (GEOM_VPAR_ONE_Q >> 6);
    g_vpar_a[3] = GEOM_VPAR_ONE_Q;
    geom_vpar_matvec4();
}
static inline void geom_vpar_matvec4(void) {
    for (int j = 0; j < 4; j++) {
        int64_t acc = 0;
        for (int i = 0; i < 4; i++) acc += (int64_t)g_vpar_a[i] * (int64_t)g_vpar_m[i * 4 + j];
        g_vpar_out[j] = geom_vpar_round_sat_(acc);
    }
}
/* B/C are their own register files (not M[0..3]/M[4..7] -- see this file's
 * header for why), matching the RTL's b_l/c_l + a_operand()'s per-lane (not
 * per-term) A select exactly: element i uses A[i] and B[i], not a
 * shared/broadcast term index (see Vpu4DFixed.v's header for the real bug
 * this distinction fixed). */
static inline void geom_vpar_vmul4(void) {
    for (int i = 0; i < 4; i++) g_vpar_out[i] = geom_vpar_round_sat_((int64_t)g_vpar_a[i] * (int64_t)g_vpar_b[i]);
}
/* C is Q(FB) scale (a plain S17.10 value) but A[i]*B[i] is Q(2*FB) scale,
 * so C must be shifted left by FB before the add -- matching the RTL's
 * seed-scale fix exactly (see its header for the real bug this was). */
static inline void geom_vpar_vfma4(void) {
    for (int i = 0; i < 4; i++)
        g_vpar_out[i] = geom_vpar_round_sat_((int64_t)g_vpar_a[i] * (int64_t)g_vpar_b[i]
                                              + ((int64_t)g_vpar_c[i] << GEOM_VPAR_FRAC_BITS));
}
static inline int32_t geom_vpar_read_out(int idx) { return g_vpar_out[idx & 0x3]; }

#endif

/* Convenience: transform a homogeneous float vec4 by a float 4x4 matrix
 * (row-major, out[j] = sum_i v[i]*m[i*4+j]) through the CFU, matching
 * gm_matvec()'s existing signature so this is a drop-in candidate for
 * load_vertices()'s call site once ready -- not called from geom_pipeline.c
 * yet. */
static inline void geom_vpar_matvec_f(const float m[16], const float v[4], float out[4]) {
    for (int i = 0; i < 16; i++) geom_vpar_load_m(i, geom_vpar_to_fixed(m[i]));
    for (int i = 0; i < 4; i++)  geom_vpar_load_a(i, geom_vpar_to_fixed(v[i]));
    geom_vpar_matvec4();
    for (int j = 0; j < 4; j++) out[j] = geom_vpar_from_fixed(geom_vpar_read_out(j));
}

/* out[i] = normal . lights[i] for i in 0..3 -- up to 4 light directions
 * dotted against one normal, all at once, via MATVEC4 with M's 4 columns
 * loaded from the light directions and A[3]=0 to drop the unused 4th
 * term (normals are 3D). This is NOT a new hardware op: MATVEC4 already
 * computes out[j] = sum_i A[i]*M[i*4+j], which IS a dot product per
 * column when A is shared across all 4 -- exactly what N lights dotted
 * against one shared normal needs. `lights[k]` unused (k >= n_lights)
 * should be passed as {0,0,0} so their column contributes nothing (the
 * caller decides which of the 4 output slots are meaningful). Uses M, so
 * it CANNOT be interleaved with a resident MATVEC4-transform matrix
 * without reloading that matrix afterward -- call this between vertex
 * batches (or before load_vertices() reloads M for the next one), not
 * mid-batch. */
static inline void geom_vpar_light4(const float normal[3], const float lights[4][3], float out[4]) {
    for (int lane = 0; lane < 4; lane++)
        for (int comp = 0; comp < 3; comp++)
            geom_vpar_load_m(comp * 4 + lane, geom_vpar_to_fixed(lights[lane][comp]));
    for (int comp = 0; comp < 3; comp++) geom_vpar_load_m(3 * 4 + comp, 0);   /* row 3 unused, but keep it defined */
    geom_vpar_load_a(0, geom_vpar_to_fixed(normal[0]));
    geom_vpar_load_a(1, geom_vpar_to_fixed(normal[1]));
    geom_vpar_load_a(2, geom_vpar_to_fixed(normal[2]));
    geom_vpar_load_a(3, 0);   /* drop the 4th term entirely */
    geom_vpar_matvec4();
    for (int lane = 0; lane < 4; lane++) out[lane] = geom_vpar_from_fixed(geom_vpar_read_out(lane));
}

/* out[i] = a[i]*b[i] for i in 0..3, element-wise -- e.g. the perspective
 * divide's x,y,z * a broadcast 1/w (repeat invw into b[0..2]). Safe to
 * interleave with a resident MATVEC4 matrix (uses B, not M). */
static inline void geom_vpar_vmul4_f(const float a[4], const float b[4], float out[4]) {
    for (int i = 0; i < 4; i++) geom_vpar_load_b(i, geom_vpar_to_fixed(b[i]));
    for (int i = 0; i < 4; i++) geom_vpar_load_a(i, geom_vpar_to_fixed(a[i]));
    geom_vpar_vmul4();
    for (int i = 0; i < 4; i++) out[i] = geom_vpar_from_fixed(geom_vpar_read_out(i));
}

/* out[i] = a[i]*b[i] + c[i] for i in 0..3 -- e.g. viewport scale+translate
 * (b=scale, c=translate, loaded once per vertex batch since they're
 * frame-constant; only `a` changes per vertex). Safe to interleave with a
 * resident MATVEC4 matrix (uses B/C, not M). */
static inline void geom_vpar_vfma4_f(const float a[4], const float b[4], const float c[4], float out[4]) {
    for (int i = 0; i < 4; i++) geom_vpar_load_b(i, geom_vpar_to_fixed(b[i]));
    for (int i = 0; i < 4; i++) geom_vpar_load_c(i, geom_vpar_to_fixed(c[i]));
    for (int i = 0; i < 4; i++) geom_vpar_load_a(i, geom_vpar_to_fixed(a[i]));
    geom_vpar_vfma4();
    for (int i = 0; i < 4; i++) out[i] = geom_vpar_from_fixed(geom_vpar_read_out(i));
}

#endif /* GEOM_VPAR_H */
