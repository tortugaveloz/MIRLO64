/* The RDP's shadow plane with the core's "Picture: 32-bit" setting (docs/video_options.md), end to end on the whole SoC: the CPU starts the
 * RDP on a list of commands in the RDRAM (vishadow_cmds.h: sim/vi_filter/vishadow_gen.c) that fills a 16-bit image and draws four ramps of
 * shaded triangles, 0 to 255 across 256 pixels; the VI in hardware (VI_STATUS 0x302, the filters off) scans it out. With the setting on
 * (the bench's INTERACT=0x10000144:1) the pads show 8-bit levels; sim/vi_filter/vishadow_check.sh compares the frame with the model's.
 * (The rest of this comment is viaa_test's: the VI in hardware
 * (MIRLO_MON, MIRLO_MI, MIRLO_VI; MONITOR.CTL's VI bit: the scan-out's base, gamma and filters are the VI registers'),
 * on a 320 x 240 framebuffer (the filters are off for a hi-res one, shown at half its width). The game (MCTL.GAME, as vi_test) writes VI_STATUS
 * -- 16-bit, AA_MODE 0 (anti-alias), DIVOT_ENABLE, gamma off -- VI_ORIGIN and H_START; the picture: colour bands with
 * every pixel where (x ^ y) % 7 == 0 partial (its coverage (x + y) % 7: bit 0 and the hidden plane's two -- layout.h
 * LAYOUT_HID), the rest complete (7). sim/mips_soc's VGA=1 writes the pads'
 * frames; sim/vi_filter/viaa_check.py compares the last with vi_model.py's filters on the same picture. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }

#define RES ((volatile uint32_t *)0xA0000400u)       /* the game's arguments, RDRAM 0x400 (KSEG1) */
#define FB_PHYS 0x00100000u                          /* the framebuffer in the RDRAM */
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
__attribute__((section("migame"), noinline, used, optimize("O1", "no-jump-tables"))) void viaa_game(void)
{
    volatile uint32_t *res = (volatile uint32_t *)0xA0000400u;
    volatile uint32_t *vi  = (volatile uint32_t *)0xA4400000u;
    vi[2] = W;                                       /* WIDTH */
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

/* the picture: the RDP's */
#include "vishadow_cmds.h"
#define DPC  ((volatile uint32_t *)0x20000040u)      /* 0 START 1 END 2 CURRENT 3 STATUS */
#define MI   ((volatile uint32_t *)0xA4300000u)

int main(void)
{
    puts_("vishadow_test\n");
    if ((monitor_id_read() & 0xAu) != 0xAu) { puts_("no VI in this SoC\nvishadow_test: SKIPPED\n"); putc_(4); for (;;) ; }
    copy_to_rdram(0x1000u, __start_migame, __stop_migame);
    const uint32_t n = sizeof vishadow_cmds / 4u, list = 0x350000u;
    volatile uint32_t *l = (volatile uint32_t *)(uintptr_t)(0xA0000000u + list);
    for (uint32_t i = 0; i < n; i++) l[i] = vishadow_cmds[i];
    flush_cpu_dcache();
    video_framebuffer_vtg_mode_write(1);
    video_framebuffer_window_write(0u);
    video_framebuffer_width_write(0u);               /* 320 x 240 */
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
    for (int i = 0; i < 8; i++) mr[i] = 0;
    monitor_pend_write(0xF);
    monitor_mask_write(0u);
    monitor_ctl_write(0xDu);                         /* OWN, IP2 the MI's, the VI the hardware's */
    monitor_miclr_write(0x3Eu);
    /* the RDP: START and END of the list, then SYNC FULL's interrupt */
    DPC[0] = list; DPC[1] = list + 4u * n;
    for (uint32_t k = 0; k < 40000000u && !(MI[2] & 0x20u); k++) { }
    puts_((MI[2] & 0x20u) ? "vishadow_test: RDP done\n" : "vishadow_test: RDP timed out\n");
    game(0x302u, FB_PHYS);                           /* 16-bit, no filters, gamma off; the origin */
    puts_("vishadow_test: set\n");
    for (;;) ;
}
