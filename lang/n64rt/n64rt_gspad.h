/* The GameShark picker's inputs from the Pocket's keys, the stick bytes and the core's settings (n64rt_gspick.c): a pure function, host-tested by test/gspad_host.c.
 * The picker is read as an N64 controller is: n64rt_pad_map() gives the buttons the game would see -- so the Controls menu's mapping and the core's Start, C and R-modifier
 * settings are the player's own -- and the picker takes its actions from those:
 *   move up and down   the D-pad (the Pocket's, whatever the "D-pad" setting does with it, and the N64's), or a stick pushed past 40 % (and kept to 25 %) -- the left or the right analog stick of a pad
 *                      that has them (the dead zone is the setting's) --, or what the "Stick" setting makes of the keys
 *   option, < and >    the same, to the left and to the right
 *   page up            L or Z (identical: a page of the list up)         page down   R
 *   all off            C-down (the Pocket's Select, as the Controls menu has it)
 *   toggle             A                                                  cancel      B
 *   play               Start (the N64's Start as the settings make it, or the Pocket's Start key always: a setting that moves the N64's Start away cannot lock the picker)
 * There are no X, Y or Select in the N64's controller: the Pocket's X and Y are C-right and C-left. */
#ifndef N64RT_GSPAD_H
#define N64RT_GSPAD_H
#include "n64rt_pad.h"

enum { GP_UP = 1u << 0, GP_DOWN = 1u << 1, GP_LEFT = 1u << 2, GP_RIGHT = 1u << 3, GP_A = 1u << 4, GP_B = 1u << 5, GP_PREV = 1u << 6, GP_NEXT = 1u << 7,
       GP_OFF = 1u << 8, GP_START = 1u << 9 };

/* a stick's deflection (-80 .. 80, after the dead zone) as a direction: a move starts at 30 and goes on down to 20 (about 48 and 32 of the stick's 127 past a 13 % dead zone: no
 * flicker at the edge), on the longer axis only, so that a diagonal is one move. `prev`: the directions held a moment ago. */
static inline uint32_t n64rt_gspad_dir(int x, int y, uint32_t prev)
{
    const int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ay >= ax) {
        if (y > 0) return y >= ((prev & GP_UP) ? 20 : 30) ? GP_UP : 0u;
        return -y >= ((prev & GP_DOWN) ? 20 : 30) ? GP_DOWN : 0u;
    }
    if (x > 0) return x >= ((prev & GP_RIGHT) ? 20 : 30) ? GP_RIGHT : 0u;
    return -x >= ((prev & GP_LEFT) ? 20 : 30) ? GP_LEFT : 0u;
}

/* `k`, `j`: controller 1's key word and stick word (APF_INPUT's), `set0`, `set1`: the settings' two words, `prev`: the result of the last call. The result: the GP_ bits held now. */
static inline uint32_t n64rt_gspad(uint32_t k, uint32_t j, uint32_t set0, uint32_t set1, uint32_t prev)
{
    uint16_t b; int8_t sx, sy;
    n64rt_pad_map(k, j, set0, set1, &b, &sx, &sy);
    uint32_t r = 0;
    if ((k & KEY_DPAD_UP) || (b & CONT_UP)) r |= GP_UP;
    if ((k & KEY_DPAD_DOWN) || (b & CONT_DOWN)) r |= GP_DOWN;
    if ((k & KEY_DPAD_LEFT) || (b & CONT_LEFT)) r |= GP_LEFT;
    if ((k & KEY_DPAD_RIGHT) || (b & CONT_RIGHT)) r |= GP_RIGHT;
    r |= n64rt_gspad_dir(sx, sy, prev);
    const uint32_t type = KEY_TYPE(k);
    if (type == 3u || (type == 2u && j != 0u && j != 0xFFFFFFFFu)) {          /* (the analog sticks, whatever the "Stick" setting reads) */
        for (int s = 0; s < 2; s++) {
            int8_t x, y;
            n64rt_pad_stick(j >> (16 * s), (set1 >> 5) & 31u, &x, &y);
            r |= n64rt_gspad_dir(x, y, prev);
        }
    }
    if (b & CONT_A) r |= GP_A;
    if (b & CONT_B) r |= GP_B;
    if (b & (CONT_L | CONT_G)) r |= GP_PREV;
    if (b & CONT_R) r |= GP_NEXT;
    if (b & CONT_D) r |= GP_OFF;
    if ((b & CONT_START) || (k & KEY_START)) r |= GP_START;
    return r;
}
#endif
