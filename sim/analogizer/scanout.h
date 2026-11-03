// The scan-out's video timing (rtl/soc/n64_video.sv, vid: timing), one call a 25 MHz clock: a pixel each two clocks, lines of 790
// (NTSC) / 800 (PAL) pixel clocks, the last of the short lines 2 / 8 pixel clocks shorter, 264 / 313 lines a field; hs high from pixel
// clock 656 to 704, de for the pixel clocks 0..639 of the 240 picture lines, vs from pixel clock 650 of line 241 to pixel clock 650
// of line 249 (so it changes six pixel clocks before an hs pulse).
#pragma once
#include <cstdint>

struct Scanout {
    bool pal;
    int hc = 0, line = 0, ce = 0;                // pixel clock in the line, line in the field, clock within the pixel
    bool hs = false, vs = false, de = false;
    explicit Scanout(bool p) : pal(p) {}
    int hscan() const { return pal ? 800 : 790; }
    int nlines() const { return pal ? 313 : 264; }
    bool is_short() const { return line >= 253 && line < (pal ? 303 : 262); }
    int line_len() const { return hscan() - (is_short() ? (pal ? 8 : 2) : 0); }
    // the signals of the current clock (before step())
    void outputs() {
        hs = hc >= 656 && hc < 704;
        de = hc < 640 && line < 240;
    }
    // vs: set where the pixel clock 650 of line 241 is reached, cleared at 650 of line 249
    void step() {
        if (++ce < 2) return;
        ce = 0;
        hc++;
        if (hc == 650 && line == 241) vs = true;
        if (hc == 650 && line == 249) vs = false;
        if (hc >= line_len()) { hc = 0; if (++line >= nlines()) line = 0; }
    }
};
