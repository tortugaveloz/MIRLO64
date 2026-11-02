/* n64rt: the core's "Restart Game" action (interact.json: variable 251, INTERACT1[21:20]) -- the N64 game starts over, as if the console were
 * switched off and on, while the Pocket's data slots stay as they are: the ROM, the saves, the Transfer Paks' ROMs and their cartridges.
 *
 * A console that is turned off and on again has its Transfer Pak asked for its cartridge from the start. A game that looks at a pak
 * once, a moment after it boots, and never again, finds a cartridge only if it was already in; the Pocket loads the Game Boy ROM
 * ("Load Transfer Pak 1") when the game is running, so the game has to start after that. This is how: the Pocket writes the
 * action's value (1) to the field when it is chosen, the runtime sees it at its next tick (4 ms, n64rt_restart_poll), clears it (so
 * that the next choice is a change again, whatever the Pocket sends: it sends the same value every time), and then
 *
 *   1. writes back what is dirty: the Game Boy cartridges' RAM (the Game Boy game's own save file, n64rt_gbv_restart_prep, which
 *      also makes a cartridge of a ROM just loaded, and powers each cartridge off and on); the N64 game's own save, its Controller
 *      Pak and the Pocket's table of their file sizes are the Pocket's and stay as they are;
 *   2. lets the hardware come to rest: the RSP halted, its DMA and the RDP's list done, the PI's engine and the SI idle, the
 *      AI's buffers played out, at the top rate and unheard (ai_drain; n64rt_hw_quiesce, as a save state does), none of the monitor's wake-ups on;
 *   3. puts the RCP's registers back to the values the SoC's reset gives them (hw_reset), and the game CPU's (cpu_reset): the CPU's
 *      TLB, Compare, Cause, the monitor level's modes (MCTL);
 *   4. leaves the monitor for the program's start (n64rt_restart_go): the stack at the boot stack, Status as the start sets it. The
 *      program's .data is put back as it was when the program started (a copy made then, n64rt_restart_snapshot), its .bss is zeroed
 *      as at the start, and main() runs -- the loader, n64rt_run, the game's boot as IPL3 does it -- on the ROM the Pocket put in the SDRAM,
 *      which is still there. Only what is named below is kept: the Transfer Paks' cartridges (n64rt_gbv_keep) and a test build's trace.
 *
 * Not reset, because software cannot: the SI's "block holds commands" flag, the VI's "V_SYNC has been written" (the TV system the
 * game chose stays until it writes its own), the RDP's tile and mode registers and TMEM (a display list sets them), the SoC's uptime
 * counter and the game CPU's Count (a console's Count after a reset is a boot's long). docs/mips.md ("Restart Game") has the list. */
#include <stdio.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
#include "n64rt.h"

#ifdef N64RT_GBTRACE
void n64rt_trace_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *n64rt_trace_keep(u32 *bytes);
#define RSLOG(...) do { n64rt_log(__VA_ARGS__); n64rt_trace_log(__VA_ARGS__); } while (0)
#else
#define RSLOG(...) n64rt_log(__VA_ARGS__)
#endif

extern char _fdata[], _edata[], _fbss[], _ebss[];
int main(void);
void n64rt_restart_go(void) __attribute__((noreturn));      /* (below) */
void n64rt_restart_boot(void) __attribute__((noreturn));
void *n64rt_gbv_keep(u32 *bytes);
void n64rt_gbv_restart_prep(void);
int n64rt_hw_quiesce(int force);                            /* n64rt_state.c */
int n64rt_state_busy(void);
void n64rt_rcp_events(void);                                /* n64rt_io.c */

u32 n64rt_restarted;                    /* .bss: 0 when the program started on its own, the count of restarts when this run is one's */

/* ---- the program's .data as it started -------------------------------------------------------------------------
 * SDRAM that nothing else uses (above the save states' scratch, below the second Transfer Pak's ROM; layout.h): a header and the words. */
#define RS_AREA     (LAYOUT_STATE + 0x00AD0000u)
#define RS_MAGIC    0x52535452u         /* "RSTR" */
#define RS_DATA     0x100u
#define RS_DATA_MAX 0x1F00u
typedef struct { u32 magic, bytes, sum, count; } rs_head_t;
_Static_assert(RS_AREA >= LAYOUT_STATE + 0x00AC0000u + 0x1000u && RS_AREA + 0x2000u <= LAYOUT_GBROM2, "the restart's copy lies between the save states' scratch and the second ROM");
static rs_head_t *rs_head(void) { return (rs_head_t *)(uintptr_t)RS_AREA; }
static u32 data_sum(const u32 *p, u32 bytes)
{
    u32 s = 0x52535452u;
    for (u32 i = 0; i < bytes / 4u; i++) s = (s << 5 | s >> 27) ^ p[i];
    return s;
}
/* main(), at a start of the program that is not a restart's */
void n64rt_restart_snapshot(void)
{
    const u32 bytes = (u32)(_edata - _fdata);
    rs_head_t *h = rs_head();
    h->magic = 0;
    if (bytes > RS_DATA_MAX) return;                    /* (a restart is then refused) */
    memcpy((void *)(uintptr_t)(RS_AREA + RS_DATA), _fdata, bytes);
    h->bytes = bytes; h->count = 0;
    h->sum = data_sum((const u32 *)(uintptr_t)(RS_AREA + RS_DATA), bytes);
    h->magic = RS_MAGIC;
    flush_cpu_dcache();
}
static int snapshot_ok(void)
{
    flush_cpu_dcache();
    const rs_head_t *h = rs_head();
    return h->magic == RS_MAGIC && h->bytes == (u32)(_edata - _fdata) &&
           h->sum == data_sum((const u32 *)(uintptr_t)(RS_AREA + RS_DATA), h->bytes);
}

/* ---- the state a restart keeps ------------------------------------------------------------------------------- */
typedef void *(*keep_fn)(u32 *bytes);
static const keep_fn s_keepers[] = {
    n64rt_gbv_keep,
#ifdef N64RT_GBTRACE
    n64rt_trace_keep,
#endif
};
#define NKEEP (sizeof s_keepers / sizeof s_keepers[0])
#define KEEP_MAX 2048u
static u32 keep_bytes(void)
{
    u32 total = 0;
    for (u32 k = 0; k < NKEEP; k++) { u32 n; (void)s_keepers[k](&n); total += (n + 3u) & ~3u; }
    return total;
}

/* ---- the registers ------------------------------------------------------------------------------------------- */
#define RSP(o)          (*(volatile u32 *)(uintptr_t)(0x20000000u + (o)))
#define SP_MEM_ADDR     RSP(0x00)
#define SP_DRAM_ADDR    RSP(0x04)
#define SP_RD_LEN       RSP(0x08)
#define SP_STATUS       RSP(0x10)
#define SP_DMA_FULL     RSP(0x14)
#define SP_DMA_BUSY     RSP(0x18)
#define SP_SEMAPHORE    RSP(0x1C)
#define SP_PC           RSP(0x20)
#define DPC_START       RSP(0x40)
#define DPC_END         RSP(0x44)
#define DPC_STATUS      RSP(0x4C)
#define MI_MODE         0x04300000u
#define MI_MASK         0x0430000Cu
#define VI_REG(n)       (0x04400000u + 4u * (n))
#define VI_BASE         0x04400038u     /* (rcp_vi.sv: the runtime's) */
#define AI_DRAM_ADDR    0x04500000u
#define AI_CONTROL      0x04500008u
#define AI_STATUS       0x0450000Cu
#define AI_DACRATE      0x04500010u
#define SI_DRAM_ADDR    0x04800000u
#define SI_STATUS       0x04800018u
#define PIF_RAM         0x1FC007C0u
static inline u32 hw_rd(u32 pa) { return *(volatile u32 *)(uintptr_t)(0xA0000000u | pa); }
static inline void hw_wr(u32 pa, u32 v) { *(volatile u32 *)(uintptr_t)(0xA0000000u | pa) = v; }
#define MFC0(r)     ({ u32 _v; __asm__ volatile("mfc0 %0, $" #r : "=r"(_v)); _v; })
#define MTC0(r, v)  __asm__ volatile("mtc0 %0, $" #r "\n\tnop\n\tnop" :: "r"((u32)(v)))
#define TLBWI()     __asm__ volatile("tlbwi\n\tnop\n\tnop")
static inline u64 uptime(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
#define MS(n) ((u64)(n) * (CONFIG_CLOCK_FREQUENCY / 1000u))

/* the RSP's DMA, DMEM (0) and IMEM (0x1000) to or from the RDRAM's first bytes */
static void sp_dma(u32 sp, u32 rdram, u32 bytes, int to_rdram)
{
    while (SP_DMA_FULL || SP_DMA_BUSY) { }
    SP_MEM_ADDR = sp; SP_DRAM_ADDR = rdram;
    if (to_rdram) RSP(0x0C) = bytes - 1u; else SP_RD_LEN = bytes - 1u;
    while (SP_DMA_FULL || SP_DMA_BUSY) { }
}

/* The RCP's registers as the SoC's reset leaves them (the values are rtl/soc's: rcp_mi.sv, rcp_vi.sv, rcp_ai.sv, rcp_si.sv,
 * rdp_dpc.sv, rsp_top.sv, mirlo_regs.sv). The RSP is halted, the RDP idle, the PI's engine idle and the AI's queue played when this
 * runs (n64rt_hw_quiesce); a register that something else keeps (the PEND bits the changes below raise) is cleared at the end. */
static void hw_reset(void)
{
    /* the monitor level: no wake-up, no interrupt line to the game */
    monitor_mask_write(0);
    monitor_ctl_write(0);
    /* the RSP: halted, no break, no interrupt, no single step, no interrupt on a break, the signals clear, PC 0, the semaphore free; IMEM zero
     * (DMEM is written whole by the boot: n64rt_gfx_init) */
    SP_STATUS = 0x2u | 0x4u | 0x8u | 0x20u | 0x80u | 0x00AAAA00u;
    SP_SEMAPHORE = 0;
    SP_PC = 0;
    memset((void *)(uintptr_t)LAYOUT_RDRAM, 0, 0x1000u);
    flush_cpu_dcache_range((const void *)(uintptr_t)LAYOUT_RDRAM, 0x1000u);
    sp_dma(0x1000u, 0x000000u, 0x1000u, 0);
    SP_MEM_ADDR = 0; SP_DRAM_ADDR = 0;
    /* the RDP: no XBUS, not frozen, no flush; START and END at 0 (it is idle: CURRENT follows) */
    DPC_STATUS = 0x1u | 0x4u | 0x10u;
    DPC_START = 0;
    DPC_END = 0;
    /* (the status the console shows has two bits, START_GCLK and PIPE_BUSY, which a write of END and an unfreeze set and only a SYNC FULL
     * clears: rcp_dpc_view.sv. One goes through the RDP's command port; the DP interrupt it raises is cleared below) */
    rdp_cmd_data_write(0x29000000u); rdp_cmd_data_write(0u);
    for (u32 n = 0; !rdp_status_idle_read() && n < 1000000u; n++) { }
    /* the MI: the mode bits clear (init, EBus test, upper mode), the DP interrupt cleared, every interrupt masked, none pending */
    hw_wr(MI_MODE, 0x80u | 0x200u | 0x800u | 0x1000u);
    hw_wr(MI_MASK, 0x555u);
    monitor_miclr_write(0x3Eu);
    /* the VI: every register 0 (WIDTH first, so that the origin is taken as it is written), the origin and the field, the interrupt */
    hw_wr(VI_REG(0), 0);
    hw_wr(VI_REG(2), 0);
    for (u32 n = 1; n <= 13u; n++) if (n != 2u && n != 4u) hw_wr(VI_REG(n), 0);
    hw_wr(VI_BASE, 0);
    hw_wr(VI_REG(4), 0);                                /* (any write clears the VI interrupt) */
    /* the AI: off, no address, the rate 0, the interrupt cleared. Its queue was played (quiesce): the next buffer starts it */
    hw_wr(AI_CONTROL, 0);
    hw_wr(AI_DRAM_ADDR, 0);
    hw_wr(AI_DACRATE, 0);
    hw_wr(AI_STATUS, 0);
    /* the SI: no address, its interrupt and error cleared, the PIF RAM zero */
    hw_wr(SI_DRAM_ADDR, 0);
    hw_wr(SI_STATUS, 0);
    for (u32 n = 0; n < 16u; n++) hw_wr(PIF_RAM + 4u * n, 0);
    /* the monitor's own: domain 2 is no FlashRAM, the VI's threshold open, the game's pads as the boot finds the settings */
    monitor_pi2_write(0);
    monitor_vithr_write(0x1FFFFu);
    /* the Game Boy cartridge port (the Pocket's slot): off */
    if (gb_cart_id_read() == 0x47420001u) gb_cart_ctrl_write(0);
    /* what the changes above raised (not bit 5: the Pocket's request for a save state is its own, and waits) */
    for (volatile int i = 0; i < 64; i++) { }
    monitor_pend_write(0x1Fu);
    monitor_miclr_write(0x3Eu);
}

/* the game CPU: its TLB (the VR4300's own, if the core has it: every entry one that matches nothing, as the entry points do not), Index,
 * Wired, Compare (its write clears the timer interrupt) and Cause's software bits */
static void cpu_reset(void)
{
    if (n64rt_tlbreal) {
        MTC0(2, 0); MTC0(3, 0); MTC0(5, 0);
        for (u32 i = 0; i < 32u; i++) { MTC0(0, i); MTC0(10, 0x80000000u | i << 13); TLBWI(); }
        MTC0(6, 0);
        MTC0(0, 0);
    }
    MTC0(11, 0);
    MTC0(13, 0);
}

/* Status, the monitor level's modes and the stack as the start leaves them, then the start. The modes: MCTL's bit 1 (GAME), and the
 * enables 9, 11, 13 and 15 written with their values (bits 8, 10, 12, 14: the pass-through, the 4 MiB of RDRAM, the game's view of
 * 0x2000_0000 - 0x4FFF_FFFF, the SP's registers) 0 -- first, while ERL is set, so that the runtime's own code is not in the game's view
 * when the level is left. */
__asm__(".text\n.set push\n.set noreorder\n.set noat\n"
        ".globl n64rt_restart_go\n"
        "n64rt_restart_go:\n"
        "    li      $t0, 0xAA00\n"
        "    mtc0    $t0, $7\n"
        "    nop\n    nop\n"
        "    lui     $t0, 0x3400\n"                  /* (init_asm.S: CU1, CU0, FR; BEV, ERL, EXL, IE clear) */
        "    mtc0    $t0, $12\n"
        "    nop\n    nop\n    nop\n"
        "    ctc1    $zero, $31\n"
        "    la      $sp, _fstack\n"
        "    j       n64rt_restart_boot\n"
        "    nop\n"
        ".set pop\n");

/* The start again, on the boot stack with the monitor level left: .data as it was, .bss zero, what is kept back in place, main(). Nothing
 * of the runtime's own state is used between the two copies. */
void n64rt_restart_boot(void)
{
    u32 keep[KEEP_MAX / 4u], off = 0;
    for (u32 k = 0; k < NKEEP; k++) {
        u32 n; void *p = s_keepers[k](&n);
        memcpy((u8 *)keep + off, p, n);
        off += (n + 3u) & ~3u;
    }
    const rs_head_t *h = rs_head();
    const u32 count = h->count + 1u;
    memcpy(_fdata, (const void *)(uintptr_t)(RS_AREA + RS_DATA), h->bytes);
    memset(_fbss, 0, (u32)(_ebss - _fbss));
    off = 0;
    for (u32 k = 0; k < NKEEP; k++) {
        u32 n; void *p = s_keepers[k](&n);
        memcpy(p, (const u8 *)keep + off, n);
        off += (n + 3u) & ~3u;
    }
    rs_head()->count = count;
    n64rt_restarted = count;
    flush_cpu_dcache();
    flush_cpu_icache();
    main();
    for (;;) { }
}

void n64rt_restart_booted(void) { RSLOG("n64rt: restart: the game starts again (%lu)\n", (unsigned long)n64rt_restarted); }

/* ---- the action ------------------------------------------------------------------------------------------------- */
#define ACT_SHIFT 20
#define ACT_MASK  (3u << ACT_SHIFT)
static int s_busy;

/* The AI's queue (a buffer playing and one waiting) can only be emptied by playing it: the resampler takes the buffers at the top rate it has
 * (a step of nearly 16 frames to each of the 48 kHz samples) and the Pocket's FIFO is emptied as it fills, so that nothing of it is heard and
 * a buffer of any length is gone in some milliseconds. The AI's DMA is on for it, which a game may have left off. */
static void ai_drain(void)
{
    monitor_airate_write(0xFFFFFu);
    hw_wr(AI_CONTROL, 1);
    const u64 t0 = uptime();
    while ((hw_rd(AI_STATUS) & 0xC0000001u) && uptime() - t0 < MS(500)) {
        apf_audio_buffer_flush_write(1);
        if (monitor_pend_read() & monitor_mask_read() & 0x1Fu) n64rt_rcp_events();
    }
    apf_audio_buffer_flush_write(1);
}

static void restart(void) __attribute__((noreturn));
static void restart(void)
{
    const u64 t_start = uptime();
    RSLOG("n64rt: restart: the game restarts (%lu before); RSP %04lx, RDP list %06lx / %06lx, AI %08lx\n", (unsigned long)n64rt_restarted,
          (unsigned long)SP_STATUS, (unsigned long)RSP(0x48), (unsigned long)DPC_END, (unsigned long)hw_rd(AI_STATUS));
    /* the RSP stops where it is, the RDP finishes the list it has (while the Pocket writes the saves back, below) */
    SP_STATUS = 0x2u;
    DPC_STATUS = 0x4u;                                  /* (not frozen: a frozen list would never end) */
    n64rt_gbv_restart_prep();
    ai_drain();
    const u64 t_saved = uptime();
    const int quiet = n64rt_hw_quiesce(1);
    if (!quiet) RSLOG("n64rt: restart: the hardware was not at rest after a second: going on\n");
    hw_reset();
    cpu_reset();
    RSLOG("n64rt: restart: the saves written in %lu ms, the hardware at rest in %lu ms\n", (unsigned long)((t_saved - t_start) / MS(1)), (unsigned long)((uptime() - t_saved) / MS(1)));
    flush_cpu_dcache();
    n64rt_restart_go();
}

/* MONITOR.PEND bit 2, every tick (n64rt_rcp_events): the action was chosen if its field is not 0 */
void n64rt_restart_poll(void)
{
    if (s_busy || !(apf_interact_interact1_read() & ACT_MASK)) return;
    if (n64rt_state_busy()) return;                     /* (a save state is being made or put back: the action stays and is seen at a later tick) */
    s_busy = 1;
    apf_interact_interact1_write(apf_interact_interact1_read() & ~ACT_MASK);
    if (!snapshot_ok() || keep_bytes() > KEEP_MAX) {
        RSLOG("n64rt: restart: refused (the program's start was not kept)\n");
        s_busy = 0;
        return;
    }
    restart();
}

#ifdef N64RT_RESTART_FP
/* A test build (RESTART_FP=1): the hardware's registers as the game is about to start, a line each, to be compared between the start and
 * a restart's (sim/mips_soc/restart_check.sh) */
void n64rt_restart_fp(void)
{
    u32 pif = 0, imem = 0, dmem = 0;
    for (u32 n = 0; n < 16u; n++) pif = (pif << 5 | pif >> 27) ^ hw_rd(PIF_RAM + 4u * n);
    {   /* DMEM and IMEM, summed through the RDRAM's last 8 KiB (not yet the game's) */
        const u32 at = n64rt_game->mem_size - 0x2000u;
        volatile u32 *p = (volatile u32 *)(uintptr_t)(LAYOUT_RDRAM + at);
        sp_dma(0x0000u, at, 0x1000u, 1);
        sp_dma(0x1000u, at + 0x1000u, 0x1000u, 1);
        flush_cpu_dcache_range((const void *)p, 0x2000u);
        for (u32 n = 0; n < 0x400u; n++) { dmem = (dmem << 5 | dmem >> 27) ^ p[n]; imem = (imem << 5 | imem >> 27) ^ p[0x400u + n]; }
        for (u32 n = 0; n < 0x800u; n++) p[n] = 0;
        flush_cpu_dcache_range((const void *)p, 0x2000u);
    }
    printf("fp: SP_STATUS %08lx PC %05lx DMEM %08lx IMEM %08lx\n", (unsigned long)SP_STATUS, (unsigned long)SP_PC, (unsigned long)dmem, (unsigned long)imem);
    printf("fp: DPC START %06lx END %06lx STATUS %03lx\n", (unsigned long)DPC_START, (unsigned long)DPC_END, (unsigned long)DPC_STATUS);
    printf("fp: MI mode %03lx intr/mask %05lx; AI status %08lx; SI dram %06lx status %lx; PIF %08lx\n", (unsigned long)hw_rd(MI_MODE),
           (unsigned long)monitor_miintr_read(), (unsigned long)hw_rd(AI_STATUS), (unsigned long)hw_rd(SI_DRAM_ADDR), (unsigned long)hw_rd(SI_STATUS), (unsigned long)pif);
    for (u32 n = 0; n < 14u; n++) if (n != 4u) printf("fp: VI[%lu] %08lx\n", (unsigned long)n, (unsigned long)hw_rd(VI_REG(n)));      /* (VI_CURRENT is the line the scan-out is at) */
    printf("fp: VI base %08lx\n", (unsigned long)(hw_rd(VI_BASE) & 0x7FFFFFFFu));      /* (its bit 31 is the field the scan-out is in) */
    printf("fp: MONITOR ctl %lx mask %lx pend %lx (not the field's and the tick's) pi2 %lx airate %lx\n", (unsigned long)monitor_ctl_read(), (unsigned long)monitor_mask_read(),
           (unsigned long)(monitor_pend_read() & ~6u), (unsigned long)monitor_pi2_read(), (unsigned long)monitor_airate_read());
    printf("fp: COP0 status %08lx wired %lx compare %08lx mctl %08lx\n", (unsigned long)MFC0(12), (unsigned long)MFC0(6), (unsigned long)MFC0(11), (unsigned long)(MFC0(7) & 0x07FFFF03u));
    printf("fp: video base %08lx width %08lx window %08lx hwindow %08lx scale %08lx rowwords %lx on %lx vtg %lx\n", (unsigned long)video_framebuffer_dma_base_read(),
           (unsigned long)video_framebuffer_width_read(), (unsigned long)video_framebuffer_window_read(), (unsigned long)video_framebuffer_hwindow_read(),
           (unsigned long)video_framebuffer_scale_read(), (unsigned long)video_framebuffer_rowwords_read(), (unsigned long)video_framebuffer_dma_enable_read(),
           (unsigned long)video_framebuffer_vtg_enable_read());
}
#endif
