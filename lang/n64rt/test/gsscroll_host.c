/* Host test of n64rt_gsscroll() (../n64rt_gsscroll.h): the scroll of a row that does not fit.
 *   gcc -Wall -I.. gsscroll_host.c -o /tmp/gsscroll_host && /tmp/gsscroll_host */
#include <stdio.h>
#include "../n64rt_gsscroll.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    /* a text that fits does not move, at any time */
    for (uint32_t t = 0; t < 20000u; t += 137u) CHECK(n64rt_gsscroll(t, 312u, 312u) == 0 && n64rt_gsscroll(t, 100u, 312u) == 0 && n64rt_gsscroll(t, 0u, 312u) == 0, "fits at %u", t);
    /* 412 pixels in a 312-pixel window: 100 pixels to go, 2857 ms at 35 pixels a second */
    const uint32_t text = 412u, win = 312u, d = 100u, run = (d * 1000u + 34u) / 35u;
    CHECK(run == 2858u, "the run is %u ms", run);
    CHECK(n64rt_gsscroll(0, text, win) == 0 && n64rt_gsscroll(999u, text, win) == 0, "the 1 s pause at the start");
    CHECK(n64rt_gsscroll(1000u + 1000u, text, win) == 35u, "35 pixels after one second of movement: %u", n64rt_gsscroll(2000u, text, win));
    CHECK(n64rt_gsscroll(1000u + 2000u, text, win) == 70u, "70 pixels after two");
    CHECK(n64rt_gsscroll(1000u + run, text, win) == d, "at the end of the text at the end of the run");
    CHECK(n64rt_gsscroll(1000u + run + 1999u, text, win) == d, "the 2 s pause at the end");
    CHECK(n64rt_gsscroll(1000u + run + 2000u, text, win) == d, "and its last instant");
    CHECK(n64rt_gsscroll(1000u + run + 2000u + 1000u, text, win) == d - 35u, "coming back at the same speed: %u", n64rt_gsscroll(1000u + run + 3000u, text, win));
    CHECK(n64rt_gsscroll(1000u + 2u * run + 2000u, text, win) == 0, "back at the start at the end of the return");
    const uint32_t period = 1000u + 2u * run + 2000u;
    CHECK(n64rt_gsscroll(period + 500u, text, win) == 0 && n64rt_gsscroll(period + 1000u + 1000u, text, win) == 35u, "and it begins again with the pause");
    /* the offset is within 0 .. d everywhere, never jumps more than the speed allows between two instants 17 ms apart, and every period is the same */
    uint32_t prev = 0;
    for (uint32_t t = 0; t < 3u * period; t += 17u) {
        const uint32_t o = n64rt_gsscroll(t, text, win);
        CHECK(o <= d, "offset %u at %u", o, t);
        CHECK((o > prev ? o - prev : prev - o) <= 2u, "a jump of %u at %u", o > prev ? o - prev : prev - o, t);
        CHECK(o == n64rt_gsscroll(t + period, text, win), "periodic at %u", t);
        prev = o;
    }
    /* a text one pixel too long still moves one pixel; a very long one does not overflow */
    CHECK(n64rt_gsscroll(1000u + 29u, 313u, 312u) == 1u, "one pixel");
    CHECK(n64rt_gsscroll(1000u + 60000u, 2400u, 312u) <= 2088u, "a long text");
    printf(fails ? "gsscroll_host: %d FAILED\n" : "gsscroll_host: all passed\n", fails);
    return fails != 0;
}
