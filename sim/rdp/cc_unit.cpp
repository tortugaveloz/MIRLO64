// the combiner's arithmetic, RTL against the model (cc_unit.sh)
#include "Vcc_unit.h"
#include "verilated.h"
#include <cstdio>
#include <cstdint>
#include "rdp.c"      // (built as C++, as tb_rdp does)
int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    Vcc_unit *t = new Vcc_unit;
    uint64_t n = 0, bad = 0;
    // k: the operands are COMBINED's (ka kb kc: A, B, C as such), else plain positive numbers (the shade, a texel, ...)
    auto check = [&](int a, int b, int c, int d, int k = 0) {
        t->a = a; t->b = b; t->c = c; t->d = d; t->ka = k & 1; t->kb = (k >> 1) & 1; t->kc = (k >> 2) & 1; t->eval();
        int m = cc_eval((k & 1) ? cc_ext9(a) : a, (k & 2) ? cc_ext9(b) : b, (k & 4) ? cc_sgn9(c) : c, d);
        n++;
        if (t->e1 != m || t->cl != cc_color_res(m) || (cc_alpha_res(m) == 256) != (bool)t->f256) {
            if (bad++ < 10) printf("MISMATCH a %d b %d c %d d %d: rtl %d model %d\n", a, b, c, d, t->e1, m);
        }
    };
    // cycle 1: the whole domain of a result's operands on a stride, every C, with each of the roles COMBINED can take
    for (int k = 0; k < 8; k++)
        for (int a = 0; a < 512; a += 7) for (int b = 0; b < 512; b += 5) for (int d = 0; d < 512; d += 11) for (int c = 0; c < 512; c++) check(a, b, c, d, k);
    // cycle 0: plain operands of 0..511 (the shade reaches 511)
    for (int a = 0; a < 512; a += 3) for (int b = 0; b < 512; b += 3) for (int d = 0; d < 512; d += 17) for (int c = 0; c < 512; c++) check(a, b, c, d);
    // the corners
    static const int ed[] = { 0, 1, 127, 128, 255, 256, 257, 383, 384, 385, 447, 448, 510, 511 };
    for (int k = 0; k < 8; k++) for (int a : ed) for (int b : ed) for (int d : ed) for (int c : ed) check(a, b, c, d, k);
    uint32_t r = 12345;
    auto rnd = [&]() { r = r * 1664525u + 1013904223u; return r >> 7; };
    for (int i = 0; i < 100000000; i++) { int a = rnd() & 511, b = rnd() & 511, c = rnd() & 511, d = rnd() & 511; check(a, b, c, d, (int)(rnd() & 7)); }
    for (int i = 0; i < 100000000; i++) { int a = rnd() & 511, b = rnd() & 511, c = rnd() & 511, d = rnd() & 511; check(a, b, c, d); }
    printf("cc_unit: %llu operand sets, %llu mismatches\n", (unsigned long long)n, (unsigned long long)bad);
    return bad != 0;
}
