/* The scan-out showing a framebuffer smaller than the screen
 * (rtl/soc/n64_video.sv: VIDEO_FRAMEBUFFER_WIDTH, _WINDOW, _HWINDOW and _SCALE): a 292 x 216
 * picture -- a white frame at its edge, a diagonal, a vertical line every 16
 * pixels -- must sit in the middle of the 640 x 240 field, whole, each pixel two clocks wide
 * (X_SCALE 0x200, as the VI shows a 320-wide picture), black around it. Then 320 x 240, then
 * 256 x 224, in turn. sim/mips_soc's VGA=1 writes what the pads send. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
void trap_handler(void) { puts_("TRAP\n"); for (;;) ; }

#define FB ((volatile uint16_t *)0x40C00000u)
#define HOLD 80u                         /* twentieths of a second a size stays */
static const struct { uint16_t w, h; } s_m[] = { { 292, 216 }, { 320, 240 }, { 256, 224 } };

int main(void)
{
    puts_("vwin_test\n");
    video_framebuffer_vtg_mode_write(1);
    video_framebuffer_dma_base_write(0x40C00000u);
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    for (;;) for (unsigned i = 0; i < sizeof s_m / sizeof s_m[0]; i++) {
        const uint32_t w = s_m[i].w, h = s_m[i].h;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint16_t c = (uint16_t)((x * 31u / w) << 11 | (y * 31u / h) << 6 | 0x0011u);
            if (x == 0 || y == 0 || x == w - 1u || y == h - 1u) c = 0xFFFFu;
            else if (x * h == y * w || (x & 15u) == 0u) c = 0xFFC1u;
            FB[y * w + x] = c;                      /* (big-endian: pixel x is the word's high half when x is even) */
        }
        flush_cpu_dcache();
        video_framebuffer_scale_write(0x200u | 0x400u << 16);
        video_framebuffer_hwindow_write((640u - 2u * w) / 2u | (2u * w) << 16);
        video_framebuffer_window_write(((240u - h) / 2u) << 8 | (h < 240u ? h : 0u) << 16);
        video_framebuffer_rowwords_write((w + 7u) / 2u + 2u);
        video_framebuffer_width_write(w);
        puts_("size "); putc_('1' + (char)i); puts_("\n");
        timer0_uptime_latch_write(1);
        uint64_t t0 = timer0_uptime_cycles_read();
        for (;;) { timer0_uptime_latch_write(1); if (timer0_uptime_cycles_read() - t0 > (uint64_t)CONFIG_CLOCK_FREQUENCY / 20u * HOLD) break; }
    }
}
