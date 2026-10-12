/* Pure-integer S15.16 fixed-point arithmetic for the geometry core's
 * SOFTWARE math (scalar add/mul/recip/itof/ftoi, WSUM3 triangle-setup
 * blends) -- backs every op in geom_vecmath.h. NO dependency on hardware
 * floating point: every operation here compiles to native rv32im
 * instructions (mul/mulh/div; this core has no 'F' extension).
 *
 * Why S15.16, NOT the CFU's S17.10 (rtl/vpu/Vpu4DFixed.v /
 * lang/c/geom/geom_vpar.h): those 10 fractional bits were chosen so a
 * 27-bit operand packs into exactly ONE Cyclone V DSP block for the
 * PARALLEL hardware matrix-vector CFU -- a real synthesis constraint. This
 * header has no such constraint: it's plain int64 C arithmetic running on
 * the CPU, so it costs nothing to give it MORE fractional precision. S15.16
 * (1 sign + 15 integer + 16 fraction = 32 bits, fits int32_t exactly) also
 * matches the real N64's own float->fixed matrix format (libultra's
 * guMtxF2L() scales by 65536), so this format needs no separate precision
 * case-study: it inherits the original hardware's. Integer range (+-32767)
 * comfortably covers the worst clip-space value measured on real game
 * content (~23,659).
 *
 * This is a genuinely different, WIDER format than the CFU's S17.10 --
 * with the CFU's 10 fractional bits a projection scale of exactly 1/1000
 * lost ~2.3% per axis (1/1000 * 1024 = 1.024, rounds to 1 ->
 * 0.0009765625, a real quantization error); 16 fractional bits represent
 * 1/1000 to within 1/65536, matching the N64 precedent that never hit this
 * problem. Software scalar ops (this file) get that full precision;
 * the CFU's parallel per-vertex transform (geom_vpar.h) still quantizes to
 * S17.10 at its own boundary -- a separate, smaller-impact tradeoff,
 * not fixed by this file.
 *
 * The `float` C TYPE is kept as the interface throughout geom_pipeline.c/
 * geom_triangle.c/geom_vecmath.h -- only the bit PATTERN is used (via the
 * existing f2u()/u2f() union trick already in geom_vecmath.h for its
 * sign-bit comparisons), never a real hardware add/mul/div on it. A
 * `float`-typed local with no FPU present just becomes "a 32-bit value
 * moved with lw/sw", which rv32im already does fine -- the C source barely
 * changes, but no fadd.s/fmul.s/fdiv.s/fcvt.* ever gets emitted.
 */
#ifndef GEOM_FIXED_H
#define GEOM_FIXED_H

#include <stdint.h>

#define FX_FRAC_BITS 16
#define FX_ONE       (1 << FX_FRAC_BITS)
#define FX_MAX_RAW   ((int32_t)0x7FFFFFFF)
#define FX_MIN_RAW   ((int32_t)0x80000000)

/* Real (non-inline) functions, defined once in geom_fixed.c -- these used to
 * be `static inline` here, which meant every one of geom_pipeline.c/
 * geom_triangle.c/main.c (each includes this header) got its OWN compiled
 * copy. That cost enough duplicated code to overflow the geom core's 16KB
 * ROM by ~1.9KB the first time this header's functions were exercised on
 * the real rv32im (no F) target -- caught at link time (`section .text will
 * not fit in region rom`), not by any host test (the host build has no ROM
 * budget to overflow). One compiled definition + ordinary calls costs a
 * jal/ret per use instead, which this code's call volume (a few hundred
 * fixed-point ops per triangle, not a per-pixel inner loop) doesn't need to
 * avoid.
 *
 * fx_clamp() and the div/wsum3/bits comments live with their
 * implementations in geom_fixed.c now; see that file. */
int32_t  fx_clamp(int64_t v);
int32_t  fx_mul(int32_t a, int32_t b);
int32_t  fx_add(int32_t a, int32_t b);
int32_t  fx_sub(int32_t a, int32_t b);
int32_t  fx_recip(int32_t a);
int32_t  fx_div_ii(int32_t num, int32_t den);
int32_t  fx_wsum3(int32_t a, int32_t w0, int32_t b, int32_t w1, int32_t c, int32_t w2);
int32_t  fx_from_int(int32_t i);
/* Inline: called per triangle corner (to_edge_fixed), and a call into
 * geom_fixed.c costs more than the body. */
static inline int32_t fx_to_int_round(int32_t x) {
    int32_t half = FX_ONE >> 1;
    return (x >= 0) ? (x + half) >> FX_FRAC_BITS : -(((-x) + half) >> FX_FRAC_BITS);
}
int32_t  fx_from_bits(uint32_t bits);
uint32_t fx_to_bits(int32_t fx);

/* Generic core of fx_from_bits()/fx_to_bits(), parameterised over fractional
 * bits and clamp range -- shared with geom_vpar.h's S17.10 CFU boundary
 * conversion (geom_vpar_to_fixed()/geom_vpar_from_fixed()), which is the
 * exact same IEEE-754 bit-decode algorithm at different constants. Exposed
 * here (not `static` in geom_fixed.c) specifically so geom_vpar.h doesn't
 * need its own ~90-line duplicate of this logic -- that duplication existed
 * for one review cycle and was the difference between fitting the geom
 * core's 16KB ROM and not (see this header's comment above; -DDIAG/hosttest
 * builds don't have a ROM budget, so this was invisible there). */
/* High-precision x * 2^16 / a for a >= FX_RECIP_NORM_MIN (2^17): prepare
 * the divisor once with fx_recip_norm(), then fx_div_norm() per numerator
 * (~30-bit reciprocal; x/a exact to rounding). */
#define FX_RECIP_NORM_MIN (1 << 17)
typedef struct { uint32_t r; int e; } fx_recip_norm_t;
fx_recip_norm_t fx_recip_norm(uint32_t a);
int32_t fx_div_norm(int32_t x, fx_recip_norm_t k);

int32_t  fx_from_bits_n(uint32_t bits, int frac_bits, int32_t max_raw, int32_t min_raw);
uint32_t fx_to_bits_n(int32_t fx, int frac_bits);

/* On the geom core, fx_mul() is ONE instruction: rtl/vpu/GeomSetupUnit.v's
 * 0x52 computes exactly the C in geom_fixed.c (checked by
 * sim/vpu/tb_setupunit.cpp) in 5 cycles, where the call + 64-bit multiply +
 * rounding + fx_clamp() call cost ~35. fx_div_norm_last() is 0x53: x divided
 * by the divisor of the most recent fx_recip_norm() -- only for a caller that
 * has just called fx_recip_norm() for that divisor with nothing in between
 * (divw(), the barycentric weights). The host keeps the C (the reference). */
#if defined(__mips__) && !defined(GEOM_HOST_TEST)
#include "mips_cfu.h"             /* the geom core on MIPS (rtl/mips/mips_geom.sv) */
#define GEOM_FX_CFU(id, rd, a, b) ((rd) = MCFU(id, a, b))
static inline __attribute__((always_inline)) int32_t fx_mul_cfu(int32_t a, int32_t b) {
    int32_t rd; GEOM_FX_CFU(0x52, rd, a, b); return rd;
}
/* x / a for the divisor of the most recent fx_recip_norm(), given its k.r:
 * k.e is already in the unit (a hardware RECIPN leaves it there, the
 * software fallback sends it with 0x57). */
static inline __attribute__((always_inline)) int32_t fx_div_norm_last(int32_t x, uint32_t r) {
    int32_t rd; GEOM_FX_CFU(0x53, rd, x, r); return rd;
}

/* Optional GeomSetupUnit ops, found present or not by geom_setup_probe() at
 * boot: the same geom firmware then runs on any build of the unit, falling
 * back to the C for whatever an area-constrained bitstream left out (an
 * absent op answers 0). BLEND/BLEND3/FXMUL/DIVN are always present. */
#ifdef GEOM_SU_PROBE
extern uint8_t g_su_recip, g_su_normf, g_su_conv;
void geom_setup_probe(void);
#else
/* geom.bin is baked into the bitstream it runs on, so the unit's op set is
 * known when it is built: GeomSetupUnit.v as the .qsf builds it, RECIPN and
 * NORMF in, the float<->fixed conversions gone since 2026-09-23. Constants
 * let the dead fallbacks and the probe drop out of the 16 KB ROM. Build with
 * CFLAGS_EXTRA=-DGEOM_SU_PROBE for a bitstream with SU_NO_RECIP/SU_NO_NORMF. */
#define g_su_recip 1
#define g_su_normf 1
#define g_su_conv  0
#define geom_setup_probe() ((void)0)
#endif
/* Saturating add/sub inline: exactly fx_clamp((int64_t)a +- b) (an int32
 * sum can only overflow in the direction of b's sign), without the two
 * calls. */
static inline __attribute__((always_inline)) int32_t fx_add_i(int32_t a, int32_t b) {
    int32_t r;
    if (__builtin_add_overflow(a, b, &r)) r = (b < 0) ? FX_MIN_RAW : FX_MAX_RAW;
    return r;
}
static inline __attribute__((always_inline)) int32_t fx_sub_i(int32_t a, int32_t b) {
    int32_t r;
    if (__builtin_sub_overflow(a, b, &r)) r = (b < 0) ? FX_MAX_RAW : FX_MIN_RAW;
    return r;
}
/* float bits <-> fixed: GeomSetupUnit 0x56 (rs2 0: S15.16, 1: S17.10) and
 * 0x55 (S15.16 -> float), exactly fx_from_bits_n()/fx_to_bits_n() in 5
 * cycles instead of ~50 / ~24 -- the matrix path decodes and re-encodes
 * every element (gm_mat4_mul, the light directions). */
static inline __attribute__((always_inline)) int32_t fx_from_bits_cfu(uint32_t bits, int mode) {
    int32_t rd; GEOM_FX_CFU(0x56, rd, bits, mode); return rd;
}
static inline __attribute__((always_inline)) uint32_t fx_to_bits_cfu(int32_t fx) {
    uint32_t rd; GEOM_FX_CFU(0x55, rd, fx, 0); return rd;
}
#define GEOM_FX_CONV_CFU 1
/* fx_from_bits()/fx_to_bits() themselves pick the CFU or the C (one copy of
 * the choice, in geom_fixed.c, instead of one per call site: the geom ROM
 * is 16 KB). */
#ifndef GEOM_FIXED_IMPL
#define fx_mul(a, b) fx_mul_cfu((a), (b))
#define fx_add(a, b) fx_add_i((a), (b))
#define fx_sub(a, b) fx_sub_i((a), (b))
#endif
#define GEOM_FX_DIVN_LAST 1
#endif

#endif /* GEOM_FIXED_H */
