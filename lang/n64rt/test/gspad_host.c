/* Host test of n64rt_gspad() (../n64rt_gspad.h): the GameShark picker's inputs from the Pocket's keys, the stick and the settings.
 *   gcc -Wall -I.. gspad_host.c -o /tmp/gspad_host && /tmp/gspad_host */
#include <stdio.h>
#include "../n64rt_gspad.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
/* a pad's stick word: x and y as bytes, 0x80 centred, y up is the low end */
static uint32_t joy(int x, int y, int rx, int ry) { return (uint32_t)(0x80 + x) | (uint32_t)(0x80 - y) << 8 | (uint32_t)(0x80 + rx) << 16 | (uint32_t)(0x80 - ry) << 24; }
static uint32_t pad(uint32_t k, uint32_t j, uint32_t s0, uint32_t s1) { return n64rt_gspad(k, j, s0, s1, 0); }
#define T3 0x30000000u
#define T1 0x10000000u

int main(void)
{
    /* the keys, with the settings the core starts with (all 0) */
    CHECK(pad(T1 | KEY_DPAD_UP, 0, 0, 0) == GP_UP, "D-pad up");
    CHECK(pad(T1 | KEY_DPAD_DOWN, 0, 0, 0) == GP_DOWN, "D-pad down");
    CHECK(pad(T1 | KEY_DPAD_LEFT, 0, 0, 0) == GP_LEFT, "D-pad left");
    CHECK(pad(T1 | KEY_DPAD_RIGHT, 0, 0, 0) == GP_RIGHT, "D-pad right");
    CHECK(pad(T1 | KEY_A, 0, 0, 0) == GP_A, "A");
    CHECK(pad(T1 | KEY_B, 0, 0, 0) == GP_B, "B");
    CHECK(pad(T1 | KEY_START, 0, 0, 0) == GP_START, "Start");
    CHECK(pad(T1 | KEY_L1, 0, 0, 0) == GP_PREV, "Z (the Pocket's L) is a page up");
    CHECK(pad(T1 | KEY_L2, 0, 0, 0) == GP_PREV, "L (the Pocket's ZL) is a page up too");
    CHECK(pad(T1 | KEY_R1, 0, 0, 0) == GP_NEXT, "R is a page down");
    CHECK(pad(T1 | KEY_SELECT, 0, 0, 0) == GP_OFF, "C-down (the Pocket's Select) is all off");
    CHECK(pad(T1 | KEY_X, 0, 0, 0) == 0 && pad(T1 | KEY_Y, 0, 0, 0) == 0, "X and Y (C-right, C-left) do nothing");
    CHECK(pad(T1 | KEY_R2, 0, 0, 0) == 0, "C-up does nothing");
    CHECK(pad(T1, 0, 0, 0) == 0, "no key, no action");
    /* the stick of a pad that has one (type 3), in each of the "Stick" settings; the dead zone is the setting's (0: 13 %) */
    for (uint32_t mode = 0; mode < 4; mode++) {
        const uint32_t s1 = mode << 15;
        CHECK(pad(T3, joy(0, 0, 0, 0), 0, s1) == 0, "stick centred, mode %u", mode);
        CHECK(pad(T3, joy(0, 120, 0, 0), 0, s1) == GP_UP, "left stick up, mode %u", mode);
        CHECK(pad(T3, joy(0, -120, 0, 0), 0, s1) == GP_DOWN, "left stick down, mode %u", mode);
        CHECK(pad(T3, joy(-120, 0, 0, 0), 0, s1) == GP_LEFT, "left stick left, mode %u", mode);
        CHECK(pad(T3, joy(120, 0, 0, 0), 0, s1) == GP_RIGHT, "left stick right, mode %u", mode);
        CHECK(pad(T3, joy(0, 0, 0, -120), 0, s1) == GP_DOWN, "right stick down, mode %u", mode);
        CHECK(pad(T3, joy(0, 0, 120, 0), 0, s1) == GP_RIGHT, "right stick right, mode %u", mode);
        CHECK(pad(T3, joy(15, 15, 0, 0), 0, s1) == 0, "inside the dead zone, mode %u", mode);
        CHECK(pad(T3, joy(70, 120, 0, 0), 0, s1) == GP_UP, "a diagonal is one move: the longer axis, mode %u", mode);
        CHECK(pad(T3, joy(120, 70, 0, 0), 0, s1) == GP_RIGHT, "a diagonal is one move: the longer axis, mode %u", mode);
    }
    /* the hysteresis: a move starts at 30 and goes on to 20 (of 80, after the dead zone), and a diagonal's longer axis only */
    {   const uint32_t on = joy(0, 70, 0, 0), mid = joy(0, 50, 0, 0), low = joy(0, 40, 0, 0);
        CHECK(n64rt_gspad(T3, mid, 0, 0, 0) == 0, "a deflection between the thresholds does not start a move");
        CHECK(n64rt_gspad(T3, on, 0, 0, 0) == GP_UP, "a deflection past the start threshold moves");
        CHECK(n64rt_gspad(T3, mid, 0, 0, GP_UP) == GP_UP, "and goes on while it is above the end threshold");
        CHECK(n64rt_gspad(T3, low, 0, 0, GP_UP) == 0 && n64rt_gspad(T3, low, 0, 0, 0) == 0, "and stops below it");
        CHECK(n64rt_gspad_dir(25, 0, 0) == 0 && n64rt_gspad_dir(25, 0, GP_RIGHT) == GP_RIGHT && n64rt_gspad_dir(-19, 0, GP_LEFT) == 0, "the thresholds, left and right"); }
    /* the larger dead zone (code 15: 70 %) keeps a half deflection out */
    CHECK(pad(T3, joy(0, 60, 0, 0), 0, 15u << 5) == 0, "a half stick inside a large dead zone");
    CHECK(pad(T3, joy(0, 127, 0, 0), 0, 15u << 5) == GP_UP, "a full stick past a large dead zone");
    /* a Pocket's own controls (type 1) have no stick: its bytes are not looked at */
    CHECK(pad(T1, joy(0, 120, 0, 0), 0, 0) == 0, "no stick on a type 1 pad");
    /* the stick and the D-pad together: one action each, no more */
    CHECK(pad(T3 | KEY_DPAD_UP, joy(0, 120, 0, 0), 0, 1u << 15) == GP_UP, "the D-pad key the dock raises with the stick is the same move");
    /* the N64's D-pad setting (the Pocket's D-pad or a stick as the N64's D-pad) moves too */
    CHECK(pad(T1 | KEY_DPAD_DOWN, 0, 0, 1u << 22) == GP_DOWN, "N64 D-pad setting 1");
    CHECK(pad(T3, joy(0, 120, 0, 0), 0, 2u << 22) == GP_UP, "N64 D-pad on the left stick");
    /* the buttons follow the player's settings: Start moved to another key still starts (the Pocket's own Start key too), Z, L and R moved */
    CHECK(pad(T1 | KEY_A, 0, 0, 6u) == GP_START, "Start on A: that key is the N64's Start, and the picker's");
    CHECK(pad(T1 | KEY_START, 0, 0, 5u) == GP_START, "Start key with Start on none: the picker still starts");
    CHECK(pad(T1 | KEY_X, 0, 0, 8u) == GP_START, "Start on X");
    /* R as the face-button modifier: R held turns B into C-down (all off) */
    CHECK(pad(T1 | KEY_R1 | KEY_B, 0, 1u << 10, 0) == GP_OFF, "R held and B: C-down, not a cancel");
    printf(fails ? "gspad_host: %d FAILED\n" : "gspad_host: all passed\n", fails);
    return fails != 0;
}
