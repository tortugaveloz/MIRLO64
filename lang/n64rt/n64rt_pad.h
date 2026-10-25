/* The N64 controller from the Pocket's (or the dock's) keys and the core's settings: n64rt_pad_map(), a pure function (no
 * hardware), so that lang/n64rt/test/pad_test.py can run every combination of settings against the spec on the host.
 *
 * The keys are APF_INPUT's (docs/control.md: Controller/Joystick Bitmap; the Pocket's own Controls menu remaps what the
 * buttons give, the runtime only sees these bits), the stick bytes are its four joystick bytes (left X, left Y, right X,
 * right Y, from the low byte up; 0x80 centred) and the settings are the two words of APF_INTERACT, as interact.json
 * (pkg/pocket/Cores/tortuga.Mirlo64/interact.json) writes them -- a field at a time, at the addresses 0x1000_0100 + 4 x n:
 *
 *   word 0 [4:0]    n=2  "Controller 1 pak": n64rt_pak.c
 *          [9:5]    n=3  "Crop Vertical" (the video: rtl/soc/mirlo_regs.sv)
 *          [14:10]  n=4  "R = Face C buttons" (1): R held turns the face buttons into the C buttons where they sit (X up, B down,
 *                        Y left, A right); R then never reaches the game
 *          [19:15]  n=5  "Crop Right" (the video; a list: 0 none, 1 pixel)
 *          [24:20]  n=6  "R = Joystick half press" (1): R held makes the D-pad's stick 60 % (R then never reaches the game)
 *          [29:25]  n=7  "R stick dead zone": 1 + its tenths of half the range (0 % .. 50 % in 5 % steps), 0 (or above 11) 10 %
 *   word 1 [4:0]    n=8  "Start button": which Pocket key is the N64's Start (a button code, below; 0 the Start key), or 12
 *                        Select and Start together (neither counts for anything else then); the key is the Start only
 *          [9:5]    n=9  "L stick dead zone": as the R stick's
 *          [14:10]  n=10 "C buttons": 0 none (the face buttons only), 1 also the right stick, 2 also the left stick, 3 also the
 *                        D-pad
 *          [19:15]  n=11 "Stick": 0 the D-pad (full deflection), 1 the left analog stick, 2 the right analog stick, 3 none
 *                        (the stick stays centred); any other value is the D-pad
 *          [21:20]  n=12 "D-pad double tap" (1): the D-pad's stick is 60 %, 100 % on a press within 300 ms of the last release
 *                        (dfull: the caller keeps that state)
 *          [23:22]  n=13 "D-pad": 0 none, 1 the D-pad, 2 the left analog stick, 3 the right analog stick
 *          [24]     n=14 free (was "Show FPS", which nothing ever read)
 *          [25]     n=15 free (was "Start = Select+Start")
 *
 * A button setting (Z, L, R, Modifier) is a code, 0 the setting's own default (so that the word's reset value, 0, is the
 * default layout before the Pocket has written anything): 1 the Pocket's L, 2 R, 3 ZL (L2), 4 ZR (R2), 5 none, 6 A, 7 B, 8 X,
 * 9 Y, 10 Select, 11 Start (anything else is none too).
 *   Modifier: 0 = none
 * The N64's Z, L and R are the Pocket's L, ZL and R keys, always: the Controls menu (input.json) is where they move.
 * Nothing couples them: a key may serve two N64 buttons if asked to, and the Modifier key never reaches the game.
 *
 * The C buttons are always on the face buttons: C-left on Y, C-right on X, C-down on Select, C-up on ZR (the Controls menu's C-Up),
 * and on the Start key when "Start button" does not take it (another key, none; Select+Start: Start alone) -- and through the
 * Modifier (its key held turns X, B, Y, A into C-up, C-down, C-left, C-right where they sit). "C buttons" = 1 adds the right
 * analog stick, 2 the left one, as another source (ORed): past +-64 of 127 on its longer axis it is the one C button of that axis
 * (a quarter of the circle each, the diagonals to up and down: never two at once); 3 adds the D-pad, a key a button.
 *
 * Analog sticks. The dock says a pad has them with KEY_TYPE 3; the Pocket's own controls (type 1) have none. The pad of a type
 * the docs call "without analog" (2) is believed, for its stick bytes, when they are not 0x00000000 or 0xFFFFFFFF (a pad with
 * nothing to send), in case the dock reports a pad with a stick so (lang/mips/test/padtest.c shows what the dock sends). The
 * "Stick" setting:
 *   D-pad    the D-pad keys, full deflection (half with the Modifier held); the stick bytes are not looked at
 *   L stick  the left stick (bytes 0, 1)    R stick  the right stick (bytes 2, 3)
 * With L or R stick, a type 3 pad gives the stick (centred: 0, 0). A type 2 pad gives the stick when it is deflected and, centred,
 * the D-pad keys (so a pad with no stick, or one the dock calls type 2, still moves); any other pad (the Pocket's own controls)
 * gives the D-pad keys. The "D-pad" setting (the N64's D-pad) takes a stick as the four directions too, past +-64 of 127, from
 * the same pads that have stick bytes to believe. A and B are fixed. */
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
#define KEY_SELECT (1u << 14)
#define KEY_START  (1u << 15)
#define KEY_TYPE(k) ((k) >> 28)          /* 1 Pocket, 2 dock pad without analog, 3 dock pad with analog */

/* a stick byte (0x80 centred) as the N64's axis, -80 .. 80: nothing within the dead zone (dzc: the setting's code), linear from
 * its edge to the end of the range */
static inline int8_t n64rt_pad_axis(uint32_t v, int flip, uint32_t dzc)
{
    int a = (int)(v & 0xFFu) - 128;
    if (flip) a = -a;
    const int pct = (dzc >= 1u && dzc <= 11u) ? (int)(dzc - 1u) * 5 : 10;
    const int t = pct * 128 / 100, m = a < 0 ? -a : a;
    if (m <= t) return 0;
    int o = (m - t) * 80 / (127 - t);
    if (o > 80) o = 80;
    return (int8_t)(a < 0 ? -o : o);
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
    case 6:  return KEY_A;
    case 7:  return KEY_B;
    case 8:  return KEY_X;
    case 9:  return KEY_Y;
    case 10: return KEY_SELECT;
    case 11: return KEY_START;
    default: return 0;
    }
}
static inline void n64rt_pad_map(uint32_t k, uint32_t j, uint32_t set0, uint32_t set1, int dfull, uint16_t *button, int8_t *stick_x,
                                 int8_t *stick_y)
{
    const uint32_t type = KEY_TYPE(k);
    /* stick bytes to believe: a type 3 pad's always, a type 2 pad's unless they are what a pad with none would send */
    const int analog = type == 3u || (type == 2u && j != 0u && j != 0xFFFFFFFFu);
    uint16_t b = 0;

    const int face = ((set0 >> 10) & 31u) == 1u, half = ((set0 >> 20) & 31u) == 1u;   /* the R checkboxes */
    const uint32_t mkey = (face || half) ? KEY_R1 : 0u;
    const int rmod = (k & mkey) != 0u;
    k &= ~mkey;
    if (rmod && face) {
        if (k & KEY_X) b |= CONT_E;
        if (k & KEY_B) b |= CONT_D;
        if (k & KEY_Y) b |= CONT_C;
        if (k & KEY_A) b |= CONT_F;
        k &= ~(KEY_A | KEY_B | KEY_X | KEY_Y);
    }
    const uint32_t scode = set1 & 31u;                                /* Start (and C-up on the Start key it leaves) */
    if (scode == 12u) {                       /* (Select with Start, or with C-Up's key: C-Up may be moved onto the Start button) */
        if ((k & KEY_SELECT) && (k & (KEY_START | KEY_R2))) { b |= CONT_START; k &= ~(KEY_SELECT | KEY_START | KEY_R2); }
        else if (k & KEY_START) b |= CONT_E;
    } else {
        const uint32_t skey = n64rt_pad_key(scode, KEY_START);
        if (k & skey) b |= CONT_START;
        if (skey != KEY_START && (k & KEY_START)) b |= CONT_E;
        k &= ~skey;                                                   /* (the Start's key is that only) */
    }
    if (k & KEY_R2) b |= CONT_E;                                      /* C-up (the Controls menu's, on ZR) */
    if (k & KEY_A) b |= CONT_A;
    if (k & KEY_B) b |= CONT_B;
    if (k & KEY_L1) b |= CONT_G;                                              /* Z (the Controls menu's) */
    if (k & KEY_L2) b |= CONT_L;                                              /* L (the Controls menu's, on ZL) */
    if (k & KEY_R1) b |= CONT_R;                                              /* R (the Controls menu's) */

    if (k & KEY_SELECT) b |= CONT_D;                     /* C buttons: the face buttons, always */
    if (k & KEY_Y) b |= CONT_C;
    if (k & KEY_X) b |= CONT_F;
    const uint32_t cm = (set1 >> 10) & 31u;                          /* and with "C buttons" a stick, that too */
    if ((cm == 1u || cm == 2u) && analog) {
        const int csh = cm == 1u ? 16 : 0;
        const int rx = (int)((j >> csh) & 0xFFu) - 128, ry = (int)((j >> (csh + 8)) & 0xFFu) - 128;
        const int ax = rx < 0 ? -rx : rx, ay = ry < 0 ? -ry : ry;
        if (ax > ay) { if (ax > 64) b |= rx < 0 ? CONT_C : CONT_F; }  /* (the longer axis only: one C button) */
        else if (ay > 64) b |= ry < 0 ? CONT_E : CONT_D;
    }
    if (cm == 3u) {
        if (k & KEY_DPAD_UP) b |= CONT_E;
        if (k & KEY_DPAD_DOWN) b |= CONT_D;
        if (k & KEY_DPAD_LEFT) b |= CONT_C;
        if (k & KEY_DPAD_RIGHT) b |= CONT_F;
    }

    const uint32_t mode = (set1 >> 15) & 31u;                       /* Stick */
    int8_t sx = 0, sy = 0;
    int from_dpad = mode != 1u && mode != 2u && mode != 3u;     /* (3: none -- the stick stays centred) */
    if (!from_dpad && mode != 3u) {
        int8_t x = 0, y = 0;
        if (analog) {
            const int sh = mode == 2u ? 16 : 0;
            const uint32_t dzc = mode == 2u ? (set0 >> 25) & 31u : (set1 >> 5) & 31u;   /* (its stick's dead zone) */
            x = n64rt_pad_axis(j >> sh, 0, dzc); y = n64rt_pad_axis(j >> (sh + 8), 1, dzc);
        }
        sx = x; sy = y;
        if (type != 3u && sx == 0 && sy == 0) from_dpad = 1;       /* (a pad without a deflected stick, or none: the D-pad) */
    }
    if (from_dpad) {
        const int part = (rmod && half) || (((set1 >> 20) & 3u) == 1u && !dfull);   /* (half press; a single tap) */
        const int8_t d = part ? 48 : 80;
        if (k & KEY_DPAD_LEFT) sx -= d;
        if (k & KEY_DPAD_RIGHT) sx += d;
        if (k & KEY_DPAD_UP) sy += d;
        if (k & KEY_DPAD_DOWN) sy -= d;
        if (part && sx && sy) { sx = (int8_t)(sx < 0 ? -34 : 34); sy = (int8_t)(sy < 0 ? -34 : 34); }
    }
    const uint32_t dp = (set1 >> 22) & 3u;                          /* D-pad */
    if (dp == 1u) {
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
    *button = b;
    *stick_x = sx;
    *stick_y = sy;
}
#endif
