// fpu_d64 against its model (sim/n64/fpd64.h) and the model against the host's IEEE doubles.
//   obj_dir/Vfpu_d64 [millions of random operands per operation, default 1] [seed]
// The host's arithmetic is the reference wherever a double has no flush to apply: inputs below the normals are zeros of
// their sign first (the FPU's FS rule); a result below the normals is a zero of its sign (a mul whose host result is the
// smallest normal is rounded again with an unbounded exponent, by an fma on the scaled operands); a NaN is the default one.
#include "Vfpu_d64.h"
#include "verilated.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include "../n64/fpd64.h"

static double D(uint64_t x) { double d; memcpy(&d, &x, 8); return d; }
static uint64_t U(double d) { uint64_t x; memcpy(&x, &d, 8); return x; }

static uint64_t host(uint64_t x, uint64_t y, int op)   // 0 add 1 sub 2 mul 3 abs 4 neg
{
    x = fpd_ftz(x); y = fpd_ftz(y);
    double a = D(x), b = D(y), r;
    switch (op) {
    case 0: r = a + b; break;
    case 1: r = a - b; break;
    case 2: r = a * b; break;
    case 3: return fpd_isnan(x) ? FPD_QNAN : x & 0x7FFFFFFFFFFFFFFFull;
    default: return fpd_isnan(x) ? FPD_QNAN : x ^ 0x8000000000000000ull;
    }
    uint64_t u = U(r);
    if (r != r) return FPD_QNAN;
    if (op == 2 && (u & 0x7FFFFFFFFFFFFFFFull) == 0x0010000000000000ull) {          // the smallest normal: exact below it?
        double a2 = fabs(a) * 0x1p600, bb = fabs(b), p2 = a2 * bb, r2 = fma(a2, bb, -p2), t2 = 0x1p-422;   // (the threshold 2^-422 - 2^-476 is 54 bits: its distance from t2 is added as a term)
        if (!(((p2 - t2) + 0x1p-476) + r2 >= 0.0)) return u & 0x8000000000000000ull;
    }
    if ((u >> 52 & 0x7FF) == 0) return u & 0x8000000000000000ull;
    return u;
}
static int cmp_host(uint64_t x, uint64_t y, int *un, int *eq, int *lt)
{
    double a = D(fpd_ftz(x)), b = D(fpd_ftz(y));
    *un = a != a || b != b; *eq = !*un && a == b; *lt = !*un && a < b;
    return 0;
}

static const uint64_t SP[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FF0000000000000ull,
    0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull, 0xFFF7FFFFFFFFFFFFull, 0x0010000000000000ull,
    0x000FFFFFFFFFFFFFull, 0x0000000000000001ull, 0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull, 0x4000000000000000ull,
    0x3FE0000000000000ull, 0x3FF0000000000001ull, 0x3FEFFFFFFFFFFFFFull, 0x4330000000000000ull, 0x433FFFFFFFFFFFFFull,
    0x3CA0000000000000ull, 0x3CB0000000000000ull, 0x3C90000000000000ull, 0x7FE0000000000000ull, 0x7FDFFFFFFFFFFFFFull,
    0x2000000000000000ull, 0x5FE0000000000000ull, 0x1FF0000000000000ull, 0x4008000000000000ull, 0x3FF8000000000000ull,
    0x3FF0000000000002ull, 0x400921FB54442D18ull, 0x7FE8000000000000ull, 0x0018000000000000ull, 0x001FFFFFFFFFFFFFull,
    0x3FFFFFFFFFFFFFFFull, 0x1E60000000000000ull, 0x2010000000000000ull, 0x5FF0000000000000ull };
static const int NSP = sizeof(SP) / sizeof(SP[0]);

static std::mt19937_64 rng;
static uint64_t rnd_val()
{
    uint64_t k = rng() % 16, v = rng();
    switch (k) {
    case 0: case 1: return SP[rng() % NSP] ^ (rng() % 4 == 0 ? 0x8000000000000000ull : 0);
    case 2: return SP[rng() % NSP] + (rng() % 5) - 2;                                    // next to a special
    case 3: return (v & 0x800FFFFFFFFFFFFFull) | (uint64_t)(1023 + (int)(rng() % 120) - 60) << 52;            // middling
    case 4: return (v & 0x800FFFFFFFFFFFFFull) | (uint64_t)(1 + rng() % 40) << 52;                           // near underflow
    case 5: return (v & 0x800FFFFFFFFFFFFFull) | (uint64_t)(2046 - rng() % 40) << 52;                        // near overflow
    case 6: return (v & 0x8000000000000000ull) | (uint64_t)(1023 + (int)(rng() % 8) - 4) << 52 | (v & ((1ull << (rng() % 52 + 1)) - 1));     // few bits
    case 7: return (v & 0x8000000000000000ull) | (uint64_t)(1023 + (int)(rng() % 8) - 4) << 52 | ~((1ull << (rng() % 52 + 1)) - 1) & 0xFFFFFFFFFFFFFull;   // many ones
    case 8: return v & 0x800FFFFFFFFFFFFFull;                                        // denormals
    default: return v;
    }
}
// a second operand made to cancel, tie or reach the first's exponent
static uint64_t rnd_pair(uint64_t x)
{
    switch (rng() % 8) {
    case 0: return x ^ 0x8000000000000000ull;
    case 1: return x + (rng() % 5) - 2;
    case 2: return (x & 0x8000000000000000ull) | ((x & 0x7FF0000000000000ull) + ((uint64_t)((int)(rng() % 120) - 60) << 52)) | (rng() & 0xFFFFFFFFFFFFFull);
    case 3: return (x ^ (rng() & 0x8000000000000000ull)) + ((rng() % 3) - 1) * (1ull << (rng() % 52));
    default: return rnd_val();
    }
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    double mill = argc > 1 ? atof(argv[1]) : 1.0;
    rng.seed(argc > 2 ? strtoull(argv[2], 0, 0) : 1);
    Vfpu_d64 *m = new Vfpu_d64;
    uint64_t nhost = 0, nrtl = 0, bad_host = 0, bad_rtl = 0, cyc[5] = {0}, cmax[5] = {0}, cnt[5] = {0};
    auto tick = [&] { m->clk = 0; m->eval(); m->clk = 1; m->eval(); };
    m->rst = 1; m->go = 0; m->live = 1; m->take = 0; tick(); tick(); m->rst = 0;
    auto run = [&](uint64_t x, uint64_t y, int fn, int cls) -> uint64_t {
        m->fn = fn; m->a = x; m->b = y; m->go = 1;
        int n = 0;
        do { tick(); n++; } while (m->busy && n < 400);
        if (n >= 400) { fprintf(stderr, "hang fn %d %016llx %016llx\n", fn, (unsigned long long)x, (unsigned long long)y); exit(2); }
        uint64_t r = m->res;
        m->take = 1; tick(); m->take = 0; m->go = 0; tick();
        if (cls >= 0) { cyc[cls] += n; cnt[cls]++; if ((uint64_t)n > cmax[cls]) cmax[cls] = n; }
        return r;
    };
    const char *nm[] = {"add", "sub", "mul", "abs", "neg"};
    const int fns[] = {0, 1, 2, 5, 7};
    auto one = [&](uint64_t x, uint64_t y, int op) {
        uint64_t m_ = op == 0 ? fpd_add(x, y, 0) : op == 1 ? fpd_add(x, y, 1) : op == 2 ? fpd_mul(x, y) : op == 3 ? fpd_abs(x) : fpd_neg(x);
        uint64_t h_ = host(x, y, op), r_ = run(x, y, fns[op], op);
        nhost++; nrtl++;
        if (m_ != h_) { if (bad_host++ < 10) fprintf(stderr, "MODEL %s %016llx %016llx: model %016llx host %016llx\n", nm[op], (unsigned long long)x, (unsigned long long)y, (unsigned long long)m_, (unsigned long long)h_); }
        if (r_ != m_) { if (bad_rtl++ < 10) fprintf(stderr, "RTL %s %016llx %016llx: rtl %016llx model %016llx\n", nm[op], (unsigned long long)x, (unsigned long long)y, (unsigned long long)r_, (unsigned long long)m_); }
    };
    // grid of specials, every pair, both signs
    for (int i = 0; i < NSP; i++) for (int j = 0; j < NSP; j++) for (int s = 0; s < 4; s++)
        for (int op = 0; op < 5; op++) one(SP[i] ^ ((uint64_t)(s & 1) << 63), SP[j] ^ ((uint64_t)(s >> 1) << 63), op);
    uint64_t grid = nhost;
    // compares
    uint64_t ncmp = 0, bad_cmp = 0;
    auto cmpone = [&](uint64_t x, uint64_t y) {
        int un, eq, lt; cmp_host(x, y, &un, &eq, &lt);
        run(x, y, 0x3C, -1);
        int gun = m->c_un, geq = m->c_eq, glt = m->c_lt;
        ncmp++;
        if (gun != un || geq != eq || glt != lt) { if (bad_cmp++ < 10) fprintf(stderr, "CMP %016llx %016llx: rtl un %d eq %d lt %d host %d %d %d\n", (unsigned long long)x, (unsigned long long)y, gun, geq, glt, un, eq, lt); }
    };
    for (int i = 0; i < NSP; i++) for (int j = 0; j < NSP; j++) for (int s = 0; s < 4; s++)
        cmpone(SP[i] ^ ((uint64_t)(s & 1) << 63), SP[j] ^ ((uint64_t)(s >> 1) << 63));
    uint64_t n = (uint64_t)(mill * 1e6);
    for (uint64_t k = 0; k < n; k++) {
        uint64_t x = rnd_val(), y = rng() % 2 ? rnd_pair(x) : rnd_val();
        one(x, y, k % 3);
        if (k % 16 == 0) one(x, y, 3 + (k / 16) % 2);
        if (k % 4 == 0) cmpone(x, rng() % 2 ? rnd_pair(x) : y);
    }
    printf("grid %llu operations; %llu random; compares %llu\n", (unsigned long long)grid, (unsigned long long)(nhost - grid), (unsigned long long)ncmp);
    for (int i = 0; i < 5; i++) printf("  %s: %llu ops, %.1f cycles on average, %llu at most (with the one that takes the result)\n", nm[i], (unsigned long long)cnt[i], cnt[i] ? (double)cyc[i] / cnt[i] : 0.0, (unsigned long long)cmax[i]);
    printf("model vs host: %llu mismatches / %llu\nrtl vs model: %llu mismatches / %llu\ncompare rtl vs host: %llu mismatches / %llu\n",
           (unsigned long long)bad_host, (unsigned long long)nhost, (unsigned long long)bad_rtl, (unsigned long long)nrtl, (unsigned long long)bad_cmp, (unsigned long long)ncmp);
    printf(bad_host || bad_rtl || bad_cmp ? "FAIL\n" : "PASS\n");
    return bad_host || bad_rtl || bad_cmp;
}
