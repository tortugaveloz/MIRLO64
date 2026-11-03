// tb_origin: rcp_vi's scan-out base (`base`) for a serrated picture's origins, field by field.
//
// A serrated picture of the pitches that are sent woven (WIDTH 640, 576, 1280: n64_video's wv_rq) has the base of its
// first field whatever the game writes for the second (one row on): the woven frame is the rows of both. A serrated picture
// of any other pitch (a 512-pixel picture a row of 512 on a pitch of 1024: the game renders the rows of the field not being
// shown while the other is) is sent a field at a time: the base is the origin the game wrote for each field, so each field
// shows its own rows.
//   regress.sh builds it (obj_origin/tb_origin)
#include "Vrcp_vi.h"
#include "verilated.h"
#include <cstdio>

static Vrcp_vi *t;
static int fails;
static void tick() { t->clk = 0; t->eval(); t->clk = 1; t->eval(); }
static void wr(int a, uint32_t v) { t->a = a; t->wd = v; t->wr_s = 1; tick(); t->wr_s = 0; tick(); tick(); }
static void field(int f) { wr(14, (uint32_t)f << 31 | (t->base & 0x7FFFFF)); }     // (a save state's field and base, put back)
static void expect(const char *what, uint32_t want)
{
    const uint32_t got = t->base & 0x7FFFFF;
    if (got != want) { printf("FAIL %s: base %06x, want %06x\n", what, got, want); fails++; }
}
// a game's retrace handler for two fields: each field's origin; the base after each
static void fields(const char *name, uint32_t width, uint32_t o0, uint32_t o1, uint32_t want0, uint32_t want1)
{
    wr(0, 0x3246);                 // 16 bits, SERRATE
    wr(2, width);
    char s[96];
    for (int k = 0; k < 3; k++) {
        field(0); wr(1, o0); snprintf(s, sizeof s, "%s, field 0 (%d)", name, k); expect(s, want0);
        field(1); wr(1, o1); snprintf(s, sizeof s, "%s, field 1 (%d)", name, k); expect(s, want1);
    }
}
int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    t = new Vrcp_vi;
    t->rst = 1; for (int i = 0; i < 4; i++) tick(); t->rst = 0; tick();
    t->thr = 0x1FFFF; t->tv_def = 0; t->tv_pal = 0; t->sc_line = 0; t->sc_latch = 0; t->wr_s = 0;
    // WIDTH 640, libultra's interlaced hi-res modes: the second field's origin a row (1280 bytes) on: one frame from the first's
    fields("WIDTH 640", 640, 0x100500, 0x100A00, 0x100500, 0x100500);
    // WIDTH 576 (a 576-pixel picture, Y_SCALE 2.0: libultra's interlaced mode of that size): the second field's a row (1152 bytes) on
    fields("WIDTH 576", 576, 0x100500, 0x100980, 0x100500, 0x100500);
    // WIDTH 1280 (a pitch of two 640-pixel rows): the second field's a row (1280 bytes) on
    fields("WIDTH 1280", 1280, 0x200000, 0x200500, 0x200000, 0x200000);
    // the buffer swapped each field (a flight game): each field's origin in a buffer of its own -- the base is the first row of each
    fields("WIDTH 640, swapped", 640, 0x300500, 0x400A00, 0x300500, 0x400500);
    // WIDTH 1024 (512 pixels a row, 1024 a pitch: the fields' rows are every other row): each field its own origin
    fields("WIDTH 1024", 1024, 0x790400, 0x790800, 0x790400, 0x790800);
    // WIDTH 704, 800: any other serrated pitch is a field at a time, too
    fields("WIDTH 704", 704, 0x500000, 0x500580, 0x500000, 0x500580);
    printf("tb_origin: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
