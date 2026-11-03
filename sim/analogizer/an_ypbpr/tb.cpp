// an_ypbpr over all 2^24 colours, de low for some of them; writes pr, y, pb, a byte each, in the order of the inputs
// (the input of clock k is rgb = k, de = (k * 2654435761 >> 16) & 7 != 0, the output of clock k + LATENCY is its result).
//   tb <out file>
#include <cstdio>
#include <vector>
#include <cstdint>
#include "Van_ypbpr.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const long N = 1L << 24;
    const int LAT = 2;
    Van_ypbpr top;
    std::vector<uint8_t> out(N * 3);
    for (long k = 0; k < N + LAT; k++) {
        long kk = k < N ? k : 0;
        top.rgb = (uint32_t)kk;
        top.de = (((uint32_t)kk * 2654435761u) >> 16 & 7) != 0;
        top.clk = 0; top.eval();
        if (k >= LAT) {
            out[(k - LAT) * 3 + 0] = top.pr; out[(k - LAT) * 3 + 1] = top.y; out[(k - LAT) * 3 + 2] = top.pb;
        }
        top.clk = 1; top.eval();
    }
    FILE *f = fopen(argv[1], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
    return 0;
}
