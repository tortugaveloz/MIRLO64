// an_csync against the scan-out's timing: writes one byte a clock {csync_n, vs, hs} for the checker.
//   tb <ntsc|pal> <fields> <out file>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "Van_csync.h"
#include "../scanout.h"

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    bool pal = !strcmp(argv[1], "pal");
    int fields = atoi(argv[2]);
    Van_csync top;
    Scanout s(pal);
    std::vector<uint8_t> out;
    long n = (long)fields * s.nlines() * s.hscan() * 2;
    for (long i = 0; i < n; i++) {
        s.outputs();
        top.hs = s.hs; top.vs = s.vs;
        top.clk = 0; top.eval();
        out.push_back(top.csync_n << 2 | s.vs << 1 | s.hs);
        top.clk = 1; top.eval();
        s.step();
    }
    FILE *f = fopen(argv[3], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
    return 0;
}
