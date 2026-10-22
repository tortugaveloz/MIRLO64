/* n64rt on Mirlo's RSP (rtl/rsp, the SoC built with MIRLO_RSP): the game's
 * tasks run on its own microcode, as on the N64.
 *
 * A task is loaded the way libultra's osSpTaskLoad does it -- the header,
 * its pointers physical, by DMA to DMEM 0xFC0, the boot microcode by DMA to
 * IMEM, the PC at its start -- and started by clearing halt; its SP event
 * comes when the RSP stops at its BREAK (polled from the timer's tick). A
 * graphics task can be asked to yield (osSpTaskYield) and is resumed from
 * its yield data, as libultra does.
 *
 * The RDP (rtl/rdp) takes the commands the microcode leaves in RDRAM by
 * itself (the SoC's DPC registers and fetch); a graphics task's DP event comes
 * when its SYNC FULL has been through -- every write of the frame in memory.
 *
 * The AI: in hardware (rtl/soc/rcp_ai.sv, n64rt_io.c's n64rt_ai_hw) on the
 * SoC the runtime ships for. On a SoC without it, or in a build without
 * OWN=1, the game CPU feeds the Pocket's audio FIFO itself, from the timer's
 * tick -- the game's buffers (two queued, as the AI's), resampled from the
 * game's rate to the Pocket's 48 kHz, each frame read when the AI would play
 * it. */
#include <stdio.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include "n64rt.h"

/* ---- the RSP's registers (rtl/soc/mirlo_mips.sv, MIRLO_RSP) -------------------- */
#define RSP_BASE        0x20000000u
#define SP_MEM_ADDR     (*(volatile u32 *)(uintptr_t)(RSP_BASE + 0x00))
#define SP_DRAM_ADDR    (*(volatile u32 *)(uintptr_t)(RSP_BASE + 0x04))
#define SP_RD_LEN       (*(volatile u32 *)(uintptr_t)(RSP_BASE + 0x08))
#define SP_STATUS       (*(volatile u32 *)(uintptr_t)(RSP_BASE + 0x10))
#define SP_DMA_BUSY     (*(volatile u32 *)(uintptr_t)(RSP_BASE + 0x18))
#define SP_PC           (*(volatile u32 *)(uintptr_t)(RSP_BASE + 0x20))
#define SP_REG_DBG(o)   (*(volatile u32 *)(uintptr_t)(RSP_BASE + (o)))
/* SP_STATUS: what it reads, what a write does */
#define SP_ST_HALT      0x001u
#define SP_ST_BROKE     0x002u
#define SP_ST_YIELD     0x080u          /* signal 0 */
#define SP_ST_YIELDED   0x100u          /* signal 1 */
#define SP_CLR_HALT     0x00001u
#define SP_CLR_BROKE    0x00004u
#define SP_CLR_INTR     0x00008u
#define SP_CLR_SSTEP    0x00020u
#define SP_SET_INTBRK   0x00100u
#define SP_CLR_YIELD    0x00200u
#define SP_SET_YIELD    0x00400u
#define SP_CLR_YIELDED  0x00800u
#define SP_CLR_TASKDONE 0x02000u
#define OS_TASK_YIELDED  0x0001u
#define OS_TASK_DP_WAIT  0x0002u
#define OS_TASK_LOADABLE 0x0004u
#define OS_YIELD_DATA_SIZE 0x900u

static OSTask *s_hdr;                   /* the header as the RSP gets it: in RDRAM, for the DMA */
static int s_running;                   /* 0, or the type of the task on the RSP */
static int s_dp_wait;                   /* a graphics task ran to its end: its SYNC FULL is awaited */
static u32 s_sync_seen;                 /* the RDP's SYNC FULLs when that task began */
static u32 s_tasks, s_t0, s_sum, s_max;
/* A yield asked of a graphics task can be held back (n64rt_yield_wait_ms, a
 * port's to set; 0: yield at once, as the N64). A game yields its graphics
 * task for an audio task, and the frame then ends an audio task later -- the
 * RDP's heaviest work comes at the graphics task's end, and only starts to be
 * drawn then. Held back, a graphics task that ends within the wait is not
 * stopped (the game sees the yield it asked for lose the race, as it can on
 * the N64) and the audio task runs while the RDP draws; one that goes on is
 * yielded as asked. The audio task starts that much late: for a game whose
 * audio is a buffer or more ahead of the AI (Star Fox 64 renders the buffer
 * the AI gets two tasks later), not for one that queues the buffer its task
 * is still filling (Pokemon Stadium). */
u32 n64rt_yield_wait_ms __attribute__((weak)) = 0;
#define YIELD_WAIT (CONFIG_CLOCK_FREQUENCY / 1000u * n64rt_yield_wait_ms)
static u32 s_yield_at;                  /* 0, or when the yield was asked (| 1) */
static u32 s_yields, s_yields_kept;     /* asked; of them, not needed in the end */
static u32 s_freq = 32000, s_ai_full, s_ai_starved, s_ai_bufs;   /* the AI's (below): its rate; buffers refused
                                                                  * (both slots taken), runs dry, buffers taken */

static inline u32 cyc(void) { timer0_uptime_latch_write(1); return (u32)timer0_uptime_cycles_read(); }
static void dma_wait(void) { for (u32 n = 0; SP_DMA_BUSY && n < 10000000u; n++) ; }
static void dma_in(u32 sp, u32 rdram, u32 bytes)
{
    SP_MEM_ADDR = sp; SP_DRAM_ADDR = rdram; SP_RD_LEN = bytes - 1u;
    dma_wait();
}

static void rsp_start(OSTask *tp)
{
    /* (a task still on the RSP: the game's scheduler runs one at a time, so
     * this is only the tail of one whose event has not been polled yet) */
    for (u32 n = 0; s_running && !(SP_STATUS & SP_ST_HALT) && n < 200000000u; n++) ;
    OSTask_t *h = &s_hdr->t;
    *h = tp->t;
    h->ucode_boot  = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.ucode_boot);
    h->ucode       = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.ucode);
    h->ucode_data  = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.ucode_data);
    h->dram_stack  = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.dram_stack);
    h->output_buff = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.output_buff);
    h->output_buff_size = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.output_buff_size);
    h->data_ptr    = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.data_ptr);
    h->yield_data_ptr = (u64 *)(uintptr_t)N64RT_PHYS(tp->t.yield_data_ptr);
    if (tp->t.type == M_GFXTASK && !(h->flags & OS_TASK_YIELDED)) s_sync_seen = rdp_sync_count_read();
    if (h->flags & OS_TASK_YIELDED) {                /* a yielded task goes on from its yield data */
        h->ucode_data = h->yield_data_ptr;
        h->ucode_data_size = tp->t.yield_data_size;
        tp->t.flags &= ~OS_TASK_YIELDED;
        if (tp->t.flags & OS_TASK_LOADABLE)
            h->ucode = (u64 *)(uintptr_t)*(volatile u32 *)(uintptr_t)(N64RT_K1(tp->t.yield_data_ptr) + OS_YIELD_DATA_SIZE - 4u);   /* (the RSP wrote it) */
    }
    /* A CIC-6105 game's boot microcode (the Zeldas'). Its first run checks
     * what the console's own boot left in the RSP -- registers set by the PIF
     * ROM's code, which no cartridge holds -- and by the outcome sets or
     * clears signal 7 and writes two of its own words over its first two in
     * the game's copy (a jump past the check, for every task after; the other
     * outcome's jump leads to code that spoils the RDP's registers now and
     * then). The good outcome is given to it here: "lui at, 0x100; addi s0,
     * zero, <where the two words are>" is that path, in both variants. (The
     * RSP here could not do it itself either: its DMA out of IMEM reads
     * DMEM.) */
    {
        volatile u32 *boot = (volatile u32 *)(uintptr_t)N64RT_K0(N64RT_PHYS(tp->t.ucode_boot));
        u32 words = tp->t.ucode_boot_size / 4u;
        if (words > 0x58u) words = 0x58u;
        for (u32 i = 0; i + 1u < words; i++)
            if (boot[i] == 0x3C010100u && (boot[i + 1u] >> 16) == 0x2010u) {
                u32 src = ((boot[i + 1u] & 0xFFFFu) - 0x1000u) / 4u;
                if (src + 1u < words && boot[0] != boot[src]) {
                    boot[0] = boot[src]; boot[1] = boot[src + 1u];
                    SP_STATUS = 1u << 24;            /* set signal 7 */
                }
                break;
            }
    }
    SP_STATUS = SP_CLR_YIELD | SP_CLR_YIELDED | SP_CLR_TASKDONE | SP_SET_INTBRK;
    SP_PC = 0;
    dma_in(0x0FC0u, N64RT_PHYS(s_hdr), sizeof(OSTask_t));
    u32 bs = (tp->t.ucode_boot_size + 7u) & ~7u;
    dma_in(0x1000u, N64RT_PHYS(tp->t.ucode_boot), bs && bs <= 0x1000u ? bs : 0x1000u);
    s_running = (int)tp->t.type ? (int)tp->t.type : 3;
    s_yield_at = 0;
#ifdef N64RT_RSPDUMP
    ctrl_scratch_write(0xA3000000u | (u32)s_running);        /* (sim/mips_soc AIDUMP: a task starts) */
#endif
    s_t0 = cyc();
    SP_STATUS = SP_SET_INTBRK | SP_CLR_SSTEP | SP_CLR_BROKE | SP_CLR_HALT | SP_CLR_INTR;
}

/* the events due (bit OS_EVENT_SP, OS_EVENT_DP); interrupts off */
static u32 rsp_poll(void)
{
    u32 ev = 0;
    if (s_dp_wait && rdp_sync_count_read() != s_sync_seen) {     /* the frame is in memory */
        s_dp_wait = 0;
        ev |= 1u << OS_EVENT_DP;
#ifdef N64RT_DEBUG
        { static u32 s_n; if (s_n++ < 6u) n64rt_log("n64rt: the RDP's SYNC FULL (%lu), after its task\n", (unsigned long)rdp_sync_count_read()); }
#endif
    }
    if (!s_running) return ev;
    u32 st = SP_STATUS;
    if (!(st & SP_ST_HALT)) {
        if (s_yield_at && cyc() - s_yield_at >= YIELD_WAIT) { s_yield_at = 0; SP_STATUS = SP_SET_YIELD; }
        return ev;
    }
    if (s_yield_at) { s_yield_at = 0; s_yields_kept++; }
    u32 d = cyc() - s_t0;
    ev |= 1u << OS_EVENT_SP;
#ifdef N64RT_RSPDUMP
    ctrl_scratch_write(0xA4000000u | (u32)s_running | ((st & SP_ST_YIELDED) ? 0x100u : 0u));   /* (it ended, or yielded) */
#endif
#ifdef N64RT_DEBUG
    { static u32 s_n; if (s_running == M_GFXTASK && s_n++ < 6u)
        n64rt_log("n64rt: gfx task ends: SP_STATUS %04lx, DPC start %06lx end %06lx current %06lx status %04lx, SYNC FULLs %lu (were %lu)\n",
                  (unsigned long)st, (unsigned long)SP_REG_DBG(0x40), (unsigned long)SP_REG_DBG(0x44), (unsigned long)SP_REG_DBG(0x48), (unsigned long)SP_REG_DBG(0x4C),
                  (unsigned long)rdp_sync_count_read(), (unsigned long)s_sync_seen); }
#endif
    /* a graphics task that ran to its end: the RDP's SYNC FULL is its DP event */
    if (s_running == M_GFXTASK && !(st & SP_ST_YIELDED)) {
        if (rdp_sync_count_read() != s_sync_seen) ev |= 1u << OS_EVENT_DP;
        else s_dp_wait = 1;
    }
    s_running = 0;
    s_sum += d; if (d > s_max) s_max = d;
    if ((++s_tasks & 255u) == 0u) {
        const u32 us = CONFIG_CLOCK_FREQUENCY / 1000000u;
        n64rt_log("n64rt: RSP: %lu tasks, the last 256 %lu us each (longest %lu us); %lu yields asked, %lu not needed; AI %lu Hz, %lu buffers, %lu refused, %lu times dry\n",
                  (unsigned long)s_tasks, (unsigned long)(s_sum / 256u / us), (unsigned long)(s_max / us),
                  (unsigned long)s_yields, (unsigned long)s_yields_kept,
                  (unsigned long)s_freq, (unsigned long)s_ai_bufs, (unsigned long)s_ai_full, (unsigned long)s_ai_starved);
        s_sum = s_max = 0;
    }
    return ev;
}

/* (the exception handler, interrupts off) the SP interrupt: taken back, and
 * the task's event posted as the timer's poll would */
void n64rt_tasks_poll(void);
void n64rt_sp_irq(void)
{
    SP_STATUS = SP_CLR_INTR;
    { extern void n64rt_tasks_events(void); n64rt_tasks_events(); }      /* (the audio's feed is the tick's) */
}

/* ---- what n64rt_io.c's task path calls (as n64rt_gfx.c gives it in the earlier mode) ---- */
const int n64rt_gfx_sp_early = 0;
/* The game CPU at the RSP's memory (0x0400_0000: DMEM, IMEM), a word at a
 * time: by DMA through a word of RDRAM, the RSP halted. For the few games
 * that look there themselves -- a CIC-6105 game runs a small microcode over
 * what IPL3 left in DMEM and reads its answer back. */
#ifdef N64RT_OWN
/* The two words of RDRAM the DMA goes through are the game's own, saved and put back around each use (KSEG1: the DMA is
 * behind the D-cache): the runtime keeps nothing in the game's RDRAM, so that any size of it, up to 8 MiB, and an IPL3
 * copy at 0 (CIC 6103) coexist with it. */
#define SPW_PA 0x200u
#define s_spw ((volatile u32 *)(uintptr_t)N64RT_K1(SPW_PA))
#else
static volatile u32 *s_spw;             /* two words of RDRAM (KSEG1: the DMA is behind the D-cache) */
#endif
int n64rt_sp_mem(u32 off, u32 *v, int write)
{
    if (!s_spw || !(SP_STATUS & SP_ST_HALT)) return 0;
#ifdef N64RT_OWN
    const u32 keep0 = s_spw[0], keep1 = s_spw[1];
#endif
    u32 m; __asm__ volatile("mfc0 %0, $12" : "=r"(m)); __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(m & ~1u));
    u32 a = off & 0x1FF8u, i = (off >> 2) & 1u;
    /* The game's SP_MEM_ADDR and SP_DRAM_ADDR are what the SP registers say: a read of DMEM here is a DMA, which moves both. A
     * game that does a DMA and then looks at its registers (n64-systemtest: "SP-Address after DMA", "SP sp-address") must find its
     * own DMA's, so they are put back after. (SP_RD_LEN and SP_WR_LEN read the same after any DMA: the count run out, 0xFF8 --
     * but for the skip, which this DMA's is 0: the one thing the hardware's registers do not let be restored, a write to a length
     * starts a DMA.) A DMA of the game's still under way is waited for first: a write to the registers while one runs corrupts it. */
    dma_wait();
#ifdef N64RT_OWN
    const u32 keep_mem = SP_MEM_ADDR, keep_dram = SP_DRAM_ADDR;
#endif
    SP_MEM_ADDR = a; SP_DRAM_ADDR = N64RT_PHYS(s_spw); SP_REG_DBG(0x0C) = 7u;       /* DMEM / IMEM -> RDRAM */
    dma_wait();
    if (write) {
        s_spw[i] = *v;
        dma_in(a, N64RT_PHYS(s_spw), 8u);
    } else *v = s_spw[i];
#ifdef N64RT_OWN
    s_spw[0] = keep0; s_spw[1] = keep1;
    SP_MEM_ADDR = keep_mem; SP_DRAM_ADDR = keep_dram;
#endif
    __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(m));
    return 1;
}
/* What a CIC-6105 boot leaves in the RSP's registers. The PIF's code runs a program on the RSP (IPL3 starts it and goes on) that
 * ends with a few registers holding words of IPL3 and a checksum in a vector register; the same boot microcode every 6105 game
 * has, run at the start of its first graphics task, compares them (r4, r5, r6, r11 and the sum of v13's eight lanes) and DMEM's
 * first halfword with what a console has, and by the outcome sets or clears signal 7 and writes two of its own words over its
 * first two in the game's copy: a jump past the check for every task after, or -- the outcome of an RSP that was not so left,
 * as this one is after reset -- a jump to code that, at every task's start, writes the RDP's DPC_END with what it read of
 * DPC_CLOCK (0 for the RSP here) and so spoils a list the RDP is still taking. The same state is left here by a program of
 * its own, run once: r4 = IPL3's first word (ROM 0x40), r6 an `addiu t1, t1, 4` of it (ROM 0x520), r5 an address in
 * 0x4C000..0x4FFFF, r11 0x3D8, v13's lanes adding up to -7510. */
static void rsp_ipl_state(volatile u32 *t)
{
    static const u32 prog[] = {
        0x3C0403A0u, 0x34844820u,          /* lui r4, 0x03A0; ori r4, r4, 0x4820 */
        0x3C050004u, 0x34A5C000u,          /* lui r5, 0x0004; ori r5, r5, 0xC000 */
        0x3C062529u, 0x34C60004u,          /* lui r6, 0x2529; ori r6, r6, 0x0004 */
        0x340B03D8u,                       /* ori r11, zero, 0x03D8 */
        0x3401E2AAu,                       /* ori r1, zero, 0xE2AA */
        0x48816800u,                       /* mtc2 r1, v13[0] */
        0x34010000u,                       /* ori r1, zero, 0 */
        0x0000000Du, 0u };                 /* break */
    const u32 n = sizeof prog / sizeof prog[0];
    for (u32 k = 0; k < n; k++) t[k] = prog[k];
    dma_in(0x1000u, N64RT_PHYS(t), n * 4u);
    SP_PC = 0u;
    SP_STATUS = SP_CLR_BROKE | SP_CLR_SSTEP | SP_CLR_HALT;
    for (u32 i = 0; !(SP_STATUS & SP_ST_HALT) && i < 1000000u; i++) ;
    SP_STATUS = SP_CLR_BROKE;
    for (u32 k = 0; k < n; k++) t[k] = 0u;          /* (IMEM as it was: nothing in it) */
    dma_in(0x1000u, N64RT_PHYS(t), n * 4u);
    SP_PC = 0u;
}
void n64rt_gfx_init(void)
{
#ifndef N64RT_OWN
    s_spw = (volatile u32 *)(uintptr_t)N64RT_K1(n64rt_rdram_alloc(8));
#endif
    /* DMEM as IPL3 leaves it: itself, from the cartridge's 0x40 on (the PIF's
     * ROM put it there to run it), unless it wipes it (below) */
    if (n64rt_game && n64rt_game->rom_size >= 0x1000u) {
        volatile u32 *t = (volatile u32 *)(uintptr_t)N64RT_K1(n64rt_game->mem_size > 0x2000u ? n64rt_game->mem_size - 0x1000u : 0x1000u);
        u32 keep[0x400];
        /* (the 6103's and the 6106's IPL3 end by filling both memories with
         * 0xFFFFFFFF and writing their osCicId into IMEM's first word: games
         * look at the first words of DMEM and IMEM -- one copies its RSP boot
         * code out of the cartridge only if they are so, and without that
         * code its first task never ends; another, finding anything else,
         * never starts its game) */
        const u32 fill = n64rt_game->cic == 6103u || n64rt_game->cic == 6106u;
        for (u32 k = 0; k < 0x400u; k++) { keep[k] = t[k]; t[k] = fill ? 0xFFFFFFFFu : k >= 0x10u || n64rt_game->cic == 6105u ? n64rt_rom_word(4u * k) : 0u; }
        dma_in(0u, N64RT_PHYS(t), 0x1000u);
        if (fill) { t[0] = n64rt_cic_id(n64rt_game->cic); dma_in(0x1000u, N64RT_PHYS(t), 0x1000u); }
        if (n64rt_game->cic == 6105u) rsp_ipl_state(t);
        for (u32 k = 0; k < 0x400u; k++) t[k] = keep[k];
    }
#ifndef N64RT_OWN
    s_hdr = n64rt_rdram_alloc(sizeof(OSTask));
#endif
    n64rt_log("n64rt: RSP: SP_STATUS %04lx (the game's own microcode runs its tasks)\n", (unsigned long)SP_STATUS);
}
void n64rt_gfx_task(OSTask *t) { rsp_start(t); }
int  n64rt_audio_task(OSTask *t) { rsp_start(t); return 1; }
u32  n64rt_gfx_poll(void) { return rsp_poll(); }
int  n64rt_gfx_busy(void) { return s_running != 0 || s_dp_wait; }

/* The cartridge's DMA into RDRAM, held back while the RDP has a frame to
 * draw. The N64's PI moves a 256-byte piece in tens of microseconds, so
 * whatever the RDP was told before the DMA began is long in memory when the
 * DMA's data arrives; here the copy takes a few microseconds, and a frame
 * the RSP has just ended (the RDP draws for another millisecond or two) lands
 * over data the game had already DMA'd into the same memory. Star Fox 64
 * reads a compressed file into its frame buffers (which the frame just
 * drawn is still being written into) and decompresses it from there: a few
 * thousand words of its flag and link tables were overwritten with the clear
 * colour, the decompressor ran on past its end over the audio heap, and the
 * audio thread looped on zeros for good. Read from the hardware, not from
 * s_running / s_dp_wait: their events are the poll's to deliver. Bounded: an
 * RSP that does not halt must not hold the game for ever. */
static int gfx_pending(void)
{
    if (s_running == M_GFXTASK && !(SP_STATUS & SP_ST_HALT)) return 1;      /* the RSP still writes the RDP's commands */
    /* The DPC has commands to fetch: DPC_STATUS' DMA busy (bit 8: CURRENT short of END) or END valid (bit 9: a second list
     * queued). Not bits 3..6: since 118640e the status is the console's, whose START_GCLK (3) and PIPE_BUSY (5) are set by an END
     * write and stay until the RDP completes a SYNC FULL -- a list the game ends without one (1080 Snowboarding's loading
     * phase) left them set for ever, and every PI DMA into the frame buffers waited its 50 ms. What the RDP draws is the
     * line below's. */
    if (SP_REG_DBG(0x4C) & 0x300u) return 1;
    return !rdp_status_idle_read();                                         /* the RDP draws */
}
void n64rt_gfx_quiesce(void)
{
    u32 t0 = cyc();
    while (gfx_pending() && cyc() - t0 < CONFIG_CLOCK_FREQUENCY / 20u) ;      /* (50 ms) */
}

void n64rt_osSpTaskYield(void)
{
    s_yields++;
    if (s_running == M_GFXTASK && n64rt_yield_wait_ms) { if (!s_yield_at) s_yield_at = cyc() | 1u; }
    else SP_STATUS = SP_SET_YIELD;
}
u32 n64rt_osSpTaskYielded(OSTask *tp)
{
    u32 st = SP_STATUS, r = (st & SP_ST_YIELDED) ? OS_TASK_YIELDED : 0u;
    if (st & SP_ST_YIELD) {
        tp->t.flags |= r;
        tp->t.flags &= ~OS_TASK_DP_WAIT;
    }
    return r;
}

/* ---- the AI ------------------------------------------------------------------- */
#define VI_NTSC_CLOCK n64rt_vi_clock()     /* (the console's: PAL 49,656,530, NTSC 48,681,812, MPAL 48,628,316) */
#define FIFO_TARGET   512u              /* 48 kHz samples kept in the Pocket's FIFO (~10.7 ms) */
static u32 s_cur, s_cur_left, s_next, s_next_len;   /* the buffer being played (K0 address, bytes left), the one queued */
static u32 s_step;                      /* source frames per output sample, Q16 */
static u32 s_phase;
static s32 s_pl, s_pr, s_nl, s_nr;
static int s_have, s_ai_on;
static u32 s_remain;                    /* what osAiGetLength gives */

static u32 s_ai_done;                   /* buffers played to their end (an AI interrupt each, as the N64's) */
static int s_cur_active;
u32 n64rt_ai_buffers_done(void) { u32 n = s_ai_done; s_ai_done = 0; return n; }
static int fetch_frame(void)
{
    if (s_cur_left < 4u) {
        if (s_cur_active) { s_cur_active = 0; s_ai_done++; }
        if (!s_next) { s_cur_left = 0; return 0; }
        s_cur = s_next; s_cur_left = s_next_len & ~3u; s_next = 0;
        if (s_cur_left < 4u) return 0;
        s_cur_active = 1;
    }
    /* the N64's frame: L, then R. Read past the D-cache (KSEG1): the RSP wrote
     * it by DMA, and a line of this buffer still cached from its last turn
     * would give that turn's samples -- eight frames of old sound at a time */
    u32 w = *(volatile u32 *)(uintptr_t)N64RT_K1(s_cur);
    s_cur += 4u; s_cur_left -= 4u;
    s_pl = s_nl; s_pr = s_nr;
    s_nl = (s16)(w >> 16); s_nr = (s16)w;
    return 1;
}
/* The feeder plays a frame when the N64's AI would, not before: a game may
 * hand a buffer over while the task that fills it is still running (Pokemon
 * Stadium starts its audio task and gives the AI that task's buffer 75 us
 * later; the AI gets to it a few ms on, when the buffer before it runs out).
 * So the Pocket's FIFO is kept at FIFO_TARGET by time, not by reading ahead:
 * silence goes in first when the sound starts, and while the AI has run dry. */
static int s_started;                   /* the FIFO has its lead */
static u32 s_dry;                       /* samples of silence since the AI ran dry */
static void ai_service(void)
{
    u32 fill = apf_audio_buffer_fill_read();
    if (!s_started) {
        if (s_cur_left < 4u && !s_next) { s_ai_on = fill != 0; return; }
        for (; fill < FIFO_TARGET; fill++) apf_audio_out_write(0);
        s_started = 1; s_dry = 0;
    }
    while (fill < FIFO_TARGET) {
        if (!s_have) {                               /* two frames to start between */
            if (!fetch_frame()) goto dry;
            s_have = 1; s_phase = 0x10000u;
        }
        /* on to the pair of frames this output sample falls between */
        while (s_phase >= 0x10000u) {
            if (!fetch_frame()) goto dry;
            s_phase -= 0x10000u;
        }
        {
            s32 ph = (s32)(s_phase >> 1);            /* Q15: a 17-bit difference times it fits 32 bits */
            s32 l = s_pl + (((s_nl - s_pl) * ph) >> 15), r = s_pr + (((s_nr - s_pr) * ph) >> 15);
            apf_audio_out_write((u32)(u16)l << 16 | (u16)r);
        }
        s_phase += s_step;
        s_dry = 0;
        fill++;
        continue;
    dry:
        /* the AI is out of buffers: silence, as it would give -- until it has
         * been so for a while, then nothing (the next sound starts over) */
        if (s_dry == 0) s_ai_starved++;
        if (++s_dry > 48000u / 8u) { s_started = 0; s_have = 0; break; }
        apf_audio_out_write(0);
        fill++;
    }
    s_remain = s_cur_left;                           /* AI_LEN: what is left of the buffer being played */
    s_ai_on = s_started || s_cur_left || s_next || fill;
}

void n64rt_audio_init(void)
{
    s_step = (u32)(((u64)s_freq << 16) / 48000u);
    apf_audio_buffer_flush_write(1);
    apf_audio_playback_en_write(1);
}
/* the tick's poll (n64rt_io.c: with the graphics one): the AI is kept fed here */
#ifdef N64RT_OWN
u32 n64rt_ai_hw(void);
#else
static inline u32 n64rt_ai_hw(void) { return 0; }     /* (the AI in hardware is the OWN runtime's: a legacy build feeds it in software) */
#endif
u32 n64rt_audio_poll(void)
{
    if (n64rt_ai_hw()) return 0;                     /* (the AI in hardware: nothing to feed) */
    ai_service();
    return 0;
}
int n64rt_audio_busy(void) { return s_ai_on && !n64rt_ai_hw(); }

s32 n64rt_osAiSetFrequency(u32 f)
{
    if (!f) return -1;
    u32 dac = (VI_NTSC_CLOCK + f / 2u) / f;          /* libultra: the DAC rate, rounded */
    if (dac < 132u) return -1;
    s_freq = VI_NTSC_CLOCK / dac;
    s_step = (u32)(((u64)s_freq << 16) / 48000u);
    if (n64rt_ai_hw()) monitor_airate_write(s_step);   /* (the hardware's resampler: MONITOR.AIRATE, Q16 frames a sample) */
    return (s32)s_freq;
}
/* The AI in hardware (n64rt_io.c): AI_DACRATE was written -- the rate and the step as the software AI took them from
 * osAiSetFrequency(osViClock / (DACRATE + 1)); and at start, the default rate (the registers are those of the last game). */
void n64rt_ai_hw_event(void)
{
    (void)n64rt_osAiSetFrequency(VI_NTSC_CLOCK / ((monitor_airate_read() & 0x3FFFu) + 1u));
}
void n64rt_ai_hw_reset(void)
{
    s_freq = 32000;
    s_step = (u32)(((u64)s_freq << 16) / 48000u);
    monitor_airate_write(s_step);
}
s32 n64rt_osAiSetNextBuffer(void *buf, u32 size)
{
    if (s_next) { s_ai_full++; return -1; }          /* both slots taken */
    s_ai_bufs++;
#ifdef N64RT_RSPDUMP
    /* (sim/mips_soc AIDUMP=<file>: the buffer as the game hands it over) */
    ctrl_scratch_write(0xA1000000u | (N64RT_PHYS(buf) & 0x7FFFFFu));
    ctrl_scratch_write(0xA2000000u | (size & 0xFFFFFFu));
#endif
    s_next_len = size;
    s_next = N64RT_K0(N64RT_PHYS(buf));
    s_ai_on = 1;                                     /* (the next tick takes it, and polls from then on) */
    return 0;
}
u32 n64rt_osAiGetLength(void) { return s_remain; }
u32 n64rt_osAiGetStatus(void) { return (s_next ? 0x80000000u : 0u) | (s_remain ? 0x40000000u : 0u); }
