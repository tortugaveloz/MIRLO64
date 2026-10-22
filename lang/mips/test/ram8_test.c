/* The whole 8 MiB of the N64's RDRAM (the Expansion Pak's: a valid console for every game), through every block that
 * addresses it (the SoC built with MIRLO_RSP, MIRLO_MON, MIRLO_MI, MIRLO_PI, MIRLO_VI, MIRLO_AI, MIRLO_SI; sim/mips_soc):
 * the RDRAM is SDRAM 0x4380_0000 - 0x43FF_FFFF, and everything above its first 4 MiB -- where a 22-bit address would wrap
 * to the lower half -- is used here:
 *
 *   A  the CPU: a word at an address above 4 MiB is a word of its own (K0, K1, and the SDRAM address), and the word
 *      4 MiB below it is not touched
 *   B  the RSP's DMA, both ways, from and to the top of the RDRAM (the last 8 bytes included)
 *   C  the RDP: a colour image above 4 MiB (a fill rectangle; triangles with a depth image above 4 MiB, written and compared),
 *      a texture above 4 MiB (loaded by Load_Block, drawn by a Texture_Rectangle)
 *   D  the PI's engine: SDRAM into the RDRAM above 4 MiB, the RDRAM's top out to the SDRAM, RDRAM to RDRAM
 *   E  the SI: WR64B from and RD64B to the RDRAM's last 64 bytes, and 64 bytes above 4 MiB
 *   F  the AI: a buffer above 4 MiB (the bench's AILOG: tools/ai_check.py --snap says the frames the reader fetched are the
 *      buffer as it was when its length was written)
 *   G  the VI: ORIGIN above 4 MiB is the scan-out's base (the bench's VITRACE prints it, VGA=1 writes the picture: sim/mips_soc
 *      ram8_check.sh looks at both)
 *
 * The RDP's hidden coverage plane (SDRAM 0x4370_0000, 1 MiB) holds the two coverage bits of each 16-bit word of the 8 MiB. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}
void trap_handler(void) { puts_("TRAP\n"); for (;;) ; }
/* the monitor level's vector is not used here (nothing is entered); a stray entry is reported */
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

#define RDRAM_SDRAM 0x43800000u
#define K0(pa) ((volatile uint32_t *)(uintptr_t)(0x80000000u | (pa)))
#define K1(pa) ((volatile uint32_t *)(uintptr_t)(0xA0000000u | (pa)))
#define SD(pa) ((volatile uint32_t *)(uintptr_t)(RDRAM_SDRAM + (pa)))
#define RDRAM_TOP 0x00800000u
static uint32_t lcg_s;
static uint32_t lcg(void) { lcg_s = lcg_s * 1664525u + 1013904223u; return lcg_s ^ (lcg_s >> 15); }
static uint32_t now(void) { timer0_uptime_latch_write(1); return (uint32_t)timer0_uptime_cycles_read(); }
static void wait_cycles(uint32_t n) { uint32_t t0 = now(); while (now() - t0 < n) { } }
static void fill(uint32_t pa, uint32_t bytes, uint32_t seed)
{
    lcg_s = seed;
    for (uint32_t i = 0; i < bytes / 4; i++) *K1(pa + 4 * i) = lcg();
}
static uint32_t diff(uint32_t pa, uint32_t bytes, uint32_t seed)
{
    uint32_t bad = 0;
    lcg_s = seed;
    for (uint32_t i = 0; i < bytes / 4; i++) if (*K1(pa + 4 * i) != lcg()) bad++;
    return bad;
}

/* ---- A: the CPU -------------------------------------------------------------------------- */
static void test_cpu(void)
{
    static const uint32_t hi[] = { 0x400000u, 0x400004u, 0x5FFFF0u, 0x7FFFFCu, 0x6A5A58u };
    for (unsigned i = 0; i < sizeof hi / sizeof hi[0]; i++) {
        const uint32_t h = hi[i], l = h - 0x400000u;
        *K1(l) = 0x10C0FFEEu + i;
        *K1(h) = 0x8C0FFEE0u + i;
        check("A: the word 4 MiB below is untouched", *K1(l), 0x10C0FFEEu + i);
        check("A: the word above 4 MiB (K1)", *K1(h), 0x8C0FFEE0u + i);
        check("A: ... as the SDRAM has it", *SD(h), 0x8C0FFEE0u + i);
        *K0(h) = 0x5C0DE000u + i;                       /* cached: written through */
        flush_cpu_dcache();
        check("A: a store through K0", *K1(h), 0x5C0DE000u + i);
        check("A: ... and the lower word still", *K1(l), 0x10C0FFEEu + i);
    }
}

/* ---- B: the RSP's DMA --------------------------------------------------------------------- */
#define RSP_BASE   0x20000000u
#define SP_REG(o)  (*(volatile uint32_t *)(uintptr_t)(RSP_BASE + (o)))
#define SP_MEM_ADDR SP_REG(0x00)
#define SP_DRAM_ADDR SP_REG(0x04)
#define SP_RD_LEN  SP_REG(0x08)
#define SP_WR_LEN  SP_REG(0x0C)
#define SP_STATUS  SP_REG(0x10)
#define SP_DMA_BUSY SP_REG(0x18)
#define SP_PC      SP_REG(0x20)
static void sp_wait(void)
{
    uint32_t n;
    for (n = 0; n < 20000u && SP_DMA_BUSY; n++) { }
    check("the RSP's DMA ended", n < 20000u, 1u);
}
static void test_rsp(void)
{
    SP_STATUS = 0x2u;                                    /* halted */
    struct { uint32_t src, dst, len; } t[] = {
        { 0x7FFF00u, 0x640000u, 256u }, { 0x5A5A00u, 0x7FFF00u, 256u }, { 0x400000u, 0x400100u, 64u }, { 0x7FFFF8u, 0x7FFF00u, 8u },
    };
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
        fill(t[k].src, t[k].len, 0x1234u + k);
        fill(t[k].dst, t[k].len, 0x9999u);
        /* (the aliases 4 MiB down: a sentinel each) */
        *K1(t[k].src - 0x400000u) = 0xA11A5000u + k;
        *K1(t[k].dst - 0x400000u) = 0xA11A6000u + k;
        SP_MEM_ADDR = 0x100u; SP_DRAM_ADDR = t[k].src; SP_RD_LEN = t[k].len - 1u;      /* RDRAM -> DMEM */
        sp_wait();
        SP_MEM_ADDR = 0x100u; SP_DRAM_ADDR = t[k].dst; SP_WR_LEN = t[k].len - 1u;      /* DMEM -> RDRAM */
        sp_wait();
        check("B: DMA out of and into the RDRAM above 4 MiB", diff(t[k].dst, t[k].len, 0x1234u + k), 0u);
        check("B: the alias of the source untouched", *K1(t[k].src - 0x400000u), 0xA11A5000u + k);
        check("B: the alias of the destination untouched", *K1(t[k].dst - 0x400000u), 0xA11A6000u + k);
    }
}

/* ---- C: the RDP ----------------------------------------------------------------------------- */
#define UCODE 0x00001000u
#define CMDS  0x00002000u
#define CIMG  0x00640000u                                /* the colour image: 320 x 240 x 16 bits, to 0x665800 */
#define ZIMG  0x00700000u                                /* the depth image, likewise to 0x725800 */
#define TIMG  0x006C0000u                                /* a texture */
static int rdp_run(uint32_t words)
{
    volatile uint32_t *u = K1(UCODE);
    uint32_t end = CMDS + 4u * words;
    u[0] = 0x3C010000u | (CMDS >> 16);  u[1] = 0x34210000u | (CMDS & 0xFFFFu);     /* lui / ori $1: START */
    u[2] = 0x3C020000u | (end >> 16);   u[3] = 0x34420000u | (end & 0xFFFFu);      /* $2: END */
    u[4] = 0x34030015u;                 u[5] = 0x40835800u;                         /* DPC_STATUS: no XBUS, freeze, flush */
    u[6] = 0x40814000u;                 u[7] = 0x40824800u;                         /* mtc0 $1, START; mtc0 $2, END */
    u[8] = 0x0000000Du;                 u[9] = 0u;                                  /* break */
    for (int i = 10; i < 16; i++) u[i] = 0u;
    uint32_t sync0 = rdp_sync_count_read();
    SP_STATUS = 0x2u;
    SP_MEM_ADDR = 0x1000u; SP_DRAM_ADDR = UCODE; SP_RD_LEN = 63u;
    sp_wait();
    SP_PC = 0;
    SP_STATUS = 0x100u | 0x20u | 0x4u | 0x1u | 0x8u;     /* clear single step, broke, halt, interrupt */
    for (uint32_t i = 0; i < 6000000u; i++)
        if ((SP_STATUS & 1u) && rdp_sync_count_read() != sync0) { wait_cycles(2000); return 1; }
    return 0;
}
static uint32_t *cmd_base(void) { return (uint32_t *)(uintptr_t)(0xA0000000u + CMDS); }
static uint16_t px(uint32_t x, uint32_t y) { return (uint16_t)(*K1(CIMG + ((y * 320u + x) & ~1u) * 2u) >> ((x & 1u) ? 0 : 16)); }
static void clear_image(uint16_t c)
{
    for (uint32_t i = 0; i < 320u * 240u / 2u; i++) *K1(CIMG + 4u * i) = (uint32_t)c << 16 | c;
}
/* a triangle over the whole screen (0,0) (960,0) (0,720), one colour, one depth (zburst_test.c) */
static uint32_t *triangle(uint32_t *c, uint32_t rgba, uint32_t z, uint32_t modes)
{
    *c++ = 0xE7000000u; *c++ = 0u;                       /* sync pipe */
    *c++ = 0xEF0000F0u; *c++ = modes;                    /* other modes: one cycle, no dither */
    *c++ = 0xFA000000u; *c++ = rgba;                     /* primitive colour */
    *c++ = 0xEE000000u; *c++ = z << 16;                  /* primitive depth */
    *c++ = 0x09800B40u; *c++ = 0u;                       /* depth-buffered triangle, the long edge on the left; YL 720, YM = YH = 0 */
    *c++ = 0x03C00000u; *c++ = 0xFFFEAAABu;              /* XL 960, -4/3 a line */
    *c++ = 0u; *c++ = 0u;                                /* XH 0 */
    *c++ = 0u; *c++ = 0u;                                /* XM */
    *c++ = z << 16; *c++ = 0u; *c++ = 0u; *c++ = 0u;     /* Z, DzDx, DzDe, DzDy */
    return c;
}
#define GREEN  0x07C1u
#define RED    0xF801u
#define BLUE   0x003Fu
static void test_rdp(void)
{
    /* C1: a fill rectangle into a colour image above 4 MiB */
    clear_image(0x1234u);
    *K1(CIMG - 0x400000u) = 0xA11A7001u;
    uint32_t *c = cmd_base();
    *c++ = 0xFF10013Fu; *c++ = CIMG;                     /* colour image: 16 bits, 320 wide */
    *c++ = 0xED000000u; *c++ = 0x005003C0u;              /* scissor 320 x 240 */
    *c++ = 0xEF300000u; *c++ = 0u;                       /* other modes: fill */
    *c++ = 0xF7000000u; *c++ = 0xF801F801u;              /* fill colour: red, twice */
    *c++ = 0xF6000000u | ((320u << 2) << 12) | (16u << 2); *c++ = 0u;      /* fill rectangle (0,0) - (320,16) */
    *c++ = 0xE9000000u; *c++ = 0u;                       /* sync full */
    check("C1: the RDP ran", (uint32_t)rdp_run((uint32_t)(c - cmd_base())), 1u);
    check("C1: a pixel of the rectangle", px(100, 5), RED);
    check("C1: its last pixel of the row", px(318, 5), RED);
    check("C1: its last row", px(100, 14), RED);
    check("C1: below it: untouched", px(100, 40), 0x1234u);
    check("C1: the lower image untouched", *K1(CIMG - 0x400000u), 0xA11A7001u);

    /* C2: triangles with a depth image above 4 MiB: stripes five pixels wide, near (0x3000) and far (0x5000) in turn written
     * (colour green), then the whole screen red at 0x4000, compared only: it passes where the stored depth is the far one */
    clear_image(BLUE);
    for (uint32_t i = 0; i < 320u * 240u / 2u; i++) *K1(ZIMG + 4u * i) = 0u;       /* (nothing written there yet: depth 0 would fail every compare) */
    *K1(ZIMG - 0x400000u) = 0xA11A7002u;
    c = cmd_base();
    *c++ = 0xFF10013Fu; *c++ = CIMG;
    *c++ = 0xFE000000u; *c++ = ZIMG;                     /* depth image */
    *c++ = 0xFC887F10u; *c++ = 0x88FDF6FBu;              /* combine: the primitive colour */
    const uint32_t sw = 5u, ns = 320u / sw;
    for (uint32_t k = 0; k < ns; k++) {
        *c++ = 0xED000000u | (4u * sw * k) << 12; *c++ = (4u * sw * (k + 1u)) << 12 | 0x3C0u;
        c = triangle(c, 0x00FF00FFu, (k & 1u) ? 0x5000u : 0x3000u, 0x24u);
    }
    *c++ = 0xED000000u; *c++ = 0x005003C0u;
    c = triangle(c, 0xFF0000FFu, 0x4000u, 0x14u);
    *c++ = 0xE9000000u; *c++ = 0u;
    check("C2: the RDP ran", (uint32_t)rdp_run((uint32_t)(c - cmd_base())), 1u);
    uint32_t bad = 0, red = 0, green = 0;
    for (uint32_t y = 40; y < 80; y++) for (uint32_t x = 8; x < 300; x++) {      /* (inside the triangle) */
        const int far = (int)((x / sw) & 1u);
        const uint16_t v = px(x, y) & 0xFFFEu;
        if (v == (RED & 0xFFFEu)) red++; else if (v == (GREEN & 0xFFFEu)) green++;
        if (v != ((far ? RED : GREEN) & 0xFFFEu)) bad++;
    }
    check("C2: far stripes red, near ones green (depth compared above 4 MiB)", bad, 0u);
    check("C2: red pixels seen", red != 0u, 1u);
    check("C2: green pixels seen", green != 0u, 1u);
    check("C2: the lower depth word untouched", *K1(ZIMG - 0x400000u), 0xA11A7002u);

    /* C3: a texture above 4 MiB: 8 x 8 texels of 16 bits, each its own colour, loaded with Load_Block and copied to the screen by a
     * Texture_Rectangle in copy mode, 1:1 */
    for (uint32_t i = 0; i < 32u; i++) *K1(TIMG + 4u * i) = (uint32_t)(((0x0801u + 0x1082u * (2u * i)) & 0xFFFEu) | 1u) << 16 | (uint32_t)(((0x0801u + 0x1082u * (2u * i + 1u)) & 0xFFFEu) | 1u);
    *K1(TIMG - 0x400000u) = 0xA11A7003u;
    clear_image(0x0001u);
    c = cmd_base();
    *c++ = 0xFF10013Fu; *c++ = CIMG;
    *c++ = 0xED000000u; *c++ = 0x005003C0u;
    *c++ = 0xEF200000u; *c++ = 0u;                       /* other modes: copy */
    *c++ = 0xFD100007u; *c++ = TIMG;                     /* texture image: RGBA 16 bits, 8 wide */
    *c++ = 0xF5100400u; *c++ = 0x07000000u;              /* tile 7: RGBA 16 bits, 2 words a row, TMEM 0 */
    *c++ = 0xF3000000u; *c++ = (7u << 24) | (63u << 12) | 0x400u;       /* load block: 64 texels, dxt 0x400 */
    *c++ = 0xE6000000u; *c++ = 0u;                       /* load sync */
    *c++ = 0xF5100400u; *c++ = 0x00000000u;              /* tile 0: the same, to draw with */
    *c++ = 0xF2000000u; *c++ = (((7u << 2) << 12)) | (7u << 2);     /* tile size 8 x 8 */
    *c++ = 0xE4000000u | (((16u + 8u) << 2) << 12) | ((32u + 8u) << 2); *c++ = (((16u) << 2) << 12) | (32u << 2);   /* texture rectangle (16,32) - (24,40) */
    *c++ = 0u; *c++ = ((4u << 10) << 16) | (1u << 10);          /* S 0 T 0, DsDx 4.0 (copy mode: four pixels a clock), DtDy 1.0 */
    *c++ = 0xE9000000u; *c++ = 0u;
    check("C3: the RDP ran", (uint32_t)rdp_run((uint32_t)(c - cmd_base())), 1u);
    bad = 0;
    for (uint32_t t = 0; t < 8u; t++) for (uint32_t s = 0; s < 8u; s++) {
        const uint32_t i = t * 8u + s, want = (0x0801u + 0x1082u * i) & 0xFFFEu;
        if ((px(16u + s, 32u + t) & 0xFFFEu) != want) bad++;
    }
    check("C3: the texture's texels, as drawn", bad, 0u);
    if (bad) {
        for (uint32_t t = 0; t < 10u; t++) {
            for (uint32_t x = 12; x < 28u; x++) { puthex(px(x, 31u + t)); putc_(' '); }
            puts_("\n");
        }
    }
    check("C3: outside the rectangle: untouched", px(40, 32), 0x0001u);
    check("C3: the lower texture word untouched", *K1(TIMG - 0x400000u), 0xA11A7003u);
}

/* ---- D: the PI's engine ------------------------------------------------------------------------ */
static void pi_wait(void) { for (uint32_t t = 0; t < 4000000u && (monitor_pilen_read() >> 31); t++) { } }
/* dst, src: SDRAM addresses (0x4000_0000 + ...); bytes a multiple of 4, as the runtime's pi_hw_dma does a whole copy */
static void pi_copy(uint32_t dst, uint32_t src, uint32_t bytes)
{
    pi_wait();
    monitor_pidst_write((dst - 0x40000000u) >> 2);
    monitor_pisrc_write((src - 0x40000000u) >> 2);
    monitor_pilen_write(bytes >> 2);
    pi_wait();
}
static void test_pi(void)
{
    volatile uint32_t *s = (volatile uint32_t *)(uintptr_t)0x41000000u;      /* a scratch megabyte of the SDRAM */
    lcg_s = 0x777u;
    for (uint32_t i = 0; i < 1024u; i++) s[i] = lcg();
    flush_cpu_dcache();
    *K1(0x640000u - 0x400000u) = 0xA11A8001u;
    pi_copy(RDRAM_SDRAM + 0x640000u, 0x41000000u, 4096u);                     /* SDRAM -> the RDRAM above 4 MiB */
    check("D: SDRAM -> RDRAM above 4 MiB", diff(0x640000u, 4096u, 0x777u), 0u);
    check("D: ... the lower word untouched", *K1(0x640000u - 0x400000u), 0xA11A8001u);
    fill(0x7FF000u, 4096u, 0x888u);
    pi_copy(0x41100000u, RDRAM_SDRAM + 0x7FF000u, 4096u);                     /* the RDRAM's top out to the SDRAM */
    flush_cpu_dcache();
    lcg_s = 0x888u;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 1024u; i++) if (*(volatile uint32_t *)(uintptr_t)(0x41100000u + 4u * i) != lcg()) bad++;
    check("D: the RDRAM's top 4 KiB -> SDRAM", bad, 0u);
    fill(0x600000u, 8192u, 0x999u);
    *K1(0x7F0000u - 0x400000u) = 0xA11A8002u;
    pi_copy(RDRAM_SDRAM + 0x7F0000u, RDRAM_SDRAM + 0x600000u, 8192u);         /* RDRAM -> RDRAM, both above 4 MiB */
    check("D: RDRAM -> RDRAM above 4 MiB", diff(0x7F0000u, 8192u, 0x999u), 0u);
    check("D: ... the lower word untouched", *K1(0x7F0000u - 0x400000u), 0xA11A8002u);
}

/* ---- E: the SI --------------------------------------------------------------------------------- */
#define SI  ((volatile uint32_t *)0xA4800000u)       /* 0 DRAM_ADDR, 1 RD64B, 4 WR64B, 6 STATUS */
#define PIF ((volatile uint32_t *)0xBFC007C0u)
static void mctl(uint32_t v) { __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(v)); }
static void si_idle(void) { for (uint32_t n = 0; n < 1000000u && (SI[6] & 1u); n++) { } }
static void test_si(void)
{
    lcg_s = 0x31337u;
    for (int blk = 0; blk < 2; blk++) {
        const uint32_t a = blk ? 0x7FFFC0u : 0x680040u;                        /* the last 64 bytes; 64 bytes above 4 MiB */
        volatile uint32_t *d = K1(a);
        for (int i = 0; i < 16; i++) d[i] = lcg();
        d[15] &= ~0xFFu;                                                       /* (the PIF's status byte, no run flag) */
        *K1(a - 0x400000u) = 0xA11A9000u + (uint32_t)blk;
        SI[0] = a; SI[4] = 0x1FC007C0u;                                        /* WR64B: RDRAM -> PIF RAM */
        si_idle();
        uint32_t bad = 0;
        for (int i = 0; i < 16; i++) if (PIF[i] != d[i]) bad++;
        check("E: WR64B from the RDRAM above 4 MiB", bad, 0u);
        const uint32_t b = blk ? 0x7F0000u : 0x6C0100u;
        for (int i = 0; i < 16; i++) *K1(b + 4u * i) = 0;
        SI[0] = b; SI[1] = 0x1FC007C0u;                                        /* RD64B: PIF RAM -> RDRAM */
        si_idle();
        bad = 0;
        for (int i = 0; i < 16; i++) if (*K1(b + 4u * i) != PIF[i]) bad++;
        check("E: RD64B into the RDRAM above 4 MiB", bad, 0u);
        check("E: the lower word untouched", *K1(a - 0x400000u), 0xA11A9000u + (uint32_t)blk);
    }
}

/* ---- F: the AI ----------------------------------------------------------------------------------- */
#define AI  ((volatile uint32_t *)0xA4500000u)
static void test_ai(void)
{
    apf_audio_buffer_flush_write(1);
    wait_cycles(1000);
    monitor_airate_write(0x10000u);                                            /* a frame a sample */
    apf_audio_playback_en_write(1);
    AI[3] = 0;
    const uint32_t a = 0x7FF000u, len = 0x800u;
    lcg_s = 1;                                                                 /* (ai_check.py --lcg's pattern) */
    for (uint32_t i = 0; i < len / 4; i++) *K1(a + 4u * i) = lcg();
    *K1(a - 0x400000u) = 0xA11AA000u;
    AI[2] = 1;
    AI[0] = a; AI[1] = len;
    for (uint32_t n = 0; n < 4000000u && AI[1] != 0u; n++) { }
    check("F: the buffer above 4 MiB played to its end", AI[1], 0u);
    AI[2] = 0;
    check("F: the lower word untouched", *K1(a - 0x400000u), 0xA11AA000u);
}

/* ---- G: the VI ----------------------------------------------------------------------------------- */
#define VI ((volatile uint32_t *)0xA4400000u)
static void test_vi(void)
{
    const uint32_t org = 0x640000u;
    /* 320 x 240, 16 bits: the left half red, the right half blue, a white row at the top and bottom */
    for (uint32_t y = 0; y < 240u; y++) for (uint32_t x = 0; x < 320u; x += 2) {
        uint16_t c = x < 160u ? RED : BLUE;
        if (y == 0 || y == 239u) c = 0xFFFFu;
        *K1(org + (y * 320u + x) * 2u) = (uint32_t)c << 16 | c;
    }
    VI[0] = 0x3202u; VI[2] = 320u; VI[5] = 0x03E52239u; VI[6] = 0x20Du; VI[7] = 0x0C15u; VI[8] = 0x0C150C15u;
    VI[9] = 0x006C02ECu; VI[10] = 0x002501FFu; VI[11] = 0x000E0204u; VI[12] = 0x200u; VI[13] = 0x400u;
    VI[1] = org;
    video_framebuffer_vtg_mode_write(1);
    video_framebuffer_width_write(0);
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    wait_cycles(4u * 1045440u);                                                /* a few fields */
    check("G: VI_ORIGIN reads back", VI[1] & 0x00FFFFFFu, org);
    puts_("G: VI_ORIGIN "); puthex(org); puts_(" set; see the bench's VITRACE and VGA output\n");
}

int main(void)
{
    puts_("ram8_test\n");
    uint32_t id = monitor_id_read();
    if ((id & 0x7Eu) != 0x7Eu) { puts_("no MI, PI, VI, AI, SI in this SoC\nram8_test: SKIPPED\n"); putc_(4); for (;;) ; }
    volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
    for (int i = 0; i < 8; i++) mr[i] = 0;
    monitor_pend_write(0x1F);
    monitor_mask_write(0u);
    monitor_miclr_write(0x3Eu);
    puts_("A\n"); test_cpu();
    puts_("B\n"); test_rsp();
    puts_("C\n"); test_rdp();
    monitor_ctl_write(0x1Du);                                                  /* OWN, MI, VI and AI the hardware's */
    mctl(0x300u);                                                              /* the monitor's accesses reach the RCP's registers */
    puts_("D\n"); test_pi();
    puts_("E\n"); test_si();
    puts_("F\n"); test_ai();
    puts_("G\n"); test_vi();
    check("no monitor entry", mr[2], 0u);
    puts_(fails ? "ram8_test: FAILED\n" : "ram8_test: PASS\n");
    putc_(4);
    for (;;);
}
