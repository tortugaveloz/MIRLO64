// an_yc on colour bars in the scan-out's timing; writes 6 bytes a clock for check.py:
//   flags {csync_n << 3 | de << 2 | vs << 1 | hs}, y, c, R, G, B  (the inputs of that clock and the outputs seen in it)
// The picture: eight bars of 80 pixels (white, yellow, cyan, green, magenta, red, blue, black), the lines 0..119 at 100 %,
// the lines 120..239 at 75 %.
//   tb <ntsc|pal> <fields> <out file>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "Van_yc.h"
#include "../scanout.h"

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    bool pal = !strcmp(argv[1], "pal");
    int fields = atoi(argv[2]);
    Van_yc top;
    Scanout s(pal);
    std::vector<uint8_t> out;
    static const int bars[8][3] = {{1,1,1},{1,1,0},{0,1,1},{0,1,0},{1,0,1},{1,0,0},{0,0,1},{0,0,0}};
    long n = (long)fields * s.nlines() * s.hscan() * 2;
    top.pal = pal; top.rst = 1;
    for (long i = 0; i < n; i++) {
        s.outputs();
        int lvl = s.line < 120 ? 255 : 191;
        int b = s.hc < 640 ? s.hc / 80 : 7;
        int r = bars[b][0] * lvl, g = bars[b][1] * lvl, bl = bars[b][2] * lvl;
        if (!s.de) { r = g = bl = (int)((i * 2654435761u) >> 24) & 0xff; }    // (the colour in the blanking must not matter)
        top.hs = s.hs; top.vs = s.vs; top.de = s.de;
        top.rgb = (r << 16) | (g << 8) | bl;
        if (i == 4) top.rst = 0;
        top.clk = 0; top.eval();
        out.push_back(top.csync_n << 3 | s.de << 2 | s.vs << 1 | s.hs);
        out.push_back(top.y); out.push_back(top.c);
        out.push_back(r); out.push_back(g); out.push_back(bl);
        top.clk = 1; top.eval();
        s.step();
    }
    FILE *f = fopen(argv[3], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
    return 0;
}
