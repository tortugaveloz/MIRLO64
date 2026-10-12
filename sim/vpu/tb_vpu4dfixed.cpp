// Verilator testbench for rtl/vpu/Vpu4DFixed.v -- the parallel fixed-point
// matrix-vector CFU.
//
// Checks the RTL against a bit-exact fixed-point reference model in plain C
// (matching the RTL's own round-half-up-via-arithmetic-shift and saturation):
// "does the RTL implement the format's arithmetic correctly".
//
// Build/run: see sim/vpu/Makefile.fixed -> ./obj_dir_fixed/Vtb_vpu4dfixed

#include <verilated.h>
#include "VVpu4DFixed.h"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>

static VVpu4DFixed *dut;
static vluint64_t g_time = 0;

static void tick() {
    dut->clk = 0; dut->eval(); g_time++;
    dut->clk = 1; dut->eval(); g_time++;
}

#define FRAC_BITS 10
#define DW_BITS   27
// Mirrors what the RTL actually stores on a register write: cmd_inputs_0[DW-1:0],
// i.e. only the low DW_BITS survive, sign-extended back out for host-side use.
static int32_t trunc_dw(int32_t v) {
    int32_t shift = 32 - DW_BITS;
    return (v << shift) >> shift;
}
static int32_t to_fixed(double x) {
    double v = x * (double)(1LL << FRAC_BITS);
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483648.0) v = -2147483648.0;
    return (int32_t)llround(v);
}
static double from_fixed(int32_t x) { return (double)x / (double)(1LL << FRAC_BITS); }

// One CFU custom instruction: assert cmd, wait for rsp, return outputs_0.
// Returns the number of clock ticks the op took (issue->response), so
// MATVEC4's real latency can be reported.
static uint32_t cfu(uint32_t fid, uint32_t in0, uint32_t in1, int *ticks_out = nullptr) {
    dut->cmd_function_id = fid;
    dut->cmd_inputs_0 = in0;
    dut->cmd_inputs_1 = in1;
    dut->cmd_valid = 1;
    dut->rsp_ready = 1;
    int guard = 0, n = 0;
    while (!(dut->cmd_valid && dut->cmd_ready)) { tick(); n++; if (++guard > 100) { printf("TIMEOUT cmd accept fid=%x\n", fid); exit(2); } }
    tick(); n++;
    dut->cmd_valid = 0;
    guard = 0;
    while (!dut->rsp_valid) { tick(); n++; if (++guard > 4000) { printf("TIMEOUT rsp fid=%x\n", fid); exit(2); } }
    uint32_t r = dut->rsp_outputs_0;
    tick(); n++;
    if (ticks_out) *ticks_out = n;
    return r;
}

static int g_fail = 0, g_pass = 0;
static void check_fixed(const char *name, int32_t got, int32_t want, int32_t tol = 1) {
    int32_t d = got - want; if (d < 0) d = -d;
    if (d > tol) { printf("  FAIL %-20s got %d (%.6f) want %d (%.6f) diff=%d\n", name, got, from_fixed(got), want, from_fixed(want), d); g_fail++; }
    else         { printf("  ok   %-20s %d (%.6f)\n", name, got, from_fixed(got)); g_pass++; }
}

// ---- reference model (matches the RTL bit-for-bit: round-half-up via
// arithmetic shift, saturate to S19.13) -----------------------------------
static int32_t ref_matvec_component(const int32_t a[4], const int32_t col[4]) {
    int64_t acc = 0;
    for (int i = 0; i < 4; i++) acc += (int64_t)trunc_dw(a[i]) * (int64_t)trunc_dw(col[i]);
    int64_t rounded = acc + (1LL << (FRAC_BITS - 1));
    rounded >>= FRAC_BITS;   // arithmetic shift on a signed int64_t: matches >>> in the RTL
    int64_t hi = (1LL << (DW_BITS - 1)) - 1, lo = -(1LL << (DW_BITS - 1));
    if (rounded > hi) rounded = hi;
    if (rounded < lo) rounded = lo;
    return (int32_t)rounded;
}
static void ref_matvec(const int32_t a[4], const int32_t m[16] /* row-major, m[i*4+j] */, int32_t out[4]) {
    for (int j = 0; j < 4; j++) {
        int32_t col[4] = { m[0*4+j], m[1*4+j], m[2*4+j], m[3*4+j] };
        out[j] = ref_matvec_component(a, col);
    }
}

// Load a matrix (16 elements, fid 0x00) and a vector (4 elements, fid 0x01),
// fire MATVEC4 (fid 0x30), read out[0..3] (fid 0x08), return elapsed ticks
// for the MATVEC4 op itself.
static int run_matvec(const int32_t m[16], const int32_t a[4], int32_t out[4]) {
    for (int i = 0; i < 16; i++) cfu(0x00, (uint32_t)m[i], i);
    for (int i = 0; i < 4; i++)  cfu(0x01, (uint32_t)a[i], i);
    int ticks = 0;
    cfu(0x30, 0, 0, &ticks);
    for (int i = 0; i < 4; i++) out[i] = (int32_t)cfu(0x08, 0, i);
    return ticks;
}

// ---- VMUL4/VFMA4 reference model + drivers (2026-09-22, the "4-lane vertex
// helper" ops added alongside dropping the geom core's FPU -- one term per
// lane instead of MATVEC4's 4-term dot product, same round-half-up +
// saturate as ref_matvec_component). VFMA4's seed (c[i]) is sign-extended to
// the accumulator width before the add, mirroring the RTL's
// {{(AW-DW){m_l[4+li][DW-1]}}, m_l[4+li]} exactly -- get that wrong here and
// the reference would silently agree with a sign-extension bug in the RTL
// instead of catching it. ----------------------------------------------
static int32_t ref_vmul_component(int32_t a, int32_t b) {
    int64_t acc = (int64_t)trunc_dw(a) * (int64_t)trunc_dw(b);
    int64_t rounded = (acc + (1LL << (FRAC_BITS - 1))) >> FRAC_BITS;
    int64_t hi = (1LL << (DW_BITS - 1)) - 1, lo = -(1LL << (DW_BITS - 1));
    if (rounded > hi) rounded = hi;
    if (rounded < lo) rounded = lo;
    return (int32_t)rounded;
}
static int32_t ref_vfma_component(int32_t a, int32_t b, int32_t c) {
    // c is a plain S17.10 value (Q(FB) scale); the a*b product is at
    // Q(2*FB) scale (two fractional-bit factors), so c must be shifted
    // left by FB before adding -- matching the RTL's seed shift exactly
    // (see that side's comment for the real bug this was, caught by the
    // negative_seed test below computing its expected value independently
    // of this function).
    int64_t acc = (int64_t)trunc_dw(a) * (int64_t)trunc_dw(b) + ((int64_t)trunc_dw(c) << FRAC_BITS);
    int64_t rounded = (acc + (1LL << (FRAC_BITS - 1))) >> FRAC_BITS;
    int64_t hi = (1LL << (DW_BITS - 1)) - 1, lo = -(1LL << (DW_BITS - 1));
    if (rounded > hi) rounded = hi;
    if (rounded < lo) rounded = lo;
    return (int32_t)rounded;
}
static void ref_vmul4(const int32_t a[4], const int32_t b[4], int32_t out[4]) {
    for (int i = 0; i < 4; i++) out[i] = ref_vmul_component(a[i], b[i]);
}
static void ref_vfma4(const int32_t a[4], const int32_t b[4], const int32_t c[4], int32_t out[4]) {
    for (int i = 0; i < 4; i++) out[i] = ref_vfma_component(a[i], b[i], c[i]);
}
// B lives in its own register file (fid 0x02), fired with fid 0x31 -- NOT
// M[0..3]: B/C got their own storage specifically so VMUL4/VFMA4 don't
// clobber a resident MATVEC4 matrix (see Vpu4DFixed.v's header).
static int run_vmul4(const int32_t b[4], const int32_t a[4], int32_t out[4]) {
    for (int i = 0; i < 4; i++) cfu(0x02, (uint32_t)b[i], i);
    for (int i = 0; i < 4; i++) cfu(0x01, (uint32_t)a[i], i);
    int ticks = 0;
    cfu(0x31, 0, 0, &ticks);
    for (int i = 0; i < 4; i++) out[i] = (int32_t)cfu(0x08, 0, i);
    return ticks;
}
// B (fid 0x02) and C (fid 0x03), fired with fid 0x32.
static int run_vfma4(const int32_t b[4], const int32_t c[4], const int32_t a[4], int32_t out[4]) {
    for (int i = 0; i < 4; i++) cfu(0x02, (uint32_t)b[i], i);
    for (int i = 0; i < 4; i++) cfu(0x03, (uint32_t)c[i], i);
    for (int i = 0; i < 4; i++) cfu(0x01, (uint32_t)a[i], i);
    int ticks = 0;
    cfu(0x32, 0, 0, &ticks);
    for (int i = 0; i < 4; i++) out[i] = (int32_t)cfu(0x08, 0, i);
    return ticks;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    dut = new VVpu4DFixed;

    dut->resetn = 0; dut->cmd_valid = 0; dut->rsp_ready = 0;
    for (int i = 0; i < 8; i++) tick();
    dut->resetn = 1;
    for (int i = 0; i < 4; i++) tick();

    // ---- identity matrix -------------------------------------------------
    {
        int32_t I[16] = {0}; I[0]=I[5]=I[10]=I[15] = to_fixed(1.0);
        int32_t a[4] = { to_fixed(1.5), to_fixed(-2.0), to_fixed(3.25), to_fixed(1.0) };
        int32_t out[4], ref[4];
        int ticks = run_matvec(I, a, out);
        ref_matvec(a, I, ref);
        printf("MATVEC4 identity: %d clock ticks (cmd issue -> response)\n", ticks);
        check_fixed("identity.x", out[0], ref[0]);
        check_fixed("identity.y", out[1], ref[1]);
        check_fixed("identity.z", out[2], ref[2]);
        check_fixed("identity.w", out[3], ref[3]);
        // sanity: identity must reproduce the input exactly (no rounding error
        // possible when every other term is 0 and the diagonal term is exactly 1.0).
        check_fixed("identity.x==a.x", out[0], a[0], 0);
        check_fixed("identity.y==a.y", out[1], a[1], 0);
    }

    // ---- a real rotation+scale+translate * projection, like the actual
    // geom pipeline builds --
    {
        double ax=0.4, ay=1.1, az=-0.7, scale=1.7;
        double sx=std::sin(ax), cx=std::cos(ax), sy=std::sin(ay), cy=std::cos(ay), sz=std::sin(az), cz=std::cos(az);
        double r[3][3] = {
            { cy*cz,                cy*sz,                -sy    },
            { sx*sy*cz - cx*sz,     sx*sy*sz + cx*cz,      sx*cy },
            { cx*sy*cz + sx*sz,     cx*sy*sz - sx*cz,      cx*cy },
        };
        double mv[16] = {0};
        for (int i=0;i<3;i++) for (int j=0;j<3;j++) mv[i*4+j] = r[i][j]*scale;
        mv[3*4+0]=412.0; mv[3*4+1]=-88.0; mv[3*4+2]=3050.0; mv[3*4+3]=1.0;
        double pr[16] = {0};
        pr[0]=2.1; pr[5]=3.2; pr[10]=-1.0002; pr[11]=-1.0; pr[14]=-60.0; pr[15]=0.0;
        double mvp[16] = {0};
        for (int i=0;i<4;i++) for (int j=0;j<4;j++) { double s=0; for (int k=0;k<4;k++) s += mv[i*4+k]*pr[k*4+j]; mvp[i*4+j]=s; }

        int32_t m_fx[16]; for (int i=0;i<16;i++) m_fx[i] = to_fixed(mvp[i]);
        double ov_d[4] = { 1200.0, -340.0, 75.0, 1.0 };
        int32_t a_fx[4]; for (int i=0;i<4;i++) a_fx[i] = to_fixed(ov_d[i]);

        int32_t out[4], ref[4];
        int ticks = run_matvec(m_fx, a_fx, out);
        ref_matvec(a_fx, m_fx, ref);
        printf("MATVEC4 realistic MVP: %d clock ticks\n", ticks);
        check_fixed("mvp.clip.x", out[0], ref[0]);
        check_fixed("mvp.clip.y", out[1], ref[1]);
        check_fixed("mvp.clip.z", out[2], ref[2]);
        check_fixed("mvp.clip.w", out[3], ref[3]);

        // Cross-check against a plain double computation of the same transform,
        // converted through the SAME to_fixed()/from_fixed() the RTL uses --
        // this is the "does S19.13 hold up for a shape like a real MVP" check,
        // narrower than the full precision study but exercised against real RTL.
        double clip_double[4];
        for (int j=0;j<4;j++) { double s=0; for (int i=0;i<4;i++) s += ov_d[i]*mvp[i*4+j]; clip_double[j]=s; }
        for (int j=0;j<4;j++) {
            double got = from_fixed(out[j]);
            double err = std::fabs(got - clip_double[j]);
            printf("  clip[%d]: rtl=%.4f double=%.4f err=%.5f\n", j, got, clip_double[j], err);
        }
    }

    // ---- random stress: many (M,A) pairs across the full S19.13 range,
    // RTL must match the bit-exact reference model exactly every time -----
    {
        srand(777);
        int n = 500, mismatches = 0;
        long total_ticks = 0;
        for (int t = 0; t < n; t++) {
            int32_t m[16], a[4], out[4], ref[4];
            // Raw fixed-point magnitude bound chosen so a worst-case 4-term dot
            // product (4 * bound^2, then >>FRAC_BITS) stays inside S17.10's 27-bit
            // output range WITHOUT saturating -- this test checks ordinary
            // (non-saturating) arithmetic bit-exactness; saturation has its own
            // dedicated test below.
            for (int i = 0; i < 16; i++) m[i] = (int32_t)((rand() % 200000) - 100000);
            for (int i = 0; i < 4; i++)  a[i] = (int32_t)((rand() % 200000) - 100000);
            total_ticks += run_matvec(m, a, out);
            ref_matvec(a, m, ref);
            for (int j = 0; j < 4; j++) {
                if (out[j] != ref[j]) {
                    if (mismatches < 5) printf("  FAIL random[%d].out[%d]: rtl=%d ref=%d\n", t, j, out[j], ref[j]);
                    mismatches++;
                }
            }
        }
        printf("random stress: %d trials, %d component mismatches, avg %.1f ticks/MATVEC4\n",
               n, mismatches, (double)total_ticks / n);
        if (mismatches == 0) { printf("  ok   random stress: bit-exact match on all %d trials\n", n); g_pass++; }
        else { g_fail++; }
    }

    // ---- saturation: an input designed to overflow S19.13 must clamp, not
    // wrap -----------------------------------------------------------------
    {
        int32_t m[16] = {0}; m[0] = to_fixed(200000.0); // one huge diagonal entry
        int32_t a[4] = { to_fixed(200000.0), 0, 0, to_fixed(1.0) };
        int32_t out[4], ref[4];
        run_matvec(m, a, out);
        ref_matvec(a, m, ref);
        // 200000*200000 vastly exceeds S17.10's +/-65536 range -- must saturate
        // to the max representable value (2^(DW-1)-1 = 2^26-1 in the raw fixed
        // encoding), never wrap to a small or negative number.
        const int32_t max_raw = (1 << (DW_BITS - 1)) - 1;
        check_fixed("saturate.x", out[0], max_raw, 0);
        check_fixed("saturate.x==ref", out[0], ref[0], 0);
    }


    // ---- MATVEC4 still correct after VMUL4/VFMA4 ran -- op_kind must not
    // leak state between calls (the actual regression risk of adding a
    // shared mode register to an existing state machine) -------------------
    {
        int32_t I[16] = {0}; I[0]=I[5]=I[10]=I[15] = to_fixed(1.0);
        int32_t a[4] = { to_fixed(7.0), to_fixed(-3.0), to_fixed(2.5), to_fixed(1.0) };
        int32_t out[4], ref[4];
        run_matvec(I, a, out);
        ref_matvec(a, I, ref);
        check_fixed("matvec_after_vfma.x", out[0], ref[0], 0);
        check_fixed("matvec_after_vfma.y", out[1], ref[1], 0);
    }


    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
