/* The N64 controller from the Pocket's (or the dock's) keys and the core's settings: n64rt_pad_map(), a pure function (no
 * hardware), so that lang/n64rt/test/pad_test.py can run the combinations of settings against the spec on the host.
 *
 * The keys are APF_INPUT's (docs/control.md: Controller/Joystick Bitmap; the Pocket's own Controls menu remaps what the
 * buttons give, the runtime only sees these bits), the stick bytes are its four joystick bytes (left X, left Y, right X,
 * right Y, from the low byte up) and the settings are the two words of APF_INTERACT, as interact.json
 * (pkg/pocket/Cores/tortuga.Mirlo64/interact.json) writes them -- a field at a time, at the addresses 0x1000_0100 + 4 x n --
 * and the remap table (below), five words the runtime keeps in RAM (the Pocket's menu has 16 entries at most: it edits the table one field at
 * a time, n64rt_ctrl.h; which layout the settings and the table are -- the live one, a game's own or a family's -- is picked there):
 *
 *   word 0 [4:0]    n=2  "Controller 1 pak": n64rt_pak.c
 *          [9:5]    n=3  "Crop Vertical" (the video: rtl/soc/mirlo_regs.sv)
 *          [14:10]  n=4  "Modifier": which Pocket key (a button code, below; 0 none) selects the Held layer of the remap table while
 *                        it is held; that key never reaches the game (so not as any mapped button either)
 *          [19:15]  n=5  "Crop Right" (the video; a list: 0 none, 1 pixel)
 *          [24:20]  n=6  "Stick dead zone" (n64rt_pad_deadzone)
 *          [29:25]  n=7  "Stick range" (n64rt_pad_range)
 *          [14:10]  n=10 "C buttons": 0 none, 1 also the right stick (as C buttons)
 *          [19:15]  n=11 "Stick": 0 the D-pad (full deflection), 1 the left analog stick, 2 the right analog stick; any other
 *                        value is the D-pad
 *          [23:22]  n=13 "D-pad": 0 none, 1 the D-pad, 2 the left analog stick, 3 the right analog stick
 *          [25]     n=15 free (was "Start = Select+Start")
 *          the rest free (n=8, 9, 12, 14 were "EEPROM size", "Z button", "Start" and "C-button layer")
 *
 * A button code (the Modifier) is 0 none, 1 the Pocket's L, 2 R, 3 ZL (L2), 4 ZR (R2), 6 Select, anything else none.
 *
 * THE REMAP TABLE: 30 five-bit fields, six to a word (field f is bits [5(f%6)+4:5(f%6)] of
 * word f/6, map[0..4] here). Two layers: Base (the Modifier key not held, or no Modifier)
 * and Held (held):
 *   fields 0-11   Base layer, one per key:  A, B, X, Y, L, R, ZL, ZR, Select, Start, L3, R3 (the Pocket's keys; L is KEY_L1,
 *                 ZL KEY_L2; L3, R3 only on a dock pad)
 *   fields 12-14  Base layer, a group each: the four D-pad keys (12), the left analog stick (13), the right analog stick (14)
 *   fields 15-26  Held layer, the same twelve keys
 *   fields 27-29  Held layer, the same three groups
 * A key field's code (an N64 output; each key gives one): 0 the entry's own default (the reset value, 0, is the default layout),
 *   1 A  2 B  3 Z  4 Start  5 L  6 R  7 C-up  8 C-down  9 C-left  10 C-right  11 D-up  12 D-down  13 D-left  14 D-right
 *   15 "same as Base" (the Held layer: what the key does in the Base layer; in the Base layer, none)   anything else: none
 * The defaults: Base: A A, B B, X C-right, Y C-left, L Z, R R, ZL none, ZR none, Select C-down, Start Start, L3 and R3 none (the
 * N64's L is on nothing: set ZL or ZR to it); Held: A C-right, B C-down, X C-up, Y C-left, all others same as Base.
 * A group field's code: 0 the default -- the D-pad keys: the "Stick" and "D-pad" settings below (Held: with their Modifier effects: half a
 * stick, or the N64's D-pad swapped for the stick); the left stick of a dock pad: the N64's stick (code 1); the right stick: the C buttons
 * (code 4) --, 1 the stick (the D-pad keys: full; an analog stick: as it is), 2 half a stick, 3 the N64's D-pad (an analog stick: its four
 * directions, past +-64 of 127), 4 the C buttons (the same), 5 none; 6 same as Base (the Base layer's, as if unheld; in the Base layer,
 * none); 7 the stick (full) and the N64's D-pad together (the D-pad keys; an analog stick: none); anything else none. An analog stick is a group's (it is centred for the "Stick", "D-pad" and "C buttons" settings, which the
 * menu no longer has: they are 0, and what they did the groups do).
 * Nothing couples the fields: two keys may give the same output (an N64 button is down while any of its keys is). The stick the
 * groups give adds to the one the settings give; the sum is clamped to +-80.
 *
 * "C buttons" = 1 adds the right analog stick as another source of the C buttons (ORed): past +-64 of 127 it is C-left / C-right /
 * C-up / C-down.
 *
 * Analog sticks. The dock says a pad has them with KEY_TYPE 3; the Pocket's own controls (type 1) have none. The pad of a type
 * the docs call "without analog" (2) is believed, for its stick bytes, when they are not 0x00000000 or 0xFFFFFFFF (a pad with
 * nothing to send), in case the dock reports a pad with a stick so (lang/mips/test/padtest.c shows what the dock sends). The
 * "Stick" setting:
 *   D-pad    the D-pad keys, full deflection (half with the Modifier held); the stick bytes are not looked at.
 *            With "D-pad" = D-pad too, the Modifier instead selects digital D-pad only (no stick); unheld is stick only
 *   L stick  the left stick (bytes 0, 1)    R stick  the right stick (bytes 2, 3)
 * With L or R stick, a type 3 pad gives the stick (centred: 0, 0). A type 2 pad gives the stick when it is deflected and, centred,
 * the D-pad keys (so a pad with no stick, or one the dock calls type 2, still moves); any other pad (the Pocket's own controls)
 * gives the D-pad keys. The "D-pad" setting (the N64's D-pad) takes a stick as the four directions too, past +-64 of 127, from
 * the same pads that have stick bytes to believe. */
#ifndef N64RT_PAD_H
#define N64RT_PAD_H
#include <stdint.h>

#define CONT_A 0x8000u
#define CONT_B 0x4000u
#define CONT_G 0x2000u          /* Z */
#define CONT_START 0x1000u
#define CONT_UP 0x0800u
#define CONT_DOWN 0x0400u
#define CONT_LEFT 0x0200u
#define CONT_RIGHT 0x0100u
#define CONT_L 0x0020u
#define CONT_R 0x0010u
#define CONT_E 0x0008u          /* C-up */
#define CONT_D 0x0004u          /* C-down */
#define CONT_C 0x0002u          /* C-left */
#define CONT_F 0x0001u          /* C-right */
/* APF_INPUT's keys */
#define KEY_DPAD_UP    (1u << 0)
#define KEY_DPAD_DOWN  (1u << 1)
#define KEY_DPAD_LEFT  (1u << 2)
#define KEY_DPAD_RIGHT (1u << 3)
#define KEY_A      (1u << 4)
#define KEY_B      (1u << 5)
#define KEY_X      (1u << 6)
#define KEY_Y      (1u << 7)
#define KEY_L1     (1u << 8)
#define KEY_R1     (1u << 9)
#define KEY_L2     (1u << 10)
#define KEY_R2     (1u << 11)
#define KEY_L3     (1u << 12)
#define KEY_R3     (1u << 13)
#define KEY_SELECT (1u << 14)
#define KEY_START  (1u << 15)
#define KEY_TYPE(k) ((k) >> 28)          /* 1 Pocket, 2 dock pad without analog, 3 dock pad with analog */

/* the sticks' dead zone and range: "Stick dead zone" (interact0 [24:20]: 0 12, 1 none, 2 6, 3 20, 4 30; any other 12) and
 * "Stick range" (interact0 [29:25]: 0 80, 1 85, 2 75, 3 70, 4 60; any other 80) */
static inline int n64rt_pad_deadzone(uint32_t set0)
{
    static const uint8_t dz[5] = { 12, 0, 6, 20, 30 };
    const uint32_t c = (set0 >> 20) & 31u;
    return c < 5u ? dz[c] : 12;
}
static inline int n64rt_pad_range(uint32_t set0)
{
    static const uint8_t r[5] = { 80, 85, 75, 70, 60 };
    const uint32_t c = (set0 >> 25) & 31u;
    return c < 5u ? r[c] : 80;
}
static inline int8_t n64rt_pad_axis(uint32_t v, int flip, int dz, int rng)
{
    int a = (int)(v & 0xFFu) - 128;
    if (flip) a = -a;
    if (a > -dz && a < dz) return 0;
    a = a * rng / 127;
    return (int8_t)(a > rng ? rng : a < -rng ? -rng : a);
}
/* the Pocket key a button setting names (0: no key) */
static inline uint32_t n64rt_pad_key(uint32_t code, uint32_t dflt)
{
    switch (code) {
    case 0:  return dflt;
    case 1:  return KEY_L1;
    case 2:  return KEY_R1;
    case 3:  return KEY_L2;
    case 4:  return KEY_R2;
    case 6:  return KEY_SELECT;
    default: return 0;
    }
}
/* the remap table */
#define PAD_KEYS 12
#define PAD_SAME 15u
#define PAD_HELD 15u                                   /* the Held layer's first field */
static const uint32_t n64rt_pad_keys[PAD_KEYS] = { KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L1, KEY_R1, KEY_L2, KEY_R2, KEY_SELECT, KEY_START, KEY_L3, KEY_R3 };
static const uint8_t n64rt_pad_base_dflt[PAD_KEYS] = { 1, 2, 10, 9, 3, 6, 31, 31, 8, 4, 31, 31 };
static const uint8_t n64rt_pad_held_dflt[PAD_KEYS] = { 10, 8, 7, 9, 15, 15, 15, 15, 15, 15, 15, 15 };
static inline uint32_t n64rt_pad_field(const uint32_t *map, unsigned f)
{
    return (map[f / 6u] >> (5u * (f % 6u))) & 31u;
}
/* the output (a code of 1-14, or 0: none) key i gives in a layer */
static inline uint32_t n64rt_pad_target(const uint32_t *map, unsigned i, int held)
{
    uint32_t c = n64rt_pad_field(map, held ? PAD_HELD + i : i);
    if (c == 0u) c = held ? n64rt_pad_held_dflt[i] : n64rt_pad_base_dflt[i];
    if (c == PAD_SAME) {
        c = held ? n64rt_pad_field(map, i) : 0u;
        if (held && c == 0u) c = n64rt_pad_base_dflt[i];
    }
    return c >= 1u && c <= 14u ? c : 0u;
}
static inline uint16_t n64rt_pad_out(uint32_t c)
{
    static const uint16_t bit[15] = { 0, CONT_A, CONT_B, CONT_G, CONT_START, CONT_L, CONT_R, CONT_E, CONT_D, CONT_C, CONT_F,
                                      CONT_UP, CONT_DOWN, CONT_LEFT, CONT_RIGHT };
    return c < 15u ? bit[c] : 0u;
}
/* group g (0 the D-pad keys, 1 the left stick, 2 the right stick): its code in the layer the Modifier selects: 0 the settings', 1-4 a
 * target, 5 none; *lmod is set when the settings are used with the Modifier's effects (the D-pad keys' Held 0) */
static inline uint32_t n64rt_pad_gsel(const uint32_t *map, unsigned g, int held, int *lmod)
{
    uint32_t sel = n64rt_pad_field(map, 12u + g);
    if (held) {
        const uint32_t h = n64rt_pad_field(map, PAD_HELD + 12u + g);
        if (h == 0u) { sel = 0u; *lmod = 1; }
        else if (h != 6u) sel = h;
    }
    return sel == 7u ? 6u : sel > 4u ? 5u : sel;
}
/* k: the key word, j: the four stick bytes, set0 / set1: the two interact words, map: the five remap words (the runtime's table) */
static inline void n64rt_pad_map(uint32_t k, uint32_t j, uint32_t set0, uint32_t set1, const uint32_t *map, uint16_t *button, int8_t *stick_x, int8_t *stick_y)
{
    const int dz = n64rt_pad_deadzone(set0), rng = n64rt_pad_range(set0);
    const uint32_t type = KEY_TYPE(k);
    /* stick bytes to believe: a type 3 pad's always, a type 2 pad's unless they are what a pad with none would send */
    const int analog = type == 3u || (type == 2u && j != 0u && j != 0xFFFFFFFFu);
    uint16_t b = 0;
    int tx = 0, ty = 0;                                            /* the stick, from the D-pad keys given to it */

    const uint32_t mkey = n64rt_pad_key((set0 >> 10) & 31u, 0u);     /* Modifier */
    const int rmod = (k & mkey) != 0u;
    k &= ~mkey;
    for (unsigned i = 0; i < PAD_KEYS; i++)                         /* the keys' table, in the layer the Modifier selects */
        if (k & n64rt_pad_keys[i]) b |= n64rt_pad_out(n64rt_pad_target(map, i, rmod));

    /* the D-pad keys' group: 0 the "Stick" / "D-pad" settings, else its own target */
    int lmod = 0;                                                   /* the Modifier's effects in the "Stick" / "D-pad" settings */
    const uint32_t dsel = n64rt_pad_gsel(map, 0u, rmod, &lmod);
    if (dsel != 0u) {
        const int dx = ((k & KEY_DPAD_RIGHT) != 0u) - ((k & KEY_DPAD_LEFT) != 0u);
        const int dy = ((k & KEY_DPAD_UP) != 0u) - ((k & KEY_DPAD_DOWN) != 0u);
        switch (dsel) {
        case 1: tx += rng * dx; ty += rng * dy; break;
        case 2:
            if (dx && dy) { tx += (rng * 7 / 20) * dx; ty += (rng * 7 / 20) * dy; } else { tx += (rng / 2) * dx; ty += (rng / 2) * dy; }
            break;
        case 3:
            if (k & KEY_DPAD_UP) b |= CONT_UP;
            if (k & KEY_DPAD_DOWN) b |= CONT_DOWN;
            if (k & KEY_DPAD_LEFT) b |= CONT_LEFT;
            if (k & KEY_DPAD_RIGHT) b |= CONT_RIGHT;
            break;
        case 4:
            if (k & KEY_DPAD_UP) b |= CONT_E;
            if (k & KEY_DPAD_DOWN) b |= CONT_D;
            if (k & KEY_DPAD_LEFT) b |= CONT_C;
            if (k & KEY_DPAD_RIGHT) b |= CONT_F;
            break;
        case 6:                                                     /* the stick, full, and the N64's D-pad (the code 7) */
            tx += 80 * dx; ty += 80 * dy;
            if (k & KEY_DPAD_UP) b |= CONT_UP;
            if (k & KEY_DPAD_DOWN) b |= CONT_DOWN;
            if (k & KEY_DPAD_LEFT) b |= CONT_LEFT;
            if (k & KEY_DPAD_RIGHT) b |= CONT_RIGHT;
            break;
        default: break;
        }
        k &= ~(KEY_DPAD_UP | KEY_DPAD_DOWN | KEY_DPAD_LEFT | KEY_DPAD_RIGHT);
    }
    /* the analog sticks' groups (1 the left, 2 the right): 0 the settings', else its own target; the settings see it centred */
    for (unsigned g = 1; g <= 2; g++) {
        int unused = 0;
        uint32_t sel = n64rt_pad_gsel(map, g, rmod, &unused);
        if (sel == 0u) sel = g == 1u ? 1u : 4u;         /* the default: a dock pad's left stick is the N64's stick, its right the C buttons */
        const unsigned sh = g == 1u ? 0u : 16u;
        if (analog) {
            const int x = (int)((j >> sh) & 0xFFu) - 128, y = (int)((j >> (sh + 8)) & 0xFFu) - 128;
            switch (sel) {
            case 1: case 2: {
                const int dv = sel == 2u ? 2 : 1;
                const int ax = n64rt_pad_axis(j >> sh, 0, dz, rng) / dv, ay = n64rt_pad_axis(j >> (sh + 8), 1, dz, rng) / dv;
                tx += ax; ty += ay;
                break;
            }
            case 3:
                if (x < -64) b |= CONT_LEFT;
                if (x > 64) b |= CONT_RIGHT;
                if (y < -64) b |= CONT_UP;
                if (y > 64) b |= CONT_DOWN;
                break;
            case 4:
                if (x < -64) b |= CONT_C;
                if (x > 64) b |= CONT_F;
                if (y < -64) b |= CONT_E;
                if (y > 64) b |= CONT_D;
                break;
            default: break;
            }
        }
        j = (j & ~(0xFFFFu << sh)) | (0x8080u << sh);
    }

    if (((set1 >> 10) & 31u) == 1u && analog) {          /* "C buttons" = right stick: that too */
        const int rx = (int)((j >> 16) & 0xFFu) - 128, ry = (int)((j >> 24) & 0xFFu) - 128;
        if (rx < -64) b |= CONT_C;
        if (rx > 64) b |= CONT_F;
        if (ry < -64) b |= CONT_E;
        if (ry > 64) b |= CONT_D;
    }

    const uint32_t mode = (set1 >> 15) & 31u;                       /* Stick */
    const uint32_t dp = (set1 >> 22) & 3u;                       /* D-pad */
    const int dpad_swap = dp == 1u && mode != 1u && mode != 2u;
    int8_t sx = 0, sy = 0;
    int from_dpad = mode != 1u && mode != 2u;
    if (!from_dpad) {
        int8_t x = 0, y = 0;
        if (analog) {
            const int sh = mode == 2u ? 16 : 0;
            x = n64rt_pad_axis(j >> sh, 0, dz, rng); y = n64rt_pad_axis(j >> (sh + 8), 1, dz, rng);
        }
        sx = x; sy = y;
        if (type != 3u && sx == 0 && sy == 0) from_dpad = 1;       /* (a pad without a deflected stick, or none: the D-pad) */
    }
    if (from_dpad && !(dpad_swap && lmod)) {
        const int8_t d = (int8_t)(lmod ? rng / 2 : rng);
        if (k & KEY_DPAD_LEFT) sx -= d;
        if (k & KEY_DPAD_RIGHT) sx += d;
        if (k & KEY_DPAD_UP) sy += d;
        if (k & KEY_DPAD_DOWN) sy -= d;
        if (lmod && sx && sy) { const int8_t dg = (int8_t)(rng * 7 / 20); sx = (int8_t)(sx < 0 ? -dg : dg); sy = (int8_t)(sy < 0 ? -dg : dg); }
    }
    if (dp == 1u && (!dpad_swap || lmod)) {
        if (k & KEY_DPAD_UP) b |= CONT_UP;
        if (k & KEY_DPAD_DOWN) b |= CONT_DOWN;
        if (k & KEY_DPAD_LEFT) b |= CONT_LEFT;
        if (k & KEY_DPAD_RIGHT) b |= CONT_RIGHT;
    } else if (dp >= 2u && analog) {
        const int sh = dp == 3u ? 16 : 0;
        const int x = (int)((j >> sh) & 0xFFu) - 128, y = (int)((j >> (sh + 8)) & 0xFFu) - 128;
        if (x < -64) b |= CONT_LEFT;
        if (x > 64) b |= CONT_RIGHT;
        if (y < -64) b |= CONT_UP;
        if (y > 64) b |= CONT_DOWN;
    }
    int ox = sx + tx, oy = sy + ty;
    *button = b;
    *stick_x = (int8_t)(ox > rng ? rng : ox < -rng ? -rng : ox);
    *stick_y = (int8_t)(oy > rng ? rng : oy < -rng ? -rng : oy);
}
#endif
