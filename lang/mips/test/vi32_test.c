/* A 32-bit frame buffer in the scan-out (rtl/soc/n64_video.sv vi_px32; docs/cimg32.md) with the VI in hardware (MIRLO_MON,
 * MIRLO_MI, MIRLO_VI; MONITOR.CTL's VI bit: the scan-out's base and filters are the VI registers'). The CPU fills a
 * 320 x 240 RGBA8888 picture: R a ramp across, G bands, B a ramp down, each with low bits the scan-out drops; the
 * coverage in the alpha byte's top three bits -- partial where (x ^ y) % 7 == 0 (coverage (x + y) % 7), complete (7)
 * elsewhere -- and noise in its low five, which nothing reads. The game (MCTL.GAME, as viaa_test) writes VI_STATUS type 3
 * with AA_MODE 0 (anti-alias) and DIVOT_ENABLE, VI_WIDTH 320 (pixels), ORIGIN and H_START; the program writes what the
 * runtime would (n64rt_io.c vi_width_gamma): VIDEO_FRAMEBUFFER.WIDTH 320 with bit 17 (32-bit pixels) and ROWWORDS 328 (a
 * word a pixel: 319 + 8 + 1). sim/mips_soc's VGA=1 writes the pads' frames; sim/vi_filter/vi32_check.py compares the last
 * with vi_model.py's filters on the picture's 16-bit view (each channel's top five bits, the alpha's coverage). */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }

#define RES ((volatile uint32_t *)0xA0000400u)       /* the game's arguments, RDRAM 0x400 (KSEG1) */
#define FB_PHYS 0x00100000u                          /* the frame buffer in the RDRAM */
#define W 320u
#define H 240u

/* the monitor: any entry is unexpected here (counted at 0xFFFF_8008) */
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl exception_entry\n"
"exception_entry:\n"
"    sw    $k0, -0x8000($zero)\n"
"    sw    $k1, -0x7FFC($zero)\n"
"    lw    $k0, -0x7FF8($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF8($zero)\n"
"    mfc0  $k0, $30\n"
"    addiu $k0, $k0, 4\n"
"    mtc0  $k0, $30\n"
"    lw    $k0, -0x8000($zero)\n"
"    lw    $k1, -0x7FFC($zero)\n"
"    eret\n"
"    nop\n"
);

/* the game: the VI's registers */
__attribute__((section("migame"), noinline, used, optimize("O1", "no-jump-tables"))) void vi32_game(void)
{
    volatile uint32_t *res = (volatile uint32_t *)0xA0000400u;
    volatile uint32_t *vi  = (volatile uint32_t *)0xA4400000u;
    vi[2] = W;                                       /* WIDTH: pixels, whatever their size */
    vi[9] = 0x006C02ECu;                             /* H_START (not 0: not black) */
    vi[1] = res[2];                                  /* ORIGIN */
    vi[0] = res[1];                                  /* STATUS */
}

extern const uint32_t __start_migame[], __stop_migame[];
static void copy_to_rdram(uint32_t phys, const uint32_t *a, const uint32_t *b)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(0xA0000000u + phys);
    while (a < b) *d++ = *a++;
}
typedef void (*game_t)(void);
static void game(uint32_t a1, uint32_t a2)
{
    RES[1] = a1; RES[2] = a2;
    flush_cpu_dcache();
    uint32_t m = 0x302u;
    __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(m));
    ((game_t)0x80001000u)();
    __asm__ volatile(".set push\n.set mips3\n li $t0, 0x200\n mtc0 $t0, $7\n.set pop" ::: "t0");
}

/* the picture (vi32_check.py makes the same): a big-endian word, R the lowest byte in memory */
static uint32_t cvg(uint32_t x, uint32_t y) { return ((x ^ y) % 7u) == 0u ? (x + y) % 7u : 7u; }
static uint32_t pixel(uint32_t x, uint32_t y)
{
    uint32_t r = (x * 255u) / (W - 1u), g = ((x + y) / 24u) & 1u ? 0xE5u : 0x1Bu, b = (y * 255u) / (H - 1u);
    return r << 24 | g << 16 | b << 8 | cvg(x, y) << 5 | (x & 31u);
}

int main(void)
{
    puts_("vi32_test\n");
    if ((monitor_id_read() & 0xAu) != 0xAu) { puts_("no VI in this SoC\nvi32_test: SKIPPED\n"); putc_(4); for (;;) ; }
    copy_to_rdram(0x1000u, __start_migame, __stop_migame);
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)(0xA0000000u + FB_PHYS);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) fb[y * W + x] = pixel(x, y);
    flush_cpu_dcache();
    video_framebuffer_vtg_mode_write(1);
    video_framebuffer_window_write(0u);
    video_framebuffer_rowwords_write(328u);          /* a word a pixel: the window's last source pixel 319, the priming's 8, one more */
    video_framebuffer_width_write(W | 1u << 17);     /* 320 pixels a row, 32-bit */
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
    for (int i = 0; i < 8; i++) mr[i] = 0;
    monitor_pend_write(0xF);
    monitor_mask_write(0u);
    monitor_ctl_write(0xDu);                         /* OWN, IP2 the MI's, the VI the hardware's */
    monitor_miclr_write(0x3Eu);
    game(0x3013u, FB_PHYS);                          /* type 3 (32-bit), divot, AA_MODE 0, gamma off; the origin */
    puts_("vi32_test: set\n");
    for (;;) ;
}
