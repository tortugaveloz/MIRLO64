/* The scan-out's sizes (rtl/soc/n64_video.sv: VIDEO_FRAMEBUFFER_WIDTH, _WINDOW, _HWINDOW, _SCALE and _ROWWORDS), in turn -- every
 * one goes out in the one 640 x 240 field, as the N64's VI shows it:
 *   1  640 x 480: Y_SCALE 2.0, the even rows (an interlaced picture's first field)
 *   2  640 x 240: X_SCALE 1.0, Y_SCALE 1.0
 *   3  320 x 240: X_SCALE 0.5, each pixel two clocks wide, 237 lines (a line short of the field's 240: the VI's own window)
 *   4  292 x 216 inside it (a framebuffer smaller than its frame: Banjo-Kazooie's way)
 *   5  440 x 330 whole: X_SCALE 0.6875, Y_SCALE 1.375 (GoldenEye's)
 *   6  384 x 240 in a 597 clock, 222 line window at (20, 9): X_SCALE 0x20E (Mario Tennis')
 * The picture: a white frame at its edge, a diagonal, a vertical line every 16
 * pixels, a tick at each row that is a multiple of 8 on the left edge (long:
 * 64), odd rows with their blue lowered -- a weave of the two fields shows as
 * stripes if rows are taken in the wrong order (size 1 must show none: the even
 * rows only). sim/mips_soc's VGA=1 writes what the pads send (v*.ppm). */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
void trap_handler(void) { puts_("TRAP\n"); for (;;) ; }

#define FB ((volatile uint16_t *)0x40C00000u)
#define HOLD 4u                          /* twentieths of a second a size stays */
static const struct { uint16_t w, h, xs, ys, px0, pw, y0, rows; } s_m[] = {
    { 640, 480, 0x400, 0x800, 0, 640, 0, 237 }, { 640, 240, 0x400, 0x400, 0, 640, 0, 0 }, { 320, 240, 0x200, 0x400, 0, 640, 0, 237 },
    { 292, 216, 0x200, 0x400, 28, 584, 12, 216 }, { 440, 330, 0x2C0, 0x580, 0, 640, 0, 237 }, { 384, 240, 0x20E, 0x400, 20, 597, 9, 222 },
};

int main(void)
{
    puts_("hires_test\n");
    video_framebuffer_vtg_mode_write(1);
    video_framebuffer_dma_base_write(0x40C00000u);
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    for (;;) for (unsigned i = 0; i < sizeof s_m / sizeof s_m[0]; i++) {
        const uint32_t w = s_m[i].w, h = s_m[i].h;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint16_t c = (uint16_t)((x * 31u / w) << 11 | (y * 31u / h) << 6 | ((y & 1u) ? 0x0003u : 0x001Fu) << 1 | 1u);
            if (x == 0 || y == 0 || x == w - 1u || y == h - 1u) c = 0xFFFFu;
            else if (x * h == y * w || (x & 15u) == 0u) c = 0xFFC1u;
            else if (x < ((y & 63u) == 0u ? 64u : (y & 7u) == 0u ? 16u : 0u)) c = 0xFFFFu;
            FB[y * w + x] = c;                      /* (big-endian: pixel x is the word's high half when x is even) */
        }
        flush_cpu_dcache();
        {
            const uint32_t xs = s_m[i].xs, pw = s_m[i].pw, smax = ((pw - 1u) * xs) >> 10;
            video_framebuffer_scale_write(xs | (uint32_t)s_m[i].ys << 16);
            video_framebuffer_hwindow_write(s_m[i].px0 | pw << 16);
            video_framebuffer_window_write((uint32_t)s_m[i].y0 << 8 | (uint32_t)s_m[i].rows << 16);
            video_framebuffer_rowwords_write((smax + 8u) / 2u + 1u);
            video_framebuffer_width_write(w);
        }
        puts_("size "); putc_('1' + (char)i); puts_("\n");
        timer0_uptime_latch_write(1);
        uint64_t t0 = timer0_uptime_cycles_read();
        for (;;) { timer0_uptime_latch_write(1); if (timer0_uptime_cycles_read() - t0 > (uint64_t)CONFIG_CLOCK_FREQUENCY / 20u * HOLD) break; }
    }
}
