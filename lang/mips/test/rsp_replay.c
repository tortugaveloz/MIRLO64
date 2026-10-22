/* One RSP task, replayed on the whole SoC (sim/mips_soc built with MIRLO_RSP):
 * a task dump (the RDRAM and the task header as a game left them when the
 * task began) is put in SDRAM by the bench, and this starts it the way the
 * runtime does and times it -- the RSP to its BREAK, the RDP to its SYNC FULL
 * -- with the scan-out reading the frame and, in turn, the game CPU idle and
 * busy in memory at several rates. What the blocks cost each other on the one
 * SDRAM, which none of their own benches sees.
 *
 *   PRELOAD=task.bin@0x437FFFF0 tb_soc build/rsp_replay.bin -c 60
 *   (the dump's bytes 8..11: the colour image's address, for the scan-out;
 *    12..15: the number of walker rates to run, 0 for all) */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

#define RDRAM      0x43800000u
#define DUMP_HDR   ((volatile uint32_t *)(uintptr_t)(RDRAM - 16u))
#define RSP_BASE   0x20000000u
#define SP_REG(o)  (*(volatile uint32_t *)(uintptr_t)(RSP_BASE + (o)))
#define SP_MEM_ADDR SP_REG(0x00)
#define SP_DRAM_ADDR SP_REG(0x04)
#define SP_RD_LEN  SP_REG(0x08)
#define SP_STATUS  SP_REG(0x10)
#define SP_DMA_BUSY SP_REG(0x18)
#define SP_PC      SP_REG(0x20)

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putu(uint32_t v)
{
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) putc_(b[--n]);
}
static inline uint32_t cyc(void) { timer0_uptime_latch_write(1); return (uint32_t)timer0_uptime_cycles_read(); }
static void dma_in(uint32_t sp, uint32_t rdram, uint32_t bytes)
{
    SP_MEM_ADDR = sp; SP_DRAM_ADDR = rdram; SP_RD_LEN = bytes - 1u;
    while (SP_DMA_BUSY) ;
}
void trap_handler(void) { puts_("TRAP\n"); for (;;) ; }

/* the walker: a word read anywhere in RDRAM (a cache miss: a line from the
 * SDRAM), one in four followed by a write to a scratch megabyte, then `gap`
 * turns of nothing */
static uint32_t s_x = 0x2545F491u;
static inline uint32_t rnd(void) { s_x ^= s_x << 13; s_x ^= s_x >> 17; s_x ^= s_x << 5; return s_x; }
static volatile uint32_t s_sink;

static void run(uint32_t task, int gap)
{
    volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)(RDRAM + task);
    static const uint8_t ptrs[] = { 2, 4, 6, 8, 10, 11, 12, 14 };
    for (unsigned i = 0; i < sizeof ptrs; i++) h[ptrs[i]] &= 0x007FFFFFu;
    uint32_t sync0 = rdp_sync_count_read();
    SP_STATUS = 0x200u | 0x800u | 0x2000u | 0x100u;     /* clear yield, yielded, task done; interrupt on break */
    SP_PC = 0;
    dma_in(0x0FC0u, task, 64u);
    uint32_t bs = (h[3] + 7u) & ~7u;
    dma_in(0x1000u, h[2], bs && bs <= 0x1000u ? bs : 0x1000u);
    ctrl_scratch_write(0xA5000000u);                    /* (sim/mips_soc MEMSTAT: a run from here) */
    uint32_t t0 = cyc(), t_sp = 0, t_dp = h[0] == 1u ? 0u : 1u, n = 0;     /* (only a graphics task has the RDP's end to wait for) */
    SP_STATUS = 0x100u | 0x20u | 0x4u | 0x1u | 0x8u;    /* clear single step, broke, halt, interrupt */
    while (!t_sp || !t_dp) {
        if (!t_sp && (SP_STATUS & 1u)) t_sp = cyc() - t0;
        if (!t_dp && rdp_sync_count_read() != sync0) t_dp = cyc() - t0;
        if (gap < 0) continue;
        for (int k = 0; k < 8; k++) {
            uint32_t r = rnd();
            s_sink += *(volatile uint32_t *)(uintptr_t)(RDRAM + (r & 0x007FFFFCu));
            if ((r >> 30) == 0u) *(volatile uint32_t *)(uintptr_t)(0x41000000u + ((r >> 4) & 0x000FFFFCu)) = r;
            for (volatile int g = 0; g < gap; g++) ;
            n++;
        }
    }
    ctrl_scratch_write(0xA6000000u);                    /* (to here) */
    const uint32_t per = CONFIG_CLOCK_FREQUENCY / 10000u;       /* cycles in 0.1 ms */
    puts_("REPLAY gap "); if (gap < 0) puts_("idle"); else putu((uint32_t)gap);
    puts_(": RSP "); putu(t_sp / per); puts_(" x0.1ms, RDP done "); putu(t_dp / per);
    puts_(" x0.1ms, CPU accesses "); putu(n); puts_(" (");
    putu(t_dp > t_sp ? n / (t_dp / per / 10u + 1u) : n / (t_sp / per / 10u + 1u)); puts_(" /ms)\n");
}

int main(void)
{
    uint32_t task = DUMP_HDR[1], fb = DUMP_HDR[2], nr = DUMP_HDR[3];
    puts_("rsp_replay: task header at "); putu(task); puts_(", colour image "); putu(fb); puts_("\n");
    video_framebuffer_vtg_mode_write(1);                 /* 320 x 240, the game's own framebuffer */
    video_framebuffer_width_write(0);
    video_framebuffer_dma_base_write(RDRAM + fb);
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    static const int gaps[] = { -1, 256, 64, 16, 4, 0 };
    if (!nr || nr > sizeof gaps / sizeof gaps[0]) nr = sizeof gaps / sizeof gaps[0];
    for (uint32_t i = 0; i < nr; i++) run(task, gaps[i]);
    puts_("REPLAY DONE\n");
    for (;;) ;
}
